#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

// ============================================================================
// KR106Switch — Vertical bitmap-based multi-position switch.
// Always uses the 3-way sprite sheet (3 frames stacked vertically, @2x).
// For 2-way params: displays frames 0 and 2 (skipping middle).
// Click cycles through states; vertical drag snaps to nearest position.
// ============================================================================
class KR106Switch : public juce::Component
{
public:
    KR106Switch(juce::RangedAudioParameter* param, const juce::Image& spriteSheet, int numPositions)
        : mParam(param)
        , mSpriteSheet(spriteSheet)
        , mNumPositions(numPositions)
    {
    }

    void paint(juce::Graphics& g) override
    {
        float val = mParam ? mParam->getValue() : 0.f;
        int pos = (int)std::round(val * (mNumPositions - 1));
        pos = juce::jlimit(0, mNumPositions - 1, pos);

        // Map position to sprite frame: 2-way uses frames 0,2; 3-way uses 0,1,2
        int spriteIdx = (mNumPositions == 2) ? (pos == 0 ? 0 : 2) : pos;

        int frameW2x = mSpriteSheet.getWidth();
        int frameH2x = mSpriteSheet.getHeight() / 3; // always 3 frames in sheet
        float frameW = frameW2x / 2.f;
        float frameH = frameH2x / 2.f;

        g.drawImage(mSpriteSheet,
                    0.f, 0.f, frameW, frameH,
                    0, spriteIdx * frameH2x, frameW2x, frameH2x);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        mDragStartY = e.position.y;
        mDragStartIdx = currentIdx();
        mDragged = false;
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!mParam || mNumPositions <= 1) return;

        float totalDrag = e.position.y - mDragStartY;
        if (std::abs(totalDrag) > 3.f) mDragged = true;

        int frameH2x = mSpriteSheet.getHeight() / 3;
        float frameH = frameH2x / 2.f;

        int steps = (int)(totalDrag / (frameH * 0.5f));
        int newIdx = mDragStartIdx + steps;
        newIdx = juce::jlimit(0, mNumPositions - 1, newIdx);

        float newVal = (mNumPositions > 1) ? (float)newIdx / (float)(mNumPositions - 1) : 0.f;
        mParam->beginChangeGesture();
        mParam->setValueNotifyingHost(newVal);
        mParam->endChangeGesture();
        repaint();
    }

    void mouseUp(const juce::MouseEvent& /*e*/) override
    {
        if (!mDragged && mParam && mNumPositions > 1)
        {
            int idx = (currentIdx() + 1) % mNumPositions;
            float newVal = (float)idx / (float)(mNumPositions - 1);
            mParam->beginChangeGesture();
            mParam->setValueNotifyingHost(newVal);
            mParam->endChangeGesture();
            repaint();
        }
    }

private:
    int currentIdx() const
    {
        float val = mParam ? mParam->getValue() : 0.f;
        return juce::jlimit(0, mNumPositions - 1, (int)std::round(val * (mNumPositions - 1)));
    }

    juce::RangedAudioParameter* mParam = nullptr;
    juce::Image mSpriteSheet;
    int mNumPositions = 2;
    float mDragStartY = 0.f;
    int mDragStartIdx = 0;
    bool mDragged = false;
};

// ============================================================================
// KR106HorizontalSwitch — Horizontal bitmap-based multi-position switch.
// Always uses the 3-way horizontal sprite sheet (3 frames stacked vertically, @2x).
// For 2-way params: displays frames 0 and 2 (skipping middle).
// Drag is horizontal instead of vertical.
// ============================================================================
class KR106HorizontalSwitch : public juce::Component
{
public:
    KR106HorizontalSwitch(juce::RangedAudioParameter* param, const juce::Image& spriteSheet, int numPositions)
        : mParam(param)
        , mSpriteSheet(spriteSheet)
        , mNumPositions(numPositions)
    {
    }

    void paint(juce::Graphics& g) override
    {
        float val = mParam ? mParam->getValue() : 0.f;
        int pos = (int)std::round(val * (mNumPositions - 1));
        pos = juce::jlimit(0, mNumPositions - 1, pos);

        // Map position to sprite frame: 2-way uses frames 0,2; 3-way uses 0,1,2
        int spriteIdx = (mNumPositions == 2) ? (pos == 0 ? 0 : 2) : pos;

        int frameW2x = mSpriteSheet.getWidth();
        int frameH2x = mSpriteSheet.getHeight() / 3; // always 3 frames in sheet
        float frameW = frameW2x / 2.f;
        float frameH = frameH2x / 2.f;

        g.drawImage(mSpriteSheet,
                    0.f, 0.f, frameW, frameH,
                    0, spriteIdx * frameH2x, frameW2x, frameH2x);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) return;
        mDragStartX = e.position.x;
        mDragStartIdx = currentIdx();
        mDragged = false;
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!mParam || mNumPositions <= 1) return;

        float totalDrag = e.position.x - mDragStartX;
        if (std::abs(totalDrag) > 3.f) mDragged = true;

        float frameW = mSpriteSheet.getWidth() / 2.f;

        int steps = (int)(totalDrag / (frameW * 0.5f));
        int newIdx = mDragStartIdx + steps;
        newIdx = juce::jlimit(0, mNumPositions - 1, newIdx);

        float newVal = (mNumPositions > 1) ? (float)newIdx / (float)(mNumPositions - 1) : 0.f;
        mParam->beginChangeGesture();
        mParam->setValueNotifyingHost(newVal);
        mParam->endChangeGesture();
        repaint();
    }

    void mouseUp(const juce::MouseEvent& /*e*/) override
    {
        if (!mDragged && mParam && mNumPositions > 1)
        {
            int idx = (currentIdx() + 1) % mNumPositions;
            float newVal = (float)idx / (float)(mNumPositions - 1);
            mParam->beginChangeGesture();
            mParam->setValueNotifyingHost(newVal);
            mParam->endChangeGesture();
            repaint();
        }
    }

private:
    int currentIdx() const
    {
        float val = mParam ? mParam->getValue() : 0.f;
        return juce::jlimit(0, mNumPositions - 1, (int)std::round(val * (mNumPositions - 1)));
    }

    juce::RangedAudioParameter* mParam = nullptr;
    juce::Image mSpriteSheet;
    int mNumPositions = 2;
    float mDragStartX = 0.f;
    int mDragStartIdx = 0;
    bool mDragged = false;
};
