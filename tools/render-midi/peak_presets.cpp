// Peak volume analyzer — renders each preset in J6 and J106 mode,
// with single note and 6-note drag tests. Reports peak levels.
//
// Usage: peak_presets [note] [duration_sec] [samplerate]
//   Defaults: note=60 (C4), duration=3s, 44100 Hz
//
// Output: CSV to stdout (mode, index, name, test, peak, peak_dBFS, clips)

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

  for (int i = 0; i <= 19; i++)
    setParam(dsp, i, p.values[i] / 127.f);
  setParam(dsp, kPortaRate, p.values[40] / 127.f);

  for (int i = 20; i <= 39; i++)
    setParam(dsp, i, static_cast<float>(p.values[i]));
  for (int i = 41; i <= 43; i++)
    setParam(dsp, i, static_cast<float>(p.values[i]));
}

struct TestResult
{
  float peak;
  float dBFS;
  bool clips;
};

// Render a preset and measure peak level.
// If dragNotes > 1, triggers notes in quick succession (50ms apart)
// to simulate dragging across the keyboard.
static TestResult renderPreset(int presetIdx, int baseNote, int dragNotes,
                               float duration, float sr, float adsrMode)
{
  static constexpr int kBlockSize = 512;

  KR106DSP<float> dsp(6);
  dsp.Reset(sr, kBlockSize);

  setParam(dsp, kPower, 1.f);
  setParam(dsp, kAdsrMode, adsrMode);
  dsp.mMasterVol = 0.5f * 0.5f;
  setParam(dsp, kPortaMode, 2.f);

  loadPreset(dsp, presetIdx);

  int noteOnSamples = static_cast<int>(duration * sr);
  int tailSamples = static_cast<int>(2.f * sr);
  int totalSamples = noteOnSamples + tailSamples;

  // Schedule note-on times: 50ms apart for drag simulation
  int dragInterval = static_cast<int>(0.05f * sr); // 50ms
  std::vector<int> noteOnTimes(dragNotes);
  std::vector<int> notes(dragNotes);
  std::vector<bool> noteOffSent(dragNotes, false);
  for (int n = 0; n < dragNotes; n++)
  {
    noteOnTimes[n] = n * dragInterval;
    notes[n] = baseNote + n * 2; // whole steps up
  }

  std::vector<float> outL(kBlockSize, 0.f);
  std::vector<float> outR(kBlockSize, 0.f);

  float peak = 0.f;
  int rendered = 0;

  while (rendered < totalSamples)
  {
    int blockSize = std::min(kBlockSize, totalSamples - rendered);
    int blockEnd = rendered + blockSize;

    // Trigger note-ons that fall in this block
    for (int n = 0; n < dragNotes; n++)
    {
      if (noteOnTimes[n] >= rendered && noteOnTimes[n] < blockEnd)
      {
        // Release previous note (drag behavior: release on next key)
        if (n > 0)
          dsp.NoteOff(notes[n - 1]);
        dsp.NoteOn(notes[n], 127);
      }
    }

    // Note off after hold duration (release last note)
    if (rendered < noteOnSamples && blockEnd >= noteOnSamples)
      dsp.NoteOff(notes[dragNotes - 1]);

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
  return {peak, dBFS, peak > 1.0f};
}

int main(int argc, char* argv[])
{
  int note = (argc > 1) ? atoi(argv[1]) : 60;
  float duration = (argc > 2) ? static_cast<float>(atof(argv[2])) : 3.f;
  float sr = (argc > 3) ? static_cast<float>(atof(argv[3])) : 44100.f;

  fprintf(stderr, "Rendering %d presets x 2 modes x 2 tests, note=%d, %.1fs + 2s tail, %.0f Hz\n",
          kNumFactoryPresets, note, duration, sr);

  printf("mode,index,name,test,peak,peak_dBFS,clips\n");

  int clipCount = 0;

  for (int mode = 0; mode < 2; mode++)
  {
    const char* modeName = (mode == 0) ? "J6" : "J106";
    float adsrMode = static_cast<float>(mode);
    int bankOffset = mode * 128;

    for (int pi = 0; pi < 128; pi++)
    {
      int absIdx = pi + bankOffset;
      const char* name = kFactoryPresets[absIdx].name;

      // Test 1: single note
      auto r1 = renderPreset(absIdx, note, 1, duration, sr, adsrMode);
      printf("%s,%d,\"%s\",single,%.6f,%.1f,%d\n",
             modeName, pi, name, r1.peak, r1.dBFS, r1.clips ? 1 : 0);
      if (r1.clips) clipCount++;

      // Test 2: 6-note drag
      auto r6 = renderPreset(absIdx, note, 6, duration, sr, adsrMode);
      printf("%s,%d,\"%s\",drag6,%.6f,%.1f,%d\n",
             modeName, pi, name, r6.peak, r6.dBFS, r6.clips ? 1 : 0);
      if (r6.clips) clipCount++;

      const char* flag1 = r1.clips ? " ** CLIP" : "";
      const char* flag6 = r6.clips ? " ** CLIP" : "";
      fprintf(stderr, "  %s [%3d] %-25s  1-note: %+6.1f dBFS%s  6-drag: %+6.1f dBFS%s\n",
              modeName, pi, name, r1.dBFS, flag1, r6.dBFS, flag6);
    }
  }

  fprintf(stderr, "\nDone. %d tests clipped out of %d total.\n",
          clipCount, kNumFactoryPresets * 2);

  return 0;
}
