/* mode_bank_probe — voice_mode, bank and slot must not change the sample.
 *
 * One sample (jammi_a1) covers the whole keybed. Writing cubbi, another bank
 * or another slot must leave pad 80 — the sample's own speed — on that file.
 * The other tones exist so a switch would be audible as the wrong frequency.
 *
 * It needs a sample directory whose files are labelled BY FREQUENCY, so the
 * spectrum says which file played:
 *
 *   cubbi_a1 440  cubbi_a2 480  cubbi_a3 520  cubbi_a4 560  cubbi_a5 600
 *   cubbi_b1 640  cubbi_b2 680
 *   jammi_a1 261.63 (C4: the card's own tone)  jammi_a2 880  jammi_a3 920
 *   jammi_b1 1000
 *
 * tools/make_probe_samples.py writes exactly that set. Every measurement is
 * then "which file is this", not "does it make a noise".
 *
 * Build (from schwung-chompi/):
 *   g++ -std=c++17 -O1 -D_Static_assert=static_assert \
 *       -I module/dsp/third_party/schwung -o /tmp/mode_bank_probe \
 *       tools/mode_bank_probe.cpp -ldl
 * Run:
 *   python3 tools/make_probe_samples.py /tmp/fullmod/samples
 *   /tmp/mode_bank_probe build/chompi_move.so /tmp/fullmod
 */

#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "host/plugin_api_v1.h"

static uint8_t mailbox[8 * 1024 * 1024] __attribute__((aligned(4096)));

static plugin_api_v2_t* api  = nullptr;
static void*            inst = nullptr;

static double goertzel(const double* x, long n, double f)
{
    const double w = 2.0 * M_PI * f / (double)MOVE_SAMPLE_RATE;
    const double c = 2.0 * cos(w);
    double       s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for(long i = 0; i < n; i++)
    {
        s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

static int checks = 0, failures = 0;

/* Presses `note`, renders, and reports the dominant frequency of what came
 * out. Everything is in Hz and the tone set is in Hz, so the answer reads
 * directly as "which sample is this" -- and can be checked against the number
 * it should be. The engine runs at 44.1 kHz while the files are 48 kHz, hence
 * the 44100/48000 factor, and JAMMI additionally transposes by the note. */
static void check(const char* what, bool ok, const char* detail)
{
    printf("%s %-52s %s\n", ok ? " ok " : "FAIL", what, detail);
    checks++;
    if(!ok)
        failures++;
}

static double press_note(const char* label, int note, double expect_hz)
{
    uint8_t on[3]  = {0x90, (uint8_t)note, 100};
    uint8_t off[3] = {0x80, (uint8_t)note, 0};
    api->on_midi(inst, on, 3, MOVE_MIDI_SOURCE_INTERNAL);

    int16_t       out[256];
    static double x[1 << 15];
    long          n = 0, rn = 0;
    double        sum = 0.0;

    for(int b = 0; b < (int)(0.5 * MOVE_SAMPLE_RATE / MOVE_FRAMES_PER_BLOCK); b++)
    {
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        for(int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++)
        {
            const double s = out[2 * i] / 32768.0;
            if(n < (long)(sizeof x / sizeof x[0]))
                x[n++] = s;
            sum += s * s;
            rn++;
        }
        usleep(2900);
    }
    api->on_midi(inst, off, 3, MOVE_MIDI_SOURCE_INTERNAL);

    const double rms = sqrt(sum / (rn ? rn : 1));

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

    const bool ok = (rms >= 0.004) && (fabs(best_f - expect_hz) <= 6.0);
    printf("%s %-44s rms=%6.4f peak=%7.1fHz  want %6.1f\n",
           ok ? " ok " : "FAIL", label, rms, best_f, expect_hz);
    checks++;
    if(!ok)
        failures++;
    return best_f;
}

/* The diagnostic string, for the checks that are about bookkeeping rather
 * than sound: debug_voice prints "ram0/ram1 slotN" per voice, i.e. whether the
 * voice reads a file or CHOMPI's built-in RAM buffer, and which slot it
 * believes it is bound to. */
static const char* dbg()
{
    static char buf[4096];
    if(api->get_param(inst, "debug_voice", buf, sizeof buf) <= 0)
        buf[0] = '\0';
    return buf;
}

static void set(const char* k, const char* v)
{
    api->set_param(inst, k, v);
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

int main(int argc, char** argv)
{
    const char* so  = argc > 1 ? argv[1] : "build/chompi_move.so";
    const char* dir = argc > 2 ? argv[2] : "/tmp/fullmod";

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

    api  = init(&h);
    inst = api->create_instance(dir, NULL);
    settle_ms(1500);

    char buf[1024];
    const char* mode = (api->get_param(inst, "voice_mode", buf, sizeof buf) > 0) ? strdup(buf) : "?";
    printf("at load: voice_mode=%s\n\n", mode);

    /* n.b. the engine runs at 44.1 kHz while these files are 48 kHz, so every
     * tone lands at 0.919x its nominal frequency (see sample_map_probe), and
     * JAMMI additionally transposes by the pad. */
    /* PORT PATCH P22: one slot is one OCTAVE -- twelve pads -- and the pads are
     * a chromatic ladder read left to right and bottom to top (PORT PATCH P27),
     * so pad 68 is the slot the `slot` knob selects and pad 80 is the NEXT slot
     * up. A pad's distance from its own octave's C is the pitch, and the whole
     * keybed reads from pad 80: -12 semitones at the bottom left, +19 at the
     * top right, both exactly what the ladder says. The external-note presses
     * this used to do (MIDI 48/50/52) are not the device's pads at all - they
     * fall back to the same ladder, which is a different (and separately
     * probed) path, so they are not pressed here.
     *
     * One sample (jammi_a1, C4 261.63 Hz content → 240.4 through a 44.1 kHz
     * engine reading a 48 kHz file). Pad 80 is 1.00x. Mode, bank and slot must
     * not switch the file.
     *
     * This reference was 840 Hz because jammi_a1 used to be an 840 Hz tone.
     * When the card set changed, the module kept playing the file correctly and
     * this probe went red on five checks -- a constant ratio of 840/261.63 =
     * 3.21 against every reading, which is the signature of a stale reference
     * rather than a wrong playback rate (the ratios BETWEEN keys stayed right).
     * Checked against a build from before the tape window existed: identical
     * numbers, so it never was a playback fault. If the samples change again,
     * measure the file first and change the constant here. */
    const double one = 261.63 * 44100.0 / 48000.0;
    printf("--- one sample on the keybed; mode/bank/slot must not switch it --\n");
    press_note("pad 80, the sample at its own speed (jammi_a1)", 80, one);
    press_note("pad 68, that speed halved", 68, one * 0.5);
    press_note("pad 92, that speed doubled", 92, one * 2.0);
    set("voice_mode", "cubbi");
    set("bank", "1");
    set("slot", "5");
    settle_ms(800);
    press_note("after cubbi / bank 1 / slot 5, still jammi_a1", 80, one);
    press_note("and the octave above it is still that file", 92, one * 2.0);
    check("voice stays on the card sample, slot 1", strstr(dbg(), "ram0 slot1") != nullptr,
          dbg());
    printf("\n%s (%d failure%s in %d checks)\n", failures ? "VERDICT: FAIL" : "VERDICT: PASS",
           failures, failures == 1 ? "" : "s", checks);
    return failures ? 1 : 0;
}
