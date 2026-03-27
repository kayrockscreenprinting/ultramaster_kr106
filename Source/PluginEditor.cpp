#include "PluginEditor.h"
#include "BinaryData.h"
#include "Controls/KR106QwertyDiagram.h"

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
        add(new KR106ClipLED(ledRed, 1.f), 53, 127, 9, 9));
    { auto* k = new KR106Knob(param(kPortaRate), smallKnob, tip, 32);
      k->setMidiLearn(&p, kPortaRate);
      add(k, 66, 118, 28, 27); }
    add(new KR106Switch(param(kPortaMode), switchV, 3),    92, 161,   9, 24);

    // Bender sensitivity sliders
    addSlider(kBenderDco, new KR106Slider(param(kBenderDco), tip, sliderHdl),  34, 147, 20, 49)->setExtraRight(1);
    addSlider(kBenderVcf, new KR106Slider(param(kBenderVcf), tip, sliderHdl),  52, 147, 20, 49)->setExtraRight(1);
    addSlider(kBenderLfo, new KR106Slider(param(kBenderLfo), tip, sliderHdl),  70, 147, 19, 49);

    // Pitch bend lever with vertical LFO trigger
    add(new KR106Bender(param(kBender), benderGrad, &p),   66, 200,  60, 12);

    // === ARPEGGIATOR SECTION ===
    add(new KR106ButtonLED(param(kTranspose), 1, ledRed, param(kPower)),  95, 43, 17, 28);
    add(new KR106ButtonLED(param(kHold),      1, ledRed, param(kPower)), 122, 43, 17, 28);
    add(new KR106ButtonLED(param(kArpeggio),  1, ledRed, param(kPower)), 154, 43, 17, 28);

    add(new KR106Switch(param(kArpMode),  switchV, 3), 175, 46, 9, 24);
    add(new KR106Switch(param(kArpRange), switchV, 3), 212, 46, 9, 24);
    addSlider(kArpRate, new KR106ArpRateSlider(param(kArpRate), tip, sliderHdl, &p.mArpSyncHost), 227, 33, 19, 49);

    // === LFO SECTION ===
    addSlider(kLfoRate, new KR106LfoRateSlider(param(kLfoRate), tip, sliderHdl, &p.mLfoSyncHost), 249, 33, 20, 49)->setExtraRight(1);
    addSlider(kLfoDelay, new KR106Slider(param(kLfoDelay), tip, sliderHdl), 267, 33, 19, 49);
    add(new KR106Switch(param(kLfoMode), switchV, 2),   294, 46,  9, 24);

    // === DCO SECTION ===
    addSlider(kDcoLfo, new KR106Slider(param(kDcoLfo), tip, sliderHdl), 313, 33, 20, 49)->setExtraRight(1);
    addSlider(kDcoPwm, new KR106Slider(param(kDcoPwm), tip, sliderHdl), 331, 33, 19, 49);
    add(new KR106Switch(param(kPwmMode), switchV, 3),   351, 46,  9, 24);

    // DCO waveform buttons+LEDs
    add(new KR106ButtonLED(param(kDcoPulse), 1, ledRed, param(kPower)), 377, 43, 17, 28);
    add(new KR106ButtonLED(param(kDcoSaw),   1, ledRed, param(kPower)), 393, 43, 17, 28);
    add(new KR106ButtonLED(param(kDcoSubSw), 2, ledRed, param(kPower)), 409, 43, 17, 28);

    // Octave transpose horizontal switch (under DCO buttons)
    add(new KR106HorizontalSwitch(param(kOctTranspose), switchH, 3), 389, 84, 24, 9);

    // DCO Sub/Noise sliders
    addSlider(kDcoSub, new KR106Slider(param(kDcoSub), tip, sliderHdl), 427, 33, 20, 49)->setExtraRight(1);
    addSlider(kDcoNoise, new KR106Slider(param(kDcoNoise), tip, sliderHdl), 445, 33, 19, 49);

    // === HPF SECTION ===
    { auto* s = new KR106HPFSlider(param(kHpfFreq), tip, sliderHdl, &p.mDSP.mAdsrMode);
      s->setMidiLearn(&p, kHpfFreq);
      add(s, 470, 33, 19, 49); }

    // === VCF SECTION ===
    addSlider(kVcfFreq, new KR106Slider(param(kVcfFreq), tip, sliderHdl), 495, 33, 20, 49)->setExtraRight(1);
    addSlider(kVcfRes, new KR106Slider(param(kVcfRes), tip, sliderHdl), 513, 33, 19, 49);
    add(new KR106Switch(param(kVcfEnvInv), switchV, 2), 536, 46,  9, 24);
    addSlider(kVcfEnv, new KR106Slider(param(kVcfEnv), tip, sliderHdl), 549, 33, 20, 49)->setExtraRight(1);
    addSlider(kVcfLfo, new KR106Slider(param(kVcfLfo), tip, sliderHdl), 567, 33, 20, 49)->setExtraRight(1);
    addSlider(kVcfKbd, new KR106Slider(param(kVcfKbd), tip, sliderHdl), 585, 33, 19, 49);

    // === VCA SECTION ===
    add(new KR106Switch(param(kVcaMode), switchV, 2), 614, 45,  9, 24);
    addSlider(kVcaLevel, new KR106Slider(param(kVcaLevel), tip, sliderHdl), 634, 33, 19, 49);

    // === ENVELOPE SECTION ===
    addSlider(kEnvA, new KR106Slider(param(kEnvA), tip, sliderHdl), 656, 33, 20, 49)->setExtraRight(1);
    addSlider(kEnvD, new KR106Slider(param(kEnvD), tip, sliderHdl), 674, 33, 20, 49)->setExtraRight(1);
    addSlider(kEnvS, new KR106Slider(param(kEnvS), tip, sliderHdl), 692, 33, 20, 49)->setExtraRight(1);
    addSlider(kEnvR, new KR106Slider(param(kEnvR), tip, sliderHdl), 710, 33, 19, 49);

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
    // Left column
    items.push_back(KR106MenuItem::makeRadio(1,  "100%", mUIScale == 1.f));
    items.push_back(KR106MenuItem::makeRadio(2,  "150%", mUIScale == 1.5f));
    items.push_back(KR106MenuItem::makeRadio(3,  "200%", mUIScale == 2.f));
    items.push_back(KR106MenuItem::sep());
    items.push_back(KR106MenuItem::makeRadio(10, "06 Voices",  vc == 6));
    items.push_back(KR106MenuItem::makeRadio(11, "08 Voices",  vc == 8));
    items.push_back(KR106MenuItem::makeRadio(12, "10 Voices", vc == 10));
    items.push_back(KR106MenuItem::sep());
    int os = mProcessor.mVcfOversample;
    items.push_back(KR106MenuItem::makeRadio(30, "VCF Oversample 2x", os == 2));
    items.push_back(KR106MenuItem::makeRadio(31, "VCF Oversample 4x", os == 4));
    // Right column                                                    // index 10
    items.push_back(KR106MenuItem::item(20, "Ignore MIDI Velocity",      true, mProcessor.mIgnoreVelocity));
    items.push_back(KR106MenuItem::item(21, "Limit Arp to Kbd Range", true, mProcessor.mArpLimitKbd));
    items.push_back(KR106MenuItem::item(24, "Sync Arp to Host",      true, mProcessor.mArpSyncHost));
    items.push_back(KR106MenuItem::item(25, "Sync LFO to Host",      true, mProcessor.mLfoSyncHost));
    items.push_back(KR106MenuItem::item(22, "Mono Retrigger",        true, mProcessor.mMonoRetrigger));
    items.push_back(KR106MenuItem::item(23, "Classic VCF Frq Scale", true, mProcessor.mJ6ClassicVcf));
    items.push_back(KR106MenuItem::sep());
    items.push_back(KR106MenuItem::makeAction(40, "Component Variance Editor"));
    items.push_back(KR106MenuItem::makeAction(41, "Keyboard Shortcuts"));

    mSettingsMenu = std::make_unique<KR106MenuSheet>(std::move(items), mMenuTypeface,
        [this](int r)
        {
            if (r == 0) { mSettingsMenu.reset(); return; }
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
            if (r == 24)
            {
                mProcessor.mArpSyncHost = !mProcessor.mArpSyncHost;
            }
            if (r == 25)
            {
                mProcessor.mLfoSyncHost = !mProcessor.mLfoSyncHost;
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
                });
            }
            if (r == 40)
                showVarianceSheet();
            if (r == 41)
                showQwertyDiagram();
            mProcessor.saveGlobalSettings();
        }, 10);

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

void KR106Editor::showQwertyDiagram()
{
    if (mQwertyDiagram) return;

    // Transparent overlay covers the whole window to catch clicks anywhere
    struct Overlay : public juce::Component {
        std::function<void()> onClose;
        KR106QwertyDiagram diagram;
        Overlay() { addAndMakeVisible(diagram); }
        void mouseDown(const juce::MouseEvent&) override { if (onClose) onClose(); }
        void resized() override {
            int w = 386, h = 120;
            diagram.setBounds((getWidth() - w) / 2, (getHeight() - h) / 2, w, h);
        }
    };

    auto* overlay = new Overlay();
    overlay->onClose = [this]() { mQwertyDiagram.reset(); };
    overlay->diagram.onClose = [this]() { mQwertyDiagram.reset(); };
    overlay->setBounds(getLocalBounds());
    overlay->setAlwaysOnTop(true);
    addAndMakeVisible(overlay);
    mQwertyDiagram.reset(overlay);
}

int KR106Editor::qwertyToNote(int keyCode) const
{
    // Standard piano layout: two octaves + extensions on all rows.
    // Lower octave (Z row naturals + A row sharps):
    //   Z=C  S=C#  X=D  D=D#  C=E  V=F  G=F#  B=G  H=G#  N=A  J=A#  M=B
    //   ,=C+1  L=C#+1  .=D+1  ;=D#+1  /=E+1  '=F+1
    // Upper octave (Q row naturals + number row sharps):
    //   Q=C  2=C#  W=D  3=D#  E=E  R=F  5=F#  T=G  6=G#  Y=A  7=A#  U=B
    //   I=C+1  9=C#+1  O=D+1  0=D#+1  P=E+1  [=F+1  ]=F#+1  \=G+1  ==G#+1
    switch (keyCode)
    {
        // Lower octave naturals (Z row)
        case 'Z': return mQwertyBase;
        case 'X': return mQwertyBase + 2;
        case 'C': return mQwertyBase + 4;
        case 'V': return mQwertyBase + 5;
        case 'B': return mQwertyBase + 7;
        case 'N': return mQwertyBase + 9;
        case 'M': return mQwertyBase + 11;
        case ',': return mQwertyBase + 12;
        case '.': return mQwertyBase + 14;
        case '/': return mQwertyBase + 16;
        // Lower octave sharps (A row)
        case 'S': return mQwertyBase + 1;
        case 'D': return mQwertyBase + 3;
        case 'G': return mQwertyBase + 6;
        case 'H': return mQwertyBase + 8;
        case 'J': return mQwertyBase + 10;
        case 'L': return mQwertyBase + 13;
        case ';': return mQwertyBase + 15;
        case '\'': return mQwertyBase + 17;
        // Upper octave naturals (Q row)
        case 'Q': return mQwertyBase + 12;
        case 'W': return mQwertyBase + 14;
        case 'E': return mQwertyBase + 16;
        case 'R': return mQwertyBase + 17;
        case 'T': return mQwertyBase + 19;
        case 'Y': return mQwertyBase + 21;
        case 'U': return mQwertyBase + 23;
        case 'I': return mQwertyBase + 24;
        case 'O': return mQwertyBase + 26;
        case 'P': return mQwertyBase + 28;
        case '[': return mQwertyBase + 29;
        case ']': return mQwertyBase + 31;
        // Upper octave sharps (number row)
        case '2': return mQwertyBase + 13;
        case '3': return mQwertyBase + 15;
        case '5': return mQwertyBase + 18;
        case '6': return mQwertyBase + 20;
        case '7': return mQwertyBase + 22;
        case '9': return mQwertyBase + 25;
        case '0': return mQwertyBase + 27;
        case '-': return mQwertyBase + 30;
        case '=': return mQwertyBase + 32;
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
    // Ignore case
    if (code >= 'a' && code <= 'z')
    {
        code -= 'a' - 'A';
    }

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

    // `/1: octave shift
    if (code == '`' || code == 0x60) { mQwertyBase = juce::jmax(0, mQwertyBase - 12);  qwertyAllNotesOff(); return true; }
    if (code == '1')                 { mQwertyBase = juce::jmin(108, mQwertyBase + 12); qwertyAllNotesOff(); return true; }

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
    // Check for released QWERTY keys.
    // Multiple keys can map to the same note (overlapping octaves),
    // so check all keys that produce this note and only release if
    // none of them are held.
    static constexpr int kNoteKeys[][3] = {
        // offset: up to 3 key codes that produce base+offset (0 = end)
        /* 0  C   */ {'Z', 0, 0},
        /* 1  C#  */ {'S', 0, 0},
        /* 2  D   */ {'X', 0, 0},
        /* 3  D#  */ {'D', 0, 0},
        /* 4  E   */ {'C', 0, 0},
        /* 5  F   */ {'V', 0, 0},
        /* 6  F#  */ {'G', 0, 0},
        /* 7  G   */ {'B', 0, 0},
        /* 8  G#  */ {'H', 0, 0},
        /* 9  A   */ {'N', 0, 0},
        /* 10 A#  */ {'J', 0, 0},
        /* 11 B   */ {'M', 0, 0},
        /* 12 C+1 */ {'Q', ',', 0},
        /* 13 C#+1*/ {'2', 'L', 0},
        /* 14 D+1 */ {'W', '.', 0},
        /* 15 D#+1*/ {'3', ';', 0},
        /* 16 E+1 */ {'E', '/', 0},
        /* 17 F+1 */ {'R', '\'', 0},
        /* 18 F#+1*/ {'5', 0, 0},
        /* 19 G+1 */ {'T', 0, 0},
        /* 20 G#+1*/ {'6', 0, 0},
        /* 21 A+1 */ {'Y', 0, 0},
        /* 22 A#+1*/ {'7', 0, 0},
        /* 23 B+1 */ {'U', 0, 0},
        /* 24 C+2 */ {'I', 0, 0},
        /* 25 C#+2*/ {'9', 0, 0},
        /* 26 D+2 */ {'O', 0, 0},
        /* 27 D#+2*/ {'0', 0, 0},
        /* 28 E+2 */ {'P', 0, 0},
        /* 29 F+2 */ {'[', 0, 0},
        /* 30 F#+2*/ {'-', 0, 0},
        /* 31 G+2 */ {']', 0, 0},
        /* 32 G#+2*/ {'=', 0, 0},
    };
    static constexpr int kMaxOffset = 32;

    bool handled = false;
    for (int i = 0; i < 128; i++)
    {
        if (!mQwertyDown[i]) continue;
        int offset = i - mQwertyBase;
        if (offset < 0 || offset > kMaxOffset) { mQwertyDown[i] = false; continue; }

        // Check if any key for this note is still held
        bool anyHeld = false;
        for (int k = 0; k < 3 && kNoteKeys[offset][k] != 0; k++)
            if (juce::KeyPress::isKeyCurrentlyDown(kNoteKeys[offset][k]))
                anyHeld = true;

        if (!anyHeld)
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
