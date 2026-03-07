#include "PluginEditor.h"
#include "BinaryData.h"

KR106Editor::KR106Editor(KR106AudioProcessor& p)
    : AudioProcessorEditor(p), mProcessor(p)
{
    setSize(940, 224);

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

    auto param = [&](int idx) { return p.getParam(idx); };
    auto* tip = &mTooltip;

    // Helper: create control, set bounds, add as visible child, store in array
    auto add = [&](auto* ctrl, int x, int y, int w, int h) {
        ctrl->setBounds(x, y, w, h);
        addAndMakeVisible(ctrl);
        mControls.add(ctrl);
        return ctrl;
    };

    // === LEFT SECTION: Master controls ===
    add(new KR106PowerSwitch(param(kPower)),              46,  40,  15, 19);
    add(new KR106Knob(param(kTuning), smallKnob, tip),    40,  64,  28, 27);
    add(new KR106Knob(param(kPortaRate), smallKnob, tip),  87, 120,  28, 27);
    add(new KR106Switch(param(kPortaMode), switchV, 3),    96, 144,   9, 24);

    // Bender sensitivity sliders
    add(new KR106Slider(param(kBenderDco), tip),  23, 127, 13, 49);
    add(new KR106Slider(param(kBenderVcf), tip),  41, 127, 13, 49);
    add(new KR106Slider(param(kBenderLfo), tip),  59, 127, 13, 49);

    // LFO trigger + pitch bend lever
    add(new KR106LFOTrig(&p),                             75, 182,  41, 19);
    add(new KR106Bender(param(kBender), benderGrad),       66, 206,  60,  8);

    // === ARPEGGIATOR SECTION ===
    add(new KR106ButtonLED(param(kTranspose), 1, ledRed, param(kPower)),  95, 43, 17, 28);
    add(new KR106ButtonLED(param(kHold),      1, ledRed, param(kPower)), 122, 43, 17, 28);
    add(new KR106ButtonLED(param(kArpeggio),  1, ledRed, param(kPower)), 154, 43, 17, 28);

    add(new KR106Switch(param(kArpMode),  switchV, 3), 175, 45, 9, 24);
    add(new KR106Switch(param(kArpRange), switchV, 3), 212, 45, 9, 24);
    add(new KR106Slider(param(kArpRate), tip),         229, 33, 13, 49);

    // === LFO SECTION ===
    add(new KR106Slider(param(kLfoRate), tip),          251, 33, 13, 49);
    add(new KR106Slider(param(kLfoDelay), tip),         269, 33, 13, 49);
    add(new KR106Switch(param(kLfoMode), switchV, 2),   284, 45,  9, 24);

    // === DCO SECTION ===
    add(new KR106Slider(param(kDcoLfo), tip),           316, 33, 13, 49);
    add(new KR106Slider(param(kDcoPwm), tip),           334, 33, 13, 49);
    add(new KR106Switch(param(kPwmMode), switchV, 3),   349, 45,  9, 24);

    // DCO waveform buttons+LEDs
    add(new KR106ButtonLED(param(kDcoPulse), 1, ledRed, param(kPower)), 377, 43, 17, 28);
    add(new KR106ButtonLED(param(kDcoSaw),   1, ledRed, param(kPower)), 393, 43, 17, 28);
    add(new KR106ButtonLED(param(kDcoSubSw), 2, ledRed, param(kPower)), 409, 43, 17, 28);

    // Octave transpose horizontal switch (under DCO buttons)
    add(new KR106HorizontalSwitch(param(kOctTranspose), switchH, 3), 389, 84, 24, 9);

    // DCO Sub/Noise sliders
    add(new KR106Slider(param(kDcoSub), tip),   430, 33, 13, 49);
    add(new KR106Slider(param(kDcoNoise), tip), 448, 33, 13, 49);

    // === HPF SECTION ===
    add(new KR106Slider(param(kHpfFreq), tip),   472, 33, 13, 49);

    // === VCF SECTION ===
    add(new KR106Slider(param(kVcfFreq), tip),          496, 33, 13, 49);
    add(new KR106Slider(param(kVcfRes), tip),           513, 33, 13, 49);
    add(new KR106Switch(param(kVcfEnvInv), switchV, 2), 535, 45,  9, 24);
    add(new KR106Slider(param(kVcfEnv), tip),           552, 33, 13, 49);
    add(new KR106Slider(param(kVcfLfo), tip),           570, 33, 13, 49);
    add(new KR106Slider(param(kVcfKbd), tip),           588, 33, 13, 49);

    // === VCA SECTION ===
    add(new KR106Switch(param(kVcaMode), switchV, 2), 614, 45,  9, 24);
    add(new KR106Slider(param(kVcaLevel), tip),       638, 33, 13, 49);

    // === ENVELOPE SECTION ===
    add(new KR106Slider(param(kEnvA), tip), 659, 33, 13, 49);
    add(new KR106Slider(param(kEnvD), tip), 676, 33, 13, 49);
    add(new KR106Slider(param(kEnvS), tip), 695, 33, 13, 49);
    add(new KR106Slider(param(kEnvR), tip), 713, 33, 13, 49);

    // ADSR mode: horizontal 2-way (centered below envelope sliders)
    add(new KR106HorizontalSwitch(param(kAdsrMode), switchH, 2), 680, 84, 24, 9);

    // === CHORUS SECTION ===
    add(new KR106ChorusOff(param(kChorusI), param(kChorusII)), 735, 52, 17, 19);
    add(new KR106ButtonLED(param(kChorusI),  1, ledRed, param(kPower)), 751, 43, 17, 28);
    add(new KR106ButtonLED(param(kChorusII), 2, ledRed, param(kPower)), 767, 43, 17, 28);

    // === SCOPE ===
    mScope = add(new KR106Scope(&p), 791, 21, 128, 74);

    // === KEYBOARD ===
    mKeyboard = add(new KR106Keyboard(&p, chevron), 129, 106, 792, 114);

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

void KR106Editor::paint(juce::Graphics& g)
{
    // Draw @2x background at 1x logical size
    g.drawImage(mBackground,
                0.f, 0.f, 940.f, 224.f,
                0, 0, mBackground.getWidth(), mBackground.getHeight());
}

void KR106Editor::timerCallback()
{
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

    // Update scope and keyboard from processor state
    mScope->updateFromProcessor();
    mKeyboard->updateFromProcessor();

    // Repaint all controls to sync with host automation
    for (auto* ctrl : mControls)
        ctrl->repaint();
}
