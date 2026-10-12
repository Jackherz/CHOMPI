/* mode_bank_probe — what do voice_mode, bank and slot actually change?
 *
 * The device report was "changing the mode from jammi to cubbi makes the sound
 * disappear, and bank/slot do nothing in either mode". Those three params are
 * supposed to be the heart of the instrument, so this measures them instead of
 * arguing about them.
 *
 * It needs a sample directory whose files are labelled BY FREQUENCY, so the
 * spectrum says which file played:
 *
 *   cubbi_a1 440  cubbi_a2 480  cubbi_a3 520  cubbi_a4 560  cubbi_a5 600
 *   cubbi_b1 640  cubbi_b2 680
 *   jammi_a1 840  jammi_a2 880  jammi_a3 920  jammi_b1 1000
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
    /* PORT PATCH P22: one slot is one OCTAVE -- twelve pads -- and the pads are a
     * chromatic ladder read left to right and bottom to top, so pad 68 is the
     * slot the `slot` knob selects and pad 80 is the NEXT slot up. A pad's
     * distance from its own octave's C is the pitch, hence the x1.4983 factors
     * (a G, +7 semitones: pad 75 in the first octave, pad 87 in the second).
     * The external-note presses this used to do (MIDI 48/50/52) are not the
     * device's pads at all - they fall back to a chromatic reading, which is a
     * different (and separately probed) path. */
    printf("--- CUBBI: one slot per octave of the pads -------------------------\n");
    set("voice_mode", "cubbi");
    settle_ms(800);
    set("slot", "1");
    settle_ms(600);
    press_note("cubbi slot 1, bottom-left pad (slot 1: 440)", 68, 404.25);
    press_note("cubbi slot 1, pad G (+7 st)", 75, 404.25 * 1.4983);
    press_note("cubbi slot 2, pad 80 (480)", 80, 441.00);
    press_note("cubbi slot 2, pad G (+7 st)", 87, 441.00 * 1.4983);
    set("slot", "3");
    settle_ms(600);
    press_note("cubbi slot 3, pad 68 (520)", 68, 477.75);
    press_note("cubbi slot 4, pad 80 (560)", 80, 514.50);

    printf("\n--- bank changes which set the pads play ---------------------------\n");
    set("bank", "1");
    set("slot", "1");
    settle_ms(800);
    press_note("cubbi bank b, pad 68 (bank b slot 1: 640)", 68, 588.00);
    press_note("cubbi bank b, pad 80 (bank b slot 2: 680)", 80, 624.75);
    set("bank", "0");
    settle_ms(800);
    press_note("cubbi bank a again, pad 68 (440)", 68, 404.25);

    printf("\n--- JAMMI: one sample, played chromatically -----------------------\n");
    set("voice_mode", "jammi");
    settle_ms(800);
    set("slot", "1");
    settle_ms(800);
    /* JAMMI's own speed is pad 80 (PORT PATCH P22), so that pad is the 840 Hz
     * file at the 0.919x of the rate mismatch, and four pads up is a major
     * third (x1.2599). */
    press_note("jammi slot 1 (840), pad 80", 80, 771.75);
    press_note("jammi slot 1 (840), pad 84 (x1.2599)", 84, 771.75 * 1.2599);
    set("slot", "2");
    settle_ms(800);
    press_note("jammi slot 2 (880), pad 80", 80, 808.50);
    set("slot", "3");
    settle_ms(800);
    press_note("jammi slot 3 (920), pad 80", 80, 845.25);
    /* Bank b's set in this tone directory only has slot 1, so select it before
     * switching banks -- the same thing a user does. */
    set("slot", "1");
    set("bank", "1");
    settle_ms(800);
    press_note("jammi bank b slot 1 (1000), pad 80", 80, 918.75);

    printf("\n--- slot 15 is the recording buffer, not a file -------------------\n");
    /* Slot 15 plays CHOMPI's built-in RAM buffer (a decaying tone at 523 Hz, so
     * half speed twelve pads below the JAMMI root). What matters here is the
     * bookkeeping: the voice must know it is on the buffer, or moving back to a
     * card slot leaves it reading the wrong thing forever. */
    set("slot", "15");
    settle_ms(600);
    /* FillDefaultSample() writes its tone with a .5 factor -- 261.6 Hz at the
     * buffer's rate -- so the pad an octave below the root (x0.5) lands at
     * 130.8, not 261. */
    press_note("jammi slot 15 (RAM buffer), pad 68", 68, 130.8);
    check("the voice reads the RAM buffer on slot 15", strstr(dbg(), "ram1 slot15") != nullptr,
          "(debug_voice)");
    set("slot", "1");
    settle_ms(600);
    /* Still in bank b from the section above, so slot 1 is jammi_b1 (1000 Hz). */
    press_note("back to slot 1 (bank b: 1000), pad 80", 80, 918.75);
    check("and reads the card sample again on slot 1", strstr(dbg(), "ram0 slot1") != nullptr,
          "(debug_voice)");

    printf("\n--- back to cubbi, and the slot knob the samples page uses --------\n");
    set("voice_mode", "cubbi");
    settle_ms(800);
    set("bank", "0");
    settle_ms(800);
    set("slot", "3");
    settle_ms(600);
    press_note("cubbi bank a, pad 68  (slot 3: 520)", 68, 477.75);
    printf("\n%s (%d failure%s in %d checks)\n", failures ? "VERDICT: FAIL" : "VERDICT: PASS",
           failures, failures == 1 ? "" : "s", checks);
    return failures ? 1 : 0;
}
