/* unload_probe — does removing the plugin from a chain crash the host?
 *
 * The chain host's unload path is destroy_instance() followed immediately by
 * dlclose() (shadow_chain_mgmt.c: fx_slot_unload_impl — "destroy the instance,
 * dlclose, forget the identity", with nothing in between). This module owns a
 * worker thread, so if any of our code is still running when the library is
 * unmapped, the host dies on the next instruction fetch — which is what
 * "Move crashes whenever I remove the plugin from the chain" looks like from
 * the outside.
 *
 * This probe performs that exact sequence, several times, and then keeps
 * running: a crash here is the device crash, reproduced.
 *
 * Build (from schwung-chompi/):
 *   g++ -std=c++17 -O1 -D_Static_assert=static_assert \
 *       -I module/dsp/third_party/schwung -o /tmp/unload_probe \
 *       tools/unload_probe.cpp -ldl
 * Run:
 *   /tmp/unload_probe build/chompi_move.so .
 */

#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "host/plugin_api_v1.h"

static uint8_t mailbox[8 * 1024 * 1024] __attribute__((aligned(4096)));

typedef plugin_api_v2_t* (*init_fn)(const host_api_v1_t*);

/* One full load / play / remove cycle, the way the host does it. */
static int cycle(const char* so, const char* dir, int index)
{
    void* lib = dlopen(so, RTLD_NOW);
    if(!lib)
    {
        printf("cycle %d: dlopen: %s\n", index, dlerror());
        return 1;
    }
    init_fn init = (init_fn)dlsym(lib, "move_plugin_init_v2");
    if(!init)
    {
        printf("cycle %d: no move_plugin_init_v2\n", index);
        dlclose(lib);
        return 1;
    }

    static host_api_v1_t h;
    memset(&h, 0, sizeof h);
    h.api_version      = MOVE_PLUGIN_API_VERSION;
    h.sample_rate      = MOVE_SAMPLE_RATE;
    h.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    h.mapped_memory    = mailbox;
    h.audio_out_offset = MOVE_AUDIO_OUT_OFFSET;
    h.audio_in_offset  = MOVE_AUDIO_IN_OFFSET;

    plugin_api_v2_t* api  = init(&h);
    void*            inst = api->create_instance(dir, NULL);
    api->set_param(inst, "voice_mode", "cubbi");
    api->set_param(inst, "bank", "0");
    api->set_param(inst, "slot", "1");

    /* Play for a moment: this leaves the worker with real work to do (opens,
     * reads, the file scan), which is the state a crash needs. */
    int16_t out[256];
    const uint8_t on[3]  = {0x90, 48, 100};
    api->on_midi(inst, on, 3, MOVE_MIDI_SOURCE_INTERNAL);
    for(int b = 0; b < 200; b++)
    {
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        usleep(2900);
    }

    /* The removal, exactly as fx_slot_unload_impl does it. */
    api->destroy_instance(inst);

    /* This is the window the crash lives in: dlclose unmaps our text while the
     * worker thread is still scheduled. */
    dlclose(lib);

    /* Keep executing for a while. If a thread of ours was left inside the
     * unloaded library, it faults here, asynchronously, after the removal
     * looked complete. */
    usleep(150 * 1000);
    printf("cycle %d: removed and unloaded cleanly\n", index);
    return 0;
}

int main(int argc, char** argv)
{
    const char* so  = argc > 1 ? argv[1] : "build/chompi_move.so";
    const char* dir = argc > 2 ? argv[2] : ".";

    for(int i = 1; i <= 5; i++)
    {
        if(cycle(so, dir, i))
            return 1;
    }

    /* Two instances, one removed while the other keeps playing: the worker
     * must survive that (it is still serving), and the LAST removal must still
     * leave nothing of ours behind. */
    {
        void* lib = dlopen(so, RTLD_NOW);
        if(!lib)
        {
            printf("pair: dlopen: %s\n", dlerror());
            return 1;
        }
        init_fn init = (init_fn)dlsym(lib, "move_plugin_init_v2");

        static host_api_v1_t h;
        memset(&h, 0, sizeof h);
        h.api_version      = MOVE_PLUGIN_API_VERSION;
        h.sample_rate      = MOVE_SAMPLE_RATE;
        h.frames_per_block = MOVE_FRAMES_PER_BLOCK;
        h.mapped_memory    = mailbox;
        h.audio_out_offset = MOVE_AUDIO_OUT_OFFSET;
        h.audio_in_offset  = MOVE_AUDIO_IN_OFFSET;

        plugin_api_v2_t* api = init(&h);
        void*            a   = api->create_instance(dir, NULL);
        void*            b   = api->create_instance(dir, NULL);
        api->set_param(a, "voice_mode", "cubbi");
        api->set_param(b, "voice_mode", "cubbi");
        api->set_param(b, "slot", "2");

        int16_t out[256];
        const uint8_t on[3] = {0x90, 50, 100};
        api->on_midi(b, on, 3, MOVE_MIDI_SOURCE_INTERNAL);

        for(int i = 0; i < 100; i++)
        {
            api->render_block(a, out, MOVE_FRAMES_PER_BLOCK);
            api->render_block(b, out, MOVE_FRAMES_PER_BLOCK);
            usleep(2900);
        }

        api->destroy_instance(a);          /* one goes, the worker stays */

        for(int i = 0; i < 100; i++)
        {
            api->render_block(b, out, MOVE_FRAMES_PER_BLOCK);
            usleep(2900);
        }

        api->destroy_instance(b);          /* now everything must be gone */
        dlclose(lib);
        usleep(150 * 1000);
        printf("pair: second removal with another instance live, then the last — clean\n");
    }

    printf("\nPASS: five load/play/remove cycles survived (no thread outlived the library)\n");
    return 0;
}
