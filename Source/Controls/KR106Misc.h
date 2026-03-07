#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

class KR106AudioProcessor;

// Pixel-perfect drawing helpers
static inline void HLine(juce::Graphics& g, juce::Colour c, float x1, float y, float x2)
{
    g.setColour(c);
    g.fillRect(x1, y, x2 - x1, 1.f);
}

static inline void VLine(juce::Graphics& g, juce::Colour c, float x, float y1, float y2)
{
    g.setColour(c);
    g.fillRect(x, y1, 1.f, y2 - y1);
}

static inline void PixelRect(juce::Graphics& g, juce::Colour c, float l, float t, float r, float b)
{
    HLine(g, c, l, t, r);
    HLine(g, c, l, b - 1, r);
    VLine(g, c, l, t, b);
    VLine(g, c, r - 1, t, b);
}

// ============================================================================
// KR106PowerSwitch -- 15x19 toggle switch (power on/off)
// Port of KR106PowerSwitchControl from iPlug2
// ============================================================================
class KR106PowerSwitch : public juce::Component
{
public:
    KR106PowerSwitch(juce::RangedAudioParameter* param) : mParam(param) {}

    void paint(juce::Graphics& g) override
    {
        const auto black = juce::Colour(0, 0, 0);
        const auto gray  = juce::Colour(128, 128, 128);
        const auto red   = juce::Colour(255, 0, 0);

        bool on = mParam && mParam->getValue() > 0.5f;

        // Black background
        g.setColour(black);
        g.fillRect(0.f, 0.f, 15.f, 19.f);

        // Gray pixel rect border
        PixelRect(g, gray, 2.f, 2.f, 13.f, 17.f);

        if (on)
        {
            HLine(g, gray, 4.f, 11.f, 11.f);
            HLine(g, red,  6.f, 13.f, 10.f);
        }
        else
        {
            HLine(g, gray, 4.f, 5.f, 11.f);
        }
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        if (!mParam) return;
        float newVal = mParam->getValue() < 0.5f ? 1.f : 0.f;
        mParam->beginChangeGesture();
        mParam->setValueNotifyingHost(newVal);
        mParam->endChangeGesture();
        repaint();
    }

private:
    juce::RangedAudioParameter* mParam = nullptr;
};

// ============================================================================
// KR106LFOTrig -- 41x19 momentary button for manual LFO trigger
// Sends CC1 (mod wheel) 127 on press, 0 on release
// Port of KR106LFOTrigControl from iPlug2
// ============================================================================
class KR106LFOTrig : public juce::Component
{
public:
    KR106LFOTrig(KR106AudioProcessor* processor) : mProcessor(processor) {}

    void paint(juce::Graphics& g) override
    {
        const auto black = juce::Colour(0, 0, 0);
        const auto cream = juce::Colour(220, 220, 178);
        const auto gray  = juce::Colour(37, 37, 37);

        // Black border, cream fill
        PixelRect(g, black, 0.f, 0.f, 41.f, 19.f);
        g.setColour(cream);
        g.fillRect(1.f, 1.f, 39.f, 17.f);

        if (mPressed)
        {
            // Inner black border when pressed
            PixelRect(g, black, 1.f, 1.f, 40.f, 18.f);

            // Gray decoration lines (shifted inward by 1)
            HLine(g, gray, 6.f, 4.f, 35.f);
            HLine(g, gray, 6.f, 14.f, 35.f);
            VLine(g, gray, 5.f, 5.f, 14.f);
            VLine(g, gray, 35.f, 5.f, 14.f);
        }
        else
        {
            // Gray decoration lines
            HLine(g, gray, 5.f, 4.f, 36.f);
            HLine(g, gray, 5.f, 14.f, 36.f);
            VLine(g, gray, 4.f, 5.f, 14.f);
            VLine(g, gray, 36.f, 5.f, 14.f);
        }
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        mPressed = true;
        if (mProcessor)
            mProcessor->sendMidiFromUI(0xB0, 1, 127);
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        mPressed = false;
        if (mProcessor)
            mProcessor->sendMidiFromUI(0xB0, 1, 0);
        repaint();
    }

private:
    KR106AudioProcessor* mProcessor = nullptr;
    bool mPressed = false;
};
