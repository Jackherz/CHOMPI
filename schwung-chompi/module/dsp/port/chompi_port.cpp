/**
 * port/chompi_port.cpp — the one global the vendored engine needs (PORT PATCH P4).
 * Defined here rather than in chompi_move.cpp so the host harness can also
 * link the engine without the plugin.
 */
#include "chompi_port.h"

namespace chompi
{
char g_sample_dir[kSampleDirMax + 1] = {0};
}
