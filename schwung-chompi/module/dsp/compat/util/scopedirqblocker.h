/**
 * compat/util/scopedirqblocker.h — the firmware used this to make FIFO hand-off
 * between the audio callback and the SD callback atomic.
 *
 * On Move those two contexts are real threads, so the guarantee lives in
 * daisy_fifo.h (acquire/release indices) instead of an interrupt mask. The
 * include path is kept so FileStreamingManager.cpp stays unmodified.
 */
#pragma once

namespace daisy
{
namespace util
{
class ScopedIrqBlocker
{
  public:
    ScopedIrqBlocker() {}
    ~ScopedIrqBlocker() {}
};
} // namespace util
} // namespace daisy
