#pragma once

#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdint>

// BA662 feedback architecture: the hardware attenuates the LP4 output
// 67:1 (R3 100K / R1 1.5K) before the differential pair tanh, placing
// the tanh argument at ~0.26 during self-oscillation — barely nonlinear.
// A memoryless scaled tanh on the feedback path (kFbScale, g1-dependent)
// replaces the previous envelope-based amplitude control. Harmonic purity
// (H3 ≈ -48 dB), flat amplitude across frequency and resonance, and pitch
// accuracy all fall out of the weak-drive physics rather than requiring
// per-parameter tuning.
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
// SoftClipK, InputComp, g1/G powers, 1/(1+kG)) is hoisted
// into UpdateCoeffs() and cached on (frq, res, j106). Held notes pay
// nothing; modulated cutoff/res hits libm transcendentals once per
// base-rate sample, not once per oversampled sample.
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

  // Oversample factor (1, 2, or 4). Determines how frq is scaled
  // for the tan() coefficient and noise seeding. Caller is responsible
  // for driving ProcessSample at this rate.
  int mOversample = 4;
  float mOverScale = 0.25f;  // 1.0 / mOversample

  void SetOversample(int os)
  {
    mOversample = (os <= 1) ? 1 : (os <= 2) ? 2 : 4;
    mOverScale = 1.f / static_cast<float>(mOversample);
    InvalidateCache();
  }

  // ----- control-rate cache -----
  // Cached on (frq, res, j106) so held notes skip the entire
  // UpdateCoeffs() preamble.
  float mCacheFrq = -1.f;
  float mCacheRes = -1.f;
  bool  mCacheJ106 = false;
  float mCacheK = 0.f;
  float mCacheG = 0.f;

  // ----- per-UpdateCoeffs() derived coefficients -----
  // Everything ProcessSample needs in its inner loop. Recomputed at
  // control rate (i.e. once per base sample or less, if nothing moved).
  struct Coeffs
  {
    float k;
    float g, g1, g1_2, g1_3, G;
    float invDen;        // 1 / (1 + k*G)
    float comp;          // InputComp(k)
    float kFbScale;
    float invKFbScale;
    float hfFade;        // describing function HF rolloff
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
    // Per base-rate sample (22 ms time constant). mInputEnv is updated
    // in UpdateCoeffs (control rate), so its decay is at base rate.
    mEnvDecay = expf(-1.f / (0.022f * sampleRate));
    InvalidateCache();
  }

  // OTA differential-pair tanh (Padé approximant of tanh(x)).
  // Used in two places: (1) per-stage IR3109 OTA saturation in NLStage,
  // scaled by kOTAScaleBase, and (2) the BA662 feedback path, scaled by
  // kFbScale to model the R3(100K)/R1(1.5K) voltage divider that
  // attenuates the LP4 output before the BA662 differential pair.
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

  static float ResK_J106(float res)
  {
    float r = res;
    float r2 = r * r;
    float r3 = r2 * r;
    float r4 = r2 * r2;
    float k = 4.7116f * r - 6.5743f * r2 + 13.4633f * r3 - 8.2197f * r4;
    
    // Targeted boost: ramp from 1.0× at r=0.898 to 1.24× at r=1.0.
    // Below r=0.898 (~R=114, fader position 9), peaks match hardware.
    // Above that, k needs to reach 4.0+ for self-oscillation onset.
    float boost = std::max(0.f, (r - 0.898f) * (0.24f / 0.102f));
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

  // Input Q compensation: BA662 differential topology feeds input through
  // R5(47K) alongside LP4 feedback on R3(100K). Higher resonance boosts
  // the input, counteracting passband volume drop.
  static float InputComp(float k) { return (0.142f + 0.065f * k); }

  static constexpr float kOTAScaleBase = 0.35f;

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
  // Call once per base-rate sample (or whenever frq/res change).
  // Recomputes all transcendentals from frq/res and writes mC.
  //
  // frq: normalized cutoff at the BASE sample rate [0, ~0.95].
  //      1.0 == base-rate Nyquist.
  // res: resonance amount [0, 1]
  void UpdateCoeffs(float frq, float res)
  {
    frq = std::min(frq, 0.95f);

    // ---- Cache the parameter-derived terms ----
    // tanf and expf (in ResK) only re-fire when frq, res, or
    // J106 mode actually changes.
    if (frq != mCacheFrq || res != mCacheRes || mJ106Res != mCacheJ106)
    {
      mCacheFrq  = frq;
      mCacheRes  = res;
      mCacheJ106 = mJ106Res;

      float k = mJ106Res ? ResK_J106(res) : ResK_J6(res);
      if (!mJ106Res) k = SoftClipK(k); // J106 quartic already saturates

      // Tame resonance at ultrasonic cutoff. The peak itself is inaudible
      // but its skirt aliases back into the audible range.
      // frqFadeStart should be 20 kHz at 1×, 40 kHz at 2×, 80 kHz at 4×
      // i.e. always ~80% of the effective Nyquist
      float frqFadeStart = static_cast<float>(mOversample) * 0.8f;

      if (frq > frqFadeStart)
      {
        float fade = std::max(1.f - (frq - frqFadeStart) / (0.95f - frqFadeStart), 0.f);
        k *= fade;
      }

      // Hardware self-oscillation fades above ~30 kHz due to OTA bandwidth.
      // (From osc_calibrate: HW self-osc drops 3 dB at byte 114 (~32 kHz),
      //  gone at byte 127.)
      float cutoffHz = frq * mSampleRate * 0.5f;
      float hzFade = 1.f - std::clamp((cutoffHz - 25000.f) / 10000.f, 0.f, 1.f);
      k *= hzFade;

      mCacheK = k;

      // tanf argument clamped to 0.85 to stay away from Nyquist.
      // frqOver is frq normalized to the oversampled rate's Nyquist.
      float frqOver = std::min(frq * mOverScale, 0.85f);

      mCacheG = tanf(frqOver * static_cast<float>(M_PI) * 0.5f);
    }

    // ---- Cheap per-block derivations from cached k, g ----
    const float k    = mCacheK;
    const float g    = mCacheG;
    const float g1   = g / (1.f + g);
    const float g1_2 = g1 * g1;
    const float g1_3 = g1_2 * g1;
    const float G    = g1_2 * g1_2;

    mC.k    = k;
    mC.g    = g;
    mC.g1   = g1;
    mC.g1_2 = g1_2;
    mC.g1_3 = g1_3;
    mC.G    = G;
    mC.invDen      = 1.f / (1.f + k * G);
    mC.comp        = InputComp(k);

    // Describing function pitch compensation coefficient.
    mC.dfCoeff     = 0.6f;

    mC.otaScale = kOTAScaleBase;

    // Feedback saturation: base drive from resonance (k), plus a
    // g1-dependent term that flattens self-oscillation amplitude
    // across cutoff frequency. At higher cutoff, the cascade's
    // through-gain increases; stronger feedback compression keeps
    // the self-osc level flat (matched to hardware within ±0.7 dB).
    static constexpr float kFbg1Scale = 14.8f;
    static constexpr float kFbg1Power = 0.79f;
    float baseFbScale = 4.20f * std::clamp((k - 2.5f) * 1.0f, 0.3f, 1.f);
    mC.kFbScale = baseFbScale + kFbg1Scale * powf(g1, kFbg1Power);
    mC.invKFbScale = 1.f / mC.kFbScale;

    {
      float frqEq = std::min(frq * mOverScale, 0.85f);
      mC.hfFade = std::clamp((0.12f - frqEq) * 25.f, 0.f, 1.f);
    }

    // Adaptive noise level — control rate.
    // Decays at base-rate; its only role is seeding self-oscillation
    // startup, so control-rate update is inaudibly identical to per-sample.
    {
      float stateEnergy = fabsf(mS[0]) + fabsf(mS[1]) + fabsf(mS[2]) + fabsf(mS[3]);
      float energy = std::max(mInputEnv, stateEnergy);
      mC.noiseLevel = 1e-2f / (static_cast<float>(mOversample) * (1.f + energy * 1000.f));
    }
  }

  // Update the envelope follower with the latest input magnitude.
  // Separate from UpdateCoeffs so the caller can drive it however
  // it wants (e.g. with the osc output at base rate). No-op impact
  // if called inside the oversampled inner loop too — just slightly faster
  // tracking, which is harmless.
  void TrackInputEnv(float input)
  {
    mInputEnv = std::max(fabsf(input), mInputEnv * mEnvDecay);
  }

  // ============================================================
  // Audio-rate inner sample. Call os× per base sample, using
  // coefficients from the most recent UpdateCoeffs().
  // ============================================================
  // The assembly survey showed clang was leaving this out-of-line —
  // 5 bl-branches per base sample per voice (1 osc + 4 vcf), plus lost
  // opportunities to hoist mC loads across the 4 consecutive calls and
  // to schedule instructions across the call boundary. Forcing inline
  // reclaimed ~2–4% CPU in microbenchmarks on Apple Silicon.
#if defined(_MSC_VER)
  __forceinline
#else
  __attribute__((always_inline)) inline
#endif
  float ProcessSample(float input)
  {
    // Fast white noise via bit-trick float conversion.
    // Builds a float in [2,4) by stuffing PRNG bits into the mantissa,
    // then subtracts 3 to get [-1,1).
    mNoiseSeed = mNoiseSeed * 196314165u + 907633515u;
    union { uint32_t u; float f; } n;
    n.u = (mNoiseSeed >> 9) | 0x40000000u;
    float white = n.f - 3.f;
    input += white * mC.noiseLevel;

    // FTZ-independent denormal prevention: a tiny DC bias on one
    // integrator state keeps the cascade out of the denormal range.
    // Critical for the WASM build (no FTZ flag), harmless on native.
    mS[0] += 1e-20f;

    // Linear cascade predictor: exact for linear stages.
    float S = mS[0] * mC.g1_3 + mS[1] * mC.g1_2 + mS[2] * mC.g1 + mS[3];

    // BA662 feedback path: weakly-driven scaled tanh.
    float fbSig = OTASat(S * mC.kFbScale) * mC.invKFbScale;

    float u = (input * mC.comp - mC.k * fbSig) * mC.invDen;

    // Per-stage describing function pitch compensation.
    float stateAmp = fabsf(mS[3]);
    float dfGain = 1.f / sqrtf(1.f + mC.dfCoeff * stateAmp * stateAmp);
    dfGain = std::max(dfGain, 0.65f);
    // hfFade prevents aliasing feedback near Nyquist.
    dfGain = 1.f - mC.hfFade * (1.f - dfGain);
    float g1NL = std::min(mC.g1 / dfGain, 0.98f);
    float gNL  = g1NL / (1.f - g1NL);

    float ota = mC.otaScale;
    float lp1 = NLStage(mS[0], u,   gNL, g1NL, ota);
    float lp2 = NLStage(mS[1], lp1, gNL, g1NL, ota);
    float lp3 = NLStage(mS[2], lp2, gNL, g1NL, ota);
    float lp4 = NLStage(mS[3], lp3, gNL, g1NL, ota);

    // Output gain: passband makeup gain for the 4-pole
    // cascade at the calibrated InputComp level.
    return lp4 * 8.59f;
  }

  // Convenience wrapper: run one base-rate sample through a single
  // ProcessSample (no oversampling). Only useful for priming /
  // diagnostics — normal use is to call UpdateCoeffs once then
  // ProcessSample os× from the voice's inner loop.
  float Process(float input, float frq, float res)
  {
    UpdateCoeffs(frq, res);
    TrackInputEnv(input);
    return ProcessSample(input);
  }
};

} // namespace kr106
