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
python3 -m venv /tmp/zigvenv
/tmp/zigvenv/bin/pip install -q ziglang

# builds the device .so + the three test programs, then copies them to the
# Move at /data/UserData/schwung/chompi-probe/  (scratch dir — this does NOT
# install anything into modules/, nothing is touched yet)
make probe ZIG=1 ZIG_BIN="/tmp/zigvenv/bin/python3 -m ziglang"
```

The `python3 -m ziglang` form matters: it finds zig inside the venv whatever
your Python version is (the `site-packages/.../ziglang/zig` path differs
between Pythons), so this works on any Mac. If your SSH user or hostname
differ from `ableton@move.local`, add `MOVE_USER=… MOVE=…`.

`make module` alone just produces `build/module/` — `dsp.so` +
`module.json` + `samples/`. `make check` verifies the one thing that would
otherwise fail silently on the Move: that the `.so` needs nothing newer than
**glibc 2.17** (the zig build targets it deliberately — zig's default is 2.27)
and exports `move_plugin_init_v2`:

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

### The full sample set (do this once — the Samples page is empty without it)

`samples/` ships three files (bank a's first CUBBI and JAMMI samples) so the
port is testable out of the box, and it is *not* obvious from the UI that the
rest are missing: keys 1–2 play, everything else is silent. Push the whole card
set once and leave it there:

```sh
# same flags step 0 uses for the ZIG=1 toolchain
make deploy-full ZIG=1 ZIG_BIN="/tmp/zigvenv/bin/python3 -m ziglang"
# a normal deploy, plus the 168-WAV card set (~161 MB, one scp; the files stay
# on the device afterwards)
```

When the deploy finishes it prints how many WAVs the device now has — 168 plus
the three bundled ones is what "everything is there" looks like. `make deploy`
alone re-sends only `samples/` and reminds you if the device is still on the
bundled few. To stage the set locally instead (for the offline probes):

```sh
make samples-full          # copies cubbi_* + jammi_* into samples/ (~161 MB)
```

**Every card sample has a `_double` beside it** — the same audio at twice the
speed (half the frames, an octave up), which the reader switches to above 1.5×
so the top of a slot's octave is not read from a thinned-out copy. The three
bundled files have none, and the module must be fully playable without them:
above 1.5× a pad then plays the one copy it has, at the right pitch, with the
aliasing the double exists to avoid. Both states are covered offline
(`tools/keybed_octave_probe` passes on a tree with no companions, with a real
companion, and with a 0-byte one) and worth one listen each on the device —
see step 3.1 and the P23 row in "What a failure looks like".

---

## 1. On-device ABI probe (no UI, no chain host)

> **Confirmed working on a Move, 2026-10-11.** Observed: `chain_params` 2127 B,
> `ui_hierarchy` 1352 B, `split_voices` empty, `state` 289 B, slot map
> `cubbi/0:110000000000001-`, `key_probe` `peak=4729` over 2 s,
> `state_roundtrip` → `round-trip: OK`.

This runs the plugin **on the Move**, through the same v2 ABI the chain host
uses, with no involvement from the Shadow UI. If the module is going to fail
on the device, it fails here first, with a readable error.

`make probe` (step 0) already copied everything to
`/data/UserData/schwung/chompi-probe/` on the Move. Run the probes there:

```sh
ssh ableton@move.local 'cd /data/UserData/schwung/chompi-probe && ./serve_params ./dsp.so'
```

(The `Permanently added 'move.local' … to the list of known hosts` line is
just SSH accepting the host key the first time — harmless.)

**Then the pages themselves** — the one probe that answers "does the host have
anything to draw at all":

```sh
ssh ableton@move.local 'cd /data/UserData/schwung/chompi-probe && ./hierarchy_probe ./dsp.so .'
```

It must end `VERDICT: PASS (0 failures)`. It parses `ui_hierarchy` and
`chain_params` the way the host does, and checks that every page names a param
the module serves and links to a level that exists. Run it on the device for
the reason it exists: a stray quote in one level's param list once left the
Move showing a single preset page reading "no presets" and none of the module's
pages, while every other probe passed — they all ask for params by key, and
none of them ever asked for the pages.

One practical note: give each probe its own working directory. Several of them
read and write the state the module keeps beside it (`samples.map`,
`looper.wav`), so a shared directory makes them fail against each other's
leftovers — `sample_format_probe` reported the card file missing that way, and
`env_probe` wants a directory it is allowed to clear.

Expect four sections. The important ones:

| key | expected |
| --- | --- |
| `chain_params` | 3458 bytes, 34 `{…}` entries, *starts with `[` and ends with `]`* |
| `ui_hierarchy` | 1760 bytes, starts `{"pad_layout":"chromatic"…`, levels root/tape/looper/recorder/setup/samples |
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
make deploy ZIG=1 ZIG_BIN="/tmp/zigvenv/bin/python3 -m ziglang"
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

1. **Sample playback**: one sample, every pad. The pads are a piano — white
   keys on rows 1 and 3, black keys (the sharps) on rows 2 and 4, one column
   to the right of the white they split. The C at the end of row 1 plays the
   sample at its own speed. The three pads a piano leaves empty (above C,
   above F, and above the octave C) are not silent: each plays the white key
   below it, one octave higher. `octave` transposes the whole keyboard.
   Then the **top of the keybed**, where the sample is read above 1.5×: sweeps
   of the top two rows and the top-gap pad must all *sound* (with a companion
   file they are clean, without one they are a little aliased), and playing
   them must not leave a 0-byte `_double` file behind. On the device:
   `ssh ableton@move.local "ls -la …/chompi/samples/ | grep double"` before and
   after — nothing may appear, and nothing already there may grow. Sliding
   `slot` or `bank` across while pressing any key must keep the keybed alive
   (P23's wedge was exactly this: silence that only a reload fixed).
2. **Streaming under a load spike**: hold several keys in a row and listen for
   dropouts. This is the untested bit — on real hardware the reads contend
   with whatever else touches the filesystem. If you hear clicks, the fix is
   RAM (raise `CHOMPI_RAM_BUFFER_SAMPLES`) or longer refills, and I'd want the
   `debug_files` reading at the moment it happens.
3. **Tape/loop behaviour**: `varspeed`, `reverse`, `start`/`end`, `auto_loop`.
4. **Looper**, seven knobs on the page: `REC` (`looper_record`), `LOPLAY`
   (`looper_play`), `LOOPIT` (Loop Pitch), `DUGAIN` (Dub Gain), the window's
   `STRT`/`END`, and `CLLOOP` (Clear Loop). `looper_reverse` and
   `looper_scrub` are still served as KEYS and still work, but no page draws
   them — the window took over the job Scrub was on the page for, Loop Pitch
   turns the tape around by its sign, and the two freed slots went to the
   window's bounds and to Clear.
   Writes `looper.wav` and `temp_rec.wav` next to the samples — check the
   directory is writable (`ls -la` after a few seconds of recording;
   a 0-byte `looper.wav` means the write path failed).
   Then **clear it**: the Looper page's `Clear Loop` cell. It is a tape stop —
   the loop winds down over about a second and then stays silent; recording
   again afterwards must make a fresh loop (not a dub of the cleared one).
   **Overdub**: record a loop, play it back, then record again — the second
   pass layers onto the old loop instead of replacing it (`Dub Gain` at 1), and
   the record cell reads `rec` while the pass runs and `lorec` afterwards.
   Turn `Dub Gain` to 0 before a pass and the pass *replaces* the old loop
   instead of layering; at 0.5 the old loop comes back at half its level. That
   law is hardware-shared (the sampler's write loop) and measured offline by
   `tools/looper_probe`, so anything else on the device is worth reporting.
   **The knobs must stick**: turn `Loop Pitch` to 2 and let go — the cell keeps
   reading 2.0000 (it used to snap home while the loop stayed transposed), and
   the tape is audibly twice as fast. Turning it *below* zero is how the tape
   turns around now that no `Loop Rev` cell is drawn: the module flips its own
   reverse state to match the sign, and flips it back above zero. A `Clear Loop`
   leaves the knobs where the tape lands: pitch 1.0000, reverse `Off`.
   **The playhead is off the page** (it survives as the `looper_scrub` key):
   with the bounds on the page, moving the head by hand is not how anyone gets
   to a part of a take. The odd-sample freeze that key could once cause stays
   fixed in the engine — the window test asks about the stereo *pair* the next
   read would take, and a move lands on an even sample — and `tools/looper_probe`
   still checks it offline, so there is nothing to listen for by hand here.
   **The window**: the Looper page's last two knobs, `Strt` and `End`. `Strt`
   up starts the loop later in the take, `End` down stops it earlier, and the
   pass gets shorter by exactly that much — half the take loops about twice as
   often, which is the whole feature in one listen. Both cells keep reading
   back the number they were set to, `End` below `Strt` included; that crossed
   pair is the span between the bounds played forwards for now — running it
   backwards through the span is the next piece of this work, not a fault.
   `tools/looper_probe` measures the pass in laps (whole 2, half 4) and checks
   the window's edges offline; what it cannot do is feel a knob or read the
   cells off the screen.
5. **One sample** (Samples page): pick a file. Every pad plays that file,
   pitched by the piano map above. There is no slot, bank or mode knob — a
   second file is not on the keybed until you pick it. On load, every knob
   shows the value the sound is at (Volume 0.80, not 0).
6. **FX route** (Setup page, `FX Route`): set a long delay
   (`delay_time` up, `delay_feedback` up), record a short loop in `pre`, then
   listen back with `delay_feedback` at zero — the delay's tail is *in* the
   loop. Clear, switch to `post` and repeat: the loop is dry, and the FX now
   colours its playback instead.
7. **Audio in**: `input_source`, `input_gain`, `input_monitor` — play a source
   into the Move's input and confirm it monitors, then record it.
8. **State**: turn knobs, save the Set, power-cycle, reload. Values must come
   back — that is `state` doing its job (`fx_pre_loop` is in there too now). If
   a value resets, tell me which knob; the mirror in `ChompiInstance::Saved` is
   the likely suspect.
9. **The ugly cases**: pull the sample files out from under it, load a set with
   a missing bank, switch `voice_mode` mid-note. It should stay silent and
   stable, not reboot the chain.
   A companion file is part of this: truncate one (`: > samples/cubbi_a1_double.wav`)
   and play the pad above 1.5× that uses it — the pad plays the base sample, and
   the keybed keeps working. Delete it and the same. The reader treats a
   companion that is missing, empty or un-openable as absent; it must never
   swap onto one.

---

## 4. Sample import

Pick a WAV on the Move and the whole keybed plays it. The pick sticks across a power cycle.

```sh
cd ~/CHOMPI/schwung-chompi
make deploy ZIG=1 ZIG_BIN="/tmp/zigvenv/bin/python3 -m ziglang"
```

Then, with the chain loaded, open the module's editor and go to **Samples**:

1. `SMPL` → the browser opens inside the **sample portal**,
   `/data/UserData/chompi-samples`, which holds two entries:

   * `[Built-in]` — the card set that ships with the module
   * `[User Library]` — your own samples
     (`scp myloop.wav ableton@move.local:/data/UserData/UserLibrary/Samples/`
     to put one there)

   The browser is fenced to that directory: `..` at the top of it does nothing.
   If `[User Library]` is missing, the directory above does not exist on the
   device — create it, then reopen the module (or `make deploy`, which creates
   the portal and the symlinks for you). `debug_portal` reports both links.
2. Jog through the list — the keybed follows the highlighted file (live
   preview), so you hear each one before you commit. Click to commit, back out
   to cancel.
3. Play the pads. Every pad plays that one file, as a piano: white keys on
   rows 1 and 3, black keys on rows 2 and 4. The C at the end of row 1 is the
   sample at its own speed. There is no second sample on the keybed.
4. `STRT` / `END` trim that file and the pads draw its waveform.
5. `CLR` returns the keybed to the shipped sample.
6. Power-cycle and reload the Set: your assignment is still there, and every
   knob still shows the value the sound is at (Volume 0.80, not 0).

If you want the whole loop without a Move:

```sh
make host
g++ -std=c++17 -O1 -D_Static_assert=static_assert \
    -I module/dsp/third_party/schwung -o /tmp/probe tools/sample_map_probe.cpp -ldl
/tmp/probe build/chompi_move.so . /tmp/tone.wav
```

It renders a 1 kHz tone, assigns it, tears the plugin down and back up, then
measures which file played. Expect `PASS`, with the assigned tone at ~90% of
the block's energy and the shipped sample ~10%.

### Any WAV or AIFF a user brings

The engine streams raw bytes and assumes 16-bit stereo starting at byte 44;
your file is usually neither. The port parses the container at open and hands
the engine a converted view of the file (`module/dsp/port/sample_view.h`), so
format, channel count and chunk layout stop mattering:

```sh
make host tools
python3 tools/make_sample_variants.py /tmp/wavfmt/samples
mkdir -p /tmp/wavfmt/samples && cp samples/*.wav /tmp/wavfmt/samples/
build/sample_format_probe build/chompi_move.so /tmp/wavfmt /tmp/wavfmt/samples
```

Expect `PASS` — every variant plays the tone it holds. What it covers: 8/16/24/
32-bit PCM, float32/float64, WAVE_FORMAT_EXTENSIBLE, LIST/bext chunks before
`data`, mono (which used to play at double speed), multichannel, truncated
files, u-law and A-law; AIFF and AIFF-C (`sowt`, `fl32`, `ulaw`, signed 8-bit);
reverse playback, start/end regions and looping *on a converted file*; IMA
ADPCM going silent instead of noisy; and the shipped card still being read
raw. `get_param("debug_view")` (or `debug_voice`) names the files that needed
translating, and says so when one could not be decoded.

The browser offers `.wav`, `.aif` and `.aiff` (`sample_file`'s own filter).
Anything else — MP3, FLAC, Ogg — has no decoder here yet: rename it to `.wav`
and it will be silent rather than noisy, and `debug_view` will name it.

---

## What a failure looks like

| Symptom | Where to look |
| --- | --- |
| `dlopen` error mentioning `GLIBC_2.2x not found` | toolchain: rebuild with `ZIG=1` (targets 2.17), confirm with `make check` |
| `make: *** No rule to make target 'probe'` | you are not inside `schwung-chompi/`, or on `main` instead of the `arena/…` branch — see step 0 |
| `cd: /data/UserData/…/chompi-probe: No such file or directory` | `make probe` did not run (it creates that directory) — fix step 0 first |
| Module missing from the list, nothing in the log | wrong directory/`id`/filename — must be `<id>/dsp.so` |
| Loads, but no knobs / no pad layout | `serve_params` on the device (level 1): `chain_params`, `ui_hierarchy` |
| **No pages at all after a deploy** — one preset page reading "no presets" | the hierarchy JSON did not parse, and the host throws the whole document away rather than drawing half of it. `./hierarchy_probe ./dsp.so .` in the probe directory names the document and prints `VERDICT: FAIL`; a stray quote in one level's param list did exactly this |
| Knobs work, keys silent | `key_probe` on the device — `1`s + `peak=0` is the streaming path |
| Sound went away and never came back | ask for `debug_voice`: a voice with `p1` and `op1` is one whose open failed and stuck. Deleted/renamed sample files do this; P12/P13 in `VENDORED.md` are the fix |
| Silent after playing the *top* of the keybed (or after sweeping `slot`/`bank`), fine again after a reload | a `_double` companion that is not there was opened as a 0-byte placeholder and streamed forever — P23 in `VENDORED.md`. Check `ls -la …/chompi/samples/ \| grep double` for a 0-byte file; the fixed reader ignores it and plays the base sample (rebuild/redeploy if you are on an older `.so`) |
| One key is silent, its neighbours play | that slot's file is gone or unreadable — the assignment still points at it, so clear it (`CLR`) or reassign |
| Only the first key or two make sound, the rest of the Samples page is silent | the device is still on the three bundled WAVs — `make deploy-full` (this is the classic one; step 0 has the whole story) |
| In JAMMI, `slot`/`bank` seem to do nothing | they bind on the **next key press** by design (VENDORED P14/P17), and slot 15 is the RAM buffer, not a file — press a key after turning the knob, and check `debug_voice`'s summary line (`mode=… bank=… sel=…`) |
| Switching JAMMI → CUBBI goes silent | CUBBI pads need their slot's file: with only the bundled samples the first two octaves play (pads 68–91); pads 92–99 are slot 3 and stay silent until the card set is there. `make deploy-full`, then check `debug_files` for `cubbi/0:110000000000001…` |
| Everything works, output clips | `volume`/`gain`/`saturate`; CHOMPI's defaults are hot |
| Browser opens, picking a file does nothing | the read-back key: `ssh ableton@move.local` then ask the plugin — `sample_file` must show the path, `sample_map` the assignment |
| Assignment gone after a power cycle | `<module dir>/samples/samples.map` — check it exists and the module dir is writable |
| Clicks under load, fine otherwise | filesystem streaming — report the `debug_files` reading and the timing |
| Chain host crashes on load | send `debug.log` — likely an ABI mismatch, so include the Schwung version |
| **The Move crashes or reboots when you remove the plugin from the chain** | a thread of ours outliving `dlclose` — `destroy_instance` joins the worker for exactly this (README "Instance lifecycle"); reproduce without a device with `tools/unload_probe` |

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
