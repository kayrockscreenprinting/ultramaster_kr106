#pragma once

#include <cmath>
#include <algorithm>
#include "KR106ADSR.h" // for ADSR::AttackIncFromSlider

// Global triangle LFO with delayed onset
// Ported from kr106_lfo.h/kr106_lfo.C
//
// Juno-6:   RC exponential delay envelope (eases in at top, capacitor curve)
// Juno-106: Two-stage digital envelope from D7811G firmware:
//           1. Holdoff — silent period, duration from attack table
//           2. Ramp    — linear fade-in, rate from 8-entry LFO delay ramp table
//
// Improvements over naive version:
// - Rounded triangle (soft-clipped peaks, matching capacitor charge curve)
// - Free-running in auto mode (delay envelope persists across legato notes)

namespace kr106
{

// LFO note divisions for DAW sync (ordered slowest to fastest)
enum LfoDivision
{
  kLfoMaxima = 0, // 24 beats
  kLfoLonga,      // 16 beats
  kLfoBreve,      // 8 beats
  kLfoDiv1,       // 4 beats (whole note)
  kLfoDiv2,       // 2 beats (half note)
  kLfoDiv4,       // 1 beat (quarter note)
  kLfoDiv4T,      // 1/4 triplet
  kLfoDiv8,       // 1/8 note
  kLfoDiv8T,      // 1/8 triplet
  kLfoDiv16,      // 1/16 note
  kLfoDiv16T,     // 1/16 triplet
  kLfoDiv32,      // 1/32 note
  kLfoDiv64,      // 1/64 note
  kNumLfoDivisions
};

// Beats per LFO cycle for each division
static constexpr double kLfoDivBeats[kNumLfoDivisions] = {
  24.0,        // Maxima
  16.0,        // Longa
  8.0,         // Breve
  4.0,         // 1/1
  2.0,         // 1/2
  1.0,         // 1/4
  2.0 / 3.0,   // 1/4T
  0.5,         // 1/8
  1.0 / 3.0,   // 1/8T
  0.25,        // 1/16
  1.0 / 6.0,   // 1/16T
  0.125,       // 1/32
  0.0625       // 1/64
};

static constexpr const char* kLfoDivNames[kNumLfoDivisions] = {
  "24 Beats", "16 Beats", "8 Beats", "4 Beats", "2 Beats", "Quarter", "Quarter Triplet", "Eighth", "Eighth Triplet", "16th Note", "16th Triplet", "32nd Note", "64th Note"
};

static inline int lfoDivisionFromSlider(float t)
{
  int d = static_cast<int>(std::round(t * (kNumLfoDivisions - 1)));
  return std::max(0, std::min(d, kNumLfoDivisions - 1));
}

static inline float sliderFromLfoDivision(int d)
{
  return static_cast<float>(d) / static_cast<float>(kNumLfoDivisions - 1);
}

struct LFO
{
  float mPos        = 0.f; // phase [0, 1)
  float mFreq       = 0.f; // cycles per sample
  float mAmp        = 0.f; // current amplitude envelope [0, 1]
  float mLastTri    = 0.f; // raw triangle waveform [-1,+1] before envelope
  float mDelayCoeff = 0.f; // J6: RC envelope coefficient (0 = instant)
  float mDelayParam = 0.f; // J6: tau in seconds
  float mSampleRate = 44100.f;
  bool mActive      = false; // any voice currently gated?
  bool mWasActive   = false;
  int mMode         = 0;     // 0=auto, 1=manual
  bool mTrigger     = false; // manual trigger state
  Model mModel      = kJ106;

  // DAW sync state (set by processor each block)
  bool mSyncToHost   = false;
  bool mHostPlaying  = false;
  bool mHostWasPlaying = false;
  double mHostBPM    = 120.0;
  int mDivision      = kLfoDiv4; // current division (when synced)

  // --- J106 two-stage delay state ---
  float mHoldoffRemaining = 0.f; // samples remaining in holdoff phase
  float mRampPerSample    = 0.f; // depth increment per sample during ramp
  bool  mInHoldoff        = false;
  float mSlider           = 0.f; // stored slider value for reset

  float lfoFreq(float t) { return (mModel != kJ106) ? lfoFreqJ6(t) : lfoFreqJ106(t); }

  // LFO rate comparison (Hz) at slider positions 0–10:
  //
  //   slider   1982      1984
  //   ------   ------    ------
  //     0      0.14      0.04
  //     1      0.27      1.07
  //     2      0.47      1.99
  //     3      0.78      2.91
  //     4      1.28      3.84
  //     5      2.04      4.76
  //     6      3.23      6.26
  //     7      5.07      7.73
  //     8      7.93     10.95
  //     9     12.55     18.67
  //    10     21.11     29.76
  //
  // 1982: A-taper pot (logarithmic feel), 0.14–21 Hz
  // 1984: firmware ROM table (piecewise linear), 0.04–30 Hz

  // Juno-6 LFO rate: slider [0,1] → frequency (Hz)
  // Derived from TA75558S integrator + Schmitt trigger circuit model
  // VR2 50K(A), R24 4.7K, R25 330Ω, R32 150K, C6 0.1µF, R78/R19 = 33K/47K
  static float lfoFreqJ6(float slider) {
    static constexpr float kR24 = 4700.f;
    static constexpr float kR25 = 330.f;
    static constexpr float kR32 = 150000.f;
    static constexpr float kVR2 = 50000.f;
    static constexpr float kC6  = 1e-7f;
    static constexpr float kVsat = 13.5f;
    static constexpr float kBeta = 33000.f / 47000.f;  // R78/R19
    static constexpr float k4VthC6R32 = 4.f * kVsat * kBeta * kC6 * kR32;

    float s = std::clamp(slider, 0.f, 1.f);
    float alpha = (std::pow(10.f, 2.f * s) - 1.f) / 99.f;

    float R_up = (1.f - alpha) * kVR2 + kR24;
    float R_dn = alpha * kVR2 + kR25;

    float Vw = kVsat / (R_up * (1.f/R_up + 1.f/R_dn + 1.f/kR32));
    return Vw / k4VthC6R32;
  }

  // Maps normalized slider 0..1 to LFO frequency in Hz.
  //
  // The LFO is a triangle accumulator in the D7811G main loop ($074E):
  //   - 16-bit value in $FF4D, range $0000–$1FFF
  //   - Rate coefficient from 0C60_lfoSpeedTbl in $FF4B
  //   - Rising: DADD EA,BC, clamp at $1FFF, flip direction
  //   - Falling: DSUBNB EA,BC, clamp at $0000, flip direction
  //   - Full triangle cycle = 2 × $2000 = 16384 accumulator steps
  //
  // Two-cycle: the LFO alternates rising/falling on successive loop
  // iterations (like the sub oscillator), so it updates at half the
  // main loop rate. The 4.2ms loop runs at 238.1 Hz; the LFO's
  // effective tick rate is ~119 Hz. Measured LFO at MIDI 66:
  // 4.84 Hz (table coeff 698 → effective rate ≈ 113.6 Hz).
  //
  // freq = effective_rate × rate_coeff / 16384
  //
  // Clean-room piecewise linear approximation of 0C60_lfoSpeedTbl.
  // Slopes and breakpoints derived from curve analysis of the table
  // shape: linear 0–63 (slope 10), linear 64–95 (slope 16),
  // then accelerating 96–127 in four sub-segments.
  static float lfoSpeedCoeff(float i)
  {
    if (i < 8.f)   return 5.f + i * 12.14f;              // slow ramp
    if (i < 64.f)  return 20.f + i * 10.f;               // linear, step=10
    if (i < 96.f)  return -358.f + i * 16.f;             // linear, step=16
    if (i < 104.f) return 1214.f + (i - 96.f) * 52.3f;   // accelerating
    if (i < 112.f) return 1650.f + (i - 104.f) * 84.3f;
    if (i < 120.f) return 2340.f + (i - 112.f) * 100.f;
    return 3160.f + (i - 120.f) * 133.7f;                // fastest
  }

  static float lfoFreqJ106(float t)
  {
    float coeff = lfoSpeedCoeff(t * 127.f);
    return coeff * kMainLoopRate / 16384.f;
  }

  void SetRate(float slider, float sampleRate)
  {
    mSampleRate = sampleRate;
    mFreq       = lfoFreq(slider) / sampleRate;
  }

  // FIXME(kr106) Measure LFO delay vs slider voltage on hardware Juno-6
  // Returns tau in seconds for RC exponential envelope.
  static float lfoDelayJ6(float t) { return t * 1.5f; }

  // --- Juno-106 LFO delay: two-stage envelope from D7811G firmware ---
  //
  // Stage 1 — Holdoff: accumulator += attackTable[pot] per tick (238.1 Hz).
  //   Completes when accumulator >= 0x4000. LFO depth = 0 during this phase.
  //   Uses the same attack rate table as the ADSR (0B60_envAtkTbl).
  //
  // Stage 2 — Ramp: accumulator += rampTable[pot>>4] per tick.
  //   LFO depth = high byte of 16-bit accumulator / 255.
  //   Linear fade-in until 16-bit overflow, then clamp to full depth.
  //   Ramp table (0B30_LfoDelayRampTbl): 8 entries indexed by pot >> 4.

  // LFO effective tick rate: the main loop runs at 238.1 Hz (4.2 ms), but
  // the LFO is two-cycle (alternates rising/falling each iteration), so
  // it updates at half the loop rate. Measured: MIDI 66 → table coeff 698
  // → 4.84 Hz → 4.84 × 16384 / 698 = 113.6 Hz effective.
  static constexpr float kMainLoopRate = 113.6f;

  // Clean-room LFO delay ramp table (0B30_LfoDelayRampTbl).
  // 8 entries, indexed by (pot >> 4). Larger value = faster ramp.
  static constexpr uint16_t kLfoRampTable[8] = {
    0xFFFF, // pot 0-15:   instant ramp
    0x0419, // pot 16-31:  fast
    0x020C, // pot 32-47
    0x015E, // pot 48-63
    0x0100, // pot 64-79:  slow
    0x0100, // pot 80-95:  (same)
    0x0100, // pot 96-111: (same)
    0x0100  // pot 112-127:(same)
  };

  // Compute holdoff duration in seconds for J106 LFO delay.
  // Uses the same clean-room attack rate function as the ADSR.
  static float lfoHoldoffSeconds106(float slider)
  {
    uint16_t inc = ADSR::AttackIncFromSlider(slider);
    if (inc >= ADSR::kEnvMax) return 0.f; // instant
    // ticks to reach 0x4000: accumulator >= 0x4000 after ceil(0x4000/inc) ticks
    float ticks = static_cast<float>(ADSR::kEnvMax) / static_cast<float>(inc);
    return ticks / kMainLoopRate;
  }

  // Compute ramp rate (depth per second) for J106 LFO delay.
  // Depth = high byte of 16-bit accumulator / 255, so full scale at overflow.
  // Rate per tick = rampTable[idx] / 65536 (normalized 0..1).
  // Rate per second = rate_per_tick * tickRate.
  static float lfoRampPerSecond106(float slider)
  {
    int pot = static_cast<int>(slider * 127.f + 0.5f);
    int idx = std::clamp(pot >> 4, 0, 7);
    uint16_t rampInc = kLfoRampTable[idx];
    if (rampInc == 0xFFFF) return 1e6f; // instant
    return (static_cast<float>(rampInc) / 65536.f) * kMainLoopRate;
  }

  void SetDelay(float slider)
  {
    mSlider = slider;
    if (mModel != kJ106)
    {
      mDelayParam = lfoDelayJ6(slider);
      RecalcDelayJ6();
    }
    else
    {
      RecalcDelay106();
    }
  }

  void SetMode(int mode) { mMode = mode; }
  void SetTrigger(bool trig) { mTrigger = trig; }
  void SetVoiceActive(bool active) { mActive = active; }

  // Process one sample, returns [-1, +1]
  float Process()
  {
    // When synced to host, override frequency from tempo + division
    float freq = mFreq;
    if (mSyncToHost)
    {
      int div = std::max(0, std::min(mDivision, static_cast<int>(kNumLfoDivisions) - 1));
      freq = static_cast<float>(mHostBPM / (60.0 * kLfoDivBeats[div])) / mSampleRate;

      // Reset phase on transport start
      if (mHostPlaying && !mHostWasPlaying)
        mPos = 0.f;
      mHostWasPlaying = mHostPlaying;
    }

    // Auto: LFO runs while any voice is active, delay envelope free-runs
    //        (persists across legato notes, only resets when all voices stop)
    // Manual: LFO runs while any voice is active, delay envelope resets
    //         on each new note-on (retrigger on every keypress)
    bool newState = mActive || mTrigger;

    if (newState && !mWasActive)
    {
      // Auto: only reset envelope when starting from silence
      // Manual: always reset on new activation
      if (mMode == 1 || mAmp <= 0.f)
      {
        mAmp = 0.f;
        if (mModel != kJ106)
          RecalcDelayJ6();
        else
          RecalcDelay106();
      }
    }

    mWasActive = newState;

    mPos += freq;
    if (mPos >= 1.f)
      mPos -= 1.f;

    if (newState && mAmp < 1.f)
    {
      if (mModel != kJ106)
      {
        // J6: RC exponential envelope approaching 1.0 asymptotically
        if (mDelayCoeff <= 0.f)
          mAmp = 1.f; // instant
        else
          mAmp += mDelayCoeff * (1.f - mAmp);
      }
      else
      {
        // J106: holdoff (silent) then linear ramp
        if (mInHoldoff)
        {
          mHoldoffRemaining -= 1.f;
          if (mHoldoffRemaining <= 0.f)
            mInHoldoff = false;
          // mAmp stays 0 during holdoff
        }
        else
        {
          mAmp += mRampPerSample;
          if (mAmp >= 1.f) mAmp = 1.f;
        }
      }
    }

    // Rounded triangle: linear triangle with soft-clipped peaks
    // Linear triangle: +1 at pos=0, -1 at pos=0.5, +1 at pos=1
    float tri = 1.f - 4.f * fabsf(mPos - 0.5f);
    // Soft-clip peaks using cubic saturation (matches capacitor rounding)
    tri = tri * (1.5f - 0.5f * tri * tri);

    mLastTri = tri; // raw waveform before envelope (for J106 integer VCF path)
    return tri * mAmp;
  }

private:
  void RecalcDelayJ6()
  {
    if (mDelayParam <= 0.f)
    {
      mDelayCoeff = 0.f; // instant (handled as special case)
    }
    else
    {
      // mDelayParam is tau in seconds (from lfoDelayJ6)
      mDelayCoeff = 1.f - expf(-1.f / (mDelayParam * mSampleRate));
    }
  }

  void RecalcDelay106()
  {
    float holdoff = lfoHoldoffSeconds106(mSlider);
    mHoldoffRemaining = holdoff * mSampleRate;
    mInHoldoff = (mHoldoffRemaining > 0.f);
    float rampPerSec = lfoRampPerSecond106(mSlider);
    mRampPerSample = rampPerSec / mSampleRate;
  }
};

} // namespace kr106