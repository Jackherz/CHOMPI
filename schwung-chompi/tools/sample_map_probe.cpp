/*
 * tools/sample_map_probe.cpp — prove an assigned sample is the one that plays.
 *
 *   ./sample_map_probe [so] [module_dir] [tone_path]
 *
 * The sample map (`set_param("sample_file", <path>)`) is the feature this
 * checks, end to end:
 *
 *   1. render slot 1 as shipped            -> the stock CUBBI sample
 *   2. assign a 1 kHz test tone to slot 1  -> the tone must now play
 *   3. clear the assignment                -> back to the CUBBI sample
 *
 * "Which file played" is measured, not assumed: the probe reports the energy
 * at 1 kHz relative to the block's RMS (a Goertzel filter). A pure tone puts
 * nearly all of its energy in that one bin; a music loop puts a little. The
 * three phases have to disagree for the test to pass — a probe that only ever
 * saw the tone (or never did) tells us nothing.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include "host/plugin_api_v1.h"

static uint8_t mailbox[8192];
static plugin_api_v2_t* api;
static void*            inst;

/* --- a 44-byte-header stereo 16-bit 48 kHz WAV holding one sine ----------- */
static void write_tone(const char* path, double freq, double seconds)
{
    const int    rate   = 48000;
    const int    frames = (int)(seconds * rate);
    const size_t data   = (size_t)frames * 4;           /* stereo int16 */
    FILE*        f      = fopen(path, "wb");
    if(!f)
    {
        printf("cannot write %s\n", path);
        exit(1);
    }
    uint8_t h[44] = {0};
    memcpy(h, "RIFF", 4);
    uint32_t riff = (uint32_t)(36 + data);
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t sz = 16;   memcpy(h + 16, &sz, 4);
    uint16_t fmt = 1, ch = 2, bits = 16, align = 4;
    uint32_t br = rate * 4;
    memcpy(h + 20, &fmt, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &br, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    uint32_t ds = (uint32_t)data;
    memcpy(h + 40, &ds, 4);
    fwrite(h, 1, 44, f);
    for(int i = 0; i < frames; i++)
    {
        const int16_t v = (int16_t)(sin(2.0 * M_PI * freq * i / rate) * 0.5 * 32767.0);
        fwrite(&v, 2, 1, f);
        fwrite(&v, 2, 1, f);
    }
    fclose(f);
}

/* --- phase: press the key, render, find where the energy is ---------------
 * A single-frequency share is the wrong instrument here: the CUBBI samples on
 * the card are 48 kHz and this engine runs at 44.1 kHz, so a "1 kHz" tone in
 * the file comes out at 1000 * 44100/48000 = 919 Hz. Measure the PEAK
 * frequency by scanning, then refine it — the answer is then a fact about the
 * audio, not an assumption about the file.
 */
static double g_tone = 1000.0;

/* Goertzel over a block of samples, returning |X|^2 (unnormalised). */
static double goertzel(const double* x, long n, double freq)
{
    const double w     = 2.0 * M_PI * freq / MOVE_SAMPLE_RATE;
    const double coeff = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0;
    for(long i = 0; i < n; i++)
    {
        const double t = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = t;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

static double phase(const char* label, double seconds)
{
    uint8_t on[3] = {0x90, 48, 100};    /* C3 -> slot 1 */
    api->on_midi(inst, on, 3, MOVE_MIDI_SOURCE_INTERNAL);

    const int blocks = (int)(seconds * MOVE_SAMPLE_RATE / MOVE_FRAMES_PER_BLOCK);
    int16_t   out[256];
    static double x[1 << 16];
    long          n = 0, rms_n = 0;
    double        rms_sum = 0.0;

    for(int b = 0; b < blocks; b++)
    {
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        for(int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++)
        {
            const double s = out[2 * i] / 32768.0;
            if(n < (long)(sizeof x / sizeof x[0]))
                x[n++] = s;
            rms_sum += s * s;
            rms_n++;
        }
        usleep(2900);
    }

    const double rms = sqrt(rms_sum / (rms_n ? rms_n : 1));

    /* Coarse scan, then a 1 Hz refine around the winner. */
    double best_f = 0.0, best_e = 0.0;
    for(double f = 200.0; f <= 4000.0; f += 25.0)
    {
        const double e = goertzel(x, n, f);
        if(e > best_e)
        {
            best_e = e;
            best_f = f;
        }
    }
    for(double f = best_f - 25.0; f <= best_f + 25.0; f += 1.0)
    {
        if(f <= 0.0)
            continue;
        const double e = goertzel(x, n, f);
        if(e > best_e)
        {
            best_e = e;
            best_f = f;
        }
    }

    /* Amplitude of that component: for a real sinusoid |X| = A*N/2. */
    const double amp   = (n > 0) ? (2.0 * sqrt(best_e) / (double)n) : 0.0;
    const double share = (rms > 1e-9) ? (amp / (sqrt(2.0) * rms)) : 0.0;
    printf("%-34s rms=%7.4f  peak=%6.1fHz  share=%6.2f%%\n",
           label, rms, best_f, share * 100.0);
    return share;
}

static void wait_for_scan(int ms)
{
    for(int i = 0; i < ms / 20; i++)
        usleep(20 * 1000);
}

int main(int argc, char** argv)
{
    const char* so   = argc > 1 ? argv[1] : "build/chompi_move.so";
    const char* dir  = argc > 2 ? argv[2] : ".";
    const char* tone = argc > 3 ? argv[3] : "/tmp/chompi_probe_tone.wav";

    void* lib = dlopen(so, RTLD_NOW);
    if(!lib)
    {
        printf("dlopen(%s): %s\n", so, dlerror());
        return 1;
    }
    auto init = (plugin_api_v2_t * (*)(const host_api_v1_t*)) dlsym(lib, "move_plugin_init_v2");

    static host_api_v1_t h;
    memset(&h, 0, sizeof h);
    h.api_version      = MOVE_PLUGIN_API_VERSION;
    h.sample_rate      = MOVE_SAMPLE_RATE;
    h.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    h.mapped_memory    = mailbox;
    h.audio_out_offset = MOVE_AUDIO_OUT_OFFSET;
    h.audio_in_offset  = MOVE_AUDIO_IN_OFFSET;

    /* Start from a clean map. The module reads <module_dir>/samples/samples.map
     * at create, so a leftover assignment from an earlier run would show up as
     * phase 1 playing the tone — the test assert itself would pass for the
     * wrong reason. */
    {
        char map_path[512];
        snprintf(map_path, sizeof map_path, "%s/samples/samples.map", dir);
        remove(map_path);
    }

    api  = init(&h);
    inst = api->create_instance(dir, NULL);
    api->set_param(inst, "voice_mode", "cubbi");
    api->set_param(inst, "bank", "0");
    api->set_param(inst, "slot", "1");

    char buf[1024];

    write_tone(tone, g_tone, 4.0);
    printf("tone written: %s\n", tone);

    wait_for_scan(1500);
    int n = api->get_param(inst, "debug_files", buf, sizeof buf);
    printf("slots: %s\n", n > 0 ? buf : "-");
    n = api->get_param(inst, "sample_file", buf, sizeof buf);
    printf("sample_file (before) : %s\n", n > 0 ? buf : "-");

    const double stock = phase("1. shipped slot 1", 1.0);

    api->set_param(inst, "sample_file", tone);
    wait_for_scan(1500);
    n = api->get_param(inst, "sample_file", buf, sizeof buf);
    printf("sample_file (after)  : %s\n", n > 0 ? buf : "-");
    n = api->get_param(inst, "sample_map", buf, sizeof buf);
    printf("sample_map           : %s\n", n > 0 ? buf : "-");

    const double assigned = phase("2. assigned tone on slot 1", 1.0);

    /* 3. persistence: tear the instance down and build a new one, the way the
     * chain host does on a reload or a power cycle. The map is read back from
     * <sample_dir>/samples.map on create, so the assignment must survive — and
     * the tone must still be what plays. */
    api->destroy_instance(inst);
    usleep(300 * 1000);
    inst = api->create_instance(dir, NULL);
    api->set_param(inst, "voice_mode", "cubbi");
    api->set_param(inst, "bank", "0");
    api->set_param(inst, "slot", "1");
    wait_for_scan(1500);
    n = api->get_param(inst, "sample_map", buf, sizeof buf);
    printf("sample_map (reloaded): %s\n", n > 0 ? buf : "-");
    n = api->get_param(inst, "sample_file", buf, sizeof buf);
    printf("sample_file(reloaded): %s\n", n > 0 ? buf : "-");
    const double reloaded = phase("3. after reload", 1.0);

    api->set_param(inst, "sample_clear", "1");
    wait_for_scan(1500);
    n = api->get_param(inst, "sample_file", buf, sizeof buf);
    printf("sample_file (cleared): %s\n", n > 0 ? buf : "-");
    n = api->get_param(inst, "sample_map", buf, sizeof buf);
    printf("sample_map (cleared) : %s\n", n > 0 ? buf : "-");

    const double cleared = phase("4. slot 1 after clear", 1.0);

    printf("\nVERDICT stock=%.2f%% assigned=%.2f%% reloaded=%.2f%% cleared=%.2f%%\n",
           stock * 100, assigned * 100, reloaded * 100, cleared * 100);
    /* A pure tone is a single partial: most of the block's energy sits in one
     * frequency. A music loop spreads it. So the assignment must make the
     * share JUMP, and clearing must bring it back down. */
    /* phase() returns a FRACTION; the percentages above are for reading. */
    const bool ok = assigned > 0.50 && assigned > stock * 3.0
                    && reloaded > 0.50 /* the map persisted */
                    && cleared < 0.25;
    printf("%s\n", ok ? "PASS: the assigned sample is what plays"
                      : "FAIL: the override did not change what plays");
    return ok ? 0 : 3;
}
