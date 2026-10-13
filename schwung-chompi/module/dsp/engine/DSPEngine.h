/** DSPEngine
 *  Core DSP for sampling engine, looping engine, and additional DSP
 */
#pragma once
#include "daisy.h"
#include "chompi_port.h" // PORT PATCH P4: sample-directory prefixing on Linux
#include "FileStreamingManager.h"
#include "SampleReader.h"
#include "LooperEngine.h"
#include "daisysp.h"
#include "DJFilter.h"
#include "Warble.h"
// PORT PATCH P1: PresetManager (FatFS + coreJSON) intentionally not ported; the engine never uses it.
#include "EnvFollower.h"
#include "MicFilter.h"
#include "reverb.h"
#include "RamBuffer.h"
#include "limiter.h" // PORT PATCH P2: case-sensitive on Linux
#include "InterpolatedDelayLine.h"
#include <algorithm>

using namespace daisy;
static constexpr size_t kMaxDelayTime = 48128 * 2; // stereo, > 1 seconds at 48kHz

static constexpr float kLineOutGain = .3f;
static constexpr float kHpGain = .2f;
static constexpr float kMicGain = 5.f;
static constexpr float kLineInGain = 3.f;
static constexpr size_t kMaxPoly = 7;

namespace daisy
{
    enum class VUTarget
    {
        VU_INPUT = 0,
        VU_OUTPUT,
        VU_LAST
    };

    enum class InputSource
    {
        MIC = 0,
        LINE_IN,
        RESAMPLE,
        LAST,
    };

    enum class MonitorMode
    {
        HP = 0,
        BOTH,
        SEND_RET,
        LAST,
    };

    enum class VoiceMode
    {
        JAMMI,
        CUBBI,
        LAST,
    };

    struct KeyRequest
    {
        enum class Type
        {
            START,
            STOP,
            DUMMY,
        };

        Type type_;
        float transpose_nn_;
        int key_;
        float vel_;
        /** PORT PATCH P22: the slot this press plays, when the caller knows it
         *  better than KeyToSlot(key) does. 0 = "ask KeyToSlot()". */
        int slot_hint_;

        /** constructor for full request data */
        KeyRequest(Type type,
                    float transpose_nn,
                    int key,
                    float vel,
                    int slot_hint = 0
                  )
            : type_(type),
                transpose_nn_(transpose_nn),
                key_(key),
                vel_(vel),
                slot_hint_(slot_hint)
        {
        }

        /** Empty, invalid request */
        KeyRequest()
            : type_(Type::DUMMY),
                transpose_nn_(0.f),
                key_(0),
                vel_(127.f),
                slot_hint_(0)
        {
        }


    };

    // This shouldn't live here, but it's visible where it's needed, so...
    static const uint8_t kSlotNone = 100;
    static size_t const KeyToSlot(size_t buttonID)
    {
        if (buttonID == 7)
            return kSlotNone;
        else if (buttonID < 12)
            return buttonID - 6;
        else if (buttonID < 15)
            return kSlotNone;
        else if (buttonID == 15)
            return 1;
        else if (buttonID < 21)
            return buttonID - 10;
        else if (buttonID < 24)
            return kSlotNone;
        else if (buttonID < 29)
            return buttonID - 13;

        return kSlotNone;
    }


    /** @brief core engine for running entire modules audio
     *
     *  For now now abstraction for various sampling modes,
     *  possibly that can all be handled in the UI.
     *
     *  This will build out CHOMPI Mode and add the looping engine, and other fixed engines
     */
    class Engine
    {
    public:
        Engine() {}
        ~Engine() {}

        FileStreamingManager* GetFileManager()
        {
            return &file_manager;
        }

        void Init(
            float samplerate, 
            daisysp::Reverb* reverb, 
            chompi::InterpolatedDelayLine::AudioSample* del,
            RamBufferMemory* loop_buff,
            RamBufferMemory* chompi_buff,
            bool latch,
            bool tape_slew,
            MonitorMode mon_mode)
        {
            voice_mode = VoiceMode::JAMMI;
            latest_voice = 0;
            monitor_mode = mon_mode;

            input_env_follower.Init();
            output_env_follower.Init();

            for (size_t i = 0; i < size_t(VoiceMode::LAST); i++)
                bank[i] = 0;

            reverb_ = reverb;

            reverb_->Init(samplerate);
            reverb_->SetAmount(0.f);
            reverb_->SetInputGain(.3f);
            reverb_->SetLowpass(1.f);

            del_.Init(del, kMaxDelayTime);
            del_.SetDelay(kMaxDelayTime * .5f);

            SetDelayTime(.5f);
            dly_time_ = dly_time_target_;

            mic_filter_.Init(samplerate);

            filter_.Init(samplerate);
            filter_.SetControl(.5f);
            cutoff_target_ = .5f;
            res_ = res_target_ = 0.f;

            SetSaturate(0.f);

            mgain_ = mgain_target_ = .8f;

            warble_.Init(samplerate);
            warble_.SetFreq(.1f);
            
            dcblock_mic_in_.Init(samplerate);
            dcblock_line_in_l_.Init(samplerate);
            dcblock_line_in_r_.Init(samplerate);
            dcblock_fx_l_.Init(samplerate);
            dcblock_fx_r_.Init(samplerate);

            file_manager.Init(samplerate);

            fx_pre_loop = true;
            /* PORT PATCH P15: the routing the user asked for, which the
             * crossfade below lands on. */
            fx_pre_loop_target = true;
            fx_env_ = fx_env_target_ = 1.f;
            final_lim_ = 0.f;

            resamp_env_ = resamp_env_target_ = 1.f;

            /* voices */
            for (size_t i = 0; i < kMaxPoly; i++)
            {
                chompi_voice[i].Init(file_manager, samplerate, chompi_buff);
            }
            chompi_writer.Init(chompi_buff);
            record = false;
            record_latch = latch;

            SetVoiceMode(VoiceMode::JAMMI);
            SetBank(0);
            SetVoiceSlot(15, true);

            /* looper */
            looper.Init(samplerate, loop_buff, tape_slew);

            /** Final output compressors */
            lim_hp_l_.Init();
            lim_hp_r_.Init();
            lim_line_l_.Init();
            lim_line_r_.Init();
        }

        // fill chompi buffer with 2 second long cosine
        void FillDefaultSample(float samplerate)
        {
            float rads_l = 1.f;
            float rads_r = 0.f;
            float detune = 1.01f;
            float inc_r = .5f * 523.2511f * (TWOPI_F / samplerate) ; // hz -> increment in rads
            float inc_l = .5f * 523.2511f * detune * (TWOPI_F / samplerate) ;
            float envelope;

            for(size_t i = 0; i < 3 * samplerate; i++)
            {
                if (i > 24000) {
                    float decay_rate = 0.00007195578f;
                    envelope = expf(-decay_rate * (i - 24000));
                }
                else {
                    envelope = 1.f;
                }
                
                const size_t end = (4 * samplerate) - 2000;
                float gain = 1.f;
                if(i < 2000)
                    gain = i / 2000.f;
                else if(i > end)
                    gain = 1.f - (i - end) / 2000.f;

                rads_l += inc_l;
                if(rads_l >= TWOPI_F)
                    rads_l -= TWOPI_F;

                rads_r += inc_r;
                if(rads_r >= TWOPI_F)
                    rads_r -= TWOPI_F;

                float tri_l = 2.0f * fabsf(rads_l / TWOPI_F - 0.5f) - 1.0f;
                float tri_r = 2.0f * fabsf(rads_r / TWOPI_F - 0.5f) - 1.0f;
                // sum and output
                const int16_t sum_l = f2s16(gain * .4f * tri_l * envelope);
                const int16_t sum_r = f2s16(gain * .4f * tri_r * envelope);
                chompi_writer.StereoWrite(sum_l, sum_r, false, true);
            }
        }

        void ApplyFx(float* outl, float* outr, size_t size)
        {
            for(size_t i = 0; i < size; i++)
            {
                // slew controls at audio rate
                fonepole(cutoff_, cutoff_target_, .001f);
                fonepole(res_, res_target_, .001f);
                fonepole(saturate_amt_, saturate_amt_target_, .001f);

                // input gain
                fonepole(fx_env_, fx_env_target_, .001f);
                outl[i] *= fx_env_;
                outr[i] *= fx_env_;

                filter_.SetControl(cutoff_);
                filter_.SetRes(res_);

                outl[i] = dcblock_fx_l_.Process(outl[i]);
                outr[i] = dcblock_fx_r_.Process(outr[i]);

                // filter
                filter_.Process(outl[i], outr[i], &outl[i], &outr[i]);

                // then clip
                outl[i] = daisysp::SoftClip(saturate_amt_ * outl[i]); 
                outr[i] = daisysp::SoftClip(saturate_amt_ * outr[i]); 

                // reduce amplitude to prevent LUFs from blowing up
                const float gain = 1.f - daisysp::SoftClip(.4f * (saturate_amt_ - 1.f)) * .7f;
                outl[i] *= gain;
                outr[i] *= gain;

                // wow and flutter
                warble_.Process(outl[i], outr[i], &outl[i], &outr[i]);
            }

            // delay, in its own block to optimize SDRAM access
            for(size_t i = 0; i < size; i++)
            {
                // fonepole and set controls
                // dly_amt_ is a duplicate of reverb_amt_, but we want to fonepole it in this loop
                fonepole(dly_feedback_, dly_feedback_target_, .001f);
                fonepole(dly_time_, dly_time_target_, .001f);
                fonepole(dly_amt_, dly_amt_target_, .001f); 
                del_.SetDelay(dly_time_); 

                // read
                const float del_vol = dly_feedback_ < .2f ? dly_feedback_ * 5.f : 1.f;
                InterpolatedDelayLine::AudioSample del_read = del_.Read();
                const float delsig_l = s162f(del_read.l) * del_vol;
                const float delsig_r = s162f(del_read.r) * del_vol;

                // write
                const float mono_sum = (outl[i] + outr[i]) * .5f;
                const float del_in = mono_sum + delsig_r * powf(dly_feedback_, .7f);
                const InterpolatedDelayLine::AudioSample del_write = {int16_t(f2s16(del_in)), int16_t(f2s16(delsig_l))};
                del_.Write(del_write);

                // dry/wet mix
                const float wet_mix = dly_feedback_ > .25f ? .5f : 2.f * dly_feedback_; // quickly to 50%
                const float dry_mix = dly_feedback_ > .83f ? .5f : (1 - .6f * dly_feedback_); // slowly to 50%

                outl[i] = outl[i] * dry_mix + delsig_l * wet_mix;
                outr[i] = outr[i] * dry_mix + delsig_r * wet_mix;
            }

            // reverb, in its own block to optimize SDRAM access
            for(size_t i = 0; i < size; i++)
            {
                // fonepole and set controls
                fonepole(reverb_amt_, reverb_amt_target_, .001f);
                fonepole(reverb_time_, reverb_time_target_, .001f);
                reverb_->SetAmount(reverb_amt_ * reverb_amt_ * .8f);
                reverb_->SetTime(reverb_time_);
                reverb_->SetLowpass(reverb_amt_ * .6f + .4f);
                reverb_->SetDiffusion(reverb_amt_ * .6f);

                reverb_->Process(&outl[i], &outr[i]);
            }
        }

        /** Apply the total monitor signal to the input envelope follower */
        void ApplyEnvelopeFollower(size_t size, float* monitor)
        {
            for (size_t i = 0; i < size; i++)
                input_env_follower.Process((monitor[i] + monitor[size + i]) * .8f);
        }

        /** add the mic monitor into both outputs, 
            also add it to the monitor buffer, which will be used to feed the env follower at the end
        */
        void ApplyMicMonitor(const float* const* in, float **out, size_t size, float* monitor)
        {
            for (size_t i = 0; i < size; i++)
            {
                float sig = dcblock_mic_in_.Process(in[0][i] * ingain_ * kMicGain);
                sig = mic_filter_.Process(sig);

                monitor[i] += sig;
                monitor[size + i] += sig;

                out[0][i] += sig;
                out[1][i] += sig;
            }
        }

        /** add the line in monitor into both outputs, 
            also add it to the monitor buffer, which will be used to feed the env follower at the end
        */
        void ApplyLineMonitor(const float* const* in, float **out, size_t size, float* monitor)
        {
            for (size_t i = 0; i < size; i++)
            {
                const float sigl = dcblock_line_in_l_.Process(in[2][i] * ingain_ * kLineInGain);
                const float sigr = dcblock_line_in_r_.Process(in[3][i] * ingain_ * kLineInGain);

                monitor[i] += sigl;
                monitor[size + i] += sigr;

                out[0][i] += sigl;
                out[1][i] += sigr;
            }
        }

        bool looper_reset = false;
        bool CheckReset()
        {
            if(looper_reset)
            {
                looper_reset = false;
                return true;
            }

            return false;
        }

        float old_fx_outl, old_fx_outr;
        void Process(const float *const *in, float **out, size_t size)
        {            
            // we'll use out[0] and out[1] as our working space, 
            // then copy to out[2] and out[3] at the end (and apply gain settings)
            std::fill(out[0], out[0] + size, 0.f);
            std::fill(out[1], out[1] + size, 0.f);

            /** TODO: debug why this is happening */
            if(record != true && record != false)
                record = false;

            for (size_t voice = 0; voice < kMaxPoly; voice++)
                // cache the first block of samples as needed
                chompi_voice[voice].CacheSamples();


            /** voice read*/
            for (size_t voice = 0; voice < kMaxPoly; voice++)
            {
                if (chompi_voice[voice].IsPlaying() || chompi_voice[voice].deferred_trig)
                {
                    for (size_t i = 0; i < size; i++)
                    {
                        float aol = 0.f;
                        float aor = 0.f;

                        chompi_voice[voice].PopStereoSamps(&aol, &aor);
                        out[0][i] += aol;
                        out[1][i] += aor;
                    }
                }
            }

            for(size_t i = 0; i < size; i++)
            {
                out[0][i] = daisysp::SoftClip(out[0][i]);
                out[1][i] = daisysp::SoftClip(out[1][i]);
            }

            // add the monitor to the working space pre-FX
            float monitor[2][size];
            std::fill(&monitor[0][0], &monitor[1][size], 0.f);

            if(monitor_mode == MonitorMode::BOTH)
            {
                if(in_source == InputSource::MIC)
                    ApplyMicMonitor(in, out, size, &monitor[0][0]);
                else if(in_source == InputSource::LINE_IN)
                    ApplyLineMonitor(in, out, size, &monitor[0][0]);
            }
            else if (monitor_mode == MonitorMode::SEND_RET && input_monitor && in_source == InputSource::MIC)
            {
                ApplyMicMonitor(in, out, size, &monitor[0][0]);
            }


            /** looper read + write */
            looper.CheckRecordReady();
            if(looper.CheckReset())
                looper_reset = true;

            //fx_xfade_pre
            if(fx_pre_loop)
                ApplyFx(out[0], out[1], size);

            looper.Process(out[0], out[1], size);

            if(!fx_pre_loop)
                ApplyFx(out[0], out[1], size);

            if(fx_env_ < .01f)
            {
                /* PORT PATCH P15: land on the requested routing. Upstream
                 * flipped fx_pre_loop here; two quick toggles (or a knob swept
                 * across the enum faster than the fade) then ended on the side
                 * the user had already left, and the parameter read back the
                 * wrong value for the rest of the session. */
                fx_pre_loop = fx_pre_loop_target;
                fx_env_target_ = 1.f;
            }

            //working space (HP) copy to line out
            std::copy(out[0], out[0] + size, out[2]);
            std::copy(out[1], out[1] + size, out[3]);

            // add the dry monitor to the HPs only
            if (monitor_mode == MonitorMode::HP && input_monitor)
            {
                if (in_source == InputSource::MIC)
                    ApplyMicMonitor(in, out, size, &monitor[0][0]);
                if (in_source == InputSource::LINE_IN)
                    ApplyLineMonitor(in, out, size, &monitor[0][0]);
            }
            else if (monitor_mode == MonitorMode::SEND_RET)
            {
                ApplyLineMonitor(in, out, size, &monitor[0][0]);
            }

            // apply resample gain as appropriate (and envelope in/out of that situation)
            if(in_source == InputSource::RESAMPLE)
            {
                for(size_t i = 0; i < size; i++)
                {
                    daisysp::fonepole(resamp_env_, resamp_env_target_, .001f);

                    out[0][i] = monitor[0][i] = resamp_env_ * out[0][i];
                    out[1][i] = monitor[1][i] = resamp_env_ * out[1][i];
        
                    input_env_follower.Process((monitor[0][i] + monitor[1][i]) * .2f);

                    if(monitor_mode == MonitorMode::BOTH)
                    {
                        out[2][i] = out[0][i];
                        out[3][i] = out[1][i];
                    }
                }
            }
            else
            {
                ApplyEnvelopeFollower(size, &monitor[0][0]);
            }


            /** voice write */
            if (record)
            {
                for (size_t i = 0; i < size; i++)
                {
                    const int16_t inl = f2s16(monitor[0][i]);
                    const int16_t inr = f2s16(monitor[1][i]);

                    chompi_writer.StereoWrite(inl, inr, false, true);
                }
            }

            if(record && chompi_writer.WriteFullLength())
                StopRecording();

            /** main gain control */
            for(size_t i = 0; i < size; i++)
            {
                fonepole(mgain_, mgain_target_, .001f);
                fonepole(ingain_, ingain_target_, .001f);
                fonepole(final_lim_, final_lim_target_, .001f);

                // apply headphone and main gain
                out[0][i] *= kHpGain * mgain_;
                out[1][i] *= kHpGain * mgain_;

                // apply lineout and main gain
                out[2][i] *= kLineOutGain * mgain_;
                out[3][i] *= kLineOutGain * mgain_;

                // compress
                const float thresh = 1.f / (10.f * final_lim_ + 4.f);
                const float ratio = 1.f + final_lim_ * final_lim_ * 7.f;
                const float makeup = .9f + final_lim_ * .6f;
                const float pregain = 7.f * final_lim_ + 1.f;
                out[0][i] = lim_hp_l_.ProcessComp(out[0][i], pregain, thresh, ratio, makeup);
                out[1][i] = lim_hp_r_.ProcessComp(out[1][i], pregain, thresh, ratio, makeup);
                out[2][i] = lim_line_l_.ProcessComp(out[2][i], pregain, thresh, ratio, makeup);
                out[3][i] = lim_line_r_.ProcessComp(out[3][i], pregain, thresh, ratio, makeup);

                output_env_follower.Process((out[0][i] + out[1][i]));
            }
        }

        inline void SetInputMonitor(bool monitor) 
        {
            input_monitor = monitor;
            
            if(in_source == InputSource::RESAMPLE)
                resamp_env_target_ = monitor ? ingain_target_ : 1.f;
        }

        inline float GetVUSample(VUTarget target)
        { 
            if(target == VUTarget::VU_INPUT)
                return input_env_follower.GetLastSamp();
            else if(target == VUTarget::VU_OUTPUT)
                return output_env_follower.GetLastSamp();

            return 0.f;
        }

        /** @brief Opens a new file and enables recording for a file
         *  @param slot position in the bank to record the sample (ignored for now)
         *
         *  @todo add delay/preprocessing for declicking
         */
        void StartNewRecording(int slot)
        {
            /** if the looper is overdubbing, stop it */
            if(looper.IsRecording())
                looper.ToggleRecord();

            /** if it was the first recording, stop it overdubbing :\ */
            if(looper.IsRecording())
                looper.ToggleRecord();

            /** Send open file request and toggle record flag */
            char name_buffer[256];
            // PORT PATCH P4: absolute-ish path under the sample directory
            chompi::SamplePath(name_buffer, sizeof(name_buffer), "temp_rec.wav");

            // char name_buffer_dbl[256];
            // strcpy(name_buffer_dbl, "temp_rec_double.wav");

            /** For now we only need to record to the first-voices file..
             *  since all voices will be using the same file for playback
             */
            chompi_writer.Reset();
            record = true;
        }

        /** @brief stops recording, and syncs file
         *  @todo add delay/preprocessing for declicking (fade, etc.)
         */
        void StopRecording()
        {
            for(size_t i = 0; i < kMaxPoly; i++)
            {
                chompi_voice[i].RestoreDefaults();
                chompi_voice[i].StopPlaying();
                chompi_voice[i].CloseFile();

                SetVoiceMode(VoiceMode::JAMMI);
                SetVoiceSlot(15, false);
            }

            record = false;
        }
        inline bool Recording() const { return record; }

        /**
         * @brief Resets the assignment of keyboard keys to voices used in voice stealing algorithm.
         *        Call this when we change voice modes, banks, or slots 
         *        (except for some cases involving the chompi buffer)
         */
        inline void ResetVoiceKeys()
        {
            StopAllVoices();

            for(size_t i = 0; i < kMaxPoly; i++)
                playing_key[i] = 0;
        }

        /** PORT PATCH P22 -- tell a voice, before it opens a file, whether that
         *  file has the card's prepared 2x-speed companion. The reader swaps to
         *  it above 1.5x; a user's assignment has none, and claiming otherwise
         *  played it an octave low (see SampleReader.h, double_available). */
        void BindDoubleAvailability(size_t voice, int mode, int bank, size_t slot)
        {
            if(voice >= kMaxPoly)
                return;
            chompi_voice[voice].SetDoubleAvailable(
                chompi::SlotHasDoubleSpeedCopy(mode, bank, (int)slot));
        }

        /** @brief Voice stealing algorithm.
         *  Starts playback of sample in slot at desired playback ratio
         *  @param transpose_nn MIDI note number transposition to play w/ middle C being 0
         *  @param key the identity of the key that was pressed (CHOMPI button id
         *         from the panel, or the host's pad note -- see slot_hint)
         *  @param slot_hint PORT PATCH P22: 0 = ask KeyToSlot() which slot this
         *         key is (the hardware's one-key-one-slot keybed). 1..15 = the
         *         caller has already resolved it, which is what the port does:
         *         on Move a slot is an OCTAVE of the keybed (chompi_move.cpp:
         *         keybed_position() / keybed_slot()), so which slot a press plays
         *         depends on the note, not on the key id.
         */
        void StartPlayback(float transpose_nn, int key, float vel, int slot_hint = 0)
        {
            /** Is this key already playing? */
            int free_idx = -1;
            for(size_t i = 0; i < kMaxPoly; i++)
            {
                if(playing_key[i] == key)
                {
                    free_idx = i;
                    break;
                }
            }

            /** Otherwise, take over an available voice with no key assigned */
            if (free_idx < 0)
            {
                for (size_t i = 0; i < kMaxPoly; i++)
                {
                    if (!chompi_voice[i].IsPlaying() && playing_key[i] == 0)
                    {
                        free_idx = i;
                        break;
                    }
                }
            }

            /** Otherwise, take the first available voice we can get */
            if (free_idx < 0)
            {
                for (size_t i = 0; i < kMaxPoly; i++)
                {
                    if (!chompi_voice[i].IsPlaying())
                    {
                        free_idx = i;
                        break;
                    }
                }
            }

            /** No available voice, take over oldest */
            if (free_idx < 0)
            {
                free_idx = 0;
                uint32_t oldest_time = play_start_time[0];
                for(size_t i = 1; i < kMaxPoly; i++)
                {
                    if(play_start_time[i] <= oldest_time)
                    {
                        free_idx = i;
                        oldest_time = play_start_time[i];
                    }
                }
            }

            // we're only using one file for now. That gets opened on init, or on record end
            uint32_t now = System::GetNow();
            play_start_time[free_idx] = now;

            const bool copy_occurred = chompi_voice[free_idx].GetCopyOccurred();
            if(voice_mode == VoiceMode::CUBBI)
            {
                char name_buffer[256];
                char name_buffer_dbl[256];
                /* PORT PATCH P22: the port resolves the slot itself, because in
                 * CUBBI on Move a slot is an octave of the keybed rather than
                 * one key. KeyToSlot() stays the answer for a request that
                 * carries no hint (the hardware panel, and every probe that
                 * drives button ids). */
                size_t slot;
                if(slot_hint > 0)
                    slot = (size_t)slot_hint;
                else
                    slot = KeyToSlot(key);

                if(slot == kSlotNone || slot < 1 || slot > kSlotCount)
                    return;

                if(!file_exists[1][bank[int(voice_mode)]][slot - 1])
                    return; // no file in that slot

                // reopen the file if it's not a retrigger, or we just switched modes or banks on this voice
                if(slot != 15 && 
                    (playing_key[free_idx] != key 
                        || !chompi_voice[free_idx].GetCubbiMode()
                        || chompi_voice[free_idx].GetBank() != bank[int(voice_mode)]
                        /* PORT PATCH P22: the bound SLOT is part of the test
                         * now. It used to be implied by the key -- one key,
                         * one slot, always -- but a slot is an octave of the
                         * keybed here, so moving the slot knob moves the pad
                         * to a different file while its key id stays put. The
                         * voice used to keep playing the file it already had
                         * (measured: the C of the keybed still ringing slot 1's
                         * tone after the knob moved to 3). */
                        || chompi_voice[free_idx].GetSlot() != (int)slot
                        || copy_occurred
                    ))
                {
                    GetFileNameForSlot(slot, bank[int(voice_mode)], voice_mode, name_buffer);
                    GetFileNameForSlot(slot, bank[int(voice_mode)], voice_mode, name_buffer_dbl, true);
                    BindDoubleAvailability(free_idx, int(voice_mode), bank[int(voice_mode)], slot);
                    chompi_voice[free_idx].OpenFile(name_buffer, name_buffer_dbl, true); 
                    chompi_voice[free_idx].SetSlot(slot); 
                    chompi_voice[free_idx].SetUsingRam(false, true); 
                }
                else if(slot == 15)
                    chompi_voice[free_idx].SetUsingRam(true, true); 

                cubbi_slot_ = slot;
                latest_voice = free_idx;
                /* PORT PATCH P19: this pad's own values, then apply them to the
                 * voice being started -- what the hardware's pad handler does
                 * when it calls OpenCubbiSlot(slot) before the key request
                 * (NormalPage.h). Reading the cache instead is what made every
                 * pad share one set of knob values. */
                LoadSlotValues(slot);
                ApplySlotValues(free_idx);
            }
            else /* JAMMI */
            {
                /* PORT PATCH P14: JAMMI gets a bind of its own.
                 *
                 * Upstream opened a JAMMI file only from SetVoiceSlot(), which
                 * the hardware UI calls when its slot knob picks a tape. The
                 * port's slot knob is a Samples-page selection and deliberately
                 * does not rebind voices (see chompi_move.cpp), so JAMMI had no
                 * way to reach its own samples at all: every key replayed
                 * whatever file the voice last held -- usually the built-in RAM
                 * tone that slot 15 plays -- and the Mode, Bank and Slot knobs
                 * looked dead next to CUBBI's working ones.
                 *
                 * So bind here, the way the CUBBI branch binds the key's slot:
                 * the whole keyboard plays the selected jammi sample, and the
                 * note decides the speed (MidiNoteToPlaybackRatio below). The
                 * bind happens once per selection change, not once per key
                 * press, so a knob sweep cannot storm the file FIFO.
                 *
                 * Bank and mode changes fall out of the bound-mode/bank checks,
                 * and an assignment (set_param "sample_file") marks every voice
                 * with SetAllCopyOccurred(), so copy_occurred picks up the new
                 * file on the next key press. */
                const size_t sel      = voice_slot_;
                const int    sel_bank = bank[int(voice_mode)];

                if(sel == 15)
                {
                    /* Slot 15 is the recording buffer, not a file: upstream
                     * skips the open for it and plays RAM. Tag the voice all the
                     * same -- the bound slot is what tells the key path whether
                     * the next press needs a bind, and a voice left on slot 1
                     * while the selection moved to 15 (or back) would keep
                     * playing the buffer it had. */
                    chompi_voice[free_idx].SetUsingRam(true, true);
                    chompi_voice[free_idx].SetSlot(15);
                    chompi_voice[free_idx].SetBank((uint8_t)sel_bank);
                    latest_voice = free_idx;
                }
                else if(file_exists[int(voice_mode)][sel_bank][sel - 1] &&
                        (chompi_voice[free_idx].GetCubbiMode()
                         || chompi_voice[free_idx].GetSlot() != (int)sel
                         || chompi_voice[free_idx].GetBank() != sel_bank
                         || copy_occurred))
                {
                    char name_buffer[256];
                    char name_buffer_dbl[256];
                    GetFileNameForSlot(sel, sel_bank, voice_mode, name_buffer);
                    GetFileNameForSlot(sel, sel_bank, voice_mode, name_buffer_dbl, true);
                    BindDoubleAvailability(free_idx, int(voice_mode), sel_bank, sel);
                    chompi_voice[free_idx].OpenFile(name_buffer, name_buffer_dbl, true);
                    chompi_voice[free_idx].SetSlot(sel);
                    chompi_voice[free_idx].SetUsingRam(false, true);
                    latest_voice = free_idx;
                }
            }

            playing_key[free_idx] = key;

            // hacks to fix issue with cubbi voices not changing samples sometimes
            chompi_voice[free_idx].SetCubbiMode(voice_mode == VoiceMode::CUBBI);
            chompi_voice[free_idx].SetBank(bank[int(voice_mode)]);

            chompi_voice[free_idx].SetVelocity(vel);

            if(voice_mode == VoiceMode::JAMMI)
            {
                /* PORT PATCH P19: JAMMI applies the SELECTED slot's values on
                 * every press, the way CUBBI applies the pressed pad's. Its
                 * envelope is per voice, and plenty of paths leave a voice with
                 * the wrong one: Choke() rewrites decay to .01 on every
                 * StopAllVoices (bank/mode switch, looper stop), and a voice
                 * that keeps its file across a selection change would otherwise
                 * keep the previous slot's attack forever. */
                LoadSlotValues(voice_slot_);
                ApplySlotValues(free_idx);
                chompi_voice[free_idx].SetVarispeed(MidiNoteToPlaybackRatio(transpose_nn));
            }
            else if(voice_mode == VoiceMode::CUBBI)
            {
                /* PORT PATCH P22: in CUBBI each slot is an octave of the
                 * keybed, so the key WITHIN that octave is what the sample is
                 * pitched by -- root C at 1.0x, B above it at 1.89x. The port
                 * sends that interval (0..11 semitones) as transpose_nn, which
                 * is the same field JAMMI uses for its whole-keyboard pitch.
                 * A hinted request is therefore pitched; a hardware request
                 * (slot_hint 0, one key = one slot, one sample per pad) keeps
                 * the firmware's 1.0x. */
                if(slot_hint > 0)
                    chompi_voice[free_idx].SetVarispeed((float)MidiNoteToPlaybackRatio((int)transpose_nn));
                else
                    chompi_voice[free_idx].SetVarispeed(1.f);
            }


            chompi_voice[free_idx].StartPlaying();
        }

        void StopPlayback(int key)
        {
            for(size_t i = 0; i < kMaxPoly; i++)
            {
                if(playing_key[i] == key)
                {
                    chompi_voice[i].StopPlaying();
                    // chompi_voice[i].JumpToStart();
                }
            }
        }

        void ProcessKeyReqs()
        {
            if (!request_fifo.IsEmpty())
            {
                KeyRequest req = request_fifo.PopFront();
                
                if (req.type_ == KeyRequest::Type::START)
                    StartPlayback(req.transpose_nn_, req.key_, req.vel_, req.slot_hint_);
                else if (req.type_ == KeyRequest::Type::STOP)
                    StopPlayback(req.key_);
            }
        }

        /** Do SD Card stuff
            return true if there are more requests to complete
        */
        bool ProcessFileRequests()
        {
            file_manager.ProcessRequests();
            return !file_manager.request_fifo.IsEmpty();
        }

        void Prepare()
        {
            ProcessKeyReqs();
        }

        bool IsKeyPlaying(int key)
        {
            for(size_t i = 0; i < kMaxPoly; i++)
            {
                if (playing_key[i] == key && chompi_voice[i].IsPlaying())
                {
                    return true;
                }
            }

            return false;
        }

        void SetAttack(float val)
        {
            val = powf(val, 3.f) + .01f;

            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetAttack(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                    chompi_voice[i].SetAttack(val);            
            }
        }

        void SetDecay(float val)
        {
            val = powf(val, 3.f) + .01f;

            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetDecay(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                    chompi_voice[i].SetDecay(val);            
            }
        }

        void SetStartPointForce(float val)
        {
            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetStartPointForce(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                {
                    chompi_voice[i].SetStartPointForce(val);
                }
            }
        }

        void SetEndPointForce(float val)
        {
            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetEndPointForce(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                {
                    chompi_voice[i].SetEndPointForce(val);
                }
            }   
        }

        bool SetStartPoint(float val)
        {
            bool ret = false;
            if(voice_mode == VoiceMode::CUBBI)
            {
                ret = chompi_voice[latest_voice].SetStartPoint(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                    ret = chompi_voice[i].SetStartPoint(val);            
            }

            return ret;
        }

        bool SetEndPoint(float val)
        {
            bool ret = false;
            if(voice_mode == VoiceMode::CUBBI)
            {
                ret = chompi_voice[latest_voice].SetEndPoint(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                    ret = chompi_voice[i].SetEndPoint(val);            
            }

            return ret;
        }

        void SetAutoLoop(bool loop)
        {
            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetAutoLoop(loop);
            }
            else
            {
                for(size_t i = 0; i< kMaxPoly; i++)
                {
                    chompi_voice[i].SetAutoLoop(loop);
                }
            }        
        }

        void ToggleAutoLoop()
        {
            for(size_t i = 0; i< kMaxPoly; i++)
            {
                chompi_voice[i].ToggleAutoLoop();
            }
        }

        bool GetAutoLoop() { return chompi_voice[latest_voice].GetAutoLoop(); }

        void ResetGlobalPitchQuant()
        {
            fifth = false;
            encoder_chunk = 0.f;
        }

        // returns the value for the encoder tracking
        float SetGlobalPitchQuantized(int16_t turns, float enc_pos)
        {
            encoder_chunk += turns * .25f;
            if(encoder_chunk >= 1.f || encoder_chunk <= -1.f)
            {
                // get current semi
                encoder_chunk = round(encoder_chunk);

                float pitch = GetGlobalPitch();
                float orig_pitch = pitch;

                // snap to fifths and octaves
                // calculate the consts via 2^(x/12) e.g. 2^(-5/12) for down 5 semis
                float mul;
                if(!GetReverse())
                {
                    if(fifth)
                        mul = encoder_chunk < 0 ? .667419927085f : 1.33483985417f;
                    else
                        mul = encoder_chunk < 0 ? .749153538438f : 1.49830707688f;
                }
                else
                {
                    if(fifth)
                        mul = encoder_chunk < 0 ? 1.33483985417f : .667419927085f;
                    else
                        mul = encoder_chunk < 0 ? 1.49830707688f : .749153538438f;                            
                }

                // jump, then do nothing if we've gone over the end
                pitch *= mul;
                if(pitch > 2.f || pitch < -2.f)
                    return enc_pos;

                fifth = !fifth;
                encoder_chunk = 0.f;

                // handle direction change
                if(pitch < .0625)
                {
                    // we weren't already in the turn-around zone
                    if(orig_pitch > .0625)
                    {
                        fifth = !fifth;
                        pitch = orig_pitch;
                        ToggleReverse();
                    }
                    // we were already in the zone, and we're headed over the middle
                    else if((GetReverse() && turns > 0) || (!GetReverse() && turns < 0))
                    {
                        fifth = !fifth;
                        pitch = orig_pitch;
                        ToggleReverse();
                    }
                }

                SetGlobalPitch(pitch);

                if(pitch < .5f) // 0 - .33
                    pitch = (pitch - .01) * 0.673469f;
                else if (pitch < 1.f ) // .33 - .66
                    pitch = (pitch - .5f) * .66f + .33f;
                else // .66 - 1
                    pitch = (pitch - 1.f) * .34f + .66f;
                
                return GetReverse() ? (1.f - pitch) * .5f : pitch * .5f + .5f;
            }

            return enc_pos;
        }

        /** unquantized pitch
            * curved s.t.
            *  0.f - .33f = .01x - .5x
            *  .33f - .66f = .5x - 1x
            *  .66f - 1.f = 1x - 2x
        */
        void SetGlobalPitchFree(float val)
        {
            val = val < .5f ? (.5f - val) * -2.f : (val - .5f) * 2.f; // 1 - 0 - 1

            float pitch;
            float inv = val < 0.f ? -1.f : 1.f;
            if(fabsf(val) < .33f) // .01x - .5x
                pitch = val * 1.484848f + .01f * inv;
            else if (fabsf(val) < .66f ) // .5x - 1x
                pitch = (val - .33f * inv) * 1.515151 + .5f * inv;
            else // 1x - 2x
                pitch = (val - .66 * inv) * 2.941176 + 1.f * inv;

            SetGlobalPitch(fabsf(pitch));
            SetReverse(pitch < 0.f);
        }

        void SetGlobalPitch(float val)
        {
            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetGlobalPitch(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                    chompi_voice[i].SetGlobalPitch(val);            
            }
        }
        inline float GetGlobalPitch() { return chompi_voice[latest_voice].GetGlobalPitch(); }

        void SetGain(float val)
        {
            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetGain(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                    chompi_voice[i].SetGain(val);            
            }
        }

        void SetPan(float val)
        {
            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetPan(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                    chompi_voice[i].SetPan(val);            
            }            
        }

        float GetPan() 
        {
            if(voice_mode == VoiceMode::CUBBI)
                return chompi_voice[latest_voice].GetPan();

            return chompi_voice[0].GetPan();            
        }

        void SetReverse(bool rev)
        {
            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetReverse(rev);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                    chompi_voice[i].SetReverse(rev);            
            }
        }
        void ToggleReverse() { SetReverse(!chompi_voice[latest_voice].GetReverse()); }
        inline bool GetReverse() { return chompi_voice[latest_voice].GetReverse(); }



        void SetSustainActive(bool sus)
        {
            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetSustainActive(sus);
            }
            else
            {
                for(size_t i = 0; i< kMaxPoly; i++)
                {
                    chompi_voice[i].SetSustainActive(sus);
                }
            }        
        }

        void ToggleSustainActive() 
        {
            for(size_t i = 0; i < kMaxPoly; i++)
            {
                chompi_voice[i].ToggleSustainActive();
            }
        }
        inline bool GetSustainActive() { return chompi_voice[latest_voice].GetSustainActive(); }

        void SetInputGain(float gain) 
        {
            ingain_target_ = gain; 
         
            if(input_monitor && in_source == InputSource::RESAMPLE)
                resamp_env_target_ = ingain_target_;
        }
        
        inline void SetMainGain(float gain) { mgain_target_ = gain; }
        inline void SetFinalComp(float comp) { final_lim_target_ = comp; }
        inline float GetFinalComp() { return final_lim_target_; }

        inline void SetReverb(float val) { dly_amt_target_ = reverb_amt_target_ = 1.3f * logf(val + 1.f); }
        inline void SetDelayFeedback(float val) { dly_feedback_target_ = val * .9f; }
        inline void SetDelayTime(float val) {
            dly_time_target_ = .99f * powf(val, 3.f) * kMaxDelayTime + 450;

            reverb_time_target_ = val;
            reverb_time_target_ = fclamp(reverb_time_target_, .05, .97);
        }

        inline void SetWarble(float val) { warble_.SetFreq(val); }

        inline void SetFilter(float val) { cutoff_target_ = val; }
        inline void SetFilterResonance(float val) { res_target_ = val; }
        inline void SetSaturate(float val) 
        { 
            val = logf(1.7f * val + 1.f); 
            saturate_amt_target_ = val * 13.f + 1.f; 
            // saturate_amt_target_ *= .5f; 
        
        }

        inline bool GetLooperRecordArm() { return looper.GetRecordArm(); }
        inline bool GetLooperIsEmpty() { return looper.GetIsEmpty(); }
        inline float GetLooperPosition() { return looper.GetPosition(); }
        inline float GetLooperPitch() { return looper.GetPitch(); }

        inline void LooperRecordButton(bool rising) { looper.RecordButton(rising); }
        inline void LooperPlayButton(bool rising) { looper.PlayButton(rising); }

        inline void ToggleLooperRecord() { looper.ToggleRecord(); }

        /* PORT PATCH P16: clearing a recorded loop. On the hardware it is the
         * record+play buttons held together, which CheckReset()/Reset() above
         * decode from a button press; the chain host has no pair of buttons to
         * hold, so the port exposes the same call as an action parameter
         * ("loop_clear"). LooperEngine::Reset() drops an in-flight recording,
         * stops the transport and empties the loop -- audibly a tape stop, as
         * on the hardware: the last loop winds down over about a second. */
        inline void ClearLooper()
        {
            looper.Reset();
            looper_reset = true;
        }
        inline void ToggleLooperPlaying() { looper.TogglePlaying(); }
        inline bool GetLooperReverse() { return looper.GetReverse(); }

        void ResetLooperPitchQuant()
        {
            looper_fifth = false;
            looper_encoder_chunk = 0.f;
        }

        float SetLooperPitchQuantized(int16_t turns, float enc_pos)
        {
            if (IsLooperPlaying())
            {
                looper_encoder_chunk += turns * .25f;
                if(looper_encoder_chunk >= 1.f || looper_encoder_chunk <= -1.f)
                {
                    // get current semi
                    looper_encoder_chunk = round(looper_encoder_chunk);

                    float pitch = GetLooperPitch();
                    float orig_pitch = pitch;

                    // snap to fifths and octaves
                    // calculate the consts via 2^(x/12) e.g. 2^(-5/12) for down 5 semis
                    float mul;
                    if(pitch > 0.f)
                    {
                        if(looper_fifth)
                            mul = looper_encoder_chunk < 0 ? .667419927085f : 1.33483985417f;
                        else
                            mul = looper_encoder_chunk < 0 ? .749153538438f : 1.49830707688f;
                    }
                    else
                    {
                        if(looper_fifth)
                            mul = looper_encoder_chunk < 0 ? 1.33483985417f : .667419927085f;
                        else
                            mul = looper_encoder_chunk < 0 ? 1.49830707688f : .749153538438f;                            
                    }

                    // snap to next note, return of out of bounds
                    pitch *= mul;
                    if(pitch > 2.f || pitch < -2.f)
                        return enc_pos;

                    looper_fifth = !looper_fifth;
                    looper_encoder_chunk = 0.f;

                    // handle reverse
                    if(pitch < .0625 && pitch > -.0625)
                    {
                        // we weren't already in the turn-around zone
                        if(orig_pitch > .0625 || orig_pitch < -.0625)
                        {
                            looper_fifth = !looper_fifth;
                            pitch = orig_pitch;
                            pitch *= -1.f;
                        }
                        // we were already in the zone, and we're headed over the middle
                        else if((GetLooperReverse() && turns > 0) || (!GetLooperReverse() && turns < 0))
                        {
                            looper_fifth = !looper_fifth;
                            pitch = orig_pitch;
                            pitch *= -1.f;
                        }
                    }

                    SetLooperPitch(pitch);

                    return pitch * .25f + .5f;

                }
            }

            return enc_pos;
        }

        inline void SetLooperPitchFree(float val)
        {
            SetLooperPitch(val * 4.f - 2.f); // -2 - 2            
        }

        inline void SetLooperPitch(float val) { looper.SetPitch(val); }
        inline void SetLooperScrub(float scrub) { looper.SetScrub(scrub); }
        inline float GetLooperScrub() { return looper.GetScrub(); }

        bool AnyVoicesPlaying() 
        {
            for(size_t i = 0; i < kMaxPoly; i++)
            { 
                if(chompi_voice[i].IsPlaying())
                    return true;
            }

            return false;
        }

        inline bool IsLooperPlaying() { return looper.IsPlaying(); };
        inline bool IsLooperRecording() { return looper.IsRecording(); };
        inline bool IsLooperFirstRecording() { return looper.IsFirstRecording(); };
        inline bool IsLooperRecordArmed() { return looper.GetRecordArm(); };
        /* PORT PATCH P24: a record pass running OR requested -- what a write to
         * the port's record cell has to be compared against (see
         * LooperEngine::WantsRecord). */
        inline bool LooperRecordWanted() { return looper.WantsRecord(); }

        inline bool GetRecordLatch() { return record_latch; }

        inline void IncrementLooperDubGain(float gain) { looper.IncrementDubGain(gain); }
        inline float GetLooperDubGain() { return looper.GetDubGain(); }

        inline void LooperOpenFile() { looper.OpenFile(); }

        inline int GetBank() { return bank[int(voice_mode)];}
        inline size_t GetVoiceBank() 
        {
            if(voice_mode == VoiceMode::CUBBI)
                return bank[int(voice_mode)];
            
            return voice_bank_; 
        }
        inline void IncrementBank() 
        {
            bank[int(voice_mode)] = (bank[int(voice_mode)] + 1) % 5;

            if(voice_mode == VoiceMode::CUBBI)
                ResetVoiceKeys();
        }
        inline void SetBank(size_t b) 
        {
            bank[int(voice_mode)] = b % 5;

            if(voice_mode == VoiceMode::CUBBI)
                ResetVoiceKeys();
        }

        daisy::FIFO<KeyRequest, 32> request_fifo;

        /** @brief populates expected filename for a given slot
         *  @param slot position in the bank
         *  @param bank bank number
         *  @param name string to fill with filename
         *
         *  name must be a buffer of at least 16 bytes to fit the name:
         *  - "chompi_xy.wav" where xy is a letter-number combo indicating bank/slot
         */
        static void GetFileNameForSlot(int slot, int b, VoiceMode m, char *name, bool dbl = false)
        {
            /* PORT PATCH P9: naming moved into chompi::SlotSamplePath so a slot
             * can carry a user-assigned sample (set_param "sample_file").
             * Without an override it returns exactly the name this function
             * used to build: "<sample_dir>/<mode>_<bank><slot>[_double].wav".
             *
             * Everything that names a slot file goes through here — the key
             * playback path, SetVoiceSlot()'s RAM load, EraseStart() and the
             * UpdateFileExists() scan — so one override table covers them all
             * and a slot with an assigned sample is scanned as present. */
            if(name)
                chompi::SlotSamplePath((int)m, b, (int)slot, dbl, name, 256);
        }

        char fname[256];
        char fname_double[256];
        uint8_t erasing = 0;
        void EraseStart(uint8_t target, int b, VoiceMode m)
        {
            SetFileExists(target - 1, b, m, false);

            GetFileNameForSlot(target, b, m, fname);
            GetFileNameForSlot(target, b, m, fname_double, true);

            /* PORT PATCH P10: only unlink files inside our own sample directory.
             * A slot may point at a WAV the user chose from the device library
             * ("sample_file"), and CHOMPI's erase is not entitled to delete it.
             * The counter tracks requests actually queued, so EraseFinished()
             * still balances — this path is unused by the port today, but a
             * future erase/record binding must not be able to destroy user
             * data. */
            if(chompi::IsManagedPath(fname))
            {
                FileRequest req(FileRequest::Type::UNLINK, nullptr, fname, 0, nullptr, this);
                file_manager.request_fifo.PushBack(req);
                erasing++;
            }

            if(chompi::IsManagedPath(fname_double))
            {
                FileRequest double_req(FileRequest::Type::UNLINK, nullptr, fname_double, 0, nullptr, this);
                file_manager.request_fifo.PushBack(double_req);
                erasing++;
            }
        }

        inline void EraseFinished()
        { 
            if(erasing != 0)
                erasing--; 
        }

        inline bool IsErasing() { return erasing != 0; }

        /* "A slot's file changed." On the hardware the FileCopier calls this
         * after copying a sample into a slot, and the key path reads it back as
         * copy_occurred to decide that a voice must re-open its file.
         *
         * PORT: the port has no copier -- its equivalent is an assignment
         * through the sample map -- so the plugin calls this from that path.
         * Without it, pointing a slot at a new file and pressing its key
         * replayed whatever the voice already held: on a fresh module (nothing
         * loaded yet) silence, and with several voices in play the pads end up
         * holding different files. Both were reported from the device. */
        inline void SetAllCopyOccurred()
        {
            for(size_t i = 0; i < kMaxPoly; i++)
                chompi_voice[i].SetCopyOccurred();
        }

        void UpdateFileExists()
        {
            for(size_t m = 0; m < 2; m++)
            {
                for(size_t b = 0; b < 5; b++)
                {
                    for(size_t s = 0; s < 15; s++)
                    {
                        if (s >= 14)
                        {
                            file_exists[m][b][s] = true;
                        }
                        else
                        {
                            GetFileNameForSlot(s + 1, b, VoiceMode(m), fname);
                            file_exists[m][b][s] = f_stat(fname, nullptr) == FR_OK;
                        }
                    }
                }
            }
        }

        inline void SetFileExists(size_t slot, size_t bank, VoiceMode mode, bool exists)
        {
            file_exists[int(mode)][bank][slot] = exists;
        }

        inline bool GetFileExists(size_t slot)
        { 
            return file_exists[int(voice_mode)][GetBank()] [slot];
        }

        inline size_t GetVoiceSlot()
        { 
            if(voice_mode == VoiceMode::JAMMI)
                return voice_slot_;
            
            return cubbi_slot_;
        }

        /* PORT PATCH P19: the per-slot values.
         *
         * On the hardware these live in the preset store, nine per (mode,
         * bank, slot), and the UI's encoder array is a window onto whichever
         * slot is selected. Two paths keep them honest:
         *
         *   - a pad press re-reads that slot's values into the cache and the
         *     key path applies them (NormalPage::OpenCubbiSlot);
         *   - a knob writes the selected slot's value and applies it live.
         *
         * The port had only the cache, one global set, so the Setup page's
         * bank/slot knobs changed nothing and Attack and Decay looked wired
         * together: CUBBI's key path re-applied CUBBI's values on every press,
         * and nothing ever updated those. */
        inline void SetSlotAttack(float val)   { StoreSelected(kSlotAttack, val);   cubbi_attack = val;   SetAttack(val); }
        inline void SetSlotDecay(float val)    { StoreSelected(kSlotDecay, val);    cubbi_decay = val;    SetDecay(val); }
        inline void SetSlotStart(float val)    { StoreSelected(kSlotStart, val);    cubbi_start = val;    SetStartPoint(val); }
        inline void SetSlotEnd(float val)      { StoreSelected(kSlotEnd, val);      cubbi_end = val;      SetEndPoint(val); }
        inline void SetSlotGain(float val)     { StoreSelected(kSlotGain, val);     cubbi_gain = val;     SetGain(val); }
        inline void SetSlotPan(float val)      { StoreSelected(kSlotPan, val);      cubbi_pan = val;      SetPan(val); }
        inline void SetSlotAutoLoop(bool loop) { StoreSelected(kSlotLoop, loop ? 1.f : 0.f); cubbi_autoloop = loop; SetAutoLoop(loop); }
        inline void SetSlotSustain(bool sus)   { StoreSelected(kSlotSustain, sus ? 1.f : 0.f); cubbi_sustain = sus; SetSustainActive(sus); }
        /* Pitch has two units and they are not interchangeable: the UI (and the
         * hardware's preset store) holds the ENCODER value, 0..1 with the sign
         * meaning reverse, while the engine's cubbi_pitch is a playback RATIO
         * (NormalPage::OpenCubbiSlot converts between them exactly once). Only
         * SetSlotPitchFromUi()/LoadSlotValues() may write it, so no caller has
         * to remember which is which. */

        /** The slot the Tape page's knobs edit: the word the UI set. */
        inline size_t SelectedSlot() const { return voice_slot_; }

        /** Writes one value into the selected slot's store. A knob is an edit of
         *  that slot, so it is recorded there -- the hardware's encoder path
         *  does exactly this (PresetManager::SetValue in OnEncoderTurned). */
        inline void StoreSelected(chompi::SlotValue field, float val)
        {
            const int m = int(voice_mode);
            chompi::SetSlotValue(m, bank[m], (int)voice_slot_, (int)field, val);
        }

        /** Reads `slot`'s nine stored values into the cache. The hardware does
         *  this for the pressed pad before starting the voice, which is what
         *  makes bank/slot audible: each slot carries its own envelope, gain,
         *  pan, pitch and loop settings. */
        inline void LoadSlotValues(size_t slot)
        {
            const int m = int(voice_mode);
            const int b = bank[m];
            const float enc = chompi::GetSlotValue(m, b, (int)slot, chompi::kSlotPitch);
            cubbi_pitch    = (enc < 0.f ? -1.f : 1.f) * PitchEncoderToRatio(fabsf(enc));
            cubbi_start    = chompi::GetSlotValue(m, b, (int)slot, chompi::kSlotStart);
            cubbi_end      = chompi::GetSlotValue(m, b, (int)slot, chompi::kSlotEnd);
            cubbi_attack   = chompi::GetSlotValue(m, b, (int)slot, chompi::kSlotAttack);
            cubbi_decay    = chompi::GetSlotValue(m, b, (int)slot, chompi::kSlotDecay);
            cubbi_autoloop = chompi::GetSlotValue(m, b, (int)slot, chompi::kSlotLoop) >= .5f;
            cubbi_sustain  = chompi::GetSlotValue(m, b, (int)slot, chompi::kSlotSustain) >= .5f;
            cubbi_gain     = chompi::GetSlotValue(m, b, (int)slot, chompi::kSlotGain);
            cubbi_pan      = chompi::GetSlotValue(m, b, (int)slot, chompi::kSlotPan);
        }

        /** The cache, for a page that has to show what the selected slot holds
         *  ("what the engine will actually use"), and for the state blob. */
        inline void CopySlotValues(float* out9, bool* loop, bool* sustain) const
        {
            /* Back to the UI's unit: the page shows a knob it can hand straight
             * back to set_param. */
            out9[chompi::kSlotPitch]  = (cubbi_pitch < 0.f ? -1.f : 1.f)
                                        * PitchRatioToEncoder(fabsf(cubbi_pitch));
            out9[chompi::kSlotStart]  = cubbi_start;
            out9[chompi::kSlotEnd]    = cubbi_end;
            out9[chompi::kSlotAttack] = cubbi_attack;
            out9[chompi::kSlotDecay]  = cubbi_decay;
            out9[chompi::kSlotGain]   = cubbi_gain;
            out9[chompi::kSlotPan]    = cubbi_pan;
            *loop    = cubbi_autoloop;
            *sustain = cubbi_sustain;
        }

        /** Applies the cache to one voice -- what the key path does after it
         *  has loaded the slot's values (or, in JAMMI, the selected slot's). */
        inline void ApplySlotValues(size_t voice)
        {
            const size_t saved_latest = latest_voice;
            latest_voice = voice; /* the setters below write to latest_voice */
            SetGlobalPitch(fabsf(cubbi_pitch));
            SetReverse(cubbi_pitch < 0.f);
            SetStartPoint(cubbi_start);
            SetEndPoint(cubbi_end);
            SetAttack(cubbi_attack);
            SetDecay(cubbi_decay);
            SetAutoLoop(cubbi_autoloop);
            SetSustainActive(cubbi_sustain);
            SetGain(cubbi_gain);
            SetPan(cubbi_pan);
            latest_voice = saved_latest;
        }

        /** The firmware's pitch-encoder curve (NormalPage::OpenCubbiSlot): a
         *  0..1 UI value becomes a ratio in 0.01..2, always positive. */
        static float PitchEncoderToRatio(float val)
        {
            val = val < .5f ? (.5f - val) * -2.f : (val - .5f) * 2.f; /* 1 - 0 - 1 */
            const float inv = val < 0.f ? -1.f : 1.f;                 /* magnitude only */

            if(fabsf(val) < .33f)
                return fabsf(val * 1.484848f + .01f * inv);
            if(fabsf(val) < .66f)
                return fabsf((val - .33f * inv) * 1.515151f + .5f * inv);
            return fabsf((val - .66f * inv) * 2.941176f + 1.f * inv);
        }

        /** The inverse of PitchEncoderToRatio: a playback ratio back onto the
         *  encoder's 0..1 scale. Used when a page has to show the ratio the
         *  engine holds, so the knob it draws maps back to the same ratio. */
        static float PitchRatioToEncoder(float ratio)
        {
            ratio = fabsf(ratio);
            if(ratio < .01f)  ratio = .01f;
            if(ratio > 2.f)   ratio = 2.f;
            /* The forward curve's breakpoints land on encoder .5 (ratio .01),
             * .665 (ratio .5) and .83 (ratio 1) -- .83 being the hardware's
             * default, which is why 1x reads as .83 and not as 1.0. */
            if(ratio < .5f)
                return .5f + (ratio - .01f) / 2.969696f;
            if(ratio < 1.f)
                return .665f + (ratio - .5f) / 3.030302f;
            return .83f + (ratio - 1.f) / 5.882352f;
        }

        /** The slot's pitch from the UI's two controls: the varispeed value
         *  (0..1, centre = original speed) and the reverse switch. On the
         *  hardware the pitch encoder IS the pitch — its lower half plays the
         *  sample backwards — and the key path derives reverse from the sign on
         *  every press (see the CUBBI branch above), so the switch has to be
         *  part of the slot's value to survive a key. */
        inline void SetSlotPitchFromUi(float varspeed, bool reverse)
        {
            const float ratio = PitchEncoderToRatio(daisysp::fclamp(varspeed, 0.f, 1.f));
            cubbi_pitch = reverse ? -ratio : ratio;
            /* Store the ENCODER value, not the ratio: the store is a picture of
             * the UI, and the page reads .83 back as "1x" the same way the
             * hardware's encoder does. */
            StoreSelected(chompi::kSlotPitch, reverse ? -daisysp::fclamp(varspeed, 0.f, 1.f)
                                                      : daisysp::fclamp(varspeed, 0.f, 1.f));
            SetGlobalPitch(ratio);
            SetReverse(reverse);
        }

        // these should really be in a struct defined in the presets file
        float cubbi_pitch;
        float cubbi_start;
        float cubbi_end;
        float cubbi_attack;
        float cubbi_decay;
        bool cubbi_autoloop;
        bool cubbi_sustain;
        float cubbi_gain;
        float cubbi_pan;

        void OpenCubbiSlot(float pitch, float start, float end, float attack, float decay, bool autoloop, bool sustain, float gain, float pan)
        {
            cubbi_pitch = pitch;
            cubbi_start = start;
            cubbi_end = end;
            cubbi_attack = attack;
            cubbi_decay = decay;
            cubbi_autoloop = autoloop;
            cubbi_sustain = sustain;
            cubbi_gain = gain;
            cubbi_pan = pan;
        }

        void SetVoiceSlot(size_t idx, bool click)
        {
            voice_slot_ = idx;
            
            if(voice_mode == VoiceMode::JAMMI)
            {
                voice_bank_ = bank[int(voice_mode)];
            }

            char name_buffer[256];
            GetFileNameForSlot(idx, bank[int(voice_mode)], voice_mode, name_buffer);

            char name_buffer_dbl[256];
            GetFileNameForSlot(idx, bank[int(voice_mode)], voice_mode, name_buffer_dbl, true);

            for(size_t i = 0; i < kMaxPoly; i++)
            {
                if(idx != 15)
                {
                    BindDoubleAvailability(i, int(voice_mode), bank[int(voice_mode)], idx);
                    chompi_voice[i].OpenFile(name_buffer, name_buffer_dbl, true);
                }

                chompi_voice[i].SetSlot(idx); 
                chompi_voice[i].SetCubbiMode(voice_mode == VoiceMode::CUBBI);
                chompi_voice[i].SetUsingRam(idx == 15, click);
            }
        }

        /* PORT PATCH P17: choose the slot without binding it.
         *
         * SetVoiceSlot() above does two jobs: it records which slot is selected
         * and it opens that slot's file for every voice. The port needs them
         * apart. Its slot knob is the Samples page's selection and (in JAMMI)
         * the tape the keys play; driving SetVoiceSlot() from a knob sweep is
         * what used to wedge a reader with a queue of half-finished opens.
         *
         * JAMMI's key path (P14) binds from voice_slot_ on the next key press
         * anyway, so recording the choice here is all it takes -- one open, on
         * the press that follows the turn of the knob, with the bank the knob
         * was turned in. */
        inline void SelectVoiceSlot(size_t idx)
        {
            voice_slot_ = idx;

            if(voice_mode == VoiceMode::JAMMI)
                voice_bank_ = bank[int(voice_mode)];
        }

        inline VoiceMode GetVoiceMode () { return voice_mode; }

        inline void SetVoiceMode(VoiceMode m)
        {
            if(voice_mode != m)
                ResetVoiceKeys();

            voice_mode = m;
        }

        void StopAllVoices(size_t idx = 100)
        {
            for(size_t i = 0; i < kMaxPoly; i++)
            {
                if(idx == 100 || idx == chompi_voice[i].GetSlot())
                    chompi_voice[i].Choke();
            }
        }

        // otherwise, they are post looper
        void SetFxPreLooper(bool pre)
        {
            /* PORT PATCH P15: remember the request as well as starting the
             * fade; the fade itself only decides WHEN the routing changes, not
             * which side it ends on. */
            fx_pre_loop_target = pre;

            if(pre != fx_pre_loop)
            {
                looper.FXEnvelope();
                fx_env_target_ = 0.f;
            }
        }

        inline bool GetFxPreLooper() { return fx_pre_loop; }

        inline InputSource GetInputSource() { return in_source; }

        void SetInputSource(InputSource source)
        {
            in_source = source;
        }

        inline void IncrementMonitorMode() { monitor_mode = MonitorMode( (int(monitor_mode) + 1) % int(MonitorMode::LAST) ); }
        inline MonitorMode GetMonitorMode() { return monitor_mode; }

        inline bool IsManagerEmpty() __attribute__((optimize("-O0"))) { return file_manager.request_fifo.IsEmpty(); }

        /** Per-voice state, one line each, for diagnosing silence on a device
         *  where the only other symptom is "no sound until I restart it". The
         *  readings that decide whether a voice can make a sound at all: the
         *  file it holds (size 0 = nothing to read), whether its open requests
         *  reached the worker, and how far its last read went.
         *
         *  PORT (not a patch to existing firmware behaviour): the firmware had
         *  no such accessor, and the voices are private to this class. */
        int DescribeVoices(char* buf, size_t len)
        {
            if(buf == nullptr || len == 0)
                return 0;
            int off = 0;
            for(size_t i = 0; i < kMaxPoly; i++)
            {
                const int n = snprintf(buf + off, len - (size_t)off,
                                       "%sv%zu p%d(g%d/e%d) key%d sz%ld op%zu sk%zu rd%zu buf%zu dj%d dt%d last%zu ram%d slot%d at=%.4f rt=%.4f fs=%.3f fe=%.3f ib%d rv%d rs%zu seg%d %s\n",
                                       i ? " | " : "", i,
                                       chompi_voice[i].IsPlaying() ? 1 : 0,
                                       chompi_voice[i].env_gate_ ? 1 : 0,
                                       chompi_voice[i].env_.IsRunning() ? 1 : 0,
                                       playing_key[i],
                                       (long)f_size(&chompi_voice[i].fptr_read),
                                       chompi_voice[i].open_requests_,
                                       chompi_voice[i].seek_requests_,
                                       chompi_voice[i].read_requests_,
                                       chompi_voice[i].read_samps.GetNumElements(),
                                       chompi_voice[i].deferred_jump ? 1 : 0,
                                       chompi_voice[i].deferred_trig ? 1 : 0,
                                       chompi_voice[i].last_read_size_,
                                       chompi_voice[i].using_ram ? 1 : 0,
                                       (int)chompi_voice[i].GetSlot(),
                                       chompi_voice[i].att_time_, chompi_voice[i].rel_time_,
                                       chompi_voice[i].fstart_, chompi_voice[i].fend_,
                                       chompi_voice[i].is_buffered ? 1 : 0,
                                       chompi_voice[i].reverse_ ? 1 : 0,
                                       chompi_voice[i].read_samps.GetNumElements(),
                                       (int)chompi_voice[i].env_.GetCurrentSegment(),
                                       chompi_voice[i].fname_rx);
                if(n <= 0 || n >= (int)(len - (size_t)off))
                    break;
                off += n;
            }
            char flags[kSlotModes * kSlotBanks * kSlotCount + 1];
            for(size_t m = 0; m < kSlotModes; m++)
                for(size_t b = 0; b < kSlotBanks; b++)
                    for(size_t sl = 0; sl < kSlotCount; sl++)
                        flags[(m * kSlotBanks + b) * kSlotCount + sl] =
                            file_exists[m][b][sl] ? '1' : '0';
            flags[kSlotModes * kSlotBanks * kSlotCount] = 0;
            const int kn = snprintf(buf + off, len - (size_t)off,
                                    "keyfifo=%zu filefifo=%zu refused=%zu fe=%s\n",
                                    request_fifo.GetNumElements(),
                                    file_manager.request_fifo.GetNumElements(),
                                    file_manager.request_fifo.refused_pushes, flags);
            /* PORT (not a patch): what the Setup-page knobs are driving --
             * which mode/bank/slot the voices bind from, and whether the FX sit
             * before or after the looper. Without this the only way to tell a
             * dead knob from a dead bind is to listen. */
            const int sn = snprintf(buf + off, len - (size_t)off,
                                    "mode=%s bank=%d sel=%zu fx_pre=%d fx_target=%d loop_empty=%d\n",
                                    voice_mode == VoiceMode::CUBBI ? "cubbi" : "jammi",
                                    GetBank(), GetVoiceSlot(),
                                    fx_pre_loop ? 1 : 0, fx_pre_loop_target ? 1 : 0,
                                    looper.GetIsEmpty() ? 1 : 0);
            if(sn > 0 && sn < (int)(len - (size_t)off))
                off += sn;
            const int en = snprintf(buf + off, len - (size_t)off,
                                    "openfails=%d last='%s' errno=%d(%s)\n",
                                    chompi::g_open_fail_count, chompi::g_last_open_fail,
                                    chompi::g_last_open_errno,
                                    strerror(chompi::g_last_open_errno));
            if(en > 0 && en < (int)(len - (size_t)off))
                off += en;
            /* PORT PATCH P21: user samples the WAV translator had to convert,
             * and what they were. A file it could not decode is silent by
             * design and named here. */
            const int wn = snprintf(buf + off, len - (size_t)off,
                                    "sampleviews=%d last='%s'\n",
                                    chompi::g_sample_view_count, chompi::g_last_sample_view);
            if(wn > 0 && wn < (int)(len - (size_t)off))
                off += wn;
            if(kn > 0 && kn < (int)(len - (size_t)off))
                off += kn;
            return off;
        }

        // helper for stuck key hack
        inline int GetPlayingKey(size_t idx) { return playing_key[idx]; }

    private:
        /** @brief Converts a MIDI note number to a ratio of playback speed
         *  @param nn number of MIDI notes to transpose above or below original speed
         *
         *  @todo handle as a value from 0-127 w/ Middle C being 0
         *  @todo handle w/ lookup table to improve performance
         */
        float MidiNoteToPlaybackRatio(int nn) { return pow(2.f, nn * 0.08333333f); } // .0833 = 1/12

        /** the looper */
        LooperEngine looper;

        float encoder_chunk = 0.f; // chunk up the quantized pitch controls
        float looper_encoder_chunk = 0.f;
        bool fifth, looper_fifth;
        
        /** env followers for VU meters */
        EnvFollower input_env_follower;
        EnvFollower output_env_follower;

        /** Sampling Bits */
        daisy::FileStreamingManager file_manager;
        daisy::FileSampleReader chompi_voice[kMaxPoly];
        RamBuffer chompi_writer;

        chompi::Limiter lim_hp_l_;
        chompi::Limiter lim_hp_r_;
        chompi::Limiter lim_line_l_;
        chompi::Limiter lim_line_r_;



        bool record, record_latch;
        bool input_monitor;
        int playing_key[kMaxPoly];

        VoiceMode voice_mode;

        bool file_exists[2][5][15]; // mode, bank, slot
        size_t voice_slot_;
        size_t cubbi_slot_;

        // in jammi mode, which bank is currently active?
        // we can change bank pages without selecting a new slot in that bank
        size_t voice_bank_;

        uint32_t play_start_time[kMaxPoly];

        float ingain_, ingain_target_;
        float resamp_env_, resamp_env_target_;
        float mgain_, mgain_target_;
        float final_lim_, final_lim_target_;

        /** FX */
        MicFilter mic_filter_;
        DjFilter filter_;
        daisysp::Reverb* reverb_;
        chompi::InterpolatedDelayLine del_;
        Warble warble_;
        daisysp::DcBlock dcblock_mic_in_;
        daisysp::DcBlock dcblock_line_in_l_;
        daisysp::DcBlock dcblock_line_in_r_;
        daisysp::DcBlock dcblock_fx_l_;
        daisysp::DcBlock dcblock_fx_r_;

        size_t latest_voice;

        float dly_time_, dly_time_target_;
        float dly_feedback_, dly_feedback_target_;
        float reverb_amt_, reverb_amt_target_;
        float reverb_time_, reverb_time_target_;
        float dly_amt_, dly_amt_target_;
        float saturate_amt_, saturate_amt_target_;
        float cutoff_, cutoff_target_;
        float res_, res_target_;
        
        bool fx_pre_loop;
        bool fx_pre_loop_target;
        float fx_env_, fx_env_target_;

        int bank[int(VoiceMode::LAST)];

        InputSource in_source;
        MonitorMode monitor_mode;

        /** No copy, no assign */
        Engine(const Engine &);
        void operator=(const Engine &);
    };

} // namespace chompi