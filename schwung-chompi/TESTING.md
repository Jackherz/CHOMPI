# Testing on the Move

Three levels, cheapest first. Level 1 needs no UI and no chain — it answers
"does my build work on this device" in about a minute. Do them in order; each
one narrows what a failure means.

Everything here assumes Schwung is already installed and you can reach the
Move over SSH (`ssh ableton@move.local`, key in `/data/authorized_keys` —
Schwung's installer sets that up; Cyberduck/SFTP to `move.local` works too).

---

## 0. Get the code and build it

**The port is not in `main`.** It lives on the branch
`arena/fd058e6a-chompi` (PR #1), so a plain clone of the repo will not have a
`schwung-chompi/` directory at all. Clone it with the branch, sparse so you
skip the 160 MB card images:

```sh
git clone --depth 1 --filter=blob:none --sparse \
    -b arena/fd058e6a-chompi https://github.com/Jackherz/CHOMPI.git
cd CHOMPI
git sparse-checkout set schwung-chompi firmware/card-profiles/tape-2.0
cd schwung-chompi
```

`make` must run **from inside `schwung-chompi/`** — there is no Makefile at the
repo root, and running it elsewhere gives
`make: *** No rule to make target 'probe'. Stop.`

```sh
python3 -m venv /tmp/zigvenv && /tmp/zigvenv/bin/pip install ziglang
make module ZIG=1 ZIG_BIN=/tmp/zigvenv/lib/python3.11/site-packages/ziglang/zig
make check  ZIG=1 ZIG_BIN=/tmp/zigvenv/lib/python3.11/site-packages/ziglang/zig
```

`make module` produces `build/module/` — `dsp.so` + `module.json` +
`samples/`. `make check` then verifies the one thing that would otherwise fail
silently on the Move: that the `.so` needs nothing newer than **glibc 2.17**
(the zig build targets it deliberately — zig's default is 2.27) and exports
`move_plugin_init_v2`:

```
build/module/dsp.so: 2906496 bytes
  arch           AArch64 64-bit
  export         move_plugin_init_v2  ✓
  glibc versions GLIBC_2.17
  glibc floor    GLIBC_2.17 ≤ 2.17  ✓
  result         OK
```

`make check` uses `tools/check_abi.py`, a plain Python ELF reader, so it works
on macOS and Windows too — no `readelf`, no binutils. (On Linux,
`readelf --version-info build/module/dsp.so | grep -o 'GLIBC_[0-9.]*' | sort -u`
says the same thing.)

Two macOS notes: the system `make` is GNU Make 3.81, which is old but handles
everything here; and `zip`/`unzip` are fine, but `scp` needs the Move reachable
as `move.local` (mDNS) — on some networks use the IP instead:
`make probe MOVE=192.168.x.y`.

### More samples (optional, but you want this)

`samples/` ships two CUBBI files so the port is testable out of the box. The
real set is in this repo:

```sh
make samples-full          # copies cubbi_* + jammi_* into samples/ (~150 MB)
```

---

## 1. On-device ABI probe (no UI, no chain host)

This runs the plugin **on the Move**, through the same v2 ABI the chain host
uses, with no involvement from the Shadow UI. If the module is going to fail
on the device, it fails here first, with a readable error.

```sh
make probe ZIG=1 ZIG_BIN=/tmp/zigvenv/lib/python3.11/site-packages/ziglang/zig
```

`make probe` cross-compiles the three probes, then copies the probes + the
staged module to `/data/UserData/schwung/chompi-probe/` on the Move. Then:

```sh
ssh ableton@move.local 'cd /data/UserData/schwung/chompi-probe && ./serve_params ./dsp.so'
```

(The `Permanently added 'move.local' … to the list of known hosts` line is
just SSH accepting the host key the first time — harmless.)

Expect four sections. The important ones:

| key | expected |
| --- | --- |
| `chain_params` | 2127 bytes, 31 `{…}` entries, *starts with `[` and ends with `]`* |
| `ui_hierarchy` | 1352 bytes, starts `{"pad_layout":"chromatic"…`, levels root/tape/looper/recorder/setup |
| `split_voices` | empty (`(none)`) — that is the correct answer, not a failure |
| `state` | 16-key flat JSON |

Then the audio path, which is the part that needs the device's real filesystem:

```sh
ssh ableton@move.local 'cd /data/UserData/schwung/chompi-probe && ./key_probe ./dsp.so . 2'
```

`key_probe` prints the slot map, presses C3 (note 48 → slot 1 in CUBBI mode)
and renders 2 s in paced 128-frame blocks.

- **`cubbi/0:110000000000001-`** → slots 1 and 2 found on the filesystem, I/O
  queue idle (`-`).
- **`peak=4610`** → the streamed sample actually played.
- **`peak=0` with `1`s in the slot map** → the streaming path is broken on this
  device/filesystem. That is the interesting failure; send me the output.
- **`dlopen(./dsp.so): …`** → the module itself didn't load (libc/symbol
  problem, wrong architecture, or a corrupt copy).
- **exit code 2** → silence, same as above (handy for scripting).

`./state_roundtrip ./dsp.so .` does the last check: it writes a state blob,
reloads it and reports `round-trip: OK` — that is what chain save/restore
relies on. Note `serve_params`/`key_probe` use `RTLD_NOW`, exactly like the
chain host, so anything that would silently fail there fails loudly here.

When you're done, `rm -rf /data/UserData/schwung/chompi-probe`.

---

## 2. Install it for real and load it in a chain

```sh
make deploy ZIG=1 ZIG_BIN=/tmp/zigvenv/lib/python3.11/site-packages/ziglang/zig
```

That copies the staged module to
`/data/UserData/schwung/modules/sound_generators/chompi/`. Turn logging on
first so you can see the load:

```sh
ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on"
ssh ableton@move.local "tail -f /data/UserData/schwung/debug.log"     # leave running
```

Then on the Move: open the Shadow UI (hold a Track for ~500 ms opens that
slot's editor), pick the **synth** slot, and choose **CHOMPI** from the module
list. Add `samples/` (or a copy of the full CUBBI set from
`firmware/card-profiles/tape-2.0/`) before you play, or every slot reads as
empty.

What to look for:

- The name appears in the synth list. If it doesn't: the module directory name
  must match `id` in `module.json` (`chompi`), and the loader wants
  `dsp.so` — a mismatch is a silent `dlopen` failure recorded in `debug.log`.
- Knobs: page 1 should be Volume/Filter/Saturate/Warble/Delay/Dly Time/Reverb/
  Varispeed, with Tape/Looper/Recorder/Setup as levels. Pads should light
  chromatically (`pad_layout`).
- `chain_params`/`ui_hierarchy` are served by the plugin, so a UI that shows
  raw keys or nothing at all points at the plugin, not the manifest.

If the module never appears even though level 1 passed, the difference is the
loader, not the DSP — check `debug.log` for the module id and the path it
tried.

---

## 3. Musical smoke test

In order, each step isolates one subsystem:

1. **Sample playback**: keys/pads → CUBBI samples. Slot 1 = C3, slot 2 = D3…
   (15 keys, two octaves folded; `octave` shifts the window).
2. **Streaming under a load spike**: hold several keys in a row and listen for
   dropouts. This is the untested bit — on real hardware the reads contend
   with whatever else touches the filesystem. If you hear clicks, the fix is
   RAM (raise `CHOMPI_RAM_BUFFER_SAMPLES`) or longer refills, and I'd want the
   `debug_files` reading at the moment it happens.
3. **Tape/loop behaviour**: `varspeed`, `reverse`, `start`/`end`, `auto_loop`.
4. **Looper**: `looper_record` → play → `looper_play`, then `looper_pitch` and
   `looper_scrub`. Writes `looper.wav` and `temp_rec.wav` next to the samples —
   check the directory is writable (`ls -la` after a few seconds of recording;
   a 0-byte `looper.wav` means the write path failed).
5. **Audio in**: `input_source`, `input_gain`, `input_monitor` — play a source
   into the Move's input and confirm it monitors, then record it.
6. **State**: turn knobs, save the Set, power-cycle, reload. Values must come
   back — that is `state` doing its job. If a value resets, tell me which
   knob; the mirror in `ChompiInstance::Saved` is the likely suspect.
7. **The ugly cases**: pull the sample files out from under it, load a set with
   a missing bank, switch `voice_mode` mid-note. It should stay silent and
   stable, not reboot the chain.

---

## What a failure looks like

| Symptom | Where to look |
| --- | --- |
| `dlopen` error mentioning `GLIBC_2.2x not found` | toolchain: rebuild with `ZIG=1` (targets 2.17), confirm with `make check` |
| `make: *** No rule to make target 'probe'` | you are not inside `schwung-chompi/`, or on `main` instead of the `arena/…` branch — see step 0 |
| `cd: /data/UserData/…/chompi-probe: No such file or directory` | `make probe` did not run (it creates that directory) — fix step 0 first |
| Module missing from the list, nothing in the log | wrong directory/`id`/filename — must be `<id>/dsp.so` |
| Loads, but no knobs / no pad layout | `serve_params` on the device (level 1): `chain_params`, `ui_hierarchy` |
| Knobs work, keys silent | `key_probe` on the device — `1`s + `peak=0` is the streaming path |
| Everything works, output clips | `volume`/`gain`/`saturate`; CHOMPI's defaults are hot |
| Clicks under load, fine otherwise | filesystem streaming — report the `debug_files` reading and the timing |
| Chain host crashes on load | send `debug.log` — likely an ABI mismatch, so include the Schwung version |

Useful one-liners:

```sh
ssh ableton@move.local "tail -n 200 /data/UserData/schwung/debug.log"
ssh ableton@move.local "ls -la /data/UserData/schwung/modules/sound_generators/chompi/"
ssh ableton@move.local "ls -la /data/UserData/schwung/modules/sound_generators/chompi/samples/ | head"
make logs logs-off        # same as the touch/rm pair above
```

Report back with: which level failed, the exact command, and its output. That
is enough to tell whether it is the DSP, the manifest, or the device's
filesystem without guesswork.
