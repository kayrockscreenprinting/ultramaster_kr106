#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "KR106_Presets_JUCE.h"

KR106AudioProcessor::KR106AudioProcessor()
  : AudioProcessor(BusesProperties()
      .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
  // --- Sliders (0–1 range) ---
  using SFV = std::function<juce::String(float, int)>;
  auto addSlider = [this](int idx, const char* name, float def,
                          float min = 0.f, float max = 1.f,
                          SFV fmt = nullptr) {
    auto attrs = juce::AudioParameterFloatAttributes();
    if (fmt) attrs = attrs.withStringFromValueFunction(std::move(fmt));
    auto* p = new juce::AudioParameterFloat(
      juce::ParameterID("p" + juce::String(idx), 1), name,
      juce::NormalisableRange<float>(min, max, 0.01f), def, attrs);
    addParameter(p);
    mParams[idx] = p;
  };

  // --- Int switches ---
  using SFI = std::function<juce::String(int, int)>;
  auto addSwitch = [this](int idx, const char* name, int def, int min, int max,
                          SFI fmt = nullptr) {
    auto attrs = juce::AudioParameterIntAttributes();
    if (fmt) attrs = attrs.withStringFromValueFunction(std::move(fmt));
    auto* p = new juce::AudioParameterInt(
      juce::ParameterID("p" + juce::String(idx), 1), name, min, max, def, attrs);
    addParameter(p);
    mParams[idx] = p;
  };

  // --- Toggle buttons ---
  auto addBool = [this](int idx, const char* name, bool def) {
    auto* p = new juce::AudioParameterBool(
      juce::ParameterID("p" + juce::String(idx), 1), name, def);
    addParameter(p);
    mParams[idx] = p;
  };

  // --- Display formatters ---
  SFV fmtPct = [](float v, int) {
    return juce::String(juce::roundToInt(v * 100.f)) + "%";
  };
  SFV fmtVcfHz = [](float v, int) {
    double hz = 20.0 * std::pow(900.0, (double)v);
    if (hz >= 1000.0) return juce::String(hz / 1000.0, 1) + " kHz";
    return juce::String(juce::roundToInt(hz)) + " Hz";
  };
  SFV fmtLfoRate = [](float v, int) {
    double hz = (18.0 + (double)v * 1182.0) / 60.0;
    return juce::String(hz, 1) + " Hz";
  };
  SFV fmtLfoDelay = [](float v, int) {
    if (v <= 0.f) return juce::String("Off");
    return juce::String(juce::roundToInt(v * 1500.f)) + " ms";
  };
  SFV fmtVcaLevel = [](float v, int) {
    double dB = ((double)v * 2.0 - 1.0) * 6.0;
    if (dB >= 0.0) return "+" + juce::String(dB, 1) + " dB";
    return juce::String(dB, 1) + " dB";
  };
  SFV fmtArpBpm = [](float v, int) {
    return juce::String(juce::roundToInt(v)) + " bpm";
  };
  SFV fmtTuning = [](float v, int) {
    double cents = (double)v * 100.0;
    if (cents >= 0.0) return "+" + juce::String(juce::roundToInt(cents)) + " cents";
    return juce::String(juce::roundToInt(cents)) + " cents";
  };
  SFV fmtPorta = [](float v, int) {
    if (v < 0.01f) return juce::String("Off");
    double t = (double)v * (double)v * (double)v * 3000.0;
    if (t >= 1000.0) return juce::String(t / 1000.0, 2) + " s";
    return juce::String(juce::roundToInt(t)) + " ms";
  };
  // Juno-6 slider→tau formulas (must match KR106_DSP_SetParam.h)
  auto j6AttackMs = [](float s) { return 0.001f * std::pow(3000.f, s) * 1000.f; };
  auto j6DecayMs  = [](float s) { return 0.004f * std::pow(1000.f, s) * 1000.f; };

  auto fmtMs = [this](const float* lut106, std::function<float(float)> j6Fn) -> SFV {
    return [this, lut106, j6Fn](float v, int) {
      bool j6 = mParams[kAdsrMode] && mParams[kAdsrMode]->getValue() < 0.5f;
      float ms = j6 ? j6Fn(v) : KR106DSP<float>::LookupLUT(lut106, v);
      if (ms >= 1000.f) return juce::String(ms / 1000.f, 2) + " s";
      return juce::String(juce::roundToInt(ms)) + " ms";
    };
  };

  // Bender sensitivity sliders
  addSlider(kBenderDco,  "Bender DCO",  0.f, 0.f, 1.f, fmtPct);
  addSlider(kBenderVcf,  "Bender VCF",  0.f, 0.f, 1.f, fmtPct);
  addSlider(kBenderLfo,  "Bender LFO",  0.f, 0.f, 1.f, fmtPct);

  // Arp rate (BPM range — DSP expects raw BPM value)
  {
    auto attrs = juce::AudioParameterFloatAttributes()
                   .withStringFromValueFunction(fmtArpBpm);
    auto* p = new juce::AudioParameterFloat(
      juce::ParameterID("p" + juce::String(kArpRate), 1), "Arp Rate",
      juce::NormalisableRange<float>(15.f, 1800.f, 0.1f, 0.3f), 120.f, attrs);
    addParameter(p);
    mParams[kArpRate] = p;
  }

  // LFO
  addSlider(kLfoRate,    "LFO Rate",    0.24f, 0.f, 1.f, fmtLfoRate);
  addSlider(kLfoDelay,   "LFO Delay",   0.f,   0.f, 1.f, fmtLfoDelay);

  // DCO
  addSlider(kDcoLfo,     "DCO LFO",     0.f, 0.f, 1.f, fmtPct);
  addSlider(kDcoPwm,     "DCO PWM",     0.f, 0.f, 1.f, fmtPct);
  addSlider(kDcoSub,     "DCO Sub",     1.f, 0.f, 1.f, fmtPct);
  addSlider(kDcoNoise,   "DCO Noise",   0.f, 0.f, 1.f, fmtPct);

  // HPF (4-position switch)
  SFI fmtHpf = [](int v, int) -> juce::String {
    constexpr const char* labels[] = { "Bass Boost", "Flat", "240 Hz", "720 Hz" };
    return labels[juce::jlimit(0, 3, v)];
  };
  addSwitch(kHpfFreq,    "HPF",         1, 0, 3, fmtHpf);

  // VCF
  addSlider(kVcfFreq,    "VCF Freq",    0.5f, 0.f, 1.f, fmtVcfHz);
  addSlider(kVcfRes,     "VCF Res",     0.f,  0.f, 1.f, fmtPct);
  addSlider(kVcfEnv,     "VCF Env",     0.f,  0.f, 1.f, fmtPct);
  addSlider(kVcfLfo,     "VCF LFO",     0.f,  0.f, 1.f, fmtPct);
  addSlider(kVcfKbd,     "VCF Kbd",     0.f,  0.f, 1.f, fmtPct);

  // VCA
  addSlider(kVcaLevel,   "Volume",      0.5f, 0.f, 1.f, fmtVcaLevel);

  // ADSR (raw 0-1 slider values; DSP applies curve + range via LUT)
  addSlider(kEnvA,       "Attack",      0.25f, 0.f, 1.f,
            fmtMs(KR106DSP<float>::kAttackLUT, j6AttackMs));
  addSlider(kEnvD,       "Decay",       0.25f, 0.f, 1.f,
            fmtMs(KR106DSP<float>::kDecayLUT, j6DecayMs));
  addSlider(kEnvS,       "Sustain",     0.9f,  0.f, 1.f, fmtPct);
  addSlider(kEnvR,       "Release",     0.25f, 0.f, 1.f,
            fmtMs(KR106DSP<float>::kReleaseLUT, j6DecayMs));

  // Toggle buttons
  addBool(kTranspose,    "Transpose",   false);
  addBool(kHold,         "Hold",        false);
  addBool(kArpeggio,     "Arpeggio",    false);
  addBool(kDcoPulse,     "Pulse",       true);
  addBool(kDcoSaw,       "Saw",         true);
  addBool(kDcoSubSw,     "Sub Sw",      false);
  addBool(kChorusOff,    "Chorus Off",  true);
  addBool(kChorusI,      "Chorus I",    false);
  addBool(kChorusII,     "Chorus II",   false);

  // Switches
  addSwitch(kOctTranspose, "Octave",      1, 0, 2);
  addSwitch(kArpMode,      "Arp Mode",    0, 0, 2);
  addSwitch(kArpRange,     "Arp Range",   0, 0, 2);
  addSwitch(kLfoMode,      "LFO Mode",    0, 0, 1);
  addSwitch(kPwmMode,      "PWM Mode",    1, 0, 2);
  addSwitch(kVcfEnvInv,    "VCF Env Inv", 0, 0, 1);
  addSwitch(kVcaMode,      "VCA Mode",    0, 0, 1);
  addSwitch(kAdsrMode,     "ADSR Mode",   1, 0, 1);

  // Special controls
  addSlider(kBender,       "Bender",      0.f, -1.f, 1.f);
  addSlider(kTuning,       "Tuning",      0.f, -1.f, 1.f, fmtTuning);
  addBool(kPower,          "Power",       true);
  addSwitch(kPortaMode,    "Porta Mode",  2, 0, 2);
  addSlider(kPortaRate,    "Porta Rate",  0.f, 0.f, 1.f, fmtPorta);
  addSwitch(kTransposeOffset, "Transpose Offset", 0, -24, 36);

  // Master volume knob (applied after scope, before output)
  SFV fmtMasterVol = [](float v, int) {
    if (v <= 0.f) return juce::String("-inf dB");
    double dB = 20.0 * std::log10((double)v);
    if (dB >= 0.0) return "+" + juce::String(dB, 1) + " dB";
    return juce::String(dB, 1) + " dB";
  };
  addSlider(kMasterVol, "Master Volume", 0.2f, 0.f, 1.f, fmtMasterVol);
}

void KR106AudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
  mDSP.Reset(sampleRate, samplesPerBlock);

  // Push all current param values to DSP
  for (int i = 0; i < kNumParams; i++)
  {
    float val = getParamValue(i);
    mLastParamValues[i] = val;
    parameterChanged(i, val);
  }

  // Re-trigger held notes after voice clear
  if (mDSP.mHold)
  {
    if (mDSP.mArp.mEnabled)
    {
      if (!mDSP.mArp.mHeldNotes.empty())
        mDSP.mArp.mPhase = 1.f;
    }
    else
    {
      for (int i = 0; i < 128; i++)
        if (mDSP.mHeldNotes.test(i))
          mDSP.SendToSynth(i, true, 127);
    }
  }
}

void KR106AudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                        juce::MidiBuffer& midiMessages)
{
  juce::ScopedNoDenormals noDenormals;

  int nFrames = buffer.getNumSamples();
  int nOutputs = std::min(getTotalNumOutputChannels(), 2);

  // Clear extra channels
  for (int c = nOutputs; c < getTotalNumOutputChannels(); c++)
    buffer.clear(c, 0, nFrames);

  // --- Sync parameter changes from JUCE → DSP ---
  for (int i = 0; i < kNumParams; i++)
  {
    float cur = getParamValue(i);
    if (cur != mLastParamValues[i])
    {
      mLastParamValues[i] = cur;
      parameterChanged(i, cur);
    }
  }

  // --- Hold-off: clear keyboard visual state ---
  if (mHoldOff.exchange(false))
    mKeyboardHeld.reset();

  // --- Drain force-release queue ---
  while (true)
  {
    int tail = mForceReleaseTail.load(std::memory_order_relaxed);
    if (tail == mForceReleaseHead.load(std::memory_order_acquire))
      break;
    int note = mForceReleaseQueue[tail];
    mDSP.ForceRelease(note);
    mKeyboardHeld.reset(note);
    mForceReleaseTail.store((tail + 1) % kForceReleaseQueueSize,
                            std::memory_order_release);
  }

  // --- Drain UI MIDI queue (keyboard, LFO trigger) ---
  while (true)
  {
    int tail = mUIMidiTail.load(std::memory_order_relaxed);
    if (tail == mUIMidiHead.load(std::memory_order_acquire))
      break;
    auto& evt = mUIMidiQueue[tail];
    if ((evt.status & 0xF0) == 0x90 && evt.data2 > 0)
    {
      mDSP.NoteOn(evt.data1, evt.data2);
      mKeyboardHeld.set(evt.data1);
    }
    else if ((evt.status & 0xF0) == 0x80 || ((evt.status & 0xF0) == 0x90 && evt.data2 == 0))
    {
      mDSP.NoteOff(evt.data1);
      if (!mDSP.mHold)
        mKeyboardHeld.reset(evt.data1);
    }
    else if ((evt.status & 0xF0) == 0xB0)
    {
      mDSP.ControlChange(evt.data1, evt.data2 / 127.f);
    }
    mUIMidiTail.store((tail + 1) % kUIMidiQueueSize, std::memory_order_release);
  }

  // --- Decode host MIDI ---
  for (const auto meta : midiMessages)
  {
    auto msg = meta.getMessage();

    if (msg.isNoteOn())
    {
      mDSP.NoteOn(msg.getNoteNumber(), msg.getVelocity());
      mKeyboardHeld.set(msg.getNoteNumber());
    }
    else if (msg.isNoteOff())
    {
      mDSP.NoteOff(msg.getNoteNumber());
      if (!mDSP.mHold)
        mKeyboardHeld.reset(msg.getNoteNumber());
    }
    else if (msg.isController())
    {
      int cc = msg.getControllerNumber();
      if (cc == 64) // sustain pedal → toggle Hold
      {
        bool pedalDown = msg.getControllerValue() >= 64;
        mParams[kHold]->setValueNotifyingHost(pedalDown ? 1.0f : 0.0f);
      }
      else
      {
        mDSP.ControlChange(cc, msg.getControllerValue() / 127.f);
      }
    }
    else if (msg.isPitchWheel())
    {
      float bend = (msg.getPitchWheelValue() - 8192) / 8192.f;
      mDSP.SetParam(kBender, static_cast<double>(bend));
    }
  }

  // --- Process DSP ---
  float* outputs[2] = { buffer.getWritePointer(0),
                         nOutputs > 1 ? buffer.getWritePointer(1) : buffer.getWritePointer(0) };
  mDSP.ProcessBlock(nullptr, outputs, nOutputs, nFrames);

  // --- Write scope ring buffer (before saturation, so display stays tall) ---
  {
    float* syncBuf = mDSP.GetSyncBuffer();
    int wp = mScopeWritePos.load(std::memory_order_relaxed);
    for (int i = 0; i < nFrames; i++)
    {
      mScopeRing[wp] = outputs[0][i];
      mScopeRingR[wp] = nOutputs > 1 ? outputs[1][i] : outputs[0][i];
      mScopeSyncRing[wp] = syncBuf[i];
      wp = (wp + 1) % kScopeRingSize;
    }
    mScopeWritePos.store(wp, std::memory_order_release);
  }

  // --- Master volume (after scope, before output) ---
  float masterVol = getParamValue(kMasterVol);
  for (int i = 0; i < nFrames; i++)
  {
    outputs[0][i] *= masterVol;
    if (nOutputs > 1) outputs[1][i] *= masterVol;
  }

  // --- Mute if power off ---
  if (!mPowerOn)
  {
    buffer.clear();
    return;
  }

  // --- Output saturation: Padé tanh(x*0.35) ---
  for (int c = 0; c < nOutputs; c++)
  {
    float* ch = buffer.getWritePointer(c);
    for (int i = 0; i < nFrames; i++)
    {
      float x = ch[i] * 0.35f;
      float x2 = x * x;
      ch[i] = x * (27.f + x2) / (27.f + 9.f * x2);
    }
  }
}

void KR106AudioProcessor::parameterChanged(int paramIdx, float newValue)
{
  if (paramIdx == kPower)
  {
    mPowerOn = newValue > 0.5f;
    if (!mPowerOn)
    {
      mDSP.PowerOff();
      mHoldOff = true;
    }
  }
  else if (paramIdx == kTransposeOffset)
  {
    mDSP.SetKeyTranspose(static_cast<int>(newValue));
  }
  else if (paramIdx == kMasterVol)
  {
    // handled in processBlock, not dispatched to DSP
  }
  else
  {
    mDSP.SetParam(paramIdx, static_cast<double>(newValue));
    if (paramIdx == kHold && newValue < 0.5f)
      mHoldOff = true;
  }
}

void KR106AudioProcessor::forceReleaseNote(int noteNum)
{
  int head = mForceReleaseHead.load(std::memory_order_relaxed);
  int next = (head + 1) % kForceReleaseQueueSize;
  if (next != mForceReleaseTail.load(std::memory_order_acquire))
  {
    mForceReleaseQueue[head] = noteNum;
    mForceReleaseHead.store(next, std::memory_order_release);
  }
}

void KR106AudioProcessor::sendMidiFromUI(uint8_t status, uint8_t data1, uint8_t data2)
{
  int head = mUIMidiHead.load(std::memory_order_relaxed);
  int next = (head + 1) % kUIMidiQueueSize;
  if (next != mUIMidiTail.load(std::memory_order_acquire))
  {
    mUIMidiQueue[head] = { status, data1, data2 };
    mUIMidiHead.store(next, std::memory_order_release);
  }
}

void KR106AudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
  juce::MemoryOutputStream stream(destData, true);
  stream.writeInt(kNumParams);
  for (int i = 0; i < kNumParams; i++)
    stream.writeFloat(getParamValue(i));
}

void KR106AudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
  juce::MemoryInputStream stream(data, static_cast<size_t>(sizeInBytes), false);
  int numParams = stream.readInt();
  if (numParams > kNumParams) numParams = kNumParams;

  for (int i = 0; i < numParams; i++)
  {
    float val = stream.readFloat();
    if (mParams[i] != nullptr)
      mParams[i]->setValueNotifyingHost(mParams[i]->convertTo0to1(val));
  }
}

// --- Program / Preset management ---

static bool isLivePerformanceParam(int idx)
{
  return idx == kTuning || idx == kTranspose || idx == kHold ||
         idx == kArpeggio || idx == kArpRate || idx == kArpMode || idx == kArpRange ||
         idx == kPortaMode || idx == kPortaRate || idx == kTransposeOffset ||
         idx == kMasterVol;
}

int KR106AudioProcessor::getNumPrograms()  { return kNumPresets; }
int KR106AudioProcessor::getCurrentProgram()  { return mCurrentPreset; }

const juce::String KR106AudioProcessor::getProgramName(int index)
{
  if (index >= 0 && index < kNumPresets)
    return kPresets[index].name;
  return {};
}

void KR106AudioProcessor::setCurrentProgram(int index)
{
  if (index < 0 || index >= kNumPresets) return;
  mCurrentPreset = index;

  for (int i = 0; i < kNumParams; i++)
  {
    if (isLivePerformanceParam(i)) continue;
    float denorm = kPresets[index].values[i];
    mParams[i]->setValueNotifyingHost(mParams[i]->convertTo0to1(denorm));
  }
}

juce::AudioProcessorEditor* KR106AudioProcessor::createEditor()
{
  return new KR106Editor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
  return new KR106AudioProcessor();
}
