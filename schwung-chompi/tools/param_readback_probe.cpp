/* Item 2 of the port's fix list: every knob has to answer with the value it is
 * actually set to.
 *
 * The host draws each knob's position and its "edited" marker from get_param,
 * so a key that answers nothing -- or answers "0" -- shows up as a knob left at
 * zero. That is the report: "Volume is set at 0 when you first start the module
 * but is actually at full". This probe reads back the whole parameter set at
 * startup and after a write, and pins the two conventions:
 *
 *   - floats answer with the value (no rounding to something else);
 *   - enums answer with the option NAME, because the host matches that string
 *     against the parameter's `options` list (see param_on() in chompi_move.cpp
 *     -- the same rule in the write direction is why "On" used to mean OFF).
 *
 * Usage:  ./param_readback_probe build/chompi_move.so [state_dir]
 * Prints every reading; exits non-zero if any of the pinned ones is wrong. */
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include "host/plugin_api_v1.h"

static int g_fails = 0;

static void expect(plugin_api_v2_t* api, void* inst, const char* key,
                   const char* want, const char* what)
{
    char buf[256]; buf[0] = 0;
    api->get_param(inst, key, buf, sizeof buf);
    const bool ok = (strcmp(buf, want) == 0);
    if(!ok) g_fails++;
    printf("%-6s %-15s %-30s = %s (want %s)\n", ok ? "ok" : "FAIL", key, what, buf, want);
}

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
    char buf[256];
    const char* keys[] = {"input_monitor","record","sustain","auto_loop","reverse",
                          "looper_reverse","voice_mode","fx_pre_loop","input_source",
                          "gain","attack","decay","pan","start","end","varspeed","slot","bank","volume"};
    for(const char* k : keys){ buf[0]=0; api->get_param(inst, k, buf, sizeof buf);
        printf("       %-15s %-30s = %s\n", k, "default", buf); }

    /* The report, pinned: at startup these are the values, not zero. */
    expect(api, inst, "volume", "1.0000", "startup (was reported as 0)");
    expect(api, inst, "filter", "1.0000", "startup");
    expect(api, inst, "gain", "0.7040", "startup: the hardware's unity");
    expect(api, inst, "pan", "0.5000", "startup");
    expect(api, inst, "start", "0.0000", "startup");
    expect(api, inst, "end", "1.0000", "startup");
    expect(api, inst, "attack", "0.0000", "startup");
    expect(api, inst, "decay", "0.0000", "startup");
    expect(api, inst, "varspeed", "0.8300", "startup: 1x on the pitch encoder");
    expect(api, inst, "sustain", "On", "enum name");
    expect(api, inst, "auto_loop", "On", "enum name");
    expect(api, inst, "record", "Off", "enum name");
    expect(api, inst, "input_monitor", "Off", "enum name");
    expect(api, inst, "reverse", "Off", "enum name");
    expect(api, inst, "looper_reverse", "Off", "enum name");
    expect(api, inst, "voice_mode", "jammi", "enum name");
    expect(api, inst, "fx_pre_loop", "pre", "enum name");
    expect(api, inst, "input_source", "line", "enum name");
    /* Writing the option NAME and writing the INDEX must both mean the same
     * thing, and both must read back as the name. */
    api->set_param(inst, "input_monitor", "On");  expect(api, inst, "input_monitor", "On",  "after write \"On\"");
    api->set_param(inst, "input_monitor", "0");   expect(api, inst, "input_monitor", "Off", "after write \"0\"");
    api->set_param(inst, "input_monitor", "Off"); expect(api, inst, "input_monitor", "Off", "after write \"Off\"");
    api->set_param(inst, "input_monitor", "1");   expect(api, inst, "input_monitor", "On",  "after write \"1\"");

    api->set_param(inst, "attack", "0.75");   expect(api, inst, "attack", "0.7500", "after write");
    api->set_param(inst, "auto_loop", "Off"); expect(api, inst, "auto_loop", "Off", "after write");
    api->set_param(inst, "auto_loop", "On");  expect(api, inst, "auto_loop", "On", "after write");

    printf("\n%s (%d failed)\n", g_fails ? "FAIL: a knob does not read back" : "PASS: every knob reads back", g_fails);
    return g_fails ? 1 : 0;
}
