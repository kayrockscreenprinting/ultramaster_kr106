#pragma once

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>

namespace kr106 {

enum Model { kJ6 = 0, kJ60 = 1, kJ106 = 2 };

// Decay/release per-tick multiplier table (0.16 unsigned fixed-point).
// Reproduces the D7811G ROM table (0D60_envDecRelTbl) structure:
// piecewise linear, 7 segments with step sizes chosen so that key
// complement values (0x10000 - entry) land on powers of 2.
// Index 0 = $1000 (very fast, ~6% retained per tick),
// index 127 = $FFF4 (slowest, multiplier ~ 0.9993).
static constexpr std::array<uint16_t, 128> GenerateDecRelTable()
{
  constexpr int kCounts[] = {  4,      1,      10,     28,     22,     58,     4     };
  constexpr uint16_t kSteps[] = { 0x2000, 0x1000, 0x0800, 0x0080, 0x000C, 0x0004, 0x0001 };
  std::array<uint16_t, 128> t{};
  uint16_t val = 0x1000;
  int i = 0;
  t[i++] = val;
  for (int seg = 0; seg < 7; ++seg)
    for (int n = 0; n < kCounts[seg]; ++n)
      t[i++] = (val = static_cast<uint16_t>(val + kSteps[seg]));
  return t;
}
static constexpr auto kDecRelTable = GenerateDecRelTable();

// ============================================================
// ADSR Envelope
// ============================================================
//
// J6/J60 mode (mModel != kJ106):
//   All stages are pure RC curves from the IR3R01 analog EG.
//   Attack charges toward 1.2 (comparator overshoot), decay/release
//   discharge toward undershoot targets to ensure finite completion.
//   Slider->tau mapped by exponential formula from measured hardware.
//
// J106 mode (mModel == kJ106):
//   ROM-accurate D7811G digital envelope generator, reverse-engineered
//   from the IC29 (uPD7811G-152) disassembly of the Juno-106 voice CPU.
//   ROM data sourced from: https://github.com/ErroneousBosh/j106roms
//
//   The D7811G runs one envelope tick per DAC update (every 4.2ms, ~238 Hz).
//   The envelope is a 14-bit integer (0–$3FFF), processed as follows:
//
//   Attack:  Linear ramp. Each tick adds a value from 0B60_envAtkTbl[slider].
//            ROM values range from $4000 (instant) to $0015 (~3.3s to peak).
//            The envelope is clamped at $3FFF (ONI A,$C0 check at $057C).
//
//   Decay:   Exponential toward sustain. Each tick computes:
//              diff = env - sustain
//              diff = CalcDecay(diff, 0D60_envDecRelTbl[slider])
//              env  = diff + sustain
//            This is exponential convergence toward the sustain level —
//            NOT toward zero. When env <= sustain, it snaps to sustain
//            (DMOV EA,BC at $0524 in the ROM).
//
//   Release: Exponential toward zero. Each tick computes:
//              env = CalcDecay(env, 0D60_envDecRelTbl[slider])
//            Terminates when env reaches 0 (integer truncation ensures
//            this always happens in finite time).
//
//   CalcDecay ($083D in ROM):
//            16×16 fixed-point multiply using three 8×8 MUL instructions,
//            accumulating partial products VH*CH + (VH*CL)>>8 + (VL*CH)>>8.
//            The fourth partial product (VL*CL) is never computed — the 8-bit
//            uPD7811 ALU would need an extra MUL and the precision isn't needed.
//            This omission means each tick truncates slightly more than a
//            mathematically exact multiply. At the slowest coefficient ($FFF4),
//            the per-tick loss is ~4 instead of ~3. Over thousands of ticks this
//            compounds to make decay ~6% faster than a full 16×16 multiply,
//            producing the characteristic ~12s max decay-to-50% observed in
//            hardware. We replicate this exactly.
//
//   The integer output is linearly interpolated between tick boundaries
//   to produce smooth audio-rate output (the real DAC staircase is smoothed
//   by the analog output circuitry anyway).
struct ADSR
{
  enum State { kAttack, kDecay, kRelease, kFinished };
  // Gate mode: hardware snaps VCA fully open/closed in one DAC tick.
  // BA662 analog slew provides ~1ms smoothing. We model this as a
  // fast per-sample ramp (32 audio samples ≈ 0.7ms at 44.1k).
  static constexpr float kGateSlope       = 1.f / 32.f;
  static constexpr float kSilence         = 1e-5f;   // -100dB: J6 release termination threshold
  static constexpr float kAttackTarget    = 1.2f;    // RC charge overshoot (hardware comparator)
  static constexpr float kReleaseTarget   = -0.1f;   // below zero (ensures finite release)
  static constexpr float kTickRate        = 1000.f / 4.2f; // D7811G DAC update rate ≈ 238 Hz
  static constexpr uint16_t kEnvMax       = 0x3FFF;  // 14-bit envelope maximum

  // Attack increment from slider position (0..1).
  // Cleanroom piecewise approximation of the D7811G ROM attack table
  // (0B60_envAtkTbl), derived from curve-shape analysis of the ROM's
  // timing behaviour. Five regions:
  //   idx 0:       instant (kEnvMax)
  //   idx 1-63:    harmonic series  8192 / idx
  //   idx 64-86:   linear ramp,  slope ~ -11/4 per idx
  //   idx 87-107:  linear ramp,  slope ~ -3/2 per idx
  //   idx 108-121: linear ramp,  slope ~ -1/2 per idx
  //   idx 122-127: linear ramp,  slope -1 per idx
  // 123/128 exact matches; 5 entries off by ±1 (max 1.1%).
  static uint16_t AttackIncFromSlider(float slider)
  {
    float s = std::clamp(slider, 0.f, 1.f);
    if (s < 0.003937f) return kEnvMax;
    if (s <= 0.500000f)
        return static_cast<uint16_t>(8192.f / (s * 127.f) + 0.5f);
    if (s <= 0.681102f)
        return static_cast<uint16_t>(305.03f - 352.26f * s + 0.5f);
    if (s <= 0.846457f)
        return static_cast<uint16_t>(194.74f - 190.50f * s + 0.5f);
    if (s <= 0.956693f)
        return static_cast<uint16_t>(86.37f - 62.52f * s + 0.5f);
    return static_cast<uint16_t>(std::max(
        static_cast<int>(148.f - 127.f * s + 0.5f), 1));
  }

  // ROM-accurate fixed-point multiply matching D7811G calcDecay at $083D.
  // The 8-bit uPD7811 decomposes the 16×16 multiply into three 8×8 MUL
  // instructions (VH*CH, VH*CL, VL*CH), dropping the VL*CL partial product.
  // A is separate from EA on this CPU (A = m_va.b.l, EA = m_ea.w.l), so
  // the third MUL computes VH*CH (not a cross-term as initially assumed).
  // Confirmed via MAME uPD7810 core (mamedev/mame upd7810_opcodes.cpp).
  //
  // At max coefficient ($FFF4), this gives ~9.6s to 50% / ~25.5s to digital
  // zero. The long tail is inaudible through the VCA — matching real hardware
  // where the perceived decay is ~12s but the digital envelope runs longer.
  static uint16_t CalcDecay(uint16_t value, uint16_t coeff)
  {
    uint8_t vh = value >> 8;     // value high byte
    uint8_t vl = value & 0xFF;   // value low byte
    uint8_t ch = coeff >> 8;     // coefficient high byte
    uint8_t cl = coeff & 0xFF;   // coefficient low byte
    return static_cast<uint16_t>(vh * ch)           // VH*CH  ($0849)
         + static_cast<uint16_t>((vh * cl) >> 8)    // VH*CL >> 8  ($083D)
         + static_cast<uint16_t>((vl * ch) >> 8);   // VL*CH >> 8  ($0843)
  }

  State mState = kFinished;
  float mEnv = 0.f;
  float mGateEnv = 0.f;
  Model mModel = kJ106;

  // --- Juno-106 integer state ---
  uint16_t mEnvInt = 0;        // current 14-bit envelope (0–kEnvMax)
  uint16_t mAtkInc = 0;        // attack increment per tick
  uint16_t mDecMul = 0;        // decay multiplier per tick
  uint16_t mRelMul = 0;        // release multiplier per tick
  uint16_t mSusInt = 0;        // sustain level as integer
  float mTickAccum = 0.f;      // fractional tick accumulator
  float mEnvPrev = 0.f;        // previous tick output (for interpolation)
  float mEnvNext = 0.f;        // current tick output (for interpolation)
  float mTickStep = 0.f;       // ticks per sample (kTickRate * mTimeScale / mSampleRate)

  // --- Juno-6 mode coefficients (one-pole RC) ---
  float mAttackCoeff = 0.f;
  float mDecayCoeff = 0.f;
  float mReleaseCoeff = 0.f;

  float mSustain = 1.f;
  float mSampleRate = 44100.f;
  float mTimeScale = 1.f;      // per-voice component tolerance

  void SetSampleRate(float sr)
  {
    mSampleRate = sr;
    mTickStep = (kTickRate * mTimeScale) / mSampleRate;
  }

  // --- Juno-106 setters (ROM table index 0–127) ---

  void Set106Attack(float slider)
  {
    mAtkInc = AttackIncFromSlider(slider);
  }

  void Set106Decay(int index)
  {
    mDecMul = kDecRelTable[std::clamp(index, 0, 127)];
  }

  void Set106Release(int index)
  {
    mRelMul = kDecRelTable[std::clamp(index, 0, 127)];
  }

  // --- Juno-6 setters (tau in seconds, from exponential formula) ---

  void SetAttackTau(float tauSeconds)
  {
    mAttackCoeff = 1.f - expf(-1.f / (tauSeconds * mTimeScale * mSampleRate));
  }

  void SetDecayTau(float tauSeconds)
  {
    mDecayCoeff = 1.f - expf(-1.f / (tauSeconds * mTimeScale * mSampleRate));
  }

  void SetReleaseTau(float tauSeconds)
  {
    mReleaseCoeff = 1.f - expf(-1.f / (tauSeconds * mTimeScale * mSampleRate));
  }

  void SetSustain(float s)
  {
    mSustain = s;
    // ROM stores sustain via store16Bit (<<7), ceiling 0x3F80.
    // Envelope peaks at 0x3FFF, so full sustain still triggers one
    // decay tick ($3FFF > $3F80) before snapping — matches hardware.
    mSusInt = static_cast<uint16_t>(s * 0x3F80);
  }

  // --- Display helpers (slider 0-1 -> ms, for tooltips and scope) ---

  // J6 attack tau (seconds) from 11-point log-interpolated hardware measurement
  static float AttackTauJ6(float slider)
  {
    static constexpr float kAttackTau[11] = {
      0.000558f, 0.001674f, 0.008762f, 0.029468f, 0.064015f, 0.120998f,
      0.238481f, 0.495993f, 0.607950f, 1.392486f, 1.674332f
    };
    float s = slider * 10.f;
    int idx = std::min(static_cast<int>(s), 9);
    float frac = s - idx;
    return std::exp(std::log(kAttackTau[idx])
          + frac * (std::log(kAttackTau[idx + 1]) - std::log(kAttackTau[idx])));
  }

  // J6 decay/release tau (seconds) from exponential fit of hardware measurements
  static float DecRelTauJ6(float slider)
  {
    return 0.003577f * std::exp(12.9460f * slider - 5.0638f * slider * slider);
  }

  // J6 attack time in ms (tau * ln(6) * 1000, from IR3R01 comparator overshoot)
  static float AttackMsJ6(float slider)
  {
    return AttackTauJ6(slider) * 1791.8f;
  }

  // J6 decay/release time in ms (tau * ln(11) * 1000)
  static float DecRelMsJ6(float slider)
  {
    return DecRelTauJ6(slider) * 2397.9f;
  }

  // J60 ADSR stubs -- same IR3R01 chip and circuit as J6.
  // TODO: J60 has different pot + shunt resistors (50KB + 22K/18K).
  static float AttackTauJ60(float slider) { return AttackTauJ6(slider); }
  static float DecRelTauJ60(float slider) { return DecRelTauJ6(slider); }
  static float AttackMsJ60(float slider) { return AttackMsJ6(slider); }
  static float DecRelMsJ60(float slider) { return DecRelMsJ6(slider); }

  // J106 attack tau (for ParamValue display)
  static float AttackTauJ106(float slider)
  {
    return AttackMs(slider) / 1791.8f; // approximate, from ms back to tau
  }

  // J106 attack: 1ms RC settling + (ticks - 1) x tick period
  static float AttackMs(float slider)
  {
    uint16_t inc = AttackIncFromSlider(slider);
    int ticks = (kEnvMax + inc - 1) / inc;
    return 1.f + (ticks - 1) * (1000.f / kTickRate);
  }

  // Decay/Release: simulate integer decay from max to -20dB (10%), return time in ms.
  // -20dB matches perceived decay length observed in hardware recordings.
  // Uses CalcDecay integer truncation for accuracy matching actual audio behavior.
  static float DecRelMs(float slider)
  {
    int idx = std::clamp(static_cast<int>(slider * 127.f + 0.5f), 0, 127);
    uint16_t coeff = kDecRelTable[idx];
    uint16_t val = kEnvMax;
    uint16_t target = kEnvMax / 10;  // -20dB
    int ticks = 0;
    while (val > target && ticks < 50000)
    {
      val = CalcDecay(val, coeff);
      ticks++;
    }
    return std::max(1.5f, ticks / kTickRate * 1000.f);
  }

  void NoteOn()
  {
    mState = kAttack;
    if (mModel == kJ106)
    {
      // Save current level as interpolation starting point (for smooth retrigger)
      mEnvPrev = static_cast<float>(mEnvInt) / kEnvMax;
      // Apply first attack tick immediately — matches firmware behavior where
      // NoteOn and envelope update happen in the same 4.2ms loop iteration.
      // Without this, the envelope sits at zero for up to one full tick period.
      // Don't reset mTickAccum — keep in phase with the VCF tick accumulator
      // so both sample the same tick boundaries (as in the real D7811G main loop).
      Tick106();
      mEnvNext = static_cast<float>(mEnvInt) / kEnvMax;
    }
  }

  void NoteOff() { mState = kRelease; }

  bool GetBusy() const { return mState != kFinished || mGateEnv > 0.f; }

  float Process()
  {
    if (mModel != kJ106)
    {
      switch (mState)
      {
        case kAttack:
          mEnv += (kAttackTarget - mEnv) * mAttackCoeff;
          mGateEnv += kGateSlope;
          if (mEnv >= 1.f) { mEnv = 1.f; mState = kDecay; }
          break;
        case kDecay:
          mEnv += (mSustain - mEnv) * mDecayCoeff;
          mGateEnv += kGateSlope;
          break;
        case kRelease:
          mGateEnv -= kGateSlope;
          mEnv += (kReleaseTarget - mEnv) * mReleaseCoeff;
          if (mEnv < kSilence) { mEnv = 0.f; mState = kFinished; }
          break;
        case kFinished:
          mGateEnv -= kGateSlope;
          break;
      }
    }
    else
    {
      // 106 mode: tick-based integer processing with interpolation
      mTickAccum += mTickStep;
      while (mTickAccum >= 1.f)
      {
        mTickAccum -= 1.f;
        mEnvPrev = mEnvNext;
        Tick106();
        mEnvNext = static_cast<float>(mEnvInt) / kEnvMax;
      }
      mEnv = mEnvPrev + (mEnvNext - mEnvPrev) * mTickAccum;

      // Gate envelope
      switch (mState)
      {
        case kAttack:
        case kDecay:
          mGateEnv += kGateSlope;
          break;
        case kRelease:
        case kFinished:
          mGateEnv -= kGateSlope;
          break;
      }
    }

    mGateEnv = std::clamp(mGateEnv, 0.f, 1.f);
    return mEnv;
  }

  // Per-sample gate envelope update, called externally in J106 unified tick mode.
  // Separated from Process() so Voice can drive the tick accumulator while
  // still updating the gate ramp every sample.
  void UpdateGateEnv()
  {
    switch (mState)
    {
      case kAttack:
      case kDecay:
        mGateEnv += kGateSlope;
        break;
      case kRelease:
      case kFinished:
        mGateEnv -= kGateSlope;
        break;
    }
    mGateEnv = std::clamp(mGateEnv, 0.f, 1.f);
  }

  // Run one D7811G envelope tick (called at ~238 Hz, matching the 4.2ms DAC period).
  // This replicates the envelope update loop from the IC29 ROM ($0500–$059A).
  // Public so Voice can call it from the unified firmware tick.
  void Tick106()
  {
    switch (mState)
    {
      case kAttack: {
        // ROM $0570–$0590: linear ramp, add atkInc per tick, clamp at $3FFF
        uint32_t sum = static_cast<uint32_t>(mEnvInt) + mAtkInc;
        if (sum >= kEnvMax)
        {
          mEnvInt = kEnvMax;
          mState = kDecay;
        }
        else
          mEnvInt = static_cast<uint16_t>(sum);
        break;
      }
      case kDecay:
        // ROM $0510–$0530: exponential decay toward sustain level.
        // Subtracts sustain, multiplies the difference, adds sustain back.
        // When env <= sustain, snaps to sustain (ROM: DMOV EA,BC at $0524).
        if (mEnvInt > mSusInt)
        {
          uint16_t diff = mEnvInt - mSusInt;
          diff = CalcDecay(diff, mDecMul);
          mEnvInt = diff + mSusInt;
        }
        else
          mEnvInt = mSusInt;
        break;
      case kRelease:
        // ROM $0540–$0560: exponential release toward zero.
        // Multiplies env directly — converges to 0 via integer truncation.
        mEnvInt = CalcDecay(mEnvInt, mRelMul);
        if (mEnvInt == 0)
          mState = kFinished;
        break;
      case kFinished:
        break;
    }
  }
};

} // namespace kr106
