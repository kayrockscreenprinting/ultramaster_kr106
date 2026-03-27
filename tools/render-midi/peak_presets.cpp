// Peak volume analyzer — renders one note per preset, reports peak levels.
//
// Usage: peak_presets [note] [duration_sec] [samplerate]
//   Defaults: note=60 (C4), duration=3s, 44100 Hz
//
// Output: CSV to stdout (preset_index, name, peak, peak_dBFS)

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>

#include "../../Source/DSP/KR106_DSP.h"
#include "../../Source/DSP/KR106_DSP_SetParam.h"
#include "../../Source/KR106_Presets_JUCE.h"

enum EParams
{
  kBenderDco = 0, kBenderVcf, kArpRate, kLfoRate, kLfoDelay,
  kDcoLfo, kDcoPwm, kDcoSub, kDcoNoise, kHpfFreq,
  kVcfFreq, kVcfRes, kVcfEnv, kVcfLfo, kVcfKbd,
  kVcaLevel, kEnvA, kEnvD, kEnvS, kEnvR,
  kTranspose, kHold, kArpeggio, kDcoPulse, kDcoSaw, kDcoSubSw,
  kChorusOff, kChorusI, kChorusII,
  kOctTranspose, kArpMode, kArpRange, kLfoMode, kPwmMode,
  kVcfEnvInv, kVcaMode,
  kBender, kTuning, kPower,
  kPortaMode, kPortaRate,
  kTransposeOffset, kBenderLfo,
  kAdsrMode,
  kMasterVol,
  kNumParams
};

static void setParam(KR106DSP<float>& dsp, int param, float value)
{
  dsp.SetParam(param, static_cast<double>(value));
}

static void loadPreset(KR106DSP<float>& dsp, int presetIdx)
{
  const auto& p = kFactoryPresets[presetIdx];

  // Sliders (indices 0-19 + 40): raw 7-bit values, convert to 0-1
  for (int i = 0; i <= 19; i++)
    setParam(dsp, i, p.values[i] / 127.f);
  setParam(dsp, kPortaRate, p.values[40] / 127.f);

  // Switches (indices 20-39, 41-43): integer values
  for (int i = 20; i <= 39; i++)
    setParam(dsp, i, static_cast<float>(p.values[i]));
  for (int i = 41; i <= 43; i++)
    setParam(dsp, i, static_cast<float>(p.values[i]));
}

int main(int argc, char* argv[])
{
  int note = (argc > 1) ? atoi(argv[1]) : 60;
  float duration = (argc > 2) ? static_cast<float>(atof(argv[2])) : 3.f;
  float sr = (argc > 3) ? static_cast<float>(atof(argv[3])) : 44100.f;

  static constexpr int kBlockSize = 512;

  // Note-on duration and release tail
  int noteOnSamples = static_cast<int>(duration * sr);
  int tailSamples = static_cast<int>(2.f * sr); // 2s release tail
  int totalSamples = noteOnSamples + tailSamples;

  fprintf(stderr, "Rendering %d presets, note=%d, %.1fs + %.1fs tail, %.0f Hz\n",
          kNumFactoryPresets, note, duration, 2.f, sr);

  printf("index,name,peak,peak_dBFS\n");

  for (int pi = 0; pi < kNumFactoryPresets; pi++)
  {
    KR106DSP<float> dsp(6);
    dsp.Reset(sr, kBlockSize);

    // Defaults
    setParam(dsp, kPower, 1.f);
    setParam(dsp, kAdsrMode, 1.f);
    dsp.mMasterVol = 0.5f * 0.5f;
    setParam(dsp, kPortaMode, 2.f);

    // Load preset
    loadPreset(dsp, pi);

    // Allocate output
    std::vector<float> outL(kBlockSize, 0.f);
    std::vector<float> outR(kBlockSize, 0.f);

    float peak = 0.f;
    int rendered = 0;
    bool noteOff = false;

    while (rendered < totalSamples)
    {
      int blockSize = std::min(kBlockSize, totalSamples - rendered);

      // Note on at sample 0
      if (rendered == 0)
        dsp.NoteOn(note, 127);

      // Note off after duration
      if (!noteOff && rendered >= noteOnSamples)
      {
        dsp.NoteOff(note);
        noteOff = true;
      }

      std::fill(outL.begin(), outL.begin() + blockSize, 0.f);
      std::fill(outR.begin(), outR.begin() + blockSize, 0.f);

      float* outputs[2] = {outL.data(), outR.data()};
      dsp.ProcessBlock(nullptr, outputs, 2, blockSize);

      for (int i = 0; i < blockSize; i++)
      {
        peak = std::max(peak, fabsf(outL[i]));
        peak = std::max(peak, fabsf(outR[i]));
      }

      rendered += blockSize;
    }

    float dBFS = (peak > 1e-10f) ? 20.f * log10f(peak) : -200.f;
    printf("%d,\"%s\",%.6f,%.1f\n", pi, kFactoryPresets[pi].name, peak, dBFS);
    fprintf(stderr, "  [%3d] %-25s  peak=%.4f  %+.1f dBFS\n",
            pi, kFactoryPresets[pi].name, peak, dBFS);
  }

  return 0;
}
