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
//
// ============================================================
// OPTIMIZATION NOTES
// ============================================================
// Control-rate computation (tanf, expf via ResK, powf via FreqComp,
// SoftClipK, InputComp, kFbScale, g1/G powers, 1/(1+kG)) is hoisted
// into Process() and cached on (frq, res, oversample, j106). Held
// notes pay nothing; modulated cutoff/res hits libm transcendentals
// once per output sample instead of once per oversampled sample.
//
// Per-sample denormal flushing replaced with a 1e-20f DC bias on
// mS[0]. FTZ-independent — works under WASM where there is no FTZ
// flag. ~130 dB below anything audible.

namespace kr106 {

// ============================================================
// Inline half-band polyphase IIR resampler
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
// Four 1-pole trapezoidal integrators with global resonance feedback
// and per-stage OTA tanh nonlinearity (IR3109 model). State variables
// are integrator outputs — coefficient-independent, so per-sample
// cutoff modulation is artifact-free.
struct VCF
{
  float mS[4] = {}; // integrator states
  bool mJ106Res = false;            // true = J106 resonance curve, false = J6
  int mOversample = 4;              // 1, 2, or 4 — runtime selectable
  uint32_t mNoiseSeed = 123456789u; // thermal noise PRNG state
  float mInputEnv = 0.f;            // peak envelope follower for noise suppression
  float mEnvDecay = 0.999f;         // per-output-sample decay (22 ms time constant)
  float mSampleRate = 44100.f;

  Upsampler2x mUp1, mUp2;
  Downsampler2x mDown1, mDown2;

  // ----- control-rate cache -----
  // Cached on (frq, res, oversample, j106) so held notes skip the
  // entire Process() preamble.
  float mCacheFrq = -1.f;
  float mCacheRes = -1.f;
  int   mCacheOver = -1;
  bool  mCacheJ106 = false;
  float mCacheK = 0.f;
  float mCacheG = 0.f;
  float mCacheFreqGain = 1.f;
  float mCacheResByte = 0.f;   // raw slider value 0-127 for table lookup

  // ----- per-Process() coefficients -----
  struct Coeffs
  {
    float k;
    float g, g1, g1_2, g1_3, G;
    float invDen;        // 1 / (1 + k*G)
    float comp;          // InputComp(k)
    float kFbScale;
    float invKFbScale;
    float hfFade;        // dfGain HF rolloff
    float noiseLevel;    // adaptive thermal noise (control-rate)
    float outputScale;   // freq+res dependent output attenuation
    float otaScale;      // frequency-dependent OTA drive
    float dfCoeff;       // describing function coefficient (freq-dependent)
  } mC;

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

  void InvalidateCache() { mCacheFrq = -1.f; }

  void Reset()
  {
    mS[0] = mS[1] = mS[2] = mS[3] = 0.f;
    mUp1.clear_buffers();
    mUp2.clear_buffers();
    mDown1.clear_buffers();
    mDown2.clear_buffers();
    mInputEnv = 0.f;
    InvalidateCache();
  }

  void SetSampleRate(float sampleRate)
  {
    mSampleRate = sampleRate;
    // Per output-rate sample (22 ms time constant).
    mEnvDecay = expf(-1.f / (0.022f * sampleRate));
    InvalidateCache();
  }

  void SetOversample(int factor)
  {
    int prev = mOversample;
    mOversample = (factor <= 1) ? 1 : (factor == 2) ? 2 : 4;
    if (mOversample == 4 && prev == 2)
    {
      mUp2.clear_buffers();
      mDown2.clear_buffers();
    }
    InvalidateCache();
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
    // Fitted to hardware peak amplitudes from noise grid measurements
    // (SN#193284). Small-signal k(res) extracted by inverting the
    // 4-pole TPT cascade peak response, scaled 1.24x to compensate
    // for OTA compression underestimate in extraction. RMSE 0.07
    // over res >= 28 (pre-scale). k(1.0) = 4.19, threshold ~ res 0.9.
    float r = res;
    float r2 = r * r;
    float r3 = r2 * r;
    float r4 = r2 * r2;
    return 1.24f * (4.7116f * r - 6.5743f * r2 + 13.4633f * r3 - 8.2197f * r4);
  }

  // Hardware-calibrated frequency compensation. Boosts pole frequency
  // at low cutoff to match passband levels from hardware noise sweep
  // measurements (J106 service mode 3, KBD=max, R=0). Power law
  // 0.48 * frq^-0.12 fitted to VCF bytes 0-72, clamped to 1.0 at high
  // frequencies. At high resonance, blends toward 1.0 to preserve
  // self-oscillation pitch accuracy.
  // Hardware-calibrated 2D frequency compensation lookup table.
  // Measured from uncorrected DSP vs hardware -3dB points across
  // two grids: 10x10 (full range, R=0..127) and 15x5 (preset zone, R=0..32).
  // Bilinear interpolation on (freq_byte, res_byte) axes at runtime.
  // Each entry is the pole frequency multiplier needed to match hardware.
  static constexpr int kFCFreqN = 20;
  static constexpr int kFCResN = 10;
  static constexpr float kFCFreq[20] = {0.f,14.f,28.f,32.f,36.f,40.f,44.f,48.f,52.f,56.f,60.f,64.f,68.f,72.f,76.f,80.f,84.f,98.f,112.f,127.f};
  static constexpr float kFCRes[10] = {0.f,14.f,28.f,42.f,56.f,70.f,84.f,98.f,112.f,127.f};
  static constexpr float kFCTable[10][20] = {
    {0.96f, 0.96f, 0.96f, 0.91f, 0.86f, 0.81f, 0.79f, 0.78f, 0.78f, 0.78f, 0.83f, 0.87f, 0.91f, 0.96f, 1.00f, 1.04f, 1.09f, 1.09f, 1.09f, 1.09f},  // R=0
    {0.94f, 0.94f, 0.94f, 0.92f, 0.90f, 0.88f, 0.87f, 0.87f, 0.86f, 0.85f, 0.87f, 0.89f, 0.90f, 0.90f, 0.88f, 0.85f, 0.83f, 0.83f, 0.83f, 0.83f},  // R=14
    {0.96f, 0.96f, 0.96f, 0.92f, 0.89f, 0.86f, 0.85f, 0.87f, 0.88f, 0.90f, 0.93f, 0.97f, 1.00f, 1.00f, 0.95f, 0.91f, 0.87f, 0.87f, 0.87f, 0.87f},  // R=28
    {1.02f, 1.02f, 1.02f, 0.98f, 0.94f, 0.91f, 0.89f, 0.90f, 0.91f, 0.92f, 0.93f, 0.95f, 0.96f, 0.96f, 0.94f, 0.92f, 0.90f, 0.90f, 0.90f, 0.90f},  // R=42
    {0.96f, 0.96f, 0.96f, 0.95f, 0.93f, 0.92f, 0.91f, 0.92f, 0.93f, 0.94f, 0.96f, 0.98f, 1.00f, 0.99f, 0.97f, 0.94f, 0.91f, 0.91f, 0.91f, 0.91f},  // R=56
    {0.99f, 0.99f, 0.99f, 0.96f, 0.94f, 0.91f, 0.91f, 0.92f, 0.93f, 0.94f, 0.93f, 0.92f, 0.92f, 0.91f, 0.91f, 0.92f, 0.92f, 0.92f, 0.92f, 0.92f},  // R=70
    {0.98f, 0.98f, 0.98f, 0.96f, 0.94f, 0.91f, 0.90f, 0.91f, 0.91f, 0.91f, 0.93f, 0.94f, 0.95f, 0.95f, 0.93f, 0.92f, 0.90f, 0.90f, 0.90f, 0.90f},  // R=84
    {1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},  // R=98  (high res: -3dB unreliable)
    {1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},  // R=112 (high res: -3dB unreliable)
    {1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},  // R=127 (high res: -3dB unreliable)
  };

  // Convert normalized frequency (frq = hz / Nyquist) to equivalent VCF byte
  // for table lookup. Inverse of dacToHz: byte = ln(hz / 5.53) * 1143 / (0.693 * 128)
  static float frqToByte(float frq, float sampleRate)
  {
    float hz = frq * sampleRate * 0.5f;
    if (hz <= 5.53f) return 0.f;
    float dac = logf(hz / 5.53f) / (0.693147f / 1143.f);
    return std::clamp(dac / 128.f, 0.f, 127.f);
  }

  static float FreqCompensationClamped(float k, float frq, float resByte, float sampleRate)
  {
    // Convert runtime frq to equivalent freq byte for table lookup
    // frq is at the 4x-oversampled rate, so scale back to base rate
    float freqByte = frqToByte(frq * 4.f, sampleRate);

    // Bilinear interpolation on the 2D lookup table
    int fi = 0;
    for (int i = 0; i < kFCFreqN - 1; i++)
      if (kFCFreq[i + 1] <= freqByte) fi = i + 1;
    int fi1 = std::min(fi + 1, kFCFreqN - 1);
    float ff = (fi == fi1) ? 0.f : (freqByte - kFCFreq[fi]) / (kFCFreq[fi1] - kFCFreq[fi]);
    ff = std::clamp(ff, 0.f, 1.f);

    int ri = 0;
    for (int i = 0; i < kFCResN - 1; i++)
      if (kFCRes[i + 1] <= resByte) ri = i + 1;
    int ri1 = std::min(ri + 1, kFCResN - 1);
    float fr = (ri == ri1) ? 0.f : (resByte - kFCRes[ri]) / (kFCRes[ri1] - kFCRes[ri]);
    fr = std::clamp(fr, 0.f, 1.f);

    float v00 = kFCTable[ri][fi],  v10 = kFCTable[ri][fi1];
    float v01 = kFCTable[ri1][fi], v11 = kFCTable[ri1][fi1];
    float top = v00 + (v10 - v00) * ff;
    float bot = v01 + (v11 - v01) * ff;
    float tableVal = top + (bot - top) * fr;

    return tableVal;
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
  //
  // Frequency-dependent gain: OTA bias current affects passband transmission.
  // Power law fitted to 10x10 hardware noise sweep (96 kHz, R=0).
  // freqGain should be pre-computed and cached (contains powf).
  static float InputComp(float k, float freqGain)
  {
    return (0.379f + 0.087f * k) * freqGain;
  }

  static float FreqGain(float frq)
  {
    // Bell curve in log-frequency space. Attenuates input gain at both
    // low and high cutoff frequencies, peak near 1 kHz. Empirically
    // tuned from A/B preset comparison against hardware recordings --
    // matches level across all 128 factory presets, but the physical
    // basis is not fully understood. Possibly OTA bias current effects
    // at low cutoff and bandwidth rolloff at high cutoff.
    float logFrq = logf(std::max(frq, 1e-6f));
    static constexpr float kCenter = -3.9f;  // ln(0.02) ≈ -3.9 (~1 kHz at 96k)
    static constexpr float kSigma  = 2.5f;   // width in log units
    float d = (logFrq - kCenter) / kSigma;
    float fg = expf(-0.5f * d * d);
    return std::max(0.25f, fg);
  }

  static constexpr float kOTAScaleBase = 0.35f;

  // Frequency-dependent OTA drive: at low bias currents (low cutoff),
  // the IR3109 OTAs are more linear, producing a shallower rolloff slope.
  // Measured: HW slope is ~12 dB/oct at F=14 vs ideal 24 dB/oct.
  // Crossover at frq ~0.005 (240 Hz at 96k). Below that, OTA drive
  // ramps down, making tanh more linear.
  static float OTAScaleForFreq(float frq, float resByte)
  {
    float scale = kOTAScaleBase;
    if (frq < 0.005f)
    {
      float blend = std::max(frq / 0.005f, 0.15f);
      scale *= blend;
    }
    // At high resonance, restore full OTA drive so self-oscillation
    // pitch stays accurate. The slope correction only matters at low Q.
    float resK = (resByte > 0.f) ? 1.024f * (expf(2.128f * resByte / 127.f) - 1.f) : 0.f;
    float resBlend = std::min(resK * resK * 0.0625f, 1.f);
    return scale + resBlend * (kOTAScaleBase - scale);
  }

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

  // frq: normalized cutoff [0, ~0.95] where 1.0 = Nyquist (base rate)
  // res: resonance amount [0, 1]
  float Process(float input, float frq, float res)
  {
    frq = std::min(frq, 0.95f);

    // ---- Cache the parameter-derived terms ----
    // tanf, expf (in ResK), and powf (in FreqComp) only re-fire when
    // frq, res, oversample factor, or J106 mode actually changes.
    if (frq != mCacheFrq || res != mCacheRes ||
        mOversample != mCacheOver || mJ106Res != mCacheJ106)
    {
      mCacheFrq  = frq;
      mCacheRes  = res;
      mCacheOver = mOversample;
      mCacheJ106 = mJ106Res;

      float k = mJ106Res ? ResK_J106(res) : ResK_J6(res);
      if (!mJ106Res) k = SoftClipK(k); // J106 quartic already saturates

      // Tame resonance at ultrasonic cutoff. The peak itself is inaudible
      // but its skirt aliases back into the audible range.
      // Hardware bandwidth (~50 kHz) naturally kills resonance here.
      // Fade k from full at 20 kHz to zero at Nyquist (frq 0.95).
      // Threshold is absolute Hz so behavior is consistent across sample rates.
      float frqFadeStart = 20000.f / (mSampleRate * 0.5f); // 20 kHz normalized
      if (frq > frqFadeStart)
      {
        float fade = std::max(1.f - (frq - frqFadeStart) / (0.95f - frqFadeStart), 0.f);
        k *= fade;
      }

      mCacheK = k;

      // ProcessSample used to clamp the already-divided frq to 0.85
      // to keep tanf away from Nyquist; preserve that here.
      float overScale = (mOversample == 4) ? 0.25f
                      : (mOversample == 2) ? 0.5f
                      : 1.f;
      float frqOver = std::min(frq * overScale, 0.85f);

      // FreqComp from 2D hardware-calibrated lookup table.
      float fc = FreqCompensationClamped(k, frq * 0.25f, mCacheResByte, mSampleRate);
      mCacheG = tanf(frqOver * static_cast<float>(M_PI) * 0.5f) * fc;
      mCacheFreqGain = FreqGain(frq);
    }

    // ---- Cheap per-Process derivations from cached k, g ----
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
    mC.comp        = InputComp(k, mCacheFreqGain);
    mC.otaScale    = OTAScaleForFreq(mCacheFrq, mCacheResByte);

    // Describing function pitch compensation coefficient. Constant 0.6
    // gives +/-2 cents from 220 Hz up; freq ramp was vestigial and went
    // sharp at HF (+5.6 cents at 7 kHz).
    mC.dfCoeff     = 0.6f;
    // BA662 tanh saturation is set by 2*Vt, independent of IR3109 bias
    // current -- amplitude is naturally flat.
    mC.kFbScale    = 4.20f * std::clamp((k - 2.5f) * 1.0f, 0.3f, 1.f);
    mC.invKFbScale = 1.f / mC.kFbScale;

    // Output scaling disabled -- self-osc level is now controlled by
    // fbBoost alone. OutputScale was attenuating the passband at high
    // freq+res which caused KBD-tracking patches to lose level at high notes.
    mC.outputScale = 1.f;
    if (false) // DISABLED
    {
      float frqFactor = std::clamp((mCacheFrq - 0.02f) * 20.f, 0.f, 1.f);
      float resFactor = std::clamp((k - 2.f) * 0.5f, 0.f, 1.f);           // ramps 0->1 from k=2 to k=4
      float atten = frqFactor * resFactor * 0.5f;                           // max 0.5 = -6 dB
      mC.outputScale = 1.f - atten;
    }

    {
      float overScale = (mOversample == 4) ? 0.25f
                      : (mOversample == 2) ? 0.5f
                      : 1.f;
      float frqEq = std::min(frq * overScale, 0.85f);
      mC.hfFade = std::clamp((0.12f - frqEq) * 25.f, 0.f, 1.f);
    }

    // Adaptive noise level — control rate.
    // Used only to seed self-oscillation startup; updating once per
    // output sample is inaudibly identical.
    mInputEnv = std::max(fabsf(input), mInputEnv * mEnvDecay);
    {
      float stateEnergy = fabsf(mS[0]) + fabsf(mS[1]) + fabsf(mS[2]) + fabsf(mS[3]);
      float energy = std::max(mInputEnv, stateEnergy);
      mC.noiseLevel = 1e-2f / (static_cast<float>(mOversample) * (1.f + energy * 1000.f));
    }

    if (mOversample == 4)
      return Process4x(input);
    else if (mOversample == 2)
      return Process2x(input);
    else
      return ProcessSample(input);
  }

private:
  // 2x oversampled
  float Process2x(float input)
  {
    float up[2], down[2];
    mUp1.process_sample(up[0], up[1], input);
    down[0] = ProcessSample(up[0]);
    down[1] = ProcessSample(up[1]);
    return mDown1.process_sample(down);
  }

  // 4x oversampled
  float Process4x(float input)
  {
    float up2x[2];
    mUp1.process_sample(up2x[0], up2x[1], input);

    float down4x[2], down2x[2];

    float up4x_a[2];
    mUp2.process_sample(up4x_a[0], up4x_a[1], up2x[0]);
    down4x[0] = ProcessSample(up4x_a[0]);
    down4x[1] = ProcessSample(up4x_a[1]);
    down2x[0] = mDown2.process_sample(down4x);

    float up4x_b[2];
    mUp2.process_sample(up4x_b[0], up4x_b[1], up2x[1]);
    down4x[0] = ProcessSample(up4x_b[0]);
    down4x[1] = ProcessSample(up4x_b[1]);
    down2x[1] = mDown2.process_sample(down4x);

    return mDown1.process_sample(down2x);
  }

  // Inner per-sample filter at the oversampled rate.
  // No transcendentals on params, no FTZ dependency.
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
    // The IR3109 OTA stages (kOTAScale=0.35) have effective gain < 1
    // at self-oscillation amplitude, pulling pitch flat. dfGain boosts
    // g1 to compensate. Tuned against Juno-6 SN#193284 self-oscillation
    // pitch data: ±5 cents from 55–1760 Hz at R=1.0, ±3 cents at R=0.9.
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

    // Output gain: InputComp * outputGain preserves passband level (1.22x).
    // Self-osc level calibrated from hardware: ~1.07x pulse at R=127.
    return lp4 * 3.22f * mC.outputScale;
  }
};

} // namespace kr106
