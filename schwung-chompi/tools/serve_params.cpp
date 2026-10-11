/*
 * tools/serve_params.cpp — dump what the chain host would see.
 *
 *   ./serve_params [path/to/dsp.so] [key ...]
 *                       ^ default build/chompi_move.so    ^ default the ABI keys
 *
 * Runs on the host against the host .so, or on the Move against the installed
 * dsp.so (cross-compile with `make tools-aarch64`) — which is the quickest way
 * to tell a device-side load problem from a UI-side one.
 */
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include "host/plugin_api_v1.h"

static const char* const kDefaultKeys[] = {"chain_params", "ui_hierarchy",
                                           "split_voices", "state"};

int main(int argc, char** argv)
{
    const char* so = argc > 1 ? argv[1] : "build/chompi_move.so";
    int first_key  = argc > 1 ? 2 : 1;

    void* lib = dlopen(so, RTLD_NOW);
    if(!lib)
    {
        printf("dlopen(%s): %s\n", so, dlerror());
        return 1;
    }
    auto init = (plugin_api_v2_t * (*)(const host_api_v1_t*)) dlsym(lib, "move_plugin_init_v2");
    if(!init)
    {
        printf("dlsym move_plugin_init_v2: %s\n", dlerror());
        return 1;
    }

    static host_api_v1_t host;
    memset(&host, 0, sizeof host);
    host.api_version      = MOVE_PLUGIN_API_VERSION;
    host.sample_rate      = MOVE_SAMPLE_RATE;
    host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    static uint8_t mb[8192];
    host.mapped_memory     = mb;
    host.audio_out_offset  = MOVE_AUDIO_OUT_OFFSET;
    host.audio_in_offset   = MOVE_AUDIO_IN_OFFSET;

    plugin_api_v2_t* api = init(&host);
    /* "." — the module dir on the device is wherever the .so lives. */
    void* inst = api->create_instance(".", NULL);

    static char buf[16384];
    const int   nkeys = argc - first_key > 0 ? argc - first_key
                                             : (int)(sizeof(kDefaultKeys) / sizeof(kDefaultKeys[0]));
    for(int i = 0; i < nkeys; i++)
    {
        const char* key = argc - first_key > 0 ? argv[first_key + i] : kDefaultKeys[i];
        int         n   = api->get_param(inst, key, buf, sizeof buf);
        printf("### %s (%d)\n%s\n", key, n, n > 0 ? buf : "(none)");
    }
    return 0;
}
