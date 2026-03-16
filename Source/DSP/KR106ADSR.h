#pragma once

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>

namespace kr106 {

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
// Juno-6 mode (mJ6Mode = true):
//   All stages are pure RC curves from the IR3R01 analog EG.
//   Attack charges toward 1.2 (comparator overshoot), decay/release
//   discharge toward undershoot targets to ensure finite completion.
//   Slider→tau mapped by exponential formula from measured hardware.
//
// Juno-106 mode (mJ6Mode = false):
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
  static constexpr float kGateSlope       = 1.f / 32.f;
  static constexpr float kSilence         = 1e-5f;   // -100dB: J6 release termination threshold
  static constexpr float kAttackTarget    = 1.2f;    // RC charge overshoot (hardware comparator)
  static constexpr float kReleaseTarget   = -0.1f;   // below zero (ensures finite release)
  static constexpr float kTickRate        = 1000.f / 4.2f; // D7811G DAC update rate ≈ 238 Hz
  static constexpr uint16_t kEnvMax       = 0x3FFF;  // 14-bit envelope maximum

  // Attack increment from slider position (0..1).
  // Attempt to reproduce the timing curve of the D7811G ROM attack table
  // (0B60_envAtkTbl) without including copyrighted ROM data.
  // Range: ~1.5ms at slider 0 to ~3.3s at slider 1.
  // The first half (idx 0-63) follows a harmonic series: inc = 8192/idx,
  // giving ticks linear in slider. The second half accelerates.
  // Formula fitted to within 11% of all 128 ROM entries.
  static uint16_t AttackIncFromSlider(float slider)
  {
    float s = std::clamp(slider, 0.f, 1.f);
    if (s < 0.001f) return kEnvMax; // instant attack (~1.5ms)
    float ticks = 268.66f * s * expf(-0.8914f * s + 1.9573f * s * s);
    uint16_t inc = static_cast<uint16_t>(static_cast<float>(kEnvMax) / ticks + 0.5f);
    return std::max(inc, static_cast<uint16_t>(1));
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
  bool  mJ6Mode = false;       // true = Juno-6 RC curves, false = Juno-106 digital

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
    mSusInt = static_cast<uint16_t>(s * kEnvMax);
  }

  // --- Display helpers (slider 0–1 → ms, for tooltips and scope) ---

  // Attack: ticks for linear ramp to reach peak
  static float AttackMs(float slider)
  {
    uint16_t inc = AttackIncFromSlider(slider);
    int ticks = (kEnvMax + inc - 1) / inc;
    return std::max(1.5f, ticks / kTickRate * 1000.f);
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
    if (!mJ6Mode)
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
    if (mJ6Mode)
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

private:
  // Run one D7811G envelope tick (called at ~238 Hz, matching the 4.2ms DAC period).
  // This replicates the envelope update loop from the IC29 ROM ($0500–$059A).
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
