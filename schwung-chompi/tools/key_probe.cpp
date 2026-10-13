/*
 * tools/key_probe.cpp — no Move, no UI: does a key press make sound?
 *
 *   ./key_probe [path/to/dsp.so] [module_dir] [seconds] [mode]
 *                              ^ "." default           ^ 1    ^ cubbi
 *
 * Polls `debug_files` (slot-presence flags + whether the I/O queue has work),
 * presses C3 (note 48 → slot 1 in CUBBI mode) and renders in paced
 * 128-frame blocks, reporting the peak. A present slot that renders silence
 * is the signature of the streaming path being broken; peak 0 with an absent
 * slot is just a missing sample.
 *
 * `module_dir` is what `create_instance()` gets, so on the device pass the
 * installed module directory and the default `<module_dir>/samples` applies.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "host/plugin_api_v1.h"

static uint8_t mailbox[4096];

int main(int argc, char** argv)
{
    const char* so   = argc > 1 ? argv[1] : "build/chompi_move.so";
    const char* dir  = argc > 2 ? argv[2] : ".";
    double      secs = argc > 3 ? atof(argv[3]) : 1.0;
    const char* mode = argc > 4 ? argv[4] : "cubbi";

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

    static host_api_v1_t h;
    memset(&h, 0, sizeof(h));
    h.api_version      = MOVE_PLUGIN_API_VERSION;
    h.sample_rate      = MOVE_SAMPLE_RATE;
    h.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    h.mapped_memory    = mailbox;
    h.audio_out_offset = MOVE_AUDIO_OUT_OFFSET;
    h.audio_in_offset  = MOVE_AUDIO_IN_OFFSET;

    auto  api  = init(&h);
    void* inst = api->create_instance(dir, NULL);

    /* CUBBI is the demanding path (streams a sample off the filesystem at
     * note-on); JAMMI plays from the RAM buffer. */
    api->set_param(inst, "voice_mode", strcmp(mode, "jammi") == 0 ? "jammi" : "cubbi");
    api->set_param(inst, "bank", "0");
    printf("mode=%s dir=%s so=%s\n", mode, dir, so);

    char buf[256];
    /* The slot scan runs on the plugin's worker; give it a moment. */
    for(int i = 0; i < 6; i++)
    {
        usleep(200 * 1000);
        int n = api->get_param(inst, "debug_files", buf, sizeof(buf));
        printf("t=%dms n=%d %s\n", i * 200, n, n > 0 ? buf : "(no answer)");
    }

    /* Press C3 and render — paced like the callback (128 frames / 2.9 ms). */
    uint8_t on[3] = {0x90, 48, 100};
    api->on_midi(inst, on, 3, MOVE_MIDI_SOURCE_INTERNAL);

    const int blocks = (int)(secs * MOVE_SAMPLE_RATE / MOVE_FRAMES_PER_BLOCK);
    int16_t   out[256];
    long      peak = 0;
    for(int b = 0; b < blocks; b++)
    {
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        for(int i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++)
        {
            int v = out[i] < 0 ? -out[i] : out[i];
            if(v > peak)
                peak = v;
        }
        usleep(2900);
    }
    printf("after note: peak=%ld (%d blocks)\n", peak, blocks);

    api->get_param(inst, "debug_files", buf, sizeof(buf));
    printf("%s\n", buf);
    char err[160];
    if(api->get_error(inst, err, sizeof(err)) > 0)
        printf("err: %s\n", err);
    return peak > 0 ? 0 : 2;
}
