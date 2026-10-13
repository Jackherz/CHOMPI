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
            const double v  = on ? 0.5 * sin(2.0 * M_PI * 440.0 * tt) : 0.0;
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
    get("looper_scrub", buf, sizeof buf);
    snprintf(buf, sizeof buf, "head %.4f -> %.4f, cell %s", before, after, buf);
    check("a parked turn puts the head where it was sent, and the cell reads it",
          fabs(after - 0.25) < 0.03 && fabs(atof(buf) - after) < 0.01, buf);

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
    start_tape(); /* play from there */
    const double near_seam = render(0.5, -1, -1, 0.05, 0.45);
    snprintf(buf, sizeof buf, "rms=%.4f", near_seam);
    check("parked before the tone, playback plays it straight away",
          near_seam > 0.01, buf);

    stop_tape();
    scrub_to(0.30);
    render(0.1, -1, -1, 0, 0);
    start_tape();
    const double past_tone = render(0.5, -1, -1, 0.05, 0.45);
    snprintf(buf, sizeof buf, "rms=%.4f", past_tone);
    check("parked past the tone, it waits for the loop to come round",
          past_tone < 0.004, buf);
    stop_tape(); /* leave the tape parked for whatever runs next */

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
