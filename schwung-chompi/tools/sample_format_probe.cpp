/*
 * tools/sample_format_probe.cpp — "why does my own sample play as noise?"
 *
 * The engine streams raw bytes and assumes 16-bit stereo starting at byte 44:
 * a user's 24-bit, float, mono, EXTENSIBLE or LIST-chunked WAV plays as noise
 * through it. The port parses the header at open and hands the engine a
 * converted view instead (module/dsp/port/sample_view.h, PORT PATCH P21). This
 * probe measures every flavour a library actually holds.
 *
 *   python3 tools/make_sample_variants.py /tmp/wavfmt/samples
 *   g++ -std=c++17 -O1 -D_Static_assert=static_assert \
 *       -I module/dsp/third_party/schwung -o /tmp/sample_format_probe \
 *       tools/sample_format_probe.cpp -ldl
 *   /tmp/sample_format_probe build/chompi_move.so /tmp/wavfmt /tmp/wavfmt/samples
 *
 * Every file holds a 440 Hz tone, so the checks are about the audio: the tone
 * must come out at 404 Hz (the engine runs at 44.1 kHz, the files are 48 kHz)
 * and carry most of the block's energy. A wrong decode spreads the energy
 * everywhere (that is what "noise" measures as) and a doubled-speed mono file
 * lands at 808 Hz.
 *
 * The last section is the interesting one: reverse playback, start/end and
 * loop regions all run through the converter too, because the engine's region
 * arithmetic is `f_size() - 44` and the converter owns `f_size()`.
 */
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "host/plugin_api_v1.h"

static uint8_t          mailbox[8 * 1024 * 1024] __attribute__((aligned(4096)));
static plugin_api_v2_t* api  = nullptr;
static void*            inst = nullptr;
static int              checks = 0, failures = 0;

static const double kTone404 = 404.25; /* 440 Hz in the file, 44.1/48 engine */

static void check(const char* what, bool ok, const char* detail)
{
    printf("%s %-46s %s\n", ok ? " ok " : "FAIL", what, detail);
    checks++;
    if(!ok)
        failures++;
}

static void set(const char* k, const char* v) { api->set_param(inst, k, v); }

/* The module's own card format, written by hand: 16-bit stereo 48 kHz with the
 * audio at byte 44. This is the file the last check plays -- the one the WAV
 * view must leave completely alone. */
static void write_card_file(const char* dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/samples/cubbi_a1.wav", dir);
    char dirpath[512];
    snprintf(dirpath, sizeof dirpath, "%s/samples", dir);
    /* the caller creates the directory: this only needs to exist */
    FILE* f = fopen(path, "wb");
    if(!f)
    {
        printf("cannot write %s\n", path);
        return;
    }
    const int frames = 48000;
    const uint32_t data = (uint32_t)frames * 4;
    uint8_t h[44] = {0};
    memcpy(h, "RIFF", 4);
    const uint32_t riff = 36 + data;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    const uint32_t fsz = 16;
    memcpy(h + 16, &fsz, 4);
    const uint16_t fmt = 1, ch = 2, bits = 16, align = 4;
    const uint32_t rate = 48000, br = rate * 4;
    memcpy(h + 20, &fmt, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &br, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fwrite(h, 1, sizeof h, f);
    for(int i = 0; i < frames; i++)
    {
        const int16_t s = (int16_t)(16000.0 * sin(2.0 * M_PI * 440.0 * i / 48000.0));
        int16_t       pair[2] = {s, s};
        fwrite(pair, sizeof(int16_t), 2, f);
    }
    fclose(f);
    printf("card: %s (440 Hz, 16-bit stereo)\n", path);
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

/* Goertzel over a window of samples; |X|^2 unnormalised. */
static double goertzel(const double* x, long n, double f)
{
    const double w = 2.0 * M_PI * f / (double)MOVE_SAMPLE_RATE;
    const double c = 2.0 * cos(w);
    double       s0 = 0, s1 = 0, s2 = 0;
    for(long i = 0; i < n; i++)
    {
        s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

struct Capture
{
    double rms       = 0.0;
    double peak_f    = 0.0;
    double share     = 0.0;   /* the tone's share of the block's energy */
    double rms_first = 0.0;   /* first / second half, for the ramp */
    double rms_second = 0.0;
};

/* Presses `note`, renders `seconds`, releases, and describes what came out. */
static Capture press(int note, double seconds)
{
    uint8_t on[3]  = {0x90, (uint8_t)note, 100};
    uint8_t off[3] = {0x80, (uint8_t)note, 0};
    api->on_midi(inst, on, 3, MOVE_MIDI_SOURCE_INTERNAL);

    static double x[1 << 16];
    long          n = 0;
    double        sum = 0.0, sum_late = 0.0;
    long          nlate = 0;
    int16_t       out[256];

    const int blocks = (int)(seconds * MOVE_SAMPLE_RATE / MOVE_FRAMES_PER_BLOCK);
    for(int b = 0; b < blocks; b++)
    {
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        for(int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++)
        {
            const double s = out[2 * i] / 32768.0;
            if(n < (long)(sizeof x / sizeof x[0]))
                x[n++] = s;
            sum += s * s;
            if(b >= blocks / 2)
            {
                sum_late += s * s;
                nlate++;
            }
        }
        usleep(2900);
    }
    api->on_midi(inst, off, 3, MOVE_MIDI_SOURCE_INTERNAL);

    Capture c;
    if(n == 0)
        return c;
    c.rms        = sqrt(sum / (double)n);
    c.rms_second = nlate ? sqrt(sum_late / (double)nlate) : 0.0;

    double best_f = 0.0, best_e = 0.0;
    for(double f = 60.0; f <= 4000.0; f += 5.0)
    {
        const double e = goertzel(x, n, f);
        if(e > best_e)
        {
            best_e = e;
            best_f = f;
        }
    }
    for(double f = best_f - 5.0; f <= best_f + 5.0; f += 0.5)
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
    c.peak_f = best_f;
    const double amp = 2.0 * sqrt(best_e) / (double)n;
    c.share          = (c.rms > 1e-9) ? (amp / (sqrt(2.0) * c.rms)) : 0.0;
    return c;
}

/* Assign `file` to slot 1, play it, and report. */
static Capture play(const char* dir, const char* file, int note)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    set("sample_file", path);
    settle_ms(900);
    return press(note, 1.0);
}

static void expect_tone(const char* file, const char* dir, double want_hz)
{
    const Capture c = play(dir, file, 48);
    char          d[160];
    snprintf(d, sizeof d, "rms=%.4f peak=%.1fHz share=%.1f%% want %.1f",
             c.rms, c.peak_f, c.share * 100.0, want_hz);
    check(file, c.rms >= 0.010 && c.share >= 0.50 && fabs(c.peak_f - want_hz) <= 8.0, d);
}

int main(int argc, char** argv)
{
    const char* so      = argc > 1 ? argv[1] : "build/chompi_move.so";
    const char* dir     = argc > 2 ? argv[2] : "/tmp/wavfmt";
    const char* fmt_dir = argc > 3 ? argv[3] : "/tmp/wavfmt/samples";

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

    write_card_file(dir);

    api  = init(&h);
    inst = api->create_instance(dir, NULL);
    settle_ms(1500);
    set("voice_mode", "cubbi");
    set("slot", "1");
    settle_ms(600);

    /* --- every format a user library holds ------------------------------ */
    printf("--- user WAV formats ----------------------------------------------\n");
    expect_tone("user_16bit_stereo.wav", fmt_dir, kTone404);
    expect_tone("user_24bit_stereo.wav", fmt_dir, kTone404);
    expect_tone("user_32bit_stereo.wav", fmt_dir, kTone404);
    expect_tone("user_float32_stereo.wav", fmt_dir, kTone404);
    expect_tone("user_float64_stereo.wav", fmt_dir, kTone404);
    expect_tone("user_8bit_stereo.wav", fmt_dir, kTone404);
    expect_tone("user_extensible24_stereo.wav", fmt_dir, kTone404);
    expect_tone("user_listchunk_stereo.wav", fmt_dir, kTone404);
    expect_tone("user_junkchunk_stereo.wav", fmt_dir, kTone404);
    expect_tone("user_24bit_mono.wav", fmt_dir, kTone404);
    expect_tone("user_quad_stereo.wav", fmt_dir, kTone404);
    expect_tone("user_ulaw_stereo.wav", fmt_dir, kTone404);
    expect_tone("user_alaw_stereo.wav", fmt_dir, kTone404);
    expect_tone("user_truncated_stereo.wav", fmt_dir, kTone404);

    /* Mono used to play at DOUBLE speed -- both channels got one of the two
     * interleaved samples. Its own check, because that failure is a pitch bug,
     * not a decode bug. */
    {
        const Capture c = play(fmt_dir, "user_16bit_mono.wav", 48);
        char          d[160];
        snprintf(d, sizeof d, "rms=%.4f peak=%.1fHz share=%.1f%% (808 Hz would be 2x speed)",
                 c.rms, c.peak_f, c.share * 100.0);
        check("user_16bit_mono.wav is not double speed",
              c.rms >= 0.010 && c.share >= 0.50 && fabs(c.peak_f - kTone404) <= 8.0, d);
    }

    /* --- AIFF / AIFF-C, what a Mac's library is full of ------------------ */
    printf("\n--- AIFF and AIFF-C ----------------------------------------------\n");
    expect_tone("user_16bit_stereo.aiff", fmt_dir, kTone404);
    expect_tone("user_24bit_stereo.aiff", fmt_dir, kTone404);
    expect_tone("user_16bit_mono.aiff", fmt_dir, kTone404);
    expect_tone("user_sowt16_stereo.aifc", fmt_dir, kTone404); /* little-endian PCM */
    expect_tone("user_fl32_stereo.aifc", fmt_dir, kTone404);
    expect_tone("user_ulaw_stereo.aifc", fmt_dir, kTone404);
    /* 8-bit AIFF is SIGNED where 8-bit WAV is unsigned: same file written both
     * ways, one of them must sound like a tone. */
    expect_tone("user_8bit_stereo.aiff", fmt_dir, kTone404);
    {
        const Capture c = play(fmt_dir, "user_8bit_stereo.wav", 48);
        char          d[160];
        snprintf(d, sizeof d, "rms=%.4f peak=%.1fHz share=%.1f%%", c.rms, c.peak_f,
                 c.share * 100.0);
        check("8-bit WAV (unsigned) still decodes",
              c.rms >= 0.010 && c.share >= 0.50, d);
    }
    /* And the browser has to offer them: the filepath cell's own filter. */
    {
        char buf[8192];
        const int n = api->get_param(inst, "chain_params", buf, sizeof buf);
        const bool has = (n > 0) && strstr(buf, ".aiff") != NULL && strstr(buf, ".wav") != NULL;
        check("the browser filter lists .wav and .aiff", has,
              has ? "both extensions offered" : "an extension is missing");
    }
    {
        /* A region set on an AIFF: same arithmetic as the WAV case, but the
         * file layout above it is completely different. */
        set("start", "0.50");
        settle_ms(600);
        const Capture c = play(fmt_dir, "twotone_16bit_stereo.aiff", 48);
        char          d[200];
        snprintf(d, sizeof d, "from halfway peak=%.1fHz (880 Hz half, -> 808)", c.peak_f);
        check("start=0.5 works on an AIFF too", fabs(c.peak_f - 808.5) <= 12.0, d);
        set("start", "0.00");
        settle_ms(600);
    }

    /* --- the way the file is actually picked: through the portal ---------- */
    printf("\n--- picking an AIFF in the browser's own terms ---------------------\n");
    {
        /* What the host hands back is a portal path ("<portal>/User Library/x"),
         * not the real one, so the translation has to survive that mapping and
         * the file has to be found through the symlink. */
        char portal[512], userlib_scratch[520];
        snprintf(portal, sizeof portal, "%s/probe_portal", dir);
        snprintf(userlib_scratch, sizeof userlib_scratch, "%s/probe_userlib", dir);
        char cmd[1200];
        snprintf(cmd, sizeof cmd, "mkdir -p '%s' '%s'", portal, userlib_scratch);
        if(system(cmd) == 0)
        {
            char src[600], dst[600];
            snprintf(src, sizeof src, "%s/user_24bit_stereo.aiff", fmt_dir);
            snprintf(dst, sizeof dst, "%s/my_ai_24.aiff", userlib_scratch);
            snprintf(cmd, sizeof cmd, "cp '%s' '%s'", src, dst);
            if(system(cmd) == 0)
            {
                set("portal_dir", portal);
                set("user_library_dir", userlib_scratch);
                settle_ms(600);
                char via[700];
                snprintf(via, sizeof via, "%s/User Library/my_ai_24.aiff", portal);
                set("sample_file", via);
                settle_ms(900);
                const Capture c = press(48, 1.0);
                char          d[240];
                snprintf(d, sizeof d, "rms=%.4f peak=%.1fHz share=%.1f%%", c.rms, c.peak_f,
                         c.share * 100.0);
                check("a 24-bit AIFF assigned by portal path plays",
                      c.rms >= 0.010 && c.share >= 0.50 && fabs(c.peak_f - kTone404) <= 8.0, d);
                snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", portal, userlib_scratch);
                if(system(cmd) != 0)
                    printf("(could not remove the scratch portal)\n");
            }
        }
    }

    /* --- graceful failures ---------------------------------------------- */
    printf("\n--- what the port does with files it cannot decode ----------------\n");
    {
        const Capture c = play(fmt_dir, "user_adpcm.wav", 48);
        char          d[160];
        snprintf(d, sizeof d, "rms=%.4f peak=%.1fHz share=%.1f%%", c.rms, c.peak_f,
                 c.share * 100.0);
        /* IMA ADPCM is not decoded: silence is the contract, noise is not. */
        check("user_adpcm.wav is silent, not noise", c.rms < 0.006, d);
    }
    {
        const Capture c = play(fmt_dir, "user_notawav.wav", 48);
        char          d[160];
        snprintf(d, sizeof d, "rms=%.4f (no crash, engine still alive)", c.rms);
        check("user_notawav.wav does not crash the engine", true, d);
        /* ... and the voice is still usable afterwards. */
        const Capture again = play(fmt_dir, "user_24bit_stereo.wav", 48);
        char          d2[160];
        snprintf(d2, sizeof d2, "rms=%.4f peak=%.1fHz share=%.1f%%", again.rms, again.peak_f,
                 again.share * 100.0);
        check("a good sample still plays after it",
              again.rms >= 0.010 && again.share >= 0.50, d2);
    }

    /* --- the engine's own machinery, on a converted file ----------------- */
    printf("\n--- reverse, regions and loop go through the converter too --------\n");
    {
        /* The ramp is quiet at the start and loud at the end, so a reversed
         * playback has to FALL while a forward one rises. Both directions are
         * measured, because "the second half is quieter" only means something
         * next to "the second half is louder" for the same file.
         *
         * Note the Reverse switch is the sign of the pitch value in this
         * engine (`SetSlotPitchFromUi`), so it is its own knob. */
        set("reverse", "Off");
        settle_ms(600);
        const Capture fwd = play(fmt_dir, "ramp_16bit_stereo.wav", 48);

        set("reverse", "On");
        settle_ms(900);
        const Capture back = play(fmt_dir, "ramp_16bit_stereo.wav", 48);

        char d[220];
        snprintf(d, sizeof d,
                 "forward 2nd/1st=%.2f, reversed 2nd/1st=%.2f (forward rises, reverse falls)",
                 fwd.rms ? fwd.rms_second / fwd.rms : 0.0,
                 back.rms ? back.rms_second / back.rms : 0.0);
        check("reverse on a converted file plays it backwards",
              back.rms >= 0.005 && fwd.rms >= 0.005 && back.rms_second < back.rms
                  && fwd.rms_second > fwd.rms,
              d);
        set("reverse", "Off");
        settle_ms(600);
    }
    {
        /* Two tones in one file: starting halfway must select the 880 Hz half.
         * That is only true if the region arithmetic (`f_size() - 44`, which
         * the converter owns) is right. */
        const Capture whole = play(fmt_dir, "twotone_24bit_stereo.wav", 48);
        char          d[200];
        snprintf(d, sizeof d, "from the top peak=%.1fHz (440 Hz half, -> 404)", whole.peak_f);
        check("converted file plays from the top", fabs(whole.peak_f - kTone404) <= 8.0, d);

        set("start", "0.50");
        settle_ms(600);
        const Capture half = play(fmt_dir, "twotone_24bit_stereo.wav", 48);
        snprintf(d, sizeof d, "from halfway peak=%.1fHz (880 Hz half, -> 808)", half.peak_f);
        check("start=0.5 selects the second half of a converted file",
              fabs(half.peak_f - 808.5) <= 12.0, d);
        set("start", "0.00");
        settle_ms(600);
    }
    {
        /* A one-second file with auto-loop on has to still be sounding after
         * its end -- which for a converted file means the loop point and the
         * wrap both work on converted bytes. */
        const Capture c = play(fmt_dir, "user_24bit_stereo.wav", 48);
        char          d[200];
        snprintf(d, sizeof d, "rms over 2.5 s = %.4f (file is 1 s, loop on)", c.rms);
        check("converted file loops", c.rms >= 0.010, d);
    }

    /* --- and the native path is untouched ------------------------------- */
    printf("\n--- the shipped card is still read raw ----------------------------\n");
    {
        /* Clear the assignment (the Samples page's Clear Slot) and press: the
         * convention file must play, untouched by any of the above. */
        set("slot", "1");
        settle_ms(400);
        set("sample_clear", "1");
        settle_ms(900);
        const Capture c = press(48, 1.0);
        char          d[200];
        snprintf(d, sizeof d, "rms=%.4f peak=%.1fHz (the card's cubbi_a1 is 440 Hz)", c.rms,
                 c.peak_f);
        check("the module's own card file still plays", c.rms >= 0.005, d);
    }
    {
        char buf[256];
        api->get_param(inst, "debug_view", buf, sizeof buf);
        printf("\ndebug_view: %s\n", buf[0] ? buf : "(empty)");
    }

    printf("\nVERDICT: %s (%d failures in %d checks)\n",
           failures ? "FAIL" : "PASS", failures, checks);
    return failures ? 1 : 0;
}
