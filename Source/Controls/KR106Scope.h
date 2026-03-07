#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include <algorithm>
#include <cmath>

// ============================================================================
// KR106Scope -- oscilloscope display (green waveform on black)
// Triggered by oscillator sync pulse; reads from processor's ring buffer
// Port of KR106ScopeControl from iPlug2
// ============================================================================
class KR106Scope : public juce::Component
{
public:
    static constexpr int RING_SIZE = 4096;

    KR106Scope(KR106AudioProcessor* processor) : mProcessor(processor)
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void paint(juce::Graphics& g) override
    {
        int w = getWidth();
        int h = getHeight();
        int v2 = h / 2;

        const auto black  = juce::Colour(0, 0, 0);
        const auto dim    = juce::Colour(0, 128, 0);
        const auto mid    = juce::Colour(0, 192, 0);
        const auto bright = juce::Colour(0, 255, 0);

        // Black background
        g.setColour(black);
        g.fillRect(0, 0, w, h);

        // Check power
        if (mProcessor && mProcessor->getParam(kPower)->getValue() <= 0.5f)
            return;

        // Vertical grid lines (every w/4 pixels)
        int wlines = w / 4;
        for (int i = 0; i < w; i += wlines)
        {
            g.setColour(dim);
            g.fillRect(static_cast<float>(i), 0.f, 1.f, static_cast<float>(h));
        }
        g.setColour(dim);
        g.fillRect(static_cast<float>(w - 1), 0.f, 1.f, static_cast<float>(h));

        // Horizontal grid lines every 0.5 amplitude units
        float scale = kScales[mScaleIdx];
        int numSteps = static_cast<int>(scale / 0.5f);
        for (int i = -numSteps; i <= numSteps; i++)
        {
            float y = std::clamp(std::round((i * 0.5f / scale) * -v2 + v2),
                                 0.f, static_cast<float>(h - 1));
            g.setColour(i == 0 ? mid : dim);
            g.fillRect(0.f, y, static_cast<float>(w), 1.f);
        }

        // Waveform -- one full period interpolated to fill the display width
        if (mHasData && mDisplayLen > 1)
        {
            g.setColour(bright);
            int lastY = v2;
            for (int i = 0; i < w; i++)
            {
                float pos = static_cast<float>(i) / static_cast<float>(w) * mDisplayLen;
                int s0 = static_cast<int>(pos);
                float frac = pos - s0;
                if (s0 >= mDisplayLen - 1) { s0 = mDisplayLen - 2; frac = 1.f; }

                float sample = mDisplay[s0] + frac * (mDisplay[s0 + 1] - mDisplay[s0]);
                int y = static_cast<int>((sample / scale) * -v2 + v2);
                y = std::clamp(y, 0, h - 1);

                int y1 = std::min(lastY, y);
                int y2 = std::max(lastY, y) + 1;
                g.fillRect(static_cast<float>(i), static_cast<float>(y1),
                           1.f, static_cast<float>(y2 - y1));
                lastY = y;
            }
        }
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        mScaleIdx = (mScaleIdx + 1) % 3;
        repaint();
    }

    // Call from editor's timer callback (~30 Hz) to pull data from processor
    void updateFromProcessor()
    {
        if (!mProcessor) return;

        // Read processor's write position (acquire ordering)
        int writePos = mProcessor->mScopeWritePos.load(std::memory_order_acquire);

        // Calculate how many new samples are available
        int newSamples = (writePos - mLocalReadPos + KR106AudioProcessor::kScopeRingSize)
                         % KR106AudioProcessor::kScopeRingSize;
        if (newSamples == 0) return;

        // Copy new samples into local ring buffer
        float peak = 0.f;
        for (int i = 0; i < newSamples; i++)
        {
            int srcIdx = (mLocalReadPos + i) % KR106AudioProcessor::kScopeRingSize;
            float s = mProcessor->mScopeRing[srcIdx];
            float absS = s < 0.f ? -s : s;
            if (absS > peak) peak = absS;

            mRing[mRingWritePos]     = s;
            mSyncRing[mRingWritePos] = mProcessor->mScopeSyncRing[srcIdx];
            mRingWritePos = (mRingWritePos + 1) % RING_SIZE;
        }
        mLocalReadPos = writePos;
        mSamplesAvail += newSamples;
        if (mSamplesAvail > RING_SIZE) mSamplesAvail = RING_SIZE;

        // If audio is silent, clear display
        if (peak < 1e-6f)
        {
            mHasData = false;
            repaint();
            return;
        }

        // Search backwards for two consecutive sync pulses -- the samples
        // between them are one full sub-oscillator period (two DCO cycles)
        int endDist = -1, startDist = -1;
        for (int s = 1; s <= mSamplesAvail; s++)
        {
            int idx = (mRingWritePos - s + RING_SIZE) % RING_SIZE;
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
                int startIdx = (mRingWritePos - startDist + RING_SIZE) % RING_SIZE;
                for (int i = 0; i < period; i++)
                    mDisplay[i] = mRing[(startIdx + i) % RING_SIZE];
                mHasData = true;
            }
        }

        repaint();
    }

private:
    static constexpr float kScales[3] = { 0.5f, 1.f, 2.f };
    int mScaleIdx = 1; // default 1.0

    KR106AudioProcessor* mProcessor = nullptr;

    // Local ring buffers (copied from processor)
    float mRing[RING_SIZE] = {};
    float mSyncRing[RING_SIZE] = {};
    int mRingWritePos = 0;
    int mLocalReadPos = 0;
    int mSamplesAvail = 0;

    // Display buffer (one extracted period)
    float mDisplay[RING_SIZE] = {};
    int mDisplayLen = 0;
    bool mHasData = false;
};
