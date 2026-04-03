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
#include "Controls/KR106MenuSheet.h"
#include "Controls/KR106VarianceSheet.h"

class KR106Editor : public juce::AudioProcessorEditor,
                    private juce::Timer
{
public:
    KR106Editor(KR106AudioProcessor&);
    ~KR106Editor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    bool keyStateChanged(bool isKeyDown) override;
    void showSettingsMenu();

private:
    static constexpr int kBaseWidth = 940;
    static constexpr int kBaseHeight = 224;

    int qwertyToNote(int keyCode) const;
    void qwertyAllNotesOff();

    int mQwertyBase = 48;  // C3
    bool mQwertyDown[128] = {};

    void timerCallback() override;
    void applyScale(float s);

    KR106AudioProcessor& mProcessor;
    juce::Image mBackground;
    juce::Component mContent; // inner wrapper; all controls are children of this

    juce::OwnedArray<juce::Component> mControls;
    KR106Scope* mScope = nullptr;
    KR106Keyboard* mKeyboard = nullptr;
    KR106PresetDisplay* mPresetDisplay = nullptr;
    KR106ClipLED* mClipLED = nullptr;
    KR106ArpRateSlider* mArpRateSlider = nullptr;
    KR106LfoRateSlider* mLfoRateSlider = nullptr;
    KR106Tooltip mTooltip;

    float mUIScale = 1.f;
    bool mNeedChevronRestore = true;
    bool mWasActive = true;
    int mRepaintDivider = 0;

    juce::Typeface::Ptr mMenuTypeface;
    std::unique_ptr<KR106MenuSheet> mSettingsMenu;
    std::unique_ptr<KR106VarianceSheet> mVarianceSheet;
    std::unique_ptr<juce::Component> mQwertyDiagram;

    void showVarianceSheet();
    void showQwertyDiagram();
};
