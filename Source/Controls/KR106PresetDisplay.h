#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"
#include "PluginProcessor.h"
#include "KR106MenuSheet.h"

// ============================================================================
// KR106PresetSheet — full-screen overlay showing all 128 presets in 8 columns
// Black background, green 14-segment font. Click to select, Escape to close.
// ============================================================================
class KR106PresetSheet : public juce::Component
{
public:
    static constexpr int kCols = 8;
    static constexpr int kRows = 16;
    static constexpr int kPadX = 4;

    float rowH() const { return getHeight() / static_cast<float>(kRows); }

    KR106PresetSheet(KR106AudioProcessor* proc, juce::Typeface::Ptr typeface,
                     std::function<void()> onClose)
        : mProcessor(proc), mTypeface(typeface), mOnClose(std::move(onClose))
    {
        setWantsKeyboardFocus(true);
        setAlwaysOnTop(true);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(KR106Theme::bg());

        if (!mProcessor) return;

        int current = mProcessor->getCurrentProgram();
        int num = mProcessor->getNumPrograms();
        float rh = rowH();
        int colW = getWidth() / kCols;

        g.setFont(KR106Theme::ledFont(mTypeface));

        // Grid lines
        g.setColour(KR106Theme::grid());
        for (int row = 0; row <= kRows; row++)
        {
            int y = juce::roundToInt(row * rh);
            g.drawHorizontalLine(y, 0.f, static_cast<float>(getWidth()));
        }
        for (int col = 1; col < kCols; col++)
        {
            int x = col * colW;
            g.drawVerticalLine(x, 0.f, static_cast<float>(getHeight()));
        }

        // Preset entries
        for (int i = 0; i < num && i < kCols * kRows; i++)
        {
            int col = i % kCols;
            int row = i / kCols;
            int x = col * colW;
            int y = juce::roundToInt(row * rh);
            int cellH = juce::roundToInt((row + 1) * rh) - y;

            juce::String name = mProcessor->getProgramName(i);
            KR106Theme::drawCell(g, name.substring(0, 14), x, y, colW, cellH,
                                 i == mHoverIndex, i == current);
        }

        g.setColour(KR106Theme::border());
        g.drawRect(getLocalBounds());

        // [x] close button in top-right corner
        int bx = getWidth() - kCloseSize;
        auto closeBounds = juce::Rectangle<int>(bx, 0, kCloseSize, kCloseSize);
        g.setColour(mHoverClose ? KR106Theme::hoverBg() : KR106Theme::bg());
        g.fillRect(closeBounds);
        g.setColour(KR106Theme::border());
        g.drawRect(closeBounds);
        g.setColour(KR106Theme::bright());
        g.setFont(juce::Font(juce::FontOptions(mTypeface)
            .withMetricsKind(juce::TypefaceMetricsKind::legacy)).withHeight(10.f));
        g.drawText("x", closeBounds.translated(2, 0), juce::Justification::centred);
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        bool overClose = closeRect().contains(e.getPosition());
        int idx = overClose ? -1 : hitTest(e.getPosition());
        if (idx != mHoverIndex || overClose != mHoverClose)
        {
            mHoverIndex = idx;
            mHoverClose = overClose;
            repaint();
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (closeRect().contains(e.getPosition()))
        {
            dismiss();
            return;
        }
        int idx = hitTest(e.getPosition());
        if (idx >= 0)
        {
            mProcessor->setCurrentProgram(idx);
        }
        dismiss();
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (mHoverIndex >= 0)
        {
            mHoverIndex = -1;
            repaint();
        }
    }



    void focusLost(FocusChangeType) override
    {
        dismiss();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        int num = mProcessor ? mProcessor->getNumPrograms() : 0;
        int maxIdx = std::min(num, kCols * kRows) - 1;
        if (maxIdx < 0) return false;

        // Initialize nav index from current preset if not yet navigating
        if (mHoverIndex < 0)
            mHoverIndex = mProcessor->getCurrentProgram();

        int col = mHoverIndex % kCols;
        int row = mHoverIndex / kCols;

        if (key == juce::KeyPress::leftKey)
            col = (col - 1 + kCols) % kCols;
        else if (key == juce::KeyPress::rightKey)
            col = (col + 1) % kCols;
        else if (key == juce::KeyPress::upKey)
            row = (row - 1 + kRows) % kRows;
        else if (key == juce::KeyPress::downKey)
            row = (row + 1) % kRows;
        else if (key == juce::KeyPress::returnKey)
        {
            mProcessor->setCurrentProgram(mHoverIndex);
            dismiss();
            return true;
        }
        else if (key == juce::KeyPress::escapeKey)
        {
            dismiss();
            return true;
        }
        else
            return false;

        int newIdx = row * kCols + col;
        if (newIdx > maxIdx) newIdx = maxIdx;
        mHoverIndex = newIdx;
        repaint();
        return true;
    }

private:
    static constexpr int kCloseSize = 14;

    juce::Rectangle<int> closeRect() const
    {
        return { getWidth() - kCloseSize, 0, kCloseSize, kCloseSize };
    }

    int hitTest(juce::Point<int> pos) const
    {
        float rh = rowH();
        int colW = getWidth() / kCols;
        int col = pos.x / colW;
        int row = static_cast<int>(pos.y / rh);
        if (col < 0 || col >= kCols || row < 0 || row >= kRows) return -1;
        int idx = row * kCols + col;
        int num = mProcessor ? mProcessor->getNumPrograms() : 0;
        return (idx >= 0 && idx < num) ? idx : -1;
    }

    void dismiss()
    {
        setVisible(false);
        if (mOnClose) mOnClose();
    }

    KR106AudioProcessor* mProcessor = nullptr;
    juce::Typeface::Ptr mTypeface;
    std::function<void()> mOnClose;
    int mHoverIndex = -1;
    bool mHoverClose = false;
};

// ============================================================================
// KR106PresetDisplay — preset name display with click-to-browse
// Left-click: preset sheet. Right-click: context menu. Scroll: browse.
// ============================================================================
class KR106PresetDisplay : public juce::Component
{
public:
    KR106PresetDisplay(KR106AudioProcessor* processor)
        : mProcessor(processor)
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        mTypeface = juce::Typeface::createSystemTypefaceFor(
            BinaryData::Segment14_otf,
            BinaryData::Segment14_otfSize);
    }

    void paint(juce::Graphics& g) override
    {
        int w = getWidth(), h = getHeight();

        // Black background, no border
        g.setColour(juce::Colour(0, 0, 0));
        g.fillRect(0, 0, w, h);

        if (!mProcessor) return;

        // Get current preset name
        juce::String name;
        bool dirty = false;
        if (mProcessor->mInitialDefault)
            name = "Manual";
        else
        {
            int idx = mProcessor->getCurrentProgram();
            name = mProcessor->getProgramName(idx).substring(0, 18);
            dirty = mProcessor->isCurrentPresetDirty();
        }

        // Draw preset name left-aligned in green (Segment14 font)
        // Use drawSingleLineText at a fixed baseline to bypass JUCE font metrics
        g.setColour(juce::Colour(0, 220, 0));
        auto font = juce::Font(juce::FontOptions(mTypeface).withMetricsKind(juce::TypefaceMetricsKind::legacy)).withHeight(8.f);
        g.setFont(font);
        g.drawSingleLineText(name, 3, h - 3);

        // Blinking ~ at right edge when preset has been modified
        // Toggles every 4 repaints (~1Hz at 7.5Hz repaint rate)
        if (dirty)
        {
            mBlinkCounter++;
            if ((mBlinkCounter / 4) & 1)
                g.drawSingleLineText(juce::String::charToString(0x005F),
                                     w - 8, h - 3);
        }
        else
        {
            mBlinkCounter = 0;
        }
    }

    void openPresetSheet() { showPresetSheet(); }
    void openContextMenu() { showContextMenu(); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            showContextMenu();
            return;
        }
        showPresetSheet();
    }

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        if (!mProcessor) return;
        if (wheel.deltaY > 0.f)
            changePreset(1);
        else if (wheel.deltaY < 0.f)
            changePreset(-1);
    }

private:
    void changePreset(int delta)
    {
        int num = mProcessor->getNumPrograms();
        if (num <= 0) return;
        int idx = mProcessor->getCurrentProgram() + delta;
        idx = ((idx % num) + num) % num; // wrap around
        mProcessor->setCurrentProgram(idx);
        repaint();
    }

    void showPresetSheet()
    {
        if (!mProcessor || mSheet) return;

        auto* editor = findParentComponentOfClass<juce::AudioProcessorEditor>();
        juce::Component* target = editor ? static_cast<juce::Component*>(editor) : getTopLevelComponent();
        if (!target) return;

        int sheetH = target->getHeight();
        int sheetW = target->getWidth();
        int sheetY = 0;

        mSheet = std::make_unique<KR106PresetSheet>(mProcessor, mTypeface,
            [this]() {
                // Deferred cleanup — can't delete during callback
                juce::MessageManager::callAsync([this]() {
                    if (mSheet)
                    {
                        if (auto* parent = mSheet->getParentComponent())
                            parent->removeChildComponent(mSheet.get());
                        mSheet.reset();
                    }
                    repaint();
                });
            });

        mSheet->setBounds(0, sheetY, sheetW, sheetH);
        target->addAndMakeVisible(mSheet.get());
        mSheet->grabKeyboardFocus();
    }

    void showContextMenu()
    {
        if (mContextMenu) return;

        std::vector<KR106MenuItem> items;
        items.push_back(KR106MenuItem::makeAction(1, "Save Patch"));
        items.push_back(KR106MenuItem::makeAction(5, "Clear Patch"));
        items.push_back(KR106MenuItem::sep());
        items.push_back(KR106MenuItem::makeAction(6, "Load Patch Bank"));
      #if JUCE_MAC
        items.push_back(KR106MenuItem::makeAction(7, "Reveal in Finder"));
      #elif JUCE_WINDOWS
        items.push_back(KR106MenuItem::makeAction(7, "Show in Explorer"));
      #else
        items.push_back(KR106MenuItem::makeAction(7, "Show in Files"));
      #endif

        juce::Component* editor = findParentComponentOfClass<juce::AudioProcessorEditor>();
        if (!editor) editor = getTopLevelComponent();
        if (!editor) return;

        juce::Component::SafePointer<KR106PresetDisplay> safeThis(this);
        mContextMenu = std::make_unique<KR106MenuSheet>(std::move(items), mTypeface,
            [safeThis](int result)
            {
                if (safeThis == nullptr) return;
                safeThis->mContextMenu.reset();
                switch (result)
                {
                    case 1: safeThis->showSaveDialog(); break;
                    case 3: safeThis->copyPreset(); break;
                    case 4: safeThis->pastePreset(); break;
                    case 5: safeThis->clearPreset(); break;
                    case 6: safeThis->showLoadDialog(); break;
                    case 7: safeThis->revealPresetFile(); break;
                }
            });

        int menuH = mContextMenu->calcHeight();
        int menuW = mContextMenu->calcWidth();
        auto displayInEditor = editor->getLocalArea(this, getLocalBounds());
        int menuX = displayInEditor.getX() - menuW;
        int menuY = (editor->getHeight() - menuH) / 2;

        editor->addAndMakeVisible(mContextMenu.get());
        mContextMenu->showAt({ menuX, menuY, menuW, menuH });
    }

    void showSaveDialog()
    {
        int idx = mProcessor->getCurrentProgram();
        auto preset = mProcessor->getPreset(idx);

        // Bank (A/B) and patch (1-8, 1-8) from current index
        int bank = idx / 64;        // 0=A, 1=B
        int group = (idx % 64) / 8; // 0-7
        int patch = idx % 8;        // 0-7

        auto* alert = new juce::AlertWindow("Save Preset", "Location:", juce::MessageBoxIconType::NoIcon);
        alert->addComboBox("bank", {"A", "B"}, "Bank:");
        alert->addComboBox("group", {"1", "2", "3", "4", "5", "6", "7", "8"}, "Group:");
        alert->addComboBox("patch", {"1", "2", "3", "4", "5", "6", "7", "8"}, "Patch:");
        alert->addTextEditor("name", preset.name, "Name:");

        alert->getComboBoxComponent("bank")->setSelectedItemIndex(bank, juce::dontSendNotification);
        alert->getComboBoxComponent("group")->setSelectedItemIndex(group, juce::dontSendNotification);
        alert->getComboBoxComponent("patch")->setSelectedItemIndex(patch, juce::dontSendNotification);

        alert->addButton("Save", 1);
        alert->addButton("Cancel", 0);

        juce::Component::SafePointer<KR106PresetDisplay> safeThis(this);
        alert->enterModalState(true, juce::ModalCallbackFunction::create(
            [safeThis, alert](int result)
            {
                if (result == 1 && safeThis != nullptr)
                {
                    juce::String name = alert->getTextEditorContents("name").trim();
                    int b = alert->getComboBoxComponent("bank")->getSelectedItemIndex();
                    int g = alert->getComboBoxComponent("group")->getSelectedItemIndex();
                    int p = alert->getComboBoxComponent("patch")->getSelectedItemIndex();
                    int targetIdx = b * 64 + g * 8 + p;

                    if (name.isNotEmpty())
                    {
                        // Switch to target slot, then save
                        safeThis->mProcessor->setCurrentProgramIndex(targetIdx);
                        safeThis->mProcessor->saveCurrentPresetToCSV(name);
                        safeThis->repaint();
                    }
                }
                delete alert;
            }), false);
    }

    void copyPreset()
    {
        mClipboard = mProcessor->getPreset(mProcessor->getCurrentProgram());
        mHasClipboard = true;
    }

    void pastePreset()
    {
        if (!mHasClipboard) return;
        mProcessor->pastePreset(mClipboard);
        repaint();
    }

    void clearPreset()
    {
        int idx = mProcessor->getCurrentProgram();
        auto name = mProcessor->getProgramName(idx);

        auto* alert = new juce::AlertWindow("Clear Patch",
            "Reset \"" + name + "\" to INIT?",
            juce::MessageBoxIconType::WarningIcon);
        alert->addButton("Clear", 1);
        alert->addButton("Cancel", 0);

        juce::Component::SafePointer<KR106PresetDisplay> safeThis(this);
        alert->enterModalState(true, juce::ModalCallbackFunction::create(
            [safeThis, alert](int result)
            {
                if (result == 1 && safeThis != nullptr)
                {
                    safeThis->mProcessor->clearCurrentPreset();
                    safeThis->repaint();
                }
                delete alert;
            }), false);
    }

    void showLoadDialog()
    {
        mFileChooser = std::make_unique<juce::FileChooser>(
            "Load Preset CSV",
            KR106PresetManager::getDefaultCSVPath().getParentDirectory(),
            "*.csv");

        auto flags = juce::FileBrowserComponent::openMode |
                     juce::FileBrowserComponent::canSelectFiles;

        juce::Component::SafePointer<KR106PresetDisplay> safeThis(this);
        mFileChooser->launchAsync(flags, [safeThis](const juce::FileChooser& fc)
        {
            if (safeThis == nullptr) return;
            auto result = fc.getResult();
            if (result.existsAsFile())
            {
                safeThis->mProcessor->reloadPresetsFromFile(result);
                safeThis->repaint();
            }
        });
    }

    void revealPresetFile()
    {
        auto csvFile = KR106PresetManager::getDefaultCSVPath();
        if (csvFile.existsAsFile())
            csvFile.revealToUser();
        else
            csvFile.getParentDirectory().revealToUser();
    }

    KR106AudioProcessor* mProcessor = nullptr;
    juce::Typeface::Ptr mTypeface;
    int mBlinkCounter = 0;

    // Preset sheet overlay
    std::unique_ptr<KR106PresetSheet> mSheet;

    // Context menu overlay
    std::unique_ptr<KR106MenuSheet> mContextMenu;

    // Copy/paste clipboard
    KR106Preset mClipboard;
    bool mHasClipboard = false;

    // FileChooser must outlive async callback
    std::unique_ptr<juce::FileChooser> mFileChooser;
};
