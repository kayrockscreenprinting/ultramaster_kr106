#pragma once

#include <cmath>
#include <algorithm>
#include <vector>

// KR-106 BBD Chorus Emulation
//
// Architecture (from Juno-6 schematic + measurements, March 2026):
//
// The Juno-6 chorus uses two BBD delay lines mixed with dry signal.
// Each output jack has an IC6 inverting summer combining dry (100K/47K)
// and wet (100K/39K). "Mono" jack = dry + tap 0, "Stereo" jack = dry + tap 1.
// A single triangle-wave LFO drives both MN3009 BBD clock VCOs in antiphase:
//   tap0 clock = center_clock + lfo(t) * clock_depth
//   tap1 clock = center_clock - lfo(t) * clock_depth
//
// Delay is an emergent inverse property of clock frequency:
//   delay = N_stages / (2 * f_clock)
// This produces a hyperbolic sweep in delay space — the delay lingers at
// long values (closely-spaced comb notches, deeper cancellation) and sweeps
// quickly through short delays. A symmetric triangle in clock-frequency
// space becomes asymmetric in delay-time space.
//
// Mode switching (SW4/SW5) selects resistor values in the LFO circuit,
// changing rate and clock excursion simultaneously. From schematic:
//   I:    triangle, 20 Vpp, 2.5s period (0.4 Hz)
//   II:   triangle, 20 Vpp, 1.5s period (0.67 Hz)
//   I+II: sine,     2.6 Vpp, 124ms period (8 Hz) — vibrato character
//
// Measured parameters (Chorus I, 496 Hz sine, Juno-6 serial# unknown):
//   LFO rate:  0.45 Hz (confirmed triangle, antiphase at -179°)
//   BBD gain:  ~3-5 dB over dry level
//   BBD bandwidth: gentle rolloff (-3 dB at ~10 kHz)
//
// Anti-aliasing and reconstruction filters (Tr13/Tr14 and Tr15/Tr16):
//   Two identical 2-transistor active filter stages, one before and one
//   after each MN3009. Each stage has four RC poles:
//     Pole 1: 10,620 Hz  (R89=22K, C31=680pF — input shunt)
//     Pole 2:  8,830 Hz  (R88=22K, C33=820pF — first stage)
//     Pole 3:  7,234 Hz  (R122=10K, C52=2.2nF — MN3009 pin RC)
//     Pole 4:  4,020 Hz  (R92=22K, C34=1.8nF — interstage, dominant)
//   Post-filter is a matched mirror: R97/R98, C37(820p), C35(680p),
//   R95/R96, C38(1.8nF), C36(270p), R100(10K) — identical values.
//   Combined with the BBD Nyquist-tracking pole (clock-dependent), the
//   total rolloff is considerably steeper than -18 dB/oct.
//
// Clock depth derived from measured delay maximum (~6.2ms for modes I/II).
// Center clock ≈ 42,667 Hz (from 3.0ms center delay).
// Mode I/II: clock sweeps ±22 kHz → delay 1.98ms–6.20ms (asymmetric).
// Mode I+II: clock sweeps ±5.2 kHz → delay 2.67ms–3.42ms (vibrato).

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
// 4-pole cascaded TPT lowpass — models Tr13/Tr14 (pre) and
// Tr15/Tr16 (post) active filter stages plus the MN3009
// input/output RC from the Juno-6 chorus board.
// Identical topology and values on both sides.
//
// Pole frequencies from schematic RC pairs:
//   Pole 1: R89(22K) + C31(680pF) → 10,620 Hz  (input shunt)
//   Pole 2: R88(22K) + C33(820pF) →  8,830 Hz  (first stage)
//   Pole 3: R122(10K) + C52(2.2nF) → 7,234 Hz  (MN3009 pin RC)
//   Pole 4: R92(22K) + C34(1.8nF) →  4,020 Hz  (interstage, dominant)
// ============================================================
struct BBDFilter
{
  TPT1 mPole1, mPole2, mPole3, mPole4;
  float mG1 = 0.f, mG2 = 0.f, mG3 = 0.f, mG4 = 0.f;

  // 1 / (2 * pi * R * C)
  static constexpr float kPole1Hz = 10620.f; // R89/C31
  static constexpr float kPole2Hz =  8830.f; // R88/C33
  static constexpr float kPole3Hz =  7234.f; // R122/C52 (MN3009 pin)
  static constexpr float kPole4Hz =  4020.f; // R92/C34 (dominant)

  void Init(float sampleRate)
  {
    auto safeG = [&](float hz) {
      float fc = std::min(hz, sampleRate * 0.45f);
      return tanf(static_cast<float>(M_PI) * fc / sampleRate);
    };
    mG1 = safeG(kPole1Hz);
    mG2 = safeG(kPole2Hz);
    mG3 = safeG(kPole3Hz);
    mG4 = safeG(kPole4Hz);
    Reset();
  }

  void Reset()
  {
    mPole1.Reset();
    mPole2.Reset();
    mPole3.Reset();
    mPole4.Reset();
  }

  // Set all integrator states (for click-free bypass warmth)
  void SetState(float value)
  {
    mPole1.mS = value;
    mPole2.mS = value;
    mPole3.mS = value;
    mPole4.mS = value;
  }

  float Process(float input)
  {
    float x = mPole1.Process(input, mG1);
    x = mPole2.Process(x, mG2);
    x = mPole3.Process(x, mG3);
    x = mPole4.Process(x, mG4);
    return x;
  }
};

// ============================================================
// BBD delay line — one MN3009 signal path
// ============================================================
struct BBDLine
{
  static constexpr int kNumStages = 256; // MN3009

  std::vector<float> mBuf;
  int mMask = 0;
  int mWPos = 0;

  // 4-pole pre/post filters (matched anti-aliasing + reconstruction pair)
  BBDFilter mPreFilter;
  BBDFilter mPostFilter;

  // No BBD ZOH filter needed: the MN3009 staircase output has a
  // sinc(f/f_clock) envelope, but that only matters at ~43 kHz
  // (the MN3101 clock frequency), which is well above audio band.
  // We don't model the clock noise, so there's nothing to filter.

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

    mPreFilter.Init(sampleRate);
    mPostFilter.Init(sampleRate);
  }

  void Clear()
  {
    std::fill(mBuf.begin(), mBuf.end(), 0.f);
    mWPos = 0;
    mPreFilter.Reset();
    mPostFilter.Reset();
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
    // 1. Pre-filter (4-pole anti-aliasing: 10.6k / 8.8k / 7.2k / 4.0k Hz)
    float filtered = mPreFilter.Process(input);

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

    // 6. Post-filter (4-pole reconstruction, matched to pre-filter)
    wet = mPostFilter.Process(wet);

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

  // Center clock frequency derived from center delay:
  // f_clock = N_stages / (2 * delay_sec) = 256 / (2 * 0.003) ≈ 42,667 Hz
  static constexpr float kCenterClockHz =
      static_cast<float>(BBDLine::kNumStages) / (2.f * kCenterDelayMs * 0.001f);

  // Floor clock freq — prevents delay exceeding buffer length
  static constexpr float kMinClockHz = 5000.f;

  // Mode I: measured 0.45 Hz, schematic says 0.4 Hz / 2.5s.
  // Mode II: schematic says 0.67 Hz / 1.5s.
  // Both at 20 Vpp — same clock excursion.
  static constexpr float kChorusIRate  = 0.45f;
  static constexpr float kChorusIIRate = 0.67f;

  // Clock depth in Hz — derived from measured delay maxima.
  // LFO modulates clock VCO, delay is inverse: delay = N/(2*f_clock).
  // clock_depth = kCenterClockHz - N/(2 * delay_max_sec)
  //
  // Mode I/II: delay_max ≈ 6.2ms → clock_min ≈ 20,645 Hz
  // clock_depth ≈ 22,022 Hz; gives delay_min ≈ 1.98ms
  static constexpr float kChorusIClockDepth = kCenterClockHz
      - static_cast<float>(BBDLine::kNumStages) / (2.f * 0.0062f);
  static constexpr float kChorusIIClockDepth = kChorusIClockDepth; // same Vpp

  // Mode I+II: 8 Hz sine, 2.6 Vpp = 0.13x of 20 Vpp.
  // delay_max ≈ 3.42ms → clock_depth ≈ 5,240 Hz
  // Pitch vibrato, not traditional chorus.
  static constexpr float kChorusI_IIRate  = 8.0f;
  static constexpr float kChorusI_IIClockDepth = kCenterClockHz
      - static_cast<float>(BBDLine::kNumStages) / (2.f * 0.00342f);

  // Dry/wet mix from schematic: IC6 inverting summer per channel.
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

  // Smoothed clock depth (Hz) for mode transitions
  float mClockDepth = 0.f;
  float mTargetClockDepth = 0.f;

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
      mClockDepth = mTargetClockDepth;
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
        mClockDepth = mTargetClockDepth;
        mFadeTarget = 1.f;
      }
    }

    // Always write to delay lines and keep filter state warm
    // so chorus engages without clicks.
    if (mFade <= 0.f)
    {
      if (mLine0.mBuf.empty()) { outL = outR = input; return; }
      mLine0.mBuf[mLine0.mWPos & mLine0.mMask] = input;
      mLine0.mWPos = (mLine0.mWPos + 1) & mLine0.mMask;
      mLine1.mBuf[mLine1.mWPos & mLine1.mMask] = input;
      mLine1.mWPos = (mLine1.mWPos + 1) & mLine1.mMask;
      mLine0.mPreFilter.SetState(input);
      mLine0.mPostFilter.SetState(input);
      mLine1.mPreFilter.SetState(input);
      mLine1.mPostFilter.SetState(input);
      // Keep LFO running so phase is arbitrary on engage (no click)
      mLFO.mPhase += mLFO.mInc;
      if (mLFO.mPhase >= 1.f) mLFO.mPhase -= 1.f;
      outL = outR = input;
      return;
    }

    // Smooth clock depth
    mClockDepth += (mTargetClockDepth - mClockDepth) * mFadeInc;

    // Single LFO, antiphase for L/R
    float lfo = mUseSine ? mLFO.Sine() : mLFO.Triangle();

    // LFO modulates clock frequency — physically correct for BBD.
    // Delay is the inverse: delay = N_stages / (2 * f_clock).
    // This produces a hyperbolic sweep in delay space: lingers at
    // long delays (deep comb notches) and zips through short delays.
    float clock0 = kCenterClockHz + mClockDepth * lfo;
    float clock1 = kCenterClockHz - mClockDepth * lfo;
    clock0 = std::max(clock0, kMinClockHz);
    clock1 = std::max(clock1, kMinClockHz);

    constexpr float kHalfStages = static_cast<float>(BBDLine::kNumStages) * 0.5f;
    float delay0samp = kHalfStages / clock0 * mSampleRate;
    float delay1samp = kHalfStages / clock1 * mSampleRate;

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
        mTargetClockDepth = kChorusIClockDepth;
        mUseSine = false;
        break;
      case 2:
        mLFO.SetRate(kChorusIIRate, mSampleRate);
        mTargetClockDepth = kChorusIIClockDepth;
        mUseSine = false;
        break;
      case 3:
        mLFO.SetRate(kChorusI_IIRate, mSampleRate);
        mTargetClockDepth = kChorusI_IIClockDepth;
        mUseSine = true;
        break;
    }
  }
};

} // namespace kr106
