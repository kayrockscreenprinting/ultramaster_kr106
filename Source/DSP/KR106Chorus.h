#pragma once

#include <cmath>
#include <algorithm>
#include <vector>
#include "KR106AnalogNoise.h"

// KR-106 BBD Chorus Emulation
//
// ---- Architecture ----
//
// The Juno-6 chorus uses two MN3009 BBD delay lines mixed with dry signal.
// Each output jack has an IC6 inverting summer combining dry (100K/47K)
// and wet (100K/39K). "Mono" jack = dry + tap 0, "Stereo" jack = dry + tap 1.
// A single LFO drives both BBD clock generators in antiphase, modulating
// delay time symmetrically around the center.
//
// The MN3009 is a 256-stage analog shift register; real hardware sets
// delay via an external clock per delay = N_stages / (2 * f_clock). The
// MN3101 clock generator is driven externally from a ~5-transistor V→f
// converter that takes the LFO voltage as input. The 1/f BBD relationship
// combined with the V→f circuit's nonlinearity produces a near-linear
// delay trajectory with subtle asymmetry, modeled here as a linear-in-LFO
// delay sweep plus a clock-rate-dependent gain term that captures BBD
// charge transfer efficiency loss (see "BBD gain modulation" below).
//
// ---- BBDLine implementation strategy ----
//
// The BBDLine is implemented as a Hermite-interpolated fractional delay
// line running at the host audio sample rate, NOT as a stage-by-stage
// shift register clocked at f_clk. This is a deliberate choice.
//
// A real hardware BBD is a discrete-time sampler at its clock rate, so it
// naturally produces two flavors of aliasing: "BBD-generated aliasing"
// (BGA) from sampling at f_clk, and in a digital simulation, additional
// "simulation-generated aliasing" (SGA) from the stepped sample-and-hold
// output being read at a different host rate. See:
//
//   Gabrielli, D'Angelo, Squartini, "Antialiasing in BBD Chips Using
//   BLEP", Proc. DAFx25, Ancona, 2025
//
// For guitar-pedal-style delays (f_clk = 10–25 kHz, long delay times) BGA
// is an audible, desired sonic characteristic of BBD sound. For the Juno
// chorus the situation is different: center delay ~3 ms means f_clk ~40
// kHz, so BGA sits at or above audio Nyquist and contributes essentially
// nothing audible. The characteristic Juno chorus sound comes from the
// LFO dynamics, compander noise, IC6 summing mix, the BBDFilter pre/post
// pair, clock-rate-dependent gain modulation, and the antiphase dual-BBD
// structure — all of which this fractional delay line reproduces.
//
// A shift-register implementation was trialed but introduced audible SGA
// at the short-delay end of the LFO sweep (peak f_clk ~145 kHz, far above
// host Nyquist at any common sample rate). Fixing that cleanly requires
// polyBLEP correction per the DAFx25 paper or internal oversampling — both
// feasible future work if we later build a delay/flanger plugin that
// operates at BBD clock rates where BGA matters. For Juno chorus duty the
// ring-buffer approach is correct.
//
// ---- LFO rates and delay depths ----
//
//   Chorus I   : triangle LFO, 0.393 Hz (2.55 s period), delay swing 4.66 ms pp
//   Chorus II  : triangle LFO, 0.797 Hz — inherited, not re-verified
//   Chorus I+II: sine LFO,     8.00 Hz — inherited, not re-verified
//
// Chorus I was cross-verified from clean hardware recordings at 137 Hz and
// 1 kHz via envelope autocorrelation (periods 2.543 s and 2.552 s
// respectively). Chorus II and Chorus I+II rates come from earlier
// measurements made before the current measurement hygiene and should be
// re-verified against clean recordings before treating them as final.
//
// ---- Gain allocation at IC6 summer output ----
//
//   kDry = 0.863
//   kWet = 1.257
//
// Solved from the envelope extremes of a 137 Hz sine through Chorus I on
// hardware J6, using env_max = A*(kDry+kWet) and env_min = A*|kDry−kWet|.
// The total gain sum kDry + kWet ≈ 2.12 matches measured hardware within
// 0.1 dB, but the distribution does NOT match the raw schematic resistor
// ratio (39K/47K = 1.205): hardware's wet path is ~21% stronger relative
// to dry than the resistor ratio alone predicts. Likely causes include
// unmodeled dry-path loss in the summer input network or gain staging in
// the BBD chain we don't currently model (emitter-follower insertion loss,
// charge-transfer loss, etc). The measured values are used directly rather
// than attempting to factor them into physical sub-components.
//
// ---- BBD gain modulation ----
//
// The MN3009 loses a small fraction of charge per transfer; the cumulative
// loss over 256 stages grows as the clock slows (longer hold time per
// stage → more substrate/junction leakage per hop). This produces an
// LFO-rate amplitude modulation on each wet path: louder at short delays
// (fast clock, full transfer efficiency) and quieter at long delays (slow
// clock, accumulated leakage). Modeled as a linear-in-LFO gain term
// calibrated to reproduce the hardware envelope peak-ratio asymmetry at
// the Chorus I delay swing; scaled to other modes in proportion to their
// delay depth via mGainModScale, since a smaller sweep produces a smaller
// clock-rate range and therefore proportionally smaller gain variation.
//
// ---- Anti-aliasing and reconstruction filters ----
//
// Tr13/Tr14 (pre) and Tr15/Tr16 (post): two identical 2-transistor active
// filter stages (2SA1015 PNP emitter followers), one before and one after
// each MN3009. See BBDFilter.h for the digital model.
//   −3 dB at 9,661 Hz, flat through 5 kHz, −22 dB/oct stopband.

namespace kr106 {

// ============================================================
// LFO — triangle or sine, single oscillator
// ============================================================
struct ChorusLFO {
  float mPhase = 0.f; // [0, 1)
  float mInc = 0.f;

  void SetRate(float hz, float sampleRate) { mInc = hz / sampleRate; }

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

// BBD pre/post filter — see BBDFilter.h for implementation details.
#include "BBDFilter.h"

// ============================================================
// BBD delay line — one MN3009 signal path
//
// Ring-buffer Hermite-interpolated fractional delay running at the host
// audio sample rate. Models the BBD as a smooth fractional delay line
// with matched pre/post filters. Does not reproduce BBD-generated
// aliasing (BGA) — see header comment for rationale.
// ============================================================
struct BBDLine {
  static constexpr int kNumStages = 256; // MN3009 (for reference; not used internally)

  // Power-of-two ring buffer sized for up to ~20 ms of delay at the
  // host sample rate, with Hermite tap margin.
  std::vector<float> mBuf;
  int mMask = 0;
  int mWPos = 0;

  // Matched anti-aliasing + reconstruction filter pair
  BBDFilter mPreFilter;
  BBDFilter mPostFilter;
  static constexpr float kBBDSatDrive = 2.58f;  // measured J6 hardware soft saturation

  float mSampleRate = 44100.f;

  void Init(float sampleRate)
  {
    mSampleRate = sampleRate;

    // Buffer: max delay ~20 ms + Hermite interpolation margin (4 taps)
    int minLen = static_cast<int>(sampleRate * 0.020f) + 4;
    int len = 1;
    while (len < minLen) len <<= 1;
    mBuf.assign(static_cast<size_t>(len), 0.f);
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
  
  // Process one audio-rate sample. delaySamples is the delay in audio
  // samples (typically derived from delay_ms * sampleRate / 1000).
  float Process(float input, float delaySamples)
  {
    // 1. Pre-filter (matched anti-aliasing stage, Tr13/Tr14 model)
   float filtered = mPreFilter.Process(input);
   float sat = tanhf(filtered * kBBDSatDrive) / kBBDSatDrive;  // BBD saturates its input

    // 2. Write to delay buffer
    mBuf[mWPos & mMask] = filtered;

    // 3. Read with Hermite interpolation at fractional position
    float wet = ReadHermite(delaySamples);

    // 4. Advance write position
    mWPos = (mWPos + 1) & mMask;

    // 5. Post-filter (matched reconstruction stage, Tr15/Tr16 model)
    float out = mPostFilter.Process(wet);

    return out;
  }
};

// ============================================================
// Stereo Chorus
// ============================================================
struct Chorus {
  BBDLine mLine0, mLine1;
  ChorusLFO mLFO;
  int mMode = 0;         // 0=off, 1=I, 2=II, 3=I+II
  int mPendingMode = 0;  // deferred mode for click-free mode-to-mode switches
  bool mUseSine = false; // true for mode I+II (sine LFO vibrato)
  float mSampleRate = 44100.f;

  // Center delay: 3.30 ms. Measured directly from hardware click timing
// (single-note click patch, ~50 measurements across one LFO period,
// cross-verified via (τ_L + τ_R)/2 for the two antiphase BBDs).
// The antiphase L and R delays consistently average to this center
// across the entire LFO cycle (std < 0.04 ms).
static constexpr float kCenterDelayMs = 3.30f;   // was 3.20f

  // Floor clock — used for clock-bleed synthesis and debug logging only.
  // The delay line itself is ms-driven and doesn't need a clock floor.
  static constexpr float kMinClockHz = 5000.f;

  // Minimum delay floor — prevents Hermite read from going past the write
  // position. Needs to be larger than ~2 samples at any host rate; 0.1 ms
  // is 10 samples at 96 kHz, 4.4 at 44.1 kHz — safe headroom for Hermite.
  static constexpr float kMinDelayMs = 0.1f;

  // LFO rates. Chorus I verified from clean hardware recordings at 137 Hz
  // and 1 kHz carriers (envelope autocorrelation → 2.54–2.55 s period,
  // cross-checked). Chorus II and Chorus I+II rates are inherited from
  // earlier measurements made before the current measurement hygiene and
  // have not been re-verified against clean recordings; treat as
  // provisional until re-measured.
  static constexpr float kChorusIRate    = 0.393f; // verified J6
  static constexpr float kChorusIIRate   = 0.842f; // verified J6
  static constexpr float kChorusI_IIRate = 7.85f;  // verified J6

  // Chorus I delay swing: ±2.13 ms half-amplitude (4.26 ms pp).
  // Measured from same click data — linear triangle trajectory verified
  // (residual 16 μs RMS against linear fit, no detectable exponential
  // curvature from the V→f converter).
  static constexpr float kChorusIDelayDepthMs = 2.13f;  
  static constexpr float kChorusIIDelayDepthMs = 1.71f;

  // Delay swing half-amplitudes (pp = 2 * this). Measured from hardware
  // Hilbert analysis; C1 value is consistent with clean envelope extremes
  // at 137 Hz and 1 kHz. C2 and C1+II values inherited along with their
  // LFO rates — provisional pending re-measurement.
  static constexpr float kChorusI_IIDelayDepthMs = 0.236f; 

  // Dry and wet path gains at the IC6 summer output. Solved from hardware
  // envelope extremes of a 137 Hz sine through Chorus I on J6:
  //   env_max = A * (kDry + kWet)  →  measured 2.12
  //   env_min = A * |kDry − kWet|  →  measured 0.39
  // giving kDry = 0.863, kWet = 1.257. These do NOT match the raw IC6
  // resistor ratio (R71/R72 = 47K/39K → 1.205); hardware's wet path is
  // ~21% stronger relative to dry than the schematic alone predicts.
  // Likely causes are unmodeled dry-path loss or gain staging inside the
  // BBD chain; see the file header comment for discussion. The measured
  // values are used directly rather than trying to factor them into
  // physical sub-components. Total sum kDry + kWet ≈ 2.12 matches
  // hardware within 0.1 dB.
  static constexpr float kDryGain = 0.863f;
  static constexpr float kWetGain = 1.257f;

  // Clock-rate-dependent BBD charge transfer efficiency. Real MN3009 stages
  // lose a small fraction of charge per transfer; over 256 stages this produces
  // cumulative loss that increases as the clock slows (longer hold time per
  // stage → more leakage). At C1 extremes the effective gain varies by ~1.4 dB
  // between τ_min (fast clock, ~147 kHz) and τ_max (slow clock, ~23 kHz).
  // Modeled as a small linear-in-LFO gain term with the sign chosen so that
  // short delays (negative LFO for line0, positive for line1) produce louder
  // output. Calibrated to match the hardware peak-ratio asymmetry measured
  // via envelope extremes at 137 Hz carrier through Chorus I.
  static constexpr float kBBDGainModC1 = 0.08f; // was 0.0, was originally 0.153 with wrong sign

  // Per-BBD clock-rate trim. Real MN3009 timing networks have ±5% cap
  // and ±1% resistor tolerances; two BBDs are never identical. Matters
  // for mono summing (prevents artificial deterministic cancellations).
  // Applied in the delay domain as a ±1.5% delay-time offset (longer
  // delay = lower effective clock, matching hardware behavior).
  static constexpr float kBBDClockTrim = 0.015f; // ±1.5% → 3% between lines

  // Per-BBD transfer efficiency trim (MN3009 unit-to-unit variation)
  static constexpr float kBBDGainTrim = 0.04f; // ±4%

  // Crossfade for mode on/off (avoids clicks)
  static constexpr float kFadeMs = 5.f;
  float mFade = 0.f;
  float mFadeTarget = 0.f;
  float mFadeInc = 0.f;

  // Smoothed per-mode parameters (avoid discontinuities on mode switch)
  float mDelayDepth = 0.f;
  float mTargetDelayDepth = 0.f;
  float mGainModScale = 1.f; // scales kBBDGainModC1 by (mode_depth / C1_depth)

  // Analog noise injection (see KR106AnalogNoise.h)
  AnalogFloorNoise mWetPink0, mWetPink1;
  RailRipple mWetRipple;
  BBDClockFeedthrough mClockBleed0, mClockBleed1;
  float mAnalogMul = 1.f; // broadband noise multiplier (variance sheet)
  float mMainsMul = 1.f;  // mains ripple multiplier (variance sheet)
  float mClockMul = 1.f;  // BBD clock bleed multiplier (variance sheet)

  void Init(float sampleRate)
  {
    mSampleRate = sampleRate;
    mLine0.Init(sampleRate);
    mLine1.Init(sampleRate);
    mWetPink0.Init(sampleRate);
    mWetPink1.Init(sampleRate);
  
    mWetRipple.SetMainsHz(60.f, sampleRate); // 50.f for EU
    mWetRipple.SetAmplitudes(analog_noise::kWetRipple120, analog_noise::kWetRipple240,
                             analog_noise::kWetRipple360);
    mWetRipple.Reset();
    mClockBleed0.Init(sampleRate);
    mClockBleed1.Init(sampleRate);
    mLFO.Reset();

    mFadeInc = 1.f / (kFadeMs * 0.001f * sampleRate);

    if (mMode > 0)
    {
      ConfigureMode();
      mDelayDepth = mTargetDelayDepth;
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
    // Safety: ensure fade increment is valid (Init may not have been called yet)
    if (mFadeInc <= 0.f && mSampleRate > 0.f)
        mFadeInc = 1.f / (kFadeMs * 0.001f * mSampleRate);

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
    // Ensure mFade is above the bypass threshold so Process doesn't
    // short-circuit on the first sample after mode activation.
    if (mFade <= 0.f) mFade = mFadeInc;
  }

  // Each channel is an inverting summer mixing dry (HPF out) + wet (BBD out).
  void Process(float input, float& outL, float& outR)
  {
    // Crossfade
    if (mFade < mFadeTarget) mFade = std::min(mFade + mFadeInc, mFadeTarget);
    else if (mFade > mFadeTarget) mFade = std::max(mFade - mFadeInc, mFadeTarget);

    // Apply pending mode switch once fade hits zero
    if (mFade <= 0.f && mPendingMode != mMode)
    {
      mMode = mPendingMode;
      if (mMode > 0)
      {
        ConfigureMode();
        mDelayDepth = mTargetDelayDepth;
        mFadeTarget = 1.f;
      }
    }

    // Bypass: keep filters/LFO warm so chorus engages without clicks
    if (mFade <= 0.f)
    {
      mLine0.mPreFilter.SetState(input);
      mLine0.mPostFilter.SetState(input);
      mLine1.mPreFilter.SetState(input);
      mLine1.mPostFilter.SetState(input);
      mLFO.mPhase += mLFO.mInc;
      if (mLFO.mPhase >= 1.f) mLFO.mPhase -= 1.f;
      outL = outR = input;
      return;
    }

    // Smooth delay depth across mode transitions
    mDelayDepth += (mTargetDelayDepth - mDelayDepth) * mFadeInc;

    // Single LFO, antiphase for L/R
    float lfo = mUseSine ? mLFO.Sine() : mLFO.Triangle();

    // Delay-domain modulation (linear in LFO). Per-BBD trim is applied as
    // a small delay-time offset, matching the effect of clock-network
    // tolerance on hardware (longer delay = lower effective clock).
    float delay0Ms = (kCenterDelayMs + mDelayDepth * lfo) * (1.f - kBBDClockTrim);
    float delay1Ms = (kCenterDelayMs - mDelayDepth * lfo) * (1.f + kBBDClockTrim);

    delay0Ms = std::max(delay0Ms, kMinDelayMs);
    delay1Ms = std::max(delay1Ms, kMinDelayMs);

    // Convert delay in ms to samples for the fractional delay line
    float delay0samp = delay0Ms * 0.001f * mSampleRate;
    float delay1samp = delay1Ms * 0.001f * mSampleRate;

    // Clock rates in Hz — still needed for the BBDClockFeedthrough synth
    // and for the debug clock log. Not used to drive the delay line.
    float clock0 = static_cast<float>(BBDLine::kNumStages) / (2.f * delay0Ms * 0.001f);
    float clock1 = static_cast<float>(BBDLine::kNumStages) / (2.f * delay1Ms * 0.001f);
    clock0 = std::max(clock0, kMinClockHz);
    clock1 = std::max(clock1, kMinClockHz);

    float wet0 = mLine0.Process(input, delay0samp);
    float wet1 = mLine1.Process(input, delay1samp);

    // Physically-motivated: BBD charge transfer loss increases with stage hold time.
// Gain is normalized to unity at center delay (clock_center = 40 kHz for 3.2 ms).
// Coefficient calibrated to match the hardware peak-ratio asymmetry of 1.174
// (1.4 dB) on top of the baseline comb-geometry asymmetry.
static constexpr float kBBDCTELossCoeff = 4468.f;  // was 4.4e4, arithmetic error
static constexpr float kBBDInvClockCenter = 1.f / 40000.f;  // 3.2 ms center delay

float gain0 = (1.f + kBBDGainTrim) * (1.f - kBBDCTELossCoeff * (1.f/clock0 - kBBDInvClockCenter));
float gain1 = (1.f - kBBDGainTrim) * (1.f - kBBDCTELossCoeff * (1.f/clock1 - kBBDInvClockCenter));

    wet0 *= gain0;
    wet1 *= gain1;

    // Analog artifacts: wet-path broadband, BBD clock feedthrough,
    // and mains rail ripple (common-mode, shared rail).
    wet0 += mWetPink0.Process() * analog_noise::kWetBroadbandGain * mAnalogMul;
    wet1 += mWetPink1.Process() * analog_noise::kWetBroadbandGain * mAnalogMul;

    wet0 += mClockBleed0.Process(clock0) * analog_noise::kBBDClockGain * mClockMul;
    wet1 += mClockBleed1.Process(clock1) * analog_noise::kBBDClockGain * mClockMul;

    const float ripple = mWetRipple.Process() * mMainsMul;
    wet0 += ripple;
    wet1 += ripple;

    // Inverting summer per channel: L = dry + wet0, R = dry + wet1.
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
        mTargetDelayDepth = kChorusIDelayDepthMs;
        mGainModScale = 1.f; // C1 is the reference
        mUseSine = false;
        break;
      case 2:
        mLFO.SetRate(kChorusIIRate, mSampleRate);
        mTargetDelayDepth = kChorusIIDelayDepthMs;
        mGainModScale = kChorusIIDelayDepthMs / kChorusIDelayDepthMs;
        mUseSine = false;
        break;
      case 3:
        mLFO.SetRate(kChorusI_IIRate, mSampleRate);
        mTargetDelayDepth = kChorusI_IIDelayDepthMs;
        mGainModScale = kChorusI_IIDelayDepthMs / kChorusIDelayDepthMs;
        mUseSine = true;
        break;
    }
  }
};

} // namespace kr106
