#pragma once

#include <cmath>
#include <algorithm>
#include <vector>

// KR-106 BBD Chorus Emulation
//
// Architecture (from Juno-6 schematic + measurements, March–April 2026):
//
// The Juno-6 chorus uses two MN3009 BBD delay lines mixed with dry signal.
// Each output jack has an IC6 inverting summer combining dry (100K/47K)
// and wet (100K/39K). "Mono" jack = dry + tap 0, "Stereo" jack = dry + tap 1.
// A single LFO drives both BBD clock generators in antiphase, modulating
// delay time symmetrically around the center.
//
// The MN3009 BBD is a 256-stage analog shift register; delay is set by the
// external clock via delay = N_stages / (2 * f_clock). The MN3101 clock
// generator is driven externally from a ~5-transistor V→f converter that
// takes the LFO voltage as input. The 1/f relationship of the BBD combined
// with the V→f circuit's nonlinearity produces an empirically LINEAR
// delay trajectory — verified by Hilbert-transform analysis of 200Hz
// self-osc recordings through the chorus.
//
// For that reason this model computes delay directly from the LFO and
// derives the BBD clock from delay. The hardware's V→f circuit is
// implicitly pre-compensating for the 1/f delay math, and modeling it
// in the delay domain is simpler than trying to match the curve of the
// V→f circuit explicitly.
//
// Measured parameters (J6 hw, 200 Hz self-osc carrier, Hilbert analysis):
//   Chorus I   : triangle LFO, 0.413 Hz (2.422s period), delay swing 4.66 ms pp
//   Chorus II  : triangle LFO, 0.797 Hz (1.254s period), delay swing 4.64 ms pp
//   Chorus I+II: sine LFO,     7.86 Hz (0.127s period),  delay swing 0.40 ms pp
//
// Anti-aliasing and reconstruction filters (Tr13/Tr14 and Tr15/Tr16):
//   Two identical 2-transistor active filter stages (2SA1015 PNP emitter
//   followers), one before and one after each MN3009. See BBDFilter.h.
//   −3 dB at 9,661 Hz, flat through 5 kHz, −22 dB/oct stopband.
//
// BBD gain modulation: measured 1.36x amplitude ratio between LFO extremes
// on Chorus I. Attributed to clock-rate-dependent MN3009 transfer efficiency
// and MN3101 Vgg bias tracking. Modeled as anti-phase gain modulation on
// each wet path, scaled proportionally to delay swing (so C1+II's tiny
// excursion produces proportionally tiny gain modulation).

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
// ============================================================
struct BBDLine {
  static constexpr int kNumStages = 256; // MN3009

  // Quadratic charge-transfer nonlinearity (signal-dependent asymmetry).
  // Real MN3009 THD ~1-2%, dominated by 2nd harmonic. 0.02-0.08 plausible.
  static constexpr float kBBDNonlin = 0.00f;

  // True BBD shift register: 256 analog sample-and-hold stages.
  // Clocked at a variable rate (~20-65 kHz), not at the audio sample rate.
  float mStages[kNumStages] = {};
  int mWriteIdx = 0; // next stage to write (circular)

  // Clock accumulator: advances at 2*clockHz/sampleRate per audio sample.
  // clockHz is data-sheet f_clk; ticks happen at 2*f_clk because each
  // f_clk period advances 2 stages (two-phase BBD clocking).
  float mClockPhase = 0.f;

  // Sample-and-hold output: holds last clocked value between ticks
  float mHeldOutput = 0.f;

  // Matched anti-aliasing + reconstruction filter pair
  BBDFilter mPreFilter;
  BBDFilter mPostFilter;

  float mSampleRate = 44100.f;

  void Init(float sampleRate)
  {
    mSampleRate = sampleRate;
    mClockPhase = 0.f;
    mWriteIdx = 0;
    mHeldOutput = 0.f;
    std::fill(std::begin(mStages), std::end(mStages), 0.f);
    mPreFilter.Init(sampleRate);
    mPostFilter.Init(sampleRate);
  }

  void Clear()
  {
    std::fill(std::begin(mStages), std::end(mStages), 0.f);
    mWriteIdx = 0;
    mClockPhase = 0.f;
    mHeldOutput = 0.f;
    mPreFilter.Reset();
    mPostFilter.Reset();
  }

  // Process one audio-rate sample. clockHz is the data-sheet f_clk.
  float Process(float input, float clockHz)
  {
    float filtered = mPreFilter.Process(input);

    // Tick at 2*f_clk (two-phase BBD clocking: one f_clk period = 2 stages)
    float clockInc = (2.f * clockHz) / mSampleRate;
    mClockPhase += clockInc;

    while (mClockPhase >= 1.f)
    {
      mClockPhase -= 1.f;
      // Read oldest sample (N ticks old, about to be overwritten)
      mHeldOutput = mStages[mWriteIdx];
      // Write newest with quadratic nonlinearity
      const float x = filtered;
      mStages[mWriteIdx] = x + kBBDNonlin * x * x;
      mWriteIdx = (mWriteIdx + 1) % kNumStages;
    }

    return mPostFilter.Process(mHeldOutput);
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

  // Center delay: 3.2 ms. Derived from matching hardware C1+2/C2 RMS ratio
  // at 200 Hz (hw 0.679, solves to effective τ ≈ 3.29 ms via comb filter
  // transfer |0.719 + 0.866·e^(-jωτ)| against C2's sweep-averaged |H| ≈ 1.125).
  // Rounded to 3.2 ms. This is close to the nominal MN3009 operating point.
  static constexpr float kCenterDelayMs = 3.2f;

  // Floor clock — prevents runaway delay at extremes
  static constexpr float kMinClockHz = 5000.f;

  // LFO rates (measured from J6 hardware, Hilbert analysis of 200Hz carrier)
  static constexpr float kChorusIRate = 0.413f;   // was schematic 0.4
  static constexpr float kChorusIIRate = 0.797f;  // was schematic 0.67
  static constexpr float kChorusI_IIRate = 8.00f; // measured 7.86, close enough

  // Delay swing half-amplitudes (pp = 2 * this)
  // Measured: C1 = 4.66, C2 = 4.64, C1+II = 0.40 ms pp
  static constexpr float kChorusIDelayDepthMs = 2.33f;    // pp 4.66
  static constexpr float kChorusIIDelayDepthMs = 2.32f;   // pp 4.64
  static constexpr float kChorusI_IIDelayDepthMs = 0.15f; // pp 0.40 (vibrato)

  // Dry/wet mix from schematic: IC6 inverting summer per channel.
  //   Dry: -R70/R71 = -100K/47K = gain 2.128
  //   Wet: -R70/R72 = -100K/39K = gain 2.564
  // Measured chorus ON vs OFF: ~3-5 dB boost (+4 dB midpoint = 1.585x).
  static constexpr float kChorusBoost = 1.585f;
  static constexpr float kDryGain = 2.128f / (2.128f + 2.564f) * kChorusBoost; // 0.719
  static constexpr float kWetGain = 2.564f / (2.128f + 2.564f) * kChorusBoost; // 0.866

  // Clock-rate-dependent BBD gain. Measured 1.36x ratio on Chorus I
  // (g ≈ 0.153 → (1+g)/(1-g) ≈ 1.36). This is calibrated to the C1 delay
  // swing and is scaled automatically for other modes via mGainModScale.
  // Each BBD has its own LFO sign → gains anti-phase → LFO-rate amp pan.
  static constexpr float kBBDGainModC1 = 0.153f;

  // Per-BBD clock-rate trim. Real MN3009 timing networks have ±5% cap
  // and ±1% resistor tolerances; two BBDs are never identical. Matters
  // for mono summing (prevents artificial deterministic cancellations).
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

    // Delay-domain modulation (linear in LFO). Clock is derived from delay.
    // This matches the measured J6 trajectory, implicitly absorbing the
    // V→f circuit's pre-compensation for the 1/f BBD delay relationship.
    float delay0Ms = kCenterDelayMs + mDelayDepth * lfo;
    float delay1Ms = kCenterDelayMs - mDelayDepth * lfo;

    float clock0 = static_cast<float>(BBDLine::kNumStages) / (2.f * delay0Ms * 0.001f);
    float clock1 = static_cast<float>(BBDLine::kNumStages) / (2.f * delay1Ms * 0.001f);

    // Per-BBD clock trim (humanizes mono mix)
    clock0 *= (1.f + kBBDClockTrim);
    clock1 *= (1.f - kBBDClockTrim);

    clock0 = std::max(clock0, kMinClockHz);
    clock1 = std::max(clock1, kMinClockHz);

    float wet0 = mLine0.Process(input, clock0);
    float wet1 = mLine1.Process(input, clock1);

    // Clock-rate-dependent BBD gain (anti-phase, scaled to mode depth)
    float gMod = kBBDGainModC1 * mGainModScale;
    float gain0 = (1.f + kBBDGainTrim) * (1.f + gMod * lfo);
    float gain1 = (1.f - kBBDGainTrim) * (1.f - gMod * lfo);
    wet0 *= gain0;
    wet1 *= gain1;

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