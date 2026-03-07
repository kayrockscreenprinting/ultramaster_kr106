#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "KR106Tooltip.h"

// ============================================================================
// KR106Knob — Bitmap knob with manhattan distance drag.
// Sprite sheet is @2x, 32 frames in a horizontal strip.
// For knob@2x: 1792x54 -> each frame is 56x54 @2x = 28x27 @1x.
// Both vertical and horizontal mouse movement contribute to the value.
// Up and right increase; down and left decrease.
// ============================================================================
class KR106Knob : public juce::Component
{
public:
    KR106Knob(juce::RangedAudioParameter* param, const juce::Image& spriteSheet,
              KR106Tooltip* tip = nullptr)
        : mParam(param)
        , mSpriteSheet(spriteSheet)
        , mTooltip(tip)
    {
    }

    void paint(juce::Graphics& g) override
    {
        float val = mParam ? mParam->getValue() : 0.f;
        int idx = (int)std::round(val * 31.f);
        idx = juce::jlimit(0, 31, idx);

        int frameW2x = mSpriteSheet.getWidth() / 32;
        int h2x = mSpriteSheet.getHeight();
        float frameW = frameW2x / 2.f;
        float frameH = h2x / 2.f;

        g.drawImage(mSpriteSheet,
                    0.f, 0.f, frameW, frameH,                      // dest: 1x
                    idx * frameW2x, 0, frameW2x, h2x);             // src: 2x
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        mDragStartVal = mParam ? mParam->getValue() : 0.f;
        if (mParam) mParam->beginChangeGesture();
        if (mTooltip) mTooltip->show(mParam, this);
        setMouseCursor(juce::MouseCursor::NoCursor);
        e.source.enableUnboundedMouseMovement(true);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!mParam) return;

        float gearing = e.mods.isShiftDown() ? 20.f : 2.f;
        float offsetY = (float)e.getOffsetFromDragStart().y;
        float offsetX = (float)e.getOffsetFromDragStart().x;

        // Manhattan drag: -(dY - dX) so up+right = increase
        float deltaVal = -(offsetY - offsetX) / ((float)getHeight() * gearing);
        float newVal = juce::jlimit(0.f, 1.f, mDragStartVal + deltaVal);

        mParam->setValueNotifyingHost(newVal);
        if (mTooltip) mTooltip->update();
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& /*e*/) override
    {
        if (!mParam) return;
        mParam->beginChangeGesture();
        mParam->setValueNotifyingHost(mParam->getDefaultValue());
        mParam->endChangeGesture();
        repaint();
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (mParam) mParam->endChangeGesture();
        if (mTooltip) mTooltip->hide();
        setMouseCursor(juce::MouseCursor::NormalCursor);
        e.source.enableUnboundedMouseMovement(false);
    }

private:
    juce::RangedAudioParameter* mParam = nullptr;
    juce::Image mSpriteSheet;
    KR106Tooltip* mTooltip = nullptr;
    float mDragStartVal = 0.f;
};
