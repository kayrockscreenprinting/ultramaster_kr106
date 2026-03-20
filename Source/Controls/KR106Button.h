#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

// Draw a 3D bevel button (pixel-perfect, no AA).
// Types: 0=Cream, 1=Yellow, 2=Orange.
// When pressed: highlight and shadow swap.
static inline void DrawKR106Button(juce::Graphics& g, float l, float t, float r, float b, int type, bool pressed)
{
    struct ColorTriplet { juce::Colour highlight, face, shadow; };

    static const ColorTriplet colors[3] = {
        { juce::Colour(253, 247, 203), juce::Colour(220, 220, 178), juce::Colour(178, 179, 144) }, // Cream
        { juce::Colour(253, 247, 203), juce::Colour(228, 229, 88),  juce::Colour(180, 179, 69)  }, // Yellow
        { juce::Colour(253, 247, 203), juce::Colour(240, 173, 56),  juce::Colour(180, 126, 41)  }, // Orange
    };

    const auto& c = colors[type];
    const auto hi = pressed ? c.shadow : c.highlight;
    const auto lo = pressed ? c.highlight : c.shadow;
    const auto black = juce::Colour(0, 0, 0);

    // Face fill
    g.setColour(c.face);
    g.fillRect(l + 2, t + 2, r - l - 4, b - t - 4);

    // 3D bevel — top/left = highlight, bottom/right = shadow
    g.setColour(hi);
    g.fillRect(l + 1, t + 1, r - l - 2, 1.f);   // top highlight
    g.fillRect(l + 1, t + 1, 1.f, b - t - 2);    // left highlight
    g.setColour(lo);
    g.fillRect(l + 2, b - 2, r - l - 3, 1.f);    // bottom shadow
    g.fillRect(r - 2, t + 2, 1.f, b - t - 3);    // right shadow

    // 1px black border
    g.setColour(black);
    g.fillRect(l, t, r - l, 1.f);                 // top
    g.fillRect(l, b - 1, r - l, 1.f);             // bottom
    g.fillRect(l, t, 1.f, b - t);                 // left
    g.fillRect(r - 1, t, 1.f, b - t);             // right
}

// ============================================================================
// KR106ButtonLED — Button with LED indicator above it.
// Total size: 17x28 (LED 9x9 at top, button 17x19 below).
// LED image is @2x, 18x36, 2 frames stacked vertically (frame 0=off, frame 1=on).
// LED is on when param value > 0.5 AND power param value > 0.5.
// ============================================================================
class KR106ButtonLED : public juce::Component
{
public:
    KR106ButtonLED(juce::RangedAudioParameter* param, int buttonType,
                   const juce::Image& ledImage, juce::RangedAudioParameter* powerParam)
        : mParam(param)
        , mButtonType(buttonType)
        , mLedImage(ledImage)
        , mPowerParam(powerParam)
    {
    }

    void paint(juce::Graphics& g) override
    {
        // LED at (4, 0), 9x9
        // ledImage is 18x36 @2x, 2 frames stacked vertically
        bool powered = mPowerParam && mPowerParam->getValue() > 0.5f;
        bool on = powered && mParam && mParam->getValue() > 0.5f;

        int frameW2x = mLedImage.getWidth();   // 18
        int frameH2x = mLedImage.getHeight() / 2; // 18 per frame
        int frameY2x = on ? frameH2x : 0;     // frame 1 = on, frame 0 = off

        float ledW = frameW2x / 2.f; // 9
        float ledH = frameH2x / 2.f; // 9

        g.drawImage(mLedImage,
                    4.f, 0.f, ledW, ledH,          // dest: 1x coords
                    0, frameY2x, frameW2x, frameH2x); // src: 2x coords

        // Button at (0, 9), 17x19
        DrawKR106Button(g, 0.f, 9.f, 17.f, 28.f, mButtonType, mPressed);
    }

    void mouseDown(const juce::MouseEvent& /*e*/) override
    {
        if (!mParam) return;
        mPressed = true;
        float newVal = mParam->getValue() > 0.5f ? 0.f : 1.f;
        mParam->beginChangeGesture();
        mParam->setValueNotifyingHost(newVal);
        mParam->endChangeGesture();
        repaint();
    }

    void mouseUp(const juce::MouseEvent& /*e*/) override
    {
        mPressed = false;
        repaint();
    }

private:
    juce::RangedAudioParameter* mParam = nullptr;
    juce::RangedAudioParameter* mPowerParam = nullptr;
    int mButtonType = 0;
    juce::Image mLedImage;
    bool mPressed = false;
};

// ============================================================================
// KR106ClipLED — Standalone LED that lights when a level exceeds a threshold.
// Driven from timerCallback by reading an atomic peak value.
// Size: 9x9 @1x (uses same 18x36 @2x two-frame strip as ButtonLED).
// ============================================================================
class KR106ClipLED : public juce::Component
{
public:
    KR106ClipLED(const juce::Image& ledImage, float threshold)
        : mLedImage(ledImage), mThreshold(threshold) {}

    void paint(juce::Graphics& g) override
    {
        int frameW2x = mLedImage.getWidth();
        int frameH2x = mLedImage.getHeight() / 2;
        int frameY2x = mLit ? frameH2x : 0;
        g.drawImage(mLedImage,
                    0.f, 0.f, frameW2x / 2.f, frameH2x / 2.f,
                    0, frameY2x, frameW2x, frameH2x);
    }

    // Call from timerCallback with the current peak value
    void update(float peak)
    {
        bool lit = peak >= mThreshold;
        if (lit != mLit) { mLit = lit; repaint(); }
    }

private:
    juce::Image mLedImage;
    float mThreshold;
    bool mLit = false;
};

// ============================================================================
// KR106ChorusOff — Cream button that turns off both Chorus I and Chorus II.
// Size: 17x19.
// ============================================================================
class KR106ChorusOff : public juce::Component
{
public:
    KR106ChorusOff(juce::RangedAudioParameter* chorusI, juce::RangedAudioParameter* chorusII)
        : mChorusI(chorusI)
        , mChorusII(chorusII)
    {
    }

    void paint(juce::Graphics& g) override
    {
        DrawKR106Button(g, 0.f, 0.f, 17.f, 19.f, 0 /*Cream*/, mPressed);
    }

    void mouseDown(const juce::MouseEvent& /*e*/) override
    {
        mPressed = true;

        if (mChorusI)
        {
            mChorusI->beginChangeGesture();
            mChorusI->setValueNotifyingHost(0.f);
            mChorusI->endChangeGesture();
        }

        if (mChorusII)
        {
            mChorusII->beginChangeGesture();
            mChorusII->setValueNotifyingHost(0.f);
            mChorusII->endChangeGesture();
        }

        repaint();
    }

    void mouseUp(const juce::MouseEvent& /*e*/) override
    {
        mPressed = false;
        repaint();
    }

private:
    juce::RangedAudioParameter* mChorusI = nullptr;
    juce::RangedAudioParameter* mChorusII = nullptr;
    bool mPressed = false;
};
