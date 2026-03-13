#pragma once

#include <cmath>

// Global triangle LFO with delayed onset
// Ported from kr106_lfo.h/kr106_lfo.C
//
// Improvements over naive version:
// - Rounded triangle (soft-clipped peaks, matching capacitor charge curve)
// - RC exponential delay envelope (eases in at top, not linear ramp)
// - Free-running in auto mode (delay envelope persists across legato notes)

namespace kr106
{

struct LFO
{
  float mPos        = 0.f; // phase [0, 1)
  float mFreq       = 0.f; // cycles per sample
  float mAmp        = 0.f; // current amplitude envelope [0, 1]
  float mDelayCoeff = 0.f; // RC envelope coefficient (0 = instant)
  float mDelayParam = 0.f; // stored delay parameter
  float mSampleRate = 44100.f;
  bool mActive      = false; // any voice currently gated?
  bool mWasActive   = false;
  int mMode         = 0;     // 0=auto, 1=manual
  bool mTrigger     = false; // manual trigger state
  bool mJ6Mode      = false; // true = Juno-6, false = Juno-106

  float lfoFreq(float t) { return mJ6Mode ? lfoFreqJ6(t) : lfoFreqJ106(t); }

  // FIXME(kr106) Measure LFO frq compared to slider voltage on Hardward Juno 6
  static float lfoFreqJ6(float t) { 
    // linear mapping from our original 2001 codebaser
    return ( ( 18.f + t * 1182.f ) / 60.f );
  }

  // Maps normalized slider 0..1 to LFO frequency in Hz
  // Derived from curve analysis of Roland Juno-106 voice firmware timing.
  // Frame rate 238.1 Hz (1/4.2ms), LFO accumulator full cycle = 16384 steps.
  static float lfoFreqJ106(float t)
  {
    float i = t * 127.f;
    float coeff;
    if (i < 96.f)
      coeff = 5.f + 12.18f * i; // linear region
    else
      coeff = 1214.f * powf(1.039f, i - 96.f); // exponential region

    // freq = coeff * frameRate / cyclePeriod
    //      = coeff * 238.1 / 16384
    return coeff / 68.81f;
  }

  void SetRate(float slider, float sampleRate)
  {
    mSampleRate = sampleRate;
    mFreq       = lfoFreq(slider) / sampleRate;
  }

  void SetDelay(float delayParam)
  {
    mDelayParam = delayParam;
    RecalcDelay();
  }

  void SetMode(int mode) { mMode = mode; }
  void SetTrigger(bool trig) { mTrigger = trig; }
  void SetVoiceActive(bool active) { mActive = active; }

  // Process one sample, returns [-1, +1]
  float Process()
  {
    bool newState = (mMode == 0) ? mActive : mTrigger;

    if (newState && !mWasActive)
    {
      // Auto mode: only reset envelope on first note (not legato)
      // Manual mode: always reset on trigger
      if (mMode == 1 || mAmp <= 0.f)
      {
        mAmp = 0.f;
        RecalcDelay();
      }
    }

    if (!newState && mWasActive && mMode == 1)
      mAmp = 0.f; // manual mode: cut immediately on release

    mWasActive = newState;

    mPos += mFreq;
    if (mPos >= 1.f)
      mPos -= 1.f;

    // RC exponential envelope: mAmp approaches 1.0 asymptotically
    if (newState && mAmp < 1.f)
    {
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

    return tri * mAmp;
  }

private:
  void RecalcDelay()
  {
    if (mDelayParam <= 0.f)
    {
      mDelayCoeff = 0.f; // instant (handled as special case)
    }
    else
    {
      // RC time constant: delay param 0-1 maps to 0–1.5s (per KR-106 spec)
      // Coefficient = 1 - exp(-1 / (tau * sampleRate))
      float tau   = mDelayParam * 1.5f;
      mDelayCoeff = 1.f - expf(-1.f / (tau * mSampleRate));
    }
  }
};

} // namespace kr106
