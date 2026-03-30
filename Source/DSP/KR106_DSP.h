#pragma once

#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <bitset>
#include <memory>
#include <cstdint>
#include <climits>
#include <atomic>

#include "KR106Voice.h"
#include "KR106LFO.h"
#include "KR106Noise.h"
#include "KR106VcfFreqJ6.h"
#include "KR106Arpeggiator.h"
#include "KR106Chorus.h"
#include "KR106_HPF.h"

// Top-level KR-106 DSP orchestrator

// Modulation bus indices (passed as inputs[] to voices)
enum EModulations
{
  kModLFO = 0,
  kModLFORaw, // raw triangle waveform (before onset envelope), for J106 integer VCF
  kModNoise,  // shared noise source (single generator, like real hardware)
  kNumModulations
};

// ============================================================
// RBJ Biquad HPF
// ============================================================
namespace kr106 {

// KR-106 HPF — per-model 4-position switch.
//
// J6:   4 samples of continuous pot curve (38.6-1394 Hz, PCHIP)
// J60:  flat / 122 Hz / 269 Hz / 571 Hz (no bass boost)
// J106: bass boost (+9.4 dB shelf @ 103 Hz) / flat / 236 Hz / 754 Hz
//
// Frequencies from circuit analysis + ngspice simulation (see KR106_HPF.h).
struct HPF
{
  static constexpr float kDCBlockHz = 5.f;
  static constexpr int kXfadeSamples = 64; // ~1.5 ms at 44.1k
  // J6 PCHIP curve sampled at 4 switch positions (0/3, 1/3, 2/3, 3/3)
  static constexpr float kJ6Freqs[4] = { 38.6f, 260.f, 530.f, 1394.f };

  Model mModel = kJ106;
  int   mMode = 1;          // 0-3 = switch position
  float mFreqHz = 0.f;      // resolved HPF frequency for current mode (0 = flat, -1 = bass boost)
  float mSampleRate = 44100.f;
  float mG   = 0.f;
  float mHpS = 0.f;
  float mDcG = 0.f;  // DC blocker coefficient
  float mDcS = 0.f;  // DC blocker state
  BassBoostFilter mBassBoost;

  // Crossfade state for click-free mode switching
  float mPrevFreqHz = 0.f;  // previous mode's freq (for crossfade)
  float mPrevG = 0.f;       // previous mode's coefficient
  float mPrevHpS = 0.f;     // previous mode's HPF state
  float mPrevDcS = 0.f;     // previous mode's DC blocker state
  BassBoostFilter mPrevBassBoost;
  int   mXfadeCount = 0;    // samples remaining in crossfade (0 = inactive)

  void Init()
  {
    mHpS = 0.f;
    mDcS = 0.f;
    mXfadeCount = 0;
    mBassBoost.Init(mSampleRate);
    mPrevBassBoost.Init(mSampleRate);
  }

  void SetSampleRate(float sr)
  {
    mSampleRate = sr;
    mBassBoost.Init(mSampleRate);
    mPrevBassBoost.Init(mSampleRate);
    Recalc();
  }

  void SetMode(int mode)
  {
    int newMode = std::clamp(mode, 0, 3);
    float newFreqHz;
    if (mModel == kJ6)
      newFreqHz = kJ6Freqs[newMode];
    else if (mModel == kJ60)
      newFreqHz = getJuno60HPFFreq(newMode);
    else
      newFreqHz = getJuno106HPFFreq(newMode);

    if (newFreqHz != mFreqHz)
    {
      // Snapshot current state for crossfade
      mPrevFreqHz = mFreqHz;
      mPrevG = mG;
      mPrevHpS = mHpS;
      mPrevDcS = mDcS;
      mPrevBassBoost = mBassBoost;
      mXfadeCount = kXfadeSamples;
    }

    mMode = newMode;
    mFreqHz = newFreqHz;
    Recalc();
  }

  void Recalc()
  {
    // DC blocker (~5 Hz)
    float dcFrq = std::clamp(kDCBlockHz / (mSampleRate * 0.5f), 0.001f, 0.9f);
    mDcG = tanf(dcFrq * static_cast<float>(M_PI) * 0.5f);

    if (mFreqHz <= 0.f) { mG = 0.f; return; } // flat or bass boost
    float frq = std::clamp(mFreqHz / (mSampleRate * 0.5f), 0.001f, 0.9f);
    mG = tanf(frq * static_cast<float>(M_PI) * 0.5f);
  }

  // 1-pole DC blocker: subtract LP at ~5 Hz
  float DCBlock(float input, float& dcState)
  {
    float v = (input - dcState) * mDcG / (1.f + mDcG);
    float lp = dcState + v;
    dcState = lp + v;
    return input - lp;
  }

  float ProcessWith(float input, float freqHz, float g, float& hpState, float& dcState, BassBoostFilter& boost)
  {
    if (freqHz < 0.f) return boost.Process(input);
    if (freqHz == 0.f) return DCBlock(input, dcState);
    float v = (input - hpState) * g / (1.f + g);
    float lp = hpState + v;
    hpState = lp + v;
    return input - lp;
  }

  float Process(float input)
  {
    float out = ProcessWith(input, mFreqHz, mG, mHpS, mDcS, mBassBoost);
    if (mXfadeCount > 0)
    {
      float prev = ProcessWith(input, mPrevFreqHz, mPrevG, mPrevHpS, mPrevDcS, mPrevBassBoost);
      float t = static_cast<float>(mXfadeCount) / static_cast<float>(kXfadeSamples);
      out = out * (1.f - t) + prev * t;
      mXfadeCount--;
    }
    return out;
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
  static constexpr int kMaxVoices = 10;

  KR106DSP(int nVoices = kMaxVoices)
  {
    mActiveVoices = std::min(nVoices, kMaxVoices);
    for (int i = 0; i < kMaxVoices; i++)
    {
      auto voice = std::make_unique<kr106::Voice<T>>();
      voice->InitVariance(i);
      mVoices.push_back(std::move(voice));
    }
    std::fill(std::begin(mVoiceNote), std::end(mVoiceNote), -1);
    std::fill(std::begin(mVoiceAge), std::end(mVoiceAge), int64_t(0));
    mLFOBuffer.reserve(4096);
    mLFORawBuffer.reserve(4096);
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
    float vel = mIgnoreVelocity ? 1.f : velocity / 127.f;
    bool anyBusy = false;
    for (int i = 0; i < mActiveVoices; i++) anyBusy |= mVoices[i]->GetBusy();
    for (int i = 0; i < mActiveVoices; i++)
    {
      auto& v = *mVoices[i];
      v.mMidiNote = note;
      v.SetUnisonPitch(pitch);
      v.Trigger(vel, anyBusy);
    }
  }

  void ReleaseUnisonVoices()
  {
    // Release ALL voices — excess voices from a previous higher count
    // may still be sounding (unison doesn't use mVoiceNote to track).
    ForEachVoice([](kr106::Voice<T>& v) { v.Release(); });
  }

  void GlideUnisonVoices(int note)
  {
    double pitch = MidiToPitch(note);
    for (int i = 0; i < mActiveVoices; i++)
    {
      auto& v = *mVoices[i];
      v.mMidiNote = note;
      v.SetUnisonPitch(pitch);
    }
  }

  int FindLowestFreeVoice()
  {
    for (int i = 0; i < mActiveVoices; i++)
      if (mVoiceNote[i] < 0 && mVoices[i]->GetBusy()) return i;
    for (int i = 0; i < mActiveVoices; i++)
      if (!mVoices[i]->GetBusy()) return i;
    int oldest = 0;
    int64_t oldestAge = mVoiceAge[0];
    for (int i = 1; i < mActiveVoices; i++)
      if (mVoiceAge[i] < oldestAge) { oldestAge = mVoiceAge[i]; oldest = i; }
    return oldest;
  }

  int FindRoundRobinVoice()
  {
    for (int j = 0; j < mActiveVoices; j++)
    {
      int i = (mRoundRobinNext + j) % mActiveVoices;
      if (!mVoices[i]->GetBusy()) { mRoundRobinNext = (i + 1) % mActiveVoices; return i; }
    }
    for (int j = 0; j < mActiveVoices; j++)
    {
      int i = (mRoundRobinNext + j) % mActiveVoices;
      if (mVoiceNote[i] < 0) { mRoundRobinNext = (i + 1) % mActiveVoices; return i; }
    }
    int oldest = 0;
    int64_t oldestAge = mVoiceAge[0];
    for (int i = 1; i < mActiveVoices; i++)
      if (mVoiceAge[i] < oldestAge) { oldestAge = mVoiceAge[i]; oldest = i; }
    mRoundRobinNext = (oldest + 1) % mActiveVoices;
    return oldest;
  }

  void TriggerVoice(int voiceIdx, int note, int velocity)
  {
    auto& v = *mVoices[voiceIdx];
    v.mMidiNote = note;
    v.SetUnisonPitch(MidiToPitch(note));
    v.Trigger(mIgnoreVelocity ? 1.f : velocity / 127.f, v.GetBusy());
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
        bool wasPlaying = (mUnisonNote >= 0);
        mUnisonNote = note;
        if (wasPlaying && !mMonoRetrigger)
          GlideUnisonVoices(note);
        else
          TriggerUnisonVoices(note, velocity);
      }
      else
      {
        auto it = std::find(mUnisonStack.begin(), mUnisonStack.end(), note);
        if (it != mUnisonStack.end()) mUnisonStack.erase(it);
        if (note == mUnisonNote)
        {
          if (!mUnisonStack.empty())
          { mUnisonNote = mUnisonStack.back(); GlideUnisonVoices(mUnisonNote); }
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
          if (mVoiceNote[i] == note) { mVoices[i]->Release(); mVoiceNote[i] = -1; }
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
          if (mVoiceNote[i] == note) { mVoices[i]->Release(); mVoiceNote[i] = -1; }
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
      mLFORawBuffer.resize(nFrames, T(0));
      mNoiseBuffer.resize(nFrames, T(0));
      mSyncBuffer.resize(nFrames, T(0));
      mModulations.resize(kNumModulations, nullptr);
    }

    memset(mSyncBuffer.data(), 0, nFrames * sizeof(T));
    int nv = static_cast<int>(NVoices());
    int scopeVoice = -1;
    int64_t oldestAge = INT64_MAX;
    for (int i = 0; i < nv; i++)
      if (mVoices[i]->GetBusy() && mVoiceAge[i] < oldestAge) { oldestAge = mVoiceAge[i]; scopeVoice = i; }

    bool anyActive = false;
    ForEachVoice([&](kr106::Voice<T>& v) { anyActive |= v.GetBusy(); });
    mLFO.SetVoiceActive(anyActive);

    for (int s = 0; s < nFrames; s++)
    {
      mLFOBuffer[s] = static_cast<T>(mLFO.Process());
      mLFORawBuffer[s] = static_cast<T>(mLFO.mLastTri);

      mNoiseBuffer[s] = static_cast<T>(mNoise.Process());
    }

    mModulations[kModLFO]    = mLFOBuffer.data();
    mModulations[kModLFORaw] = mLFORawBuffer.data();
    mModulations[kModNoise]  = mNoiseBuffer.data();

    mArp.Process(nFrames,
      [this](int note, int offset) { SendToSynth(note, true,  127, offset); },
      [this](int note, int offset) { SendToSynth(note, false, 0,   offset); });

    for (auto& v : mVoices)
    {
      if (!v->GetBusy()) continue;
      v->mLfoEnvAmp = mLFO.mAmp; // pass LFO onset envelope for J106 integer VCF
      v->ProcessSamplesAccumulating(mModulations.data(), outputs, kNumModulations, nOutputs, 0, nFrames);
    }

    // Scope sync: single accumulator running at the oldest voice's base
    // pitch (without LFO). Pulses every 2 cycles (sub-oscillator period)
    // so the display shows one full sub cycle. For very low frequencies
    // where 2 cycles won't fit in the scope ring buffer, pulse every cycle.
    if (scopeVoice >= 0)
    {
      float syncCps = mVoices[scopeVoice]->GetScopeSyncCPS();
      // 2 cycles fit if period*2 < ring size (with margin for the scope
      // to find both pulses within its available samples).
      // Scope ring is 4096 samples; need two sync pulses to fit with margin.
      bool useSub = syncCps > 0.f && (2.f / syncCps) < 3200.f;
      for (int s = 0; s < nFrames; s++)
      {
        mScopeSyncPhase += syncCps;
        if (mScopeSyncPhase >= 1.f)
        {
          mScopeSyncPhase -= 1.f;
          if (useSub)
          {
            mScopeSyncSub = !mScopeSyncSub;
            if (mScopeSyncSub)
              mSyncBuffer[s] = T(1);
          }
          else
          {
            mSyncBuffer[s] = T(1);
          }
        }
      }
    }

    for (int s = 0; s < nFrames; s++)
      outputs[0][s] = static_cast<T>(mHPF.Process(static_cast<float>(outputs[0][s])));

    // VCA level + master volume before chorus — matches hardware signal chain
    // (JU-106 Service Notes: VCA IC5 → Chorus BBD).
    for (int s = 0; s < nFrames; s++)
      outputs[0][s] *= static_cast<T>(mVcaLevel * mMasterVol);

    // Always call Chorus::Process — even when bypassed it keeps the
    // delay lines, filter state, and LFO warm for click-free engagement.
    for (int s = 0; s < nFrames; s++)
    {
      float mono = static_cast<float>(outputs[0][s]);
      float L, R;
      mChorus.Process(mono, L, R);
      outputs[0][s] = static_cast<T>(L);
      if (nOutputs > 1) outputs[1][s] = static_cast<T>(R);
    }
  }

  T* GetSyncBuffer() { return mSyncBuffer.data(); }

  void Reset(double sampleRate, int blockSize)
  {
    mSampleRate = static_cast<float>(sampleRate);
    mSawTables.Init(mSampleRate);
    ForEachVoice([this, sampleRate, blockSize](kr106::Voice<T>& v) {
      v.SetSampleRateAndBlockSize(sampleRate, blockSize);
      v.mOsc.SetTables(&mSawTables);
    });
    // Clear voice allocation so prepareToPlay re-trigger starts clean
    std::fill(std::begin(mVoiceNote), std::end(mVoiceNote), -1);
    mUnisonNote = -1;
    mUnisonStack.clear();
    mRoundRobinNext = 0;
    mHPF.mModel = mSynthModel;
    mHPF.SetSampleRate(mSampleRate);
    mHPF.Init();
    mHPF.SetMode(1);
    mChorus.Init(mSampleRate);
    mArp.SetSampleRate(mSampleRate);
    mNoise.SetSampleRate(mSampleRate);
    mLFOBuffer.resize(blockSize);
    mLFORawBuffer.resize(blockSize);
    mNoiseBuffer.resize(blockSize);
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
    if (cc == 1) {
      bool on = value > 0.0f;
      mLFO.SetTrigger(on);
      float mod = on ? 1.f : 0.f;
      ForEachVoice([mod](kr106::Voice<T>& v) { v.mBenderModAmt = mod; });
      return;
    }
  }

  void SetParam(int paramIdx, double value);

public:
  std::vector<std::unique_ptr<kr106::Voice<T>>> mVoices;
  kr106::SawTables mSawTables;
  kr106::LFO mLFO;
  kr106::HPF mHPF;
  kr106::Chorus mChorus;
  kr106::Arpeggiator mArp;

  float mVcaLevel = 1.f;
  float mMasterVol = 1.f;
  float mSampleRate = 44100.f;
  int mOctaveTranspose = 0;
  double mTuning = 0.;
  int mKeyTranspose = 0;
  bool mHold = false;
  bool mTranspose = false;
  int mPortaMode = 2;
  int mUnisonNote = -1;
  std::vector<int> mUnisonStack;
  int mActiveVoices = 6;
  bool mIgnoreVelocity = true;
  bool mMonoRetrigger = true;
  int mVoiceNote[kMaxVoices] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
  int64_t mVoiceAge[kMaxVoices] = {};
  int64_t mVoiceAgeCounter = 0;
  int mRoundRobinNext = 0;
  bool mChorusI = false;
  bool mChorusII = false;
  kr106::Model mSynthModel = kr106::kJ106;
  float mSliderA = 0.25f;
  float mSliderD = 0.25f;
  float mSliderR = 0.25f;
  float mSliderLfoRate = 0.24f;
  float mSliderArpRate = 0.06f;
  float mSliderDcoLfo = 0.f;
  float mSliderVcfLfo = 0.f;
  float mSliderLfoDelay = 0.f;
  float mSliderVcfFreq = 0.5f;
  float mSliderVcfEnv = 0.f;
  float mSliderVcfKbd = 0.f;
  float mSliderBenderVcf = 0.f;
  float mSliderDcoSub = 1.f;
  float mSliderDcoNoise = 0.f;
  float mSliderVcaLevel = 0.5f;
  float mSliderHpf = 1.f; // default = Flat (mode 1)

  std::bitset<128> mHeldNotes;
  std::bitset<128> mKeysDown;
  bool mSuppressHoldRelease = false;
  std::vector<T> mLFOBuffer;
  std::vector<T> mLFORawBuffer; // raw triangle (before onset envelope)
  std::vector<T> mNoiseBuffer;  // shared noise source (single generator for all voices)
  kr106::Noise mNoise;
  std::vector<T> mSyncBuffer;
  float mScopeSyncPhase = 0.f;
  bool mScopeSyncSub = false;
  std::vector<T*> mModulations;

  void SetKeyTranspose(int semitones)
  {
    if (semitones == mKeyTranspose) return;
    mKeyTranspose = semitones;
    float semi = mOctaveTranspose * 12.f + static_cast<float>(mTuning) + mKeyTranspose;
    ForEachVoice([semi](kr106::Voice<T>& v) { v.mOctTranspose = semi; });
  }

  void AllNotesOff()
  {
    mArp.Reset();
    mKeysDown.reset();
    mHeldNotes.reset();
    ForEachVoice([](kr106::Voice<T>& v) { v.Release(); });
    std::fill(std::begin(mVoiceNote), std::end(mVoiceNote), -1);
    mUnisonNote = -1;
    mUnisonStack.clear();
  }

  void PowerOff()
  {
    mArp.Reset();
    mKeysDown.reset();
    mHeldNotes.reset();
    ForEachVoice([](kr106::Voice<T>& v) { v.Release(); });
    std::fill(std::begin(mVoiceNote), std::end(mVoiceNote), -1);
    mUnisonNote = -1;
    mUnisonStack.clear();
  }

  void SetActiveVoices(int n)
  {
    n = std::clamp(n, 1, kMaxVoices);
    if (n == mActiveVoices) return;
    // Release ALL voices beyond the new limit — unison mode doesn't
    // set mVoiceNote, so we can't rely on it to detect busy voices.
    for (int i = n; i < kMaxVoices; i++)
    {
      mVoices[i]->Release();
      mVoiceNote[i] = -1;
    }
    mActiveVoices = n;
    if (mRoundRobinNext >= n) mRoundRobinNext = 0;
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
          if (mArp.mEnabled) mArp.NoteOff(i);
          auto it = std::find(mUnisonStack.begin(), mUnisonStack.end(), i);
          if (it != mUnisonStack.end()) mUnisonStack.erase(it);
        }
      }
      if (mUnisonNote >= 0 && mUnisonStack.empty())
      { ReleaseUnisonVoices(); mUnisonNote = -1; }
      else if (mUnisonNote >= 0 && !mHeldNotes.test(mUnisonNote))
      { /* Current note wasn't held — keep playing it */ }
      else if (!mUnisonStack.empty())
      { mUnisonNote = mUnisonStack.back(); GlideUnisonVoices(mUnisonNote); }
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
    // If arp was seeded from held notes (arp turned on while hold was on),
    // mHeldNotes is empty but arp still has the notes. Clear them too.
    if (mArp.mEnabled && !mArp.mHeldNotes.empty() && mKeysDown.none())
    {
      if (mArp.mLastNote >= 0) { SendToSynth(mArp.mLastNote, false, 0); mArp.mLastNote = -1; }
      mArp.mHeldNotes.clear();
    }
  }
};

// SetParam is defined out-of-line but still in the header (template)
#include "KR106_DSP_SetParam.h"
