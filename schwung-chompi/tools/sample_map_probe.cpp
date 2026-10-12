/*
 * tools/sample_map_probe.cpp — prove an assigned sample is the one that plays.
 *
 *   ./sample_map_probe [so] [module_dir] [tone_path]
 *
 * The sample map (`set_param("sample_file", <path>)`) is the feature this
 * checks, end to end:
 *
 *   1. render slot 1 as shipped            -> the stock CUBBI sample
 *   2. assign a 1 kHz test tone to slot 1  -> the tone must now play
 *   3. clear the assignment                -> back to the CUBBI sample
 *
 * "Which file played" is measured, not assumed: the probe reports the energy
 * at 1 kHz relative to the block's RMS (a Goertzel filter). A pure tone puts
 * nearly all of its energy in that one bin; a music loop puts a little. The
 * three phases have to disagree for the test to pass — a probe that only ever
 * saw the tone (or never did) tells us nothing.
 */
#include <sys/stat.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include "host/plugin_api_v1.h"

static uint8_t mailbox[8192];
static plugin_api_v2_t* api;
static void*            inst;

/* --- a 44-byte-header stereo 16-bit 48 kHz WAV holding one sine ----------- */
static void write_tone(const char* path, double freq, double seconds)
{
    const int    rate   = 48000;
    const int    frames = (int)(seconds * rate);
    const size_t data   = (size_t)frames * 4;           /* stereo int16 */
    FILE*        f      = fopen(path, "wb");
    if(!f)
    {
        printf("cannot write %s\n", path);
        exit(1);
    }
    uint8_t h[44] = {0};
    memcpy(h, "RIFF", 4);
    uint32_t riff = (uint32_t)(36 + data);
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t sz = 16;   memcpy(h + 16, &sz, 4);
    uint16_t fmt = 1, ch = 2, bits = 16, align = 4;
    uint32_t br = rate * 4;
    memcpy(h + 20, &fmt, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &br, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    uint32_t ds = (uint32_t)data;
    memcpy(h + 40, &ds, 4);
    fwrite(h, 1, 44, f);
    for(int i = 0; i < frames; i++)
    {
        const int16_t v = (int16_t)(sin(2.0 * M_PI * freq * i / rate) * 0.5 * 32767.0);
        fwrite(&v, 2, 1, f);
        fwrite(&v, 2, 1, f);
    }
    fclose(f);
}

/* --- phase: press the key, render, find where the energy is ---------------
 * A single-frequency share is the wrong instrument here: the CUBBI samples on
 * the card are 48 kHz and this engine runs at 44.1 kHz, so a "1 kHz" tone in
 * the file comes out at 1000 * 44100/48000 = 919 Hz. Measure the PEAK
 * frequency by scanning, then refine it — the answer is then a fact about the
 * audio, not an assumption about the file.
 */
static double g_tone = 1000.0;

/* Goertzel over a block of samples, returning |X|^2 (unnormalised). */
static double goertzel(const double* x, long n, double freq)
{
    const double w     = 2.0 * M_PI * freq / MOVE_SAMPLE_RATE;
    const double coeff = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0;
    for(long i = 0; i < n; i++)
    {
        const double t = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = t;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

static double phase(const char* label, double seconds, int note = 48)
{
    uint8_t on[3] = {0x90, (uint8_t)note, 100}; /* C3 (48) -> slot 1 */
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

    const double rms = sqrt(rms_sum / (rms_n ? rms_n : 1));

    /* Coarse scan, then a 1 Hz refine around the winner. */
    double best_f = 0.0, best_e = 0.0;
    for(double f = 200.0; f <= 4000.0; f += 25.0)
    {
        const double e = goertzel(x, n, f);
        if(e > best_e)
        {
            best_e = e;
            best_f = f;
        }
    }
    for(double f = best_f - 25.0; f <= best_f + 25.0; f += 1.0)
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

    /* Amplitude of that component: for a real sinusoid |X| = A*N/2. */
    const double amp   = (n > 0) ? (2.0 * sqrt(best_e) / (double)n) : 0.0;
    const double share = (rms > 1e-9) ? (amp / (sqrt(2.0) * rms)) : 0.0;
    printf("%-34s rms=%7.4f  peak=%6.1fHz  share=%6.2f%%\n",
           label, rms, best_f, share * 100.0);
    return share;
}

static void note_press(int note, double seconds)
{
    uint8_t on[3]  = {0x90, (uint8_t)note, 100};
    uint8_t off[3] = {0x80, (uint8_t)note, 0};
    api->on_midi(inst, on, 3, MOVE_MIDI_SOURCE_INTERNAL);
    const int blocks = (int)(seconds * MOVE_SAMPLE_RATE / MOVE_FRAMES_PER_BLOCK);
    int16_t   out[256];
    for(int b = 0; b < blocks; b++)
    {
        api->render_block(inst, out, MOVE_FRAMES_PER_BLOCK);
        usleep(2900);
    }
    api->on_midi(inst, off, 3, MOVE_MIDI_SOURCE_INTERNAL);
}

static void wait_for_scan(int ms)
{
    for(int i = 0; i < ms / 20; i++)
        usleep(20 * 1000);
}

int main(int argc, char** argv)
{
    const char* so   = argc > 1 ? argv[1] : "build/chompi_move.so";
    const char* dir  = argc > 2 ? argv[2] : ".";
    const char* tone = argc > 3 ? argv[3] : "/tmp/chompi_probe_tone.wav";

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

    /* Start from a clean map. The module reads <module_dir>/samples/samples.map
     * at create, so a leftover assignment from an earlier run would show up as
     * phase 1 playing the tone — the test assert itself would pass for the
     * wrong reason. */
    {
        char map_path[512];
        snprintf(map_path, sizeof map_path, "%s/samples/samples.map", dir);
        remove(map_path);
    }

    api  = init(&h);
    inst = api->create_instance(dir, NULL);
    api->set_param(inst, "voice_mode", "cubbi");
    api->set_param(inst, "bank", "0");
    api->set_param(inst, "slot", "1");

    char buf[1024];

    write_tone(tone, g_tone, 4.0);
    printf("tone written: %s\n", tone);

    wait_for_scan(1500);
    int n = api->get_param(inst, "debug_files", buf, sizeof buf);
    printf("slots: %s\n", n > 0 ? buf : "-");
    n = api->get_param(inst, "sample_file", buf, sizeof buf);
    printf("sample_file (before) : %s\n", n > 0 ? buf : "-");

    const double stock = phase("1. shipped slot 1", 1.0);

    api->set_param(inst, "sample_file", tone);
    wait_for_scan(1500);
    n = api->get_param(inst, "sample_file", buf, sizeof buf);
    printf("sample_file (after)  : %s\n", n > 0 ? buf : "-");
    n = api->get_param(inst, "sample_map", buf, sizeof buf);
    printf("sample_map           : %s\n", n > 0 ? buf : "-");

    const double assigned = phase("2. assigned tone on slot 1", 1.0);

    /* 3. persistence: tear the instance down and build a new one, the way the
     * chain host does on a reload or a power cycle. The map is read back from
     * <sample_dir>/samples.map on create, so the assignment must survive — and
     * the tone must still be what plays. */
    api->destroy_instance(inst);
    usleep(300 * 1000);
    inst = api->create_instance(dir, NULL);
    api->set_param(inst, "voice_mode", "cubbi");
    api->set_param(inst, "bank", "0");
    api->set_param(inst, "slot", "1");
    wait_for_scan(1500);
    n = api->get_param(inst, "sample_map", buf, sizeof buf);
    printf("sample_map (reloaded): %s\n", n > 0 ? buf : "-");
    n = api->get_param(inst, "sample_file", buf, sizeof buf);
    printf("sample_file(reloaded): %s\n", n > 0 ? buf : "-");
    const double reloaded = phase("3. after reload", 1.0);

    api->set_param(inst, "sample_clear", "1");
    wait_for_scan(1500);
    n = api->get_param(inst, "sample_file", buf, sizeof buf);
    printf("sample_file (cleared): %s\n", n > 0 ? buf : "-");
    n = api->get_param(inst, "sample_map", buf, sizeof buf);
    printf("sample_map (cleared) : %s\n", n > 0 ? buf : "-");

    const double cleared = phase("4. slot 1 after clear", 1.0);

    /* Cancel safety. The host's file browser seeds itself with the path we
     * report and writes that same value straight back when the user backs out,
     * so "assign what the slot already plays by convention" has to be a no-op.
     * If it pinned an override instead, backing out of the browser would leave
     * a relative name in the map that outlives the shipped sample. */
    api->set_param(inst, "slot", "3");
    wait_for_scan(300);
    n = api->get_param(inst, "sample_file", buf, sizeof buf);
    char convention[256] = {0};
    snprintf(convention, sizeof convention, "%s", n > 0 ? buf : "");

    api->set_param(inst, "sample_file", tone);       /* a real assignment */
    wait_for_scan(1200);
    n = api->get_param(inst, "sample_map", buf, sizeof buf);
    const bool assigned_slot3 = (strstr(buf, "3=") != NULL);

    api->set_param(inst, "sample_file", convention); /* the browser's cancel */
    wait_for_scan(1200);
    n = api->get_param(inst, "sample_map", buf, sizeof buf);
    const bool cancel_clean = (n <= 0) || (strstr(buf, "3=") == NULL);
    n = api->get_param(inst, "sample_file", buf, sizeof buf);
    const bool back_to_stock = (n > 0) && strcmp(buf, convention) == 0;

    printf("\ncancel: assigned=%d clean=%d restores=%d (convention=%s)\n",
           assigned_slot3, cancel_clean, back_to_stock, convention);

    /* The settle window is 500 ms, so an assignment made just before the host
     * unloads the synth would otherwise be dropped: destroy has to flush it.
     * No wait here on purpose — that is the whole test. */
    api->set_param(inst, "slot", "4");
    wait_for_scan(300);
    api->set_param(inst, "sample_file", tone);
    api->destroy_instance(inst);          /* immediately, mid-settle */
    usleep(300 * 1000);
    inst = api->create_instance(dir, NULL);
    api->set_param(inst, "voice_mode", "cubbi");
    api->set_param(inst, "bank", "0");
    api->set_param(inst, "slot", "4");
    wait_for_scan(1500);
    n = api->get_param(inst, "sample_map", buf, sizeof buf);
    const bool flushed = (n > 0) && (strstr(buf, "4=") != NULL);
    printf("flush-on-unload: %s\n", flushed ? "kept" : "LOST");

    /* P11: touching a slot whose file is missing must never create files
     * outside the module's own directory. Two halves, so the asymmetry is
     * pinned: a user-library path stays missing (no stubs in somebody's
     * library), a convention path still becomes the placeholder the firmware
     * would have made. */
    remove("/tmp/chompi_missing_probe.wav");
    api->set_param(inst, "slot", "5");
    wait_for_scan(300);
    api->set_param(inst, "sample_file", "/tmp/chompi_missing_probe.wav");
    wait_for_scan(1200);

    /* Play the two slots: the open happens on the key path (SetVoiceSlot), not
     * on the selection knob. 55 -> slot 5 (assigned, file gone) and 57 -> slot
     * 6 (no assignment, no file on this card). */
    note_press(55, 0.25);
    note_press(57, 0.25);
    wait_for_scan(600);
    struct stat st;
    const bool user_stub = (stat("/tmp/chompi_missing_probe.wav", &st) == 0);
    char managed[512];
    snprintf(managed, sizeof managed, "%s/samples/cubbi_a6.wav", dir);
    const bool managed_ok = (stat(managed, &st) == 0);
    printf("P11 create-guard: user-path stub=%d (want 0), managed placeholder=%d (want 1)\n",
           user_stub, managed_ok);

    /* This probe walks slots 3-6, so the engine's placeholder behaviour leaves
     * 0-byte convention files behind in the module's samples/ directory. Remove
     * the ones it caused (only ever empty files -- a shipped sample is not
     * touched) so a local run does not leave the working tree dirty. */
    int cleaned = 0;
    for(int slot = 3; slot <= 6; slot++)
    {
        char path[512];
        snprintf(path, sizeof path, "%s/samples/cubbi_a%d.wav", dir, slot);
        if(stat(path, &st) == 0 && st.st_size == 0 && remove(path) == 0)
            cleaned++;
    }
    if(cleaned)
        printf("cleaned %d placeholder file(s) from samples/\n", cleaned);

    /* --- the portal: where the browser lives (PORT PATCH P18) --------------
     *
     * The module keeps a browser root — a directory with symlinks to the
     * shipped samples and to the user's library — and presents sample paths
     * through it while storing the real files. On a device that root is
     * /data/UserData/chompi-samples; on this host it does not exist, so the
     * two directories are pointed at a scratch tree, which is the same code
     * path with a different prefix. */
    printf("\n--- the sample portal -------------------------------------------\n");
    /* Under /tmp, like the probe's tone: the module directory is usually the
     * repo itself, and the browser root does not have to live beside the
     * samples. Removed at the start and the end so a rerun starts clean. */
    const char* portal  = "/tmp/chompi_probe_portal";
    const char* userlib = "/tmp/chompi_probe_userlib";
    char        link[600];

    /* A stand-in user library with one WAV in it. */
    char user_tone[600];
    snprintf(user_tone, sizeof user_tone, "%s/my_sample.wav", userlib);
    {
        char cmd[1200];
        snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s' && mkdir -p '%s' '%s'",
                 userlib, portal, userlib, portal);
        if(system(cmd) != 0)
            printf("(could not create the scratch tree under %s)\n", dir);
        FILE* in = fopen(tone, "rb");
        FILE* out = fopen(user_tone, "wb");
        if(in && out)
        {
            char copy[4096];
            size_t got;
            while((got = fread(copy, 1, sizeof copy, in)) > 0)
                fwrite(copy, 1, got, out);
        }
        if(in)
            fclose(in);
        if(out)
            fclose(out);
    }

    api->set_param(inst, "portal_dir", portal);
    api->set_param(inst, "user_library_dir", userlib);
    wait_for_scan(400);

    n = api->get_param(inst, "sample_root", buf, sizeof buf);
    const bool root_reported = (n > 0) && strcmp(buf, portal) == 0;

    snprintf(link, sizeof link, "%s/Built-in", portal);
    struct stat lst;
    const bool builtin_link = (lstat(link, &lst) == 0 && S_ISLNK(lst.st_mode)
                               && stat(link, &lst) == 0 && S_ISDIR(lst.st_mode));
    snprintf(link, sizeof link, "%s/User Library", portal);
    const bool userlib_link = (lstat(link, &lst) == 0 && S_ISLNK(lst.st_mode)
                               && stat(link, &lst) == 0 && S_ISDIR(lst.st_mode));
    printf("portal: root=%s built-in-link=%d user-library-link=%d\n",
           root_reported ? "ok" : "WRONG", builtin_link, userlib_link);
    if(n > 0)
        printf("        %s\n", buf);
    n = api->get_param(inst, "debug_portal", buf, sizeof buf);
    printf("        %s\n", n > 0 ? buf : "-");

    /* A shipped sample is presented through the portal … */
    api->set_param(inst, "slot", "1");
    wait_for_scan(300);
    n = api->get_param(inst, "sample_file", buf, sizeof buf);
    char presented[512] = {0};
    snprintf(presented, sizeof presented, "%s", n > 0 ? buf : "");
    snprintf(link, sizeof link, "%s/Built-in/", portal);
    const bool presented_builtin = (strncmp(presented, link, strlen(link)) == 0);
    const bool presented_readable = (presented[0] && stat(presented, &lst) == 0);
    printf("shipped slot 1 presents as : %s\n", presented);

    /* … and writing back exactly what it was given (the host's cancel) still
     * leaves no override behind. This is the case a portal path could quietly
     * break: the value read is not the value the engine opens. */
    api->set_param(inst, "sample_file", presented);
    wait_for_scan(1200);
    n = api->get_param(inst, "sample_map", buf, sizeof buf);
    const bool presented_cancel_clean = (n <= 0) || (strstr(buf, "1=") == NULL);

    /* A user-library sample assigned through the portal plays. */
    char via_portal[700];
    snprintf(via_portal, sizeof via_portal, "%s/User Library/my_sample.wav", portal);
    api->set_param(inst, "slot", "6");
    wait_for_scan(300);
    api->set_param(inst, "sample_file", via_portal);
    wait_for_scan(1500);
    n = api->get_param(inst, "sample_map", buf, sizeof buf);
    char map_after[1024] = {0};
    snprintf(map_after, sizeof map_after, "%s", n > 0 ? buf : "");
    const bool stored_canonical = (strstr(map_after, user_tone) != NULL);
    n = api->get_param(inst, "sample_file", buf, sizeof buf);
    const bool presented_userlib = (n > 0) && (strcmp(buf, via_portal) == 0);
    /* 57 -> slot 6, i.e. the sample reached through the symlink. The tone
     * share is the same measurement the phases at the top use. */
    const double via_portal_share = phase("5. user sample via the portal", 1.0, 57);
    printf("user sample via portal        : written=%s stored=%s reads-back=%s\n",
           via_portal, stored_canonical ? "canonical" : "AS GIVEN",
           presented_userlib ? "portal path" : (n > 0 ? buf : "-"));
    printf("portal: presented-builtin=%d readable=%d cancel-clean=%d\n",
           presented_builtin, presented_readable, presented_cancel_clean);

    /* Leave nothing behind: the portal tree is scratch, and the map and the
     * placeholders are cleaned above. */
    {
        char cmd[1200];
        snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", userlib, portal);
        if(system(cmd) != 0)
            printf("(could not remove the scratch portal tree)\n");
    }

    printf("\nVERDICT stock=%.2f%% assigned=%.2f%% reloaded=%.2f%% cleared=%.2f%%\n",
           stock * 100, assigned * 100, reloaded * 100, cleared * 100);
    /* A pure tone is a single partial: most of the block's energy sits in one
     * frequency. A music loop spreads it. So the assignment must make the
     * share JUMP, and clearing must bring it back down. */
    /* phase() returns a FRACTION; the percentages above are for reading. */
    const bool ok = assigned > 0.50 && assigned > stock * 3.0
                    && reloaded > 0.50 /* the map persisted */
                    && cleared < 0.25
                    && assigned_slot3 && cancel_clean && back_to_stock
                    && flushed && !user_stub && managed_ok
                    && root_reported && builtin_link && userlib_link
                    && presented_builtin && presented_readable
                    && presented_cancel_clean && stored_canonical
                    && presented_userlib && via_portal_share > 0.50;
    printf("%s\n", ok ? "PASS: the assigned sample is what plays"
                      : "FAIL: the override did not change what plays");
    return ok ? 0 : 3;
}
