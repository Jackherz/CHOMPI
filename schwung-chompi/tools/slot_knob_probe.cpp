/* slot_knob_probe — does moving the `slot` knob break key playback?
 *
 * Reported from the device: "as I move to the second page and change the
 * sample slot knob the sound disappears and only returns if I restart the
 * module". `slot` is a UI selection (which slot the Samples page edits), but
 * the adapter also called the engine's SetVoiceSlot() with it, which rebinds
 * every voice to that slot's file. This probe measures the consequence:
 * press a key, move the knob, press again — and, crucially, press again after
 * moving the knob BACK. If the sound does not come back, the voice state is
 * wedged and a restart is the only cure, exactly as reported.
 *
 * Build (from schwung-chompi/):
 *   g++ -std=c++17 -O1 -D_Static_assert=static_assert \
 *       -I module/dsp/third_party/schwung -o /tmp/slot_knob_probe \
 *       tools/slot_knob_probe.cpp -ldl
 * Run:
 *   /tmp/slot_knob_probe build/chompi_move.so .
 */

#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "host/plugin_api_v1.h"

#define NOTE_C3 48 /* not a pad: the reading below just needs a note that
                      * sounds slot 1 (the chromatic fallback clamps to it) */

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

/* "Is it silent?" is a question about THIS build's level: the hardware's gain
 * default (.704 on the encoder) is unity, which puts this directory's sample at
 * roughly 0.005 measured over a press -- exactly where an absolute 0.005
 * threshold sits. Anchor on the baseline press instead. */
static double g_silent_below = 0.002;

/* Presses the key, renders `seconds` of audio, releases it, and reports what
 * came out. rms is the whole point: silence is 0.0000. */
static double press_note(const char* label, int note, double seconds)
{
    uint8_t on[3]  = {0x90, (uint8_t)note, 100};
    uint8_t off[3] = {0x80, (uint8_t)note, 0};
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
    api->on_midi(inst, off, 3, MOVE_MIDI_SOURCE_INTERNAL);

    const double rms = sqrt(rms_sum / (rms_n ? rms_n : 1));

    double best_f = 0.0, best_e = 0.0;
    for(double f = 60.0; f <= 6000.0; f += 25.0)
    {
        const double e = goertzel(x, n, f);
        if(e > best_e)
        {
            best_e = e;
            best_f = f;
        }
    }

    if(label)
        printf("%-30s rms=%7.4f  peak=%6.1fHz  %s\n", label, rms, best_f,
               rms > g_silent_below ? "sound" : "SILENT");
    return rms;
}

static double press(const char* label, double seconds)
{
    return press_note(label, NOTE_C3, seconds);
}

/* Open file descriptors in this process -- /proc is enough on Linux, and a
 * count that climbs with every key press is a leak with a name. */
static int fd_count()
{
    DIR* d = opendir("/proc/self/fd");
    if(!d)
        return -1;
    int   n = 0;
    while(readdir(d))
        n++;
    closedir(d);
    return n;
}

static void dump_voices(const char* when)
{
    static char buf[4096];
    const int   n = api->get_param(inst, "debug_voice", buf, sizeof buf);
    printf("--- voices %s ---\n%s\n", when, n > 0 ? buf : "(none)");
}

static void wait_ms(int ms)
{
    usleep(ms * 1000);
}

static void settle(int ms)
{
    int16_t out[256];
    for(int i = 0; i < ms / 3; i++)
    {
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        usleep(2900);
    }
}

static void set_slot(int slot)
{
    char v[8];
    snprintf(v, sizeof v, "%d", slot);
    api->set_param(inst, "slot", v);
}

int main(int argc, char** argv)
{
    const char* so  = argc > 1 ? argv[1] : "build/chompi_move.so";
    const char* dir = argc > 2 ? argv[2] : ".";

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

    {
        char map_path[512];
        snprintf(map_path, sizeof map_path, "%s/samples/samples.map", dir);
        remove(map_path);
    }

    api  = init(&h);
    inst = api->create_instance(dir, NULL);
    api->set_param(inst, "voice_mode", "cubbi");
    api->set_param(inst, "bank", "0");
    set_slot(1);
    settle(1500);

    int fails = 0;
    printf("fds at start: %d\n", fd_count());

    const double base = press("1. slot 1 (baseline)", 0.4);
    dump_voices("baseline");
    if(base <= 0.002) { printf("   baseline is silent — nothing to compare\n"); fails++; }
    g_silent_below = base * 0.4; /* a later press at 40% of the first one is the symptom */

    /* One step of the knob, which is what the device report describes. */
    set_slot(2);
    settle(1200);
    press("2. after slot -> 2", 0.4);

    set_slot(1);
    settle(1200);
    const double back = press("3. back to slot 1", 0.4);
    if(back <= base * 0.4) { printf("   sound did NOT come back\n"); fails++; }

    /* Slot 0: our param allowed it, and there is no cubbi_a0.wav. */
    set_slot(0);
    settle(1200);
    press("4. after slot -> 0", 0.4);

    set_slot(1);
    settle(1200);
    const double back0 = press("5. back to slot 1 (from 0)", 0.4);
    if(back0 <= base * 0.4) { printf("   sound did NOT come back\n"); fails++; }

    /* A sweep, the way a knob gets scrolled before it settles. */
    for(int s = 1; s <= 15; s++)
    {
        set_slot(s);
        usleep(60 * 1000);
    }
    set_slot(1);
    settle(1500);
    const double swept = press("6. back to slot 1 (after sweep)", 0.4);
    if(swept <= base * 0.4) { printf("   sound did NOT come back\n"); fails++; }

    printf("fds after the knob sweep: %d\n", fd_count());

    /* The pads, which is what the user actually plays. PORT PATCH P22: one slot
     * is one OCTAVE -- twelve pads -- so with `slot` at 1 the bottom-left pad is
     * slot 1's octave root and pad 80 is slot 2's; the two are the only slots
     * this directory has files for (on a full card the rest are present, and the
     * pads for them are silent here for a reason that has nothing to do with any
     * of this). */
    set_slot(1);
    press_note("7. pad 68 (slot 1, bottom-left C)", 68, 0.4);
    press_note("7. pad 80 (slot 2, the C twelve pads up)", 80, 0.4);
    dump_voices("after two keys");

    /* Every key press calls SetVoiceSlot(), exactly like the knob used to --
     * so fast playing across slots opens files in the same burst. If that
     * wedges the voice, it is not a knob bug: playing does it. */
    for(int n = 68; n <= 99; n++)
    {
        press_note(NULL, n, 0.06);
        usleep(20 * 1000);
    }
    settle(1200);
    const double after_keys = press_note("8. pad 68 after a key sweep", 68, 0.4);
    if(after_keys <= base * 0.4) { printf("   sound did NOT come back\n"); fails++; }

    printf("fds after the key sweep: %d\n", fd_count());

    for(int i = 0; i < 30; i++)
    {
        press_note(NULL, (i & 1) ? 80 : 68, 0.05);
        usleep(15 * 1000);
    }
    printf("fds after fast playing: %d\n", fd_count());
    dump_voices("after the key sweep");

    settle(1200);
    const double after_fast = press_note("9. pad 68 after fast playing", 68, 0.4);
    if(after_fast <= base * 0.4) { printf("   sound did NOT come back\n"); fails++; }

    /* A file that has since been deleted is the case that used to be fatal:
     * the open fails, and a failed open left the reader's pending-open count
     * set, which gates the refill and the reader's own recovery -- all seven
     * voices wedged and the module stayed silent until it was recreated.
     * Pressing the slot with the missing file must be silent; pressing a slot
     * with a real file must still sound. */
    set_slot(2);
    wait_ms(300);
    api->set_param(inst, "sample_file", "/tmp/chompi_vanished.wav");
    wait_ms(1200);
    set_slot(1); /* back to the window: pads 68.. are slot 1, pads 80.. slot 2 */
    wait_ms(300);
    press_note("10. pad 80 (slot 2's file is gone)", 80, 0.4);
    const double after_vanished = press_note("10. pad 68 (slot 1) still plays", 68, 0.4);
    if(after_vanished <= base * 0.4) { printf("   a missing file silenced the module\n"); fails++; }
    printf("fds after the vanished file: %d\n", fd_count());
    dump_voices("after the vanished file");

    /* Walking slots 3-14 leaves the engine's 0-byte placeholders behind (a
     * missing convention file is created on open, as on the firmware). Remove
     * the empties this run caused; never a shipped sample. */
    {
        int cleaned = 0;
        for(int slot = 0; slot <= 14; slot++)
        {
            char path[512];
            struct stat st;
            snprintf(path, sizeof path, "%s/samples/cubbi_a%d.wav", dir, slot);
            if(stat(path, &st) == 0 && st.st_size == 0 && remove(path) == 0)
                cleaned++;
        }
        if(cleaned)
            printf("cleaned %d placeholder file(s) from samples/\n", cleaned);
        /* The map too: this probe writes slot overrides through the plugin, and
         * a leftover assignment (especially one pointing at /tmp) would change
         * what the next probe or a hand-run of the module in this directory
         * hears. */
        char map_path[512];
        snprintf(map_path, sizeof map_path, "%s/samples/samples.map", dir);
        remove(map_path);
    }

    printf("\n%s\n", fails ? "FAIL: moving the slot knob silences the voice"
                          : "PASS: the slot knob does not break playback");
    return fails ? 1 : 0;
}
