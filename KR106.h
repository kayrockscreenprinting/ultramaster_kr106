#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "ISender.h"
#include "IPlugQueue.h"
#include "DSP/KR106_DSP.h"
#include <atomic>
#include <bitset>

const int kNumPresets = 205;

// Parameter indices — maps to kr106_control_t from kr106_common.h
enum EParams
{
  // Sliders (continuous)
  kBenderDco = 0,   // SL_BENDER_DCO
  kBenderVcf,       // SL_BENDER_VCF
  kArpRate,         // SL_ARPEGGIO_RATE
  kLfoRate,         // SL_LFO_RATE
  kLfoDelay,        // SL_LFO_DELAY
  kDcoLfo,          // SL_DCO_LFO
  kDcoPwm,          // SL_DCO_PWM
  kDcoSub,          // SL_DCO_SUB
  kDcoNoise,        // SL_DCO_NOISE
  kHpfFreq,         // SL_HPF_FREQ
  kVcfFreq,         // SL_VCF_FREQ
  kVcfRes,          // SL_VCF_RES
  kVcfEnv,          // SL_VCF_ENV
  kVcfLfo,          // SL_VCF_LFO
  kVcfKbd,          // SL_VCF_KBYD
  kVcaLevel,        // SL_VCA_LEVEL
  kEnvA,            // SL_ENV_A
  kEnvD,            // SL_ENV_D
  kEnvS,            // SL_ENV_S
  kEnvR,            // SL_ENV_R

  // Buttons (toggle 0/1)
  kTranspose,       // BT_TRANSPOSE
  kHold,            // BT_HOLD
  kArpeggio,        // BT_ARPEGGIO
  kDcoPulse,        // BT_DCO_PULSE
  kDcoSaw,          // BT_DCO_SAW
  kDcoSubSw,        // BT_DCO_SUB (switch)
  kChorusOff,       // BT_CHORUS_OFF
  kChorusI,         // BT_CHORUS_I
  kChorusII,        // BT_CHORUS_II

  // Switches (int positions)
  kOctTranspose,    // SW_OCTAVE_TRANSPOSE (0,1,2)
  kArpMode,         // SW_ARP_MODE (0,1,2)
  kArpRange,        // SW_ARP_RANGE (0,1,2)
  kLfoMode,         // SW_LFO_MODE (0,1)
  kPwmMode,         // SW_PWM_MODE (0,1,2)
  kVcfEnvInv,       // SW_VCF_ENV_INVERT (0,1)
  kVcaMode,         // SW_VCA_MODE (0,1)

  // Special controls
  kBender,          // BD_BENDER
  kTuning,          // KN_TUNING
  kPower,

  kNumParams
};

enum ECtrlTags
{
  kCtrlTagKeyboard = 0,
  kCtrlTagScope,
  kCtrlTagAnalyzer
};

using namespace iplug;
using namespace igraphics;

class KR106 final : public Plugin
{
public:
  KR106(const InstanceInfo& info);

  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
  void ProcessMidiMsg(const IMidiMsg& msg) override;
  void OnReset() override;
  void OnParamChange(int paramIdx) override;
  void OnIdle() override;
  int UnserializeState(const IByteChunk& chunk, int startPos) override;

private:
  KR106DSP<sample> mDSP{6};
  IBufferSender<2> mScopeSender; // ch0=audio, ch1=osc sync
  bool mPowerOn = true;
  IPlugQueue<IMidiMsg> mMidiForKeyboard {512}; // audio→UI thread-safe MIDI display queue
  std::atomic<bool> mHoldOff{false};     // set from UI when Hold turns off
  std::bitset<128> mKeyboardHeld;        // visually held notes (audio thread only)
  IPlugQueue<int> mForceRelease{16};     // notes to force-release from hold/arp (UI→audio)

public:
  // Called from UI to individually release a held note (bypasses hold suppression).
  void ForceReleaseNote(int noteNum) { mForceRelease.Push(noteNum); }
};
