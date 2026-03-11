#pragma once

#include <cmath>
#include <algorithm>
#include <vector>

// KR-106 BBD Chorus Emulation
//
// Architecture (from Juno-6 schematic + measurements, March 2026):
//
// The Juno-6 chorus is a stereo BBD effect with NO dry signal path.
// Both outputs are BBD delay lines — "Mono" jack = tap 0, "Stereo" jack = tap 1.
// A single triangle-wave LFO drives both MN3009 BBD clocks in antiphase:
//   tap0 clock = center_clock + lfo(t) * depth
//   tap1 clock = center_clock - lfo(t) * depth
//
// Mode switching (SW4/SW5) selects resistor values in the LFO circuit,
// changing rate and depth simultaneously. From schematic annotations:
//   I:    triangle, 20 Vpp, 2.5s period (0.4 Hz)
//   II:   triangle, 20 Vpp, 1.5s period (0.67 Hz)
//   I+II: sine,     2.6 Vpp, 124ms period (8 Hz) — vibrato character
//
// Measured parameters (Chorus I, 496 Hz sine, Juno-6 serial# unknown):
//   LFO rate:  0.45 Hz (confirmed triangle, antiphase at -179°)
//   Mod depth: ±3.2 ms (averaged L/R)
//   BBD gain:  ~3-5 dB over dry level
//   BBD bandwidth: gentle rolloff, 1-pole fit at ~14 kHz (-3 dB at ~10 kHz)
//     This is the combined response of the anti-aliasing filter, the BBD's
//     inherent sinc rolloff from sample-and-hold, and the reconstruction
//     filter. A single 1-pole models the total measured response.
//
// Mode I+II has 2.6/20 = 0.13x the Vpp of I/II, so depth ≈ ±0.42 ms.
// At 8 Hz with low depth, this is pitch vibrato, not chorus.
// The narrower stereo width measured for I+II (0.73 L/R correlation vs 0.51)
// is simply the smaller delay modulation, not a different architecture.

namespace kr106 {

// ============================================================
// LFO — triangle or sine, single oscillator
// ============================================================
struct ChorusLFO
{
  float mPhase = 0.f; // [0, 1)
  float mInc = 0.f;

  void SetRate(float hz, float sampleRate)
  {
    mInc = hz / sampleRate;
  }

  void Reset() { mPhase = 0.f; }

  // Triangle: [-1, +1], linear ramps, peak at phase 0/1
  float Triangle()
  {
    mPhase += mInc;
    if (mPhase >= 1.f) mPhase -= 1.f;
    return 1.f - 4.f * fabsf(mPhase - 0.5f);
  }

  // Sine: [-1, +1], for mode I+II (8 Hz vibrato)
  float Sine()
  {
    mPhase += mInc;
    if (mPhase >= 1.f) mPhase -= 1.f;
    return sinf(2.f * static_cast<float>(M_PI) * mPhase);
  }
};

// ============================================================
// 1-pole TPT lowpass
// ============================================================
struct TPT1
{
  float mS = 0.f;

  void Reset() { mS = 0.f; }

  float Process(float input, float g)
  {
    float v = (input - mS) * g / (1.f + g);
    float lp = mS + v;
    mS = lp + v;
    return lp;
  }
};

// ============================================================
// BBD delay line — one MN3009 signal path
// ============================================================
struct BBDLine
{
  std::vector<float> mBuf;
  int mMask = 0;
  int mWPos = 0;

  // Single 1-pole at ~14 kHz models the total measured BBD response:
  // anti-aliasing + sinc rolloff + reconstruction combined.
  // Measurement: flat to 4 kHz, -3 dB at 10 kHz, -3.5 dB at 15 kHz.
  // Best fit: 1-pole at 14 kHz (RMSE 0.92 dB over 2–18 kHz).
  TPT1 mBBDFilter;
  float mFilterG = 0.f;

  static constexpr float kBBDFilterHz = 14000.f;

  float mSampleRate = 44100.f;

  void Init(float sampleRate)
  {
    mSampleRate = sampleRate;

    // Buffer: max delay ~10ms + interpolation margin
    int minLen = static_cast<int>(sampleRate * 0.012f) + 4;
    int len = 1;
    while (len < minLen) len <<= 1;
    mBuf.assign(len, 0.f);
    mMask = len - 1;
    mWPos = 0;

    float fc = std::min(kBBDFilterHz, sampleRate * 0.45f);
    mFilterG = tanf(static_cast<float>(M_PI) * fc / sampleRate);

    mBBDFilter.Reset();
  }

  void Clear()
  {
    std::fill(mBuf.begin(), mBuf.end(), 0.f);
    mWPos = 0;
    mBBDFilter.Reset();
  }

  // 4-point Hermite interpolation for fractional delay
  static float Hermite(float frac, float y0, float y1, float y2, float y3)
  {
    float c0 = y1;
    float c1 = 0.5f * (y2 - y0);
    float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
    float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * frac + c2) * frac + c1) * frac + c0;
  }

  float ReadHermite(float delaySamples) const
  {
    float rPos = static_cast<float>(mWPos) - delaySamples;
    if (rPos < 0.f) rPos += static_cast<float>(mMask + 1);

    int i1 = static_cast<int>(rPos);
    float frac = rPos - static_cast<float>(i1);

    return Hermite(frac,
      mBuf[(i1 - 1) & mMask],
      mBuf[i1 & mMask],
      mBuf[(i1 + 1) & mMask],
      mBuf[(i1 + 2) & mMask]);
  }

  float Process(float input, float delaySamples)
  {
    mBuf[mWPos & mMask] = input;
    float wet = ReadHermite(delaySamples);
    mWPos = (mWPos + 1) & mMask;

    // BBD bandwidth filter
    wet = mBBDFilter.Process(wet, mFilterG);

    // NaN guard
    if (!(mBBDFilter.mS > -100.f && mBBDFilter.mS < 100.f))
      mBBDFilter.mS = 0.f;

    return wet;
  }
};

// ============================================================
// Stereo Chorus
// ============================================================
struct Chorus
{
  BBDLine mLine0, mLine1;
  ChorusLFO mLFO;          // single LFO — L gets +output, R gets -output
  int mMode = 0;           // 0=off, 1=I, 2=II, 3=I+II
  bool mUseSine = false;   // true for mode I+II (8 Hz vibrato)
  float mSampleRate = 44100.f;

  // From schematic annotations + measurement:
  //
  // Center delay: nominal 3.0 ms (typical MN3009 chorus operating point).
  // Gives BBD clock ~43 kHz, Nyquist ~10.7 kHz — consistent with
  // measured -3 dB at ~10 kHz.
  static constexpr float kCenterDelayMs = 3.0f;

  // Mode I: measured 0.45 Hz, schematic says 0.4 Hz / 2.5s.
  // Mode II: schematic says 0.67 Hz / 1.5s.
  // Both at 20 Vpp — same depth.
  static constexpr float kChorusIRate  = 0.45f;
  static constexpr float kChorusIIRate = 0.67f;

  // Mode I depth: measured ±3.2 ms (average of L and R).
  // Mode II: same 20 Vpp → same depth.
  static constexpr float kChorusIDepth  = 3.2f;
  static constexpr float kChorusIIDepth = 3.2f;

  // Mode I+II: 8 Hz sine, 2.6 Vpp = 0.13x of 20 Vpp → ±0.42 ms.
  // This is pitch vibrato, not traditional chorus.
  static constexpr float kChorusI_IIRate  = 8.0f;
  static constexpr float kChorusI_IIDepth = 0.42f;

  // BBD adds ~3-5 dB gain. Measured wet level / dry level ≈ 1.5x.
  static constexpr float kBBDGain = 1.5f;

  // Crossfade for mode switching (avoids clicks)
  static constexpr float kFadeMs = 5.f;
  float mFade = 0.f;
  float mFadeTarget = 0.f;
  float mFadeInc = 0.f;

  // Smoothed depth for mode transitions
  float mDepth = 0.f;
  float mTargetDepth = 0.f;

  void Init(float sampleRate)
  {
    mSampleRate = sampleRate;
    mLine0.Init(sampleRate);
    mLine1.Init(sampleRate);
    mLFO.Reset();

    mFadeInc = 1.f / (kFadeMs * 0.001f * sampleRate);

    if (mMode > 0)
    {
      ConfigureMode();
      mDepth = mTargetDepth;
      mFade = mFadeTarget = 1.f;
    }
  }

  void Clear()
  {
    mLine0.Clear();
    mLine1.Clear();
    mLFO.Reset();
  }

  void SetMode(int newMode)
  {
    if (newMode == mMode) return;

    int oldMode = mMode;
    mMode = newMode;

    if (mMode == 0)
    {
      mFadeTarget = 0.f;
      return;
    }

    if (oldMode == 0)
    {
      mLine0.Clear();
      mLine1.Clear();
      mLFO.Reset();
    }

    ConfigureMode();
    mFadeTarget = 1.f;
  }

  // Both outputs are BBD taps — no dry signal.
  // The Juno-6 "Mono" and "Stereo" jacks are the two BBD outputs.
  void Process(float input, float& outL, float& outR)
  {
    // Crossfade
    if (mFade < mFadeTarget)
      mFade = std::min(mFade + mFadeInc, mFadeTarget);
    else if (mFade > mFadeTarget)
      mFade = std::max(mFade - mFadeInc, mFadeTarget);

    if (mFade <= 0.f)
    {
      outL = outR = input;
      return;
    }

    // Smooth depth
    mDepth += (mTargetDepth - mDepth) * mFadeInc;

    // Single LFO, antiphase for L/R
    float lfo = mUseSine ? mLFO.Sine() : mLFO.Triangle();

    float delay0ms = kCenterDelayMs + mDepth * lfo;
    float delay1ms = kCenterDelayMs - mDepth * lfo; // antiphase: single LFO inverted

    float delay0samp = delay0ms * 0.001f * mSampleRate;
    float delay1samp = delay1ms * 0.001f * mSampleRate;

    // Clamp to positive delay (can't read the future)
    delay0samp = std::max(delay0samp, 1.f);
    delay1samp = std::max(delay1samp, 1.f);

    float wet0 = mLine0.Process(input, delay0samp) * kBBDGain;
    float wet1 = mLine1.Process(input, delay1samp) * kBBDGain;

    // Crossfade between dry bypass and chorus
    outL = input + mFade * (wet0 - input);
    outR = input + mFade * (wet1 - input);
  }

private:
  void ConfigureMode()
  {
    switch (mMode)
    {
      case 1:
        mLFO.SetRate(kChorusIRate, mSampleRate);
        mTargetDepth = kChorusIDepth;
        mUseSine = false;
        break;
      case 2:
        mLFO.SetRate(kChorusIIRate, mSampleRate);
        mTargetDepth = kChorusIIDepth;
        mUseSine = false;
        break;
      case 3:
        mLFO.SetRate(kChorusI_IIRate, mSampleRate);
        mTargetDepth = kChorusI_IIDepth;
        mUseSine = true;
        break;
    }
  }
};

} // namespace kr106
