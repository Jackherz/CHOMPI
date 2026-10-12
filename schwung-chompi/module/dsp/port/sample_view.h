/**
 * port/sample_view.h — read the user's WAV, whatever it is.
 *
 * The vendored engine streams raw bytes and assumes the file is 16-bit stereo
 * with the audio starting at byte 44 (`sizeof(WAV_FormatTypeDef)`); every
 * region calculation is `f_size() - 44`, and the reader hands the bytes to
 * `s162f()` untouched. CHOMPI's own card is exactly that, so on hardware the
 * assumption costs nothing. A user's sample usually is not:
 *
 *   24-bit or 32-bit or float, mono, WAVE_FORMAT_EXTENSIBLE, a LIST/INFO
 *   chunk before `data` — play any of those raw and you hear noise.
 *
 * Breakbeat solves this at load: it parses the chunks, works out the sample
 * format, and plays the file through a decoded view of it. This is the same
 * idea at the port's own boundary — the POSIX FatFS shim. `f_open()` parses
 * the header, and if the file is not in the engine's native layout it gets a
 * SampleView: `f_size()`/`f_tell()`/`f_lseek()` then speak in *virtual* bytes
 * (a canonical 44-byte header plus int16 stereo frames) and `f_read()`
 * converts on the way out. Nothing on disk is touched, no copy is written, and
 * the engine — including all of its region, loop and reverse arithmetic — sees
 * exactly the format it was written for.
 *
 * All of this runs on the I/O worker thread, never on the audio thread.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace chompi
{
/** One translated file. Owned by the FIL that opened it. */
struct SampleView;

/** Parses `fd` (a file of `file_size` bytes at `path`, used for the log).
 *
 *  Returns nullptr when the file needs no translation: it is already 16-bit
 *  stereo PCM starting at byte 44, or it is not a WAV this code understands
 *  (an empty file, a recording in progress, MP3s renamed .wav) — in which case
 *  the engine sees the raw bytes exactly as it did before.
 *
 *  A WAV whose codec we cannot decode still gets a view: it becomes silence
 *  rather than a burst of noise, and `SampleViewDescribe` says what it was. */
SampleView* SampleViewOpen(int fd, int64_t file_size, const char* path);

/** The file size the engine must see (header + converted frames). */
int64_t SampleViewSize(const SampleView* v);

/** Fills `dst[0..n)` from virtual offset `vpos`. Returns bytes written; a
 *  short count means EOF. */
size_t SampleViewRead(SampleView* v, int64_t vpos, void* dst, size_t n);

/** Short human-readable description ("24-bit stereo -> 16-bit stereo"). */
const char* SampleViewDescribe(const SampleView* v);

void SampleViewFree(SampleView* v);

} // namespace chompi
