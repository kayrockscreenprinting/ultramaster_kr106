#pragma once

#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdint>

#include "KR106ADSR.h"
#include "KR106Oscillators.h"
#include "KR106VCF.h"
#include "KR106VcfFreqJ106.h"

// Complete KR-106 voice: oscillators -> VCF -> ADSR -> VCA

namespace kr106
{

// ============================================================
// KR106Voice — complete per-voice signal chain
// ============================================================
template <typename T> class Voice
{
public:
  // DSP modules
  Oscillators mOsc;
  VCF mVCF;
  ADSR mADSR;

  // Voice control (replaces iPlug2 mInputs ControlRamps)
  double mPitch     = 0.0; // 1V/oct relative to A440
  double mPitchBend = 0.0; // pitch bend in octaves

  // Voice parameters (set by KR106DSP::SetParam via ForEachVoice)
  float mDcoLfo       = 0.f;
  float mDcoPwm       = 0.f;
  float mDcoSub       = 1.f;
  float mDcoNoise     = 0.f;
  float mVcfFreq      = 700.f; // Hz
  float mVcfRes       = 0.f;
  float mVcfEnv       = 0.f;
  float mVcfLfo       = 0.f;
  float mVcfKbd       = 0.f;
  float mBendDco      = 0.f;
  float mBendVcf      = 0.f;
  float mBendLfo      = 0.f;
  float mRawBend      = 0.f; // UI bender lever horizontal [-1, +1]
  float mBenderModAmt = 0.f; // UI bender lever vertical push [0, 1]
  float mOctTranspose = 0.f; // octave shift in semitones (±12)

  bool mSawOn       = true;
  bool mPulseOn     = true;
  bool mSubOn       = false;
  int mPwmMode      = 0;       // -1=LFO, 0=MAN, 1=ENV
  int mVcfEnvInvert = 1;       // 1 or -1
  int mVcaMode      = 0;       // 0=ADSR, 1=Gate
  float mVelocity   = 0.f;     // stored from Trigger()
  T* mSyncOut       = nullptr; // scope sync output (pulse on oscillator phase reset)

  // Shadow phase accumulator for scope sync — runs at un-modulated base
  // frequency so the scope timebase is stable and LFO pitch mod is visible.
  float mScopeSyncPhase = 0.f;
  bool mScopeSyncSub    = false; // sub-oscillator toggle (sync every 2 cycles)

  // Portamento / glide
  float mGlidePitch     = -100.f; // current glide pitch (1V/oct); -100 = uninitialized
  bool mPortaEnabled    = false;
  float mPortaRateParam = 0.f; // knob value [0,1], stored for SR-change recompute
  float mPortaStep      = 0.f; // linear glide step (octaves per sample)

  float mSampleRate = 44100.f;

  // --- Unified D7811G firmware tick state (J106 mode) ---
  // The real D7811G runs one main loop at ~238 Hz that computes ADSR and
  // VCF DAC for each voice in sequence. A single tick accumulator keeps
  // them perfectly synchronized — the VCF always reads the just-updated
  // envelope value from the same loop iteration.
  int mMidiNote           = 60;    // MIDI note number (for firmware 8.8 pitch)
  float mLfoEnvAmp        = 0.f;   // LFO onset envelope (set by DSP per block)
  float mFwTickAccum      = 0.f;   // unified tick accumulator [0, 1)
  float mFwTickStep       = 0.f;   // ticks per sample (kTickRate / sampleRate)
  float mFwEnvNext        = 0.f;   // envelope at current tick
  float mFwEnvSmooth      = 0.f;   // RC-smoothed envelope output
  uint16_t mVcfDacNext    = 0;     // current tick's DAC output
  float mVcfDacSmooth     = 0.f;   // RC-smoothed DAC value (before expo conversion)
  float mDacSmoothCoeff   = 0.f;   // one-pole coefficient for DAC/env smoothing

  // J106 integer parameter cache (set by SetParam, avoids per-sample conversion)
  uint16_t mVcfCutoffInt  = 0;     // 14-bit slider value (0x0000–0x3F80)
  uint8_t mVcfEnvModInt   = 0;     // env mod depth 0–254 (slider × 2)
  uint8_t mVcfKeyTrackInt = 0;     // key track depth 0–254 (slider × 2)
  uint8_t mVcfLfoDepthInt = 0;     // LFO→VCF depth 0–254 (slider × 2)
  uint8_t mVcfBendSensInt = 0;     // bend→VCF sensitivity 0–255

  // Per-voice component tolerance offsets (fixed at construction).
  // Models resistor/capacitor/OTA matching tolerances in the hardware.
  float mVcfFreqOffset = 0.f; // log-freq offset (±5% cutoff)
  float mPitchOffset   = 0.f; // octave offset (±3 cents)
  float mVcaGainScale  = 1.f; // linear gain (±0.5 dB)
  float mPwMinOffset   = 0.f; // ±0.02 around 0.50 (PW range low end)
  float mPwMaxOffset   = 0.f; // ±0.02 around 0.95 (PW range high end)

  void InitVariance(int voiceIndex)
  {
    // Deterministic LCG PRNG seeded by voice index — same offsets every
    // session, modeling one specific unit's fixed component tolerances.
    uint32_t seed = static_cast<uint32_t>(voiceIndex) * 2654435761u + 0x46756E6Bu;
    auto rng      = [&seed]() -> float
    {
      seed = seed * 196314165u + 907633515u;
      return static_cast<float>(seed) / static_cast<float>(0xFFFFFFFF) * 2.f - 1.f;
    };

    mVcfFreqOffset   = 0.f; // rng() * 0.05f;        // ±5% filter cutoff — disabled for testing
    mPitchOffset     = rng() * 3.f / 1200.f; // ±3 cents (in octaves)
    mADSR.mTimeScale = 1.f + rng() * 0.08f;  // ±8% envelope timing
    mVcaGainScale    = 1.f + rng() * 0.06f;  // ±0.5 dB VCA gain
    mPwMinOffset     = rng() * 0.02f;        // PW min: 48–52%
    mPwMaxOffset     = rng() * 0.02f;        // PW max: 93–97%
  }

  // FIXME(kr106) Measure DCO LFO depth vs slider voltage on hardware Juno-6
  // Placeholder: linear, ±4 st assumed until measured
  static float dcoLfoDepth6(float t) { return t * 4.f; }

  // Maps the Juno-6 VCF LFO depth slider (0..1) to a peak pitch deviation
  // in semitones (per direction).
  //
  // CLEAN ROOM IMPLEMENTATION: Derived entirely from hardware measurements
  // on a real Roland Juno-6. No proprietary firmware or schematics used.
  //
  // DEPTH CURVE: Power law with exponent ~1.20, giving a slightly super-linear
  // taper — the upper half of the knob travel covers proportionally more range
  // than the lower half. Measured by comparing semitone deviations at slider
  // positions 0.25, 0.50, 0.75, and 1.00 against a fixed base frequency.
  //
  // MAX DEPTH: 37.0 semitones per direction at t=1.0, confirmed by direct
  // measurement at 596 Hz base on a calibrated unit. Consistent with the
  // Roland published specification of 6 octaves (72 st) total peak-to-peak
  // swing (37 * 2 = 74 st, within measurement tolerance). We will go with the spec.
  //
  // FIXME CALIBRATION NOTE: This function assumes a correctly trimmed LFO integrator.
  // Our 43 year old uncalibrated unit showed ~8% downward bias in the sweep, we assume correctable
  // via the service trim adjustment. On a calibrated unit we assume the sweep is symmetric
  // so this function returns the correct per-direction deviation for both up
  // and down.
  //
  // Returns: peak semitone deviation in each direction (symmetric ±).
  //          At t=0, returns 0 (no modulation).
  //          At t=1, returns ~36.0 st (~3 octaves per direction, ~6 oct total).
  //

  static float vcfLfoDepth6(float t)
  {
    static constexpr float kMaxSemitones = 36.0f; // ±36 st = ~6 oct peak-to-peak
    static constexpr float kExponent     = 1.20f; // power law taper, measured

    return std::pow(t, kExponent) * kMaxSemitones;
  }

  // vcfLfoDepth106()
  // Juno-106 VCF LFO depth: linear mapping confirmed from ROM (IC29 $01B3).
  // Slider value doubled (0–127 → 0–254) and multiplied directly against
  // LFO signal. No curve shaping. ±3.5 octaves (42 st) at maximum.
  static float vcfLfoDepth106(float t)
  {
    static constexpr float kMaxSemitones = 42.0f; // ±3.5 octaves, per spec
    return t * kMaxSemitones;
  }

  // dcoLfoDepth106() - Maps normalized DCO LFO depth slider (0..1) to
  // peak pitch deviation in semitones (±4 st = ±400 cents at max).
  //
  // CLEAN ROOM IMPLEMENTATION: Derived by curve analysis of the Roland Juno-106
  // voice firmware table at ROM address $0A80_lfoDepthTbl, 128 single-byte
  // entries. No table data copied.
  //
  // ANALOG TAPER: The physical slider is a 50K linear pot with a 10K resistor
  // from wiper to ground. This creates a non-linear voltage curve before the
  // ADC: V(x) = x / (1 + kx - kx²) where k = R_pot/R_shunt = 5. The curve
  // compresses the mid/upper range, giving fine control at low depths.
  //
  // FIRMWARE MECHANICS: The returned value (scaled to 0–255 internally) is
  // stored at $FF49_lfoToPitchScaler. Each frame it is multiplied by the LFO
  // delay envelope (0–$FF), then by the current LFO value (0–$1FFF), with
  // three right-shifts on the 16-bit result before adding into the pitch
  // accumulator.
  //
  // TABLE SHAPE: Three segments with increasing slope — opposite to portamento:
  //
  //   i=0..2:   value=0,     dead zone, no LFO modulation
  //   i=3..63:  step +1,     values 1→61   (fine control region)
  //   i=64..95: step +2,     values 62→124 (medium region)
  //   i=96..127: step +4,    values 128→255 (deep modulation, clamped at 255)
  //
  // REFERENCE: Roland Juno-106 voice CPU firmware disassembly, DCO LFO depth
  // table $0A80_lfoDepthTbl. Used at $023A via LDAX (HL+A) indexed lookup.

  static float dcoLfoDepth106(float t)
  {
    // Analog taper: 10K shunt to ground on 50K linear pot.
    // V(x) = x / (1 + kx - kx²), k = R_pot / R_shunt = 5.
    t = t / (1.f + 5.f * t - 5.f * t * t);

    float i = t * 127.f;

    // Dead zone: bottom three table entries are zero.
    // The LFO has no effect until the knob is meaningfully advanced.
    if (i < 3.f)
      return 0.f;

    float coeff;

    if (i < 64.f)
      // Segment 1: fine linear region, +1 per index.
      // Covers most of the knob travel for precise low-depth settings.
      coeff = i - 2.f; // 1 → 61

    else if (i < 96.f)
      // Segment 2: medium linear region, +2 per index.
      coeff = 62.f + 2.f * (i - 64.f); // 62 → 124

    else
      // Segment 3: deep region, +4 per index, hard clamped at 255.
      // The top two table entries are both 0xFF, so the last ~8 indices
      // all saturate at maximum depth.
      coeff = std::min(255.f, 128.f + 4.f * (i - 96.f)); // 128 → 255

    // Scale to semitones: ±400 cents (±4 st) at max depth.
    static constexpr float kMaxSemitones = 4.f;
    return (coeff / 255.f) * kMaxSemitones;
  }

  // portaRate() - Maps normalized portamento slider (0..1) to a glide rate
  // in semitones per second.
  //
  // CLEAN ROOM IMPLEMENTATION: This function was derived by curve analysis of
  // the Roland Juno-6 voice firmware (IC29, voice.bin). The original firmware
  // uses a 128-entry lookup table at ROM address $0A00 to map the portamento
  // slider to a per-frame pitch step coefficient. This function reproduces the
  // observed curve shape without copying any table data.
  //
  // FIRMWARE MECHANICS: The coefficient is used as a linear per-frame add/subtract
  // to an 8.8 fixed-point pitch accumulator (EAH = semitones, EAL = 256ths of a
  // semitone), running at one frame per 4.2ms (238.1 Hz). Portamento glides toward
  // the target note and clamps when it arrives. Rate in semitones/sec:
  //
  //   rate = coeff * 238.1 / 256
  //
  // TABLE SHAPE: Three observed segments in the original firmware table:
  //
  //   i=0:       coeff=0,   instant glide (porta bypassed entirely)
  //   i=1..25:   coeff 255→63, linear descent step -8 per index
  //   i=26..47:  coeff 61→19,  linear descent step -2 per index
  //   i=48..127: coeff 18→1,   exponential decay ~0.9625 per index
  //
  // This gives an overall glide time range of approximately:
  //   fastest (i=1): ~50ms per octave
  //   slowest (i=127): ~12.9 seconds per octave
  //
  // REFERENCE: Roland Juno-6 voice CPU firmware disassembly, portamento
  // coefficient table $0A00_portCoeffTbl, 128 single-byte entries.
  // CPU frame rate: 1/4.2ms = 238.1 Hz. Pitch accumulator resolution: 8.8
  // fixed point (256 steps per semitone).

  static float portaRate(float t)
  {
    // t=0 is a special case: the first table entry is zero, meaning
    // the firmware skips the portamento calculation entirely and jumps
    // straight to the target pitch each frame — instant glide.
    if (t == 0.f)
      return 0.f;

    float i = t * 127.f;
    float coeff;

    if (i <= 25.f)
      // Segment 1: fast glide range, coarse linear taper.
      // Covers the upper quarter of the slider travel.
      // Original table steps by -8 per index: 255, 247, 239 ... 63
      coeff = 255.f - 8.f * (i - 1.f);

    else if (i <= 47.f)
      // Segment 2: medium glide range, finer linear taper.
      // Original table steps by -2 per index: 61, 59, 57 ... 19
      coeff = 63.f - 2.f * (i - 25.f);

    else
      // Segment 3: slow glide range, exponential decay.
      // Original table quantises to integers with ~5 indices per value:
      // 18, 18, 18, 18, 18, 17, 17 ... 1
      // powf(0.9625) reproduces this spacing without copying table data.
      coeff = roundf(18.f * powf(0.9625f, i - 48.f));

    // Convert firmware coefficient to semitones/sec.
    // Caller should divide by sampleRate to get semitones/sample.
    return coeff * 238.1f / 256.f;
  }

  void UpdatePortaCoeff()
  {
    // portaRate() returns semitones/sec; convert to octaves/sample
    float semiPerSec = portaRate(mPortaRateParam);
    mPortaStep       = semiPerSec / (12.f * mSampleRate);
  }

  // J106 DAC output RC filter: uPD7811G R/2R ladder → 10K/0.1µF lowpass (1ms tau)
  static constexpr float kDacRcTau = 0.001f;

  // Precomputed sample-rate constants (defaults for 44100 Hz)
  float mInvNyq = 1.f / 22050.f;
  float mMinCPS = 20.f / 22050.f;

  // Run one D7811G main loop iteration for this voice.
  // Computes ADSR and VCF DAC in sequence, matching the real firmware where
  // both are updated in the same 4.2ms loop pass. The VCF always reads the
  // just-updated mEnvInt — no tick drift possible.
  void FirmwareTick(float lfoRaw)
  {
    // 1. ADSR tick — updates mEnvInt, may transition state
    mADSR.Tick106();
    mFwEnvNext = static_cast<float>(mADSR.mEnvInt) / ADSR::kEnvMax;

    // 2. VCF DAC — reads the just-updated mEnvInt
    // Convert LFO to firmware format: magnitude + polarity
    uint16_t lfoVal = static_cast<uint16_t>(fabsf(lfoRaw) * 0x1FFF);
    bool lfoPolarity = (lfoRaw < 0.f);
    uint8_t depthScalar = static_cast<uint8_t>(mLfoEnvAmp * 255.f);
    uint16_t vcfLfoSignal = kr106::calc_vcf_lfo_signal(
        mVcfLfoDepthInt, depthScalar, lfoVal);

    // Convert bend to firmware format: magnitude + polarity
    uint8_t bendVal = static_cast<uint8_t>(fabsf(mRawBend) * 255.f);
    bool bendPol = (mRawBend < 0.f);
    uint16_t vcfBendAmt = kr106::calc_vcf_bend_amt(mVcfBendSensInt, bendVal);

    // Pitch: 8.8 fixed-point semitones from gliding pitch (follows portamento)
    // mGlidePitch is in octaves relative to A440: MIDI note = glidePitch * 12 + 69
    float glideMidi = mGlidePitch * 12.f + 69.f + mOctTranspose;
    uint16_t pitch88 = static_cast<uint16_t>(
        std::clamp(static_cast<int>(glideMidi * 256.f), 0, 127 << 8));

    bool envPol = (mVcfEnvInvert > 0);
    mVcfDacNext = kr106::calc_vcf_freq(
        mVcfCutoffInt, vcfLfoSignal, vcfBendAmt,
        mVcfEnvModInt, mVcfKeyTrackInt,
        lfoPolarity, bendPol, envPol,
        mADSR.mEnvInt, pitch88);
  }

  bool GetBusy() const { return mADSR.GetBusy(); }

  void SetUnisonPitch(double pitch) { mPitch = pitch; }

  void Trigger(double level, bool isRetrigger)
  {
    // Square-root curve: perceptually even dynamics (linear feels too quiet)
    mVelocity = sqrtf(static_cast<float>(level));

    // Snap glide pitch if portamento is off, or voice has never been triggered
    float newPitch = static_cast<float>(mPitch);
    if (!mPortaEnabled || mGlidePitch < -10.f)
      mGlidePitch = newPitch;

    mADSR.NoteOn();

    // In J106 mode, NoteOn calls Tick106() internally for the immediate
    // first attack step. Snap the RC smoothers to the post-attack value
    // so the envelope and VCF start immediately at the right level.
    if (!mADSR.mJ6Mode)
    {
      mFwEnvNext = mADSR.mEnvNext;
      mFwEnvSmooth = mADSR.mEnvNext; // snap — no ramp from zero

      // Compute initial VCF DAC with the post-attack envelope so the
      // filter cutoff starts at the correct frequency on the first sample.
      float glideMidi = mGlidePitch * 12.f + 69.f + mOctTranspose;
      uint16_t pitch88 = static_cast<uint16_t>(
          std::clamp(static_cast<int>(glideMidi * 256.f), 0, 127 << 8));
      bool envPol = (mVcfEnvInvert > 0);
      mVcfDacNext = kr106::calc_vcf_freq(
          mVcfCutoffInt, 0, 0,
          mVcfEnvModInt, mVcfKeyTrackInt,
          false, false, envPol,
          mADSR.mEnvInt, pitch88);
      mVcfDacSmooth = static_cast<float>(mVcfDacNext);
    }

    if (!isRetrigger)
    {
      mOsc.Reset();
      mVCF.Reset();
      mScopeSyncPhase = 0.f;
      mScopeSyncSub = false;
      // Seed filter state for instant self-oscillation startup.
      // On real hardware the filter is always processing (even when VCA
      // is closed), so self-oscillation is already at full amplitude when
      // a key is pressed. Digitally, idle voices have zero state, so we
      // inject energy proportional to resonance. At res=1.0 the seed of
      // 0.3 matches the steady-state self-oscillation amplitude, giving
      // near-instant startup for patches like Glockenspiel.
      float resSeed = std::max(mVcfRes - 0.7f, 0.f) / 0.3f; // 0 at res<=0.7, 1 at res=1.0
      mVCF.mS[0]    = 0.3f * resSeed;
    }
  }

  void Release() { mADSR.NoteOff(); }

  void SetSampleRateAndBlockSize(double sampleRate, int blockSize)
  {
    (void)blockSize;
    mSampleRate = static_cast<float>(sampleRate);
    mADSR.SetSampleRate(mSampleRate);
    mOsc.Init(mSampleRate);

    // Precomputed constants for VCF frequency calculation.
    // VCF modulation works in log-frequency space; these convert
    // between log-Hz and normalized cutoff (cycles per sample).
    float nyq = mSampleRate * 0.5f;
    mInvNyq   = 1.f / nyq;      // Hz → normalized cutoff
    mMinCPS   = 20.f * mInvNyq; // 20 Hz floor in normalized units

    // Unified D7811G firmware tick rate (238.1 Hz) — drives ADSR + VCF
    mFwTickStep = ADSR::kTickRate / mSampleRate;

    mDacSmoothCoeff = 1.f - expf(-1.f / (kDacRcTau * mSampleRate));

    UpdatePortaCoeff();
  }

  void ProcessSamplesAccumulating(T** inputs, T** outputs, int nInputs, int nOutputs, int startIdx,
                                  int nFrames)
  {
    double pitch     = mPitch;
    double pitchBend = mPitchBend;
    float velocity   = mVelocity;

    // Portamento: glide mGlidePitch toward target per sample; otherwise snap once
    float targetPitch = static_cast<float>(pitch);
    float baseFreq    = 0.f;
    if (!mPortaEnabled)
    {
      mGlidePitch = targetPitch;
      baseFreq    = 440.f * powf(2.f, targetPitch + static_cast<float>(pitchBend));
    }

    // LFO buffer from global modulation (index 0)
    T* lfoBuffer = (nInputs > 0 && inputs[0]) ? inputs[0] : nullptr;
    // Raw LFO triangle (before onset envelope) for J106 integer VCF path (index 1)
    T* lfoRawBuffer = (nInputs > 1 && inputs[1]) ? inputs[1] : nullptr;

    for (int i = startIdx; i < startIdx + nFrames; i++)
    {
      if (mPortaEnabled)
      {
        float diff = targetPitch - mGlidePitch;
        if (mPortaStep > 0.f && fabsf(diff) > mPortaStep)
          mGlidePitch += (diff > 0.f) ? mPortaStep : -mPortaStep;
        else
          mGlidePitch = targetPitch;
        baseFreq = 440.f * powf(2.f, mGlidePitch + static_cast<float>(pitchBend));
      }

      float lfo = lfoBuffer ? static_cast<float>(lfoBuffer[i]) : 0.f;
      float env;

      if (mADSR.mJ6Mode)
      {
        env = mADSR.Process();
      }
      else
      {
        // J106: unified firmware tick drives ADSR + VCF DAC together.
        float rawLfo = lfoRawBuffer ? static_cast<float>(lfoRawBuffer[i]) : 0.f;
        mFwTickAccum += mFwTickStep;
        while (mFwTickAccum >= 1.f)
        {
          mFwTickAccum -= 1.f;
          FirmwareTick(rawLfo);
        }
        // RC smooth the stairstepped DAC output (1ms tau)
        mFwEnvSmooth += (mFwEnvNext - mFwEnvSmooth) * mDacSmoothCoeff;
        env = mFwEnvSmooth;
        mADSR.mEnv = env;
        mADSR.UpdateGateEnv();
      }

      // --- Pulse width modulation ---
      float pw;
      switch (mPwmMode)
      {
        case -1:
          pw = mDcoPwm * (lfo + 1.f) * 0.5f;
          break; // LFO
        case 0:
          pw = mDcoPwm;
          break; // Manual
        case 1:
          pw = mDcoPwm * env;
          break; // ENV
        default:
          pw = mDcoPwm;
      }
      // Per-voice PW calibration variance (hardware component tolerance)
      float pwMin = 0.50f + mPwMinOffset; // [0.48, 0.52]
      float pwMax = 0.95f + mPwMaxOffset; // [0.93, 0.97]
      pw          = pwMin + pw * (pwMax - pwMin);

      // --- Pitch modulation ---
      // mDcoLfo is in semitones (from dcoLfoDepth6/106).
      float lfoSemitones = lfo * (mDcoLfo + mBenderModAmt * mBendLfo);
      float pitchMod =
          mOctTranspose / 12.f + mPitchOffset + lfoSemitones / 12.f + mRawBend * mBendDco;
      float freq = baseFreq * powf(2.f, pitchMod);
      float cps  = freq / mSampleRate;

      // Safety clamp
      if (cps <= 0.f || cps >= 0.5f)
      {
        outputs[0][i] += 0.;
        if (nOutputs > 1)
          outputs[1][i] += 0.;
        continue;
      }

      // --- Oscillators ---
      bool sync    = false;
      float oscOut = mOsc.Process(cps, pw, mSawOn, mPulseOn, mSubOn, mDcoSub, mDcoNoise, sync);

      // Scope sync: shadow accumulator at base pitch (all mod except LFO)
      // so LFO pitch modulation is visible as waveform drift on the scope.
      if (mSyncOut)
      {
        float nonLfoPitch = mOctTranspose / 12.f + mPitchOffset + mRawBend * mBendDco;
        float syncCps = baseFreq * powf(2.f, nonLfoPitch) / mSampleRate;
        mScopeSyncPhase += syncCps;
        if (mScopeSyncPhase >= 1.f)
        {
          mScopeSyncPhase -= 1.f;
          mScopeSyncSub = !mScopeSyncSub;
          if (mScopeSyncSub)
            mSyncOut[i] = T(1);
        }
      }

      // --- VCF frequency calculation ---
      float vcfCPS;
      if (mADSR.mJ6Mode)
      {
        // J6: All modulation sources sum in log-frequency space, modeling
        // voltage summing into the IR3109's exponential converter.
        //
        // Scaling from Juno-6 published specs and circuit analysis:
        //   ENV:  10 octaves at max slider (invert path 1.121× normal,
        //         from IC8 gain asymmetry: R104/R99 vs R103/R98)
        //   LFO:  depth in semitones (from dcoLfoDepth6/106)
        //   KBD:  0–100% keyboard tracking (1.0 = 1V/oct)
        //   Bend: 6 octaves range (tuned independently from LFO)
        static constexpr float kEnvScale = 6.931f; // 10 * ln(2): 10 octaves
        static constexpr float kEnvInvScale =
            7.766f; // 10 * 1.121 * ln(2): inverted path gain asymmetry
        static constexpr float kSemiToLogFreq = 0.05776f; // ln(2)/12: semitones to log-freq

        float envScale = (mVcfEnvInvert > 0) ? kEnvScale : kEnvInvScale;

        float vcfFrq = logf(mVcfFreq) + mVcfFreqOffset;

        vcfFrq += logf(baseFreq / 32.703f) * mVcfKbd; // keyboard tracking: 1.0 = 100% = 1V/oct
        vcfFrq += env * mVcfEnv * envScale * float(mVcfEnvInvert);
        vcfFrq += lfo * mVcfLfo * kSemiToLogFreq;
        vcfFrq += 4.15888f * mRawBend * mBendVcf; // bender (6 oct range, tuned separately)

        vcfCPS = expf(vcfFrq) * mInvNyq;
      }
      else
      {
        // J106: DAC already computed by unified firmware tick above.
        // RC smooth the stairstepped DAC value, then convert through
        // the exponential converter — matches the real hardware signal
        // path: DAC → analog RC → IR3109 expo converter.
        mVcfDacSmooth += (static_cast<float>(mVcfDacNext) - mVcfDacSmooth) * mDacSmoothCoeff;
        float vcfHz = kr106::dacToHz(static_cast<uint16_t>(mVcfDacSmooth));
        vcfCPS = vcfHz * mInvNyq;
      }

      // Clamp to 20 Hz floor (no upper clamp — 2x oversampled TPT is stable to Nyquist)
      vcfCPS = std::max(vcfCPS, mMinCPS);

      // Fade resonance near base-rate Nyquist: self-oscillation above
      // ~18 kHz folds into the audible range during 2x→1x downsampling.
      // Models IR3109 OTA transconductance rolloff near fT limits.
      float res = mVcfRes * (1.f - std::clamp((vcfCPS - 0.8f) * 5.f, 0.f, 1.f));

      float filtered = mVCF.Process(oscOut, vcfCPS, res);

      float signal = filtered;

      // --- VCA ---
      float vcaOut;
      if (mVcaMode)
        vcaOut = signal * mADSR.mGateEnv * velocity * mVcaGainScale; // Gate mode
      else
        vcaOut = signal * env * velocity * mVcaGainScale; // ADSR mode

      // Accumulate mono (chorus does stereo later)
      outputs[0][i] += static_cast<T>(vcaOut);
      if (nOutputs > 1)
        outputs[1][i] += static_cast<T>(vcaOut);
    }
  }
};

} // namespace kr106
