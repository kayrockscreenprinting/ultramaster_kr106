#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "KR106_Presets_JUCE.h"

// MIDI CC → EParams mapping (-1 = unmapped)
static constexpr int kCCtoParam[128] = {
  -1, -1, -1, kDcoLfo, -1, kPortaRate, -1, kVcaLevel,  // CC 0-7
  -1, kDcoSub, -1, -1, kHpfFreq, kDcoNoise, -1, -1,       // CC 8-15
  -1, -1, -1, -1, -1, -1, -1, -1,                       // CC 16-23
  -1, -1, -1, -1, -1, -1, -1, -1,                       // CC 24-31
  -1, -1, -1, -1, -1, -1, -1, -1,                       // CC 32-39
  -1, -1, -1, -1, -1, -1, -1, -1,                       // CC 40-47
  -1, -1, -1, -1, -1, -1, -1, -1,                       // CC 48-55
  -1, -1, -1, -1, -1, -1, -1, -1,                       // CC 56-63
  -1, -1, -1, -1, -1, -1, kEnvS, kVcfRes,               // CC 64-71
  kEnvR, kEnvA, kVcfFreq, kEnvD, kVcfEnv, kVcfLfo,      // CC 72-77
  kVcfKbd, kDcoPwm, kLfoRate, kLfoDelay, kArpRate, -1,   // CC 78-83
  -1, -1, -1, -1, -1, -1, -1, -1,                       // CC 84-91
  -1, -1, -1, -1, -1, -1, -1, -1,                       // CC 92-99
  -1, -1, -1, -1, -1, -1, -1, -1,                       // CC 100-107
  -1, -1, -1, -1, -1, -1, -1, -1,                       // CC 108-115
  -1, -1, -1, -1, -1, -1, -1, -1,                       // CC 116-123
  -1, -1, -1, -1,                                        // CC 124-127
};

// Roland Juno-106 SysEx control → EParams mapping (0x00–0x0F)
static constexpr int kSysExToParam[16] = {
  kLfoRate, kLfoDelay, kDcoLfo, kDcoPwm,    // 0x00-0x03
  kDcoNoise, kVcfFreq, kVcfRes, kVcfEnv,    // 0x04-0x07
  kVcfLfo, kVcfKbd, kVcaLevel, kEnvA,       // 0x08-0x0B
  kEnvD, kEnvS, kEnvR, kDcoSub,             // 0x0C-0x0F
};

KR106AudioProcessor::KR106AudioProcessor()
  : AudioProcessor(BusesProperties()
      .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
  // --- Sliders (0–1 range) ---
  using SFV = std::function<juce::String(float, int)>;
  using VFS = std::function<float(const juce::String&)>;
  auto addSlider = [this](int idx, const char* name, float def,
                          float min = 0.f, float max = 1.f,
                          SFV fmt = nullptr, VFS vfs = nullptr) {
    // For 0-1 sliders, append 7-bit SysEx value for debugging
    SFV displayFmt = std::move(fmt);
    if (min == 0.f && max == 1.f)
    {
      displayFmt = [f = std::move(displayFmt)](float v, int maxLen) {
        juce::String base = f ? f(v, maxLen)
                              : juce::String(juce::roundToInt(v * 100.f)) + "%";
        return base + " [" + juce::String(juce::roundToInt(v * 127.f)) + "]";
      };
    }
    auto attrs = juce::AudioParameterFloatAttributes();
    if (displayFmt) attrs = attrs.withStringFromValueFunction(std::move(displayFmt));
    if (vfs) attrs = attrs.withValueFromStringFunction(std::move(vfs));
    auto* p = new juce::AudioParameterFloat(
      juce::ParameterID("p" + juce::String(idx), 1), name,
      juce::NormalisableRange<float>(min, max), def, attrs);
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
  VFS parsePct = [](const juce::String& text) -> float {
    return juce::jlimit(0.f, 1.f, text.getFloatValue() / 100.f);
  };

  // Generic binary search: find slider value (0-1) that produces target output
  // from a monotonically increasing slider→value function.
  auto bsearch = [](std::function<float(float)> fn, float target) -> float {
    float lo = 0.f, hi = 1.f;
    for (int i = 0; i < 32; i++)
    {
      float mid = (lo + hi) * 0.5f;
      if (fn(mid) < target) lo = mid; else hi = mid;
    }
    return (lo + hi) * 0.5f;
  };

  // Parse text with "k" suffix support (e.g. "2 kHz" → 2000)
  auto parseHz = [](const juce::String& text) {
    float hz = text.getFloatValue();
    if (text.containsIgnoreCase("k")) hz *= 1000.f;
    return hz;
  };
  // Parse text with "s" suffix (e.g. "1.5 s" → 1500 ms)
  auto parseMs = [](const juce::String& text) {
    float ms = text.getFloatValue();
    if (text.containsIgnoreCase("s") && !text.containsIgnoreCase("ms")) ms *= 1000.f;
    return ms;
  };

  SFV fmtVcfHz = [this](float v, int) {
    float hz;
    if (mDSP.mAdsrMode == 0)
      hz = kr106::j6_vcf_freq_from_slider(v);
    else
      hz = kr106::dacToHz(static_cast<uint16_t>(v * 0x3F80));
    if (hz >= 1000.f) return juce::String(hz / 1000.f, 1) + " kHz";
    return juce::String(static_cast<int>(hz)) + " Hz";
  };
  VFS parseVcfHz = [this, bsearch, parseHz](const juce::String& text) -> float {
    float hz = juce::jlimit(1.f, 20000.f, parseHz(text));
    return bsearch([this](float v) {
      return (mDSP.mAdsrMode == 0)
          ? kr106::j6_vcf_freq_from_slider(v)
          : kr106::dacToHz(static_cast<uint16_t>(v * 0x3F80));
    }, hz);
  };
  SFV fmtLfoRate = [this](float v, int) {
    float hz = (mDSP.mAdsrMode == 0) ? kr106::LFO::lfoFreqJ6(v)
                                      : kr106::LFO::lfoFreqJ106(v);
    return juce::String(hz, 1) + " Hz";
  };
  VFS parseLfoRate = [this, bsearch, parseHz](const juce::String& text) -> float {
    return bsearch([this](float v) {
      return (mDSP.mAdsrMode == 0) ? kr106::LFO::lfoFreqJ6(v)
                                    : kr106::LFO::lfoFreqJ106(v);
    }, parseHz(text));
  };
  SFV fmtLfoDelay = [](float v, int) {
    if (v <= 0.f) return juce::String("Off");
    return juce::String(juce::roundToInt(v * 1500.f)) + " ms";
  };
  VFS parseLfoDelay = [](const juce::String& text) -> float {
    return juce::jlimit(0.f, 1.f, text.getFloatValue() / 1500.f);
  };
  SFV fmtVcaLevel = [](float v, int) {
    double dB = ((double)v * 2.0 - 1.0) * 6.0;
    if (dB >= 0.0) return "+" + juce::String(dB, 1) + " dB";
    return juce::String(dB, 1) + " dB";
  };
  VFS parseVcaLevel = [](const juce::String& text) -> float {
    return juce::jlimit(0.f, 1.f, static_cast<float>(text.getDoubleValue() / 12.0 + 0.5));
  };
  SFV fmtTuning = [](float v, int) {
    double cents = (double)v * 100.0;
    if (cents >= 0.0) return "+" + juce::String(juce::roundToInt(cents)) + " cents";
    return juce::String(juce::roundToInt(cents)) + " cents";
  };
  VFS parseTuning = [](const juce::String& text) -> float {
    return juce::jlimit(-1.f, 1.f, text.getFloatValue() / 100.f);
  };
  SFV fmtPorta = [](float v, int) {
    float semiPerSec = kr106::Voice<float>::portaRate(v);
    if (semiPerSec <= 0.f) return juce::String("Off");
    // Time for one octave (12 semitones)
    double ms = 12000.0 / (double)semiPerSec;
    if (ms >= 1000.0) return juce::String(ms / 1000.0, 2) + " s/oct";
    return juce::String(juce::roundToInt(ms)) + " ms/oct";
  };
  VFS parsePorta = [bsearch, parseMs](const juce::String& text) -> float {
    if (text.containsIgnoreCase("off")) return 0.f;
    float ms = parseMs(text);
    // porta display is ms/oct = 12000/portaRate; portaRate is monotonically
    // DECREASING (high slider = slow = large ms), so search in reverse
    float lo = 0.001f, hi = 1.f;
    for (int i = 0; i < 32; i++)
    {
      float mid = (lo + hi) * 0.5f;
      float rate = kr106::Voice<float>::portaRate(mid);
      float testMs = (rate > 0.f) ? 12000.f / rate : 1e6f;
      if (testMs < ms) lo = mid; else hi = mid;
    }
    return (lo + hi) * 0.5f;
  };
  using ADSR = kr106::ADSR;
  auto fmtAtkMs = [this](float v, int) -> juce::String {
    bool j6 = mParams[kAdsrMode] && mParams[kAdsrMode]->getValue() < 0.5f;
    float ms = j6 ? 0.001500f * std::exp(11.7382f * v + -4.7207f * v * v) * 1791.8f : ADSR::AttackMs(v);
    if (ms >= 1000.f) return juce::String(ms / 1000.f, 2) + " s";
    return juce::String(juce::roundToInt(ms)) + " ms";
  };
  VFS parseAtkMs = [this, bsearch, parseMs](const juce::String& text) -> float {
    float ms = parseMs(text);
    return bsearch([this](float v) {
      bool j6 = mParams[kAdsrMode] && mParams[kAdsrMode]->getValue() < 0.5f;
      return j6 ? 0.001500f * std::exp(11.7382f * v + -4.7207f * v * v) * 1791.8f
                : ADSR::AttackMs(v);
    }, ms);
  };
  auto fmtDRMs = [this](float v, int) -> juce::String {
    bool j6 = mParams[kAdsrMode] && mParams[kAdsrMode]->getValue() < 0.5f;
    float ms = j6 ? 0.003577f * std::exp(12.9460f * v + -5.0638f * v * v) * 9966.f : ADSR::DecRelMs(v);
    if (ms >= 1000.f) return juce::String(ms / 1000.f, 2) + " s";
    return juce::String(juce::roundToInt(ms)) + " ms";
  };
  VFS parseDRMs = [this, bsearch, parseMs](const juce::String& text) -> float {
    float ms = parseMs(text);
    return bsearch([this](float v) {
      bool j6 = mParams[kAdsrMode] && mParams[kAdsrMode]->getValue() < 0.5f;
      return j6 ? 0.003577f * std::exp(12.9460f * v + -5.0638f * v * v) * 9966.f
                : ADSR::DecRelMs(v);
    }, ms);
  };

  // Bender sensitivity sliders
  addSlider(kBenderDco,  "Bender DCO",  0.f, 0.f, 1.f, fmtPct, parsePct);
  addSlider(kBenderVcf,  "Bender VCF",  0.f, 0.f, 1.f, fmtPct, parsePct);
  addSlider(kBenderLfo,  "Bender LFO",  0.f, 0.f, 1.f, fmtPct, parsePct);

  addSlider(kArpRate, "Arp Rate", 30.f/128.f, 0.f, 1.f, [](float v, int) {
    return juce::String(juce::roundToInt(kr106::Arpeggiator::arpRate(v))) + " bpm";
  }, [bsearch](const juce::String& text) -> float {
    return bsearch([](float v) { return kr106::Arpeggiator::arpRate(v); },
                   text.getFloatValue());
  });

  // LFO
  addSlider(kLfoRate,    "LFO Rate",    0.24f, 0.f, 1.f, fmtLfoRate, parseLfoRate);
  addSlider(kLfoDelay,   "LFO Delay",   0.f,   0.f, 1.f, fmtLfoDelay, parseLfoDelay);

  // DCO
  SFV fmtDcoLfo = [this](float v, int) -> juce::String {
    float st = (mDSP.mAdsrMode == 0) ? kr106::Voice<float>::dcoLfoDepth6(v)
                                      : kr106::Voice<float>::dcoLfoDepth106(v);
    if (st < 0.05f) return juce::String("Off");
    return juce::String(st, 1) + " st";
  };
  VFS parseDcoLfo = [this, bsearch](const juce::String& text) -> float {
    if (text.containsIgnoreCase("off")) return 0.f;
    return bsearch([this](float v) {
      return (mDSP.mAdsrMode == 0) ? kr106::Voice<float>::dcoLfoDepth6(v)
                                    : kr106::Voice<float>::dcoLfoDepth106(v);
    }, text.getFloatValue());
  };
  addSlider(kDcoLfo,     "DCO LFO",     0.f, 0.f, 1.f, fmtDcoLfo, parseDcoLfo);
  addSlider(kDcoPwm,     "DCO PWM",     0.5f, 0.f, 1.f, fmtPct, parsePct);
  addSlider(kDcoSub,     "DCO Sub",     1.f, 0.f, 1.f, fmtPct, parsePct);
  addSlider(kDcoNoise,   "DCO Noise",   0.f, 0.f, 1.f, fmtPct, parsePct);

  // HPF (4-position switch)
  SFI fmtHpf = [](int v, int) -> juce::String {
    constexpr const char* labels[] = { "Bass Boost", "Flat", "240 Hz", "720 Hz" };
    return labels[juce::jlimit(0, 3, v)];
  };
  addSwitch(kHpfFreq,    "HPF",         1, 0, 3, fmtHpf);

  // VCF
  addSlider(kVcfFreq,    "VCF Freq",    1.f,  0.f, 1.f, fmtVcfHz, parseVcfHz);
  addSlider(kVcfRes,     "VCF Res",     0.f,  0.f, 1.f, fmtPct, parsePct);
  addSlider(kVcfEnv,     "VCF Env",     0.f,  0.f, 1.f, fmtPct, parsePct);
  SFV fmtVcfLfo = [this](float v, int) -> juce::String {
    float st = (mDSP.mAdsrMode == 0) ? kr106::Voice<float>::vcfLfoDepth6(v)
                                      : kr106::Voice<float>::vcfLfoDepth106(v);
    if (st < 0.5f) return juce::String("Off");
    return juce::String(st, 1) + " st";
  };
  VFS parseVcfLfo = [this, bsearch](const juce::String& text) -> float {
    if (text.containsIgnoreCase("off")) return 0.f;
    return bsearch([this](float v) {
      return (mDSP.mAdsrMode == 0) ? kr106::Voice<float>::vcfLfoDepth6(v)
                                    : kr106::Voice<float>::vcfLfoDepth106(v);
    }, text.getFloatValue());
  };
  addSlider(kVcfLfo,     "VCF LFO",     0.f,  0.f, 1.f, fmtVcfLfo, parseVcfLfo);
  addSlider(kVcfKbd,     "VCF Kbd",     0.f,  0.f, 1.f, fmtPct, parsePct);

  // VCA
  addSlider(kVcaLevel,   "Volume",      0.5f, 0.f, 1.f, fmtVcaLevel, parseVcaLevel);

  // ADSR (raw 0-1 slider values; DSP applies curve + range via LUT)
  addSlider(kEnvA,       "Attack",      0.25f, 0.f, 1.f, fmtAtkMs, parseAtkMs);
  addSlider(kEnvD,       "Decay",       0.25f, 0.f, 1.f, fmtDRMs, parseDRMs);
  addSlider(kEnvS,       "Sustain",     0.9f,  0.f, 1.f, fmtPct, parsePct);
  addSlider(kEnvR,       "Release",     0.25f, 0.f, 1.f, fmtDRMs, parseDRMs);

  // Toggle buttons
  addBool(kTranspose,    "Transpose",   false);
  addBool(kHold,         "Hold",        false);
  addBool(kArpeggio,     "Arpeggio",    false);
  addBool(kDcoPulse,     "Pulse",       true);
  addBool(kDcoSaw,       "Saw",         true);
  addBool(kDcoSubSw,     "Sub Sw",      true);
  addBool(kChorusOff,    "Chorus Off",  false);
  addBool(kChorusI,      "Chorus I",    true);
  addBool(kChorusII,     "Chorus II",   false);

  // Switches
  addSwitch(kOctTranspose, "Octave",      1, 0, 2);
  addSwitch(kArpMode,      "Arp Mode",    0, 0, 2);
  addSwitch(kArpRange,     "Arp Range",   0, 0, 2);
  addSwitch(kLfoMode,      "LFO Mode",    0, 0, 1);
  addSwitch(kPwmMode,      "PWM Mode",    1, 0, 2);
  addSwitch(kVcfEnvInv,    "VCF Env Inv", 0, 0, 1);
  addSwitch(kVcaMode,      "VCA Mode",    1, 0, 1);
  addSwitch(kAdsrMode,     "ADSR Mode",   1, 0, 1);

  // Special controls
  addSlider(kBender,       "Bender",      0.f, -1.f, 1.f);
  addSlider(kTuning,       "Tuning",      0.f, -1.f, 1.f, fmtTuning, parseTuning);
  addBool(kPower,          "Power",       true);
  addSwitch(kPortaMode,    "Porta Mode",  2, 0, 2);
  addSlider(kPortaRate,    "Porta Rate",  0.f, 0.f, 1.f, fmtPorta, parsePorta);
  addSwitch(kTransposeOffset, "Transpose Offset", 0, -24, 36);

  // Master volume knob (applied after scope, before output)
  SFV fmtMasterVol = [](float v, int) {
    if (v <= 0.f) return juce::String("-inf dB");
    double dB = 20.0 * std::log10((double)v);
    if (dB >= 0.0) return "+" + juce::String(dB, 1) + " dB";
    return juce::String(dB, 1) + " dB";
  };
  VFS parseMasterVol = [](const juce::String& text) -> float {
    if (text.containsIgnoreCase("inf")) return 0.f;
    double dB = text.getDoubleValue();
    return juce::jlimit(0.f, 1.f, static_cast<float>(std::pow(10.0, dB / 20.0)));
  };
  addSlider(kMasterVol, "Master Volume", 0.5f, 0.f, 1.f, fmtMasterVol, parseMasterVol);
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
      else if (cc == 1)
      {
        mDSP.ControlChange(1, msg.getControllerValue() / 127.f);
        mLfoTriggered.store(msg.getControllerValue() > 0, std::memory_order_relaxed);
      }
      else if (kCCtoParam[cc] >= 0)
      {
        int param = kCCtoParam[cc];
        if (param == kHpfFreq)
        {
          int hpf = msg.getControllerValue() / 32; // 0-31→0, 32-63→1, 64-95→2, 96-127→3
          if (hpf > 3) hpf = 3;
          mParams[param]->setValueNotifyingHost(mParams[param]->convertTo0to1(static_cast<float>(hpf)));
        }
        else
          mParams[param]->setValueNotifyingHost(msg.getControllerValue() / 127.f);
      }
    }
    else if (msg.isPitchWheel())
    {
      float bend = (msg.getPitchWheelValue() - 8192) / 8192.f;
      mParams[kBender]->setValueNotifyingHost(bend * 0.5f + 0.5f);
    }
    else if (msg.isSysEx())
    {
      // Roland Juno-106 SysEx: F0 41 32 0n cc vv F7
      auto* data = msg.getSysExData();
      int len = msg.getSysExDataSize();
      if (len >= 4 && data[0] == 0x41 && data[1] == 0x32)
      {
        int ctrl = data[3];
        int val  = (len >= 5) ? data[4] : 0;
        if (ctrl <= 0x0F)
          mParams[kSysExToParam[ctrl]]->setValueNotifyingHost(val / 127.f);
        else if (ctrl == 0x10)
        {
          // Switches 1: octave, pulse, saw, chorus
          int oct = (val & 0x04) ? 2 : (val & 0x02) ? 1 : 0; // bit2=4', bit1=8', bit0=16'
          mParams[kOctTranspose]->setValueNotifyingHost(mParams[kOctTranspose]->convertTo0to1(static_cast<float>(oct)));
          mParams[kDcoPulse]->setValueNotifyingHost((val & 0x08) ? 1.f : 0.f);
          mParams[kDcoSaw]->setValueNotifyingHost((val & 0x10) ? 1.f : 0.f);
          bool chorusOn = !(val & 0x20); // bit5: 0 = chorus on
          bool chorusL1 = (val & 0x40);  // bit6: 1 = level 1, 0 = level 2
          mParams[kChorusOff]->setValueNotifyingHost(chorusOn ? 0.f : 1.f);
          mParams[kChorusI]->setValueNotifyingHost((chorusOn && chorusL1) ? 1.f : 0.f);
          mParams[kChorusII]->setValueNotifyingHost((chorusOn && !chorusL1) ? 1.f : 0.f);
        }
        else if (ctrl == 0x11)
        {
          // Switches 2: PWM mode, VCF polarity, VCA mode, HPF
          mParams[kPwmMode]->setValueNotifyingHost(mParams[kPwmMode]->convertTo0to1(static_cast<float>((val & 0x01) ? 0 : 1)));
          mParams[kVcfEnvInv]->setValueNotifyingHost((val & 0x02) ? 1.f : 0.f);
          mParams[kVcaMode]->setValueNotifyingHost((val & 0x04) ? 0.f : 1.f); // 0=GATE, 1=ENV
          int hpf = 3 - ((val >> 3) & 0x03); // 00=3, 01=2, 10=1, 11=0
          mParams[kHpfFreq]->setValueNotifyingHost(mParams[kHpfFreq]->convertTo0to1(static_cast<float>(hpf)));
        }
      }
    }
  }

  // --- Process DSP ---
  float* outputs[2] = { buffer.getWritePointer(0),
                         nOutputs > 1 ? buffer.getWritePointer(1) : buffer.getWritePointer(0) };
  mDSP.mMasterVol = getParamValue(kMasterVol);
  mDSP.ProcessBlock(nullptr, outputs, nOutputs, nFrames);

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
      float x = ch[i] * 0.65f;
      float x2 = x * x;
      ch[i] = x * (27.f + x2) / (27.f + 9.f * x2);
    }
  }

  // --- Write scope ring buffer (after volume and saturation) ---
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

  // UI state (backwards-compatible: old hosts just ignore extra bytes)
  stream.writeInt(0x4B523130); // 'KR10' magic
  stream.writeFloat(mUIScale);
  stream.writeInt(mVoiceCount);
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

  // Read UI state if present (magic marker check for backwards compatibility)
  if (!stream.isExhausted() && stream.readInt() == 0x4B523130)
  {
    mUIScale = stream.readFloat();
    if (!stream.isExhausted())
    {
      mVoiceCount = stream.readInt();
      mDSP.SetActiveVoices(mVoiceCount);
    }
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
