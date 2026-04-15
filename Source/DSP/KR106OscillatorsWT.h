#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>

// Oscillator mix constants (from hardware measurements)
namespace kr106 {

// J6/J60: BA662 VCA per waveform, calibrated from Juno-6 recordings
static constexpr float kSawAmpJ6    = 0.5f;      // SAW is 0v to 12v
static constexpr float kPulseAmpJ6  = 0.5f;      // PULSE is 0v to +12v TL074
static constexpr float kSubAmpJ6    = 0.5942f;   // SUB at +1.5 dB over saw (J6 measurement)
static constexpr float kNoiseAmpJ6  = 1.f;       // Noise level (J6 calibration)

// J106: MC5534 pre-mixed saw/pulse, separate sub and noise mixing resistors.
// Calibrated from hardware 106_calibration recording (peak-to-peak matched
// to noise RMS at HPF flat, VCF wide open).
// Sub/pulse ratio: 1.51x from osc_calibrate recording (both square waves,
// ratio is recording-gain-independent).
static constexpr float kSawAmpJ106   = 0.5f;     // scaled to match J6 saw level
static constexpr float kPulseAmpJ106 = 0.417f;   // 0.834x saw (hardware measurement)
static constexpr float kSubAmpJ106   = 0.630f;   // 1.51x pulse, +5.3 dB vs saw (hardware cal)
static constexpr float kNoiseAmpJ106 = 1.385f;   // 2.77x saw (39K mixing R, hardware cal)

static constexpr float kSwitchRamp = 1.f / 64.f; // ~1.5ms at 44.1k
} // namespace kr106

// Bandlimited wavetable oscillators modeled on the Juno-6/106 DCO
//
// Zero-aliasing alternative to the PolyBLEP oscillator. Waveforms are
// generated from precomputed additive-synthesis saw tables (one per
// octave), with Hermite interpolation and octave crossfading.
//
// Saw:   direct table lookup with optional phase curvature
// Pulse: saw(phase) - saw(phase - pw) — bandlimited by construction
// Sub:   saw(subPhase - 0.5) - saw(subPhase) at half frequency
// Noise: identical to PolyBLEP version (LCG + CLT + TPT lowpass)
//
// The saw table is the only stored waveform. Pulse and sub are derived
// algebraically from saw lookups, inheriting the band-limiting. This
// means continuous PW modulation works perfectly with no additional
// tables or interpolation.
//
// With wavetable oscillators, oversampling is unnecessary for alias
// suppression — the tables contain no energy above Nyquist by
// construction.
//
// The oscillator runs at the base sample rate. Its output is upsampled
// to 4× per-voice before feeding into the VCF (see Voice.h), and the
// summed 4× mix bus is decimated once at the DSP level. This keeps
// oscillator interpolation cost at baseline while still sharing the
// mix-bus decimator across voices.

namespace kr106 {

// Shared bandlimited saw wavetable storage.
// Allocate one instance (e.g. in the DSP class) and pass a pointer
// to each voice's OscillatorsWT. Read-only after Init().
struct SawTables {
  static constexpr int kSize = 4096;
  static constexpr int kMask = kSize - 1;
  static constexpr int kNumTables = 11; // octaves 0–10 (C0 through ~C10)
  static constexpr float kBaseFreq = 16.352f; // C0 in Hz

  float tables[kNumTables][kSize] = {};
  float sampleRate = 44100.f; // the rate at which the osc will be READ
                              // (4× base rate in the current architecture)

  // Generate all tables via additive synthesis.
  //
  //   sampleRate         — the rate at which the oscillator will be
  //                        clocked. cps (cycles-per-sample) passed to
  //                        Process() is implicitly relative to this.
  //   bandlimitHz        — optional upper bound on harmonic content.
  //                        If 0 (default), uses sampleRate * 0.5 (the
  //                        read-rate Nyquist). Pass the base-rate
  //                        Nyquist here when running the oscillator at
  //                        an oversampled rate but decimating back to
  //                        the base rate downstream — it prevents
  //                        populating the tables with harmonics that
  //                        the decimator would discard anyway.
  //
  // Table t is safe for fundamentals up to kBaseFreq * 2^(t+1).
  // Each table contains a bandlimited saw: f(phase) ≈ 2*phase - 1,
  // with harmonics truncated at bandlimitHz for that octave's ceiling.
  // Uses double precision accumulation to keep numerical error below
  // -140 dB.
  void Init(float sr, float bandlimitHz = 0.f) {
    sampleRate = sr;
    float nyquist = (bandlimitHz > 0.f) ? bandlimitHz : (sr * 0.5f);

    for (int t = 0; t < kNumTables; t++) {
      float freqBound = kBaseFreq * powf(2.f, static_cast<float>(t + 1));
      int maxH = std::max(1, static_cast<int>(nyquist / freqBound));

      for (int n = 0; n < kSize; n++) {
        double phase = static_cast<double>(n) / kSize;
        double val = 0.0;
        for (int h = 1; h <= maxH; h++)
          val -= sin(2.0 * M_PI * h * phase) / h;
        val *= 2.0 / M_PI;
        tables[t][n] = static_cast<float>(val);
      }
    }

  }

  // 4-tap cubic Hermite (Catmull-Rom) interpolation from a cyclic table.
  //
  // Replaces a 16-tap windowed sinc. Why this is fine here:
  //   - Tables are 4096 samples/cycle and bandlimited per octave. The
  //     highest stored harmonic (octave 0) sits at ~0.17 × table Nyquist,
  //     well inside the region where Hermite is flat (<0.1 dB error).
  //     Higher octaves have fewer harmonics, so even more headroom.
  //   - Kernel error energy lands at multiples of the effective table
  //     rate (4096 × fundamental) — tens of kHz and up, filtered
  //     downstream by the upsampler, VCF passband, and mix-bus decimator.
  //
  // Per-read cost: 4 loads + ~8 mults + ~7 adds. That's roughly 3×
  // cheaper than the 16-tap sinc, and the polyphase kernel table
  // (previously 64 KB of static storage) is gone too.
  //
  // If you ever need to A/B against the old sinc, it's recoverable
  // from git history — same function signature.
  static float Read(const float* tbl, float phase) {
    float idx = phase * kSize;
    int i = static_cast<int>(idx);
    float t = idx - static_cast<float>(i);

    // 4 neighboring samples with cyclic wrap.
    float y0 = tbl[(i - 1) & kMask];
    float y1 = tbl[(i    ) & kMask];
    float y2 = tbl[(i + 1) & kMask];
    float y3 = tbl[(i + 2) & kMask];

    // Catmull-Rom cubic in Horner form. Passes through y1 at t=0 and
    // y2 at t=1, with C¹ tangent continuity at sample boundaries.
    float c0 = y1;
    float c1 = 0.5f * (y2 - y0);
    float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
    float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + c0;
  }

  // Read with crossfade between two adjacent octave tables.
  // tblIdx and blend are precomputed from the pitch (see OctaveToTable).
  float ReadBlended(float phase, int tblIdx, float blend) const {
    phase -= floorf(phase); // wrap to [0, 1)
    float v0 = Read(tables[tblIdx], phase);
    float v1 = Read(tables[tblIdx + 1], phase);
    return v0 + (v1 - v0) * blend;
  }

  // Convert a continuous octave index to table index + blend fraction.
  // octave = log2f(freq / kBaseFreq).
  // Table t covers fundamentals up to kBaseFreq * 2^(t+1), so a note
  // at exactly the boundary should use table t, not t+1. The -1 offset
  // ensures the mapping is correct: at 261.6 Hz (octave 4.0), tblIdx=3
  // with blend=1.0, reading fully from table 3 (183 harmonics at 96k).
  static void OctaveToTable(float octave, int& tblIdx, float& blend) {
    octave = std::max(0.f, std::min(octave - 1.f, static_cast<float>(kNumTables - 2)));
    tblIdx = static_cast<int>(octave);
    blend = octave - static_cast<float>(tblIdx);
  }
};

// Wavetable oscillator — drop-in replacement for kr106::Oscillators.
// Same Process() signature, same member layout for state that Voice
// accesses directly (mPos, mSubState, mPulseInvert, gains).
struct OscillatorsWT {
  float mPos = 0.f;       // phase accumulator [0, 1)
  bool mSubState = false;  // flip-flop for sub oscillator
  float mSawGain = 1.f;   // crossfade gains for pop-free switching
  float mPulseGain = 1.f;
  float mSubGain = 0.f;
  float mSwitchRamp = kSwitchRamp;

  bool mPulseInvert = false; // J106: inverted duty cycle (see Oscillators.h)

  // Per-model mix levels (set by Voice when model changes)
  float mSawAmp   = kSawAmpJ6;
  float mPulseAmp = kPulseAmpJ6;
  float mSubAmp   = kSubAmpJ6;
  float mNoiseAmp = kNoiseAmpJ6;

  // Ramp curvature: subtle quadratic bow matching hardware measurements.
  // Applied as phase warp before table lookup. At 0.03 the spectral
  // effect is below -100 dB so it doesn't meaningfully affect aliasing.
  // I think the curve we saw was a result of my Juno6 HPF never being fully off
  static constexpr float kSawCurve = 0.00f; // disabled curve
  static constexpr bool  kSawCurveEnabled = (kSawCurve != 0.f);

  // Gain threshold below which a waveform is considered silent and
  // its sinc reads are skipped. 1e-4 = -80 dB, safely below audibility.
  // The one-pole switch ramp still updates every sample so state stays
  // consistent — only the table lookups + output accumulate are gated.
  static constexpr float kGainEps = 1e-4f;

  const SawTables* mTables = nullptr;

  // ----- Per-base-sample cached values (set by PrepareBlock) -----
  // In the 4×-everywhere architecture the Voice calls PrepareBlock
  // once per base sample with the current cps/pw/switch settings, then
  // calls ProcessSample four times in an inner loop. Everything here
  // is constant across those four inner calls, so we hoist all the
  // per-sample derivations (log2f, OctaveToTable, effPW clamp, DC
  // offset, on/off targets) out of the hot path.
  float mCpsCached   = 0.f;
  int   mSawTblIdx   = 0;
  float mSawTblBlend = 0.f;
  int   mSubTblIdx   = 0;
  float mSubTblBlend = 0.f;
  float mEffPW       = 0.5f;
  float mPwDC        = 0.f;   // 2 * effPW - 1
  float mSawTarget   = 1.f;   // target for mSawGain ramp
  float mPulseTarget = 1.f;
  float mSubTarget   = 0.f;
  float mSubLevel    = 1.f;

  void SetTables(const SawTables* tables) { mTables = tables; }

  // sampleRate here is the BASE rate (used for the waveform-switch
  // crossfade time constant only — the actual clocking is determined
  // by the cps passed to Process()).
  void Init(float sampleRate) {
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

  // Audio taper: 50K pot emulation (exponential curve, same for sub + noise).
  // Call once per block at the voice level; pass the result to Process.
  static float AudioTaper(float x) {
    static const float kScale = 1.f / (std::exp(3.f) - 1.f);
    return (std::exp(3.f * x) - 1.f) * kScale;
  }

  // ============================================================
  // Control-rate preparation.
  // ============================================================
  // Call once per base sample. Computes the table indices, pulse-width
  // intermediates, and on/off ramp targets — all values that are
  // constant across the 4× inner loop.
  //
  // cps here is the OSC's per-sample phase increment. Normally this is
  // freq / baseSampleRate (oscillator runs at 1× base rate) — the
  // Voice then upsamples the oscillator output to 4× before the VCF.
  void PrepareBlock(float cps, float pulseWidth,
                    bool sawOn, bool pulseOn, bool subOn, float subLevel)
  {
    mCpsCached = cps;

    // Table selection: log2f + two OctaveToTable calls, hoisted from 4×.
    float freq = cps * mTables->sampleRate;
    float octave = log2f(std::max(freq, 1.f) / SawTables::kBaseFreq);
    SawTables::OctaveToTable(octave,        mSawTblIdx, mSawTblBlend);
    SawTables::OctaveToTable(octave - 1.f,  mSubTblIdx, mSubTblBlend);

    // Pulse width: invert + clamp, then the -DC offset term.
    float effPW = pulseWidth;
    if (mPulseInvert) effPW = 1.f - effPW;
    effPW = std::clamp(effPW, 0.01f, 0.99f);
    mEffPW = effPW;
    mPwDC  = 2.f * effPW - 1.f;

    // Gain-ramp targets. The per-sample ramp itself stays in
    // ProcessSample because it's state, but the target is constant.
    mSawTarget   = sawOn   ? 1.f : 0.f;
    mPulseTarget = pulseOn ? 1.f : 0.f;
    mSubTarget   = subOn   ? 1.f : 0.f;
    mSubLevel    = subLevel;
  }

  // ============================================================
  // Audio-rate inner sample (4× per base sample in hybrid arch,
  // or 1× if you're calling PrepareBlock + one ProcessSample).
  // Uses values cached by the most recent PrepareBlock().
  // ============================================================
  // Forced inline — see comment on VCF::ProcessSample. The out-of-line
  // body compiled to 344 lines of arm64 asm, which clang judged too
  // expensive to inline at -O3 despite it being the hottest call site.
#if defined(_MSC_VER)
  __forceinline
#else
  __attribute__((always_inline)) inline
#endif
  float ProcessSample(bool& sync)
  {
    // --- Phase accumulator ---
    mPos += mCpsCached;
    sync = false;
    if (mPos >= 1.f) {
      mPos -= 1.f;
      mSubState = !mSubState;
      sync = mSubState; // sync pulse every 2 DCO cycles (sub period)
    }

    // --- Gain ramps (always update; cheap and keeps state consistent) ---
    mSawGain   += (mSawTarget   - mSawGain)   * mSwitchRamp;
    mPulseGain += (mPulseTarget - mPulseGain) * mSwitchRamp;
    mSubGain   += (mSubTarget   - mSubGain)   * mSwitchRamp;

    // --- Activity flags: skip sinc reads for silent waveforms ---
    // The target is included so that a freshly-enabled waveform starts
    // computing immediately on the first sample of its ramp-up, even
    // though mXGain is still near zero.
    const bool sawActive   = (mSawGain   > kGainEps) || (mSawTarget   > 0.f);
    const bool pulseActive = (mPulseGain > kGainEps) || (mPulseTarget > 0.f);
    const bool subActive   = (mSubGain   > kGainEps) || (mSubTarget   > 0.f);

    float out = 0.f;

    // --- Saw / Pulse: share the ReadBlended(mPos) when both are needed ---
    if (sawActive || pulseActive)
    {
      float sawAtPos = mTables->ReadBlended(mPos, mSawTblIdx, mSawTblBlend);

      if (sawActive)
      {
        float saw;
        if constexpr (kSawCurveEnabled) {
          float curvedPos = mPos * (1.f + kSawCurve * (1.f - mPos));
          saw = mTables->ReadBlended(curvedPos, mSawTblIdx, mSawTblBlend);
        } else {
          saw = sawAtPos;
        }
        out += saw * mSawAmp * mSawGain;
      }

      if (pulseActive)
      {
        // Pulse = saw(phase) - saw(phase - pw) - DC. Bandlimited by
        // construction (difference of two bandlimited saws).
        float pulseShift = mTables->ReadBlended(mPos - mEffPW, mSawTblIdx, mSawTblBlend);
        float pulse = sawAtPos - pulseShift - mPwDC;
        out += pulse * mPulseAmp * mPulseGain;
      }
    }

    // --- Sub: half-frequency square via saw-difference identity ---
    if (subActive)
    {
      float subPhase = (mPos + (mSubState ? 1.f : 0.f)) * 0.5f;
      float sub = mTables->ReadBlended(subPhase - 0.5f, mSubTblIdx, mSubTblBlend)
                - mTables->ReadBlended(subPhase,        mSubTblIdx, mSubTblBlend);
      out += sub * mSubAmp * mSubLevel * mSubGain;
    }

    return out;
  }

  // Backward-compatible wrapper: one-shot Process with all args, no
  // split. Useful for priming and for any caller that doesn't want
  // to use the PrepareBlock / ProcessSample split.
  float Process(float cps, float pulseWidth, bool sawOn, bool pulseOn, bool subOn, float subLevel,
                float noiseAmp, bool& sync) {
    (void)noiseAmp;
    PrepareBlock(cps, pulseWidth, sawOn, pulseOn, subOn, subLevel);
    return ProcessSample(sync);
  }
};

} // namespace kr106
