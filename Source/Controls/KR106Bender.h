#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>

// ============================================================================
// KR106Bender -- 60x8 horizontal pitch bend lever
// Gradient bitmap background, trig-based pointer, spring-back to center
// Port of KR106BenderControl from iPlug2
// ============================================================================
class KR106Bender : public juce::Component
{
public:
    KR106Bender(juce::RangedAudioParameter* param, const juce::Image& gradientImage)
        : mParam(param), mGradient(gradientImage)
    {}

    void paint(juce::Graphics& g) override
    {
        const auto black = juce::Colour(0, 0, 0);
        const auto gray  = juce::Colour(128, 128, 128);
        const auto white = juce::Colour(255, 255, 255);

        // Black background (4,0) to (56,8)
        g.setColour(black);
        g.fillRect(4.f, 0.f, 52.f, 8.f);

        // Gradient image at (5,1), draw @2x source (100x12) at 1x size (50x6)
        g.drawImage(mGradient, 5.f, 1.f, 50.f, 6.f,
                    0, 0, mGradient.getWidth(), mGradient.getHeight());

        // Pointer -- param value normalized 0-1, map to -1..+1
        float value = mParam ? mParam->getValue() * 2.f - 1.f : 0.f;
        float midpoint = 30.f;
        float angle = juce::MathConstants<float>::pi * (2.f - value) / 4.f;

        float basex1  = cosf(angle + juce::MathConstants<float>::pi / 20.f) * 24.f + midpoint;
        float basex2  = cosf(angle - juce::MathConstants<float>::pi / 20.f) * 24.f + midpoint;
        float pointx1 = cosf(angle + juce::MathConstants<float>::pi / 50.f) * 36.f + midpoint;
        float pointx2 = cosf(angle - juce::MathConstants<float>::pi / 50.f) * 36.f + midpoint;

        // Gray outer shape
        g.setColour(gray);
        if (basex1 < pointx1)
            g.fillRect(basex1, 1.f, pointx2 + 2.f - basex1, 6.f);
        else
            g.fillRect(pointx1, 1.f, basex2 + 1.f - pointx1, 6.f);

        // White inner pointer
        g.setColour(white);
        g.fillRect(pointx1, 1.f, pointx2 + 1.f - pointx1, 6.f);

        // Gray corner pixels
        g.setColour(gray);
        g.fillRect(pointx1, 1.f, 1.f, 1.f);
        g.fillRect(pointx1, 6.f, 1.f, 1.f);
        g.fillRect(pointx2, 1.f, 1.f, 1.f);
        g.fillRect(pointx2, 6.f, 1.f, 1.f);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (!mParam) return;

        // Map click x to bender value
        float lx = static_cast<float>(e.getPosition().x);
        float midpoint = 30.f;
        float value = (lx - midpoint) / midpoint;
        value = juce::jlimit(-1.f, 1.f, value);

        mParam->beginChangeGesture();
        mParam->setValueNotifyingHost((value + 1.f) / 2.f);
        mDragStartVal = (value + 1.f) / 2.f;
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!mParam) return;

        float scale = e.mods.isShiftDown() ? 512.f : 128.f;
        float dX = static_cast<float>(e.getDistanceFromDragStartX());
        float value = mDragStartVal * 2.f - 1.f;
        value += dX / scale;
        value = juce::jlimit(-1.f, 1.f, value);

        mParam->setValueNotifyingHost((value + 1.f) / 2.f);
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (!mParam) return;

        // Spring back to center
        mParam->setValueNotifyingHost(0.5f);
        mParam->endChangeGesture();
        repaint();
    }

private:
    juce::RangedAudioParameter* mParam = nullptr;
    juce::Image mGradient;
    float mDragStartVal = 0.5f;
};
