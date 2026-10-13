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
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    /* These are the engine's startup targets, not the minimum the host paints
     * when chain_params omits `default`. Volume at 0 while the sound was at
     * 0.8 was that bug. */
    expect(api, inst, "volume", "0.8000", "startup (engine mgain, was reported as 0)");
    expect(api, inst, "filter", "0.5000", "startup (engine cutoff)");
    expect(api, inst, "warble", "0.1000", "startup (engine SetFreq)");
    expect(api, inst, "delay_time", "0.5000", "startup (engine SetDelayTime)");
    expect(api, inst, "delay_feedback", "0.0000", "startup");
    expect(api, inst, "reverb", "0.0000", "startup");
    expect(api, inst, "saturate", "0.0000", "startup");
    expect(api, inst, "looper_pitch", "1.0000", "startup (not the -2 minimum)");
    expect(api, inst, "looper_scrub", "0.0000", "startup (not the -2 minimum)");
    expect(api, inst, "looper_dub_gain", "1.0000", "startup (engine dub gain)");
    expect(api, inst, "input_gain", "1.0000", "startup");
    expect(api, inst, "octave", "0", "startup (not the -2 minimum)");
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
    expect(api, inst, "input_monitor", "On", "enum name (monitor starts on)");
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

    /* The writes above moved two knobs. Put them back so the cross-check
     * below is comparing chain_params `default` to the startup value. */
    api->set_param(inst, "attack", "0");
    api->set_param(inst, "input_monitor", "1");

    /* The host paints chain_params `default` before (and, on some paths,
     * instead of) get_param. A control with no default shows its minimum —
     * Volume 0, Octave -2, Loop Pitch -2 — while the engine is elsewhere.
     * Every on-screen control therefore declares a default, and that default
     * is the value get_param returns at startup. */
    {
        char chain[8192];
        const int n = api->get_param(inst, "chain_params", chain, sizeof chain);
        if(n <= 0 || n >= (int)sizeof chain)
        {
            printf("FAIL  chain_params             missing or truncated (%d)\n", n);
            g_fails++;
        }
        else if(strstr(chain, "\"key\":\"slot\"") || strstr(chain, "\"key\":\"bank\"")
                || strstr(chain, "\"key\":\"voice_mode\""))
        {
            printf("FAIL  chain_params             still publishes slot/bank/mode\n");
            g_fails++;
        }
        else
        {
            const char* p = chain;
            int checked = 0;
            while((p = strstr(p, "\"key\":\"")) != nullptr)
            {
                p += 7;
                char key[64];
                int ki = 0;
                while(*p && *p != '"' && ki + 1 < (int)sizeof key)
                    key[ki++] = *p++;
                key[ki] = 0;
                const char* obj_end = strchr(p, '}');
                if(!obj_end)
                    break;
                const char* def = strstr(p, "\"default\":");
                if(!def || def > obj_end)
                {
                    /* Filepath has no numeric default; the cell is the path. */
                    if(strcmp(key, "sample_file") != 0)
                    {
                        printf("FAIL  %-22s no default in chain_params\n", key);
                        g_fails++;
                    }
                    continue;
                }
                def += 10;
                char got[128]; got[0] = 0;
                api->get_param(inst, key, got, sizeof got);
                const char* opt = strstr(p, "\"options\":[");
                if(opt && opt < obj_end)
                {
                    /* Enum default is an index. get_param answers with the name. */
                    const int idx = atoi(def);
                    const char* o = strchr(opt, '[');
                    const char* name = nullptr;
                    char ename[64] = {0};
                    for(int i = 0; o && i <= idx; i++)
                    {
                        o = strchr(o + 1, '"');
                        if(!o || o > obj_end)
                            break;
                        const char* e = strchr(o + 1, '"');
                        if(!e || e > obj_end)
                            break;
                        if(i == idx)
                        {
                            int len = (int)(e - (o + 1));
                            if(len > 63) len = 63;
                            memcpy(ename, o + 1, (size_t)len);
                            ename[len] = 0;
                            name = ename;
                        }
                        o = e;
                    }
                    const bool ok = name && strcmp(got, name) == 0;
                    if(!ok) g_fails++;
                    printf("%-6s %-22s chain default %d -> %s (get %s)\n",
                           ok ? "ok" : "FAIL", key, idx, name ? name : "?", got);
                }
                else
                {
                    const double want = atof(def);
                    const double have = atof(got);
                    const bool ok = fabs(have - want) < 0.001;
                    if(!ok) g_fails++;
                    printf("%-6s %-22s chain default %.4f (get %s)\n",
                           ok ? "ok" : "FAIL", key, want, got);
                }
                checked++;
            }
            if(checked < 20)
            {
                printf("FAIL  chain_params             only %d controls checked\n", checked);
                g_fails++;
            }
        }
        char hier[4096];
        const int h = api->get_param(inst, "ui_hierarchy", hier, sizeof hier);
        if(h <= 0 || h >= (int)sizeof hier)
        {
            printf("FAIL  ui_hierarchy            missing or truncated (%d)\n", h);
            g_fails++;
        }
        else if(strstr(hier, "slot") || strstr(hier, "Slot") || strstr(hier, "voice_mode")
                || strstr(hier, "bank"))
        {
            printf("FAIL  ui_hierarchy            still mentions slot/bank/mode\n");
            g_fails++;
        }
        else
            printf("ok     ui_hierarchy            no slot, bank or mode\n");
    }

    printf("\n%s (%d failed)\n", g_fails ? "FAIL: a knob does not read back" : "PASS: every knob reads back", g_fails);
    return g_fails ? 1 : 0;
}
