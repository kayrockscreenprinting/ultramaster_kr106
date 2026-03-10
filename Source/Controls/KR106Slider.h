#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "KR106Tooltip.h"

// Pixel-perfect vertical fader — port of KR106SliderControl from iPlug2.
// 13×49 draw area: white/gray tick marks, black well, gray thumb.
class KR106Slider : public juce::Component
{
public:
  KR106Slider(juce::RangedAudioParameter* param, KR106Tooltip* tip = nullptr,
              const juce::Image& handleImg = {})
    : mParam(param), mTooltip(tip), mHandleImg(handleImg) {}

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

    // Well
    g.setColour(black); g.fillRect(5.f, 1.f, 3.f, 47.f);
    vLine(dark, 4, 1, 48); vLine(dark, 8, 1, 48);
    hLine(dark, 5, 0, 8);  hLine(dark, 5, 48, 8);

    // Handle
    float val = mParam ? mParam->getValue() : 0.f;
    float fy = std::round(44.f - val * 40.f);

    if (mHandleImg.isValid())
    {
      float hw = mHandleImg.getWidth() / 2.f;
      float hh = mHandleImg.getHeight() / 2.f;
      float hx = (13.f - hw) * 0.5f;
      g.drawImage(mHandleImg,
                  hx, fy - hh * 0.5f + 1.f, hw, hh,
                  0, 0, mHandleImg.getWidth(), mHandleImg.getHeight());
    }
    else
    {
      pixelRect(black, 1.f, fy - 2, 12.f, fy + 3);
      hLine(dark, 2, fy - 1, 11);
      hLine(dark, 2, fy + 1, 11);
      hLine(white, 2, fy, 11);
    }
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
    mLastRawDY = 0.f;
    if (mParam) mParam->beginChangeGesture();
    if (mTooltip) mTooltip->show(mParam, this);
    setMouseCursor(juce::MouseCursor::NoCursor);
    e.source.enableUnboundedMouseMovement(true);
  }

  void mouseDrag(const juce::MouseEvent& e) override
  {
    if (!mParam) return;
    float dy = static_cast<float>(e.getOffsetFromDragStart().y);
    float increment = dy - mLastRawDY;
    mLastRawDY = dy;

    // Cumulative gearing: shift only affects new movement
    // Scale by display DPI so non-retina screens get the same physical throw
    float dpiScale = std::max(1.f, static_cast<float>(getTopLevelComponent()->getDesktopScaleFactor()));
    float gearing = (e.mods.isShiftDown() ? 1270.f : 127.f) / dpiScale;
    mAccumVal = juce::jlimit(0.f, 1.f, mAccumVal + -increment / gearing);
    float newVal = mAccumVal;
    mParam->setValueNotifyingHost(newVal);
    if (mTooltip) mTooltip->update();
    repaint();
  }

  void mouseUp(const juce::MouseEvent& e) override
  {
    mDragging = false;
    if (mParam) mParam->endChangeGesture();
    if (mTooltip) mTooltip->hide();
    setMouseCursor(juce::MouseCursor::NormalCursor);
    e.source.enableUnboundedMouseMovement(false);

    // Warp cursor to the handle so it appears on the control after release
    float val = mParam ? mParam->getValue() : 0.f;
    float fy = std::round(44.f - val * 40.f);
    auto screenPos = localPointToGlobal(juce::Point<int>(6, static_cast<int>(fy)));
    juce::Desktop::getInstance().getMainMouseSource().setScreenPosition(screenPos.toFloat());
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
  juce::Image mHandleImg;
  float mAccumVal = 0.f;
  float mLastRawDY = 0.f;
  bool mDragging = false;
};
