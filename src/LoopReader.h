#pragma once
#include <cmath>
#include <cstddef>
#include "LoopBuffer.h"

// Fractional read pointer over a window [wstart, wstart+wlen) inside a loop of
// loopLen samples. Buffer addressing wraps at loopLen; the window wraps at wlen.
//
//  - window boundary: equal-power tail->head crossfade (scaled to the window);
//    resume-offset + increment compensation keep the audible period = wlen/speed.
//  - windows shorter than kHann: a per-grain Hann envelope instead of the
//    crossfade (granular self-scanning).
//  - Reverse flip and Retrigger jump operate on the window-local position and
//    reuse the crossfade against the dying trajectory.
//
// Window off == wlen == loopLen, wstart == 0: identical to a plain loop read.
// Read-side only — nothing is written back.
class LoopReader
{
  public:
    static constexpr int kXfadeMax = 128;  // ~2.7 ms @ 48 kHz
    static constexpr int kMinWin   = 1440; // 30 ms
    static constexpr int kHann     = 2880; // 60 ms — below this: Hann, not xfade

    void Init()
    {
        lpos_   = 0.0;
        speed_  = 1.0f;
        dir_    = 1;
        jleft_  = 0;
        wstart_ = 0.0;
        wlen_   = 1.0;
    }

    void SetSpeed(float mag) { speed_ = mag < 0.f ? -mag : mag; }

    void SetWindow(double start, double wlen)
    {
        wstart_ = start;
        wlen_   = wlen < 1.0 ? 1.0 : wlen;
        if(lpos_ >= wlen_)
            lpos_ = 0.0;
    }

    float  Speed() const { return speed_; }
    int    Dir() const { return dir_; }
    size_t LastIntPos() const { return last_ipos_; }
    double WinStart() const { return wstart_; }
    double WinLen() const { return wlen_; }

    float GlobalPhase(size_t loopLen) const
    {
        if(!loopLen)
            return 0.f;
        return (float)(Wrapf(wstart_ + lpos_, (double)loopLen) / (double)loopLen);
    }

    void Flip()
    {
        Arm();
        dir_ = -dir_;
    }

    void JumpLocal(double lp)
    {
        Arm();
        lpos_ = Wrapf(lp, wlen_);
    }

    float Process(const LoopBuffer& b, size_t loopLen)
    {
        if(loopLen == 0)
            return 0.f;

        const double L = (double)loopLen;
        const double W = wlen_;

        int xf = (int)(W / 16.0);
        if(xf > kXfadeMax)
            xf = kXfadeMax;
        const bool useHann  = W < (double)kHann;
        const bool useXfade = !useHann && xf > 0 && W >= (double)(2 * xf);

        const double comp = useXfade ? (W - (double)xf) / W : 1.0;
        const double step = (double)speed_ * comp * (double)dir_;

        last_ipos_ = (size_t)Wrapf(wstart_ + lpos_, L);

        float out = ReadLocal(b, lpos_, dir_, L, W, xf, useXfade);

        if(jleft_ > 0)
        {
            float g   = (float)jleft_ / (float)kXfadeMax;
            float old = ReadLocal(b, jlpos_, jdir_, L, W, xf, useXfade);
            out       = old * EqPow(g) + out * EqPow(1.f - g);
            jlpos_    = Advance(jlpos_,
                             (double)speed_ * comp * (double)jdir_,
                             W,
                             xf,
                             useXfade);
            jleft_--;
        }

        if(useHann)
            out *= 0.5f - 0.5f * cosf(6.2831853f * (float)(lpos_ / W));

        lpos_ = Advance(lpos_, step, W, xf, useXfade);
        return out;
    }

  private:
    static inline float EqPow(float x)
    {
        return x <= 0.f ? 0.f : (x >= 1.f ? 1.f : sinf(1.57079633f * x));
    }

    static inline double Wrapf(double p, double m)
    {
        if(m <= 0.0)
            return 0.0;
        while(p >= m)
            p -= m;
        while(p < 0.0)
            p += m;
        return p;
    }

    static double Advance(double lp, double step, double W, int xf, bool useXfade)
    {
        lp += step;
        if(!useXfade)
            return Wrapf(lp, W);
        if(step >= 0.0 && lp >= W)
            lp -= (W - (double)xf);
        else if(step < 0.0 && lp < 0.0)
            lp += (W - (double)xf);
        return Wrapf(lp, W);
    }

    float ReadLocal(const LoopBuffer& b,
                    double            lp,
                    int               dir,
                    double            L,
                    double            W,
                    int               xf,
                    bool              useXfade) const
    {
        float main = b.ReadHermite(Wrapf(wstart_ + lp, L), (size_t)L);
        if(!useXfade)
            return main;

        double d = (dir >= 0) ? (W - lp) : lp;
        if(d < (double)xf)
        {
            float  t   = (float)(d / (double)xf); // 1 -> 0 toward the boundary
            double off = (dir >= 0) ? (lp - (W - (double)xf))
                                    : (lp + (W - (double)xf));
            float head = b.ReadHermite(Wrapf(wstart_ + off, L), (size_t)L);
            return main * EqPow(t) + head * EqPow(1.f - t);
        }
        return main;
    }

    void Arm()
    {
        jlpos_ = lpos_;
        jdir_  = dir_;
        jleft_ = kXfadeMax;
    }

    double lpos_   = 0.0;
    double wstart_ = 0.0;
    double wlen_   = 1.0;
    float  speed_  = 1.0f;
    int    dir_    = 1;

    int    jleft_ = 0;
    double jlpos_ = 0.0;
    int    jdir_  = 1;

    size_t last_ipos_ = 0;
};
