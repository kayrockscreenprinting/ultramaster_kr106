#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

// Lightweight tooltip overlay — shows formatted parameter value during drag.
// Owned by KR106Editor, passed to sliders/knobs as a raw pointer.
class KR106Tooltip : public juce::Component
{
public:
    KR106Tooltip() { setInterceptsMouseClicks(false, false); }

    void show(juce::RangedAudioParameter* param, juce::Component* source)
    {
        if (!param || !source || !getParentComponent()) return;
        mParam = param;

        updateText();

        // Position below the source control, centered
        auto srcBounds = getParentComponent()->getLocalArea(source->getParentComponent(),
                                                             source->getBounds());
        int tw = textWidth(mText) + 10;
        int th = 16;
        int tx = srcBounds.getCentreX() - tw / 2;
        int ty = srcBounds.getBottom() + 2;

        // Clamp to parent bounds
        auto pb = getParentComponent()->getLocalBounds();
        tx = juce::jlimit(0, pb.getWidth() - tw, tx);
        ty = juce::jmin(pb.getHeight() - th, ty);

        setBounds(tx, ty, tw, th);
        setVisible(true);
        repaint();
    }

    void update()
    {
        if (!mParam || !isVisible()) return;
        updateText();
        // Resize width to fit text
        int tw = textWidth(mText) + 10;
        setBounds(getX() + (getWidth() - tw) / 2, getY(), tw, getHeight());
        repaint();
    }

    void hide()
    {
        setVisible(false);
        mParam = nullptr;
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(juce::Colour(0, 0, 0));
        g.fillRect(getLocalBounds());
        g.setColour(juce::Colour(128, 128, 128));
        g.drawRect(getLocalBounds());
        g.setColour(juce::Colour(255, 255, 255));
        g.setFont(mFont);
        g.drawText(mText, getLocalBounds(), juce::Justification::centred);
    }

private:
    void updateText()
    {
        if (mParam)
            mText = mParam->getCurrentValueAsText();
    }

    int textWidth(const juce::String& s) const
    {
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText(mFont, s, 0.f, 0.f);
        return juce::roundToInt(glyphs.getBoundingBox(0, glyphs.getNumGlyphs(), false).getWidth());
    }

    juce::RangedAudioParameter* mParam = nullptr;
    juce::String mText;
    juce::Font mFont{juce::FontOptions(11.f)};
};
