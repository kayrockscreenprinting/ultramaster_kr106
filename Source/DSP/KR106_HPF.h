#pragma once

#include <cmath>

// ============================================================
// J60 HPF — 4-position switched (CD4051B selects capacitor)
//
// Circuit: signal -> 1M to GND (DC bias) -> C (switched) ->
//   470K to GND + 38K series -> 10µF NP -> uPC1252 VCA (30K input Z)
//
// R_eff = 470K || (38K + 30K_VCA) = 59.4K
// First-order HPF, -6 dB/oct slope.
//
// Frequencies derived from ngspice AC simulation with 30K VCA
// load (uPC1252H2 input impedance).
//
// Position 0: FLAT (bypass, no capacitor in path)
// Position 1: C=.022µF  -> fc = 122 Hz
// Position 2: C=.01µF   -> fc = 269 Hz
// Position 3: C=.0047µF -> fc = 571 Hz
// ============================================================
inline float getJuno60HPFFreq(int position)
{
  switch (position)
  {
    case 0:  return 0.f;     // FLAT (bypass)
    case 1:  return 122.f;   // .022µF, ngspice: 122 Hz
    case 2:  return 269.f;   // .01µF,  ngspice: 269 Hz
    case 3:  return 571.f;   // .0047µF, ngspice: 571 Hz
    default: return 0.f;
  }
}

// ============================================================
// J106 HPF — 4-position switched (CD4051B selects capacitor)
//
// Circuit: C (switched) -> 1M to GND -> 47K series ->
//   inverting op amp with 47K feedback (gain = -1)
//
// R_eff = 1M || 47K = 44.9K (op amp virtual ground loads 47K)
// First-order HPF, -6 dB/oct slope.
//
// Position 0: C=.0047µF -> fc = 754 Hz
// Position 1: C=.015µF  -> fc = 236 Hz
// Position 2: FLAT (bypass)
// Slider positions (UI order, bottom to top):
//   0: Bass boost (+9.4 dB low shelf)
//   1: Flat (bypass)
//   2: HPF ~236 Hz  (CD4051 pin 1: C=.015µF)
//   3: HPF ~754 Hz  (CD4051 pin 0: C=.0047µF)
// ============================================================
inline float getJuno106HPFFreq(int position)
{
  switch (position)
  {
    case 0:  return -1.f;    // Bass boost (negative signals HPF::Process to use biquad)
    case 1:  return 0.f;     // FLAT (bypass)
    case 2:  return 236.f;   // .015µF
    case 3:  return 754.f;   // .0047µF
    default: return 0.f;
  }
}

// ============================================================
// J6 HPF — continuous pot (measured PCHIP interpolation)
//
// 11-point curve measured from hardware Juno-6.
// Input: slider position 0–1
// Output: HPF cutoff frequency in Hz (38.6 – 1394.2 Hz)
//
// The J6 uses a continuous potentiometer to vary the HPF
// cutoff, unlike the J60/J106 which use switched capacitors.
// ============================================================
inline float getJuno6HPFFreqPCHIP(float x)
{
  static const float y[] = {
    38.6f, 83.5f, 181.3f, 394.7f, 418.4f,
    437.1f, 455.8f, 605.5f, 988.6f, 1183.2f, 1394.2f
  };
  static constexpr int N = 11;
  static constexpr float h = 0.1f;

  if (x <= 0.0f) return y[0];
  if (x >= 1.0f) return y[N - 1];

  float x_scaled = x * 10.0f;
  int i = (int)x_scaled;
  if (i >= N - 1) i = N - 2;
  float t = x_scaled - (float)i;

  auto get_slope = [&](int idx) -> float {
    if (idx <= 0 || idx >= N - 1) return 0.0f;
    float d_prev = (y[idx] - y[idx - 1]) / h;
    float d_next = (y[idx + 1] - y[idx]) / h;
    if (d_prev * d_next <= 0.0f) return 0.0f;
    return 2.0f / (1.0f / d_prev + 1.0f / d_next);
  };

  float m_i = get_slope(i);
  float m_next = get_slope(i + 1);

  float t2 = t * t;
  float t3 = t2 * t;
  float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
  float h10 = t3 - 2.0f * t2 + t;
  float h01 = -2.0f * t3 + 3.0f * t2;
  float h11 = t3 - t2;

  return h00 * y[i] + h10 * h * m_i + h01 * y[i + 1] + h11 * h * m_next;
}

// Juno-106 HPF position 0: Bass Boost
  //
  // Circuit: Two op amp stages (M5218L) with frequency-dependent
  // feedback. U2: non-inverting amp with C3(.022µF)||R(1M) feedback
  // over R46(10K). U1: inverting summer mixing the boost signal
  // (R44=220K) with the direct path (R43=47K), R45(47K) feedback.
  //
  // Produces a low-frequency shelf boost:
  //   +9.2 dB below 50 Hz
  //   +4.8 dB at 100 Hz
  //   +2.2 dB at 200 Hz
  //   Flat above 500 Hz
  //
  // Modeled as a low-shelf biquad (Audio EQ Cookbook).
  // Derived from ngspice AC simulation. RMS error 0.14 dB.
  //
  // Parameters: fc=103 Hz, gain=9.4 dB, Q=0.470
  struct BassBoostFilter
  {
    static constexpr float kShelfFc   = 103.1f;   // Hz
    static constexpr float kShelfGain = 9.37f;     // dB
    static constexpr float kShelfQ    = 0.4695f;

    float b0 = 1.f, b1 = 0.f, b2 = 0.f;
    float a1 = 0.f, a2 = 0.f;
    float z1 = 0.f, z2 = 0.f;

    void Init(float sampleRate)
    {
      // Audio EQ Cookbook low-shelf biquad
      float A = powf(10.f, kShelfGain / 40.f);
      float w0 = 2.f * static_cast<float>(M_PI) * kShelfFc / sampleRate;
      float cosw = cosf(w0);
      float sinw = sinf(w0);
      float alpha = sinw / (2.f * kShelfQ);
      float sqrtA2alpha = 2.f * sqrtf(A) * alpha;

      float a0_inv = 1.f / ((A + 1.f) + (A - 1.f) * cosw + sqrtA2alpha);

      b0 = A * ((A + 1.f) - (A - 1.f) * cosw + sqrtA2alpha) * a0_inv;
      b1 = 2.f * A * ((A - 1.f) - (A + 1.f) * cosw) * a0_inv;
      b2 = A * ((A + 1.f) - (A - 1.f) * cosw - sqrtA2alpha) * a0_inv;
      a1 = -2.f * ((A - 1.f) + (A + 1.f) * cosw) * a0_inv;
      a2 = ((A + 1.f) + (A - 1.f) * cosw - sqrtA2alpha) * a0_inv;

      Reset();
    }

    void Reset() { z1 = z2 = 0.f; }

    float Process(float x)
    {
      float y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };