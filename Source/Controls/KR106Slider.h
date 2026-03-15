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

  void mouseDoubleClick(const juce::MouseEvent&) override
  {
    if (!mParam) return;
    if (dynamic_cast<juce::AudioParameterBool*>(mParam)) return;
    if (dynamic_cast<juce::AudioParameterInt*>(mParam)) return;
    if (mEditor) dismissEdit();

    if (mTooltip) mTooltip->hide();

    // Find a parent large enough to host the editor overlay
    auto* parent = mTooltip ? mTooltip->getParentComponent() : getTopLevelComponent();
    if (!parent) return;

    mEditor = std::make_unique<juce::TextEditor>();
    mEditor->setLookAndFeel(&getEditBoxLnF());
    mEditor->setFont(juce::FontOptions(11.f));
    mEditor->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0, 0, 0));
    mEditor->setColour(juce::TextEditor::textColourId, juce::Colour(255, 255, 255));
    mEditor->setColour(juce::TextEditor::outlineColourId, juce::Colour(128, 128, 128));
    mEditor->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(128, 128, 128));
    mEditor->setColour(juce::TextEditor::highlightColourId, juce::Colour(80, 80, 80));
    mEditor->setJustification(juce::Justification::centred);

    // Pre-fill with formatted value (includes units)
    juce::String displayText = mParam->getCurrentValueAsText();
    mEditor->setText(displayText, false);

    // Position below slider, sized to fit text (like tooltip)
    auto srcBounds = parent->getLocalArea(getParentComponent(), getBounds());
    juce::Font font{juce::FontOptions(11.f)};
    int tw = std::max(50, juce::roundToInt(font.getStringWidthFloat(displayText)) + 16);
    int th = 16;
    int tx = srcBounds.getCentreX() - tw / 2;
    int ty = srcBounds.getBottom() + 2;
    auto pb = parent->getLocalBounds();
    tx = juce::jlimit(0, pb.getWidth() - tw, tx);
    ty = juce::jmin(pb.getHeight() - th, ty);
    mEditor->setBounds(tx, ty, tw, th);

    parent->addAndMakeVisible(mEditor.get());
    mEditor->grabKeyboardFocus();

    // Select only the numeric portion (not the unit suffix)
    int numEnd = findNumericEnd(displayText);
    mEditor->setHighlightedRegion(juce::Range<int>(0, numEnd));

    mEditor->onReturnKey = [this]() { commitEdit(); };
    mEditor->onEscapeKey = [this]() { dismissEdit(); };
    mEditor->onFocusLost = [this]() { dismissEdit(); };
  }

private:
  void commitEdit()
  {
    if (!mEditor || !mParam) return;
    juce::String raw = mEditor->getText().trim();
    if (raw.isEmpty()) { dismissEdit(); return; }

    float typedNum = extractLeadingNumber(raw);
    juce::String typedUnit = extractUnit(raw);

    // Normalize to base units: kHz→Hz, s→ms, s/oct→ms/oct
    if (typedUnit == "khz") typedNum *= 1000.f;
    else if (typedUnit == "s/oct") typedNum *= 1000.f;
    else if (typedUnit == "s") typedNum *= 1000.f;

    // Binary search the 0–1 normalized range for the matching display value
    float lo = 0.f, hi = 1.f;
    bool inverted = getDisplayValueInBaseUnits(0.f) > getDisplayValueInBaseUnits(1.f);
    for (int iter = 0; iter < 50; iter++)
    {
      float mid = (lo + hi) * 0.5f;
      float midVal = getDisplayValueInBaseUnits(mid);
      bool goHigh = inverted ? (midVal > typedNum) : (midVal < typedNum);
      if (goHigh) lo = mid; else hi = mid;
    }

    float normalized = juce::jlimit(0.f, 1.f, (lo + hi) * 0.5f);
    mParam->beginChangeGesture();
    mParam->setValueNotifyingHost(normalized);
    mParam->endChangeGesture();
    repaint();
    dismissEdit();
  }

  void dismissEdit()
  {
    if (!mEditor) return;
    auto editor = std::move(mEditor);
    editor->setLookAndFeel(nullptr);
    editor->onFocusLost = nullptr;
    if (auto* parent = editor->getParentComponent())
      parent->removeChildComponent(editor.get());
  }

  // --- Edit-box LookAndFeel: 1px sharp border matching tooltip ---
  struct EditBoxLnF : juce::LookAndFeel_V4
  {
    void drawTextEditorOutline(juce::Graphics& g, int w, int h, juce::TextEditor&) override
    {
      g.setColour(juce::Colour(128, 128, 128));
      g.drawRect(0, 0, w, h, 1);
    }
  };
  static EditBoxLnF& getEditBoxLnF() { static EditBoxLnF lnf; return lnf; }

  // Find where the numeric portion ends (for text selection)
  static int findNumericEnd(const juce::String& text)
  {
    int i = 0, len = text.length();
    if (i < len && (text[i] == '+' || text[i] == '-')) i++;
    bool hasDigit = false;
    while (i < len && (juce::CharacterFunctions::isDigit(text[i]) || text[i] == '.'))
    { if (text[i] != '.') hasDigit = true; i++; }
    return hasDigit ? i : len;
  }

  // Extract leading number from formatted text (e.g. "+5.0 Hz" → 5.0)
  static float extractLeadingNumber(const juce::String& text)
  {
    int i = 0, len = text.length();
    while (i < len && text[i] == ' ') i++;
    int start = i;
    if (i < len && (text[i] == '+' || text[i] == '-')) i++;
    bool hasDigit = false;
    while (i < len && (juce::CharacterFunctions::isDigit(text[i]) || text[i] == '.'))
    { if (text[i] != '.') hasDigit = true; i++; }
    return hasDigit ? text.substring(start, i).getFloatValue() : 0.f;
  }

  // Extract unit suffix from text (e.g. "5.0 Hz" → "hz"), case-insensitive
  static juce::String extractUnit(const juce::String& text)
  {
    int i = 0, len = text.length();
    while (i < len && text[i] == ' ') i++;
    if (i < len && (text[i] == '+' || text[i] == '-')) i++;
    while (i < len && (juce::CharacterFunctions::isDigit(text[i]) || text[i] == '.')) i++;
    return text.substring(i).trim().toLowerCase();
  }

  // Get display value in base units (Hz not kHz, ms not s) at a normalized position
  float getDisplayValueInBaseUnits(float normalized) const
  {
    juce::String text = mParam->getText(normalized, 100);
    if (text.containsIgnoreCase("inf")) return -1000.f;
    float num = extractLeadingNumber(text);
    juce::String unit = extractUnit(text);
    if (unit == "khz") return num * 1000.f;
    if (unit == "s/oct") return num * 1000.f;
    if (unit == "s") return num * 1000.f;
    return num;
  }

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
  std::unique_ptr<juce::TextEditor> mEditor;
  float mAccumVal = 0.f;
  float mLastRawDY = 0.f;
  bool mDragging = false;
};
