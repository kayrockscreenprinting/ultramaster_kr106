// VCF frequency response analyzer — measures filter characteristics from
// a WAV file containing noise through the VCF at known cutoff/res steps.
//
// Usage: vcf_analyze input.wav [step_ms] [settle_ms]
//   Defaults: 1000ms step, 100ms settle (skipped at start of each step)
//
// Expects the test grid: 9 cutoff frequencies (C1–C9) × 11 resonance
// values (0.0–1.0 in 0.1 steps), with res sweeping at each fixed cutoff.
// Total: 99 steps.
//
// Output (stdout): CSV with columns:
//   target_hz, res, minus3db_hz, passband_db, peak_db, peak_hz,
//   prominence_db, slope_db_oct
//
// Output (stderr): human-readable progress and summary.

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>

// --- WAV reader (mono or stereo, 16/24/32-bit PCM or 32-bit float) ---

struct WavData {
    int sampleRate;
    int channels;
    int numSamples; // per channel
    std::vector<float> data; // interleaved if stereo
};

static bool readWav(const char* filename, WavData& wav)
{
    FILE* f = fopen(filename, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", filename); return false; }

    fseek(f, 0, SEEK_END);
    size_t fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> buf(fileSize);
    if (fread(buf.data(), 1, fileSize, f) != fileSize)
    {
        fprintf(stderr, "Error: failed to read %s\n", filename);
        fclose(f);
        return false;
    }
    fclose(f);

    if (fileSize < 44 || memcmp(buf.data(), "RIFF", 4) != 0 ||
        memcmp(buf.data() + 8, "WAVE", 4) != 0)
    {
        fprintf(stderr, "Error: not a WAV file\n");
        return false;
    }

    // Find fmt and data chunks
    uint16_t audioFmt = 0, channels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0;
    const uint8_t* dataPtr = nullptr;
    uint32_t dataSize = 0;

    size_t pos = 12;
    while (pos + 8 <= fileSize)
    {
        uint32_t chunkSize = *(uint32_t*)(buf.data() + pos + 4);

        if (memcmp(buf.data() + pos, "fmt ", 4) == 0 && chunkSize >= 16)
        {
            audioFmt = *(uint16_t*)(buf.data() + pos + 8);
            channels = *(uint16_t*)(buf.data() + pos + 10);
            sampleRate = *(uint32_t*)(buf.data() + pos + 12);
            bitsPerSample = *(uint16_t*)(buf.data() + pos + 22);
        }
        else if (memcmp(buf.data() + pos, "data", 4) == 0)
        {
            dataPtr = buf.data() + pos + 8;
            dataSize = chunkSize;
        }

        pos += 8 + chunkSize;
        if (chunkSize & 1) pos++; // pad byte
    }

    if (!dataPtr || channels == 0)
    {
        fprintf(stderr, "Error: missing fmt/data chunks\n");
        return false;
    }

    wav.sampleRate = sampleRate;
    wav.channels = channels;

    int bytesPerSample = bitsPerSample / 8;
    int frameSize = bytesPerSample * channels;
    wav.numSamples = dataSize / frameSize;
    wav.data.resize(wav.numSamples * channels);

    for (int i = 0; i < wav.numSamples * channels; i++)
    {
        const uint8_t* p = dataPtr + i * bytesPerSample;

        if (audioFmt == 3 && bitsPerSample == 32) // float
        {
            wav.data[i] = *(float*)p;
        }
        else if (audioFmt == 1 && bitsPerSample == 16)
        {
            int16_t v = *(int16_t*)p;
            wav.data[i] = v / 32768.f;
        }
        else if (audioFmt == 1 && bitsPerSample == 24)
        {
            int32_t v = (p[0]) | (p[1] << 8) | (p[2] << 16);
            if (v & 0x800000) v |= 0xFF000000; // sign extend
            wav.data[i] = v / 8388608.f;
        }
        else if (audioFmt == 1 && bitsPerSample == 32)
        {
            int32_t v = *(int32_t*)p;
            wav.data[i] = v / 2147483648.f;
        }
        else
        {
            fprintf(stderr, "Error: unsupported format (fmt=%d, bits=%d)\n",
                    audioFmt, bitsPerSample);
            return false;
        }
    }

    return true;
}

// --- Radix-2 FFT ---

static void fft(std::vector<float>& re, std::vector<float>& im)
{
    int n = static_cast<int>(re.size());

    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; i++)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
        {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }

    // Cooley-Tukey butterfly
    for (int len = 2; len <= n; len <<= 1)
    {
        float ang = -2.f * static_cast<float>(M_PI) / len;
        float wRe = cosf(ang), wIm = sinf(ang);
        for (int i = 0; i < n; i += len)
        {
            float curRe = 1.f, curIm = 0.f;
            for (int j = 0; j < len / 2; j++)
            {
                int u = i + j;
                int v = i + j + len / 2;
                float tRe = re[v] * curRe - im[v] * curIm;
                float tIm = re[v] * curIm + im[v] * curRe;
                re[v] = re[u] - tRe;
                im[v] = im[u] - tIm;
                re[u] += tRe;
                im[u] += tIm;
                float newCurRe = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = newCurRe;
            }
        }
    }
}

// --- Spectrum analysis for one step ---

struct StepResult {
    float targetHz;
    float res;
    float minus3dbHz;    // -3dB frequency
    float passbandDb;    // average level below cutoff
    float peakDb;        // resonance peak level
    float peakHz;        // frequency of resonance peak
    float prominenceDb;  // peak above passband
    float slopeDbOct;    // stopband rolloff rate
};

static StepResult analyzeStep(const float* samples, int numSamples, int sampleRate,
                               float targetHz, float res)
{
    StepResult r;
    r.targetHz = targetHz;
    r.res = res;

    // Zero-pad to next power of 2 for FFT
    int fftSize = 1;
    while (fftSize < numSamples) fftSize <<= 1;
    if (fftSize < 4096) fftSize = 4096; // minimum resolution

    std::vector<float> re(fftSize, 0.f), im(fftSize, 0.f);

    // Apply Hanning window
    for (int i = 0; i < numSamples; i++)
    {
        float w = 0.5f * (1.f - cosf(2.f * static_cast<float>(M_PI) * i / (numSamples - 1)));
        re[i] = samples[i] * w;
    }

    fft(re, im);

    // Compute magnitude spectrum (dB)
    int specSize = fftSize / 2 + 1;
    std::vector<float> mag(specSize);
    std::vector<float> magDb(specSize);
    float binHz = static_cast<float>(sampleRate) / fftSize;

    for (int i = 0; i < specSize; i++)
    {
        mag[i] = sqrtf(re[i] * re[i] + im[i] * im[i]);
        magDb[i] = 20.f * log10f(mag[i] + 1e-30f);
    }

    // Smooth the spectrum (moving average, ~50 Hz window) for stable measurements
    int smoothBins = std::max(1, static_cast<int>(50.f / binHz));
    std::vector<float> smoothDb(specSize);
    for (int i = 0; i < specSize; i++)
    {
        float sum = 0.f;
        int count = 0;
        for (int j = std::max(0, i - smoothBins); j <= std::min(specSize - 1, i + smoothBins); j++)
        {
            sum += magDb[j];
            count++;
        }
        smoothDb[i] = sum / count;
    }

    // Passband level: average from 0.1× to 0.4× cutoff (well below resonance)
    int pbLo = std::max(1, static_cast<int>(targetHz * 0.1f / binHz));
    int pbHi = std::max(pbLo + 1, static_cast<int>(targetHz * 0.4f / binHz));
    pbHi = std::min(pbHi, specSize - 1);

    float pbSum = 0.f;
    int pbCount = 0;
    for (int i = pbLo; i <= pbHi; i++)
    {
        pbSum += smoothDb[i];
        pbCount++;
    }
    r.passbandDb = (pbCount > 0) ? pbSum / pbCount : -200.f;

    // Resonance peak: maximum in 0.5× to 3× cutoff range
    int peakLo = std::max(1, static_cast<int>(targetHz * 0.5f / binHz));
    int peakHi = std::min(specSize - 1, static_cast<int>(targetHz * 3.f / binHz));

    r.peakDb = -200.f;
    r.peakHz = targetHz;
    for (int i = peakLo; i <= peakHi; i++)
    {
        if (smoothDb[i] > r.peakDb)
        {
            r.peakDb = smoothDb[i];
            r.peakHz = i * binHz;
        }
    }

    r.prominenceDb = r.peakDb - r.passbandDb;

    // -3dB frequency: where the smoothed spectrum drops 3dB below passband
    float threshold = r.passbandDb - 3.f;
    r.minus3dbHz = -1.f;
    int searchStart = std::max(1, static_cast<int>(targetHz * 0.3f / binHz));
    for (int i = searchStart; i < specSize - 1; i++)
    {
        if (smoothDb[i] >= threshold && smoothDb[i + 1] < threshold)
        {
            // Linear interpolation
            float frac = (threshold - smoothDb[i]) / (smoothDb[i + 1] - smoothDb[i]);
            r.minus3dbHz = (i + frac) * binHz;
            break;
        }
    }

    // Stopband slope: linear regression of dB vs log2(freq) from 2× to 8× cutoff
    int slopeLo = std::max(1, static_cast<int>(targetHz * 2.f / binHz));
    int slopeHi = std::min(specSize - 1, static_cast<int>(targetHz * 8.f / binHz));

    if (slopeHi > slopeLo + 2)
    {
        double sumX = 0, sumY = 0, sumXY = 0, sumXX = 0;
        int n = 0;
        for (int i = slopeLo; i <= slopeHi; i++)
        {
            float freq = i * binHz;
            if (freq < 20.f) continue;
            double x = log2(freq);
            double y = smoothDb[i];
            sumX += x;
            sumY += y;
            sumXY += x * y;
            sumXX += x * x;
            n++;
        }
        if (n > 2)
        {
            double denom = n * sumXX - sumX * sumX;
            r.slopeDbOct = (denom > 1e-10) ?
                static_cast<float>((n * sumXY - sumX * sumY) / denom) : 0.f;
        }
        else
        {
            r.slopeDbOct = 0.f;
        }
    }
    else
    {
        r.slopeDbOct = 0.f;
    }

    return r;
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: vcf_analyze input.wav [step_ms] [settle_ms]\n");
        fprintf(stderr, "  Analyzes VCF test grid: 9 cutoffs (C1-C9) x 11 res (0.0-1.0)\n");
        fprintf(stderr, "  step_ms: duration of each step (default 1000)\n");
        fprintf(stderr, "  settle_ms: settling time to skip at start of each step (default 100)\n");
        return 1;
    }

    const char* inFile = argv[1];
    float stepMs = (argc > 2) ? static_cast<float>(atof(argv[2])) : 1000.f;
    float settleMs = (argc > 3) ? static_cast<float>(atof(argv[3])) : 100.f;

    // Read WAV
    WavData wav;
    if (!readWav(inFile, wav)) return 1;

    fprintf(stderr, "Input: %d Hz, %d ch, %d samples (%.2f sec)\n",
            wav.sampleRate, wav.channels, wav.numSamples,
            wav.numSamples / static_cast<float>(wav.sampleRate));

    // Extract mono (left channel or mono)
    std::vector<float> mono(wav.numSamples);
    for (int i = 0; i < wav.numSamples; i++)
        mono[i] = wav.data[i * wav.channels]; // first channel

    // Test grid
    float cutoffs[] = {32.7f, 65.4f, 130.8f, 261.6f, 523.3f, 1046.5f, 2093.f, 4186.f, 8372.f};
    int nCutoffs = 9;
    int nRes = 11; // 0.0, 0.1, ... 1.0

    int stepSamples = static_cast<int>(stepMs * wav.sampleRate / 1000.f);
    int settleSamples = static_cast<int>(settleMs * wav.sampleRate / 1000.f);
    int analyzeSamples = stepSamples - settleSamples;

    int totalSteps = nCutoffs * nRes;
    int expectedSamples = totalSteps * stepSamples;

    fprintf(stderr, "Grid: %d cutoffs x %d res = %d steps\n", nCutoffs, nRes, totalSteps);
    fprintf(stderr, "Step: %d ms (%d samples), settle: %d ms (%d samples), analyze: %d samples\n",
            static_cast<int>(stepMs), stepSamples,
            static_cast<int>(settleMs), settleSamples, analyzeSamples);
    fprintf(stderr, "Expected duration: %.1f sec, actual: %.1f sec\n",
            expectedSamples / static_cast<float>(wav.sampleRate),
            wav.numSamples / static_cast<float>(wav.sampleRate));

    if (wav.numSamples < expectedSamples)
    {
        fprintf(stderr, "Warning: file is shorter than expected (%d < %d samples)\n",
                wav.numSamples, expectedSamples);
    }

    // CSV header
    printf("target_hz,res,minus3db_hz,minus3db_cents,passband_db,peak_db,peak_hz,prominence_db,slope_db_oct\n");

    int step = 0;
    for (int c = 0; c < nCutoffs; c++)
    {
        for (int r = 0; r < nRes; r++)
        {
            float targetHz = cutoffs[c];
            float res = r * 0.1f;

            int offset = step * stepSamples + settleSamples;
            if (offset + analyzeSamples > wav.numSamples)
            {
                fprintf(stderr, "  Step %d (%.0f Hz, R=%.1f): beyond end of file\n",
                        step, targetHz, res);
                step++;
                continue;
            }

            StepResult result = analyzeStep(mono.data() + offset, analyzeSamples,
                                         wav.sampleRate, targetHz, res);

            float cents = (result.minus3dbHz > 0)
                ? 1200.f * log2f(result.minus3dbHz / targetHz)
                : 0.f;

            printf("%.1f,%.1f,%.1f,%+.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
                   result.targetHz, result.res,
                   result.minus3dbHz, cents,
                   result.passbandDb, result.peakDb, result.peakHz,
                   result.prominenceDb, result.slopeDbOct);

            fprintf(stderr, "  %7.0f Hz R=%.1f: -3dB=%7.0f Hz (%+5.0f cts)  PB=%.1f  peak=%.1f@%.0f (%+.1f)  slope=%.0f dB/oct\n",
                    result.targetHz, result.res,
                    result.minus3dbHz, cents,
                    result.passbandDb, result.peakDb, result.peakHz,
                    result.prominenceDb, result.slopeDbOct);

            step++;
        }
    }

    fprintf(stderr, "\nDone: %d steps analyzed\n", step);
    return 0;
}
