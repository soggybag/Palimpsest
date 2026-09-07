#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "LoopBuffer.h"
#include "LoopReader.h"
#include "Snapshot.h"
#include "Clock.h"

// M5 core: record / play / feedback, non-destructive edits (Reverse / Speed /
// Retrigger), destructive edits on the Snapshot COW layer (Overdub / Substitute
// / Undo), a scannable Loop Window, and clock-quantised Record / Mute /
// Substitute. One mono voice.
//
// When a clock is present, Record close/open, Mute, and Substitute engage/
// disengage are ARMED and fire on the next clock pulse. With no clock they act
// immediately (identical to M4).
class Engine
{
  public:
    enum class State
    {
        EMPTY,
        REC_FIRST,
        PLAYING,
    };

    void Init(LoopBuffer* loop, Snapshot* snap, Clock* clock, float sample_rate)
    {
        loop_  = loop;
        snap_  = snap;
        clock_ = clock;
        sr_    = sample_rate;
        state_ = State::EMPTY;
        wpos_  = 0.0;
        len_   = 0;
        fb_    = 1.0f;

        decay_pos_  = 0;
        od_         = false;
        sub_        = false;
        sub_commit_ = false;

        armed_        = Armed::NONE;
        mute_want_    = false;
        mute_arm_     = false;
        mute_target_  = false;
        mute_gain_    = 1.0f;
        sub_arm_      = false;
        sub_arm_want_ = false;
        sub_req_prev_ = false;
        cycle_flag_   = false;
        reader_.Init();
    }

    // ---- performance controls --------------------------------------------
    void SetFeedback(float fb) { fb_ = fb; }
    void SetSpeed(float mag) { reader_.SetSpeed(mag); }

    void SetReverse(bool rev)
    {
        if(state_ != State::PLAYING)
            return;
        if(rev != (reader_.Dir() < 0))
            reader_.Flip();
    }

    void Retrigger()
    {
        if(state_ == State::PLAYING)
            reader_.JumpLocal(0.0);
    }

    void SetWindow(float startNorm, float lenNorm)
    {
        if(state_ != State::PLAYING
           || len_ < (size_t)(2 * LoopReader::kMinWin))
            return;
        // rescale so the pot's real 0..~0.98 travel covers the full range and
        // the top ~8% is a hard "window off" zone (pots don't reach 1.0)
        float ln = (lenNorm - 0.02f) / 0.90f;
        if(ln >= 1.0f)
        {
            reader_.SetWindow(0.0, (double)len_); // truly off: start 0, full length
            return;
        }
        double wlen;
        {
            if(ln < 0.f)
                ln = 0.f;
            double f = (double)LoopReader::kMinWin
                       * powf((float)len_ / (float)LoopReader::kMinWin, ln);
            if(f < (double)LoopReader::kMinWin)
                f = (double)LoopReader::kMinWin;
            if(f > (double)len_)
                f = (double)len_;
            wlen = f;
        }
        // keep the window inside [0, len_) so it never straddles the recording's
        // origin (that internal seam has no crossfade -> clicks per cycle)
        double wstart   = (double)startNorm * (double)len_;
        double maxstart = (double)len_ - wlen;
        if(maxstart < 0.0)
            maxstart = 0.0;
        if(wstart > maxstart)
            wstart = maxstart;
        reader_.SetWindow(wstart, wlen);
    }

    void Overdub(bool engaged)
    {
        if(state_ != State::PLAYING)
        {
            od_ = false;
            return;
        }
        if(engaged && !od_)
        {
            snap_->BeginTake();
            od_ = true;
        }
        else if(!engaged && od_)
        {
            snap_->Commit();
            od_ = false;
        }
    }

    // Substitute: replace. commit_intent (latched) -> keep + undo point;
    // else (held / gate) -> revert on release. Engage/disengage quantise to the
    // clock when present.
    void Substitute(bool req_engaged, bool commit_intent)
    {
        if(state_ != State::PLAYING)
        {
            sub_ = false;
            return;
        }
        sub_commit_req_ = commit_intent;
        if(!(clock_ && clock_->Present()))
        {
            sub_req_prev_ = req_engaged;
            ApplySub(req_engaged);
            return;
        }
        if(req_engaged != sub_req_prev_)
        {
            sub_arm_      = true;
            sub_arm_want_ = req_engaged;
            sub_req_prev_ = req_engaged;
        }
    }

    void Undo()
    {
        if(state_ == State::PLAYING && snap_->CanUndo())
            snap_->UndoRedo(*loop_);
    }

    // Loop-output mute (input monitor unaffected). Quantised to the clock;
    // the reader keeps running so re-entry is phase-locked.
    void Mute(bool engaged)
    {
        if(state_ != State::PLAYING)
            return;
        if(engaged == mute_want_)
            return;
        mute_want_ = engaged;
        if(clock_ && clock_->Present())
            mute_arm_ = true;
        else
            mute_target_ = engaged;
    }

    // EMPTY -> REC_FIRST -> PLAYING -> EMPTY  (open/close quantise to the clock)
    void TrigRecord()
    {
        bool clk = clock_ && clock_->Present();
        switch(state_)
        {
            case State::EMPTY:
                if(armed_ == Armed::REC_START)
                    armed_ = Armed::NONE; // second tap cancels the arm
                else if(clk)
                    armed_ = Armed::REC_START;
                else
                {
                    wpos_  = 0.0;
                    len_   = 0;
                    state_ = State::REC_FIRST;
                }
                break;
            case State::REC_FIRST:
                if(armed_ == Armed::REC_CLOSE)
                    armed_ = Armed::NONE;
                else if(clk)
                    armed_ = Armed::REC_CLOSE;
                else
                    CloseNow();
                break;
            case State::PLAYING:
                od_ = sub_ = false;
                armed_       = Armed::NONE;
                mute_target_ = mute_want_ = false;
                state_       = State::EMPTY;
                break;
        }
    }

    // Call once per audio block, before Process(), when Clock::Tick() is true.
    void OnClockTick()
    {
        if(armed_ == Armed::REC_START)
        {
            wpos_  = 0.0;
            len_   = 0;
            state_ = State::REC_FIRST;
            armed_ = Armed::NONE;
        }
        else if(armed_ == Armed::REC_CLOSE && wpos_ >= (double)(sr_ * 0.5f))
        {
            // wait for at least ~0.5 s before a quantised close can fire, so a
            // fast clock can't snap the loop to a buzzy sub-second length
            CloseNow();
            armed_ = Armed::NONE;
        }
        if(mute_arm_)
        {
            mute_target_ = mute_want_;
            mute_arm_    = false;
        }
        if(sub_arm_)
        {
            ApplySub(sub_arm_want_);
            sub_arm_ = false;
        }
    }

    float Process(float in)
    {
        switch(state_)
        {
            case State::EMPTY: return in;

            case State::REC_FIRST:
            {
                size_t i = (size_t)wpos_;
                if(i < loop_->Capacity())
                    loop_->Write(i, in);
                wpos_ += 1.0;
                if((size_t)wpos_ >= loop_->Capacity())
                {
                    len_ = loop_->Capacity() - 1;
                    reader_.Init();
                    reader_.SetWindow(0.0, (double)len_);
                    decay_pos_ = 0;
                    state_     = State::PLAYING;
                }
                return in;
            }

            case State::PLAYING:
            {
                float  sig = reader_.Process(*loop_, len_);
                size_t rp  = reader_.LastIntPos();
                if(reader_.TakeWrapped())
                    cycle_flag_ = true;

                if(fb_ < kFreezeThresh)
                {
                    loop_->Write(decay_pos_, loop_->ReadRaw(decay_pos_) * fb_);
                    if(++decay_pos_ >= len_)
                        decay_pos_ = 0;
                }

                // Destructive write, placed a few samples behind the read head
                // so it never lands inside the Hermite read kernel (rp-1..rp+2).
                // Otherwise every pass the interpolation straddles a fresh write
                // vs. old content -> broadband hash over the loop.
                if((od_ || sub_) && len_ > 8)
                {
                    long wi = (long)rp + (reader_.Dir() >= 0 ? -3 : 3);
                    if(wi < 0)
                        wi += (long)len_;
                    else if(wi >= (long)len_)
                        wi -= (long)len_;
                    size_t wp = (size_t)wi;
                    snap_->Touch(*loop_, wp);
                    float v = sub_ ? in : (loop_->ReadRaw(wp) + in);
                    loop_->Write(wp, v);
                }

                float tgt = mute_target_ ? 0.f : 1.f;
                float d   = tgt - mute_gain_;
                if(d > kMuteRamp)
                    d = kMuteRamp;
                else if(d < -kMuteRamp)
                    d = -kMuteRamp;
                mute_gain_ += d;

#if PALIMPSEST_NO_MUTE_GAIN
                return in + sig;
#else
                return in + sig * mute_gain_;
#endif
            }
        }
        return in;
    }

    // ---- UI / IO accessors --------------------------------------------
    State  GetState() const { return state_; }
    float  Phase() const { return reader_.GlobalPhase(len_); }
    float  LoopSeconds() const { return (float)len_ / sr_; }
    float  WinStartPhase() const
    {
        return len_ ? (float)(reader_.WinStart() / (double)len_) : 0.f;
    }
    float WinLenPhase() const
    {
        return len_ ? (float)(reader_.WinLen() / (double)len_) : 1.f;
    }
    bool   Windowed() const { return reader_.WinLen() < (double)len_ * 0.98; }
    float  WinMs() const { return 1000.f * (float)reader_.WinLen() / sr_; }
    size_t LoopLen() const { return len_; }
    bool   Frozen() const { return fb_ >= kFreezeThresh; }
    float  SpeedRatio() const { return reader_.Speed() * (float)reader_.Dir(); }
    bool   Reversed() const { return reader_.Dir() < 0; }
    bool   Overdubbing() const { return od_; }
    bool   Substituting() const { return sub_; }
    bool   CanUndo() const { return snap_->CanUndo(); }
    bool   Undone() const { return snap_->IsUndone(); }
    bool   Muted() const { return mute_target_; }
    bool   IsArmed() const { return armed_ != Armed::NONE; }
    bool   ClockOn() const { return clock_ && clock_->Present(); }
    bool   CycleStarted()
    {
        bool c      = cycle_flag_;
        cycle_flag_ = false;
        return c;
    }
    float CyclePhase() const { return reader_.LocalPhase(); }

  private:
    enum class Armed
    {
        NONE,
        REC_START,
        REC_CLOSE,
    };

    static constexpr size_t kMinLen       = 480;
    static constexpr float  kFreezeThresh = 0.999f;
    static constexpr float  kMuteRamp     = 1.0f / 96.0f; // ~2 ms

    void CloseNow()
    {
        len_ = (size_t)wpos_;
        if(len_ < kMinLen)
        {
            state_ = State::EMPTY;
        }
        else
        {
            reader_.Init();
            reader_.SetSpeed(1.0f);
            reader_.SetWindow(0.0, (double)len_);
            decay_pos_ = 0;
            state_     = State::PLAYING;
        }
    }

    void ApplySub(bool engaged)
    {
        if(engaged && !sub_)
        {
            snap_->BeginTake();
            sub_        = true;
            sub_commit_ = false;
        }
        if(engaged)
            sub_commit_ |= sub_commit_req_;
        if(!engaged && sub_)
        {
            if(sub_commit_)
                snap_->Commit();
            else
                snap_->RevertTake(*loop_);
            sub_ = false;
        }
    }

    LoopBuffer* loop_  = nullptr;
    Snapshot*   snap_  = nullptr;
    Clock*      clock_ = nullptr;
    LoopReader  reader_;
    float       sr_    = 48000.f;
    State       state_ = State::EMPTY;

    double wpos_       = 0.0;
    size_t len_        = 0;
    float  fb_         = 1.0f;
    size_t decay_pos_  = 0;
    bool   od_         = false;
    bool   sub_        = false;
    bool   sub_commit_ = false;

    Armed armed_          = Armed::NONE;
    bool  mute_want_      = false;
    bool  mute_arm_       = false;
    bool  mute_target_    = false;
    float mute_gain_      = 1.0f;
    bool  sub_arm_        = false;
    bool  sub_arm_want_   = false;
    bool  sub_req_prev_   = false;
    bool  sub_commit_req_ = false;
    bool  cycle_flag_     = false;
};
