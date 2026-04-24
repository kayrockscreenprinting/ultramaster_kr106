#pragma once

#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdint>

#include "KR106ADSR.h"
#include "KR106Oscillators.h"
#include "KR106OscillatorsWT.h"
#include "KR106Resampler.h"
#include "KR106VCA.h"
#include "KR106VCF_OPTIMIZED.h"
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
  OscillatorsWT mOsc;
  Oscillators mOscBLEP;
  VCF mVCF;
  ADSR mADSR;

  // Oscillator mode: 0 = wavetable at 1x + upsampling (default)
  //                  1 = polyBLEP at oversampled rate (no upsampling)
  int mOscMode = 0;

  // Voice control (replaces iPlug2 mInputs ControlRamps)
  double mPitch     = 0.0; // 1V/oct relative to A440
  double mPitchBend = 0.0; // pitch bend in octaves

  // Voice parameters (set by KR106DSP::SetParam via ForEachVoice)
  float mDcoLfo       = 0.f;
  float mDcoPwm       = 0.f;
  float mDcoSub       = 1.f; // pre-computed from dcoSubLevel (normalized to 1.0)
  float mDcoNoise     = 0.f;     // pre-computed from dcoNoiseLevel_j6()
  float mVcfFreq      = 700.f; // Hz (J6 mode)
  float mVcfFreqJ60   = 700.f; // Hz (J60 mode)
  float mVcfFreqJ106  = 700.f; // Hz (J106 mode)
  Model mModel = kJ106;
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
  // Returns the scope sync frequency: base pitch with octave transpose,
  // pitch offset, and bend, but without LFO (so scope timebase is stable).
  float GetScopeSyncCPS() const
  {
    float nonLfoPitch = mOctTranspose / 12.f + mPitchOffset + mRawBend * mBendDco;
    float freq = 440.f * powf(2.f, mGlidePitch + nonLfoPitch);
    return freq / mSampleRate;
  }

  // Portamento / glide
  float mGlidePitch     = -100.f; // current glide pitch (1V/oct); -100 = uninitialized
  bool mPortaEnabled    = false;
  float mPortaRateParam = 0.f; // knob value [0,1], stored for SR-change recompute
  float mPortaStep      = 0.f; // linear glide step (octaves per sample)

  float mSampleRate = 44100.f;

  // Running peak of the oscillator output, used to drive the VCF's
  // input-envelope follower at base rate (the env is only used to
  // scale noise seeding, so control-rate updates are audibly identical).
  float mLastOscPeak = 0.f;

  // BA662 VCA analog slew: one-pole RC on VCA gain, models the physical
  // response time of the VCA circuit. Both gate and envelope modes pass
  // through this. Prevents clicks on instant transitions (ATK=0, gate on/off).
  float mVcaSlew      = 0.f;
  float mVcaSlewCoeff = 0.f;

  // Per-voice upsampler pair: 1× → 2× → 4×.
  // Oscillator runs at base rate (cheap); its output is upsampled to
  // 4× before feeding into the VCF. The mix bus then sums all voices
  // at 4× and a single shared decimator pair (in KR106DSP) drops back
  // to base rate, so we pay upsampling per voice but downsampling
  // only once across the whole mix.
  int mOversample = 4;         // 1, 2, or 4 — set by DSP on oversample change
  kr106::Upsampler2x mOscUp1;  // 1× → 2× (used at 2x and 4x)
  kr106::Upsampler2x mOscUp2;  // 2× → 4× (used at 4x only)

  // --- Unified D7811G firmware tick state (J106 mode) ---
  // The real D7811G runs one main loop at ~234 Hz (~4.27ms period — emergent
  // from firmware timing; canonical value in ADSR::kLoopPeriodMs). Each DAC
  // channel is written sequentially, not simultaneously. The multiplexed DAC
  // takes one loop pass to cycle through all 23 channels. Each channel's RC
  // output filter starts integrating from the moment its DAC is written, so
  // channels written earlier in the loop have more settling time.
  //
  // Per-voice VCF and VCA CVs are interleaved: VCF_N then VCA_N.
  // VCF leads VCA by ~0.125ms (voices 1-5) or ~0.403ms (voice 6).
  // Voice-to-voice spacing is ~0.287ms.
  //
  // We model this with separate sub-tick phase offsets for VCF and VCA
  // within each tick period. The RC smoothers run every sample; the
  // "next" values update at their respective phase offsets.
  int mVoiceIndex         = 0;     // set by InitVariance
  int mMidiNote           = 60;    // MIDI note number (for firmware 8.8 pitch)
  float mLfoEnvAmp        = 0.f;   // LFO onset envelope (set by DSP per block)
  float mFwTickAccum      = 0.f;   // main loop tick accumulator [0, 1)
  float mFwTickStep       = 0.f;   // ticks per sample (kTickRate / sampleRate)
  float mFwEnvPending     = 0.f;   // computed by FirmwareTick, not yet written to DAC
  float mFwEnvNext        = 0.f;   // latched to DAC at VCA phase offset
  uint16_t mVcfDacPending = 0;     // computed by FirmwareTick, not yet written to DAC
  uint16_t mVcfDacNext    = 0;     // latched to DAC at VCF phase offset
  bool  mVcfDacUpdated    = false; // has VCF DAC been written this tick?
  bool  mVcaDacUpdated    = false; // has VCA (env) DAC been written this tick?
  float mVcfPhase         = 0.f;   // fractional tick offset for VCF DAC write
  float mVcaPhase         = 0.f;   // fractional tick offset for VCA DAC write

  // DAC timeline phase offsets per voice (from firmware cycle count analysis).
  // Expressed as fraction of the main-loop tick period.
  //
  // The hardware has exactly 6 DAC "slots" for per-voice VCF/VCA writes.
  // Extended-polyphony modes (8, 10 voices) pair extra logical voices onto
  // these same 6 physical slots — see kVoiceSlotMap below. This preserves
  // authentic modulation timing regardless of polyphony setting, at the cost
  // that paired voices share a DAC-write sample moment (their envelope,
  // pitch, and audio all remain independent).
  static constexpr float kTickPeriodMs = ADSR::kLoopPeriodMs;
  static constexpr float kVcfPhaseTable[6] = {
    2.2055f / kTickPeriodMs,  // slot 0
    2.4929f / kTickPeriodMs,  // slot 1
    2.7803f / kTickPeriodMs,  // slot 2
    3.0677f / kTickPeriodMs,  // slot 3
    3.3552f / kTickPeriodMs,  // slot 4
    3.6426f / kTickPeriodMs,  // slot 5
  };
  static constexpr float kVcaPhaseTable[6] = {
    2.3308f / kTickPeriodMs,  // slot 0
    2.6182f / kTickPeriodMs,  // slot 1
    2.9057f / kTickPeriodMs,  // slot 2
    3.1931f / kTickPeriodMs,  // slot 3
    3.4805f / kTickPeriodMs,  // slot 4
    4.0451f / kTickPeriodMs,  // slot 5 (longer gap — ADC/bend block on hardware)
  };

  // Logical voice index -> physical DAC slot (0-5).
  //
  // Voices 0-5 map directly to their hardware-equivalent slots.
  // Voices 6-9 ride shotgun on the uniformly-spaced slots 1-4, deliberately
  // avoiding slot 5 (which has the atypical 0.56 ms post-ADC gap). This
  // keeps extra voices on symmetric timing so voice-stealing and
  // round-robin allocation behave consistently across the pool.
  //
  // Size is the maximum polyphony the voice pool supports; runtime code
  // uses only the first N entries based on the selected polyphony setting.
  static constexpr int kMaxPolyphony = 10;
  static constexpr int kVoiceSlotMap[kMaxPolyphony] = {
    0, 1, 2, 3, 4, 5,   // voices 0-5: hardware-authentic 6-voice layout
    1, 2, 3, 4          // voices 6-9: paired onto regular slots 1-4
  };

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

  // Number of variance parameters per voice
  static constexpr int kNumVarianceParams = 6;

  // Get/set variance by index (for UI grid)
  float GetVariance(int idx) const
  {
    switch (idx) {
      case 0: return mVcfFreqOffset;
      case 1: return mPitchOffset;
      case 2: return mADSR.mTimeScale - 1.f; // store as offset from 1.0
      case 3: return mVcaGainScale - 1.f;     // store as offset from 1.0
      case 4: return mPwMinOffset;
      case 5: return mPwMaxOffset;
      default: return 0.f;
    }
  }

  void SetVariance(int idx, float v)
  {
    switch (idx) {
      case 0: mVcfFreqOffset   = v; break;
      case 1: mPitchOffset     = v; break;
      case 2: mADSR.mTimeScale = 1.f + v; break;
      case 3: mVcaGainScale    = 1.f + v; break;
      case 4: mPwMinOffset     = v; break;
      case 5: mPwMaxOffset     = v; break;
    }
  }

  // Variance parameter metadata for UI display
  struct VarianceInfo {
    const char* name;     // column header
    float range;          // max absolute value
    float step;           // drag/arrow increment
    float displayScale;   // multiply raw value for display
    float displayOffset;  // add after scaling (for absolute display)
    const char* unit;     // display unit suffix
  };

  static const VarianceInfo& GetVarianceInfo(int idx)
  {
    static const VarianceInfo info[kNumVarianceParams] = {
      { "VCF FRQ", 0.10f,  0.01f,        100.f,  0.f, "cts" }, // ±10 cts cutoff
      { "DCO FRQ", 0.025f, 1.f/1200.f, 1200.f,  0.f, "cts" }, // ±30 cts, step 1 ct
      { "ADSR",    0.15f,  0.01f,       100.f,  0.f, "pct" }, // ±15% timing
      { "VCA",     0.12f,  0.01f,       100.f,  0.f, "pct" }, // ±12% gain
      { "PW Lo",   0.05f,  0.01f,       100.f, 50.f, "pct" }, // 48–52% (base 50)
      { "PW Hi",   0.05f,  0.01f,       100.f, 95.f, "pct" }, // 93–97% (base 95)
    };
    return info[idx];
  }

  void InitVariance(int voiceIndex)
  {
    mVoiceIndex = voiceIndex;
    // Map logical voice index -> physical DAC slot. Voices beyond the
    // first 6 are paired onto slots 1-4 (see kVoiceSlotMap comment above).
    int slot = (voiceIndex >= 0 && voiceIndex < kMaxPolyphony)
               ? kVoiceSlotMap[voiceIndex]
               : 0;
    mVcfPhase = kVcfPhaseTable[slot];
    mVcaPhase = kVcaPhaseTable[slot];
    // Deterministic LCG PRNG seeded by voice index — same offsets every
    // session, modeling one specific unit's fixed component tolerances.
    uint32_t seed = static_cast<uint32_t>(voiceIndex) * 2654435761u + 0x46756E6Bu;
    auto rng      = [&seed]() -> float
    {
      seed = seed * 196314165u + 907633515u;
      return static_cast<float>(seed) / static_cast<float>(0xFFFFFFFF) * 2.f - 1.f;
    };

    mVcfFreqOffset   = rng() * 0.05f;        // ±5% filter cutoff
    mPitchOffset     = rng() * 3.f / 1200.f; // ±3 cents (in octaves)
    mADSR.mTimeScale = 1.f + rng() * 0.08f;  // ±8% envelope timing
    mVcaGainScale    = 1.f + rng() * 0.06f;  // ±0.5 dB VCA gain
    mPwMinOffset     = rng() * 0.02f;        // PW min: 48–52%
    mPwMaxOffset     = rng() * 0.02f;        // PW max: 93–97%
  }

  // DCO LFO depth calibrated from Juno-6 strobe tuner measurements
  // at 10 slider positions (base=1048 Hz, peak semitone deviation):
  //   slider  min_Hz  max_Hz  peak_st
  //   0.1     1045    1050    0.041
  //   0.2     1039    1055    0.132
  //   0.3     1035    1059    0.198
  //   0.4     1030    1065    0.289
  //   0.5     1024    1073    0.405
  //   0.6     1019    1078    0.487
  //   0.7      994    1108    0.940
  //   0.8      967    1142    1.440
  //   0.9      934    1184    2.053
  //   1.0      909    1217    2.526
  // Circuit: A10K log taper pot (EVA TOHC14A14) -> R5 82K -> summing
  // node with Tune (R7 47K) and Bender (R6 22K), then op amp gain
  // 20x (1K/20K).
  //
  // TAPER ANALYSIS: The curve is consistent with a standard two-segment
  // carbon A-taper pot, not a smooth logarithmic element. Normalized to
  // max depth, the midpoint (t=0.5) sits at 16% -- right in the standard
  // A-taper spec of 10-20% at 50% rotation. The slope jumps ~3x between
  // t=0.6 and t=0.7, indicating a knee at ~63% rotation typical of
  // Japanese carbon A-taper pots (Alps, Noble, EVA). Slight curvature
  // within each segment is normal for carbon elements and loading through
  // the 82K series resistor. No datasheet found for EVA TOHC14A14.
  static float dcoLfoDepth6(float t)
  {
    static constexpr float kTable[11] = {
      0.f, 0.041f, 0.132f, 0.198f, 0.289f, 0.405f,
      0.487f, 0.940f, 1.440f, 2.053f, 2.526f
    };
    float idx = t * 10.f;
    int i0 = std::min(static_cast<int>(idx), 9);
    float frac = idx - i0;
    return kTable[i0] + frac * (kTable[i0 + 1] - kTable[i0]);
  }

  // dcoLfoDepth_j60() - J60: same IR3109 circuit, linear pot.
  // TODO: model 50KB linear pot + 10K trim + PNP transistor CV path.
  static float dcoLfoDepth_j60(float t) { return dcoLfoDepth6(t); }

  // dcoLfoDepth_j106() - wrapper for consistent naming.
  static float dcoLfoDepth_j106(float t) { return dcoLfoDepth106(t); }

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

  // vcfLfoDepth_j60() - J60: same IR3109 circuit as J6.
  static float vcfLfoDepth_j60(float t) { return vcfLfoDepth6(t); }

  // vcfLfoDepth_j106() - wrapper for consistent naming.
  static float vcfLfoDepth_j106(float t) { return vcfLfoDepth106(t); }

  // vcfEnvDepth6() - Maps normalized VCF ENV MOD slider (0..1) to
  // modulation depth in log-frequency space (natural log units).
  //
  // CLEAN ROOM IMPLEMENTATION: Derived from hardware measurements on a
  // real Juno-6. VCF cutoff Hz measured at 11 slider positions with
  // sustain at full (envelope = 1.0), two overlapping measurement sets
  // stitched at slider 0.5. Base VCF freq calibrated to isolate the
  // env mod contribution.
  //
  // Circuit: A10K log taper pot scaling the envelope CV before the
  // IC9 summing node. The pot taper suppresses the low end (3.9 st at
  // slider 0.1) and compresses the top (step halves from 0.9 to 1.0).
  // Knee at ~10% rotation, then roughly linear 0.2-0.9.
  //
  // At full depth (t=1.0), the envelope sweeps 10.6 octaves (127 st).
  // The value is added directly to log(vcfBaseHz) before exp().
  //
  //   slider  Hz(base=110)  log_depth  octaves
  //   0.0         110       0.000        0.0
  //   0.1         138       0.227        0.3
  //   0.2         335       1.114        1.6
  //   0.3         800       1.984        2.9
  //   0.4        1600       2.677        3.9
  //   0.5        3680       3.510        5.1
  //   0.6        7854       4.268        6.2
  //   0.7       19215       5.163        7.4
  //   0.8        2570*      6.064        8.7
  //   0.9        6084*      6.925       10.0
  //   1.0        9260*      7.345       10.6
  //   (* set 2, base=200 Hz, stitched via shared f(0.5) calibration)
  static float vcfEnvDepth6(float t)
  {
    static constexpr float kTable[11] = {
      0.f, 0.227f, 1.114f, 1.984f, 2.677f, 3.510f,
      4.268f, 5.163f, 6.064f, 6.925f, 7.345f
    };
    float idx = t * 10.f;
    int i0 = std::min(static_cast<int>(idx), 9);
    float frac = idx - i0;
    return kTable[i0] + frac * (kTable[i0 + 1] - kTable[i0]);
  }

  // vcfEnvDepth_j60() - J60: same IR3109 + pot circuit as J6.
  // TODO: J60 uses 50KB linear pot (vs J6 A10K). Downstream circuit may differ.
  static float vcfEnvDepth_j60(float t) { return vcfEnvDepth6(t); }

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
  // semitone), running at one frame per main-loop pass (ADSR::kTickRate, ~234 Hz
  // at the calibrated 4.27 ms loop period). Portamento glides toward the target
  // note and clamps when it arrives. Rate in semitones/sec:
  //
  //   rate = coeff * ADSR::kTickRate / 256
  //
  // TABLE SHAPE: Three observed segments in the original firmware table:
  //
  //   i=0:       coeff=0,   instant glide (porta bypassed entirely)
  //   i=1..25:   coeff 255->63, linear descent step -8 per index
  //   i=26..47:  coeff 61->19,  linear descent step -2 per index
  //   i=48..127: coeff 18->1,   exponential decay ~0.9625 per index
  //
  // This gives an overall glide time range of approximately:
  //   fastest (i=1): ~50ms per octave
  //   slowest (i=127): ~12.9 seconds per octave
  //
  // REFERENCE: Roland Juno-6 voice CPU firmware disassembly, portamento
  // coefficient table $0A00_portCoeffTbl, 128 single-byte entries.
  // Pitch accumulator resolution: 8.8 fixed point (256 steps per semitone).

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
    return coeff * ADSR::kTickRate / 256.f;
  }

  // portaRate_j6() - wrapper for consistent naming.
  static float portaRate_j6(float t) { return portaRate(t); }

  // portaRate_j60() - J60: same firmware portamento table.
  static float portaRate_j60(float t) { return portaRate_j6(t); }

  // portaRate_j106() - J106: same firmware portamento table.
  static float portaRate_j106(float t) { return portaRate_j6(t); }

  // dcoSubLevel_j6() - Maps Juno-6 DCO Sub slider (0..1) to linear gain.
  // From hardware measurements (docs/J6_MEASUREMENTS/SUB_OSC.csv):
  //   dB relative to saw/pulse at each slider position (0-10).
  // Normalized to 1.0 at max. kSubAmp at the mix point sets the +1.5 dB
  // level over saw measured on hardware.
  static float dcoSubLevel_j6(float t)
  {
    static constexpr float kTable[11] = {
      0.00285f,   // 0: -49.4 dB (effectively silent)
      0.00285f,   // 1: -49.4 dB
      0.02512f,   // 2: -30.5 dB
      0.05820f,   // 3: -23.2 dB
      0.09661f,   // 4: -18.8 dB
      0.15139f,   // 5: -14.9 dB
      0.20654f,   // 6: -12.2 dB
      0.44158f,   // 7: -5.6 dB
      0.66834f,   // 8: -2.0 dB
      0.88106f,   // 9:  +0.4 dB
      1.00000f    // 10: 0 dB (normalized; kSubAmp scales to +1.5 dB over saw)
    };
    float idx = t * 10.f;
    int i0 = std::min(static_cast<int>(idx), 9);
    float frac = idx - i0;
    return kTable[i0] + frac * (kTable[i0 + 1] - kTable[i0]);
  }

  // dcoSubLevel_j60() — Juno-60 sub oscillator level scaling.
  //
  // CIRCUIT ANALYSIS (from service manual):
  // The Juno-60 uses a diode shunt attenuator — not a VCA — to
  // control sub oscillator level. This was a cost optimization:
  // using BA662 VCAs for all 4 waveforms × 6 voices would have
  // required 24 VCA chips.
  //
  // Signal path:
  //   CMOS 4013 flip-flop (sub osc square wave)
  //   -> R31 (68K) -> Q9 (2SA1015 PNP, emitter=GND)
  //   -> Q9 collector -> D3 (1S2473) -> summing network
  //
  // Attenuation mechanism:
  //   R1 (33K) connects Q9's collector to a bias node.
  //   D2 (1S2473) at the bias node acts as a voltage-controlled
  //   resistor: rd ≈ 26mV / I_bias.
  //   U3 (M5218L inverting amp, gain=-68K/27K=-2.52) converts
  //   the 8-bit DAC voltage (0-5V) to a control current through
  //   D2, varying its dynamic resistance.
  //
  //   Slider at 0:  DAC=0V -> U3 out≈0V -> D2 off -> rd high
  //                 -> signal passes through D3 -> sub audible
  //   Slider at 10: DAC=5V -> U3 out=-12.6V -> D2 conducts hard
  //                 -> rd≈0 -> signal bleeds through R1 -> sub silent
  //   (Note: slider polarity and exact attenuation direction need
  //    verification — the inverting amp may flip the sense.)
  //
  // WHY WE CAN'T FULLY SIMULATE THIS:
  //   The attenuation depends on D2's AC dynamic resistance
  //   interacting with the signal amplitude at Q9's collector.
  //   A proper simulation requires transient analysis with
  //   simultaneous AC signal and DC bias — ngspice can do this
  //   but the ideal op amp models don't interact correctly with
  //   the D2 clamping circuit. The real M5218L has finite output
  //   current that creates a proper equilibrium with D2; the
  //   ideal op amp overwhelms D2.
  //
  // SONIC CHARACTER:
  //   Because diodes are nonlinear, this circuit introduces
  //   subtle harmonic distortion that varies with attenuation
  //   level — part of the Juno's character. The passive shunt
  //   also cannot achieve perfect silence; a faint "ghostly
  //   bleed" of the oscillators is audible at zero slider,
  //   which is a known Juno-60 trait.
  //
  // CURRENT MODEL:
  //   Using measured audio taper approximation as placeholder.
  //   The real circuit's transfer function is determined by the
  //   diode's rd vs bias current curve, which produces a
  //   nonlinear but roughly logarithmic attenuation. An audio
  //   taper (exp) is in the right ballpark.
  //
  // TODO: Derive proper transfer function from hardware
  //   measurements or improved SPICE simulation with realistic
  //   op amp model. Also model the signal-dependent THD that
  //   the diode shunt introduces at intermediate levels.
  static float dcoSubLevel_j60(float t)
  {
    return dcoSubLevel_j6(t);
  }

  // dcoSubLevel_j106() - J106 sub level from hardware measurement.
  // DAC drives transistor shunt (same topology as J60 but with 12-bit DAC).
  // Much more linear than J6's A-taper pot: no dead zone, smooth ramp.
  // Calibrated from osc_calibrate recording (RMS normalized to max).
  static float dcoSubLevel_j106(float t)
  {
    static constexpr float kTable[11] = {
      0.00154f,   // 0: -56.3 dB
      0.01251f,   // 1: -38.1 dB
      0.10875f,   // 2: -19.3 dB
      0.22125f,   // 3: -13.1 dB
      0.33576f,   // 4: -9.5 dB
      0.45206f,   // 5: -6.9 dB
      0.55750f,   // 6: -5.1 dB
      0.67178f,   // 7: -3.5 dB
      0.78447f,   // 8: -2.1 dB
      0.91132f,   // 9: -0.8 dB
      1.00000f    // 10: 0 dB
    };
    float idx = t * 10.f;
    int i0 = std::min(static_cast<int>(idx), 9);
    float frac = idx - i0;
    return kTable[i0] + frac * (kTable[i0 + 1] - kTable[i0]);
  }

  // dcoNoiseLevel_j6() - Maps Juno-6 DCO Noise slider (0..1) to linear gain.
  // From hardware measurements (docs/J6_MEASUREMENTS/NOISE.csv).
  // J6 noise CV circuit: A-taper pot (54K) -> collector 31.8K +/- 25K (trim),
  // no pulldown to -15V. The A-taper pot provides most of the curve;
  // downstream circuit is similar to J60 but lower collector R.
  // Different curve than sub -- more gradual taper, no dead zone at slider 1.
  // Normalized to 1.0 at max (0 dB relative to saw). kNoiseAmp at the mix
  // point scales to match saw * kSawAmp.
  static float dcoNoiseLevel_j6(float t)
  {
    static constexpr float kTable[11] = {
      0.0f,       // 0: -inf
      0.01496f,   // 1: -36.5 dB
      0.04571f,   // 2: -26.8 dB
      0.06166f,   // 3: -24.2 dB
      0.08318f,   // 4: -21.6 dB
      0.10116f,   // 5: -19.9 dB
      0.11092f,   // 6: -19.1 dB
      0.20654f,   // 7: -13.7 dB
      0.30200f,   // 8: -10.4 dB
      0.51286f,   // 9: -5.8 dB
      1.00000f    // 10: 0 dB (normalized to 1.0; kNoiseAmp scales to saw level)
    };
    float idx = t * 10.f;
    int i0 = std::min(static_cast<int>(idx), 9);
    float frac = idx - i0;
    return kTable[i0] + frac * (kTable[i0 + 1] - kTable[i0]);
  }

  // dcoNoiseLevel_j60() — Juno-60 noise level scaling.
  //
  // Circuit: 8-bit R2R DAC (0-5V via CD4050B buffers) ->
  //   R36 (1.5k) -> VR14 (50k trim) -> R14 (6.8k) ->
  //   Q8 (2SA1015 PNP, base=GND) -> BA662 Iabc pin.
  //
  // BA662 Iabc is a Wilson current mirror input. Q8's exponential
  // Ic(Vbe) driving into the mirror's log V-I characteristic
  // produces a nearly linear transfer with a soft turn-on knee
  // at ~11% of slider travel (Q8 Vbe threshold).
  //
  // Derived from ngspice DC sweep: 2SA1015 model, 1N4148 diode
  // as BA662 Iabc stand-in. RMS error < 0.002. Normalized to
  // 0 dB (1.0) at full slider.
  static float dcoNoiseLevel_j60(float t)
  {
    constexpr float kA = 1.1260f;  // output scale (1.0 at t=1)
    constexpr float kB = 0.0227f;  // knee softness
    constexpr float kC = 0.1120f;  // turn-on threshold
    float d = t - kC;
    return kA * (sqrtf(d * d + kB * kB) + d) * 0.5f;
  }

  // dcoNoiseLevel_j106() — Juno-106 noise level scaling.
  //
  // Circuit: 12-bit DAC (0-5V, scaled to 0-10V by op amp x2) ->
  //   VR (100k trim) -> R (10k) -> Q (2SA1015 PNP, base=GND) ->
  //   BA662 Iabc. 2.2M bleeder to -15V.
  //
  // Same topology as J60, wider DAC range, higher impedances.
  // Shorter dead zone (~6%) due to 10V range reaching Vbe
  // threshold sooner in normalized slider travel.
  //
  // Derived from ngspice DC sweep. RMS error < 0.002.
  // Normalized to 0 dB (1.0) at full slider.
  static float dcoNoiseLevel_j106(float t)
  {
    constexpr float kA = 1.0632f;  // output scale (1.0 at t=1)
    constexpr float kB = 0.0146f;  // knee softness
    constexpr float kC = 0.0594f;  // turn-on threshold
    float d = t - kC;
    return kA * (sqrtf(d * d + kB * kB) + d) * 0.5f;
  }

  void UpdatePortaCoeff()
  {
    // portaRate() returns semitones/sec; convert to octaves/sample
    float semiPerSec = portaRate(mPortaRateParam);
    mPortaStep       = semiPerSec / (12.f * mSampleRate);
  }


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
    mFwEnvPending = static_cast<float>(mADSR.mEnvInt) / ADSR::kEnvMax;

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
    // The J106 firmware sees the raw MIDI note — the octave range switch only
    // changes the DCO clock divider, not the note the VCF firmware tracks.
    float glideMidi = mGlidePitch * 12.f + 69.f;
    uint16_t pitch88 = static_cast<uint16_t>(
        std::clamp(static_cast<int>(glideMidi * 256.f), 0, 127 << 8));

    bool envPol = (mVcfEnvInvert > 0);
    mVcfDacPending = kr106::calc_vcf_freq(
        mVcfCutoffInt, vcfLfoSignal, vcfBendAmt,
        mVcfEnvModInt, mVcfKeyTrackInt,
        lfoPolarity, bendPol, envPol,
        mADSR.mEnvInt, pitch88);
  }

  // Idle-voice variant of FirmwareTick (J106): computes only the VCF DAC.
  //
  // The D7811G firmware runs the same main-loop VCF calculation for every
  // voice on every pass, regardless of gate state — see ic29.txt around
  // $04D5 (the voice loop iterates voicePtr 0..5 unconditionally). The
  // key-follow term reads from $FF71_notePitchFrac, which is written by
  // the portamento code from $FF09_voice1Note — initialized to $3C
  // (middle C) at power-on and overwritten by the assigner on each note
  // assignment, never cleared on note-off. This matches the observable
  // hardware behavior that self-oscillating voices never stop oscillating,
  // they just retune across keyTrack changes.
  //
  // We skip ADSR.Tick106() here: an at-rest ADSR (mEnvInt == 0) stays at
  // rest, and by definition this path only runs when GetBusy() is false.
  // Envelope contribution to the VCF DAC is therefore 0 regardless.
  void FirmwareTickIdleVcfJ106(float lfoRaw)
  {
    // LFO path: same as FirmwareTick. mLfoEnvAmp has already been set
    // by the DSP at block start and reflects the current onset envelope.
    uint16_t lfoVal = static_cast<uint16_t>(fabsf(lfoRaw) * 0x1FFF);
    bool lfoPolarity = (lfoRaw < 0.f);
    uint8_t depthScalar = static_cast<uint8_t>(mLfoEnvAmp * 255.f);
    uint16_t vcfLfoSignal = kr106::calc_vcf_lfo_signal(
        mVcfLfoDepthInt, depthScalar, lfoVal);

    // Bend path: same as FirmwareTick.
    uint8_t bendVal = static_cast<uint8_t>(fabsf(mRawBend) * 255.f);
    bool bendPol = (mRawBend < 0.f);
    uint16_t vcfBendAmt = kr106::calc_vcf_bend_amt(mVcfBendSensInt, bendVal);

    // Pitch for keyTrack: middle C if this voice has never been played
    // (mGlidePitch < -10.f is the uninitialized sentinel set in the
    // class-member initializer; see also Trigger's snap-on-first-note
    // logic). Otherwise the last glide pitch, which persists across
    // note-off exactly like $FF09/$FF71 in hardware. Middle C in the
    // voice's A440-relative octave convention: (60 - 69) / 12 = -0.75.
    float pitchForKbd = (mGlidePitch < -10.f) ? -0.75f : mGlidePitch;
    float glideMidi = pitchForKbd * 12.f + 69.f;
    uint16_t pitch88 = static_cast<uint16_t>(
        std::clamp(static_cast<int>(glideMidi * 256.f), 0, 127 << 8));

    bool envPol = (mVcfEnvInvert > 0);
    // mADSR.mEnvInt is 0 for a fully-idle voice; passing it through is
    // identical to zeroing it explicitly but avoids an implicit
    // assumption about ADSR internals.
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
    if (mModel == kJ106)
    {
      mFwEnvPending = mFwEnvNext = mADSR.mEnvNext;

      // Compute initial VCF DAC with the actual bend so the filter cutoff
      // starts at the correct frequency on the first sample, even when the
      // bender is deflected at note-on. LFO signal is zero here because the
      // onset envelope is always zero at note-on — correct regardless of LFO phase.
      float glideMidi = mGlidePitch * 12.f + 69.f;
      uint16_t pitch88 = static_cast<uint16_t>(
          std::clamp(static_cast<int>(glideMidi * 256.f), 0, 127 << 8));
      uint8_t bendVal = static_cast<uint8_t>(fabsf(mRawBend) * 255.f);
      bool bendPol = (mRawBend < 0.f);
      uint16_t vcfBendAmt = kr106::calc_vcf_bend_amt(mVcfBendSensInt, bendVal);
      bool envPol = (mVcfEnvInvert > 0);
      mVcfDacPending = mVcfDacNext = kr106::calc_vcf_freq(
          mVcfCutoffInt, 0, vcfBendAmt,
          mVcfEnvModInt, mVcfKeyTrackInt,
          false, bendPol, envPol,
          mADSR.mEnvInt, pitch88);
    }

    // VCF is NOT reset — matches hardware where the filter runs
    // continuously (self-oscillation persists between notes).
    // DCO resets only when the voice was fully idle (release finished).
    // Retriggers while still sounding preserve oscillator phase,
    // matching hardware where the DCO free-runs during release.
    if (!isRetrigger)
    {
      mOsc.Reset();
      mVcaSlew = 0.f;
    }
  }

  void Release() { mADSR.NoteOff(); }

  void SetSampleRateAndBlockSize(double sampleRate, int blockSize)
  {
    (void)blockSize;
    mSampleRate = static_cast<float>(sampleRate);
    mADSR.SetSampleRate(mSampleRate);
    mVCF.SetSampleRate(mSampleRate);
    mVCF.Reset();
    mOscBLEP.Init(mSampleRate * mOversample);
    // Per-voice upsampler pair (1× → 2× → 4×). Uses the same
    // HIIR half-band coefficients as the shared mix-bus decimator.
    mOscUp1.set_coefs(kr106::kResamplerCoefs2x);
    mOscUp2.set_coefs(kr106::kResamplerCoefs2x);
    mOscUp1.clear_buffers();
    mOscUp2.clear_buffers();
    // Prime the upsamplers by pushing a short run of silence so the
    // first non-zero sample doesn't produce an IIR startup transient.
    {
      float a, b, a2, b2;
      for (int i = 0; i < 32; i++)
      {
        mOscUp1.process_sample(a, b, 0.f);
        mOscUp2.process_sample(a2, b2, a);
        mOscUp2.process_sample(a2, b2, b);
      }
    }
    mOsc.Init(mSampleRate);

    static constexpr float kVcaSlewTau = 0.00058f; // ~0.58ms, matches hardware
    mVcaSlewCoeff = 1.f - expf(-1.f / (kVcaSlewTau * mSampleRate));

    // Precomputed constants for VCF frequency calculation.
    // VCF modulation works in log-frequency space; these convert
    // between log-Hz and normalized cutoff (cycles per sample).
    float nyq = mSampleRate * 0.5f;
    mInvNyq   = 1.f / nyq;      // Hz → normalized cutoff
    mMinCPS   = 20.f * mInvNyq; // 20 Hz floor in normalized units

    // Unified D7811G firmware tick rate (~234 Hz at calibrated 4.27ms loop
    // period; canonical value in ADSR::kLoopPeriodMs) — drives ADSR + VCF
    mFwTickStep = ADSR::kTickRate / mSampleRate;

    UpdatePortaCoeff();

  }

  // Process a block of base-rate samples.
  //
  // mixBus: shared mono mix bus at mOversample× the base sample rate,
  //         sized (startIdx + nFrames) * mOversample floats. This voice
  //         ACCUMULATES into mixBus[i*mOversample + k]. The DSP is
  //         responsible for clearing it and decimating afterwards.
  void ProcessSamplesAccumulating(T** inputs, float* mixBus, int nInputs, int startIdx,
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
    // Shared noise source (index 2) — generated at base rate; held
    // across the 4 sub-samples. Any aliased images from the hold sit
    // above base Nyquist and are removed by the mix-bus decimator.
    T* noiseBuffer = (nInputs > 2 && inputs[2]) ? inputs[2] : nullptr;
    float noiseAT = (noiseBuffer && mDcoNoise > 0.f) ? mDcoNoise : 0.f;

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
      float lfoRaw = lfoRawBuffer ? static_cast<float>(lfoRawBuffer[i]) : 0.f;
      float env;

      if (mModel != kJ106)
      {
        env = mADSR.Process();
      }
      else
      {
        // J106: firmware tick computes new ADSR + VCF DAC values.
        // On hardware the multiplexed DAC writes each channel at a
        // different point within the tick period (see kVcfPhaseTable /
        // kVcaPhaseTable for the per-voice offsets). Currently all CVs
        // latch simultaneously at tick time -- the phase infrastructure
        // is preserved for future stagger experiments but inactive.
        mFwTickAccum += mFwTickStep;
        if (mFwTickAccum >= 1.f)
        {
          mFwTickAccum -= 1.f;
          FirmwareTick(lfoRaw);
          mVcfDacNext = mVcfDacPending;
          mFwEnvNext = mFwEnvPending;
        }

        env = mFwEnvNext;
        mADSR.mEnv = env;
        mADSR.UpdateGateEnv();
      }

      // --- Pulse width modulation ---
      float pw;
      switch (mPwmMode)
      {
        case -1:
          // PWM uses raw LFO directly, not onset-enveloped.
          // ROM $0761-$0788: lfoVal used unconditionally, onset envelope
          // ($FF5A) only gates pitch/VCF depth scalars, not PWM path.
          pw = mDcoPwm * (1.f + lfoRaw) * 0.5f;
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
      // Linear PW mapping from 106-point hardware duty cycle sweep
      // (pwm_test, lfrancis unit at 192 kHz). Max error < 0.1%.
      float pwMin = 0.4949f + mPwMinOffset;
      float pwMax = 0.9724f + mPwMaxOffset;
      pw = pwMin + pw * (pwMax - pwMin);


      // --- Pitch modulation ---
      // mDcoLfo is in semitones (from dcoLfoDepth6/106).
      float lfoSemitones = lfo * (mDcoLfo + mBenderModAmt * mBendLfo);
      float pitchMod =
          mOctTranspose / 12.f + mPitchOffset + lfoSemitones / 12.f + mRawBend * mBendDco;
      float freq = baseFreq * powf(2.f, pitchMod);
      float cps  = freq / mSampleRate;

      // Safety clamp — nothing to write, skip the inner 4× loop.
      if (cps <= 0.f || cps >= 0.5f)
        continue;

      // --- VCF frequency calculation (base-rate normalized to base Nyquist) ---
      float vcfCPS;
      if (mModel != kJ106)
      {
        static constexpr float kEnvScale = 6.931f;
        static constexpr float kEnvInvScale = 7.766f;
        static constexpr float kSemiToLogFreq = 0.05776f;

        float envScale = (mVcfEnvInvert > 0) ? kEnvScale : kEnvInvScale;
        float vcfBaseHz = (mModel == kJ60) ? mVcfFreqJ60
                        : mVcfFreq; // J6 (J106 takes the other branch)
        // J6: KBD tracks from C1 (32.7 Hz). J60 and J106: from C4 (261.6 Hz).
        float kbdRef    = (mModel == kJ6) ? 32.703f : 261.626f;
        float vcfFrq = logf(vcfBaseHz) + mVcfFreqOffset;

        // J6: KBD tracking sees the pitch CV (octave switch + portamento)
        // but not LFO or pitch bend. Bend modulates the VCF separately
        // (line 608) -- the KBD tracking tap is before the bender summing point.
        float kbdPitch = mOctTranspose / 12.f + mPitchOffset;
        float kbdFreq = baseFreq * powf(2.f, kbdPitch);
        vcfFrq += logf(kbdFreq / kbdRef) * mVcfKbd;
        vcfFrq += env * mVcfEnv * envScale * float(mVcfEnvInvert);
        vcfFrq += lfo * mVcfLfo * kSemiToLogFreq;
        vcfFrq += 4.15888f * mRawBend * mBendVcf;

        vcfCPS = expf(vcfFrq) * mInvNyq;
      }
      else
      {
        float vcfHz = kr106::dacToHz(std::min(mVcfDacNext, static_cast<uint16_t>(16256)));
        vcfCPS = vcfHz * mInvNyq;
      }

      // --- VCF coefficient update (control rate, ~once per base sample) ---
      // This is where tanf, expf, powf etc. live. Running it 1× instead of
      // 4× is the whole point of the refactor.
      mVCF.TrackInputEnv(mLastOscPeak);
      mVCF.UpdateCoeffs(vcfCPS, mVcfRes);

      // --- VCA gain (base-rate; same gain applied to all 4 sub-samples) ---
      // Gate mode: full on/off from gate state. Env mode: ADSR envelope.
      // Both pass through the BA662 VCA slew (one-pole RC) which models
      // the physical response time -- prevents clicks on instant transitions.
      float vcaRaw = mVcaMode
        ? (mADSR.mState != kr106::ADSR::kRelease && mADSR.mState != kr106::ADSR::kFinished ? 1.f : 0.f)
        : env;
      mVcaSlew += mVcaSlewCoeff * (vcaRaw - mVcaSlew);
      float vcaGain = kr106::VCAGain(mVcaSlew, mModel) * velocity * mVcaGainScale;

      float* mixSlot = mixBus + i * mOversample;

      if (mOscMode == 1)
      {
        // --- PolyBLEP at oversampled rate (no upsampling) ---
        // Oscillator runs at Nx, feeding VCF directly each sub-sample.
        // Noise: zero-order hold from base rate. Same value on all
        // sub-samples gives correct amplitude; the sinc² rolloff is
        // above base Nyquist (>22 kHz) and well into VCF stopband.
        float cpsOS = cps / static_cast<float>(mOversample);
        float noise = (noiseAT > 0.f)
            ? static_cast<float>(noiseBuffer[i]) * mOscBLEP.mNoiseAmp * noiseAT
            : 0.f;
        float peak = 0.f;
        for (int k = 0; k < mOversample; k++)
        {
          bool sync = false;
          float s = mOscBLEP.Process(cpsOS, pw, mSawOn, mPulseOn, mSubOn, mDcoSub, 0.f, sync);
          s += noise;
          mixSlot[k] += mVCF.ProcessSample(s) * vcaGain;
          float a = fabsf(s);
          if (a > peak) peak = a;
        }
        mLastOscPeak = peak;
      }
      else
      {
        // --- Wavetable at 1× (base rate) + upsampling ---
        mOsc.PrepareBlock(cps, pw, mSawOn, mPulseOn, mSubOn, mDcoSub);
        bool syncBase = false;
        float oscOut = mOsc.ProcessSample(syncBase);
        if (noiseAT > 0.f)
          oscOut += static_cast<float>(noiseBuffer[i]) * mOsc.mNoiseAmp * noiseAT;
        mLastOscPeak = fabsf(oscOut);
        (void)syncBase;

        // --- Upsample and VCF at Nx rate, accumulate into mix bus ---
        if (mOversample == 4)
        {
          float s2_0, s2_1;
          mOscUp1.process_sample(s2_0, s2_1, oscOut);
          float s4[4];
          mOscUp2.process_sample(s4[0], s4[1], s2_0);
          mOscUp2.process_sample(s4[2], s4[3], s2_1);
        
          mixSlot[0] += mVCF.ProcessSample(s4[0]) * vcaGain;
          mixSlot[1] += mVCF.ProcessSample(s4[1]) * vcaGain;
          mixSlot[2] += mVCF.ProcessSample(s4[2]) * vcaGain;
          mixSlot[3] += mVCF.ProcessSample(s4[3]) * vcaGain;
        }
        else if (mOversample == 2)
        {
          float s2_0, s2_1;
          mOscUp1.process_sample(s2_0, s2_1, oscOut);
        
          mixSlot[0] += mVCF.ProcessSample(s2_0) * vcaGain;
          mixSlot[1] += mVCF.ProcessSample(s2_1) * vcaGain;
        }
        else // 1x — no upsampling
        {
          mixSlot[0] += mVCF.ProcessSample(oscOut) * vcaGain;
        }
      }
    }
  }

  // J106 idle-voice processing: keeps the VCF running (filter state +
  // self-oscillation) while the voice has no active note.
  //
  // Rationale: on real hardware, the D7811G firmware runs the same
  // VCF-DAC calculation for every voice on every main-loop pass (ic29.txt,
  // voice loop at $04D5). The IR3109 OTA filters are continuously biased
  // and continuously self-oscillate at high Q, independent of the VCA
  // gate. Skipping this for idle voices cold-starts each filter on
  // note-on, which loses:
  //   • Free-running self-osc phase across notes (six independent
  //     oscillators drifting against each other between notes)
  //   • Filter state continuity — e.g. a note triggered while the filter
  //     is mid-ring hears the ring complete, not a reset
  //   • Per-voice component variance beating at high Q
  //
  // This path writes nothing to the mix bus — the VCA is closed for an
  // idle voice, and we are not modeling VCA leakage. Only the VCF's
  // internal state advances.
  //
  // J106 only. Non-J106 models fall through to the DSP's skip-continue
  // (see KR106_DSP.h ProcessBlock dispatch).
  void ProcessIdleVcfJ106(T** inputs, int nInputs, int startIdx, int nFrames)
  {
    // Raw LFO triangle (pre-onset-envelope) is what the J106 firmware
    // integer path consumes — same as the busy path at index kModLFORaw.
    T* lfoRawBuffer = (nInputs > 1 && inputs[1]) ? inputs[1] : nullptr;

    for (int i = startIdx; i < startIdx + nFrames; i++)
    {
      float lfoRaw = lfoRawBuffer ? static_cast<float>(lfoRawBuffer[i]) : 0.f;

      // Fire firmware VCF ticks at ADSR::kTickRate (~234 Hz). Accumulator
      // phase is shared with the busy path (mFwTickAccum is a single member)
      // so tick timing is continuous across idle/busy transitions — matches
      // the real firmware loop which runs continuously regardless of voice
      // gate state.
      mFwTickAccum += mFwTickStep;
      while (mFwTickAccum >= 1.f)
      {
        mFwTickAccum -= 1.f;
        FirmwareTickIdleVcfJ106(lfoRaw);
      }

      float vcfHz = kr106::dacToHz(std::min(mVcfDacNext, static_cast<uint16_t>(16256)));
      float vcfCPS = vcfHz * mInvNyq;

      // Control-rate VCF coefficient update. Pass 0 to TrackInputEnv so
      // the noise-seeding envelope follower decays naturally while idle;
      // when the voice is triggered again, mLastOscPeak drives it
      // normally from the first busy sample.
      mVCF.TrackInputEnv(0.f);
      mVCF.UpdateCoeffs(vcfCPS, mVcfRes);

      // Run the filter at the configured oversample rate with zero
      // input. Output is discarded — the VCA is closed for an idle
      // voice, so nothing reaches the mix bus. The internal state
      // (mS[0..3]) advances, which is the whole point: self-oscillation
      // continues to phase-evolve across note boundaries.
      if (mOversample == 4)
      {
        mVCF.ProcessSample(0.f);
        mVCF.ProcessSample(0.f);
        mVCF.ProcessSample(0.f);
        mVCF.ProcessSample(0.f);
      }
      else if (mOversample == 2)
      {
        mVCF.ProcessSample(0.f);
        mVCF.ProcessSample(0.f);
      }
      else
      {
        mVCF.ProcessSample(0.f);
      }
    }
  }
};

} // namespace kr106
