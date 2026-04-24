#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>

// PolyBLEP oscillators modeled on the Juno-6/106 DCO
//
// Saw:   naive ramp + polyBLEP at reset
// Pulse: comparator on saw phase + polyBLEP at both edges
// Sub:   flip-flop toggled on saw reset + polyBLEP
// Noise: mixed at voice level from shared source (see Voice.h)
//
// All three oscillators derive from a single phase accumulator,
// matching the hardware where saw, pulse, and sub are all generated
// from the same capacitor ramp circuit. Phase relationships are locked:
// the saw reset, pulse falling edge, and sub toggle all coincide.

namespace kr106 {

// J6/J60: BA662 VCA per waveform, calibrated from Juno-6 recordings
static constexpr float kSawAmpJ6    = 0.5f;      // SAW is 0v to 12v
static constexpr float kPulseAmpJ6  = 0.5f;      // PULSE is 0v to +12v TL074
static constexpr float kSubAmpJ6    = 0.5942f;   // SUB at +1.5 dB over saw (J6 measurement)
static constexpr float kNoiseAmpJ6  = 1.f;

// J106: MC5534 pre-mixed saw/pulse, separate sub and noise mixing resistors.
// Calibrated from hardware 106_calibration recording (peak-to-peak matched
// to noise RMS at HPF flat, VCF wide open).
// Sub/pulse ratio: 1.51x from osc_calibrate recording (both square waves,
// ratio is recording-gain-independent).
static constexpr float kSawAmpJ106    = 0.6f;
static constexpr float kPulseAmpJ106  = 0.5f;
static constexpr float kSubAmpJ106    = 0.75f;
static constexpr float kNoiseAmpJ106  = 1.2f;

static constexpr float kSwitchRamp = 1.f / 64.f; // ~1.5ms at 44.1k (overwritten by Init)

// 4th-order polyBLEP residual — smooths a discontinuity of height 2.
// 4-sample window (2 before + 2 after the transition).
// Derived from the integrated cubic B-spline.
// t: phase position [0, 1)
// dt: phase increment per sample (freq / sampleRate)
inline float PolyBLEP(float t, float dt) {
  float dt2 = dt + dt;
  if (t < dt) {
    // 0..1 samples after transition
    float n = t / dt;
    float n2 = n * n;
    return 0.25f * n2 * n2 - 0.66666667f * n2 * n
           + 1.33333333f * n - 1.f;
  } else if (t < dt2) {
    // 1..2 samples after transition
    float u = 1.f - (t - dt) / dt;
    float u2 = u * u;
    return -0.08333333f * u2 * u2;        // -(1/12)(1-n)^4
  } else if (t > 1.f - dt) {
    // 0..1 samples before next transition
    float n = (t - 1.f) / dt;
    float n2 = n * n;
    return -0.25f * n2 * n2 - 0.66666667f * n2 * n
           + 1.33333333f * n + 1.f;
  } else if (t > 1.f - dt2) {
    // 1..2 samples before next transition
    float u = 1.f + (t - 1.f + dt) / dt;
    float u2 = u * u;
    return 0.08333333f * u2 * u2;         // (1/12)(1+n)^4
  }
  return 0.f;
}

struct Oscillators {
  float mPos = 0.f;       // phase accumulator [0, 1)
  bool mSubState = false; // flip-flop for sub oscillator
  float mSawGain = 1.f;   // crossfade gains for pop-free switching
  float mPulseGain = 1.f;
  float mSubGain = 0.f;
  float mSwitchRamp = kSwitchRamp; // rate-corrected waveform switch ramp (set by Init)

  // Pulse duty cycle: J6 vs J106
  //
  // Both circuits generate the pulse by comparing the saw ramp against a PWM CV
  // threshold. The pulse is LOW while the saw is on one side of the threshold,
  // HIGH on the other. Both produce 50%–95% duty (always ≥50% HIGH).
  //
  // J6:  Saw falls 0V → −12V. Comparator has saw on (−), PWM on (+).
  //      Output is LOW while saw > threshold (start of ramp near 0V),
  //      flips HIGH when saw falls below threshold.
  //      → The narrow LOW portion is at the START of the falling ramp.
  //
  // J106: Saw rises ~0V → +V. Comparator (TL074-B) has saw on (+), PWM on (−).
  //       Output is HIGH while saw > threshold (end of ramp near peak),
  //       LOW while saw < threshold (start of ramp near 0V).
  //       → The narrow LOW portion is at the END of the rising ramp.
  //
  // The saw direction is inverted (falling vs rising) and the comparator inputs
  // are swapped ((−)/(+) vs (+)/(−)), but these two inversions do NOT cancel.
  // They cancel for the polarity of the output (both start LOW, go HIGH), but
  // the narrow LOW dip sits at opposite ends of the ramp:
  //
  //   J6:  |_|‾‾‾‾‾‾‾|  narrow at start, where saw begins falling from 0V
  //   J106: |‾‾‾‾‾‾‾|_|  narrow at end, where saw approaches peak before reset
  //
  // When saw + pulse are mixed, the pulse notch coincides with the saw reset
  // edge on the J6, but with the saw peak on the J106 — a timbral difference.
  //
  // In code, the J106 model inverts the effective pulse width: effPW = 1 - effPW.
  // All other oscillator math (saw shape, sub, noise) is identical between models.
  bool mPulseInvert = false;

  // Per-model mix levels (set by Voice when model changes).
  // Defaults to J6; Voice overwrites via direct assignment when switching models.
  float mSawAmp   = kSawAmpJ6;
  float mPulseAmp = kPulseAmpJ6;
  float mSubAmp   = kSubAmpJ6;
  float mNoiseAmp = kNoiseAmpJ6;

  // Gain threshold below which a waveform is considered silent and its
  // body is skipped. 1e-4 = -80 dB, safely below audibility. The one-pole
  // gain ramp still updates every sample so state stays consistent —
  // only the waveform computation and mix add are gated.
  static constexpr float kGainEps = 1e-4f;

  void Init(float sampleRate) {
    // ~1.45ms time constant (matches original 1/64 at 44.1 kHz)
    mSwitchRamp = 1.f - expf(-1.f / (0.00145f * sampleRate));
    Reset();
  }

  void Reset() {
    mPos = 0.f;
    mSubState = false;
    // Don't reset mSawGain/mPulseGain/mSubGain — they ramp via mSwitchRamp
    // and resetting to 0 on every new voice causes audible fade-in artifacts
    // during rapid retriggering.
  }

  // Process one sample.
  // cps: frequency in cycles per sample (freqHz / sampleRate)
  // pulseWidth: pulse width in [0.50, 0.95] (caller scales from knob/LFO)
  // sawOn, pulseOn, subOn: waveform enable switches
  // subLevel: sub oscillator mix gain (pre-tapered via AudioTaper)
  // noiseAmp: unused (noise is mixed at voice level from a shared source)
  // sync: set true on phase wraparound (for scope sync output)
  //
  // Audio taper: 50K pot emulation (exponential curve, same for sub + noise).
  // Call once per block at the voice level; pass the result to Process.
  // J6 SUB OSC measurements (docs/J6_MEASUREMENTS/SUB_OSC.csv):
  //   AudioTaper * kSubAmp/kSawAmp matches hardware within ~0.5 dB for slider 7-10.
  //   Diverges at low settings: slider 1 is ~16 dB too loud (-33 vs -49 measured),
  //   slider 2-5 are 2-3 dB too loud. Real pot has steeper cutoff at low end.
  static float AudioTaper(float x) {
    static const float kScale = 1.f / (std::exp(3.f) - 1.f);
    return (std::exp(3.f * x) - 1.f) * kScale;
  }

  float Process(float cps, float pulseWidth, bool sawOn, bool pulseOn, bool subOn, float subLevel,
                float noiseAmp, bool &sync) {

    // --- Phase accumulator (shared by all oscillators) ---
    mPos += cps;
    sync = false;

    // Phase wraparound: saw resets, sub toggles.
    // Wrap BEFORE waveform computation so the fractional overshoot
    // (mPos after wrap) tells polyBLEP exactly where we crossed.
    if (mPos >= 1.f) {
      mPos -= 1.f;
      mSubState = !mSubState;
      sync = mSubState; // sync pulse every 2 DCO cycles (sub period)
    }

    // --- Gain ramps ---
    // Always update (cheap, keeps state consistent even for gated
    // waveforms so they resume smoothly when re-enabled).
    mSawGain   += ((sawOn   ? 1.f : 0.f) - mSawGain)   * mSwitchRamp;
    mPulseGain += ((pulseOn ? 1.f : 0.f) - mPulseGain) * mSwitchRamp;
    mSubGain   += ((subOn   ? 1.f : 0.f) - mSubGain)   * mSwitchRamp;

    // --- Shared polyBLEP at the phase reset ---
    // The saw reset, pulse falling edge, and sub toggle all coincide at
    // mPos wraparound (they're driven by the same accumulator). One BLEP
    // evaluation serves all three; sub uses the absolute value with its
    // own sign. In non-edge samples this returns 0 via early-out; in edge
    // samples we compute the polynomial once instead of three times.
    const float blepAtReset = PolyBLEP(mPos, cps);

    // --- Saw: linear ramp + polyBLEP at reset (unconditional) ---
    // Harmonic analysis of Juno-6 hardware across 8 octaves shows the real
    // oscillator ramp is very close to linear; the curvature previously
    // visible in time-domain recordings was interface AC-coupling HPF
    // draining charge on slow ramps, not the DCO itself.
    //
    // Active cost is ~3 ops (1 mult, 2 subs) — cheaper than the branch
    // that would gate it, so saw runs regardless of mSawGain.
    float saw = 2.f * mPos - 1.f;
    saw -= blepAtReset; // step at reset is 2.0

    float out = saw * mSawAmp * mSawGain;

    // --- Pulse: gated when off ---
    // Active cost is significant: PW clamp, invert branch, comparator,
    // pw2 wrap, and a non-shared PolyBLEP call at the rising edge.
    // Skipping the whole block when the gain has decayed below kGainEps
    // and pulseOn is false avoids all of that.
    //
    // Hardware maxes at ~97% duty, clamping effPW to [0.03, 0.97]
    // leaves comfortable headroom for the 4-sample polyBLEP window
    // without a frequency-dependent squeeze. If the two BLEP windows
    // overlap at high notes with narrow PW, the corrections sum
    // correctly (additive).
    if (pulseOn || (mPulseGain > kGainEps)) {
      float effPW = pulseWidth;
      if (mPulseInvert) effPW = 1.f - effPW; // J106: inverted duty cycle
      effPW = std::clamp(effPW, 0.03f, 0.97f);
      float pulse = (mPos < effPW) ? -1.f : 1.f;
      pulse -= (1.f - 2.f * effPW);   // DC correction: subtract the mean
      pulse -= blepAtReset;              // falling edge at reset (shared)
      float pw2 = mPos - effPW;
      if (pw2 < 0.f) pw2 += 1.f;
      pulse += PolyBLEP(pw2, cps);       // rising edge at PW crossing
      out += pulse * mPulseAmp * mPulseGain;
    }

    // --- Sub: gated when off ---
    // Half-frequency square wave, phase-locked to saw reset (CD4013 flip-flop).
    // fabsf corrects for alternating transition directions: the pre-transition
    // window smooths toward the upcoming (opposite) step, and the post-transition
    // window smooths the step that just happened. Both push toward the midpoint.
    // Factored form: subSign * (1 - fabsf(blep)) is equivalent to the original
    // s - fabsf(b)*s but computes the sign multiply once.
    if (subOn || (mSubGain > kGainEps)) {
      const float subSign = mSubState ? -1.f : 1.f;
      float sub = subSign * (1.f - fabsf(blepAtReset));
      out += sub * mSubAmp * subLevel * mSubGain;
    }

    // Linear sum. Verified against Juno-6 hardware: square+saw measured
    // -14.0dBFS vs -13.1dBFS predicted coherent sum — only 0.9dB of
    // compression at two-oscillator levels. The per-harmonic differences
    // (6-10dB below predicted) are phase cancellation from the shared
    // phase accumulator, not mixer saturation. The VCF input OTA
    // provides soft clipping downstream when driven harder.
    //
    // Noise is mixed at the voice level from a shared source
    // (single generator for all voices, matching real hardware).
    (void)noiseAmp;

    return out;
  }
};

} // namespace kr106
