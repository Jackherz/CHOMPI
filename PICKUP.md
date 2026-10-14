# CHOMPI — what's next

*(Rewritten 2026-10-14 after the second sandbox wipe — the earlier copy, `LOOPER-MODEL.md` and
`WINDOW-DEVICE-CHECK.md` went with it. The P27 commit was recovered and verified exact; see
`RECOVER-P27.md`. Everything a session needs is also in the repo working tree under
`p27-recovery/`.)*

## State

- **`main`** = `21e1157` — PR #2 merged (`88f954e`, +1673/−86) on top of PR #1 (`b851126`).
  `main`'s tree is `e8e536b7…`, identical to `88f954e`'s tree.
- **P27 — the keybed — is on the remote now (2026-10-14).** The lost local commit `cc89729`
  (`10565eea…`, never pushed) was recreated from its diff as `b641400` on
  `arena/a0c40056-chompi` and opened as **PR #3** (base `main`): ten files byte-identical to
  `cc89729`'s tree (blob hashes match the diff's `index` lines), `chompi_move.cpp` re-based onto
  `main` and byte-identical too; `VENDORED.md` completed 2026-10-14 from the owner's surviving
  stash (blob `97ca7b5`, one stray `+` the chat paste had added) -- **all eleven files are now
  byte-identical to `cc89729`'s tree; the recovery is exact.** Host suite green in the recovery
  sandbox; against the pre-patch build the new `keybed_octave_probe` fails 14 checks with the old
  table's signature (pad 80 at 312.0 Hz where 441.0 was due). **Remaining: the device pass.** (`RECOVER-P27.md`, `DEVICE-RECIPE.md` and `p27-recovery/` died with the old sandbox;
  this file and the repo are what survived.)
- The old session branch `arena/237e5843-chompi` sits at `88f954e`, whose content is already inside
  `main`. Base new work on `main`.

## P27 — the keybed reads the note

The owner's words: *"on a real keyboard there aren't any notes that play the same pitch."* One rule:
`keybed_pitch(note) = note - 80`. Pad 68 is 0.50×, pad 80 the sample at its own speed, pad 99
3.00× — one semitone a pad, nothing repeated, and Move's octave/transpose move the whole keyboard.
Before the patch **29 / 30 / 31 of the 32 pads were on the wrong pitch** at rest / +12 / −12.
Numbering: the keybed is P27; the tape's window is P26 (`e7f2b41`); the moving head is P25.
Caveat (in the code): a pad held across Move's own octave/transpose press stops — the note is the
key, and that press renumbers it.

## Next, in order

1. **The device pass for P27** — §1 build + the two `grep -c` gates, §2 `keybed_octave_probe` on
   the Move, §3 the by-ear pass = `TESTING.md` §3 item 1. This closes P23's keybed check.
2. **The window's direction** (slice 2): END below START runs the tape backwards through the span,
   the flip gliding through zero (their 0.35 s), kept in its own field so the read-back cannot
   report a direction nobody selected. Today a crossed pair is only the span, played forwards.
3. **The one-button transport** — the read-back names the next press (STOP / DUB / PLAY).
4. **Item 4** (stop shipping `*_double.wav`; above 1.5× playback keeps using the original) **and
   item 5** (Tape's `end` renamed so it draws as a loop bound) — both small.
5. **Item 3** (resample: record what the pads play; the take becomes the one sample; no slots back).

## Device checks owed

1. The keybed (P27) — the probe run + by-ear, as above.
2. The reshaped Looper page, by eye: `REC LOPLAY LOOPIT DUGAIN STRT END CLLOOP`.
3. The Mac `make check-hierarchy` for `a7e0e3c` — never reported back.

## Notes for later

- **Dub gain** (implemented by the owner, `0c6a37d`, device-confirmed) — they say it "could be
  implemented better"; wait for them to raise it. Candidates noted before: the step edge the first
  pass writes into the take; the frame written twice when pitch ≠ 1 → a ramp + a per-frame guard.
- The Looper page has a free 8th knob cell if something wants it.

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
- Suite (13, all green at P27): keybed, sample_map, mode_bank 6/6, sample_format 34/34, env 35/35,
  looper, key, unload, slot_knob, param_readback, state_roundtrip, hierarchy, render.
- Give every probe its **own working directory**. One-sample convention: `kOneMode=0` (JAMMI),
  `kOneBank=0`, `kOneSlot=1` (`chompi_move.cpp`); slot-1 file = `jammi_a1.wav`.
- `key_probe` passes unmodified (`jammi/0:011111111111111-`); `key_probe.cpp` and
  `unload_probe.cpp` were untouched by P27.
- **This sandbox has been wiped twice** (2026-10-13 21:24, 2026-10-14 08:07): `/home/user` empties,
  the repo is re-cloned, and only the repo's own working tree survives. Keep nothing only in
  `/home/user`.
