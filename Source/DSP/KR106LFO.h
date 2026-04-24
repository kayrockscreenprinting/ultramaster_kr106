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
  bool mActive      = false; // any voice busy (including release tails)?
  bool mGated       = false; // any voice has active gate (key held)?
  bool mWasGated    = false; // previous block's gated state
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

  // --- J106 integer LFO state (firmware-accurate) ---
  // 16-bit accumulator, range 0x0000-0x1FFF, advances by speed coeff
  // every other main-loop tick. Direction flips on overflow/underflow
  // with clamping. Polarity toggles on each direction change.
  uint16_t mIntAccum     = 0;      // $FF4D: LFO accumulator 0x0000-0x1FFF
  uint16_t mIntCoeff     = 0;      // speed table coefficient for current rate
  bool     mIntRising    = true;   // accumulator direction
  bool     mIntPolarity  = false;  // toggles each direction change (bit 1 of $FF4A)
  float    mIntTri       = 0.f;    // held triangle output [-1,+1]

  // --- J106 two-stage delay state ---
  // Delay envelope also advances at tick rate on hardware.
  uint16_t mHoldoffAccum  = 0;     // $FF56: holdoff accumulator
  uint16_t mHoldoffInc    = 0;     // attack table coefficient for holdoff rate
  uint16_t mRampAccum     = 0;     // $FF5A: ramp accumulator (depth = high byte / 255)
  uint16_t mRampInc       = 0;     // ramp table coefficient
  bool     mInHoldoff     = false;
  bool     mArmed         = true;  // armed for reset (all voices were silent)
  float    mSlider        = 0.f;   // stored slider value for reset
  float    mAmpInt        = 0.f;   // held onset envelope [0,1] for J106

  // J60 LFO rate: same circuit as J6 (A-taper pot differs but function is same).
  // TODO: J60 uses 50KA pot (J6 uses A54), may need different alpha curve.
  static float lfoFreqJ60(float slider) { return lfoFreqJ6(slider); }

  float lfoFreq(float t)
  {
    if (mModel == kJ106) return lfoFreqJ106(t);
    if (mModel == kJ60)  return lfoFreqJ60(t);
    return lfoFreqJ6(t);
  }

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
  //   - 16-bit value in $FF4D, range $0000-$1FFF
  //   - Rate coefficient from 0C60_lfoSpeedTbl in $FF4B
  //   - Rising: DADD EA,BC, overflow at >= $2000, clamp to $1FFF, flip direction
  //   - Falling: DSUBNB EA,BC, underflow < $0000, clamp to $0000, flip direction
  //   - Each main-loop pass advances the accumulator by one coefficient step
  //
  // INTEGER STEPPING WITH CLAMPING IS THE DOMINANT EFFECT.
  // The firmware's LFO is not continuous — it takes integer steps and hard-
  // clamps on overflow, discarding any excess motion. So the number of main-
  // loop passes per half-cycle is ceil(8192 / coeff), not 8192/coeff:
  //   coeff=380:  ceil(8192/380)  = 22 passes/half-cycle  (ideal: 21.56)
  //   coeff=1800: ceil(8192/1800) =  5 passes/half-cycle  (ideal: 4.55)
  //   coeff=1960: ceil(8192/1960) =  5 passes/half-cycle  (ideal: 4.18)
  //   coeff=4096: ceil(8192/4096) =  2 passes/half-cycle  (ideal: 2.00)
  // At high rates this quantizes the LFO frequency onto a coarse discrete set:
  // coefficients 1800 and 1960 both produce the same 5-passes-per-half-cycle
  // rate (confirmed on hardware: B32=11.67 Hz, B85=11.76 Hz, 0.8% difference
  // is measurement noise over the 20-40 counted cycles).
  //
  // The polarity bit (bit 1 of $FF4A_lfoFlag) toggles at half the rate of the
  // direction bit, since INRW increments the flag once per direction change.
  // Polarity determines whether the LFO adds to or subtracts from modulation
  // targets, so one audible period = 2 full accumulator cycles = 4 half-cycles:
  //   passes_per_full_LFO_period = 4 * ceil(8192 / coeff)
  //   freq = 1 / (passes_per_period * kLoopPeriodMs / 1000)
  //
  // Hardware verification (lfrancis unit, 96 kHz capture):
  //   coeff  HW Hz  Integer model @ 4.27ms  |  Old continuous formula
  //    380   2.686       2.662 (-0.9%)      |      2.761 (+2.8%)
  //    698   4.840       4.880 (+0.8%)      |      5.072 (+4.8%)
  //   1800  11.666      11.710 (+0.4%)      |     13.079 (+12%)
  //   1960  11.759      11.710 (-0.4%)      |     14.242 (+21%)
  //
  // Byte-exact 0C60_lfoSpeedTbl from D7811G ROM (128 × 16-bit entries).
  // Linear interpolation between integer indices for smooth slider sweeps.
  //
  // Structure of the table:
  //   idx 0–7:    slow-rate fine control (5,15,25,40,55,70,80,90)
  //   idx 8–63:   perfectly linear, step=10
  //   idx 64–95:  perfectly linear, step=16
  //   idx 96–127: accelerating region with irregular steps
  //
  // The bottom 8 entries give finer LFO-rate resolution at the slowest
  // settings, important for ambient/drone patches with many-second cycles.
  static constexpr uint16_t kLfoSpeedTbl[128] = {
    0x0005, 0x000f, 0x0019, 0x0028, 0x0037, 0x0046, 0x0050, 0x005a, // 0c60 (idx 0-7)
    0x0064, 0x006e, 0x0078, 0x0082, 0x008c, 0x0096, 0x00a0, 0x00aa, // 0c70 (idx 8-15)
    0x00b4, 0x00be, 0x00c8, 0x00d2, 0x00dc, 0x00e6, 0x00f0, 0x00fa, // 0c80 (idx 16-23)
    0x0104, 0x010e, 0x0118, 0x0122, 0x012c, 0x0136, 0x0140, 0x014a, // 0c90 (idx 24-31)
    0x0154, 0x015e, 0x0168, 0x0172, 0x017c, 0x0186, 0x0190, 0x019a, // 0ca0 (idx 32-39)
    0x01a4, 0x01ae, 0x01b8, 0x01c2, 0x01cc, 0x01d6, 0x01e0, 0x01ea, // 0cb0 (idx 40-47)
    0x01f4, 0x01fe, 0x0208, 0x0212, 0x021c, 0x0226, 0x0230, 0x023a, // 0cc0 (idx 48-55)
    0x0244, 0x024e, 0x0258, 0x0262, 0x026c, 0x0276, 0x0280, 0x028a, // 0cd0 (idx 56-63)
    0x029a, 0x02aa, 0x02ba, 0x02ca, 0x02da, 0x02ea, 0x02fa, 0x030a, // 0ce0 (idx 64-71)
    0x031a, 0x032a, 0x033a, 0x034a, 0x035a, 0x036a, 0x037a, 0x038a, // 0cf0 (idx 72-79)
    0x039a, 0x03aa, 0x03ba, 0x03ca, 0x03da, 0x03ea, 0x03fa, 0x040a, // 0d00 (idx 80-87)
    0x041a, 0x042a, 0x043a, 0x044a, 0x045a, 0x046a, 0x047a, 0x048a, // 0d10 (idx 88-95)
    0x04be, 0x04f2, 0x0526, 0x055a, 0x058e, 0x05c2, 0x05f6, 0x062c, // 0d20 (idx 96-103)
    0x0672, 0x06b8, 0x0708, 0x0758, 0x07a8, 0x07f8, 0x085c, 0x08c0, // 0d30 (idx 104-111)
    0x0924, 0x0988, 0x09ec, 0x0a50, 0x0ab4, 0x0b18, 0x0b7c, 0x0be0, // 0d40 (idx 112-119)
    0x0c58, 0x0cd0, 0x0d48, 0x0dde, 0x0e74, 0x0f0a, 0x0fa0, 0x1000  // 0d50 (idx 120-127)
  };

  static float lfoSpeedCoeff(float i)
  {
    if (i <= 0.f)   return static_cast<float>(kLfoSpeedTbl[0]);
    if (i >= 127.f) return static_cast<float>(kLfoSpeedTbl[127]);
    int idx = static_cast<int>(i);
    float frac = i - static_cast<float>(idx);
    float a = static_cast<float>(kLfoSpeedTbl[idx]);
    float b = static_cast<float>(kLfoSpeedTbl[idx + 1]);
    return a + frac * (b - a);
  }

  static float lfoFreqJ106(float t)
  {
    // Integer-stepping model. The firmware advances the accumulator by one
    // `coeff` per main-loop pass and hard-clamps on overflow — discrete steps,
    // not continuous arithmetic. Hardware measurements confirm this dominates
    // the rate relationship, especially at high coefficients.
    float coeff_f = lfoSpeedCoeff(t * 127.f);
    int coeff = static_cast<int>(coeff_f + 0.5f);
    if (coeff < 1) coeff = 1;                                // divide-by-zero guard
    // Passes per half accumulator sweep = ceil(8192 / coeff)
    int passesHalf = (8192 + coeff - 1) / coeff;
    // One audible period = 4 half-sweeps (polarity bit toggles at half rate)
    int passesFull = 4 * passesHalf;
    return 1000.f / (static_cast<float>(passesFull) * kLoopPeriodMs);
  }

  void SetRate(float slider, float sampleRate)
  {
    mSampleRate = sampleRate;
    mFreq       = lfoFreq(slider) / sampleRate;
    // J106: store integer speed coefficient for tick-rate accumulator
    if (mModel == kJ106)
    {
      int byte = static_cast<int>(slider * 127.f + 0.5f);
      byte = std::clamp(byte, 0, 127);
      mIntCoeff = kLfoSpeedTbl[byte];
    }
  }

  // FIXME(kr106) Measure LFO delay vs slider voltage on hardware Juno-6
  // Returns tau in seconds for RC exponential envelope.
  static float lfoDelayJ6(float t) { return t * 1.5f; }

  // J60 LFO delay: same circuit as J6.
  // TODO: J60 uses 50KB inverted pot + 22K shunt to GND.
  static float lfoDelayJ60(float t) { return lfoDelayJ6(t); }

  // --- Juno-106 LFO delay: two-stage envelope from D7811G firmware ---
  //
  // Stage 1 - Holdoff: accumulator += attackTable[pot] per main-loop pass.
  //   Completes when accumulator >= 0x4000. LFO depth = 0 during this phase.
  //   Uses the same attack rate table as the ADSR (0B60_envAtkTbl).
  //
  // Stage 2 - Ramp: accumulator += rampTable[pot>>4] per main-loop pass.
  //   LFO depth = high byte of 16-bit accumulator / 255.
  //   Linear fade-in until 16-bit overflow, then clamp to full depth.
  //   Ramp table (0B30_LfoDelayRampTbl): 8 entries indexed by pot >> 4.

  // Main-loop period: canonical value lives in ADSR::kLoopPeriodMs. All
  // firmware-rate modulation (ADSR, LFO delay holdoff/ramp, VCF tick,
  // portamento) shares the same constant so changes stay coherent.
  // See ADSR header for measurement history and calibration notes.
  static constexpr float kLoopPeriodMs = ADSR::kLoopPeriodMs;
  static constexpr float kLoopTickRate = ADSR::kTickRate;  // ~234.2 Hz

  // Legacy alias kept so external callers (if any) still compile. Prefer
  // kLoopTickRate in new code.
  static constexpr float kDelayTickRate = kLoopTickRate;

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
    return ticks / kLoopTickRate;
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
    return (static_cast<float>(rampInc) / 65536.f) * kLoopTickRate;
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
  void SetVoiceActive(bool busy, bool gated) { mActive = busy; mGated = gated; }

  // Update gate/reset state. Call once per block before filling LFO buffer.
  // For J106, this handles the arm/reset logic that was in Process() for J6.
  void UpdateGateState()
  {
    bool newState = mActive || mTrigger;
    bool gated = mGated || mTrigger;

    if (gated && !mWasGated)
    {
      if (mMode == 1 || mArmed)
      {
        mAmp = 0.f;
        mAmpInt = 0.f;
        mArmed = false;
        if (mModel != kJ106)
          RecalcDelayJ6();
        else
        {
          // Reset only the onset envelope ($FF56 holdoff, $FF5A ramp)
          // — matches ROM $0318/$031C. The LFO accumulator $FF4D and
          // direction/polarity flag $FF4A are never reset in hardware;
          // they free-run from power-on. This is the mechanism behind
          // the B54/A87 "growing signal over retriggers" behavior.
          RecalcDelay106();
        }
      }
    }
    if (!gated && mWasGated)
      mArmed = true;

    mWasGated = gated;
    mWasActive = newState;
  }

  // J106 integer LFO tick. Call once per main-loop tick (~234 Hz).
  // Advances the integer accumulator and onset envelope, matching the
  // D7811G firmware at $074E.
  void Tick106()
  {
    // Advance accumulator
    if (mIntRising)
    {
      uint32_t sum = static_cast<uint32_t>(mIntAccum) + mIntCoeff;
      if (sum >= 0x2000)
      {
        mIntAccum = 0x1FFF; // clamp at ceiling
        mIntRising = false;
        // Polarity does NOT flip here. INRW $FF4A increments the flag byte:
        // at the top clamp, bit 0 goes 0->1 or 0->1 (after bit 1 flip), but
        // bit 1 (polarity) is only toggled by the carry out of bit 0, which
        // occurs at the *bottom* clamp. One full audible period = 4 half-
        // sweeps of the accumulator, with a single polarity flip per period.
      }
      else
        mIntAccum = static_cast<uint16_t>(sum);
    }
    else
    {
      if (mIntCoeff > mIntAccum)
      {
        mIntAccum = 0x0000; // clamp at floor
        mIntRising = true;
        mIntPolarity = !mIntPolarity; // INRW carry: %01->%10 or %11->%00
      }
      else
        mIntAccum -= mIntCoeff;
    }

    // Convert accumulator to triangle [-1, +1].
    // Accumulator range 0x0000-0x1FFF maps to 0.0-1.0.
    // Polarity bit determines sign.
    float mag = static_cast<float>(mIntAccum) / 8191.f;
    mIntTri = mIntPolarity ? -mag : mag;

    // Onset envelope: holdoff then ramp, also at tick rate.
    if (mInHoldoff)
    {
      uint32_t sum = static_cast<uint32_t>(mHoldoffAccum) + mHoldoffInc;
      if (sum >= 0x4000)
      {
        mInHoldoff = false;
        mHoldoffAccum = 0x4000;
      }
      else
        mHoldoffAccum = static_cast<uint16_t>(sum);
      mAmpInt = 0.f; // silent during holdoff
    }
    else
    {
      uint32_t sum = static_cast<uint32_t>(mRampAccum) + mRampInc;
      if (sum >= 0x10000)
      {
        mRampAccum = 0xFFFF;
        mAmpInt = 1.f;
      }
      else
      {
        mRampAccum = static_cast<uint16_t>(sum);
        mAmpInt = static_cast<float>(mRampAccum >> 8) / 255.f;
      }
    }
  }

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

    // Gate/reset state is handled by UpdateGateState() called once per block.
    // For J6/J60 Process() is called per-sample, but gate state only changes
    // at block boundaries when SetVoiceActive is called, so this is equivalent.
    bool newState = mActive || mTrigger;

    mPos += freq;
    if (mPos >= 1.f)
      mPos -= 1.f;

    // Advance whenever any voice is active — matches ROM $030D-$0323 where
    // holdoff ($FF56) and ramp ($FF5A) accumulators advance every main loop
    // tick regardless of gate state. mArmed only gates the reset on note-on
    // after silence; it does not pause accumulation during release tails.
    if (newState && mAmp < 1.f)
    {
      // J6/J60: RC exponential envelope approaching 1.0 asymptotically.
      // (J106 onset envelope is handled by Tick106(), not Process().)
      if (mDelayCoeff <= 0.f)
        mAmp = 1.f; // instant
      else
        mAmp += mDelayCoeff * (1.f - mAmp);
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
    // Integer delay state for Tick106()
    mHoldoffInc = ADSR::AttackIncFromSlider(mSlider);
    mHoldoffAccum = 0;
    mInHoldoff = (mHoldoffInc < ADSR::kEnvMax); // instant if inc >= kEnvMax
    int pot = static_cast<int>(mSlider * 127.f + 0.5f);
    int idx = std::clamp(pot >> 4, 0, 7);
    mRampInc = kLfoRampTable[idx];
    mRampAccum = 0;
    mAmpInt = 0.f;
  }
};

} // namespace kr106