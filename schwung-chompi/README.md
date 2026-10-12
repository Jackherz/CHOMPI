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

**Run `make deploy-full` once before you judge the Samples page.** Three
files ship in `samples/` — the first CUBBI and JAMMI samples of bank a — which
is enough to hear the module, and leaves slots 3–14 (and banks b/c) silent.
That is exactly what "none of the cubbi samples load" looks like from the UI.
The full card set is in this repo at `firmware/card-profiles/tape-2.0/` (168
WAVs, ~161 MB, MIT-licensed), and `make deploy-full` pushes it to the device in
one `scp` on top of a normal deploy; later `make deploy` runs only re-send
`samples/`, so the full set stays put. `make deploy` prints a reminder when the
device would still be on the bundled few, and `make samples-full` stages the set
locally instead (for the probes). The engine scans slots 1–14 across banks a–e
(`UpdateFileExists()`), so a bank with no files on the device reads as empty —
`debug_files` shows exactly which slots exist.

## Mapping CHOMPI's hardware to the Move

| CHOMPI | Move / Schwung |
| --- | --- |
| 15 keys (C3–C5) | MIDI notes; two octaves fold onto the 15 keys |
| velocity-sensitive key press | note-on velocity → key request |
| 8 knobs | `chain_params` — 35 entries, paged by the shadow UI |
| pitch/speed fader | `varspeed`, `looper_pitch`, `looper_scrub` |
| loop start/end trimmers | `start`, `end` (sample start/end, 0–1) |
| reverse / sustain / autoloop switches | `reverse`, `sustain`, `auto_loop` |
| rec / play buttons | `looper_record`, `looper_play`, `record` |
| clear the loop (rec+play held) | `loop_clear` — the Looper page's `Clear Loop`, a momentary cell |
| FX before or after the looper | `fx_pre_loop` — the Setup page's `FX Route`: `pre` / `post` |
| audio in + monitor switch | `input_monitor`, `input_gain`, `input_source` |
| SD-card slot files | `samples/cubbi_aN.wav` / `jammi_aN.wav` on the Move's FS |
| load a sample onto a key | the **Samples** level: pick `slot`, then `sample_file` opens the host's browser at the portal (`Built-in` = the card, `User Library` = yours) |
| (no CHOMPI equivalent) | `start` / `end` are `wav_position`s: the host draws the assigned file's waveform on the pads |

The shadow UI gets its page layout from `get_param("ui_hierarchy")` (levels
root / tape / looper / recorder / setup / **samples**) and the knob list from
`get_param("chain_params")`. Slot state is saved/restored through
`get_param("state")` / `set_param("state", …)` — a flat 16-key JSON object;
the ids in `chain_params` are the same ids `state` writes, so a saved chain
reloads with the same knobs, mode, bank and octave.

Beyond the 35 knob entries the plugin also serves a few keys that are *not*
in `chain_params`, so they never appear in the UI but stay readable/writable
by a chain or by hand: `monitor_mode` (`both` / `hp` / `sendret`),
`record` (off/on — the CHOMPI capture button, as opposed to the looper's
`looper_record`), and four read-only diagnostics, `debug_files` (per-slot
file-presence flags for the current mode+bank plus whether the I/O queue
still has work), `debug_voice` (one line per voice — playing, envelope gate,
assigned key, file size, pending open/seek/read counts, buffered samples, the
file it holds; this is the reading to ask for when a voice goes quiet),
`sample_slot` (the slot the Samples page is editing) and
`sample_map` (the assignments for that mode+bank, `"1=/abs/path;3=/other;"`),
plus `vu_in` / `vu_out` (envelope followers). All are in-memory reads — safe
to poll.

### Assigning a sample to a key

`slot` selects which slot 1–14 the Samples page is editing (slot 15 is the
recorder's buffer, and an assignment to it is refused with a message rather
than silently dropped). `sample_file` is a `filepath` param: the host opens
its browser, and with `live_preview` set, jogging through the list assigns as
you go so you hear each candidate before committing. Backing out restores the
previous value, which is the same path we reported — that is a *clear*, not an
assignment, so cancelling never leaves a stray override behind.

Where that browser opens is the **sample portal**:

```
/data/UserData/chompi-samples/
├── Built-in       -> <module dir>/samples        (the card set)
└── User Library   -> /data/UserData/UserLibrary/Samples   (your own wavs)
```

Both sources side by side in one directory, and the parameter's `root` is that
directory, so the browser cannot wander off into the rest of `/data` (Schwung
clamps a filepath browser to its root). The module creates the portal itself on
load and `make deploy` creates it on the device too, so a store-style install
that never ran a deploy script still gets it. The layout and the symlinks are
mestela's `schwung-breakbeat`, which solved the same problem the same way —
see PORT PATCH P18.

The paths you see are portal paths ("`Built-in/cubbi_a1.wav`"), while the map
stores real ones: an assignment that names a shipped sample through the portal
still compares equal to that slot's convention name and *clears* the override
instead of pinning a symlink route into the map. Two related parameters exist
for tests and unusual installs: `sample_root` (read-only — the portal in use)
and `portal_dir` / `user_library_dir` (write, to move it). `debug_portal` says
whether each link resolves.

If the file an assignment points at is later deleted, the slot goes silent —
the module will not recreate it (only its own convention names get placeholders;
see PORT PATCH P11 in `VENDORED.md`).

Assignments live in `<module dir>/samples/samples.map`, one line per slot
(`<mode> <bank> <slot> <abs path>`), written atomically (temp + rename) by the
I/O worker. Writes are coalesced: the worker waits for the map to be quiet for
500 ms before touching the card, and `destroy_instance` flushes anything still
pending, so an assignment made just before the host unloads the synth is not
lost. On the next load the map is read back before the first file scan, which
is why an assignment survives a power cycle (`state`, by contrast, only covers
the knobs).

Two things worth knowing:

* **`pad_layout` is served as `"chromatic"`**, re-derived from the engine's
  voice mode on every read. MODULES.md is explicit that an absent
  `pad_layout` is not the same as `"chromatic"`, and a sound generator's
  `module.json` hierarchy is never read — the plugin has to serve it.
* **No MIDI notes are declared.** Notes arrive where they arrive; the key map
  (`note_to_button`) is CHOMPI's own.

### Mode, bank and slot

The **Setup** page carries the three knobs the hardware has:

| knob | what it does |
| --- | --- |
| `voice_mode` | **JAMMI** — one sample for the whole keyboard, played at the note's speed: the original tape behaviour. **CUBBI** — each key plays the sample of its own slot. |
| `bank` | which set of samples that mode plays: 0–4, i.e. a–e on the card. The card set fills a, b and c. |
| `slot` | which slot 1–14 is selected (15 is the recording buffer). In CUBBI it is what the Samples page edits — the keys still address their own slots. In JAMMI it is the sample the keys play. |

**Selecting a slot never cuts off what is playing.** The new sample is bound on
the next key press: one file open per change, instead of a burst of opens while
a knob is being swept, which is what used to wedge a reader and leave the module
silent until it was reloaded (VENDORED P14/P17). In JAMMI, move `slot` or `bank`
and press any key to hear the sample you picked; in CUBBI, `slot` only changes
what the Samples page is editing, because there the key chooses the slot.

Slot 15 is CHOMPI's built-in RAM buffer — out of the box that is a decaying test
tone, which is what a fresh module used to play regardless of the knobs. To hear
the card, start on slot 1.

### Clearing the loop, and where the FX sit

| | |
| --- | --- |
| `loop_clear` (Looper page) | empties the loop exactly as holding rec+play does on the hardware. It is a **tape stop**: the remains of the loop wind down over about a second, and then it is silent. |
| `fx_pre_loop` (`FX Route` on the Setup page) | `pre` — the looper records the FX'd signal, so a delay's tail is printed into the loop. `post` — the looper records the dry signal and the FX colours its playback. The switch is crossfaded, and the fade lands on the value you chose even if you flip it twice quickly (VENDORED P15). |

Both were missing from the port until this batch; `tools/looper_probe` measures
them with audio rather than trusting the parameter list (record a burst through
a long delay, listen to the loop with the FX off: `pre` keeps the tail, `post`
does not).

## Instance lifecycle

The module owns one worker thread that does all file I/O (scans, map writes,
sample opens and reads) while the audio callback stays allocation-, lock- and
I/O-free. Its lifetime is the risky part, because the chain host's unload path
is `destroy_instance()` followed **immediately** by `dlclose()` — its own
comment calls that step "destroy_instance + dlclose of the detached old synth.
Slow for some modules (threads to join, child processes to reap)", i.e. the
host hands the module the job of not leaving threads behind.

So `destroy_instance`:

* sets the instance's `stopping` flag, then **waits until the worker is out of
  that instance** (`worker_busy`) before freeing anything. That wait used to be
  a fixed 2 ms sleep — a guess about how long a file operation takes, and wrong
  on a device with a slow card, where the worker stayed inside memory that had
  just been freed;
* flushes a pending sample-map write (now the only writer, since the worker is
  out of the way);
* unregisters the instance and shrinks the roster (which only ever grew
  before, so repeated loads walked it off the end of the array);
* and, when that was the **last** instance, **joins** the worker before
  returning. The thread retires by itself once no instances are left, so this
  is a short wait — one pass of its 500 µs loop plus at most one file
  operation — and it is what makes removing the plugin from a chain safe: no
  thread of ours is still running when the host unmaps the library.

Both halves are covered by `tools/unload_probe`, which performs the host's own
sequence (load, play, `destroy_instance`, `dlclose`) five times, plus a
two-instance case where one is removed while the other keeps playing. It also
records how the crash was reproduced: with a slow filesystem (a 50 ms file
operation) the pre-fix build segfaults the host on the first removal.

## Verifying without a Move

```sh
make host && make tools
./build/serve_params chain_params ui_hierarchy state   # what the chain host sees
./build/state_roundtrip                               # set/get/edit round-trip
./build/key_probe                                     # presses a key, polls slot files, reports peak
./build/render_test build/chompi_move.so out.wav cubbi samples 4

# The behavioural probes. Each builds in a second and prints its own verdict.
#   sample_map_probe  assign a WAV, reload the plugin, check what plays
#   slot_knob_probe   the slot knob, fast playing, and a vanished file must
#                     none of them wedge the voices (see VENDORED P12/P13)
#   unload_probe      load / play / destroy / dlclose -- the host's own removal
#                     sequence, which must not crash the host
#   mode_bank_probe   which sample each mode/bank/slot plays, measured as a
#                     frequency because every probe sample is a tone
#   looper_probe      record, play and clear a loop; and where the FX sit
for p in sample_map_probe slot_knob_probe unload_probe mode_bank_probe looper_probe; do
    g++ -std=c++17 -O1 -D_Static_assert=static_assert \
        -I module/dsp/third_party/schwung -o /tmp/$p tools/$p.cpp -ldl
done
python3 tools/make_probe_samples.py /tmp/fullmod/samples  # tone-labelled set
/tmp/sample_map_probe
/tmp/slot_knob_probe  build/chompi_move.so .
/tmp/unload_probe     build/chompi_move.so .
/tmp/mode_bank_probe  build/chompi_move.so /tmp/fullmod
/tmp/looper_probe     build/chompi_move.so /tmp/fullmod
```

`render_test` dlopens the plugin, feeds it the same `host_api_v1_t` the chain
host does, plays a note per block group and writes a WAV. Three scenarios:
`ram` (CHOMPI's built-in RAM buffer — it pins `slot` 15, which is where that
lives), `cubbi` (streams `samples/cubbi_a1.wav` off disk), `record` (line-in
capture into the looper). Peaks at this revision ≈ 1748 / 3176 / 3206; the
figures move a little with the FX defaults and the sample set, so treat them as
"not silent, in the same range" rather than exact. Silence means a regression,
and `make render` is the smoke test.

## Status

| | |
| --- | --- |
| Offline (host build, 3 render scenarios) | ✅ working |
| Device, ABI level (`make probe`, no UI/chain) | ✅ **confirmed on an Ableton Move**, 2026-10-11 |
| Device, loaded in a chain via the Shadow UI | ✅ **confirmed on an Ableton Move**, 2026-10-11 |
| Keys over a running loop, looper record/playback | ✅ confirmed — briefly filed as a defect, then falsified (the keys do sound over the loop) |
| Fast playing under load, audio in | not yet tested |
| Sample import (browser, assignment, waveform trim) | offline-verified (`tools/sample_map_probe`), not yet on device |
| Mode / bank / slot, loop clear, FX routing | offline-verified (`tools/mode_bank_probe`, `tools/looper_probe`), not yet on device |

The device check that passed: the aarch64 build dlopens on the Move, serves
`chain_params` (2127 B / 31 entries when that check ran — 2796 B / 35 entries
today), `ui_hierarchy` (1352 B then, 6 levels — 1864 B today, plus
`pad_layout`), `state` (16 keys then, 17 today), finds the slot files on the
Move's filesystem (`cubbi/0:110000000000001-`) and streams a CUBBI sample off
disk into the render block — `peak=4729` over 2 s, i.e. the sample played, and
the state round-trip came back byte-identical.

## Device unknowns (still untested on hardware)

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
3. **Sample set** — three files ship here, so most slots read as empty until
   `make deploy-full` pushes the card set (~161 MB, one time). The scan reports
   empty slots honestly (`debug_files`), which is the state to expect before
   that push; the Samples page itself is fine.

## Licence

CHOMPI's firmware and samples are MIT (© 2026 CHOMPI Club, see the root
`LICENSE`). The vendored DSP keeps that licence; the adapter is under the
same terms.
