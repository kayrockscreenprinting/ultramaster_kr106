#pragma once

#include <algorithm>
#include <vector>

// KR-106 arpeggiator
//
// Modes: Up, Up/Down, Down
// Range: 1 oct (held notes only), 2 oct, Full (3 oct)
// Rate: steps per minute (from slider, 60-960 BPM)
//
// The arpeggiator intercepts note-on/off and generates its own note
// events at the arp rate. Each step retriggers the gate (envelope
// re-attacks), matching the hardware behavior.

namespace kr106 {

struct Arpeggiator
{
  bool mEnabled = false;
  int mMode = 0;       // 0=Up, 1=Up/Down, 2=Down
  int mRange = 0;      // 0=1oct, 1=2oct, 2=3oct
  float mRate = 120.f; // steps per minute (mapped from slider)
  float mSampleRate = 44100.f;

  /*
  * arp_rate_hz — Juno-106 Arpeggio Rate oscillator frequency
  *
  * The VR1 section is a Schmitt-trigger relaxation oscillator: capacitor C4
  * charges and discharges through the total timing resistance Rt = R21 + VR1,
  * switching between two thresholds set by the R16/R17 positive-feedback
  * divider on the op-amp's non-inverting input.
  *
  * The threshold fraction is α = R16/(R16+R17) = 47K/147K ≈ 0.3197, so on
  * a ±15V supply the cap swings between −4.796V and +4.796V each half-cycle.
  *
  * Solving the RC charging equation for the half-period:
  *   t_half = Rt · C4 · ln((1+α)/(1−α))
  *          = Rt · C4 · ln(1.3197/0.6803)
  *          ≈ Rt · C4 · 0.6633
  *
  * Full period T = 2 · t_half, so:
  *   f = 1 / (2 · Rt · C4 · 0.6633)
  *     = 1 / (2 · (R21 + pos·VR1_MAX) · C4 · 0.6633)
  *
  * pos=0 → Rt=33K  → f ≈ 48.5 Hz  (fastest arp rate)
  * pos=1 → Rt=1.033M → f ≈  1.55 Hz  (slowest arp rate)
  *
  * Note: response is hyperbolic (f ∝ 1/pos), not linear — the pot
  * is linear taper but the perceived curve is naturally logarithmic.
  */
  static float arpRate(float t) {
    // Our original 2001 linear fader code return 15.f + t * 1785.f;

    // Schmitt-trigger relaxation oscillator: f in Hz, convert to BPM
    // Invert slider: 0=slowest (max resistance), 1=fastest (min resistance)
    float pos = 1.f - t;
    float hz = 1.0f / (2.0f * (33000.0f + pos * 1000000.0f) * 0.47e-6f * 0.6633f);
    return hz * 60.f;
  }

  std::vector<int> mHeldNotes; // sorted ascending

  Arpeggiator() { mHeldNotes.reserve(128); }
  int mStepIndex = 0;
  int mDirection = 1;  // 1=ascending, -1=descending (for Up/Down)
  float mPhase = 0.f;
  int mLastNote = -1;  // currently sounding arp note

  void SetSampleRate(float sr) { mSampleRate = sr; }

  void NoteOn(int note)
  {
    if (note < 0 || note > 127) return;
    auto it = std::lower_bound(mHeldNotes.begin(), mHeldNotes.end(), note);
    if (it != mHeldNotes.end() && *it == note) return; // already held
    mHeldNotes.insert(it, note);

    // First note in: trigger immediately on next process call
    if (mHeldNotes.size() == 1)
    {
      mPhase = 1.f;
      mStepIndex = 0;
      mDirection = 1;
    }
  }

  void NoteOff(int note)
  {
    auto it = std::find(mHeldNotes.begin(), mHeldNotes.end(), note);
    if (it != mHeldNotes.end())
      mHeldNotes.erase(it);

    if (mHeldNotes.empty())
    {
      mStepIndex = 0;
      mDirection = 1;
      mPhase = 0.f;
    }
  }

  void Reset()
  {
    mHeldNotes.clear();
    mStepIndex = 0;
    mDirection = 1;
    mPhase = 0.f;
    mLastNote = -1;
  }

  // Physical keyboard top note (Juno-106: 61 keys, C1–C6, MIDI 36–96).
  // When mLimitToKeyboard is true, notes transposed above this wrap down
  // by octaves until they fit — matching hardware behavior where the arp
  // cannot produce notes above the physical keyboard.
  static constexpr int kMaxArpNote = 96;
  bool mLimitToKeyboard = true;

  // Full sequence length — all slots always present (wrapped notes still play)
  int SeqLen() const
  {
    if (mLimitToKeyboard)
      return static_cast<int>(mHeldNotes.size()) * (mRange + 1);

    int count = 0;
    int octaves = mRange + 1;
    for (int oct = 0; oct < octaves; oct++)
      for (int n : mHeldNotes)
        if (n + oct * 12 <= 127) count++;
    return count;
  }

  // Note at ascending index in the full sequence
  int SeqNote(int idx) const
  {
    int i = 0;
    int octaves = mRange + 1;
    for (int oct = 0; oct < octaves; oct++)
      for (int n : mHeldNotes)
      {
        int note = n + oct * 12;
        if (mLimitToKeyboard)
        {
          while (note > kMaxArpNote) note -= 12;
        }
        else
        {
          if (note > 127) continue;
        }
        if (i == idx) return note;
        i++;
      }
    return -1;
  }

  // Advance step and return the next note to play
  int NextNote()
  {
    int len = SeqLen();
    if (len == 0) return -1;

    // Clamp after sequence changes
    if (mStepIndex >= len) mStepIndex = 0;
    if (mStepIndex < 0) mStepIndex = len - 1;

    int note;
    switch (mMode)
    {
      case 0: // Up
        note = SeqNote(mStepIndex);
        mStepIndex = (mStepIndex + 1) % len;
        break;

      case 1: // Up/Down — peak and trough notes play once
        note = SeqNote(mStepIndex);
        if (len > 1)
        {
          mStepIndex += mDirection;
          if (mStepIndex >= len) { mStepIndex = len - 2; mDirection = -1; }
          else if (mStepIndex < 0) { mStepIndex = 1; mDirection = 1; }
        }
        break;

      case 2: // Down
        note = SeqNote(len - 1 - mStepIndex);
        mStepIndex = (mStepIndex + 1) % len;
        break;

      default:
        note = SeqNote(0);
    }
    return note;
  }

  // Process one block. Calls noteOn(noteNum, sampleOffset) and
  // noteOff(noteNum, sampleOffset) for each arp step.
  template <typename NoteOnF, typename NoteOffF>
  void Process(int nFrames, NoteOnF noteOn, NoteOffF noteOff)
  {
    if (!mEnabled || mHeldNotes.empty())
    {
      // Release lingering arp note when disabled or all keys released
      if (mLastNote >= 0)
      {
        noteOff(mLastNote, 0);
        mLastNote = -1;
      }
      return;
    }

    float inc = mRate / (60.f * mSampleRate);

    for (int s = 0; s < nFrames; s++)
    {
      mPhase += inc;
      if (mPhase >= 1.f)
      {
        mPhase -= 1.f;

        // Release previous arp note
        if (mLastNote >= 0)
          noteOff(mLastNote, s);

        // Trigger next arp note
        int note = NextNote();
        if (note >= 0)
        {
          noteOn(note, s);
          mLastNote = note;
        }
      }
    }
  }
};

} // namespace kr106
