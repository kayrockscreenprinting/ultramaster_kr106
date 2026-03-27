// VCF frequency response analyzer — hardware test mode 3 format.
//
// Analyzes WAV files from the Juno-106 test mode 3 filter test:
//   6 resonance values × 6 notes × 2 noise states (off/on)
//   Each segment: 3s hold + 0.5s gap = 3.5s per noise state, 7s per note
//
// Usage: vcf_analyze_hw input.wav [offset_ms]
//   offset_ms: time before first note-on (default: auto-detect)
//
// Analyzes only the noise-on segments (oscillator-only segments skipped).
// Reports: peak frequency, peak level, passband level, prominence.
//
// Output (stdout): CSV
// Output (stderr): human-readable

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>
#include <algorithm>

// --- WAV reader ---

struct WavData {
    int sampleRate;
    int channels;
    int numSamples;
    std::vector<float> data;
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
        if (chunkSize & 1) pos++;
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

        if (audioFmt == 3 && bitsPerSample == 32)
            wav.data[i] = *(float*)p;
        else if (audioFmt == 1 && bitsPerSample == 16)
            wav.data[i] = *(int16_t*)p / 32768.f;
        else if (audioFmt == 1 && bitsPerSample == 24)
        {
            int32_t v = (p[0]) | (p[1] << 8) | (p[2] << 16);
            if (v & 0x800000) v |= 0xFF000000;
            wav.data[i] = v / 8388608.f;
        }
        else if (audioFmt == 1 && bitsPerSample == 32)
            wav.data[i] = *(int32_t*)p / 2147483648.f;
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
    for (int i = 1, j = 0; i < n; i++)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        float ang = -2.f * static_cast<float>(M_PI) / len;
        float wRe = cosf(ang), wIm = sinf(ang);
        for (int i = 0; i < n; i += len)
        {
            float curRe = 1.f, curIm = 0.f;
            for (int j = 0; j < len / 2; j++)
            {
                int u = i + j, v = i + j + len / 2;
                float tRe = re[v] * curRe - im[v] * curIm;
                float tIm = re[v] * curIm + im[v] * curRe;
                re[v] = re[u] - tRe; im[v] = im[u] - tIm;
                re[u] += tRe; im[u] += tIm;
                float nr = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = nr;
            }
        }
    }
}

// --- Analyze one noise segment ---

struct SegResult {
    float peakHz;
    float peakDb;
    float passbandDb;
    float prominenceDb;
    float rmsDb;
};

static SegResult analyzeSegment(const float* samples, int numSamples, int sampleRate,
                                 float targetHz)
{
    SegResult r = {0, -200, -200, 0, -200};

    // RMS
    float rmsSum = 0;
    for (int i = 0; i < numSamples; i++) rmsSum += samples[i] * samples[i];
    r.rmsDb = 10.f * log10f(rmsSum / numSamples + 1e-30f);

    // FFT
    int fftSize = 1;
    while (fftSize < numSamples) fftSize <<= 1;
    if (fftSize < 8192) fftSize = 8192;

    std::vector<float> re(fftSize, 0.f), im(fftSize, 0.f);
    for (int i = 0; i < numSamples; i++)
    {
        float w = 0.5f * (1.f - cosf(2.f * static_cast<float>(M_PI) * i / (numSamples - 1)));
        re[i] = samples[i] * w;
    }
    fft(re, im);

    int specSize = fftSize / 2 + 1;
    std::vector<float> magDb(specSize);
    float binHz = static_cast<float>(sampleRate) / fftSize;

    for (int i = 0; i < specSize; i++)
        magDb[i] = 20.f * log10f(sqrtf(re[i] * re[i] + im[i] * im[i]) + 1e-30f);

    // Smooth (~30 Hz window)
    int smoothBins = std::max(1, static_cast<int>(30.f / binHz));
    std::vector<float> smooth(specSize);
    for (int i = 0; i < specSize; i++)
    {
        float sum = 0; int cnt = 0;
        for (int j = std::max(0, i - smoothBins); j <= std::min(specSize - 1, i + smoothBins); j++)
        { sum += magDb[j]; cnt++; }
        smooth[i] = sum / cnt;
    }

    // Passband: average 20 Hz to 0.3× target
    int pbLo = std::max(1, static_cast<int>(20.f / binHz));
    int pbHi = std::max(pbLo + 1, std::min(specSize - 1, static_cast<int>(targetHz * 0.3f / binHz)));
    float pbSum = 0; int pbCnt = 0;
    for (int i = pbLo; i <= pbHi; i++) { pbSum += smooth[i]; pbCnt++; }
    r.passbandDb = (pbCnt > 0) ? pbSum / pbCnt : -200.f;

    // Peak: max in 0.3× to 3× target
    int pkLo = std::max(1, static_cast<int>(targetHz * 0.3f / binHz));
    int pkHi = std::min(specSize - 1, static_cast<int>(targetHz * 3.f / binHz));
    for (int i = pkLo; i <= pkHi; i++)
    {
        if (smooth[i] > r.peakDb)
        {
            r.peakDb = smooth[i];
            r.peakHz = i * binHz;
        }
    }

    r.prominenceDb = r.peakDb - r.passbandDb;
    return r;
}

// --- Auto-detect first onset ---

static float findFirstOnset(const float* mono, int numSamples, int sampleRate, float threshDb)
{
    int window = sampleRate / 20; // 50ms
    for (int i = 0; i + window < numSamples; i += window / 4)
    {
        float rms = 0;
        for (int j = 0; j < window; j++) rms += mono[i + j] * mono[i + j];
        rms = 10.f * log10f(rms / window + 1e-30f);
        if (rms > threshDb)
            return static_cast<float>(i) / sampleRate;
    }
    return 0;
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: vcf_analyze_hw input.wav [offset_ms]\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "Analyzes Juno-106 test mode 3 filter test recordings.\n");
        fprintf(stderr, "Format: 6 resonance × 6 notes × (noise_off 3s + noise_on 3s + gaps)\n");
        fprintf(stderr, "  Resonance: 0, 25, 50, 76, 101, 127 (SysEx values)\n");
        fprintf(stderr, "  Notes: C2(36), C3(48), C4(60), C5(72), C6(84), C7(96)\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "Target frequencies from calc_vcf_freq (cutoff=49, KBD=127, env=0):\n");
        fprintf(stderr, "  C2=62.0, C3=124.0, C4=248.1, C5=496.1, C6=992.3, C7=1984.5 Hz\n");
        return 1;
    }

    const char* inFile = argv[1];

    WavData wav;
    if (!readWav(inFile, wav)) return 1;

    // Extract mono
    std::vector<float> mono(wav.numSamples);
    for (int i = 0; i < wav.numSamples; i++)
        mono[i] = wav.data[i * wav.channels];

    fprintf(stderr, "Input: %d Hz, %d ch, %.1f sec\n",
            wav.sampleRate, wav.channels,
            wav.numSamples / static_cast<float>(wav.sampleRate));

    // Auto-detect or manual offset
    float offset;
    if (argc > 2)
        offset = static_cast<float>(atof(argv[2])) / 1000.f;
    else
    {
        offset = findFirstOnset(mono.data(), wav.numSamples, wav.sampleRate, -55.f);
        // The first detected onset is the osc-only segment for res=0 C2.
        // Back up slightly to catch the note-on.
        offset = std::max(0.f, offset - 0.1f);
    }
    fprintf(stderr, "Offset: %.2f sec\n", offset);

    // Test grid
    static const int resValues[] = {0, 25, 50, 76, 101, 127};
    static const char* noteNames[] = {"C2", "C3", "C4", "C5", "C6", "C7"};
    static const int midiNotes[] = {36, 48, 60, 72, 84, 96};
    static const float targetHz[] = {62.0f, 124.0f, 248.1f, 496.1f, 992.3f, 1984.5f};
    static const int dacVals[] = {3986, 5129, 6272, 7415, 8558, 9701};

    int nRes = 6, nNotes = 6;

    // Timing: per note = 3s osc + 0.5s gap + 3s noise + 0.5s gap = 7s
    // Per res = 6 notes × 7s = 42s
    float noteTime = 7.0f;     // seconds per note (osc + gap + noise + gap)
    float noiseStart = 3.5f;   // noise-on starts 3.5s into each note slot
    float settleSkip = 0.5f;   // skip first 0.5s for settling
    float analyzeLen = 2.0f;   // analyze 2s of steady state

    // CSV header
    printf("res_int,res_norm,k,note,midi,dac,target_hz,peak_hz,peak_cents,peak_db,passband_db,prominence_db,rms_db\n");

    for (int ri = 0; ri < nRes; ri++)
    {
        int res = resValues[ri];
        float resNorm = res / 127.f;
        // k = 0.811 * (exp(2.128 * resNorm) - 1), then soft-clip
        float k = 0.811f * (expf(2.128f * resNorm) - 1.f);
        if (k > 3.0f)
        {
            float excess = k - 3.0f;
            k = 3.0f + excess / (1.0f + excess * 0.2f);
        }

        for (int ni = 0; ni < nNotes; ni++)
        {
            // Noise-on segment timing
            float segStart = offset + ri * nNotes * noteTime + ni * noteTime + noiseStart;
            float aStart = segStart + settleSkip;
            float aEnd = aStart + analyzeLen;

            int sStart = static_cast<int>(aStart * wav.sampleRate);
            int sEnd = static_cast<int>(aEnd * wav.sampleRate);

            if (sEnd > wav.numSamples)
            {
                fprintf(stderr, "  R=%d %s: beyond end of file (need %.1fs, have %.1fs)\n",
                        res, noteNames[ni], aEnd,
                        wav.numSamples / static_cast<float>(wav.sampleRate));
                continue;
            }

            SegResult sr = analyzeSegment(mono.data() + sStart, sEnd - sStart,
                                           wav.sampleRate, targetHz[ni]);

            float cents = (sr.peakHz > 0)
                ? 1200.f * log2f(sr.peakHz / targetHz[ni])
                : 0.f;

            printf("%d,%.4f,%.4f,%s,%d,%d,%.1f,%.1f,%+.1f,%.1f,%.1f,%.1f,%.1f\n",
                   res, resNorm, k, noteNames[ni], midiNotes[ni], dacVals[ni],
                   targetHz[ni], sr.peakHz, cents,
                   sr.peakDb, sr.passbandDb, sr.prominenceDb, sr.rmsDb);

            fprintf(stderr, "  R=%3d %s: peak=%7.1f Hz (%+5.0f cts) prom=%+.1f dB  PB=%.1f  RMS=%.1f\n",
                    res, noteNames[ni], sr.peakHz, cents,
                    sr.prominenceDb, sr.passbandDb, sr.rmsDb);
        }
    }

    fprintf(stderr, "\nDone.\n");
    return 0;
}
