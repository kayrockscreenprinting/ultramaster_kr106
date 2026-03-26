// Quick VCF self-oscillation pitch/amplitude test
// Generates a CSV summary at 10 frequency points.
//
// Usage: vcf_quick [samplerate]
//   Default: 44100
//
// Output (stdout): CSV with columns:
//   frq, target_hz, zc_hz, error_cents, amp_db, osc_r1 (yes/no), osc_r08 (yes/no)

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <vector>

#include "../../Source/DSP/KR106VCF.h"

struct OscResult {
    bool oscillating;
    double frequency;
    double amplitude;
};

static OscResult testOsc(float frq, float res, float sr)
{
    kr106::VCF vcf;
    vcf.SetSampleRate(sr);
    vcf.Reset();

    int settle = static_cast<int>(sr * 1.0f);
    int measure = static_cast<int>(sr * 1.0f);

    for (int i = 0; i < settle; i++)
        vcf.Process(0.f, frq, res);

    std::vector<float> buf(measure);
    for (int i = 0; i < measure; i++)
        buf[i] = vcf.Process(0.f, frq, res);

    float peak = 0.f;
    for (float s : buf)
        peak = std::max(peak, fabsf(s));

    OscResult r;
    r.amplitude = peak;
    r.oscillating = (peak >= 0.01f);

    if (!r.oscillating) { r.frequency = 0; return r; }

    int crossings = 0;
    for (int i = 1; i < measure; i++)
        if (buf[i - 1] <= 0.f && buf[i] > 0.f)
            crossings++;

    r.frequency = static_cast<double>(crossings) * sr / measure;
    return r;
}

int main(int argc, char* argv[])
{
    float res = (argc > 1) ? static_cast<float>(atof(argv[1])) : 1.0f;
    float sr  = (argc > 2) ? static_cast<float>(atof(argv[2])) : 44100.f;

    // Octaves from 55 Hz: 55, 110, 220, 440, 880, 1760, 3520, 7040, 14080
    float frqBuf[9];
    float hz = 55.f;
    for (int i = 0; i < 9; i++) {
        frqBuf[i] = hz / (sr * 0.5f);
        hz *= 2.f;
    }
    const float* kFrqs = frqBuf;
    static constexpr int kN = 9;

    printf("frq,target_hz,zc_hz,error_cents,amp_db,osc\n");

    for (int i = 0; i < kN; i++)
    {
        float frq = kFrqs[i];
        float target = frq * sr * 0.5f;

        OscResult r = testOsc(frq, res, sr);

        double cents = 0;
        if (r.oscillating && r.frequency > 0)
            cents = 1200.0 * log2(r.frequency / target);

        double ampDb = r.amplitude > 1e-10 ? 20.0 * log10(r.amplitude) : -200.0;

        printf("%.4f,%.1f,%.1f,%+.1f,%.1f,%s\n",
               frq, target, r.frequency, cents, ampDb,
               r.oscillating ? "yes" : "NO");
    }

    return 0;
}
