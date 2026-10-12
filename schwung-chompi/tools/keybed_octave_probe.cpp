/*
 * tools/keybed_octave_probe.cpp — one slot is one octave of the keybed.
 *
 *   g++ -std=c++17 -O1 -D_Static_assert=static_assert \
 *       -I module/dsp/third_party/schwung -o /tmp/keybed_octave_probe \
 *       tools/keybed_octave_probe.cpp -ldl
 *   mkdir -p /tmp/kboct/samples && cp build/chompi_move.so /tmp/kboct/
 *   (cd /tmp/kboct && /tmp/keybed_octave_probe ./chompi_move.so .)
 *
 * What the port promises (PORT PATCH P22) and what this measures, on a host
 * build with the module's own keybed map:
 *
 *   - the pads are a chromatic keybed read left to right and bottom to top:
 *     pad 68 is the C the selected slot starts on, pad 80 the C two slots'
 *     worth up, and every pad sounds;
 *   - ONE SLOT IS ONE OCTAVE -- twelve pads. The slot the `slot` knob selects
 *     owns the first twelve pads, the next twelve are the next slot up, and
 *     within an octave the pad's distance from its own C is the playback ratio:
 *     the C of a slot plays at 1.00x, the G above it at 1.50x, the B at 1.89x;
 *   - the `octave` knob transposes the keybed an octave per step;
 *   - JAMMI is one sample for the whole keybed, with pad 80 (twelve pads up)
 *     at 1.00x -- the same pad a CUBBI slot's octave starts on.
 *
 * Each slot's file is a distinct tone (200 + 60 * slot Hz at 48 kHz, which the
 * engine's 44.1 kHz output renders 0.919x slower), so "which slot" and "which
 * pitch" are both read off the sound rather than trusted from the code: the
 * probe reports the strongest Goertzel bin over the whole spectrum and compares
 * it with the tone that slot and semitone predict.
 *
 * The 1.587x case is deliberately in here. Above 1.5x the engine wants a
 * `<name>_double.wav` companion; a user's assignment has none, and before PORT
 * PATCH P22 the reader claimed double speed anyway and played it an OCTAVE LOW
 * (measured 321 Hz where 642 was due).
 */
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "host/plugin_api_v1.h"

static uint8_t mailbox[8 * 1024 * 1024] __attribute__((aligned(4096)));
static plugin_api_v2_t* api = nullptr;
static void* inst = nullptr;

static void set(const char* k, const char* v) { api->set_param(inst, k, v); }

static int g_fail = 0;

/* Render until nothing is playing: the reader pre-buffers for up to 2 s, and a
 * release tail would otherwise land inside the next measurement. */
static void settle()
{
    int16_t out[256];
    for(int i = 0; i < 900; i++) { api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK); usleep(2900); }
}
static void settle_ms(int ms)
{
    int16_t out[256];
    for(int i = 0; i < ms / 3; i++) { api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK); usleep(2900); }
}
static double goertzel(const double* x, long n, double f)
{
    const double w = 2.0 * M_PI * f / (double)MOVE_SAMPLE_RATE;
    const double c = 2.0 * cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for(long i = 0; i < n; i++) { s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}
/** Press a pad, report the strongest frequency and the block rms. */
static void press(int note, double seconds, double* out_f, double* out_rms)
{
    /* The previous note's release tail must be gone before this one starts,
     * or a silent pad "measures" the note before it. */
    settle();
    uint8_t on[3] = {0x90, (uint8_t)note, 100}, off[3] = {0x80, (uint8_t)note, 0};
    api->on_midi(inst, on, 3, MOVE_MIDI_SOURCE_INTERNAL);
    static double x[1 << 15];
    long n = 0; double sum = 0;
    int16_t out[256];
    for(int b = 0; b < (int)(seconds * MOVE_SAMPLE_RATE / MOVE_FRAMES_PER_BLOCK); b++)
    {
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        for(int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++)
        {
            const double s = out[2 * i] / 32768.0;
            if(n < (long)(sizeof x / sizeof x[0])) x[n++] = s;
            sum += s * s;
        }
        usleep(2900);
    }
    api->on_midi(inst, off, 3, MOVE_MIDI_SOURCE_INTERNAL);
    const double rms = sqrt(sum / (n ? n : 1));
    double best_f = 0, best_e = 0;
    for(double f = 60; f <= 4000; f += 5) { const double e = goertzel(x, n, f); if(e > best_e) { best_e = e; best_f = f; } }
    for(double f = best_f - 5; f <= best_f + 5; f += 0.5) { if(f <= 0) continue; const double e = goertzel(x, n, f); if(e > best_e) { best_e = e; best_f = f; } }
    *out_f = best_f; *out_rms = rms;
}

/* Slot i is a 200 + 60*i Hz tone in its file; the engine plays 0.919x. */
static double slot_hz(int slot) { return (200.0 + 60.0 * slot) * 44100.0 / 48000.0; }
/** The pitch a slot's sample comes out at, `semitones` above that slot's C. */
static double want_hz(int slot, int semitones) { return slot_hz(slot) * pow(2.0, semitones / 12.0); }

/* The Move's pads, and the keybed kMoveKey[] lays over them. Pads are listed
 * bottom row first, 68..99. */
static const int kPadLow = 68;
static const int kPadHigh = 99;

static void expect(const char* what, int note, int slot, int semitones)
{
    double f = 0, rms = 0;
    press(note, 1.0, &f, &rms);
    const double want = want_hz(slot, semitones);
    const bool ok = fabs(f - want) <= 8.0 && rms >= 0.005;
    if(!ok) g_fail++;
    printf("%s  pad %2d  %-28s slot %2d %+3d st -> %6.1f Hz (want %6.1f) rms=%.4f\n",
           ok ? " ok " : "FAIL", note, what, slot, semitones, f, want, rms);
}
static void expect_silent(const char* what, int note)
{
    double f = 0, rms = 0;
    press(note, 0.5, &f, &rms);
    const bool ok = rms < 0.005;
    if(!ok) g_fail++;
    printf("%s  pad %2d  %-28s silent (rms=%.4f, %.0f Hz)\n",
           ok ? " ok " : "FAIL", note, what, rms, f);
}

static void write_tone_frames(const char* path, double hz, int frames);
static void write_tone(const char* path, double hz) { write_tone_frames(path, hz, 48000); }
static void write_tone_frames(const char* path, double hz, int frames)
{
    const int sr = 48000;
    FILE* f = fopen(path, "wb");
    if(!f) { printf("cannot write %s\n", path); exit(1); }
    const uint32_t data = (uint32_t)frames * 4;
    uint8_t h[44] = {0};
    memcpy(h, "RIFF", 4); const uint32_t riff = 36 + data; memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8); const uint32_t sz = 16; memcpy(h + 16, &sz, 4);
    const uint16_t fmt = 1, ch = 2, bits = 16, align = 4; const uint32_t rate = sr, br = rate * 4;
    memcpy(h + 20, &fmt, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &br, 4); memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    fwrite(h, 1, sizeof h, f);
    for(int i = 0; i < frames; i++)
    {
        const int16_t s = (int16_t)(12000.0 * sin(2 * M_PI * hz * i / sr));
        int16_t pair[2] = {s, s};
        fwrite(pair, sizeof(int16_t), 2, f);
    }
    fclose(f);
}

static const char* join(const char* dir, const char* name)
{
    static char buf[512];
    snprintf(buf, sizeof buf, "%s/%s", dir, name);
    return buf;
}

/* Load a tone for `slot` the way the page does: select the slot, then pick. */
static void load_into(int slot, const char* dir)
{
    char buf[16]; snprintf(buf, sizeof buf, "%d", slot);
    set("slot", buf);
    char path[512]; snprintf(path, sizeof path, "%s/user_slot%02d.wav", dir, slot);
    set("sample_file", path);
    settle_ms(1200);
}

int main(int argc, char** argv)
{
    const char* so  = argc > 1 ? argv[1] : "build/chompi_move.so";
    const char* dir = argc > 2 ? argv[2] : "/tmp/kboct";

    void* lib = dlopen(so, RTLD_NOW);
    if(!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
    auto init = (plugin_api_v2_t * (*)(const host_api_v1_t*)) dlsym(lib, "move_plugin_init_v2");
    static host_api_v1_t h; memset(&h, 0, sizeof h);
    h.api_version = MOVE_PLUGIN_API_VERSION; h.sample_rate = MOVE_SAMPLE_RATE;
    h.frames_per_block = MOVE_FRAMES_PER_BLOCK; h.mapped_memory = mailbox;
    h.audio_out_offset = MOVE_AUDIO_OUT_OFFSET; h.audio_in_offset = MOVE_AUDIO_IN_OFFSET;
    api = init(&h);

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/samples'", dir);
    if(system(cmd) != 0) return 1;
    for(int slot = 1; slot <= 4; slot++)
    {
        char p[512]; snprintf(p, sizeof p, "%s/user_slot%02d.wav", dir, slot);
        write_tone(p, 200.0 + 60.0 * slot);
    }

    inst = api->create_instance(dir, NULL);
    settle_ms(1500);
    set("voice_mode", "cubbi");
    settle_ms(400);

    printf("--- load tones into slots 1..4, then play the keybed ---\n");
    for(int slot = 1; slot <= 4; slot++)
        load_into(slot, dir);

    /* slot 1 selected: the lower pair of rows is slot 1, the pair above it
     * slot 2. Pads, bottom row first: 68..75 whites, 76..83 blacks, 84..91
     * whites, 92..99 blacks. */
    printf("\n--- CUBBI, slot knob at 1: twelve pads per slot, in rows of eight ---\n");
    set("slot", "1"); settle_ms(200);
    /* Pads 68..79 are slot 1's octave (C..B), pads 80..91 are slot 2's, pads
     * 92..99 are the first eight of slot 3. Rows are eight pads wide, so the
     * slot boundary falls inside row 2 (pad 80). */
    expect("C   (slot 1 root)", 68, 1, 0);
    expect("C#",                69, 1, 1);
    expect("D",                 70, 1, 2);
    expect("E",                 72, 1, 4);
    expect("F",                 73, 1, 5);
    expect("G",                 75, 1, 7);
    expect("A",                 77, 1, 9);
    expect("A#",                78, 1, 10);
    expect("B   (1.89x)",       79, 1, 11);
    expect("C   (slot 2 root)", 80, 2, 0);
    expect("E",                 84, 2, 4);
    expect("F#  (1.41x)",       86, 2, 6);
    expect("G#  (1.59x)",       88, 2, 8);
    expect("B   (1.89x)",       91, 2, 11);
    expect("C   (slot 3 root)", 92, 3, 0);
    expect("G",                 99, 3, 7);

    printf("\n--- the octave knob transposes the keybed ---\n");
    set("octave", "1"); settle_ms(300);
    expect("pad 68, octave +1",          68, 1, 12); /* 2.00x */
    expect("pad 72, octave +1",          72, 1, 16);
    set("octave", "-1"); settle_ms(300);
    expect("pad 68, octave -1",          68, 1, -12); /* 0.50x */
    expect("pad 79, octave -1",          79, 1, -1);
    set("octave", "0"); settle_ms(300);

    printf("\n--- a `_double` companion, over 1.5x (a convention slot) ---\n");
    write_tone_frames(join(dir, "cubbi_a5.wav"), 200.0 + 60.0 * 5, 48000);
    write_tone_frames(join(dir, "cubbi_a5_double.wav"), 2.0 * (200.0 + 60.0 * 5), 24000);
    /* The slot scan is triggered, not periodic: a file that appears in the
     * sample directory behind the module's back is not seen until something
     * asks it to look (an assignment, a record, or this). Writing `sample_dir`
     * to the value it already has is that ask. */
    set("sample_dir", dir);
    settle_ms(900);
    set("slot", "4"); settle_ms(600);   /* pads 80..91 are slot 5 */
    expect("C   (slot 5, 1.00x)", 80, 5, 0);
    expect("D#  (1.19x)",         83, 5, 3);
    expect("G#  (1.59x: the swap)", 88, 5, 8);

    printf("\n--- the slot knob pages the ladder ---\n");
    set("slot", "3"); settle_ms(300);
    expect("C, slot 3",          68, 3, 0);
    expect("C, slot 4",          80, 4, 0);
    set("slot", "14"); settle_ms(300);
    expect_silent("slot 14 (empty)", 68);
    set("slot", "1"); settle_ms(300);

    printf("\n--- JAMMI: one sample, pad 80 at 1.00x ---\n");
    set("voice_mode", "jammi"); settle_ms(600);
    /* JAMMI's slot 1 is its own map entry (mode 0): assign the same tone. */
    load_into(1, dir);
    set("slot", "1"); settle_ms(300);
    expect("(the root)",           80, 1, 0);
    expect("(-4 st)",              76, 1, -4);
    expect("(an octave down)",     68, 1, -12);
    expect("(+8 st, over 1.5x)",   88, 1, 8);
    expect("(+12 st)",             92, 1, 12);
    expect("(+19 st, the top row)", 99, 1, 19);

    settle_ms(600);
    set("voice_mode", "cubbi"); settle_ms(600);

    printf("\n%s — %d failed check(s)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
