# CHOMPI — what's next

*(Rewritten 2026-10-14 after the second sandbox wipe — the earlier copy, `LOOPER-MODEL.md` and
`WINDOW-DEVICE-CHECK.md` went with it. The P27 commit was recovered and verified exact.
Updated later the same day for P28. Everything a session needs is in this file and the repo.)*

## State

- **`main`** = `95d6990` — PR #3 merged 2026-10-14 (P27 and this document), on top of
  PR #2 (`21e1157`) and PR #1 (`b851126`).
- **P28 — the window's direction — is on this session's branch `arena/40d0251a-chompi`**
  (rebased onto `main`, so it fast-forwards): commit `283b08d`, opened as **PR #4** (base
  `main`). Bench green 2026-10-14 (the whole host suite; the new looper_probe section fails 6
  checks against the P27 build with the old behaviour's signature). **Device pass owed** — see
  Device checks owed, item 0. Not merged until it is.
- **P27 — the keybed — is in `main` (PR #3, merged 2026-10-14).** The lost local commit `cc89729`
  (`10565eea…`, never pushed) was recreated from its diff as `b641400` on
  `arena/a0c40056-chompi` and opened as **PR #3** (base `main`): ten files byte-identical to
  `cc89729`'s tree (blob hashes match the diff's `index` lines), `chompi_move.cpp` re-based onto
  `main` and byte-identical too; `VENDORED.md` completed 2026-10-14 from the owner's surviving
  stash (blob `97ca7b5`, one stray `+` the chat paste had added) -- **all eleven files are now
  byte-identical to `cc89729`'s tree; the recovery is exact.** Host suite green in the recovery
  sandbox; against the pre-patch build the new `keybed_octave_probe` fails 14 checks with the old
  table's signature (pad 80 at 312.0 Hz where 441.0 was due). **Device pass 2026-10-14: green** --
  gates 1/1 on the Mac build, every probe green on the Move, and the by-ear pass done: the owner,
  "everything works as it should". P27 has nothing left owed. (`RECOVER-P27.md`, `DEVICE-RECIPE.md` and `p27-recovery/` died with the old sandbox;
  this file and the repo are what survived.)
- The old session branches: `arena/237e5843-chompi` (`88f954e`) and `arena/a0c40056-chompi`
  (`bb24f5c`) are both inside `main`. This session's branch is `arena/40d0251a-chompi`.

## P27 — the keybed reads the note

The owner's words: *"on a real keyboard there aren't any notes that play the same pitch."* One rule:
`keybed_pitch(note) = note - 80`. Pad 68 is 0.50×, pad 80 the sample at its own speed, pad 99
3.00× — one semitone a pad, nothing repeated, and Move's octave/transpose move the whole keyboard.
Before the patch **29 / 30 / 31 of the 32 pads were on the wrong pitch** at rest / +12 / −12.
Numbering: the keybed is P27; the tape's window is P26 (`e7f2b41`); the moving head is P25.
Caveat (in the code): a pad held across Move's own octave/transpose press stops — the note is the
key, and that press renumbers it.

## P28 — the window has a direction

The order of the pair is the direction: `Strt` below `End` plays the span forwards, `End` below
`Strt` plays the **same span backwards**. The knobs never swap meaning. The direction is its
**own field** in `FileSampler` (`window_dir_`, +1/−1), multiplied into the tape's rate at the one
place it is decided (the target in `PopStereoSamps`) — never the pitch's sign, so `Loop Pitch` /
`Loop Rev` go on reading what the user set, and a negative pitch on a crossed pair is forward
again. It **glides through zero** over 0.35 s (`kWindowFlipSeconds`, forgetful's number), a linear
ramp the tape's own slew follows; `reverse_` flips where it always did (the slewed rate crossing
zero). It advances once a block from `PerBlock(frames)`, whatever the transport is doing, so a pair
crossed while parked plays backwards from the first pass. One diagnostic key: `looper_window_dir`.
Nothing new is served: `chain_params` 3458 B / 34, `ui_hierarchy` 1760 B, as before.

## Next, in order

1. ~~**The device pass for P27**~~ — **done 2026-10-14** (see Device checks owed). P23's keybed
   check closed with it.
2. ~~**The window's direction**~~ (slice 2) — **built and bench-verified 2026-10-14 as P28**, PR #4.
   **Owed: the device pass** (Device checks owed, item 0). Then merge PR #4. **Next up after
   that: item 3.**
3. **The one-button transport** — the read-back names the next press (STOP / DUB / PLAY).
4. **Item 4** (stop shipping `*_double.wav`; above 1.5× playback keeps using the original) **and
   item 5** (Tape's `end` renamed so it draws as a loop bound) — both small.
5. **Item 3** (resample: record what the pads play; the take becomes the one sample; no slots back).

## Device checks owed

0. **The window's direction (P28)** — the recipe, for the Mac (zsh). Fetch and build, with the
   gates (two P28 gates added: the new key in `dsp.so`, the new section in the probe):

   ```sh
   cd ~/CHOMPI
   git fetch origin arena/40d0251a-chompi
   git merge FETCH_HEAD
   cd ~/CHOMPI/schwung-chompi
   make clean
   make check ZIG=1 ZIG_BIN="/tmp/zigvenv/bin/python3 -m ziglang"
   grep -c "Loop End" build/module/dsp.so
   grep -c "the tape playhead" build/aarch64/looper_probe
   grep -c "looper_window_dir" build/module/dsp.so
   grep -c "End below Start runs it backwards" build/aarch64/looper_probe
   make check-hierarchy
   ```

   All four greps must print `1`; `check-hierarchy` must end `VERDICT: PASS (0 failures)` (this
   also closes owed item 3 below, the Mac host run). Then the probes on the Move — `looper_probe`
   in a directory of its own, because it writes `looper.wav` beside the samples; it takes about
   two minutes, paced like the audio callback:

   ```sh
   make probe ZIG=1 ZIG_BIN="/tmp/zigvenv/bin/python3 -m ziglang"
   ssh ableton@move.local 'cd /data/UserData/schwung/chompi-probe && ./serve_params ./dsp.so chain_params ui_hierarchy | grep "###"'
   ssh ableton@move.local 'cd /data/UserData/schwung/chompi-probe && ./hierarchy_probe ./dsp.so . | tail -3'
   ssh ableton@move.local 'cd /data/UserData/schwung/chompi-probe && rm -rf lp && mkdir -p lp/samples && cp dsp.so lp/ && cp samples/*.wav lp/samples/ && cd lp && ../looper_probe ./dsp.so . 2>&1 | tail -32'
   ```

   Expected: `chain_params (3458)`, `ui_hierarchy (1760)`, `hierarchy_probe` PASS, and
   `looper_probe` `VERDICT: PASS (0 failures)` with the direction section reading, give or take
   the last digit: forward `dir 1.0000, head +1.00, melody +1.00`; the glide `at the write
   1.0000, 0.17 s in 0.0049, 0.57 s in -1.0000`; reversed `head -0.75 … melody -1.00`;
   `reverse Off, pitch 1.0000, start 0.8000, end 0.2000`; `head 0.204..0.777, 3 laps`;
   negative pitch on top `head +0.75, melody +1.00, reverse On`; parked `dir while parked
   -1.0000, head on play -0.75`. Then install and listen:

   ```sh
   make deploy ZIG=1 ZIG_BIN="/tmp/zigvenv/bin/python3 -m ziglang"
   ```

   By ear (TESTING.md §3 item 4, "The window's direction"): record a few distinct notes, set
   `Strt`/`End` to a span, then take `End` down past `Strt`. The tape slows, stops and runs back
   over about a third of a second — not a cut — and the notes come out in reverse order, the pass
   as long as it was forwards. `LOOPIT` still reads `1.0000`. `End` back above `Strt`: it turns
   round again. `LOOPIT` below zero on a crossed pair: forward again, and the pitch cell shows the
   reverse because you set it. Cross the pair while stopped, then play: backwards from the first
   pass, no turnaround. Report what is different, if anything; then PR #4 merges.
1. ~~The keybed (P27)~~ — **done 2026-10-14.** On the Mac, the gates: `grep -c "Loop End"
   build/module/dsp.so` = 1, `grep -c "the tape playhead" build/aarch64/looper_probe` = 1. On the
   Move: `serve_params` 3458 B / 34 entries and `ui_hierarchy` 1760 B; `hierarchy_probe` PASS
   (0 failures, 34 served / 32 named); `key_probe` peak=1211 over 689 blocks,
   `jammi/0:000000000000001-`; `state_roundtrip` OK; **`keybed_octave_probe` PASS (0 failed)** —
   32 pads at rest, Move-octave ±12, octave knob ±2, cubbi/bank/slot pinned, all three
   `_double` states, 220.5 / 441.0 / 1321.5 Hz, the desktop bench's numbers. By ear: "everything
   works as it should."
2. ~~The reshaped Looper page, by eye~~ — **done 2026-10-14**: reads
   `REC LOPLAY LOOPIT DUGAIN STRT END CLLOOP`.
3. The Mac `make check-hierarchy` for `a7e0e3c` — never reported back. (The same documents now
   also passed on the Move 2026-10-14, but the Mac host run remains the one owed.)

## Notes for later

- **Dub gain** (implemented by the owner, `0c6a37d`, device-confirmed) — they say it "could be
  implemented better"; wait for them to raise it. Candidates noted before: the step edge the first
  pass writes into the take; the frame written twice when pitch ≠ 1 → a ramp + a per-frame guard.
- The Looper page has a free 8th knob cell if something wants it.
- **A dub into a window smears the take** (found 2026-10-14 while building P28; it is P26's,
  not P28's, and P28 does not change it). The write head wraps at the TAKE's ends
  (`WriteLoop`/`WriteJumpStart`), the read head at the WINDOW's (`ReenterWindow`), so once the
  read head has re-entered, the FIFO's `old × gain + input` is written where the write head is —
  past the window's end, then from the top of the take — and each lap copies the window's content
  to a shifted place. Playback of a window is fine; it is a record pass over a window that drifts.
  Candidate fix: on `ReenterWindow()`, re-seat the write head on the read head the way
  `SetReverse()` does (clear the FIFO, `WriteJump(read head)`, `rev_pushback`), dropping ≤32
  samples inside the 480-sample edge fade. Wait for the owner to raise it, or fold it into the
  dub-gain item. `tools/looper_probe` sidesteps it by ending the dub pass a first take rolls into
  before it measures the window.

## Design reference (was `LOOPER-MODEL.md`)

Distilled from `kliegsablaze/forgetful` (re-clone if needed):

- **Window**: `start/end` fractions 0..1, stored unordered and ordered at *use*, derived per block
  into the head's bounds. The head re-enters at the edge it travels away from.
- **Direction**: END<START is the reversed window, with a through-zero glide
  (`WINDOW_FLIP_SECONDS 0.35`) in its **own** state field — never folded into the speed. Slice 2.
- Constants worth keeping: `MIN_WINDOW_MS 50`, `TAKE_EDGE_FADE_S 0.004`.
- **One button names the next press** (STOP / DUB / PLAY); names and value words ≤5 chars; a
  read-only *string* routes to the 2-char opaque renderer, an *enum* gets the 3-char square.
- Transients need a time window to be polled; enums must match magnitude **and** direction.
- Recording and playing are **one transport**.

## User constraints (standing)

- **One list item at a time, stop after each, hardware-verify between items.**
- **Hardware-first**: paste-ready commands for the Mac (zsh) — **no inline `#` comments**, `cd
  ~/CHOMPI/schwung-chompi` before `make` (the Makefile is not at the repo root), and never assume
  `origin/<branch>` exists (their `git fetch origin <branch>` writes FETCH_HEAD only — merge
  FETCH_HEAD, don't `git pull`).
- **Gate the device build** with `grep -c "Loop End" build/module/dsp.so` = 1 **and**
  `grep -c "the tape playhead" build/aarch64/looper_probe` = 1. A green host probe says nothing
  about the device, and vice versa.
- **Retired keys stay declared** in `chain_params` (served, undrawn) so old Sets round-trip.
- **Scrub** (`looper_scrub`) and **Loop Rev** are off the Looper page; the code paths stay as-is.
  `Clear Loop` is back on a knob. Negative Loop Pitch is the reverse control.
- **Item 3**: do not bring slots back; the take becomes the one sample.
- **Item 4**: never delete the originals in `firmware/card-profiles`.
- **Item 5**: fix in our names/keys, **not in the host**; "loop" + "end" both in the key or name.
- Rec cell stays **stateful** (`lorec`/`rec` live read-back) — approved on hardware as "much clearer
  now"; never back to a `["-","Trig"]` trigger.

## Bench (host)

- Build: `cd schwung-chompi && make host tools`; cross-build `make check ZIG=1 ZIG_BIN="…"` (zig
  0.17.0 via a venv on the Mac: `/private/tmp/zigvenv`).
- Suite (13, all green at P28): keybed, sample_map, mode_bank 6/6, sample_format 34/34, env 35/35,
  looper (now with the direction section; ~2 min), key, unload, slot_knob, param_readback,
  state_roundtrip, hierarchy, render (peaks 1364 / 279 / 3228 ram / cubbi / record, identical to
  `main`'s build). `env_probe` wants a directory of its own that it may clear — not the one the
  `.so` sits in; `sample_format_probe` needs `make_sample_variants.py` run first.
- Give every probe its **own working directory**. One-sample convention: `kOneMode=0` (JAMMI),
  `kOneBank=0`, `kOneSlot=1` (`chompi_move.cpp`); slot-1 file = `jammi_a1.wav`.
- `key_probe` passes unmodified (`jammi/0:011111111111111-`); `key_probe.cpp` and
  `unload_probe.cpp` were untouched by P27.
- **This sandbox has been wiped twice** (2026-10-13 21:24, 2026-10-14 08:07): `/home/user` empties,
  the repo is re-cloned, and only the repo's own working tree survives. Keep nothing only in
  `/home/user`.
