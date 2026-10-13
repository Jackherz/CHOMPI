/**
 * compat/daisysp.h — the DaisySP surface the vendored CHOMPI engine uses.
 *
 * Upstream CHOMPI includes the full DaisySP library. The engine only needs
 * four things from it (DcBlock, an ADSR, and the inline helpers in dsp.h:
 * fclamp / fonepole / SoftClip / SoftLimit), so the port vendors just those
 * files under third_party/daisysp (MIT, (c) Electrosmith).
 *
 * Everything else DaisySP offers — drum voices, physical models, etc. — was
 * only reachable from CHOMPI's UI pages, which the Move port replaces.
 */
#pragma once

#include "Utility/dsp.h"     // daisysp::fclamp / fonepole / SoftClip / SoftLimit / mtof / ...
#include "Utility/dcblock.h" // daisysp::DcBlock
#include "Utility/delayline.h" // daisysp::DelayLine (used by chompi::Warble)
#include "Filters/svf.h"     // daisysp::Svf (used by chompi::MicFilter)
#include "Control/adsr.h"    // daisysp::Adsr + ADSR_SEG_* enums
