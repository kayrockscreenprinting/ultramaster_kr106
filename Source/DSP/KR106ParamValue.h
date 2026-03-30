#pragma once

// Unified parameter value query API.
// Returns raw numeric values (Hz, ms, dB, semitones, BPM).
// String formatting is the UI layer's responsibility.
// Used by: PluginProcessor.cpp (JUCE tooltips), kr106_wasm.cpp (WASM API).

#include "KR106ADSR.h"
#include "KR106LFO.h"
#include "KR106Voice.h"
#include "KR106VcfFreqJ6.h"
#include "KR106VcfFreqJ106.h"
#include "KR106Arpeggiator.h"

namespace kr106
{

struct ParamValue
{
  // VCF cutoff frequency in Hz
  static float vcfFreqHz(float slider, bool j6)
  {
    if (j6) return j60_vcf_freq_from_slider(slider); // mode 0 is J60
    return dacToHz(static_cast<uint16_t>(slider * 0x3F80));
  }

  // LFO rate in Hz
  static float lfoRateHz(float slider, bool j6)
  {
    return j6 ? LFO::lfoFreqJ6(slider) : LFO::lfoFreqJ106(slider);
  }

  // LFO delay in ms
  static float lfoDelayMs(float slider)
  {
    return slider * 1500.f;
  }

  // DCO LFO depth in semitones
  static float dcoLfoSemitones(float slider, bool j6)
  {
    return j6 ? Voice<float>::dcoLfoDepth6(slider)
              : Voice<float>::dcoLfoDepth106(slider);
  }

  // VCF LFO depth in semitones
  static float vcfLfoSemitones(float slider, bool j6)
  {
    return j6 ? Voice<float>::vcfLfoDepth6(slider)
              : Voice<float>::vcfLfoDepth106(slider);
  }

  // VCA level in dB (-10 to +10)
  static float vcaLevelDb(float slider)
  {
    return -10.f + 20.f * slider;
  }

  // Master volume in dB (squared taper, -inf at 0)
  static float masterVolDb(float slider)
  {
    if (slider <= 0.f) return -200.f;
    return 20.f * std::log10(slider * slider);
  }

  // Tuning offset in cents (-100 to +100)
  static float tuningCents(float slider)
  {
    return slider * 100.f;
  }

  // Arpeggiator rate in BPM
  static float arpRateBpm(float slider)
  {
    return Arpeggiator::arpRate(slider);
  }

  // Attack time in ms
  static float attackMs(float slider, bool j6)
  {
    return j6 ? ADSR::AttackMsJ6(slider) : ADSR::AttackMs(slider);
  }

  // Decay or Release time in ms
  static float decRelMs(float slider, bool j6)
  {
    return j6 ? ADSR::DecRelMsJ6(slider) : ADSR::DecRelMs(slider);
  }

  // Portamento time in ms per octave (0 = off)
  static float portaMsPerOct(float slider)
  {
    float semiPerSec = Voice<float>::portaRate(slider);
    if (semiPerSec <= 0.f) return 0.f;
    return 12000.f / semiPerSec;
  }
};

} // namespace kr106
