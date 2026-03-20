#pragma once

#include "PluginProcessor.h"
#include "Controls/KR106Slider.h"
#include "Controls/KR106Button.h"
#include "Controls/KR106Switch.h"
#include "Controls/KR106Knob.h"
#include "Controls/KR106Bender.h"
#include "Controls/KR106Misc.h"
#include "Controls/KR106Scope.h"
#include "Controls/KR106Keyboard.h"
#include "Controls/KR106Tooltip.h"
#include "Controls/KR106PresetDisplay.h"

class KR106Editor : public juce::AudioProcessorEditor,
                    private juce::Timer
{
public:
    KR106Editor(KR106AudioProcessor&);
    ~KR106Editor() override;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    void timerCallback() override;

    KR106AudioProcessor& mProcessor;
    juce::Image mBackground;

    juce::OwnedArray<juce::Component> mControls;
    KR106Scope* mScope = nullptr;
    KR106Keyboard* mKeyboard = nullptr;
    KR106ClipLED* mClipLED = nullptr;
    KR106ClipLED* mClipLED2 = nullptr;
    KR106Tooltip mTooltip;

    float mUIScale = 1.f;
    bool mNeedChevronRestore = true;
    bool mWasActive = true;
    int mRepaintDivider = 0;
};
