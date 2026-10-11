# Vendored CHOMPI source — what was changed and why

Everything under `module/dsp/engine/` is CHOMPI TAPE's DSP, copied from
`../firmware/chompi-tape/code/src/` and patched only where the Move's POSIX
environment differs from the Daisy Seed's firmware. `module/dsp/compat/`
holds the stand-ins that let the firmware's libDaisy/FatFs/DaisySP calls
compile and run unchanged.

**Original files are never edited**: the firmware directory stays a pristine
checkout, so re-vendoring after a CHOMPI release is a copy plus the patches
below.

## The six patches

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
