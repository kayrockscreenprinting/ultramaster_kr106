#pragma once

#include <cmath>
#include <algorithm>
#include <cstring>

#include "SynthVoice.h"
#include "KR106Oscillators.h"
#include "FPUUpsampler2x.h"
#include "FPUDownsampler2x.h"

// Complete KR-106 voice: oscillators -> VCF -> ADSR -> VCA
// Ported from kr106_voice.h/kr106_voice.C

namespace kr106 {

// ============================================================
// 4-pole TPT Cascade LPF (models IR3109 OTA filter)
// ============================================================
// Four 1-pole trapezoidal integrators with global resonance feedback.
// State variables are integrator outputs — coefficient-independent,
// so per-sample cutoff modulation is artifact-free. No sin/cos needed.
struct VCF
{
  float mS[4] = {}; // integrator states
  uint32_t mNoiseSeed = 123456789u; // thermal noise PRNG state

  // 2x oversampling: HIIR polyphase filters for anti-imaging/aliasing
  hiir::Upsampler2xFPU<12, float> mUpsampler;
  hiir::Downsampler2xFPU<12, float> mDownsampler;

  VCF()
  {
    static constexpr double kCoeffs2x[12] = {
      0.036681502163648017, 0.13654762463195794, 0.27463175937945444,
      0.42313861743656711, 0.56109869787919531, 0.67754004997416184,
      0.76974183386322703, 0.83988962484963892, 0.89226081800387902,
      0.9315419599631839,  0.96209454837808417, 0.98781637073289585
    };
    mUpsampler.set_coefs(kCoeffs2x);
    mDownsampler.set_coefs(kCoeffs2x);
  }

  void Reset()
  {
    mS[0] = mS[1] = mS[2] = mS[3] = 0.f;
    mUpsampler.clear_buffers();
    mDownsampler.clear_buffers();
  }

  // OTA saturation on the feedback path (Padé tanh approximant).
  // Models the IR3109's resonance feedback re-entering through an OTA
  // differential pair: tanh(V_diff / 2V_T). This is what limits
  // resonance amplitude and gives the filter its character.
  static float OTASat(float x)
  {
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
  }

  // frq: normalized cutoff [0, ~0.9] where 1.0 = Nyquist (base rate)
  // res: resonance amount [0, 1]
  // 2x oversampled: upsample input, run filter at 2x rate, downsample
  float Process(float input, float frq, float res)
  {
    float up[2], down[2];
    mUpsampler.process_sample(up[0], up[1], input);

    // Halve frq for the oversampled domain (Nyquist is 2x higher)
    float frq2x = frq * 0.5f;
    down[0] = ProcessSample(up[0], frq2x, res);
    down[1] = ProcessSample(up[1], frq2x, res);

    return mDownsampler.process_sample(down);
  }

private:
  // Internal per-sample filter at the oversampled rate
  float ProcessSample(float input, float frq, float res)
  {
    // Warp cutoff to continuous-time frequency, then to integrator coeff
    float g = tanf(std::min(frq, 0.85f) * static_cast<float>(M_PI) * 0.5f);

    // Adaptive thermal noise: models BA662/IR3109 OTA bias current noise.
    // High level when filter is quiet (to seed self-oscillation startup),
    // fades to inaudible once oscillation is established. This keeps
    // pure self-oscillation patches (e.g. Glockenspiel) clean while
    // still providing reliable oscillation seeding.
    mNoiseSeed = mNoiseSeed * 196314165u + 907633515u;
    float white = static_cast<float>(mNoiseSeed) / static_cast<float>(0xFFFFFFFFu) * 2.f - 1.f;
    float stateEnergy = fabsf(mS[0]) + fabsf(mS[1]) + fabsf(mS[2]) + fabsf(mS[3]);
    float noiseLevel = 1e-3f / (1.f + stateEnergy * 1000.f);
    input += white * noiseLevel;

    // Feedback amount (k=0 no resonance, k=4 self-oscillation threshold)
    float k = res * 4.f;

    // Precompute gains for the 4-pole cascade solution
    float g1 = g / (1.f + g);  // one-pole gain

    // Linear cascade: the predictor is exact, so the filter is
    // unconditionally stable regardless of modulation speed.
    float G = g1 * g1 * g1 * g1;

    // OTA bandwidth rolloff: the IR3109's transconductance drops at
    // high frequencies due to transistor fT limits. Below frq=0.3
    // (where aliased harmonics fall above audible range), allow full
    // self-oscillation character. Above 0.3, progressively reduce the
    // loop gain to prevent the resonance limit cycle from producing
    // audible aliased harmonics near Nyquist.
    float maxLoop = 1.2f - std::max(frq - 0.3f, 0.f);
    maxLoop = std::max(maxLoop, 0.4f);
    float maxK = maxLoop / std::max(G, 1e-6f);
    k = std::min(k, maxK);

    float S = mS[0] * g1 * g1 * g1
            + mS[1] * g1 * g1
            + mS[2] * g1
            + mS[3];

    // Feedback-corrected input — OTA saturation on the feedback sum
    // limits resonance amplitude. This is where the analog character
    // lives: the feedback OTA is what shapes the resonance peak.
    float u = (input - k * OTASat(S)) / (1.f + k * G);

    // 4 cascaded linear 1-pole TPT stages.
    // Linear stages ensure the predictor above is exact, preventing
    // the instability that per-stage nonlinearity caused during fast
    // cutoff sweeps with high resonance.
    float v, s;
    s = mS[0]; v = g1 * (u - s);   mS[0] = s + 2.f * v; float lp1 = s + v;
    s = mS[1]; v = g1 * (lp1 - s); mS[1] = s + 2.f * v; float lp2 = s + v;
    s = mS[2]; v = g1 * (lp2 - s); mS[2] = s + 2.f * v; float lp3 = s + v;
    s = mS[3]; v = g1 * (lp3 - s); mS[3] = s + 2.f * v; float lp4 = s + v;

    // Flush denormals from integrator states
    for (auto& st : mS)
      if (fabsf(st) < 1e-15f) st = 0.f;

    return lp4;
  }
};

// ============================================================
// ADSR Envelope — models the IR3R01 OTA envelope generator
// ============================================================
// Key behaviors matching the hardware:
// - Attack overshoot: linear ramp doesn't clamp at 1.0, so faster
//   attacks overshoot more (~2-3% at 1ms), giving percussive "bite"
// - Decay rate independent of sustain: fixed exponential toward 0,
//   stopped when it crosses the sustain level. The decay knob sets
//   the time constant, sustain only sets where it stops.
// - Separate decay/release multipliers (no shared mM state)
struct ADSR
{
  enum State { kAttack, kDecay, kSustain, kRelease, kFinished };
  static constexpr float kGateSlope = 1.f / 32.f;
  static constexpr float kMinLevel     = 0.001f;  // -60dB: calibrates decay/release time constants
  static constexpr float kSilence      = 1e-5f;   // -100dB: release termination threshold
  static constexpr float kAttackTarget = 1.5f;     // RC charge target (above comparator threshold of 1.0)

  State mState = kFinished;
  float mEnv = 0.f;
  float mGateEnv = 0.f;
  float mAttackRate = 0.f;   // linear per-sample increment (Juno-106 mode)
  float mAttackCoeff = 0.f;  // one-pole RC coefficient (Juno-6 mode)
  bool  mExponentialAttack = false; // true = Juno-6 RC charging attack
  float mDecayRate = 0.f;    // linear rate (for sustain tracking)
  float mDecayMul = 1.f;     // fixed exponential multiplier (toward 0)
  float mSustain = 1.f;
  float mReleaseMul = 1.f;   // fixed exponential multiplier (toward 0)
  float mSampleRate = 44100.f;
  float mTimeScale = 1.f;    // per-voice component tolerance (scales ms times)

  void SetSampleRate(float sr) { mSampleRate = sr; }

  void SetAttack(float ms)
  {
    mAttackRate = 1000.f / (ms * mTimeScale * mSampleRate);
  }

  // Juno-6 exponential attack: RC charge toward kAttackTarget (1.5),
  // reaching 1.0 in exactly ms milliseconds
  void SetAttackExp(float ms)
  {
    float T = ms * mTimeScale * mSampleRate / 1000.f;
    mAttackCoeff = 1.f - expf(-logf(3.f) / T);
  }

  void SetDecay(float ms)
  {
    // Fixed-rate exponential decay toward 0. Reaches -60dB in ms.
    // Rate is independent of sustain level.
    mDecayRate = 1000.f / (ms * mTimeScale * mSampleRate);
    mDecayMul = expf(logf(kMinLevel) * mDecayRate);
  }

  void SetSustain(float s) { mSustain = s; }

  void SetRelease(float ms)
  {
    float releaseRate = 1000.f / (ms * mTimeScale * mSampleRate);
    mReleaseMul = expf(logf(kMinLevel) * releaseRate);
  }

  void NoteOn() { mState = kAttack; }
  void NoteOff() { mState = kRelease; }

  bool GetBusy() const { return mState != kFinished || mGateEnv > 0.f; }

  float Process()
  {
    switch (mState)
    {
      case kAttack:
        if (mExponentialAttack)
          mEnv += (kAttackTarget - mEnv) * mAttackCoeff; // Juno-6: RC charge curve
        else
          mEnv += mAttackRate; // Juno-106: linear ramp (overshoot ~2-3% at 1ms)
        mGateEnv += kGateSlope;
        if (mEnv >= 1.f)
          mState = kDecay;
        break;

      case kDecay:
        mEnv *= mDecayMul;
        mGateEnv += kGateSlope;
        if (mEnv <= mSustain)
        {
          mEnv = mSustain;
          mState = kSustain;
        }
        break;

      case kSustain:
        // Smoothly track sustain level changes
        if (mEnv < mSustain)
        {
          // Sustain raised: ramp up at 3x decay rate
          mEnv += 3.f * mDecayRate;
          if (mEnv > mSustain) mEnv = mSustain;
        }
        else if (mEnv > mSustain)
        {
          // Sustain lowered: decay at normal rate
          mEnv *= mDecayMul;
          if (mEnv < mSustain) mEnv = mSustain;
        }
        break;

      case kRelease:
        mGateEnv -= kGateSlope;
        mEnv *= mReleaseMul;
        if (mEnv < kSilence)
        {
          mEnv = 0.f;
          mState = kFinished;
        }
        break;

      case kFinished:
        mGateEnv -= kGateSlope;
        break;
    }

    mGateEnv = std::clamp(mGateEnv, 0.f, 1.f);
    return mEnv;
  }
};

// ============================================================
// KR106Voice — complete per-voice signal chain as SynthVoice
// ============================================================
template <typename T>
class Voice : public iplug::SynthVoice
{
public:
  // DSP modules
  Oscillators mOsc;
  VCF mVCF;
  ADSR mADSR;

  // Voice parameters (set by KR106DSP::SetParam via ForEachVoice)
  float mDcoLfo = 0.f;
  float mDcoPwm = 0.f;
  float mDcoSub = 1.f;
  float mDcoNoise = 0.f;
  float mVcfFreq = 700.f;    // Hz
  float mVcfRes = 0.f;
  float mVcfEnv = 0.f;
  float mVcfLfo = 0.f;
  float mVcfKbd = 0.f;
  float mBendDco = 0.f;
  float mBendVcf = 0.f;
  float mBendLfo = 0.f;
  float mRawBend = 0.f; // UI bender lever value [-1, +1]
  float mOctTranspose = 0.f; // octave shift in semitones (±12)
  bool mSawOn = true;
  bool mPulseOn = true;
  bool mSubOn = false;
  int mPwmMode = 0;      // -1=LFO, 0=MAN, 1=ENV
  int mVcfEnvInvert = 1; // 1 or -1
  int mVcaMode = 0;      // 0=ADSR, 1=Gate
  float mVelocity = 0.f; // stored from Trigger()
  T* mSyncOut = nullptr;  // scope sync output (pulse on oscillator phase reset)

  // Portamento / glide
  float mGlidePitch = -100.f;  // current glide pitch (1V/oct); -100 = uninitialized
  bool  mPortaEnabled = false;
  float mPortaRateParam = 0.f; // knob value [0,1], stored for SR-change recompute
  float mPortaCoeff = 0.f;     // per-sample smoothing coeff toward target pitch

  float mSampleRate = 44100.f;

  // Per-voice component tolerance offsets (fixed at construction).
  // Models resistor/capacitor/OTA matching tolerances in the hardware.
  float mVcfFreqOffset = 0.f;  // log-freq offset (±5% cutoff)
  float mPitchOffset = 0.f;    // octave offset (±3 cents)
  float mVcaGainScale = 1.f;   // linear gain (±0.5 dB)

  void InitVariance(int voiceIndex)
  {
    // Deterministic LCG PRNG seeded by voice index — same offsets every
    // session, modeling one specific unit's fixed component tolerances.
    uint32_t seed = static_cast<uint32_t>(voiceIndex) * 2654435761u + 0x46756E6Bu;
    auto rng = [&seed]() -> float {
      seed = seed * 196314165u + 907633515u;
      return static_cast<float>(seed) / static_cast<float>(0xFFFFFFFF) * 2.f - 1.f;
    };

    mVcfFreqOffset = rng() * 0.05f;            // ±5% filter cutoff
    mPitchOffset = rng() * 3.f / 1200.f;       // ±3 cents (in octaves)
    mADSR.mTimeScale = 1.f + rng() * 0.08f;    // ±8% envelope timing
    mVcaGainScale = 1.f + rng() * 0.06f;       // ±0.5 dB VCA gain
  }

  void UpdatePortaCoeff()
  {
    // Cubic mapping: knob 0→0s, 1→3s glide time constant
    float t = mPortaRateParam * mPortaRateParam * mPortaRateParam * 3.f;
    mPortaCoeff = (t > 0.001f) ? expf(-1.f / (t * mSampleRate)) : 0.f;
  }

  // Precomputed sample-rate constants (defaults for 44100 Hz)
  float mLogNyq = 9.30792f;       // log(22050)
  float mInvNyq = 1.f / 22050.f;
  float mMinCPS = 20.f / 22050.f;

  bool GetBusy() const override { return mADSR.GetBusy(); }

  void SetUnisonPitch(double pitch)
  {
    mInputs[iplug::kVoiceControlPitch].endValue   = pitch;
    mInputs[iplug::kVoiceControlPitch].startValue = pitch;
  }

  void Trigger(double level, bool isRetrigger) override
  {
    mVelocity = static_cast<float>(level);

    // Snap glide pitch if portamento is off, or voice has never been triggered
    float newPitch = static_cast<float>(mInputs[iplug::kVoiceControlPitch].endValue);
    if (!mPortaEnabled || mGlidePitch < -10.f)
      mGlidePitch = newPitch;

    mADSR.NoteOn();
    if (!isRetrigger)
    {
      mOsc.Reset();
      mVCF.Reset();
      // Seed filter state for instant self-oscillation startup.
      // On real hardware the filter is always processing (even when VCA
      // is closed), so self-oscillation is already at full amplitude when
      // a key is pressed. Digitally, idle voices have zero state, so we
      // inject energy proportional to resonance. At res=1.0 the seed of
      // 0.3 matches the steady-state self-oscillation amplitude, giving
      // near-instant startup for patches like Glockenspiel.
      float resSeed = std::max(mVcfRes - 0.7f, 0.f) / 0.3f; // 0 at res<=0.7, 1 at res=1.0
      mVCF.mS[0] = 0.3f * resSeed;
    }
  }

  void Release() override
  {
    mADSR.NoteOff();
  }

  void SetSampleRateAndBlockSize(double sampleRate, int blockSize) override
  {
    mSampleRate = static_cast<float>(sampleRate);
    mADSR.SetSampleRate(mSampleRate);
    float nyq = mSampleRate * 0.5f;
    mLogNyq = logf(nyq);
    mInvNyq = 1.f / nyq;
    mMinCPS = 20.f * mInvNyq;
    UpdatePortaCoeff();
  }

  void ProcessSamplesAccumulating(T** inputs, T** outputs,
    int nInputs, int nOutputs, int startIdx, int nFrames) override
  {
    // Voice control ramps from MidiSynth
    double pitch = mInputs[iplug::kVoiceControlPitch].endValue;
    double pitchBend = mInputs[iplug::kVoiceControlPitchBend].endValue;
    float velocity = mVelocity;

    // Portamento: glide mGlidePitch toward target per sample; otherwise snap once
    float targetPitch = static_cast<float>(pitch);
    float baseFreq = 0.f;
    if (!mPortaEnabled)
    {
      mGlidePitch = targetPitch;
      baseFreq = 440.f * powf(2.f, targetPitch + static_cast<float>(pitchBend));
    }

    // LFO buffer from global modulation (index 0)
    T* lfoBuffer = (nInputs > 0 && inputs[0]) ? inputs[0] : nullptr;

    for (int i = startIdx; i < startIdx + nFrames; i++)
    {
      if (mPortaEnabled)
      {
        mGlidePitch = mPortaCoeff * mGlidePitch + (1.f - mPortaCoeff) * targetPitch;
        baseFreq = 440.f * powf(2.f, mGlidePitch + static_cast<float>(pitchBend));
      }

      float lfo = lfoBuffer ? static_cast<float>(lfoBuffer[i]) : 0.f;
      float env = mADSR.Process();

      // --- Pulse width modulation ---
      float pw;
      switch (mPwmMode)
      {
        case -1: pw = mDcoPwm * (lfo + 1.f) * 0.5f; break; // LFO
        case  0: pw = mDcoPwm; break;                        // Manual
        case  1: pw = mDcoPwm * env; break;                   // ENV
        default: pw = mDcoPwm;
      }
      pw = 0.52f + pw * 0.46f; // scale to [0.52, 0.98]

      // --- Pitch modulation ---
      // Combined into single exp2: octave transpose + LFO + bender
      float pitchMod = mOctTranspose / 12.f
        + mPitchOffset
        + 2.f * lfo * (mDcoLfo + fabsf(mRawBend) * mBendLfo) / 12.f
        + mRawBend * mBendDco;
      float freq = baseFreq * powf(2.f, pitchMod);
      float cps = freq / mSampleRate;

      // Safety clamp
      if (cps <= 0.f || cps >= 0.5f)
      {
        outputs[0][i] += 0.;
        if (nOutputs > 1) outputs[1][i] += 0.;
        continue;
      }

      // --- Oscillators ---
      bool sync = false;
      float oscOut = mOsc.Process(cps, pw, mSawOn, mPulseOn, mSubOn, mDcoSub, mDcoNoise, sync);

      if (sync)
      {
        if (mSyncOut) mSyncOut[i] = T(1);
      }

      // --- VCF frequency calculation (sample-rate independent) ---
      float vcfFrq = logf(mVcfFreq) + mVcfFreqOffset;                        // base cutoff (Hz) + tolerance
      vcfFrq += logf(baseFreq / 32.703f) * mVcfKbd;                       // keyboard tracking (key CV only, not LFO-modulated pitch)
      vcfFrq += mLogNyq * env * mVcfEnv * 0.73f * mVcfEnvInvert;          // envelope
      vcfFrq += mLogNyq * lfo * mVcfLfo * 0.2f;                           // LFO
      vcfFrq += 4.15888f * mRawBend * mBendVcf;                           // bender (6 oct range)

      float vcfCPS = expf(vcfFrq) * mInvNyq;
      vcfCPS = std::clamp(vcfCPS, mMinCPS, 0.82f);

      float filtered = mVCF.Process(oscOut, vcfCPS, mVcfRes);

      // --- VCA ---
      float vcaOut;
      if (mVcaMode)
        vcaOut = filtered * mADSR.mGateEnv * velocity * mVcaGainScale; // Gate mode
      else
        vcaOut = filtered * env * velocity * mVcaGainScale;              // ADSR mode

      // Accumulate mono (chorus does stereo later)
      outputs[0][i] += static_cast<T>(vcaOut);
      if (nOutputs > 1) outputs[1][i] += static_cast<T>(vcaOut);
    }
  }
};

} // namespace kr106
