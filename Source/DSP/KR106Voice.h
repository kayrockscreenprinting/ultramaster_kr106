#pragma once

#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdint>

#include "KR106ADSR.h"
#include "KR106Oscillators.h"
#include "KR106OscillatorsWT.h"
#include "KR106VCA.h"
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
  OscillatorsWT mOsc;
  VCF mVCF;
  ADSR mADSR;



  // Voice control (replaces iPlug2 mInputs ControlRamps)
  double mPitch     = 0.0; // 1V/oct relative to A440
  double mPitchBend = 0.0; // pitch bend in octaves

  // Voice parameters (set by KR106DSP::SetParam via ForEachVoice)
  float mDcoLfo       = 0.f;
  float mDcoPwm       = 0.f;
  float mDcoSub       = 0.95080f; // pre-computed from dcoSubLevel_j6(1.0)
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

  // dcoSubLevel_j6() - Maps Juno-6 DCO Sub slider (0..1) to linear gain.
  // From hardware measurements (docs/J6_MEASUREMENTS/SUB_OSC.csv):
  //   dB relative to saw/pulse at each slider position (0-10).
  // Table stores linear gain values (10^(dB/20)), normalized so that
  // slider=10 produces the measured +1.5 dB relative to saw when combined
  // with kSubAmp/kSawAmp (0.625/0.5 = +1.94 dB). Net effect at max:
  // subLevel * kSubAmp/kSawAmp = +1.5 dB, so subLevel(10) = 10^(-0.44/20).
  static float dcoSubLevel_j6(float t)
  {
    static constexpr float kTable[11] = {
      0.00271f,   // 0: -49.4 dB (effectively silent)
      0.00271f,   // 1: -49.4 dB
      0.02388f,   // 2: -30.5 dB
      0.05534f,   // 3: -23.2 dB
      0.09186f,   // 4: -18.8 dB
      0.14394f,   // 5: -14.9 dB
      0.19638f,   // 6: -12.2 dB
      0.41985f,   // 7: -5.6 dB
      0.63546f,   // 8: -2.0 dB
      0.83771f,   // 9:  +0.4 dB
      0.95080f    // 10: +1.5 dB (net +1.5 dB with kSubAmp/kSawAmp)
    };
    float idx = t * 10.f;
    int i0 = std::min(static_cast<int>(idx), 9);
    float frac = idx - i0;
    return kTable[i0] + frac * (kTable[i0 + 1] - kTable[i0]);
  }

  // dcoSubLevel_j60() - J60 sub level: linear pot + linear circuit.
  // The J6 nonlinearity comes entirely from the A-taper pot. The J60 uses
  // a 50KB linear pot, and the downstream circuit (inverting amp + transistor
  // shunt + diode) is essentially linear. Scaled to match J6 max level.
  // TODO: verify with J60 hardware measurements.
  static float dcoSubLevel_j60(float t)
  {
    return t * 0.95080f; // linear, scaled to match J6 max (+1.5 dB net)
  }

  // dcoSubLevel_j106() - J106 sub level: no ROM table, straight DAC output.
  // Same transistor shunt circuit as J60. Linear.
  static float dcoSubLevel_j106(float t)
  {
    return t * 0.95080f; // linear, scaled to match J6 max (+1.5 dB net)
  }

  // dcoNoiseLevel_j6() - Maps Juno-6 DCO Noise slider (0..1) to linear gain.
  // From hardware measurements (docs/J6_MEASUREMENTS/NOISE.csv).
  // J6 noise CV circuit: A-taper pot (54K) -> collector 31.8K +/- 25K (trim),
  // no pulldown to -15V. The A-taper pot provides most of the curve;
  // downstream circuit is similar to J60 but lower collector R.
  // Different curve than sub -- more gradual taper, no dead zone at slider 1.
  // Noise is mixed directly (no kNoiseAmp constant), so the table values are
  // absolute linear gains relative to saw * kSawAmp.
  static float dcoNoiseLevel_j6(float t)
  {
    static constexpr float kTable[11] = {
      0.0f,       // 0: -inf
      0.00748f,   // 1: -36.5 dB
      0.02286f,   // 2: -26.8 dB
      0.03083f,   // 3: -24.2 dB
      0.04159f,   // 4: -21.6 dB
      0.05058f,   // 5: -19.9 dB
      0.05546f,   // 6: -19.1 dB
      0.10327f,   // 7: -13.7 dB
      0.15100f,   // 8: -10.4 dB
      0.25643f,   // 9: -5.8 dB
      0.50000f    // 10: 0 dB (= kSawAmp, unity relative to saw)
    };
    float idx = t * 10.f;
    int i0 = std::min(static_cast<int>(idx), 9);
    float frac = idx - i0;
    return kTable[i0] + frac * (kTable[i0 + 1] - kTable[i0]);
  }

  // dcoNoiseLevel_j60() - J60 noise level: linear pot + PNP transistor circuit.
  // 50KB linear pot -> DAC 0-6V -> 50K trim + 6.8K -> PNP 2SA1015 (base GND) -> BA662.
  // Collector: 50K trim + 6.8K (~7K-57K), no pulldown. CV range 0-6V.
  // The BA662 is linear; the PNP transistor gives the curve.
  // FIXME: model PNP 2SA1015 transistor curve (Vbe threshold + emitter
  // degeneration from trim pot). Using AudioTaper as placeholder -- it's an
  // exponential in the right ballpark, but not derived from the actual circuit.
  static float dcoNoiseLevel_j60(float t)
  {
    static const float kScale = 1.f / (std::exp(3.f) - 1.f);
    return (std::exp(3.f * t) - 1.f) * kScale * 0.50000f;
  }

  // dcoNoiseLevel_j106() - J106 noise level: same PNP + BA662 circuit as J60.
  // Collector: 2.26M +/- 50K (trim) to signal, 2.2M to -15V. CV range 0-10V.
  // Much higher collector R and wider CV range than J60 -- expect steeper curve.
  // FIXME: model actual transistor curve; using AudioTaper placeholder.
  static float dcoNoiseLevel_j106(float t)
  {
    static const float kScale = 1.f / (std::exp(3.f) - 1.f);
    return (std::exp(3.f * t) - 1.f) * kScale * 0.50000f;
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
    // The J106 firmware sees the raw MIDI note — the octave range switch only
    // changes the DCO clock divider, not the note the VCF firmware tracks.
    float glideMidi = mGlidePitch * 12.f + 69.f;
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
    if (mModel == kJ106)
    {
      mFwEnvNext = mADSR.mEnvNext;
      // Don't snap mFwEnvSmooth — let the 1ms RC filter smooth the onset,
      // matching the real DAC output stage. Snapping causes clicks at attack=0.

      // Compute initial VCF DAC with the post-attack envelope so the
      // filter cutoff starts at the correct frequency on the first sample.
      float glideMidi = mGlidePitch * 12.f + 69.f;
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
      // VCF is NOT reset on note-on — matches real hardware where
      // the filter runs continuously (self-oscillation persists
      // between notes, frequency just shifts with keyboard tracking).
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
    // Prime the VCF's polyphase resamplers by feeding silent samples.
    // Without this, the first non-zero sample causes an upsampler transient (click).
    for (int i = 0; i < 64; i++)
      mVCF.Process(0.f, 0.01f, 0.f);
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
    // Shared noise source (index 2)
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
      float env;

      if (mModel != kJ106)
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

      // --- VCF frequency calculation ---
      float vcfCPS;
      if (mModel != kJ106)
      {
        static constexpr float kEnvScale = 6.931f;
        static constexpr float kEnvInvScale = 7.766f;
        static constexpr float kSemiToLogFreq = 0.05776f;

        float envScale = (mVcfEnvInvert > 0) ? kEnvScale : kEnvInvScale;
        float vcfBaseHz = (mModel == kJ106) ? mVcfFreqJ106
                        : (mModel == kJ60) ? mVcfFreqJ60
                        : mVcfFreq; // J6
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
        mVcfDacSmooth += (static_cast<float>(mVcfDacNext) - mVcfDacSmooth) * mDacSmoothCoeff;
        float vcfHz = kr106::dacToHz(static_cast<uint16_t>(std::clamp(mVcfDacSmooth, 0.f, 16256.f)));
        vcfCPS = vcfHz * mInvNyq;
      }

      // --- Oscillator (1x) + oversampled VCF ---
      // Wavetable oscillator is bandlimited by construction, runs at base rate.
      // VCF handles its own upsampling/downsampling internally.
      bool sync = false;
      float oscOut = mOsc.Process(cps, pw, mSawOn, mPulseOn, mSubOn, mDcoSub, 0.f, sync);

      // Noise mixed from shared source (single generator, matches hardware)
      if (noiseAT > 0.f)
        oscOut += static_cast<float>(noiseBuffer[i]) * noiseAT;
      float signal = mVCF.Process(oscOut, vcfCPS, mVcfRes);

      // --- VCA (BA662 + TR17 exponential converter) ---
      float vcaOut;
      if (mVcaMode)
        vcaOut = signal * kr106::VCAGain(mADSR.mGateEnv) * velocity * mVcaGainScale; // Gate mode
      else
        vcaOut = signal * kr106::VCAGain(env) * velocity * mVcaGainScale; // ADSR mode

      // Accumulate mono (chorus does stereo later)
      outputs[0][i] += static_cast<T>(vcaOut);
      if (nOutputs > 1)
        outputs[1][i] += static_cast<T>(vcaOut);
    }
  }
};

} // namespace kr106
