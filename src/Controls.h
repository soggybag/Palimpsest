#pragma once
#include <cstddef>
#include <cstdint>

// Rule 2 — "length is the verb" — as one abstraction.
//
// Every performance function has a single event source, driven by a momentary
// contact (panel button or MIDI note) and/or a gate. Two flavours:
//
//   latching  (Reverse, Overdub, Substitute, Mute):
//     tap  -> toggle latch      hold -> momentary while held     gate -> level
//     read via Engaged() / RisingEdge() / Momentary()
//
//   impulse   (Record, Retrigger, Undo):
//     every press / gate-rising -> one Trigger()
//     a release after a >= kTapMs hold (or gate-falling) -> one ReleaseAfterHold()
//     latch is never used
//
// The engine only asks the accessors; it never cares whether a hand or a cable
// did it.

enum class Func : uint8_t
{
    RECORD = 0,
    OVERDUB,
    SUBSTITUTE,
    MUTE,
    REVERSE,
    RETRIGGER,
    UNDO,
    COUNT,
};

// MIDI note per function. Middle C (60) up — unambiguous in any MIDI tool, and
// what pico_rig/main.py sends as 60 + key_index.
static constexpr uint8_t kFuncNote[(int)Func::COUNT] = {
    60, // RECORD
    61, // OVERDUB
    62, // SUBSTITUTE
    63, // MUTE
    64, // REVERSE
    65, // RETRIGGER
    66, // UNDO
};

class FuncControl
{
  public:
    void SetLatching(bool l) { latching_ = l; }

    void Press()
    {
        pressed_      = true;
        held_ms_      = 0.f;
        trig_pending_ = true;
    }

    void Release()
    {
        if(pressed_)
        {
            if(held_ms_ < kTapMs && latching_)
                latch_ = !latch_; // tap -> toggle (latching funcs only)
            if(held_ms_ >= kTapMs)
                relhold_pending_ = true; // release of a hold
        }
        pressed_ = false;
    }

    void SetGate(bool g)
    {
        if(g && !gate_)
            trig_pending_ = true; // gate rising == a press
        if(!g && gate_)
            relhold_pending_ = true; // gate falling == release of a hold
        gate_ = g;
    }

    void Tick(float dt_ms)
    {
        if(pressed_)
            held_ms_ += dt_ms;

        bool prev  = engaged_;
        engaged_   = latch_ || pressed_ || gate_;
        momentary_ = gate_ || (pressed_ && held_ms_ >= kTapMs);
        rising_    = engaged_ && !prev;
        falling_   = !engaged_ && prev;
    }

    // latching-style
    bool Engaged() const { return engaged_; }
    bool Momentary() const { return momentary_; }
    bool RisingEdge() const { return rising_; }
    bool FallingEdge() const { return falling_; }
    bool Latched() const { return latch_; }
    void ClearLatch() { latch_ = false; }

    // impulse-style (one-shot, consumed on read)
    bool Trigger()
    {
        bool t        = trig_pending_;
        trig_pending_ = false;
        return t;
    }
    bool ReleaseAfterHold()
    {
        bool t           = relhold_pending_;
        relhold_pending_ = false;
        return t;
    }

  private:
    static constexpr float kTapMs = 300.f;

    bool  latching_        = true;
    bool  pressed_         = false;
    bool  gate_            = false;
    bool  latch_           = false;
    bool  engaged_         = false;
    bool  momentary_       = false;
    bool  rising_          = false;
    bool  falling_         = false;
    bool  trig_pending_    = false;
    bool  relhold_pending_ = false;
    float held_ms_         = 0.f;
};

class Controls
{
  public:
    void Init()
    {
        f_[(int)Func::RECORD].SetLatching(false);
        f_[(int)Func::RETRIGGER].SetLatching(false);
        f_[(int)Func::UNDO].SetLatching(false);
    }

    void Note(uint8_t note, bool on)
    {
        for(int i = 0; i < (int)Func::COUNT; i++)
        {
            if(kFuncNote[i] == note)
            {
                on ? f_[i].Press() : f_[i].Release();
                return;
            }
        }
    }

    void SetGate(Func fn, bool g) { f_[(int)fn].SetGate(g); }

    void Tick(float dt_ms)
    {
        for(int i = 0; i < (int)Func::COUNT; i++)
            f_[i].Tick(dt_ms);
    }

    FuncControl&       operator[](Func fn) { return f_[(int)fn]; }
    const FuncControl& operator[](Func fn) const { return f_[(int)fn]; }

  private:
    FuncControl f_[(int)Func::COUNT];
};
