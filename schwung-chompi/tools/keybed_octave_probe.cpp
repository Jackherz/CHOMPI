/*
 * tools/keybed_octave_probe.cpp — the keybed is a keyboard: 32 pads, 32
 * pitches, and the pitch follows the note.
 *
 *   g++ -std=c++17 -O1 -D_Static_assert=static_assert \
 *       -I module/dsp/third_party/schwung -o /tmp/keybed_octave_probe \
 *       tools/keybed_octave_probe.cpp -ldl
 *   mkdir -p /tmp/kboct && cp build/chompi_move.so /tmp/kboct/
 *   (cd /tmp/kboct && /tmp/keybed_octave_probe ./chompi_move.so .)
 *
 * PORT PATCH P27. The host delivers the pads as notes (MIDI 68..99 at rest)
 * and the module reads the note as a chromatic ladder whose root is pad 80 —
 * twelve pads up from the bottom left, the sample at its own speed, 0.50x at
 * the bottom-left pad and 3.00x at the top right. What this probe exists to
 * prove, in the order the checks run:
 *
 *   1. every pad sounds, exactly one semitone above the pad before it, and no
 *      two pads play the same pitch;
 *   2. Move's octave buttons move the pads' note numbers by twelve, and the
 *      whole keyboard moves with them — the same +12 on every pad, at every
 *      pad, in both directions. This is the check a fixed 32-entry table
 *      indexed by `note - 68` cannot pass: the shifted notes fall off the end
 *      of it, and the pads that run off are read as some other key entirely;
 *   3. the module's own `octave` knob adds twelve semitones a step on top;
 *   4. it is still one sample — mode, bank and slot cannot put a second file
 *      under a pad;
 *   5. above 1.5x, where the reader would like a `<name>_double.wav`
 *      companion: with none, with a 0-byte one (P23's guard) and with a real
 *      one, the pad's pitch is the same, because that is all it is for.
 *
 * The tone is 480 Hz at 48 kHz. The engine runs at 44.1 kHz, so 1.00x comes
 * out at 480 * 44100/48000 = 441 Hz.
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

static const int    kPadLow  = 68;           /* bottom-left pad         */
static const int    kPadRoot = kPadLow + 12; /* the sample's own speed  */
static const double kFileHz  = 480.0;

/** What the module must play for a note: the ladder, nothing else. */
static double ladder_hz(int note) { return kFileHz * (44100.0 / 48000.0) * pow(2.0, (note - kPadRoot) / 12.0); }

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

/** Presses `note`, renders, and reports the dominant frequency of what came
 *  out. The scan reaches past the top of the tape's range: the top pads at
 *  Move's octave-up are +31 semitones. */
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
    const double hi = ladder_hz(kPadRoot + 31) + 160.0; /* +31 st: the top pad, octave up */
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

static void write_tone(const char* path, double hz, int frames)
{
    const int sr = 48000; /* 1 s is long enough to measure at that length */
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

/** One pitch, against the semitone count the caller names — the label IS the
 *  expectation, so a check that disagrees with itself cannot pass. */
static void expect_pitch(const char* what, int note, int semitones)
{
    double f = 0, rms = 0;
    press(note, 0.30, &f, &rms);
    const double want = kFileHz * (44100.0 / 48000.0) * pow(2.0, semitones / 12.0);
    const bool ok = rms >= 0.01 && fabs(f - want) <= 8.0;
    if(!ok) g_fail++;
    printf("%s  %-56s note %3d %+3d st -> %6.1f Hz (want %6.1f) rms=%.4f\n",
           ok ? " ok " : "FAIL", what, note, semitones, f, want, rms);
}

/** A sweep of the whole 32-pad surface at one octave of note numbers. Every
 *  pad must sound, and must play the ladder's pitch for the note it sent —
 *  which is also the "one semitone above the pad before it, no two the same"
 *  check, since the ladder is what the notes are. */
static void sweep(const char* what, int note_low)
{
    printf("\n--- %s ---\n", what);
    int silent = 0, wrong = 0;
    for(int i = 0; i < 32; i++)
    {
        const int note = note_low + i;
        double f = 0, rms = 0;
        press(note, 0.30, &f, &rms);
        const double want = ladder_hz(note);
        const bool quiet = rms < 0.01;
        const bool off   = fabs(f - want) > 8.0;
        if(quiet) silent++;
        if(!quiet && off) wrong++;
        if(quiet || off)
            printf("%s  pad %2d (note %3d) %+3d st -> %6.1f Hz (want %6.1f) rms=%.4f\n",
                   quiet ? "FAIL" : "FAIL", i, note, note - kPadRoot, f, want, rms);
    }
    const bool ok = (silent == 0 && wrong == 0);
    if(!ok) g_fail++;
    printf("%s  %d pads: %d silent, %d on the wrong pitch\n",
           ok ? " ok " : "FAIL", 32, silent, wrong);
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
    write_tone(tone, kFileHz, 48000);
    write_tone(other, 900.0, 48000); /* a second file the keybed must not play */
    {
        char cubbi[512];
        snprintf(cubbi, sizeof cubbi, "%s/samples/cubbi_a1.wav", dir);
        write_tone(cubbi, 700.0, 48000);
        char jammi_b[512];
        snprintf(jammi_b, sizeof jammi_b, "%s/samples/jammi_b1.wav", dir);
        write_tone(jammi_b, 1000.0, 48000);
    }

    inst = api->create_instance(dir, NULL);
    settle_ms(800);
    set("sample_file", tone);
    settle_ms(1200);

    /* --- 1. the surface, at the note numbers the pads send at rest -------- */
    sweep("the 32 pads at rest: one semitone a pad, none on one pitch", kPadLow);

    /* The pads the ladder is pinned to, named, so the numbers are readable
     * even when the sweep above is all green. */
    printf("\n--- the ladder's ends, its root, and a MIDI keyboard ------------\n");
    printf("     pad 68 = -12 st (0.50x), pad %d = 0 (1.00x), pad 99 = +19 st (3.00x)\n",
           kPadRoot);
    expect_pitch("bottom-left pad, one octave down", 68, -12);
    expect_pitch("the root: the sample at its own speed", kPadRoot, 0);
    expect_pitch("top-right pad, three times", 99, 19);
    expect_pitch("a MIDI keyboard walks the same ladder: note 60", 60, -20);
    expect_pitch("and note 80 is the root there too", 80, 0);

    /* --- 2. Move's octave buttons: the notes move, the keyboard moves ------ */
    sweep("Move's octave UP: every pad's note +12, every pitch +12", kPadLow + 12);
    sweep("Move's octave DOWN: every pad's note -12, every pitch -12", kPadLow - 12);

    /* --- 3. the module's own octave knob ---------------------------------- */
    printf("\n--- the octave knob: twelve semitones a step --------------------\n");
    set("octave", "1");
    settle_ms(200);
    expect_pitch("octave +1", kPadRoot, 12);
    set("octave", "2");
    settle_ms(200);
    expect_pitch("octave +2", kPadRoot, 24);
    set("octave", "-1");
    settle_ms(200);
    expect_pitch("octave -1", kPadRoot, -12);
    set("octave", "0");
    settle_ms(200);

    /* --- 4. one sample, whatever mode/bank/slot say ----------------------- */
    printf("\n--- mode/bank/slot cannot put a second file under a pad ---------\n");
    set("voice_mode", "cubbi");
    set("bank", "1");
    set("slot", "5");
    settle_ms(400);
    expect_pitch("after cubbi / bank 1 / slot 5, still the one sample", kPadRoot, 0);

    char mode[32] = {0}, slot[32] = {0};
    api->get_param(inst, "voice_mode", mode, sizeof mode);
    api->get_param(inst, "slot", slot, sizeof slot);
    const bool pinned = strcmp(mode, "jammi") == 0 && strcmp(slot, "1") == 0;
    if(!pinned) g_fail++;
    printf("%s  voice_mode=%s slot=%s (want jammi / 1)\n", pinned ? " ok " : "FAIL", mode, slot);

    /* --- 5. above 1.5x: the _double companion must not move the pitch ------ */
    /* Pad 99 is 3.00x, so the reader looks for `the_sample_double.wav` there.
     * The card ships one beside every sample: the same audio at twice the
     * speed, half the frames, so a high pad reads a copy that is not thinned
     * out. None of the three states may change the pitch -- and the 0-byte
     * one is P23's wedge: a companion that was not really there used to be
     * opened as an empty file and streamed forever, silence that only a
     * reload fixed. */
    printf("\n--- above 1.5x: the _double companion --------------------------\n");
    char dbl[600];
    snprintf(dbl, sizeof dbl, "%s_double.wav", tone);
    expect_pitch("no companion at all: pad 99 is still 3.00x", 99, 19);
    {
        FILE* empty = fopen(dbl, "wb"); /* 0 bytes: the placeholder P23 catches */
        if(empty)
            fclose(empty);
    }
    expect_pitch("a 0-byte companion is dropped: pad 99, 3.00x", 99, 19);
    write_tone(dbl, kFileHz * 2.0, 24000); /* the real thing: 2x speed, half the frames */
    expect_pitch("a real companion plays the same pitch: pad 99, 3.00x", 99, 19);
    remove(dbl);

    printf("\n%s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    api->destroy_instance(inst);
    return g_fail ? 1 : 0;
}
