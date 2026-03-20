#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <bitset>
#include "DSP/KR106_DSP.h"
#include "KR106PresetManager.h"

// Parameter indices — must match KR106DSP::SetParam's internal enum
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

class KR106AudioProcessor : public juce::AudioProcessor
{
public:
  KR106AudioProcessor();
  ~KR106AudioProcessor() override = default;

  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  void releaseResources() override {}
  void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

  juce::AudioProcessorEditor* createEditor() override;
  bool hasEditor() const override { return true; }

  const juce::String getName() const override { return "KR-106"; }
  bool acceptsMidi() const override { return true; }
  bool producesMidi() const override { return true; }
  double getTailLengthSeconds() const override { return 0.0; }

  int getNumPrograms() override;
  int getCurrentProgram() override;
  void setCurrentProgram(int index) override;
  const juce::String getProgramName(int index) override;
  void changeProgramName(int, const juce::String&) override {}

  void getStateInformation(juce::MemoryBlock& destData) override;
  void setStateInformation(const void* data, int sizeInBytes) override;

  // Thread-safe queues for UI↔audio communication
  void forceReleaseNote(int noteNum);
  void sendMidiFromUI(uint8_t status, uint8_t data1, uint8_t data2);

  // Fast param access for UI controls
  juce::RangedAudioParameter* getParam(int idx) { return mParams[idx]; }

  KR106DSP<float> mDSP;
  KR106PresetManager mPresetMgr;
  float mUIScale = 0.f;  // 0 = auto-detect (default), otherwise 1.0/1.5/2.0
  int mVoiceCount = 6;   // persisted per instance (6/8/10)
  bool mIgnoreVelocity = true; // persisted per instance
  bool mArpLimitKbd = true;    // persisted per instance
  bool mInitialDefault = true; // shows "Default" until first preset change

  // Preset CSV operations
  void reloadPresetsFromFile(const juce::File& file);
  void saveCurrentPresetToCSV(const juce::String& name);
  void renameCurrentPreset(const juce::String& name);
  void clearCurrentPreset();
  void pastePreset(const KR106Preset& preset);
  KR106Preset getPreset(int idx) const;
  bool isCurrentPresetDirty() const;
  juce::File getPresetCSVPath() const { return KR106PresetManager::getDefaultCSVPath(); }

  // Persist global settings (right-click menu: zoom, voices, etc.)
  void saveGlobalSettings();
  void loadGlobalSettings();

  // Clip indicator: peak level before saturator (audio writes, UI reads)
  std::atomic<float> mPeakLevel{0.f};

  // Scope ring buffer (audio writes, UI reads via timer)
  static constexpr int kScopeRingSize = 4096;
  float mScopeRing[kScopeRingSize] = {};    // L channel
  float mScopeRingR[kScopeRingSize] = {};   // R channel
  float mScopeSyncRing[kScopeRingSize] = {};
  std::atomic<int> mScopeWritePos{0};

  // Keyboard held-note display (audio thread writes, UI timer reads)
  std::bitset<128> mKeyboardHeld;
  std::atomic<bool> mLfoTriggered{false}; // CC1 mod state (audio→UI)

private:
  // Parameter listener: push param changes to DSP
  void parameterChanged(int paramIdx, float newValue);

  // Fast access to JUCE params by index
  juce::RangedAudioParameter* mParams[kNumParams] = {};

  // Get denormalized param value (0-1 for sliders, int range for switches, etc.)
  float getParamValue(int idx) const
  {
    return mParams[idx]->convertFrom0to1(mParams[idx]->getValue());
  }

  // Last-applied values — compared in processBlock to detect changes
  float mLastParamValues[kNumParams] = {};
  bool mPowerOn = true;

  // UI→audio thread-safe note release queue
  static constexpr int kForceReleaseQueueSize = 64;
  int mForceReleaseQueue[kForceReleaseQueueSize] = {};
  std::atomic<int> mForceReleaseHead{0};
  std::atomic<int> mForceReleaseTail{0};
  std::atomic<bool> mHoldOff{false};

  // UI→audio MIDI queue (keyboard notes, LFO trigger CC)
  struct UIMidiEvent { uint8_t status, data1, data2; };
  static constexpr int kUIMidiQueueSize = 256;
  UIMidiEvent mUIMidiQueue[kUIMidiQueueSize] = {};
  std::atomic<int> mUIMidiHead{0};
  std::atomic<int> mUIMidiTail{0};

  int mCurrentPreset = 0;

  // Flag: emit SysEx dump for current preset values on next processBlock
  std::atomic<bool> mSendPresetSysEx{false};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KR106AudioProcessor)
};
