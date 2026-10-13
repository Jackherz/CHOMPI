/**
 * compat/daisy.h — the slice of libDaisy that CHOMPI's DSP engine touches,
 * re-implemented for Linux (the Ableton Move module host).
 *
 * PROVIDES
 *   daisy::System::GetNow()/Delay()/DelayUs()      (millisecond clock)
 *   daisy::FIFO<T,N>                               (thread-safe, see daisy_fifo.h)
 *   f2s16()/s162f() and friends                    (daisy_core.h value conversions)
 *
 * Nothing here is hardware specific, so the vendored engine sources compile
 * unchanged. See PORTING.md for the full platform-layer inventory.
 */
#pragma once

#include <chrono>
#include <cstring> // upstream relied on libDaisy to pull this in (strcpy in SampleReader)
#include <cstdint>
#include <thread>

#include "daisy_core.h"
#include "daisy_fifo.h"

namespace daisy
{

class System
{
  public:
    /** Milliseconds since boot. Matches libDaisy's uint32_t wrap-around. */
    static uint32_t GetNow();

    /** Blocking sleep — never call from the audio callback. */
    static void Delay(uint32_t ms)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    static void DelayUs(uint32_t us)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(us));
    }
};

} // namespace daisy
