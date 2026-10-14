#pragma once
#include "daisy_seed.h"
#include "daisysp.h"
#include "fatfs.h"
#include "RamBuffer.h"
#include "limiter.h" // PORT PATCH P2

namespace daisy
{

    class FileSampler
    {
    public:
        void Init(float sr, RamBufferMemory* buff, bool tape_slew)
        {
            ram_buff.Init(buff);
            sr_ = sr;
            varispeed_factor = 1.f;
            scrub_ =  1.f;
            reverse_ = false;
            
            tape_slew_ = tape_slew;

            lim_l_.Init();
            lim_r_.Init();
        }

        /** PORT PATCH P26: the tape's WINDOW -- where the loop turns over.
         *
         *  Until now the loop was always the whole take. It still is by
         *  default (start 0, end 1), but the window is now a real thing the
         *  read head turns over in, because that is what makes "End" a loop
         *  bound rather than a label: the reference module for this work
         *  (forgetful) keeps start/end as fractions of the take, derives them
         *  into sample positions every block, and reads the take between
         *  them.
         *
         *  Three rules are theirs, and they are the reason this is not just a
         *  subtraction:
         *
         *  1. ORDER IS ENFORCED HERE, NOT IN THE SETTER. A knob that snaps
         *     back under the hand reads as a broken control, so the two
         *     fractions are stored as written and put in order at use. THE
         *     ORDER IS THE DIRECTION (PORT PATCH P28): End below Start is the
         *     same span of tape run backwards, which is what a tape does when
         *     you cross the ends over. See the direction block below.
         *  2. THE HEAD RE-ENTERS AT THE EDGE IT IS TRAVELLING AWAY FROM, or it
         *     lands on the boundary it is about to cross and wraps every
         *     sample. Checking happens BEFORE the read, so nothing outside
         *     the window is ever played, and a head parked exactly on an edge
         *     starts moving instead of sitting there.
         *  3. THERE IS A MINIMUM SPAN, because the 480-sample end fade has to
         *     have something to fade over.
         *
         *  Window = 0..1 is the old behaviour exactly, which is what keeps
         *  every existing bench result valid. */
        inline uint32_t WindowBase() const { return win_lo_; }
        inline uint32_t WindowTop() const { return win_hi_ >= 2 ? win_hi_ - 2 : 0; }

        /** PORT PATCH P28: the window's DIRECTION -- which way round its two
         *  ends are.
         *
         *  End below Start runs the tape BACKWARDS through the span: the two
         *  knobs never swap meaning (Start is where the window starts from in
         *  time, End where it stops), and which of them is on the left decides
         *  the direction of travel. The reference module does it the same way.
         *
         *  Three things about how, and each is a rule rather than a detail:
         *
         *  - IT IS ITS OWN FIELD, never folded into the speed. The port's
         *    Loop Pitch / Loop Rev cells read back the number the user set,
         *    and the sign of that number is the direction THEY selected.
         *    Written into the varispeed, a crossed window would make those
         *    cells report a reverse nobody asked for -- and the host learns an
         *    enum's wire format from exactly that read. So the direction is
         *    +1 / -1 beside the speed, and the two MULTIPLY at the one place
         *    the tape's rate is decided (the target in PopStereoSamps): a
         *    reversed window at -1x is forward again, which is what two
         *    reversals mean.
         *
         *  - IT GLIDES THROUGH ZERO rather than flipping: a linear ramp from
         *    +1 to -1 over kWindowFlipSeconds, so crossing End past Start
         *    slows the tape, stops it, and runs it back the way a tape would,
         *    instead of cutting to the other direction at full speed. The
         *    tape's own slew (the fonepole on scrub_) then follows the ramp,
         *    as it follows every other change of speed on this instrument.
         *    0.35 s is the reference module's number: long enough not to
         *    click, short enough that the gesture and the sound belong to
         *    each other -- this one is a direct consequence of the knob under
         *    the hand, and a long lag would read as the control being broken.
         *
         *  - IT ADVANCES ONCE A BLOCK, FROM PerBlock(), WHATEVER THE TRANSPORT
         *    IS DOING. The glide runs from the knob move, not from the next
         *    play: a window crossed while the tape is parked (or empty) has
         *    turned round by the time it rolls, so the take starts in the
         *    direction the knobs say.
         *
         *  A step is re-measured from wherever the glide is when the target
         *  changes, so crossing back mid-glide turns round from there. */
        inline float GetWindowDirection() const { return window_dir_; }

        void UpdateWindowDirection(size_t frames)
        {
            const float wdir = (window_end_ < window_start_) ? -1.f : 1.f;
            if(wdir != window_dir_target_)
            {
                window_dir_target_ = wdir;
                window_dir_step_   = fabsf(wdir - window_dir_) / (kWindowFlipSeconds * sr_);
            }
            if(frames == 0 || window_dir_ == window_dir_target_)
                return;
            const float move = window_dir_step_ * static_cast<float>(frames);
            const float diff = window_dir_target_ - window_dir_;
            if(fabsf(diff) <= move || move <= 0.f)
                window_dir_ = window_dir_target_;
            else
                window_dir_ += (diff > 0.f) ? move : -move;
        }

        inline bool WindowOutside()
        {
            const uint32_t head = ram_buff.GetReadHead();
            /* THE PAIR, not the head. A stereo read takes two samples: forward
             * that is mem[head..head+1], backwards mem[head-2..head-1]. Asking
             * only whether the head has passed the edge is not enough: a scrub
             * move is a whole number of samples and can leave the head on an
             * ODD one, where it can neither satisfy >= win_hi_ nor be read as a
             * pair, so it sits one sample short of the top edge forever. That
             * is the parked-playhead stall, and it was intermittent because it
             * took an odd landing to trigger it. */
            return reverse_ ? (head < win_lo_ + 2) : (head + 2 > win_hi_);
        }

        inline void ReenterWindow() { ReadJump(reverse_ ? WindowTop() : WindowBase()); }

        /** What is left of the window in the direction the tape is running --
         *  the window's answer to RamBuffer::GetRemainingRead, and the thing
         *  the end fade counts down. */
        inline size_t GetRemainingWindow(bool rev)
        {
            const uint32_t head = ram_buff.GetReadHead();
            if(!rev)
                return head < win_hi_ ? (win_hi_ - head) : 0;
            return head > win_lo_ ? (head - win_lo_) : 0;
        }

        /** Derive the window in samples from the two fractions. Once a block,
         *  like the reference module: cheap, and it means a take being
         *  recorded or a changed fraction is picked up without every sample
         *  doing the arithmetic.
         *
         *  `frames` is the block just about to be rendered, and it is what the
         *  direction glide advances by (PORT PATCH P28). The setters call this
         *  with 0: they re-derive the bounds and re-aim the glide at once, and
         *  leave the advancing to the audio thread. */
        void PerBlock(size_t frames = 0)
        {
            /* The direction first, and before the size test below: an empty
             * take has no bounds to derive, but the glide still has to run,
             * so the first take recorded into a crossed window plays
             * backwards from its first pass. */
            UpdateWindowDirection(frames);

            /* in samples, kept even so a stereo pair stays a pair */
            const uint32_t size = static_cast<uint32_t>(ram_buff.GetSize()) & ~1u;
            if(size < kMinWindowSamples)
            {
                win_lo_ = 0;
                win_hi_ = size;
                return;
            }

            uint32_t lo = static_cast<uint32_t>(window_start_ * static_cast<float>(size)) & ~1u;
            uint32_t hi = static_cast<uint32_t>(window_end_ * static_cast<float>(size)) & ~1u;
            if(hi < lo)
            {
                const uint32_t t = lo;
                lo = hi;
                hi = t;
            }
            if(hi > size)
                hi = size;
            if(hi - lo < kMinWindowSamples)
                hi = lo + kMinWindowSamples;
            if(hi > size)
            {
                hi = size;
                lo = hi - kMinWindowSamples;
            }

            win_lo_ = lo;
            win_hi_ = hi;
        }

        /* The control's own range is clamped here; the RELATIONSHIP between
         * the two is not (see the note above). */
        void SetWindowStart(float f)
        {
            window_start_ = f < 0.f ? 0.f : (f > 1.f ? 1.f : f);
            PerBlock();
        }

        void SetWindowEnd(float f)
        {
            window_end_ = f < 0.f ? 0.f : (f > 1.f ? 1.f : f);
            PerBlock();
        }

        inline float GetWindowStart() const { return window_start_; }
        inline float GetWindowEnd() const { return window_end_; }

        void ReadJumpStart()
        {
            /* PORT PATCH P26: "start" means the edge the tape is playing AWAY
             * from -- the tail of the window when it runs backwards, the head
             * of it when it runs forwards. */
            ReadJump(reverse_ ? WindowTop() : WindowBase());
        }

        void WriteJumpStart()
        {
            if(reverse_)
                WriteJump(ram_buff.GetSize());
            else
                WriteJump(0);
        }

        inline void ReadJump(uint32_t pos = 0) { ram_buff.SetReadHead(pos); }

        void WriteJump(size_t pos)
        {
            ram_buff.SetWriteHead(pos);
        }

        /** Marks current buffer as invalid, and requests new data at beginning of file */
        void JumpTo(uint32_t pos = 0, bool clear_read = true)
        {
            ram_buff.SetWriteHead(pos);
            ram_buff.SetReadHead(pos);
        }

        void JumpToStart()
        {
            ReadJumpStart();
            WriteJumpStart();
        }

        inline void Reset() { reset = true; }    
        inline bool IsResetting() { return reset; }

        uint16_t write_left_a, write_right_a;
        void ReadyToRecord()
        {
            write_left_a = write_right_a = 0;
        }

        int16_t last_l, last_r;

        /** returns true if a sample was popped */
        bool old_rec = false;
        void PopStereoSamps(int16_t inl, int16_t inr, int16_t *out_l, int16_t *out_r, bool recording, bool playing)
        {
            if(!old_rec && recording) // rising edge
            {
                input_env_dec = .001f;
                old_samps_l.Clear();
                old_samps_r.Clear();
                rev_pushback = true; // poorly named var imo
                ram_buff.SetWriteHead(ram_buff.GetReadHead());
            }
            else if(!recording) // falling edge
            {
                input_env_dec = -.001f;
            }

            old_rec = recording;

            {
                float target = playing ? varispeed_factor : scrub_target_;
                /* PORT PATCH P28: the window's own direction, on top of the
                 * knob's. This is the one place the two meet, and they meet
                 * as a product: the varispeed keeps the sign the user set
                 * (which is what the port reads back), and a crossed window
                 * turns the result round -- through zero, because the glide
                 * is a ramp and the slew below follows it. */
                target *= window_dir_;
                if(reset)
                    target = 0.f;

                
                daisysp::fonepole(scrub_, target, tape_slew_ ? .0001f : .01f);
                scrub_ = daisysp::fclamp(scrub_, -2.f, 2.f);

                if((scrub_ < 0.f && target < 0.f) || (scrub_ > 0.f && target > 0.f))
                {
                    // rev_check = 512;
                    if(SetReverse(scrub_ < 0.f))
                    {
                        *out_l = last_l;
                        *out_r = last_r;
                        return;
                    }
                }

                if(ram_buff.GetSize() == 0)
                {
                    reset = false;
                }

                const float abs_scrub = fabsf(scrub_);
                if(reset && abs_scrub < 0.05f)
                {
                    *out_l = last_l * reset_env;
                    *out_r = last_r * reset_env;
                    reset_env -= .0001f;
                    if(reset_env <= 0.f)
                    {
                        loop_env = 0.f;
                        loop_env_dec = loop_env_dec_val; // ~5ms
                        scrub_ = scrub_target_ = 0.f;
                        SetVarispeed(1.f, true);
                        ram_buff.Reset();
                        ForceSetScrub(1.f);
                        old_samps_l.Clear();
                        old_samps_r.Clear();
                        *out_l = *out_r = 0;
                        reset = false;
                        reset_env = 1.f;
                    }

                    return;
                }
                else if(!reset)
                {
                    reset_env += .005f;

                    if(reset_env > 1.f)
                        reset_env = 1.f;
                }

                old_rpos_frac = rpos_frac_;
                rpos_frac_ += abs_scrub;
                if(turn_period_count_ > kTurnPeriodTimeout)
                {
                    if(!tape_slew_)
                    {
                        if(turn_count_ > 0.f)
                            turn_count_ = 5.f * varispeed_factor;
                        else if(turn_count_ < 0.f)
                            turn_count_ = -5.f * varispeed_factor;
                    }
                    
                    scrub_target_ = turn_count_ * .2f;
                    turn_count_ = 0.f;
                    turn_period_count_ = 0;
                }
                else
                    turn_period_count_++;

                if (rpos_frac_ >= 1.f)
                {
                    /** increment that shouldn't be susceptible to f32 precision loss w/ long files*/
                    const uint32_t samp_stride_ = (uint32_t)rpos_frac_;

                    rpos_frac_ -= samp_stride_;

                    for (uint32_t i = 0; i < samp_stride_; i++)
                    {
                        al = bl;
                        ar = br;

                        /* PORT PATCH P26: turn over at the WINDOW's edge, and
                         * re-enter at the edge the head is travelling away
                         * from -- checked BEFORE the read, so no sample from
                         * outside the window is ever played, and so a head
                         * parked exactly on an edge (by a scrub, a window
                         * move, or a reverse flip) starts moving again instead
                         * of sitting there. The old post-read test asked the
                         * buffer's own ends and jumped to
                         * (reverse_ ? size : 0) -- which parks a backwards
                         * tape on the very edge it is about to cross, and is
                         * where the parked-playhead stall came from. */
                        if(WindowOutside())
                            ReenterWindow();

                        ram_buff.StereoRead(&bl, &br, reverse_);

                        if(GetRemainingWindow(reverse_) <= 480 && loop_env_dec > 0.f)
                        {
                            loop_env_dec = -1.f * loop_env_dec_val;
                        }                    

                        // stop a junk sample from being pushed after changing directions
                        if(!rev_pushback)
                        {
                            old_samps_l.PushBack(al);
                            old_samps_r.PushBack(ar);
                        }
                        rev_pushback = false;

                        loop_env += loop_env_dec;
                        /* the window's own two edges, in signed maths so a
                         * base near zero cannot underflow */
                        const int32_t head_at = static_cast<int32_t>(ram_buff.GetReadHead());
                        const bool ote = (reverse_ && head_at > static_cast<int32_t>(WindowTop()) - 50)
                                         || (!reverse_ && head_at < static_cast<int32_t>(WindowBase()) + 50);
                        if(loop_env < 0.f && ote)
                        {
                            loop_env = 0.f;
                            loop_env_dec = loop_env_dec_val;
                        }
                    }

                    const float num_samps = old_samps_l.GetNumElements() - 1.f;
                    float pos = (1.f - old_rpos_frac);
                    const float delta = 1.f / (num_samps + rpos_frac_ - old_rpos_frac);
                    while(old_samps_l.GetNumElements() > 1) // leave one sample on the stack
                    {
                        int32_t write_left = old_samps_l.PopFront() * dub_gain_;
                        int32_t write_right = old_samps_r.PopFront() * dub_gain_;

                        input_env += input_env_dec;
                        input_env = daisysp::fclamp(input_env, 0.f, 1.f);

                        if(recording || input_env > .01f)
                        {
                            daisysp::fonepole(dub_gain_, dub_gain_target_, .0001f);

                            const float lerp_val = pos * delta;
                            write_left += (old_inl + (inl - old_inl) * lerp_val) * input_env;
                            write_right += (old_inr + (inr - old_inr) * lerp_val) * input_env;

                            write_left  = f2s16(lim_l_.ProcessHard(s162f(write_left)));
                            write_right = f2s16(lim_r_.ProcessHard(s162f(write_right)));

                            pos += 1.f;
                            ram_buff.StereoWrite(write_left, write_right, reverse_, false);

                            if(ram_buff.WriteLoop(reverse_))
                            {
                                WriteJumpStart();
                            }
                        }
                        else if(ram_buff.GetSize())
                            ram_buff.AdvanceWrite(reverse_);
                    }
                }

                loop_env = daisysp::fclamp(loop_env, 0.f, 1.f);

                rev_env += rev_env_dec; // rev_env_dec should be positive at this point
                rev_env = daisysp::fclamp(rev_env, 0.f, 1.f);

                /** linear interpolation */
                const float tl = al + (bl - al) * rpos_frac_;
                const float tr = ar + (br - ar) * rpos_frac_;

                old_inl = inl;
                old_inr = inr;

                // Amp env
                *out_l = static_cast<int16_t>(tl * loop_env * rev_env * reset_env);
                last_l = *out_l;
                *out_r = static_cast<int16_t>(tr * loop_env * rev_env * reset_env);
                last_r = *out_r;
            }
        }

        // only used for first recording. no varispeed
        inline void PushStereoSamps(int16_t l, int16_t r) { ram_buff.StereoWrite(l, r, reverse_, true); }

        /** @brief sets the rate at which to read from the file
         *  @param speed a value relative to original speed
         *      (e.g. 1.0 for original, 2.0 for double, 0.5 for half)
         *      TODO: negative values will be reverse (unless we setup a diff. mechanism for that).
         */
        void SetVarispeed(float speed, bool force = false)
        {
            varispeed_factor = speed;

            if(force)
            {
                SetReverse(speed < 0.f);
            }
        }

        inline float GetVarispeed() { return varispeed_factor; }

        // reverse functions only used internally (except getter)
        bool SetReverse(bool rev) 
        { 
            const bool change = rev != reverse_;
            if(change)
            {
                old_samps_l.Clear();
                old_samps_r.Clear();
                
                rev_pushback = true;
                WriteJump(ram_buff.GetReadHead());
    
                reverse_ = rev;
            }

            return change;
        }
        inline bool GetReverse() { return reverse_; }


        inline void ForceSetScrub(float s) { scrub_ = s; }
        inline void SetScrub(float addl) { turn_count_ += addl; }
        inline float GetScrub() { return scrub_; }

        /** PORT PATCH P25: move the READ head, and wrap it at the loop's ends.
         *
         *  This is the other thing you can do to a tape: not change its speed
         *  (SetScrub above, which the hardware wheel drives and which keeps
         *  running while the tape does), but put the playhead somewhere else.
         *  The port's Scrub knob asks for this, and reads back where the head
         *  landed -- a loop is a circle, so a move past either end comes round
         *  to the other side instead of stopping.
         *
         *  A move is a SPLICE: the audio steps to an unrelated part of the
         *  waveform, and on a loud loop that step is audible as a click. That
         *  is what a tape's playhead does; tools/looper_probe measures it. */
        void MoveReadHead(int32_t delta)
        {
            const int32_t size = static_cast<int32_t>(ram_buff.GetSize());
            if(size <= 1)
                return; /* nothing recorded: there is no head to move */

            int32_t pos = static_cast<int32_t>(ram_buff.GetReadHead()) + delta;
            pos %= size;
            if(pos < 0)
                pos += size;
            /* Samples, not frames: a head left on an odd sample is a head the
             * stereo read cannot take a pair at, and (see WindowOutside) one
             * that can sit one sample short of the window's top edge forever.
             * Every other way the head moves -- StereoRead, ReadJumpStart --
             * moves it in pairs, so a move has to land on one. */
            pos &= ~1;
            ram_buff.SetReadHead(static_cast<size_t>(pos));
        }

        inline uint32_t GetReadTell() { return ram_buff.GetReadHead(); }
        inline uint32_t GetReadSize() { return ram_buff.GetSize(); }

        inline bool WriteFullLength() { return ram_buff.WriteFullLength(); }

        void IncrementDubGain(float increment, bool force)
        {
            dub_gain_target_ += increment;
            dub_gain_target_ = daisysp::fclamp(dub_gain_target_, 0.f, 1.f);

            if(force)
                dub_gain_ = dub_gain_target_;
        }
        inline float GetDubGain() { return dub_gain_target_; }

        void ResetRamBuff() { ram_buff.Reset(); }

        RamBuffer ram_buff;
        volatile bool reset;
        float reset_env = 1.f;

        /** Compression for overdub feedback loop */
        chompi::Limiter lim_l_;
        chompi::Limiter lim_r_;

        /** Varispeed handling */
        float varispeed_factor;
        float rpos_frac_;

        /** PORT PATCH P26: the window, as two fractions of the take and the
         *  sample positions they derive to (see ReadJumpStart above). */
        float window_start_ = 0.f, window_end_ = 1.f;
        uint32_t win_lo_ = 0, win_hi_ = 0;
        /* ~23 ms at 44.1 kHz. The top of the window is where the 480-sample
         * end fade runs, so a window shorter than that would fade into its
         * own beginning. */
        const uint32_t kMinWindowSamples = 1024;

        /** PORT PATCH P28: the window's direction, +1 forward / -1 backwards,
         *  as a glide: where it is, where it is going, and how far it moves a
         *  sample (see UpdateWindowDirection). Its own field, never the
         *  varispeed's sign. */
        float window_dir_ = 1.f, window_dir_target_ = 1.f, window_dir_step_ = 0.f;
        /* How long the head takes to turn round when End crosses Start. The
         * reference module's figure; it passes through zero on the way. */
        const float kWindowFlipSeconds = 0.35f;

        /** reverse handling */
        bool reverse_;

        /** scrubbing */
        float scrub_, scrub_target_;
        uint32_t turn_period_count_;
        const size_t kTurnPeriodTimeout = 6000; // 1/8 second
        float turn_count_ = 0.f;

        /** cached varispeed data */
        int16_t old_inl = 0;
        int16_t old_inr = 0;
        float old_rpos_frac = 0.f;

        FIFO<int16_t, 32> old_samps_l;
        FIFO<int16_t, 32> old_samps_r;

        int16_t al, bl = 0;
        int16_t ar, br = 0;

        /** Envelope to avoid clicking over the end */
        float loop_env = 0.f;
        const float loop_env_dec_val = .00464f;
        float loop_env_dec = loop_env_dec_val;

        float input_env;
        float input_env_dec = .001f;

        float rev_env = 1.f;
        float rev_env_dec = .001f;
        float rev_pushback = true;

        float dub_gain_ = 1.f;
        float dub_gain_target_ = 1.f;

        float sr_;

        bool tape_slew_;
    };

} // namespace daisy