#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// ============================================================================
// KR106Keyboard — pixel-perfect 61-key keyboard (C2-C7)
// 792x114 pixels: 5px strip for transpose chevron + 109px keys
// Port of KR106KeyboardControl from iPlug2
// ============================================================================
class KR106Keyboard : public juce::Component
{
public:
    static constexpr int kMinNote = 36;  // C2
    static constexpr int kNumKeys = 61;

    KR106Keyboard(KR106AudioProcessor* processor, const juce::Image& chevronImage)
        : mProcessor(processor)
        , mChevronImage(chevronImage)
    {
    }

    void paint(juce::Graphics& g) override
    {
        float ox = 0.f;
        float strip = 0.f;  // 5px strip at top for transpose indicator
        float oy = 5.f;     // keys start here

        // Fill entire white-key area first to avoid sub-pixel gaps on Windows
        g.setColour(kKB_Light);
        g.fillRect(ox, oy, 792.f, 108.f);

        for (int oct = 0; oct < 5; oct++)
        {
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

        // Transpose indicator — chevron bitmap in the 5px strip above keys
        if (mTransposeKey >= 0)
        {
            static const int xOffsets[12] = {0, 13, 22, 40, 44, 66, 79, 88, 103, 110, 128, 132};
            static const float kCX[12]    = {7.f, 7.f,12.f, 7.f,16.f, 7.f, 7.f,10.f, 7.f,13.f, 7.f,16.f};
            float kx = (mTransposeKey == 60) ? ox + 770
                      : ox + (mTransposeKey / 12) * 154.f + xOffsets[mTransposeKey % 12];
            float cx = kx + (mTransposeKey == 60 ? 12.f : kCX[mTransposeKey % 12]);
            float bw = mChevronImage.getWidth() / 2.f;   // 1x width
            float bh = mChevronImage.getHeight() / 2.f;  // 1x height
            g.drawImage(mChevronImage,
                        cx - bw / 2.f, strip + (5.f - bh) / 2.f, bw, bh,
                        0, 0, mChevronImage.getWidth(), mChevronImage.getHeight());
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        int key = mouseToKey(e.position.x, e.position.y);
        if (key < 0) return;

        bool transposeOn = mProcessor && mProcessor->getParam(kTranspose)->getValue() > 0.5f;
        if (transposeOn)
        {
            int offset = (key + kMinNote) - 60;
            mTransposeKey = (offset == 0) ? -1 : key;
            setTransposeOffset(offset);
            mPressedKey = -1;
            repaint();
            return;
        }

        bool holdOn = mProcessor && mProcessor->getParam(kHold)->getValue() > 0.5f;
        if (holdOn && mKeys[key])
        {
            // Second click on held key: force release
            mKeys[key] = false;
            mForceReleasePending[key] = true;
            if (mProcessor) mProcessor->forceReleaseNote(key + kMinNote);
            mHoldRelease = true;
            mPressedKey = -1;
        }
        else
        {
            mKeys[key] = true;
            mPressedKey = key;
            sendNoteOn(key);
            mHoldRelease = false;
        }
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        bool transposeOn = mProcessor && mProcessor->getParam(kTranspose)->getValue() > 0.5f;
        if (transposeOn)
        {
            int key = mouseToKey(e.position.x, e.position.y);
            if (key < 0 || key == mTransposeKey) return;
            int offset = (key + kMinNote) - 60;
            mTransposeKey = (offset == 0) ? -1 : key;
            setTransposeOffset(offset);
            repaint();
            return;
        }

        if (mHoldRelease) return; // don't re-trigger during force release click

        int key = mouseToKey(e.position.x, e.position.y);
        if (key == mPressedKey) return;
        if (mPressedKey >= 0)
        {
            mKeys[mPressedKey] = false;
            sendNoteOff(mPressedKey);
        }
        if (key >= 0)
        {
            mKeys[key] = true;
            mPressedKey = key;
            sendNoteOn(key);
        }
        else
        {
            mPressedKey = -1;
        }
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        mHoldRelease = false;
        if (mPressedKey >= 0)
        {
            bool holdOn = mProcessor && mProcessor->getParam(kHold)->getValue() > 0.5f;
            if (!holdOn)
                mKeys[mPressedKey] = false;
            sendNoteOff(mPressedKey);
            mPressedKey = -1;
            repaint();
        }
    }

    // Called from editor timer (~30 Hz) to sync external MIDI display
    void updateFromProcessor()
    {
        if (!mProcessor) return;

        bool changed = false;
        for (int i = 0; i < kNumKeys; i++)
        {
            if (i == mPressedKey) continue; // don't override mouse-pressed key
            bool held = mProcessor->mKeyboardHeld.test(i + kMinNote);
            if (mForceReleasePending[i])
            {
                // Don't re-light until audio thread has processed the release
                if (!held)
                {
                    mForceReleasePending[i] = false;
                    if (mKeys[i]) { mKeys[i] = false; changed = true; }
                }
                continue;
            }
            if (mKeys[i] != held)
            {
                mKeys[i] = held;
                changed = true;
            }
        }
        // Sync transpose chevron from parameter (e.g. MIDI set transpose)
        auto* tp = mProcessor->getParam(kTransposeOffset);
        if (tp)
        {
            int offset = static_cast<int>(tp->convertFrom0to1(tp->getValue()));
            int key = offset + 60 - kMinNote;
            int newKey = (key >= 0 && key < kNumKeys) ? key : -1;
            if (offset == 0) newKey = -1;
            if (newKey != mTransposeKey) { mTransposeKey = newKey; changed = true; }
        }

        if (changed)
            repaint();
    }

    void setNoteFromMidi(int noteNum, bool played)
    {
        int key = noteNum - kMinNote;
        if (key < 0 || key >= kNumKeys) return;
        mKeys[key] = played;
        repaint();
    }

    // Restore transpose chevron from saved offset (called after state restore)
    void setTransposeKeyFromOffset(int semitones)
    {
        int key = semitones + 60 - kMinNote;
        if (key >= 0 && key < kNumKeys)
            mTransposeKey = key;
        else
            mTransposeKey = -1;
        repaint();
    }

private:
    // ========== Keyboard colors ==========
    static inline const juce::Colour kKB_Black{0, 0, 0};
    static inline const juce::Colour kKB_Dark{128, 128, 128};
    static inline const juce::Colour kKB_Mid{179, 179, 179};
    static inline const juce::Colour kKB_Light{221, 221, 221};
    static inline const juce::Colour kKB_White{255, 255, 255};

    // ========== Drawing helpers ==========
    static void hLine(juce::Graphics& g, juce::Colour c, float x1, float y, float x2)
    {
        g.setColour(c); g.fillRect(x1, y, x2 - x1, 1.f);
    }
    static void vLine(juce::Graphics& g, juce::Colour c, float x, float y1, float y2)
    {
        g.setColour(c); g.fillRect(x, y1, 1.f, y2 - y1);
    }
    static void pixel(juce::Graphics& g, juce::Colour c, float x, float y)
    {
        g.setColour(c); g.fillRect(x, y, 1.f, 1.f);
    }
    static void fillRect(juce::Graphics& g, juce::Colour c, float l, float t, float r, float b)
    {
        g.setColour(c); g.fillRect(l, t, r - l, b - t);
    }

    // ========== Key drawing functions ==========
    static void KB_DrawBottom(juce::Graphics& g, float ox, float oy, bool pressed)
    {
        if (pressed) {
            pixel(g, kKB_White, ox + 3, oy + 105);
            hLine(g, kKB_Light, ox + 4, oy + 105, ox + 20);
            hLine(g, kKB_Mid, ox + 3, oy + 106, ox + 20);
            hLine(g, kKB_Mid, ox + 2, oy + 107, ox + 21);
            pixel(g, kKB_Dark, ox + 20, oy + 106);
            vLine(g, kKB_Black, ox + 1, oy + 106, oy + 108);
            vLine(g, kKB_Black, ox + 21, oy + 106, oy + 108);
            hLine(g, kKB_Black, ox + 1, oy + 108, ox + 22);
        } else {
            hLine(g, kKB_Mid, ox + 3, oy + 105, ox + 20);
            hLine(g, kKB_Mid, ox + 2, oy + 106, ox + 21);
            hLine(g, kKB_Black, ox, oy + 107, ox + 22);
        }
    }

    static void KB_DrawLeft(juce::Graphics& g, float ox, float oy, bool pressed)
    {
        // background
        fillRect(g, kKB_Light, ox + 4, oy, ox + 11, oy + 57);
        fillRect(g, kKB_Light, ox + 4, oy + 57, ox + 20, oy + 105);
        // left edge
        vLine(g, kKB_Black, ox, oy, oy + 108);
        pixel(g, kKB_Black, ox + 1, oy + 106);
        vLine(g, pressed ? kKB_Black : kKB_Dark, ox + 1, oy, oy + 106);
        // highlight
        vLine(g, pressed ? kKB_Dark : kKB_White, ox + 2, oy, oy + 107);
        vLine(g, kKB_White, ox + 3, oy, oy + 106);
        KB_DrawBottom(g, ox, oy, pressed);
        // right edge
        fillRect(g, kKB_Dark, ox + 11, oy, ox + 13, oy + 57);
        hLine(g, kKB_Dark, ox + 11, oy + 56, ox + 21);
        fillRect(g, kKB_Dark, ox + 20, oy + 56, ox + 22, oy + 106);
        if (pressed) {
            fillRect(g, kKB_Dark, ox + 11, oy + 57, ox + 13, oy + 59);
            fillRect(g, kKB_Black, ox + 13, oy + 56, ox + 22, oy + 59);
        }
        pixel(g, kKB_Black, ox + 21, oy + 106);
    }

    static void KB_DrawMiddle1(juce::Graphics& g, float ox, float oy, bool pressed)
    {
        // background
        fillRect(g, kKB_Light, ox + 7, oy, ox + 16, oy + 57);
        fillRect(g, kKB_Light, ox + 4, oy + 57, ox + 20, oy + 105);
        // left side
        vLine(g, pressed ? kKB_Black : kKB_Dark, ox + 5, oy, oy + 57);
        hLine(g, kKB_Dark, ox + 1, oy + 56, ox + 5);
        vLine(g, pressed ? kKB_Black : kKB_Dark, ox + 1, oy + 56, oy + 107);
        vLine(g, kKB_Black, ox, oy + 56, oy + 107);
        pixel(g, kKB_Black, ox + 1, oy + 106);
        // highlight
        vLine(g, kKB_White, ox + 6, oy, oy + 57);
        vLine(g, pressed ? kKB_Dark : kKB_White, ox + 2, oy + 57, oy + 107);
        vLine(g, kKB_White, ox + 3, oy + 57, oy + 106);
        if (pressed) {
            fillRect(g, kKB_Black, ox + 2, oy + 56, ox + 6, oy + 59);
            vLine(g, kKB_White, ox + 6, oy + 56, oy + 59);
        }
        KB_DrawBottom(g, ox, oy, pressed);
        // right side
        fillRect(g, kKB_Dark, ox + 16, oy, ox + 18, oy + 57);
        hLine(g, kKB_Dark, ox + 18, oy + 56, ox + 20);
        fillRect(g, kKB_Dark, ox + 20, oy + 56, ox + 22, oy + 106);
        pixel(g, kKB_Black, ox + 21, oy + 106);
        if (pressed) {
            fillRect(g, kKB_Dark, ox + 16, oy + 56, ox + 18, oy + 59);
            fillRect(g, kKB_Black, ox + 18, oy + 56, ox + 22, oy + 59);
        }
    }

    static void KB_DrawMiddle2(juce::Graphics& g, float ox, float oy, bool pressed)
    {
        // background
        fillRect(g, kKB_Light, ox + 7, oy, ox + 13, oy + 57);
        fillRect(g, kKB_Light, ox + 4, oy + 57, ox + 20, oy + 105);
        // left side
        vLine(g, kKB_Dark, ox + 5, oy, oy + 57);
        hLine(g, kKB_Dark, ox + 1, oy + 56, ox + 5);
        vLine(g, pressed ? kKB_Black : kKB_Dark, ox + 1, oy + 56, oy + 107);
        vLine(g, kKB_Black, ox, oy + 56, oy + 107);
        pixel(g, kKB_Black, ox + 1, oy + 106);
        // highlight
        vLine(g, kKB_White, ox + 6, oy, oy + 57);
        vLine(g, pressed ? kKB_Dark : kKB_White, ox + 2, oy + 57, oy + 107);
        vLine(g, kKB_White, ox + 3, oy + 57, oy + 106);
        if (pressed) {
            fillRect(g, kKB_Black, ox + 1, oy + 56, ox + 5, oy + 59);
            vLine(g, kKB_Dark, ox + 5, oy + 56, oy + 59);
            vLine(g, kKB_White, ox + 6, oy + 56, oy + 59);
        }
        KB_DrawBottom(g, ox, oy, pressed);
        // right side
        pixel(g, kKB_Black, ox + 21, oy + 106);
        fillRect(g, kKB_Dark, ox + 13, oy, ox + 15, oy + 57);
        hLine(g, kKB_Dark, ox + 15, oy + 56, ox + 20);
        fillRect(g, kKB_Dark, ox + 20, oy + 56, ox + 22, oy + 106);
        if (pressed) {
            fillRect(g, kKB_Dark, ox + 13, oy + 56, ox + 15, oy + 59);
            fillRect(g, kKB_Black, ox + 15, oy + 56, ox + 22, oy + 59);
        }
    }

    static void KB_DrawMiddle3(juce::Graphics& g, float ox, float oy, bool pressed)
    {
        // background
        fillRect(g, kKB_Light, ox + 9, oy, ox + 16, oy + 57);
        fillRect(g, kKB_Light, ox + 4, oy + 57, ox + 20, oy + 105);
        // left side
        vLine(g, kKB_Dark, ox + 7, oy, oy + 57);
        hLine(g, kKB_Dark, ox + 1, oy + 56, ox + 7);
        vLine(g, pressed ? kKB_Black : kKB_Dark, ox + 1, oy + 56, oy + 107);
        vLine(g, kKB_Black, ox, oy + 56, oy + 107);
        pixel(g, kKB_Black, ox + 1, oy + 106);
        // highlight
        vLine(g, kKB_White, ox + 8, oy, oy + 57);
        vLine(g, pressed ? kKB_Dark : kKB_White, ox + 2, oy + 57, oy + 107);
        vLine(g, kKB_White, ox + 3, oy + 57, oy + 106);
        if (pressed) {
            fillRect(g, kKB_Black, ox + 1, oy + 56, ox + 7, oy + 59);
            vLine(g, kKB_Dark, ox + 7, oy + 56, oy + 59);
            vLine(g, kKB_White, ox + 8, oy + 56, oy + 59);
        }
        KB_DrawBottom(g, ox, oy, pressed);
        // right side
        pixel(g, kKB_Black, ox + 21, oy + 106);
        fillRect(g, kKB_Dark, ox + 16, oy, ox + 18, oy + 57);
        hLine(g, kKB_Dark, ox + 18, oy + 56, ox + 20);
        fillRect(g, kKB_Dark, ox + 20, oy + 56, ox + 22, oy + 106);
        if (pressed) {
            fillRect(g, kKB_Dark, ox + 16, oy + 56, ox + 18, oy + 59);
            fillRect(g, kKB_Black, ox + 18, oy + 56, ox + 22, oy + 59);
        }
    }

    static void KB_DrawRight(juce::Graphics& g, float ox, float oy, bool pressed)
    {
        // background
        fillRect(g, kKB_Light, ox + 12, oy, ox + 20, oy + 57);
        fillRect(g, kKB_Light, ox + 4, oy + 57, ox + 20, oy + 105);
        // left side
        vLine(g, kKB_Black, ox, oy + 56, oy + 108);
        pixel(g, kKB_Black, ox + 1, oy + 106);
        vLine(g, kKB_Dark, ox + 10, oy, oy + 57);
        hLine(g, kKB_Dark, ox + 1, oy + 56, ox + 10);
        vLine(g, pressed ? kKB_Black : kKB_Dark, ox + 1, oy + 56, oy + 107);
        // highlight
        vLine(g, kKB_White, ox + 11, oy, oy + 57);
        vLine(g, pressed ? kKB_Dark : kKB_White, ox + 2, oy + 57, oy + 107);
        vLine(g, kKB_White, ox + 3, oy + 57, oy + 106);
        if (pressed) {
            vLine(g, kKB_Dark, ox + 10, oy + 56, oy + 59);
            vLine(g, kKB_White, ox + 11, oy + 56, oy + 59);
            fillRect(g, kKB_Black, ox + 1, oy + 56, ox + 10, oy + 59);
        }
        KB_DrawBottom(g, ox, oy, pressed);
        // right side
        fillRect(g, kKB_Dark, ox + 20, oy, ox + 22, oy + 106);
        pixel(g, kKB_Black, ox + 21, oy + 106);
    }

    static void KB_DrawBlack(juce::Graphics& g, float ox, float oy, bool pressed)
    {
        int dy = pressed ? 2 : 0;
        fillRect(g, kKB_Black, ox, oy, ox + 14, oy + 56);
        vLine(g, kKB_Dark, ox + 1, oy, oy + 51);
        vLine(g, kKB_Dark, ox + 1, oy + 52, oy + 55);
        vLine(g, kKB_Light, ox + 2, oy + 49 + dy, oy + 51 + dy);
        hLine(g, kKB_Light, ox + 3, oy + 51 + dy, ox + 5);
    }

    static void KB_DrawLast(juce::Graphics& g, float ox, float oy, bool pressed)
    {
        // background
        fillRect(g, kKB_Light, ox + 4, oy, ox + 20, oy + 107);
        // left edge
        vLine(g, kKB_Black, ox, oy, oy + 108);
        pixel(g, kKB_Black, ox + 1, oy + 106);
        vLine(g, pressed ? kKB_Black : kKB_Dark, ox + 1, oy, oy + 106);
        // highlight
        vLine(g, pressed ? kKB_Dark : kKB_White, ox + 2, oy, oy + 107);
        vLine(g, kKB_White, ox + 3, oy, oy + 106);
        // bottom
        if (pressed) {
            hLine(g, kKB_Mid, ox + 3, oy + 106, ox + 20);
            hLine(g, kKB_Mid, ox + 2, oy + 107, ox + 21);
            hLine(g, kKB_Black, ox, oy + 108, ox + 22);
        } else {
            hLine(g, kKB_Mid, ox + 3, oy + 105, ox + 20);
            hLine(g, kKB_Mid, ox + 2, oy + 106, ox + 21);
            hLine(g, kKB_Black, ox, oy + 107, ox + 22);
        }
        // right side
        fillRect(g, kKB_Dark, ox + 20, oy, ox + 22, oy + 106);
        vLine(g, kKB_Black, ox + 22, oy, oy + 108);
        pixel(g, kKB_Black, ox + 21, oy + 106);
    }

    // ========== Hit detection ==========
    static constexpr int kTopOffsets[] = {13, 27, 40, 54, 66, 79, 93, 103, 117, 128, 142, 154};
    static constexpr int kBottomOffsets[] = {22, 22, 44, 44, 66, 88, 88, 110, 110, 132, 132, 154};

    int mouseToKey(float mx, float my) const
    {
        int lx = (int)mx;
        int ly = (int)my - 5; // account for 5px strip above keys
        if (lx < 0 || lx >= 792 || ly < 0 || ly >= 109) return -1;

        int octave = lx / 154;
        int offset = lx % 154;
        const int* offsets = (ly <= 56) ? kTopOffsets : kBottomOffsets;

        for (int i = 0; i < 12; i++)
        {
            if (offset < offsets[i])
            {
                int key = octave * 12 + i;
                return (key < kNumKeys) ? key : kNumKeys - 1;
            }
        }
        return -1;
    }

    // ========== MIDI helpers ==========
    void sendNoteOn(int key)
    {
        if (mProcessor)
            mProcessor->sendMidiFromUI(0x90, static_cast<uint8_t>(key + kMinNote), 127);
    }

    void sendNoteOff(int key)
    {
        if (mProcessor)
            mProcessor->sendMidiFromUI(0x80, static_cast<uint8_t>(key + kMinNote), 0);
    }

    void setTransposeOffset(int semitones)
    {
        if (!mProcessor) return;
        auto* p = mProcessor->getParam(kTransposeOffset);
        if (!p) return;
        p->beginChangeGesture();
        p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(semitones)));
        p->endChangeGesture();
    }

    // ========== State ==========
    KR106AudioProcessor* mProcessor = nullptr;
    juce::Image mChevronImage;
    bool mKeys[kNumKeys] = {};
    bool mForceReleasePending[kNumKeys] = {};
    int mPressedKey = -1;
    int mTransposeKey = -1;
    bool mHoldRelease = false;
};
