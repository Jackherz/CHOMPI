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
#include <mutex>

#include "host/plugin_api_v1.h"

#include "DSPEngine.h"

// ---------------------------------------------------------------- constants

#define CHOMPI_BLOCK MOVE_FRAMES_PER_BLOCK /* 128 */
#define CHOMPI_MAX_INSTANCES 4
/* Quiet time before the sample map is written out (see map_dirty_ms). The
 * write is coalesced, not deferred indefinitely: the worker checks every loop,
 * and destroy flushes what is still pending. */
#define CHOMPI_MAP_SETTLE_MS 500
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

    /* True while the worker is inside this instance. destroy() waits for it to
     * fall instead of sleeping and hoping: the worker dereferences the engine
     * and the sample map, and everything below it is freed before destroy
     * returns. A 2 ms sleep was a guess about how long a file operation takes,
     * and on a device with a slow card the guess is wrong -- the worker then
     * touched freed memory, and the host crashed on the next removal. */
    std::atomic<bool> worker_busy{false};

    /* Flags raised by set_param (callback) and drained by the worker. */
    std::atomic<bool> want_scan_files{false};
    /* When the slot-value store last changed and when it was last written, so
     * a knob sweep leaves one file behind instead of one per detent. */
    unsigned values_written = 0;
    unsigned values_saved   = 0;
    uint64_t values_dirty_ms = 0;

    /* The slot a samples-page edit applies to (set_param "slot"), and the
     * dirty flag that asks the worker to persist the sample map. */
    int               selected_slot = 1;
    std::atomic<bool>     map_dirty{false};
    /* When the assignment last changed. The worker waits for the map to settle
     * before writing it: the browser's live preview assigns as you JOG, so a
     * sweep through a folder would otherwise be one SD write per step. */
    std::atomic<uint64_t> map_dirty_ms{0};

    /* Octave folding window for incoming notes. */
    std::atomic<int> octave{0};

    /* Mirror of the last value applied for each savable parameter. The engine
     * exposes setters (and only some getters), so the slot autosave is answered
     * from here rather than by reaching into the DSP. Only touched from the
     * callback entry points. */
    /* The instrument's non-per-slot state. The Tape page's knobs are NOT
     * here: they are per (mode, bank, slot) and live in the engine's cache and
     * the port's slot-value store (PORT PATCH P19), so the page can read back
     * whichever slot is selected without a second copy to keep in sync. */
    struct Saved
    {
        float volume = 1.f, filter = 1.f, resonance = 0.f, saturate = 0.f;
        float warble = 0.f, delay_time = 0.f, delay_feedback = 0.f;
        float reverb = 0.f, final_comp = 0.f, input_gain = 1.f;
        float looper_pitch = 1.f, looper_scrub = 0.f, looper_dub_gain = 0.f;
        int   input_monitor = 0, octave = 0, bank = 0;
        char  input_source[8]  = "line";
        char  monitor_mode[8]  = "both";
        char  voice_mode[12]   = "jammi"; /* or "cubbi" */
        char  fx_pre_loop[8]   = "pre";   /* or "post": FX before/after the looper */
        char  looper_reverse[4] = "Off";
        char  sample_dir[CHOMPI_PATH_LEN] = {0};
    } saved;

    /* Honest, small error channel. */
    char lasterr[160] = {0};

    bool recording = false;
    bool loop_recording = false;
};

static uint64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static ChompiInstance*   g_instances[CHOMPI_MAX_INSTANCES];
static std::atomic<int>  g_instance_count{0};
/* True while chompi_io_worker() is running. The worker retires when the last
 * instance goes away (it must not outlive the library — the chain host
 * dlcloses it on a synth swap), and a later create has to be able to bring it
 * back. It used to be a never-reset one-shot latch, so the first destroy
 * silenced every instance created afterwards. */
static std::atomic<bool> g_worker_active{false};
/* The worker lives in THIS library, and the chain host's retire step is
 * destroy_instance followed immediately by dlclose (chain_synth_load.c). So
 * destroy must not return while the thread is still running: it is joined
 * there. The handle is only written by start_worker_if_needed() and only
 * joined by the destroy that takes the last instance away, both under
 * g_worker_lock -- create and destroy run on the host's management threads,
 * never on the audio callback. */
static pthread_t         g_worker_thread;
static bool              g_worker_joinable = false;
static std::mutex        g_worker_lock;
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

            /* From here to the store below, this instance is ours: destroy
             * spins on the flag before it frees anything. */
            inst->worker_busy.store(true, std::memory_order_release);

            /* CHOMPI's SDCallback: copier.CopyProcess() -> engine.ProcessFileRequests() */
            inst->engine.ProcessFileRequests();

            if(inst->want_scan_files.exchange(false))
                inst->engine.UpdateFileExists();

            /* Persist the sample map when an assignment landed. Off the
             * callback by contract: this touches the filesystem. */
            if(inst->map_dirty.load(std::memory_order_acquire) &&
               now_ms() - inst->map_dirty_ms.load(std::memory_order_acquire) >= CHOMPI_MAP_SETTLE_MS)
            {
                inst->map_dirty.store(false, std::memory_order_release);
                char map_path[CHOMPI_PATH_LEN];
                snprintf(map_path, sizeof(map_path), "%s/samples.map", chompi::g_sample_dir);
                chompi::WriteSlotOverrideMap(map_path);
            }

            /* Same idea for the per-slot values: a knob turn marks the store,
             * and one write lands once the knob has stopped moving. */
            const unsigned vw = chompi::SlotValuesWriteCount();
            if(vw != inst->values_written)
            {
                inst->values_written  = vw;
                inst->values_dirty_ms = now_ms();
            }
            else if(vw != inst->values_saved &&
                    now_ms() - inst->values_dirty_ms >= CHOMPI_MAP_SETTLE_MS)
            {
                char values_path[CHOMPI_PATH_LEN];
                snprintf(values_path, sizeof(values_path), "%s/slots.values", chompi::g_sample_dir);
                if(chompi::WriteSlotValues(values_path))
                    inst->values_saved = vw;
            }

            inst->worker_busy.store(false, std::memory_order_release);
        }
        if(!any_alive && g_instance_count.load(std::memory_order_acquire) == 0)
        {
            /* Retire — but only once nobody can be left waiting on us. A
             * create publishes its instance BEFORE asking for a worker, so if
             * one arrived in this window it saw g_worker_active still true and
             * started nothing: we have to either hand the role over or keep
             * it. Dropping the flag first makes that decision race-free. */
            g_worker_active.store(false, std::memory_order_release);
            if(g_instance_count.load(std::memory_order_acquire) == 0)
                break;                                    /* nobody arrived */
            if(!g_worker_active.exchange(true))          /* nobody took over */
                continue;                                 /* keep working */
            break;                                        /* another worker has it */
        }
        usleep(500); /* 2 kHz: CHOMPI ran its SD callback at 1 kHz */
    }
    return nullptr;
}

static void start_worker_if_needed()
{
    if(g_worker_active.exchange(true))
        return; /* already running */

    std::lock_guard<std::mutex> guard(g_worker_lock);
    pthread_t                   t;
    if(pthread_create(&t, nullptr, chompi_io_worker, nullptr) == 0)
    {
        /* Kept joinable on purpose: destroy() reaps it, and a detached thread
         * cannot be waited for at all. */
        g_worker_thread   = t;
        g_worker_joinable = true;
        return;
    }
    /* The thread never started, so the flag must not claim it did. */
    g_worker_active.store(false, std::memory_order_release);
}

/** Wait for the worker to finish, when it is this call's job to take it away.
 *
 *  Only ever called with no instances left: the worker retires by itself at
 *  that point, so this is a wait for one pass of its loop (500 us plus at most
 *  one file operation) rather than a hang. It is the difference between a
 *  plugin that can be removed from a chain and one that takes the host with
 *  it. */
static void join_worker_if_idle()
{
    std::lock_guard<std::mutex> guard(g_worker_lock);

    /* Re-checked under the lock: a create that published an instance while we
     * were freeing this one has already handed the thread new work. (The host
     * serialises load and retire, so this is belt and braces -- but joining a
     * live worker would hang the loader instead of leaking a stack.) */
    if(g_instance_count.load(std::memory_order_acquire) != 0)
        return;  /* still serving another instance */
    if(!g_worker_joinable)
        return;

    g_worker_joinable = false;
    pthread_join(g_worker_thread, nullptr);
}

// ---------------------------------------------------------------- params

/** Every parameter is applied straight to the engine: the engine slews its
 *  own targets internally (fonepole), which is what makes live knob moves
 *  click-free — the same path CHOMPI's UI used. */
static void apply_state_json(ChompiInstance* inst, const char* json);

/* Momentary enums ("-"/"Clear", "-"/"Trig") are written by the shadow UI as
 * the option NAME when it can read a current value back, and as the INDEX ("1")
 * when it cannot -- which is what happens here, because get_param answers -1
 * for an action that has no state. Accept both; only the idle option is a
 * no-op. */
static bool momentary_act(const char* val)
{
    return val && val[0] != '\0' && strcmp(val, "-") != 0 && strcmp(val, "0") != 0;
}

/* An On/Off enum write, in either form the host can send.
 *
 * The shadow UI reads the current value with get_param and then:
 *   - if that answer IS one of the param's options, it writes the option's
 *     NAME back ("On" / "Off": this is what our On/Off enums answer, so this
 *     is the form the device sends);
 *   - otherwise it writes the option's INDEX ("0" / "1").
 *
 * A numeric-only parse (strtof) reads the name form as 0, which made every one
 * of these switches one-way: writing "On" turned them OFF. That is why the
 * Loop switch "changed nothing" -- and the same bug held Sustain, Reverse,
 * Record and Loop Reverse. Accept both forms. */
static bool param_on(const char* val)
{
    if(val == nullptr || val[0] == '\0')
        return false;
    if(strcmp(val, "-") == 0 || strcmp(val, "0") == 0)
        return false;
    if(strcasecmp(val, "off") == 0 || strcasecmp(val, "false") == 0)
        return false;
    return true;
}

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
    const bool  on = param_on(val); /* On/Off enums: the host sends the name */

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
    /* Tape page. Every one of these edits the SELECTED slot's values (the
     * engine's cache AND the slot-value store), then applies them live: the
     * next key press re-applies them from the store, whichever pad it is
     * (PORT PATCH P19). A handler that only poked the live voices was undone by
     * the next press, which is what made Attack look dead next to Decay. */
    else if(strcmp(key, "gain") == 0)
        e.SetSlotGain(f);
    else if(strcmp(key, "pan") == 0)
        e.SetSlotPan(f);
    else if(strcmp(key, "attack") == 0)
        e.SetSlotAttack(f);
    else if(strcmp(key, "decay") == 0)
        e.SetSlotDecay(f);
    else if(strcmp(key, "start") == 0)
        e.SetSlotStart(f);
    else if(strcmp(key, "end") == 0)
        e.SetSlotEnd(f);
    else if(strcmp(key, "varspeed") == 0)
    {
        /* 0..1 UI value. The engine maps it onto the firmware's 0.01x..2x
         * pitch curve and keeps the reverse switch as the sign of the result,
         * so the two controls compose instead of fighting. */
        float v9[chompi::kSlotValueCount];
        bool  loop = true, sustain = true;
        e.CopySlotValues(v9, &loop, &sustain);
        e.SetSlotPitchFromUi(f, v9[chompi::kSlotPitch] < 0.f);
    }
    else if(strcmp(key, "reverse") == 0)
    {
        float v9[chompi::kSlotValueCount];
        bool  loop = true, sustain = true;
        e.CopySlotValues(v9, &loop, &sustain);
        e.SetSlotPitchFromUi(fabsf(v9[chompi::kSlotPitch]), on);
    }
    else if(strcmp(key, "sustain") == 0)
        e.SetSlotSustain(on);
    else if(strcmp(key, "auto_loop") == 0)
        e.SetSlotAutoLoop(on);
    else if(strcmp(key, "input_monitor") == 0)
        inst->saved.input_monitor = on, e.SetInputMonitor(on);
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
        /* The new mode's bind happens on the next key press (P14), and every
         * voice must take it: without this a voice that was mid-gesture keeps
         * the previous mode's file and the pads disagree with each other. */
        e.SetAllCopyOccurred();
        /* Each mode carries its own values per slot (the hardware's preset
         * store is keyed by mode too), so the Tape page has to re-read. */
        e.LoadSlotValues(e.SelectedSlot());
    }
    else if(strcmp(key, "bank") == 0)
    {
        const size_t bank = (size_t)(f < 0.f ? 0 : (f > 4.f ? 4 : (int)f));
        inst->saved.bank = (int)bank;
        e.SetBank(bank);
        /* Every slot of the new bank carries its own values: this is what makes
         * the bank knob do something you can see on the Tape page. */
        e.LoadSlotValues(e.SelectedSlot());
    }
    else if(strcmp(key, "slot") == 0)
    {
        /* SELECTION ONLY -- never SetVoiceSlot().
         *
         * This knob says which slot the Samples page is editing, and the
         * engine's SetVoiceSlot() is a different thing: it rebinds every voice
         * to that slot's file. Calling it here hijacked playback (the keys no
         * longer decided their own slot) and, worse, scrolling the knob
         * wedged the voice for good -- a burst of re-opens ends with the
         * readers bound to a file that never finished opening, and the module
         * stays silent until it is recreated. Keys pick their own slot through
         * KeyToSlot(); an assignment changes the file that slot will play.
         *
         * JAMMI is the exception that needs the slot to travel: it plays ONE
         * sample across the keyboard, so the knob is how you pick which one.
         * SelectVoiceSlot() records the choice without opening anything --
         * JAMMI's key path opens it on the next key press, once per change
         * instead of once per knob step (PORT PATCH P14/P17). */
        int slot = (int)f;
        if(slot < 1)
            slot = 1;
        if(slot > 15)
            slot = 15;
        inst->selected_slot = slot;
        e.SelectVoiceSlot((size_t)slot);
        /* The Tape page shows the SELECTED slot's values, so every step of this
         * knob re-reads them: on the hardware that is what the slot knob is
         * for, and it is why bank/slot look dead when nothing re-reads. */
        e.LoadSlotValues((size_t)slot);
    }
    else if(strcmp(key, "record") == 0)
    {
        if(on && !inst->recording)
        {
            e.StartNewRecording(0);
            inst->recording = true;
        }
        else if(!on && inst->recording)
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
        e.ResetLooperPitchQuant(), e.SetLooperPitch(on ? -1.f : 1.f);
    else if(strcmp(key, "looper_pitch") == 0)
        e.SetLooperPitch(f);
    else if(strcmp(key, "looper_scrub") == 0)
        e.SetLooperScrub(f);
    else if(strcmp(key, "looper_dub_gain") == 0)
        e.IncrementLooperDubGain(f);
    else if(strcmp(key, "loop_clear") == 0)
    {
        /* Momentary, like sample_clear: the host writes the enum when the
         * choice is committed, and we act on the non-idle value. Clears exactly
         * what the hardware's record+play gesture clears -- an in-flight
         * recording included. */
        if(momentary_act(val))
            e.ClearLooper();
    }
    else if(strcmp(key, "fx_pre_loop") == 0)
    {
        /* The hardware's FX pre/post-loop setting: does the looper take the
         * FX'd signal (pre) or does the FX run on the loop playback (post)?
         * The engine crossfades the change, so the audible routing follows one
         * fade later -- SetFxPreLooper() remembers the request, so flipping
         * twice in a row still ends on the side the user picked. */
        const bool pre = (strcmp(val, "post") != 0);
        if((strcmp(inst->saved.fx_pre_loop, "pre") == 0) != pre)
        {
            snprintf(inst->saved.fx_pre_loop, sizeof(inst->saved.fx_pre_loop), "%s",
                     pre ? "pre" : "post");
            e.SetFxPreLooper(pre);
        }
    }
    else if(strcmp(key, "sample_file") == 0)
    {
        /* What the browser hands back is a portal path ("…/chompi-samples/
         * Built-in/cubbi_a1.wav"); what the engine plays is the real file. The
         * map stores the real one — see CanonicalSamplePath() — which is also
         * what keeps the cancel case below a no-op when the browser writes back
         * the value it was given. */
        char canonical[chompi::kPortalPathMax];
        chompi::CanonicalSamplePath(val, canonical, sizeof(canonical));
        val = canonical;

        /* The shadow UI's file browser hands us an absolute path. The map is
         * keyed by the slot the user is editing (the `slot` param), so the
         * flow is: pick Slot, browse, and that slot now plays that file. An
         * empty value clears the assignment. */
        const int mode = (e.GetVoiceMode() == daisy::VoiceMode::CUBBI) ? 1 : 0;
        const int bank = e.GetBank();
        const int slot = inst->selected_slot;
        if(slot < 1 || slot > 14)
        {
            set_err(inst, "slot 15 is the recording buffer - assign to slots 1-14");
            return;
        }
        if(val[0] == '\0')
            chompi::ClearSlotOverride(mode, bank, slot);
        else
        {
            /* The browser seeds itself with the EFFECTIVE path and writes it
             * straight back on cancel, so "assign what the slot already plays by
             * convention" must not leave an override behind: it would pin a
             * relative name into the map, and later survive the shipped sample
             * being replaced. Anything else — including a copy of the same
             * sample from another directory — is a real assignment. */
            char conv[chompi::kSlotPathMax];
            chompi::SlotConventionPath(mode, bank, slot, false, conv, sizeof(conv));
            if(strcmp(conv, val) == 0)
                chompi::ClearSlotOverride(mode, bank, slot);
            else
                chompi::SetSlotOverride(mode, bank, slot, val);
        }
        inst->map_dirty_ms.store(now_ms(), std::memory_order_release);
        inst->map_dirty.store(true);
        inst->want_scan_files.store(true); /* re-scan: the slot's file changed */
        /* "This slot's file changed" — what the hardware's FileCopier signals
         * after a copy, and what makes each voice re-open on its next key press.
         * Without it, assigning a file to the slot you are already on changed
         * nothing you could hear (the voice kept the file it had), and with
         * several voices in play the pads ended up holding different files. */
        e.SetAllCopyOccurred();
    }
    else if(strcmp(key, "sample_clear") == 0)
    {
        if(!momentary_act(val))
            return;
        const int mode = (e.GetVoiceMode() == daisy::VoiceMode::CUBBI) ? 1 : 0;
        chompi::ClearSlotOverride(mode, e.GetBank(), inst->selected_slot);
        inst->map_dirty_ms.store(now_ms(), std::memory_order_release);
        inst->map_dirty.store(true);
        inst->want_scan_files.store(true);
        e.SetAllCopyOccurred(); /* back to the shipped sample on the next key */
    }
    else if(strcmp(key, "sample_dir") == 0)
    {
        /* Loader thread normally; a live change just re-points the prefix for
         * the next filename the engine builds and asks for a rescan. */
        chompi::SetSampleDir(val);
        /* …and for the browser's `Built-in` link, which follows the sample
         * directory: one entry point, always pointing at the live set. */
        chompi::EnsureSamplePortal();
        inst->want_scan_files.store(true);
    }
    else if(strcmp(key, "portal_dir") == 0)
    {
        /* Test/embedding hook: the browser root defaults to a device path that
         * does not exist on a build host. */
        chompi::SetPortalDir(val);
        chompi::EnsureSamplePortal();
    }
    else if(strcmp(key, "user_library_dir") == 0)
    {
        chompi::SetUserLibraryDir(val);
        chompi::EnsureSamplePortal();
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
        "slot", "voice_mode", "fx_pre_loop",
    };
    char val[CHOMPI_PATH_LEN];
    for(size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++)
        if(json_field(json, kKeys[i], val, sizeof(val)))
            apply_param(inst, kKeys[i], val);
    /* The selection owns the Tape page: after the mode/bank/slot above have
     * landed, the page must be showing that slot's stored values. */
    apply_param(inst, "voice_mode", inst->saved.voice_mode);
    {
        char slotbuf[16];
        snprintf(slotbuf, sizeof slotbuf, "%d", inst->selected_slot);
        apply_param(inst, "slot", slotbuf);
    }
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
      /* PORT: start/end are trim points over the slot's WAV. wav_position gives
       * them the waveform view and trim markers in the Shadow UI's edit mode
       * (docs/MODULES.md). The host draws that from the file named by
       * filepath_param, so nothing is asked of this plugin. */
      "{\"key\":\"start\",\"name\":\"Start\",\"short_name\":\"STRT\",\"type\":\"wav_position\",\"mode\":\"start\",\"filepath_param\":\"sample_file\",\"display_unit\":\"percent\",\"min\":0,\"max\":1,\"step\":0.01},"
      "{\"key\":\"end\",\"name\":\"End\",\"short_name\":\"END\",\"type\":\"wav_position\",\"mode\":\"end\",\"filepath_param\":\"sample_file\",\"display_unit\":\"percent\",\"min\":0,\"max\":1,\"step\":0.01},"
      "{\"key\":\"gain\",\"name\":\"Gain\",\"type\":\"float\",\"min\":0,\"max\":1},"
      "{\"key\":\"pan\",\"name\":\"Pan\",\"type\":\"float\",\"min\":0,\"max\":1},"
      "{\"key\":\"looper_record\",\"name\":\"Loop Rec\",\"type\":\"enum\",\"options\":[\"-\",\"Trig\"]},"
      "{\"key\":\"looper_play\",\"name\":\"Loop Play\",\"type\":\"enum\",\"options\":[\"-\",\"Trig\"]},"
      "{\"key\":\"looper_reverse\",\"name\":\"Loop Rev\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
      "{\"key\":\"looper_pitch\",\"name\":\"Loop Pitch\",\"type\":\"float\",\"min\":-2,\"max\":2},"
      "{\"key\":\"looper_scrub\",\"name\":\"Scrub\",\"type\":\"float\",\"min\":-2,\"max\":2},"
      "{\"key\":\"looper_dub_gain\",\"name\":\"Dub Gain\",\"type\":\"float\",\"min\":-1,\"max\":1},"
      /* The sample map. `root` keeps the browser on the device user data,
       * `start_path` opens on the Move's own sample library. No nested
       * objects in these entries on purpose: the chain host's C-side
       * merge walks braces and keeps only the types it models (a filepath
       * is not a knob). */
      "{\"key\":\"sample_file\",\"name\":\"Sample\",\"short_name\":\"SMPL\",\"type\":\"filepath\",\"root\":\"/data/UserData/chompi-samples\",\"start_path\":\"/data/UserData/chompi-samples\",\"filter\":[\".wav\"],\"live_preview\":true},"
      "{\"key\":\"sample_clear\",\"name\":\"Clear Slot\",\"short_name\":\"CLR\",\"type\":\"enum\",\"options\":[\"-\",\"Clear\"]},"
      "{\"key\":\"record\",\"name\":\"Chompi Rec\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
      "{\"key\":\"input_monitor\",\"name\":\"Monitor\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
      "{\"key\":\"input_gain\",\"name\":\"In Gain\",\"type\":\"float\",\"min\":0,\"max\":2},"
      "{\"key\":\"input_source\",\"name\":\"Input\",\"type\":\"enum\",\"options\":[\"line\",\"mic\"]},"
      "{\"key\":\"voice_mode\",\"name\":\"Mode\",\"type\":\"enum\",\"options\":[\"jammi\",\"cubbi\"]},"
      "{\"key\":\"bank\",\"name\":\"Bank\",\"type\":\"int\",\"min\":0,\"max\":4},"
      "{\"key\":\"slot\",\"name\":\"Slot\",\"type\":\"int\",\"min\":1,\"max\":15},"
      "{\"key\":\"octave\",\"name\":\"Octave\",\"type\":\"int\",\"min\":-2,\"max\":2},{\"key\":\"loop_clear\",\"name\":\"Clear Loop\",\"type\":\"enum\",\"options\":[\"-\",\"Clear\"]},{\"key\":\"fx_pre_loop\",\"name\":\"FX Route\",\"type\":\"enum\",\"options\":[\"pre\",\"post\"]}]";

static const char* kUiHierarchy
    = "{\"levels\":{\"root\":{\"label\":\"CHOMPI\",\"params\":["
      "{\"key\":\"volume\"},{\"key\":\"filter\"},{\"key\":\"saturate\"},{\"key\":\"warble\"},"
      "{\"key\":\"delay_feedback\"},{\"key\":\"delay_time\"},{\"key\":\"reverb\"},{\"key\":\"varspeed\"},"
      "{\"level\":\"samples\",\"label\":\"Samples\"},{\"level\":\"tape\",\"label\":\"Tape\"},{\"level\":\"looper\",\"label\":\"Looper\"},"
      "{\"level\":\"recorder\",\"label\":\"Recorder\"},{\"level\":\"setup\",\"label\":\"Setup\"}],"
      "\"knobs\":[\"volume\",\"filter\",\"saturate\",\"warble\",\"delay_feedback\",\"delay_time\",\"reverb\",\"varspeed\"]},"
      "\"tape\":{\"label\":\"Tape\",\"params\":[{\"key\":\"reverse\"},{\"key\":\"sustain\"},{\"key\":\"auto_loop\"},"
      "{\"key\":\"attack\"},{\"key\":\"decay\"},{\"key\":\"start\"},{\"key\":\"end\"},{\"key\":\"gain\"},{\"key\":\"pan\"}],"
      "\"knobs\":[\"reverse\",\"sustain\",\"attack\",\"decay\",\"start\",\"end\",\"gain\",\"pan\"]},"
      "\"looper\":{\"label\":\"Looper\",\"params\":[{\"key\":\"looper_record\"},{\"key\":\"looper_play\"},"
      "{\"key\":\"looper_reverse\"},{\"key\":\"looper_pitch\"},{\"key\":\"looper_scrub\"},{\"key\":\"looper_dub_gain\"},{\"key\":\"loop_clear\"}],"
      "\"knobs\":[\"looper_record\",\"looper_play\",\"looper_reverse\",\"looper_pitch\",\"looper_scrub\",\"looper_dub_gain\"]},"
      "\"recorder\":{\"label\":\"Recorder\",\"params\":[{\"key\":\"record\"},{\"key\":\"input_monitor\"},"
      "{\"key\":\"input_gain\"},{\"key\":\"input_source\"}],"
      "\"knobs\":[\"record\",\"input_monitor\",\"input_gain\",\"input_source\"]},"
      "\"setup\":{\"label\":\"Setup\",\"params\":[{\"key\":\"voice_mode\"},{\"key\":\"bank\"},{\"key\":\"slot\"},"
      "{\"key\":\"octave\"},{\"key\":\"fx_pre_loop\"}],\"knobs\":[\"voice_mode\",\"bank\",\"slot\",\"octave\"]},\"samples\":{\"label\":\"Samples\",\"params\":[{\"key\":\"slot\",\"name\":\"Slot\",\"short_name\":\"SLOT\",\"type\":\"int\",\"min\":1,\"max\":15},{\"key\":\"sample_file\",\"name\":\"Sample\",\"short_name\":\"SMPL\",\"type\":\"filepath\",\"root\":\"/data/UserData/chompi-samples\",\"start_path\":\"/data/UserData/chompi-samples\",\"filter\":[\".wav\"],\"live_preview\":true},{\"key\":\"sample_clear\",\"name\":\"Clear Slot\",\"short_name\":\"CLR\",\"type\":\"enum\",\"options\":[\"-\",\"Clear\"]}],\"knobs\":[\"slot\"]}}}";

// ---------------------------------------------------------------- plugin API

static void  v2_destroy_instance(void* instance); /* used by create on a full registry */

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
    /* Slot 1, not 15. Slot 15 is the recording buffer, whose contents are the
     * decaying test tone FillDefaultSample() writes -- with 15 selected, a
     * fresh module could only ever play that one sound, in every mode and bank,
     * until the user found the Samples page. Slot 1 is the first sound of the
     * selected mode's bank, so the mode/bank/slot knobs do something audible
     * from the start (see PORT PATCH P14 for JAMMI's bind). */
    inst->engine.SetVoiceSlot(1, false);
    inst->engine.SetInputSource(daisy::InputSource::LINE_IN);
    inst->engine.SetGain(1.f);

    /* The per-slot values (pitch, start/end, attack/decay, loop, sustain, gain,
     * pan) are read from the store on every selection change and every key
     * press, but the cache must not start on heap garbage — nothing else fills
     * it before the first key. The store's defaults for an untouched slot are
     * the hardware's (ui.h enc_defaults): 1x pitch, start 0, end 1, attack 0,
     * decay 0, loop on, sustain on, unity gain (.704 on the encoder), pan
     * centre. OpenCubbiSlot() writes the cache directly; LoadSlotValues() below
     * then puts the selected slot's stored values in it. */
    inst->engine.OpenCubbiSlot(1.f /*pitch ratio*/, 0.f /*start*/, 1.f /*end*/,
                               0.f /*attack*/, 0.f /*decay*/,
                               true /*autoloop*/, true /*sustain*/,
                               0.704f /*gain*/, 0.5f /*pan*/);
    /* The browser's root: a directory with symlinks to the shipped card set
     * and to the user's library, so one place shows both and nothing outside it
     * is reachable. Created here rather than assumed, because a module installed
     * from a store tarball never runs a deploy script (PORT PATCH P18). */
    chompi::EnsureSamplePortal();

    /* Restore the user's sample assignments before the first scan, so slots
     * they pointed at a library WAV read as present from the start. */
    {
        char map_path[CHOMPI_PATH_LEN];
        snprintf(map_path, sizeof(map_path), "%s/samples.map", chompi::g_sample_dir);
        chompi::ReadSlotOverrideMap(map_path);
    }

    /* The per-slot values the user has tuned, per (mode, bank, slot) — the
     * Tape page's knobs. Read here for the same reason: the first key press
     * must already use them. */
    {
        char values_path[CHOMPI_PATH_LEN];
        snprintf(values_path, sizeof(values_path), "%s/slots.values", chompi::g_sample_dir);
        chompi::ReadSlotValues(values_path);
    }
    inst->values_written = inst->values_saved = chompi::SlotValuesWriteCount();

    /* What the selected slot holds, as the engine's cache: the page reads it,
     * and the first key press applies it. */
    inst->engine.LoadSlotValues(inst->engine.SelectedSlot());

    inst->want_scan_files.store(true);

    /* Publish the slot, THEN the count: the worker walks g_instances[0..count)
     * and treats a null slot as "nothing here", so a count that ran ahead of
     * the pointer would let it retire on a roster it cannot see. */
    const int idx = g_instance_count.load(std::memory_order_acquire);
    if(idx >= CHOMPI_MAX_INSTANCES)
    {
        /* Refuse rather than overrun the registry. */
        set_err(inst, "no free instance slot");
        v2_destroy_instance(inst);
        return nullptr;
    }
    g_instances[idx] = inst;
    g_instance_count.store(idx + 1, std::memory_order_release);

    start_worker_if_needed();
    return inst;
}

static void v2_destroy_instance(void* instance)
{
    ChompiInstance* inst = (ChompiInstance*)instance;
    if(!inst)
        return;

    /* Find it in the roster FIRST. An instance that is not ours -- a double
     * removal, or a pointer this module never handed out -- must not be freed
     * on the strength of the argument alone. */
    const int count   = g_instance_count.load(std::memory_order_acquire);
    int       removed = -1;
    for(int i = 0; i < count && removed < 0; i++)
    {
        if(g_instances[i] == inst)
            removed = i;
    }
    if(removed < 0)
        return; /* not ours: never free a pointer on the strength of the argument alone */

    /* Stop the worker, then WAIT for it to leave this instance rather than
     * sleeping and hoping: it dereferences the engine and the sample map, and
     * everything below frees them. See worker_busy for the bug this replaced
     * (a device with a slow card removing the plugin -> host SIGSEGV). */
    inst->stopping.store(true, std::memory_order_release);
    while(inst->worker_busy.load(std::memory_order_acquire))
        usleep(100);

    /* A settle window may still be open (assign, then unload within 500 ms);
     * destroy is the last chance to keep that assignment, and with the worker
     * out of the way this is the only writer. This thread is not the render
     * callback, so the write is allowed. */
    if(inst->map_dirty.exchange(false))
    {
        char map_path[CHOMPI_PATH_LEN];
        snprintf(map_path, sizeof(map_path), "%s/samples.map", chompi::g_sample_dir);
        chompi::WriteSlotOverrideMap(map_path);
    }

    /* Unregister and shrink the count. The count used to only ever grow, so a
     * few synth swaps walked g_instances[] off the end of the array; and with
     * the slot nulled but the count unchanged, the worker read an empty roster
     * and retired. Compact by moving the last live slot down: the worker only
     * dereferences slots it saw non-null, and every slot it can still see is
     * live. */
    {
        const int last = count - 1;
        g_instances[removed] = (removed == last) ? nullptr : g_instances[last];
        g_instances[last]    = nullptr;
        g_instance_count.store(last, std::memory_order_release);
    }

    free(inst->loop_mem);
    free(inst->chompi_mem);
    free(inst->delay_mem);
    delete inst;

    /* LAST, and the reason this function exists in the shape it does: the host
     * dlcloses this library as soon as we return, so no thread of ours may
     * still be inside it. The worker retires on its own once the last instance
     * is gone; reaping it here is what makes removing the plugin from a chain
     * safe. */
    join_worker_if_idle();
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
            /* CUBBI's keys ARE its pads: each one plays the sample in its own
             * slot, and KeyToSlot() is the map. A key that is not a pad (the
             * transport buttons on CHOMPI's panel) has no slot and is silent in
             * this mode, exactly as on the hardware.
             *
             * The slot's own values are loaded by the engine's key path, one
             * press at a time (PORT PATCH P19).
             *
             * PORT PATCH P20: this used to call SetVoiceSlot(slot, false) --
             * which binds that file to EVERY voice -- on every press. A press
             * therefore re-pointed voices that were mid-note at a file that had
             * not finished opening, and a pad whose slot had no file took the
             * whole keybed silent. That is the reported "pads start playing
             * different samples" and "sometimes it plays white noise instead of
             * the sample". One voice, opened once, is what the engine already
             * does by itself. */
            if(daisy::KeyToSlot((size_t)button) == daisy::kSlotNone)
                return;
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
        /* The Tape page's values are NOT in here: they are per (mode, bank,
         * slot) and live in the module's own store (slots.values, next to
         * samples.map), which the host has no way to keep per Set. What the
         * state has to carry is which slot is selected, so restoring a Set puts
         * the page back on the same slot it was saved on. */
        const ChompiInstance::Saved& v = inst->saved;
        const int n = snprintf(buf, (size_t)buf_len,
            "{\"volume\":%.4f,\"filter\":%.4f,\"resonance\":%.4f,\"saturate\":%.4f,"
            "\"warble\":%.4f,\"delay_time\":%.4f,\"delay_feedback\":%.4f,\"reverb\":%.4f,"
            "\"final_comp\":%.4f,\"input_gain\":%.4f,\"input_monitor\":%d,"
            "\"input_source\":\"%s\",\"monitor_mode\":\"%s\",\"octave\":%d,\"bank\":%d,"
            "\"slot\":%d,\"voice_mode\":\"%s\",\"fx_pre_loop\":\"%s\"}",
            v.volume, v.filter, v.resonance, v.saturate, v.warble,
            v.delay_time, v.delay_feedback, v.reverb, v.final_comp, v.input_gain,
            v.input_monitor, v.input_source, v.monitor_mode, v.octave, v.bank,
            inst->selected_slot, v.voice_mode, v.fx_pre_loop);
        return (n > 0 && n < buf_len) ? n : -1;
    }

    /* Root page. */
    if(strcmp(key, "volume") == 0)
        return snprintf(buf, buf_len, "%.4f", inst->saved.volume);
    if(strcmp(key, "filter") == 0)
        return snprintf(buf, buf_len, "%.4f", inst->saved.filter);
    if(strcmp(key, "resonance") == 0)
        return snprintf(buf, buf_len, "%.4f", inst->saved.resonance);
    if(strcmp(key, "saturate") == 0)
        return snprintf(buf, buf_len, "%.4f", inst->saved.saturate);
    if(strcmp(key, "warble") == 0)
        return snprintf(buf, buf_len, "%.4f", inst->saved.warble);
    if(strcmp(key, "delay_time") == 0)
        return snprintf(buf, buf_len, "%.4f", inst->saved.delay_time);
    if(strcmp(key, "delay_feedback") == 0)
        return snprintf(buf, buf_len, "%.4f", inst->saved.delay_feedback);
    if(strcmp(key, "reverb") == 0)
        return snprintf(buf, buf_len, "%.4f", inst->saved.reverb);
    if(strcmp(key, "final_comp") == 0)
        return snprintf(buf, buf_len, "%.4f", inst->saved.final_comp);

    /* Tape page. These answer with the SELECTED slot's values -- the ones the
     * next key press will use -- read straight out of the engine's cache, so a
     * bank or slot change is reflected here immediately and there is no second
     * copy to fall out of step with it. */
    if(strcmp(key, "attack") == 0 || strcmp(key, "decay") == 0 || strcmp(key, "gain") == 0
       || strcmp(key, "pan") == 0 || strcmp(key, "start") == 0 || strcmp(key, "end") == 0
       || strcmp(key, "varspeed") == 0 || strcmp(key, "reverse") == 0
       || strcmp(key, "sustain") == 0 || strcmp(key, "auto_loop") == 0)
    {
        float v9[chompi::kSlotValueCount];
        bool  loop = true, sustain = true;
        inst->engine.CopySlotValues(v9, &loop, &sustain);

        if(strcmp(key, "attack") == 0)
            return snprintf(buf, buf_len, "%.4f", v9[chompi::kSlotAttack]);
        if(strcmp(key, "decay") == 0)
            return snprintf(buf, buf_len, "%.4f", v9[chompi::kSlotDecay]);
        if(strcmp(key, "gain") == 0)
            return snprintf(buf, buf_len, "%.4f", v9[chompi::kSlotGain]);
        if(strcmp(key, "pan") == 0)
            return snprintf(buf, buf_len, "%.4f", v9[chompi::kSlotPan]);
        if(strcmp(key, "start") == 0)
            return snprintf(buf, buf_len, "%.4f", v9[chompi::kSlotStart]);
        if(strcmp(key, "end") == 0)
            return snprintf(buf, buf_len, "%.4f", v9[chompi::kSlotEnd]);
        if(strcmp(key, "varspeed") == 0)
            /* The knob is the pitch encoder's magnitude; the sign is Reverse. */
            return snprintf(buf, buf_len, "%.4f", fabsf(v9[chompi::kSlotPitch]));
        if(strcmp(key, "reverse") == 0)
            return snprintf(buf, buf_len, "%s", v9[chompi::kSlotPitch] < 0.f ? "On" : "Off");
        if(strcmp(key, "sustain") == 0)
            return snprintf(buf, buf_len, "%s", sustain ? "On" : "Off");
        return snprintf(buf, buf_len, "%s", loop ? "On" : "Off");
    }

    /* Looper page. */
    if(strcmp(key, "looper_reverse") == 0)
        return snprintf(buf, buf_len, "%s", inst->saved.looper_reverse);
    if(strcmp(key, "looper_pitch") == 0)
        return snprintf(buf, buf_len, "%.4f", inst->saved.looper_pitch);
    if(strcmp(key, "looper_scrub") == 0)
        return snprintf(buf, buf_len, "%.4f", inst->saved.looper_scrub);
    if(strcmp(key, "looper_dub_gain") == 0)
        return snprintf(buf, buf_len, "%.4f", inst->saved.looper_dub_gain);
    /* Momentary actions have no state to report, and the idle option is the
     * honest answer: the host then reads "not triggered" and writes the other
     * option when the user picks it. */
    if(strcmp(key, "looper_record") == 0 || strcmp(key, "looper_play") == 0
       || strcmp(key, "sample_clear") == 0 || strcmp(key, "loop_clear") == 0)
        return snprintf(buf, buf_len, "%s", "-");

    /* Recorder page. */
    if(strcmp(key, "record") == 0)
        return snprintf(buf, buf_len, "%s", inst->recording ? "On" : "Off");
    if(strcmp(key, "input_monitor") == 0)
        /* Option NAME, like every other enum here: the host matches this string
         * against the parameter's `options`, and a bare number is read as an
         * index into the list it happens to be holding (see param_on()). */
        return snprintf(buf, buf_len, "%s", inst->saved.input_monitor ? "On" : "Off");
    if(strcmp(key, "input_gain") == 0)
        return snprintf(buf, buf_len, "%.4f", inst->saved.input_gain);
    if(strcmp(key, "input_source") == 0)
        return snprintf(buf, buf_len, "%s", inst->saved.input_source);

    /* Setup page. */
    if(strcmp(key, "bank") == 0)
        return snprintf(buf, buf_len, "%d", inst->engine.GetBank());
    if(strcmp(key, "slot") == 0)
        return snprintf(buf, buf_len, "%d", inst->selected_slot);
    if(strcmp(key, "octave") == 0)
        return snprintf(buf, buf_len, "%d", inst->octave.load());

    if(strcmp(key, "split_voices") == 0)
        return snprintf(buf, buf_len, "%s", ""); /* one mixed voice for now */
    if(strcmp(key, "sample_dir") == 0)
        return snprintf(buf, buf_len, "%s", chompi::g_sample_dir);
    if(strcmp(key, "sample_file") == 0)
    {
        /* What the selected slot plays right now — the assigned sample if
         * there is one, otherwise the convention name. The host reads this
         * back to seed the browser (and to draw the wav_position waveform),
         * so it must be the EFFECTIVE path, not the assignment. */
        const int mode = (inst->engine.GetVoiceMode() == daisy::VoiceMode::CUBBI) ? 1 : 0;
        char      path[chompi::kSlotPathMax];
        chompi::SlotSamplePath(mode, inst->engine.GetBank(), inst->selected_slot,
                               false, path, sizeof(path));
        /* …and through the portal, so the browser opens on the file's own
         * folder inside its root, and the cell reads "Built-in/…" instead of the
         * whole install path. */
        char presented[chompi::kPortalPathMax];
        chompi::PresentSamplePath(path, presented, sizeof(presented));
        return snprintf(buf, buf_len, "%s", presented);
    }
    /* --- the parameter pages, read back -----------------------------------
     *
     * Every knob on every page answers with the value it is actually set to.
     * The host draws a knob's position and its "edited" marker from whatever
     * this returns, so a key that is missing (they all were, except the root
     * page and the samples page) shows up as 0 — which is exactly the report:
     * "Volume is set at 0 when you first start the module but is actually at
     * full". The values live in the mirror (ChompiInstance::Saved), which is
     * also what `state` saves, so the two cannot disagree.
     *
     * Enums answer with the option NAME: the host matches the string against
     * the parameter's `options` to choose the displayed entry, and a bare "1"
     * would be read as an index into someone else's list. */

    /* Where the file browser starts, and the fence around it (PORT PATCH P18).
     * `sample_root` is the read-only answer; `portal_dir` moves it. */
    if(strcmp(key, "sample_root") == 0)
        return snprintf(buf, buf_len, "%s",
                        chompi::g_portal_dir[0] ? chompi::g_portal_dir : "(unset)");
    if(strcmp(key, "debug_portal") == 0)
        return chompi::DescribePortal(buf, (size_t)buf_len);
    if(strcmp(key, "sample_map") == 0)
    {
        /* Diagnostic: the assignments for the current mode/bank, as
         * "slot=path;" pairs. Empty when everything uses convention names. */
        const int mode  = (inst->engine.GetVoiceMode() == daisy::VoiceMode::CUBBI) ? 1 : 0;
        const int bank  = inst->engine.GetBank();
        int       off   = 0;
        for(int slot = 1; slot <= 14 && off < (int)buf_len - 1; slot++)
        {
            if(!chompi::SlotHasOverride(mode, bank, slot))
                continue;
            char path[chompi::kSlotPathMax];
            chompi::SlotSamplePath(mode, bank, slot, false, path, sizeof(path));
            const int n = snprintf(buf + off, buf_len - (size_t)off, "%d=%s;", slot, path);
            if(n <= 0 || n >= (int)(buf_len - (size_t)off))
                break;
            off += n;
        }
        return off;
    }
    if(strcmp(key, "debug_voice") == 0)
    {
        /* See Engine::DescribeVoices. Serve it as one string: the readings
         * that decide whether a voice can sound at all. */
        return inst->engine.DescribeVoices(buf, buf_len);
    }
    if(strcmp(key, "sample_slot") == 0)
        return snprintf(buf, buf_len, "%d", inst->selected_slot);
    if(strcmp(key, "voice_mode") == 0)
        return snprintf(buf, buf_len, "%s",
                        inst->engine.GetVoiceMode() == daisy::VoiceMode::CUBBI ? "cubbi" : "jammi");
    if(strcmp(key, "fx_pre_loop") == 0)
        /* What the user asked for. The engine's effective routing is in
         * debug_voice's summary line while a crossfade is in flight. */
        return snprintf(buf, buf_len, "%s", inst->saved.fx_pre_loop);
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
