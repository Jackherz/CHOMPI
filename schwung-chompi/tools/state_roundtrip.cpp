#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include "host/plugin_api_v1.h"
int main(int argc, char** argv){
    const char* so = argc > 1 ? argv[1] : "build/chompi_move.so";
    void* lib = dlopen(so, RTLD_NOW);
    if(!lib){ printf("dlopen(%s): %s\n", so, dlerror()); return 1; }
    auto init = (plugin_api_v2_t* (*)(const host_api_v1_t*))dlsym(lib, "move_plugin_init_v2");
    static host_api_v1_t host; memset(&host, 0, sizeof host);
    host.api_version = MOVE_PLUGIN_API_VERSION;
    host.sample_rate = MOVE_SAMPLE_RATE; host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    static uint8_t mailbox[8192]; memset(mailbox, 0, sizeof mailbox);
    host.mapped_memory = mailbox;
    host.audio_out_offset = MOVE_AUDIO_OUT_OFFSET; host.audio_in_offset = MOVE_AUDIO_IN_OFFSET;
    plugin_api_v2_t* api = init(&host);
    void* inst = api->create_instance(argc > 2 ? argv[2] : "module", NULL);
    api->set_param(inst, "filter", "0.42"); api->set_param(inst, "voice_mode", "cubbi");
    api->set_param(inst, "octave", "-1"); api->set_param(inst, "reverb", "0.33");
    api->set_param(inst, "sample_dir", "samples");
    char buf[1024]; int n = api->get_param(inst, "state", buf, sizeof buf);
    printf("state[%d]=%s\n", n, n > 0 ? buf : "(none)");
    void* inst2 = api->create_instance(argc > 2 ? argv[2] : "module", NULL);
    api->set_param(inst2, "state", buf);
    char buf2[1024]; int n2 = api->get_param(inst2, "state", buf2, sizeof buf2);
    printf("restored[%d]=%s\n", n2, n2 > 0 ? buf2 : "(none)");
    printf("round-trip: %s\n", (n == n2 && strcmp(buf, buf2) == 0) ? "OK" : "MISMATCH");
    n = api->get_param(inst, "chain_params", buf, sizeof buf);  printf("chain_params len=%d\n", n);
    n = api->get_param(inst, "ui_hierarchy", buf, sizeof buf);  printf("ui_hierarchy len=%d\n", n);
    n = api->get_param(inst, "split_voices", buf, sizeof buf);  printf("split_voices len=%d\n", n);
    api->destroy_instance(inst2); api->destroy_instance(inst);
    return 0;
}
