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
//   BBD bandwidth: gentle rolloff (-3 dB at ~10 kHz)
//     Modeled as three stages: 15 kHz pre-filter (anti-aliasing),
//     clock-rate-dependent BBD filter (Nyquist = 256 stages / 4×delay),
//     and 15 kHz post-filter (reconstruction). Total ~-18 dB/oct.
//     Charge-well saturation models MN3009 nonlinearity at high levels.
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
  static constexpr int kNumStages = 256;           // MN3009
  static constexpr float kFixedFilterHz = 15000.f; // pre/post anti-aliasing

  std::vector<float> mBuf;
  int mMask = 0;
  int mWPos = 0;

  // Fixed 15 kHz pre/post filters (anti-aliasing + reconstruction)
  TPT1 mPreLPF;
  TPT1 mPostLPF;
  float mFixedG = 0.f;

  // Modulated BBD-bandwidth filter (inline 1-pole TPT state)
  // Cutoff tracks BBD Nyquist = kNumStages / (4 * delay_seconds),
  // so bandwidth sweeps with the LFO: ~10 kHz at max delay, ~21 kHz at center.
  float mBBD_S1 = 0.f;

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

    float fc = std::min(kFixedFilterHz, sampleRate * 0.45f);
    mFixedG = tanf(static_cast<float>(M_PI) * fc / sampleRate);

    mPreLPF.Reset();
    mPostLPF.Reset();
    mBBD_S1 = 0.f;
  }

  void Clear()
  {
    std::fill(mBuf.begin(), mBuf.end(), 0.f);
    mWPos = 0;
    mPreLPF.Reset();
    mPostLPF.Reset();
    mBBD_S1 = 0.f;
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
    // 1. Pre-filter (anti-aliasing, 15 kHz)
    float filtered = mPreLPF.Process(input, mFixedG);

    // 2. Write to delay buffer
    mBuf[mWPos & mMask] = filtered;

    // 3. Read with Hermite interpolation
    float wet = ReadHermite(delaySamples);

    // 4. Advance write position
    mWPos = (mWPos + 1) & mMask;

    // 5. Charge-well saturation: models cumulative charge transfer
    //    nonlinearity across 256 MN3009 stages. Linear below ±0.7,
    //    gently compresses peaks above that threshold.
    if (fabsf(wet) > 0.7f)
    {
      float sign = (wet > 0.f) ? 1.f : -1.f;
      float d = fabsf(wet) - 0.7f;
      wet = sign * (0.7f + d / (1.f + 2.f * d));
    }

    // 6. Modulated BBD-bandwidth lowpass (1-pole TPT)
    //    Cutoff = BBD Nyquist = kNumStages / (4 * delay_seconds).
    //    Combined with pre/post 15 kHz filters gives ~-18 dB/oct total.
    float delaySec = delaySamples / mSampleRate;
    float bbdNyquist = static_cast<float>(kNumStages) / (4.f * delaySec);
    float nyq = mSampleRate * 0.45f;
    float cutoff = std::min(bbdNyquist, nyq);
    float gBBD = tanf(static_cast<float>(M_PI) * cutoff / mSampleRate);

    float v1 = (wet - mBBD_S1) * gBBD / (1.f + gBBD);
    float lp1 = mBBD_S1 + v1;
    mBBD_S1 = lp1 + v1;
    wet = lp1;

    // 7. Post-filter (reconstruction, 15 kHz)
    wet = mPostLPF.Process(wet, mFixedG);

    // NaN guard
    if (!(mBBD_S1 > -100.f && mBBD_S1 < 100.f)) mBBD_S1 = 0.f;

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
  int mPendingMode = 0;    // deferred mode for click-free mode-to-mode switches
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

  // Dry/wet mix from schematic: IC8 inverting summer per channel.
  //   Dry: -R70/R71 = -100K/47K = gain 2.128
  //   Wet: -R70/R72 = -100K/39K = gain 2.564
  // Measured chorus ON vs OFF: ~3-5 dB boost (using +4 dB midpoint = 1.585x).
  // Resistor ratio sets dry:wet balance, total scaled to match measured boost.
  static constexpr float kChorusBoost = 1.585f;  // +4 dB measured ON vs OFF
  static constexpr float kDryGain = 2.128f / (2.128f + 2.564f) * kChorusBoost; // 0.719
  static constexpr float kWetGain = 2.564f / (2.128f + 2.564f) * kChorusBoost; // 0.866

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
    if (newMode == mMode && newMode == mPendingMode) return;

    if (mMode > 0 && newMode > 0)
    {
      // Mode-to-mode: fade out, switch at zero, fade back in
      mPendingMode = newMode;
      mFadeTarget = 0.f;
      return;
    }

    mPendingMode = newMode;
    mMode = newMode;

    if (mMode == 0)
    {
      mFadeTarget = 0.f;
      return;
    }

    ConfigureMode();
    mFadeTarget = 1.f;
  }

  // Each channel is an inverting summer mixing dry (HPF out) + wet (BBD out).
  void Process(float input, float& outL, float& outR)
  {
    // Crossfade
    if (mFade < mFadeTarget)
      mFade = std::min(mFade + mFadeInc, mFadeTarget);
    else if (mFade > mFadeTarget)
      mFade = std::max(mFade - mFadeInc, mFadeTarget);

    // When fade reaches zero, apply any pending mode switch
    if (mFade <= 0.f && mPendingMode != mMode)
    {
      mMode = mPendingMode;
      if (mMode > 0)
      {
        ConfigureMode();
        mDepth = mTargetDepth;
        mFadeTarget = 1.f;
      }
    }

    // Always write to delay lines and keep filter state warm
    // so chorus engages without clicks.
    if (mFade <= 0.f)
    {
      mLine0.mBuf[mLine0.mWPos & mLine0.mMask] = input;
      mLine0.mWPos = (mLine0.mWPos + 1) & mLine0.mMask;
      mLine1.mBuf[mLine1.mWPos & mLine1.mMask] = input;
      mLine1.mWPos = (mLine1.mWPos + 1) & mLine1.mMask;
      mLine0.mPreLPF.mS = input;
      mLine0.mPostLPF.mS = input;
      mLine0.mBBD_S1 = input;
      mLine1.mPreLPF.mS = input;
      mLine1.mPostLPF.mS = input;
      mLine1.mBBD_S1 = input;
      // Keep LFO running so phase is arbitrary on engage (no click)
      mLFO.mPhase += mLFO.mInc;
      if (mLFO.mPhase >= 1.f) mLFO.mPhase -= 1.f;
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

    float wet0 = mLine0.Process(input, delay0samp);
    float wet1 = mLine1.Process(input, delay1samp);

    // Inverting summer: dry×(100K/47K) + wet×(100K/39K), normalized.
    // Crossfade from bypass (dry only) to schematic mix.
    float dryMix = 1.f - mFade * (1.f - kDryGain);
    float wetMix = mFade * kWetGain;
    outL = dryMix * input + wetMix * wet0;
    outR = dryMix * input + wetMix * wet1;
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
