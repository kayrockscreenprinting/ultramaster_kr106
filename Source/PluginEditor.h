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
    // Disable JUCE's default Tab/Shift+Tab focus traversal so we can
    // use those keys for MIDI learn navigation in keyPressed().
    std::unique_ptr<juce::ComponentTraverser> createKeyboardFocusTraverser() override { return nullptr; }
    void showSettingsMenu();

private:
    static constexpr int kBaseWidth = 940;
    static constexpr int kBaseHeight = 224;

    int qwertyToNote(int keyCode) const;
    void qwertyAllNotesOff();

    int mQwertyBase = 48;  // C3
    bool mQwertyDown[128] = {};

    void timerCallback() override;
    void applyScale(float s, bool resizeWindow = true);
    void advanceMidiLearn(int dir);

    KR106AudioProcessor& mProcessor;
    juce::Image mBackground;
    juce::Component mContent; // inner wrapper; all controls are children of this

    juce::OwnedArray<juce::Component> mControls;
    struct LearnableControl { int paramIdx; juce::Component* ctrl; };
    std::vector<LearnableControl> mLearnableControls; // UI layout order (for Tab navigation)
    std::vector<int> mLearnableParams; // param indices only (parallel, for quick lookup)
    KR106Scope* mScope = nullptr;
    KR106Keyboard* mKeyboard = nullptr;
    KR106PresetDisplay* mPresetDisplay = nullptr;
    KR106ClipLED* mClipLED = nullptr;
    KR106ArpRateSlider* mArpRateSlider = nullptr;
    KR106LfoRateSlider* mLfoRateSlider = nullptr;
    KR106Tooltip mTooltip;

    float mUIScale = 1.f;
    bool mInternalResize = false; // guard: true when applyScale() initiated the resize
    bool mNeedChevronRestore = true;
    bool mWasActive = true;
    int mRepaintDivider = 0;

    juce::Typeface::Ptr mMenuTypeface;
    std::unique_ptr<KR106MenuSheet> mSettingsMenu;
    std::unique_ptr<KR106VarianceSheet> mVarianceSheet;
    std::unique_ptr<juce::Component> mQwertyDiagram;

    void showVarianceSheet();
    void showQwertyDiagram();

    // Click debugger for AU hit-test investigation
    class ClickDebugger : public juce::MouseListener
    {
    public:
        juce::Point<int>& lastClickRef;
        explicit ClickDebugger(juce::Component& rootIn, juce::Point<int>& clickRef)
            : root(rootIn), lastClickRef(clickRef)
        {
            auto logFile = KR106PresetManager::getAppDataDir()
                             .getChildFile("KR106-clicks.log");
            log = std::make_unique<juce::FileLogger>(logFile, "KR-106 click debug", 0);
            log->logMessage("KR-106 " JucePlugin_VersionString
                " / " + juce::SystemStats::getOperatingSystemName()
                + " / host: " + juce::PluginHostType().getHostDescription()
                + " / editor size: " + juce::String(root.getWidth()) + "x" + juce::String(root.getHeight()));
        }
        void logDirect(juce::Point<int> pos)
        {
            log->logMessage("  [editor mouseDown] root: " + juce::String(pos.x) + ", " + juce::String(pos.y));
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            const auto rootPos = e.getEventRelativeTo(&root).getPosition();
            lastClickRef = rootPos;
            root.repaint();
            log->logMessage("=== CLICK (listener) ===");
            log->logMessage("  screen:     " + juce::String(e.getScreenPosition().x) + ", " + juce::String(e.getScreenPosition().y));
            log->logMessage("  root:       " + juce::String(rootPos.x) + ", " + juce::String(rootPos.y));
            log->logMessage("  root size:  " + juce::String(root.getWidth()) + " x " + juce::String(root.getHeight()));
            log->logMessage("  originator: " + describe(e.eventComponent));
            if (auto* hit = root.getComponentAt(rootPos))
            {
                log->logMessage("  hit test:   " + describe(hit));
                int depth = 0;
                for (auto* c = hit; c != nullptr; c = c->getParentComponent())
                {
                    bool selfI = false, childI = false;
                    c->getInterceptsMouseClicks(selfI, childI);
                    log->logMessage("    [" + juce::String(depth++) + "] " + describe(c)
                        + "  bounds=" + c->getBounds().toString()
                        + "  self=" + juce::String((int)selfI) + "  child=" + juce::String((int)childI)
                        + "  visible=" + juce::String((int)c->isVisible()));
                    if (c == &root) break;
                }
            }
            else
                log->logMessage("  hit test:   <nothing>");
        }
    private:
        static juce::String describe(juce::Component* c)
        {
            if (!c) return "<null>";
            auto name = c->getName();
            if (name.isEmpty()) name = "<unnamed>";
            return name + " (" + juce::String(typeid(*c).name()) + ")";
        }
        juce::Component& root;
        std::unique_ptr<juce::FileLogger> log;
    };
    std::unique_ptr<ClickDebugger> mClickDebugger;
    bool mClickDebugEnabled = false;

    // Visual click marker
    juce::Point<int> mLastClickPoint { -1, -1 };
    void paintOverChildren(juce::Graphics& g) override
    {
        if (mClickDebugEnabled && mLastClickPoint.x >= 0)
        {
            g.setColour(juce::Colours::magenta);
            g.fillEllipse((float)mLastClickPoint.x - 6, (float)mLastClickPoint.y - 6, 12, 12);
            g.setColour(juce::Colours::white);
            g.drawEllipse((float)mLastClickPoint.x - 6, (float)mLastClickPoint.y - 6, 12, 12, 1);
        }
    }
};
