#!/usr/bin/env python3
"""
tools/check_abi.py — verify a built .so will load on the Move.

    python3 tools/check_abi.py build/module/dsp.so [--module] [--max-glibc 2.17]

--module also requires the move_plugin_init_v2 export (probe binaries and
other executables do not have it, and that is fine).

Checks, with no toolchain and no readelf (works on macOS/Linux/Windows):

  * the file is an ELF shared object of some ABI class
  * it exports move_plugin_init_v2 (with --module; the symbol the host dlsyms)
  * the symbol versions it requires (e.g. GLIBC_2.27) are not newer than the
    floor we target — the failure mode this exists to catch is a .so that
    builds fine and then silently fails to load on older Move firmware

Exit status 0 = OK, 1 = problem found.
"""

import struct
import sys

MAX_GLIBC_DEFAULT = (2, 17)


def parse_elf_versions(data: bytes):
    """Return (exports, needed_libs, version_names, info). Minimal ELF reader."""
    if data[:4] != b"\x7fELF":
        raise ValueError("not an ELF file (is this the .so, or a text file?)")

    ei_class, ei_data = data[4], data[5]
    is64 = ei_class == 2
    endian = "<" if ei_data == 1 else ">"

    if is64:
        hdr = struct.unpack_from(endian + "HHIQQQIHHHHHH", data, 16)
        e_machine, e_shoff, e_shentsize, e_shnum, e_shstrndx = (
            hdr[1], hdr[5], hdr[10], hdr[11], hdr[12])
        sh_fmt, sh_size = endian + "IIQQQQIIQQ", 64
    else:
        hdr = struct.unpack_from(endian + "HHIIIIIHHHHHH", data, 16)
        e_machine, e_shoff, e_shentsize, e_shnum, e_shstrndx = (
            hdr[1], hdr[5], hdr[10], hdr[11], hdr[12])
        sh_fmt, sh_size = endian + "IIIIIIIIII", 40

    sections = {}
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh = struct.unpack_from(sh_fmt, data, off)
        sections[i] = {
            "name_off": sh[0], "type": sh[1], "offset": sh[4],
            "size": sh[5], "link": sh[6], "entsize": sh[9],
        }

    def cstr(off, limit=4096):
        end = data.index(b"\x00", off, off + limit)
        return data[off:end].decode("utf-8", "replace")

    shstr = sections[e_shstrndx]
    names = {}
    for idx, sh in sections.items():
        try:
            names[idx] = cstr(shstr["offset"] + sh["name_off"], 256)
        except ValueError:
            names[idx] = ""

    by_name = {n: s for n, s in ((names[i], sections[i]) for i in sections)}
    dynstr = by_name.get(".dynstr")

    # --- exports -----------------------------------------------------------
    exports = []
    dynsym = by_name.get(".dynsym")
    if dynsym and dynstr:
        ent = dynsym["entsize"] or (24 if is64 else 16)
        n = dynsym["size"] // ent
        for i in range(n):
            off = dynsym["offset"] + i * ent
            if is64:
                st_name, st_info, st_shndx = struct.unpack_from(endian + "IBH", data, off)
            else:
                st_name, _, st_shndx = struct.unpack_from(endian + "III", data, off)[0:3]
            if st_shndx == 0:      # undefined — not an export
                continue
            if st_name == 0:
                continue
            exports.append(cstr(dynstr["offset"] + st_name, 256))
    exports = sorted(set(exports))

    # --- NEEDED libraries and version requirement strings -------------------
    needed, versions = [], []
    dynamic = by_name.get(".dynamic")
    if dynamic and dynstr:
        ent = dynamic["entsize"] or (16 if is64 else 8)
        n = dynamic["size"] // ent
        for i in range(n):
            off = dynamic["offset"] + i * ent
            tag, val = struct.unpack_from(endian + ("qQ" if is64 else "iI"), data, off)
            if tag == 1:           # DT_NEEDED
                needed.append(cstr(dynstr["offset"] + val, 256))

    verneed = by_name.get(".gnu.version_r")
    if verneed and dynstr:
        off = verneed["offset"]
        end = off + verneed["size"]
        while off < end:
            vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from(
                endian + "HHIII", data, off)
            aux = off + vn_aux
            for _ in range(vn_cnt):
                vna_hash, vna_flags, vna_other, vna_name, vna_next = struct.unpack_from(
                    endian + "IHHII", data, aux)
                versions.append(cstr(dynstr["offset"] + vna_name, 256))
                if vna_next == 0:
                    break
                aux += vna_next
            if vn_next == 0:
                break
            off += vn_next
    versions = sorted(set(versions))

    info = {
        "class": "64-bit" if is64 else "32-bit",
        "machine": {0x3E: "x86-64", 0xB7: "AArch64", 0x28: "ARM",
                    0x03: "i386"}.get(e_machine, hex(e_machine)),
    }
    return exports, needed, versions, info


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip())
        return 1
    path = argv[1]
    max_glibc = MAX_GLIBC_DEFAULT
    if "--max-glibc" in argv:
        max_glibc = tuple(int(x) for x in argv[argv.index("--max-glibc") + 1].split("."))

    want_module = "--module" in argv

    with open(path, "rb") as fh:
        data = fh.read()

    print(f"{path}: {len(data)} bytes")
    try:
        exports, needed, versions, info = parse_elf_versions(data)
    except Exception as exc:                       # noqa: BLE001 — CLI tool
        print(f"  FAIL: {exc}")
        return 1

    print(f"  arch           {info['machine']} {info['class']}")
    print(f"  needs          {', '.join(needed) or '(none)'}")

    ok = True
    if "move_plugin_init_v2" in exports:
        print("  export         move_plugin_init_v2  ✓")
    elif want_module:
        print("  export         move_plugin_init_v2  MISSING — nothing for "
              "the chain host to call")
        ok = False

    glibc = [v for v in versions if v.startswith("GLIBC_")]
    other = [v for v in versions if not v.startswith("GLIBC_")]
    if glibc:
        print(f"  glibc versions {', '.join(glibc)}")
        worst = max(glibc, key=lambda v: tuple(int(x) for x in v.split("_")[1].split(".")))
        if tuple(int(x) for x in worst.split("_")[1].split(".")) > max_glibc:
            print(f"  FAIL: needs {worst}, target is "
                  f"GLIBC_{'.'.join(str(x) for x in max_glibc)} — an older Move "
                  f"would refuse to load this. Rebuild with ZIG=1 (see TESTING.md).")
            ok = False
        else:
            print(f"  glibc floor    {worst} ≤ "
                  f"{'.'.join(str(x) for x in max_glibc)}  ✓")
    if other:
        print(f"  other versions {', '.join(other)}")

    print("  result         " + ("OK" if ok else "PROBLEM FOUND"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
