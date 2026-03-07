#pragma once

#include <cmath>
#include <cstdint>

// PolyBLEP oscillators modeled on the KR-106 DCO
//
// Saw:   naive ramp + polyBLEP at reset — matches the DCO's capacitor ramp
// Pulse: comparator on saw + polyBLEP at both edges — matches hardware derivation
// Sub:   flip-flop toggled on saw reset + polyBLEP — matches the CD4013 divider
// Noise: LCG white noise

namespace kr106 {

static constexpr float kSawAmp = 0.5f;
static constexpr float kPulseAmp = 0.5f;
static constexpr float kSubAmp = 0.67f;
static constexpr float kSwitchRamp = 1.f / 64.f; // ~1.5ms at 44.1k

// 2nd-order polyBLEP residual — smooths a discontinuity of height 1
// t: distance past the discontinuity in phase units [0, 1)
// dt: phase increment per sample
inline float PolyBLEP(float t, float dt)
{
  if (t < dt)
  {
    // Just past the discontinuity
    float n = t / dt;
    return n + n - n * n - 1.f;
  }
  else if (t > 1.f - dt)
  {
    // Just before the discontinuity
    float n = (t - 1.f) / dt;
    return n * n + n + n + 1.f;
  }
  return 0.f;
}

struct Oscillators
{
  float mPos = 0.f;        // phase accumulator [0, 1)
  bool mSubState = false;  // flip-flop for sub oscillator
  float mSawGain = 1.f;    // crossfade gains for pop-free switching
  float mPulseGain = 1.f;
  float mSubGain = 0.f;
  uint32_t mRandSeed = 22222;
  float mBlipEnv = 0.f;    // capacitor discharge transient envelope
  float mSubLPState = 0.f; // sub oscillator passive LP state
  float mNoiseLPState = 0.f; // noise spectral tilt (~8kHz RC lowpass)

  // DCO capacitor ramp curvature: the current source charges through
  // finite output impedance, bowing the ramp upward (steeper at start,
  // flatter approaching reset). Subtle but shapes the harmonic spectrum.
  static constexpr float kSawCurve = 0.15f;

  // Capacitor discharge blip: when the reset transistor fires, the cap
  // briefly undershoots below ground before the current source recovers.
  static constexpr float kBlipAmp = 0.1f;
  static constexpr float kBlipDecay = 0.5f;

  // Sub oscillator passive RC lowpass (~4.2kHz at 44.1k).
  // The CD4013 square wave passes through coupling caps and the mixer
  // resistor network, gently rounding its edges.
  static constexpr float kSubLPCoeff = 0.45f;

  void Reset()
  {
    mPos = 0.f;
    mSubState = false;
    mBlipEnv = 0.f;
    mSubLPState = 0.f;
    mNoiseLPState = 0.f;
  }

  // Process one sample
  // cps: frequency in cycles per sample (freqHz / sampleRate)
  // pulseWidth: pulse width [0.52, 0.98] (caller should scale)
  // sawOn, pulseOn, subOn: waveform switches
  // subLevel: sub oscillator mix level [0, 1]
  // noiseAmp: noise mix level [0, 1]
  // sync: set true on phase wraparound (for scope sync)
  float Process(float cps, float pulseWidth, bool sawOn, bool pulseOn,
                bool subOn, float subLevel, float noiseAmp, bool& sync)
  {
    // Advance phase
    mPos += cps;
    sync = false;

    if (mPos >= 1.f)
    {
      mPos -= 1.f;
      mSubState = !mSubState; // flip-flop toggles on each saw cycle
      sync = mSubState;       // sync once per sub period (every 2 DCO cycles)
      mBlipEnv = 1.f;         // trigger discharge transient
    }

    // --- Saw: curved ramp + polyBLEP + reset blip ---
    // Capacitor charge curvature: pos*(1+k*(1-pos)) bows upward,
    // steeper at the start of each cycle, flatter near reset
    float curvedPos = mPos * (1.f + kSawCurve * (1.f - mPos));
    float saw = 2.f * curvedPos - 1.f;
    saw += PolyBLEP(mPos, cps); // step at reset is still 2.0

    // Discharge transient: brief undershoot after capacitor reset
    mBlipEnv *= kBlipDecay;
    saw -= mBlipEnv * kBlipAmp;

    // --- Pulse: comparator on curved ramp + polyBLEP ---
    // In hardware the comparator sees the actual capacitor voltage,
    // so the PW threshold crossing shifts with curvature. Invert the
    // curvature to find the linear phase of the crossing for polyBLEP.
    float effPW = pulseWidth / (1.f + kSawCurve * (1.f - pulseWidth));
    float pulse = (mPos < effPW) ? -1.f : 1.f;
    pulse -= PolyBLEP(mPos, cps);                          // falling edge at reset
    float pw2 = mPos - effPW;
    if (pw2 < 0.f) pw2 += 1.f;
    pulse += PolyBLEP(pw2, cps);                            // rising edge at PW crossing

    // --- Sub: flip-flop + polyBLEP + passive LP ---
    float sub = mSubState ? 1.f : -1.f;
    if (sync)
      sub += PolyBLEP(mPos, cps) * (mSubState ? 1.f : -1.f); // smooth the transition
    // RC lowpass rounding the CD4013 square wave edges
    mSubLPState += kSubLPCoeff * (sub - mSubLPState);
    sub = mSubLPState;

    // Ramp gains toward target to avoid pops on switch
    mSawGain += ((sawOn ? 1.f : 0.f) - mSawGain) * kSwitchRamp;
    mPulseGain += ((pulseOn ? 1.f : 0.f) - mPulseGain) * kSwitchRamp;
    mSubGain += ((subOn ? 1.f : 0.f) - mSubGain) * kSwitchRamp;

    float out = saw * kSawAmp * mSawGain
              + pulse * kPulseAmp * mPulseGain
              + sub * kSubAmp * subLevel * mSubGain;

    // Noise (LCG PRNG + RC lowpass for spectral tilt)
    // The hardware noise source (reverse-biased transistor) passes through
    // the mixer resistor network, rolling off highs around 8kHz.
    if (noiseAmp > 0.f)
    {
      mRandSeed = mRandSeed * 196314165 + 907633515;
      float white = 2.f * mRandSeed / static_cast<float>(0xFFFFFFFF) - 1.f;
      mNoiseLPState += 0.7f * (white - mNoiseLPState); // ~8kHz at 44.1k
      out += mNoiseLPState * noiseAmp;
    }

    return out;
  }
};

} // namespace kr106
