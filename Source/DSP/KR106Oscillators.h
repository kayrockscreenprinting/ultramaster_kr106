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

static constexpr float kSawAmp = 0.5f; // SAW is 0v to 12v
static constexpr float kPulseAmp = 0.5f; // PULSE is 0v to +12v TL074
static constexpr float kSubAmp = 0.625f; // SUB is 0v to +15v CMOS
static constexpr float kSwitchRamp = 1.f / 64.f; // ~1.5ms at 44.1k

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
  float mBlipEnv = 0.f;      // capacitor discharge transient envelope
  float mSubLPState = 0.f;   // sub oscillator passive LP state
  float mSwitchRamp = kSwitchRamp; // rate-corrected waveform switch ramp (set by Init)

  // Pulse duty cycle: J6 vs J106
  //
  // Both circuits generate the pulse by comparing the saw ramp against a PWM CV
  // threshold. The pulse is LOW while the saw is on one side of the threshold,
  // HIGH on the other. Both produce 50%–97% duty (always ≥50% HIGH).
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
//  static constexpr float kSawCurve = 0.03f;
static constexpr float kSawCurve = 0.00f;

  // Saw reset blip: when TR5 discharges C7, the circuit rings at ~20 kHz
  // (LC parasitic resonance) with Q ≈ 10, decaying in ~280 µs.
  // Measured at 384 kHz: initial spike 11.6% of ramp, total deviation 19.2%.
  // At 44.1 kHz the ring is unresolvable (2.2 samples/cycle), so we model
  // it as an energy-equivalent PolyBLEP impulse. 0.15 splits the difference
  // between spike-only and total ring energy.
  // FIXME 192 kHz measurement actually shows — a predominantly exponential decay
  // (~100µs time constant) rather than a 20 kHz ring with Q ≈ 10.
  // The 384 kHz ring data was likely a resampler artifact.
  static constexpr float kBlipAmp = 0.15f;
  static constexpr float kBlipDecay = 0.5f;

  void Init(float sampleRate) {
    // ~1.45ms time constant (matches original 1/64 at 44.1 kHz)
    mSwitchRamp = 1.f - expf(-1.f / (0.00145f * sampleRate));
    Reset();
  }

  void Reset() {
    mPos = 0.f;
    mSubState = false;
    mBlipEnv = 0.f;
    mSubLPState = 0.f;
    // Don't reset mSawGain/mPulseGain/mSubGain — they ramp via mSwitchRamp
    // and resetting to 0 on every new voice causes audible fade-in artifacts
    // during rapid retriggering.
  }

  // Process one sample.
  // cps: frequency in cycles per sample (freqHz / sampleRate)
  // pulseWidth: pulse width [0.52, 0.98] (caller scales from knob/LFO)
  // sawOn, pulseOn, subOn: waveform enable switches
  // subLevel: sub oscillator mix gain (pre-tapered via AudioTaper)
  // noiseAmp: noise mix gain (pre-tapered via AudioTaper, 0 = off)
  // sync: set true on phase wraparound (for scope sync output)
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
    saw -= PolyBLEP(mPos, cps); // step at reset is 2.0

    // Blip disabled: the 384 kHz ring data was likely a resampler artifact
    // (FIXME note above), and the raw step adds uncompensated aliasing.
    // mBlipEnv *= kBlipDecay;
    // saw -= mBlipEnv * kBlipAmp;

    // --- Pulse: comparator on phase + polyBLEP at both edges ---
    // The hardware comparator sees the curved capacitor voltage, so
    // the PW threshold crossing shifts with curvature. Invert the
    // quadratic to find the linear phase of the crossing for polyBLEP.
    float effPW = pulseWidth / (1.f + kSawCurve * (1.f - pulseWidth));
    if (mPulseInvert) effPW = 1.f - effPW; // J106: inverted duty cycle
    float minPW = cps + cps; // 4th-order BLEP needs 2*dt clearance from edges
    effPW = std::clamp(effPW, minPW, 1.f - minPW);
    float pulse = (mPos < effPW) ? -1.f : 1.f;
    pulse -= PolyBLEP(mPos, cps);   // falling edge at reset
    float pw2 = mPos - effPW;
    if (pw2 < 0.f)
      pw2 += 1.f;
    pulse += PolyBLEP(pw2, cps);    // rising edge at PW crossing

    // --- Sub: CD4013 flip-flop + polyBLEP ---
    // Half-frequency square wave, phase-locked to saw reset.
    // fabsf corrects for alternating transition directions: the pre-transition
    // window smooths toward the upcoming (opposite) step, and the post-transition
    // window smooths the step that just happened. Both push toward the midpoint.
    float sub = mSubState ? -1.f : 1.f;
    sub -= fabsf(PolyBLEP(mPos, cps)) * (mSubState ? -1.f : 1.f);

    // --- Oscillator mixing ---
    // Pop-free crossfade when waveform switches change mid-note.
    mSawGain += ((sawOn ? 1.f : 0.f) - mSawGain) * mSwitchRamp;
    mPulseGain += ((pulseOn ? 1.f : 0.f) - mPulseGain) * mSwitchRamp;
    mSubGain += ((subOn ? 1.f : 0.f) - mSubGain) * mSwitchRamp;

    // Linear sum. Verified against Juno-6 hardware: square+saw measured
    // -14.0dBFS vs -13.1dBFS predicted coherent sum — only 0.9dB of
    // compression at two-oscillator levels. The per-harmonic differences
    // (6-10dB below predicted) are phase cancellation from the shared
    // phase accumulator, not mixer saturation. The VCF input OTA
    // provides soft clipping downstream when driven harder.
    float out = saw * kSawAmp * mSawGain + pulse * kPulseAmp * mPulseGain +
                sub * kSubAmp * subLevel * mSubGain;

    // Noise is now mixed at the voice level from a shared source
    // (single generator for all voices, matching real hardware).
    (void)noiseAmp;

    return out;
  }
};

} // namespace kr106
