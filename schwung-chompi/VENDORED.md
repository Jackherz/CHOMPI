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
