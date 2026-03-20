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
    auto add = [&](auto* ctrl, int x, int y, int w, int h) {
        ctrl->setBounds(x, y, w, h);
        addAndMakeVisible(ctrl);
        mControls.add(ctrl);
        return ctrl;
    };

    // Power section
    add(new KR106PowerSwitch(param(kPower)),              46,  40,  15, 19);
    add(new KR106Knob(param(kTuning), smallKnob, tip, 32), 40,  64,  28, 27);

    // Performance section
    add(new KR106Knob(param(kMasterVol), smallKnob, tip, 32),  30, 118,  28, 27);
    mClipLED = dynamic_cast<KR106ClipLED*>(
        add(new KR106ClipLED(ledRed, 2.5f), 62, 127, 9, 9));
    mClipLED2 = dynamic_cast<KR106ClipLED*>(
        add(new KR106ClipLED(ledRed, 1.5f), 53, 127, 9, 9));
    add(new KR106Knob(param(kPortaRate), smallKnob, tip, 32),  66, 118,  28, 27);
    add(new KR106Switch(param(kPortaMode), switchV, 3),    92, 161,   9, 24);

    // Bender sensitivity sliders
    add(new KR106Slider(param(kBenderDco), tip, sliderHdl),  37, 147, 13, 49);
    add(new KR106Slider(param(kBenderVcf), tip, sliderHdl),  55, 147, 13, 49);
    add(new KR106Slider(param(kBenderLfo), tip, sliderHdl),  73, 147, 13, 49);

    // Pitch bend lever with vertical LFO trigger
    add(new KR106Bender(param(kBender), benderGrad, &p),   66, 200,  60, 12);

    // === ARPEGGIATOR SECTION ===
    add(new KR106ButtonLED(param(kTranspose), 1, ledRed, param(kPower)),  95, 43, 17, 28);
    add(new KR106ButtonLED(param(kHold),      1, ledRed, param(kPower)), 122, 43, 17, 28);
    add(new KR106ButtonLED(param(kArpeggio),  1, ledRed, param(kPower)), 154, 43, 17, 28);

    add(new KR106Switch(param(kArpMode),  switchV, 3), 175, 46, 9, 24);
    add(new KR106Switch(param(kArpRange), switchV, 3), 212, 46, 9, 24);
    add(new KR106Slider(param(kArpRate), tip, sliderHdl),         230, 33, 13, 49);

    // === LFO SECTION ===
    add(new KR106Slider(param(kLfoRate), tip, sliderHdl),          252, 33, 13, 49);
    add(new KR106Slider(param(kLfoDelay), tip, sliderHdl),         270, 33, 13, 49);
    add(new KR106Switch(param(kLfoMode), switchV, 2),   294, 46,  9, 24);

    // === DCO SECTION ===
    add(new KR106Slider(param(kDcoLfo), tip, sliderHdl),           316, 33, 13, 49);
    add(new KR106Slider(param(kDcoPwm), tip, sliderHdl),           334, 33, 13, 49);
    add(new KR106Switch(param(kPwmMode), switchV, 3),   351, 46,  9, 24);

    // DCO waveform buttons+LEDs
    add(new KR106ButtonLED(param(kDcoPulse), 1, ledRed, param(kPower)), 377, 43, 17, 28);
    add(new KR106ButtonLED(param(kDcoSaw),   1, ledRed, param(kPower)), 393, 43, 17, 28);
    add(new KR106ButtonLED(param(kDcoSubSw), 2, ledRed, param(kPower)), 409, 43, 17, 28);

    // Octave transpose horizontal switch (under DCO buttons)
    add(new KR106HorizontalSwitch(param(kOctTranspose), switchH, 3), 389, 84, 24, 9);

    // DCO Sub/Noise sliders
    add(new KR106Slider(param(kDcoSub), tip, sliderHdl),   430, 33, 13, 49);
    add(new KR106Slider(param(kDcoNoise), tip, sliderHdl), 448, 33, 13, 49);

    // === HPF SECTION ===
    add(new KR106Slider(param(kHpfFreq), tip, sliderHdl),   473, 33, 13, 49);

    // === VCF SECTION ===
    add(new KR106Slider(param(kVcfFreq), tip, sliderHdl),          496, 33, 13, 49);
    add(new KR106Slider(param(kVcfRes), tip, sliderHdl),           514, 33, 13, 49);
    add(new KR106Switch(param(kVcfEnvInv), switchV, 2), 536, 46,  9, 24);
    add(new KR106Slider(param(kVcfEnv), tip, sliderHdl),           552, 33, 13, 49);
    add(new KR106Slider(param(kVcfLfo), tip, sliderHdl),           570, 33, 13, 49);
    add(new KR106Slider(param(kVcfKbd), tip, sliderHdl),           588, 33, 13, 49);

    // === VCA SECTION ===
    add(new KR106Switch(param(kVcaMode), switchV, 2), 614, 45,  9, 24);
    add(new KR106Slider(param(kVcaLevel), tip, sliderHdl),       638, 33, 13, 49);

    // === ENVELOPE SECTION ===
    add(new KR106Slider(param(kEnvA), tip, sliderHdl), 659, 33, 13, 49);
    add(new KR106Slider(param(kEnvD), tip, sliderHdl), 677, 33, 13, 49);
    add(new KR106Slider(param(kEnvS), tip, sliderHdl), 695, 33, 13, 49);
    add(new KR106Slider(param(kEnvR), tip, sliderHdl), 713, 33, 13, 49);

    // ADSR mode: horizontal 2-way (centered below envelope sliders)
    add(new KR106HorizontalSwitch(param(kAdsrMode), switchH, 2), 680, 84, 24, 9);

    // === CHORUS SECTION ===
    add(new KR106ChorusOff(param(kChorusI), param(kChorusII)), 735, 52, 17, 19);
    add(new KR106ButtonLED(param(kChorusI),  1, ledRed, param(kPower)), 751, 43, 17, 28);
    add(new KR106ButtonLED(param(kChorusII), 2, ledRed, param(kPower)), 767, 43, 17, 28);

    // === SCOPE ===
    mScope = add(new KR106Scope(&p), 790, 7, 128, 74);

    // === PRESET DISPLAY ===
    add(new KR106PresetDisplay(&p), 790, 86, 128, 14);

    // === KEYBOARD ===
    mKeyboard = add(new KR106Keyboard(&p, chevron), 129, 106, 792, 114);

    // Accept keyboard focus so UP/DN arrow keys cycle presets
    setWantsKeyboardFocus(true);

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
    if (!e.mods.isPopupMenu()) return;

    juce::PopupMenu menu;
    menu.addItem(1, "100%", true, mUIScale == 1.f);
    menu.addItem(2, "150%", true, mUIScale == 1.5f);
    menu.addItem(3, "200%", true, mUIScale == 2.f);

    menu.addSeparator();
    int vc = mProcessor.mVoiceCount;
    menu.addItem(10, "6 Voices", true, vc == 6);
    menu.addItem(11, "8 Voices", true, vc == 8);
    menu.addItem(12, "10 Voices", true, vc == 10);

    menu.addSeparator();
    menu.addItem(20, "Ignore MIDI Velocity", true, mProcessor.mIgnoreVelocity);
    menu.addItem(21, "Limit Arp to Keyboard Range", true, mProcessor.mArpLimitKbd);

    menu.showMenuAsync({}, [this](int r) {
        if (r == 0) return;
        // UI scale
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
        // Voice count
        int voices = r == 10 ? 6 : r == 11 ? 8 : r == 12 ? 10 : 0;
        if (voices > 0 && voices != mProcessor.mVoiceCount)
        {
            mProcessor.mVoiceCount = voices;
            mProcessor.mDSP.SetActiveVoices(voices);
        }
        // Velocity
        if (r == 20)
        {
            mProcessor.mIgnoreVelocity = !mProcessor.mIgnoreVelocity;
            mProcessor.mDSP.mIgnoreVelocity = mProcessor.mIgnoreVelocity;
        }
        // Arp keyboard limit
        if (r == 21)
        {
            mProcessor.mArpLimitKbd = !mProcessor.mArpLimitKbd;
            mProcessor.mDSP.mArp.mLimitToKeyboard = mProcessor.mArpLimitKbd;
        }
        mProcessor.saveGlobalSettings();
    });
}

bool KR106Editor::keyPressed(const juce::KeyPress& key)
{
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
    return false;
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
    }
}
