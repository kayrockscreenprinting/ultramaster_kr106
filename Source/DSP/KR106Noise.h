#pragma once

#include <cmath>
#include <cstdint>

// Shared noise generator modeled on the Juno-6/106 noise source.
//
// Single 2SC945 NZ transistor in avalanche mode, shared across all voices
// via the mixer bus. White noise approximated by 4-sample CLT (sum of
// uniform randoms), then filtered:
//
//   2x LPF ~12.6 kHz  (C9/R65 BA662 feedback + OTA/transistor bandwidth)
//   HPF ~67 Hz         (C11/R67 + C10/R70 coupling, effective single pole)
//
// Calibrated against Juno-6 hardware recording at 96 kHz.
// Fit: 0.55 dB RMS error, 25 Hz-44 kHz.
//
// Uses EMA (exponential moving average) filters instead of TPT. Unlike
// the bilinear transform, EMA has no frequency compression at Nyquist,
// so the spectral shape is consistent across sample rates. A small
// correction to the LPF cutoff compensates for the EMA's HF excess:
//   fc_corrected = fc * (1 - 2.2 * (fc / sr)^2)
// This converges to the true analog value at high sample rates.

namespace kr106 {

struct Noise
{
  uint32_t mSeed = 22222;
  float mLP1State = 0.f;
  float mLP2State = 0.f;
  float mHPState = 0.f;
  float mLPA = 0.f;  // EMA coefficient for LP poles (exp decay)
  float mHPA = 0.f;  // EMA coefficient for HP

  void SetSampleRate(float sampleRate)
  {
    constexpr float kLPFreq = 12600.f;  // BA662 feedback + OTA bandwidth
    constexpr float kHPFreq = 67.f;     // C11/R67 + C10/R70 effective

    // Correct LPF cutoff for EMA's high-frequency excess
    float r = kLPFreq / sampleRate;
    float fcLP = kLPFreq * (1.f - 2.2f * r * r);

    mLPA = expf(-2.f * static_cast<float>(M_PI) * fcLP / sampleRate);
    mHPA = expf(-2.f * static_cast<float>(M_PI) * kHPFreq / sampleRate);
    mLP1State = 0.f;
    mLP2State = 0.f;
    mHPState = 0.f;
  }

  float Process()
  {
    // 4-sample CLT: sum of uniform randoms approximates Gaussian
    float g = 0.f;
    for (int j = 0; j < 4; j++)
    {
      mSeed = mSeed * 196314165u + 907633515u;
      g += 2.f * mSeed / static_cast<float>(0xFFFFFFFFu) - 1.f;
    }
    float white = g * 0.5f;

    // LPF pole 1 (C9/R65 BA662 feedback)
    mLP1State = mLPA * mLP1State + (1.f - mLPA) * white;

    // LPF pole 2 (BA662 OTA / TR15/TR14 bandwidth)
    mLP2State = mLPA * mLP2State + (1.f - mLPA) * mLP1State;

    // HPF ~67 Hz (C11/R67 + C10/R70 coupling)
    mHPState = mHPA * mHPState + (1.f - mHPA) * mLP2State;

    return mLP2State - mHPState;
  }
};

} // namespace kr106
