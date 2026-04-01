#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "KR106_Presets_JUCE.h"
#include "DSP/KR106_HPF.h"
#include "DSP/KR106ParamValue.h"

// Debug logging to ~/Library/Application Support/KR106/debug.log
// Automatically disabled in Release builds (NDEBUG is set by CMake).
#ifdef NDEBUG
  #define KR106_DEBUG 0
#else
  #define KR106_DEBUG 1
#endif
#if KR106_DEBUG
static void dbgLog(const juce::String& msg)
{
  auto f = KR106PresetManager::getAppDataDir().getChildFile("debug.log");
  f.appendText(juce::Time::getCurrentTime().toString(true, true, true, true) + "  " + msg + "\n");
}
#else
static void dbgLog(const juce::String&) {}
#endif

static bool isLivePerformanceParam(int idx)
{
  return idx == kTuning || idx == kTranspose || idx == kHold ||
         idx == kArpeggio || idx == kArpRate || idx == kArpMode || idx == kArpRange ||
         idx == kPortaMode || idx == kPortaRate || idx == kTransposeOffset ||
         idx == kMasterVol || idx == kBender || idx == kBenderDco || idx == kBenderVcf ||
         idx == kBenderLfo || idx == kPower ||
         idx == kAdsrMode ||  // mode selects the bank, not a preset parameter
         (idx >= kSettingVoices && idx <= kSettingMidiSysEx);
}

static bool sExcludeMask[kNumParams] = {};
static bool sExcludeMaskInit = []() {
  for (int i = 0; i < kNumParams; i++)
    sExcludeMask[i] = isLivePerformanceParam(i);
  return true;
}();

// MIDI CC → EParams mapping (-1 = unmapped)
static constexpr int kCCtoParam[128] = {
  -1, -1, -1, kDcoLfo, -1, kPortaRate, -1, kMasterVol,  // CC 0-7
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

// Reverse mapping: EParams → MIDI CC (-1 = no CC for this param)
static int sParamToCC[kNumParams] = {};
static bool sParamToCCInit = []() {
  for (int i = 0; i < kNumParams; i++) sParamToCC[i] = -1;
  for (int cc = 0; cc < 128; cc++)
    if (kCCtoParam[cc] >= 0)
      sParamToCC[kCCtoParam[cc]] = cc;
  return true;
}();

// Reverse mapping: EParams → SysEx control (-1 = not a continuous SysEx param)
static int sParamToSysEx[kNumParams] = {};
static bool sParamToSysExInit = []() {
  for (int i = 0; i < kNumParams; i++) sParamToSysEx[i] = -1;
  for (int cc = 0; cc < 16; cc++)
    sParamToSysEx[kSysExToParam[cc]] = cc;
  return true;
}();

// Switch params packed into SysEx 0x10
static constexpr int kSw1Params[] = {
  kOctTranspose, kDcoPulse, kDcoSaw, kChorusOff, kChorusI
};
// Switch params packed into SysEx 0x11
static constexpr int kSw2Params[] = {
  kPwmMode, kVcfEnvInv, kVcaMode, kHpfFreq
};

KR106AudioProcessor::KR106AudioProcessor()
  : AudioProcessor(BusesProperties()
#if KR106_AUMF_BUILD
      .withInput("Input", juce::AudioChannelSet::stereo(), true)
#endif
      .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
  std::fill(std::begin(mParamCC), std::end(mParamCC), -1);

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

  using PV = kr106::ParamValue;

  auto isJ6 = [this]() { return mDSP.mSynthModel != kr106::kJ106; };

  SFV fmtVcfHz = [this, isJ6](float v, int) {
    float hz = PV::vcfFreqHz(v, isJ6());
    if (hz >= 1000.f) return juce::String(hz / 1000.f, 1) + " kHz";
    return juce::String(hz, 1) + " Hz";
  };
  VFS parseVcfHz = [this, bsearch, parseHz, isJ6](const juce::String& text) -> float {
    float hz = juce::jlimit(1.f, 20000.f, parseHz(text));
    return bsearch([isJ6](float v) { return PV::vcfFreqHz(v, isJ6()); }, hz);
  };
  SFV fmtLfoRate = [this, isJ6](float v, int) -> juce::String {
    if (mLfoSyncHost)
    {
      int d = kr106::lfoDivisionFromSlider(v);
      return juce::String(kr106::kLfoDivNames[d]);
    }
    return juce::String(PV::lfoRateHz(v, isJ6()), 1) + " Hz";
  };
  VFS parseLfoRate = [this, bsearch, parseHz, isJ6](const juce::String& text) -> float {
    if (mLfoSyncHost)
    {
      for (int d = 0; d < kr106::kNumLfoDivisions; d++)
        if (text.trim().equalsIgnoreCase(kr106::kLfoDivNames[d]))
          return kr106::sliderFromLfoDivision(d);
    }
    return bsearch([isJ6](float v) { return PV::lfoRateHz(v, isJ6()); }, parseHz(text));
  };
  SFV fmtLfoDelay = [](float v, int) {
    float ms = PV::lfoDelayMs(v);
    if (ms <= 0.f) return juce::String("Off");
    return juce::String(juce::roundToInt(ms)) + " ms";
  };
  VFS parseLfoDelay = [](const juce::String& text) -> float {
    return juce::jlimit(0.f, 1.f, text.getFloatValue() / 1500.f);
  };
  SFV fmtVcaLevel = [](float v, int) {
    float dB = PV::vcaLevelDb(v);
    if (dB >= 0.f) return "+" + juce::String(dB, 1) + " dB";
    return juce::String(dB, 1) + " dB";
  };
  VFS parseVcaLevel = [](const juce::String& text) -> float {
    double dB = text.getDoubleValue();
    return juce::jlimit(0.f, 1.f, static_cast<float>((dB + 10.0) / 20.0));
  };
  SFV fmtTuning = [](float v, int) {
    float cents = PV::tuningCents(v);
    if (cents >= 0.f) return "+" + juce::String(juce::roundToInt(cents)) + " cents";
    return juce::String(juce::roundToInt(cents)) + " cents";
  };
  VFS parseTuning = [](const juce::String& text) -> float {
    return juce::jlimit(-1.f, 1.f, text.getFloatValue() / 100.f);
  };
  SFV fmtPorta = [](float v, int) {
    float ms = PV::portaMsPerOct(v);
    if (ms <= 0.f) return juce::String("Off");
    if (ms >= 1000.f) return juce::String(ms / 1000.f, 2) + " s/oct";
    return juce::String(juce::roundToInt(ms)) + " ms/oct";
  };
  VFS parsePorta = [bsearch, parseMs](const juce::String& text) -> float {
    if (text.containsIgnoreCase("off")) return 0.f;
    float ms = parseMs(text);
    float lo = 0.001f, hi = 1.f;
    for (int i = 0; i < 32; i++)
    {
      float mid = (lo + hi) * 0.5f;
      float testMs = PV::portaMsPerOct(mid);
      if (testMs <= 0.f) testMs = 1e6f;
      if (testMs < ms) lo = mid; else hi = mid;
    }
    return (lo + hi) * 0.5f;
  };
  SFV fmtAtkMs = [this, isJ6](float v, int) -> juce::String {
    float ms = PV::attackMs(v, isJ6());
    if (ms >= 1000.f) return juce::String(ms / 1000.f, 2) + " s";
    return juce::String(juce::roundToInt(ms)) + " ms";
  };
  VFS parseAtkMs = [this, bsearch, parseMs, isJ6](const juce::String& text) -> float {
    float ms = parseMs(text);
    return bsearch([isJ6](float v) { return PV::attackMs(v, isJ6()); }, ms);
  };
  SFV fmtDRMs = [this, isJ6](float v, int) -> juce::String {
    float ms = PV::decRelMs(v, isJ6());
    if (ms >= 1000.f) return juce::String(ms / 1000.f, 2) + " s";
    return juce::String(juce::roundToInt(ms)) + " ms";
  };
  VFS parseDRMs = [this, bsearch, parseMs, isJ6](const juce::String& text) -> float {
    float ms = parseMs(text);
    return bsearch([isJ6](float v) { return PV::decRelMs(v, isJ6()); }, ms);
  };

  // Bender sensitivity sliders
  addSlider(kBenderDco,  "Bender DCO",  0.f, 0.f, 1.f, fmtPct, parsePct);
  addSlider(kBenderVcf,  "Bender VCF",  0.f, 0.f, 1.f, fmtPct, parsePct);
  addSlider(kBenderLfo,  "Bender LFO",  0.f, 0.f, 1.f, fmtPct, parsePct);

  addSlider(kArpRate, "Arp Rate", 30.f/128.f, 0.f, 1.f,
    [this](float v, int) -> juce::String {
      if (mArpSyncHost)
      {
        int d = kr106::divisionFromSlider(v);
        return juce::String(kr106::kDivNames[d]);
      }
      return juce::String(juce::roundToInt(PV::arpRateBpm(v))) + " bpm";
    },
    [this, bsearch](const juce::String& text) -> float {
      // When synced, try to match division name
      if (mArpSyncHost)
      {
        for (int d = 0; d < kr106::kNumArpDivisions; d++)
          if (text.trim().equalsIgnoreCase(kr106::kDivNames[d]))
            return kr106::sliderFromDivision(d);
      }
      return bsearch([](float v) { return kr106::Arpeggiator::arpRate(v); },
                     text.getFloatValue());
    });

  // LFO
  addSlider(kLfoRate,    "LFO Rate",    0.24f, 0.f, 1.f, fmtLfoRate, parseLfoRate);
  addSlider(kLfoDelay,   "LFO Delay",   0.f,   0.f, 1.f, fmtLfoDelay, parseLfoDelay);

  // DCO
  SFV fmtDcoLfo = [this, isJ6](float v, int) -> juce::String {
    float st = PV::dcoLfoSemitones(v, isJ6());
    if (st < 0.05f) return juce::String("Off");
    return juce::String(st, 1) + " st";
  };
  VFS parseDcoLfo = [this, bsearch, isJ6](const juce::String& text) -> float {
    if (text.containsIgnoreCase("off")) return 0.f;
    return bsearch([isJ6](float v) { return PV::dcoLfoSemitones(v, isJ6()); },
                   text.getFloatValue());
  };
  addSlider(kDcoLfo,     "DCO LFO",     0.f, 0.f, 1.f, fmtDcoLfo, parseDcoLfo);
  addSlider(kDcoPwm,     "DCO PWM",     0.5f, 0.f, 1.f, fmtPct, parsePct);
  addSlider(kDcoSub,     "DCO Sub",     1.f, 0.f, 1.f, fmtPct, parsePct);
  addSlider(kDcoNoise,   "DCO Noise",   0.f, 0.f, 1.f, fmtPct, parsePct);

  // HPF (4-position switch)
  SFV fmtHpf = [this](float v, int) -> juce::String {
    int pos = juce::jlimit(0, 3, juce::roundToInt(v));
    if (mDSP.mSynthModel == kr106::kJ60) {
      constexpr const char* labels[] = { "Flat", "122 Hz", "269 Hz", "571 Hz" };
      return juce::String(labels[pos]);
    }
    constexpr const char* labels[] = { "Bass Boost", "Flat", "236 Hz", "754 Hz" };
    return juce::String(labels[pos]);
  };
  addSlider(kHpfFreq,    "HPF",         1.f, 0.f, 3.f, fmtHpf);

  // VCF
  addSlider(kVcfFreq,    "VCF Freq",    1.f,  0.f, 1.f, fmtVcfHz, parseVcfHz);
  addSlider(kVcfRes,     "VCF Res",     0.f,  0.f, 1.f, fmtPct, parsePct);
  addSlider(kVcfEnv,     "VCF Env",     0.f,  0.f, 1.f, fmtPct, parsePct);
  SFV fmtVcfLfo = [this, isJ6](float v, int) -> juce::String {
    float st = PV::vcfLfoSemitones(v, isJ6());
    if (st < 0.5f) return juce::String("Off");
    return juce::String(st, 1) + " st";
  };
  VFS parseVcfLfo = [this, bsearch, isJ6](const juce::String& text) -> float {
    if (text.containsIgnoreCase("off")) return 0.f;
    return bsearch([isJ6](float v) { return PV::vcfLfoSemitones(v, isJ6()); },
                   text.getFloatValue());
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
    float dB = PV::masterVolDb(v);
    if (dB <= -200.f) return juce::String("-inf dB");
    if (dB >= 0.f) return "+" + juce::String(dB, 1) + " dB";
    return juce::String(dB, 1) + " dB";
  };
  VFS parseMasterVol = [](const juce::String& text) -> float {
    if (text.containsIgnoreCase("inf")) return 0.f;
    double dB = text.getDoubleValue();
    return juce::jlimit(0.f, 1.f, static_cast<float>(std::sqrt(std::pow(10.0, dB / 20.0))));
  };
  addSlider(kMasterVol, "Master Volume", 0.5f, 0.f, 1.f, fmtMasterVol, parseMasterVol);

  // Settings (exposed as params for headless LV2 hosts)
  addSwitch(kSettingVoices,      "Voices",             6, 6, 10,
    [](int v, int) {
      if (v == 6 || v == 8 || v == 10) return juce::String(v);
      return juce::String(6); // snap to nearest valid
    });
  addSwitch(kSettingOversample,  "VCF Oversample",     4, 2, 4,
    [](int v, int) { return juce::String(v) + "x"; });
  addBool(kSettingIgnoreVel,     "Ignore Velocity",    true);
  addBool(kSettingArpLimitKbd,   "Arp Limit Kbd",      true);
  addBool(kSettingArpSync,       "Arp Sync Host",      false);
  addBool(kSettingLfoSync,       "LFO Sync Host",      false);
  addBool(kSettingMonoRetrig,    "Mono Retrigger",     true);
  addBool(kSettingMidiSysEx,     "MIDI SysEx",         false);

  // Build factory presets from compiled-in data (raw 7-bit integer values)
  constexpr int kFactoryValues = sizeof(KR106FactoryPreset::values) / sizeof(int);
  for (int j = 0; j < kNumFactoryPresets; j++)
  {
    KR106Preset p;
    p.name = KR106PresetManager::stripFactoryPrefix(kFactoryPresets[j].name);
    p.values.assign(kNumParams, 0.f);
    for (int i = 0; i < kFactoryValues; i++)
    {
      int raw = kFactoryPresets[j].values[i];
      // 0-1 float params: convert raw 7-bit (0-127) to float (0.0-1.0)
      auto* fp = dynamic_cast<juce::AudioParameterFloat*>(mParams[i]);
      if (fp)
      {
        auto range = fp->getNormalisableRange();
        if (range.start == 0.f && range.end == 1.f)
          p.values[i] = raw / 127.f;
        else
          p.values[i] = static_cast<float>(raw);
      }
      else
        p.values[i] = static_cast<float>(raw);
    }
    if (kMasterVol < kNumParams)
      p.values[kMasterVol] = 0.5f;
    mPresetMgr.mPresets.push_back(std::move(p));
  }
  mPresetMgr.initializeFromDisk(mParams, kNumParams, sExcludeMask);
  loadGlobalSettings();
}

void KR106AudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
  dbgLog("prepareToPlay: sr=" + juce::String(sampleRate) + " bs=" + juce::String(samplesPerBlock));

  // Log held notes before Reset
  juce::String heldBefore;
  for (int i = 0; i < 128; i++)
    if (mDSP.mHeldNotes.test(i)) heldBefore += juce::String(i) + " ";
  dbgLog("  heldNotes before Reset: [" + heldBefore.trim() + "]");
  dbgLog("  mHold=" + juce::String(mDSP.mHold ? 1 : 0)
       + " portaMode=" + juce::String(mDSP.mPortaMode)
       + " arpEnabled=" + juce::String(mDSP.mArp.mEnabled ? 1 : 0));

  // Log voice allocation before Reset
  juce::String voicesBefore;
  for (int i = 0; i < mDSP.mActiveVoices; i++)
    voicesBefore += juce::String(mDSP.mVoiceNote[i]) + " ";
  dbgLog("  voiceNote before Reset: [" + voicesBefore.trim() + "]");

  mDSP.Reset(sampleRate, samplesPerBlock);

  // Log voice allocation after Reset
  juce::String voicesAfter;
  for (int i = 0; i < mDSP.mActiveVoices; i++)
    voicesAfter += juce::String(mDSP.mVoiceNote[i]) + " ";
  dbgLog("  voiceNote after Reset: [" + voicesAfter.trim() + "]");

  // Push all current param values to DSP
  for (int i = 0; i < kNumParams; i++)
  {
    float val = getParamValue(i);
    mLastParamValues[i] = val;
    parameterChanged(i, val);
  }

  dbgLog("  mHold after param push=" + juce::String(mDSP.mHold ? 1 : 0));

  // Re-trigger held notes after voice clear
  if (mDSP.mHold)
  {
    juce::String retriggered;
    if (mDSP.mArp.mEnabled)
    {
      if (!mDSP.mArp.mHeldNotes.empty())
        mDSP.mArp.mPhase = 1.f;
      dbgLog("  arp re-trigger, arpHeldNotes=" + juce::String((int)mDSP.mArp.mHeldNotes.size()));
    }
    else
    {
      for (int i = 0; i < 128; i++)
      {
        if (mDSP.mHeldNotes.test(i))
        {
          retriggered += juce::String(i) + " ";
          mDSP.SendToSynth(i, true, 127);
        }
      }
      dbgLog("  re-triggered held notes: [" + retriggered.trim() + "]");
    }

    // Log voice allocation after re-trigger
    juce::String voicesRetrig;
    for (int i = 0; i < mDSP.mActiveVoices; i++)
      voicesRetrig += juce::String(mDSP.mVoiceNote[i]) + " ";
    dbgLog("  voiceNote after re-trigger: [" + voicesRetrig.trim() + "]");
  }
  else
  {
    dbgLog("  mHold=0, no re-trigger");
  }
}

void KR106AudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                        juce::MidiBuffer& midiMessages)
{
  juce::ScopedNoDenormals noDenormals;

  static bool firstBlock = true;
  if (firstBlock) { dbgLog("processBlock FIRST CALL"); firstBlock = false; }

  int nFrames = buffer.getNumSamples();
  int nOutputs = std::min(getTotalNumOutputChannels(), 2);

  // Clear extra channels
  for (int c = nOutputs; c < getTotalNumOutputChannels(); c++)
    buffer.clear(c, 0, nFrames);

  // Outgoing MIDI buffer — collected separately so we don't corrupt
  // the host's midiMessages while iterating it for note events.
  juce::MidiBuffer midiOut;

  // --- Sync parameter changes from JUCE → DSP + emit MIDI ---
  // Skip MIDI emission during preset change (preset dump covers it)
  bool presetChange = mSendPresetSysEx.load(std::memory_order_relaxed);
  for (int i = 0; i < kNumParams; i++)
  {
    float cur = getParamValue(i);
    if (cur != mLastParamValues[i])
    {
      mLastParamValues[i] = cur;
      parameterChanged(i, cur);

      // Emit MIDI for individual param changes (not preset load)
      if (!presetChange)
      {
        // First edit after preset load: send 0x31 (manual mode) once
        if (mPresetClean && mMidiOutSysEx && !isLivePerformanceParam(i))
        {
          mPresetClean = false;
          // Build and send APR with 0x31 (manual mode, current params)
          uint8_t apr[24];
          apr[0] = 0xF0; apr[1] = 0x41; apr[2] = 0x31; apr[3] = 0x00;
          apr[4] = 0x00; // no patch number in manual mode
          for (int cc = 0; cc < 16; cc++)
            apr[5 + cc] = static_cast<uint8_t>(juce::roundToInt(getParamValue(kSysExToParam[cc]) * 127.f));
          {
            int oct = juce::roundToInt(getParamValue(kOctTranspose));
            uint8_t sw1 = 0;
            if (oct == 2) sw1 |= 0x04;
            else if (oct == 1) sw1 |= 0x02;
            else sw1 |= 0x01;
            if (getParamValue(kDcoPulse) > 0.5f) sw1 |= 0x08;
            if (getParamValue(kDcoSaw) > 0.5f)   sw1 |= 0x10;
            bool cI = getParamValue(kChorusI) > 0.5f;
            bool cII = getParamValue(kChorusII) > 0.5f;
            if (!cI && !cII) sw1 |= 0x20;
            if (cI) sw1 |= 0x40;
            apr[21] = sw1;
          }
          {
            uint8_t sw2 = 0;
            if (juce::roundToInt(getParamValue(kPwmMode)) != 0) sw2 |= 0x01;
            if (getParamValue(kVcfEnvInv) > 0.5f) sw2 |= 0x02;
            if (juce::roundToInt(getParamValue(kVcaMode)) != 0) sw2 |= 0x04;
            int hpf = juce::roundToInt(getParamValue(kHpfFreq));
            sw2 |= static_cast<uint8_t>((3 - hpf) << 3);
            apr[22] = sw2;
          }
          apr[23] = 0xF7;
          midiOut.addEvent(apr, 24, 0);
        }

        if (mMidiOutSysEx)
        {
          // SysEx mode: IPR (Individual Parameter) messages
          int sx = sParamToSysEx[i];
          if (sx >= 0)
          {
            uint8_t val = static_cast<uint8_t>(juce::roundToInt(cur * 127.f));
            uint8_t sysex[] = { 0xF0, 0x41, 0x32, 0x00,
                                 static_cast<uint8_t>(sx), val, 0xF7 };
            midiOut.addEvent(sysex, 7, 0);
          }

          // Switch byte 0x10: octave, pulse, saw, chorus
          for (int s : kSw1Params)
          {
            if (i == s)
            {
              int oct = juce::roundToInt(getParamValue(kOctTranspose));
              uint8_t sw1 = 0;
              if (oct == 2) sw1 |= 0x04;
              else if (oct == 1) sw1 |= 0x02;
              else sw1 |= 0x01;
              if (getParamValue(kDcoPulse) > 0.5f) sw1 |= 0x08;
              if (getParamValue(kDcoSaw) > 0.5f)   sw1 |= 0x10;
              bool chorusI   = getParamValue(kChorusI)   > 0.5f;
              bool chorusII  = getParamValue(kChorusII)  > 0.5f;
              if (!chorusI && !chorusII) sw1 |= 0x20;
              if (chorusI) sw1 |= 0x40;
              uint8_t sysex[] = { 0xF0, 0x41, 0x32, 0x00, 0x10, sw1, 0xF7 };
              midiOut.addEvent(sysex, 7, 0);
              break;
            }
          }

          // Switch byte 0x11: PWM mode, VCF polarity, VCA mode, HPF
          for (int s : kSw2Params)
          {
            if (i == s)
            {
              uint8_t sw2 = 0;
              if (juce::roundToInt(getParamValue(kPwmMode)) != 0) sw2 |= 0x01; // bit=1 is MAN/ENV
              if (getParamValue(kVcfEnvInv) > 0.5f) sw2 |= 0x02;
              if (juce::roundToInt(getParamValue(kVcaMode)) != 0) sw2 |= 0x04; // bit=1 is GATE
              int hpf = juce::roundToInt(getParamValue(kHpfFreq));
              sw2 |= static_cast<uint8_t>((3 - hpf) << 3);
              uint8_t sysex[] = { 0xF0, 0x41, 0x32, 0x00, 0x11, sw2, 0xF7 };
              midiOut.addEvent(sysex, 7, 0);
              break;
            }
          }
        }
        else
        {
          // CC mode: standard MIDI CC for param changes
          int cc = sParamToCC[i];
          if (cc >= 0)
          {
            int ccVal = (i == kHpfFreq)
                ? juce::roundToInt(cur) * 32  // 0-3 -> 0/32/64/96
                : juce::roundToInt(cur * 127.f);
            midiOut.addEvent(juce::MidiMessage::controllerEvent(1, cc,
                juce::jlimit(0, 127, ccVal)), 0);
          }
        }
      }
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

  // --- Log all MIDI events ---
  for (const auto meta : midiMessages)
  {
    auto msg = meta.getMessage();
    auto raw = msg.getRawData();
    int len = msg.getRawDataSize();
    juce::String hex;
    for (int b = 0; b < std::min(len, 8); b++)
      hex += juce::String::toHexString(raw[b]).paddedLeft('0', 2) + " ";
    dbgLog("MIDI pos=" + juce::String(meta.samplePosition) + " len=" + juce::String(len) + " " + hex.trim());
  }

  // --- Handle MIDI program changes first (before notes) ---
  for (const auto meta : midiMessages)
  {
    auto msg = meta.getMessage();
    if (msg.isProgramChange())
    {
      dbgLog("PC pgm=" + juce::String(msg.getProgramChangeNumber())
           + " samplePos=" + juce::String(meta.samplePosition)
           + " blockSize=" + juce::String(nFrames));
      setCurrentProgram(msg.getProgramChangeNumber());
      for (int i = 0; i < kNumParams; i++)
      {
        if (!mParams[i]) continue;
        float cur = mParams[i]->getValue();
        mLastParamValues[i] = cur;
        float denorm = mParams[i]->convertFrom0to1(cur);
        parameterChanged(i, denorm);
      }
    }
  }

  // --- Decode host MIDI ---
  for (const auto meta : midiMessages)
  {
    auto msg = meta.getMessage();
    if (msg.isProgramChange()) continue; // already handled above

    if (msg.isNoteOn())
    {
      dbgLog("NoteOn note=" + juce::String(msg.getNoteNumber())
           + " vel=" + juce::String(msg.getVelocity())
           + " samplePos=" + juce::String(meta.samplePosition)
           + " blockSize=" + juce::String(nFrames));
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

      // MIDI learn: capture first CC received while learning
      int learnParam = mMidiLearnParam.load(std::memory_order_acquire);
      if (learnParam >= 0)
      {
        mParamCC[learnParam] = cc;
        mMidiLearnResult.store(cc, std::memory_order_release);
        mMidiLearnParam.store(-1, std::memory_order_release);
        continue;
      }

      // Apply user CC mappings (multiple params can share a CC)
      bool userHandled = false;
      for (int i = 0; i < kNumParams; i++)
      {
        if (mParamCC[i] == cc)
        {
          mParams[i]->setValueNotifyingHost(msg.getControllerValue() / 127.f);
          userHandled = true;
        }
      }
      // Hardcoded mappings: sustain pedal, mod wheel, and CC→param table.
      // Skip the hardcoded map if the target param has a user MIDI learn assignment
      // (the user assignment replaces the default for that param).
      if (cc == 120 || cc == 123) // All Sound Off / All Notes Off
      {
        mDSP.AllNotesOff();
        mKeyboardHeld.reset();
      }
      else if (cc == 64) // sustain pedal → toggle Hold
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
        if (mParamCC[param] >= 0) {} // param has user assignment — skip default
        else if (param == kHpfFreq)
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
      auto* data = msg.getSysExData();
      int len = msg.getSysExDataSize();
      if (len >= 4 && data[0] == 0x41)
      {
        int cmd = data[1];

        if (cmd == 0x32 && len >= 5)
        {
          // IPR (Individual Parameter): F0 41 32 0n cc vv F7
          int ctrl = data[3];
          int val  = data[4];
          if (ctrl <= 0x0F)
          {
            mParams[kSysExToParam[ctrl]]->setValueNotifyingHost(val / 127.f);
            // J106 has no sub switch; infer from sub level
            if (ctrl == 0x0F)
              mParams[kDcoSubSw]->setValueNotifyingHost(val > 0 ? 1.f : 0.f);
          }
          else if (ctrl == 0x10)
            decodeSwitches1(val);
          else if (ctrl == 0x11)
            decodeSwitches2(val);
        }
        else if ((cmd == 0x30 || cmd == 0x31) && len >= 21)
        {
          // APR: F0 41 30 0n pp [16 sliders] [sw1] [sw2] F7
          // 0x30 = bank/patch change, 0x31 = manual mode
          int patchNum = data[3];
          const uint8_t* p = data + 4;

          // Set all 18 params
          for (int cc = 0; cc < 16; cc++)
            mParams[kSysExToParam[cc]]->setValueNotifyingHost(p[cc] / 127.f);
          decodeSwitches1(p[16]);
          decodeSwitches2(p[17]);
          // J106 has no sub switch; infer from sub level (cc 0x0F)
          mParams[kDcoSubSw]->setValueNotifyingHost(p[0x0F] > 0 ? 1.f : 0.f);

          if (cmd == 0x30)
          {
            // Bank/patch change: select the patch slot
            int numPresets = getNumPrograms();
            if (patchNum >= 0 && patchNum < numPresets)
              mCurrentPreset = patchNum;
            mInitialDefault = false;
          }
          else
          {
            // Manual mode: no preset selected
            mInitialDefault = true;
          }
        }
      }
    }
  }

  // --- Drain UI MIDI queue (keyboard, LFO trigger) → DSP + MIDI output ---
  // Done after host MIDI loop so echoed events aren't double-processed
  while (true)
  {
    int tail = mUIMidiTail.load(std::memory_order_relaxed);
    if (tail == mUIMidiHead.load(std::memory_order_acquire))
      break;
    auto& evt = mUIMidiQueue[tail];
    if ((evt.status & 0xF0) == 0x90 && evt.data2 > 0)
    {
      dbgLog("UI NoteOn " + juce::String(evt.data1) + " hold=" + juce::String(mDSP.mHold ? 1 : 0)
           + " arp=" + juce::String(mDSP.mArp.mEnabled ? 1 : 0) + " arpNotes=" + juce::String((int)mDSP.mArp.mHeldNotes.size()));
      mDSP.NoteOn(evt.data1, evt.data2);
      mKeyboardHeld.set(evt.data1);
    }
    else if ((evt.status & 0xF0) == 0x80 || ((evt.status & 0xF0) == 0x90 && evt.data2 == 0))
    {
      dbgLog("UI NoteOff " + juce::String(evt.data1) + " hold=" + juce::String(mDSP.mHold ? 1 : 0)
           + " arp=" + juce::String(mDSP.mArp.mEnabled ? 1 : 0) + " arpNotes=" + juce::String((int)mDSP.mArp.mHeldNotes.size())
           + " heldNotes=" + juce::String((int)mDSP.mHeldNotes.count()));
      mDSP.NoteOff(evt.data1);
      if (!mDSP.mHold)
        mKeyboardHeld.reset(evt.data1);
    }
    else if ((evt.status & 0xF0) == 0xB0)
    {
      mDSP.ControlChange(evt.data1, evt.data2 / 127.f);
    }
    // Echo to MIDI output
    midiOut.addEvent(juce::MidiMessage(evt.status, evt.data1, evt.data2), 0);
    mUIMidiTail.store((tail + 1) % kUIMidiQueueSize, std::memory_order_release);
  }

  // --- Host sync for arpeggiator and LFO ---
  // LV2 hosts (Jalv/Zynthian) only send transport atoms on state changes,
  // not every cycle. Cache the last known state and extrapolate beat position
  // when the host doesn't provide fresh data.
  mDSP.mArp.mSyncToHost = mArpSyncHost;
  mDSP.mLFO.mSyncToHost = mLfoSyncHost;
  if (mArpSyncHost || mLfoSyncHost)
  {
    if (auto* ph = getPlayHead())
    {
      if (auto pos = ph->getPosition())
      {
        // Fresh transport data from host -- update cache
        mCachedPlaying = pos->getIsPlaying();
        if (auto b = pos->getBpm())         { mCachedBPM = *b; mCachedHaveBPM = true; }
        if (auto p = pos->getPpqPosition()) { mCachedPPQ = *p; mCachedHavePPQ = true; }
      }
      else if (mCachedPlaying && mCachedHaveBPM)
      {
        // No fresh data but transport was running -- extrapolate beat position
        double beatsPerSample = mCachedBPM / (60.0 * getSampleRate());
        mCachedPPQ += beatsPerSample * nFrames;
      }
    }

    if (mArpSyncHost)
    {
      mDSP.mArp.mSyncToHost = mCachedHaveBPM;
      mDSP.mArp.mHostPlaying = mCachedPlaying && mCachedHavePPQ;
      mDSP.mArp.mHostBPM = mCachedBPM;
      mDSP.mArp.mHostBeatPos = mCachedPPQ;
    }
    if (mLfoSyncHost)
    {
      mDSP.mLFO.mSyncToHost = mCachedHaveBPM;
      mDSP.mLFO.mHostPlaying = mCachedPlaying;
      mDSP.mLFO.mHostBPM = mCachedBPM;
    }
  }

  // --- Process DSP ---
  float* outputs[2] = { buffer.getWritePointer(0),
                         nOutputs > 1 ? buffer.getWritePointer(1) : buffer.getWritePointer(0) };
  float vol = getParamValue(kMasterVol);
  mDSP.mMasterVol = vol * vol; // audio taper (squared)
  mDSP.ProcessBlock(nullptr, outputs, nOutputs, nFrames);

  // --- Mute if power off ---
  if (!mPowerOn)
  {
    buffer.clear();
    return;
  }

  // --- Peak detection (pre-saturator) for clip LED ---
  {
    float peak = 0.f;
    for (int c = 0; c < nOutputs; c++)
    {
      const float* ch = buffer.getReadPointer(c);
      for (int i = 0; i < nFrames; i++)
        peak = std::max(peak, std::abs(ch[i]));
    }
    // Hold peak across blocks — UI timer decays it
    float prev = mPeakLevel.load(std::memory_order_relaxed);
    if (peak > prev)
      mPeakLevel.store(peak, std::memory_order_relaxed);
  }

  // --- Write scope ring buffer ---
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

  // --- Emit MIDI on preset change ---
  if (mSendPresetSysEx.exchange(false, std::memory_order_acquire))
  {
    if (mMidiOutSysEx)
    {
      // SysEx mode: APR (All Parameter Report), matches real Juno-106
      // F0 41 30 0n pp [16 sliders] [sw1] [sw2] F7 = 24 bytes
      uint8_t apr[24];
      apr[0] = 0xF0;
      apr[1] = 0x41;
      apr[2] = 0x30;             // APR command
      apr[3] = 0x00;             // channel
      apr[4] = static_cast<uint8_t>(mCurrentPreset & 0x7F);

      for (int cc = 0; cc < 16; cc++)
        apr[5 + cc] = static_cast<uint8_t>(juce::roundToInt(getParamValue(kSysExToParam[cc]) * 127.f));

      {
        int oct = juce::roundToInt(getParamValue(kOctTranspose));
        uint8_t sw1 = 0;
        if (oct == 2) sw1 |= 0x04;
        else if (oct == 1) sw1 |= 0x02;
        else sw1 |= 0x01;
        if (getParamValue(kDcoPulse) > 0.5f) sw1 |= 0x08;
        if (getParamValue(kDcoSaw) > 0.5f)   sw1 |= 0x10;
        bool chorusI  = getParamValue(kChorusI)  > 0.5f;
        bool chorusII = getParamValue(kChorusII) > 0.5f;
        if (!chorusI && !chorusII) sw1 |= 0x20;
        if (chorusI) sw1 |= 0x40;
        apr[21] = sw1;
      }

      {
        uint8_t sw2 = 0;
        int pwm = juce::roundToInt(getParamValue(kPwmMode));
        if (pwm != 0) sw2 |= 0x01; // bit=1 is MAN/ENV
        if (getParamValue(kVcfEnvInv) > 0.5f) sw2 |= 0x02;
        if (juce::roundToInt(getParamValue(kVcaMode)) != 0) sw2 |= 0x04; // bit=1 is GATE
        int hpf = juce::roundToInt(getParamValue(kHpfFreq));
        sw2 |= static_cast<uint8_t>((3 - hpf) << 3);
        apr[22] = sw2;
      }

      apr[23] = 0xF7;
      midiOut.addEvent(apr, 24, 0);
      mPresetClean = true;
    }
    else
    {
      // CC mode: Program Change for preset selection
      midiOut.addEvent(juce::MidiMessage::programChange(1, mCurrentPreset & 0x7F), 0);
      mPresetClean = true;
    }
  }

  // --- Merge outgoing MIDI into the host buffer ---
  midiMessages.addEvents(midiOut, 0, nFrames, 0);
}

void KR106AudioProcessor::decodeSwitches1(uint8_t val)
{
    int oct = (val & 0x04) ? 2 : (val & 0x02) ? 1 : 0;
    mParams[kOctTranspose]->setValueNotifyingHost(mParams[kOctTranspose]->convertTo0to1(static_cast<float>(oct)));
    mParams[kDcoPulse]->setValueNotifyingHost((val & 0x08) ? 1.f : 0.f);
    mParams[kDcoSaw]->setValueNotifyingHost((val & 0x10) ? 1.f : 0.f);
    bool chorusOn = !(val & 0x20);
    bool chorusL1 = (val & 0x40) != 0;
    mParams[kChorusOff]->setValueNotifyingHost(chorusOn ? 0.f : 1.f);
    mParams[kChorusI]->setValueNotifyingHost((chorusOn && chorusL1) ? 1.f : 0.f);
    mParams[kChorusII]->setValueNotifyingHost((chorusOn && !chorusL1) ? 1.f : 0.f);
}

void KR106AudioProcessor::decodeSwitches2(uint8_t val)
{
    mParams[kPwmMode]->setValueNotifyingHost(mParams[kPwmMode]->convertTo0to1(static_cast<float>((val & 0x01) ? 1 : 0))); // bit=1 is MAN/ENV
    mParams[kVcfEnvInv]->setValueNotifyingHost((val & 0x02) ? 1.f : 0.f);
    mParams[kVcaMode]->setValueNotifyingHost((val & 0x04) ? 1.f : 0.f); // bit=1 is GATE
    int hpf = 3 - ((val >> 3) & 0x03);
    mParams[kHpfFreq]->setValueNotifyingHost(mParams[kHpfFreq]->convertTo0to1(static_cast<float>(hpf)));
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
  else if (paramIdx == kSettingVoices)
  {
    // Snap to valid values: 6, 8, or 10
    int v = juce::roundToInt(newValue);
    if (v <= 7) v = 6;
    else if (v <= 9) v = 8;
    else v = 10;
    mVoiceCount = v;
    mDSP.SetActiveVoices(v);
  }
  else if (paramIdx == kSettingOversample)
  {
    int os = juce::roundToInt(newValue) <= 3 ? 2 : 4;
    mVcfOversample = os;
    mDSP.ForEachVoice([os](kr106::Voice<float>& voice) {
      voice.mVCF.SetOversample(os);
    });
  }
  else if (paramIdx == kSettingIgnoreVel)
  {
    mIgnoreVelocity = newValue > 0.5f;
    mDSP.mIgnoreVelocity = mIgnoreVelocity;
  }
  else if (paramIdx == kSettingArpLimitKbd)
  {
    mArpLimitKbd = newValue > 0.5f;
    mDSP.mArp.mLimitToKeyboard = mArpLimitKbd;
  }
  else if (paramIdx == kSettingArpSync)
  {
    mArpSyncHost = newValue > 0.5f;
  }
  else if (paramIdx == kSettingLfoSync)
  {
    mLfoSyncHost = newValue > 0.5f;
  }
  else if (paramIdx == kSettingMonoRetrig)
  {
    mMonoRetrigger = newValue > 0.5f;
    mDSP.mMonoRetrigger = mMonoRetrigger;
  }
  else if (paramIdx == kSettingMidiSysEx)
  {
    mMidiOutSysEx = newValue > 0.5f;
  }
  else
  {
    if (paramIdx == kHold)
    {
      juce::String heldNotes;
      for (int i = 0; i < 128; i++)
        if (mDSP.mHeldNotes.test(i)) heldNotes += juce::String(i) + " ";
      juce::String voiceNotes;
      for (int i = 0; i < mDSP.mActiveVoices; i++)
        voiceNotes += juce::String(mDSP.mVoiceNote[i]) + " ";
      dbgLog("parameterChanged kHold=" + juce::String(newValue)
           + " mHold=" + juce::String(mDSP.mHold ? 1 : 0)
           + " heldNotes=[" + heldNotes.trim() + "]"
           + " voiceNote=[" + voiceNotes.trim() + "]"
           + " portaMode=" + juce::String(mDSP.mPortaMode));
    }
    mDSP.SetParam(paramIdx, static_cast<double>(newValue));

    // Remap params when switching modes (preserve Hz/ms values, not slider positions)
    // Only remap if the mode actually changed (not just reloaded same value)
    if (paramIdx == kAdsrMode && (newValue > 0.5f) != mWasJ106Mode)
    {
      bool toJ106 = (newValue > 0.5f);
      mWasJ106Mode = toJ106;

      // Switch to Manual mode (no preset loaded)
      mInitialDefault = true;

      // Binary search helper: find slider position on targetFn that matches srcHz
      auto remap = [](float srcHz,
                      std::function<float(float)> targetFn) -> float {
        float lo = 0.f, hi = 1.f;
        for (int i = 0; i < 32; i++)
        {
          float mid = (lo + hi) * 0.5f;
          if (targetFn(mid) < srcHz) lo = mid; else hi = mid;
        }
        return (lo + hi) * 0.5f;
      };

      auto j60Hz  = [](float v) { return kr106::j60_vcf_freq_from_slider(v); };
      auto j106Hz = [](float v) { return kr106::dacToHz(static_cast<uint16_t>(v * 0x3F80)); };

      // VCF Freq: preserve frequency in Hz
      {
        float vcfSlider = mParams[kVcfFreq]->getValue();
        float srcHz = toJ106 ? j60Hz(vcfSlider) : j106Hz(vcfSlider);
        auto targetFn = toJ106 ? std::function<float(float)>(j106Hz)
                                : std::function<float(float)>(j60Hz);
        mParams[kVcfFreq]->setValueNotifyingHost(remap(srcHz, targetFn));
      }

      // HPF: remap positions across models
      // J106 pos 0/1 (boost/flat) -> J60 pos 0 (flat)
      // J60 pos 0 (flat) -> J106 pos 1 (flat, not boost)
      {
        int hpf = juce::roundToInt(getParamValue(kHpfFreq));
        if (!toJ106 && hpf <= 1)
          mParams[kHpfFreq]->setValueNotifyingHost(0.f / 3.f);
        else if (toJ106 && hpf == 0)
          mParams[kHpfFreq]->setValueNotifyingHost(1.f / 3.f);
      }
    }

    // Keep kChorusOff in sync: clear it when either chorus is engaged
    if ((paramIdx == kChorusI || paramIdx == kChorusII) && newValue > 0.5f)
    {
      if (getParamValue(kChorusOff) > 0.5f)
        mParams[kChorusOff]->setValueNotifyingHost(0.f);
    }

    if (paramIdx == kHold)
    {
      juce::String heldAfter;
      for (int i = 0; i < 128; i++)
        if (mDSP.mHeldNotes.test(i)) heldAfter += juce::String(i) + " ";
      juce::String voicesAfter;
      for (int i = 0; i < mDSP.mActiveVoices; i++)
        voicesAfter += juce::String(mDSP.mVoiceNote[i]) + " ";
      dbgLog("  after SetParam: mHold=" + juce::String(mDSP.mHold ? 1 : 0)
           + " heldNotes=[" + heldAfter.trim() + "]"
           + " voiceNote=[" + voicesAfter.trim() + "]");
      if (newValue < 0.5f)
        mHoldOff = true;
    }
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
  // Voice count, velocity, arp limit etc. are now params (kSetting*) — saved above
}

void KR106AudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
  mInitialDefault = false;
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
    // Old state format stored voice count, velocity, arp limit here.
    // New format stores them as params (kSetting*), already restored above.
    // Consume leftover bytes from old format so we don't misparse.
    if (!stream.isExhausted())
    {
      int oldVoices = stream.readInt();
      // If settings params were at default (old state predates kSetting*),
      // apply the legacy values
      if (numParams < kSettingVoices + 1)
      {
        mVoiceCount = oldVoices;
        mDSP.SetActiveVoices(mVoiceCount);
      }
    }
    if (!stream.isExhausted())
    {
      bool oldIgnVel = stream.readBool();
      if (numParams < kSettingIgnoreVel + 1)
      {
        mIgnoreVelocity = oldIgnVel;
        mDSP.mIgnoreVelocity = mIgnoreVelocity;
      }
    }
    if (!stream.isExhausted())
    {
      bool oldArpLimit = stream.readBool();
      if (numParams < kSettingArpLimitKbd + 1)
      {
        mArpLimitKbd = oldArpLimit;
        mDSP.mArp.mLimitToKeyboard = mArpLimitKbd;
      }
    }
  }
}

// --- Program / Preset management ---

int KR106AudioProcessor::getNumPrograms()
{
  return 128; // each bank has 128 slots
}

int KR106AudioProcessor::getCurrentProgram()  { return mCurrentPreset; }

const juce::String KR106AudioProcessor::getProgramName(int index)
{
  return mPresetMgr.getDisplayName(index + presetBankOffset());
}

void KR106AudioProcessor::setCurrentProgram(int index)
{
  int absIndex = index + presetBankOffset();
  dbgLog("setCurrentProgram index=" + juce::String(index) + " abs=" + juce::String(absIndex));

  // Copy values out under lock
  float values[kNumParams] = {};
  {
    std::lock_guard<std::mutex> lock(mPresetMgr.mMutex);
    if (absIndex < 0 || absIndex >= (int)mPresetMgr.mPresets.size()) return;
    auto& src = mPresetMgr.mPresets[absIndex].values;
    int n = std::min((int)src.size(), (int)kNumParams);
    for (int i = 0; i < n; i++)
      values[i] = src[i];
  }

  mCurrentPreset = index;
  mInitialDefault = false;

  for (int i = 0; i < kNumParams; i++)
  {
    if (isLivePerformanceParam(i)) continue;
    mParams[i]->setValueNotifyingHost(mParams[i]->convertTo0to1(values[i]));
  }

  mSendPresetSysEx.store(true, std::memory_order_release);
}

void KR106AudioProcessor::setCurrentProgramIndex(int index)
{
  mCurrentPreset = index;
  mInitialDefault = false;
}

void KR106AudioProcessor::reloadPresetsFromFile(const juce::File& file)
{
  mPresetMgr.loadFromFile(file, mParams, kNumParams);
  mPresetMgr.setActiveCSVPath(file);
  int numPresets = getNumPrograms();
  if (mCurrentPreset >= numPresets)
    mCurrentPreset = 0;
}

void KR106AudioProcessor::saveCurrentPresetToCSV(const juce::String& name)
{
  int abs = absPresetIndex();
  mPresetMgr.captureCurrentParams(abs, name, mParams, kNumParams);
  mPresetMgr.saveOnePreset(abs, mPresetMgr.getActiveCSVPath(), mParams, kNumParams, sExcludeMask);
}

void KR106AudioProcessor::renameCurrentPreset(const juce::String& name)
{
  int abs = absPresetIndex();
  mPresetMgr.renamePreset(abs, name);
  mPresetMgr.saveOnePreset(abs, mPresetMgr.getActiveCSVPath(), mParams, kNumParams, sExcludeMask);
}

void KR106AudioProcessor::clearCurrentPreset()
{
  int abs = absPresetIndex();
  mPresetMgr.clearPreset(abs, kNumParams);
  mPresetMgr.saveOnePreset(abs, mPresetMgr.getActiveCSVPath(), mParams, kNumParams, sExcludeMask);
  setCurrentProgram(mCurrentPreset);
}

void KR106AudioProcessor::pastePreset(const KR106Preset& preset)
{
  int abs = absPresetIndex();
  mPresetMgr.setPreset(abs, preset);
  mPresetMgr.saveOnePreset(abs, mPresetMgr.getActiveCSVPath(), mParams, kNumParams, sExcludeMask);
  setCurrentProgram(mCurrentPreset);
}

KR106Preset KR106AudioProcessor::getPreset(int idx) const
{
  return mPresetMgr.getPreset(idx + presetBankOffset());
}

bool KR106AudioProcessor::isCurrentPresetDirty() const
{
  auto preset = mPresetMgr.getPreset(absPresetIndex());
  for (int i = 0; i < kNumParams; i++)
  {
    if (isLivePerformanceParam(i)) continue;
    float stored = (i < (int)preset.values.size()) ? preset.values[i] : 0.f;
    float current = mParams[i]->convertFrom0to1(mParams[i]->getValue());
    if (std::abs(current - stored) > 1e-4f)
      return true;
  }
  return false;
}

void KR106AudioProcessor::loadGlobalSettings()
{
  auto scale = KR106PresetManager::getSetting("uiScale", 0.f);
  if ((float)scale > 0.f) mUIScale = (float)scale;

  // Load settings into params (parameterChanged will sync to DSP)
  auto setParamFromSetting = [this](int idx, float val) {
    auto* p = mParams[idx];
    if (p) p->setValueNotifyingHost(p->convertTo0to1(val));
  };
  setParamFromSetting(kSettingVoices,      static_cast<float>((int)KR106PresetManager::getSetting("voiceCount", 6)));
  setParamFromSetting(kSettingOversample,  static_cast<float>(((int)KR106PresetManager::getSetting("vcfOversample", 4) == 2) ? 2 : 4));
  setParamFromSetting(kSettingIgnoreVel,   (bool)KR106PresetManager::getSetting("ignoreVelocity", true)  ? 1.f : 0.f);
  setParamFromSetting(kSettingArpLimitKbd, (bool)KR106PresetManager::getSetting("arpLimitKbd", true)     ? 1.f : 0.f);
  setParamFromSetting(kSettingArpSync,     (bool)KR106PresetManager::getSetting("arpSyncHost", false)    ? 1.f : 0.f);
  setParamFromSetting(kSettingLfoSync,     (bool)KR106PresetManager::getSetting("lfoSyncHost", false)    ? 1.f : 0.f);
  setParamFromSetting(kSettingMonoRetrig,  (bool)KR106PresetManager::getSetting("monoRetrigger", true)   ? 1.f : 0.f);
  setParamFromSetting(kSettingMidiSysEx,   (bool)KR106PresetManager::getSetting("midiOutSysEx", false)   ? 1.f : 0.f);

  // Load MIDI learn CC map (param→CC, multiple params can share a CC)
  auto ccMap = KR106PresetManager::getSetting("midiLearnMap");
  if (ccMap.isObject())
  {
    auto* obj = ccMap.getDynamicObject();
    for (auto& prop : obj->getProperties())
    {
      int param = prop.name.toString().getIntValue();
      int cc = (int)prop.value;
      if (param >= 0 && param < kNumParams && cc >= 0 && cc < 128)
        mParamCC[param] = cc;
    }
  }

  // Load per-voice variance values (if saved)
  auto variance = KR106PresetManager::getSetting("voiceVariance");
  if (variance.isArray())
  {
    auto* arr = variance.getArray();
    int idx = 0;
    for (int v = 0; v < 10 && idx < arr->size(); v++)
      for (int p = 0; p < 6 && idx < arr->size(); p++, idx++)
        mDSP.GetVoice(v)->SetVariance(p, static_cast<float>((double)(*arr)[idx]));
  }
}

void KR106AudioProcessor::saveGlobalSettings()
{
  KR106PresetManager::saveSetting("uiScale", mUIScale);
  KR106PresetManager::saveSetting("voiceCount", mVoiceCount);
  KR106PresetManager::saveSetting("ignoreVelocity", mIgnoreVelocity);
  KR106PresetManager::saveSetting("arpLimitKbd", mArpLimitKbd);
  KR106PresetManager::saveSetting("arpSyncHost", mArpSyncHost);
  KR106PresetManager::saveSetting("lfoSyncHost", mLfoSyncHost);
  KR106PresetManager::saveSetting("monoRetrigger", mMonoRetrigger);
  KR106PresetManager::saveSetting("midiOutSysEx", mMidiOutSysEx);
  KR106PresetManager::saveSetting("vcfOversample", mVcfOversample);

  // Save per-voice variance as flat array [v0p0, v0p1, ..., v9p5]
  juce::Array<juce::var> varArr;
  for (int v = 0; v < 10; v++)
    for (int p = 0; p < 6; p++)
      varArr.add(static_cast<double>(mDSP.GetVoice(v)->GetVariance(p)));
  KR106PresetManager::saveSetting("voiceVariance", varArr);

  // Save MIDI learn CC map (param→CC, only non-empty entries)
  auto* ccObj = new juce::DynamicObject();
  for (int i = 0; i < kNumParams; i++)
    if (mParamCC[i] >= 0)
      ccObj->setProperty(juce::String(i), mParamCC[i]);
  KR106PresetManager::saveSetting("midiLearnMap", juce::var(ccObj));
}

// --- MIDI Learn ---

void KR106AudioProcessor::startMidiLearn(int paramIdx)
{
  mMidiLearnResult.store(-1, std::memory_order_relaxed);
  mMidiLearnParam.store(paramIdx, std::memory_order_release);
}

void KR106AudioProcessor::cancelMidiLearn()
{
  mMidiLearnParam.store(-1, std::memory_order_relaxed);
  mMidiLearnResult.store(-1, std::memory_order_relaxed);
}

void KR106AudioProcessor::clearMidiLearn(int paramIdx)
{
  if (paramIdx >= 0 && paramIdx < kNumParams)
    mParamCC[paramIdx] = -1;
  saveGlobalSettings();
}

int KR106AudioProcessor::getCCForParam(int paramIdx) const
{
  // User map takes priority
  if (paramIdx >= 0 && paramIdx < kNumParams && mParamCC[paramIdx] >= 0)
    return mParamCC[paramIdx];
  // Fall back to hardcoded map
  for (int cc = 0; cc < 128; cc++)
    if (kCCtoParam[cc] == paramIdx) return cc;
  return -1;
}

juce::AudioProcessorEditor* KR106AudioProcessor::createEditor()
{
  return new KR106Editor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
  return new KR106AudioProcessor();
}
