/*
 * render_test.cpp — offline harness for the CHOMPI Move module.
 *
 * It loads the module the same way the device does (dlopen + dlsym
 * "move_plugin_init_v2"), hands it a fake host_api_v1_t with a mailbox, and
 * renders blocks into a WAV file. Two scenarios:
 *
 *   --scenario ram     play the built-in sample (CHOMPI's RAM buffer, slot 15)
 *   --scenario cubbi   stream cubbi_a1.wav through the worker thread + FIFOs
 *   --scenario record  record the line input, then play it back
 *
 * Because the module's file I/O lives on a second thread (exactly as on the
 * device), the renderer paces itself in real time by default: burst-rendering
 * would starve the FIFOs and measure nothing.
 */
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "host/plugin_api_v1.h"

#define SR     44100
#define BLOCK  MOVE_FRAMES_PER_BLOCK
#define MAILBOX_BYTES (4096 + MOVE_AUDIO_OUT_OFFSET)

typedef plugin_api_v2_t* (*init_v2_fn)(const host_api_v1_t*);

static double now_s()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static const host_api_v1_t* g_host;
static uint8_t*             g_mailbox;

static int      g_voices;                 /* overlapping notes for the chord test */

struct Wav
{
    FILE* f;
    uint32_t frames;
};

static void wav_open(Wav* w, const char* path)
{
    w->f = fopen(path, "wb");
    if(!w->f)
    {
        perror("fopen");
        exit(1);
    }
    uint8_t hdr[44] = {0};
    memcpy(hdr, "RIFF", 4);
    uint32_t file_size = 0; // patched on close
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t sub1 = 16;
    uint16_t fmt  = 1;
    uint16_t ch   = 2;
    uint32_t rate = SR;
    uint32_t byterate = SR * 2 * 2;
    uint16_t align = 4;
    uint16_t bits  = 16;
    memcpy(hdr + 16, &sub1, 4);
    memcpy(hdr + 20, &fmt, 2);
    memcpy(hdr + 22, &ch, 2);
    memcpy(hdr + 24, &rate, 4);
    memcpy(hdr + 28, &byterate, 4);
    memcpy(hdr + 32, &align, 2);
    memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    fwrite(hdr, 1, 44, w->f);
    (void)file_size;
    w->frames = 0;
}

static void wav_write(Wav* w, const int16_t* interleaved, int frames)
{
    fwrite(interleaved, sizeof(int16_t), (size_t)frames * 2, w->f);
    w->frames += frames;
}

static void wav_close(Wav* w)
{
    uint32_t data_bytes = w->frames * 2 * 2;
    uint32_t riff_size  = data_bytes + 36;
    fseek(w->f, 4, SEEK_SET);
    fwrite(&riff_size, 4, 1, w->f);
    fseek(w->f, 40, SEEK_SET);
    fwrite(&data_bytes, 4, 1, w->f);
    fclose(w->f);
}

static void note_on(plugin_api_v2_t* p, void* inst, int note, int vel)
{
    uint8_t msg[3] = {0x90, (uint8_t)note, (uint8_t)vel};
    p->on_midi(inst, msg, 3, MOVE_MIDI_SOURCE_INTERNAL);
}

static void note_off(plugin_api_v2_t* p, void* inst, int note)
{
    uint8_t msg[3] = {0x80, (uint8_t)note, 0};
    p->on_midi(inst, msg, 3, MOVE_MIDI_SOURCE_INTERNAL);
}

static void looper_record_pulse(plugin_api_v2_t* p, void* inst)
{
    p->set_param(inst, "looper_record", "1");
}

int main(int argc, char** argv)
{
    const char* so_path     = (argc > 1) ? argv[1] : "module/dsp/chompi_move.so";
    const char* out_path    = (argc > 2) ? argv[2] : "chompi_render.wav";
    const char* scenario    = (argc > 3) ? argv[3] : "ram";
    const char* sample_dir  = (argc > 4) ? argv[4] : "samples";
    const double seconds    = (argc > 5) ? atof(argv[5]) : 4.0;

    void* lib = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if(!lib)
    {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    init_v2_fn init = (init_v2_fn)dlsym(lib, "move_plugin_init_v2");
    if(!init)
    {
        fprintf(stderr, "no move_plugin_init_v2: %s\n", dlerror());
        return 1;
    }

    static uint8_t static_mailbox[MAILBOX_BYTES];
    memset(static_mailbox, 0, sizeof(static_mailbox));
    g_mailbox = static_mailbox;

    static host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.api_version       = MOVE_PLUGIN_API_VERSION;
    host.sample_rate       = SR;
    host.frames_per_block  = BLOCK;
    host.mapped_memory     = g_mailbox;
    host.audio_out_offset  = MOVE_AUDIO_OUT_OFFSET;
    host.audio_in_offset   = MOVE_AUDIO_IN_OFFSET;
    g_host                 = &host;

    plugin_api_v2_t* api = init(&host);
    if(!api)
    {
        fprintf(stderr, "init returned NULL\n");
        return 1;
    }

    void* inst = api->create_instance(".", NULL);
    if(!inst)
    {
        fprintf(stderr, "create_instance returned NULL\n");
        return 1;
    }

    api->set_param(inst, "sample_dir", sample_dir);
    api->set_param(inst, "volume", "0.85");
    api->set_param(inst, "filter", "0.55");
    api->set_param(inst, "saturate", "0.25");
    api->set_param(inst, "reverb", "0.35");
    api->set_param(inst, "delay_feedback", "0.4");

    if(strcmp(scenario, "cubbi") == 0)
    {
        api->set_param(inst, "voice_mode", "cubbi");
        api->set_param(inst, "bank", "0");
    }
    else
    {
        /* The ram scenario is about CHOMPI's built-in RAM buffer, which is
         * slot 15 (the module now starts on slot 1, i.e. a real sample of the
         * selected bank -- see the note in chompi_move.cpp). */
        api->set_param(inst, "slot", "15");
    }

    Wav wav;
    wav_open(&wav, out_path);

    int16_t out[BLOCK * 2];
    char    buf[64];

    /* Give the worker a moment to run the sample-directory scan. */
    usleep(50 * 1000);
    if(api->get_param(inst, "voice_mode", buf, sizeof(buf)) > 0)
        fprintf(stderr, "voice_mode=%s ", buf);
    fprintf(stderr, "scenario=%s out=%s\n", scenario, out_path);

    const int  total_blocks = (int)(seconds * SR / BLOCK);
    const double t_start    = now_s();

    /* The note sequence: a small CHOMPI-ish phrase. */
    /* In CUBBI mode only the 15 "white" keys address sample slots: C3 (48) is
     * KEY_1 -> slot 1, D3 (50) is KEY_2 -> slot 2, and so on. The demo phrase
     * walks those two slots; the JAMMI phrase is a normal melodic run. */
    const int phrase_jammi[] = {60, 64, 67, 72, 71, 67, 64, 60};
    const int phrase_cubbi[] = {48, 50, 48, 50, 48, 50, 48, 50};
    const int* phrase = (strcmp(scenario, "cubbi") == 0) ? phrase_cubbi : phrase_jammi;
    int       phrase_idx     = 0;
    int       blocks_per_note = 8;

    for(int b = 0; b < total_blocks; b++)
    {
        /* Feed Move's line input with a test tone in the record scenarios so
         * the recorder and the monitor path have something to chew on. */
        int16_t* ain = (int16_t*)(g_mailbox + MOVE_AUDIO_IN_OFFSET);
        const bool inject = (strcmp(scenario, "record") == 0);
        for(int i = 0; i < BLOCK; i++)
        {
            int16_t s = 0;
            if(inject)
            {
                const float t = (float)(b * BLOCK + i) / SR;
                const float v = 0.25f * sinf(2.f * (float)M_PI * 220.f * t);
                s = (int16_t)(v * 32000.f);
            }
            ain[2 * i]     = s;
            ain[2 * i + 1] = s;
        }

        if(strcmp(scenario, "record") == 0)
        {
            /* two bars: arm the recorder, then play it back */
            if(b == 4)
                api->set_param(inst, "record", "1");
            if(b == (int)(1.5 * SR / BLOCK))
                api->set_param(inst, "record", "0");
            if(b == (int)(2.0 * SR / BLOCK))
                note_on(api, inst, 60, 110);
            if(b == (int)(2.6 * SR / BLOCK))
                note_off(api, inst, 60);
        }
        else
        {
            if(b % blocks_per_note == 0 && b < total_blocks - 16)
            {
                if(g_voices > 0)
                    note_off(api, inst, phrase[phrase_idx % 8]);
                note_on(api, inst, phrase[phrase_idx % 8], 100 + (phrase_idx % 3) * 10);
                g_voices        = 1;
                phrase_idx++;
            }
            else if(b % blocks_per_note == 4 && phrase_idx > 1)
            {
                note_off(api, inst, phrase[(phrase_idx - 1) % 8]);
            }
        }

        memset(out, 0, sizeof(out));
        api->render_block(inst, out, BLOCK);
        wav_write(&wav, out, BLOCK);

        /* pace to real time: the module's file I/O is a second thread */
        const double target = t_start + (b + 1) * (double)BLOCK / SR;
        const double now    = now_s();
        if(target > now)
            usleep((useconds_t)((target - now) * 1e6));
    }

    wav_close(&wav);
    fprintf(stderr, "rendered %u frames to %s\n", wav.frames, out_path);

    char err[160];
    if(api->get_error(inst, err, sizeof(err)) > 0)
        fprintf(stderr, "module error: %s\n", err);

    api->destroy_instance(inst);
    dlclose(lib);
    return 0;
}
