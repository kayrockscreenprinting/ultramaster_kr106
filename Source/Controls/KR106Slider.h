#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "KR106Tooltip.h"
#include "PluginProcessor.h"

// Pixel-perfect vertical fader — port of KR106SliderControl from iPlug2.
// 13×49 draw area: white/gray tick marks, black well, gray thumb.
class KR106Slider : public juce::Component
{
public:
  KR106Slider(juce::RangedAudioParameter* param, KR106Tooltip* tip = nullptr,
              const juce::Image& handleImg = {})
    : mParam(param), mTooltip(tip), mHandleImg(handleImg) {}

  void setMidiLearn(KR106AudioProcessor* proc, int paramIdx)
  { mProcessor = proc; mParamIdx = paramIdx; }

  // Extra right-side pixel for tick marks (fills gap to adjacent slider)
  void setExtraRight(int px) { mExtraRight = px; }

  // Override to customize tick marks. Default: 11 evenly spaced marks,
  // lines 0/5/10 bright, others dim.
  virtual void paintTickMarks(juce::Graphics& g)
  {
    auto bright = juce::Colour(219, 219, 219);
    auto dim    = juce::Colour(126, 126, 126);
    float tw = 17.f + static_cast<float>(mExtraRight);
    for (int i = 0; i <= 10; i++)
    {
      float y = std::round(44.f - i * 4.f);
      g.setColour((i == 0 || i == 5 || i == 10) ? bright : dim);
      g.fillRect(1.f, y, tw, 1.f);
    }
  }

  void paint(juce::Graphics& g) override
  {
    paintTickMarks(g);

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
    const auto dark  = juce::Colour(34, 34, 34);

    // Well (centered in 19px: offset 3px from 13px layout)
    static constexpr float kOfs = 3.f;
 
    g.setColour(dark);
    g.fillRect(4.f + kOfs, 1.f, 5.f, 47.f);
    hLine(dark, 5 + kOfs, 0, 8 + kOfs);  
    hLine(dark, 5 + kOfs, 48, 8 + kOfs);
    g.setColour(black);
    g.fillRect(6.f + kOfs, 2.f, 1.f, 45.f);

    // Handle
    float val = mParam ? mParam->getValue() : 0.f;
    float fy = std::round(44.f - val * 40.f);

    if (mHandleImg.isValid())
    {
      float hw = mHandleImg.getWidth() / 2.f;
      float hh = mHandleImg.getHeight() / 2.f;
      float hx = std::floor((19.f - hw) * 0.5f);
      g.drawImage(mHandleImg,
                  hx, fy - hh * 0.5f + 1.f, hw, hh,
                  0, 0, mHandleImg.getWidth(), mHandleImg.getHeight());
    }
    else
    {
      pixelRect(black, 1.f + kOfs, fy - 2, 12.f + kOfs, fy + 3);
      hLine(dark, 2 + kOfs, fy - 1, 11 + kOfs);
      hLine(dark, 2 + kOfs, fy + 1, 11 + kOfs);
      hLine(white, 2 + kOfs, fy, 11 + kOfs);
    }

    paintMidiLearnBorder(g);
  }

  void paintMidiLearnBorder(juce::Graphics& g)
  {
    if (mProcessor && mParamIdx >= 0
        && mProcessor->mMidiLearnParam.load(std::memory_order_relaxed) == mParamIdx)
    {
      g.setColour(juce::Colour(0, 255, 0));
      g.drawRect(0, 0, getWidth() - mExtraRight, getHeight(), 1);
    }
  }

  void mouseEnter(const juce::MouseEvent&) override
  {
    if (mTooltip && !mDragging)
    {
      updateCCLine();
      mTooltip->show(mParam, this);
    }
  }

  void mouseExit(const juce::MouseEvent&) override
  {
    if (mTooltip && !mDragging) mTooltip->hide();
  }

  void mouseDown(const juce::MouseEvent& e) override
  {
    if (e.mods.isPopupMenu() && mProcessor && mParamIdx >= 0)
    {
      handleMidiLearnClick();
      return;
    }
    // Left click cancels MIDI learn if active
    if (mProcessor && mProcessor->mMidiLearnParam.load(std::memory_order_relaxed) >= 0)
    {
      mProcessor->cancelMidiLearn();
      if (mTooltip) mTooltip->hide();
      repaint();
      return;
    }
    mDragging = true;
    mRightDrag = false;
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
    float gearing = 127.f;
    if (e.mods.isCommandDown()) gearing *= 100.f;       // Cmd/Ctrl = superfine
    else if (e.mods.isShiftDown()) gearing *= 10.f;     // Shift = fine
    gearing /= dpiScale;
    mAccumVal = juce::jlimit(0.f, 1.f, mAccumVal + -increment / gearing);
    float newVal = mAccumVal;
    mParam->setValueNotifyingHost(newVal);
    if (mTooltip) mTooltip->update();
    repaint();
  }

  void mouseUp(const juce::MouseEvent& e) override
  {
    if (!mDragging) return; // right-click MIDI learn, no drag to clean up
    mDragging = false;
    if (mParam) mParam->endChangeGesture();
    if (mTooltip) mTooltip->hide();
    setMouseCursor(juce::MouseCursor::NormalCursor);
    e.source.enableUnboundedMouseMovement(false);

    // Warp cursor to the handle so it appears on the control after release
    float val = mParam ? mParam->getValue() : 0.f;
    float fy = std::round(44.f - val * 40.f);
    auto screenPos = localPointToGlobal(juce::Point<int>(9, static_cast<int>(fy)));
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

    // Pre-fill with formatted value (includes units + bracket)
    juce::String displayText = mParam->getCurrentValueAsText();
    mEditor->setText(displayText, false);
    mEditOrigMidi = juce::roundToInt(mParam->getValue() * 127.f);

    // Position below slider, sized to fit text (like tooltip)
    auto srcBounds = parent->getLocalArea(getParentComponent(), getBounds());
    juce::Font font{juce::FontOptions(11.f)};
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText(font, displayText, 0.f, 0.f);
    int tw = std::max(50, juce::roundToInt(glyphs.getBoundingBox(0, -1, false).getWidth()) + 16);
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

    // Check if user edited the bracket value or the human-readable part.
    // If bracket [N] changed from original, use it; otherwise parse the text.
    float normalized;
    int bStart = raw.indexOfChar('[');
    int bEnd   = raw.indexOfChar(']');
    if (bStart >= 0 && bEnd > bStart)
    {
      int midi = raw.substring(bStart + 1, bEnd).getIntValue();
      if (midi != mEditOrigMidi)
        normalized = juce::jlimit(0.f, 1.f, midi / 127.f);
      else
        normalized = juce::jlimit(0.f, 1.f, mParam->getValueForText(raw));
    }
    else
    {
      normalized = juce::jlimit(0.f, 1.f, mParam->getValueForText(raw));
    }
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

  int getNumSteps() const
  {
    if (dynamic_cast<juce::AudioParameterBool*>(mParam)) return 1;
    if (auto* pi = dynamic_cast<juce::AudioParameterInt*>(mParam))
      return pi->getRange().getLength();
    return 0; // continuous
  }

  void updateCCLine()
  {
    if (!mTooltip) return;
    if (!mProcessor || mParamIdx < 0) { mTooltip->setLine2({}); return; }
    if (mProcessor->mMidiLearnParam.load(std::memory_order_relaxed) == mParamIdx)
    {
      mTooltip->setLine2("MIDI LEARN");
      return;
    }
    int cc = mProcessor->getCCForParam(mParamIdx);
    if (cc >= 0)
      mTooltip->setLine2("CC " + juce::String(cc));
    else
      mTooltip->setLine2({});
  }

  void handleMidiLearnClick()
  {
    if (!mProcessor || mParamIdx < 0) return;
    mProcessor->startMidiLearn(mParamIdx);
    updateCCLine();
    if (mTooltip) mTooltip->show(mParam, this);
    repaint();
  }

protected:
  juce::RangedAudioParameter* mParam = nullptr;
  KR106AudioProcessor* mProcessor = nullptr;
  int mParamIdx = -1;
  int mExtraRight = 0;

private:
  KR106Tooltip* mTooltip = nullptr;
  juce::Image mHandleImg;
  std::unique_ptr<juce::TextEditor> mEditor;
  int mEditOrigMidi = 0;
  float mAccumVal = 0.f;
  float mLastRawDY = 0.f;
  bool mDragging = false;
  bool mRightDrag = false;
};

// HPF slider: draws tick marks, snaps to 4 positions in J60/J106 mode.
// In J6 mode (mSynthModel == kJ6), behaves as a continuous slider.
class KR106HPFSlider : public KR106Slider
{
public:
  KR106HPFSlider(juce::RangedAudioParameter* param, KR106Tooltip* tip,
                 const juce::Image& handleImg, kr106::Model* synthModel)
    : KR106Slider(param, tip, handleImg), mSynthModel(synthModel) {}

  void paintTickMarks(juce::Graphics& g) override
  {
    auto bright = juce::Colour(219, 219, 219);
    auto dim    = juce::Colour(126, 126, 126);
    if (mSynthModel && *mSynthModel == kr106::kJ6)
    {
      // J6: 11 tick marks (continuous slider)
      KR106Slider::paintTickMarks(g);
    }
    else
    {
      // J60/J106: 4 tick marks at switch positions, bright
      float tw = 17.f + static_cast<float>(mExtraRight);
      for (int i = 0; i < 4; i++)
      {
        float y = std::round(44.f - i * (40.f / 3.f));
        g.setColour(bright);
        g.fillRect(1.f, y, tw, 1.f);
      }
    }
  }

  void mouseDrag(const juce::MouseEvent& e) override
  {
    KR106Slider::mouseDrag(e);
    // In J60/J106 mode, snap to 4 positions (0, 1/3, 2/3, 1)
    if (mSynthModel && *mSynthModel != kr106::kJ6 && mParam)
    {
      float val = mParam->getValue();
      float snapped = std::round(val * 3.f) / 3.f;
      if (snapped != val)
      {
        mParam->setValueNotifyingHost(snapped);
        repaint();
      }
    }
  }

  void mouseUp(const juce::MouseEvent& e) override
  {
    // Snap on release too for clean final position
    if (mSynthModel && *mSynthModel != kr106::kJ6 && mParam)
    {
      float val = mParam->getValue();
      float snapped = std::round(val * 3.f) / 3.f;
      if (snapped != val)
        mParam->setValueNotifyingHost(snapped);
    }
    KR106Slider::mouseUp(e);
  }

private:
  kr106::Model* mSynthModel = nullptr;
};

// Arp Rate slider: when DAW sync is enabled, draws tick marks for note
// divisions and snaps to discrete positions. When sync is off, behaves
// as a normal continuous slider (no tick marks).
class KR106ArpRateSlider : public KR106Slider
{
public:
  KR106ArpRateSlider(juce::RangedAudioParameter* rateParam,
                     juce::RangedAudioParameter* quantizeParam,
                     int rateParamIdx, int quantizeParamIdx,
                     KR106Tooltip* tip,
                     const juce::Image& handleImg, bool* syncFlag)
    : KR106Slider(rateParam, tip, handleImg),
      mRateParam(rateParam), mQuantizeParam(quantizeParam),
      mRateParamIdx(rateParamIdx), mQuantizeParamIdx(quantizeParamIdx),
      mSyncFlag(syncFlag) {}

  // Call from OnIdle to swap the active param when sync state changes
  void updateSyncState()
  {
    bool synced = mSyncFlag && *mSyncFlag;
    auto* wanted = synced ? mQuantizeParam : mRateParam;
    if (wanted != mParam)
    {
      mParam = wanted;
      mParamIdx = synced ? mQuantizeParamIdx : mRateParamIdx;
      repaint();
    }
  }

  void paintTickMarks(juce::Graphics& g) override
  {
    if (mSyncFlag && *mSyncFlag)
    {
      auto bright = juce::Colour(219, 219, 219);
      auto dim    = juce::Colour(126, 126, 126);
      // 9 tick marks at division positions (evenly spaced across 40px travel)
      float tw = 17.f + static_cast<float>(mExtraRight);
      for (int i = 0; i < kr106::kNumArpDivisions; i++)
      {
        float norm = static_cast<float>(i) / static_cast<float>(kr106::kNumArpDivisions - 1);
        float y = std::round(44.f - norm * 40.f);
        bool major = (i == 0 || i == kr106::kNumArpDivisions / 2 || i == kr106::kNumArpDivisions - 1);
        g.setColour(major ? bright : dim);
        g.fillRect(1.f, y, tw, 1.f);
      }
    }
    else
    {
      KR106Slider::paintTickMarks(g);
    }
  }

  void mouseDrag(const juce::MouseEvent& e) override
  {
    KR106Slider::mouseDrag(e);
    if (mSyncFlag && *mSyncFlag && mParam)
    {
      float val = mParam->getValue();
      float steps = static_cast<float>(kr106::kNumArpDivisions - 1);
      float snapped = std::round(val * steps) / steps;
      if (snapped != val)
      {
        mParam->setValueNotifyingHost(snapped);
        repaint();
      }
    }
  }

  void mouseUp(const juce::MouseEvent& e) override
  {
    if (mSyncFlag && *mSyncFlag && mParam)
    {
      float val = mParam->getValue();
      float steps = static_cast<float>(kr106::kNumArpDivisions - 1);
      float snapped = std::round(val * steps) / steps;
      if (snapped != val)
        mParam->setValueNotifyingHost(snapped);
    }
    KR106Slider::mouseUp(e);
  }

private:
  juce::RangedAudioParameter* mRateParam = nullptr;
  juce::RangedAudioParameter* mQuantizeParam = nullptr;
  int mRateParamIdx = -1;
  int mQuantizeParamIdx = -1;
  bool* mSyncFlag = nullptr;
};

// LFO Rate slider: when DAW sync is enabled, draws tick marks for note
// divisions and snaps to discrete positions. Same pattern as ArpRateSlider
// but with 12 LFO divisions (includes Maxima/Longa/Breve).
class KR106LfoRateSlider : public KR106Slider
{
public:
  KR106LfoRateSlider(juce::RangedAudioParameter* rateParam,
                     juce::RangedAudioParameter* quantizeParam,
                     int rateParamIdx, int quantizeParamIdx,
                     KR106Tooltip* tip,
                     const juce::Image& handleImg, bool* syncFlag)
    : KR106Slider(rateParam, tip, handleImg),
      mRateParam(rateParam), mQuantizeParam(quantizeParam),
      mRateParamIdx(rateParamIdx), mQuantizeParamIdx(quantizeParamIdx),
      mSyncFlag(syncFlag) {}

  void updateSyncState()
  {
    bool synced = mSyncFlag && *mSyncFlag;
    auto* wanted = synced ? mQuantizeParam : mRateParam;
    if (wanted != mParam)
    {
      mParam = wanted;
      mParamIdx = synced ? mQuantizeParamIdx : mRateParamIdx;
      repaint();
    }
  }

  void paintTickMarks(juce::Graphics& g) override
  {
    if (mSyncFlag && *mSyncFlag)
    {
      auto bright = juce::Colour(219, 219, 219);
      auto dim    = juce::Colour(126, 126, 126);
      float rw = 17.f + static_cast<float>(mExtraRight);

      // Right side: standard 11 ticks (same as base slider)
      for (int i = 0; i <= 10; i++)
      {
        float y = std::round(44.f - i * 4.f);
        g.setColour((i == 0 || i == 5 || i == 10) ? bright : dim);
        g.fillRect(10.f, y, rw - 9.f, 1.f);
      }

      // Left side: 7 division ticks (every other from 13 divisions)
      int last = kr106::kNumLfoDivisions - 1;
      int mid = last / 2;
      for (int i = 0; i <= last; i += 2)
      {
        float norm = static_cast<float>(i) / static_cast<float>(last);
        float y = std::round(44.f - norm * 40.f);
        bool major = (i == 0 || i == mid || i == last);
        g.setColour(major ? bright : dim);
        g.fillRect(1.f, y, 6.f, 1.f);
      }
    }
    else
    {
      KR106Slider::paintTickMarks(g);
    }
  }

  void mouseDrag(const juce::MouseEvent& e) override
  {
    KR106Slider::mouseDrag(e);
    if (mSyncFlag && *mSyncFlag && mParam)
    {
      float val = mParam->getValue();
      float steps = static_cast<float>(kr106::kNumLfoDivisions - 1);
      float snapped = std::round(val * steps) / steps;
      if (snapped != val)
      {
        mParam->setValueNotifyingHost(snapped);
        repaint();
      }
    }
  }

  void mouseUp(const juce::MouseEvent& e) override
  {
    if (mSyncFlag && *mSyncFlag && mParam)
    {
      float val = mParam->getValue();
      float steps = static_cast<float>(kr106::kNumLfoDivisions - 1);
      float snapped = std::round(val * steps) / steps;
      if (snapped != val)
        mParam->setValueNotifyingHost(snapped);
    }
    KR106Slider::mouseUp(e);
  }

private:
  juce::RangedAudioParameter* mRateParam = nullptr;
  juce::RangedAudioParameter* mQuantizeParam = nullptr;
  int mRateParamIdx = -1;
  int mQuantizeParamIdx = -1;
  bool* mSyncFlag = nullptr;
};
