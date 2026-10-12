/*
 * tools/keybed_octave_probe.cpp — one sample, piano keybed, every pad sounds.
 *
 *   g++ -std=c++17 -O1 -D_Static_assert=static_assert \
 *       -I module/dsp/third_party/schwung -o /tmp/keybed_octave_probe \
 *       tools/keybed_octave_probe.cpp -ldl
 *   mkdir -p /tmp/kboct/samples && cp build/chompi_move.so /tmp/kboct/
 *   (cd /tmp/kboct && /tmp/keybed_octave_probe ./chompi_move.so .)
 *
 * The host delivers pads as MIDI 68..99. The module maps them to a piano:
 * white keys on rows 1 and 3, black keys on rows 2 and 4, and the three
 * columns a piano leaves empty play the white key below them an octave up.
 * Pad 75 (and pad 84) is the sample at its own speed. One file covers every
 * pad; voice_mode, bank and slot must not put a second file on the keybed.
 *
 * The tone is 480 Hz at 48 kHz. The engine runs at 44.1 kHz, so 1.00x comes
 * out at 480 * 44100/48000 = 441 Hz. Keep kPadPitch in step with
 * chompi_move.cpp.
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
static int g_fail = 0;

static void set(const char* k, const char* v) { api->set_param(inst, k, v); }

static const signed char kPadPitch[32] = {
    -12, -10, -8, -7, -5, -3, -1,  0,
      0, -11, -9,  5, -6, -4, -2, 12,
      0,   2,  4,  5,  7,  9, 11, 12,
     12,   1,  3, 17,  6,  8, 10, 24,
};
static const double kFileHz = 480.0;
static double played_hz(int semitones)
{
    return kFileHz * (44100.0 / 48000.0) * pow(2.0, semitones / 12.0);
}

static void settle_ms(int ms)
{
    int16_t out[256];
    for(int i = 0; i < ms / 3; i++)
    {
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        usleep(2900);
    }
}

static double goertzel(const double* x, long n, double f)
{
    const double w = 2.0 * M_PI * f / (double)MOVE_SAMPLE_RATE;
    const double c = 2.0 * cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for(long i = 0; i < n; i++) { s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

static void press(int note, double seconds, double* out_f, double* out_rms)
{
    settle_ms(180);
    uint8_t on[3] = {0x90, (uint8_t)note, 100}, off[3] = {0x80, (uint8_t)note, 0};
    api->on_midi(inst, on, 3, MOVE_MIDI_SOURCE_INTERNAL);
    static double x[1 << 15];
    long n = 0; double sum = 0;
    int16_t out[256];
    const int blocks = (int)(seconds * MOVE_SAMPLE_RATE / MOVE_FRAMES_PER_BLOCK);
    for(int b = 0; b < blocks; b++)
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
    const double hi = played_hz(24) + 80.0;
    for(double f = 40; f <= hi; f += 4)
    {
        const double e = goertzel(x, n, f);
        if(e > best_e) { best_e = e; best_f = f; }
    }
    for(double f = best_f - 4; f <= best_f + 4; f += 0.5)
    {
        if(f <= 0) continue;
        const double e = goertzel(x, n, f);
        if(e > best_e) { best_e = e; best_f = f; }
    }
    *out_f = best_f;
    *out_rms = rms;
}

static void write_tone(const char* path, double hz)
{
    const int sr = 48000, frames = sr; /* 1 s, long enough to measure */
    FILE* f = fopen(path, "wb");
    if(!f) { printf("cannot write %s\n", path); exit(1); }
    const uint32_t data = (uint32_t)frames * 4;
    uint8_t h[44] = {0};
    memcpy(h, "RIFF", 4); const uint32_t riff = 36 + data; memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8); const uint32_t sz = 16; memcpy(h + 16, &sz, 4);
    const uint16_t fmt = 1, ch = 2, bits = 16, align = 4;
    const uint32_t rate = sr, br = rate * 4;
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

static void expect_pitch(const char* what, int note, int semitones)
{
    double f = 0, rms = 0;
    press(note, 0.45, &f, &rms);
    const double want = played_hz(semitones);
    const bool ok = rms >= 0.01 && fabs(f - want) <= 8.0;
    if(!ok) g_fail++;
    printf("%s  %-32s note %3d %+3d st -> %6.1f Hz (want %6.1f) rms=%.4f\n",
           ok ? " ok " : "FAIL", what, note, semitones, f, want, rms);
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

    char tone[512], other[512];
    snprintf(tone, sizeof tone, "%s/the_sample.wav", dir);
    snprintf(other, sizeof other, "%s/samples/jammi_a2.wav", dir);
    write_tone(tone, kFileHz);
    write_tone(other, 900.0); /* a second file the keybed must not play */
    {
        char cubbi[512];
        snprintf(cubbi, sizeof cubbi, "%s/samples/cubbi_a1.wav", dir);
        write_tone(cubbi, 700.0);
        char jammi_b[512];
        snprintf(jammi_b, sizeof jammi_b, "%s/samples/jammi_b1.wav", dir);
        write_tone(jammi_b, 1000.0);
    }

    inst = api->create_instance(dir, NULL);
    settle_ms(800);
    set("sample_file", tone);
    settle_ms(1200);

    printf("--- piano: whites, blacks, and the pads a piano leaves empty ---\n");
    expect_pitch("bottom-left C, one octave down", 68, -12);
    expect_pitch("D, white", 69, -10);
    expect_pitch("C at its own speed (pad 75)", 75, 0);
    expect_pitch("gap above C plays C up an octave", 76, 0);
    expect_pitch("C# black key", 77, -11);
    expect_pitch("gap above F plays F up an octave", 79, 5);
    expect_pitch("F# black key", 80, -6);
    expect_pitch("upper white C, own speed", 84, 0);
    expect_pitch("upper C# black key", 93, 1);
    expect_pitch("top gap, C two octaves up", 99, 24);
    expect_pitch("keyboard middle C is the same 1.00x", 60, 0);

    printf("\n--- every pad sounds ---\n");
    int silent = 0;
    for(int i = 0; i < 32; i++)
    {
        double f = 0, rms = 0;
        press(68 + i, 0.22, &f, &rms);
        if(rms < 0.01) { silent++; g_fail++; printf("FAIL  pad %d silent (rms=%.4f)\n", 68 + i, rms); }
    }
    printf("%s  32 pads, %d silent\n", silent ? "FAIL" : " ok ", silent);

    printf("\n--- octave knob transposes; mode/bank/slot do not change the file ---\n");
    set("octave", "1");
    settle_ms(200);
    expect_pitch("octave +1, pad 75 is an octave up", 75, 12);
    set("octave", "0");
    set("voice_mode", "cubbi");
    set("bank", "1");
    set("slot", "5");
    settle_ms(400);
    expect_pitch("after cubbi/bank/slot, still the one sample", 75, 0);

    char mode[32] = {0}, slot[32] = {0};
    api->get_param(inst, "voice_mode", mode, sizeof mode);
    api->get_param(inst, "slot", slot, sizeof slot);
    const bool pinned = strcmp(mode, "jammi") == 0 && strcmp(slot, "1") == 0;
    if(!pinned) g_fail++;
    printf("%s  voice_mode=%s slot=%s (want jammi / 1)\n", pinned ? " ok " : "FAIL", mode, slot);

    printf("\n%s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    api->destroy_instance(inst);
    return g_fail ? 1 : 0;
}
