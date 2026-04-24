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
#include "KR106AnalogNoise.h"
#include "KR106Resampler.h"

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
  static constexpr float kDCBlockHz = 4.f;

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
    float dcFrq = std::clamp(kDCBlockHz / (mSampleRate * 0.5f), 0.f, 0.9f);
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
    if (freqHz < 0.f) return DCBlock(boost.Process(input), dcState);
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
    ResetVoiceTbl();
    mLFOBuffer.reserve(4096);
    mLFORawBuffer.reserve(4096);
    mSyncBuffer.reserve(4096);
    mModulations.resize(kNumModulations, nullptr);

    // Shared mix-bus decimator pair (same coefficients as the
    // per-voice pair used to inside the VCF).
    mMixDown1.set_coefs(kr106::kResamplerCoefs2x);
    mMixDown2.set_coefs(kr106::kResamplerCoefs2x);
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

  // Poly II allocator: simple linear search matching IC20 ROM at $0A76.
  // Walks voices 0-5 looking for the first free voice (released or finished).
  // Fixed priority: voice 0 always gets first pick.
  // Returns -1 if all voices are active (no-steal policy, matches hardware).
  int FindLowestFreeVoice()
  {
    for (int i = 0; i < mActiveVoices; i++)
      if (mVoiceNote[i] < 0) return i;
    return -1; // all voices active, note dropped (matches hardware)
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

  // Hardware-accurate voice allocator (Juno-106 IC20 Poly II algorithm).
  // Uses bubble-sort priority table matching the actual ROM code at $0AAF/$0AF8.
  int FindHardwareVoice(int note)
  {
    int nv = mActiveVoices;

    // Step 1: Check capacity. If the last entry in voiceTbl is still playing
    // (mVoiceNote >= 0), all voices are active — drop the note.
    int lastVoice = mVoiceTbl[nv - 1];
    if (mVoiceNote[lastVoice] >= 0)
      return -1; // all voices active, note dropped

    // Step 2: Search from back for restart candidate (same note, free).
    // Walk backward through voiceTbl from the end (free voices are at back).
    int restartPos = -1;
    int firstFreePos = -1;
    for (int pos = nv - 1; pos >= 0; pos--)
    {
      int vi = mVoiceTbl[pos];
      if (mVoiceNote[vi] >= 0)
      {
        // Hit a playing voice — stop searching.
        // The first free voice is the one just after this.
        firstFreePos = pos + 1;
        break;
      }
      // This voice is free. Check if it last played the same note.
      if (mVoices[vi]->mMidiNote == note)
        restartPos = pos;
    }
    // If all voices are free, firstFreePos is 0
    if (firstFreePos < 0) firstFreePos = 0;

    // Step 3: Pick the voice
    int selectedPos = (restartPos >= 0) ? restartPos : firstFreePos;

    // Step 4: Bubble selected voice to front (position 0).
    // Swap with each adjacent entry above it, preserving order of others.
    for (int pos = selectedPos; pos > 0; pos--)
      std::swap(mVoiceTbl[pos], mVoiceTbl[pos - 1]);

    return mVoiceTbl[0];
  }

  // Hardware-accurate NoteOff: bubble released voice to back of voiceTbl.
  void HardwareNoteOff(int note)
  {
    int nv = mActiveVoices;

    // Find the voice playing this note (walk from front)
    int foundPos = -1;
    for (int pos = 0; pos < nv; pos++)
    {
      int vi = mVoiceTbl[pos];
      if (mVoiceNote[vi] == note)
      {
        foundPos = pos;
        break;
      }
    }
    if (foundPos < 0) return; // not found

    int vi = mVoiceTbl[foundPos];
    mVoices[vi]->Release();
    mVoiceNote[vi] = -1;

    // Bubble released voice to back of voiceTbl
    for (int pos = foundPos; pos < nv - 1; pos++)
      std::swap(mVoiceTbl[pos], mVoiceTbl[pos + 1]);
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
      // Poly I: hardware bubble-sort allocator with restart candidate search
      // and portamento. Matches IC20 ROM at $0AAF/$0AF8.
      if (noteOn)
      {
        int vi = FindHardwareVoice(note);
        if (vi < 0) return; // all voices active, note dropped (matches hardware)
        if (mVoiceNote[vi] >= 0) mVoiceNote[vi] = -1;
        TriggerVoice(vi, note, velocity);
      }
      else
      {
        HardwareNoteOff(note);
      }
    }
    else
    {
      // Poly II: simple linear search, fixed priority. Voice 0 always gets
      // first pick. Matches IC20 ROM at $0A76/$0B30.
      if (noteOn)
      {
        int vi = FindLowestFreeVoice();
        if (vi < 0) return; // all voices active, note dropped (matches hardware)
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

    // Size the Nx mix bus for this block.
    const int nFramesOS = nFrames * mOversample;
    if (static_cast<int>(mMixBusOS.size()) < nFramesOS)
      mMixBusOS.resize(nFramesOS, 0.f);
    std::memset(mMixBusOS.data(), 0, sizeof(float) * nFramesOS);

    memset(mSyncBuffer.data(), 0, nFrames * sizeof(T));
    int scopeVoice = -1;
    int64_t oldestAge = INT64_MAX;
    for (int i = 0; i < mActiveVoices; i++)
      if (mVoices[i]->GetBusy() && mVoiceAge[i] < oldestAge) { oldestAge = mVoiceAge[i]; scopeVoice = i; }

    bool anyBusy = false;
    ForEachVoice([&](kr106::Voice<T>& v) { anyBusy |= v.GetBusy(); });
    bool anyGated = false;
    for (int i = 0; i < mActiveVoices; i++) anyGated |= (mVoiceNote[i] >= 0);
    if (mPortaMode == 0 && mUnisonNote >= 0) anyGated = true;
    mLFO.SetVoiceActive(anyBusy, anyGated);

    mLFO.UpdateGateState();

    if (mSynthModel == kr106::kJ106 && !mLFO.mSyncToHost)
    {
      // J106 free-running: integer LFO advances at tick rate (~234 Hz).
      // Output is held (stairstepped) between ticks, matching firmware.
      for (int s = 0; s < nFrames; s++)
      {
        mLfoTickAccum += mLfoTickStep;
        if (mLfoTickAccum >= 1.f)
        {
          mLfoTickAccum -= 1.f;
          mLFO.Tick106();
        }
        float tri = mLFO.mIntTri;
        float amp = mLFO.mAmpInt;
        mLFOBuffer[s] = static_cast<T>(tri * amp);
        mLFORawBuffer[s] = static_cast<T>(tri);
        mNoiseBuffer[s] = static_cast<T>(mNoise.Process());
      }
      mLFO.mLastTri = mLFO.mIntTri;
      mLFO.mAmp = mLFO.mAmpInt;
    }
    else
    {
      // J6/J60 or DAW-synced J106: per-sample smooth LFO
      for (int s = 0; s < nFrames; s++)
      {
        mLFOBuffer[s] = static_cast<T>(mLFO.Process());
        mLFORawBuffer[s] = static_cast<T>(mLFO.mLastTri);
        mNoiseBuffer[s] = static_cast<T>(mNoise.Process());
      }
    }

    mModulations[kModLFO]    = mLFOBuffer.data();
    mModulations[kModLFORaw] = mLFORawBuffer.data();
    mModulations[kModNoise]  = mNoiseBuffer.data();

    mArp.Process(nFrames,
      [this](int note, int offset) { SendToSynth(note, true,  127, offset); },
      [this](int note, int offset) { SendToSynth(note, false, 0,   offset); });

    // Voices accumulate into the Nx mono mix bus.
    //
    // J106 idle voices take the VCF-only path: the filter runs (state
    // advances, self-oscillation continues) but nothing reaches the mix
    // bus. Matches hardware where the voice firmware computes the VCF
    // DAC for every voice every main-loop pass regardless of gate state
    // (see ic29.txt voice loop at $04D5), and the IR3109 filters
    // continuously self-oscillate at high Q between notes.
    //
    // J6 / J60 idle voices are skipped — those models use the log-freq
    // VCF path which does not yet have an idle-processing equivalent.
    for (int vi = 0; vi < mActiveVoices; vi++)
    {
      auto& v = *mVoices[vi];
      v.mLfoEnvAmp = mLFO.mAmp;
      if (v.GetBusy())
      {
        v.ProcessSamplesAccumulating(mModulations.data(), mMixBusOS.data(),
                                      kNumModulations, 0, nFrames);
      }
      else if (v.mModel == kr106::kJ106)
      {
        v.ProcessIdleVcfJ106(mModulations.data(), kNumModulations, 0, nFrames);
      }
    }

   // Decimate Nx mono mix bus down to base rate, writing into outputs[0].
    {
      const float* mix = mMixBusOS.data();
      if (mOversample == 4)
      {
        for (int s = 0; s < nFrames; s++)
        {
          float a[2] = { mix[0], mix[1] };
          float b[2] = { mix[2], mix[3] };
          float s2_0 = mMixDown2.process_sample(a);
          float s2_1 = mMixDown2.process_sample(b);
          float s1[2] = { s2_0, s2_1 };
          outputs[0][s] = static_cast<T>(mMixDown1.process_sample(s1));
          mix += 4;
        }
      }
      else if (mOversample == 2)
      {
        for (int s = 0; s < nFrames; s++)
        {
          float s1[2] = { mix[0], mix[1] };
          outputs[0][s] = static_cast<T>(mMixDown1.process_sample(s1));
          mix += 2;
        }
      }
      else // 1x — no decimation
      {
        for (int s = 0; s < nFrames; s++)
          outputs[0][s] = static_cast<T>(mix[s]);
      }
      // Mirror to right channel so the rest of the pre-chorus mono
      // chain sees consistent data (chorus overwrites both later).
      if (nOutputs > 1)
        memcpy(outputs[1], outputs[0], nFrames * sizeof(T));
    }

    // Scope sync
    if (scopeVoice >= 0)
    {
      float syncCps = mVoices[scopeVoice]->GetScopeSyncCPS();
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

    // Analog noise floor: white broadband + small 120 Hz rail ripple.
    for (int s = 0; s < nFrames; s++)
    {
      float n = mFloorNoise.Process() * kr106::analog_noise::kDryBroadbandGain * mNoiseFloorMul;
      n += mFloorRipple.Process() * mMainsMul;
      outputs[0][s] += static_cast<T>(n);
    }

    // (Analog bandwidth is now applied at 4× inside the decimation loop
    // above — no second pass needed here.)

    // VCA level before chorus (matches hardware: VCA IC5 → Chorus BBD)
    for (int s = 0; s < nFrames; s++)
    {
      mVcaLevelSmooth += (mVcaLevel - mVcaLevelSmooth) * mDacSmoothCoeff;
      outputs[0][s] *= static_cast<T>(mVcaLevelSmooth);
    }

    // Post-VCA low-pass (uPC1252H2 output pole)
    for (int s = 0; s < nFrames; s++)
    {
      float x = static_cast<float>(outputs[0][s]);
      mPostVcaLPState += mPostVcaLPCoeff * (x - mPostVcaLPState);
      outputs[0][s] = static_cast<T>(mPostVcaLPState);
    }

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

    // Master volume after chorus (scales signal + chorus noise together)
    // Smoothed per-sample to prevent zipper noise on knob changes.
    for (int s = 0; s < nFrames; s++)
    {
      mMasterVolSmooth += (mMasterVol - mMasterVolSmooth) * 0.001f;
      outputs[0][s] *= static_cast<T>(mMasterVolSmooth);
      if (nOutputs > 1) outputs[1][s] *= static_cast<T>(mMasterVolSmooth);
    }
  }

  T* GetSyncBuffer() { return mSyncBuffer.data(); }

  void Reset(double sampleRate, int blockSize)
  {
    mSampleRate = static_cast<float>(sampleRate);
    mDacSmoothCoeff = 1.f - expf(-1.f / (0.001f * mSampleRate)); // 1ms RC tau
    mLfoTickStep = kr106::ADSR::kTickRate / mSampleRate;
    mLfoTickAccum = 0.f;

    // Post-VCA low-pass: models uPC1252H2 VCA output pole.
    static constexpr float kPostVcaFc = 100000.f;
    mPostVcaLPCoeff = 1.f - expf(-2.f * static_cast<float>(M_PI) * kPostVcaFc / mSampleRate);
    mPostVcaLPState = 0.f;

    // Saw tables are clocked at the base sample rate — the oscillator
    // runs at 1×, and its output is upsampled per-voice into the 4×
    // mix bus before hitting the VCF (see Voice.h). This keeps the
    // oscillator's interpolation cost where it was in the original
    // architecture while still sharing the mix-bus decimator across
    // voices for a net CPU win.
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
    ResetVoiceTbl();
    mHPF.mModel = mSynthModel;
    mHPF.SetSampleRate(mSampleRate);
    mHPF.Init();
    mHPF.SetMode(1);
    mChorus.Init(mSampleRate);
    mFloorNoise.Init(mSampleRate); 
    mFloorRipple.SetMainsHz(60.f, mSampleRate);  // 50.f for EU
    mFloorRipple.SetAmplitudes(
    kr106::analog_noise::kDryRipple120,
    kr106::analog_noise::kDryRipple240,
    kr106::analog_noise::kDryRipple360);
    mFloorRipple.Reset();

    mArp.SetSampleRate(mSampleRate);
    mNoise.SetSampleRate(mSampleRate);
    mLFOBuffer.resize(blockSize);
    mLFORawBuffer.resize(blockSize);
    mNoiseBuffer.resize(blockSize);
    mSyncBuffer.resize(blockSize);
    mModulations.resize(kNumModulations);

    // Prime the shared mix-bus decimators. Without this, the first
    // non-zero sample through the chain causes a startup transient
    // (the half-band IIRs need a few samples of history). Feeding
    // silence through here was previously done inside each voice's
    // VCF.Process priming loop; we now do it once at the mix-bus level.
    mMixDown1.clear_buffers();
    mMixDown2.clear_buffers();
    {
      const int nFramesOS = std::max(16, blockSize) * mOversample;
      if (static_cast<int>(mMixBusOS.size()) < nFramesOS)
        mMixBusOS.resize(nFramesOS, 0.f);
      std::memset(mMixBusOS.data(), 0, sizeof(float) * nFramesOS);
      // Prime the downsamplers with silence
      float dummy[2] = {0.f, 0.f};
      for (int i = 0; i < 32; i++)
      {
        if (mOversample >= 4) {
          float s2_0 = mMixDown2.process_sample(dummy);
          float s2_1 = mMixDown2.process_sample(dummy);
          float s1[2] = { s2_0, s2_1 };
          (void)mMixDown1.process_sample(s1);
        } else if (mOversample >= 2) {
          (void)mMixDown1.process_sample(dummy);
        }
      }
    }
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
  float mVcaLevelSmooth = 1.f;
  float mPostVcaLPState = 0.f;   // one-pole LPF after VCA level, before chorus
  float mPostVcaLPCoeff = 1.f;   // coefficient (1.0 = bypassed)
  float mDacSmoothCoeff = 0.f;  // 1ms RC filter coefficient (shared by all CV smoothers)
  float mMasterVol = 1.f;
  float mMasterVolSmooth = 1.f;
  float mSampleRate = 44100.f;
  float mLfoTickAccum = 0.f;  // LFO tick accumulator (J106 integer LFO)
  float mLfoTickStep  = 0.f;  // ticks per sample (kTickRate / sampleRate)
  int mOctaveTranspose = 0;
  double mTuning = 0.;
  int mKeyTranspose = 0;
  bool mHold = false;
  bool mTranspose = false;
  int mPortaMode = 1; // Poly I (hardware default)
  int mUnisonNote = -1;
  std::vector<int> mUnisonStack;
  int mActiveVoices = 6;
  bool mIgnoreVelocity = true;
  bool mMonoRetrigger = true;
  int mVoiceNote[kMaxVoices] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
  int64_t mVoiceAge[kMaxVoices] = {};
  int64_t mVoiceAgeCounter = 0;
  int mRoundRobinNext = 0;

  // Hardware-accurate voice priority table (mirrors IC20's voiceTbl).
  // mVoiceTbl[0] = most recently triggered voice, mVoiceTbl[N-1] = longest idle.
  // Free (released) voices accumulate at the back of the table.
  int mVoiceTbl[kMaxVoices] = {0,1,2,3,4,5,6,7,8,9};

  void ResetVoiceTbl() { for (int i = 0; i < kMaxVoices; i++) mVoiceTbl[i] = i; }
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
  kr106::AnalogFloorNoise  mFloorNoise;
  kr106::RailRipple mFloorRipple;
  float mNoiseFloorMul = 1.f;  // user-adjustable analog broadband multiplier (variance sheet)
  float mMainsMul = 1.f;      // user-adjustable mains ripple multiplier (variance sheet)
  std::vector<T> mSyncBuffer;
  float mScopeSyncPhase = 0.f;
  bool mScopeSyncSub = false;
  std::vector<T*> mModulations;

  // ------------------------------------------------------------
  // Oversampled mix bus. Voices upsample per-voice and accumulate at
  // mOversample× rate. One shared decimator pair drops to base rate.
  int mOversample = 4;  // 1, 2, or 4
  std::vector<float> mMixBusOS;
  kr106::Downsampler2x mMixDown1;  // Nx -> N/2 (used at 2x and 4x)
  kr106::Downsampler2x mMixDown2;  // 2x -> 1x  (used at 4x only)

  // Change the oversample factor (1, 2, or 4) at runtime.
  //
  // Clears the state of every resampler and the analog-BW filter so
  // re-activating a previously-idle stage (e.g. going 2×→4× switches
  // mOscUp2 and mMixDown2 back on with stale state) can't produce a
  // click. Re-primes the mix-bus decimators with silence so the first
  // non-zero sample through them doesn't cause a startup transient
  // from the polyphase IIR's unloaded state.
  //
  // NOTE: the VCF's integrator states (mS[0..3]) are intentionally NOT
  // cleared — held notes should continue to sound through the change,
  // just with a different filter-pole placement. A mild spectral
  // transient at the change point is inherent; a click from cleared
  // resamplers would be worse.
  //
  // NOTE: Audio-thread only. This mutates Upsampler2x/Downsampler2x
  // state arrays non-atomically. JUCE routes
  // parameter changes through processBlock's parameter sync, so calls
  // originating from setValueNotifyingHost() arrive here on the audio
  // thread — safe. Do NOT call directly from a UI callback, message
  // thread, or any other non-audio context.
  void SetOversample(int os)
  {
    os = (os <= 1) ? 1 : (os <= 2) ? 2 : 4;
    if (os == mOversample) return;
    mOversample = os;

    ForEachVoice([os](kr106::Voice<T>& v) {
      v.mOversample = os;
      v.mVCF.SetOversample(os);
      // Clear both upsamplers regardless of the new rate — any stage
      // that becomes active (mOscUp1 for os>=2, mOscUp2 for os==4)
      // needs to start from zero state.
      v.mOscUp1.clear_buffers();
      v.mOscUp2.clear_buffers();
    });

    // Mix-bus decimators: clear, then re-prime with silence so the
    // polyphase IIR has a settled history before it sees real audio.
    mMixDown1.clear_buffers();
    mMixDown2.clear_buffers();
    float dummy[2] = {0.f, 0.f};
    for (int i = 0; i < 32; i++)
    {
      if (os >= 4) {
        float s2_0 = mMixDown2.process_sample(dummy);
        float s2_1 = mMixDown2.process_sample(dummy);
        float s1[2] = { s2_0, s2_1 };
        (void)mMixDown1.process_sample(s1);
      } else if (os >= 2) {
        (void)mMixDown1.process_sample(dummy);
      }
      // os == 1: no decimator runs at all, nothing to prime.
    }
  }

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
    ResetVoiceTbl();
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
    ResetVoiceTbl();
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
    ResetVoiceTbl();
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
    // Safety net: release any voices still sounding that aren't keyed down.
    // This catches cases where mHeldNotes was cleared (e.g. by preset change)
    // while Hold was on, leaving orphaned voices with no tracking.
    int nv = static_cast<int>(NVoices());
    for (int i = 0; i < nv; i++)
    {
      if (mVoiceNote[i] >= 0 && !mKeysDown.test(mVoiceNote[i]))
      {
        mVoices[i]->Release();
        mVoiceNote[i] = -1;
      }
    }
  }
};

// SetParam is defined out-of-line but still in the header (template)
#include "KR106_DSP_SetParam.h"
