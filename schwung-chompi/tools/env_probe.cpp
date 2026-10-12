/* env_probe — what do Attack, Decay, Loop and Sustain actually do?
 *
 * The report from the device: "Attack and decay don't work the same way they do
 * on the CHOMPI hardware ... attack only works when decay is changed", and "the
 * loop button does nothing". Both live in the same place — the envelope and the
 * CUBBI key path — so this measures them instead of arguing about them.
 *
 * It plays tones with a KNOWN envelope (a plain 440 Hz sine at a fixed level)
 * and reports two numbers per setting:
 *
 *   attack  = seconds from key-down to 90% of the steady level
 *   release = seconds from key-up to 5% of the steady level   ("decay" on the panel)
 *
 * with everything else held constant — so independence is visible as a row that
 * does not move. It runs the whole matrix in both voice modes, because the modes
 * take different paths through the engine (JAMMI writes every voice, CUBBI
 * re-applies its per-slot values on each key press).
 *
 * Loop/sustain get their own section: a 0.30 s file held for 2 s. If the sound
 * continues past the end of the file, the sample is looping; if it stops, it is
 * one-shot. That is the difference the Loop button is supposed to make — on the
 * shipped 13 s samples it is inaudible unless you hold a key for 13 s.
 *
 * Build (from schwung-chompi/):
 *   g++ -std=c++17 -O1 -D_Static_assert=static_assert \
 *       -I module/dsp/third_party/schwung -o /tmp/env_probe \
 *       tools/env_probe.cpp -ldl
 * Run:
 *   /tmp/env_probe build/chompi_move.so /tmp/envprobe
 */

#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "host/plugin_api_v1.h"

static uint8_t mailbox[8 * 1024 * 1024] __attribute__((aligned(4096)));

static plugin_api_v2_t* api  = nullptr;
static void*            inst = nullptr;

static int checks = 0, failures = 0;

static void check(const char* what, bool ok, const char* detail = "")
{
    printf("%s %-56s %s\n", ok ? " ok " : "FAIL", what, detail);
    checks++;
    if(!ok)
        failures++;
}

static void set(const char* k, const char* v)
{
    api->set_param(inst, k, v);
}

/* One WAV: `seconds` of a 440 Hz sine at 0.5, PCM16 stereo, 44.1 kHz — the
 * canonical 44-byte-header form, so the engine reads it as-is. */
static void write_tone(const char* path, double seconds)
{
    const int      sr = 44100;
    const uint32_t n  = (uint32_t)(seconds * sr);
    FILE*          f  = fopen(path, "wb");
    if(!f)
    {
        printf("cannot write %s\n", path);
        exit(1);
    }
    struct
    {
        char     riff[4];
        uint32_t size;
        char     wave[4];
        char     fmt[4];
        uint32_t fmt_size;
        uint16_t format;
        uint16_t channels;
        uint32_t rate;
        uint32_t byte_rate;
        uint16_t align;
        uint16_t bits;
        char     data[4];
        uint32_t data_size;
    } h;
    memcpy(h.riff, "RIFF", 4);
    memcpy(h.wave, "WAVE", 4);
    memcpy(h.fmt, "fmt ", 4);
    memcpy(h.data, "data", 4);
    h.fmt_size   = 16;
    h.format     = 1;
    h.channels   = 2;
    h.rate       = sr;
    h.byte_rate  = sr * 4;
    h.align      = 4;
    h.bits       = 16;
    h.data_size  = n * 4;
    h.size       = 36 + h.data_size;
    fwrite(&h, sizeof h, 1, f);
    for(uint32_t i = 0; i < n; i++)
    {
        const int16_t s = (int16_t)(0.5 * sin(2.0 * M_PI * 440.0 * i / sr) * 32000.0);
        int16_t       frame[2] = {s, s};
        fwrite(frame, sizeof frame, 1, f);
    }
    fclose(f);
}

/* Renders `seconds`, returning the peak envelope in 10 ms windows, and driving
 * a key down/up at given times. Used for both the onset and the tail. */
static void capture(double seconds, int note, double key_down, double key_up,
                    double* env, int env_count)
{
    const int blocks = (int)(seconds * MOVE_SAMPLE_RATE / MOVE_FRAMES_PER_BLOCK);
    int16_t   out[256];
    double    t    = 0.0;
    int       win  = 0;
    double    peak = 0.0;
    /* One note-on and one note-off, ever. A `gate` flag that simply flips back
     * to false at key_up makes the note-on condition true again on the very
     * next block, and the whole capture turns into a machine-gun retrigger:
     * the voice never gets to release, "still playing" checks fire, and a
     * measurement of pad 2 is really a measurement of pad 1 being pressed 200
     * times. Send each edge once. */
    bool down_sent = false, up_sent = false;

    for(int i = 0; i < env_count; i++)
        env[i] = 0.0;

    for(int b = 0; b < blocks; b++)
    {
        const double t0 = t;
        if(!down_sent && t0 >= key_down)
        {
            uint8_t on[3] = {0x90, (uint8_t)note, 100};
            api->on_midi(inst, on, 3, MOVE_MIDI_SOURCE_INTERNAL);
            down_sent = true;
        }
        if(down_sent && !up_sent && t0 >= key_up)
        {
            uint8_t off[3] = {0x80, (uint8_t)note, 0};
            api->on_midi(inst, off, 3, MOVE_MIDI_SOURCE_INTERNAL);
            up_sent = true;
        }

        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        for(int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++)
        {
            const double v = fabs(out[2 * i] / 32768.0);
            if(v > peak)
                peak = v;
        }

        t += (double)MOVE_FRAMES_PER_BLOCK / MOVE_SAMPLE_RATE;
        const int w = (int)(t / 0.01);
        if(w != win && win < env_count)
        {
            env[win] = peak;
            if(w < env_count)
                env[w] = peak; /* carry into the new window as its first sample */
            peak = 0.0;
            win  = w;
        }
        usleep(1500);
    }
    if(win < env_count)
        env[win] = peak;
}

/* Peak level in a window, in seconds. */
static double level_between(const double* env, int env_count, double from, double to)
{
    double m = 0.0;
    const int a = (int)(from * 100.0), b = (int)(to * 100.0);
    for(int i = a; i < b && i < env_count; i++)
        if(env[i] > m)
            m = env[i];
    return m;
}

/* Seconds from `from` to the first window at or below `level` (the tail). */
static double time_to_below(const double* env, int env_count, double from, double level)
{
    for(int i = (int)(from * 100.0); i < env_count; i++)
        if(env[i] <= level)
            return i * 0.01 - from;
    return -1.0;
}

/* Two independent readings, both anchored on the level just before key-up so a
 * long attack cannot masquerade as a fast one:
 *
 *   fade_in  = early level / held level. 1.0 = instant, ~0 = still fading in.
 *   release  = seconds after key-up until the tail is 10% of the held level.
 */
/* Key 15 == CUBBI slot 1 (DSPEngine::KeyToSlot); it is also fine in JAMMI,
 * where the note only sets the playback ratio. */
static const int kProbeKey = 15;

static void measure(const char* label, double* fade_in, double* release_s)
{
    static double env[400];

    /* 3 s: key down at 0.2 s, key up at 1.8 s, tail to 3 s.
     * The note matters in CUBBI: the key decides which slot sounds (KeyToSlot),
     * and key 15 is slot 1 -- the slot these sweeps assign their tone to. Any
     * other key would play a different (or no) sample and measure that instead. */
    capture(3.0, kProbeKey, 0.2, 1.8, env, 300);
    const double early = level_between(env, 300, 0.30, 0.45);   /* 0.1-0.25 s after down */
    const double held  = level_between(env, 300, 1.60, 1.75);   /* just before up */
    const double fade  = (held > 1e-6) ? early / held : 0.0;
    const double rel   = time_to_below(env, 300, 1.8, held * 0.10);

    *fade_in    = fade;
    *release_s  = rel;

    printf("   %-44s fade-in=%5.2f (%s) release=%6.3fs (held %.3f)\n",
           label, fade, fade < 0.75 ? "still rising" : "immediate", rel, held);
}


/* PORT PATCH P22: one slot is one OCTAVE of the pads, so the two slots the
 * CUBBI checks below use are the two octave roots of the keybed: the bottom-left
 * C, which plays whatever `slot` selects, and the C an octave above it, which
 * plays the NEXT slot up. (These used to be MIDI 48 and 50, one key per slot;
 * those notes are not pads at all and now take the chromatic fallback.) */
static const int kPadOfSelectedSlot = 68; /* bottom-left pad, the octave root */
static const int kPadOfNextSlot     = 84; /* the C one octave up             */

/* A blocking wait that does not need the render loop. */
static void sleep_ms(int ms)
{
    usleep(ms * 1000);
}

static char rd[256];
static void read_back(const char* key)
{
    rd[0] = '\0';
    api->get_param(inst, key, rd, sizeof rd);
}
/* ---------------------------------------------------------------------------
 * One gesture, measured absolutely. `capture()` renders 4 s with the key held
 * from 0.3 s to 1.8 s, and this reads the envelope at points whose meaning is
 * fixed by the gesture, not by the setting under test:
 *
 *   early  0.55-0.70 s   == 0.25-0.40 s after key down (past the open latency,
 *                           which is ~0.2 s on the first press of a file)
 *   mid    1.00-1.15 s
 *   held   1.60-1.75 s   just before key up
 *   peak   the whole gesture -- the reference for "how loud this note gets"
 *   tail   2.05-2.45 s   after key up, where a long release is still audible
 *
 * Every check compares against `peak` (the note's own loudness) rather than
 * against a later window: a long attack makes a late window quiet too, and a
 * ratio of two quiet windows says nothing.
 * ------------------------------------------------------------------------- */
struct Reading
{
    double early, mid, held, peak, tail;
};

static int kReadingNote = 48;

/* Is any voice still making sound? Reads the engine's own per-voice state. */
static bool any_voice_playing()
{
    read_back("debug_voice");
    for(int i = 0; i < 8; i++)
    {
        char pat[12];
        snprintf(pat, sizeof pat, "v%d p1", i);
        if(strstr(rd, pat) != nullptr)
            return true;
    }
    return false;
}

/** Releases everything and renders until no voice is playing, so the next
 *  measurement starts from silence. Without this a probe measures the previous
 *  note's tail: an 0.75 setting has a 1.7 s release, and the window it lands in
 *  is exactly the one the next press is being judged by.
 *
 *  It doubles as a check of its own -- a voice that never goes quiet after its
 *  key is released is the "stuck pad" failure. */
static int settle()
{
    const int notes[] = {15, 48, 50};
    for(size_t i = 0; i < sizeof notes / sizeof notes[0]; i++)
    {
        uint8_t off[3] = {0x80, (uint8_t)notes[i], 0};
        api->on_midi(inst, off, 3, MOVE_MIDI_SOURCE_INTERNAL);
    }

    /* At least ~2.3 s even when nothing is playing: the reader only pre-buffers
     * a stopped voice every 2000 ms (SampleReader::CacheSamples), and a press
     * made before that window reopens starts from an empty FIFO -- silence at
     * the start of the note, which is exactly what an envelope measurement must
     * not see. */
    int16_t out[256];
    const int min_blocks = (int)(2.3 * MOVE_SAMPLE_RATE / MOVE_FRAMES_PER_BLOCK);
    for(int i = 0; i < 1200; i++)
    {
        if(i >= min_blocks && !any_voice_playing())
            return i;
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        usleep(1500);
    }
    return -1;
}

static void press(int note, Reading* r)
{
    static double env[400];
    const int waited = settle();
    if(waited < 0)
        check("A voice was still playing before the next press", false);
    capture(4.0, note, 0.3, 1.8, env, 400);
    r->early = level_between(env, 400, 0.55, 0.70);
    r->mid   = level_between(env, 400, 1.00, 1.15);
    r->held  = level_between(env, 400, 1.60, 1.75);
    r->tail  = level_between(env, 400, 2.05, 2.45);
    r->peak  = level_between(env, 400, 0.30, 1.80);
}

static void print_reading(const char* label, const Reading& r)
{
    const double ref = r.peak > 1e-6 ? r.peak : 1.0;
    printf("   %-26s early %5.2f  mid %5.2f  held %5.2f  tail %5.2f  (peak %.4f)\n",
           label, r.early / ref, r.mid / ref, r.held / ref, r.tail / ref, r.peak);
}

/* A value the page shows, as a float. */
static void check_close(const char* what, const char* key, double want, double tol)
{
    read_back(key);
    const double got = strtod(rd, nullptr);
    char         label[160];
    snprintf(label, sizeof label, "%s (%s = %.4f, wanted %.4f)", what, key, got, want);
    check(label, fabs(got - want) <= tol);
}

static void check_text(const char* what, const char* key, const char* want)
{
    read_back(key);
    char label[160];
    snprintf(label, sizeof label, "%s (%s = %s, wanted %s)", what, key, rd, want);
    check(label, strcmp(rd, want) == 0);
}

/* Assign the long tone to the selected slot, in whatever mode/bank is current:
 * the sample map is per (mode, bank, slot), so each mode needs its own. */
static void assign(const char* path)
{
    set("sample_file", path);
    usleep(450 * 1000);
}

/* ---------------------------------------------------------------------------
 * The rig. One instance per measurement group, because the engine carries
 * per-voice state across presses by design (voice stealing, retriggers, release
 * tails) and a probe that reuses one instance ends up measuring the previous
 * note. A device reloads the module; the probe does the same and then judges
 * exactly one thing at a time.
 * ------------------------------------------------------------------------- */
static const char* g_dir   = nullptr;
static char        g_long[700];
static char        g_short[700];

static void new_instance()
{
    if(inst != nullptr)
    {
        api->destroy_instance(inst);
        usleep(150 * 1000);
    }
    inst = api->create_instance(g_dir, NULL);
    usleep(250 * 1000);
}

/** Selects a slot in a mode and points it at one of the probe's tones. */
static void bind_slot(const char* mode, const char* bank, const char* slot, const char* file)
{
    set("voice_mode", mode);
    set("bank", bank);
    set("slot", slot);
    usleep(250 * 1000);
    set("sample_file", file);
    usleep(550 * 1000);
}

/** The tape page as ui.h starts it. The values belong to the slot and are
 *  kept across instances on purpose, so a group that did not write gain would
 *  measure what an earlier group left there (section 2 writes gain .2 into
 *  slot 1, and every later absolute level inherits it). Set the whole page. */
static void reset_page()
{
    set("gain",     "0.704"); /* encoder units: unity */
    set("pan",      "0.5");
    set("varspeed", "0.83");  /* 1x */
    set("reverse",  "Off");
    set("attack",   "0");
    set("decay",    "0");
    sleep_ms(150);
}

/** Attack/decay pair for one note, on a fresh instance. */
static void envelope(const char* mode, const char* attack, const char* decay, int note, Reading* r)
{
    new_instance();
    bind_slot(mode, "0", "1", g_long);
    reset_page();
    set("auto_loop", "On");
    set("sustain", "On");
    set("attack", attack);
    set("decay", decay);
    sleep_ms(600); /* past the reader's 2000 ms pre-buffer gate */
    press(note, r);
}

int main(int argc, char** argv)
{
    const char* so = argc > 1 ? argv[1] : "build/chompi_move.so";
    g_dir          = argc > 2 ? argv[2] : "/tmp/envprobe";

    char cmd[1024];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    if(system(cmd) != 0)
        return 1;

    /* The tones live in the module's sample directory (<module dir>/samples,
     * the default v2_create_instance picks), so the engine's convention scan
     * finds them exactly as it finds the shipped card. */
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/samples'", g_dir);
    if(system(cmd) != 0)
        return 1;
    snprintf(g_long, sizeof g_long, "%s/samples/long.wav", g_dir);
    snprintf(g_short, sizeof g_short, "%s/samples/short.wav", g_dir);
    write_tone(g_long, 4.0);
    write_tone(g_short, 0.30);

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
    inst = api->create_instance(g_dir, NULL);

    /* --- 1. the hardware's defaults for an untouched slot ---------------- */
    printf("--- defaults the module reports (ui.h enc_defaults) -------------\n");
    check_close("Attack starts at 0",           "attack",     0.0,   1e-3);
    check_close("Decay starts at 0",            "decay",      0.0,   1e-3);
    check_close("Gain starts at .704 (unity)",  "gain",       0.704, 1e-3);
    check_close("Pan starts centred",           "pan",        0.5,   1e-3);
    check_close("Varispeed starts at 1x (.83)", "varspeed",   0.83,  2e-3);
    check_text ("Reverse starts off",           "reverse",    "Off");
    check_text ("Loop starts on",               "auto_loop",  "On");
    check_text ("Sustain starts on",            "sustain",    "On");
    check_close("Volume starts at full",        "volume",     1.0,   1e-3);
    check_close("Filter starts open",           "filter",     1.0,   1e-3);
    check_close("Bank starts at 0",             "bank",       0.0,   1e-3);
    check_close("Slot starts at 1",             "slot",       1.0,   1e-3);
    check_text ("Mode starts at jammi",         "voice_mode", "jammi");
    check_text ("FX start before the looper",   "fx_pre_loop","pre");

    set("sample_dir", g_dir);
    usleep(200 * 1000);

    /* --- 2. the values belong to a slot (item 3) ------------------------- */
    printf("\n--- a slot's values are its own ---------------------------------\n");
    bind_slot("jammi", "0", "1", g_long);

    set("attack", "0.50");
    set("decay",  "0.30");
    set("gain",   "0.20");
    sleep_ms(200);
    check_close("Slot 1 keeps the attack that was set", "attack", 0.50, 1e-3);

    set("slot", "2");
    sleep_ms(200);
    check_close("Slot 2 starts from the default attack", "attack", 0.0,   1e-3);
    check_close("Slot 2 starts from the default gain",   "gain",   0.704, 1e-3);
    /* A knob on slot 2 must leave slot 2's OTHER values alone: an end point
     * that defaulted to 0 would make slot 2 play an empty region of its file. */
    set("decay", "0.25");
    sleep_ms(200);
    check_close("Slot 2's end point is still 1 after a decay edit", "end",   1.0, 1e-3);
    check_close("Slot 2's start point is still 0",                  "start", 0.0, 1e-3);

    set("slot", "1");
    sleep_ms(200);
    check_close("Slot 1 still holds its attack", "attack", 0.50, 1e-3);
    check_close("Slot 1 still holds its gain",   "gain",   0.20, 1e-3);
    check_close("Slot 1's end point is still 1", "end",    1.0,  1e-3);

    set("bank", "1");
    sleep_ms(200);
    check_close("Another bank's slot 1 is untouched", "attack", 0.0, 1e-3);

    set("bank", "0");
    set("slot", "1");
    sleep_ms(200);
    check_close("Back in bank 0, slot 1 has its values", "attack", 0.50, 1e-3);
    check_close("...and switching slots changed what the page shows", "gain", 0.20, 1e-3);

    /* --- 3. Attack and Decay, independent, in both modes (item 4) -------- */
    for(int mode = 0; mode < 2; mode++)
    {
        const char* name = mode ? "CUBBI" : "JAMMI";

        printf("\n--- %s: attack (decay = 0) ---------------------------------\n", name);
        Reading a0{}, a75{};
        envelope(name, "0", "0", kReadingNote, &a0);
        print_reading("attack=0.00 decay=0.00", a0);
        envelope(name, "0.75", "0", kReadingNote, &a75);
        print_reading("attack=0.75 decay=0.00", a75);
        /* The early window is a quarter-second into the note. Attack 0 is at
         * full level there; attack 0.75 (an 8.6 s ramp) is not -- and it has to
         * be clearly below the fast case, not just below a number someone
         * picked. Measured: 0.97-1.00 against 0.31-0.39. */
        const double a0_ratio  = a0.peak  > 1e-6 ? a0.early  / a0.peak  : 1.0;
        const double a75_ratio = a75.peak > 1e-6 ? a75.early / a75.peak : 0.0;
        check(mode ? "CUBBI: attack lengthens the fade-in (decay at 0)"
                   : "JAMMI: attack lengthens the fade-in (decay at 0)",
              a0_ratio > 0.60 && a75_ratio < 0.55 && a75_ratio < a0_ratio * 0.6);

        printf("\n--- %s: attack with decay at 0.50 (independence) -----------\n", name);
        Reading b0{}, b75{};
        envelope(name, "0", "0.50", kReadingNote, &b0);
        print_reading("attack=0.00 decay=0.50", b0);
        envelope(name, "0.75", "0.50", kReadingNote, &b75);
        print_reading("attack=0.75 decay=0.50", b75);
        const double b0_ratio  = b0.peak  > 1e-6 ? b0.early  / b0.peak  : 1.0;
        const double b75_ratio = b75.peak > 1e-6 ? b75.early / b75.peak : 0.0;
        check(mode ? "CUBBI: attack still works with decay set"
                   : "JAMMI: attack still works with decay set",
              b0_ratio > 0.60 && b75_ratio < 0.55 && b75_ratio < b0_ratio * 0.6);

        printf("\n--- %s: decay (attack = 0) ---------------------------------\n", name);
        Reading c0{}, c75{};
        envelope(name, "0", "0", kReadingNote, &c0);
        print_reading("decay=0.00 attack=0.00", c0);
        envelope(name, "0", "0.75", kReadingNote, &c75);
        print_reading("decay=0.75 attack=0.00", c75);
        const double c0_tail  = c0.peak  > 1e-6 ? c0.tail  / c0.peak  : 1.0;
        const double c75_tail = c75.peak > 1e-6 ? c75.tail / c75.peak : 0.0;
        check(mode ? "CUBBI: decay lengthens the release (attack at 0)"
                   : "JAMMI: decay lengthens the release (attack at 0)",
              c0_tail < 0.15 && c75_tail > 0.40);
    }

    /* --- 4. CUBBI: each pad uses its own slot's values (item 3) ---------- */
    printf("\n--- CUBBI: each pad uses its own slot's values ------------------\n");
    new_instance();
    bind_slot("cubbi", "0", "1", g_long);
    reset_page();
    set("attack", "0.75"); /* slot 1: a slow fade */
    sleep_ms(200);
    bind_slot("cubbi", "0", "2", g_long);
    reset_page();
    set("attack", "0"); /* slot 2: instant */
    sleep_ms(600);

    Reading pad1{}, pad2{}, pad1b{};
    set("slot", "1"); /* the octave the pads show starts at slot 1 */
    sleep_ms(300);
    press(kPadOfSelectedSlot, &pad1);
    print_reading("pad 1 (slot 1, attack 0.75)", pad1);
    press(kPadOfNextSlot, &pad2);
    print_reading("pad 2 (slot 2, attack 0.00)", pad2);
    press(kPadOfSelectedSlot, &pad1b);
    print_reading("pad 1 again", pad1b);
    check("CUBBI: pad 1 fades in, pad 2 does not",
          pad1.peak > 0.01 && pad1.early / pad1.peak < 0.45
          && pad2.peak > 0.01 && pad2.early / pad2.peak > 0.55);
    check("CUBBI: pad 1 still fades in on a later press (not a one-off)",
          pad1b.peak > 0.01 && pad1b.early / pad1b.peak < 0.45);

    /* --- 5. Loop and Sustain, on a 0.30 s file (item 1) ------------------ */
    printf("\n--- loop and sustain, on a 0.30 s file --------------------------\n");
    const struct
    {
        const char* loop;
        const char* sustain;
    } combos[] = {{"On", "On"}, {"Off", "On"}, {"On", "Off"}, {"Off", "Off"}};
    double held_loop_on = 0, held_loop_off = 0, onset_loop_on = 0, onset_loop_off = 0;
    for(size_t i = 0; i < sizeof combos / sizeof combos[0]; i++)
    {
        static double env[200];
        new_instance();
        bind_slot("jammi", "0", "1", g_short);
        reset_page();
        set("auto_loop", combos[i].loop);
        set("sustain", combos[i].sustain);
        sleep_ms(600);

        /* Key down at 0.1 s, held to 1.5 s. The file is 0.30 s and JAMMI plays
         * C3 at 0.5x, so it runs out around 0.70 s: anything sounding at
         * 1.0-1.4 s got there by looping. */
        capture(1.8, kReadingNote, 0.1, 1.5, env, 180);
        const double settled = level_between(env, 180, 1.00, 1.40);
        const double onset   = level_between(env, 180, 0.12, 0.40);
        printf("   loop=%-3s sustain=%-3s  after the file end %.4f (onset %.4f)\n",
               combos[i].loop, combos[i].sustain, settled, onset);
        if(i == 0) { held_loop_on = settled; onset_loop_on = onset; }
        if(i == 1) { held_loop_off = settled; onset_loop_off = onset; }
    }
    /* Relative to each row's own onset: the tone's absolute level depends on
     * which sample is bound, and that is not what this is testing. */
    check("Loop keeps a short file sounding past its end",
          onset_loop_on > 1e-4 && held_loop_on > onset_loop_on * 0.50);
    check("One-shot stops at the end of the file",
          onset_loop_off > 1e-4 && held_loop_off < onset_loop_off * 0.15
          && held_loop_off < held_loop_on * 0.50);

    /* --- 6. a released note leaves the voice idle (items 5 and 6) -------- */
    printf("\n--- a released note goes quiet ----------------------------------\n");
    new_instance();
    bind_slot("jammi", "0", "1", g_long);
    set("attack", "0");
    set("decay", "0.50");
    sleep_ms(600);
    {
        static double env[400];
        capture(4.0, kReadingNote, 0.3, 1.8, env, 400);
        const int waited = settle();
        char      label[160];
        snprintf(label, sizeof label, "No voice is left playing 2.4 s after the key (%d blocks)",
                 waited);
        check(label, waited >= 0);
    }

    /* --- 7. the values survive a reload (slots.values) ------------------- */
    printf("\n--- the store persists ------------------------------------------\n");
    set("voice_mode", "jammi");
    set("bank", "3");
    set("slot", "7");
    sleep_ms(250);
    set("attack", "0.40");
    set("pan",    "0.20");
    sleep_ms(1200); /* the worker writes once the knob has settled */

    void* inst2 = api->create_instance(g_dir, NULL);
    check("A second instance exists", inst2 != nullptr);
    if(inst2 != nullptr)
    {
        api->set_param(inst2, "bank", "3");
        api->set_param(inst2, "slot", "7");
        sleep_ms(250);
        char label[200];
        api->get_param(inst2, "attack", rd, sizeof rd);
        snprintf(label, sizeof label, "Saved attack comes back after a reload (%.3f)",
                 strtod(rd, nullptr));
        check(label, fabs(strtod(rd, nullptr) - 0.40) <= 2e-3);
        api->get_param(inst2, "pan", rd, sizeof rd);
        snprintf(label, sizeof label, "Saved pan comes back after a reload (%.3f)",
                 strtod(rd, nullptr));
        check(label, fabs(strtod(rd, nullptr) - 0.20) <= 2e-3);
        api->set_param(inst2, "slot", "8");
        sleep_ms(250);
        api->get_param(inst2, "attack", rd, sizeof rd);
        snprintf(label, sizeof label, "Untouched slot 8 is still at the default (%.4s)", rd);
        check(label, fabs(strtod(rd, nullptr) - 0.0) <= 2e-3);
        api->destroy_instance(inst2);
    }

    printf("\n%s (%d failure%s in %d checks)\n",
           failures ? "VERDICT: FAIL" : "VERDICT: PASS", failures,
           failures == 1 ? "" : "s", checks);
    return failures ? 1 : 0;
}
