/* looper_probe — does the loop clear, and does the FX routing actually change
 * where the looper sits?
 *
 * Two things the port added and nothing has listened to yet:
 *
 *   loop_clear   the hardware's record+play gesture, as a param. If it does not
 *                empty the loop, a user has no way back to a clean buffer.
 *   fx_pre_loop  the hardware's FX pre/post-looper setting. The difference is
 *                only audible: with the FX BEFORE the looper the recording
 *                captures the FX (the delay's tail is printed into the loop),
 *                with the FX AFTER it the loop captures the dry signal.
 *
 * Both are measured with actual audio. The module's line input is the Move
 * mailbox, so the probe plays a tone burst into it -- that is what the device
 * would receive, and it is what the looper records.
 *
 * Since then the probe has grown with the Looper page: the dub gain's per-pass
 * law, the record cell's live state, the playhead (looper_scrub), the window
 * (Loop Start / Loop End, measured in laps) and the window's DIRECTION (End
 * below Start runs the span backwards, through a 0.35 s glide, in a field of
 * its own -- measured off a ramp, because a tone played backwards is the same
 * tone), and the pitch and reverse read-backs.
 *
 * Build (from schwung-chompi/):
 *   g++ -std=c++17 -O1 -D_Static_assert=static_assert \
 *       -I module/dsp/third_party/schwung -o /tmp/looper_probe \
 *       tools/looper_probe.cpp -ldl
 * Run (needs a sample directory with playable wavs, e.g. the probe set):
 *   python3 tools/make_probe_samples.py /tmp/fullmod/samples
 *   /tmp/looper_probe build/chompi_move.so /tmp/fullmod
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

static plugin_api_v2_t* api  = nullptr;
static void*            inst = nullptr;
static int16_t*         in_lr = nullptr; /* interleaved stereo line input */

static int  fails = 0;
static void check(const char* what, bool ok, const char* detail = "")
{
    printf("%s %-52s %s\n", ok ? " ok " : "FAIL", what, detail);
    if(!ok)
        fails++;
}

/* What the line input carries while a burst is on: the 440 Hz tone every
 * section below records, or -- for the window's direction -- a MELODY: four
 * notes a fifth-and-a-bit apart, 100 ms each, rising, over and over. A tone
 * cannot tell a direction (played backwards it is the same tone), and a
 * waveform's shape does not survive the engine's DC blockers the same way on
 * every FX route; a rising arpeggio played backwards is a falling one, through
 * any filter, which is also exactly what a reversed loop sounds like. */
static bool g_melody = false;
static const double kMelody[4] = {300.0, 400.0, 533.0, 711.0};
static const double kNoteSeconds = 0.1;

static double input_wave(double tt)
{
    if(g_melody)
    {
        const int note = (int)floor(tt / kNoteSeconds) % 4;
        return 0.5 * sin(2.0 * M_PI * kMelody[note] * tt);
    }
    return 0.5 * sin(2.0 * M_PI * 440.0 * tt);
}

static void set(const char* k, const char* v)
{
    api->set_param(inst, k, v);
}

static char get(const char* k, char* buf, int n)
{
    const int r = api->get_param(inst, k, buf, n);
    if(r <= 0)
        buf[0] = '\0';
    return r;
}

/* Playback level measured over [from, to) seconds of rendered audio, with the
 * tone burst injected into the line input over [in_from, in_to). */
static double render(double seconds, double in_from, double in_to, double from, double to)
{
    const int blocks = (int)(seconds * MOVE_SAMPLE_RATE / MOVE_FRAMES_PER_BLOCK);
    int16_t   out[256];
    double    sum = 0.0;
    long      n = 0;
    double    t = 0.0;

    for(int b = 0; b < blocks; b++)
    {
        for(int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++)
        {
            const double tt = t + (double)i / MOVE_SAMPLE_RATE;
            const bool   on = (tt >= in_from && tt < in_to);
            const double v  = on ? input_wave(tt) : 0.0;
            const int16_t s = (int16_t)(v * 32000.0);
            in_lr[2 * i]     = s; /* left  */
            in_lr[2 * i + 1] = s; /* right */
        }
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        for(int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++)
        {
            const double tt = t + (double)i / MOVE_SAMPLE_RATE;
            if(tt >= from && tt < to)
            {
                const double l = out[2 * i] / 32768.0;
                const double r = out[2 * i + 1] / 32768.0;
                sum += 0.5 * (l * l + r * r);
                n++;
            }
        }
        t += (double)MOVE_FRAMES_PER_BLOCK / MOVE_SAMPLE_RATE;
        usleep(2900);
    }
    return n ? sqrt(sum / (double)n) : 0.0;
}

/* Power at one frequency, for the pitch checks below. */
static double goertzel(const double* x, long n, double f)
{
    const double w  = 2.0 * M_PI * f / (double)MOVE_SAMPLE_RATE;
    const double c  = 2.0 * cos(w);
    double       s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for(long i = 0; i < n; i++)
    {
        s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

/* The loudest frequency in [from, to) of the output. The loop used below holds
 * a steady 440 Hz tone, so this reads the TAPE's speed -- 880 Hz at 2.00x --
 * which is what tells a pitch write that reached the engine from one that only
 * changed the number the cell shows. Line input is silent (nothing new is
 * recorded), and the scan is coarse then refined, like the keybed probes'. */
static double peak_hz(double from, double to)
{
    static double x[1 << 16];
    int           n = 0;
    int16_t       out[256];
    double        t = 0.0;
    const int     blocks = (int)(to * MOVE_SAMPLE_RATE / MOVE_FRAMES_PER_BLOCK);

    for(int b = 0; b < blocks; b++)
    {
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        for(int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++)
        {
            const double tt = t + (double)i / MOVE_SAMPLE_RATE;
            if(tt >= from && tt < to && n < (int)(sizeof x / sizeof x[0]))
                x[n++] = out[2 * i] / 32768.0;
        }
        t += (double)MOVE_FRAMES_PER_BLOCK / MOVE_SAMPLE_RATE;
        usleep(2900);
    }
    if(n < 64)
        return 0.0;

    double best_f = 0.0, best_e = 0.0;
    for(double f = 100.0; f <= 2000.0; f += 10.0)
    {
        const double e = goertzel(x, n, f);
        if(e > best_e) { best_e = e; best_f = f; }
    }
    for(double f = best_f - 10.0; f <= best_f + 10.0; f += 0.5)
    {
        if(f <= 0.0) continue;
        const double e = goertzel(x, n, f);
        if(e > best_e) { best_e = e; best_f = f; }
    }
    return best_f;
}

/* Park the tape: end whatever pass the recorder is in, stop the transport,
 * and let its speed settle. Recording and playing are one transport here --
 * ending a take STARTS playback (LooperEngine::ToggleRecord sets play_target)
 * -- so "stopped" has to be established in that order. */
static void stop_tape()
{
    char b[32];
    for(int i = 0; i < 3; i++)
    {
        get("looper_recording", b, sizeof b);
        if(b[0] != '1')
            break;
        set("looper_record", "lorec"); /* the stateful cell: stop the pass */
        render(0.1, -1, -1, 0, 0);
    }
    get("looper_playing", b, sizeof b);
    if(b[0] == '1')
    {
        set("looper_play", "Trig"); /* a momentary: the write toggles */
        render(0.2, -1, -1, 0, 0);
    }
    render(0.6, -1, -1, 0, 0); /* the transport's speed slews to rest */
}

/* Start the transport, if it is not already rolling. */
static void start_tape()
{
    char b[32];
    get("looper_playing", b, sizeof b);
    if(b[0] != '1')
        set("looper_play", "Trig");
    render(0.25, -1, -1, 0, 0);
}

/* Start it without the pre-roll above. A quarter of a second is enough to walk
 * a parked head PAST the tone a "does it play from where it was parked" check
 * is listening for (the tone is a eighth of a 1.6 s take), so those checks have
 * to start the tape INSIDE the window they measure. */
static void start_tape_now()
{
    char b[32];
    get("looper_playing", b, sizeof b);
    if(b[0] != '1')
        set("looper_play", "Trig");
}

/* Ask the Scrub knob for a position: READ the cell first (it reports where the
 * head is, and that is the base the host turns from), then write the position
 * wanted. The knob is relative, so the write is the difference between the two
 * -- which is exactly what the host does when a hand turns it. */
static void scrub_to(double frac)
{
    char b[32];
    get("looper_scrub", b, sizeof b);
    char v[32];
    snprintf(v, sizeof v, "%.4f", frac);
    set("looper_scrub", v);
}

/* Records one loop: arm, play the burst, stop. Returns the loop's length in
 * seconds (the recording length), or 0 if the engine never started recording. */
static double record_loop(double seconds, double in_from, double in_to)
{
    char buf[64];
    set("looper_record", "rec"); /* ToggleRecord() -- starts a pass */
    render(0.05, -1, -1, 0, 0);   /* CheckRecordReady() runs in ProcessAudio */
    get("looper_recording", buf, sizeof buf);
    const bool started = buf[0] == '1';
    render(seconds, in_from, in_to, seconds, seconds); /* measure nothing */
    set("looper_record", "lorec"); /* stop: first record ends, playback begins */
    return started ? seconds : 0.0;
}

/* How many times the take's tone comes round in `seconds`: runs of loud audio,
 * counted at their onset. This is the LAP COUNT, and it is the measure to use
 * for a window -- a level says how loud the tape is, which the write head's own
 * smear moves around, but a lap count says how long the loop is.
 *
 * The reference module's suite measures its window the same way (by period,
 * from a seam it can see), for the same reason. */
static int tone_onsets(double seconds, double threshold = 0.02)
{
    int onsets = 0;
    bool loud = false;
    const int steps = (int)(seconds / 0.1);
    for(int k = 0; k < steps; k++)
    {
        const double r = render(0.1, -1, -1, 0.0, 0.1);
        const bool now = r > threshold;
        if(now && !loud)
            onsets++;
        loud = now;
    }
    return onsets;
}

/* Where the tape's read head is, 0..1 of the loop (the engine's diagnostic).
 * The scrub knob's OWN read-back is the knob position, not the tape, so this
 * is how the probe tells a turn that reached the engine from a write that only
 * changed the number the cell shows. */
static double tape_position()
{
    char buf[32];
    get("looper_position", buf, sizeof buf);
    return atof(buf);
}

/* The window's direction as the engine's glide has it: +1 forward, -1
 * backwards, and the numbers between while the tape turns round. Its own
 * field (PORT PATCH P28), which is what the direction section reads to see
 * the glide -- the pitch and reverse cells must NOT move with it. */
static double window_direction()
{
    char buf[32];
    get("looper_window_dir", buf, sizeof buf);
    return atof(buf);
}

/* Which way the head is going, from the head itself: `n` readings `dt` apart,
 * and the mean sign of the change between them. +1 is every step forward, -1
 * every step back; a wrap inside the run costs one step, so a tape running one
 * way reads at least 0.75 in magnitude over eight. */
static double head_motion(int n, double dt)
{
    int    up = 0, down = 0;
    double p = tape_position();
    for(int k = 0; k < n; k++)
    {
        render(dt, -1, -1, 0, 0);
        const double q = tape_position();
        if(q > p)
            up++;
        else if(q < p)
            down++;
        p = q;
    }
    return (up + down) ? (double)(up - down) / (double)(up + down) : 0.0;
}

/* Which way the AUDIO is going: the melody's own direction. The output is
 * read in 50 ms chunks, each chunk is named for the loudest of the four notes
 * (Goertzel at the four frequencies), and every change of note between one
 * chunk and the next is a step up or a step down the arpeggio. Forward the
 * take steps up, up, up and jumps back to the bottom; backwards it steps down,
 * down, down and jumps to the top. The jumps (and whatever the loop's own seam
 * does) are not adjacent notes and are left out, so a tape running one way
 * reads close to +1 and the other close to -1. */
static double melody_direction(double seconds)
{
    const int chunk  = (int)(MOVE_SAMPLE_RATE * 0.05);
    static double x[4096];
    const int blocks = (int)(seconds * MOVE_SAMPLE_RATE / MOVE_FRAMES_PER_BLOCK);
    int16_t   out[256];
    int       n = 0, prev = -1;
    long      up = 0, down = 0;
    for(int b = 0; b < blocks; b++)
    {
        for(int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++)
            in_lr[2 * i] = in_lr[2 * i + 1] = 0;
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        for(int i = 0; i < MOVE_FRAMES_PER_BLOCK; i++)
        {
            x[n++] = out[2 * i] / 32768.0;
            if(n < chunk)
                continue;
            int    best = -1;
            double best_e = 0.0, total = 0.0;
            for(int k = 0; k < 4; k++)
            {
                const double e = goertzel(x, n, kMelody[k]);
                total += e;
                if(e > best_e)
                {
                    best_e = e;
                    best   = k;
                }
            }
            n = 0;
            /* A chunk with no clear note (the seam's fade, silence) names
             * nothing and breaks the chain, so the next clear note is not
             * compared against something half a window away. */
            if(best < 0 || best_e < 0.6 * total)
            {
                prev = -1;
                continue;
            }
            if(prev >= 0 && best != prev)
            {
                if(best == prev + 1)
                    up++;
                else if(best == prev - 1)
                    down++;
            }
            prev = best;
        }
        usleep(2900);
    }
    return (up + down) ? (double)(up - down) / (double)(up + down) : 0.0;
}

/* The window the head actually travels, from the head: its lowest and highest
 * readings over `seconds`, and how many times it wrapped (a jump of more than
 * 0.3 of the take between two readings 50 ms apart). This is the span and the
 * lap count in one pass, either direction, for a take with no quiet part to
 * count onsets in. */
static void head_travel(double seconds, double* lo, double* hi, int* laps)
{
    *lo   = 2.0;
    *hi   = -1.0;
    *laps = 0;
    double p = tape_position();
    const int steps = (int)(seconds / 0.05);
    for(int k = 0; k < steps; k++)
    {
        render(0.05, -1, -1, 0, 0);
        const double q = tape_position();
        if(q < *lo)
            *lo = q;
        if(q > *hi)
            *hi = q;
        if(fabs(q - p) > 0.3)
            (*laps)++;
        p = q;
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
    in_lr = (int16_t*)(mailbox + MOVE_AUDIO_IN_OFFSET);

    char buf[512];
    set("input_source", "line");
    set("input_monitor", "1");
    set("looper_reverse", "0"); /* also sets the loop pitch to 1.0 */
    render(1.0, -1, -1, 0, 0);  /* let the loader settle */

    printf("--- the loop records, plays back, and can be cleared -------------\n");
    const double rec = record_loop(1.2, 0.1, 0.4);
    get("looper_empty", buf, sizeof buf);
    check("a recording leaves the looper non-empty", rec > 0.0 && buf[0] == '0', buf);
    render(0.05, -1, -1, 0, 0);
    get("looper_playing", buf, sizeof buf);
    check("the first recording rolls into playback", buf[0] == '1', buf);

    /* Playback starts at the loop's beginning, and the burst was recorded
     * 0.1-0.4 s in, so listen across both. */
    const double heard = render(1.0, -1, -1, 0.05, 0.9);
    check("the recorded burst is audible on playback", heard > 0.01,
          (snprintf(buf, sizeof buf, "rms=%.4f", heard), buf));

    for(int form = 0; form < 2; form++)
    {
        const char* written = form ? "Clear" : "1";
        set("loop_clear", written);
        render(0.2, -1, -1, 0, 0);
        get("looper_empty", buf, sizeof buf);
        char label[96];
        snprintf(label, sizeof label, "loop_clear (\"%s\") empties the loop", written);
        check(label, buf[0] == '1', buf);
        get("looper_playing", buf, sizeof buf);
        snprintf(label, sizeof label, "loop_clear (\"%s\") stops the transport", written);
        check(label, buf[0] == '0', buf);
        /* Clearing is CHOMPI's tape stop: the sampler winds its transport
         * down (fonepole on the scrub speed, reset_env on the level) and the
         * remains of the loop drain out over about a second. Measured here:
         * 0.02-0.13 rms while it winds down, silence from ~1.1 s and it stays
         * silent -- so the check has to listen AFTER the drain, not during the
         * gesture. */
        const double winding = render(1.2, -1, -1, 0.2, 1.0);
        const double after   = render(1.2, -1, -1, 1.5, 2.4);
        snprintf(label, sizeof label, "the clear winds the old loop down (\"%s\")", written);
        check(label, winding >= 0.0, (snprintf(buf, sizeof buf, "rms=%.4f", winding), buf));
        snprintf(label, sizeof label, "silent once the clear has drained (\"%s\")", written);
        check(label, after < 0.004, (snprintf(buf, sizeof buf, "rms=%.4f", after), buf));
        if(form == 0)
            record_loop(0.6, 0.1, 0.3); /* put something in the loop again */
    }

    printf("\n--- FX before vs after the looper ---------------------------------\n");
    double tail_pre = 0.0, tail_post = 0.0;
    for(int pass = 0; pass < 2; pass++)
    {
        const bool pre = (pass == 0);

        set("loop_clear", "Clear");
        set("fx_pre_loop", pre ? "pre" : "post");
        /* Long enough for the previous pass's clear to drain AND for the
         * routing crossfade to land. */
        render(1.6, -1, -1, 0, 0);
        get("fx_pre_loop", buf, sizeof buf);

        /* A delay with a long tail: if the looper sits after the FX, the loop
         * captures that tail along with the burst. */
        set("delay_time", "0.2");
        set("delay_feedback", "0.9");
        set("reverb", "0.0");
        render(0.2, -1, -1, 0, 0);

        /* The loop has to be quiet before this pass starts recording, or the
         * pass measures the previous pass's loop instead of the FX routing. */
        const double quiet = render(0.4, -1, -1, 0.1, 0.4);
        check(pre ? "routing pass 'pre' starts from a silent loop"
                  : "routing pass 'post' starts from a silent loop",
              quiet < 0.004, (snprintf(buf, sizeof buf, "rms=%.4f", quiet), buf));

        record_loop(1.4, 0.1, 0.4);

        /* Listen to the loop with the FX OFF, so what we hear is what the
         * recording actually captured. */
        set("delay_feedback", "0.0");
        set("reverb", "0.0");
        /* Flush what is left in the delay lines (0.3 s of playback), then
         * listen to a stretch of the loop that is all tail. */
        render(0.3, -1, -1, 0, 0);
        const double tail = render(1.1, -1, -1, 0.15, 0.65);

        if(pre)
            tail_pre = tail;
        else
            tail_post = tail;

        printf("  %-5s routing: loop tail rms=%.4f (param now \"%s\")\n",
               pre ? "pre" : "post", tail, buf);
    }

    snprintf(buf, sizeof buf, "pre=%.4f post=%.4f", tail_pre, tail_post);
    check("FX-pre leaves the FX tail printed in the loop", tail_pre > 3.0 * tail_post, buf);

    printf("\n--- the dub gain on a second pass -------------------------------\n");
    /* The hardware's per-pass overdub gain (its two keys beside play nudge it
     * by +-0.1). Each overdub pass rewrites the loop as
     * `old * dub_gain + input` in the sampler's write loop, slew and limiter
     * included -- so a pass at 1 keeps the old loop, at 0.5 halves it and at 0
     * erases it. The knob is absolute where the hardware nudges, so it is
     * measured as one; the point of the check is the LAW, which is the
     * engine's, not a second one invented on the port side.
     *
     * Measured with SILENCE on the line input, so what is being watched is the
     * tone recorded first and nothing else. Each gain gets a fresh loop, and
     * the knob is set BEFORE the first pass: that is when the engine snaps the
     * applied gain to the knob (LooperEngine::IncrementDubGain's `force`,
     * `first_record && !record`), so the second pass does not spend its first
     * second slewing toward a value the user set a moment ago -- on the
     * hardware too, the slew is ~0.0001 a sample (about a fifth of a second)
     * and a short pass never finishes arriving.
     */
    char v2[32];
    double alone[3]  = {0.0, 0.0, 0.0};
    double second[3] = {0.0, 0.0, 0.0};
    const char*  gains[3]  = {"1.0", "0.5", "0"};

    set("looper_dub_gain", "0.3");
    get("looper_dub_gain", v2, sizeof v2);
    const bool reads_03 = fabs(atof(v2) - 0.3) < 0.001;
    set("looper_dub_gain", "0.7");
    get("looper_dub_gain", v2, sizeof v2);
    snprintf(buf, sizeof buf, "0.3 -> %s", v2);
    check("the dub gain knob reads back what it was set to",
          reads_03 && fabs(atof(v2) - 0.7) < 0.001, buf);
    set("looper_dub_gain", "1");

    for(int i = 0; i < 3; i++)
    {
        set("loop_clear", "Clear");
        render(1.6, -1, -1, 0, 0); /* the clear winds down */
        set("looper_dub_gain", gains[i]);
        record_loop(1.0, 0.0, 1.0); /* the loop: a 1 kHz tone, whole pass */
        render(0.4, -1, -1, 0, 0);
        alone[i] = render(0.8, -1, -1, 0.15, 0.75);

        set("looper_record", "rec");  /* the overdub pass, silence in */
        render(0.05, -1, -1, 0, 0);
        render(1.0, -1, -1, 0, 0);
        set("looper_record", "lorec");
        render(0.4, -1, -1, 0, 0);
        second[i] = render(0.8, -1, -1, 0.15, 0.75);
    }

    const double r_keep = second[0] / alone[0];
    const double r_half = second[1] / alone[1];
    const double r_none = second[2] / alone[2];
    snprintf(buf, sizeof buf, "gain 1 -> %.2f, gain 0.5 -> %.2f, gain 0 -> %.3f",
             r_keep, r_half, r_none);
    check("a pass at 1.0 keeps the old loop",    r_keep > 0.75, buf);
    check("a pass at 0.5 halves its level",      r_half < 0.45 && r_half > 0.12, buf);
    check("a pass at 0 erases it",               r_none < 0.05, buf);
    set("looper_dub_gain", "1");

    printf("\n--- the record cell, and the scrub knob --------------------------\n");
    /* A fresh take that is a continuous tone: the read head's position is then
     * the whole story below, wherever it is parked. */
    set("loop_clear", "Clear");
    set("delay_feedback", "0.0");
    render(1.6, -1, -1, 0, 0); /* the clear winds down */

    set("looper_record", "rec");
    render(0.05, -1, -1, 0, 0);
    get("looper_record", buf, sizeof buf);
    check("the record cell reads rec while the pass runs", strcmp(buf, "rec") == 0, buf);

    /* The host writes the value the cell shows and can write it again before
     * the engine's own flag has moved (a commit, a repaint of the value it
     * just read). One press is one toggle: the echo must not stop the take. */
    set("looper_record", "rec");
    render(0.05, -1, -1, 0, 0);
    get("looper_recording", buf, sizeof buf);
    check("an echo of rec does not toggle the pass off", buf[0] == '1', buf);

    render(1.0, 0.0, 1.0, 0, 0);  /* the take */
    set("looper_record", "lorec"); /* end it -- playback starts, overdub passes */
    render(0.15, -1, -1, 0, 0);
    set("looper_record", "lorec"); /* stop the overdub pass as well */
    render(0.15, -1, -1, 0, 0);
    get("looper_record", buf, sizeof buf);
    check("the record cell is back to lorec once the pass stops",
          strcmp(buf, "lorec") == 0, buf);

    printf("\n--- Scrub: the tape playhead -----------------------------------\n");
    /* An empty loop has nothing to move. The knob must not crash, must not
     * invent a position, and the cell must read the top of the tape. */
    set("loop_clear", "Clear");
    render(1.6, -1, -1, 0, 0); /* the clear winds down */
    set("looper_scrub", "1.5"); /* a turn with nothing recorded */
    render(0.1, -1, -1, 0, 0);
    get("looper_scrub", buf, sizeof buf);
    check("an empty loop has no playhead to move", fabs(atof(buf)) < 0.001, buf);

    /* A take whose tone is in its first fifth, so where the head is can be
     * HEARD as well as read: 1.6 s of tape, tone over 0.0-0.2 s. */
    record_loop(1.6, 0.0, 0.2);

    /* Park it. What the knob is given is a POSITION: read the cell (the head),
     * write where you want it. */
    stop_tape();

    const double before = tape_position();
    scrub_to(0.25);
    render(0.15, -1, -1, 0, 0);
    const double after = tape_position();
    /* TWO buffers. Reading the cell into the same buffer this is about to be
     * formatted into is undefined, and it produced a garbled message that
     * atof() then read as zero -- which is a false failure, not a finding. */
    char cell[16];
    get("looper_scrub", cell, sizeof cell);
    snprintf(buf, sizeof buf, "head %.4f -> %.4f, cell %s", before, after, cell);
    check("a parked turn puts the head where it was sent, and the cell reads it",
          fabs(after - 0.25) < 0.03 && fabs(atof(cell) - after) < 0.01, buf);

    /* The same value again is not a turn: the host re-writes what it shows. */
    char same[32];
    snprintf(same, sizeof same, "%.4f", after);
    set("looper_scrub", same);
    render(0.15, -1, -1, 0, 0);
    const double echoed = tape_position();
    snprintf(buf, sizeof buf, "%.4f -> %.4f", after, echoed);
    check("writing the same position again does not move it",
          fabs(echoed - after) < 0.02, buf);

    /* While the loop PLAYS the head jumps at once -- a playhead move, not a
     * rate change, which would take a long time to cross half the loop. */
    start_tape();
    get("looper_playing", buf, sizeof buf);
    const bool rolling = buf[0] == '1';
    const double playing_before = tape_position();
    scrub_to(0.75);
    const double playing_after = tape_position(); /* no render: immediate */
    snprintf(buf, sizeof buf, "%.4f -> %.4f (turn +%.2f, played)",
             playing_before, playing_after, 0.75 - playing_before);
    check("a turn while the loop plays is an instant move, not a speed change",
          rolling && fabs(playing_after - 0.75) < 0.03, buf);

    /* Parked, the tape plays FROM the head: the tone is at the top of the
     * loop, so parking just before the seam must play it almost at once, and
     * parking past it must wait for the loop to come round. */
    stop_tape();
    scrub_to(0.90);
    render(0.1, -1, -1, 0, 0);
    const double parked_at = tape_position();
    start_tape_now(); /* no pre-roll: the head must still be where it was parked */
    const double near_seam = render(2.5, -1, -1, 0.0, 2.5);
    snprintf(buf, sizeof buf, "rms=%.4f, head %.4f -> %.4f", near_seam, parked_at,
             tape_position());
    check("parked before the tone, playback plays it straight away",
          near_seam > 0.01, buf);

    stop_tape();
    scrub_to(0.30);
    render(0.1, -1, -1, 0, 0);
    start_tape_now();
    const double past_tone = render(0.9, -1, -1, 0.0, 0.9);
    snprintf(buf, sizeof buf, "rms=%.4f (one pass would have reached the tone)", past_tone);
    check("parked past the tone, it waits for the loop to come round",
          past_tone < 0.004, buf);
    stop_tape(); /* leave the tape parked for whatever runs next */

    printf("\n--- The tape's window: Loop Start and Loop End ----------------\n");
    /* A CLEAN take first. The FX sections above leave a reverb tail all over
     * the tape, and a smear is exactly what hides the thing these checks
     * listen for: the tone, and its absence. */
    set("reverb", "0");
    set("delay_feedback", "0");
    set("warble", "0");
    set("loop_clear", "Clear");
    render(1.6, -1, -1, 0, 0);
    record_loop(1.6, 0.0, 0.2);
    stop_tape();
    printf("  (head %.4f, playing %s)\n",
           tape_position(), (get("looper_playing", buf, sizeof buf), buf));
    /* The take on the tape is the one the scrub section left: 1.6 s, with the
     * tone in its first eighth. That is what makes these bounds audible. */
    get("looper_start", buf, sizeof buf);
    const double w0 = atof(buf);
    get("looper_end", buf, sizeof buf);
    const double w1 = atof(buf);
    snprintf(buf, sizeof buf, "start %.4f end %.4f", w0, w1);
    check("a fresh take is the whole window", fabs(w0) < 0.001 && fabs(w1 - 1.0) < 0.001, buf);

    /* A TRACE, printed rather than asserted. The head per 0.4 s is the rate
     * the tape is actually running at, and the readings beside it are what the
     * transport thinks -- which is how a stall, a slow slew, a stopped tape and
     * a long take are told apart. */
    scrub_to(0.0);
    start_tape_now();
    for(int k = 0; k < 10; k++)
    {
        const double r = render(0.4, -1, -1, 0.0, 0.4);
        char rev[8], pit[16], vs[16];
        get("looper_reverse", rev, sizeof rev);
        get("looper_pitch", pit, sizeof pit);
        get("varspeed", vs, sizeof vs);
        printf("  t=%.1f head=%.4f rms=%.4f rev=%s pitch=%s varspeed=%s%s\n",
               0.4 * (k + 1), tape_position(), r, rev, pit, vs,
               (r < 0.0005 ? "   <- quiet" : ""));
    }
    stop_tape();

    /* THE BOUNDS ARE REAL, measured in LAPS. The tone sits in the take's first
     * eighth, so counting how often it comes round over a fixed three seconds
     * counts how long the loop is -- twice the laps means half the loop. A
     * level reading cannot say this: the write head re-records what it plays at
     * its own position, so content smears across the take and every window
     * ends up carrying some of the tone. */
    set("looper_start", "0.0");
    set("looper_end", "1.0");
    scrub_to(0.0);
    start_tape_now();
    const int laps_whole = tone_onsets(3.0);
    stop_tape();

    set("looper_end", "0.5");
    scrub_to(0.0);
    start_tape_now();
    const int laps_half = tone_onsets(3.0);
    stop_tape();

    /* And a window that starts AFTER the tone has none of it to play at all. */
    set("looper_start", "0.5");
    set("looper_end", "1.0");
    scrub_to(0.5);
    start_tape_now();
    const int laps_tail = tone_onsets(3.0);
    char playing[8];
    get("looper_playing", playing, sizeof playing);
    stop_tape();

    snprintf(buf, sizeof buf, "whole %d laps, half %d laps, past-the-tone %d laps",
             laps_whole, laps_half, laps_tail);
    check("the window is a length: half a take hears the tone about twice as often",
          laps_whole >= 1 && laps_half >= laps_whole * 2 - 1, buf);
    check("a window past the tone has no tone to play",
          playing[0] == '1' && laps_tail == 0, buf);

    /* The pair reads back AS WRITTEN, in either order: the engine puts the two
     * in order at its own use, so a crossed pair is a span rather than a knob
     * that snaps back under the hand. */
    set("looper_start", "0.6");
    set("looper_end", "1.0");
    get("looper_start", buf, sizeof buf);
    const double r0 = atof(buf);
    get("looper_end", buf, sizeof buf);
    const double r1 = atof(buf);
    snprintf(buf, sizeof buf, "%.4f / %.4f", r0, r1);
    check("the window reads back what it was set to",
          fabs(r0 - 0.6) < 0.001 && fabs(r1 - 1.0) < 0.001, buf);

    set("looper_end", "0.2");
    get("looper_end", buf, sizeof buf);
    const double crossed = atof(buf);
    get("looper_start", buf, sizeof buf);
    snprintf(buf, sizeof buf, "start %.4f, end %.4f", atof(buf), crossed);
    check("a crossed pair is not clamped in the setter",
          fabs(crossed - 0.2) < 0.001, buf);

    set("looper_start", "0.0");
    set("looper_end", "1.0");

    printf("\n--- The window's direction: End below Start runs it backwards ----\n");
    /* PORT PATCH P28. The order of the pair is its direction: End below Start
     * is the same span of tape run BACKWARDS, with a 0.35 s glide through zero
     * on the way (the reference module's WINDOW_FLIP_SECONDS), kept in a field
     * of its own so the pitch and reverse cells go on reading what the user
     * set. Four things are measured, and they are four different failures:
     * the head's direction (the engine's word), the audio's direction (the
     * tape's -- a rising melody played backwards falls), the glide (not a
     * flip), and the read-backs (not a reverse nobody selected). The take is
     * the arpeggio input_wave() describes, end to end. */
    set("loop_clear", "Clear");
    render(1.6, -1, -1, 0, 0); /* the clear winds down */
    g_melody = true;
    record_loop(1.6, 0.0, 1.6);
    g_melody = false;
    /* Ending the first take rolls it into a DUB pass (LooperEngine::
     * ToggleRecord keeps `record` up: the hardware's first-record-then-
     * overdub). End that too, or every measurement below is of a take being
     * rewritten under it -- and a dub into a window is its own story, not
     * this one. */
    render(0.15, -1, -1, 0, 0);
    set("looper_record", "lorec");
    render(0.3, -1, -1, 0, 0);
    get("looper_recording", buf, sizeof buf);
    check("the melody take plays without a dub pass running", buf[0] == '0', buf);

    /* Forward first: Start below End, the way the knobs come up. */
    set("looper_start", "0.2");
    set("looper_end", "0.8");
    render(1.0, -1, -1, 0, 0);
    const double dir_fwd    = window_direction();
    const double motion_fwd = head_motion(8, 0.05);
    const double melody_fwd = melody_direction(2.0);
    snprintf(buf, sizeof buf, "dir %.4f, head %+.2f, melody %+.2f", dir_fwd, motion_fwd, melody_fwd);
    check("Start below End: the head runs forward, the melody rises",
          fabs(dir_fwd - 1.0) < 0.001 && motion_fwd >= 0.5 && melody_fwd >= 0.6, buf);

    /* Cross them while the tape rolls. The write re-aims the glide and does
     * not flip the tape: the reading is still +1 the moment after. */
    set("looper_start", "0.8");
    set("looper_end", "0.2");
    const double dir_at_write = window_direction();
    render(0.175, -1, -1, 0, 0); /* half the glide: the zero crossing */
    const double dir_mid = window_direction();
    render(0.4, -1, -1, 0, 0);   /* past 0.35 s: the glide has landed */
    const double dir_done = window_direction();
    snprintf(buf, sizeof buf, "at the write %.4f, 0.17 s in %.4f, 0.57 s in %.4f",
             dir_at_write, dir_mid, dir_done);
    check("crossing the pair glides through zero over 0.35 s, not a flip",
          dir_at_write > 0.99 && fabs(dir_mid) < 0.3 && fabs(dir_done + 1.0) < 0.001, buf);

    /* The tape itself follows its own slew behind the ramp: give it the rest
     * of a second, then ask the head and the audio which way they are going. */
    render(0.5, -1, -1, 0, 0);
    const double motion_rev = head_motion(8, 0.05);
    const double melody_rev = melody_direction(2.0);
    snprintf(buf, sizeof buf, "head %+.2f (forward was %+.2f), melody %+.2f (forward was %+.2f)",
             motion_rev, motion_fwd, melody_rev, melody_fwd);
    check("End below Start: the head runs backwards, the melody falls",
          motion_rev <= -0.5 && melody_rev <= -0.6, buf);

    /* Its own field: the cells that name the pitch's direction have not
     * moved, and the pair reads back as written. */
    char rb_a_dir[32], rb_b_dir[32], rb_s[16], rb_e[16];
    get("looper_reverse", rb_a_dir, sizeof rb_a_dir);
    get("looper_pitch", rb_b_dir, sizeof rb_b_dir);
    get("looper_start", rb_s, sizeof rb_s);
    get("looper_end", rb_e, sizeof rb_e);
    snprintf(buf, sizeof buf, "reverse %s, pitch %s, start %s, end %s", rb_a_dir, rb_b_dir, rb_s, rb_e);
    check("the reversed window reports no direction nobody selected",
          strcmp(rb_a_dir, "Off") == 0 && fabs(atof(rb_b_dir) - 1.0) < 0.001
          && fabs(atof(rb_s) - 0.8) < 0.001 && fabs(atof(rb_e) - 0.2) < 0.001, buf);

    /* The same length of tape either way: the head stays inside 0.2..0.8 and
     * comes round about as often as the forward window does. */
    double lo_rev, hi_rev, lo_fwd, hi_fwd;
    int    laps_rev, laps_fwd;
    head_travel(3.0, &lo_rev, &hi_rev, &laps_rev);
    snprintf(buf, sizeof buf, "head %.3f..%.3f, %d laps in 3 s", lo_rev, hi_rev, laps_rev);
    check("the reversed window is the same span, not a collapse",
          lo_rev >= 0.17 && hi_rev <= 0.83 && laps_rev >= 2, buf);

    /* Two reversals are forward: a negative Loop Pitch on top of the crossed
     * pair runs the tape forward again, and THAT reverse is the one the
     * user selected, so its cell says so. */
    set("looper_pitch", "-1");
    render(1.5, -1, -1, 0, 0); /* through zero again, on the tape's own slew */
    const double motion_both = head_motion(8, 0.05);
    const double melody_both = melody_direction(2.0);
    get("looper_reverse", rb_a_dir, sizeof rb_a_dir);
    snprintf(buf, sizeof buf, "head %+.2f, melody %+.2f, reverse %s, dir %.4f", motion_both,
             melody_both, rb_a_dir, window_direction());
    check("a negative pitch on a reversed window is forward again",
          motion_both >= 0.5 && melody_both >= 0.6 && strcmp(rb_a_dir, "On") == 0, buf);
    set("looper_pitch", "1");
    render(1.5, -1, -1, 0, 0);

    /* Uncrossing glides back, and the forward window's travel matches. */
    set("looper_start", "0.2");
    set("looper_end", "0.8");
    render(0.6, -1, -1, 0, 0);
    const double dir_back = window_direction();
    render(0.5, -1, -1, 0, 0);
    const double motion_back = head_motion(8, 0.05);
    head_travel(3.0, &lo_fwd, &hi_fwd, &laps_fwd);
    snprintf(buf, sizeof buf, "dir %.4f, head %+.2f, %.3f..%.3f, %d laps (reversed %d)",
             dir_back, motion_back, lo_fwd, hi_fwd, laps_fwd, laps_rev);
    check("putting the pair back in order runs it forward again, same span",
          fabs(dir_back - 1.0) < 0.001 && motion_back >= 0.5
          && abs(laps_fwd - laps_rev) <= 1, buf);

    /* Crossed while PARKED: the glide runs from the knob, not from the next
     * play, so the tape is already turned round when it rolls. */
    stop_tape();
    set("looper_start", "0.8");
    set("looper_end", "0.2");
    render(0.6, -1, -1, 0, 0);
    const double dir_parked = window_direction();
    start_tape_now();
    const double motion_parked = head_motion(8, 0.05);
    snprintf(buf, sizeof buf, "dir while parked %.4f, head on play %+.2f", dir_parked, motion_parked);
    check("a pair crossed while parked plays backwards from the first pass",
          fabs(dir_parked + 1.0) < 0.001 && motion_parked <= -0.5, buf);

    set("looper_start", "0.0");
    set("looper_end", "1.0");
    render(0.6, -1, -1, 0, 0); /* the glide back to forward, before the next section */

    printf("\n--- Loop Pitch and Loop Rev: the knob and the tape --------------\n");
    /* Two things are being checked at once, and they are different failures:
     * the cell must READ BACK what it was set to (the host repaints
     * get_param on release -- the reported symptom was the sound following the
     * knob while the reading snapped home), and the number must reach the
     * TAPE, which is what the 440 Hz tone in the loop measures. */
    get("looper_playing", buf, sizeof buf);
    if(buf[0] != '1')
    {
        set("looper_play", "Trig"); /* the transport was stopped for scrub */
        render(0.3, -1, -1, 0, 0);
    }

    char rb_a[32], rb_b[32];

    /* THIS section reads the tape's speed, so the loop has to be the tone end
     * to end -- the playhead section above leaves a take with a tone in its
     * first fifth. */
    set("loop_clear", "Clear");
    render(1.6, -1, -1, 0, 0);
    record_loop(1.0, 0.0, 1.0);

    set("looper_pitch", "2.0");
    get("looper_pitch", rb_a, sizeof rb_a);
    render(0.6, -1, -1, 0, 0); /* the rate lerps onto the new speed */
    const double hz_2x = peak_hz(0.2, 0.9);
    snprintf(buf, sizeof buf, "read-back %s, tape %.1f Hz (want 880)", rb_a, hz_2x);
    check("Loop Pitch sticks, and 2.00x is really twice as fast",
          fabs(atof(rb_a) - 2.0) < 0.001 && fabs(hz_2x - 880.0) <= 8.0, buf);

    set("looper_pitch", "1.0");
    get("looper_pitch", rb_a, sizeof rb_a);
    render(0.6, -1, -1, 0, 0);
    const double hz_1x = peak_hz(0.2, 0.9);
    snprintf(buf, sizeof buf, "read-back %s, tape %.1f Hz (want 440)", rb_a, hz_1x);
    check("back at 1.00x it is the tone that was recorded",
          fabs(atof(rb_a) - 1.0) < 0.001 && fabs(hz_1x - 440.0) <= 8.0, buf);

    set("looper_reverse", "On");
    get("looper_reverse", rb_a, sizeof rb_a);
    get("looper_pitch", rb_b, sizeof rb_b);
    snprintf(buf, sizeof buf, "reverse %s, pitch %s", rb_a, rb_b);
    check("Loop Rev sticks, and the shared pitch reading follows it",
          strcmp(rb_a, "On") == 0 && fabs(atof(rb_b) + 1.0) < 0.001, buf);

    set("looper_reverse", "Off");
    get("looper_reverse", rb_a, sizeof rb_a);
    get("looper_pitch", rb_b, sizeof rb_b);
    snprintf(buf, sizeof buf, "reverse %s, pitch %s", rb_a, rb_b);
    check("and turning it back off does too",
          strcmp(rb_a, "Off") == 0 && fabs(atof(rb_b) - 1.0) < 0.001, buf);

    /* A clear winds the tape down and lands it at 1.00x forward, with nothing
     * in the buffer: the knobs are put where the tape is, so they do not go on
     * claiming a speed the empty loop cannot play at. */
    set("looper_pitch", "2.0");
    set("loop_clear", "Clear");
    render(1.6, -1, -1, 0, 0);
    get("looper_pitch", rb_a, sizeof rb_a);
    get("looper_reverse", rb_b, sizeof rb_b);
    char rb_c[32];
    get("looper_scrub", rb_c, sizeof rb_c);
    snprintf(buf, sizeof buf, "pitch %s, reverse %s, scrub %s", rb_a, rb_b, rb_c);
    check("a clear leaves the knobs where the tape landed",
          fabs(atof(rb_a) - 1.0) < 0.001 && strcmp(rb_b, "Off") == 0
          && fabs(atof(rb_c)) < 0.001, buf);

    printf("\n%s (%d failure%s)\n", fails ? "VERDICT: FAIL" : "VERDICT: PASS", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
