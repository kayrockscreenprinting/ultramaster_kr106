#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "KR106Tooltip.h"

// Pixel-perfect vertical fader — port of KR106SliderControl from iPlug2.
// 13×49 draw area: white/gray tick marks, black well, gray thumb.
class KR106Slider : public juce::Component
{
public:
  KR106Slider(juce::RangedAudioParameter* param, KR106Tooltip* tip = nullptr)
    : mParam(param), mTooltip(tip) {}

  void paint(juce::Graphics& g) override
  {
    auto hLine = [&](juce::Colour c, float x1, float y, float x2) {
      g.setColour(c); g.fillRect(x1, y, x2 - x1, 1.f);
    };
    auto vLine = [&](juce::Colour c, float x, float y1, float y2) {
      g.setColour(c); g.fillRect(x, y1, 1.f, y2 - y1);
    };
    auto pixelRect = [&](juce::Colour c, float l, float t, float r, float b) {
      hLine(c, l, t, r); hLine(c, l, b - 1.f, r);
      vLine(c, l, t, b); vLine(c, r - 1.f, t, b);
    };

    const auto white = juce::Colour(255, 255, 255);
    const auto black = juce::Colour(0, 0, 0);
    const auto dark  = juce::Colour(64, 64, 64);
    const auto light = juce::Colour(153, 153, 153);

    int nSteps = getNumSteps();

    if (nSteps > 0 && nSteps <= 10)
    {
      for (int i = 0; i <= nSteps; i++)
      {
        float t = static_cast<float>(i) / nSteps;
        float ty = std::round(44.f - t * 40.f);
        hLine(white, 0, ty, 13);
      }
    }
    else
    {
      hLine(white, 0, 4, 13);
      hLine(white, 0, 24, 13);
      hLine(white, 0, 44, 13);
      for (int i = 8; i <= 20; i += 4) hLine(light, 0, (float)i, 13);
      for (int i = 28; i <= 40; i += 4) hLine(light, 0, (float)i, 13);
    }

    // Well
    g.setColour(black); g.fillRect(5.f, 1.f, 3.f, 47.f);
    vLine(dark, 4, 1, 48); vLine(dark, 8, 1, 48);
    hLine(dark, 5, 0, 8);  hLine(dark, 5, 48, 8);

    // Handle
    float val = mParam ? mParam->getValue() : 0.f;
    float fy = std::round(44.f - val * 40.f);

    pixelRect(black, 1.f, fy - 2, 12.f, fy + 3);
    hLine(dark, 2, fy - 1, 11);
    hLine(dark, 2, fy + 1, 11);
    hLine(white, 2, fy, 11);
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
    float dy = static_cast<float>(e.getOffsetFromDragStart().y);
    float newVal = juce::jlimit(0.f, 1.f, mDragStartVal + (-dy / 49.f / gearing));
    mParam->setValueNotifyingHost(newVal);
    if (mTooltip) mTooltip->update();
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
  int getNumSteps() const
  {
    if (dynamic_cast<juce::AudioParameterBool*>(mParam)) return 1;
    if (auto* pi = dynamic_cast<juce::AudioParameterInt*>(mParam))
      return pi->getRange().getLength();
    return 0; // continuous
  }

  juce::RangedAudioParameter* mParam = nullptr;
  KR106Tooltip* mTooltip = nullptr;
  float mDragStartVal = 0.f;
};
