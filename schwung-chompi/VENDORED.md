# Vendored CHOMPI source — what was changed and why

Everything under `module/dsp/engine/` is CHOMPI TAPE's DSP, copied from
`../firmware/chompi-tape/code/src/` and patched only where the Move's POSIX
environment differs from the Daisy Seed's firmware. `module/dsp/compat/`
holds the stand-ins that let the firmware's libDaisy/FatFs/DaisySP calls
compile and run unchanged.

**Original files are never edited**: the firmware directory stays a pristine
checkout, so re-vendoring after a CHOMPI release is a copy plus the patches
below.

## The port patches

Each patch carries its label as a `PORT PATCH Pn` comment at the site.

| # | File | Change |
| --- | --- | --- |
| P1 | `DSPEngine.h` | do not include/port `PresetManager` (FatFS + coreJSON); the engine never uses it |
| P2 | `DSPEngine.h`, `Sampler.h` | `#include "Limiter.h"` → `"limiter.h"` (case-sensitive FS) |
| P3 | `Warble.h` | `SetDelay(10u)` → `SetDelay(size_t(10))` (ambiguous on LP64) |
| P4 | `chompi_port.h` + 3 call sites | bare SD-root filenames → `chompi::SamplePath()`; adds a runtime sample directory |
| P5 | `RamBuffer.h` | `kMaxRamBuffSize` becomes `CHOMPI_RAM_BUFFER_SAMPLES` (build-time; firmware hardcodes the 32 MB SDRAM arena) |
| P6 | `third_party/daisysp/Filters/svf.cpp` | DaisySP assumes its own root on the include path |
| P7 | `SampleReader.h` | `SetStartPoint`/`SetEndPoint` accept a value before the async OPEN lands (see below) |
| P8 | `FileStreamingManager.cpp` | initialise `fres` — the past-EOF branch reaches the `fres == FR_OK` test without assigning it |
| P9 | `DSPEngine.h` | `GetFileNameForSlot` delegates to `chompi::SlotSamplePath` so every path in the engine honours the slot override |
| P10 | `DSPEngine.h` | `EraseStart` unlinks only files the port owns (`IsManagedPath`); `erasing` counts queued requests, not loop iterations |
| P11 | `compat/chompi_platform.cpp` | `f_open` refuses to create or truncate files outside the module's own sample directory |
| P12 | `compat/chompi_platform.cpp` | `f_open` closes the previous descriptor when a file object is re-opened (the engine skips CLOSE for empty files; each skip leaked a kernel fd) |
| P13 | `SampleReader.h` + `FileStreamingManager.cpp` | a failed open calls the new `FileOpenFailed()`, releasing the pending-open count instead of wedging the voice |
| P14 | `DSPEngine.h` | JAMMI binds the selected sample on the key path (it used to open a file only from `SetVoiceSlot()`, which the port's slot knob does not drive — so JAMMI played whatever the voice last held and the Mode/Bank/Slot knobs looked dead) |
| P15 | `DSPEngine.h` | FX routing: `fx_pre_loop_target` remembers what was asked for, and the crossfade lands on that instead of on "the other side", so two quick flips end where the user left it |
| P16 | `DSPEngine.h` | `ClearLooper()` — the hardware's record+play gesture as one call, for the `loop_clear` parameter |
| P17 | `DSPEngine.h` | `SelectVoiceSlot()` — record the selected slot without opening files, so the Samples page's slot knob can steer JAMMI without the sweep of opens that used to wedge a reader |
| P18 | `chompi_port.h/.cpp` + `chompi_move.cpp` | the sample portal: a browser root (`/data/UserData/chompi-samples`) whose entries are symlinks to the shipped samples and the user's library, with paths presented through it and stored beside it |
| P19 | `chompi_port.h/.cpp` + `DSPEngine.h` | per-(mode, bank, slot) values in the port's own store (`samples/slots.values`), loaded on the engine's key path instead of the hardware's preset store, so the Tape page follows the slot |
| P20 | `DSPEngine.h` | a CUBBI key press no longer calls `SetVoiceSlot()` — one voice, opened once, and a slot with no file no longer takes the whole keybed silent |
| P21 | `port/sample_view.h/.cpp` + `compat/chompi_platform.cpp` | a file-byte-addressed converter in front of the engine's `f_read`: WAV at any rate/bit depth and AIFF/AIFC in, canonical 16-bit stereo out; the plain RIFF case keeps the raw fast path |
| P22 | `SampleReader.h`, `DSPEngine.h`, `chompi_port.h/.cpp`, `chompi_move.cpp` | one slot = one octave of the keybed: the pads are a chromatic ladder and every pad sounds, a pad's octave picks the slot (`KeyRequest::slot_hint_`), and a sample with no `_double` companion keeps its own pitch above 1.5× (see below). P27 later moved the pitch itself onto the note |
| P23 | `SampleReader.h` | a 2x companion must be a *different*, non-empty file: the reader verifies it on disk at the moment the open would use it, and a companion that lands empty is dropped in favour of the sample (see below) |
| P24 | `LooperEngine.h`, `DSPEngine.h` | one press is one toggle: the port's record cell is a two-state control, and a write to it is measured against the pass that is RUNNING or still REQUESTED (`WantsRecord()`), so a repeated write cannot stop the take it just started (see below) |
| P25 | `chompi_move.cpp`, `LooperEngine.h`, `DSPEngine.h` | `looper_scrub` is a tape PLAYHEAD, not the hardware's scrub wheel: the write is a turn against the value last handed the host, `get_param` answers the head's position, and `MoveLooperHead()` moves the read head at once — playing or parked (see below) |
| P26 | `Sampler.h`, `LooperEngine.h`, `DSPEngine.h`, `chompi_move.cpp` | the tape has a window: `looper_start` / `looper_end` (the cells `Loop Start` / `Loop End`) are fractions of the take, derived per block into the read head's bounds, and the head turns over between them instead of at the ends of the whole take (see below) |
| P27 | `chompi_move.cpp` | the keybed's pitch is the NOTE minus the root (pad 80), not an index into a table of pads: every pad one semitone from the next, no two pads on one pitch, and Move's octave and transpose buttons move the whole keyboard with the notes they send (see below) |

P4's call sites are the three places the engine names a file of its own —
`DSPEngine.h:606` (`temp_rec.wav`), `DSPEngine.h:1355` (slot files
`cubbi_a4_double.wav` …), `LooperEngine.h:32` (`looper.wav`). Each already
had a 256-byte buffer; the patch only replaces the literal with the
directory-prefixed path. `chompi::g_sample_dir` is defined once, in
`module/dsp/port/chompi_port.cpp`, and is clamped to 200 bytes so a long
module path can never truncate a sample name into a different one.

### P7 in detail

The firmware's UI calls `SetVoiceSlot()` long before a key press, on a
thread the file worker used to have already finished opening the file. The
port's key path does both back-to-back on the callback, so
`SetEndPoint(1.0f)` can run while `f_size()` is still 0. Both of the
function's guards then degenerate — the size-derived endpoint wraps to 0 —
and the setter returns without recording anything, leaving the reader
pointing at byte 44 of a file it thinks is empty. The patch records the
request instead of rejecting it while the size is unknown; the firmware's
own `FileOpened()` already re-runs both setters once the open lands, so the
values are re-derived from the real size with no extra bookkeeping. Without
it, sampled playback is silent and the reader issues an endless
seek-to-start loop (the symptom: `debug_files` shows the slot present,
`key_probe` reports peak 0).

### P9–P10 in detail

The port keeps a small override table: the UI can point any of slots 1–14 at
an arbitrary WAV, and the map is persisted next to the samples so it survives
a reload. Each patch closes one way the engine could sidestep it. Before P9
the engine built convention names (`cubbi_a1.wav`, …) in several places —
key playback, the RAM load behind `SetVoiceSlot`, `EraseStart`, the file-exists
sweep — so an assigned sample played in some paths and not others depending on
which voice mode and trigger you used. `GetFileNameForSlot` is now the single
funnel and asks the port for the path. P10 keeps the reverse direction honest:
`EraseStart` deletes sample files, and it must not delete a user's imported
WAV that merely happens to be the slot's target. It unlinks only paths under
the port's sample directory, and `erasing` now tracks outstanding erase
requests rather than iterations of the polling loop (the old counter could
report "done" while an unlink was still queued).

### P11 in detail

The engine opens samples `FA_OPEN_ALWAYS`, which in FatFS means "create it if
it is missing" — CHOMPI's own behaviour on its own card, and the reason a
missing convention file turns into a 0-byte placeholder that the slot scan and
`debug_files` can see. The port can point a slot at any file on the device, so
that same open could land outside the module's directory: play a slot whose
assigned file has since been deleted and a 0-byte stub would appear in the
user's sample library, next to the sample it was named after. The compat
layer now asks `chompi::IsManagedPath()` before it passes `O_CREAT`/`O_TRUNC`
through. Inside the module's directory nothing changes; outside it, a missing
file is missing and the slot is silent, which is the honest answer.
`tools/sample_map_probe` pins both halves (a user path stays missing, a
convention path still becomes a placeholder) and cleans up the empties it
causes, so a local run leaves the working tree clean.

### P12–P13 in detail

These two are a pair, and together they are what "it went silent and only came
back after I restarted the module" turned out to be.

`SampleReader::OpenFile` skips the CLOSE when the file it holds is empty
(`f_size() == 0` — FatFS's CLOSE handler and the reader both test it), and the
engine re-opens the same `FIL` in place. FatFS is a library, so skipping the
close cost the firmware nothing; on Linux a `FIL` owns a descriptor, so every
re-open of an empty file leaked one. Empty files are exactly the missing slots
(on the card profile only some slots have samples), so playing keys and turning
the slot knob both leaked, one descriptor per voice per event, and the leak
ends in `f_open` failing — `EMFILE`.

A failing open was the second half. The reader counts a pending open before it
queues the request and only clears that count when the open lands, and
`open_requests_ != 0` gates the streaming refill *and* the reader's own
recovery path in `StartPlaying()`. So one unreadable file did not silence one
slot: the voice kept its envelope gate, stopped streaming, and never recovered.
Seven of those and the module was mute until it was recreated — a deleted or
renamed assigned sample is enough, as is any slot whose file cannot be opened.

P12 closes the previous descriptor before adopting the new one; P13 releases the
count and drops the handle when an open fails, so that slot is simply silent and
the voice is usable again. `tools/slot_knob_probe` measures both: the descriptor
count stays flat across a slot sweep and a fast key sweep, and a key whose file
is gone is silent while its neighbours keep playing.

`FileStreamingManager` also gained two diagnostics that came out of this
(a `CountingFifo` that counts refused `PushBack`s — every call site ignores the
answer — and `Engine::DescribeVoices`, served as `debug_voice`). Both are
additive; P12/P13 are the behaviour changes.

### P14–P17 in detail

The batch that made the Setup page and the looper behave like the instrument.

**P14 — JAMMI had no way to reach its own samples.** Upstream JAMMI opens a file
in exactly one place: `SetVoiceSlot()`, which the hardware UI calls when its slot
knob picks a tape. The port's slot knob is the Samples page's selection and
deliberately does not rebind voices (a swept knob's burst of opens was wedging
readers — that is why the knob is selection-only in the first place), so JAMMI
had no bind at all: every key replayed whatever file the voice last held. A
fresh module therefore played the built-in RAM tone for slot 15 and nothing else
could change it — the Mode, Bank and Slot knobs were inert in JAMMI, which is
what "switching to cubbi makes the sound disappear" and "bank and slot do
nothing" turned out to be.

JAMMI now binds in the key path, mirroring CUBBI: the whole keyboard plays the
selected jammi sample, the note sets the speed, and the bind happens once per
selection change (bound mode/bank/slot compared against the voice's, plus the
existing `copy_occurred` signal that a sample assignment raises), never once per
key press. Slot 15 keeps its upstream meaning — the recording buffer — and now
tags the voice too, so moving between the buffer and a card slot and back works
in both directions.

**P17** is the other half: `SelectVoiceSlot()` gives the port a way to *record*
the selection (which the Samples page and JAMMI both need) without the opens P14
makes unnecessary. `SetVoiceSlot()` is untouched and still the one call that
does both, for the paths that want that.

**P15 — the FX routing crossfade could land on the wrong side.** `fx_pre_loop`
was flipped by the fade's completion (`fx_pre_loop = !fx_pre_loop`), so asking
for `pre` while a fade to `post` was in flight was a no-op and the engine ended
on `post`. It now follows `fx_pre_loop_target`, set by `SetFxPreLooper()`; the
fade still decides *when* the routing changes, but not *which* side it lands on.
`chompi_move.cpp` only forwards a change when the requested value differs, so a
knob that re-writes the same option costs nothing.

**P16** exposes the clear. The hardware clears by holding record+play until
`CheckReset()` times out; a chain host has no pair of buttons to hold, so
`loop_clear` calls the same `LooperEngine::Reset()` the gesture ends in. It is
audible as a tape stop — the sampler winds its speed down and drains the remains
of the loop (~1 s) — which is upstream behaviour, not a port artefact.

Also in this batch, in the adapter rather than the firmware: momentary enums
(`sample_clear`, `loop_clear`) accept both the option name ("Clear") and the
index ("1") the shadow UI writes when it has no value to read back, the module
starts on slot 1 instead of the RAM buffer so it makes a musical sound out of
the box, and `debug_voice` gained a summary line (`mode=… bank=… sel=… fx_pre=…
loop_empty=…`) because the only other way to tell a dead knob from a dead bind
was to listen.

### P18 in detail

Where the file browser starts, and where the fence is.

A `filepath` parameter in Schwung carries a `root`, the browser is clamped to it
(`isWithinRoot`), and it opens on the parameter's current value, else
`start_path`, else the root. So a root of `/data` lets the user wander into
system directories, while a root of just the user's library hides the samples
that ship with the module — and neither is a good answer for a module whose
sample set lives in its own install directory.

The portal is the same fix mestela's `schwung-breakbeat` uses:

```
/data/UserData/chompi-samples/
├── Built-in       -> <module dir>/samples
└── User Library   -> /data/UserData/UserLibrary/Samples
```

One root, both sources as entries inside it, nothing else reachable. It is
created at `create_instance` (idempotent, and `Built-in` is re-pointed whenever
`sample_dir` changes) rather than by the deploy script, because a module
installed from a store tarball never runs one. The two paths are also
parameters (`portal_dir`, `user_library_dir`) so a host build can put them
somewhere writable; `make deploy` creates the same layout on the device.

Paths are then **presented through the portal and stored beside it**:

* `get_param("sample_file")` answers `/data/UserData/chompi-samples/Built-in/
  cubbi_a1.wav` — which is both what the browser seeds itself with (so it opens
  on that file's own folder) and what the host reads to draw the `wav_position`
  waveform (a symlink resolves).
* `set_param("sample_file", …)` runs it through `CanonicalSamplePath()` first,
  so the map keeps real paths. That is not cosmetic: the host writes the value
  it was given straight back when the user cancels, and the port treats "the
  path this slot already plays by convention" as a clear. Without
  canonicalisation that cancel would look like a new assignment and pin a
  symlink route (or, if the module moved, a dead one) into the map.

Two smaller things came out of the same change:

* `CanonicalSamplePath()` rewrites only paths that name a portal entry. A
  relative path like `./samples/cubbi_a1.wav` — which is exactly what the
  browser's cancel writes back on a host build, and what the convention names
  look like when `sample_dir` is relative — passes through untouched. Re-basing
  every relative path on the portal looked tidier and broke the cancel case.
* `ForceSymlink()` makes the link target absolute with `realpath()` first. A
  relative target is resolved against the *link's* directory, so
  `portal/Built-in -> ./samples` points at `portal/samples` and dangles — which
  is what happened the first time this ran with a relative `sample_dir`.

### P22 in detail

What a slot means on a keybed, and the one file that is missing.

The firmware's CUBBI keybed gives every one of its 25 keys a slot of its own
(`KeyToSlot()`), so a sample lives on **one pad** and its neighbours belong to
other samples. On Move's 32 pads that mapping does not even survive contact:
the pads are MIDI 68–99, the port folded them into the hardware's C3–C5 window,
and the fold made slots 1–8 unreachable while slots 9–15 appeared three times
across the four rows. What the user saw was the second copy of a slot, and a
sample assigned to slot 1 that no pad could play.

So a slot became an OCTAVE, which is also how the reference implementation
(`schwung-munchi-tape`) lays its pads out:

* The pads are a chromatic ladder on the 4×8 grid: pad 68 is the C the selected
  slot starts on, the pads count up by semitone left to right and bottom to top,
  and every pad sounds. Notes that are not pads (a MIDI keyboard, or a host
  transpose) continue the same ladder.
* Twelve pads are a slot, so the `slot` knob owns pads 68–79, pads 80–91 are
  the next slot up and 92–99 the first eight of the one after that, which is why
  the same pad can play a different slot after the knob moves — hence the slot
  test added to the CUBBI bind condition, and hence `KeyRequest::slot_hint_`:
  the port resolves the slot, the engine no longer assumes `KeyToSlot(key)` when
  a hint is present.
* Inside an octave the pad is the pitch (0..11 semitones, 0 at the octave's C),
  fed through the engine's existing `MidiNoteToPlaybackRatio()`, and JAMMI keeps
  its whole-keybed pitch, measured from pad 80 (twelve pads up) at 1.0× — so pad
  80 is a C and a 1.00× in both voices. P27 replaced both of those readings with
  one rule read off the note; the ladder, and pad 80 as its root, are what
  survived.

The layout the user chose is deliberately NOT the reference's: Munchi Tape puts
a two-octave piano on the same 4×8 grid, and a piano needs pads for keys that
do not exist. Twelve pads to a slot has no such pads, and it is also what makes
a slot's span exact.

The second half of the patch is what that pitch exposes. Above 1.5× the reader
opens `<name>_double.wav` — the card ships one beside every sample, as the same
audio at twice the speed (half the frames, the content an octave up: measured
140 Hz in `cubbi_a1.wav` against 280 Hz in its double) — and
`fname_rx_dbl` for an ASSIGNED sample resolves to the same file as its normal
name. The reader then claimed double speed and played that file at half rate:
an octave low, measured at 321 Hz where 642 was due, and exactly the kind of
"wrong sound in the upper half of the keybed" the octave map would have made
routine. `SampleReader::double_available` (cleared by
`chompi::SlotHasDoubleSpeedCopy()` when a slot carries an assignment) makes the
1.5× swap conditional on the companion actually existing, so an assigned sample
plays its high keys from the one copy it has, at the right pitch.

### P23 in detail

The companion that was not there.

Above 1.5× the reader plays `<name>_double.wav`, the card's prepared copy of the
sample at twice the speed. Two ways that assumption failed on the Move — both
ending in silence:

* **An assigned sample has no companion, and its own name for one.**
  `SlotSamplePath()` returns the slot override for `dbl = false` *and*
  `dbl = true`, so the engine asked for the sample as its own companion, opened
  it, and read it at half rate: a user's sample played an octave low (the probe
  of the time measured 882 Hz where 1764 was due). `tools/keybed_octave_probe`
  covers the state directly now: its 3.00× pad plays 1321.5 Hz, and would read
  an octave low if the sample were opened as its own companion.
* **A missing companion looks present.** The port's FatFS-compatible `f_open`
  creates a 0-byte placeholder for a name that is not there (the shipped
  firmware relies on that behaviour for its own sample directory), so the
  reader's open "succeeded" on an empty file: the FIFO never filled, the voice
  stayed "playing", and the module went silent until it was reloaded. With the
  three samples this repo ships, `tools/slot_knob_probe`'s step 8 (a sweep of
  high pads, then a low one) wedged exactly that way, and the slot knob could
  no longer bring any sound back.

The reader now decides where the decision is used, in `OpenFile()`:

* a companion that is the *same path* as the sample is not a companion —
  `strcmp` of the two names the engine derived, and the voice never swaps onto
  it;
* otherwise, and only when this open would really use it (ratio > 1.5×), the
  companion is verified with `f_stat` and must exist with a non-zero size.
  Presses below 1.5× never pay for the call, and a slot that answers "no
  companion" stops being asked;
* and if one still lands empty — a file removed between the stat and the open —
  `FileOpened()` (on the worker thread, where the open completes) drops it,
  zeroes the crossfade counter so the read rate is the plain varispeed from the
  next sample on, and re-opens the sample. `FileOpenFailed()` does the same for
  a companion that will not open at all.

Measured after the patch: the keybed's octave checks were 1764/882 Hz again on
the probe of the time (today's probe reads the ladder instead — its
`a 0-byte companion is dropped` check is this state), `slot_knob_probe` passes
on a tree with no companions and leaves no 0-byte `_double` behind, and a burst
of pads 88–99 that used to end at `rms=0.0000` (five presses later still silent)
now ends at 0.0196 with the next press at 0.0205.

### P24 in detail

One press, one toggle.

The Looper page's record cell is a two-state control -- `rec` while a record pass
runs, `lorec` when the looper is idle -- rather than the momentary trigger it used
to be (`options: ["-","Trig"]`, with `get_param` always answering `-`, so the
cell could never say `rec` no matter what the looper was doing). A stateful cell
is written as a STATE: the host reads the value back and writes the value it
shows, and it may write it more than once for a single press (a commit, a repaint
of the value it just read).

`LooperEngine::record` is set the frame after `ToggleRecord()` asks for a pass
(`record_target` -> `CheckRecordReady()` on the audio thread), so a second write
arriving inside that window used to toggle the take straight back off -- the
recording the user had just started stopped, and an overdub pass with it. The
port now measures the write against the INTENT, `record || record_target`
(`LooperEngine::WantsRecord()`), and toggles only when the state asked for is not
the state the looper is in. The hardware has the same shape: its key is a
momentary whose `RecordButton()` remembers whether a press has been handled
(`toggle_record`) until it is, because a button held while `CheckReset()` polls
must not toggle once per poll.

The scrub encoder's fix lives in the same port: `set_param("looper_scrub")` kept
the knob's absolute position in `Saved` (so the cell reads it back and the knob
stops snapping home) and sends only the DIFFERENCE to the engine, whose
`FileSampler::SetScrub(addl)` accumulates the hardware wheel's turns
(`turn_count_ += addl`) and turns them into a rate every 1/8 s. Passing the
absolute position in re-applied the whole value on every detent, so the tape
crept away instead of following the wheel. (P25 moved the port's knob onto the
read head and left `SetScrub` to the hardware wheel alone.)

Measured by `tools/looper_probe` on a continuous take (so the read head's
position is unambiguous): a turn of +2.0 moved the tape 0.4617 -> 0.5157, the
same position written again moved it 0.0025 (the tail of the first turn's slew),
and `looper_scrub` read back `2.0000`. P25 below is where the knob's meaning
changed from a rate nudge to a move of the head; the probe's numbers there are
the current ones.

### P25 in detail

The knob that moves the tape, not the wheel that nudges it.

`looper_scrub` used to hand the engine the knob's difference and let
`FileSampler::SetScrub` turn it into a speed for a while (P24). The user asked
for a control that "moves to a different part of the loop in real time, like
moving a tape playhead", so the knob is now exactly that: one unit of the knob
is one pass of the loop, the write is a TURN (the difference from the value last
handed the host), and the engine applies it as a move of the read head —
`MoveLooperHead()`, at once while the loop plays, and a move of where the next
play starts while it is parked.

The turn is what is sent because the host writes ABSOLUTE values: it reads the
cell, adds the turn, and writes the sum. `get_param("looper_scrub")` answers the
head's own position (0..1 of the loop, `0.0000` when nothing is recorded), so
the base the host adds to is where the head really is, and the knob cannot
creep. The hardware's rate nudge (`FileSampler::SetScrub`, `turn_count_ +=
addl`) is untouched in the engine and unused by this knob.

Measured by `tools/looper_probe`: an empty loop has no playhead (`0.0000`); a
parked turn puts the head where it was sent and the cell reads it (0.1933 ->
0.2520, cell `0.2520`); and a turn while the loop plays moves the head at once
(0.3134 -> 0.7500) instead of changing the speed it plays at.

The knob is no longer on the Looper page — the window's two bounds took its job,
and a negative Loop Pitch is the reverse control — but the key stays served so
an existing Set still round-trips.

### P26 in detail

The tape has a window.

Modelled on `kliegsablaze/forgetful` (see `LOOPER-MODEL.md` in the session's
notes): the looper's two bounds are `looper_start` / `looper_end`, FRACTIONS of
the take (0..1, step 0.01), named `Loop Start` / `Loop End` so the host draws
them as bounds rather than as a playback cursor. `FileSampler` keeps them as
written and derives the sample positions per block, keeping the pair even so a
stereo pair stays a pair and with a floor (50 ms) so the window cannot be closed
to nothing. The read head turns over between the bounds instead of at the ends
of the whole take, and the take's edge fade follows the window's edges.

The ORDER of the pair is enforced at use, never in the setter: a knob that snaps
back under the hand reads as a broken control rather than as a limit. Today a
crossed pair (End below Start) is the span between the two, played in the tape's
direction; running the window BACKWARDS is the other half of the model and needs
the direction glide that goes with it — a 0.35 s pass through zero, in its own
state field.

`tools/looper_probe` measures the window in LAPS of the take — whole tape 2
laps, half the tape 4, a window past the tone 0 — because levels cannot tell a
window from a tape; it also reads the pair back as written (0.6000 / 1.0000) and
the crossed pair as written (0.6000 / 0.2000).

### P27 in detail

The keybed reads the note.

One sample, every pad, a pitch per pad: P22 laid a chromatic ladder on the 4×8
grid and took a pad's pitch from its place in the ladder, with a 32-entry table
indexed by `note - 68`. The pads' note numbers are not the module's, though —
Move's octave buttons shift all 32 by twelve and Move's transpose shifts them by
semitones — so the table folded the shifted notes back onto pitches the
unshifted pads already played, and the pads that ran off the end of it were read
as some other key entirely. The user's words: "on a real keyboard there aren't
any notes that play the same pitch."

The rule is now the note itself — `keybed_pitch(note) = note - 80` — read from
the pads and from anything else that sends notes, so every pad is exactly one
semitone from its neighbour, no two pads share a pitch, and the whole keyboard
moves by exactly what moved (Move's octave buttons, Move's transpose, and the
module's own `octave` knob, which adds 12 semitones a step on top). Pad 80 is
the sample at its own speed; pad 68 is 0.50× and pad 99 is 3.00×, the tape's
whole speed range — which is why the root is pad 80 and not the bottom-left pad.

The keybed is pinned to one file (JAMMI, bank 0, slot 1) so nothing else can put
a second sample under a pad; mode, bank and slot stay served for old Sets and
are no longer drawn. `tools/keybed_octave_probe` sweeps all 32 pads at rest, at
++12 and at −12, checks the module's `octave` knob, checks that mode, bank and
slot cannot change what plays, and checks the three `_double` states above 1.5×.

Measured (44.1 kHz engine on a 48 kHz 480 Hz file, so 1.00× is 441 Hz): pad 68
220.5, pad 80 441.0, pad 99 1321.5 — the same ladder at every shift. Against the
build from before the patch, 29 / 30 / 31 of the 32 pads were on the wrong pitch
at rest / +12 / −12 (pad 80 read 312.0 Hz where 441.0 was due).

## Warnings the compiler still reports (all benign)

Building with clang (which is what zig uses) is noisier than the firmware's
GCC. These come from CHOMPI's own code and are **not** patched:

| warning | why it is fine |
| --- | --- |
| `-Wvla-cxx-extension` — `float monitor[2][size]` in `DSPEngine.h:448` | a C99 VLA used in C++; both GCC and clang accept it, and `size` is always `CHOMPI_BLOCK` at the call sites |
| `-Wunknown-attributes` — `__attribute__((optimize("-O0")))` on `IsManagerEmpty()` | a GCC-only attribute clang ignores; the function only reads a flag the worker maintains, so the optimisation level is not load-bearing |
| `-Wunused-private-field` — `loop_end`, `sr_`, `done_` | upstream dead members |
| `-Wnon-c-typedef-for-linkage` — `typedef struct { … } FIL` | our compat shim; the anonymous struct is given a name by the typedef, which is the intent |

None of them change code generation in a way that matters; the port builds
clean under GCC (`make host`) and warning-light under clang.

## The compat layer (`module/dsp/compat/`)

| File | Purpose |
| --- | --- |
| `fatfs.h` | `FIL` as a POSIX fd wrapper (`fd`/`size`/`tell`/`in_use`, `obj.objsize`) plus FRESULT enums — enough for the engine's open/read/write/seek/close path |
| `chompi_platform.cpp` | the FatFs calls (`f_open`, `f_read`, `f_write`, `f_lseek`, `f_stat`, `f_size`, `f_tell`, `f_eof`) and the libDaisy bits (`System::`, `GPIO`, `SdmmcHandler`) implemented on POSIX file I/O |
| `daisy_fifo.h` | `Fifo<T, capacity>` with libDaisy semantics (`T buffer_[capacity+1]`, `capacity` usable slots). This must match upstream exactly: the port originally had a ring that reported "empty" when full, which stalled the 8 KB sample refills — the bug behind silent CUBBI streaming |
| `daisy.h`, `daisy_seed.h`, `daisysp.h`, `daisy_core.h`, `util/scopedirqblocker.h` | declarations the engine expects, no-ops where there is no hardware |

`f_stat` maps to `::stat` on the host filesystem, which is why synthetic WAVs
placed in `samples/` are picked up by the slot scan
(`UpdateFileExists()`: modes × banks × slots 1–14, slot 15 forced present).

## DaisySP

`module/dsp/third_party/daisysp/` holds the three DSP units the engine
actually instantiates — `Control/adsr`, `Utility/dcblock`, `Filters/svf` —
copied from the firmware's own `third_party`, plus the headers they need. The
port builds them verbatim; only `Filters/svf.cpp` needed its include path
fixed (P6-adjacent, build-only).

## Schwung ABI headers

`module/dsp/third_party/schwung/host/plugin_api_v1.h` is staged from
`charlesvestal/schwung`. v1 and v2 types both live in that one header;
`plugin_api_v2.h` does not exist. Note it uses C11 `_Static_assert`, so every
translation unit that includes it needs `-D_Static_assert=static_assert`
(already in the Makefile).
