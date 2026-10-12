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

Expect four sections. The important ones:

| key | expected |
| --- | --- |
| `chain_params` | 2815 bytes, 35 `{…}` entries, *starts with `[` and ends with `]`* |
| `ui_hierarchy` | 1864 bytes, starts `{"pad_layout":"chromatic"…`, levels root/tape/looper/recorder/setup/samples |
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
   Then **clear it**: the Looper page's `Clear Loop` cell. It is a tape stop —
   the loop winds down over about a second and then stays silent; recording
   again afterwards must make a fresh loop (not a dub of the cleared one).
5. **Mode / bank / slot** (Setup page): with the full card set deployed, in
   **CUBBI** each key plays its own sample and the `bank` knob changes all of
   them (bank b's key 1 sounds different from bank a's). In **JAMMI** every key
   plays the *selected* slot at the note's speed: move `slot` and press a key,
   then move `bank` and press again — each press plays the newly selected
   sample, and `slot` 15 plays CHOMPI's built-in buffer. Switching
   JAMMI → CUBBI → JAMMI mid-phrase must not leave the module silent.
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

---

## 4. Sample import

Assign any WAV on the Move to any key, and it sticks across a power cycle.

```sh
cd ~/CHOMPI/schwung-chompi
make deploy ZIG=1 ZIG_BIN="/tmp/zigvenv/bin/python3 -m ziglang"
```

Then, with the chain loaded, open the module's editor and go to **Samples**:

1. `SLOT` → the slot you want to change (1–14; slot 15 is the recorder's
   buffer and is refused on purpose).
2. `SMPL` → the browser opens inside the **sample portal**,
   `/data/UserData/chompi-samples`, which holds two entries:

   * `[Built-in]` — the card set that ships with the module
   * `[User Library]` — your own samples
     (`scp myloop.wav ableton@move.local:/data/UserData/UserLibrary/Samples/`
     to put one there)

   The browser is fenced to that directory: `..` at the top of it does nothing.
   If `[User Library]` is missing, the directory above does not exist on the
   device — create it, then reopen the module (or `make deploy`, which creates
   the portal and the symlinks for you). `debug_portal` reports both links.
3. Jog through the list — the slot follows the highlighted file (live
   preview), so you hear each one before you commit. Click to commit, back out
   to cancel. If you delete or rename an assigned file afterwards, that key
   goes silent and everything else keeps playing — the assignment is kept, so
   only that slot is affected.
4. Play the key for that slot (slot 1 = C3): it plays your file.
5. `STRT` / `END` now trim *that* file and the pads draw its waveform.
6. `CLR` returns the slot to the shipped `samples/cubbi_aN.wav`.
7. Power-cycle and reload the Set: your assignment is still there.

If you want the whole loop without a Move:

```sh
make host
g++ -std=c++17 -O1 -D_Static_assert=static_assert \
    -I module/dsp/third_party/schwung -o /tmp/probe tools/sample_map_probe.cpp -ldl
/tmp/probe build/chompi_move.so . /tmp/tone.wav
```

It renders a 1 kHz tone, assigns it, tears the plugin down and back up, then
measures which file actually played. Expect `PASS`, with the assigned tone at
~90% of the block's energy and the shipped sample ~10%.

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
| Sound went away and never came back | ask for `debug_voice`: a voice with `p1` and `op1` is one whose open failed and stuck. Deleted/renamed sample files do this; P12/P13 in `VENDORED.md` are the fix |
| One key is silent, its neighbours play | that slot's file is gone or unreadable — the assignment still points at it, so clear it (`CLR`) or reassign |
| Only the first key or two make sound, the rest of the Samples page is silent | the device is still on the three bundled WAVs — `make deploy-full` (this is the classic one; step 0 has the whole story) |
| In JAMMI, `slot`/`bank` seem to do nothing | they bind on the **next key press** by design (VENDORED P14/P17), and slot 15 is the RAM buffer, not a file — press a key after turning the knob, and check `debug_voice`'s summary line (`mode=… bank=… sel=…`) |
| Switching JAMMI → CUBBI goes silent | CUBBI keys need their slot's file: with only the bundled samples there are two playable keys (C3/D3). `make deploy-full`, then check `debug_files` for `cubbi/0:110000000000001…` |
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
