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

        if (mScaleIdx < 3)
            paintWaveform(g, w, h, black, dim, mid, bright);
        else
            paintADSR(g, w, h, dim, mid, bright);
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        mScaleIdx = (mScaleIdx + 1) % 4; // 0-2: waveform scales, 3: ADSR
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
        if (newSamples == 0)
        {
            // ADSR mode reads slider params directly — repaint even with no audio
            if (mScaleIdx == 3) repaint();
            return;
        }

        // Copy new samples into local ring buffer
        float peak = 0.f;
        for (int i = 0; i < newSamples; i++)
        {
            int srcIdx = (mLocalReadPos + i) % KR106AudioProcessor::kScopeRingSize;
            float s = mProcessor->mScopeRing[srcIdx];
            float absS = s < 0.f ? -s : s;
            if (absS > peak) peak = absS;

            mRing[mRingWritePos]     = s;
            mRingR[mRingWritePos]    = mProcessor->mScopeRingR[srcIdx];
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
                {
                    int idx = (startIdx + i) % RING_SIZE;
                    mDisplay[i]  = mRing[idx];
                    mDisplayR[i] = mRingR[idx];
                }
                mHasData = true;
            }
        }

        repaint();
    }

private:
    // ---- Waveform display ----
    void paintWaveform(juce::Graphics& g, int w, int h,
                       juce::Colour /*black*/, juce::Colour dim, juce::Colour mid, juce::Colour bright)
    {
        int v2 = h / 2;
        float scale = kScales[mScaleIdx];

        // Vertical: 3 interior lines (skip edges)
        g.setColour(dim);
        for (int i = 1; i <= 3; i++)
        {
            float x = std::round(static_cast<float>(i) / 4.f * (w - 1));
            g.fillRect(x, 0.f, 1.f, static_cast<float>(h));
        }

        // Horizontal: one line per 0.5 amplitude units (skip edge lines)
        int numSteps = static_cast<int>(scale / 0.5f);
        for (int i = -numSteps + 1; i <= numSteps - 1; i++)
        {
            float y = std::round((i * 0.5f / scale) * -v2 + v2);
            g.setColour(i == 0 ? mid : dim);
            g.fillRect(0.f, y, static_cast<float>(w), 1.f);
        }

        // Waveform -- one full period interpolated to fill the display width
        if (mHasData && mDisplayLen > 1)
        {
            // R channel (dimmer, drawn first so L overlays it)
            g.setColour(dim);
            int lastYR = static_cast<int>((mDisplayR[0] / scale) * -v2 + v2);
            for (int i = 0; i < w; i++)
            {
                float pos = static_cast<float>(i) / static_cast<float>(w) * mDisplayLen;
                int s0 = static_cast<int>(pos);
                float frac = pos - s0;
                if (s0 >= mDisplayLen - 1) { s0 = mDisplayLen - 2; frac = 1.f; }

                float sample = mDisplayR[s0] + frac * (mDisplayR[s0 + 1] - mDisplayR[s0]);
                int y = static_cast<int>((sample / scale) * -v2 + v2);

                int y1 = std::min(lastYR, y);
                int y2 = std::max(lastYR, y) + 1;
                g.fillRect(static_cast<float>(i), static_cast<float>(y1),
                           1.f, static_cast<float>(y2 - y1));
                lastYR = y;
            }

            // L channel (bright, on top)
            g.setColour(bright);
            int lastY = static_cast<int>((mDisplay[0] / scale) * -v2 + v2);
            for (int i = 0; i < w; i++)
            {
                float pos = static_cast<float>(i) / static_cast<float>(w) * mDisplayLen;
                int s0 = static_cast<int>(pos);
                float frac = pos - s0;
                if (s0 >= mDisplayLen - 1) { s0 = mDisplayLen - 2; frac = 1.f; }

                float sample = mDisplay[s0] + frac * (mDisplay[s0 + 1] - mDisplay[s0]);
                int y = static_cast<int>((sample / scale) * -v2 + v2);

                int y1 = std::min(lastY, y);
                int y2 = std::max(lastY, y) + 1;
                g.fillRect(static_cast<float>(i), static_cast<float>(y1),
                           1.f, static_cast<float>(y2 - y1));
                lastY = y;
            }
        }
    }

    // ---- ADSR envelope display ----
    void paintADSR(juce::Graphics& g, int w, int h,
                   juce::Colour dim, juce::Colour mid, juce::Colour bright)
    {
        if (!mProcessor) return;

        using DSP = KR106DSP<float>;
        auto& dsp = mProcessor->mDSP;

        // Read current ADSR parameters
        bool j6 = (dsp.mAdsrMode == 0);
        float attackMs, decayMs, releaseMs;
        if (j6) {
            // Juno-6: exponential slider→tau formulas (must match KR106_DSP_SetParam.h)
            attackMs  = 0.001f * std::pow(3000.f, dsp.mSliderA) * 1000.f;
            decayMs   = 0.004f * std::pow(1000.f, dsp.mSliderD) * 1000.f;
            releaseMs = 0.004f * std::pow(1000.f, dsp.mSliderR) * 1000.f;
        } else {
            attackMs  = DSP::LookupLUT(DSP::kAttackLUT,  dsp.mSliderA);
            decayMs   = DSP::LookupLUT(DSP::kDecayLUT,   dsp.mSliderD);
            releaseMs = DSP::LookupLUT(DSP::kReleaseLUT, dsp.mSliderR);
        }
        float sustain   = std::max(mProcessor->getParam(kEnvS)->getValue(), 0.001f);

        // Sustain display length: proportional to max(D, R), clamped
        float sustainMs = std::clamp(0.5f * std::max(decayMs, releaseMs), 50.f, 2000.f);

        float totalMs = attackMs + decayMs + sustainMs + releaseMs;

        // Phase boundaries as fractions of total time
        float tA = attackMs / totalMs;
        float tD = (attackMs + decayMs) / totalMs;
        float tS = (attackMs + decayMs + sustainMs) / totalMs;
        // tR = 1.0

        // Vertical phase boundary lines (A|D, D|S, S|R) — always interior
        g.setColour(dim);
        float xA = std::round(tA * (w - 1));
        float xD = std::round(tD * (w - 1));
        float xS = std::round(tS * (w - 1));
        g.fillRect(xA, 0.f, 1.f, static_cast<float>(h));
        g.fillRect(xD, 0.f, 1.f, static_cast<float>(h));
        g.fillRect(xS, 0.f, 1.f, static_cast<float>(h));

        // Sustain level horizontal line (skip if at top or bottom edge)
        float sustainY = std::round((1.f - sustain) * (h - 1));
        if (sustainY > 0.f && sustainY < static_cast<float>(h - 1))
        {
            g.setColour(dim);
            g.fillRect(0.f, sustainY, static_cast<float>(w), 1.f);
        }

        // ADSR time constants for exponential math
        static constexpr float kMinLevel = 0.001f; // -60dB
        static constexpr float kAttackTarget = 1.5f;

        // Precompute per-ms rates
        float decayMul = (decayMs > 0.f) ? expf(logf(kMinLevel) / decayMs) : kMinLevel;
        float releaseMul = (releaseMs > 0.f) ? expf(logf(kMinLevel) / releaseMs) : kMinLevel;
        float attackCoeff = 0.f;
        if (j6 && attackMs > 0.f)
            attackCoeff = 1.f - expf(-logf(3.f) / attackMs);

        // Draw the curve
        g.setColour(bright);
        int lastY = h - 1; // start at baseline

        for (int px = 0; px < w; px++)
        {
            float t = static_cast<float>(px) / static_cast<float>(w - 1);
            float env = 0.f;

            if (t <= tA)
            {
                // Attack phase
                float frac = (tA > 0.f) ? t / tA : 1.f;
                float ms = frac * attackMs;
                if (j6)
                {
                    // RC exponential: env = kAttackTarget * (1 - (1-coeff)^ms)
                    // Simulating: env += (kAttackTarget - env) * coeff each ms
                    env = kAttackTarget * (1.f - powf(1.f - attackCoeff, ms));
                    if (env > 1.f) env = 1.f;
                }
                else
                {
                    // Linear ramp
                    env = (attackMs > 0.f) ? ms / attackMs : 1.f;
                    if (env > 1.f) env = 1.f;
                }
            }
            else if (t <= tD)
            {
                // Decay phase: exponential from 1.0 toward 0, stopped at sustain
                float frac = (tD > tA) ? (t - tA) / (tD - tA) : 1.f;
                float ms = frac * decayMs;
                env = powf(decayMul, ms);
                if (env < sustain) env = sustain;
            }
            else if (t <= tS)
            {
                // Sustain phase: flat
                env = sustain;
            }
            else
            {
                // Release phase: exponential from sustain toward 0
                float frac = (1.f > tS) ? (t - tS) / (1.f - tS) : 1.f;
                float ms = frac * releaseMs;
                env = sustain * powf(releaseMul, ms);
            }

            int y = static_cast<int>(std::round((1.f - env) * (h - 1)));

            int y1 = std::min(lastY, y);
            int y2 = std::max(lastY, y) + 1;
            g.fillRect(static_cast<float>(px), static_cast<float>(y1),
                       1.f, static_cast<float>(y2 - y1));
            lastY = y;
        }
    }

    static constexpr float kScales[3] = { 0.5f, 1.f, 1.5f };
    int mScaleIdx = 1; // default 1.0

    KR106AudioProcessor* mProcessor = nullptr;

    // Local ring buffers (copied from processor)
    float mRing[RING_SIZE] = {};
    float mRingR[RING_SIZE] = {};
    float mSyncRing[RING_SIZE] = {};
    int mRingWritePos = 0;
    int mLocalReadPos = 0;
    int mSamplesAvail = 0;

    // Display buffer (one extracted period)
    float mDisplay[RING_SIZE] = {};
    float mDisplayR[RING_SIZE] = {};
    int mDisplayLen = 0;
    bool mHasData = false;
};
