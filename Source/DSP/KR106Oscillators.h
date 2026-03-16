#pragma once

#include <cmath>
#include <cstdint>

// PolyBLEP oscillators modeled on the Juno-6/106 DCO
//
// Saw:   naive ramp + polyBLEP at reset
// Pulse: comparator on saw phase + polyBLEP at both edges
// Sub:   flip-flop toggled on saw reset + polyBLEP + passive LP
// Noise: LCG white noise with Gaussian approximation (CLT)
//
// All three oscillators derive from a single phase accumulator,
// matching the hardware where saw, pulse, and sub are all generated
// from the same capacitor ramp circuit. Phase relationships are locked:
// the saw reset, pulse falling edge, and sub toggle all coincide.

namespace kr106 {

static constexpr float kSawAmp = 0.5f;
static constexpr float kPulseAmp = 0.5f;
static constexpr float kSubAmp = 0.67f;
static constexpr float kSwitchRamp = 1.f / 64.f; // ~1.5ms at 44.1k

// 2nd-order polyBLEP residual — smooths a discontinuity of height 1.
// t: distance past the discontinuity in phase units [0, 1)
// dt: phase increment per sample (cps)
inline float PolyBLEP(float t, float dt) {
  if (t < dt) {
    float n = t / dt;
    return n + n - n * n - 1.f;
  } else if (t > 1.f - dt) {
    float n = (t - 1.f) / dt;
    return n * n + n + n + 1.f;
  }
  return 0.f;
}

struct Oscillators {
  float mPos = 0.f;       // phase accumulator [0, 1)
  bool mSubState = false; // flip-flop for sub oscillator
  float mSawGain = 1.f;   // crossfade gains for pop-free switching
  float mPulseGain = 1.f;
  float mSubGain = 0.f;
  uint32_t mRandSeed = 22222;
  float mBlipEnv = 0.f;      // capacitor discharge transient envelope
  float mSubLPState = 0.f;   // sub oscillator passive LP state
  float mNoiseLPState = 0.f; // noise spectral tilt LP state
  bool mPulseInvert = false; // J106: pulse is positive-first (80017A vs J6's uPD4007)

  // Ramp curvature: pos*(1+k*(1-pos)) bows the ramp slightly.
  //
  // Harmonic analysis of Juno-6 hardware across 8 octaves (35Hz–4219Hz)
  // shows the even/odd harmonic ratio is ~0dB at all pitches — the real
  // oscillator ramp is very close to linear. The dramatic curvature
  // visible in time-domain recordings is actually the audio interface's
  // AC coupling cap (highpass ~1-5Hz) draining charge during slow
  // low-frequency ramps, not oscillator nonlinearity. An RC charge
  // model (1 - exp(-t/tau)) was tested but the harmonic ratio fitter
  // found minimal curvature to fit. A small quadratic is retained for
  // subtle character. Values above ~0.1 produce audible even-harmonic
  // suppression that doesn't match the hardware measurements.
  static constexpr float kSawCurve = 0.03f;

  // Saw reset blip: when TR5 discharges C7, the circuit rings at ~20 kHz
  // (LC parasitic resonance) with Q ≈ 10, decaying in ~280 µs.
  // Measured at 384 kHz: initial spike 11.6% of ramp, total deviation 19.2%.
  // At 44.1 kHz the ring is unresolvable (2.2 samples/cycle), so we model
  // it as an energy-equivalent PolyBLEP impulse. 0.15 splits the difference
  // between spike-only and total ring energy.
  static constexpr float kBlipAmp = 0.15f;
  static constexpr float kBlipDecay = 0.5f;

  void Reset() {
    mPos = 0.f;
    mSubState = false;
    mBlipEnv = 0.f;
    mSubLPState = 0.f;
    mNoiseLPState = 0.f;
  }

  // Process one sample.
  // cps: frequency in cycles per sample (freqHz / sampleRate)
  // pulseWidth: pulse width [0.52, 0.98] (caller scales from knob/LFO)
  // sawOn, pulseOn, subOn: waveform enable switches
  // subLevel: sub oscillator mix level [0, 1]
  // noiseAmp: noise mix level [0, 1]
  // sync: set true on phase wraparound (for scope sync output)
  float Process(float cps, float pulseWidth, bool sawOn, bool pulseOn, bool subOn, float subLevel,
                float noiseAmp, bool &sync) {

    // --- Phase accumulator (shared by all oscillators) ---
    mPos += cps;
    sync = false;

    // Phase wraparound: saw resets, sub toggles, blip triggers.
    // Wrap BEFORE waveform computation so the fractional overshoot
    // (mPos after wrap) tells polyBLEP exactly where we crossed.
    if (mPos >= 1.f) {
      mPos -= 1.f;
      mSubState = !mSubState;
      sync = mSubState; // sync pulse every 2 DCO cycles (sub period)
      mBlipEnv = 1.f;
    }

    // --- Saw: curved ramp + polyBLEP + reset blip ---
    float curvedPos = mPos * (1.f + kSawCurve * (1.f - mPos));
    float saw = 2.f * curvedPos - 1.f;
    saw += PolyBLEP(mPos, cps); // step at reset is still 2.0

    mBlipEnv *= kBlipDecay;
    saw -= mBlipEnv * kBlipAmp;

    // --- Pulse: comparator on phase + polyBLEP at both edges ---
    // The hardware comparator sees the curved capacitor voltage, so
    // the PW threshold crossing shifts with curvature. Invert the
    // quadratic to find the linear phase of the crossing for polyBLEP.
    float effPW = pulseWidth / (1.f + kSawCurve * (1.f - pulseWidth));
    effPW = std::clamp(effPW, cps, 1.f - cps);
    float pulse = (mPos < effPW) ? -1.f : 1.f;
    pulse -= PolyBLEP(mPos, cps);   // falling edge at reset
    float pw2 = mPos - effPW;
    if (pw2 < 0.f)
      pw2 += 1.f;
    pulse += PolyBLEP(pw2, cps);    // rising edge at PW crossing
    if (mPulseInvert) pulse = -pulse; // J106: 80017A outputs positive-first pulse

    // --- Sub: CD4013 flip-flop + polyBLEP ---
    // Half-frequency square wave, phase-locked to saw reset.
    // Measured harmonics match ideal 1/n — no audible filtering in-band.
    float sub = mSubState ? 1.f : -1.f;
    if (sync)
      sub += PolyBLEP(mPos, cps) * (mSubState ? 1.f : -1.f);

    // --- Oscillator mixing ---
    // Pop-free crossfade when waveform switches change mid-note.
    mSawGain += ((sawOn ? 1.f : 0.f) - mSawGain) * kSwitchRamp;
    mPulseGain += ((pulseOn ? 1.f : 0.f) - mPulseGain) * kSwitchRamp;
    mSubGain += ((subOn ? 1.f : 0.f) - mSubGain) * kSwitchRamp;

    // Linear sum. Verified against Juno-6 hardware: square+saw measured
    // -14.0dBFS vs -13.1dBFS predicted coherent sum — only 0.9dB of
    // compression at two-oscillator levels. The per-harmonic differences
    // (6-10dB below predicted) are phase cancellation from the shared
    // phase accumulator, not mixer saturation. The VCF input OTA
    // provides soft clipping downstream when driven harder.
    float out = saw * kSawAmp * mSawGain + pulse * kPulseAmp * mPulseGain +
                sub * kSubAmp * subLevel * mSubGain;

    // --- Noise: 2SC945 NZ avalanche source ---
    if (noiseAmp > 0.f) {
      // Sum of 4 uniform samples → approximate Gaussian via CLT.
      float g = 0.f;
      for (int i = 0; i < 4; i++) {
        mRandSeed = mRandSeed * 196314165 + 907633515;
        g += 2.f * mRandSeed / static_cast<float>(0xFFFFFFFF) - 1.f;
      }
      float white = g * 0.5f;

      // Mixer resistor network rolls off highs (~8kHz RC lowpass).
      mNoiseLPState += 0.7f * (white - mNoiseLPState);

      // 50K Audio taper pot emulation
      noiseAmp = (std::exp(3.f * noiseAmp) - 1.f) / (std::exp(3.f) - 1.f);

      out += mNoiseLPState * noiseAmp;
    }

    return out;
  }
};

} // namespace kr106
