#pragma once
#include <cstddef>
#include <cstdint>

// int16 loop store with a 4-point Hermite interpolated read.
//
// Process path is float; storage is int16 (half the SDRAM, half the bandwidth).
// The backing memory is supplied by the caller — the SDRAM array lives in
// Palimpsest.cpp so this class stays allocation-free and testable.
class LoopBuffer
{
  public:
    void Init(int16_t* mem, size_t capacity)
    {
        buf_      = mem;
        capacity_ = capacity;
    }

    size_t Capacity() const { return capacity_; }

    // One-time clear. SDRAM contents are undefined at boot; call once in main().
    void ClearTo(size_t n)
    {
        if(n > capacity_)
            n = capacity_;
        for(size_t i = 0; i < n; i++)
            buf_[i] = 0;
    }

    static inline float S2F(int16_t s) { return (float)s * (1.0f / 32768.0f); }

    static inline int16_t F2S(float f)
    {
        if(f > 1.0f)
            f = 1.0f;
        if(f < -1.0f)
            f = -1.0f;
        return (int16_t)(f * 32767.0f);
    }

    inline float ReadRaw(size_t i) const { return S2F(buf_[i]); }
    inline void  Write(size_t i, float v) { buf_[i] = F2S(v); }

    // Bit-exact int16 access for the snapshot layer.
    inline int16_t Raw(size_t i) const { return buf_[i]; }
    inline void    SetRaw(size_t i, int16_t v) { buf_[i] = v; }

    // 4-point, 3rd-order Hermite (x-form, Laurent de Soras), wrapping in [0,len).
    // At an integer position this returns the stored sample exactly.
    float ReadHermite(double pos, size_t len) const
    {
        int32_t L   = (int32_t)len;
        int32_t i   = (int32_t)pos;
        float   t   = (float)(pos - (double)i);
        int32_t im1 = i - 1;
        if(im1 < 0)
            im1 += L;
        int32_t i1 = i + 1;
        if(i1 >= L)
            i1 -= L;
        int32_t i2 = i + 2;
        if(i2 >= L)
            i2 -= L;

        float xm1 = S2F(buf_[im1]);
        float x0  = S2F(buf_[i]);
        float x1  = S2F(buf_[i1]);
        float x2  = S2F(buf_[i2]);

        float c = (x1 - xm1) * 0.5f;
        float v = x0 - x1;
        float w = c + v;
        float a = w + v + (x2 - x0) * 0.5f;
        float b = w + a;
        return ((a * t - b) * t + c) * t + x0;
    }

  private:
    int16_t* buf_      = nullptr;
    size_t   capacity_ = 0;
};
