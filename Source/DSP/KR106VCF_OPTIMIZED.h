#pragma once

#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdint>

// BA662 feedback architecture — circuit-accurate OTA model.
//
// Hardware signal path (from Juno-106 schematic):
//   OscMix ──10K──→ Filter Input Node ←── OTA Output (current)
//   OscMix ──47K──→ OTA +IN ──1.5K──→ GND
//   FilterOut─100K──→ OTA -IN ──1.5K──→ GND
//
// The input and filter output BOTH enter the BA662 OTA differential
// pair. Input is attenuated 1.5/(47+1.5) = 0.0309, feedback is
// attenuated 1.5/(100+1.5) = 0.0148. Input is 2.09× less attenuated
// than feedback. The OTA output current (proportional to Gm × tanh
// of the differential) sums with the direct input current (OscMix/10K)
// at the filter input node.
//
// This topology means resonance naturally boosts the input signal
// because the OTA amplifies the differential which includes the input
// on +IN. At high cutoff where FilterOut ≈ Input, the differential
// approaches zero and the OTA contribution vanishes — the passband
// returns to its uncompensated level without any explicit frequency-
// dependent correction.
//
// ============================================================
// ARCHITECTURE (post-refactor, April 2026)
// ============================================================
// VCF supports 1×, 2×, or 4× oversampling at any base sample rate.
// The caller is responsible for driving ProcessSample at the chosen
// rate. Downsampling (if needed) happens externally — typically once
// on the summed voice bus at the DSP level, not per-voice.
//
// API:
//   SetOversample(os)       — set 1, 2, or 4. Affects coefficient
//                              scaling and noise seeding.
//   UpdateCoeffs(frq, res)  — control-rate, call once per base sample
//                             (or whenever frq/res change). Recomputes
//                             g, k, kFbScale, etc. Writes into mC.
//   ProcessSample(input)    — audio-rate, called os× per base sample.
//                             Uses coefficients cached in mC.
//
// ============================================================
// OPTIMIZATION NOTES
// ============================================================
// Control-rate computation (tanf, expf via ResK, powf via kFbScale,
// g1/G powers, 1/(1+kG)) is hoisted into UpdateCoeffs() and cached
// on (frq, res, j106). Held notes pay nothing; modulated cutoff/res
// hits libm transcendentals once per base-rate sample, not once per
// oversampled sample.
//
// Per-sample denormal flushing replaced with a 1e-20f DC bias on
// mS[0]. FTZ-independent — works under WASM where there is no FTZ
// flag. ~130 dB below anything audible.

namespace kr106 {

// ============================================================
// 4-pole TPT Cascade LPF (models IR3109 OTA filter)
// ============================================================
// Four 1-pole trapezoidal integrators with global resonance feedback
// and per-stage OTA tanh nonlinearity (IR3109 model). State variables
// are integrator outputs — coefficient-independent, so per-sample
// cutoff modulation is artifact-free.
//
// Runs at the configured oversample rate (1×, 2×, or 4×).
// See UpdateCoeffs / ProcessSample for the control/audio split.
struct VCF
{
  float mS[4] = {}; // integrator states
  bool mJ106Res = false;            // true = J106 resonance curve, false = J6
  uint32_t mNoiseSeed = 123456789u; // thermal noise PRNG state
  float mInputEnv = 0.f;            // peak envelope follower for noise suppression
  float mEnvDecay = 0.999f;         // per-base-sample decay (22 ms time constant)
  float mSampleRate = 44100.f;      // BASE sample rate

  int mOversample = 4;
  float mOverScale = 0.25f;  // 1.0 / mOversample

  void SetOversample(int os)
  {
    mOversample = (os <= 1) ? 1 : (os <= 2) ? 2 : 4;
    mOverScale = 1.f / static_cast<float>(mOversample);
    InvalidateCache();
  }

  // ----- control-rate cache -----
  float mCacheFrq = -1.f;
  float mCacheRes = -1.f;
  bool  mCacheJ106 = false;
  float mCacheK = 0.f;
  float mCacheG = 0.f;

  // ----- per-UpdateCoeffs() derived coefficients -----
  struct Coeffs
  {
    float k;
    float g, g1, g1_2, g1_3, G;
    float invDen;        // 1 / (1 + k*G)
    float kFbScale;
    float invKFbScale;
    float noiseLevel;    // adaptive thermal noise (control-rate)
    float otaScale;      // per-stage OTA drive (constant kOTAScaleBase)
    float dfCoeff;       // describing function sensitivity
  } mC;

  VCF() {}

  void InvalidateCache() { mCacheFrq = -1.f; }

  void Reset()
  {
    mS[0] = mS[1] = mS[2] = mS[3] = 0.f;
    mInputEnv = 0.f;
    InvalidateCache();
  }

  void SetSampleRate(float sampleRate)
  {
    mSampleRate = sampleRate;
    mEnvDecay = expf(-1.f / (0.022f * sampleRate));
    InvalidateCache();
  }
  
  // OTA differential-pair tanh (Padé approximant of tanh(x)).
  static float OTASat(float x)
  {
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
  }

  // Derivative of OTASat (sech² approximant), for Newton-Raphson.
  static float OTASatDeriv(float x)
  {
    if (x > 3.f || x < -3.f) return 0.f;
    float x2 = x * x;
    float d = 27.f + 9.f * x2;
    return 27.f * (27.f - 3.f * x2) / (d * d);
  }

  // Resonance slider → feedback gain k.
  // Juno-6: exponential resonance curve calibrated from hardware
  // (SN#193284, March 2026). See repo notes for derivation.
  static float ResK_J6(float res)
  {
    static constexpr float kShape = 2.128f;
    static constexpr float kNorm = 0.811f;
    return kNorm * (expf(kShape * res) - 1.f);
  }

  // Resonance slider → feedback gain k for J106.
  // Quartic polynomial fitted to hardware resonance sweep data
  // (res_calibrate, April 2026). Noise through VCF at fixed cutoff
  // (byte 64, ~800 Hz), peak-relative-to-passband measured at 29
  // resonance steps via Welch PSD. Peak growth curve inverted through
  // the DSP's own transfer function to recover k values that produce
  // the measured peak heights.
  //
  // Polynomial reaches k=3.42 at R=127. A quadratic knee above fader
  // position 5.5 adds up to 0.35 to match the hardware curve's steep
  // onset. A targeted boost above fader position 9 pushes k past 4.0
  // for self-oscillation onset.
  static float ResK_J106(float res)
  {
    float r = res;
    float r2 = r * r;
    float r3 = r2 * r;
    float r4 = r2 * r2;
    float k = 4.6265f * r + 4.3866f * r2 - 8.9869f * r3 + 3.3902f * r4;

    if (r > 0.7f)
    {
      float knee = (r - 0.7f) / 0.3f;
      k += std::min(knee * knee * 1.2f, 0.35f);
    }

    float boost = std::max(0.f, (r - 0.898f) * (0.17f / 0.102f));
    return k * (1.f + boost);
  }

  // Soft-clip resonance above k=3.0 (OTA gain compression at high feedback).
  static float SoftClipK(float k)
  {
    if (k > 3.0f)
    {
      float excess = k - 3.0f;
      k = 3.0f + excess / (1.0f + excess * 0.2f);
    }
    return std::min(k, 6.6f);
  }

  static constexpr float kOTAScaleBase = 0.35f;

  // Circuit-derived constants for the BA662 OTA feedback path.
  //
  // Direct input gain (OscMix through 10K to filter input node):
  // Sets the passband level at R=0. Output gain (8.59) is calibrated
  // so that kDirectGain × 8.59 = 1.22 (passband near unity).
  static constexpr float kDirectGain = 0.142f;

  // Input/feedback ratio in the BA662 OTA differential pair.
  // Hardware: +IN attenuated 1.5/(47+1.5) = 0.0309,
  //           -IN attenuated 1.5/(100+1.5) = 0.0148,
  //           ratio = 2.09.
  // In DSP-normalized signal levels this maps to 0.065 — confirmed
  // by matching the old InputComp(k) = 0.142 + 0.065×k slope:
  //   linearized input gain = kDirectGain + k × kInputRatio
  //                         = 0.142 + 0.065×k  ✓
  static constexpr float kInputRatio = 0.065f;

  // Nonlinear one-pole OTA-C stage: solves y = s + g*tanh(x - y)
  // via one Newton-Raphson iteration from the linear TPT estimate.
  static float NLStage(float& s, float x, float g, float g1, float otaScale)
  {
    float y = s + g1 * (x - s);
    float diff = x - y;
    float sd = diff * otaScale;
    float t = OTASat(sd) / otaScale;
    float f = y - s - g * t;
    float df = 1.f + g * OTASatDeriv(sd);
    y -= f / df;
    s = 2.f * y - s;
    return y;
  }

  // ============================================================
  // Control-rate coefficient update.
  // ============================================================
  void UpdateCoeffs(float frq, float res)
  {
    float frqUnclamped = frq;

    if (frqUnclamped != mCacheFrq || res != mCacheRes || mJ106Res != mCacheJ106)
    {
      mCacheFrq = frqUnclamped;
      mCacheRes = res;
      mCacheJ106 = mJ106Res;

      float k = mJ106Res ? ResK_J106(res) : ResK_J6(res);
      if (!mJ106Res) k = SoftClipK(k);

      // ---- Anti-aliasing at 1× OS ----
      // At 1×, no decimator to attenuate near-Nyquist resonant peaks.
      // Fade k directly so even broad (non-self-oscillating) peaks
      // don't alias. At 2×/4× the decimators handle this.
      if (mOversample == 1 && frq > 0.7f)
      {
        float fade = std::max(1.f - (frq - 0.7f) / 0.2f, 0.f);
        k *= fade;
      }

      mCacheK = k;

      float frqOver = std::min(frq * mOverScale, 0.85f);
      mCacheG = tanf(frqOver * static_cast<float>(M_PI) * 0.5f);
    }

    const float k = mCacheK; // post-fade effective k
    const float g = mCacheG;
    const float g1 = g / (1.f + g);
    const float g1_2 = g1 * g1;
    const float g1_3 = g1_2 * g1;
    const float G = g1_2 * g1_2;

    mC.k = k;
    mC.g = g;
    mC.g1 = g1;
    mC.g1_2 = g1_2;
    mC.g1_3 = g1_3;
    mC.G = G;
    mC.invDen = 1.f / (1.f + k * G);

    mC.dfCoeff = 0.6f;
    mC.otaScale = kOTAScaleBase;

    // Feedback saturation drive. Fit empirically so kFbScale reaches
    // ~4.2 at/above self-osc threshold (k≥3.5) and collapses to a
    // floor of 1.26 at k≤2.5 so passband signals stay linear.
    float baseFbScale = 4.20f * std::clamp((k - 2.5f) * 1.0f, 0.3f, 1.f);
    mC.kFbScale = baseFbScale;
    mC.invKFbScale = 1.f / mC.kFbScale;

    // Adaptive noise level — control rate.
    {
      float stateEnergy = fabsf(mS[0]) + fabsf(mS[1]) + fabsf(mS[2]) + fabsf(mS[3]);
      float energy = std::max(mInputEnv, stateEnergy);
      mC.noiseLevel = 1e-2f / (static_cast<float>(mOversample) * (1.f + energy * 1000.f));
    }
  }

  void TrackInputEnv(float input)
  {
    mInputEnv = std::max(fabsf(input), mInputEnv * mEnvDecay);
  }

  // ============================================================
  // Audio-rate inner sample.
  // ============================================================
#if defined(_MSC_VER)
  __forceinline
#else
  __attribute__((always_inline)) inline
#endif
  float ProcessSample(float input)
  {
    mNoiseSeed = mNoiseSeed * 196314165u + 907633515u;
    union { uint32_t u; float f; } n;
    n.u = (mNoiseSeed >> 9) | 0x40000000u;
    float white = n.f - 3.f;
    input += white * mC.noiseLevel;

    mS[0] += 1e-20f;

    // Linear cascade predictor: y4 when u=0. The (1-g1) factor comes
    // from the last stage's output = g1·input_prev + (1-g1)·s_last,
    // and propagates through to all state contributions. Missing this
    // factor doubles effective k at g1=0.5 (cutoff near Fs/4), which
    // was causing the filter to self-oscillate at much lower nominal
    // k than it should.
    float S = (1.f - mC.g1) * (mS[0] * mC.g1_3 + mS[1] * mC.g1_2 + mS[2] * mC.g1 + mS[3]);

    // Circuit-accurate BA662 OTA: input and feedback share the
    // differential pair. The OTA sees:
    //   +IN: input × 1.5/(47+1.5)   (attenuated oscillator mix)
    //   -IN: FilterOut × 1.5/(100+1.5)  (attenuated filter output)
    // In DSP-normalized units, input enters at kInputRatio (0.065)
    // relative to the feedback signal S.
    //
    // Key behaviors that fall out of this topology:
    //   • Self-oscillation (input=0): otaDiff = -S, identical to
    //     feedback-only model. Self-osc unchanged.
    //   • Low cutoff (S≈0): otaDiff ≈ input×0.065, OTA boosts input
    //     proportionally to k. Natural passband compensation.
    //   • High cutoff (S≈input×gain): differential partially cancels,
    //     OTA contribution shrinks. Passband returns to uncompensated
    //     level — no explicit frequency-dependent correction needed.
    float otaDiff = input * kInputRatio - S;
    float otaOut = OTASat(otaDiff * mC.kFbScale) * mC.invKFbScale;

    // Filter input = direct path (10K) + resonance-controlled OTA output.
    float u = (input * kDirectGain + mC.k * otaOut) * mC.invDen;

    // Per-stage describing function pitch compensation.
    float stateAmp = fabsf(mS[3]);
    float dfGain = 1.f / sqrtf(1.f + mC.dfCoeff * stateAmp * stateAmp);
    dfGain = std::max(dfGain, 0.65f);
    float g1NL = std::min(mC.g1 / dfGain, 0.98f);
    float gNL  = g1NL / (1.f - g1NL);

    float lp1 = NLStage(mS[0], u,   gNL, g1NL, mC.otaScale);
    float lp2 = NLStage(mS[1], lp1, gNL, g1NL, mC.otaScale);
    float lp3 = NLStage(mS[2], lp2, gNL, g1NL, mC.otaScale);
    float lp4 = NLStage(mS[3], lp3, gNL, g1NL, mC.otaScale);

    // Output gain: passband makeup gain for the 4-pole cascade.
    // kDirectGain × 8.59 = 1.22 (passband near unity at R=0).
    return lp4 * 8.59f;
  }

  float Process(float input, float frq, float res)
  {
    UpdateCoeffs(frq, res);
    TrackInputEnv(input);
    return ProcessSample(input);
  }
};

} // namespace kr106
