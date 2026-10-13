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

/* Records one loop: arm, play the burst, stop. Returns the loop's length in
 * seconds (the recording length), or 0 if the engine never started recording. */
static double record_loop(double seconds, double in_from, double in_to)
{
    char buf[64];
    set("looper_record", "Trig"); /* ToggleRecord() -- arms/starts */
    render(0.05, -1, -1, 0, 0);   /* CheckRecordReady() runs in ProcessAudio */
    get("looper_recording", buf, sizeof buf);
    const bool started = buf[0] == '1';
    render(seconds, in_from, in_to, seconds, seconds); /* measure nothing */
    set("looper_record", "Trig"); /* stop: first record ends, playback begins */
    return started ? seconds : 0.0;
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

    printf("\n%s (%d failure%s)\n", fails ? "VERDICT: FAIL" : "VERDICT: PASS", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
