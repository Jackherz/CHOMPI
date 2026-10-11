# CHOMPI → Schwung (Ableton Move)

CHOMPI's TAPE firmware (the tape-music sampler / rolling looper from the
[CHOMPI open-source bundle](../firmware/chompi-tape)) rebuilt as a **Schwung
sound generator** for the Ableton Move.

`schwung-chompi/` is additive: it does not modify anything under
`firmware/` — CHOMPI's DSP is vendored in and patched in place (six small
patches, see [VENDORED.md](VENDORED.md)).

```
module/dsp/chompi_move.cpp   Schwung plugin ABI ⇄ CHOMPI engine adapter
module/dsp/engine/           CHOMPI TAPE DSP, vendored (see VENDORED.md)
module/dsp/compat/           POSIX stand-ins for libDaisy / FatFs / DaisySP
module/dsp/port/             chompi::g_sample_dir (one global)
module/dsp/third_party/      DaisySP bits + the two Schwung ABI headers
module/module.json           Schwung module manifest
host/render_test.cpp         offline render harness (no Move needed)
tools/                       ABI probes (params / state / MIDI), runnable here
                             and on the Move — see TESTING.md
samples/                     two CUBBI samples so the port is testable
```

## Build

Run these from inside `schwung-chompi/` (there is no Makefile at the repo
root, and this directory only exists on the `arena/fd058e6a-chompi` branch —
see [TESTING.md](TESTING.md) step 0).

```sh
make host        # x86-64 .so for this machine + everything below
make tools       # ABI probes -> build/serve_params, build/state_roundtrip, build/key_probe
make render_test # offline render harness
make render      # 6 s of audio per scenario -> build/render/chompi_{ram,cubbi,record}.wav
make aarch64     # device .so (needs an aarch64 toolchain; see below)
make module      # stage build/module/ = the drop-in directory for the Move
make check       # verify the device artifacts (glibc floor + plugin export)
```

The two `.so`s are the same sources; only the target differs. Nothing about
the port is x86-specific, so `make host` is a real test of the code the Move
runs, not an approximation — the harness calls the same `move_plugin_init_v2`
entry point the chain host calls, through the same v2 ABI structs.

**Cross-compiling:** zig works without installing a full toolchain:

```sh
python3 -m venv /tmp/zigvenv && /tmp/zigvenv/bin/pip install -q ziglang
make module ZIG=1 ZIG_BIN="/tmp/zigvenv/bin/python3 -m ziglang"
```

The build targets **glibc 2.17** (`GLIBC=2.17`, override if you like) and is
linked `BIND_NOW`. zig's default target is 2.27, which needs
`expf`/`exp2f`/`logf`/`powf@GLIBC_2.27` — a 2.17 build binds the older symbol
versions that every newer glibc still provides, so the same `.so` loads on old
and new Move firmware alike. `make check` verifies it (a self-contained Python
ELF reader, so it works on macOS too):

```
build/module/dsp.so: 2906496 bytes
  arch           AArch64 64-bit
  export         move_plugin_init_v2  ✓
  glibc floor    GLIBC_2.17 ≤ 2.17  ✓
  result         OK
```

**Deploy and test on the Move:** see [TESTING.md](TESTING.md). Short version:

```sh
make probe  ZIG=1 ZIG_BIN="…/python3 -m ziglang"   # build + copy probes to the Move
make deploy ZIG=1 ZIG_BIN="…/python3 -m ziglang"   # install into modules/sound_generators/chompi/
make logs                     # enable + tail Schwung's debug.log
```

## Install on the Move

Schwung loads sound generators from a per-module **directory** containing
`module.json` and `dsp.so`:

```
/data/UserData/schwung/modules/sound_generators/chompi/
├── module.json
├── dsp.so
└── samples/          cubbi_a*.wav / jammi_a*.wav
```

So: `make module CROSS=...`, then copy `build/module/` over (SSH/SFTP, or
whatever your Schwung setup uses) and power-cycle or reload the chain. The
samples directory is the default because `create_instance()` seeds
`sample_dir` to `<module dir>/samples`; `set_param("sample_dir", …)` moves it
at runtime (and triggers a rescan).

**Samples are not bundled beyond the two CUBBI files** in `samples/` —
`make samples-full` copies the whole set in from `firmware/card-profiles/`. The full
set lives in this repo at `firmware/card-profiles/tape-2.0/` (`cubbi_*.wav`,
`jammi_*.wav`) — MIT-licensed, so copying them into
`.../chompi/samples/` is fine. The engine scans for slots 1–14 across banks
a–e (mode × bank × slot, `UpdateFileExists()`), so extra banks need extra
files or the slot reads as empty.

## Mapping CHOMPI's hardware to the Move

| CHOMPI | Move / Schwung |
| --- | --- |
| 15 keys (C3–C5) | MIDI notes; two octaves fold onto the 15 keys |
| velocity-sensitive key press | note-on velocity → key request |
| 8 knobs | `chain_params` — 31 entries, paged by the shadow UI |
| pitch/speed fader | `varspeed`, `looper_pitch`, `looper_scrub` |
| loop start/end trimmers | `start`, `end` (sample start/end, 0–1) |
| reverse / sustain / autoloop switches | `reverse`, `sustain`, `auto_loop` |
| rec / play buttons | `looper_record`, `looper_play`, `record` |
| audio in + monitor switch | `input_monitor`, `input_gain`, `input_source` |
| SD-card slot files | `samples/cubbi_aN.wav` / `jammi_aN.wav` on the Move's FS |

The shadow UI gets its page layout from `get_param("ui_hierarchy")` (levels
root / tape / looper / recorder / setup) and the knob list from
`get_param("chain_params")`. Slot state is saved/restored through
`get_param("state")` / `set_param("state", …)` — a flat 16-key JSON object;
the ids in `chain_params` are the same ids `state` writes, so a saved chain
reloads with the same knobs, mode, bank and octave.

Beyond the 31 knob entries the plugin also serves a few keys that are *not*
in `chain_params`, so they never appear in the UI but stay readable/writable
by a chain or by hand: `monitor_mode` (`both` / `hp` / `sendret`),
`record` (off/on — the CHOMPI capture button, as opposed to the looper's
`looper_record`), and two read-only diagnostics, `debug_files` (per-slot
file-presence flags for the current mode+bank plus whether the I/O queue
still has work) and `vu_in` / `vu_out` (envelope followers). All are
in-memory reads — safe to poll.

Two things worth knowing:

* **`pad_layout` is served as `"chromatic"`**, re-derived from the engine's
  voice mode on every read. MODULES.md is explicit that an absent
  `pad_layout` is not the same as `"chromatic"`, and a sound generator's
  `module.json` hierarchy is never read — the plugin has to serve it.
* **No MIDI notes are declared.** Notes arrive where they arrive; the key map
  (`note_to_button`) is CHOMPI's own.

## Verifying without a Move

```sh
make host && make tools
./build/serve_params chain_params ui_hierarchy state   # what the chain host sees
./build/state_roundtrip                               # set/get/edit round-trip
./build/key_probe                                     # presses a key, polls slot files, reports peak
./build/render_test build/chompi_move.so out.wav cubbi samples 4
```

`render_test` dlopens the plugin, feeds it the same `host_api_v1_t` the chain
host does, plays a note per block group and writes a WAV. Three scenarios:
`ram` (JAMMI/ram playback), `cubbi` (streams `samples/cubbi_a1.wav` off disk),
`record` (line-in capture into the looper). Expected peaks ≈ 1559 / 3436 /
3035 — silence means a regression, and `make render` is the smoke test.

## Device unknowns (untested on hardware)

1. **File streaming under load** — CHOMPI streams samples in 8 KB chunks
   through a worker thread (48 kHz, 1 kHz SD callback on hardware). On the
   Move the reads land on the shared filesystem and the worker is scheduled
   off the audio core (0–2, priority 0), but the read latency of a large
   `cubbi_*.wav` during a key press has not been measured on hardware.
   `CHOMPI_RAM_BUFFER_SAMPLES` (4 M samples ≈ 8 MB per arena) trades RAM for
   safety margin. First thing to listen for: clicks when playing fast.
2. **Input monitoring latency** — CHOMPI monitors the input through the same
   DSP, so live monitoring is one block behind; fine for a looper, worth
   a listen for a live-tape feel.
3. **Sample set** — only two CUBBI files ship here. The full set is in
   `firmware/card-profiles/tape-2.0/`; copy it into the module's `samples/`,
   or the slot scan reports empty slots (visible via `debug_files`).

## Licence

CHOMPI's firmware and samples are MIT (© 2026 CHOMPI Club, see the root
`LICENSE`). The vendored DSP keeps that licence; the adapter is under the
same terms.
