#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "PluginProcessor.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// ============================================================================
// KR106Scope -- oscilloscope display (green waveform on black)
// Triggered by oscillator sync pulse; reads from processor's ring buffer
// Port of KR106ScopeControl from iPlug2
// ============================================================================
class KR106Scope : public juce::Component
{
public:
    static constexpr int RING_SIZE = 4096;

    // Shared scope drawing constants
    static constexpr float kTickW = 1.f;
    static constexpr float kTickH = 3.f;
    static constexpr float kGridW = 1.f;
    static constexpr int   kTickSpacing = 5;
    static inline juce::Colour cBlack()  { return juce::Colour(0, 0, 0); }
    static inline juce::Colour cDim()    { return juce::Colour(0, 128, 0); }
    static inline juce::Colour cMid()    { return juce::Colour(0, 192, 0); }
    static inline juce::Colour cBright() { return juce::Colour(0, 255, 0); }

    // Center crosshairs with tick marks every kTickSpacing px
    void paintCrosshairs(juce::Graphics& g, int w, int h, juce::Colour dim)
    {
        float cx = std::round(w * 0.5f);
        float cy = std::round(h * 0.5f);

        // Center lines
        g.setColour(dim);
        g.fillRect(cx, 0.f, kGridW, static_cast<float>(h));
        g.fillRect(0.f, cy, static_cast<float>(w), kGridW);

        // Ticks on horizontal center line (radiating from center, shifted 1px down)
        for (int i = kTickSpacing; ; i += kTickSpacing)
        {
            float xr = cx + i, xl = cx - i;
            bool any = false;
            if (xr < w) { g.fillRect(xr, cy - kTickH * 0.5f + 0.5f, kTickW, kTickH); any = true; }
            if (xl >= 0) { g.fillRect(xl, cy - kTickH * 0.5f + 0.5f, kTickW, kTickH); any = true; }
            if (!any) break;
        }

        // Ticks on vertical center line (radiating from center, shifted 1px right)
        for (int i = kTickSpacing; ; i += kTickSpacing)
        {
            float yd = cy + i, yu = cy - i;
            bool any = false;
            if (yd < h) { g.fillRect(cx - kTickH * 0.5f + 0.5f, yd, kTickH, kTickW); any = true; }
            if (yu >= 0) { g.fillRect(cx - kTickH * 0.5f + 0.5f, yu, kTickH, kTickW); any = true; }
            if (!any) break;
        }
    }

    KR106Scope(KR106AudioProcessor* processor) : mProcessor(processor)
    {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        if (mScaleIdx == 4)
        {
            int prev = mPatchBankHover;
            mPatchBankHover = patchIndexAt(e.x, e.y);
            if (mPatchBankHover != prev) repaint();
        }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (mPatchBankHover >= 0) { mPatchBankHover = -1; repaint(); }
    }

    void paint(juce::Graphics& g) override
    {
        int w = getWidth();
        int h = getHeight();

        auto black = cBlack(), dim = cDim(), mid = cMid(), bright = cBright();

        g.setColour(black);
        g.fillRect(0, 0, w, h);

        if (mProcessor && mProcessor->getParam(kPower)->getValue() <= 0.5f)
            return;

        switch (mScaleIdx)
        {
            case 0: paintWaveform(g, w, h, black, dim, mid, bright); break;
            case 1: paintSpectrum(g, w, h, dim, mid, bright); break;
            case 2: paintADSR(g, w, h, dim, mid, bright); break;
            case 3: paintVCF(g, w, h, dim, mid, bright); break;
            case 4: paintPatchBank(g, w, h, dim, mid, bright); break;
            default: paintAbout(g, w, h, dim, mid, bright); break;
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (mScaleIdx == 4 && mProcessor)
        {
            if (mBallActive)
            {
                mBallActive = false;
                repaint();
                return;
            }
            int idx = patchIndexAt(e.x, e.y);
            if (idx < 0) return;

            // Select patch and start drag
            mProcessor->setCurrentProgram(idx);
            mDragOriginX = (idx % 16) * (getWidth() / 16) + (getWidth() / 32);
            mDragOriginY = (idx / 16) * (getHeight() / 8) + (getHeight() / 16);
            mDragEndX = e.x;
            mDragEndY = e.y;
            mDragging = true;
            repaint();
            if (auto* parent = getParentComponent())
                parent->repaint();
            return;
        }
        cycleMode(1);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (mScaleIdx == 4 && mDragging)
        {
            mDragEndX = std::clamp(e.x, 0, getWidth() - 1);
            mDragEndY = std::clamp(e.y, 0, getHeight() - 1);
            repaint();
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (mScaleIdx == 4 && mDragging)
        {
            mDragging = false;

            // Only launch if mouse released outside the origin cell
            int releaseIdx = patchIndexAt(mDragEndX, mDragEndY);
            int originIdx = patchIndexAt(mDragOriginX, mDragOriginY);
            float dx = static_cast<float>(mDragEndX - mDragOriginX);
            float dy = static_cast<float>(mDragEndY - mDragOriginY);
            float mag = std::sqrt(dx * dx + dy * dy);

            if (releaseIdx != originIdx && mag > 1.f)
            {
                // Quadratic curve: short drags = very slow, long drags = fast
                float norm = std::min(mag / 80.f, 1.f);
                mBallSpeed = norm * norm * 0.45f;
                mBallAccum = 0.f;
                mBallErrX = 0.f;
                mBallErrY = 0.f;
                mBallCellX = originIdx % 16;
                mBallCellY = originIdx / 16;
                // Direction opposite to drag (pool cue)
                mBallDX = -dx / mag;
                mBallDY = -dy / mag;
                mBallActive = true;
            }
            repaint();
        }
    }

    void cycleMode(int delta)
    {
        mAboutActive = false;
        // Skip patch bank (mode 4) when cycling — only accessible via P key
        int n = kNumModes;
        int next = ((mScaleIdx + delta) % n + n) % n;
        if (next == 4) next = ((next + delta) % n + n) % n;
        mScaleIdx = next;
        repaint();
    }

    void togglePatchBank()
    {
        if (mScaleIdx == 4)
        {
            mBallActive = false;
            mScaleIdx = mPrePatchBankMode;
        }
        else
        {
            mPrePatchBankMode = mScaleIdx;
            mScaleIdx = 4;
        }
        mAboutActive = false;
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
            if (mScaleIdx == 5) repaint(); // animate about screen beam
            else if (mScaleIdx == 4) { updateBall(); repaint(); }
            else if (mScaleIdx >= 2) repaintIfParamsChanged();
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

        // Update bouncing ball regardless of audio level
        if (mScaleIdx == 4) updateBall();

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
    void updateBall()
    {
        if (!mBallActive || !mProcessor) return;

        // Accumulate speed; step one cell when >= 1.0
        mBallAccum += mBallSpeed;
        if (mBallAccum < 1.f) return;
        mBallAccum -= 1.f;

        // Step one cell — use Bresenham-style error to follow the angle
        // Decide whether to step X, Y, or both (diagonal)
        mBallErrX += std::abs(mBallDX);
        mBallErrY += std::abs(mBallDY);

        int sx = (mBallDX >= 0.f) ? 1 : -1;
        int sy = (mBallDY >= 0.f) ? 1 : -1;
        int cx = mBallCellX, cy = mBallCellY;

        if (mBallErrX >= mBallErrY)
        {
            cx += sx;
            mBallErrX -= 1.f;
            if (mBallErrY >= 0.5f) { cy += sy; mBallErrY -= 1.f; }
        }
        else
        {
            cy += sy;
            mBallErrY -= 1.f;
            if (mBallErrX >= 0.5f) { cx += sx; mBallErrX -= 1.f; }
        }

        // Bounce off walls
        if (cx < 0)  { cx = 1;  mBallDX = -mBallDX; }
        if (cx > 15) { cx = 14; mBallDX = -mBallDX; }
        if (cy < 0)  { cy = 1;  mBallDY = -mBallDY; }
        if (cy > 7)  { cy = 6;  mBallDY = -mBallDY; }

        mBallCellX = cx;
        mBallCellY = cy;

        int cellIdx = cy * 16 + cx;
        int num = mProcessor->getNumPrograms();
        if (cellIdx >= 0 && cellIdx < num)
        {
            mProcessor->setCurrentProgram(cellIdx);
            if (auto* parent = getParentComponent())
                parent->repaint();
        }
    }

    int patchIndexAt(int x, int y) const
    {
        int cols = 16, rows = 8;
        int cellW = getWidth() / cols;
        int cellH = getHeight() / rows;
        int col = x / cellW;
        int row = y / cellH;
        if (col < 0 || col >= cols || row < 0 || row >= rows) return -1;
        int idx = row * cols + col;
        int num = mProcessor ? mProcessor->getNumPrograms() : 128;
        return (idx >= 0 && idx < num) ? idx : -1;
    }

    // ---- Waveform display ----
    void paintWaveform(juce::Graphics& g, int w, int h,
                       juce::Colour /*black*/, juce::Colour dim, juce::Colour mid, juce::Colour bright)
    {
        int v2 = h / 2;
        float scale = 1.f;

        paintCrosshairs(g, w, h, dim);

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
            float peakL = 0.f;
            g.setColour(bright);
            int lastY = static_cast<int>((mDisplay[0] / scale) * -v2 + v2);
            for (int i = 0; i < w; i++)
            {
                float pos = static_cast<float>(i) / static_cast<float>(w) * mDisplayLen;
                int s0 = static_cast<int>(pos);
                float frac = pos - s0;
                if (s0 >= mDisplayLen - 1) { s0 = mDisplayLen - 2; frac = 1.f; }

                float sample = mDisplay[s0] + frac * (mDisplay[s0 + 1] - mDisplay[s0]);
                peakL = std::max(peakL, fabsf(sample));
                int y = static_cast<int>((sample / scale) * -v2 + v2);

                int y1 = std::min(lastY, y);
                int y2 = std::max(lastY, y) + 1;
                g.fillRect(static_cast<float>(i), static_cast<float>(y1),
                           1.f, static_cast<float>(y2 - y1));
                lastY = y;
            }

            // Peak amplitude readout (bottom-right corner)
            float peakDb = peakL > 1e-10f ? 20.f * log10f(peakL) : -200.f;
            if (peakDb >= 0.f)
                mClipHoldFrames = 30; // ~1 second at 30 Hz refresh
            else if (mClipHoldFrames > 0)
                mClipHoldFrames--;
            auto font = juce::Font(juce::FontOptions()
                .withMetricsKind(juce::TypefaceMetricsKind::legacy)).withHeight(10.f);
            g.setFont(font);
            g.setColour(mClipHoldFrames > 0 ? bright : dim);
            g.drawText(juce::String(peakDb, 1) + " dB",
                       2, h - 14, w - 4, 12, juce::Justification::bottomRight);
        }
    }

    // ---- Spectrum analyzer (FFT) ----
    void paintSpectrum(juce::Graphics& g, int w, int h,
                       juce::Colour dim, juce::Colour mid, juce::Colour bright)
    {
        static constexpr int kFFTOrder = 12; // 4096-point FFT (~10 Hz resolution)
        static constexpr int kFFTSize = 1 << kFFTOrder;

        // Fill FFT input from ring buffer (most recent samples)
        float fftData[kFFTSize * 2] = {};
        int available = std::min(mSamplesAvail, kFFTSize);
        for (int i = 0; i < available; i++)
        {
            int idx = (mRingWritePos - available + i + RING_SIZE) % RING_SIZE;
            fftData[i] = mRing[idx];
        }

        // Apply Hann window
        for (int i = 0; i < kFFTSize; i++)
        {
            float win = 0.5f * (1.f - cosf(juce::MathConstants<float>::twoPi * i / (kFFTSize - 1)));
            fftData[i] *= win;
        }

        // FFT
        juce::dsp::FFT fft(kFFTOrder);
        fft.performRealOnlyForwardTransform(fftData);

        // Compute magnitudes in dB
        int numBins = kFFTSize / 2;
        float magnitudes[kFFTSize / 2];
        for (int i = 0; i < numBins; i++)
        {
            float re = fftData[i * 2];
            float im = fftData[i * 2 + 1];
            float mag = sqrtf(re * re + im * im) / numBins;
            magnitudes[i] = 20.f * log10f(std::max(mag, 1e-7f));
        }

        // Display range
        static constexpr float kMinDb = -90.f;
        static constexpr float kMaxDb = 0.f;
        static constexpr float kDbRange = kMaxDb - kMinDb;

        float sr = 44100.f;
        if (mProcessor) sr = static_cast<float>(mProcessor->getSampleRate());
        float nyquist = sr * 0.5f;
        float logMin = log10f(20.f);
        float logMax = log10f(nyquist);
        float logRange = logMax - logMin;

        // Grid: vertical lines at decades + log tick marks at bottom
        g.setColour(dim);
        for (int decade = 1; decade <= 4; decade++)
        {
            for (int m = 1; m <= 9; m++)
            {
                float freq = m * powf(10.f, static_cast<float>(decade));
                if (freq < 20.f || freq > nyquist) continue;
                float xf = std::round((log10f(freq) - logMin) / logRange * (w - 1));
                if (xf < 0 || xf >= w) continue;
                if (m == 1)
                    g.fillRect(xf, 0.f, 1.f, static_cast<float>(h)); // full line at decades
                else
                    g.fillRect(xf, static_cast<float>(h) - kTickH, kTickW, kTickH); // tick mark
            }
        }

        // Grid: horizontal lines every 18 dB (skip 0dB top line)
        for (float db = -18.f; db > kMinDb; db -= 18.f)
        {
            if (db > kMaxDb) continue;
            float yf = (1.f - (db - kMinDb) / kDbRange) * (h - 1);
            g.setColour(dim);
            g.fillRect(0.f, std::round(yf), static_cast<float>(w), 1.f);
        }

        if (available < 64) return; // not enough data

        // Draw spectrum as thin path (sub-pixel resolution)
        auto specYf = [&](float px) -> float {
            float logF = logMin + px / (w - 1) * logRange;
            float freq = powf(10.f, logF);
            float binF = freq / nyquist * numBins;
            int b0 = std::clamp(static_cast<int>(binF), 0, numBins - 2);
            float frac = binF - b0;
            float db = magnitudes[b0] + frac * (magnitudes[b0 + 1] - magnitudes[b0]);
            db = std::clamp(db, kMinDb, kMaxDb);
            return (1.f - (db - kMinDb) / kDbRange) * (h - 1);
        };

        juce::Path specPath;
        float step = 0.5f; // half-pixel steps for smoother curves
        specPath.startNewSubPath(0.f, specYf(0.f));
        for (float px = step; px < w; px += step)
            specPath.lineTo(px, specYf(px));

        g.setColour(bright);
        g.strokePath(specPath, juce::PathStrokeType(0.75f));
    }

    // ---- ADSR envelope display (split view) ----
    // Top half: ADS (attack + decay + sustain fill)
    // Bottom half: SR (sustain fill + release curve)
    // Both halves use a fixed 15s time window.
    void paintADSR(juce::Graphics& g, int w, int h,
                   juce::Colour dim, juce::Colour mid, juce::Colour bright)
    {
        if (!mProcessor) return;

        using DSP = KR106DSP<float>;
        auto& dsp = mProcessor->mDSP;

        // --- Read ADSR parameters ---
        bool j6 = (dsp.mAdsrMode == 0);
        float sustain = std::max(mProcessor->getParam(kEnvS)->getValue(), 0.001f);

        // Constants matching KR106ADSR.h
        static constexpr float kAttackTarget    = 1.2f;
        static constexpr float kReleaseTarget   = -0.1f;
        static constexpr float kWindowMs        = 6000.f;

        using ADSR = kr106::ADSR;

        // Compute times and coefficients per mode
        float attackMs;
        float attackCoeff = 0.f, decayCoeff = 0.f, releaseCoeff = 0.f;

        if (j6)
        {
            // Juno-6: tau in seconds (must match KR106_DSP_SetParam.h)
            float attackTau  = 0.001500f * std::exp(11.7382f * dsp.mSliderA + -4.7207f * dsp.mSliderA * dsp.mSliderA);
            float decayTau   = 0.003577f * std::exp(12.9460f * dsp.mSliderD + -5.0638f * dsp.mSliderD * dsp.mSliderD);
            float releaseTau = 0.003577f * std::exp(12.9460f * dsp.mSliderR + -5.0638f * dsp.mSliderR * dsp.mSliderR);

            // Per-ms coefficients (matching ADSR per-sample coeff but at 1kHz)
            attackCoeff  = 1.f - expf(-0.001f / attackTau);
            decayCoeff   = 1.f - expf(-0.001f / decayTau);
            releaseCoeff = 1.f - expf(-0.001f / releaseTau);

            // Approximate completion times for phase boundaries
            attackMs = -logf(1.f - 1.f / kAttackTarget) / -logf(1.f - attackCoeff);
        }
        else
        {
            // Juno-106: times from ROM integer simulation
            attackMs  = ADSR::AttackMs(dsp.mSliderA);
        }

        // --- Layout ---
        int hTop = h / 2;
        int hBot = h - hTop - 1; // 1px divider
        int yDivider = hTop;
        int yBotStart = hTop + 1;

        // Horizontal divider line with tick marks
        g.setColour(dim);
        g.fillRect(0.f, static_cast<float>(yDivider), static_cast<float>(w), 1.f);
        int halfSecs = static_cast<int>(kWindowMs / 500.f);
        for (int hs = 1; hs < halfSecs; hs++)
        {
            float tx = std::round(hs * 500.f / kWindowMs * (w - 1));
            bool major = (hs % 2 == 0); // whole seconds are major
            float tickH = major ? 5.f : 3.f;
            g.fillRect(tx, static_cast<float>(yDivider) - (tickH - 1.f) / 2.f, 1.f, tickH);
        }

        // Phase boundary: A|D (top half)
        float xAD = std::round(std::min(attackMs / kWindowMs, 1.f) * (w - 1));
        g.setColour(dim);
        if (xAD > 0.f && xAD < w - 1)
            g.fillRect(xAD, 0.f, 1.f, static_cast<float>(hTop));

        // --- Draw envelope curves ---
        if (j6)
        {
            // J6: analytical per-ms evaluation using RC coefficients
            auto evalEnv = [&](float ms, int phase) -> float
            {
                float env = 0.f;
                if (phase == 0)
                {
                    env = kAttackTarget * (1.f - powf(1.f - attackCoeff, ms));
                    if (env > 1.f) env = 1.f;
                }
                else if (phase == 1)
                    env = sustain + (1.f - sustain) * powf(1.f - decayCoeff, ms);
                else
                {
                    env = kReleaseTarget + (sustain - kReleaseTarget) * powf(1.f - releaseCoeff, ms);
                    if (env < 0.f) env = 0.f;
                }
                return std::clamp(env, 0.f, 1.f);
            };

            // Top half: attack + decay
            g.setColour(bright);
            int lastY = hTop - 1;
            for (int px = 0; px < w; px++)
            {
                float ms = (static_cast<float>(px) / static_cast<float>(w - 1)) * kWindowMs;
                float env = (ms < attackMs) ? evalEnv(ms, 0) : evalEnv(ms - attackMs, 1);
                int y = static_cast<int>(std::round((1.f - env) * (hTop - 1)));
                int y1 = std::min(lastY, y), y2 = std::max(lastY, y) + 1;
                g.fillRect(static_cast<float>(px), static_cast<float>(y1),
                           1.f, static_cast<float>(y2 - y1));
                lastY = y;
            }

            // Bottom half: release starting from left edge
            lastY = static_cast<int>(std::round((1.f - sustain) * (hBot - 1)));
            for (int px = 0; px < w; px++)
            {
                float ms = (static_cast<float>(px) / static_cast<float>(w - 1)) * kWindowMs;
                float env = evalEnv(ms, 2);
                int y = static_cast<int>(std::round((1.f - env) * (hBot - 1)));
                int y1 = std::min(lastY, y), y2 = std::max(lastY, y) + 1;
                g.fillRect(static_cast<float>(px), static_cast<float>(y1 + yBotStart),
                           1.f, static_cast<float>(y2 - y1));
                lastY = y;
            }
        }
        else
        {
            // 106: integer simulation at tick rate, interpolated to pixels
            int decIdx = std::clamp(static_cast<int>(dsp.mSliderD * 127.f + 0.5f), 0, 127);
            int relIdx = std::clamp(static_cast<int>(dsp.mSliderR * 127.f + 0.5f), 0, 127);

            uint16_t atkInc  = ADSR::AttackIncFromSlider(dsp.mSliderA);
            uint16_t decCoeff = kr106::kDecRelTable[decIdx];
            uint16_t relCoeff = kr106::kDecRelTable[relIdx];
            uint16_t susI = static_cast<uint16_t>(sustain * ADSR::kEnvMax);

            static constexpr int kMaxTicks = 1429; // 6s / 4.2ms
            float topCurve[kMaxTicks + 1];
            float botCurve[kMaxTicks + 1];

            // Simulate attack + decay (top half)
            {
                uint16_t envI = 0;
                bool attacking = true;
                for (int t = 0; t <= kMaxTicks; t++)
                {
                    topCurve[t] = static_cast<float>(envI) / ADSR::kEnvMax;
                    if (attacking)
                    {
                        uint32_t sum = static_cast<uint32_t>(envI) + atkInc;
                        if (sum >= ADSR::kEnvMax) { envI = ADSR::kEnvMax; attacking = false; }
                        else envI = static_cast<uint16_t>(sum);
                    }
                    else
                    {
                        if (envI > susI)
                        {
                            uint16_t diff = envI - susI;
                            diff = ADSR::CalcDecay(diff, decCoeff);
                            envI = diff + susI;
                        }
                        else
                            envI = susI;
                    }
                }
            }

            // Simulate release from sustain (bottom half)
            {
                uint16_t envI = susI;
                for (int t = 0; t <= kMaxTicks; t++)
                {
                    botCurve[t] = static_cast<float>(envI) / ADSR::kEnvMax;
                    envI = ADSR::CalcDecay(envI, relCoeff);
                }
            }

            // Draw top half using simulated curve
            g.setColour(bright);
            int lastY = hTop - 1;
            for (int px = 0; px < w; px++)
            {
                float ms = (static_cast<float>(px) / static_cast<float>(w - 1)) * kWindowMs;
                float tickF = ms * ADSR::kTickRate / 1000.f;
                int t0 = std::min(static_cast<int>(tickF), kMaxTicks - 1);
                float frac = tickF - t0;
                float env = topCurve[t0] + (topCurve[t0 + 1] - topCurve[t0]) * frac;
                env = std::clamp(env, 0.f, 1.f);

                int y = static_cast<int>(std::round((1.f - env) * (hTop - 1)));
                int y1 = std::min(lastY, y), y2 = std::max(lastY, y) + 1;
                g.fillRect(static_cast<float>(px), static_cast<float>(y1),
                           1.f, static_cast<float>(y2 - y1));
                lastY = y;
            }

            // Draw bottom half: release starting from left edge
            lastY = static_cast<int>(std::round((1.f - sustain) * (hBot - 1)));
            for (int px = 0; px < w; px++)
            {
                float ms = (static_cast<float>(px) / static_cast<float>(w - 1)) * kWindowMs;
                float tickF = ms * ADSR::kTickRate / 1000.f;
                int t0 = std::min(static_cast<int>(tickF), kMaxTicks - 1);
                float frac = tickF - t0;
                float env = botCurve[t0] + (botCurve[t0 + 1] - botCurve[t0]) * frac;
                env = std::clamp(env, 0.f, 1.f);

                int y = static_cast<int>(std::round((1.f - env) * (hBot - 1)));
                int y1 = std::min(lastY, y), y2 = std::max(lastY, y) + 1;
                g.fillRect(static_cast<float>(px), static_cast<float>(y1 + yBotStart),
                           1.f, static_cast<float>(y2 - y1));
                lastY = y;
            }
        }
    }

    // ---- VCF frequency response display ----
    // Analytically evaluates the 4-pole cascade + feedback transfer function:
    //   |H(f)|² = 1 / ((1+x²)⁴ + 2k·(1+x²)²·cos(4·atan(x)) + k²)
    // where x = f/fc, fc = individual pole frequency (= self-oscillation freq).
    void paintVCF(juce::Graphics& g, int w, int h,
                  juce::Colour dim, juce::Colour mid, juce::Colour bright)
    {
        if (!mProcessor) return;

        auto& dsp = mProcessor->mDSP;
        bool j106 = (dsp.mAdsrMode != 0);

        float slider = dsp.mSliderVcfFreq;
        float fc;
        if (j106)
        {
            uint16_t cutoffInt = static_cast<uint16_t>(slider * 0x3F80);
            fc = kr106::dacToHz(cutoffInt);
        }
        else
            fc = kr106::j6_vcf_freq_from_slider(slider);

        float res = mProcessor->getParam(kVcfRes)->getValue();
        float k = j106 ? kr106::VCF::ResK_J106(res) : kr106::VCF::ResK_J6(res);
        k = kr106::VCF::SoftClipK(k);

        float fcSlider = fc;
        fc *= kr106::VCF::FreqCompensation(k);
        float comp = kr106::VCF::InputComp(k);

        // Display range
        static constexpr float kMinHz = 5.f;
        static constexpr float kMaxHz = 50000.f;
        static constexpr float kMinDb = -48.f;
        static constexpr float kMaxDb = 24.f;
        static constexpr float kDbRange = kMaxDb - kMinDb;
        float logMin = log10f(kMinHz);
        float logMax = log10f(kMaxHz);
        float logRange = logMax - logMin;

        // Grid: vertical lines at decades + log tick marks at bottom
        g.setColour(dim);
        for (int decade = 0; decade <= 4; decade++)
        {
            for (int m = 1; m <= 9; m++)
            {
                float freq = m * powf(10.f, static_cast<float>(decade));
                if (freq < kMinHz || freq > kMaxHz) continue;
                float xf = std::round((log10f(freq) - logMin) / logRange * (w - 1));
                if (xf < 0 || xf >= w) continue;
                if (m == 1)
                    g.fillRect(xf, 0.f, 1.f, static_cast<float>(h));
                else
                    g.fillRect(xf, static_cast<float>(h) - kTickH, kTickW, kTickH);
            }
        }

        // Grid: horizontal lines every 12 dB
        for (float db = kMinDb + 12.f; db < kMaxDb; db += 12.f)
        {
            float yf = (1.f - (db - kMinDb) / kDbRange) * (h - 1);
            g.setColour(db == 0.f ? mid : dim);
            g.fillRect(0.f, std::round(yf), static_cast<float>(w), 1.f);
        }

        // Cutoff frequency marker
        if (fcSlider >= kMinHz && fcSlider <= kMaxHz)
        {
            g.setColour(mid);
            float xfc = (log10f(fcSlider) - logMin) / logRange * (w - 1);
            g.fillRect(std::round(xfc), 0.f, 1.f, static_cast<float>(h));
        }

        // Evaluate and draw magnitude response (normalized to 0 dB at DC)
        // The transfer function for a 4-pole cascade with feedback k and
        // input compensation comp is:
        //   H(s) = comp / ((1 + s/wc)^4 + k)
        // In normalized frequency (x = f/fc):
        //   |H|² = comp² / ((1+x²)⁴ + 2k(1+x²)²cos(4·atan(x)) + k²)
        float k2 = k * k;
        float comp2 = comp * comp;

        auto evalDb = [&](float px) -> float {
            float logF = logMin + px / (w - 1) * logRange;
            float freq = powf(10.f, logF);
            float x = freq / fc;
            float x2 = x * x;
            float p2 = 1.f + x2;
            float p4 = p2 * p2;
            float p8 = p4 * p4;
            float theta4 = 4.f * atanf(x);
            float denomSq = p8 + 2.f * k * p4 * cosf(theta4) + k2;
            float magSq = comp2 / denomSq;
            float db = 10.f * log10f(std::max(magSq, 1e-12f));
            return std::clamp(db, kMinDb, kMaxDb);
        };

        auto dbToY = [&](float db) -> float {
            return (1.f - (db - kMinDb) / kDbRange) * (h - 1);
        };

        juce::Path vcfPath;
        float step = 0.5f;
        vcfPath.startNewSubPath(0.f, dbToY(evalDb(0.f)));
        for (float px = step; px < w; px += step)
            vcfPath.lineTo(px, dbToY(evalDb(px)));

        g.setColour(bright);
        g.strokePath(vcfPath, juce::PathStrokeType(0.75f));
    }

    // ---- Frequency counter (zero-crossing measurement) ----
    void paintFreqCounter(juce::Graphics& g, int w, int h,
                          juce::Colour dim, juce::Colour /*mid*/, juce::Colour bright)
    {
        if (!mProcessor || mSamplesAvail < 2) return;

        float sr = static_cast<float>(mProcessor->getSampleRate());
        if (sr <= 0.f) return;

        // Scan for positive-going zero crossings.
        // Accumulate total elapsed samples and cycle count over a measurement
        // window, then compute frequency from the aggregate.
        for (int i = 1; i < mSamplesAvail; i++)
        {
            int idx0 = (mRingWritePos - mSamplesAvail + i - 1 + RING_SIZE) % RING_SIZE;
            int idx1 = (idx0 + 1) % RING_SIZE;
            float s0 = mRing[idx0];
            float s1 = mRing[idx1];

            mFreqWindowSamples++;

            if (s0 <= 0.f && s1 > 0.f)
            {
                float frac = -s0 / (s1 - s0);

                if (mHadPriorCross)
                {
                    mFreqCycles++;
                    if (mFreqCycles == 1)
                        mFreqWindowStart = mFreqWindowSamples - 1.f + frac;
                    mFreqWindowEnd = mFreqWindowSamples - 1.f + frac;
                }

                mHadPriorCross = true;
            }
        }

        // Update frequency estimate when we have enough cycles
        if (mFreqCycles >= kFreqMinCycles && mFreqWindowEnd > mFreqWindowStart)
        {
            float totalSamples = mFreqWindowEnd - mFreqWindowStart;
            mFreqSmooth = sr * mFreqCycles / totalSamples;
            mFreqCycles = 0;
            mFreqWindowSamples = 0;
            mFreqWindowStart = 0.f;
            mFreqWindowEnd = 0.f;
        }
        // Reset if window gets too long without enough crossings (silence)
        else if (mFreqWindowSamples > sr)
        {
            mFreqSmooth = 0.f;
            mFreqCycles = 0;
            mFreqWindowSamples = 0;
            mFreqWindowStart = 0.f;
            mFreqWindowEnd = 0.f;
            mHadPriorCross = false;
        }

        // Format display
        juce::String line1, line2;
        if (mFreqSmooth > 1.f)
        {
            line1 = juce::String(mFreqSmooth, 1) + " Hz";

            // MIDI note + cents
            float midiNote = 69.f + 12.f * log2f(mFreqSmooth / 440.f);
            int noteInt = juce::roundToInt(midiNote);
            float cents = (midiNote - noteInt) * 100.f;
            static const char* noteNames[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
            int noteName = ((noteInt % 12) + 12) % 12;
            int octave = (noteInt / 12) - 1;
            char centsStr[16];
            snprintf(centsStr, sizeof(centsStr), "%+.0f", cents);
            line2 = juce::String(noteNames[noteName]) + juce::String(octave) + " " + juce::String(centsStr) + "c";
        }
        else
        {
            line1 = "---";
            line2 = "";
        }

        // Draw centered text
        auto font = juce::Font(juce::FontOptions()
            .withMetricsKind(juce::TypefaceMetricsKind::legacy)).withHeight(10.f);
        g.setFont(font);
        g.setColour(bright);
        g.drawText(line1, 0, h / 2 - 14, w, 14, juce::Justification::centred);
        g.drawText(line2, 0, h / 2 + 2, w, 14, juce::Justification::centred);
    }

    // ---- Build rasterized path for about screen (done once per resize) ----
    void buildAboutPath(int w, int h)
    {
        if (w == mAboutW && h == mAboutH && !mAboutPath.empty()) return;
        mAboutW = w;
        mAboutH = h;
        mAboutPath.clear();

        std::vector<bool> visited(w * h, false);

        auto addPixel = [&](int px, int py) {
            if (px >= 0 && px < w && py >= 0 && py < h && !visited[py * w + px]) {
                visited[py * w + px] = true;
                mAboutPath.push_back({static_cast<int16_t>(px), static_cast<int16_t>(py)});
            }
        };

        auto rasterLine = [&](float x0f, float y0f, float x1f, float y1f) {
            int x0 = static_cast<int>(std::round(x0f)), y0 = static_cast<int>(std::round(y0f));
            int x1 = static_cast<int>(std::round(x1f)), y1 = static_cast<int>(std::round(y1f));
            int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
            int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
            int err = dx - dy;
            while (true) {
                addPixel(x0, y0);
                if (x0 == x1 && y0 == y1) break;
                int e2 = 2 * err;
                if (e2 > -dy) { err -= dy; x0 += sx; }
                if (e2 < dx) { err += dx; y0 += sy; }
            }
        };

        // Vector stroke font — 4×6 grid per glyph, {-1,-1} = pen-up
        struct P { int8_t x, y; };
        static constexpr P PU {-1, -1};
        static constexpr P cA[] = {{0,6},{0,0},{4,0},{4,6},PU,{0,3},{4,3}};
        static constexpr P cE[] = {{4,0},{0,0},{0,6},{4,6},PU,{0,3},{3,3}};
        static constexpr P cK[] = {{0,0},{0,6},PU,{4,0},{0,3},{4,6}};
        static constexpr P cL[] = {{0,0},{0,6},{4,6}};
        static constexpr P cM[] = {{0,6},{0,0},{2,3},{4,0},{4,6}};
        static constexpr P cR[] = {{0,6},{0,0},{4,0},{4,3},{0,3},PU,{0,3},{4,6}};
        static constexpr P cS[] = {{4,0},{0,0},{0,3},{4,3},{4,6},{0,6}};
        static constexpr P cT[] = {{0,0},{4,0},PU,{2,0},{2,6}};
        static constexpr P cU[] = {{0,0},{0,6},{4,6},{4,0}};
        static constexpr P cV[] = {{0,0},{2,6},{4,0}};
        static constexpr P c0[] = {{0,0},{4,0},{4,6},{0,6},{0,0}};
        static constexpr P c1[] = {{1,1},{2,0},{2,6}};
        static constexpr P c2[] = {{0,0},{4,0},{4,3},{0,3},{0,6},{4,6}};
        static constexpr P c3[] = {{0,0},{4,0},{4,3},{1,3},PU,{4,3},{4,6},{0,6}};
        static constexpr P c4[] = {{0,0},{0,3},{4,3},PU,{4,0},{4,6}};
        static constexpr P c5[] = {{4,0},{0,0},{0,3},{4,3},{4,6},{0,6}};
        static constexpr P c6[] = {{4,0},{0,0},{0,6},{4,6},{4,3},{0,3}};
        static constexpr P c7[] = {{0,0},{4,0},{2,6}};
        static constexpr P c8[] = {{0,0},{4,0},{4,6},{0,6},{0,0},PU,{0,3},{4,3}};
        static constexpr P c9[] = {{0,6},{4,6},{4,0},{0,0},{0,3},{4,3}};
        static constexpr P cDash[] = {{1,3},{3,3}};
        static constexpr P cDot[]  = {{2,5},{2,6}};

        struct Glyph { char ch; const P* pts; int n; };
        const Glyph glyphs[] = {
            {'A',cA,7}, {'E',cE,7}, {'K',cK,6}, {'L',cL,3}, {'M',cM,5},
            {'R',cR,8}, {'S',cS,6}, {'T',cT,5}, {'U',cU,4}, {'V',cV,3},
            {'0',c0,5}, {'1',c1,3}, {'2',c2,6}, {'3',c3,8}, {'4',c4,6},
            {'5',c5,6}, {'6',c6,6}, {'7',c7,3}, {'8',c8,8}, {'9',c9,6},
            {'-',cDash,2}, {'.',cDot,2},
        };

        auto traceStr = [&](const char* str, float cx, float cy, float glyphH) {
            float glyphW = glyphH * (4.f / 6.f);
            float space  = glyphW * 1.5f;
            int len = static_cast<int>(strlen(str));
            float x0 = cx - (len * space - (space - glyphW)) * 0.5f;
            for (int i = 0; i < len; i++) {
                if (str[i] == ' ') continue;
                const Glyph* gl = nullptr;
                for (auto& gg : glyphs)
                    if (gg.ch == str[i]) { gl = &gg; break; }
                if (!gl) continue;
                float ox = x0 + i * space;
                float oy = cy;
                float sx = glyphW / 4.f, sy = glyphH / 6.f;
                for (int j = 1; j < gl->n; j++) {
                    if (gl->pts[j].x < 0 || gl->pts[j-1].x < 0) continue;
                    rasterLine(ox + gl->pts[j-1].x * sx, oy + gl->pts[j-1].y * sy,
                              ox + gl->pts[j].x * sx,   oy + gl->pts[j].y * sy);
                }
            }
        };

        float cx = w * 0.5f;
        float titleH = h * 0.15f;
        traceStr("ULTRAMASTER", cx, h * 0.15f, titleH);
        traceStr("KR-106", cx, h * 0.40f, titleH);
        juce::String ver = juce::String("V") + JucePlugin_VersionString;
        traceStr(ver.toRawUTF8(), cx, h * 0.70f, h * 0.10f);
    }

    // ---- Patch bank (128 rectangles in 16×8 grid) ----
    void paintPatchBank(juce::Graphics& g, int w, int h,
                        juce::Colour dim, juce::Colour mid, juce::Colour bright)
    {
        if (!mProcessor) return;

        int cols = 16, rows = 8;
        int cellW = w / cols;
        int cellH = h / rows;
        int num = mProcessor->getNumPrograms();
        int cur = mProcessor->getCurrentProgram();

        // Draw grid lines (shifted 0.5px right, 1px down to center in gaps)
        auto gridCol = juce::Colour(0, 64, 0);
        g.setColour(gridCol);
        for (int c = 1; c < cols; c++)
            g.fillRect(c * cellW - 0.5f, 1.f, 1.f, static_cast<float>(rows * cellH));
        for (int r = 1; r <= rows; r++)
            g.fillRect(0.f, static_cast<float>(r * cellH), static_cast<float>(cols * cellW), 1.f);

        // Fill selected and hovered cells
        for (int r = 0; r < rows; r++)
        {
            for (int c = 0; c < cols; c++)
            {
                int idx = r * cols + c;
                if (idx >= num) break;

                int x = c * cellW;
                int y = r * cellH;

                if (idx == cur)
                {
                    g.setColour(bright);
                    g.fillRect(x, y, cellW - 1, cellH - 1);
                }
            }
        }

        // Draw drag vector as filled cells (Bresenham on 16×8 grid)
        if (mDragging)
        {
            int c0 = mDragOriginX / cellW, r0 = mDragOriginY / cellH;
            int c1 = std::clamp(mDragEndX / cellW, 0, cols - 1);
            int r1 = std::clamp(mDragEndY / cellH, 0, rows - 1);
            int dx = std::abs(c1 - c0), dy = -std::abs(r1 - r0);
            int sx = c0 < c1 ? 1 : -1, sy = r0 < r1 ? 1 : -1;
            int err = dx + dy;
            g.setColour(mid);
            for (;;)
            {
                int idx = r0 * cols + c0;
                if (idx != cur) // don't overdraw the selected cell
                    g.fillRect(c0 * cellW, r0 * cellH, cellW - 1, cellH - 1);
                if (c0 == c1 && r0 == r1) break;
                int e2 = 2 * err;
                if (e2 >= dy) { err += dy; c0 += sx; }
                if (e2 <= dx) { err += dx; r0 += sy; }
            }
        }
    }

    // ---- About / version display (oscilloscope beam trace with phosphor decay) ----
    void paintAbout(juce::Graphics& g, int w, int h,
                    juce::Colour dim, juce::Colour /*mid*/, juce::Colour /*bright*/)
    {
        buildAboutPath(w, h);

        paintCrosshairs(g, w, h, dim);

        int totalPx = static_cast<int>(mAboutPath.size());
        if (totalPx == 0) return;

        // Start timer on first paint after entering about mode
        float now = juce::Time::getMillisecondCounter() * 0.001f;
        if (!mAboutActive) { mAboutStartTime = now; mAboutActive = true; }
        float t = now - mAboutStartTime;

        // Continuous loop: beam wraps, no clearing. All pixels always drawn.
        // Brightness based on distance behind the beam (wrapping around).
        static constexpr float kDrawSec = 3.0f;
        float phase = std::fmod(t, kDrawSec);
        int beamIdx = static_cast<int>(phase / kDrawSec * totalPx);

        for (int i = 0; i < totalPx; i++)
        {
            // Wrapped distance behind the beam head
            int dist = (beamIdx - i + totalPx) % totalPx;

            // Fade from 1.0 at beam to 0.1 at max distance (opposite side)
            float glow = 1.0f - 0.9f * static_cast<float>(dist) / static_cast<float>(totalPx);

            g.setColour(juce::Colour(static_cast<uint8_t>(0),
                                     static_cast<uint8_t>(glow * 255),
                                     static_cast<uint8_t>(0)));
            g.fillRect(static_cast<float>(mAboutPath[i].x),
                       static_cast<float>(mAboutPath[i].y), 1.f, 1.f);
        }

        // Beam dot: pure white center, grey bloom
        {
            int bx = mAboutPath[beamIdx % totalPx].x;
            int by = mAboutPath[beamIdx % totalPx].y;
            g.setColour(juce::Colour(static_cast<uint8_t>(128),
                                     static_cast<uint8_t>(128),
                                     static_cast<uint8_t>(128)));
            g.fillRect(static_cast<float>(bx - 1), static_cast<float>(by - 1), 3.f, 3.f);
            g.setColour(juce::Colour(static_cast<uint8_t>(255),
                                     static_cast<uint8_t>(255),
                                     static_cast<uint8_t>(255)));
            g.fillRect(static_cast<float>(bx), static_cast<float>(by), 1.f, 1.f);
        }
    }

    static constexpr int kNumModes = 6; // 0: waveform, 1: spectrum, 2: ADSR, 3: VCF, 4: patch bank, 5: about
    int mScaleIdx = 0;
    int mPrePatchBankMode = 0; // mode to return to when toggling patch bank off
    int mPatchBankHover = -1;  // hovered patch index, or -1

    // Bouncing ball state
    bool  mBallActive = false;
    int   mBallCellX = 0, mBallCellY = 0; // current cell position
    float mBallDX = 0.f, mBallDY = 0.f;   // direction (for Bresenham + bounce)
    float mBallSpeed = 0.f;                // cells per tick
    float mBallAccum = 0.f;                // fractional step accumulator
    float mBallErrX = 0.f, mBallErrY = 0.f; // Bresenham error terms

    // Drag-to-launch state
    bool mDragging = false;
    int  mDragOriginX = 0, mDragOriginY = 0; // center of clicked cell (pixels)
    int  mDragEndX = 0, mDragEndY = 0;       // current mouse position (pixels)

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

    // Clip indicator hold (bright green for ~1 second after clipping)
    int mClipHoldFrames = 0;

    // About screen beam trace state
    struct AboutPixel { int16_t x, y; };
    std::vector<AboutPixel> mAboutPath;
    int mAboutW = 0, mAboutH = 0;
    float mAboutStartTime = 0.f;
    bool mAboutActive = false;

    // Frequency counter state — accumulates cycles over a window
    static constexpr int kFreqMinCycles = 16;
    float mFreqSmooth = 0.f;
    int mFreqCycles = 0;
    int mFreqWindowSamples = 0;
    float mFreqWindowStart = 0.f;
    float mFreqWindowEnd = 0.f;
    bool mHadPriorCross = false;

    // Cached param snapshot for ADSR/VCF modes — only repaint when values change
    uint64_t mParamHash = 0;

    void repaintIfParamsChanged()
    {
        if (!mProcessor) return;
        auto& dsp = mProcessor->mDSP;

        // Pack relevant floats into a simple hash via bit representation
        auto fbits = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };

        uint64_t h = 0;
        if (mScaleIdx == 2) // ADSR
        {
            h = fbits(dsp.mSliderA) ^ (static_cast<uint64_t>(fbits(dsp.mSliderD)) << 16)
              ^ (static_cast<uint64_t>(fbits(dsp.mSliderR)) << 32)
              ^ (static_cast<uint64_t>(fbits(mProcessor->getParam(kEnvS)->getValue())) << 8)
              ^ static_cast<uint64_t>(dsp.mAdsrMode);
        }
        else // VCF (mScaleIdx == 3)
        {
            h = fbits(dsp.mSliderVcfFreq)
              ^ (static_cast<uint64_t>(fbits(mProcessor->getParam(kVcfRes)->getValue())) << 32)
              ^ static_cast<uint64_t>(dsp.mAdsrMode);
        }

        if (h != mParamHash)
        {
            mParamHash = h;
            repaint();
        }
    }
};
