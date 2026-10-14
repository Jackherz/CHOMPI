# CHOMPI → Schwung (Ableton Move)

CHOMPI's TAPE firmware (the tape-music sampler / rolling looper from the
[CHOMPI open-source bundle](../firmware/chompi-tape)) rebuilt as a **Schwung
sound generator** for the Ableton Move.

`schwung-chompi/` is additive: it does not modify anything under
`firmware/` — CHOMPI's DSP is vendored in and patched in place (24 surgical
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
| 25-key chromatic keyboard (C3–C5) | the 32 pads, as a keyboard: a chromatic ladder, one semitone a pad, no two pads on one pitch. One sample, every pad (see below) |
| velocity-sensitive key press | note-on velocity → key request |
| 8 knobs | `chain_params` — the on-screen controls, paged by the shadow UI |
| pitch/speed fader | `varspeed`, and `looper_pitch` / `looper_reverse` (one number: the loop's speed, with a negative value playing it backwards, so the two cells follow each other). The Looper page's `Loop Pitch` also carries the reverse: a negative value is the tape turning around. `looper_scrub` — the tape playhead — is still served but no longer drawn (the window's bounds took its job; VENDORED P25). All of them read back the number they were set to — the host repaints `get_param` when a knob is released — and a `Clear Loop` returns them to 1.00×, forward |
| loop start/end trimmers | `start`, `end` (sample start/end, 0–1) |
| (no CHOMPI equivalent) | the looper's **window**: `looper_start` / `looper_end` (`Strt` / `End` on the Looper page), fractions of the take the loop turns over between. The order of the pair is its direction: `End` below `Strt` plays the same span **backwards**, turning round through a 0.35 s glide rather than a cut, in a field of its own — `Loop Pitch` / `Loop Rev` keep reading what you set them to, and a negative pitch on a crossed pair is forward again (VENDORED P26, P28) |
| reverse / sustain / autoloop switches | `reverse`, `sustain`, `auto_loop` |
| rec / play buttons | `looper_record` (the cell reads `rec` while a pass runs, `lorec` when idle), `looper_play`, `record` |
| overdub gain (the two keys beside play, +-0.1 each) | `looper_dub_gain` — the Looper page's `Dub Gain`, 0…1. Each overdub pass rewrites the loop as `old × gain + input`, so 1 layers the new take over the old loop (0.94× measured), 0.5 halves it (0.25×), and 0 replaces it outright. The change is slewed (~0.2 s) and a pass that finishes before the slew has arrived keeps more of the old loop — the engine's own behaviour, and the same on the hardware |
| clear the loop (rec+play held) | `loop_clear` — the Looper page's `Clear Loop`, a momentary cell |
| FX before or after the looper | `fx_pre_loop` — the Setup page's `FX Route`: `pre` / `post` |
| audio in + monitor switch | `input_monitor`, `input_gain`, `input_source` |
| SD-card slot files | `samples/cubbi_aN.wav` / `jammi_aN.wav` on the Move's FS |
| load a sample onto a key | the **Samples** level: `sample_file` opens the host's browser at the portal (`Built-in` = the card, `User Library` = yours) — what you pick replaces the one sample the keybed plays, across all 32 pads |
| (no CHOMPI equivalent) | `start` / `end` are `wav_position`s: the host draws the assigned file's waveform on the pads |

The shadow UI gets its page layout from `get_param("ui_hierarchy")` (levels
root / tape / looper / recorder / setup / **samples**) and the knob list from
`get_param("chain_params")`. Slot state is saved/restored through
`get_param("state")` / `set_param("state", …)` — a flat 16-key JSON object;
the ids in `chain_params` are the same ids `state` writes, so a saved chain
reloads with the same knobs, mode, bank and octave.

Beyond the knob entries the plugin also serves a few keys that are *not*
in `chain_params`, so they never appear in the UI but stay readable/writable
by a chain or by hand: `monitor_mode` (`both` / `hp` / `sendret`),
`record` (off/on — the CHOMPI capture button, as opposed to the looper's
`looper_record`), and four read-only diagnostics, `debug_files` (per-slot
file-presence flags for the current mode+bank plus whether the I/O queue
still has work), `debug_voice` (one line per voice — playing, envelope gate,
assigned key, file size, pending open/seek/read counts, buffered samples, the
file it holds; this is the reading to ask for when a voice goes quiet),
`sample_slot` (the slot the Samples page is editing) and
`sample_map` (the one sample's assignment, `"1=/abs/path;"`),
plus `vu_in` / `vu_out` (envelope followers). All are in-memory reads — safe
to poll.

### One sample, on a keyboard

The pads are a keyboard. Bottom-left is MIDI 68, and every pad is one semitone
above the pad before it — left to right, bottom row first. Thirty-two pads,
thirty-two pitches, no two of them the same:

```
row 1 (68..75)   -12 -11 -10  -9  -8  -7  -6  -5
row 2 (76..83)    -4  -3  -2  -1   0  +1  +2  +3
row 3 (84..91)    +4  +5  +6  +7  +8  +9 +10 +11
row 4 (92..99)   +12 +13 +14 +15 +16 +17 +18 +19
```

The numbers are semitones from the sample's own speed, which is pad 80. The
bottom-left pad is 0.50× and the top-right one is 3.00× — the tape's whole
speed range, one semitone a step. The `octave` knob transposes the whole
keyboard, one octave a step.

The pitch is the NOTE minus the root, not an index into a table of pads, and
that is what makes the keybed follow the note: Move's octave buttons shift all
32 pads by twelve, its transpose shifts them by semitones, and the whole
keyboard moves with them, every pad still exactly one semitone from its
neighbour (PORT PATCH P27). A fixed table would fold the shifted pads back onto
pitches the unshifted ones already play — the same note from several pads — and
read the pads that ran off the end of it as some other key entirely.

There is one sample. `sample_file` is a `filepath` param: the host opens
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

The browser offers **`.wav`, `.aif` and `.aiff`**. The engine itself reads
RIFF/WAVE PCM and skips a fixed 44-byte header, so anything else would be played
as raw bytes — noise; a file-byte-addressed converter in front of it
(`port/sample_view.cpp`, PORT PATCH P21) turns WAV at any rate/bit depth and
AIFF/AIFC into the 16-bit stereo the engine expects, and the 16-bit stereo
44-byte-header case still takes the original fast path untouched.

The module's own runtime files (`looper.wav`, `temp_rec.wav`) live in that same
folder and therefore show up under `Built-in` next to the card set; they are
the looper's recordings, not part of it.

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

* **`pad_layout` is served as `"chromatic"`**: the pads are a keyboard, so the
  surface is declared as the keyboard it is. MODULES.md is explicit that an
  absent `pad_layout` is not the same as `"chromatic"`, and a sound generator's
  `module.json` hierarchy is never read — the plugin has to serve it.
* **No MIDI notes are declared.** Notes arrive where they arrive; the keybed
  reads the note itself (`keybed_pitch()` in `chompi_move.cpp`: the note minus
  the root, pad 80), and a note that is not one of the 32 pads continues the
  same ladder — so a MIDI keyboard, Move's octave buttons and its transpose all
  move the whole keyboard with the notes they send.

### One file, one keybed

**One sample.** `voice_mode`, `bank` and `slot` are still served, and the engine
still names files by them — the keybed plays the assigned file, or the shipped
`jammi_a1` (bank a, slot 1) when nothing is assigned — but they are pinned:
writing any of the three cannot put a second file under a pad, which
`tools/mode_bank_probe` measures as a frequency on three pads.

An old Set that names a mode, bank or slot still restores (`state` carries all
three) and the values stay readable/writable; neither is drawn. The **Setup**
page carries `octave` — the whole keybed transposed, one octave a step — and
`fx_pre_loop`. `debug_files` lists what the card holds.

**A change is bound on the next key press**, not while a note is playing: one
file open per change, instead of a burst of opens while a knob is being swept,
which is what used to wedge a reader and leave the module silent until it was
reloaded (VENDORED P14/P17).

**Pitch comes from the key, not from a companion file.** Above 1.5× the engine
wants a `<name>_double.wav` next to the sample — the card ships one for every
file, the same audio at twice the speed (half the frames, an octave up) so the
sample's own high notes can be read from a copy that is not thinned out. A user's
assignment has none, and the reader used to claim double speed anyway and play it
an OCTAVE LOW (measured 321 Hz where 642 was due); PORT PATCH P22 stopped that
claim for assigned slots. But an assigned sample's "companion" is the sample
itself — the port returns the same path for both names — and the three files
this repo ships have no `_double` at all, so the same claim was wrong a second
way: the port's open creates a 0-byte placeholder instead of failing, the reader
swapped onto it, and a burst of high pads or a slot-knob sweep ended in
permanent silence. PORT PATCH P23 answers the question at the reader, when the
open would really use the companion: it must be a *different* file that the disk
says has samples in it, a companion that lands empty (or will not open) is
dropped and the voice plays the file it has, and the top of a slot's octave plays
at the right pitch with the one copy — at the cost of the aliasing the double
exists to avoid.

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

* **waits until the instance's worker is out of that instance**
  (`worker_busy`) before freeing anything. That wait used to be
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
#                     none of them wedge the voices (see VENDORED P12/P13/P23)
#   unload_probe      load / play / destroy / dlclose -- the host's own removal
#                     sequence, which must not crash the host
#   mode_bank_probe   which sample each mode/bank/slot plays, measured as a
#                     frequency because every probe sample is a tone
#   looper_probe      record, play and clear a loop; the dub gain's per-pass
#                     law (1 keeps, 0.5 halves, 0 erases); the record cell's
#                     live state; where the FX sit; the playhead; the window
#                     (in laps) and its direction (End below Start runs the
#                     span backwards, measured off a rising melody)
#   keybed_octave_probe  the keybed is a keyboard: all 32 pads' pitches, the
#                     octave shifts (Move's and the module's), one sample on
#                     the pads, and the >1.5x companion
for p in sample_map_probe slot_knob_probe unload_probe mode_bank_probe looper_probe \
         keybed_octave_probe; do
    g++ -std=c++17 -O1 -D_Static_assert=static_assert \
        -I module/dsp/third_party/schwung -o /tmp/$p tools/$p.cpp -ldl
done
python3 tools/make_probe_samples.py /tmp/fullmod/samples  # tone-labelled set
/tmp/sample_map_probe
/tmp/slot_knob_probe  build/chompi_move.so .
/tmp/unload_probe     build/chompi_move.so .
/tmp/mode_bank_probe  build/chompi_move.so /tmp/fullmod
/tmp/looper_probe     build/chompi_move.so /tmp/fullmod
mkdir -p /tmp/kboct && cp build/chompi_move.so /tmp/kboct/
(cd /tmp/kboct && /tmp/keybed_octave_probe ./chompi_move.so .)
```

`render_test` dlopens the plugin, feeds it the same `host_api_v1_t` the chain
host does, plays a note per block group and writes a WAV. Three scenarios:
`ram` (CHOMPI's built-in RAM buffer — it pins `slot` 15, which is where that
lives), `cubbi` (streams `samples/cubbi_a1.wav` off disk), `record` (line-in
capture into the looper). With the three files this repo ships, peaks
at this revision are ≈ 1466 / 1007 / 3229 (`ram` / `cubbi` / `record`); they
move with the FX defaults and the sample set — the tone-labelled set
`tools/make_probe_samples.py` writes is hot enough to reach ~3200 in the cubbi
scenario too — so treat them as "not silent, in the same range" rather than
exact. Silence means a regression,
and `make render` is the smoke test.

## Status

| | |
| --- | --- |
| Offline (host build, 3 render scenarios) | ✅ working |
| Device, ABI level (`make probe`, no UI/chain) | ✅ **confirmed on an Ableton Move**, 2026-10-11 |
| Device, loaded in a chain via the Shadow UI | ✅ **confirmed on an Ableton Move**, 2026-10-11 |
| Keys over a running loop, looper record/playback | ✅ confirmed — briefly filed as a defect, then falsified (the keys do sound over the loop) |
| Fast playing under load, audio in | not yet tested |
| Sample import (browser, assignment, waveform trim) | offline-verified (`tools/sample_map_probe`, `tools/sample_format_probe`), not yet on device |
| Mode / bank / slot, loop clear, FX routing | offline-verified (`tools/mode_bank_probe`, `tools/looper_probe`), not yet on device |
| The keybed as a keyboard (32 pitches, octave shifts, one sample) | offline-verified (`tools/keybed_octave_probe`), not yet on device |

The device check that passed: the aarch64 build dlopens on the Move, serves
`chain_params` (2127 B / 31 entries when that check ran — 3458 B / 34 entries
today), `ui_hierarchy` (1352 B then, 6 levels — 1760 B today, plus
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
