/*
 * chompi_move.cpp — CHOMPI TAPE as a Schwung sound generator for Ableton Move.
 *
 * WHAT THIS IS
 * ------------
 * A thin adapter between Schwung's plugin ABI (move_plugin_init_v2) and the
 * unmodified CHOMPI TAPE engine (dsp/engine, copied verbatim from
 * firmware/chompi-tape/code/src — see VENDORED.md for the patch list).
 *
 * The mapping from CHOMPI's hardware to Move:
 *
 *   CHOMPI                       Move / Schwung
 *   ---------------------------------------------------------------------
 *   25-key chromatic keyboard    MIDI notes 48..72 (Move pads/keys/keys mode,
 *                                or any external controller)
 *   encoders 1..6                chain_params (set_param) driven by the
 *                                shadow UI knobs, see ui_hierarchy JSON
 *   KEY_26 record / KEY_27 play  set_param looper_record / looper_play
 *   KEY_28 loop                  set_param looper_play
 *   mic + stereo aux in          Move's line input (mapped_memory +
 *                                audio_in_offset), engine InputSource::LINE_IN
 *   headphone + line out         Move's stereo out (engine out[0]/out[1])
 *   microSD streaming            a SCHED_OTHER worker thread that drains the
 *                                same FileRequest FIFO the firmware drains on
 *                                its SD callback
 *
 * REALTIME CONTRACT (docs/REALTIME_SAFETY.md in the Schwung tree):
 *   create_instance/destroy_instance/set_param/get_param/on_midi/render_block
 *   all run ON the SPI audio callback. So: no file I/O, no allocation, no
 *   locks, no logging in any of them. The only long-running thing here is the
 *   worker thread, which demotes itself to SCHED_OTHER and pins to cores 0-2
 *   as the contract requires.
 */

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>

#include "host/plugin_api_v1.h"

#include "DSPEngine.h"

// ---------------------------------------------------------------- constants

#define CHOMPI_BLOCK MOVE_FRAMES_PER_BLOCK /* 128 */
#define CHOMPI_MAX_INSTANCES 4
#define CHOMPI_PATH_LEN 512

/** CHOMPI's keyboard->MIDI map, copied verbatim from NormalPage.h (key_map[]).
 *  Index = CHOMPI's SwId button id (what KeyToSlot() and the engine's
 *  `key` argument expect), value = the MIDI note that key sends.
 *  Only the 25 keyboard entries are listed; entries 0..6 and 32..39 are
 *  encoder switches, not keys. */
static const struct
{
    uint8_t button;
    uint8_t note;
} kChompiKeys[] = {
    {15, 0x30}, /* KEY_1  C3  */ {7, 0x31}, {8, 0x32}, {12, 0x33}, {9, 0x34},
    {10, 0x35}, {13, 0x36}, {11, 0x37}, {14, 0x38}, {16, 0x39}, {21, 0x3a},
    {17, 0x3b}, {18, 0x3c}, {22, 0x3d}, {19, 0x3e}, {23, 0x3f}, {20, 0x40},
    {24, 0x41}, {29, 0x42}, {25, 0x43}, {30, 0x44}, {26, 0x45}, {31, 0x46},
    {27, 0x47}, {28, 0x48}, /* KEY_15 C5 */
};
static const int kNumChompiKeys = sizeof(kChompiKeys) / sizeof(kChompiKeys[0]);
static const int kChompiLowNote  = 0x30; /* C3 */
static const int kChompiHighNote = 0x48; /* C5 */

/* chompi::g_sample_dir (the runtime sample directory) is defined in
 * port/chompi_port.cpp so the engine links without the plugin too. */

// ---------------------------------------------------------------- instance

struct ChompiInstance
{
    const host_api_v1_t* host = nullptr;

    /* CHOMPI engine + the memory the firmware keeps in SDRAM statics. */
    daisy::Engine  engine;
    daisysp::Reverb reverb;
    chompi::InterpolatedDelayLine::AudioSample* delay_mem = nullptr;
    daisy::RamBufferMemory                     loop_buff{};
    daisy::RamBufferMemory                     chompi_buff{};
    int16_t*                                   loop_mem   = nullptr;
    int16_t*                                   chompi_mem = nullptr;

    /* Audio scratch — sized once, never allocated on the callback. */
    float        in_f[4][CHOMPI_BLOCK];
    float        out_f[4][CHOMPI_BLOCK];
    const float* in_p[4];
    float*       out_p[4];

    /* Worker thread. */
    pthread_t        worker{};
    std::atomic<bool> worker_started{false};
    std::atomic<bool> stopping{false};

    /* Flags raised by set_param (callback) and drained by the worker. */
    std::atomic<bool> want_scan_files{false};

    /* Octave folding window for incoming notes. */
    std::atomic<int> octave{0};

    /* Mirror of the last value applied for each savable parameter. The engine
     * exposes setters (and only some getters), so the slot autosave is answered
     * from here rather than by reaching into the DSP. Only touched from the
     * callback entry points. */
    struct Saved
    {
        float volume = 1.f, filter = 1.f, resonance = 0.f, saturate = 0.f;
        float warble = 0.f, delay_time = 0.f, delay_feedback = 0.f;
        float reverb = 0.f, final_comp = 0.f, input_gain = 1.f;
        int   input_monitor = 0, octave = 0, bank = 0;
        char  input_source[8]  = "line";
        char  monitor_mode[8]  = "both";
        char  voice_mode[12]   = "jammi"; /* or "cubbi" */
        char  sample_dir[CHOMPI_PATH_LEN] = {0};
    } saved;

    /* Honest, small error channel. */
    char lasterr[160] = {0};

    bool recording = false;
    bool loop_recording = false;
};

static ChompiInstance* g_instances[CHOMPI_MAX_INSTANCES];
static std::atomic<int> g_instance_count{0};
static const host_api_v1_t* g_host_api = nullptr;

static void set_err(ChompiInstance* inst, const char* msg)
{
    if(!inst)
        return;
    // snprintf is fine here: set_param/get_param are allowed to be slow-ish by
    // comparison, but they are still on the callback, so keep it to one call.
    snprintf(inst->lasterr, sizeof(inst->lasterr), "%s", msg);
}

/** MIDI note -> CHOMPI button id, with two-octave folding so Move's pads can
 *  reach the whole CHOMPI range from any octave. Returns -1 for "no key". */
static int note_to_button(int note, int octave_shift)
{
    int n = note + 12 * octave_shift;
    while(n < kChompiLowNote)
        n += 12;
    while(n > kChompiHighNote)
        n -= 12;
    for(int i = 0; i < kNumChompiKeys; i++)
    {
        if(kChompiKeys[i].note == n)
            return kChompiKeys[i].button;
    }
    return -1;
}

// ---------------------------------------------------------------- worker

/**
 * The worker is CHOMPI's SDCallback, moved to a real thread: it drains the
 * FileRequest FIFO that the audio path fills (opens, reads, writes, seeks,
 * header updates) and runs the periodic housekeeping the firmware did in its
 * main loop (scanning the sample directory).
 */
static void* chompi_io_worker(void* arg)
{
    (void)arg;

    /* Schwung contract: demote first, then keep off core 3 (SPI). */
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = 0;
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    CPU_SET(1, &set);
    CPU_SET(2, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);

    while(true)
    {
        bool any_alive = false;
        const int count = g_instance_count.load();
        for(int i = 0; i < count; i++)
        {
            ChompiInstance* inst = g_instances[i];
            if(!inst)
                continue;
            any_alive = true;

            if(inst->stopping.load(std::memory_order_acquire))
                continue;

            /* CHOMPI's SDCallback: copier.CopyProcess() -> engine.ProcessFileRequests() */
            inst->engine.ProcessFileRequests();

            if(inst->want_scan_files.exchange(false))
                inst->engine.UpdateFileExists();
        }
        if(!any_alive)
            break;
        usleep(500); /* 2 kHz: CHOMPI ran its SD callback at 1 kHz */
    }
    return nullptr;
}

static void start_worker_if_needed()
{
    static std::atomic<bool> started{false};
    bool expected = false;
    if(started.compare_exchange_strong(expected, true))
    {
        pthread_t t;
        if(pthread_create(&t, nullptr, chompi_io_worker, nullptr) == 0)
            pthread_detach(t);
    }
}

// ---------------------------------------------------------------- params

/** Every parameter is applied straight to the engine: the engine slews its
 *  own targets internally (fonepole), which is what makes live knob moves
 *  click-free — the same path CHOMPI's UI used. */
static void apply_state_json(ChompiInstance* inst, const char* json);

static void apply_param(ChompiInstance* inst, const char* key, const char* val)
{
    if(!inst || !key || !val)
        return;

    /* Slot autosave. The host asks for `state` and later hands the same string
     * back through set_param; that restore runs on the loader thread. */
    if(strcmp(key, "state") == 0)
    {
        apply_state_json(inst, val);
        return;
    }

    daisy::Engine& e = inst->engine;

    const float f = strtof(val, nullptr);
    const int   b = (int)(f != 0.f) && (strcmp(val, "0") != 0);

    if(strcmp(key, "volume") == 0)
        inst->saved.volume = f, e.SetMainGain(f);
    else if(strcmp(key, "filter") == 0)
        inst->saved.filter = f, e.SetFilter(f);
    else if(strcmp(key, "resonance") == 0)
        inst->saved.resonance = f, e.SetFilterResonance(f);
    else if(strcmp(key, "saturate") == 0)
        inst->saved.saturate = f, e.SetSaturate(f);
    else if(strcmp(key, "warble") == 0)
        inst->saved.warble = f, e.SetWarble(f);
    else if(strcmp(key, "delay_time") == 0)
        inst->saved.delay_time = f, e.SetDelayTime(f);
    else if(strcmp(key, "delay_feedback") == 0)
        inst->saved.delay_feedback = f, e.SetDelayFeedback(f);
    else if(strcmp(key, "reverb") == 0)
        inst->saved.reverb = f, e.SetReverb(f);
    else if(strcmp(key, "final_comp") == 0)
        e.SetFinalComp(f);
    else if(strcmp(key, "gain") == 0)
        e.SetGain(f);
    else if(strcmp(key, "pan") == 0)
        e.SetPan(f);
    else if(strcmp(key, "attack") == 0)
        e.SetAttack(f);
    else if(strcmp(key, "decay") == 0)
        e.SetDecay(f);
    else if(strcmp(key, "start") == 0)
        e.SetStartPoint(f);
    else if(strcmp(key, "end") == 0)
        e.SetEndPoint(f);
    else if(strcmp(key, "varspeed") == 0)
        e.SetGlobalPitchFree(f); /* 0..1 UI value, engine maps to a ratio */
    else if(strcmp(key, "reverse") == 0)
        e.SetReverse(b);
    else if(strcmp(key, "sustain") == 0)
        e.SetSustainActive(b);
    else if(strcmp(key, "auto_loop") == 0)
        e.SetAutoLoop(b);
    else if(strcmp(key, "input_monitor") == 0)
        inst->saved.input_monitor = b, e.SetInputMonitor(b);
    else if(strcmp(key, "input_gain") == 0)
        inst->saved.input_gain = f, e.SetInputGain(f);
    else if(strcmp(key, "monitor_mode") == 0)
    {
        /* Reconstructed rather than stepped: the engine only offers
         * IncrementMonitorMode() (a UI button on hardware). */
        daisy::MonitorMode m = daisy::MonitorMode::BOTH;
        if(strcmp(val, "hp") == 0)
            m = daisy::MonitorMode::HP;
        else if(strcmp(val, "sendret") == 0)
            m = daisy::MonitorMode::SEND_RET;
        while(e.GetMonitorMode() != m)
            e.IncrementMonitorMode();
        snprintf(inst->saved.monitor_mode, sizeof(inst->saved.monitor_mode), "%s",
                 m == daisy::MonitorMode::HP ? "hp"
                 : (m == daisy::MonitorMode::SEND_RET ? "sendret" : "both"));
    }
    else if(strcmp(key, "input_source") == 0)
    {
        const bool mic = (strcmp(val, "mic") == 0);
        snprintf(inst->saved.input_source, sizeof(inst->saved.input_source), "%s",
                 mic ? "mic" : "line");
        e.SetInputSource(mic ? daisy::InputSource::MIC : daisy::InputSource::LINE_IN);
    }
    else if(strcmp(key, "octave") == 0)
    {
        inst->saved.octave = (int)f;
        inst->octave.store((int)f);
    }
    else if(strcmp(key, "voice_mode") == 0)
    {
        const bool cubbi = (strcmp(val, "cubbi") == 0);
        snprintf(inst->saved.voice_mode, sizeof(inst->saved.voice_mode), "%s",
                 cubbi ? "cubbi" : "jammi");
        e.SetVoiceMode(cubbi ? daisy::VoiceMode::CUBBI : daisy::VoiceMode::JAMMI);
    }
    else if(strcmp(key, "bank") == 0)
    {
        const size_t bank = (size_t)(f < 0.f ? 0 : (f > 4.f ? 4 : (int)f));
        inst->saved.bank = (int)bank;
        e.SetBank(bank);
    }
    else if(strcmp(key, "slot") == 0)
    {
        int slot = (int)f;
        if(slot < 0)
            slot = 0;
        if(slot > 15)
            slot = 15;
        e.SetVoiceSlot((size_t)slot, true);
    }
    else if(strcmp(key, "record") == 0)
    {
        if(b && !inst->recording)
        {
            e.StartNewRecording(0);
            inst->recording = true;
        }
        else if(!b && inst->recording)
        {
            e.StopRecording();
            inst->recording = false;
            inst->want_scan_files.store(true); /* temp_rec.wav now exists */
        }
    }
    else if(strcmp(key, "looper_record") == 0)
        e.ToggleLooperRecord();
    else if(strcmp(key, "looper_play") == 0)
        e.ToggleLooperPlaying();
    else if(strcmp(key, "looper_reverse") == 0)
        e.ResetLooperPitchQuant(), e.SetLooperPitch(b ? -1.f : 1.f);
    else if(strcmp(key, "looper_pitch") == 0)
        e.SetLooperPitch(f);
    else if(strcmp(key, "looper_scrub") == 0)
        e.SetLooperScrub(f);
    else if(strcmp(key, "looper_dub_gain") == 0)
        e.IncrementLooperDubGain(f);
    else if(strcmp(key, "sample_dir") == 0)
    {
        /* Loader thread normally; a live change just re-points the prefix for
         * the next filename the engine builds and asks for a rescan. */
        chompi::SetSampleDir(val);
        inst->want_scan_files.store(true);
    }
    else
        set_err(inst, "unknown param");
}

/** Copies the value of `key` out of a flat JSON object into `out`. Numbers
 *  come out verbatim, strings unquoted, booleans as "1"/"0". False when the
 *  key is absent. Hand-rolled on purpose: this is the callback's only parser
 *  and it must not allocate. */
static bool json_field(const char* json, const char* key, char* out, size_t out_len)
{
    if(!json || !key || !out || out_len == 0)
        return false;

    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* p = strstr(json, pattern);
    if(!p)
        return false;
    p += strlen(pattern);
    while(*p == ' ' || *p == '\t' || *p == ':')
        p++;

    size_t n = 0;
    if(*p == '"')
    {
        for(p++; *p && *p != '"' && n + 1 < out_len; p++)
            out[n++] = *p;
    }
    else if(*p == 't' || *p == 'f')
    {
        out[n++] = (*p == 't') ? '1' : '0';
    }
    else
    {
        while(*p && *p != ',' && *p != '}' && n + 1 < out_len)
            out[n++] = *p++;
        while(n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t'))
            n--;
    }
    out[n] = '\0';
    return n > 0;
}

/** Re-applies every field of a saved `state` string. */
static void apply_state_json(ChompiInstance* inst, const char* json)
{
    static const char* const kKeys[] = {
        "volume", "filter", "resonance", "saturate", "warble",
        "delay_time", "delay_feedback", "reverb", "final_comp", "input_gain",
        "input_monitor", "input_source", "monitor_mode", "octave", "bank",
        "voice_mode",
    };
    char val[CHOMPI_PATH_LEN];
    for(size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++)
        if(json_field(json, kKeys[i], val, sizeof(val)))
            apply_param(inst, kKeys[i], val);
}

/** JSON the shadow UI uses to build the parameter pages, and the entries the
 *  chain host turns into knob assignments. Kept as plain literals on purpose:
 *  building JSON on the callback would mean allocation. */
static const char* kChainParams
    = "[{\"key\":\"volume\",\"name\":\"Volume\",\"type\":\"float\",\"min\":0,\"max\":1},"
      "{\"key\":\"filter\",\"name\":\"Filter\",\"type\":\"float\",\"min\":0,\"max\":1},"
      "{\"key\":\"saturate\",\"name\":\"Saturate\",\"type\":\"float\",\"min\":0,\"max\":1},"
      "{\"key\":\"warble\",\"name\":\"Warble\",\"type\":\"float\",\"min\":0,\"max\":1},"
      "{\"key\":\"delay_feedback\",\"name\":\"Delay\",\"type\":\"float\",\"min\":0,\"max\":1},"
      "{\"key\":\"delay_time\",\"name\":\"Dly Time\",\"type\":\"float\",\"min\":0,\"max\":1},"
      "{\"key\":\"reverb\",\"name\":\"Reverb\",\"type\":\"float\",\"min\":0,\"max\":1},"
      "{\"key\":\"varspeed\",\"name\":\"Varispeed\",\"type\":\"float\",\"min\":0,\"max\":1},"
      "{\"key\":\"reverse\",\"name\":\"Reverse\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
      "{\"key\":\"sustain\",\"name\":\"Sustain\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
      "{\"key\":\"auto_loop\",\"name\":\"Loop\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
      "{\"key\":\"attack\",\"name\":\"Attack\",\"type\":\"float\",\"min\":0,\"max\":1},"
      "{\"key\":\"decay\",\"name\":\"Decay\",\"type\":\"float\",\"min\":0,\"max\":1},"
      "{\"key\":\"start\",\"name\":\"Start\",\"type\":\"float\",\"min\":0,\"max\":1},"
      "{\"key\":\"end\",\"name\":\"End\",\"type\":\"float\",\"min\":0,\"max\":1},"
      "{\"key\":\"gain\",\"name\":\"Gain\",\"type\":\"float\",\"min\":0,\"max\":1},"
      "{\"key\":\"pan\",\"name\":\"Pan\",\"type\":\"float\",\"min\":0,\"max\":1},"
      "{\"key\":\"looper_record\",\"name\":\"Loop Rec\",\"type\":\"enum\",\"options\":[\"-\",\"Trig\"]},"
      "{\"key\":\"looper_play\",\"name\":\"Loop Play\",\"type\":\"enum\",\"options\":[\"-\",\"Trig\"]},"
      "{\"key\":\"looper_reverse\",\"name\":\"Loop Rev\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
      "{\"key\":\"looper_pitch\",\"name\":\"Loop Pitch\",\"type\":\"float\",\"min\":-2,\"max\":2},"
      "{\"key\":\"looper_scrub\",\"name\":\"Scrub\",\"type\":\"float\",\"min\":-2,\"max\":2},"
      "{\"key\":\"looper_dub_gain\",\"name\":\"Dub Gain\",\"type\":\"float\",\"min\":-1,\"max\":1},"
      "{\"key\":\"record\",\"name\":\"Chompi Rec\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
      "{\"key\":\"input_monitor\",\"name\":\"Monitor\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
      "{\"key\":\"input_gain\",\"name\":\"In Gain\",\"type\":\"float\",\"min\":0,\"max\":2},"
      "{\"key\":\"input_source\",\"name\":\"Input\",\"type\":\"enum\",\"options\":[\"line\",\"mic\"]},"
      "{\"key\":\"voice_mode\",\"name\":\"Mode\",\"type\":\"enum\",\"options\":[\"jammi\",\"cubbi\"]},"
      "{\"key\":\"bank\",\"name\":\"Bank\",\"type\":\"int\",\"min\":0,\"max\":4},"
      "{\"key\":\"slot\",\"name\":\"Slot\",\"type\":\"int\",\"min\":0,\"max\":15},"
      "{\"key\":\"octave\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-2,\"max\":2}]";

static const char* kUiHierarchy
    = "{\"levels\":{\"root\":{\"label\":\"CHOMPI\",\"params\":["
      "{\"key\":\"volume\"},{\"key\":\"filter\"},{\"key\":\"saturate\"},{\"key\":\"warble\"},"
      "{\"key\":\"delay_feedback\"},{\"key\":\"delay_time\"},{\"key\":\"reverb\"},{\"key\":\"varspeed\"},"
      "{\"level\":\"tape\",\"label\":\"Tape\"},{\"level\":\"looper\",\"label\":\"Looper\"},"
      "{\"level\":\"recorder\",\"label\":\"Recorder\"},{\"level\":\"setup\",\"label\":\"Setup\"}],"
      "\"knobs\":[\"volume\",\"filter\",\"saturate\",\"warble\",\"delay_feedback\",\"delay_time\",\"reverb\",\"varspeed\"]},"
      "\"tape\":{\"label\":\"Tape\",\"params\":[{\"key\":\"reverse\"},{\"key\":\"sustain\"},{\"key\":\"auto_loop\"},"
      "{\"key\":\"attack\"},{\"key\":\"decay\"},{\"key\":\"start\"},{\"key\":\"end\"},{\"key\":\"gain\"},{\"key\":\"pan\"}],"
      "\"knobs\":[\"reverse\",\"sustain\",\"attack\",\"decay\",\"start\",\"end\",\"gain\",\"pan\"]},"
      "\"looper\":{\"label\":\"Looper\",\"params\":[{\"key\":\"looper_record\"},{\"key\":\"looper_play\"},"
      "{\"key\":\"looper_reverse\"},{\"key\":\"looper_pitch\"},{\"key\":\"looper_scrub\"},{\"key\":\"looper_dub_gain\"}],"
      "\"knobs\":[\"looper_record\",\"looper_play\",\"looper_reverse\",\"looper_pitch\",\"looper_scrub\",\"looper_dub_gain\"]},"
      "\"recorder\":{\"label\":\"Recorder\",\"params\":[{\"key\":\"record\"},{\"key\":\"input_monitor\"},"
      "{\"key\":\"input_gain\"},{\"key\":\"input_source\"}],"
      "\"knobs\":[\"record\",\"input_monitor\",\"input_gain\",\"input_source\"]},"
      "\"setup\":{\"label\":\"Setup\",\"params\":[{\"key\":\"voice_mode\"},{\"key\":\"bank\"},{\"key\":\"slot\"},"
      "{\"key\":\"octave\"}],\"knobs\":[\"voice_mode\",\"bank\",\"slot\",\"octave\"]}}}";

// ---------------------------------------------------------------- plugin API

static void* v2_create_instance(const char* module_dir, const char* json_defaults)
{
    (void)json_defaults;

    const int slot = g_instance_count.load();
    if(slot >= CHOMPI_MAX_INSTANCES)
        return nullptr;

    ChompiInstance* inst = new (std::nothrow) ChompiInstance();
    if(!inst)
        return nullptr;
    inst->host = g_host_api;

    /* We are on the loader thread here (SCHED_OTHER, cores 0-2), so this is
     * the one place allocation and file-system probing are allowed. */
    const size_t ram_samples = daisy::kMaxRamBuffSize;
    inst->loop_mem   = (int16_t*)calloc(ram_samples, sizeof(int16_t));
    inst->chompi_mem = (int16_t*)calloc(ram_samples, sizeof(int16_t));
    inst->delay_mem  = (chompi::InterpolatedDelayLine::AudioSample*)calloc(
        kMaxDelayTime, sizeof(chompi::InterpolatedDelayLine::AudioSample));

    if(!inst->loop_mem || !inst->chompi_mem || !inst->delay_mem)
    {
        free(inst->loop_mem);
        free(inst->chompi_mem);
        free(inst->delay_mem);
        delete inst;
        return nullptr;
    }

    inst->loop_buff.Init(inst->loop_mem);
    inst->chompi_buff.Init(inst->chompi_mem);
    inst->loop_buff.length   = 0;
    inst->chompi_buff.length = 0;

    for(int c = 0; c < 4; c++)
    {
        inst->in_p[c]  = inst->in_f[c];
        inst->out_p[c] = inst->out_f[c];
    }

    /* Default sample directory: <module dir>/samples. */
    if(module_dir && chompi::g_sample_dir[0] == '\0')
    {
        char dir[CHOMPI_PATH_LEN];
        snprintf(dir, sizeof(dir), "%s/samples", module_dir);
        chompi::SetSampleDir(dir);
    }

    /* The firmware's boot sequence, minus the hardware:
     *   engine.Init() -> FillDefaultSample() -> UpdateFileExists() (worker). */
    inst->engine.Init((float)MOVE_SAMPLE_RATE, &inst->reverb, inst->delay_mem,
                      &inst->loop_buff, &inst->chompi_buff,
                      false /*record latch*/, true /*tape slew*/,
                      daisy::MonitorMode::BOTH);
    /* CHOMPI's "send/return" monitor modes only make sense with its separate
     * headphone and line outs. Move has one output, so the module runs the
     * BOTH topology: the input is always in the mix and always recorded. */
    inst->engine.SetInputMonitor(true);
    inst->engine.SetInputGain(1.f);
    inst->engine.FillDefaultSample((float)MOVE_SAMPLE_RATE);
    inst->engine.SetVoiceMode(daisy::VoiceMode::JAMMI);
    inst->engine.SetBank(0);
    inst->engine.SetVoiceSlot(15, false);
    inst->engine.SetInputSource(daisy::InputSource::LINE_IN);
    inst->engine.SetGain(1.f);

    /* The per-slot CUBBI parameters (pitch, start/end, attack/decay, loop,
     * sustain, gain, pan) are plain members on the engine that the firmware's
     * UI initialises through OpenCubbiSlot() when a slot is picked. Nothing
     * else does, so a port that skips this call plays from heap garbage.
     * These are CHOMPI's defaults for an untouched slot. */
    inst->engine.OpenCubbiSlot(1.f /*pitch*/, 0.f /*start*/, 1.f /*end*/,
                               0.01f /*attack*/, 1.f /*decay*/,
                               true /*autoloop*/, true /*sustain*/,
                               1.f /*gain*/, 0.5f /*pan*/);
    inst->want_scan_files.store(true);

    g_instances[g_instance_count.fetch_add(1)] = inst;
    start_worker_if_needed();
    return inst;
}

static void v2_destroy_instance(void* instance)
{
    ChompiInstance* inst = (ChompiInstance*)instance;
    if(!inst)
        return;

    inst->stopping.store(true, std::memory_order_release);
    /* Let the worker notice before the memory goes away (it re-checks the
     * stopping flag every 500 us). */
    usleep(2000);

    const int count = g_instance_count.load();
    for(int i = 0; i < count; i++)
    {
        if(g_instances[i] == inst)
            g_instances[i] = nullptr;
    }

    free(inst->loop_mem);
    free(inst->chompi_mem);
    free(inst->delay_mem);
    delete inst;
}

static void v2_on_midi(void* instance, const uint8_t* msg, int len, int source)
{
    (void)source;
    ChompiInstance* inst = (ChompiInstance*)instance;
    if(!inst || !msg || len < 3)
        return;

    daisy::Engine& e = inst->engine;
    const uint8_t  status = msg[0] & 0xF0;
    const uint8_t  d1     = msg[1];
    const uint8_t  d2     = msg[2];

    if(status == 0x90 && d2 > 0)
    {
        /* d1 is the note, d2 the velocity — a distinction worth stating out
         * loud, because reading d2 here is exactly the bug this line had. */
        const int button = note_to_button(d1, inst->octave.load());
        if(button < 0)
            return;

        int note = d1 + 12 * inst->octave.load();
        while(note < kChompiLowNote)
            note += 12;
        while(note > kChompiHighNote)
            note -= 12;

        if(e.GetVoiceMode() == daisy::VoiceMode::CUBBI)
        {
            const size_t slot = daisy::KeyToSlot((size_t)button);
            if(slot == daisy::kSlotNone)
                return;
            /* One voice file serves the whole slot, exactly as in the firmware:
             * the UI called SetVoiceSlot() before the key request. */
            e.SetVoiceSlot(slot, false);
        }
        e.request_fifo.PushBack(daisy::KeyRequest(daisy::KeyRequest::Type::START,
                                                  (float)(note - 60), button,
                                                  (float)d2 /* velocity */));
    }
    else if(status == 0x80 || (status == 0x90 && d2 == 0))
    {
        const int button = note_to_button(d1, inst->octave.load());
        if(button < 0)
            return;
        e.request_fifo.PushBack(daisy::KeyRequest(daisy::KeyRequest::Type::STOP, 0.f,
                                                  button, 127.f));
    }
    else if(status == 0xB0)
    {
        /* Sustain pedal maps onto CHOMPI's sustain key. */
        if(d1 == 64)
            e.SetSustainActive(d2 >= 64);
    }
}

static void v2_set_param(void* instance, const char* key, const char* val)
{
    apply_param((ChompiInstance*)instance, key, val);
}

static int v2_get_param(void* instance, const char* key, char* buf, int buf_len)
{
    ChompiInstance* inst = (ChompiInstance*)instance;
    if(!inst || !key || !buf || buf_len <= 0)
        return -1;

    if(strcmp(key, "chain_params") == 0)
        return snprintf(buf, buf_len, "%s", kChainParams);
    if(strcmp(key, "ui_hierarchy") == 0)
    {
        /* A sound generator's hierarchy must be served here — the chain host
         * never reads module.json for one. pad_layout is spliced in because it
         * genuinely differs by mode: CUBBI's keys address sample slots (a pad
         * surface), JAMMI's are a chromatic keyboard. */
        const bool cubbi = (inst->engine.GetVoiceMode() == daisy::VoiceMode::CUBBI);
        const size_t prefix = (size_t)snprintf(buf, (size_t)buf_len, "{\"pad_layout\":\"%s\"",
                                              cubbi ? "drums" : "chromatic");
        if(prefix + 1 >= (size_t)buf_len)
            return -1;
        /* kUiHierarchy already opens with '{' — drop it when splicing. */
        const size_t rest = (size_t)snprintf(buf + prefix, (size_t)buf_len - prefix, ",%s",
                                             kUiHierarchy + 1);
        return (prefix + rest < (size_t)buf_len) ? (int)(prefix + rest) : -1;
    }
    if(strcmp(key, "state") == 0)
    {
        const ChompiInstance::Saved& v = inst->saved;
        const int n = snprintf(buf, (size_t)buf_len,
            "{\"volume\":%.4f,\"filter\":%.4f,\"resonance\":%.4f,\"saturate\":%.4f,"
            "\"warble\":%.4f,\"delay_time\":%.4f,\"delay_feedback\":%.4f,\"reverb\":%.4f,"
            "\"final_comp\":%.4f,\"input_gain\":%.4f,\"input_monitor\":%d,"
            "\"input_source\":\"%s\",\"monitor_mode\":\"%s\",\"octave\":%d,\"bank\":%d,"
            "\"voice_mode\":\"%s\"}",
            v.volume, v.filter, v.resonance, v.saturate, v.warble,
            v.delay_time, v.delay_feedback, v.reverb, v.final_comp, v.input_gain,
            v.input_monitor, v.input_source, v.monitor_mode, v.octave, v.bank,
            v.voice_mode);
        return (n > 0 && n < buf_len) ? n : -1;
    }
    if(strcmp(key, "split_voices") == 0)
        return snprintf(buf, buf_len, "%s", ""); /* one mixed voice for now */
    if(strcmp(key, "sample_dir") == 0)
        return snprintf(buf, buf_len, "%s", chompi::g_sample_dir);
    if(strcmp(key, "voice_mode") == 0)
        return snprintf(buf, buf_len, "%s",
                        inst->engine.GetVoiceMode() == daisy::VoiceMode::CUBBI ? "cubbi" : "jammi");
    if(strcmp(key, "looper_empty") == 0)
        return snprintf(buf, buf_len, "%d", inst->engine.GetLooperIsEmpty() ? 1 : 0);
    if(strcmp(key, "recording") == 0)
        return snprintf(buf, buf_len, "%d", inst->engine.Recording() ? 1 : 0);
    if(strcmp(key, "looper_recording") == 0)
        return snprintf(buf, buf_len, "%d", inst->engine.IsLooperRecording() ? 1 : 0);
    if(strcmp(key, "looper_playing") == 0)
        return snprintf(buf, buf_len, "%d", inst->engine.IsLooperPlaying() ? 1 : 0);
    if(strcmp(key, "debug_files") == 0)
    {
        /* Small bounded diagnostic: 15 flags for slots 1..15 in the current
         * bank/mode, plus whether the I/O queue still has work. */
        char tmp[64];
        int  off = 0;
        off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "%s/%d:",
                        inst->engine.GetVoiceMode() == daisy::VoiceMode::CUBBI ? "cubbi" : "jammi",
                        inst->engine.GetBank());
        for(int idx = 0; idx < 15; idx++)
            tmp[off++] = inst->engine.GetFileExists((size_t)idx) ? '1' : '0';
        tmp[off++] = inst->engine.IsManagerEmpty() ? '-' : '+';
        tmp[off]   = '\0';
        return snprintf(buf, (size_t)buf_len, "%s", tmp);
    }
    if(strcmp(key, "vu_in") == 0)
        return snprintf(buf, buf_len, "%.3f", inst->engine.GetVUSample(daisy::VUTarget::VU_INPUT));
    if(strcmp(key, "vu_out") == 0)
        return snprintf(buf, buf_len, "%.3f", inst->engine.GetVUSample(daisy::VUTarget::VU_OUTPUT));

    return -1;
}

static int v2_get_error(void* instance, char* buf, int buf_len)
{
    ChompiInstance* inst = (ChompiInstance*)instance;
    if(!inst || !buf || buf_len <= 0)
        return 0;
    const int n = snprintf(buf, buf_len, "%s", inst->lasterr);
    inst->lasterr[0] = '\0';
    return n;
}

static void v2_render_block(void* instance, int16_t* out_interleaved_lr, int frames)
{
    ChompiInstance* inst = (ChompiInstance*)instance;
    if(!inst || !out_interleaved_lr || frames <= 0)
        return;

    const int n = (frames < CHOMPI_BLOCK) ? frames : CHOMPI_BLOCK;

    /* Move's line input (interleaved stereo int16) lands in the mailbox. It
     * feeds CHOMPI's aux-in pair, which is what its mic/line monitoring and
     * its recorder use. Nothing here syscalls: this is a mapped pointer. */
    const int16_t* audio_in = nullptr;
    if(inst->host && inst->host->mapped_memory)
        audio_in = (const int16_t*)(inst->host->mapped_memory + inst->host->audio_in_offset);

    for(int ch = 0; ch < 4; ch++)
    {
        float* dst = inst->in_f[ch];
        if(audio_in && (ch == 2 || ch == 3))
        {
            const int off = (ch == 2) ? 0 : 1;
            for(int i = 0; i < n; i++)
                dst[i] = s162f(audio_in[2 * i + off]);
        }
        else
        {
            for(int i = 0; i < n; i++)
                dst[i] = 0.f;
        }
    }

    for(int ch = 0; ch < 4; ch++)
        memset(inst->out_f[ch], 0, sizeof(float) * (size_t)n);

    inst->engine.Prepare(); /* drains the key-request FIFO */
    inst->engine.Process((const float* const*)inst->in_p, inst->out_p, (size_t)n);

    /* CHOMPI has two stereo outs carrying the same mix at different gains:
     * out[0]/out[1] = headphone (kHpGain .2), out[2]/out[3] = line (kLineOutGain
     * .3). Move has one, so the module takes the line pair — the same signal
     * users heard on CHOMPI's main output — and leaves level to `volume`. */
    for(int i = 0; i < n; i++)
    {
        out_interleaved_lr[2 * i]     = (int16_t)f2s16(inst->out_f[2][i]);
        out_interleaved_lr[2 * i + 1] = (int16_t)f2s16(inst->out_f[3][i]);
    }
    if(frames > n)
        memset(out_interleaved_lr + 2 * n, 0, sizeof(int16_t) * 2 * (size_t)(frames - n));
}

extern "C"
{
    static plugin_api_v2_t g_plugin_v2;

    plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t* host)
    {
        g_plugin_v2.api_version     = MOVE_PLUGIN_API_VERSION_2;
        g_plugin_v2.create_instance = v2_create_instance;
        g_plugin_v2.destroy_instance = v2_destroy_instance;
        g_plugin_v2.on_midi         = v2_on_midi;
        g_plugin_v2.set_param       = v2_set_param;
        g_plugin_v2.get_param       = v2_get_param;
        g_plugin_v2.get_error       = v2_get_error;
        g_plugin_v2.render_block    = v2_render_block;

        /* Stash the host API for the mailbox (audio-in) pointer. It is only
         * read as a pointer by render_block, and only ever written here. */
        g_host_api = host;
        return &g_plugin_v2;
    }
}
