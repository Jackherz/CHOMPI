/**
 * compat/daisy_core.h — minimal stand-in for libDaisy's daisy_core.h.
 *
 * Only the pieces CHOMPI's DSP engine actually uses are provided:
 *   - FORCE_INLINE
 *   - the fixed-point <-> float conversion helpers (f2s16 / s162f / f2u8 / u82f)
 *
 * Values match libDaisy (MIT, (c) Electrosmith) so audio produced off-device
 * is bit-identical to the firmware build.
 */
#pragma once

#include <cstdint>

#ifndef FORCE_INLINE
#define FORCE_INLINE inline __attribute__((always_inline))
#endif

#define FBIPMAX 0.999985f /**< close to 1.0f-LSB at 16 bit */
#define FBIPMIN (-FBIPMAX)

#define S162F_SCALE 3.05185094759971922971282082583086642048402356028931546983245338297677541e-05f /**< 1 / (2**15 - 1) */
#define F2S16_SCALE 32767.0f                                                                        /**< (2 ** 15) - 1 */

FORCE_INLINE float s162f(int32_t x)
{
    return (float)x * S162F_SCALE;
}

FORCE_INLINE int32_t f2s16(float x)
{
    x = x <= FBIPMIN ? FBIPMIN : x;
    x = x >= FBIPMAX ? FBIPMAX : x;
    return (int32_t)(x * F2S16_SCALE);
}

FORCE_INLINE float s242f(int32_t x)
{
    return (float)x * (1.0f / 8388607.0f);
}

FORCE_INLINE int32_t f2s24(float x)
{
    x = x <= FBIPMIN ? FBIPMIN : x;
    x = x >= FBIPMAX ? FBIPMAX : x;
    return (int32_t)(x * 8388607.0f);
}
