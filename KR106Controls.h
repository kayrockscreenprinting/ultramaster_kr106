#pragma once

#include "IControl.h"
#include "IControls.h"
#include "ISender.h"

using namespace iplug;
using namespace igraphics;

// Helper: draw a 1px horizontal line as a filled rect (pixel-perfect, no AA)
static inline void HLine(IGraphics& g, const IColor& c, float x1, float y, float x2)
{
  g.FillRect(c, IRECT(x1, y, x2, y + 1.f));
}

// Helper: draw a 1px vertical line as a filled rect (pixel-perfect, no AA)
static inline void VLine(IGraphics& g, const IColor& c, float x, float y1, float y2)
{
  g.FillRect(c, IRECT(x, y1, x + 1.f, y2));
}

// Helper: draw a 1px border rect as four filled rects (pixel-perfect, no AA)
static inline void PixelRect(IGraphics& g, const IColor& c, float l, float t, float r, float b)
{
  HLine(g, c, l, t, r);         // top
  HLine(g, c, l, b - 1.f, r);   // bottom
  VLine(g, c, l, t, b);         // left
  VLine(g, c, r - 1.f, t, b);   // right
}

// Knob control with manhattan distance drag — both vertical and horizontal mouse
// movement contribute to the value. Up and right increase; down and left decrease.
class KR106KnobControl : public IBKnobControl
{
public:
  KR106KnobControl(float x, float y, const IBitmap& bitmap, int paramIdx)
  : IBKnobControl(x, y, bitmap, paramIdx, EDirection::Vertical)
  {}

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    if (mod.R) return;
    IBKnobControl::OnMouseDown(x, y, mod);
  }

  void OnMouseDblClick(float x, float y, const IMouseMod& mod) override { /* no reset */ }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
    double gearing = IsFineControl(mod, false) ? mGearing * 10.0 : mGearing;
    IRECT dragBounds = GetKnobDragBounds();
    double scale = static_cast<double>(dragBounds.T - dragBounds.B);

    // Manhattan: dY and -dX both contribute (up+right = increase)
    mMouseDragValue += static_cast<double>((dY - dX) / scale / gearing);
    mMouseDragValue = Clip(mMouseDragValue, 0., 1.);

    double v = mMouseDragValue;
    const IParam* pParam = GetParam();
    if (pParam && pParam->GetStepped() && pParam->GetStep() > 0)
      v = pParam->ConstrainNormalized(mMouseDragValue);

    SetValue(v);
    SetDirty();
  }
};

// Custom vertical slider that replicates the original KR-106 procedural fader drawing
// from kr106_slider.c: 13x49 pixels, white/gray tick marks, black well, gray thumb
class KR106SliderControl : public ISliderControlBase
{
  static constexpr float kSliderH = 49.f;

public:
  KR106SliderControl(const IRECT& bounds, int paramIdx)
  : ISliderControlBase(bounds, paramIdx, EDirection::Vertical, 2.0, 5.f)
  {}

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    if (mod.R) return;
    mMouseDown = true;
    mMouseDragValue = GetValue();
    if (mHideCursorOnDrag)
      GetUI()->HideMouseCursor(true, true);
  }

  void OnMouseDblClick(float x, float y, const IMouseMod& mod) override { /* no reset */ }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    if (mHideCursorOnDrag)
      GetUI()->HideMouseCursor(false);
    mMouseDown = false;
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
    double gearing = IsFineControl(mod, false) ? mGearing * 10.0 : mGearing;
    mMouseDragValue += static_cast<double>(dY / -kSliderH / gearing);
    mMouseDragValue = Clip(mMouseDragValue, 0., 1.);
    double v = mMouseDragValue;
    const IParam* pParam = GetParam();
    if (pParam && pParam->GetStepped() && pParam->GetStep() > 0)
      v = pParam->ConstrainNormalized(v);
    SetValue(v);
    SetDirty();
  }

  void Draw(IGraphics& g) override
  {
    float x = std::round(mRECT.L);
    float y = std::round(mRECT.T);

    const IColor white(255, 255, 255, 255);
    const IColor black(255, 0, 0, 0);
    const IColor dark(255, 64, 64, 64);
    const IColor light(255, 153, 153, 153);

    const IParam* pParam = GetParam();
    int nSteps = 0;
    if (pParam && pParam->GetStepped() && pParam->GetStep() > 0)
      nSteps = static_cast<int>(std::round(pParam->GetRange() / pParam->GetStep()));

    if (nSteps > 0 && nSteps <= 10)
    {
      for (int i = 0; i <= nSteps; i++)
      {
        float t = (nSteps > 0) ? static_cast<float>(i) / nSteps : 0.f;
        float ty = std::round(y + 44 - t * 40.f);
        HLine(g, white, x, ty, x + 13);
      }
    }
    else
    {
      // Continuous parameter: primary + secondary tick marks
      HLine(g, white, x, y + 4, x + 13);
      HLine(g, white, x, y + 24, x + 13);
      HLine(g, white, x, y + 44, x + 13);

      for (int i = 8; i <= 20; i += 4)
        HLine(g, light, x, y + i, x + 13);
      for (int i = 28; i <= 40; i += 4)
        HLine(g, light, x, y + i, x + 13);
    }

    // Fader well
    g.FillRect(black, IRECT(x + 5, y + 1, x + 8, y + 48));
    VLine(g, dark, x + 4, y + 1, y + 48);
    VLine(g, dark, x + 8, y + 1, y + 48);
    HLine(g, dark, x + 5, y, x + 8);
    HLine(g, dark, x + 5, y + 48, x + 8);

    // Fader handle position (value 0=bottom, 1=top)
    float val = (float)GetValue();
    float fy = std::round(y + 44 - val * 40.f);

    // Handle outline
    PixelRect(g, black, x + 1, fy - 2, x + 12, fy + 3);
    // Handle interior lines
    HLine(g, dark, x + 2, fy - 1, x + 11);
    HLine(g, dark, x + 2, fy + 1, x + 11);
    HLine(g, white, x + 2, fy, x + 11);
  }
};

// ============================================================================
// KR106ValueReadout — shows "param: value unit" for whichever control has focus
// Polls each frame via IsDirty(); ignores mouse input
// ============================================================================
class KR106ValueReadout : public IControl
{
public:
  KR106ValueReadout(const IRECT& bounds)
  : IControl(bounds)
  {
    mIgnoreMouse = true;
  }

  bool IsDirty() override { return true; }

  void Draw(IGraphics& g) override
  {
    IGraphics* pGraphics = GetUI();
    int idx = pGraphics->GetMouseOver();
    if (idx < 0) return;

    IControl* pCtrl = pGraphics->GetControl(idx);
    if (!pCtrl || pCtrl->GetParamIdx() < 0) return;

    const IParam* pParam = GetDelegate()->GetParam(pCtrl->GetParamIdx());
    if (!pParam) return;

    WDL_String display;
    pParam->GetDisplayWithLabel(display);

    // Position centered under the hovered control
    IRECT ctrlRect = pCtrl->GetRECT();
    float cx = (ctrlRect.L + ctrlRect.R) * 0.5f;
    float ty = ctrlRect.B + 8.f;
    IRECT textRect(cx - 30.f, ty, cx + 30.f, ty + 12.f);

    IText text(10.f, IColor(200, 255, 255, 255));
    text.mAlign = EAlign::Center;
    text.mVAlign = EVAlign::Top;
    g.DrawText(text, display.Get(), textRect);
  }
};

// Button types matching original kr106_button.h
enum EKR106ButtonType { kCream = 0, kYellow, kOrange };

// Color triplets for each button type: [highlight, face, shadow]
static const IColor kButtonColors[3][3] = {
  { {255, 253, 247, 203}, {255, 220, 220, 178}, {255, 178, 179, 144} }, // Cream
  { {255, 253, 247, 203}, {255, 228, 229, 88},  {255, 180, 179, 69}  }, // Yellow
  { {255, 253, 247, 203}, {255, 240, 173, 56},  {255, 180, 126, 41}  }, // Orange
};

// Draw a 3D bevel button (pixel-perfect)
static inline void DrawKR106Button(IGraphics& g, float l, float t, float r, float b, EKR106ButtonType type, bool pressed)
{
  const IColor black(255, 0, 0, 0);
  const IColor& hi = kButtonColors[type][pressed ? 2 : 0];
  const IColor& lo = kButtonColors[type][pressed ? 0 : 2];
  const IColor& face = kButtonColors[type][1];

  // Face fill
  g.FillRect(face, IRECT(l + 2, t + 2, r - 2, b - 2));

  // 3D bevel
  HLine(g, hi, l + 1, t + 1, r - 1);    // top highlight
  VLine(g, hi, l + 1, t + 1, b - 1);     // left highlight
  HLine(g, lo, l + 2, b - 2, r - 1);     // bottom shadow
  VLine(g, lo, r - 2, t + 2, b - 1);     // right shadow

  // Black border
  PixelRect(g, black, l, t, r, b);
}

// Custom button control that replicates the original KR-106 3D bevel button
// from kr106_button.c: 17x19 pixels with cream/yellow/orange coloring
// Button bevel is momentary (pressed while mouse held), value toggles on click
class KR106ButtonControl : public IControl
{
public:
  KR106ButtonControl(const IRECT& bounds, int paramIdx, EKR106ButtonType type)
  : IControl(bounds, paramIdx)
  , mType(type)
  {
  }

  void Draw(IGraphics& g) override
  {
    float l = std::round(mRECT.L);
    float t = std::round(mRECT.T);
    DrawKR106Button(g, l, t, l + 17, t + 19, mType, mPressed);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mPressed = true;
    SetValue(GetValue() > 0.5 ? 0.0 : 1.0);
    SetDirty(true);
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    mPressed = false;
    SetDirty(false);
  }

private:
  EKR106ButtonType mType;
  bool mPressed = false;
};

// Chorus Off button — momentary press that turns off both Chorus I and II params
class KR106ChorusOffControl : public IControl
{
public:
  KR106ChorusOffControl(const IRECT& bounds, int chorusIParam, int chorusIIParam)
  : IControl(bounds, kNoParameter)
  , mChorusIParam(chorusIParam)
  , mChorusIIParam(chorusIIParam)
  {}

  void Draw(IGraphics& g) override
  {
    float l = std::round(mRECT.L);
    float t = std::round(mRECT.T);
    DrawKR106Button(g, l, t, l + 17, t + 19, kCream, mPressed);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mPressed = true;

    // Turn off both Chorus I and II
    IControl* pI = GetUI()->GetControlWithParamIdx(mChorusIParam);
    if (pI) { pI->SetValue(0.0); pI->SetDirty(true); }

    IControl* pII = GetUI()->GetControlWithParamIdx(mChorusIIParam);
    if (pII) { pII->SetValue(0.0); pII->SetDirty(true); }

    SetDirty(false);
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    mPressed = false;
    SetDirty(false);
  }

private:
  int mChorusIParam;
  int mChorusIIParam;
  bool mPressed = false;
};

// Helper: draw a single pixel as a 1x1 filled rect
static inline void Pixel(IGraphics& g, const IColor& c, float x, float y)
{
  g.FillRect(c, IRECT(x, y, x + 1.f, y + 1.f));
}

// Helper: draw a 7-wide x 4-tall downward-pointing chevron, tip at (cx, y+3)
static inline void DrawDownChevron(IGraphics& g, const IColor& c, float cx, float y)
{
  Pixel(g, c, cx - 3, y);     Pixel(g, c, cx + 3, y);
  Pixel(g, c, cx - 2, y + 1); Pixel(g, c, cx + 2, y + 1);
  Pixel(g, c, cx - 1, y + 2); Pixel(g, c, cx + 1, y + 2);
  Pixel(g, c, cx,     y + 3);
}

// Button + LED combo (button_control in original).
// The button is at (x, y) size 17x19, LED is at (x+4, y-9) size 9x9
// Button bevel is momentary (pressed while mouse held), LED shows param state
class KR106ButtonLEDControl : public IControl
{
public:
  KR106ButtonLEDControl(const IRECT& bounds, int paramIdx, EKR106ButtonType type, const IBitmap& ledBitmap)
  : IControl(bounds, paramIdx)
  , mType(type)
  , mLEDBitmap(ledBitmap)
  {
  }

  void Draw(IGraphics& g) override
  {
    float bx = std::round(mRECT.L);
    float by = std::round(mRECT.T);
    bool powered = GetDelegate()->GetParam(kPower)->Bool();
    bool on = powered && GetValue() > 0.5;

    // LED at (bx+4, by), 9x9 — reflects parameter state
    int frameIdx = on ? 2 : 1;
    IRECT ledRect(bx + 4, by, bx + 4 + mLEDBitmap.FW(), by + mLEDBitmap.FH());
    g.DrawBitmap(mLEDBitmap, ledRect, frameIdx, &mBlend);

    // Button at (bx, by+9), 17x19 — momentary press visual
    float btnT = by + 9;
    DrawKR106Button(g, bx, btnT, bx + 17, btnT + 19, mType, mPressed);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mPressed = true;
    SetValue(GetValue() > 0.5 ? 0.0 : 1.0);
    SetDirty(true);
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    mPressed = false;
    SetDirty(false);
  }

private:
  EKR106ButtonType mType;
  IBitmap mLEDBitmap;
  bool mPressed = false;
};

// ============================================================================
// Power switch — 15x19 toggle button from kr106_powerswitch.c
// Black background, gray rect border, gray/red indicator line
// ============================================================================
class KR106PowerSwitchControl : public IControl
{
public:
  KR106PowerSwitchControl(const IRECT& bounds)
  : IControl(bounds, kPower)
  {}

  void Draw(IGraphics& g) override
  {
    float ox = std::round(mRECT.L);
    float oy = std::round(mRECT.T);
    const IColor black(255, 0, 0, 0);
    const IColor gray(255, 128, 128, 128);
    const IColor red(255, 255, 0, 0);

    bool on = GetValue() > 0.5;

    // Black background
    g.FillRect(black, IRECT(ox, oy, ox + 15, oy + 19));
    // Gray rect border (2,2,11,15 → pixel outline)
    PixelRect(g, gray, ox + 2, oy + 2, ox + 13, oy + 17);

    if (on) {
      HLine(g, gray, ox + 4, oy + 11, ox + 11);  // gray line at y=11
      HLine(g, red, ox + 6, oy + 13, ox + 10);    // red line at y=13
    } else {
      HLine(g, gray, ox + 4, oy + 5, ox + 11);    // gray line at y=5
    }
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    SetValue(GetValue() < 0.5 ? 1.0 : 0.0);
    SetDirty();
  }
};

// ============================================================================
// LFO Trigger — 41x19 momentary button from kr106_lfotrig.c
// Black border, cream fill, dark gray inner decoration
// ============================================================================
class KR106LFOTrigControl : public IControl
{
public:
  KR106LFOTrigControl(const IRECT& bounds)
  : IControl(bounds, kNoParameter)
  {}

  void Draw(IGraphics& g) override
  {
    float ox = std::round(mRECT.L);
    float oy = std::round(mRECT.T);
    const IColor black(255, 0, 0, 0);
    const IColor cream(255, 220, 220, 178);
    const IColor gray(255, 37, 37, 37);

    // Black border, cream fill
    PixelRect(g, black, ox, oy, ox + 41, oy + 19);
    g.FillRect(cream, IRECT(ox + 1, oy + 1, ox + 40, oy + 18));

    if (mPressed) {
      // Inner black border when pressed
      PixelRect(g, black, ox + 1, oy + 1, ox + 40, oy + 18);
      // Gray decoration lines (shifted inward by 1)
      HLine(g, gray, ox + 6, oy + 4, ox + 35);
      HLine(g, gray, ox + 6, oy + 14, ox + 35);
      VLine(g, gray, ox + 5, oy + 5, oy + 14);
      VLine(g, gray, ox + 35, oy + 5, oy + 14);
    } else {
      // Gray decoration lines
      HLine(g, gray, ox + 5, oy + 4, ox + 36);
      HLine(g, gray, ox + 5, oy + 14, ox + 36);
      VLine(g, gray, ox + 4, oy + 5, oy + 14);
      VLine(g, gray, ox + 36, oy + 5, oy + 14);
    }
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mPressed = true;
    // Send CC1 (mod wheel) = 127 to trigger LFO in manual mode
    IMidiMsg msg;
    msg.MakeControlChangeMsg(IMidiMsg::kModWheel, 127, 0);
    GetDelegate()->SendMidiMsgFromUI(msg);
    SetDirty(false);
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    mPressed = false;
    // Send CC1 = 0 to release LFO trigger
    IMidiMsg msg;
    msg.MakeControlChangeMsg(IMidiMsg::kModWheel, 0, 0);
    GetDelegate()->SendMidiMsgFromUI(msg);
    SetDirty(false);
  }

private:
  bool mPressed = false;
};

// ============================================================================
// Pitch Bend Lever — 60x8 horizontal control from kr106_bender.c
// Gradient bitmap background, trig-based pointer, spring-back to center
// ============================================================================
class KR106BenderControl : public IControl
{
public:
  KR106BenderControl(const IRECT& bounds, int paramIdx, const IBitmap& gradientBitmap)
  : IControl(bounds, paramIdx)
  , mGradient(gradientBitmap)
  {}

  void Draw(IGraphics& g) override
  {
    float ox = std::round(mRECT.L);
    float oy = std::round(mRECT.T);
    const IColor black(255, 0, 0, 0);
    const IColor gray(255, 128, 128, 128);
    const IColor white(255, 255, 255, 255);

    // Black background (4,0 to w-4,h)
    g.FillRect(black, IRECT(ox + 4, oy, ox + 56, oy + 8));

    // Gradient bitmap at (5,1), 50x6
    IRECT gradRect(ox + 5, oy + 1, ox + 55, oy + 7);
    g.DrawBitmap(mGradient, gradRect, 1, &mBlend);

    // Pointer — value is normalized 0-1, real range -1 to +1
    float value = (float)GetValue() * 2.f - 1.f;
    float midpoint = 30.f;
    float angle = (float)M_PI * (2.f - value) / 4.f;

    float basex1 = cosf(angle + (float)M_PI / 20.f) * 24.f + midpoint;
    float basex2 = cosf(angle - (float)M_PI / 20.f) * 24.f + midpoint;
    float pointx1 = cosf(angle + (float)M_PI / 50.f) * 36.f + midpoint;
    float pointx2 = cosf(angle - (float)M_PI / 50.f) * 36.f + midpoint;

    // Gray outer shape
    if (basex1 < pointx1)
      g.FillRect(gray, IRECT(ox + basex1, oy + 1, ox + pointx2 + 2, oy + 7));
    else
      g.FillRect(gray, IRECT(ox + pointx1, oy + 1, ox + basex2 + 1, oy + 7));

    // White inner pointer
    g.FillRect(white, IRECT(ox + pointx1, oy + 1, ox + pointx2 + 1, oy + 7));

    // Corner pixels
    Pixel(g, gray, ox + pointx1, oy + 1);
    Pixel(g, gray, ox + pointx1, oy + 6);
    Pixel(g, gray, ox + pointx2, oy + 1);
    Pixel(g, gray, ox + pointx2, oy + 6);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    float lx = x - std::round(mRECT.L);
    float midpoint = 30.f;
    float value = (lx - midpoint) / midpoint;
    value = std::max(-1.f, std::min(1.f, value));
    SetValue((value + 1.f) / 2.f);
    SetDirty(true);
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
    float scale = mod.S ? 512.f : 128.f;  // Shift for fine control
    float value = (float)GetValue() * 2.f - 1.f;
    value += dX / scale;
    value = std::max(-1.f, std::min(1.f, value));
    SetValue((value + 1.f) / 2.f);
    SetDirty(true);
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    // Spring back to center
    SetValue(0.5);
    SetDirty(true);
  }

private:
  IBitmap mGradient;
};

// ============================================================================
// KR106SwitchControl — IBSwitchControl replacement with drag up/down support
// Click cycles through states; vertical drag snaps to nearest position
// ============================================================================
class KR106SwitchControl : public IControl, public IBitmapBase
{
public:
  KR106SwitchControl(float x, float y, const IBitmap& bitmap, int paramIdx)
  : IControl(IRECT(x, y, x + (float)bitmap.FW(), y + (float)bitmap.FH()), paramIdx)
  , IBitmapBase(bitmap)
  {
    mControl = this;
    mDisablePrompt = false;
  }

  void Draw(IGraphics& g) override { DrawBitmap(g); }
  void OnRescale() override { mBitmap = GetUI()->GetScaledBitmap(mBitmap); }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    if (mod.R) { PromptUserInput(); return; }
    mDragStartY = y;
    mDragStartIdx = CurrentIdx();
    mDragged = false;
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
    int n = mBitmap.N();
    if (n <= 1) return;

    float totalDrag = y - mDragStartY;
    if (std::abs(totalDrag) > 3.f) mDragged = true;

    // Half frame height of drag = one position change
    int steps = (int)(totalDrag / ((float)mBitmap.FH() * 0.5f));
    int newIdx = mDragStartIdx + steps;
    if (newIdx < 0) newIdx = 0;
    if (newIdx >= n) newIdx = n - 1;

    SetValue((double)newIdx / (double)(n - 1));
    SetDirty(true);
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    if (!mDragged)
    {
      // Click to cycle
      int n = mBitmap.N();
      if (n > 1)
      {
        int idx = CurrentIdx() + 1;
        if (idx >= n) idx = 0;
        SetValue((double)idx / (double)(n - 1));
      }
      SetDirty(true);
    }
  }

private:
  int CurrentIdx() const
  {
    return (int)(0.5 + GetValue() * (double)(mBitmap.N() - 1));
  }

  float mDragStartY = 0.f;
  int mDragStartIdx = 0;
  bool mDragged = false;
};

// ============================================================================
// KR106ScopeControl — oscilloscope display (green waveform on black)
// Triggered by oscillator sync pulse (ch1) like original scope_buffer.c
// ============================================================================
class KR106ScopeControl : public IControl
{
public:
  static constexpr int MAXBUF = 128;
  static constexpr int RING_SIZE = 4096;

  KR106ScopeControl(const IRECT& bounds)
  : IControl(bounds)
  {}

  void OnMsgFromDelegate(int msgTag, int dataSize, const void* pData) override
  {
    if (!IsDisabled() && msgTag == ISender<>::kUpdateMessage)
    {
      IByteStream stream(pData, dataSize);
      int pos = 0;
      ISenderData<2, std::array<float, MAXBUF>> buf;
      pos = stream.Get(&buf, pos);

      // Append audio (ch0) and sync (ch1) to ring buffers
      for (int i = 0; i < MAXBUF; i++)
      {
        mRing[mWritePos] = buf.vals[0][i];
        mSyncRing[mWritePos] = buf.vals[1][i];
        mWritePos = (mWritePos + 1) % RING_SIZE;
      }
      mSamplesAvail += MAXBUF;
      if (mSamplesAvail > RING_SIZE) mSamplesAvail = RING_SIZE;

      // Search backwards for two consecutive sync pulses — the samples
      // between them are exactly one waveform period
      int endDist = -1, startDist = -1;
      for (int s = 1; s <= mSamplesAvail; s++)
      {
        int idx = (mWritePos - s + RING_SIZE) % RING_SIZE;
        if (mSyncRing[idx] > 0.f)
        {
          if (endDist < 0)
            endDist = s;
          else
          {
            startDist = s;
            break;
          }
        }
      }

      if (startDist > 0 && endDist > 0)
      {
        int period = startDist - endDist;
        if (period > 1)
        {
          mDisplayLen = period;
          int startIdx = (mWritePos - startDist + RING_SIZE) % RING_SIZE;
          for (int i = 0; i < period; i++)
            mDisplay[i] = mRing[(startIdx + i) % RING_SIZE];
          mHasData = true;
        }
      }

      SetDirty(false);
    }
  }

  void Draw(IGraphics& g) override
  {
    float ox = std::round(mRECT.L);
    float oy = std::round(mRECT.T);
    int w = (int)(mRECT.W());
    int h = (int)(mRECT.H());
    int v2 = h / 2;

    const IColor black(255, 0, 0, 0);
    const IColor dim(255, 0, 128, 0);
    const IColor mid(255, 0, 192, 0);
    const IColor bright(255, 0, 255, 0);

    // Black background
    g.FillRect(black, IRECT(ox, oy, ox + w, oy + h));

    if (!GetDelegate()->GetParam(kPower)->Bool())
      return;

    // Vertical grid lines (every w/4 pixels)
    int wlines = w / 4;
    for (int i = 0; i < w; i += wlines)
      VLine(g, dim, ox + i, oy, oy + h);
    VLine(g, dim, ox + w - 1, oy, oy + h);

    // Horizontal grid lines — 5 evenly spaced (top, 1/4, center, 3/4, bottom)
    for (int i = 0; i < 5; i++)
      HLine(g, dim, ox, oy + std::round(i * (h - 1) / 4.f), ox + w);

    // Center line (mid green, zero crossing)
    HLine(g, mid, ox, oy + v2, ox + w);

    // Waveform — one full period interpolated to fill the display width
    if (mHasData && mDisplayLen > 1)
    {
      int lastY = v2;
      for (int i = 0; i < w; i++)
      {
        float pos = static_cast<float>(i) / static_cast<float>(w) * mDisplayLen;
        int s0 = static_cast<int>(pos);
        float frac = pos - s0;
        if (s0 >= mDisplayLen - 1) { s0 = mDisplayLen - 2; frac = 1.f; }

        float sample = mDisplay[s0] + frac * (mDisplay[s0 + 1] - mDisplay[s0]);
        int y = static_cast<int>(sample * -v2 + v2);
        y = std::clamp(y, 0, h - 1);

        int y1 = std::min(lastY, y);
        int y2 = std::max(lastY, y) + 1;
        VLine(g, bright, ox + i, oy + y1, oy + y2);
        lastY = y;
      }
    }
  }

private:
  float mRing[RING_SIZE] = {};
  float mSyncRing[RING_SIZE] = {};
  float mDisplay[RING_SIZE] = {};
  int mDisplayLen = 0;
  int mWritePos = 0;
  int mSamplesAvail = 0;
  bool mHasData = false;
};

// ============================================================================
// Minimal real FFT magnitude — ported from original fft.c (Steven Trainoff, 1993)
// Uses the n/2 complex trick: n real samples → n/2 magnitude bins
// ============================================================================
namespace kr106_fft {

static inline int ilog2(int n)
{
  int i = -1;
  while (n) { i++; n >>= 1; }
  return i;
}

static inline unsigned bitrev(unsigned k, int nu)
{
  unsigned r = 0;
  for (int i = 0; i < nu; i++) { r <<= 1; r |= k & 1; k >>= 1; }
  return r;
}

// In-place complex FFT, n must be power of 2
static inline void fft(float* re, float* im, int n)
{
  int nu = ilog2(n);

  // Bit-reversal permutation
  for (int j = 0; j < n; j++)
  {
    int i = (int)bitrev((unsigned)j, nu);
    if (i > j)
    {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }

  // Butterfly stages
  for (int s = 1; s <= nu; s++)
  {
    int m = 1 << s;
    int m2 = m / 2;
    float wpr = cosf(static_cast<float>(M_PI) / m2);
    float wpi = -sinf(static_cast<float>(M_PI) / m2);
    float wr = 1.f, wi = 0.f;

    for (int j = 0; j < m2; j++)
    {
      for (int k = j; k < n; k += m)
      {
        int t = k + m2;
        float tr = wr * re[t] - wi * im[t];
        float ti = wr * im[t] + wi * re[t];
        re[t] = re[k] - tr;
        im[t] = im[k] - ti;
        re[k] += tr;
        im[k] += ti;
      }
      float tmp = wr;
      wr = tmp * wpr - wi * wpi;
      wi = tmp * wpi + wi * wpr;
    }
  }
}

// Real FFT magnitude: n real samples → n/2 magnitude bins written to mag[]
// n must be power of 2, max 256
static inline void realfftmag(const float* data, float* mag, int n)
{
  int half = n / 2;
  float re[128] = {}, im[128] = {};

  // Pack: even→real, odd→imag
  for (int i = 0; i < half; i++)
  {
    re[i] = data[2 * i];
    im[i] = data[2 * i + 1];
  }

  fft(re, im, half);

  // DC bin
  mag[0] = fabsf(re[0] + im[0]) / n;

  // Unscramble and compute magnitude
  for (int i = 1; i < half; i++)
  {
    float tr = (re[i] - re[half - i]) / 2.f;
    float ti = (im[i] + im[half - i]) / 2.f;
    float arg = 2.f * static_cast<float>(M_PI) * i / n;
    float c = cosf(arg), s = sinf(arg);

    float xr = (re[i] + re[half - i]) / 2.f + c * ti - s * tr;
    float xi = (im[i] - im[half - i]) / 2.f - s * ti - c * tr;

    xr /= half;
    xi /= half;

    mag[i] = sqrtf(xr * xr + xi * xi);
  }

  // Nyquist bin
  mag[half] = fabsf(re[0] - im[0]) / n;
}

} // namespace kr106_fft

// ============================================================================
// KR106AnalyzerControl — frequency spectrum display (green bars on black)
// 128x33 pixels, shows FFT magnitude spectrum from IBufferSender
// Ported from analyzer_buffer.c + fft.c — original collected samples then ran realfftmag
// ============================================================================
class KR106AnalyzerControl : public IControl
{
public:
  static constexpr int MAXBUF = 128;
  static constexpr int NUM_BINS = MAXBUF / 2; // 64 magnitude bins from 128 samples

  KR106AnalyzerControl(const IRECT& bounds)
  : IControl(bounds)
  {}

  void OnMsgFromDelegate(int msgTag, int dataSize, const void* pData) override
  {
    if (!IsDisabled() && msgTag == ISender<>::kUpdateMessage)
    {
      IByteStream stream(pData, dataSize);
      int pos = 0;
      pos = stream.Get(&mBuf, pos);

      // Compute FFT magnitude spectrum (like original analyzer_buffer + realfftmag)
      kr106_fft::realfftmag(mBuf.vals[0].data(), mMag, MAXBUF);
      mHasData = true;
      SetDirty(false);
    }
  }

  void Draw(IGraphics& g) override
  {
    float ox = std::round(mRECT.L);
    float oy = std::round(mRECT.T);
    int w = (int)(mRECT.W());
    int h = (int)(mRECT.H());

    const IColor black(255, 0, 0, 0);
    const IColor dim(255, 0, 128, 0);
    const IColor bright(255, 0, 255, 0);

    // Black background with dim border
    g.FillRect(black, IRECT(ox, oy, ox + w, oy + h));
    PixelRect(g, dim, ox, oy, ox + w, oy + h);

    if (mHasData)
    {
      // Scale: 64 bins across 128 pixels = 2 pixels per bin
      float binScale = static_cast<float>(w) / NUM_BINS;

      for (int i = 0; i < NUM_BINS; i++)
      {
        // Convert magnitude to dB-ish display scale (log scale looks better)
        float mag = mMag[i] * 4.f; // boost for visibility
        if (mag > 1.f) mag = 1.f;
        int newH = static_cast<int>(mag * h);

        // Persistence decay
        if (newH > mPeaks[i])
          mPeaks[i] = newH;
        else
          mPeaks[i] = static_cast<int>(mPeaks[i] * 0.9f);

        int px = static_cast<int>(ox + i * binScale);
        int px2 = static_cast<int>(ox + (i + 1) * binScale);

        // Dim trail
        if (mPeaks[i] > newH)
        {
          for (int x = px; x < px2 && x < (int)(ox + w); x++)
            VLine(g, dim, (float)x, oy + h - mPeaks[i], oy + h - newH);
        }
        // Bright bar
        if (newH > 0)
        {
          for (int x = px; x < px2 && x < (int)(ox + w); x++)
            VLine(g, bright, (float)x, oy + h - newH, oy + h);
        }
      }
    }
  }

private:
  ISenderData<1, std::array<float, MAXBUF>> mBuf;
  float mMag[NUM_BINS + 1] = {};
  int mPeaks[NUM_BINS] = {};
  bool mHasData = false;
};

// ============================================================================
// Keyboard colors (5 grayscale from original kr106_keyboard.c)
// ============================================================================
static const IColor kKB_Black(255, 0, 0, 0);
static const IColor kKB_Dark(255, 128, 128, 128);
static const IColor kKB_Mid(255, 179, 179, 179);
static const IColor kKB_Light(255, 221, 221, 221);
static const IColor kKB_White(255, 255, 255, 255);

// ============================================================================
// Keyboard key-drawing functions — pixel-perfect port of kr106_keyboard.c
// ox,oy = absolute screen position of this key's origin
// Original draw_line(x1,y1,x2,y2) is inclusive; our HLine/VLine endpoint is exclusive (+1)
// Original fill_rect(x,y,w,h) maps to FillRect(x,y,x+w,y+h)
// ============================================================================

static void KB_DrawBottom(IGraphics& g, float ox, float oy, bool pressed)
{
  if (pressed) {
    Pixel(g, kKB_White, ox + 3, oy + 105);
    HLine(g, kKB_Light, ox + 4, oy + 105, ox + 20);
    HLine(g, kKB_Mid, ox + 3, oy + 106, ox + 20);
    HLine(g, kKB_Mid, ox + 2, oy + 107, ox + 21);
    Pixel(g, kKB_Dark, ox + 20, oy + 106);
    VLine(g, kKB_Black, ox + 1, oy + 106, oy + 108);
    VLine(g, kKB_Black, ox + 21, oy + 106, oy + 108);
    HLine(g, kKB_Black, ox + 1, oy + 108, ox + 22);
    // g.FillRect(kKB_White, IRECT(ox + 3, oy + 102, ox + 22, oy + 106)); // highlight square removed
  } else {
    HLine(g, kKB_Mid, ox + 3, oy + 105, ox + 20);
    HLine(g, kKB_Mid, ox + 2, oy + 106, ox + 21);
    HLine(g, kKB_Black, ox, oy + 107, ox + 22);
  }
}

static void KB_DrawLeft(IGraphics& g, float ox, float oy, bool pressed)
{
  // background
  g.FillRect(kKB_Light, IRECT(ox + 4, oy, ox + 11, oy + 57));
  g.FillRect(kKB_Light, IRECT(ox + 4, oy + 57, ox + 20, oy + 105));
  // left edge
  VLine(g, kKB_Black, ox, oy, oy + 108);
  Pixel(g, kKB_Black, ox + 1, oy + 106);
  VLine(g, pressed ? kKB_Black : kKB_Dark, ox + 1, oy, oy + 106);
  // highlight
  VLine(g, pressed ? kKB_Dark : kKB_White, ox + 2, oy, oy + 107);
  VLine(g, kKB_White, ox + 3, oy, oy + 106);
  KB_DrawBottom(g, ox, oy, pressed);
  // right edge
  g.FillRect(kKB_Dark, IRECT(ox + 11, oy, ox + 13, oy + 57));
  HLine(g, kKB_Dark, ox + 11, oy + 56, ox + 21);
  g.FillRect(kKB_Dark, IRECT(ox + 20, oy + 56, ox + 22, oy + 106));
  if (pressed) {
    g.FillRect(kKB_Dark, IRECT(ox + 11, oy + 57, ox + 13, oy + 59));
    g.FillRect(kKB_Black, IRECT(ox + 13, oy + 56, ox + 22, oy + 59));
  }
  Pixel(g, kKB_Black, ox + 21, oy + 106);
}

static void KB_DrawMiddle1(IGraphics& g, float ox, float oy, bool pressed)
{
  // background
  g.FillRect(kKB_Light, IRECT(ox + 7, oy, ox + 16, oy + 57));
  g.FillRect(kKB_Light, IRECT(ox + 4, oy + 57, ox + 20, oy + 105));
  // left side
  VLine(g, pressed ? kKB_Black : kKB_Dark, ox + 5, oy, oy + 57);
  HLine(g, kKB_Dark, ox + 1, oy + 56, ox + 5);
  VLine(g, pressed ? kKB_Black : kKB_Dark, ox + 1, oy + 56, oy + 107);
  VLine(g, kKB_Black, ox, oy + 56, oy + 107);
  Pixel(g, kKB_Black, ox + 1, oy + 106);
  // highlight
  VLine(g, kKB_White, ox + 6, oy, oy + 57);
  VLine(g, pressed ? kKB_Dark : kKB_White, ox + 2, oy + 57, oy + 107);
  VLine(g, kKB_White, ox + 3, oy + 57, oy + 106);
  if (pressed) {
    g.FillRect(kKB_Black, IRECT(ox + 2, oy + 56, ox + 6, oy + 59));
    VLine(g, kKB_White, ox + 6, oy + 56, oy + 59);
  }
  KB_DrawBottom(g, ox, oy, pressed);
  // right side
  g.FillRect(kKB_Dark, IRECT(ox + 16, oy, ox + 18, oy + 57));
  HLine(g, kKB_Dark, ox + 18, oy + 56, ox + 20);
  g.FillRect(kKB_Dark, IRECT(ox + 20, oy + 56, ox + 22, oy + 106));
  Pixel(g, kKB_Black, ox + 21, oy + 106);
  if (pressed) {
    g.FillRect(kKB_Dark, IRECT(ox + 16, oy + 56, ox + 18, oy + 59));
    g.FillRect(kKB_Black, IRECT(ox + 18, oy + 56, ox + 22, oy + 59));
  }
}

static void KB_DrawMiddle2(IGraphics& g, float ox, float oy, bool pressed)
{
  // background
  g.FillRect(kKB_Light, IRECT(ox + 7, oy, ox + 13, oy + 57));
  g.FillRect(kKB_Light, IRECT(ox + 4, oy + 57, ox + 20, oy + 105));
  // left side
  VLine(g, kKB_Dark, ox + 5, oy, oy + 57);
  HLine(g, kKB_Dark, ox + 1, oy + 56, ox + 5);
  VLine(g, pressed ? kKB_Black : kKB_Dark, ox + 1, oy + 56, oy + 107);
  VLine(g, kKB_Black, ox, oy + 56, oy + 107);
  Pixel(g, kKB_Black, ox + 1, oy + 106);
  // highlight
  VLine(g, kKB_White, ox + 6, oy, oy + 57);
  VLine(g, pressed ? kKB_Dark : kKB_White, ox + 2, oy + 57, oy + 107);
  VLine(g, kKB_White, ox + 3, oy + 57, oy + 106);
  if (pressed) {
    g.FillRect(kKB_Black, IRECT(ox + 1, oy + 56, ox + 5, oy + 59));
    VLine(g, kKB_Dark, ox + 5, oy + 56, oy + 59);
    VLine(g, kKB_White, ox + 6, oy + 56, oy + 59);
  }
  KB_DrawBottom(g, ox, oy, pressed);
  // right side
  Pixel(g, kKB_Black, ox + 21, oy + 106);
  g.FillRect(kKB_Dark, IRECT(ox + 13, oy, ox + 15, oy + 57));
  HLine(g, kKB_Dark, ox + 15, oy + 56, ox + 20);
  g.FillRect(kKB_Dark, IRECT(ox + 20, oy + 56, ox + 22, oy + 106));
  if (pressed) {
    g.FillRect(kKB_Dark, IRECT(ox + 13, oy + 56, ox + 15, oy + 59));
    g.FillRect(kKB_Black, IRECT(ox + 15, oy + 56, ox + 22, oy + 59));
  }
}

static void KB_DrawMiddle3(IGraphics& g, float ox, float oy, bool pressed)
{
  // background
  g.FillRect(kKB_Light, IRECT(ox + 9, oy, ox + 16, oy + 57));
  g.FillRect(kKB_Light, IRECT(ox + 4, oy + 57, ox + 20, oy + 105));
  // left side
  VLine(g, kKB_Dark, ox + 7, oy, oy + 57);
  HLine(g, kKB_Dark, ox + 1, oy + 56, ox + 7);
  VLine(g, pressed ? kKB_Black : kKB_Dark, ox + 1, oy + 56, oy + 107);
  VLine(g, kKB_Black, ox, oy + 56, oy + 107);
  Pixel(g, kKB_Black, ox + 1, oy + 106);
  // highlight
  VLine(g, kKB_White, ox + 8, oy, oy + 57);
  VLine(g, pressed ? kKB_Dark : kKB_White, ox + 2, oy + 57, oy + 107);
  VLine(g, kKB_White, ox + 3, oy + 57, oy + 106);
  if (pressed) {
    g.FillRect(kKB_Black, IRECT(ox + 1, oy + 56, ox + 7, oy + 59));
    VLine(g, kKB_Dark, ox + 7, oy + 56, oy + 59);
    VLine(g, kKB_White, ox + 8, oy + 56, oy + 59);
  }
  KB_DrawBottom(g, ox, oy, pressed);
  // right side
  Pixel(g, kKB_Black, ox + 21, oy + 106);
  g.FillRect(kKB_Dark, IRECT(ox + 16, oy, ox + 18, oy + 57));
  HLine(g, kKB_Dark, ox + 18, oy + 56, ox + 20);
  g.FillRect(kKB_Dark, IRECT(ox + 20, oy + 56, ox + 22, oy + 106));
  if (pressed) {
    g.FillRect(kKB_Dark, IRECT(ox + 16, oy + 56, ox + 18, oy + 59));
    g.FillRect(kKB_Black, IRECT(ox + 18, oy + 56, ox + 22, oy + 59));
  }
}

static void KB_DrawRight(IGraphics& g, float ox, float oy, bool pressed)
{
  // background
  g.FillRect(kKB_Light, IRECT(ox + 12, oy, ox + 20, oy + 57));
  g.FillRect(kKB_Light, IRECT(ox + 4, oy + 57, ox + 20, oy + 105));
  // left side
  VLine(g, kKB_Black, ox, oy + 56, oy + 108);
  Pixel(g, kKB_Black, ox + 1, oy + 106);
  VLine(g, kKB_Dark, ox + 10, oy, oy + 57);
  HLine(g, kKB_Dark, ox + 1, oy + 56, ox + 10);
  VLine(g, pressed ? kKB_Black : kKB_Dark, ox + 1, oy + 56, oy + 107);
  // highlight
  VLine(g, kKB_White, ox + 11, oy, oy + 57);
  VLine(g, pressed ? kKB_Dark : kKB_White, ox + 2, oy + 57, oy + 107);
  VLine(g, kKB_White, ox + 3, oy + 57, oy + 106);
  if (pressed) {
    VLine(g, kKB_Dark, ox + 10, oy + 56, oy + 59);
    VLine(g, kKB_White, ox + 11, oy + 56, oy + 59);
    g.FillRect(kKB_Black, IRECT(ox + 1, oy + 56, ox + 10, oy + 59));
  }
  KB_DrawBottom(g, ox, oy, pressed);
  // right side
  g.FillRect(kKB_Dark, IRECT(ox + 20, oy, ox + 22, oy + 106));
  Pixel(g, kKB_Black, ox + 21, oy + 106);
}

static void KB_DrawBlack(IGraphics& g, float ox, float oy, bool pressed)
{
  int dy = pressed ? 2 : 0;
  g.FillRect(kKB_Black, IRECT(ox, oy, ox + 14, oy + 56));
  VLine(g, kKB_Dark, ox + 1, oy, oy + 51);
  VLine(g, kKB_Dark, ox + 1, oy + 52, oy + 55);
  VLine(g, kKB_Light, ox + 2, oy + 49 + dy, oy + 51 + dy);
  HLine(g, kKB_Light, ox + 3, oy + 51 + dy, ox + 5);
  if (pressed) {
    // g.FillRect(kKB_Dark, IRECT(ox + 4, oy + 48, ox + 10, oy + 50)); // highlight rectangle removed
  }
}

static void KB_DrawLast(IGraphics& g, float ox, float oy, bool pressed)
{
  // background
  g.FillRect(kKB_Light, IRECT(ox + 4, oy, ox + 20, oy + 107));
  // left edge
  VLine(g, kKB_Black, ox, oy, oy + 108);
  Pixel(g, kKB_Black, ox + 1, oy + 106);
  VLine(g, pressed ? kKB_Black : kKB_Dark, ox + 1, oy, oy + 106);
  // highlight
  VLine(g, pressed ? kKB_Dark : kKB_White, ox + 2, oy, oy + 107);
  VLine(g, kKB_White, ox + 3, oy, oy + 106);
  // bottom
  if (pressed) {
    HLine(g, kKB_Mid, ox + 3, oy + 106, ox + 20);
    HLine(g, kKB_Mid, ox + 2, oy + 107, ox + 21);
    HLine(g, kKB_Black, ox, oy + 108, ox + 22);
    // g.FillRect(kKB_White, IRECT(ox + 3, oy + 102, ox + 22, oy + 106)); // highlight square removed
  } else {
    HLine(g, kKB_Mid, ox + 3, oy + 105, ox + 20);
    HLine(g, kKB_Mid, ox + 2, oy + 106, ox + 21);
    HLine(g, kKB_Black, ox, oy + 107, ox + 22);
  }
  // right side
  g.FillRect(kKB_Dark, IRECT(ox + 20, oy, ox + 22, oy + 106));
  VLine(g, kKB_Black, ox + 22, oy, oy + 108);
  Pixel(g, kKB_Black, ox + 21, oy + 106);
}

// ============================================================================
// KR106KeyboardControl — pixel-perfect keyboard from original kr106_keyboard.c
// 792x109 pixels, 61 keys (C2-C7), sends MIDI NoteOn/NoteOff
// ============================================================================
class KR106KeyboardControl : public IControl
{
public:
  static constexpr int kMinNote = 36;  // C2
  static constexpr int kNumKeys = 61;

  KR106KeyboardControl(const IRECT& bounds, const IBitmap& chevronBitmap)
  : IControl(bounds)
  , mChevronBitmap(chevronBitmap)
  {
    memset(mKeys, 0, sizeof(mKeys));
  }

  void Draw(IGraphics& g) override
  {
    float ox = std::round(mRECT.L);
    float strip = std::round(mRECT.T);  // y=106: 5px strip for transpose indicator
    float oy = strip + 5;               // y=111: keys start here

    for (int oct = 0; oct < 5; oct++) {
      int key = oct * 12;
      float x = ox + oct * 154;
      KB_DrawLeft   (g, x,       oy, mKeys[key]);
      KB_DrawBlack  (g, x + 13,  oy, mKeys[key + 1]);
      KB_DrawMiddle1(g, x + 22,  oy, mKeys[key + 2]);
      KB_DrawBlack  (g, x + 40,  oy, mKeys[key + 3]);
      KB_DrawRight  (g, x + 44,  oy, mKeys[key + 4]);
      KB_DrawLeft   (g, x + 66,  oy, mKeys[key + 5]);
      KB_DrawBlack  (g, x + 79,  oy, mKeys[key + 6]);
      KB_DrawMiddle2(g, x + 88,  oy, mKeys[key + 7]);
      KB_DrawBlack  (g, x + 103, oy, mKeys[key + 8]);
      KB_DrawMiddle3(g, x + 110, oy, mKeys[key + 9]);
      KB_DrawBlack  (g, x + 128, oy, mKeys[key + 10]);
      KB_DrawRight  (g, x + 132, oy, mKeys[key + 11]);
    }
    KB_DrawLast(g, ox + 770, oy, mKeys[60]);

    // Transpose indicator — chevron bitmap in the 5px strip above the keys
    if (mTransposeKey >= 0)
    {
      static const int xOffsets[12]      = {0, 13, 22, 40, 44, 66, 79, 88, 103, 110, 128, 132};
      // Per-key center offset from key's draw origin (derived from top light-fill midpoints)
      // C    C#   D    D#   E    F    F#   G    G#   A    A#   B
      static const float kCX[12]        = {7.f, 7.f,12.f, 7.f,16.f, 7.f, 7.f,10.f, 7.f,13.f, 7.f,16.f};
      float kx = (mTransposeKey == 60) ? ox + 770
               : ox + (mTransposeKey / 12) * 154.f + xOffsets[mTransposeKey % 12];
      float cx = kx + (mTransposeKey == 60 ? 12.f : kCX[mTransposeKey % 12]);
      float bw = mChevronBitmap.FW();
      float bh = mChevronBitmap.FH();
      IRECT chevRect(cx - bw / 2.f, strip + (5.f - bh) / 2.f,
                     cx + bw / 2.f, strip + (5.f - bh) / 2.f + bh);
      g.DrawBitmap(mChevronBitmap, chevRect, 1, nullptr);
    }
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    int key = MouseToKey(x, y);
    if (key < 0) return;

    bool transposeOn = GetDelegate()->GetParam(kTranspose)->Bool();
    if (transposeOn)
    {
      // Transpose mode: just move the highlight indicator, don't press the key.
      int offset = (key + kMinNote) - 60;
      mTransposeKey = (offset == 0) ? -1 : key; // hide chevron when back at root
      static_cast<KR106*>(GetDelegate())->SetTransposeOffset(offset);
      mPressedKey = -1;
      SetDirty(false);
      return;
    }

    bool holdOn = GetDelegate()->GetParam(kHold)->Bool();
    if (holdOn && mKeys[key]) {
      // Second click on a held key: toggle it off visually and in DSP/arp
      mKeys[key] = false;
      static_cast<KR106*>(GetDelegate())->ForceReleaseNote(key + kMinNote);
      mHoldRelease = true;
      mPressedKey = -1;
    } else {
      mKeys[key] = true;
      mPressedKey = key;
      SendNoteOn(key);
      mHoldRelease = false;
    }
    SetDirty(false);
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
    bool transposeOn = GetDelegate()->GetParam(kTranspose)->Bool();
    if (transposeOn)
    {
      int key = MouseToKey(x, y);
      if (key < 0 || key == mTransposeKey) return;
      int offset = (key + kMinNote) - 60;
      mTransposeKey = (offset == 0) ? -1 : key; // hide chevron when back at root
      static_cast<KR106*>(GetDelegate())->SetTransposeOffset(offset);
      SetDirty(false);
      return;
    }

    int key = MouseToKey(x, y);
    if (key == mPressedKey) return;
    if (mPressedKey >= 0) {
      mKeys[mPressedKey] = false;
      SendNoteOff(mPressedKey);
    }
    if (key >= 0) {
      mKeys[key] = true;
      mPressedKey = key;
      SendNoteOn(key);
    } else {
      mPressedKey = -1;
    }
    mHoldRelease = false;
    SetDirty(false);
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    mHoldRelease = false;
    if (mPressedKey >= 0) {
      bool holdOn = GetDelegate()->GetParam(kHold)->Bool();
      if (!holdOn)
        mKeys[mPressedKey] = false; // Hold off: clear visual immediately
      // Always send NoteOff: when hold is on it's suppressed by ProcessMidiMsg but
      // registers in mHeldNotes so ReleaseHeldNotes() can clean up when hold turns off.
      // The visual stays lit because mKeys wasn't cleared and the NoteOff is never
      // forwarded to the keyboard display (suppressed before reaching mMidiForKeyboard).
      SendNoteOff(mPressedKey);
      mPressedKey = -1;
      SetDirty(false);
    }
  }

  void OnMidi(const IMidiMsg& msg) override
  {
    switch (msg.StatusMsg()) {
      case IMidiMsg::kNoteOn:
        SetNoteFromMidi(msg.NoteNumber(), msg.Velocity() != 0);
        break;
      case IMidiMsg::kNoteOff:
        SetNoteFromMidi(msg.NoteNumber(), false);
        break;
      default: break;
    }
  }

  void SetNoteFromMidi(int noteNum, bool played)
  {
    int key = noteNum - kMinNote;
    if (key < 0 || key >= kNumKeys) return;
    mKeys[key] = played;
    SetDirty(false);
  }

  // Called from OnIdle when Transpose turns off — clears the transpose root indicator.
  void ClearTransposeKey()
  {
    if (mTransposeKey >= 0)
    {
      mTransposeKey = -1;
      SetDirty(false);
    }
  }

private:
  static constexpr int kTopOffsets[] = {13, 27, 40, 54, 66, 79, 93, 103, 117, 128, 142, 154};
  static constexpr int kBottomOffsets[] = {22, 22, 44, 44, 66, 88, 88, 110, 110, 132, 132, 154};

  int MouseToKey(float mx, float my) const
  {
    int lx = (int)(mx - std::round(mRECT.L));
    int ly = (int)(my - std::round(mRECT.T)) - 5; // account for 5px strip above keys
    if (lx < 0 || lx >= 792 || ly < 0 || ly >= 109) return -1;

    int octave = lx / 154;
    int offset = lx % 154;
    const int* offsets = (ly <= 56) ? kTopOffsets : kBottomOffsets;

    for (int i = 0; i < 12; i++) {
      if (offset < offsets[i]) {
        int key = octave * 12 + i;
        return (key < kNumKeys) ? key : kNumKeys - 1;
      }
    }
    return -1;
  }

  void SendNoteOn(int key)
  {
    IMidiMsg msg;
    msg.MakeNoteOnMsg(key + kMinNote, 127, 0);
    GetDelegate()->SendMidiMsgFromUI(msg);
  }

  void SendNoteOff(int key)
  {
    IMidiMsg msg;
    msg.MakeNoteOffMsg(key + kMinNote, 0);
    GetDelegate()->SendMidiMsgFromUI(msg);
  }

  bool mKeys[kNumKeys] = {};
  int mPressedKey = -1;
  int mTransposeKey = -1; // key index of current transpose root indicator, -1 = none
  IBitmap mChevronBitmap;
  bool mHoldRelease = false;
};
