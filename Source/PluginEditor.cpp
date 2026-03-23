#include "PluginEditor.h"
#include "BinaryData.h"

KR106Editor::KR106Editor(KR106AudioProcessor& p)
    : AudioProcessorEditor(p), mProcessor(p)
{
    setSize(940, 224);

    // Restore saved scale, or auto-detect: 2x on non-HiDPI, 1x on Retina.
    if (p.mUIScale > 0.f)
        mUIScale = p.mUIScale;
    else
    {
        auto& displays = juce::Desktop::getInstance().getDisplays();
        auto* display = displays.getPrimaryDisplay();
        mUIScale = (display && display->scale < 1.5) ? 2.f : 1.f;
        p.mUIScale = mUIScale; // persist so it survives window recreation
    }
    if (mUIScale != 1.f)
        setTransform(juce::AffineTransform::scale(mUIScale));

    // Load @2x images from binary data
    auto loadImg = [](const void* data, int size) {
        return juce::ImageCache::getFromMemory(data, size);
    };

    mBackground = loadImg(BinaryData::kr106_background2x_png,
                          BinaryData::kr106_background2x_pngSize);

    auto switchV    = loadImg(BinaryData::switch_3way2x_png,
                              BinaryData::switch_3way2x_pngSize);
    auto switchH    = loadImg(BinaryData::switch_3way_horizontal2x_png,
                              BinaryData::switch_3way_horizontal2x_pngSize);
    auto smallKnob  = loadImg(BinaryData::smallknob2x_png,
                              BinaryData::smallknob2x_pngSize);
    auto ledRed     = loadImg(BinaryData::led_red2x_png,
                              BinaryData::led_red2x_pngSize);
    auto benderGrad = loadImg(BinaryData::kr106_bender_gradient2x_png,
                              BinaryData::kr106_bender_gradient2x_pngSize);
    auto chevron    = loadImg(BinaryData::transpose_chevron2x_png,
                              BinaryData::transpose_chevron2x_pngSize);
    auto newKnob    = loadImg(BinaryData::new_knob2x_png,
                              BinaryData::new_knob2x_pngSize);
    auto sliderHdl  = loadImg(BinaryData::slider_handle2x_png,
                              BinaryData::slider_handle2x_pngSize);

    auto param = [&](int idx) { return p.getParam(idx); };
    auto* tip = &mTooltip;

    // Helper: create control, set bounds, add as visible child, store in array
    // Automatically wires up MIDI learn for sliders that have a param.
    auto add = [&](auto* ctrl, int x, int y, int w, int h) {
        ctrl->setBounds(x, y, w, h);
        addAndMakeVisible(ctrl);
        mControls.add(ctrl);
        return ctrl;
    };

    // Helper: create slider with MIDI learn wired up
    auto addSlider = [&](int paramIdx, auto* ctrl, int x, int y, int w, int h) {
        ctrl->setMidiLearn(&p, paramIdx);
        return add(ctrl, x, y, w, h);
    };

    // Power section
    add(new KR106PowerSwitch(param(kPower)),              46,  40,  15, 19);
    { auto* k = new KR106Knob(param(kTuning), smallKnob, tip, 32);
      k->setMidiLearn(&p, kTuning);
      add(k, 40, 64, 28, 27); }

    // Performance section
    { auto* k = new KR106Knob(param(kMasterVol), smallKnob, tip, 32);
      k->setMidiLearn(&p, kMasterVol);
      add(k, 30, 118, 28, 27); }
    mClipLED = dynamic_cast<KR106ClipLED*>(
        add(new KR106ClipLED(ledRed, 2.5f), 62, 127, 9, 9));
    mClipLED2 = dynamic_cast<KR106ClipLED*>(
        add(new KR106ClipLED(ledRed, 1.5f), 53, 127, 9, 9));
    { auto* k = new KR106Knob(param(kPortaRate), smallKnob, tip, 32);
      k->setMidiLearn(&p, kPortaRate);
      add(k, 66, 118, 28, 27); }
    add(new KR106Switch(param(kPortaMode), switchV, 3),    92, 161,   9, 24);

    // Bender sensitivity sliders
    addSlider(kBenderDco, new KR106Slider(param(kBenderDco), tip, sliderHdl),  35, 147, 17, 49);
    addSlider(kBenderVcf, new KR106Slider(param(kBenderVcf), tip, sliderHdl),  53, 147, 17, 49);
    addSlider(kBenderLfo, new KR106Slider(param(kBenderLfo), tip, sliderHdl),  71, 147, 17, 49);

    // Pitch bend lever with vertical LFO trigger
    add(new KR106Bender(param(kBender), benderGrad, &p),   66, 200,  60, 12);

    // === ARPEGGIATOR SECTION ===
    add(new KR106ButtonLED(param(kTranspose), 1, ledRed, param(kPower)),  95, 43, 17, 28);
    add(new KR106ButtonLED(param(kHold),      1, ledRed, param(kPower)), 122, 43, 17, 28);
    add(new KR106ButtonLED(param(kArpeggio),  1, ledRed, param(kPower)), 154, 43, 17, 28);

    add(new KR106Switch(param(kArpMode),  switchV, 3), 175, 46, 9, 24);
    add(new KR106Switch(param(kArpRange), switchV, 3), 212, 46, 9, 24);
    addSlider(kArpRate, new KR106Slider(param(kArpRate), tip, sliderHdl), 228, 33, 17, 49);

    // === LFO SECTION ===
    addSlider(kLfoRate, new KR106Slider(param(kLfoRate), tip, sliderHdl), 250, 33, 17, 49);
    addSlider(kLfoDelay, new KR106Slider(param(kLfoDelay), tip, sliderHdl), 268, 33, 17, 49);
    add(new KR106Switch(param(kLfoMode), switchV, 2),   294, 46,  9, 24);

    // === DCO SECTION ===
    addSlider(kDcoLfo, new KR106Slider(param(kDcoLfo), tip, sliderHdl), 314, 33, 17, 49);
    addSlider(kDcoPwm, new KR106Slider(param(kDcoPwm), tip, sliderHdl), 332, 33, 17, 49);
    add(new KR106Switch(param(kPwmMode), switchV, 3),   351, 46,  9, 24);

    // DCO waveform buttons+LEDs
    add(new KR106ButtonLED(param(kDcoPulse), 1, ledRed, param(kPower)), 377, 43, 17, 28);
    add(new KR106ButtonLED(param(kDcoSaw),   1, ledRed, param(kPower)), 393, 43, 17, 28);
    add(new KR106ButtonLED(param(kDcoSubSw), 2, ledRed, param(kPower)), 409, 43, 17, 28);

    // Octave transpose horizontal switch (under DCO buttons)
    add(new KR106HorizontalSwitch(param(kOctTranspose), switchH, 3), 389, 84, 24, 9);

    // DCO Sub/Noise sliders
    addSlider(kDcoSub, new KR106Slider(param(kDcoSub), tip, sliderHdl), 430, 33, 17, 49);
    addSlider(kDcoNoise, new KR106Slider(param(kDcoNoise), tip, sliderHdl), 446, 33, 17, 49);

    // === HPF SECTION ===
    { auto* s = new KR106HPFSlider(param(kHpfFreq), tip, sliderHdl, &p.mDSP.mAdsrMode);
      s->setMidiLearn(&p, kHpfFreq);
      add(s, 471, 33, 17, 49); }

    // === VCF SECTION ===
    addSlider(kVcfFreq, new KR106Slider(param(kVcfFreq), tip, sliderHdl), 496, 33, 17, 49);
    addSlider(kVcfRes, new KR106Slider(param(kVcfRes), tip, sliderHdl), 513, 33, 17, 49);
    add(new KR106Switch(param(kVcfEnvInv), switchV, 2), 536, 46,  9, 24);
    addSlider(kVcfEnv, new KR106Slider(param(kVcfEnv), tip, sliderHdl), 550, 33, 17, 49);
    addSlider(kVcfLfo, new KR106Slider(param(kVcfLfo), tip, sliderHdl), 568, 33, 17, 49);
    addSlider(kVcfKbd, new KR106Slider(param(kVcfKbd), tip, sliderHdl), 586, 33, 17, 49);

    // === VCA SECTION ===
    add(new KR106Switch(param(kVcaMode), switchV, 2), 614, 45,  9, 24);
    addSlider(kVcaLevel, new KR106Slider(param(kVcaLevel), tip, sliderHdl), 635, 33, 17, 49);

    // === ENVELOPE SECTION ===
    addSlider(kEnvA, new KR106Slider(param(kEnvA), tip, sliderHdl), 657, 33, 17, 49);
    addSlider(kEnvD, new KR106Slider(param(kEnvD), tip, sliderHdl), 675, 33, 17, 49);
    addSlider(kEnvS, new KR106Slider(param(kEnvS), tip, sliderHdl), 693, 33, 17, 49);
    addSlider(kEnvR, new KR106Slider(param(kEnvR), tip, sliderHdl), 711, 33, 17, 49);

    // ADSR mode: horizontal 2-way (centered below envelope sliders)
    add(new KR106HorizontalSwitch(param(kAdsrMode), switchH, 2), 680, 84, 24, 9);

    // === CHORUS SECTION ===
    add(new KR106ChorusOff(param(kChorusI), param(kChorusII)), 735, 52, 17, 19);
    add(new KR106ButtonLED(param(kChorusI),  1, ledRed, param(kPower)), 751, 43, 17, 28);
    add(new KR106ButtonLED(param(kChorusII), 2, ledRed, param(kPower)), 767, 43, 17, 28);

    // === SCOPE ===
    mScope = add(new KR106Scope(&p), 790, 7, 128, 74);

    // === PRESET DISPLAY ===
    mPresetDisplay = add(new KR106PresetDisplay(&p), 790, 86, 128, 14);

    // === ICON BUTTONS (below chorus) ===
    {
        static const char* gearSvg =
            R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M10.325 4.317c.426 -1.756 2.924 -1.756 3.35 0a1.724 1.724 0 0 0 2.573 1.066c1.543 -.94 3.31 .826 2.37 2.37a1.724 1.724 0 0 0 1.065 2.572c1.756 .426 1.756 2.924 0 3.35a1.724 1.724 0 0 0 -1.066 2.573c.94 1.543 -.826 3.31 -2.37 2.37a1.724 1.724 0 0 0 -2.572 1.065c-.426 1.756 -2.924 1.756 -3.35 0a1.724 1.724 0 0 0 -2.573 -1.066c-1.543 .94 -3.31 -.826 -2.37 -2.37a1.724 1.724 0 0 0 -1.065 -2.572c-1.756 -.426 -1.756 -2.924 0 -3.35a1.724 1.724 0 0 0 1.066 -2.573c-.94 -1.543 .826 -3.31 2.37 -2.37c1 .608 2.296 .07 2.572 -1.065" /><path d="M9 12a3 3 0 1 0 6 0a3 3 0 0 0 -6 0" /></svg>)";

        static const char* dbSvg =
            R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><path d="M4 6a8 3 0 1 0 16 0a8 3 0 1 0 -16 0" /><path d="M4 6v6a8 3 0 0 0 16 0v-6" /><path d="M4 12v6a8 3 0 0 0 16 0v-6" /></svg>)";

        add(new KR106IconButton(gearSvg, [this]() {
            showSettingsMenu();
        }), 742, 82, 14, 14);

        add(new KR106IconButton(dbSvg, [this]() {
            mPresetDisplay->openContextMenu();
        }), 760, 82, 14, 14);
    }

    // === KEYBOARD ===
    mKeyboard = add(new KR106Keyboard(&p, chevron), 129, 106, 792, 114);

    // Accept keyboard focus so UP/DN arrow keys cycle presets
    setWantsKeyboardFocus(true);

    mMenuTypeface = juce::Typeface::createSystemTypefaceFor(
        BinaryData::Segment14_otf, BinaryData::Segment14_otfSize);

    // Tooltip overlay — added last so it paints on top of all controls
    addAndMakeVisible(mTooltip);
    mTooltip.setVisible(false);
    mTooltip.setAlwaysOnTop(true);

    startTimerHz(30);
}

KR106Editor::~KR106Editor()
{
    stopTimer();
}

void KR106Editor::mouseDown(const juce::MouseEvent& e)
{
    // Cancel MIDI learn if active
    if (mProcessor.mMidiLearnParam.load(std::memory_order_relaxed) >= 0)
    {
        mProcessor.cancelMidiLearn();
        mTooltip.hide();
        return;
    }
    if (!e.mods.isPopupMenu()) return;
    showSettingsMenu();
}

void KR106Editor::showSettingsMenu()
{
    if (mSettingsMenu) return;

    int vc = mProcessor.mVoiceCount;
    std::vector<KR106MenuItem> items;
    items.push_back(KR106MenuItem::item(1,  "100%", true, mUIScale == 1.f));
    items.push_back(KR106MenuItem::item(2,  "150%", true, mUIScale == 1.5f));
    items.push_back(KR106MenuItem::item(3,  "200%", true, mUIScale == 2.f));
    items.push_back(KR106MenuItem::sep());
    items.push_back(KR106MenuItem::item(10, "06 Voices",  true, vc == 6));
    items.push_back(KR106MenuItem::item(11, "08 Voices",  true, vc == 8));
    items.push_back(KR106MenuItem::item(12, "10 Voices", true, vc == 10));
    items.push_back(KR106MenuItem::sep());
    items.push_back(KR106MenuItem::item(20, "Ignore MIDI Velocity",      true, mProcessor.mIgnoreVelocity));
    items.push_back(KR106MenuItem::item(21, "Limit Arp to Kbd Range", true, mProcessor.mArpLimitKbd));
    items.push_back(KR106MenuItem::item(22, "Mono Retrigger",        true, mProcessor.mMonoRetrigger));
    items.push_back(KR106MenuItem::item(23, "Classic VCF Frq Scale", true, mProcessor.mJ6ClassicVcf));
    items.push_back(KR106MenuItem::sep());
    int os = mProcessor.mVcfOversample;
    items.push_back(KR106MenuItem::item(30, "VCF Oversample 2x", true, os == 2));
    items.push_back(KR106MenuItem::item(31, "VCF Oversample 4x", true, os == 4));
    items.push_back(KR106MenuItem::sep());
    items.push_back(KR106MenuItem::item(40, "Component Variance Editor"));

    mSettingsMenu = std::make_unique<KR106MenuSheet>(std::move(items), mMenuTypeface,
        [this](int r)
        {
            mSettingsMenu.reset();
            if (r == 0) return;
            float s = r == 1 ? 1.f : r == 2 ? 1.5f : r == 3 ? 2.f : 0.f;
            if (s > 0.f && s != mUIScale)
            {
                mUIScale = s;
                mProcessor.mUIScale = s;
                if (s == 1.f)
                    setTransform({});
                else
                    setTransform(juce::AffineTransform::scale(s));
            }
            int voices = r == 10 ? 6 : r == 11 ? 8 : r == 12 ? 10 : 0;
            if (voices > 0 && voices != mProcessor.mVoiceCount)
            {
                mProcessor.mVoiceCount = voices;
                mProcessor.mDSP.SetActiveVoices(voices);
            }
            if (r == 20)
            {
                mProcessor.mIgnoreVelocity = !mProcessor.mIgnoreVelocity;
                mProcessor.mDSP.mIgnoreVelocity = mProcessor.mIgnoreVelocity;
            }
            if (r == 21)
            {
                mProcessor.mArpLimitKbd = !mProcessor.mArpLimitKbd;
                mProcessor.mDSP.mArp.mLimitToKeyboard = mProcessor.mArpLimitKbd;
            }
            if (r == 22)
            {
                mProcessor.mMonoRetrigger = !mProcessor.mMonoRetrigger;
                mProcessor.mDSP.mMonoRetrigger = mProcessor.mMonoRetrigger;
            }
            if (r == 23)
            {
                mProcessor.mJ6ClassicVcf = !mProcessor.mJ6ClassicVcf;
                mProcessor.mDSP.SetJ6ClassicVcf(mProcessor.mJ6ClassicVcf);
            }
            int newOS = r == 30 ? 2 : r == 31 ? 4 : 0;
            if (newOS > 0 && newOS != mProcessor.mVcfOversample)
            {
                mProcessor.mVcfOversample = newOS;
                mProcessor.mDSP.ForEachVoice([newOS](kr106::Voice<float>& v) {
                    v.mVCF.SetOversample(newOS);
                    v.mOsc.Init(v.mSampleRate * static_cast<float>(newOS));
                });
            }
            if (r == 40)
                showVarianceSheet();
            mProcessor.saveGlobalSettings();
        });

    int menuH = mSettingsMenu->calcHeight();
    int menuW = mSettingsMenu->calcWidth();
    int menuX = (getWidth() - menuW) / 2;
    int menuY = (getHeight() - menuH) / 2;

    addAndMakeVisible(mSettingsMenu.get());
    mSettingsMenu->showAt({ menuX, menuY, menuW, menuH });
}

void KR106Editor::showVarianceSheet()
{
    if (mVarianceSheet) return;

    mVarianceSheet = std::make_unique<KR106VarianceSheet>(&mProcessor, mMenuTypeface,
        [this]()
        {
            mVarianceSheet.reset();
            mProcessor.saveGlobalSettings();
        });

    int sheetW = mVarianceSheet->calcWidth();
    int sheetH = mVarianceSheet->calcHeight();
    int sheetX = (getWidth() - sheetW) / 2;
    int sheetY = (getHeight() - sheetH) / 2;

    addAndMakeVisible(mVarianceSheet.get());
    mVarianceSheet->showAt({ sheetX, sheetY, sheetW, sheetH });
}

int KR106Editor::qwertyToNote(int keyCode) const
{
    // Chromatic layout: A=C, W=C#, S=D, E=D#, D=E, F=F, T=F#, G=G, Y=G#, H=A, U=A#, J=B, K=C+1, O=C#+1, L=D+1
    switch (keyCode)
    {
        case 'A': return mQwertyBase;
        case 'W': return mQwertyBase + 1;
        case 'S': return mQwertyBase + 2;
        case 'E': return mQwertyBase + 3;
        case 'D': return mQwertyBase + 4;
        case 'F': return mQwertyBase + 5;
        case 'T': return mQwertyBase + 6;
        case 'G': return mQwertyBase + 7;
        case 'Y': return mQwertyBase + 8;
        case 'H': return mQwertyBase + 9;
        case 'U': return mQwertyBase + 10;
        case 'J': return mQwertyBase + 11;
        case 'K': return mQwertyBase + 12;
        case 'O': return mQwertyBase + 13;
        case 'L': return mQwertyBase + 14;
        default:  return -1;
    }
}

void KR106Editor::qwertyAllNotesOff()
{
    for (int i = 0; i < 128; i++)
    {
        if (mQwertyDown[i])
        {
            mProcessor.sendMidiFromUI(0x80, static_cast<uint8_t>(i), 0);
            mKeyboard->setNoteFromMidi(i, false);
            mQwertyDown[i] = false;
        }
    }
}

bool KR106Editor::keyPressed(const juce::KeyPress& key)
{
    int code = key.getKeyCode();

    // Left/right arrows: cycle scope modes
    if (key == juce::KeyPress::leftKey)  { mScope->cycleMode(-1); return true; }
    if (key == juce::KeyPress::rightKey) { mScope->cycleMode(1);  return true; }

    // Up/down/page keys: preset navigation
    int delta = 0;
    if (key == juce::KeyPress::upKey)        delta = -1;
    else if (key == juce::KeyPress::downKey) delta =  1;
    else if (key == juce::KeyPress::pageUpKey)   delta = -8;
    else if (key == juce::KeyPress::pageDownKey) delta =  8;

    if (delta != 0)
    {
        int num = mProcessor.getNumPrograms();
        if (num <= 0) return true;
        int idx = mProcessor.getCurrentProgram() + delta;
        idx = ((idx % num) + num) % num;
        mProcessor.setCurrentProgram(idx);
        repaint();
        return true;
    }

    // Enter: open preset sheet
    if (key == juce::KeyPress::returnKey) { mPresetDisplay->openPresetSheet(); return true; }

    // P: toggle patch bank view
    if (code == 'P') { mScope->togglePatchBank(); return true; }

    // Z/X: octave shift
    if (code == 'Z') { mQwertyBase = juce::jmax(0, mQwertyBase - 12);  qwertyAllNotesOff(); return true; }
    if (code == 'X') { mQwertyBase = juce::jmin(108, mQwertyBase + 12); qwertyAllNotesOff(); return true; }

    // 1-9: toggle panel buttons
    if (code >= '1' && code <= '9')
    {
        int idx = code - '1';

        // 7/8/9: chorus buttons need special handling (mutual interaction)
        if (idx == 6) // '7' = Chorus Off: turn off both I and II
        {
            auto setParam = [&](int pid, float v) {
                auto* p = mProcessor.getParam(pid);
                p->beginChangeGesture();
                p->setValueNotifyingHost(v);
                p->endChangeGesture();
            };
            setParam(kChorusI, 0.f);
            setParam(kChorusII, 0.f);
            return true;
        }
        if (idx == 7 || idx == 8) // '8' = Chorus I, '9' = Chorus II: toggle
        {
            int pid = (idx == 7) ? kChorusI : kChorusII;
            auto* p = mProcessor.getParam(pid);
            float next = (p->getValue() > 0.5f) ? 0.f : 1.f;
            p->beginChangeGesture();
            p->setValueNotifyingHost(next);
            p->endChangeGesture();
            return true;
        }

        static constexpr int kButtonMap[] = {
            kTranspose, kHold, kArpeggio,
            kDcoPulse, kDcoSaw, kDcoSubSw
        };
        auto* p = mProcessor.getParam(kButtonMap[idx]);
        double cur = p->getValue();
        double next = (cur > 0.5) ? 0.0 : 1.0;
        p->beginChangeGesture();
        p->setValueNotifyingHost(static_cast<float>(next));
        p->endChangeGesture();
        return true;
    }

    // '0': filter test mode (noise + sweep)
    if (code == '0')
    {
        mProcessor.mDSP.mFilterTestTrigger.store(true);
        return true;
    }

    // QWERTY note keys
    int note = qwertyToNote(code);
    if (note >= 0 && note <= 127)
    {
        if (!mQwertyDown[note])
        {
            bool holdOn = mProcessor.getParam(kHold)->getValue() > 0.5f;
            bool alreadyHeld = holdOn && mProcessor.mKeyboardHeld.test(note);

            if (alreadyHeld)
            {
                // Second press on held note: force release (same as mouse second-click)
                mProcessor.forceReleaseNote(note);
                mKeyboard->setNoteFromMidi(note, false);
            }
            else
            {
                mQwertyDown[note] = true;
                mProcessor.sendMidiFromUI(0x90, static_cast<uint8_t>(note), 127);
                mKeyboard->setNoteFromMidi(note, true);
            }
        }
        return true;  // consume key repeats too
    }

    return false;  // unhandled keys pass to DAW
}

bool KR106Editor::keyStateChanged(bool /*isKeyDown*/)
{
    // Check for released QWERTY keys
    bool handled = false;
    for (int i = 0; i < 128; i++)
    {
        if (!mQwertyDown[i]) continue;
        int code = -1;
        int offset = i - mQwertyBase;
        switch (offset)
        {
            case 0:  code = 'A'; break;
            case 1:  code = 'W'; break;
            case 2:  code = 'S'; break;
            case 3:  code = 'E'; break;
            case 4:  code = 'D'; break;
            case 5:  code = 'F'; break;
            case 6:  code = 'T'; break;
            case 7:  code = 'G'; break;
            case 8:  code = 'Y'; break;
            case 9:  code = 'H'; break;
            case 10: code = 'U'; break;
            case 11: code = 'J'; break;
            case 12: code = 'K'; break;
            case 13: code = 'O'; break;
            case 14: code = 'L'; break;
            default: break;
        }
        if (code >= 0 && !juce::KeyPress::isKeyCurrentlyDown(code))
        {
            mQwertyDown[i] = false;
            mProcessor.sendMidiFromUI(0x80, static_cast<uint8_t>(i), 0);
            // Don't visually release if Hold is on — updateFromProcessor
            // keeps the key lit via mKeyboardHeld (same as mouse behavior)
            bool holdOn = mProcessor.getParam(kHold)->getValue() > 0.5f;
            if (!holdOn)
                mKeyboard->setNoteFromMidi(i, false);
            handled = true;
        }
    }
    return handled;
}

void KR106Editor::paint(juce::Graphics& g)
{
    // Draw @2x background at 1x logical size
    g.drawImage(mBackground,
                0.f, 0.f, 940.f, 224.f,
                0, 0, mBackground.getWidth(), mBackground.getHeight());
}

void KR106Editor::timerCallback()
{
    bool active = mProcessor.getParam(kPower)->getValue() > 0.5f
                  && !mProcessor.isSuspended();

    // When inactive (power off or host suspended), skip GUI updates to save CPU
    if (!active)
    {
        if (mWasActive)
        {
            // One final repaint so LEDs go dark, scope goes black, keys release
            mWasActive = false;
            for (auto* ctrl : mControls)
                ctrl->repaint();
            startTimerHz(4); // Drop to 4 Hz — just polling for reactivation
        }
        return;
    }

    // Just became active — restore full refresh rate
    if (!mWasActive)
    {
        mWasActive = true;
        startTimerHz(30);
    }

    // Restore transpose chevron after state restore
    if (mNeedChevronRestore)
    {
        mNeedChevronRestore = false;
        auto* p = mProcessor.getParam(kTransposeOffset);
        if (p)
        {
            int offset = (int)p->convertFrom0to1(p->getValue());
            if (offset != 0)
                mKeyboard->setTransposeKeyFromOffset(offset);
        }
    }

    // Check for MIDI learn completion
    {
        int learnedCC = mProcessor.mMidiLearnResult.exchange(-1, std::memory_order_acquire);
        if (learnedCC >= 0)
        {
            mTooltip.setLine2("CC " + juce::String(learnedCC));
            mTooltip.update();
            mProcessor.saveGlobalSettings();
        }
    }

    // Update scope, keyboard, and clip LED from processor state
    mScope->updateFromProcessor();
    mKeyboard->updateFromProcessor();
    float peak = mProcessor.mPeakLevel.exchange(0.f, std::memory_order_relaxed);
    mClipLED->update(peak);
    mClipLED2->update(peak);

    // Repaint knobs/sliders/switches at ~7.5 Hz (every 4th tick) for host
    // automation sync — they don't change during normal MIDI playback.
    // Scope and keyboard repaint themselves from updateFromProcessor() above.
    if (++mRepaintDivider >= 4)
    {
        mRepaintDivider = 0;
        for (auto* ctrl : mControls)
            if (ctrl != mScope && ctrl != mKeyboard)
                ctrl->repaint();
        // Update tooltip if visible (CC changes update value externally)
        if (mTooltip.isVisible())
            mTooltip.update();
    }
}
