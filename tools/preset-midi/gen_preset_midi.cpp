// Generate MIDI files for A/B testing presets against real Juno-106 hardware.
//
// For each factory preset, generates a .mid file containing:
//   1. SysEx messages to set all parameters (no Program Change needed)
//   2. A short pause for the hardware to settle
//   3. A sequence of test notes across the keyboard
//
// Usage: gen_preset_midi [preset_index | "all"]
//   Default: all (generates 128 files)
//   Single: gen_preset_midi 0  (generates A11_Brass.mid)
//
// Output: one .mid file per preset in the current directory.
// Play the same file into both real hardware and the plugin to compare.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

#include "../../Source/KR106_Presets_JUCE.h"
#include "../../Source/DSP/KR106ADSR.h"

// --- Standard MIDI file writer ---

static void writeVarLen(std::vector<uint8_t>& buf, uint32_t val)
{
    uint8_t bytes[4];
    int n = 0;
    bytes[n++] = val & 0x7F;
    while (val >>= 7)
        bytes[n++] = (val & 0x7F) | 0x80;
    for (int i = n - 1; i >= 0; i--)
        buf.push_back(bytes[i]);
}

static void write16(std::vector<uint8_t>& buf, uint16_t val)
{
    buf.push_back((val >> 8) & 0xFF);
    buf.push_back(val & 0xFF);
}

static void write32(std::vector<uint8_t>& buf, uint32_t val)
{
    buf.push_back((val >> 24) & 0xFF);
    buf.push_back((val >> 16) & 0xFF);
    buf.push_back((val >> 8) & 0xFF);
    buf.push_back(val & 0xFF);
}

static void addSysEx(std::vector<uint8_t>& track, uint32_t delta,
                     uint8_t ctrl, uint8_t val)
{
    // Roland Juno-106 SysEx: F0 41 32 00 ctrl val F7
    writeVarLen(track, delta);
    track.push_back(0xF0);           // SysEx start (in MIDI file format)
    writeVarLen(track, 6);           // length of data after F0 (excluding F7... actually including F7)
    track.push_back(0x41);           // Roland ID
    track.push_back(0x32);           // Juno-106
    track.push_back(0x00);           // channel
    track.push_back(ctrl);
    track.push_back(val);
    track.push_back(0xF7);           // EOX
}

static void addNoteOn(std::vector<uint8_t>& track, uint32_t delta,
                      uint8_t note, uint8_t vel)
{
    writeVarLen(track, delta);
    track.push_back(0x90);
    track.push_back(note);
    track.push_back(vel);
}

static void addNoteOff(std::vector<uint8_t>& track, uint32_t delta,
                       uint8_t note)
{
    writeVarLen(track, delta);
    track.push_back(0x80);
    track.push_back(note);
    track.push_back(0x00);
}

static void addEndOfTrack(std::vector<uint8_t>& track)
{
    writeVarLen(track, 0);
    track.push_back(0xFF);
    track.push_back(0x2F);
    track.push_back(0x00);
}

static void addTempo(std::vector<uint8_t>& track, uint32_t usPerBeat)
{
    writeVarLen(track, 0);
    track.push_back(0xFF);
    track.push_back(0x51);
    track.push_back(0x03);
    track.push_back((usPerBeat >> 16) & 0xFF);
    track.push_back((usPerBeat >> 8) & 0xFF);
    track.push_back(usPerBeat & 0xFF);
}

// Preset data layout (from CLAUDE.md / KR106_Presets_JUCE.h):
//  0 kBenderDco    11 kVcfRes       22 kArpeggio     33 kPwmMode
//  1 kBenderVcf    12 kVcfEnv       23 kDcoPulse     34 kVcfEnvInv
//  2 kArpRate      13 kVcfLfo       24 kDcoSaw       35 kVcaMode
//  3 kLfoRate      14 kVcfKbd       25 kDcoSubSw     36 kBender
//  4 kLfoDelay     15 kVcaLevel     26 kChorusOff    37 kTuning
//  5 kDcoLfo       16 kEnvA         27 kChorusI      38 kPower
//  6 kDcoPwm       17 kEnvD         28 kChorusII     39 kPortaMode
//  7 kDcoSub       18 kEnvS         29 kOctTranspose 40 kPortaRate
//  8 kDcoNoise     19 kEnvR         30 kArpMode      41 kTransposeOfs
//  9 kHpfFreq      20 kTranspose    31 kArpRange     42 kBenderLfo
// 10 kVcfFreq      21 kHold         32 kLfoMode      43 kAdsrMode

// SysEx CC mapping: CC 0x00-0x0F → slider params
// CC 0x00 = kLfoRate(3),  0x01 = kLfoDelay(4),  0x02 = kDcoLfo(5),  0x03 = kDcoPwm(6)
// CC 0x04 = kDcoNoise(8), 0x05 = kVcfFreq(10),  0x06 = kVcfRes(11), 0x07 = kVcfEnv(12)
// CC 0x08 = kVcfLfo(13),  0x09 = kVcfKbd(14),   0x0A = kVcaLevel(15), 0x0B = kEnvA(16)
// CC 0x0C = kEnvD(17),    0x0D = kEnvS(18),      0x0E = kEnvR(19),  0x0F = kDcoSub(7)

static const int kSysExToPresetIdx[16] = {
    3,   // 0x00 = kLfoRate
    4,   // 0x01 = kLfoDelay
    5,   // 0x02 = kDcoLfo
    6,   // 0x03 = kDcoPwm
    8,   // 0x04 = kDcoNoise
    10,  // 0x05 = kVcfFreq
    11,  // 0x06 = kVcfRes
    12,  // 0x07 = kVcfEnv
    13,  // 0x08 = kVcfLfo
    14,  // 0x09 = kVcfKbd
    15,  // 0x0A = kVcaLevel
    16,  // 0x0B = kEnvA
    17,  // 0x0C = kEnvD
    18,  // 0x0D = kEnvS
    19,  // 0x0E = kEnvR
    7,   // 0x0F = kDcoSub
};

static void writeMidiFile(const char* filename, const std::vector<uint8_t>& track);
static void appendPresetToTrack(std::vector<uint8_t>& track, int presetIdx, bool firstPreset);

static void generatePresetMidi(int presetIdx, const char* outDir)
{
    const auto& preset = kFactoryPresets[presetIdx];

    std::string name = preset.name;
    std::string filename;
    if (outDir && outDir[0]) { filename = outDir; filename += "/"; }
    for (char c : name)
    {
        if (c == ' ') filename += '_';
        else if ((c >= 'A' && c <= 'z') || (c >= '0' && c <= '9')) filename += c;
    }
    filename += ".mid";

    std::vector<uint8_t> track;
    appendPresetToTrack(track, presetIdx, true);
    addEndOfTrack(track);

    writeMidiFile(filename.c_str(), track);
    fprintf(stderr, "  [%3d] %s -> %s\n", presetIdx, preset.name, filename.c_str());
}

// Append one preset's SysEx + note sequence to an existing track.
// firstPreset: if true, adds tempo and no leading delta.
static void appendPresetToTrack(std::vector<uint8_t>& track, int presetIdx, bool firstPreset)
{
    const auto& preset = kFactoryPresets[presetIdx];
    const int* v = preset.values;

    // 2s gap between presets (except first)
    uint32_t presetGap = firstPreset ? 0 : 480 * 4;

    if (firstPreset)
        addTempo(track, 500000); // 120 BPM

    // --- SysEx ---
    for (int cc = 0; cc < 16; cc++)
    {
        int pidx = kSysExToPresetIdx[cc];
        uint8_t val = static_cast<uint8_t>(v[pidx]);
        uint32_t delta = (cc == 0) ? presetGap : 10;
        addSysEx(track, delta, static_cast<uint8_t>(cc), val);
    }

    // Switches 1
    {
        uint8_t sw1 = 0;
        int oct = v[29];
        if (oct == 2) sw1 |= 0x04;
        else if (oct == 1) sw1 |= 0x02;
        else sw1 |= 0x01;
        if (v[23]) sw1 |= 0x08;
        if (v[24]) sw1 |= 0x10;
        bool chorusI = v[27] != 0, chorusII = v[28] != 0;
        if (!chorusI && !chorusII) sw1 |= 0x20;
        if (chorusI) sw1 |= 0x40;
        addSysEx(track, 10, 0x10, sw1);
    }

    // Switches 2
    {
        uint8_t sw2 = 0;
        if (v[33] == 0) sw2 |= 0x01;
        if (v[34]) sw2 |= 0x02;
        if (v[35] == 0) sw2 |= 0x04;
        sw2 |= static_cast<uint8_t>((3 - v[9]) << 3);
        addSysEx(track, 10, 0x11, sw2);
    }

    // Compute timing from preset envelope values
    // Preset indices: 16=kEnvA, 17=kEnvD, 18=kEnvS, 19=kEnvR
    float atkSlider = v[16] / 127.f;
    float relSlider = v[19] / 127.f;
    float atkMs = kr106::ADSR::AttackMs(atkSlider);
    float relMs = kr106::ADSR::DecRelMs(relSlider);

    // Hold = attack time + 500ms sustain (enough to hear steady state)
    // Release tail = release time + 500ms safety margin
    // Convert ms to ticks: 480 ticks = 0.5s = 500ms at 120 BPM
    auto msToTicks = [](float ms) -> uint32_t {
        return std::max(static_cast<uint32_t>(ms * 480.f / 500.f + 0.5f), uint32_t(48));
    };

    uint32_t holdTicks = msToTicks(atkMs + 500.f);
    uint32_t relTicks = msToTicks(relMs + 500.f);

    // Settle
    uint32_t settleTicks = 240;

    // Long chord (C4-E4-G4): full attack + sustain, full release tail
    // Tests voice summing, envelope shape, and chorus interaction
    addNoteOn(track, settleTicks, 60, 100);  // C4
    addNoteOn(track, 0, 64, 100);            // E4
    addNoteOn(track, 0, 67, 100);            // G4
    addNoteOff(track, holdTicks, 60);
    addNoteOff(track, 0, 64);
    addNoteOff(track, 0, 67);

    // Quick keyboard survey: C2, C3, C4, C5, C6 (1s hold, 1s gap)
    static constexpr uint8_t testNotes[] = {36, 48, 60, 72, 84};
    for (int i = 0; i < 5; i++)
    {
        uint32_t delta = (i == 0) ? relTicks : 480 * 2;
        addNoteOn(track, delta, testNotes[i], 100);
        addNoteOff(track, 480 * 2, testNotes[i]);
    }
}

static void writeMidiFile(const char* filename, const std::vector<uint8_t>& track)
{
    std::vector<uint8_t> midi;
    midi.push_back('M'); midi.push_back('T'); midi.push_back('h'); midi.push_back('d');
    write32(midi, 6);
    write16(midi, 0);
    write16(midi, 1);
    write16(midi, 480);
    midi.push_back('M'); midi.push_back('T'); midi.push_back('r'); midi.push_back('k');
    write32(midi, static_cast<uint32_t>(track.size()));
    midi.insert(midi.end(), track.begin(), track.end());

    FILE* f = fopen(filename, "wb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", filename); return; }
    fwrite(midi.data(), 1, midi.size(), f);
    fclose(f);
}

// Generate a batch file covering presets [start, end).
static void generateBatchMidi(int start, int end, const char* filename)
{
    std::vector<uint8_t> track;

    for (int i = start; i < end; i++)
        appendPresetToTrack(track, i, i == start);

    addEndOfTrack(track);
    writeMidiFile(filename, track);

    fprintf(stderr, "Batch: presets %d-%d (%s-%s) -> %s\n",
            start, end - 1,
            kFactoryPresets[start].name,
            kFactoryPresets[end - 1].name,
            filename);
}

int main(int argc, char* argv[])
{
    if (argc > 1 && strcmp(argv[1], "batch") == 0)
    {
        // Generate bank files: A1x, A2x, A3x, ... B8x (8 presets each)
        const char* bankNames[] = {
            "A1x", "A2x", "A3x", "A4x", "A5x", "A6x", "A7x", "A8x",
            "B1x", "B2x", "B3x", "B4x", "B5x", "B6x", "B7x", "B8x"
        };
        const char* outDir = (argc > 2) ? argv[2] : ".";

        fprintf(stderr, "Generating batch MIDI files (8 presets each)...\n");
        for (int bank = 0; bank < 16; bank++)
        {
            int start = bank * 8;
            int end = std::min(start + 8, kNumFactoryPresets);
            char filename[256];
            snprintf(filename, sizeof(filename), "%s/bank_%s.mid", outDir, bankNames[bank]);
            generateBatchMidi(start, end, filename);
        }
        fprintf(stderr, "Done.\n");
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "all") == 0)
    {
        const char* outDir = (argc > 2) ? argv[2] : ".";
        fprintf(stderr, "Generating individual MIDI files for %d presets...\n", kNumFactoryPresets);
        for (int i = 0; i < kNumFactoryPresets; i++)
            generatePresetMidi(i, outDir);
        fprintf(stderr, "Done.\n");
        return 0;
    }

    if (argc > 1)
    {
        int idx = atoi(argv[1]);
        if (idx < 0 || idx >= kNumFactoryPresets)
        {
            fprintf(stderr, "Error: preset index %d out of range (0-%d)\n",
                    idx, kNumFactoryPresets - 1);
            return 1;
        }
        const char* outDir = (argc > 2) ? argv[2] : ".";
        generatePresetMidi(idx, outDir);
        return 0;
    }

    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  gen_preset_midi <index> [outdir]   Single preset\n");
    fprintf(stderr, "  gen_preset_midi all [outdir]       All 128 individual files\n");
    fprintf(stderr, "  gen_preset_midi batch [outdir]     16 bank files (8 presets each)\n");
    return 0;
}
