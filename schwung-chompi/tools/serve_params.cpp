#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include "host/plugin_api_v1.h"
int main(int argc, char** argv){
    void* lib = dlopen("build/chompi_move.so", RTLD_NOW);
    auto init = (plugin_api_v2_t* (*)(const host_api_v1_t*))dlsym(lib, "move_plugin_init_v2");
    static host_api_v1_t host; memset(&host, 0, sizeof host);
    host.api_version = MOVE_PLUGIN_API_VERSION; host.sample_rate = MOVE_SAMPLE_RATE;
    host.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    static uint8_t mb[8192]; host.mapped_memory = mb;
    host.audio_out_offset = MOVE_AUDIO_OUT_OFFSET; host.audio_in_offset = MOVE_AUDIO_IN_OFFSET;
    plugin_api_v2_t* api = init(&host);
    void* inst = api->create_instance("module", NULL);
    static char buf[16384];
    for(int i=1;i<argc;i++){ int n=api->get_param(inst, argv[i], buf, sizeof buf); printf("### %s (%d)\n%s\n", argv[i], n, n>0?buf:"(none)"); }
    return 0;
}
