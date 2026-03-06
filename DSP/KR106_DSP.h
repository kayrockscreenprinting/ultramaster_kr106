#pragma once

#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <bitset>

#include "MidiSynth.h"
#include "ISender.h"

#include "KR106Voice.h"
#include "KR106LFO.h"
#include "KR106Arpeggiator.h"
#include "KR106Chorus.h"

// Top-level KR-106 DSP orchestrator
// Pattern follows IPlugInstrument_DSP.h

using namespace iplug;

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
//
// Reference: https://electricdruid.net/juno-106-highpass-filter/
struct HPF
{
  static constexpr float kShelfFreqHz  = 150.f;
  static constexpr float kShelfGainLin = 3.162f; // ~+10 dB
  static constexpr float kHPFFreqs[4] = { 0.f, 0.f, 240.f, 720.f };

  int   mMode = 1;
  float mSampleRate = 44100.f;
  float mG   = 0.f;
  float mHpS = 0.f; // HPF integrator state
  float mLpS = 0.f; // shelf lowpass integrator state

  void Init()       { mHpS = 0.f; mLpS = 0.f; }
  void SetSampleRate(float sr) { mSampleRate = sr; Recalc(); }

  void SetMode(int mode)
  {
    int newMode = std::clamp(mode, 0, 3);
    if (newMode == mMode) return;
    mMode = newMode;
    // Don't reset integrator state — TPT state variables are
    // coefficient-independent, so the filter converges naturally
    // from any prior state. Resetting to 0 would cause a click
    // (the state tracks the signal's DC/LF content).
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
      // Low-shelf boost: TPT lowpass, add scaled copy back
      float v = (input - mLpS) * mG / (1.f + mG);
      float lp = mLpS + v;
      mLpS = lp + v;
      return input + (kShelfGainLin - 1.f) * lp;
    }

    // Modes 2 & 3: 1-pole TPT highpass
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
      auto* voice = new kr106::Voice<T>();
      voice->InitVariance(i);
      mSynth.AddVoice(voice, 0);
    }

    mSynth.SetPitchBendRange(12); // ±12 semitones for external MIDI

    // Pre-allocate audio buffers so ProcessBlock never triggers heap allocation
    mLFOBuffer.reserve(4096);
    mSyncBuffer.reserve(4096);
    mModulations.resize(kNumModulations, nullptr);
  }

  static double MidiToPitch(int note) { return (note - 69) / 12.0; }

  void TriggerUnisonVoices(int note, int velocity)
  {
    double pitch = MidiToPitch(note);
    float vel = velocity / 127.f;
    bool anyBusy = false;
    mSynth.ForEachVoice([&](SynthVoice& sv) { anyBusy |= sv.GetBusy(); });

    mSynth.ForEachVoice([pitch, vel, anyBusy](SynthVoice& sv) {
      auto& v = dynamic_cast<kr106::Voice<T>&>(sv);
      v.SetUnisonPitch(pitch);
      v.Trigger(vel, anyBusy);
    });

    mSynth.SetVoicesActive(true);
  }

  void ReleaseUnisonVoices()
  {
    mSynth.ForEachVoice([](SynthVoice& sv) { sv.Release(); });
  }

  // Lowest-first poly: prefer releasing, then idle, then steal oldest held
  int FindLowestFreeVoice()
  {
    int nv = static_cast<int>(mSynth.NVoices());

    // 1. Lowest releasing voice (NoteOff sent, still in release tail) — reuse same voice
    for (int i = 0; i < nv; i++)
      if (mVoiceNote[i] < 0 && mSynth.GetVoice(i)->GetBusy()) return i;

    // 2. Lowest idle voice (envelope finished)
    for (int i = 0; i < nv; i++)
      if (!mSynth.GetVoice(i)->GetBusy()) return i;

    // 3. All voices actively held — steal the oldest
    int oldest = 0;
    int64_t oldestAge = mVoiceAge[0];
    for (int i = 1; i < nv; i++)
    {
      if (mVoiceAge[i] < oldestAge)
      {
        oldestAge = mVoiceAge[i];
        oldest = i;
      }
    }
    return oldest;
  }

  // Round-robin poly: rotate through voices for even distribution
  int FindRoundRobinVoice()
  {
    int nv = static_cast<int>(mSynth.NVoices());

    // 1. Next idle voice (envelope finished) from rotation point
    for (int j = 0; j < nv; j++)
    {
      int i = (mRoundRobinNext + j) % nv;
      if (!mSynth.GetVoice(i)->GetBusy())
      {
        mRoundRobinNext = (i + 1) % nv;
        return i;
      }
    }

    // 2. Next releasing voice (NoteOff sent, still in release tail)
    for (int j = 0; j < nv; j++)
    {
      int i = (mRoundRobinNext + j) % nv;
      if (mVoiceNote[i] < 0)
      {
        mRoundRobinNext = (i + 1) % nv;
        return i;
      }
    }

    // 3. All held — steal oldest
    int oldest = 0;
    int64_t oldestAge = mVoiceAge[0];
    for (int i = 1; i < nv; i++)
    {
      if (mVoiceAge[i] < oldestAge) { oldestAge = mVoiceAge[i]; oldest = i; }
    }
    mRoundRobinNext = (oldest + 1) % nv;
    return oldest;
  }

  void TriggerVoice(int voiceIdx, int note, int velocity)
  {
    auto& v = dynamic_cast<kr106::Voice<T>&>(*mSynth.GetVoice(voiceIdx));
    double pitch = MidiToPitch(note);
    v.SetUnisonPitch(pitch);
    v.Trigger(velocity / 127.f, v.GetBusy());
    mVoiceNote[voiceIdx] = note;
    mVoiceAge[voiceIdx] = ++mVoiceAgeCounter;

    mSynth.SetVoicesActive(true);
  }

  void SendToSynth(int note, bool noteOn, int velocity, int offset = 0)
  {
    if (mPortaMode == 0) // Unison — last-note priority
    {
      if (noteOn)
      {
        // Remove duplicate if already in stack, then push to top
        auto it = std::find(mUnisonStack.begin(), mUnisonStack.end(), note);
        if (it != mUnisonStack.end()) mUnisonStack.erase(it);
        mUnisonStack.push_back(note);

        if (mUnisonNote >= 0)
          ReleaseUnisonVoices();
        mUnisonNote = note;
        TriggerUnisonVoices(note, velocity);
      }
      else
      {
        // Remove from stack
        auto it = std::find(mUnisonStack.begin(), mUnisonStack.end(), note);
        if (it != mUnisonStack.end()) mUnisonStack.erase(it);

        if (note == mUnisonNote)
        {
          if (!mUnisonStack.empty())
          {
            // Return to previous held note
            mUnisonNote = mUnisonStack.back();
            TriggerUnisonVoices(mUnisonNote, 127);
          }
          else
          {
            ReleaseUnisonVoices();
            mUnisonNote = -1;
          }
        }
      }
    }
    else if (mPortaMode == 1) // Poly + Porta — lowest-voice-first
    {
      if (noteOn)
      {
        int vi = FindLowestFreeVoice();
        // If stealing, clear old mapping
        if (mVoiceNote[vi] >= 0) mVoiceNote[vi] = -1;
        TriggerVoice(vi, note, velocity);
      }
      else
      {
        // Find the voice playing this note and release it
        int nv = static_cast<int>(mSynth.NVoices());
        for (int i = 0; i < nv; i++)
        {
          if (mVoiceNote[i] == note)
          {
            mSynth.GetVoice(i)->Release();
            mVoiceNote[i] = -1;
            break;
          }
        }
      }
    }
    else // Poly round-robin (mode 2)
    {
      if (noteOn)
      {
        int vi = FindRoundRobinVoice();
        if (mVoiceNote[vi] >= 0) mVoiceNote[vi] = -1;
        TriggerVoice(vi, note, velocity);
      }
      else
      {
        int nv = static_cast<int>(mSynth.NVoices());
        for (int i = 0; i < nv; i++)
        {
          if (mVoiceNote[i] == note)
          {
            mSynth.GetVoice(i)->Release();
            mVoiceNote[i] = -1;
            break;
          }
        }
      }
    }
  }

  void ProcessBlock(T** inputs, T** outputs, int nOutputs, int nFrames)
  {
    // 1. Clear outputs
    for (int c = 0; c < nOutputs; c++)
      memset(outputs[c], 0, nFrames * sizeof(T));

    // Safety: ensure buffers are allocated (in case ProcessBlock runs before Reset)
    if (static_cast<int>(mLFOBuffer.size()) < nFrames)
    {
      mLFOBuffer.resize(nFrames, T(0));
      mSyncBuffer.resize(nFrames, T(0));
      mModulations.resize(kNumModulations, nullptr);
    }

    // 2. Clear sync buffer and assign to first active voice only (for scope)
    memset(mSyncBuffer.data(), 0, nFrames * sizeof(T));
    bool syncAssigned = false;
    mSynth.ForEachVoice([this, &syncAssigned](SynthVoice& v) {
      auto& jv = dynamic_cast<kr106::Voice<T>&>(v);
      if (!syncAssigned && v.GetBusy())
      {
        jv.mSyncOut = mSyncBuffer.data();
        syncAssigned = true;
      }
      else
      {
        jv.mSyncOut = nullptr;
      }
    });

    // 3. Check if any voice is active (for LFO delay reset)
    bool anyActive = false;
    mSynth.ForEachVoice([&](SynthVoice& v) {
      anyActive |= v.GetBusy();
    });
    mLFO.SetVoiceActive(anyActive);

    // 3. Fill LFO buffer
    for (int s = 0; s < nFrames; s++)
      mLFOBuffer[s] = static_cast<T>(mLFO.Process());

    // 4. Set modulation pointers
    mModulations[kModLFO] = mLFOBuffer.data();

    // 5. Process arpeggiator — generates note events for this block
    mArp.Process(nFrames,
      [this](int note, int offset) { SendToSynth(note, true,  127, offset); },
      [this](int note, int offset) { SendToSynth(note, false, 0,   offset); });

    // 6. Process voices through MidiSynth
    mSynth.ProcessBlock(mModulations.data(), outputs,
      kNumModulations, nOutputs, nFrames);

    // 6. HPF (in-place on mono sum in channel 0)
    for (int s = 0; s < nFrames; s++)
      outputs[0][s] = static_cast<T>(mHPF.Process(static_cast<float>(outputs[0][s])));

    // 7. Stereo chorus
    if (mChorus.mMode == 0)
    {
      // No chorus: copy L to R
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

    // 8. Master volume
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
    mSynth.SetSampleRateAndBlockSize(sampleRate, blockSize);
    mSynth.Reset();

    mHPF.SetSampleRate(mSampleRate);
    mHPF.Init();
    mHPF.SetMode(1); // default: flat

    mChorus.Init(mSampleRate);

    mArp.SetSampleRate(mSampleRate);

    mLFOBuffer.resize(blockSize);
    mSyncBuffer.resize(blockSize);
    mModulations.resize(kNumModulations);
  }

  void ProcessMidiMsg(const IMidiMsg& msg)
  {
    // LFO trigger button sends CC1 (mod wheel)
    if (msg.StatusMsg() == IMidiMsg::kControlChange &&
        msg.ControlChangeIdx() == IMidiMsg::kModWheel)
    {
      mLFO.SetTrigger(msg.ControlChange(IMidiMsg::kModWheel) > 0.0);
      return;
    }

    auto status = msg.StatusMsg();
    bool isNoteOn = (status == IMidiMsg::kNoteOn && msg.Velocity() > 0);
    bool isNoteOff = (status == IMidiMsg::kNoteOff ||
                      (status == IMidiMsg::kNoteOn && msg.Velocity() == 0));

    // Track physical key state (for seeding arp on enable)
    if (isNoteOn) mKeysDown.set(msg.NoteNumber());
    else if (isNoteOff) mKeysDown.reset(msg.NoteNumber());

    // Arpeggiator intercepts note events when enabled
    if (mArp.mEnabled && (isNoteOn || isNoteOff))
    {
      if (isNoteOn)
        mArp.NoteOn(msg.NoteNumber());
      else if (mHold)
        mHeldNotes.set(msg.NoteNumber()); // track for Hold release
      else
        mArp.NoteOff(msg.NoteNumber());
      return;
    }

    // Hold logic: suppress note-off when hold is active
    if (mHold && isNoteOff)
    {
      mHeldNotes.set(msg.NoteNumber());
      return;
    }

    SendToSynth(msg.NoteNumber(), isNoteOn, msg.Velocity(), 0);
  }

  // ADSR lookup tables — slider (0-1) → ms, fit from Juno-106 factory preset data.
  // Generated by tools/adsr-calibration/fit_slider_to_ms.py via PCHIP interpolation.
  // Attack: 1.5–3500ms, Decay/Release: 1.5–24000ms.
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

  // Juno-6 ADSR lookup tables — exponential curves derived from IR3R01 specs.
  // Attack: 0.5–3000ms (virtually instant at bottom), Decay/Release: 2–12000ms.
  static constexpr float kAttackLUT_J6[128] = {
    0.5f, 0.5f, 0.6f, 0.6f, 0.7f, 0.7f, 0.8f, 0.8f,
    0.9f, 0.9f, 1.0f, 1.1f, 1.1f, 1.2f, 1.3f, 1.4f,
    1.5f, 1.6f, 1.7f, 1.8f, 2.0f, 2.1f, 2.3f, 2.4f,
    2.6f, 2.8f, 3.0f, 3.2f, 3.4f, 3.6f, 3.9f, 4.2f,
    4.5f, 4.8f, 5.1f, 5.5f, 5.9f, 6.3f, 6.8f, 7.2f,
    7.7f, 8.3f, 8.9f, 9.5f, 10.2f, 10.9f, 11.7f, 12.5f,
    13.4f, 14.3f, 15.4f, 16.5f, 17.6f, 18.9f, 20.2f, 21.6f,
    23.2f, 24.8f, 26.6f, 28.5f, 30.5f, 32.6f, 34.9f, 37.4f,
    40.1f, 42.9f, 46.0f, 49.2f, 52.7f, 56.5f, 60.5f, 64.7f,
    69.3f, 74.2f, 79.5f, 85.1f, 91.2f, 97.6f, 104.6f, 112.0f,
    119.9f, 128.4f, 137.5f, 147.3f, 157.7f, 168.9f, 180.9f, 193.7f,
    207.4f, 222.2f, 237.9f, 254.8f, 272.8f, 292.2f, 312.9f, 335.1f,
    358.8f, 384.3f, 411.5f, 440.7f, 471.9f, 505.4f, 541.2f, 579.6f,
    620.7f, 664.7f, 711.8f, 762.3f, 816.4f, 874.2f, 936.2f, 1002.6f,
    1073.7f, 1149.8f, 1231.3f, 1318.7f, 1412.1f, 1512.3f, 1619.5f, 1734.3f,
    1857.3f, 1989.0f, 2130.0f, 2281.0f, 2442.7f, 2615.9f, 2801.4f, 3000.0f
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

  // Linearly interpolate a 128-entry LUT from a 0-1 slider value
  static float LookupLUT(const float* lut, float s)
  {
    float pos = s * 127.0f;
    int idx = static_cast<int>(pos);
    if (idx >= 127) return lut[127];
    float frac = pos - static_cast<float>(idx);
    return lut[idx] + frac * (lut[idx + 1] - lut[idx]);
  }

  void SetParam(int paramIdx, double value)
  {
    // Import param enum values
    // These must match the EParams enum in KR106.h
    enum {
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
      kAdsrMode
    };

    switch (paramIdx)
    {
      // --- Per-voice continuous params ---
      case kDcoLfo:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mDcoLfo = static_cast<float>(value); });
        break;
      case kDcoPwm:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mDcoPwm = static_cast<float>(value); });
        break;
      case kDcoSub:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mDcoSub = static_cast<float>(value); });
        break;
      case kDcoNoise:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mDcoNoise = static_cast<float>(value); });
        break;
      case kVcfFreq: {
        float hz = 20.f * std::pow(900.f, static_cast<float>(value));  // 20–18000 Hz exponential
        SetVoiceParam([hz](kr106::Voice<T>& v) { v.mVcfFreq = hz; });
        break;
      }
      case kVcfRes:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mVcfRes = static_cast<float>(value); });
        break;
      case kVcfEnv:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mVcfEnv = static_cast<float>(value); });
        break;
      case kVcfLfo:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mVcfLfo = static_cast<float>(value); });
        break;
      case kVcfKbd:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mVcfKbd = static_cast<float>(value); });
        break;
      case kBenderDco:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mBendDco = static_cast<float>(value); });
        break;
      case kBenderVcf:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mBendVcf = static_cast<float>(value); });
        break;
      case kBenderLfo:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mBendLfo = static_cast<float>(value); });
        break;

      // --- ADSR (slider 0-1 → ms via LUT; 0=Juno-6, 1=Juno-106) ---
      case kEnvA: {
        mSliderA = static_cast<float>(value);
        bool j6 = (mAdsrMode == 0);
        float ms = LookupLUT(j6 ? kAttackLUT_J6 : kAttackLUT, mSliderA);
        if (j6)
          SetVoiceParam([ms](kr106::Voice<T>& v) { v.mADSR.SetAttackExp(ms); });
        else
          SetVoiceParam([ms](kr106::Voice<T>& v) { v.mADSR.SetAttack(ms); });
        break;
      }
      case kEnvD: {
        mSliderD = static_cast<float>(value);
        float ms = LookupLUT(mAdsrMode == 0 ? kDecayLUT_J6 : kDecayLUT, mSliderD);
        SetVoiceParam([ms](kr106::Voice<T>& v) { v.mADSR.SetDecay(ms); });
        break;
      }
      case kEnvS: {
        float s = std::max(static_cast<float>(value), 0.001f);  // floor at -60dB
        SetVoiceParam([s](kr106::Voice<T>& v) { v.mADSR.SetSustain(s); });
        break;
      }
      case kEnvR: {
        mSliderR = static_cast<float>(value);
        float ms = LookupLUT(mAdsrMode == 0 ? kReleaseLUT_J6 : kReleaseLUT, mSliderR);
        SetVoiceParam([ms](kr106::Voice<T>& v) { v.mADSR.SetRelease(ms); });
        break;
      }

      // --- Per-voice switches ---
      case kDcoPulse:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mPulseOn = value > 0.5; });
        break;
      case kDcoSaw:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mSawOn = value > 0.5; });
        break;
      case kDcoSubSw:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mSubOn = value > 0.5; });
        break;
      case kPwmMode:
        SetVoiceParam([value](kr106::Voice<T>& v) {
          v.mPwmMode = static_cast<int>(value) - 1; // 0->-1(LFO), 1->0(MAN), 2->1(ENV)
        });
        break;
      case kVcfEnvInv:
        SetVoiceParam([value](kr106::Voice<T>& v) {
          v.mVcfEnvInvert = (static_cast<int>(value) != 0) ? -1 : 1;
        });
        break;
      case kVcaMode:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mVcaMode = static_cast<int>(value); });
        break;
      case kAdsrMode: {
        mAdsrMode = static_cast<int>(value);
        bool expAtk = (mAdsrMode == 0); // 0=Juno-6 (exponential attack)
        SetVoiceParam([expAtk](kr106::Voice<T>& v) { v.mADSR.mExponentialAttack = expAtk; });
        // Re-apply ADSR sliders with the new LUT set
        SetParam(kEnvA, mSliderA);
        SetParam(kEnvD, mSliderD);
        SetParam(kEnvR, mSliderR);
        break;
      }
      case kBender:
        SetVoiceParam([value](kr106::Voice<T>& v) { v.mRawBend = static_cast<float>(value); });
        break;

      // --- Global: LFO ---
      case kLfoRate: {
        float bpm = 18.f + static_cast<float>(value) * 1182.f;  // 18–1200 BPM
        mLFO.SetRate(bpm, mSampleRate);
        break;
      }
      case kLfoDelay:
        mLFO.SetDelay(static_cast<float>(value));
        break;
      case kLfoMode:
        mLFO.SetMode(static_cast<int>(value));
        break;

      // --- Global: HPF ---
      case kHpfFreq:
        mHPF.SetMode(static_cast<int>(value));
        break;

      // --- Global: VCA level (+/-6 dB around unity, 0.5 = 0 dB) ---
      case kVcaLevel:
      {
        float bias = static_cast<float>(value) * 2.f - 1.f; // -1..+1
        mVcaLevel = std::pow(10.f, bias * 6.f / 20.f);      // +/-6 dB
        break;
      }

      // --- Global: Chorus ---
      case kChorusOff:
        // Handled via chorus I/II buttons
        break;
      case kChorusI:
        mChorusI = value > 0.5;
        UpdateChorusMode();
        break;
      case kChorusII:
        mChorusII = value > 0.5;
        UpdateChorusMode();
        break;

      // --- Global: Octave / Tuning ---
      case kOctTranspose:
      {
        mOctaveTranspose = 1 - static_cast<int>(value); // 0=up(+1), 1=normal(0), 2=down(-1)
        float semi = mOctaveTranspose * 12.f + static_cast<float>(mTuning) + mKeyTranspose;
        SetVoiceParam([semi](kr106::Voice<T>& v) { v.mOctTranspose = semi; });
        break;
      }
      case kTuning:
      {
        mTuning = value; // -1..+1 semitones
        float semi = mOctaveTranspose * 12.f + static_cast<float>(mTuning) + mKeyTranspose;
        SetVoiceParam([semi](kr106::Voice<T>& v) { v.mOctTranspose = semi; });
        break;
      }

      // --- Portamento mode ---
      case kPortaMode:
      {
        int prevMode = mPortaMode;
        mPortaMode = static_cast<int>(value); // 0=Unison(up), 1=Porta(mid), 2=Poly(down)

        if (mSuppressHoldRelease)
        {
          // During preset load: update portaEnabled but don't release voices or clear held notes
          bool portaOn = (mPortaMode <= 1);
          SetVoiceParam([portaOn](kr106::Voice<T>& v) { v.mPortaEnabled = portaOn; });
          break;
        }

        // Poly modes 1 and 2 share mVoiceNote[] — voices keep running
        bool prevPoly = (prevMode >= 1);
        bool newPoly  = (mPortaMode >= 1);

        if (prevPoly && newPoly)
        {
          // Poly ↔ Poly: nothing to do, voices and mVoiceNote[] stay valid
        }
        else
        {
          // Unison ↔ Poly: fundamentally different voice layout, must retrigger
          std::bitset<128> activeNotes = mKeysDown | mHeldNotes;

          if (prevMode == 0)
          {
            ReleaseUnisonVoices();
            mUnisonNote = -1;
            mUnisonStack.clear();
          }
          else
          {
            int nv = static_cast<int>(mSynth.NVoices());
            for (int i = 0; i < nv; i++)
            {
              if (mVoiceNote[i] >= 0) { mSynth.GetVoice(i)->Release(); mVoiceNote[i] = -1; }
            }
          }
          mHeldNotes.reset();

          for (int i = 0; i < 128; i++)
          {
            if (activeNotes.test(i))
            {
              SendToSynth(i, true, 127);
              if (!mKeysDown.test(i))
                mHeldNotes.set(i);
            }
          }
        }

        bool portaOn = (mPortaMode <= 1);
        SetVoiceParam([portaOn](kr106::Voice<T>& v) { v.mPortaEnabled = portaOn; });
        break;
      }
      case kPortaRate:
      {
        float rate = static_cast<float>(value);
        SetVoiceParam([rate](kr106::Voice<T>& v) {
          v.mPortaRateParam = rate;
          v.UpdatePortaCoeff();
        });
        break;
      }

      // --- Arpeggiator ---
      case kArpRate:
        mArp.mRate = static_cast<float>(value);
        break;
      case kArpMode:
        mArp.mMode = static_cast<int>(value);
        mArp.mStepIndex = 0;
        mArp.mDirection = 1;
        break;
      case kArpRange:
        mArp.mRange = static_cast<int>(value);
        break;
      case kArpeggio:
      {
        bool wasEnabled = mArp.mEnabled;
        mArp.mEnabled = value > 0.5;
        if (mSuppressHoldRelease)
          break; // preserve note state during preset load; arp will resume with existing notes
        if (mArp.mEnabled && !wasEnabled)
        {
          // Seed arp with all currently sounding notes:
          // physically held keys (mKeysDown) + hold-held notes (mHeldNotes)
          std::bitset<128> toSeed = mKeysDown | mHeldNotes;
          for (int i = 0; i < 128; i++)
          {
            if (toSeed.test(i))
            {
              mArp.NoteOn(i);
              IMidiMsg off;
              off.MakeNoteOffMsg(i, 0);
              mSynth.AddMidiMsgToQueue(off);
            }
          }
          // Hold-held notes are now owned by the arp
          mHeldNotes.reset();
        }
        else if (!mArp.mEnabled && wasEnabled)
        {
          // Release the currently sounding arp note immediately
          if (mArp.mLastNote >= 0)
          {
            IMidiMsg off;
            off.MakeNoteOffMsg(mArp.mLastNote, 0);
            mSynth.AddMidiMsgToQueue(off);
            mArp.mLastNote = -1;
          }
          // If hold is active, restore the arp's notes as held notes
          if (mHold)
          {
            mHeldNotes.reset();
            for (int n : mArp.mHeldNotes)
            {
              IMidiMsg on;
              on.MakeNoteOnMsg(n, 127, 0);
              mSynth.AddMidiMsgToQueue(on);
              mHeldNotes.set(n);
            }
          }
          mArp.Reset();
        }
        break;
      }

      // --- Hold ---
      case kHold:
        mHold = value > 0.5;
        if (!mHold && !mSuppressHoldRelease)
          ReleaseHeldNotes();
        break;

      // --- Transpose (key transpose mode) ---
      case kTranspose:
        mTranspose = value > 0.5;
        break;

      default:
        break;
    }
  }

public:
  MidiSynth mSynth { VoiceAllocator::kPolyModePoly, MidiSynth::kDefaultBlockSize };
  kr106::LFO mLFO;
  kr106::HPF mHPF;
  kr106::Chorus mChorus;
  kr106::Arpeggiator mArp;

  float mVcaLevel = 1.f;  // unity = param 0.5 (0 dB)
  float mSampleRate = 44100.f;
  int mOctaveTranspose = 0;
  double mTuning = 0.;
  int mKeyTranspose = 0;  // semitone offset from keyboard transpose mode
  bool mHold = false;
  bool mTranspose = false;
  int mPortaMode = 2;     // 0=Unison(up), 1=Poly+Porta(mid), 2=Poly(down)
  int mUnisonNote = -1;   // currently sounding unison note (-1 = none)
  std::vector<int> mUnisonStack; // held notes for last-note priority (unison mode)
  int mVoiceNote[6] = {-1,-1,-1,-1,-1,-1}; // note-to-voice map for poly modes (1 and 2)
  int64_t mVoiceAge[6] = {0,0,0,0,0,0};   // assignment order counter for voice stealing
  int64_t mVoiceAgeCounter = 0;
  int mRoundRobinNext = 0;                 // round-robin rotation index for mode 2
  bool mChorusI = false;
  bool mChorusII = false;
  int mAdsrMode = 0;          // 0 = Juno-106, 1 = Juno-6
  float mSliderA = 0.25f;     // cached ADSR slider values for mode-switch re-apply
  float mSliderD = 0.25f;
  float mSliderR = 0.25f;

  std::bitset<128> mHeldNotes;   // for Hold button release tracking
  std::bitset<128> mKeysDown;    // physical key state (for arp seeding)
  bool mSuppressHoldRelease = false; // true during preset load to keep held notes alive
  std::vector<T> mLFOBuffer;
  std::vector<T> mSyncBuffer;   // oscillator sync pulses for scope trigger
  std::vector<T*> mModulations;

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
    if (mPortaMode == 0) // Unison: release all held notes from stack
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
      {
        ReleaseUnisonVoices();
        mUnisonNote = -1;
      }
      else if (mUnisonNote >= 0 && !mHeldNotes.test(mUnisonNote))
      {
        // Current note wasn't held — keep playing it
      }
      else if (!mUnisonStack.empty())
      {
        // Current note was held, fall back to top of stack
        mUnisonNote = mUnisonStack.back();
        TriggerUnisonVoices(mUnisonNote, 127);
      }
    }
    else
    {
      for (int i = 0; i < 128; i++)
      {
        if (mHeldNotes.test(i))
        {
          if (mArp.mEnabled)
            mArp.NoteOff(i);
          else
            SendToSynth(i, false, 0);
        }
      }
    }
    mHeldNotes.reset();
  }

public:
  // Set keyboard transpose offset (semitones). Called from audio thread each block.
  void SetKeyTranspose(int semitones)
  {
    if (semitones == mKeyTranspose) return;
    mKeyTranspose = semitones;
    float semi = mOctaveTranspose * 12.f + static_cast<float>(mTuning) + mKeyTranspose;
    SetVoiceParam([semi](kr106::Voice<T>& v) { v.mOctTranspose = semi; });
  }

  // Kill all sounding notes, clear held/arp state. Called on power-off.
  void PowerOff()
  {
    mArp.Reset();
    mKeysDown.reset();
    mHeldNotes.reset();
    // Send All Notes Off (CC123) to release all synth voices
    IMidiMsg msg;
    msg.MakeControlChangeMsg(IMidiMsg::kAllNotesOff, 0, 0);
    mSynth.AddMidiMsgToQueue(msg);
  }

  // Force-release a single note, bypassing hold suppression.
  // Called from the plugin's audio block when the UI explicitly toggles a key off.
  // The note may or may not be in mHeldNotes (since OnMouseUp skips NoteOff when hold is on).
  void ForceRelease(int noteNum)
  {
    mHeldNotes.reset(noteNum);
    if (mArp.mEnabled)
      mArp.NoteOff(noteNum);
    else
      SendToSynth(noteNum, false, 0);
  }

  template <typename F>
  void SetVoiceParam(F func)
  {
    mSynth.ForEachVoice([&func](SynthVoice& v) {
      func(dynamic_cast<kr106::Voice<T>&>(v));
    });
  }
};
