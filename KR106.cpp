#include "KR106.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"
#include "KR106Controls.h"

KR106::KR106(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  // --- Display formatters ---
  auto dispPct = [](double v, WDL_String& s) { s.SetFormatted(32, "%.0f%%", v * 100.0); };

  auto dispVcfHz = [](double v, WDL_String& s) {
    double hz = 20.0 * std::pow(900.0, v);
    if (hz >= 1000.0) s.SetFormatted(32, "%.1f kHz", hz / 1000.0);
    else s.SetFormatted(32, "%.0f Hz", hz);
  };

  auto dispMs = [this](const float* lut106, const float* lut6) {
    return [this, lut106, lut6](double v, WDL_String& s) {
      const float* lut = GetParam(kAdsrMode)->Bool() ? lut106 : lut6; // 0=J6, 1=J106
      float ms = KR106DSP<float>::LookupLUT(lut, static_cast<float>(v));
      if (ms >= 1000.f) s.SetFormatted(32, "%.2f s", ms / 1000.f);
      else s.SetFormatted(32, "%.0f ms", ms);
    };
  };

  auto dispLfoRate = [](double v, WDL_String& s) {
    double hz = (18.0 + v * 1182.0) / 60.0;
    s.SetFormatted(32, "%.1f Hz", hz);
  };

  auto dispLfoDelay = [](double v, WDL_String& s) {
    if (v <= 0.0) s.Set("Off");
    else s.SetFormatted(32, "%.0f ms", v * 1500.0);
  };

  auto dispVcaLevel = [](double v, WDL_String& s) {
    double dB = (v * 2.0 - 1.0) * 6.0;
    if (dB >= 0.0) s.SetFormatted(32, "+%.1f dB", dB);
    else s.SetFormatted(32, "%.1f dB", dB);
  };

  auto dispArpBpm = [](double v, WDL_String& s) {
    s.SetFormatted(32, "%.0f bpm", v);
  };

  auto dispTuning = [](double v, WDL_String& s) {
    double cents = v * 100.0;
    if (cents >= 0.0) s.SetFormatted(32, "+%.0f cents", cents);
    else s.SetFormatted(32, "%.0f cents", cents);
  };

  auto dispPorta = [](double v, WDL_String& s) {
    if (v < 0.01) { s.Set("Off"); return; }
    double t = v * v * v * 3.0 * 1000.0; // ms
    if (t >= 1000.0) s.SetFormatted(32, "%.2f s", t / 1000.0);
    else s.SetFormatted(32, "%.0f ms", t);
  };

  // --- Sliders ---
  GetParam(kBenderDco)->InitDouble("Bender DCO", 0., 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispPct);
  GetParam(kBenderVcf)->InitDouble("Bender VCF", 0., 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispPct);
  GetParam(kLfoDelay)->InitDouble("LFO Delay", 0., 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispLfoDelay);
  GetParam(kDcoLfo)->InitDouble("DCO LFO", 0., 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispPct);
  GetParam(kDcoPwm)->InitDouble("DCO PWM", 0., 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispPct);
  GetParam(kDcoSub)->InitDouble("DCO Sub", 1., 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispPct);
  GetParam(kDcoNoise)->InitDouble("DCO Noise", 0., 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispPct);
  GetParam(kVcfFreq)->InitDouble("VCF Freq", 0.5, 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispVcfHz);
  GetParam(kVcfRes)->InitDouble("VCF Res", 0., 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispPct);
  GetParam(kVcfEnv)->InitDouble("VCF Env", 0., 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispPct);
  GetParam(kVcfLfo)->InitDouble("VCF LFO", 0., 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispPct);
  GetParam(kVcfKbd)->InitDouble("VCF Kbd", 0., 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispPct);
  GetParam(kEnvS)->InitDouble("Sustain", 0.9, 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispPct);

  GetParam(kVcaLevel)->InitDouble("Volume", 0.5, 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispVcaLevel);

  GetParam(kArpRate)->InitDouble("Arp Rate", 120., 15., 1800., 0.1, "", IParam::kFlagsNone, "", IParam::ShapeExp(), IParam::kUnitCustom, dispArpBpm);
  GetParam(kLfoRate)->InitDouble("LFO Rate", 0.24, 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispLfoRate);

  // --- 4-position HPF switch: 0=bass boost, 1=flat, 2=HPF 240Hz, 3=HPF 720Hz ---
  GetParam(kHpfFreq)->InitInt("HPF", 1, 0, 3);

  // --- ADSR: raw 0-1 slider values; DSP applies curve + range via LUT ---
  GetParam(kEnvA)->InitDouble("Attack", 0.25, 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispMs(KR106DSP<float>::kAttackLUT, KR106DSP<float>::kAttackLUT_J6));
  GetParam(kEnvD)->InitDouble("Decay", 0.25, 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispMs(KR106DSP<float>::kDecayLUT, KR106DSP<float>::kDecayLUT_J6));
  GetParam(kEnvR)->InitDouble("Release", 0.25, 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispMs(KR106DSP<float>::kReleaseLUT, KR106DSP<float>::kReleaseLUT_J6));

  // --- Buttons (toggle 0/1) ---
  GetParam(kTranspose)->InitBool("Transpose", false);
  GetParam(kHold)->InitBool("Hold", false);
  GetParam(kArpeggio)->InitBool("Arpeggio", false);
  GetParam(kDcoPulse)->InitBool("Pulse", true);
  GetParam(kDcoSaw)->InitBool("Saw", true);
  GetParam(kDcoSubSw)->InitBool("Sub Sw", false);
  GetParam(kChorusOff)->InitBool("Chorus Off", true);
  GetParam(kChorusI)->InitBool("Chorus I", false);
  GetParam(kChorusII)->InitBool("Chorus II", false);

  // --- Switches ---
  GetParam(kOctTranspose)->InitInt("Octave", 1, 0, 2); // up=0, normal=1, down=2
  GetParam(kArpMode)->InitInt("Arp Mode", 0, 0, 2);
  GetParam(kArpRange)->InitInt("Arp Range", 0, 0, 2);
  GetParam(kLfoMode)->InitInt("LFO Mode", 0, 0, 1);
  GetParam(kPwmMode)->InitInt("PWM Mode", 1, 0, 2); // LFO=0, MAN=1, ENV=2
  GetParam(kVcfEnvInv)->InitInt("VCF Env Inv", 0, 0, 1);
  GetParam(kVcaMode)->InitInt("VCA Mode", 0, 0, 1);
  GetParam(kAdsrMode)->InitInt("ADSR Mode", 1, 0, 1); // 0=Juno-6(left), 1=Juno-106(right)

  // --- Special ---
  GetParam(kBender)->InitDouble("Bender", 0., -1., 1., 0.01, "");
  GetParam(kTuning)->InitDouble("Tuning", 0., -1., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispTuning);
  GetParam(kPower)->InitBool("Power", true);
  GetParam(kPortaMode)->InitInt("Porta Mode", 2, 0, 2); // default 2 = switch down = Poly
  GetParam(kPortaRate)->InitDouble("Porta Rate", 0., 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispPorta);
  GetParam(kTransposeOffset)->InitInt("Transpose Offset", 0, -24, 36);
  GetParam(kBenderLfo)->InitDouble("Bender LFO", 0., 0., 1., 0.01, "", IParam::kFlagsNone, "", IParam::ShapeLinear(), IParam::kUnitCustom, dispPct);

#include "KR106_Presets.h"

#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, 1.f);
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    pGraphics->EnableMouseOver(true);
    pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);

    // Load bitmaps — @2x versions loaded automatically on Retina
    const IBitmap bgBitmap = pGraphics->LoadBitmap(BG_FN);
    const IBitmap knobBitmap = pGraphics->LoadBitmap(KNOB_FN, 32, true);
    const IBitmap smallKnobBitmap = pGraphics->LoadBitmap(SMALLKNOB_FN, 32, true);
    const IBitmap switch2wayBitmap = pGraphics->LoadBitmap(SWITCH_2WAY_FN, 2);
    const IBitmap switch3wayBitmap = pGraphics->LoadBitmap(SWITCH_3WAY_FN, 3);
    const IBitmap ledBitmap = pGraphics->LoadBitmap(LED_RED_FN, 2);
    const IBitmap transposeChevronBitmap = pGraphics->LoadBitmap(TRANSPOSE_CHEVRON_FN);
    const IBitmap switch2wayHorizBitmap = pGraphics->LoadBitmap(SWITCH_2WAY_HORIZ_FN, 2);
    const IBitmap switch3wayHorizBitmap = pGraphics->LoadBitmap(SWITCH_3WAY_HORIZ_FN, 3);

    // Background panel
    pGraphics->AttachControl(new IBitmapControl(0, 0, bgBitmap));

    const IBitmap benderGradient = pGraphics->LoadBitmap(BENDER_GRADIENT_FN);

    // === LEFT SECTION: Master controls ===
    // Power switch (46, 40) — 15x19 toggle
    pGraphics->AttachControl(new KR106PowerSwitchControl(IRECT(46, 40, 61, 59)));

    // Tuning knob — center 28x27 frame in original 39x39 widget at (34,64)
    pGraphics->AttachControl(new KR106KnobControl(40, 64, smallKnobBitmap, kTuning));

    // Portamento mode switch and rate knob
    pGraphics->AttachControl(new KR106KnobControl(87, 120, smallKnobBitmap, kPortaRate));
    pGraphics->AttachControl(new KR106SwitchControl(96, 144, switch3wayBitmap, kPortaMode));

    // Bender sensitivity sliders (left side, below panel)
    pGraphics->AttachControl(new KR106SliderControl(IRECT(23, 127, 36, 176), kBenderDco));
    pGraphics->AttachControl(new KR106SliderControl(IRECT(41, 127, 54, 176), kBenderVcf));
    pGraphics->AttachControl(new KR106SliderControl(IRECT(59, 127, 72, 176), kBenderLfo));

    // LFO Trigger button (75, 182) — 41x19 momentary
    pGraphics->AttachControl(new KR106LFOTrigControl(IRECT(75, 182, 116, 201)));

    // Pitch bend lever (66, 206) — 60x8 spring-back horizontal
    pGraphics->AttachControl(new KR106BenderControl(IRECT(66, 206, 126, 214), kBender, benderGradient));

    // Octave transpose 3-way horizontal switch (under DCO waveform buttons)
    pGraphics->AttachControl(new KR106HorizontalSwitchControl(IRECT(389, 84, 413, 93), switch3wayHorizBitmap, kOctTranspose));

    // === ARPEGGIATOR SECTION ===
    // Transpose button+LED (95, 52), Hold (122, 52), Arp (154, 52)
    // button_control_layout places: button at (x, y), LED at (x+4, y-9)
    // So combined bounds = (x, y-9, x+17, y+19)
    pGraphics->AttachControl(new KR106ButtonLEDControl(IRECT(95, 43, 112, 71), kTranspose, kYellow, ledBitmap));
    pGraphics->AttachControl(new KR106ButtonLEDControl(IRECT(122, 43, 139, 71), kHold, kYellow, ledBitmap));
    pGraphics->AttachControl(new KR106ButtonLEDControl(IRECT(154, 43, 171, 71), kArpeggio, kYellow, ledBitmap));

    // Arp mode 3-way switch (175, 45), range (212, 45)
    pGraphics->AttachControl(new KR106SwitchControl(175, 45, switch3wayBitmap, kArpMode));
    pGraphics->AttachControl(new KR106SwitchControl(212, 45, switch3wayBitmap, kArpRange));

    // Arp rate slider (229, 33)
    pGraphics->AttachControl(new KR106SliderControl(IRECT(229, 33, 242, 82), kArpRate));

    // === LFO SECTION ===
    pGraphics->AttachControl(new KR106SliderControl(IRECT(251, 33, 264, 82), kLfoRate));
    pGraphics->AttachControl(new KR106SliderControl(IRECT(269, 33, 282, 82), kLfoDelay));
    pGraphics->AttachControl(new KR106SwitchControl(284, 45, switch2wayBitmap, kLfoMode));

    // === DCO SECTION ===
    pGraphics->AttachControl(new KR106SliderControl(IRECT(316, 33, 329, 82), kDcoLfo));
    pGraphics->AttachControl(new KR106SliderControl(IRECT(334, 33, 347, 82), kDcoPwm));
    pGraphics->AttachControl(new KR106SwitchControl(349, 45, switch3wayBitmap, kPwmMode));

    // DCO waveform buttons+LEDs: Pulse (377), Saw (393), Sub (409)
    pGraphics->AttachControl(new KR106ButtonLEDControl(IRECT(377, 43, 394, 71), kDcoPulse, kYellow, ledBitmap));
    pGraphics->AttachControl(new KR106ButtonLEDControl(IRECT(393, 43, 410, 71), kDcoSaw, kYellow, ledBitmap));
    pGraphics->AttachControl(new KR106ButtonLEDControl(IRECT(409, 43, 426, 71), kDcoSubSw, kOrange, ledBitmap));

    // DCO Sub level and Noise sliders
    pGraphics->AttachControl(new KR106SliderControl(IRECT(430, 33, 443, 82), kDcoSub));
    pGraphics->AttachControl(new KR106SliderControl(IRECT(448, 33, 461, 82), kDcoNoise));

    // === HPF SECTION ===
    pGraphics->AttachControl(new KR106SliderControl(IRECT(472, 33, 485, 82), kHpfFreq));

    // === VCF SECTION ===
    pGraphics->AttachControl(new KR106SliderControl(IRECT(496, 33, 509, 82), kVcfFreq));
    pGraphics->AttachControl(new KR106SliderControl(IRECT(513, 33, 526, 82), kVcfRes));
    pGraphics->AttachControl(new KR106SwitchControl(535, 45, switch2wayBitmap, kVcfEnvInv));
    pGraphics->AttachControl(new KR106SliderControl(IRECT(552, 33, 565, 82), kVcfEnv));
    pGraphics->AttachControl(new KR106SliderControl(IRECT(570, 33, 583, 82), kVcfLfo));
    pGraphics->AttachControl(new KR106SliderControl(IRECT(588, 33, 601, 82), kVcfKbd));

    // === VCA SECTION ===
    pGraphics->AttachControl(new KR106SwitchControl(614, 45, switch2wayBitmap, kVcaMode));
    pGraphics->AttachControl(new KR106SliderControl(IRECT(638, 33, 651, 82), kVcaLevel));

    // === ENVELOPE SECTION ===
    pGraphics->AttachControl(new KR106SliderControl(IRECT(659, 33, 672, 82), kEnvA));
    pGraphics->AttachControl(new KR106SliderControl(IRECT(676, 33, 689, 82), kEnvD));
    pGraphics->AttachControl(new KR106SliderControl(IRECT(695, 33, 708, 82), kEnvS));
    pGraphics->AttachControl(new KR106SliderControl(IRECT(713, 33, 726, 82), kEnvR));
    // ADSR mode switch: left=Juno-6, right=Juno-106
    pGraphics->AttachControl(new KR106HorizontalSwitchControl(IRECT(668, 80, 716, 98), switch2wayHorizBitmap, kAdsrMode));

    // === CHORUS SECTION ===
    // Chorus Off is a plain button (no LED), Chorus I and II have LEDs
    pGraphics->AttachControl(new KR106ChorusOffControl(IRECT(735, 52, 752, 71), kChorusI, kChorusII));
    pGraphics->AttachControl(new KR106ButtonLEDControl(IRECT(751, 43, 768, 71), kChorusI, kYellow, ledBitmap));
    pGraphics->AttachControl(new KR106ButtonLEDControl(IRECT(767, 43, 784, 71), kChorusII, kOrange, ledBitmap));

    // Bubble tooltip for slider value readout (drawn on top of all controls)
    pGraphics->AttachBubbleControl(IText(12.f, COLOR_BLACK, "Roboto-Regular", EAlign::Center));

    // === SCOPE (upper right) ===
    pGraphics->AttachControl(new KR106ScopeControl(IRECT(791, 21, 919, 95)), kCtrlTagScope);

    // Build timestamp — baked in at compile time so you can confirm the loaded version
    //pGraphics->AttachControl(new ITextControl(IRECT(791, 96, 940, 106),
    //  __DATE__ " " __TIME__,
    //  IText(8.f, COLOR_WHITE, "Roboto-Regular", EAlign::Far)));

    // === KEYBOARD ===
    // Bounds start at y=106 (5px above keys) to include the transpose chevron strip
    pGraphics->AttachControl(new KR106KeyboardControl(IRECT(129, 106, 921, 220), transposeChevronBitmap), kCtrlTagKeyboard);

    // Keyboard handler: QWERTY = notes, Z/X = octave shift, unhandled keys pass to DAW
    auto* kbd = dynamic_cast<KR106KeyboardControl*>(pGraphics->GetControlWithTag(kCtrlTagKeyboard));
    pGraphics->SetKeyHandlerFunc([pGraphics, kbd](const IKeyPress& key, bool isUp) {
      // Number keys 1-9 toggle panel buttons
      {
        static constexpr int kButtonMap[] = {
          kTranspose, kHold, kArpeggio,
          kDcoPulse, kDcoSaw, kDcoSubSw,
          kChorusOff, kChorusI, kChorusII
        };
        int idx = key.VK - kVK_1;
        if (idx >= 0 && idx < 9 && !isUp)
        {
          int paramIdx = kButtonMap[idx];
          double cur = pGraphics->GetDelegate()->GetParam(paramIdx)->Value();
          double next = (cur > 0.5) ? 0.0 : 1.0;
          pGraphics->GetDelegate()->BeginInformHostOfParamChangeFromUI(paramIdx);
          pGraphics->GetDelegate()->SendParameterValueFromUI(paramIdx, next);
          pGraphics->GetDelegate()->EndInformHostOfParamChangeFromUI(paramIdx);
          if (auto* c = pGraphics->GetControlWithParamIdx(paramIdx))
          {
            c->SetValueFromDelegate(next);
            c->SetDirty(false);
          }
          return true;
        }
      }

      // QWERTY-to-MIDI note mapping (same layout as iPlug2 default)
      IMidiMsg msg;
      int note = 0;
      static int base = 48;
      static bool keysDown[128] = {};

      auto onOctSwitch = [&]() {
        base = std::clamp(base, 24, 96);
        for (int i = 0; i < 128; i++) {
          if (keysDown[i]) {
            msg.MakeNoteOffMsg(i, 0);
            pGraphics->GetDelegate()->SendMidiMsgFromUI(msg);
            kbd->SetNoteFromMidi(i, false);
          }
        }
      };

      switch (key.VK) {
        case kVK_A: note = 0; break;
        case kVK_W: note = 1; break;
        case kVK_S: note = 2; break;
        case kVK_E: note = 3; break;
        case kVK_D: note = 4; break;
        case kVK_F: note = 5; break;
        case kVK_T: note = 6; break;
        case kVK_G: note = 7; break;
        case kVK_Y: note = 8; break;
        case kVK_H: note = 9; break;
        case kVK_U: note = 10; break;
        case kVK_J: note = 11; break;
        case kVK_K: note = 12; break;
        case kVK_O: note = 13; break;
        case kVK_L: note = 14; break;
        case kVK_Z: if (!isUp) { base -= 12; onOctSwitch(); } return true;
        case kVK_X: if (!isUp) { base += 12; onOctSwitch(); } return true;
        default: return false; // pass unhandled keys to DAW (spacebar, arrows, etc.)
      }

      int pitch = base + note;
      if (pitch < 0 || pitch > 127) return true;

      if (!isUp) {
        if (!keysDown[pitch]) {
          msg.MakeNoteOnMsg(pitch, 127, 0);
          keysDown[pitch] = true;
          pGraphics->GetDelegate()->SendMidiMsgFromUI(msg);
          kbd->SetNoteFromMidi(pitch, true);
        }
      } else {
        if (keysDown[pitch]) {
          msg.MakeNoteOffMsg(pitch, 0);
          keysDown[pitch] = false;
          pGraphics->GetDelegate()->SendMidiMsgFromUI(msg);
          kbd->SetNoteFromMidi(pitch, false);
        }
      }
      return true;
    });
  };
#endif
}

#if IPLUG_DSP
void KR106::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  // Catch preset changes from the host UI / AU property API (may arrive
  // asynchronously on a different thread). MIDI program changes are handled
  // immediately in ProcessMidiMsg; this is a fallback for non-MIDI paths.
  {
    int presetIdx = GetCurrentPresetIdx();
    if (presetIdx != mLastSyncedPreset)
    {
      mLastSyncedPreset = presetIdx;
      mDSP.mSuppressHoldRelease = true;
      for (int i = 0; i < kNumParams; i++)
        mDSP.SetParam(i, GetParam(i)->Value());
      mDSP.mSuppressHoldRelease = false;
    }
  }

  if (mHoldOff.exchange(false))
  {
    for (int i = 0; i < 128; i++)
    {
      if (mKeyboardHeld.test(i))
      {
        IMidiMsg off;
        off.MakeNoteOffMsg(i, 0);
        mMidiForKeyboard.Push(off);
      }
    }
    mKeyboardHeld.reset();
  }

  // Drain UI-initiated individual note releases (bypasses hold suppression)
  {
    int noteNum;
    while (mForceRelease.Pop(noteNum))
    {
      mDSP.ForceRelease(noteNum);
      mKeyboardHeld.reset(noteNum); // prevent spurious NoteOff to keyboard when hold turns off
    }
  }

  mDSP.ProcessBlock(inputs, outputs, 2, nFrames);
  if (!mPowerOn)
  {
    for (int i = 0; i < nFrames; i++)
      outputs[0][i] = outputs[1][i] = sample(0);
  }

  // Feed scope before output saturation so the display stays tall
  {
    sample* scopeBuf[2] = { outputs[0], mDSP.GetSyncBuffer() };
    mScopeSender.ProcessBlock(scopeBuf, nFrames, kCtrlTagScope);
  }

  // Output saturation: scale down 6-voice sum then soft-clip to keep peaks within ±1.0
  for (int i = 0; i < nFrames; i++)
  {
    for (int c = 0; c < 2; c++)
    {
      double x = outputs[c][i] * 0.35; // ~-9 dB headroom for 6-voice sum
      // tanh approximant: x*(27+x^2) / (27+9*x^2)
      double x2 = x * x;
      outputs[c][i] = x * (27.0 + x2) / (27.0 + 9.0 * x2);
    }
  }
}

void KR106::OnReset()
{
  mDSP.Reset(GetSampleRate(), GetBlockSize());

  // Push all current parameter values to DSP on reset
  for (int i = 0; i < kNumParams; i++)
    mDSP.SetParam(i, GetParam(i)->Value());

  // Re-trigger held notes after voice clear (e.g. host called AudioUnitReset on focus loss).
  // The host re-sends its own MIDI track notes automatically; we must restore ours.
  if (mDSP.mHold)
  {
    if (mDSP.mArp.mEnabled)
    {
      // Arp: mHeldNotes survived, just force an immediate retrigger on the next block
      if (!mDSP.mArp.mHeldNotes.empty())
        mDSP.mArp.mPhase = 1.f;
    }
    else
    {
      for (int i = 0; i < 128; i++)
      {
        if (mDSP.mHeldNotes.test(i))
        {
          IMidiMsg on;
          on.MakeNoteOnMsg(i, 127, 0);
          mDSP.mSynth.AddMidiMsgToQueue(on);
        }
      }
    }
  }
}

void KR106::ProcessMidiMsg(const IMidiMsg& msg)
{
  TRACE;

  // Handle MIDI program changes immediately so params are set before any
  // note-on in the same batch triggers a voice.
  if (msg.StatusMsg() == IMidiMsg::kProgramChange)
  {
    int pgm = msg.Program();
    if (pgm >= 0 && pgm < NPresets())
    {
      RestorePreset(pgm);
      mLastSyncedPreset = pgm;
      for (int i = 0; i < kNumParams; i++)
        mDSP.SetParam(i, GetParam(i)->Value());
    }
    return;
  }

  // Sustain pedal (CC64) toggles Hold
  if (msg.StatusMsg() == IMidiMsg::kControlChange &&
      msg.ControlChangeIdx() == IMidiMsg::kSustainOnOff)
  {
    bool pedalDown = msg.ControlChange(IMidiMsg::kSustainOnOff) >= 0.5;
    GetParam(kHold)->Set(pedalDown ? 1.0 : 0.0);
    OnParamChange(kHold);
    SendParameterValueFromAPI(kHold, GetParam(kHold)->Value(), true);
    return;
  }

  mDSP.ProcessMidiMsg(msg);
  SendMidiMsg(msg); // MIDI output
  // Queue note-on/off for keyboard display (drained on UI thread in OnIdle).
  // When Hold is active, suppress note-offs so held keys stay visually lit.
  auto s = msg.StatusMsg();
  bool isNoteOn  = (s == IMidiMsg::kNoteOn  && msg.Velocity() > 0);
  bool isNoteOff = (s == IMidiMsg::kNoteOff || (s == IMidiMsg::kNoteOn && msg.Velocity() == 0));
  if (isNoteOn)
  {
    mKeyboardHeld.set(msg.NoteNumber());
    mMidiForKeyboard.Push(msg);
  }
  else if (isNoteOff)
  {
    if (!mDSP.mHold)
    {
      mKeyboardHeld.reset(msg.NoteNumber());
      mMidiForKeyboard.Push(msg);
    }
  }
}

void KR106::OnParamChange(int paramIdx)
{
  // During preset restore, skip live performance params — their values are
  // preserved by UnserializeState and must not be applied from the preset.
  if (mRestoringPreset)
  {
    switch (paramIdx)
    {
      case kTuning: case kTranspose: case kHold:
      case kArpeggio: case kArpRate: case kArpMode: case kArpRange:
      case kPortaMode: case kPortaRate: case kTransposeOffset:
        return;
      default: break;
    }
  }

  if (paramIdx == kPower)
  {
    mPowerOn = GetParam(kPower)->Bool();
    if (!mPowerOn)
    {
      mDSP.PowerOff();
      mHoldOff = true; // release visually held keyboard keys
    }
    if (auto* pUI = GetUI())
      pUI->SetAllControlsDirty();
  }
  else if (paramIdx == kTransposeOffset)
    mDSP.SetKeyTranspose((int)GetParam(kTransposeOffset)->Value());
  else
  {
    mDSP.SetParam(paramIdx, GetParam(paramIdx)->Value());
    if (paramIdx == kHold && !GetParam(kHold)->Bool() && !mRestoringPreset)
      mHoldOff = true; // signal audio thread to release held keyboard display
  }
}

int KR106::UnserializeState(const IByteChunk& chunk, int startPos)
{
  // First call is the host restoring session state — accept all values as-is.
  // Subsequent calls are preset changes — preserve live performance controls.
  if (!mHostStateLoaded)
  {
    mHostStateLoaded = true;
    return Plugin::UnserializeState(chunk, startPos);
  }

  // Live performance controls — not part of a patch, preserve across preset changes
  static const int kLiveParams[] = {
    kTuning, kTranspose, kHold,
    kArpeggio, kArpRate, kArpMode, kArpRange,
    kPortaMode, kPortaRate, kTransposeOffset
  };
  constexpr int nLive = sizeof(kLiveParams) / sizeof(kLiveParams[0]);

  double saved[nLive];
  for (int i = 0; i < nLive; i++)
    saved[i] = GetParam(kLiveParams[i])->Value();

  mRestoringPreset = true;
  mDSP.mSuppressHoldRelease = true;
  int pos = Plugin::UnserializeState(chunk, startPos);

  for (int i = 0; i < nLive; i++)
  {
    if (GetParam(kLiveParams[i])->Value() != saved[i])
    {
      GetParam(kLiveParams[i])->Set(saved[i]);
      OnParamChange(kLiveParams[i]);
    }
  }
  mRestoringPreset = false;
  mDSP.mSuppressHoldRelease = false;

  // Mark preset as synced so ProcessBlock doesn't redundantly re-apply
  // all params without the mSuppressHoldRelease guard.
  mLastSyncedPreset = GetCurrentPresetIdx();

  return pos;
}

void KR106::OnIdle()
{
  // Forward queued MIDI note events to the keyboard control for visual display
  if (auto* pUI = GetUI())
  {
    mScopeSender.TransmitData(*this);

    auto* pKb = static_cast<KR106KeyboardControl*>(pUI->GetControlWithTag(kCtrlTagKeyboard));
    if (pKb)
    {
      if (mNeedChevronRestore)
      {
        mNeedChevronRestore = false;
        int offset = (int)GetParam(kTransposeOffset)->Value();
        if (offset != 0)
          pKb->SetTransposeKeyFromOffset(offset);
      }
      IMidiMsg msg;
      while (mMidiForKeyboard.Pop(msg))
        pKb->SetNoteFromMidi(msg.NoteNumber(),
          msg.StatusMsg() == IMidiMsg::kNoteOn && msg.Velocity() > 0);
    }
  }
}
#endif
