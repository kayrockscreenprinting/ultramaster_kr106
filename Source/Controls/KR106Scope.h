#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

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
        else if (mScaleIdx == 3)
            paintADSR(g, w, h, dim, mid, bright);
        else
            paintVCF(g, w, h, dim, mid, bright);
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        mScaleIdx = (mScaleIdx + 1) % 5; // 0-2: waveform scales, 3: ADSR, 4: VCF
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
            // ADSR/VCF modes read slider params directly — repaint even with no audio
            if (mScaleIdx >= 3) repaint();
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

        // Compute cutoff frequency (Hz) from slider — same logic as SetParam
        float slider = dsp.mSliderVcfFreq;
        float fc;
        if (j106)
        {
            uint16_t cutoffInt = static_cast<uint16_t>(slider * 0x3F80);
            fc = kr106::dacToHz(cutoffInt);
        }
        else
            fc = kr106::j6_vcf_freq_from_slider(slider);

        // Compute resonance k — uses same static functions as VCF::ProcessSample
        float res = mProcessor->getParam(kVcfRes)->getValue();
        float k = j106 ? kr106::VCF::ResK_J106(res) : kr106::VCF::ResK_J6(res);

        float fcSlider = fc; // slider frequency for the marker

        // Apply the same pole-frequency compensation as the DSP
        if (j106)
            fc *= kr106::VCF::FreqCompensation(k);

        // Display range
        static constexpr float kMinHz = 20.f;
        static constexpr float kMaxHz = 20000.f;
        static constexpr float kMinDb = -48.f;
        static constexpr float kMaxDb = 24.f;
        static constexpr float kDbRange = kMaxDb - kMinDb; // 72 dB
        float logMin = log10f(kMinHz);
        float logMax = log10f(kMaxHz);
        float logRange = logMax - logMin;

        // Grid: vertical lines at decade frequencies
        g.setColour(dim);
        for (float freq : { 100.f, 1000.f, 10000.f })
        {
            float xf = (log10f(freq) - logMin) / logRange * (w - 1);
            g.fillRect(xf, 0.f, 1.f, static_cast<float>(h));
        }

        // Grid: horizontal lines every 12 dB
        for (float db = kMinDb + 12.f; db < kMaxDb; db += 12.f)
        {
            float yf = (1.f - (db - kMinDb) / kDbRange) * (h - 1);
            g.setColour(db == 0.f ? mid : dim);
            g.fillRect(0.f, std::round(yf), static_cast<float>(w), 1.f);
        }

        // Cutoff frequency marker (fixed at slider position)
        if (fcSlider >= kMinHz && fcSlider <= kMaxHz)
        {
            g.setColour(mid);
            float xfc = (log10f(fcSlider) - logMin) / logRange * (w - 1);
            g.fillRect(std::round(xfc), 0.f, 1.f, static_cast<float>(h));
        }

        // Evaluate and draw magnitude response (normalized to 0 dB at DC)
        float k2 = k * k;
        float dcGainInv = (1.f + k) * (1.f + k); // compensate feedback passband loss
        g.setColour(bright);
        int lastY = h / 2;
        for (int px = 0; px < w; px++)
        {
            float logF = logMin + static_cast<float>(px) / (w - 1) * logRange;
            float freq = powf(10.f, logF);
            float x = freq / fc;
            float x2 = x * x;
            float p2 = 1.f + x2;        // (1 + x²)
            float p4 = p2 * p2;          // (1 + x²)²
            float p8 = p4 * p4;          // (1 + x²)⁴
            float theta4 = 4.f * atanf(x);
            float denomSq = p8 + 2.f * k * p4 * cosf(theta4) + k2;
            float magSq = dcGainInv / denomSq;
            float db = 10.f * log10f(std::max(magSq, 1e-12f));
            db = std::clamp(db, kMinDb, kMaxDb);

            int y = static_cast<int>(std::round((1.f - (db - kMinDb) / kDbRange) * (h - 1)));

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
