#pragma once

#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdint>

// BA662 feedback architecture: the hardware attenuates the LP4 output
// 67:1 (R3 100K / R1 1.5K) before the differential pair tanh, placing
// the tanh argument at ~0.26 during self-oscillation — barely nonlinear.
// A single memoryless scaled tanh (kFbScale=4.2) replaces the previous
// envelope-based amplitude control. Harmonic purity (H3 ≈ -48 dB),
// flat amplitude across frequency and resonance, pitch accuracy (±3 cents),
// and saw/oscillation level matching all fall out of the weak-drive
// physics rather than requiring per-parameter tuning.

namespace kr106 {

// ============================================================
// Inline half-band polyphase IIR resampler (replaces HIIR)
// ============================================================
// 12-coefficient allpass polyphase network for 2x up/downsampling.
// Same algorithm and coefficients as Laurent de Soras' HIIR library (WTFPL).
static constexpr int kNumResamplerCoefs = 12;

struct Upsampler2x
{
  float coef[kNumResamplerCoefs] = {};
  float x[kNumResamplerCoefs] = {};
  float y[kNumResamplerCoefs] = {};

  void set_coefs(const double c[kNumResamplerCoefs])
  {
    for (int i = 0; i < kNumResamplerCoefs; i++)
      coef[i] = static_cast<float>(c[i]);
  }

  void clear_buffers()
  {
    memset(x, 0, sizeof(x));
    memset(y, 0, sizeof(y));
  }

  void process_sample(float& out_0, float& out_1, float input)
  {
    float even = input;
    float odd = input;
    // Process pairs of allpass stages
    for (int i = 0; i < kNumResamplerCoefs; i += 2)
    {
      float t0 = (even - y[i]) * coef[i] + x[i];
      float t1 = (odd - y[i + 1]) * coef[i + 1] + x[i + 1];
      x[i] = even;   x[i + 1] = odd;
      y[i] = t0;     y[i + 1] = t1;
      even = t0;     odd = t1;
    }
    out_0 = even;
    out_1 = odd;
  }
};

struct Downsampler2x
{
  float coef[kNumResamplerCoefs] = {};
  float x[kNumResamplerCoefs] = {};
  float y[kNumResamplerCoefs] = {};

  void set_coefs(const double c[kNumResamplerCoefs])
  {
    for (int i = 0; i < kNumResamplerCoefs; i++)
      coef[i] = static_cast<float>(c[i]);
  }

  void clear_buffers()
  {
    memset(x, 0, sizeof(x));
    memset(y, 0, sizeof(y));
  }

  float process_sample(const float in[2])
  {
    float spl_0 = in[1];
    float spl_1 = in[0];
    for (int i = 0; i < kNumResamplerCoefs; i += 2)
    {
      float t0 = (spl_0 - y[i]) * coef[i] + x[i];
      float t1 = (spl_1 - y[i + 1]) * coef[i + 1] + x[i + 1];
      x[i] = spl_0;   x[i + 1] = spl_1;
      y[i] = t0;       y[i + 1] = t1;
      spl_0 = t0;     spl_1 = t1;
    }
    return 0.5f * (spl_0 + spl_1);
  }
};

// ============================================================
// 4-pole TPT Cascade LPF (models IR3109 OTA filter)
// ============================================================
// Four 1-pole trapezoidal integrators with global resonance feedback.
// State variables are integrator outputs — coefficient-independent,
// so per-sample cutoff modulation is artifact-free. No sin/cos needed.
struct VCF
{
  float mS[4] = {}; // integrator states
  bool mOTASaturation = true;   // per-stage OTA tanh nonlinearity (IR3109 model)
  bool mJ106Res = false;         // true = J106 resonance curve, false = J6 (calibrated)
  int mOversample = 4;           // 2 or 4 — runtime selectable
  uint32_t mNoiseSeed = 123456789u; // thermal noise PRNG state
  float mInputEnv = 0.f;           // peak envelope follower for noise suppression
  float mEnvDecay = 0.999f;        // per-sample decay for mInputEnv (22ms time constant)
  float mSampleRate = 44100.f;     // cached for SetOversample()

  // Two cascaded 2x polyphase stages (used as 1+1 for 4x, or just 1 for 2x).
  Upsampler2x mUp1, mUp2;
  Downsampler2x mDown1, mDown2;

  VCF()
  {
    static constexpr double kCoeffs2x[12] = {
      0.036681502163648017, 0.13654762463195794, 0.27463175937945444,
      0.42313861743656711, 0.56109869787919531, 0.67754004997416184,
      0.76974183386322703, 0.83988962484963892, 0.89226081800387902,
      0.9315419599631839,  0.96209454837808417, 0.98781637073289585
    };
    mUp1.set_coefs(kCoeffs2x);
    mUp2.set_coefs(kCoeffs2x);
    mDown1.set_coefs(kCoeffs2x);
    mDown2.set_coefs(kCoeffs2x);
  }

  void Reset()
  {
    mS[0] = mS[1] = mS[2] = mS[3] = 0.f;
    mUp1.clear_buffers();
    mUp2.clear_buffers();
    mDown1.clear_buffers();
    mDown2.clear_buffers();
  }

  void SetSampleRate(float sampleRate)
  {
    mSampleRate = sampleRate;
    mEnvDecay = expf(-1.f / (0.022f * sampleRate * static_cast<float>(mOversample)));
  }

  void SetOversample(int factor)
  {
    int prev = mOversample;
    mOversample = (factor == 2) ? 2 : 4;
    mEnvDecay = expf(-1.f / (0.022f * mSampleRate * static_cast<float>(mOversample)));
    // Don't reset filter state (mS) — preserve pitch continuity.
    // Only clear stage-2 resamplers when switching to 4x, since they were
    // idle during 2x and may contain stale data.
    if (mOversample == 4 && prev == 2)
    {
      mUp2.clear_buffers();
      mDown2.clear_buffers();
    }
  }

  // OTA differential-pair tanh (Padé approximant of tanh(x)).
  // Used in two places: (1) per-stage IR3109 OTA saturation in NLStage,
  // scaled by kOTAScale, and (2) the BA662 feedback path, scaled by
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
  // Juno-6 (calibrated from hardware, March 2026):
  //   Saw C1 (32.7 Hz) through VCF, LFO sweep across full cutoff range,
  //   recorded at R=0/3/5/7 via direct line out at 88.2 kHz.
  //   Filter response extracted by dividing out 1/n saw spectrum at each
  //   harmonic, then matching transition-region slope (+1 oct past -6dB)
  //   against the 4-pole TPT cascade simulation (2x oversampled).
  //
  //   R=3 (res=0.3): k ≈ 0.91, measured slope ≈ -18.9 dB/oct
  //   R=5 (res=0.5): k ≈ 1.94, measured slope ≈ -22.7 dB/oct
  //   R=7 (res=0.7): k ≈ 3.52, measured slope ≈ -26.8 dB/oct
  //   R=0 slope (-13.1 dB/oct) matches sim with k=0 (no fit needed).
  //
  // Juno-6: exponential resonance curve. Shape from hardware measurements;
  // kMax > 4.0 to compensate for tanh stage saturation absorbing feedback
  // energy (linear k=4 would self-oscillate, but our nonlinear stages need
  // ~5.0 to sustain oscillation). Gives 3-7 dB peaks at moderate settings.
  //
  // Hardware self-oscillation measurements (Juno-6 SN#193284, March 2026)
  // show flat amplitude (±0.5 dB) across 112 Hz–14 kHz at res=1.0 and
  // res=0.9. No frequency-dependent k correction is needed. The BA662
  // feedback tanh (weakly driven via the 100K:1.5K voltage divider)
  // stabilizes the limit cycle amplitude inherently.
  static float ResK_J6(float res)
  {
    static constexpr float kShape = 2.128f;
    static constexpr float kNorm = 0.811f;
    return kNorm * (expf(kShape * res) - 1.f);
  }

  // Juno-106: TODO — capture actual J106 filter resonance data and derive
  // a proper curve. For now, use the J6 curve as a placeholder.
  static float ResK_J106(float res)
  {
    return ResK_J6(res);
  }

  // Frequency compensation: multiplier applied to g (integrator coeff)
  // after the bilinear warp, correcting the effective cutoff frequency.
  //
  // Two effects shift the cutoff away from the target:
  //   1. Cascade droop: 4 identical poles in series place the -3dB point
  //      at 0.435× the individual pole frequency (k=0). Well-known from
  //      Zavalishin's "Art of VA Filter Design" — needs ~1.96× boost.
  //
  //   2. OTASat compression: at high k, the per-stage tanh nonlinearity
  //      reduces effective integrator gain (describing function gain < 1),
  //      pulling the cutoff frequency below target. The compression is
  //      amplitude-dependent but approximately constant for a given k
  //      (the limit cycle amplitude is set by the tanh).
  //
  // Applied to g (post-warp) rather than frq (pre-warp) so the tan()
  // bilinear transform sees the true frequency. This makes the
  // compensation sample-rate independent — verified by sweep tests at
  // 44.1 kHz and 96 kHz showing < 60 cent divergence vs 500+ cents
  // when compensating frq directly.
  //
  // Coefficients fit to empirical data from tools/vcf-analyze: unit
  // impulse response FFT (-3dB cutoff at R=0.0/0.3/0.5) and resonance
  // peak frequency (parabolic interpolation at R=0.7/0.8, zero-crossing
  // at R=0.9+), swept across frq=0.04–0.80 with compensation disabled.
  // Target data in tools/vcf-analyze/compensation_targets.csv.

  static float FreqCompensation(float k)
  {
      return std::max((1.96f + 1.06f * k) / (1.f + 2.16f * k), 1.f);
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
  static float InputComp(float k)
  {
    return 1.f + k * 0.45f;
  }

  static constexpr float kOTAScale = 0.35f;

  // Nonlinear one-pole OTA-C stage: solves y = s + g*tanh(x - y)
  // via one Newton-Raphson iteration from the linear TPT estimate.
  static float NLStage(float& s, float x, float g, float g1)
  {
      float y = s + g1 * (x - s);
      float diff = x - y;
      float sd = diff * kOTAScale;
      float t = OTASat(sd) / kOTAScale;
      float f = y - s - g * t;
      float df = 1.f + g * OTASatDeriv(sd);
      y -= f / df;
      s = 2.f * y - s;
      return y;
  }

  // frq: normalized cutoff [0, ~0.9] where 1.0 = Nyquist (base rate)
  // res: resonance amount [0, 1]
  float Process(float input, float frq, float res)
  {
    frq = std::min(frq, 0.95f);
   
    if (mOversample == 4)
      return Process4x(input, frq, res);
    else
      return Process2x(input, frq, res);
  }

private:
  // 2x oversampled: upsample, filter 2 samples, downsample
  float Process2x(float input, float frq, float res)
  {
    float up[2], down[2];
    mUp1.process_sample(up[0], up[1], input);

    float frq2x = frq * 0.5f;
    down[0] = ProcessSample(up[0], frq2x, res);
    down[1] = ProcessSample(up[1], frq2x, res);

    return mDown1.process_sample(down);
  }

  // 4x oversampled: 1x→2x→4x up, filter 4 samples, 4x→2x→1x down
  float Process4x(float input, float frq, float res)
  {
    float frq4x = frq * 0.25f;

    // Stage 1 upsample: 1x → 2x
    float up2x[2];
    mUp1.process_sample(up2x[0], up2x[1], input);

    // Stage 2 upsample + filter: 2x → 4x, process 4 samples
    float down4x[2], down2x[2];

    float up4x_a[2];
    mUp2.process_sample(up4x_a[0], up4x_a[1], up2x[0]);
    down4x[0] = ProcessSample(up4x_a[0], frq4x, res);
    down4x[1] = ProcessSample(up4x_a[1], frq4x, res);
    down2x[0] = mDown2.process_sample(down4x);

    float up4x_b[2];
    mUp2.process_sample(up4x_b[0], up4x_b[1], up2x[1]);
    down4x[0] = ProcessSample(up4x_b[0], frq4x, res);
    down4x[1] = ProcessSample(up4x_b[1], frq4x, res);
    down2x[1] = mDown2.process_sample(down4x);

    // Stage 2 downsample: 2x → 1x
    return mDown1.process_sample(down2x);
  }

  // Internal per-sample filter at the oversampled rate
  float ProcessSample(float input, float frq, float res)
  {
    // Adaptive thermal noise: models BA662/IR3109 OTA bias current noise.
    // High level when filter is quiet (to seed self-oscillation startup),
    // fades to inaudible once oscillation is established. This keeps
    // pure self-oscillation patches clean while still providing reliable
    // oscillation seeding.
    mNoiseSeed = mNoiseSeed * 196314165u + 907633515u;
    float white = static_cast<float>(mNoiseSeed) / static_cast<float>(0xFFFFFFFFu) * 2.f - 1.f;
    mInputEnv = std::max(fabsf(input), mInputEnv * mEnvDecay);
    float stateEnergy = fabsf(mS[0]) + fabsf(mS[1]) + fabsf(mS[2]) + fabsf(mS[3]);
    float energy = std::max(mInputEnv, stateEnergy);
    float noiseLevel = 1e-2f / (1.f + energy * 1000.f);
    input += white * noiseLevel;

    // Resonance CV: external transistor feeds BA662 OTA control current.
    float k = mJ106Res ? ResK_J106(res) : ResK_J6(res);
    k = SoftClipK(k);

    // Clamp frq for the bilinear transform — prevents tan() from
    // blowing up near Nyquist.
    frq = std::min(frq, 0.85f);
    float g = tanf(frq * static_cast<float>(M_PI) * 0.5f);
    g *= FreqCompensation(k);

    // Precompute gains for the 4-pole cascade solution
    float g1 = g / (1.f + g);  // one-pole gain

    // Linear cascade: the predictor is exact, so the filter is
    // unconditionally stable regardless of modulation speed.
    float G = g1 * g1 * g1 * g1;

    float S = mS[0] * g1 * g1 * g1 + mS[1] * g1 * g1 + mS[2] * g1 + mS[3];

    float comp = InputComp(k);

    // BA662 feedback path: models the R3(100K)/R1(1.5K) voltage divider
    // that attenuates the LP4 output before the BA662 differential pair.
    // In the physical circuit the attenuation is 0.015 (67:1), placing
    // the tanh argument at ~0.26 during self-oscillation — barely
    // nonlinear (~2% compression). This gentle memoryless limiting
    // stabilizes the limit cycle inherently, with flat amplitude across
    // frequency and resonance. Harmonic purity (H3 ≈ -45 dB relative
    // on hardware) falls out of the weak drive rather than requiring
    // envelope-based amplitude control.
    //
    // In our normalized-amplitude model, kFbScale is larger than the
    // physical 0.015 ratio because state variables are unity-scaled
    // rather than in millivolts. The k-dependent scaling reduces
    // effective drive at lower resonance (R=0.9 vs R=1.0), keeping
    // the amplitude gap within ~1 dB across settings.
    float kFbScale = 4.20f * std::clamp((k - 3.5f) * 0.8f, 0.3f, 1.f);
    float fbSig = OTASat(S * kFbScale) / kFbScale;

    float u = (input * comp - k * fbSig) / (1.f + k * G);

    float lp4;
    if (mOTASaturation)
    {
      // Per-stage describing function pitch compensation.
      // The IR3109 OTA stages (kOTAScale=0.35) have effective gain < 1
      // at self-oscillation amplitude, pulling pitch flat. dfGain boosts
      // g1 to compensate. Coefficient 0.5 and floor 0.65 tuned against
      // Juno-6 SN#193284 self-oscillation pitch data. Achieves ±5 cents
      // from 55–1760 Hz at R=1.0, ±3 cents at R=0.9.
      float stateAmp = fabsf(mS[3]);
      float dfGain = 1.f / sqrtf(1.f + 0.5f * stateAmp * stateAmp);
      dfGain = std::max(dfGain, 0.65f);

      // Fade correction off near Nyquist to prevent aliasing feedback.
      // At 4x/44.1k: frq=0.08 ≈ 7kHz, frq=0.12 ≈ 10.6kHz. Full
      // correction below 7kHz, linear fade to zero at 10.6kHz.
      float hfFade = std::clamp((0.12f - frq) * 25.f, 0.f, 1.f);
      dfGain = 1.f - hfFade * (1.f - dfGain);
      float g1NL = g1 / dfGain;
      g1NL = std::min(g1NL, 0.98f);

      float gNL = g1NL / (1.f - g1NL);

      float lp1 = NLStage(mS[0], u, gNL, g1NL);
      float lp2 = NLStage(mS[1], lp1, gNL, g1NL);
      float lp3 = NLStage(mS[2], lp2, gNL, g1NL);
      lp4 = NLStage(mS[3], lp3, gNL, g1NL);
    }
    else
    {
      // Linear stages: standard TPT trapezoidal integrators without
      // per-stage OTA saturation. The 4-pole cascade solution (G, S, u)
      // above is exact for linear stages, so no Newton-Raphson is needed.
      // The feedback path uses the BA662 scaled tanh, which at low drive
      // levels is nearly linear — the tiny g1 boost (0.1% at max k)
      // compensates for the residual nonlinearity, keeping the
      // self-oscillation frequency on target.
      float g1L = g1 * (1.f + k * 0.0003f);
      float v, s;
      s = mS[0]; v = g1L * (u - s);   mS[0] = s + 2.f * v; float lp1 = s + v;
      s = mS[1]; v = g1L * (lp1 - s); mS[1] = s + 2.f * v; float lp2 = s + v;
      s = mS[2]; v = g1L * (lp2 - s); mS[2] = s + 2.f * v; float lp3 = s + v;
      s = mS[3]; v = g1L * (lp3 - s); mS[3] = s + 2.f * v; lp4       = s + v;
    }

    // Flush denormals from integrator states
    for (auto& st : mS)
      if (fabsf(st) < 1e-15f) st = 0.f;

    return lp4;
  }
};

} // namespace kr106
