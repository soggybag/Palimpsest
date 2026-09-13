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
        win_start_sm_ = win_start_dz_ = 0.f;
        win_len_sm_ = win_len_dz_ = 1.f;
        win_ratio_idx_        = -1;
        win_ratio_committed_  = -1;
        win_start_committed_  = -1;
        win_top_exp_committed_ = kMinWinExp;
        reader_.Init();

        last_edit_       = LastEdit::NONE;
        tile_pending_    = false;
        tile_recapture_  = false;
        tile_cap_flag_   = false;
        tile_active_     = false;
        tile_count_      = 0;
        tile_unit_len_   = 0;
        tile_prev_len_   = 0;
        tile_prev_valid_ = false;
        tile_undone_     = false;
    }

    // Scratch memory for Crop & Tile — a separate SDRAM allocation from the main
    // loop buffer. `unit` holds the pristine cropped slice for the life of a
    // tiling session; `build` is where each new tiled result is assembled before
    // being copied into the live loop; `prev` holds the live loop's content from
    // immediately before the most recent tile commit, for one-level undo/redo.
    void InitTile(int16_t* unit,
                  size_t    unit_cap,
                  int16_t*  build,
                  size_t    build_cap,
                  int16_t*  prev,
                  size_t    prev_cap)
    {
        tile_unit_     = unit;
        tile_unit_cap_ = unit_cap;
        tile_build_    = build;
        tile_build_cap_ = build_cap;
        tile_prev_     = prev;
        tile_prev_cap_ = prev_cap;
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
        // Raw ADC has broadband jitter (amplified by the exponential length
        // curve) AND low-frequency wander in the signal band. One-pole kills the
        // former; a deadband holds the value steady against the latter until the
        // knob/CV actually moves. Otherwise the read pointer wobbles every block
        // -> hash + a flickering length readout.
        win_start_sm_ += 0.08f * (startNorm - win_start_sm_);
        win_len_sm_ += 0.08f * (lenNorm - win_len_sm_);
        if(fabsf(win_start_sm_ - win_start_dz_) > kWinDeadband)
            win_start_dz_ = win_start_sm_;
        if(fabsf(win_len_sm_ - win_len_dz_) > kWinDeadband)
            win_len_dz_ = win_len_sm_;

        // rescale so the pot's real 0..~0.98 travel covers the full range and
        // the top ~8% is a hard "window off" zone (pots don't reach 1.0)
        float ln = (win_len_dz_ - 0.02f) / 0.90f;
        win_ratio_idx_ = -1; // -1 = free-running (no clock snap)
        if(ln >= 1.0f)
        {
            win_ratio_committed_ = win_start_committed_ = -1;
            reader_.SetWindow(0.0, (double)len_); // truly off: start 0, full length
            return;
        }
        if(ln < 0.f)
            ln = 0.f;

        double wlen, wstart;
        bool   clocked = clock_ && clock_->Present();

        if(clocked)
        {
            // Length snaps to a musical ratio of the clock period; start snaps
            // to the period grid — rhythmic chopping (story 4) instead of a
            // free scan. Hysteresis on each step index (separate from, and on
            // top of, the continuous deadband above): a value sitting right at
            // a bin boundary can still tip back and forth on ordinary pot/CV
            // noise once it's quantised to discrete steps.
            //
            // The ladder's bottom is fixed at 1/8 the clock period; its top
            // grows with how many whole periods actually fit the recording
            // (capped at kMaxWinExp), so a long loop isn't stuck at a small
            // fixed max — a 30 s loop at 120 BPM can reach x32/x64, not just x8.
            double period   = (double)clock_->Period();
            double nPeriods = (double)len_ / period;
            // Hysteresis on the ladder's own ceiling (grow/shrink only past an
            // 8% margin), not just on the selected step: without it, ordinary
            // clock-period jitter sitting a loop length right at a power-of-two
            // boundary -- exactly what Crop & Tile produces -- flaps the whole
            // ladder size and forces the *selected* ratio to be clamped down
            // and sprung back up, which reads as the window randomly switching
            // between two sizes. (Also: ldexpf instead of `1 << x`, which was
            // undefined behaviour for the negative exponents kMinWinExp starts
            // at.)
            if(win_top_exp_committed_ < kMinWinExp)
                win_top_exp_committed_ = kMinWinExp;
            while(win_top_exp_committed_ < kMaxWinExp
                  && ldexpf(1.0f, win_top_exp_committed_ + 1)
                         <= (float)(nPeriods * (1.0 - kTopExpHyst)))
                win_top_exp_committed_++;
            while(win_top_exp_committed_ > kMinWinExp
                  && ldexpf(1.0f, win_top_exp_committed_)
                         > (float)(nPeriods * (1.0 + kTopExpHyst)))
                win_top_exp_committed_--;
            int topExp   = win_top_exp_committed_;
            int numSteps = topExp - kMinWinExp + 1;

            int idx = QuantHyst(ln * (float)numSteps,
                               win_ratio_committed_,
                               numSteps - 1);
            win_ratio_idx_ = idx; // 0 == kMinWinExp; label/exponent = idx+kMinWinExp

            int   exp   = idx + kMinWinExp;
            float ratio = ldexpf(1.0f, exp); // 2^exp
            wlen         = (double)ratio * period;
            if(wlen < (double)LoopReader::kMinWin)
                wlen = LoopReader::kMinWin;
            if(wlen > (double)len_)
                wlen = (double)len_;

            int nsteps = (int)nPeriods;
            if(nsteps < 1)
                nsteps = 1;
            int k = QuantHyst(win_start_dz_ * (float)nsteps,
                              win_start_committed_,
                              nsteps - 1);
            wstart = (double)k * period;
        }
        else
        {
            win_ratio_committed_ = win_start_committed_ = -1; // re-arm for next clocked entry
            double f = (double)LoopReader::kMinWin
                       * powf((float)len_ / (float)LoopReader::kMinWin, ln);
            if(f < (double)LoopReader::kMinWin)
                f = (double)LoopReader::kMinWin;
            if(f > (double)len_)
                f = (double)len_;
            wlen   = f;
            wstart = (double)win_start_dz_ * (double)len_;
        }

        // keep the window inside [0, len_) so it never straddles the recording's
        // origin (that internal seam has no crossfade -> clicks per cycle)
        double maxstart = (double)len_ - wlen;
        if(maxstart < 0.0)
            maxstart = 0.0;
        if(wstart > maxstart)
            wstart = maxstart;
        reader_.SetWindow(wstart, wlen);
    }

    // UI: -1 when free-running, else an index into the 1/8..256x ratio ladder
    // (the knob only ever reaches as far up that ladder as the loop supports).
    int         WinRatioIdx() const { return win_ratio_idx_; }
    const char* WinRatioLabel() const
    {
        static const char* kLabels[kNumWinRatios] = {"1/8",
                                                     "1/4",
                                                     "1/2",
                                                     "x1",
                                                     "x2",
                                                     "x4",
                                                     "x8",
                                                     "x16",
                                                     "x32",
                                                     "x64",
                                                     "x128",
                                                     "x256"};
        int i = win_ratio_idx_;
        if(i < 0)
            i = 0;
        if(i >= kNumWinRatios)
            i = kNumWinRatios - 1;
        return kLabels[i];
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
            last_edit_ = LastEdit::SNAPSHOT;
            od_        = false;
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

    // Undo dispatches to whichever mechanism produced the most recent commit:
    // the Snapshot COW layer (Overdub/Substitute) or a Crop & Tile swap.
    void Undo()
    {
        if(state_ != State::PLAYING)
            return;
        if(last_edit_ == LastEdit::TILE)
            UndoTile();
        else if(snap_->CanUndo())
            snap_->UndoRedo(*loop_);
    }

    // Crop & Tile: cheap to call from the audio callback — just requests the
    // work. The actual memory operation runs from the main loop (ProcessTileJob),
    // never inside the audio ISR, so it can take its time without stalling audio.
    //
    // recapture=false (tap): append one more copy of the current unit (define
    // one now if no session is active yet).
    // recapture=true (hold): (re)capture the window (or whole loop) right now
    // as a new unit, resetting the count to 1 -- lets you redefine the unit
    // at any point, e.g. after hitting the capacity ceiling with a small one.
    void Tile(bool recapture)
    {
        if(state_ == State::PLAYING)
        {
            tile_pending_   = true;
            tile_recapture_ = recapture;
        }
    }

    // Call once per main-loop iteration (never from the audio callback).
    void ProcessTileJob()
    {
        if(!tile_pending_)
            return;
        tile_pending_ = false;
        DoTileWork(tile_recapture_);
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
                ResetTileSession(); // a fresh recording invalidates any tile undo
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
                    ResetTileSession();
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

                return in + sig * mute_gain_;
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
    bool   CanUndo() const
    {
        return last_edit_ == LastEdit::TILE ? tile_prev_valid_ : snap_->CanUndo();
    }
    bool Undone() const
    {
        return last_edit_ == LastEdit::TILE ? tile_undone_ : snap_->IsUndone();
    }
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
    bool  TileAtCapacity() // one-shot: a tile tap was just rejected for space
    {
        bool c         = tile_cap_flag_;
        tile_cap_flag_ = false;
        return c;
    }

  private:
    enum class Armed
    {
        NONE,
        REC_START,
        REC_CLOSE,
    };

    // Which mechanism produced the most recent destructive commit — decides
    // what the shared Undo button reverts.
    enum class LastEdit
    {
        NONE,
        SNAPSHOT, // Overdub / Substitute, via the Snapshot COW layer
        TILE,     // Crop & Tile, via the whole-buffer swap below
    };

    static constexpr size_t kMinLen       = 480;
    static constexpr float  kFreezeThresh = 0.999f;
    static constexpr float  kMuteRamp     = 1.0f / 96.0f; // ~2 ms
    static constexpr float  kWinDeadband  = 0.004f;       // ADC-noise hysteresis

    // Clock-locked window length ladder: eighth through 8x the clock period.
    static constexpr int   kMinWinExp    = -3; // 1/8x floor, fixed
    static constexpr int   kMaxWinExp    = 8;  // 256x ceiling, fixed
    static constexpr int   kNumWinRatios = kMaxWinExp - kMinWinExp + 1; // label table size
    static constexpr float kTopExpHyst   = 0.08f; // ladder-ceiling jitter margin

    static constexpr size_t kTileXfade = 240; // ~5 ms junction crossfade

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
        ResetTileSession(); // new recording; nothing to tile-undo across it
    }

    void ResetTileSession()
    {
        tile_active_     = false;
        tile_count_      = 0;
        tile_prev_valid_ = false;
        tile_undone_     = false;
        if(last_edit_ == LastEdit::TILE)
            last_edit_ = LastEdit::NONE;
    }

    // Quantise `raw` to an integer in [0, maxIdx], holding `committed` steady
    // through a +-0.15-step hysteresis band around each boundary so ADC noise
    // right at a bin edge can't flip it back and forth.
    static int QuantHyst(float raw, int& committed, int maxIdx)
    {
        static constexpr float kHyst = 0.15f;
        if(committed < 0)
        {
            committed = (int)raw;
        }
        else if(raw > (float)committed + 1.0f + kHyst)
            committed = (int)floorf(raw - kHyst);
        else if(raw < (float)committed - kHyst)
            committed = (int)floorf(raw + kHyst);
        if(committed < 0)
            committed = 0;
        if(committed > maxIdx)
            committed = maxIdx;
        return committed;
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
            {
                snap_->Commit();
                last_edit_ = LastEdit::SNAPSHOT;
            }
            else
                snap_->RevertTake(*loop_);
            sub_ = false;
        }
    }

    static inline float EqPowF(float x)
    {
        return x <= 0.f ? 0.f : (x >= 1.f ? 1.f : sinf(1.57079633f * x));
    }

    // The actual Crop & Tile memory operation. Runs synchronously from the main
    // loop (never the audio ISR) — even the largest job is well under a second,
    // and the loop's output is muted for its duration (existing Mute mechanism,
    // saved/restored around it) so any transient inconsistency in the buffer
    // while it's being rewritten is inaudible; no per-block chunking needed.
    void DoTileWork(bool recapture)
    {
        if(state_ != State::PLAYING || !tile_unit_ || !tile_build_ || !tile_prev_)
            return;

        bool   windowed = Windowed();
        size_t srcStart = windowed ? (size_t)reader_.WinStart() : (size_t)0;
        size_t srcLen   = windowed ? (size_t)reader_.WinLen() : len_;

        if(!tile_active_ || recapture)
        {
            if(srcLen == 0 || srcLen > tile_unit_cap_)
            {
                tile_cap_flag_ = true; // slice doesn't fit the unit scratch
                return;
            }
            for(size_t i = 0; i < srcLen; i++)
                tile_unit_[i] = loop_->Raw(srcStart + i);
            tile_unit_len_ = srcLen;
            tile_count_    = 0;
            tile_active_   = true;
        }

        size_t newCount = (size_t)tile_count_ + 1;
        size_t newLen   = tile_unit_len_ * newCount;
        if(newLen > tile_build_cap_)
        {
            tile_cap_flag_ = true; // at capacity; session stays as-is
            return;
        }

        // Back up "before this commit" for one-level undo/redo.
        if(len_ <= tile_prev_cap_)
        {
            for(size_t i = 0; i < len_; i++)
                tile_prev_[i] = loop_->Raw(i);
            tile_prev_len_   = len_;
            tile_prev_valid_ = true;
        }
        else
            tile_prev_valid_ = false;
        tile_undone_ = false;

        bool savedMuteTarget = mute_target_;
        mute_target_         = true; // silence output for the transition

        size_t buildLen;
        if(tile_count_ > 0)
        {
            // `len_` is the true current length (it already reflects any
            // shrinkage from earlier joins) -- NOT tile_unit_len_*tile_count_,
            // which only holds before the first join.
            size_t oldLen = len_;
            for(size_t i = 0; i < oldLen; i++)
                tile_build_[i] = loop_->Raw(i);

            size_t xfade = kTileXfade;
            if(xfade > oldLen)
                xfade = oldLen;
            if(xfade > tile_unit_len_)
                xfade = tile_unit_len_;
            // True overlap-add: the appended unit's first `xfade` samples
            // blend into (replace) the existing tail; its remaining samples
            // follow immediately after, in the SAME loop / same index formula
            // -- writing the two halves via different offsets was the bug
            // (it left an `xfade`-sized gap of stale scratch content at the
            // join, heard as a glitch).
            for(size_t i = 0; i < tile_unit_len_; i++)
            {
                size_t di = oldLen - xfade + i;
                if(i < xfade)
                {
                    float t     = (float)i / (float)xfade; // 0..1 across the join
                    float a     = LoopBuffer::S2F(tile_build_[di]);
                    float b     = LoopBuffer::S2F(tile_unit_[i]);
                    tile_build_[di]
                        = LoopBuffer::F2S(a * EqPowF(1.f - t) + b * EqPowF(t));
                }
                else
                    tile_build_[di] = tile_unit_[i];
            }
            buildLen = oldLen + tile_unit_len_ - xfade; // the join overlaps, so
                                                        // the total is shorter
                                                        // by one xfade per join
        }
        else
        {
            for(size_t i = 0; i < tile_unit_len_; i++)
                tile_build_[i] = tile_unit_[i];
            buildLen = tile_unit_len_;
        }

        for(size_t i = 0; i < buildLen; i++)
            loop_->SetRaw(i, tile_build_[i]);
        len_        = buildLen;
        tile_count_ = (int)newCount;

        reader_.Init();
        reader_.SetSpeed(1.0f);
        reader_.SetWindow(0.0, (double)len_);
        decay_pos_ = 0;
        snap_->Reset(); // block bookkeeping no longer matches the new content
        od_ = sub_ = false;

        last_edit_   = LastEdit::TILE;
        mute_target_ = savedMuteTarget;
    }

    // Swap the live loop with the pre-commit backup (toggles undo <-> redo),
    // using the build scratch as the temporary during the exchange.
    void UndoTile()
    {
        if(!tile_prev_valid_ || !tile_build_)
            return;

        bool savedMuteTarget = mute_target_;
        mute_target_         = true;

        for(size_t i = 0; i < len_; i++)
            tile_build_[i] = loop_->Raw(i);
        size_t curLen = len_;

        for(size_t i = 0; i < tile_prev_len_; i++)
            loop_->SetRaw(i, tile_prev_[i]);
        len_ = tile_prev_len_;

        for(size_t i = 0; i < curLen; i++)
            tile_prev_[i] = tile_build_[i];
        tile_prev_len_ = curLen;
        tile_undone_   = !tile_undone_;

        reader_.Init();
        reader_.SetSpeed(1.0f);
        reader_.SetWindow(0.0, (double)len_);
        decay_pos_ = 0;

        mute_target_ = savedMuteTarget;
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
    float win_start_sm_   = 0.f;
    float win_len_sm_     = 1.f;
    float win_start_dz_   = 0.f;
    float win_len_dz_     = 1.f;
    int   win_ratio_idx_  = -1;
    int   win_ratio_committed_   = -1;
    int   win_start_committed_   = -1;
    int   win_top_exp_committed_ = kMinWinExp;
    bool  sub_arm_        = false;
    bool  sub_arm_want_   = false;
    bool  sub_req_prev_   = false;
    bool  sub_commit_req_ = false;
    bool  cycle_flag_     = false;

    LastEdit last_edit_ = LastEdit::NONE;

    // Crop & Tile scratch (owned by the caller; see InitTile()).
    int16_t* tile_unit_      = nullptr;
    size_t   tile_unit_cap_  = 0;
    int16_t* tile_build_     = nullptr;
    size_t   tile_build_cap_ = 0;
    int16_t* tile_prev_      = nullptr;
    size_t   tile_prev_cap_  = 0;

    bool   tile_pending_    = false;
    bool   tile_recapture_  = false;
    bool   tile_cap_flag_   = false;
    bool   tile_active_     = false;
    int    tile_count_      = 0;
    size_t tile_unit_len_   = 0;
    size_t tile_prev_len_   = 0;
    bool   tile_prev_valid_ = false;
    bool   tile_undone_     = false;
};
