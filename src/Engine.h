#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "LoopBuffer.h"
#include "LoopReader.h"
#include "Snapshot.h"

// M3 core: record -> playback with always-on feedback decay, non-destructive
// playback edits (Reverse / Speed / Retrigger), and destructive edits (Overdub /
// Substitute) that ride the Snapshot copy-on-write layer, with one-deep Undo.
// One mono voice.
class Engine
{
  public:
    enum class State
    {
        EMPTY,
        REC_FIRST,
        PLAYING,
    };

    void Init(LoopBuffer* loop, Snapshot* snap, float sample_rate)
    {
        loop_       = loop;
        snap_       = snap;
        sr_         = sample_rate;
        state_      = State::EMPTY;
        wpos_       = 0.0;
        len_        = 0;
        fb_         = 1.0f;
        decay_pos_  = 0;
        od_         = false;
        sub_        = false;
        sub_commit_ = false;
        reader_.Init();
    }

    // ---- performance controls ----------------------------------------------
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
            reader_.JumpLocal(0.0); // window start
    }

    // startNorm, lenNorm in 0..1 (knob + summed CV). lenNorm at max -> window
    // off. Length is exponential from kMinWin to the whole loop.
    void SetWindow(float startNorm, float lenNorm)
    {
        if(state_ != State::PLAYING
           || len_ < (size_t)(2 * LoopReader::kMinWin))
            return;
        double wlen;
        if(lenNorm >= 0.98f)
            wlen = (double)len_;
        else
        {
            double f = (double)LoopReader::kMinWin
                       * powf((float)len_ / (float)LoopReader::kMinWin, lenNorm);
            if(f < (double)LoopReader::kMinWin)
                f = (double)LoopReader::kMinWin;
            if(f > (double)len_)
                f = (double)len_;
            wlen = f;
        }
        reader_.SetWindow((double)startNorm * (double)len_, wlen);
    }

    // Overdub: additive, always destructive+undoable. engaged = latch|hold|gate.
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

    // Substitute: replace. commit_intent true (latched) -> keep + snapshot for
    // Undo; false (held / clocked gate) -> revert on release.
    void Substitute(bool engaged, bool commit_intent)
    {
        if(state_ != State::PLAYING)
        {
            sub_ = false;
            return;
        }
        if(engaged && !sub_)
        {
            snap_->BeginTake();
            sub_        = true;
            sub_commit_ = false;
        }
        if(engaged)
            sub_commit_ |= commit_intent;
        if(!engaged && sub_)
        {
            if(sub_commit_)
                snap_->Commit();
            else
                snap_->RevertTake(*loop_);
            sub_ = false;
        }
    }

    void Undo()
    {
        if(state_ == State::PLAYING && snap_->CanUndo())
            snap_->UndoRedo(*loop_);
    }

    // EMPTY -> REC_FIRST -> PLAYING -> EMPTY
    void TrigRecord()
    {
        switch(state_)
        {
            case State::EMPTY:
                wpos_  = 0.0;
                len_   = 0;
                state_ = State::REC_FIRST;
                break;
            case State::REC_FIRST:
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
                break;
            case State::PLAYING:
                od_ = sub_ = false;
                state_     = State::EMPTY;
                break;
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
                    len_       = loop_->Capacity() - 1;
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

                // Always-on recirculation decay at a unit-rate cursor.
                if(fb_ < kFreezeThresh)
                {
                    loop_->Write(decay_pos_, loop_->ReadRaw(decay_pos_) * fb_);
                    if(++decay_pos_ >= len_)
                        decay_pos_ = 0;
                }

                // Destructive edits write at the read pointer (so Reverse lays
                // reversed material, off-speed lands at the moved position).
                if((od_ || sub_) && rp < len_)
                {
                    snap_->Touch(*loop_, rp);
                    float v = sub_ ? in : (loop_->ReadRaw(rp) + in);
                    loop_->Write(rp, v);
                }

                return in + sig;
            }
        }
        return in;
    }

    // ---- UI accessors ----------------------------------------------------
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

  private:
    static constexpr size_t kMinLen       = 480;
    static constexpr float  kFreezeThresh = 0.999f;

    LoopBuffer* loop_ = nullptr;
    Snapshot*   snap_ = nullptr;
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
};
