#pragma once

#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <bitset>
#include <memory>
#include <cstdint>

#include "KR106Voice.h"
#include "KR106LFO.h"
#include "KR106Arpeggiator.h"
#include "KR106Chorus.h"

// Top-level KR-106 DSP orchestrator

// Modulation bus indices (passed as inputs[] to voices)
enum EModulations
{
  kModLFO = 0,
  kNumModulations
};

// ============================================================
// RBJ Biquad HPF
// ============================================================
namespace kr106 {

// KR-106 4-position HPF switch (replicates the 4052 dual-switch network).
//
// Mode 0 (slider bottom): Low-shelf bass boost  ~+10 dB below ~150 Hz
// Mode 1:                 Flat / bypass
// Mode 2:                 1-pole HPF at ~240 Hz
// Mode 3 (slider top):    1-pole HPF at ~720 Hz
struct HPF
{
  static constexpr float kShelfFreqHz  = 150.f;
  static constexpr float kShelfGainLin = 3.162f; // ~+10 dB
  static constexpr float kHPFFreqs[4] = { 0.f, 0.f, 240.f, 720.f };

  int   mMode = 1;
  float mSampleRate = 44100.f;
  float mG   = 0.f;
  float mHpS = 0.f;
  float mLpS = 0.f;

  void Init()       { mHpS = 0.f; mLpS = 0.f; }
  void SetSampleRate(float sr) { mSampleRate = sr; Recalc(); }

  void SetMode(int mode)
  {
    int newMode = std::clamp(mode, 0, 3);
    if (newMode == mMode) return;
    mMode = newMode;
    Recalc();
  }

  void Recalc()
  {
    if (mMode == 1) { mG = 0.f; return; }
    float fc = (mMode == 0) ? kShelfFreqHz : kHPFFreqs[mMode];
    float frq = std::clamp(fc / (mSampleRate * 0.5f), 0.001f, 0.9f);
    mG = tanf(frq * static_cast<float>(M_PI) * 0.5f);
  }

  float Process(float input)
  {
    if (mMode == 1) return input;
    if (mMode == 0)
    {
      float v = (input - mLpS) * mG / (1.f + mG);
      float lp = mLpS + v;
      mLpS = lp + v;
      return input + (kShelfGainLin - 1.f) * lp;
    }
    float v = (input - mHpS) * mG / (1.f + mG);
    float lp = mHpS + v;
    mHpS = lp + v;
    return input - lp;
  }
};

} // namespace kr106

// ============================================================
// KR106DSP — top-level orchestrator
// ============================================================
template <typename T>
class KR106DSP
{
public:
  KR106DSP(int nVoices)
  {
    for (int i = 0; i < nVoices; i++)
    {
      auto voice = std::make_unique<kr106::Voice<T>>();
      voice->InitVariance(i);
      mVoices.push_back(std::move(voice));
    }
    mLFOBuffer.reserve(4096);
    mSyncBuffer.reserve(4096);
    mModulations.resize(kNumModulations, nullptr);
  }

  // --- Voice container helpers ---
  size_t NVoices() const { return mVoices.size(); }
  kr106::Voice<T>* GetVoice(int i) { return mVoices[i].get(); }

  template <typename F>
  void ForEachVoice(F func)
  {
    for (auto& v : mVoices) func(*v);
  }

  static double MidiToPitch(int note) { return (note - 69) / 12.0; }

  void TriggerUnisonVoices(int note, int velocity)
  {
    double pitch = MidiToPitch(note);
    float vel = velocity / 127.f;
    bool anyBusy = false;
    ForEachVoice([&](kr106::Voice<T>& v) { anyBusy |= v.GetBusy(); });
    ForEachVoice([pitch, vel, anyBusy](kr106::Voice<T>& v) {
      v.SetUnisonPitch(pitch);
      v.Trigger(vel, anyBusy);
    });
  }

  void ReleaseUnisonVoices()
  {
    ForEachVoice([](kr106::Voice<T>& v) { v.Release(); });
  }

  int FindLowestFreeVoice()
  {
    int nv = static_cast<int>(NVoices());
    for (int i = 0; i < nv; i++)
      if (mVoiceNote[i] < 0 && mVoices[i]->GetBusy()) return i;
    for (int i = 0; i < nv; i++)
      if (!mVoices[i]->GetBusy()) return i;
    int oldest = 0;
    int64_t oldestAge = mVoiceAge[0];
    for (int i = 1; i < nv; i++)
      if (mVoiceAge[i] < oldestAge) { oldestAge = mVoiceAge[i]; oldest = i; }
    return oldest;
  }

  int FindRoundRobinVoice()
  {
    int nv = static_cast<int>(NVoices());
    for (int j = 0; j < nv; j++)
    {
      int i = (mRoundRobinNext + j) % nv;
      if (!mVoices[i]->GetBusy()) { mRoundRobinNext = (i + 1) % nv; return i; }
    }
    for (int j = 0; j < nv; j++)
    {
      int i = (mRoundRobinNext + j) % nv;
      if (mVoiceNote[i] < 0) { mRoundRobinNext = (i + 1) % nv; return i; }
    }
    int oldest = 0;
    int64_t oldestAge = mVoiceAge[0];
    for (int i = 1; i < nv; i++)
      if (mVoiceAge[i] < oldestAge) { oldestAge = mVoiceAge[i]; oldest = i; }
    mRoundRobinNext = (oldest + 1) % nv;
    return oldest;
  }

  void TriggerVoice(int voiceIdx, int note, int velocity)
  {
    auto& v = *mVoices[voiceIdx];
    v.SetUnisonPitch(MidiToPitch(note));
    v.Trigger(velocity / 127.f, v.GetBusy());
    mVoiceNote[voiceIdx] = note;
    mVoiceAge[voiceIdx] = ++mVoiceAgeCounter;
  }

  void SendToSynth(int note, bool noteOn, int velocity, int offset = 0)
  {
    (void)offset;
    if (mPortaMode == 0)
    {
      if (noteOn)
      {
        auto it = std::find(mUnisonStack.begin(), mUnisonStack.end(), note);
        if (it != mUnisonStack.end()) mUnisonStack.erase(it);
        mUnisonStack.push_back(note);
        if (mUnisonNote >= 0) ReleaseUnisonVoices();
        mUnisonNote = note;
        TriggerUnisonVoices(note, velocity);
      }
      else
      {
        auto it = std::find(mUnisonStack.begin(), mUnisonStack.end(), note);
        if (it != mUnisonStack.end()) mUnisonStack.erase(it);
        if (note == mUnisonNote)
        {
          if (!mUnisonStack.empty())
          { mUnisonNote = mUnisonStack.back(); TriggerUnisonVoices(mUnisonNote, 127); }
          else
          { ReleaseUnisonVoices(); mUnisonNote = -1; }
        }
      }
    }
    else if (mPortaMode == 1)
    {
      if (noteOn)
      {
        int vi = FindLowestFreeVoice();
        if (mVoiceNote[vi] >= 0) mVoiceNote[vi] = -1;
        TriggerVoice(vi, note, velocity);
      }
      else
      {
        int nv = static_cast<int>(NVoices());
        for (int i = 0; i < nv; i++)
          if (mVoiceNote[i] == note) { mVoices[i]->Release(); mVoiceNote[i] = -1; break; }
      }
    }
    else
    {
      if (noteOn)
      {
        int vi = FindRoundRobinVoice();
        if (mVoiceNote[vi] >= 0) mVoiceNote[vi] = -1;
        TriggerVoice(vi, note, velocity);
      }
      else
      {
        int nv = static_cast<int>(NVoices());
        for (int i = 0; i < nv; i++)
          if (mVoiceNote[i] == note) { mVoices[i]->Release(); mVoiceNote[i] = -1; break; }
      }
    }
  }

  void ProcessBlock(T** inputs, T** outputs, int nOutputs, int nFrames)
  {
    (void)inputs;
    for (int c = 0; c < nOutputs; c++)
      memset(outputs[c], 0, nFrames * sizeof(T));

    if (static_cast<int>(mLFOBuffer.size()) < nFrames)
    {
      mLFOBuffer.resize(nFrames, T(0));
      mSyncBuffer.resize(nFrames, T(0));
      mModulations.resize(kNumModulations, nullptr);
    }

    memset(mSyncBuffer.data(), 0, nFrames * sizeof(T));
    int nv = static_cast<int>(NVoices());
    int scopeVoice = -1;
    int64_t oldestAge = INT64_MAX;
    for (int i = 0; i < nv; i++)
      if (mVoices[i]->GetBusy() && mVoiceAge[i] < oldestAge) { oldestAge = mVoiceAge[i]; scopeVoice = i; }
    for (int i = 0; i < nv; i++)
      mVoices[i]->mSyncOut = (i == scopeVoice) ? mSyncBuffer.data() : nullptr;

    bool anyActive = false;
    ForEachVoice([&](kr106::Voice<T>& v) { anyActive |= v.GetBusy(); });
    mLFO.SetVoiceActive(anyActive);

    for (int s = 0; s < nFrames; s++)
      mLFOBuffer[s] = static_cast<T>(mLFO.Process());

    mModulations[kModLFO] = mLFOBuffer.data();

    mArp.Process(nFrames,
      [this](int note, int offset) { SendToSynth(note, true,  127, offset); },
      [this](int note, int offset) { SendToSynth(note, false, 0,   offset); });

    for (auto& v : mVoices)
      if (v->GetBusy())
        v->ProcessSamplesAccumulating(mModulations.data(), outputs, kNumModulations, nOutputs, 0, nFrames);

    for (int s = 0; s < nFrames; s++)
      outputs[0][s] = static_cast<T>(mHPF.Process(static_cast<float>(outputs[0][s])));

    if (mChorus.mMode == 0)
    {
      if (nOutputs > 1)
        memcpy(outputs[1], outputs[0], nFrames * sizeof(T));
    }
    else
    {
      for (int s = 0; s < nFrames; s++)
      {
        float mono = static_cast<float>(outputs[0][s]);
        float L, R;
        mChorus.Process(mono, L, R);
        outputs[0][s] = static_cast<T>(L);
        if (nOutputs > 1) outputs[1][s] = static_cast<T>(R);
      }
    }

    for (int s = 0; s < nFrames; s++)
    {
      outputs[0][s] *= static_cast<T>(mVcaLevel);
      if (nOutputs > 1) outputs[1][s] *= static_cast<T>(mVcaLevel);
    }
  }

  T* GetSyncBuffer() { return mSyncBuffer.data(); }

  void Reset(double sampleRate, int blockSize)
  {
    mSampleRate = static_cast<float>(sampleRate);
    ForEachVoice([sampleRate, blockSize](kr106::Voice<T>& v) {
      v.SetSampleRateAndBlockSize(sampleRate, blockSize);
    });
    mHPF.SetSampleRate(mSampleRate);
    mHPF.Init();
    mHPF.SetMode(1);
    mChorus.Init(mSampleRate);
    mArp.SetSampleRate(mSampleRate);
    mLFOBuffer.resize(blockSize);
    mSyncBuffer.resize(blockSize);
    mModulations.resize(kNumModulations);
  }

  void NoteOn(int note, int velocity)
  {
    mKeysDown.set(note);
    if (mArp.mEnabled) { mArp.NoteOn(note); return; }
    SendToSynth(note, true, velocity, 0);
  }

  void NoteOff(int note)
  {
    mKeysDown.reset(note);
    if (mArp.mEnabled)
    {
      if (mHold) mHeldNotes.set(note);
      else mArp.NoteOff(note);
      return;
    }
    if (mHold) { mHeldNotes.set(note); return; }
    SendToSynth(note, false, 0, 0);
  }

  void ControlChange(int cc, float value)
  {
    if (cc == 1) { mLFO.SetTrigger(value > 0.0f); return; }
  }

  // ADSR lookup tables
  static constexpr float kAttackLUT[128] = {
    1.5f, 1.5f, 1.5f, 1.6f, 1.7f, 1.8f, 1.9f, 2.1f,
    2.2f, 2.5f, 2.7f, 3.0f, 3.3f, 3.6f, 3.9f, 4.3f,
    5.4f, 6.7f, 8.2f, 9.7f, 11.2f, 12.4f, 13.3f, 13.7f,
    14.6f, 16.2f, 18.4f, 21.1f, 24.0f, 27.1f, 30.6f, 34.8f,
    38.7f, 42.2f, 45.4f, 50.4f, 56.5f, 63.2f, 70.0f, 76.5f,
    82.0f, 86.9f, 91.9f, 97.2f, 104.2f, 112.5f, 121.7f, 131.2f,
    140.6f, 149.9f, 159.6f, 170.0f, 181.5f, 193.8f, 206.7f, 220.2f,
    234.4f, 249.1f, 264.3f, 280.1f, 296.3f, 313.1f, 331.5f, 351.3f,
    371.9f, 392.8f, 413.2f, 432.8f, 450.6f, 466.6f, 484.3f, 506.7f,
    542.7f, 580.8f, 595.2f, 606.4f, 625.5f, 650.7f, 681.1f, 715.7f,
    753.8f, 794.2f, 836.2f, 878.8f, 921.1f, 962.1f, 1001.1f, 1037.0f,
    1068.8f, 1098.4f, 1132.7f, 1175.6f, 1226.1f, 1281.4f, 1338.3f, 1395.4f,
    1453.0f, 1510.9f, 1569.2f, 1627.8f, 1687.0f, 1746.5f, 1806.5f, 1867.0f,
    1927.9f, 1989.4f, 2051.4f, 2113.9f, 2176.9f, 2240.5f, 2304.7f, 2369.5f,
    2434.9f, 2500.9f, 2567.6f, 2634.9f, 2702.9f, 2771.6f, 2841.0f, 2911.1f,
    2982.0f, 3053.6f, 3126.0f, 3199.2f, 3273.1f, 3347.9f, 3423.5f, 3500.0f
  };
  static constexpr float kDecayLUT[128] = {
    1.5f, 4.9f, 8.1f, 11.2f, 14.3f, 17.2f, 20.1f, 22.8f,
    25.5f, 28.0f, 30.5f, 32.9f, 35.3f, 37.5f, 39.7f, 41.8f,
    43.9f, 45.9f, 47.8f, 49.5f, 52.4f, 69.0f, 73.5f, 82.0f,
    95.1f, 110.8f, 127.9f, 150.8f, 177.1f, 201.9f, 220.7f, 229.3f,
    259.9f, 293.9f, 303.8f, 359.7f, 407.5f, 454.3f, 500.1f, 545.2f,
    589.8f, 634.1f, 678.3f, 722.6f, 767.2f, 811.0f, 854.5f, 905.2f,
    965.9f, 1031.2f, 1098.1f, 1170.7f, 1257.6f, 1356.1f, 1440.9f, 1510.4f,
    1586.3f, 1683.8f, 1779.8f, 1878.6f, 1977.8f, 2084.6f, 2207.5f, 2365.0f,
    2537.8f, 2694.1f, 2808.3f, 2904.8f, 3030.0f, 3195.9f, 3383.0f, 3571.1f,
    3754.5f, 3945.1f, 4106.4f, 4255.3f, 4410.2f, 4613.3f, 4951.5f, 5140.2f,
    5290.1f, 5481.2f, 5700.7f, 5938.5f, 6188.4f, 6444.3f, 6700.1f, 6949.6f,
    7183.6f, 7456.1f, 7817.8f, 8204.4f, 8545.2f, 8802.6f, 9039.1f, 9319.4f,
    9634.1f, 9968.0f, 10309.6f, 10691.0f, 11043.5f, 11267.2f, 11525.1f, 11810.4f,
    12110.7f, 12426.3f, 12757.7f, 13105.1f, 13469.0f, 13849.7f, 14247.6f, 14663.1f,
    15096.5f, 15548.2f, 16018.5f, 16507.9f, 17016.7f, 17545.3f, 18094.0f, 18663.2f,
    19253.3f, 19864.7f, 20497.7f, 21152.7f, 21830.1f, 22530.2f, 23253.3f, 24000.0f
  };
  static constexpr float kReleaseLUT[128] = {
    1.5f, 1.6f, 2.0f, 2.5f, 3.3f, 4.3f, 5.5f, 6.9f,
    8.5f, 10.2f, 12.2f, 14.3f, 16.6f, 19.0f, 21.6f, 24.4f,
    27.3f, 31.1f, 37.4f, 45.2f, 55.7f, 67.0f, 77.4f, 88.9f,
    105.5f, 122.5f, 136.9f, 153.6f, 178.5f, 205.6f, 225.3f, 231.4f,
    250.7f, 292.9f, 323.9f, 342.3f, 362.7f, 404.3f, 459.2f, 498.9f,
    538.7f, 586.2f, 603.8f, 649.8f, 718.2f, 788.4f, 842.6f, 888.3f,
    945.9f, 1030.6f, 1122.6f, 1206.2f, 1288.3f, 1369.4f, 1448.4f, 1533.7f,
    1645.0f, 1731.0f, 1862.4f, 1989.1f, 2109.9f, 2226.8f, 2339.7f, 2448.6f,
    2553.4f, 2648.5f, 2766.9f, 2885.0f, 3014.9f, 3181.4f, 3366.6f, 3554.0f,
    3755.0f, 3979.8f, 4195.6f, 4369.4f, 4468.6f, 4524.7f, 4636.6f, 4798.4f,
    5001.1f, 5235.3f, 5491.9f, 5761.6f, 6035.3f, 6303.7f, 6579.2f, 6880.9f,
    7202.2f, 7545.1f, 7898.1f, 8215.6f, 8515.8f, 8803.3f, 9082.8f, 9359.2f,
    9636.9f, 9920.8f, 10215.6f, 10525.8f, 10856.3f, 11205.3f, 11573.5f, 11982.6f,
    12466.3f, 13059.7f, 13608.7f, 14155.1f, 14698.6f, 15238.9f, 15775.5f, 16308.2f,
    16836.5f, 17360.2f, 17878.8f, 18392.0f, 18899.4f, 19400.8f, 19895.7f, 20383.7f,
    20864.6f, 21338.0f, 21803.4f, 22260.6f, 22709.3f, 23148.9f, 23579.3f, 24000.0f
  };
  static constexpr float kAttackLUT_J6[128] = {
    1.0f, 1.1f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f,
    1.7f, 1.8f, 1.9f, 2.0f, 2.1f, 2.3f, 2.4f, 2.6f,
    2.7f, 2.9f, 3.1f, 3.3f, 3.5f, 3.8f, 4.0f, 4.3f,
    4.5f, 4.8f, 5.2f, 5.5f, 5.8f, 6.2f, 6.6f, 7.1f,
    7.5f, 8.0f, 8.5f, 9.1f, 9.7f, 10.3f, 11.0f, 11.7f,
    12.4f, 13.3f, 14.1f, 15.0f, 16.0f, 17.1f, 18.2f, 19.4f,
    20.6f, 22.0f, 23.4f, 24.9f, 26.5f, 28.3f, 30.1f, 32.1f,
    34.1f, 36.4f, 38.7f, 41.2f, 43.9f, 46.8f, 49.8f, 53.1f,
    56.5f, 60.2f, 64.1f, 68.3f, 72.7f, 77.5f, 82.5f, 87.9f,
    93.6f, 99.7f, 106.2f, 113.1f, 120.4f, 128.3f, 136.6f, 145.5f,
    155.0f, 165.1f, 175.8f, 187.3f, 199.4f, 212.4f, 226.2f, 241.0f,
    256.7f, 273.4f, 291.1f, 310.1f, 330.3f, 351.8f, 374.6f, 399.0f,
    425.0f, 452.6f, 482.1f, 513.5f, 546.9f, 582.5f, 620.4f, 660.7f,
    703.7f, 749.5f, 798.3f, 850.2f, 905.6f, 964.5f, 1027.3f, 1094.1f,
    1165.3f, 1241.1f, 1321.9f, 1407.9f, 1499.5f, 1597.1f, 1701.0f, 1811.7f,
    1929.6f, 2055.2f, 2188.9f, 2331.3f, 2483.0f, 2644.6f, 2816.7f, 3000.0f
  };
  static constexpr float kDecayLUT_J6[128] = {
    2.0f, 2.1f, 2.3f, 2.5f, 2.6f, 2.8f, 3.0f, 3.2f,
    3.5f, 3.7f, 4.0f, 4.2f, 4.6f, 4.9f, 5.2f, 5.6f,
    6.0f, 6.4f, 6.9f, 7.3f, 7.9f, 8.4f, 9.0f, 9.7f,
    10.4f, 11.1f, 11.9f, 12.7f, 13.6f, 14.6f, 15.6f, 16.7f,
    17.9f, 19.2f, 20.5f, 22.0f, 23.6f, 25.2f, 27.0f, 28.9f,
    31.0f, 33.2f, 35.5f, 38.0f, 40.7f, 43.6f, 46.7f, 50.0f,
    53.6f, 57.4f, 61.4f, 65.8f, 70.5f, 75.5f, 80.8f, 86.5f,
    92.7f, 99.3f, 106.3f, 113.8f, 121.9f, 130.5f, 139.8f, 149.7f,
    160.3f, 171.7f, 183.9f, 196.9f, 210.9f, 225.8f, 241.8f, 259.0f,
    277.3f, 297.0f, 318.0f, 340.6f, 364.7f, 390.6f, 418.3f, 447.9f,
    479.7f, 513.7f, 550.1f, 589.1f, 630.9f, 675.6f, 723.5f, 774.8f,
    829.8f, 888.6f, 951.6f, 1019.1f, 1091.3f, 1168.7f, 1251.6f, 1340.3f,
    1435.3f, 1537.1f, 1646.1f, 1762.8f, 1887.8f, 2021.6f, 2165.0f, 2318.5f,
    2482.8f, 2658.9f, 2847.4f, 3049.3f, 3265.5f, 3497.0f, 3744.9f, 4010.5f,
    4294.8f, 4599.3f, 4925.4f, 5274.6f, 5648.6f, 6049.1f, 6478.0f, 6937.3f,
    7429.1f, 7955.8f, 8519.9f, 9124.0f, 9770.9f, 10463.6f, 11205.5f, 12000.0f
  };
  static constexpr float kReleaseLUT_J6[128] = {
    2.0f, 2.1f, 2.3f, 2.5f, 2.6f, 2.8f, 3.0f, 3.2f,
    3.5f, 3.7f, 4.0f, 4.2f, 4.6f, 4.9f, 5.2f, 5.6f,
    6.0f, 6.4f, 6.9f, 7.3f, 7.9f, 8.4f, 9.0f, 9.7f,
    10.4f, 11.1f, 11.9f, 12.7f, 13.6f, 14.6f, 15.6f, 16.7f,
    17.9f, 19.2f, 20.5f, 22.0f, 23.6f, 25.2f, 27.0f, 28.9f,
    31.0f, 33.2f, 35.5f, 38.0f, 40.7f, 43.6f, 46.7f, 50.0f,
    53.6f, 57.4f, 61.4f, 65.8f, 70.5f, 75.5f, 80.8f, 86.5f,
    92.7f, 99.3f, 106.3f, 113.8f, 121.9f, 130.5f, 139.8f, 149.7f,
    160.3f, 171.7f, 183.9f, 196.9f, 210.9f, 225.8f, 241.8f, 259.0f,
    277.3f, 297.0f, 318.0f, 340.6f, 364.7f, 390.6f, 418.3f, 447.9f,
    479.7f, 513.7f, 550.1f, 589.1f, 630.9f, 675.6f, 723.5f, 774.8f,
    829.8f, 888.6f, 951.6f, 1019.1f, 1091.3f, 1168.7f, 1251.6f, 1340.3f,
    1435.3f, 1537.1f, 1646.1f, 1762.8f, 1887.8f, 2021.6f, 2165.0f, 2318.5f,
    2482.8f, 2658.9f, 2847.4f, 3049.3f, 3265.5f, 3497.0f, 3744.9f, 4010.5f,
    4294.8f, 4599.3f, 4925.4f, 5274.6f, 5648.6f, 6049.1f, 6478.0f, 6937.3f,
    7429.1f, 7955.8f, 8519.9f, 9124.0f, 9770.9f, 10463.6f, 11205.5f, 12000.0f
  };

  static float LookupLUT(const float* lut, float s)
  {
    float pos = s * 127.0f;
    int idx = static_cast<int>(pos);
    if (idx >= 127) return lut[127];
    float frac = pos - static_cast<float>(idx);
    return lut[idx] + frac * (lut[idx + 1] - lut[idx]);
  }

  void SetParam(int paramIdx, double value);

public:
  std::vector<std::unique_ptr<kr106::Voice<T>>> mVoices;
  kr106::LFO mLFO;
  kr106::HPF mHPF;
  kr106::Chorus mChorus;
  kr106::Arpeggiator mArp;

  float mVcaLevel = 1.f;
  float mSampleRate = 44100.f;
  int mOctaveTranspose = 0;
  double mTuning = 0.;
  int mKeyTranspose = 0;
  bool mHold = false;
  bool mTranspose = false;
  int mPortaMode = 2;
  int mUnisonNote = -1;
  std::vector<int> mUnisonStack;
  int mVoiceNote[6] = {-1,-1,-1,-1,-1,-1};
  int64_t mVoiceAge[6] = {0,0,0,0,0,0};
  int64_t mVoiceAgeCounter = 0;
  int mRoundRobinNext = 0;
  bool mChorusI = false;
  bool mChorusII = false;
  int mAdsrMode = 0;
  float mSliderA = 0.25f;
  float mSliderD = 0.25f;
  float mSliderR = 0.25f;

  std::bitset<128> mHeldNotes;
  std::bitset<128> mKeysDown;
  bool mSuppressHoldRelease = false;
  std::vector<T> mLFOBuffer;
  std::vector<T> mSyncBuffer;
  std::vector<T*> mModulations;

  void SetKeyTranspose(int semitones)
  {
    if (semitones == mKeyTranspose) return;
    mKeyTranspose = semitones;
    float semi = mOctaveTranspose * 12.f + static_cast<float>(mTuning) + mKeyTranspose;
    ForEachVoice([semi](kr106::Voice<T>& v) { v.mOctTranspose = semi; });
  }

  void PowerOff()
  {
    mArp.Reset();
    mKeysDown.reset();
    mHeldNotes.reset();
    ForEachVoice([](kr106::Voice<T>& v) { v.Release(); });
    for (int i = 0; i < 6; i++) mVoiceNote[i] = -1;
    mUnisonNote = -1;
    mUnisonStack.clear();
  }

  void ForceRelease(int noteNum)
  {
    mHeldNotes.reset(noteNum);
    if (mArp.mEnabled)
      mArp.NoteOff(noteNum);
    else
      SendToSynth(noteNum, false, 0);
  }

private:
  void UpdateChorusMode()
  {
    int mode = 0;
    if (mChorusI) mode |= 1;
    if (mChorusII) mode |= 2;
    mChorus.SetMode(mode);
  }

  void ReleaseHeldNotes()
  {
    if (mPortaMode == 0)
    {
      for (int i = 0; i < 128; i++)
      {
        if (mHeldNotes.test(i))
        {
          auto it = std::find(mUnisonStack.begin(), mUnisonStack.end(), i);
          if (it != mUnisonStack.end()) mUnisonStack.erase(it);
        }
      }
      if (mUnisonNote >= 0 && mUnisonStack.empty())
      { ReleaseUnisonVoices(); mUnisonNote = -1; }
      else if (mUnisonNote >= 0 && !mHeldNotes.test(mUnisonNote))
      { /* Current note wasn't held — keep playing it */ }
      else if (!mUnisonStack.empty())
      { mUnisonNote = mUnisonStack.back(); TriggerUnisonVoices(mUnisonNote, 127); }
    }
    else
    {
      for (int i = 0; i < 128; i++)
      {
        if (mHeldNotes.test(i))
        {
          if (mArp.mEnabled) mArp.NoteOff(i);
          else SendToSynth(i, false, 0);
        }
      }
    }
    mHeldNotes.reset();
  }
};

// SetParam is defined out-of-line but still in the header (template)
#include "KR106_DSP_SetParam.h"
