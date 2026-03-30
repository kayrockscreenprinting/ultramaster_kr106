// Offline MIDI renderer — plays a MIDI file through the KR106 DSP engine
// and writes the output to a 24-bit stereo WAV file.
//
// Usage: render_midi input.mid [output.wav] [samplerate]
//   Defaults: output.wav, 44100
//
// Supports: Note On/Off, Roland Juno-106 SysEx (F0 41 32 ...), tempo changes.
// Renders all 128 factory preset parameters via SysEx, no JUCE dependency.

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include <string>

// Pull in the DSP engine (header-only)
#include "../../Source/DSP/KR106_DSP.h"
#include "../../Source/DSP/KR106_DSP_SetParam.h"

// EParams enum (duplicated from PluginProcessor.h to avoid JUCE dependency)
enum EParams
{
  kBenderDco = 0, kBenderVcf, kArpRate, kLfoRate, kLfoDelay,
  kDcoLfo, kDcoPwm, kDcoSub, kDcoNoise, kHpfFreq,
  kVcfFreq, kVcfRes, kVcfEnv, kVcfLfo, kVcfKbd,
  kVcaLevel, kEnvA, kEnvD, kEnvS, kEnvR,
  kTranspose, kHold, kArpeggio, kDcoPulse, kDcoSaw, kDcoSubSw,
  kChorusOff, kChorusI, kChorusII,
  kOctTranspose, kArpMode, kArpRange, kLfoMode, kPwmMode,
  kVcfEnvInv, kVcaMode,
  kBender, kTuning, kPower,
  kPortaMode, kPortaRate,
  kTransposeOffset, kBenderLfo,
  kAdsrMode,
  kMasterVol,
  kNumParams
};

// SysEx CC → EParam mapping (same as PluginProcessor.cpp)
static constexpr int kSysExToParam[16] = {
  kLfoRate, kLfoDelay, kDcoLfo, kDcoPwm,
  kDcoNoise, kVcfFreq, kVcfRes, kVcfEnv,
  kVcfLfo, kVcfKbd, kVcaLevel, kEnvA,
  kEnvD, kEnvS, kEnvR, kDcoSub,
};

// --- Simple MIDI file parser ---

struct MidiEvent {
    uint32_t tickAbs;    // absolute tick position
    uint8_t status;
    std::vector<uint8_t> data; // includes all bytes after status (or SysEx payload)
};

static uint32_t readVarLen(const uint8_t*& p, const uint8_t* end)
{
    uint32_t val = 0;
    while (p < end)
    {
        uint8_t b = *p++;
        val = (val << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return val;
}

static uint16_t read16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
static uint32_t read32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

struct MidiFile {
    uint16_t format;
    uint16_t ticksPerBeat;
    std::vector<MidiEvent> events;
    std::vector<std::pair<uint32_t, uint32_t>> tempoChanges; // tick, usPerBeat
};

static bool parseMidiFile(const char* filename, MidiFile& mf)
{
    FILE* f = fopen(filename, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", filename); return false; }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(size);
    fread(buf.data(), 1, size, f);
    fclose(f);

    const uint8_t* p = buf.data();
    const uint8_t* end = p + size;

    // MThd
    if (size < 14 || memcmp(p, "MThd", 4) != 0)
    { fprintf(stderr, "Error: not a MIDI file\n"); return false; }

    uint32_t hdrLen = read32(p + 4);
    mf.format = read16(p + 8);
    uint16_t nTracks = read16(p + 10);
    mf.ticksPerBeat = read16(p + 12);
    p += 8 + hdrLen;

    // Parse tracks
    for (int t = 0; t < nTracks && p + 8 <= end; t++)
    {
        if (memcmp(p, "MTrk", 4) != 0) break;
        uint32_t trkLen = read32(p + 4);
        p += 8;
        const uint8_t* trkEnd = p + trkLen;
        if (trkEnd > end) trkEnd = end;

        uint32_t tickAbs = 0;
        uint8_t runStatus = 0;

        while (p < trkEnd)
        {
            uint32_t delta = readVarLen(p, trkEnd);
            tickAbs += delta;

            if (p >= trkEnd) break;
            uint8_t b = *p;

            if (b == 0xFF) // Meta event
            {
                p++;
                if (p >= trkEnd) break;
                uint8_t type = *p++;
                uint32_t len = readVarLen(p, trkEnd);
                if (type == 0x51 && len == 3 && p + 3 <= trkEnd) // tempo
                {
                    uint32_t us = (p[0] << 16) | (p[1] << 8) | p[2];
                    mf.tempoChanges.push_back({tickAbs, us});
                }
                p += len;
            }
            else if (b == 0xF0) // SysEx
            {
                p++; // skip F0
                uint32_t len = readVarLen(p, trkEnd);
                MidiEvent ev;
                ev.tickAbs = tickAbs;
                ev.status = 0xF0;
                ev.data.assign(p, p + std::min(len, static_cast<uint32_t>(trkEnd - p)));
                mf.events.push_back(ev);
                p += len;
            }
            else if (b & 0x80) // Status byte
            {
                runStatus = b;
                p++;
                MidiEvent ev;
                ev.tickAbs = tickAbs;
                ev.status = runStatus;
                int nData = ((runStatus & 0xF0) == 0xC0 || (runStatus & 0xF0) == 0xD0) ? 1 : 2;
                for (int i = 0; i < nData && p < trkEnd; i++)
                    ev.data.push_back(*p++);
                mf.events.push_back(ev);
            }
            else // Running status
            {
                MidiEvent ev;
                ev.tickAbs = tickAbs;
                ev.status = runStatus;
                ev.data.push_back(b);
                p++;
                int nData = ((runStatus & 0xF0) == 0xC0 || (runStatus & 0xF0) == 0xD0) ? 1 : 2;
                for (int i = 1; i < nData && p < trkEnd; i++)
                    ev.data.push_back(*p++);
                mf.events.push_back(ev);
            }
        }

        p = trkEnd;
    }

    // Sort events by tick (stable for same-tick ordering)
    std::stable_sort(mf.events.begin(), mf.events.end(),
        [](const MidiEvent& a, const MidiEvent& b) { return a.tickAbs < b.tickAbs; });

    // Default tempo if none specified
    if (mf.tempoChanges.empty())
        mf.tempoChanges.push_back({0, 500000}); // 120 BPM

    return true;
}

// Convert tick to sample position using tempo map
static double tickToSample(uint32_t tick, const MidiFile& mf, double sr)
{
    double sample = 0.0;
    uint32_t prevTick = 0;
    double usPerBeat = 500000.0;

    for (auto& tc : mf.tempoChanges)
    {
        if (tc.first > tick) break;
        double dtTicks = tc.first - prevTick;
        double dtBeats = dtTicks / mf.ticksPerBeat;
        sample += dtBeats * usPerBeat * 1e-6 * sr;
        prevTick = tc.first;
        usPerBeat = tc.second;
    }

    double dtTicks = tick - prevTick;
    double dtBeats = dtTicks / mf.ticksPerBeat;
    sample += dtBeats * usPerBeat * 1e-6 * sr;

    return sample;
}

// --- WAV writer (stereo, 24-bit PCM) ---

static void writeWav(const char* filename, const float* L, const float* R,
                     int numSamples, int sampleRate, bool mono, int bits)
{
    FILE* f = fopen(filename, "wb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", filename); return; }

    int bytesPerSample = bits / 8;
    int channels = mono ? 1 : 2;
    int dataSize = numSamples * bytesPerSample * channels;
    int fileSize = 36 + dataSize;

    auto write16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    auto write32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };

    fwrite("RIFF", 1, 4, f);
    write32(fileSize);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    write32(16);
    write16(1);                          // PCM
    write16(channels);
    write32(sampleRate);
    write32(sampleRate * bytesPerSample * channels);
    write16(bytesPerSample * channels);
    write16(bits);
    fwrite("data", 1, 4, f);
    write32(dataSize);

    auto writeSample = [&](float s) {
        s = std::clamp(s, -1.f, 1.f);
        if (bits == 16)
        {
            int16_t v = static_cast<int16_t>(s * 32767.f);
            fwrite(&v, 2, 1, f);
        }
        else
        {
            int32_t v = static_cast<int32_t>(s * 8388607.f);
            uint8_t bytes[3] = {
                static_cast<uint8_t>(v & 0xFF),
                static_cast<uint8_t>((v >> 8) & 0xFF),
                static_cast<uint8_t>((v >> 16) & 0xFF)
            };
            fwrite(bytes, 1, 3, f);
        }
    };

    for (int i = 0; i < numSamples; i++)
    {
        if (mono)
            writeSample((L[i] + R[i]) * 0.5f);
        else
        {
            writeSample(L[i]);
            writeSample(R[i]);
        }
    }

    fclose(f);
    fprintf(stderr, "Wrote %s: %d samples, %d Hz, %d-bit %s (%.1f sec)\n",
            filename, numSamples, sampleRate, bits, mono ? "mono" : "stereo", numSamples / (float)sampleRate);
}

// --- DSP parameter dispatch ---

static void setParam(KR106DSP<float>& dsp, int param, float value)
{
    dsp.SetParam(param, static_cast<double>(value));
}

static void decodeSwitches1(KR106DSP<float>& dsp, uint8_t val)
{
    int oct = (val & 0x04) ? 2 : (val & 0x02) ? 1 : 0;
    setParam(dsp, kOctTranspose, static_cast<float>(oct));
    setParam(dsp, kDcoPulse, (val & 0x08) ? 1.f : 0.f);
    setParam(dsp, kDcoSaw, (val & 0x10) ? 1.f : 0.f);
    bool chorusOn = !(val & 0x20);
    bool chorusL1 = (val & 0x40) != 0;
    setParam(dsp, kChorusOff, chorusOn ? 0.f : 1.f);
    setParam(dsp, kChorusI, (chorusOn && chorusL1) ? 1.f : 0.f);
    setParam(dsp, kChorusII, (chorusOn && !chorusL1) ? 1.f : 0.f);
}

static void decodeSwitches2(KR106DSP<float>& dsp, uint8_t val)
{
    setParam(dsp, kPwmMode, (val & 0x01) ? 1.f : 0.f); // bit=1 is MAN/ENV
    setParam(dsp, kVcfEnvInv, (val & 0x02) ? 1.f : 0.f);
    setParam(dsp, kVcaMode, (val & 0x04) ? 1.f : 0.f); // bit=1 is GATE
    int hpf = 3 - ((val >> 3) & 0x03);
    setParam(dsp, kHpfFreq, static_cast<float>(hpf));
}

static void handleSysEx(KR106DSP<float>& dsp, const uint8_t* data, int len)
{
    if (len < 4 || data[0] != 0x41) return;
    int cmd = data[1];

    if (cmd == 0x32 && len >= 5)
    {
        // IPR (Individual Parameter): 41 32 0n cc vv
        int ctrl = data[3];
        int val = data[4];
        if (ctrl <= 0x0F)
            setParam(dsp, kSysExToParam[ctrl], val / 127.f);
        else if (ctrl == 0x10)
            decodeSwitches1(dsp, static_cast<uint8_t>(val));
        else if (ctrl == 0x11)
            decodeSwitches2(dsp, static_cast<uint8_t>(val));
    }
    else if ((cmd == 0x30 || cmd == 0x31) && len >= 21)
    {
        // APR (All Parameter): 41 30 0n pp [16 sliders] [sw1] [sw2]
        const uint8_t* p = data + 4;
        for (int cc = 0; cc < 16; cc++)
            setParam(dsp, kSysExToParam[cc], p[cc] / 127.f);
        decodeSwitches1(dsp, p[16]);
        decodeSwitches2(dsp, p[17]);
    }
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: render_midi input.mid [output.wav] [samplerate] [--mono] [--16bit]\n");
        return 1;
    }

    // Check for flags anywhere in args
    bool mono = false;
    int bits = 24;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--mono") == 0) mono = true;
        if (strcmp(argv[i], "--16bit") == 0) bits = 16;
    }

    auto isFlag = [](const char* s) { return s[0] == '-' && s[1] == '-'; };
    const char* inFile = argv[1];
    const char* outFile = (argc > 2 && !isFlag(argv[2])) ? argv[2] : "output.wav";
    float sr = (argc > 3 && !isFlag(argv[3])) ? static_cast<float>(atof(argv[3])) : 44100.f;

    // Parse MIDI
    MidiFile mf;
    if (!parseMidiFile(inFile, mf)) return 1;

    fprintf(stderr, "MIDI: %zu events, %zu tempo changes, %d ticks/beat\n",
            mf.events.size(), mf.tempoChanges.size(), mf.ticksPerBeat);

    // Find total duration
    uint32_t lastTick = 0;
    for (auto& e : mf.events)
        lastTick = std::max(lastTick, e.tickAbs);
    // Add 5 seconds of tail for release
    double totalSamples = tickToSample(lastTick, mf, sr) + 5.0 * sr;
    int numSamples = static_cast<int>(totalSamples);

    fprintf(stderr, "Rendering %.1f seconds (%d samples at %.0f Hz)\n",
            numSamples / sr, numSamples, sr);

    // Initialize DSP
    static constexpr int kBlockSize = 512;
    KR106DSP<float> dsp(6);
    dsp.Reset(sr, kBlockSize);

    // Set defaults: power on, J106 mode, master volume
    // Plugin default: slider 0.5, squared taper → 0.25
    setParam(dsp, kPower, 1.f);
    setParam(dsp, kAdsrMode, 1.f);  // J106
    dsp.mMasterVol = 0.5f * 0.5f;   // match plugin default (squared taper)
    setParam(dsp, kVcaLevel, 0.5f); // unity gain (~0 dB)
    setParam(dsp, kPortaMode, 2.f);  // Poly II (round robin)

    // Convert event ticks to sample positions
    struct RenderEvent {
        int samplePos;
        const MidiEvent* event;
    };
    std::vector<RenderEvent> renderEvents;
    for (auto& e : mf.events)
    {
        int pos = static_cast<int>(tickToSample(e.tickAbs, mf, sr));
        renderEvents.push_back({pos, &e});
    }

    // Allocate output buffers
    std::vector<float> outL(numSamples, 0.f);
    std::vector<float> outR(numSamples, 0.f);

    // Render block by block
    int eventIdx = 0;
    int samplesRendered = 0;

    while (samplesRendered < numSamples)
    {
        int blockSize = std::min(kBlockSize, numSamples - samplesRendered);

        // Process MIDI events that fall within this block
        while (eventIdx < static_cast<int>(renderEvents.size()) &&
               renderEvents[eventIdx].samplePos < samplesRendered + blockSize)
        {
            auto& re = renderEvents[eventIdx];
            auto& e = *re.event;

            if (e.status == 0xF0 && !e.data.empty())
            {
                handleSysEx(dsp, e.data.data(), static_cast<int>(e.data.size()));
            }
            else if ((e.status & 0xF0) == 0x90 && e.data.size() >= 2)
            {
                if (e.data[1] > 0)
                    dsp.NoteOn(e.data[0], e.data[1]);
                else
                    dsp.NoteOff(e.data[0]);
            }
            else if ((e.status & 0xF0) == 0x80 && !e.data.empty())
            {
                dsp.NoteOff(e.data[0]);
            }

            eventIdx++;
        }

        // Render audio
        float* outputs[2] = {outL.data() + samplesRendered, outR.data() + samplesRendered};
        dsp.ProcessBlock(nullptr, outputs, 2, blockSize);

        samplesRendered += blockSize;

        // Progress
        if (samplesRendered % (static_cast<int>(sr) * 10) < blockSize)
            fprintf(stderr, "  %.0f / %.0f sec\n", samplesRendered / sr, numSamples / sr);
    }

    // Normalize to -1 dBFS
    float peak = 0.f;
    for (int i = 0; i < numSamples; i++)
    {
        peak = std::max(peak, fabsf(outL[i]));
        peak = std::max(peak, fabsf(outR[i]));
    }
    fprintf(stderr, "Raw peak: %.4f (%.1f dBFS)\n", peak,
            peak > 1e-10f ? 20.f * log10f(peak) : -200.f);

    if (peak > 1e-10f)
    {
        float gain = 0.891f / peak; // -1 dBFS
        for (int i = 0; i < numSamples; i++)
        {
            outL[i] *= gain;
            outR[i] *= gain;
        }
        fprintf(stderr, "Normalized: gain=%.3f (%+.1f dB)\n", gain, 20.f * log10f(gain));
    }

    writeWav(outFile, outL.data(), outR.data(), numSamples, static_cast<int>(sr), mono, bits);
    return 0;
}
