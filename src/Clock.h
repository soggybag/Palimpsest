#pragma once
#include <cstdint>

// External clock tracker, advanced once per audio block.
//
// Detects rising edges on CLOCK IN, measures the period (lightly smoothed), runs
// a free phase accumulator [0,1) between edges with a hard resync on each edge.
// Tick() is true for the block containing an edge; Present() goes false a few
// periods after the clock stops.
class Clock
{
  public:
    void Init(float sample_rate)
    {
        sr_ = sample_rate;
        Reset();
    }

    void Reset()
    {
        period_  = 0;
        since_   = 0;
        age_     = 0;
        count_   = 0;
        iv_prev_ = 0;
        phase_   = 0.f;
        pinc_    = 0.f;
        have_    = false;
        prev_hi_ = false;
        tick_    = false;
    }

    // Call once per audio block with the current CLOCK IN level.
    void Block(bool hi, uint32_t nsamp)
    {
        tick_ = false;
        if(hi && !prev_hi_) // rising edge
        {
            uint32_t iv = since_;
            if(iv >= kMinPeriod && iv <= kMaxPeriod)
            {
                // lock only after two consecutive intervals agree within 25%
                bool agree = iv_prev_ && iv > (iv_prev_ * 3) / 4
                             && iv < (iv_prev_ * 5) / 4;
                if(agree)
                {
                    period_ = have_ ? (uint32_t)((period_ * 3 + iv) / 4) : iv;
                    pinc_   = 1.f / (float)period_;
                    have_   = true;
                }
                iv_prev_ = iv;
            }
            else
            {
                iv_prev_ = 0; // out of range -> restart the agreement test
            }
            since_ = 0;
            age_   = 0;
            phase_ = 0.f;
            if(have_)
            {
                tick_ = true;
                count_++;
            }
        }
        prev_hi_ = hi;

        since_ += nsamp;
        age_ += nsamp;
        if(have_)
        {
            phase_ += pinc_ * (float)nsamp;
            while(phase_ >= 1.f)
                phase_ -= 1.f;
        }
    }

    bool     Present() const { return have_ && period_ && age_ < kStale * period_; }
    bool     Tick() const { return tick_; }
    float    Phase() const { return phase_; }
    uint32_t Period() const { return period_; }
    uint32_t Count() const { return count_; }
    float    Bpm() const
    {
        return (have_ && period_) ? (sr_ * 60.f / (float)period_) : 0.f;
    }

  private:
    static constexpr uint32_t kMinPeriod = 480;    // 10 ms  (100 Hz clock ceiling)
    static constexpr uint32_t kMaxPeriod = 480000; // 10 s
    static constexpr uint32_t kStale     = 4;

    float    sr_     = 48000.f;
    uint32_t period_ = 0, since_ = 0, age_ = 0, count_ = 0, iv_prev_ = 0;
    float    phase_ = 0.f, pinc_ = 0.f;
    bool     have_ = false, prev_hi_ = false, tick_ = false;
};
