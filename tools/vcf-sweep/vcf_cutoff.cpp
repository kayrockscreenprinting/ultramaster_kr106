// VCF cutoff accuracy test — measures actual -3dB frequency vs target
// across a range of cutoff frequencies and resonance settings.
//
// Usage: vcf_cutoff [samplerate]
//   Default: 44100
//
// For each freq/res, sweeps a sine through the filter and measures
// the output amplitude at many test frequencies to find the -3dB point.
//
// Output (stdout): CSV with columns:
//   target_hz, res, actual_hz, error_cents

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>

#include "../../Source/DSP/KR106VCF.h"

// Measure the steady-state amplitude of a sine at testHz through the filter.
// Runs enough cycles for the filter to settle, then measures peak.
static float measureAmplitude(kr106::VCF& vcf, float testHz, float cutoffFrq, float res, float sr)
{
    float phase = 0.f;
    float phaseInc = testHz / sr;

    // Settle: run 200 cycles (or at least 2000 samples)
    int settleSamples = std::max(2000, static_cast<int>(200.f / phaseInc));
    for (int i = 0; i < settleSamples; i++)
    {
        float in = sinf(phase * 2.f * static_cast<float>(M_PI)) * 0.3f;
        vcf.Process(in, cutoffFrq, res);
        phase += phaseInc;
        if (phase >= 1.f) phase -= 1.f;
    }

    // Measure: capture peak over 100 cycles (or at least 1000 samples)
    int measureSamples = std::max(1000, static_cast<int>(100.f / phaseInc));
    float peak = 0.f;
    for (int i = 0; i < measureSamples; i++)
    {
        float in = sinf(phase * 2.f * static_cast<float>(M_PI)) * 0.3f;
        float out = vcf.Process(in, cutoffFrq, res);
        peak = std::max(peak, fabsf(out));
        phase += phaseInc;
        if (phase >= 1.f) phase -= 1.f;
    }

    return peak;
}

static float measureCutoff(float targetHz, float res, float sr)
{
    float cutoffFrq = targetHz / (sr * 0.5f);

    // Measure passband level (at 0.1× cutoff)
    kr106::VCF vcf;
    vcf.SetSampleRate(sr);
    vcf.Reset();

    float refHz = targetHz * 0.1f;
    if (refHz < 10.f) refHz = 10.f;
    float refAmp = measureAmplitude(vcf, refHz, cutoffFrq, res, sr);

    if (refAmp < 1e-8f) return -1.f;

    float target3dBAmp = refAmp * 0.7071f; // -3 dB

    // Binary search for the -3dB frequency between 0.2× and 5× target
    float loHz = targetHz * 0.2f;
    float hiHz = std::min(targetHz * 5.f, sr * 0.45f);

    for (int iter = 0; iter < 30; iter++)
    {
        float midHz = sqrtf(loHz * hiHz); // geometric midpoint

        vcf.Reset();
        // Re-prime resamplers
        for (int i = 0; i < 64; i++)
            vcf.Process(0.f, cutoffFrq, res);

        float amp = measureAmplitude(vcf, midHz, cutoffFrq, res, sr);

        if (amp > target3dBAmp)
            loHz = midHz;
        else
            hiHz = midHz;

        // Converge to ~1 cent
        if (hiHz / loHz < 1.0006f) break;
    }

    return sqrtf(loHz * hiHz);
}

int main(int argc, char* argv[])
{
    float sr = (argc > 1) ? static_cast<float>(atof(argv[1])) : 44100.f;

    // C1 through C11 (10 octaves)
    float freqs[] = {32.7f, 65.4f, 130.8f, 261.6f, 523.3f, 1046.5f, 2093.f, 4186.f, 8372.f, 16744.f};
    int nFreqs = 10;

    float resValues[] = {0.0f, 0.3f, 0.5f, 0.7f, 0.8f, 0.9f};
    int nRes = 6;

    fprintf(stderr, "VCF cutoff accuracy test: sr=%.0f Hz\n", sr);
    fprintf(stderr, "%d frequencies x %d resonance values = %d tests\n\n", nFreqs, nRes, nFreqs * nRes);

    printf("target_hz,res,actual_hz,error_cents\n");

    for (int r = 0; r < nRes; r++)
    {
        for (int f = 0; f < nFreqs; f++)
        {
            float targetHz = freqs[f];
            float res = resValues[r];

            if (targetHz > sr * 0.45f)
            {
                printf("%.1f,%.1f,,,above Nyquist\n", targetHz, res);
                continue;
            }

            float actualHz = measureCutoff(targetHz, res, sr);

            if (actualHz < 0.f)
            {
                printf("%.1f,%.1f,,,-3dB not found\n", targetHz, res);
                fprintf(stderr, "  %7.1f Hz R=%.1f: -3dB not found\n", targetHz, res);
                continue;
            }

            double cents = 1200.0 * log2(actualHz / targetHz);

            printf("%.1f,%.1f,%.1f,%+.1f\n", targetHz, res, actualHz, cents);
            fprintf(stderr, "  %7.1f Hz R=%.1f: actual=%7.1f Hz  error=%+.0f cents\n",
                    targetHz, res, actualHz, cents);
        }
    }

    return 0;
}
