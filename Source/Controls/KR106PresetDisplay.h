#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"
#include "PluginProcessor.h"

// Small text display showing the current preset name.
// Drag up/down to scroll through presets. Right-click for preset management.
class KR106PresetDisplay : public juce::Component
{
public:
    KR106PresetDisplay(KR106AudioProcessor* processor)
        : mProcessor(processor)
    {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
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
        if (mProcessor->mInitialDefault)
            name = "Default";
        else
        {
            int idx = mProcessor->getCurrentProgram();
            name = mProcessor->getProgramName(idx).substring(0, 18);
        }

        // Draw preset name left-aligned in green (Segment14 font)
        // Use drawSingleLineText at a fixed baseline to bypass JUCE font metrics
        g.setColour(juce::Colour(0, 220, 0));
        g.setFont(juce::Font(juce::FontOptions(mTypeface).withMetricsKind(juce::TypefaceMetricsKind::legacy)).withHeight(8.f));
        g.drawSingleLineText(name, 3, h - 3);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            showContextMenu();
            return;
        }
        mDragAccum = 0.f;
        setMouseCursor(juce::MouseCursor::NoCursor);
        e.source.enableUnboundedMouseMovement(true);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!mProcessor) return;

        float dy = static_cast<float>(e.getDistanceFromDragStartY()) - mDragAccum;
        mDragAccum = static_cast<float>(e.getDistanceFromDragStartY());

        mDragRemainder += dy;
        static constexpr float kThreshold = 8.f;

        while (mDragRemainder <= -kThreshold)
        {
            mDragRemainder += kThreshold;
            changePreset(-1);
        }
        while (mDragRemainder >= kThreshold)
        {
            mDragRemainder -= kThreshold;
            changePreset(1);
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        mDragRemainder = 0.f;
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        e.source.enableUnboundedMouseMovement(false);
        auto screenPos = localPointToGlobal(juce::Point<int>(getWidth() / 2, getHeight() / 2));
        juce::Desktop::getInstance().getMainMouseSource().setScreenPosition(screenPos.toFloat());
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

    void showContextMenu()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Overwrite Patch...", mProcessor->isCurrentPresetDirty());
        menu.addSeparator();
        menu.addItem(3, "Copy Patch");
        menu.addItem(4, "Paste Patch", mHasClipboard);
        menu.addItem(5, "Clear Patch");
        menu.addSeparator();
        menu.addItem(6, "Load Patch Bank...");
      #if JUCE_MAC
        menu.addItem(7, "Reveal in Finder");
      #elif JUCE_WINDOWS
        menu.addItem(7, "Show in File Explorer");
      #else
        menu.addItem(7, "Show in File Manager");
      #endif

        juce::Component::SafePointer<KR106PresetDisplay> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options(),
            [safeThis](int result)
            {
                if (safeThis == nullptr) return;
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
    }

    void showSaveDialog()
    {
        int idx = mProcessor->getCurrentProgram();
        auto preset = mProcessor->getPreset(idx);

        auto* alert = new juce::AlertWindow("Save Preset", "", juce::MessageBoxIconType::NoIcon);
        alert->addTextEditor("name", preset.name, "Preset name:");
        alert->addButton("Save", 1);
        alert->addButton("Cancel", 0);

        juce::Component::SafePointer<KR106PresetDisplay> safeThis(this);
        alert->enterModalState(true, juce::ModalCallbackFunction::create(
            [safeThis, alert](int result)
            {
                if (result == 1 && safeThis != nullptr)
                {
                    juce::String name = alert->getTextEditorContents("name").trim();
                    if (name.isNotEmpty())
                    {
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
        mProcessor->clearCurrentPreset();
        repaint();
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
    float mDragAccum = 0.f;
    float mDragRemainder = 0.f;

    // Copy/paste clipboard
    KR106Preset mClipboard;
    bool mHasClipboard = false;

    // FileChooser must outlive async callback
    std::unique_ptr<juce::FileChooser> mFileChooser;
};
