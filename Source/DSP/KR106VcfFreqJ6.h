#pragma once

#include <cmath>

// Juno-6 VCF frequency slider model (analog CV path).
//
// Derived from measurements on real hardware:
//   - 11 calibration points (slider 0–5) via 5-octave keyboard transpose
//   - 8 direct measurements (slider 5–8.5, self-oscillating filter)
//   - 6 repeated trials each at slider 4, 5, 6 to quantify repositioning noise
//   - 6-voice unison test confirming inter-filter spread is only ±17 cents
//
// Circuit (from Juno-6 service manual, Module Board schematic):
//   VR4: 50K(B) linear slider pot, 0V (bottom) to +15V (top)
//   Wiper → R100 (100K MF) → IC9 summing node (virtual ground at −11V)
//   IC9 is ½ TA75558S, configured as inverting summer feeding the IR3109
//   expo converter alongside KBD CV, ENV, LFO, bend, and pedal inputs.
//
// The apparent "reverse log" taper is NOT from the pot itself — it's
// resistive loading. At slider position x, the pot's two halves form
// a Thevenin source impedance R_th = x(1−x)·50K, which peaks at 12.5K
// at center travel. This sags the wiper voltage through the 100K series
// resistor, pulling the summing current ~14% below linear at midpoint.
//
// I(x) = (15·x + 11) / (50K·x·(1−x) + 100K)
//   x=0:   I = 110 µA (11V offset from −11V virtual ground)
//   x=0.5: I = 164 µA (−14% below linear midpoint of 185 µA)
//   x=1:   I = 260 µA
//
// The exponential converter maps this current to frequency:
//   log2(f) = −12.0674 + 116879.07 · I
// Calibrated so slider 5.5 produces 248 Hz self-oscillation, matching
// the Juno-6 service manual test procedure (VCF FREQ at midpoint).

namespace kr106
{

inline float j6_vcf_freq_from_slider(float s)
{
  float R_th = s * (1.f - s) * 50000.f;
  float I = (15.f * s + 11.f) / (R_th + 100000.f);
  float f = powf(2.f, -12.0674f + 116879.07f * I);

  // Same IR3109 expo converter saturation as J106 (shared hardware)
  static constexpr float kThresh = 20000.f;
  static constexpr float kCeil   = 40000.f;
  static constexpr float kRange  = kCeil - kThresh;
  if (f > kThresh)
    f = kThresh + kRange * tanhf((f - kThresh) / kRange);

  return f;
}

// Juno-60 VCF frequency slider model.
//
// Same IR3109 expo converter as J6, but different front end:
//   - 50KB linear pot, no 100K series resistor (no loading sag)
//   - KBD tracking referenced from C4 (261.6 Hz), not C1 (32.7 Hz)
//   - Calibration: slider 0.3 = 248 Hz (from J60 service manual)
//
// Simple exponential: f = 28 * exp(7.262 * slider)
//   slider 0 = 28 Hz, slider 0.3 = 248 Hz, slider 1 = ~40 kHz
// The narrower low-end range (28 Hz vs J6's 4 Hz) reflects the higher
// C4 reference point. Same IR3109 saturation clamp above 20 kHz.

inline float j60_vcf_freq_from_slider(float s)
{
  static constexpr float kBase = 28.f;
  static constexpr float kScale = 7.262f;
  float f = kBase * expf(kScale * s);

  // Same IR3109 expo converter saturation as J6/J106
  static constexpr float kThresh = 20000.f;
  static constexpr float kCeil   = 40000.f;
  static constexpr float kRange  = kCeil - kThresh;
  if (f > kThresh)
    f = kThresh + kRange * tanhf((f - kThresh) / kRange);

  return f;
}

} // namespace kr106
