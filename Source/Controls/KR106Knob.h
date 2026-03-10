#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "KR106Tooltip.h"

// ============================================================================
// KR106Knob — Bitmap knob with manhattan distance drag.
// Sprite sheet is @2x, N frames in a horizontal strip.
// Frame count is auto-detected: numFrames = imageWidth / imageHeight.
// Both vertical and horizontal mouse movement contribute to the value.
// Up and right increase; down and left decrease.
// ============================================================================
class KR106Knob : public juce::Component
{
public:
    KR106Knob(juce::RangedAudioParameter* param, const juce::Image& spriteSheet,
              KR106Tooltip* tip = nullptr, int numFrames = 0)
        : mParam(param)
        , mSpriteSheet(spriteSheet)
        , mTooltip(tip)
        , mNumFrames(numFrames > 0 ? numFrames
                     : (spriteSheet.getHeight() > 0 ? spriteSheet.getWidth() / spriteSheet.getHeight() : 32))
    {
    }

    void setBackgroundImage(const juce::Image& img) { mBgImage = img; }

    void paint(juce::Graphics& g) override
    {
        float val = mParam ? mParam->getValue() : 0.f;
        int maxIdx = mNumFrames - 1;
        int idx = (int)std::round(val * (float)maxIdx);
        idx = juce::jlimit(0, maxIdx, idx);

        int frameW2x = mSpriteSheet.getWidth() / mNumFrames;
        int h2x = mSpriteSheet.getHeight();
        float frameW = frameW2x / 2.f;
        float frameH = h2x / 2.f;

        float w = (float)getWidth();
        float h = (float)getHeight();

        if (mBgImage.isValid())
        {
            g.drawImage(mBgImage,
                        0.f, 0.f, w, h,
                        0, 0, mBgImage.getWidth(), mBgImage.getHeight());
        }

        // Centre sprite within bounds
        float ox = (w - frameW) * 0.5f;
        float oy = (h - frameH) * 0.5f;
        g.drawImage(mSpriteSheet,
                    ox, oy, frameW, frameH,                        // dest: 1x
                    idx * frameW2x, 0, frameW2x, h2x);            // src: 2x
    }

    void mouseEnter(const juce::MouseEvent&) override
    {
        if (mTooltip && !mDragging) mTooltip->show(mParam, this);
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (mTooltip && !mDragging) mTooltip->hide();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        mDragging = true;
        mAccumVal = mParam ? mParam->getValue() : 0.f;
        mLastRawDelta = 0.f;
        if (mParam) mParam->beginChangeGesture();
        if (mTooltip) mTooltip->show(mParam, this);
        setMouseCursor(juce::MouseCursor::NoCursor);
        e.source.enableUnboundedMouseMovement(true);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!mParam) return;

        float offsetY = (float)e.getOffsetFromDragStart().y;
        float offsetX = (float)e.getOffsetFromDragStart().x;

        // Manhattan drag: -(dY - dX) so up+right = increase
        float rawDelta = -(offsetY - offsetX);
        float increment = rawDelta - mLastRawDelta;
        mLastRawDelta = rawDelta;

        // Cumulative gearing: shift only affects new movement
        // Scale by display DPI so non-retina screens get the same physical throw
        float dpiScale = std::max(1.f, static_cast<float>(getTopLevelComponent()->getDesktopScaleFactor()));
        float gearing = (e.mods.isShiftDown() ? 1270.f : 127.f) / dpiScale;
        mAccumVal = juce::jlimit(0.f, 1.f, mAccumVal + increment / gearing);
        float newVal = mAccumVal;

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
        mDragging = false;
        if (mParam) mParam->endChangeGesture();
        if (mTooltip) mTooltip->hide();
        setMouseCursor(juce::MouseCursor::NormalCursor);
        e.source.enableUnboundedMouseMovement(false);

        // Warp cursor to centre of knob so it appears on the control after release
        auto screenPos = localPointToGlobal(juce::Point<int>(getWidth() / 2, getHeight() / 2));
        juce::Desktop::getInstance().getMainMouseSource().setScreenPosition(screenPos.toFloat());
    }

private:
    juce::RangedAudioParameter* mParam = nullptr;
    juce::Image mSpriteSheet;
    KR106Tooltip* mTooltip = nullptr;
    int mNumFrames = 32;
    juce::Image mBgImage;
    float mAccumVal = 0.f;
    float mLastRawDelta = 0.f;
    bool mDragging = false;
};
