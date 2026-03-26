// Generate a MIDI file that tests the VCF by running noise through it
// at 9 cutoff frequencies x 11 resonance levels.
//
// Patch: noise only (no oscillators), VCA gate mode, max noise.
// Sweep: for each cutoff (C1-C9), sweep resonance 0.0-1.0 in 0.1 steps.
// Hold: 1s per step (99 steps, ~100s total).
//
// Usage: gen_filter_test [output.mid]
//   Default: filter_test.mid

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

// --- MIDI file helpers ---

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
    writeVarLen(track, delta);
    track.push_back(0xF0);
    writeVarLen(track, 6);
    track.push_back(0x41);
    track.push_back(0x32);
    track.push_back(0x00);
    track.push_back(ctrl);
    track.push_back(val);
    track.push_back(0xF7);
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

static void addEndOfTrack(std::vector<uint8_t>& track)
{
    writeVarLen(track, 0);
    track.push_back(0xFF);
    track.push_back(0x2F);
    track.push_back(0x00);
}

// SysEx CC mapping:
// 0x04 = DCO Noise, 0x05 = VCF Freq, 0x06 = VCF Res, 0x07 = VCF Env
// 0x08 = VCF LFO,   0x09 = VCF KBD,  0x0A = VCA Level
// 0x0B = Env A,      0x0D = Env S,    0x0E = Env R

int main(int argc, char* argv[])
{
    const char* outFile = (argc > 1) ? argv[1] : "filter_test.mid";

    // At 120 BPM, 480 ticks = 0.5s, so 960 ticks = 1s
    static constexpr uint32_t kHoldTicks = 960;   // 1s per test point
    static constexpr uint32_t kSettleTicks = 240;  // 250ms settle after param change

    std::vector<uint8_t> track;
    addTempo(track, 500000); // 120 BPM

    // --- Set up noise-only patch ---
    // Zero all sliders
    for (int cc = 0; cc <= 0x0F; cc++)
        addSysEx(track, (cc == 0) ? 0 : 5, static_cast<uint8_t>(cc), 0);

    // Noise at max
    addSysEx(track, 5, 0x04, 127);
    // VCA Level at unity (~0 dB, byte 64 = slider 0.5)
    addSysEx(track, 5, 0x0A, 64);
    // Attack = 0, Sustain = max, Release = 0
    addSysEx(track, 5, 0x0B, 0);    // Env A
    addSysEx(track, 5, 0x0D, 127);  // Env S
    addSysEx(track, 5, 0x0E, 0);    // Env R
    // VCF Env = 0, VCF LFO = 0, VCF KBD = 0
    addSysEx(track, 5, 0x07, 0);
    addSysEx(track, 5, 0x08, 0);
    addSysEx(track, 5, 0x09, 0);

    // Switches 1: no oscillators, 8' range, chorus off
    addSysEx(track, 5, 0x10, 0x02 | 0x20);

    // Switches 2: MAN PWM, ENV+ polarity, GATE VCA mode, HPF=0
    addSysEx(track, 5, 0x11, 0x04 | 0x18); // bit2=GATE, bits3-4=HPF 00→3→0

    // --- Cutoff frequencies ---
    // C1-C9: approximate SysEx byte values for the J106 DAC curve.
    // These are slider bytes (0-127), the firmware converts via dacToHz.
    // Computed from: byte = round(ln(targetHz / 5.53) / (0.693147/1143) / 128)
    struct FreqPoint { const char* name; uint8_t byte; float hz; };
    FreqPoint freqs[] = {
        {"C1",  23, 32.7f},
        {"C2",  32, 65.4f},
        {"C3",  41, 130.8f},
        {"C4",  50, 261.6f},
        {"C5",  59, 523.3f},
        {"C6",  68, 1046.5f},
        {"C7",  76, 2093.f},
        {"C8",  85, 4186.f},
        {"C9",  94, 8372.f},
    };
    int nFreqs = 9;

    // Resonance: 0.0 to 1.0 in 0.1 steps (byte = res * 127)
    int nRes = 11;

    // Note on (held throughout entire test)
    addNoteOn(track, 480, 60, 100); // C4 after 500ms settle

    for (int f = 0; f < nFreqs; f++)
    {
        // Set cutoff frequency
        addSysEx(track, (f == 0) ? kSettleTicks : kHoldTicks, 0x05, freqs[f].byte);

        for (int r = 0; r < nRes; r++)
        {
            uint8_t resByte = static_cast<uint8_t>(std::min(127, static_cast<int>(r * 12.7f + 0.5f)));
            addSysEx(track, (r == 0) ? kSettleTicks : kHoldTicks, 0x06, resByte);
        }
    }

    // Hold last point, then note off
    addNoteOff(track, kHoldTicks, 60);
    addEndOfTrack(track);

    // Write MIDI file
    std::vector<uint8_t> midi;
    midi.push_back('M'); midi.push_back('T'); midi.push_back('h'); midi.push_back('d');
    write32(midi, 6);
    write16(midi, 0);
    write16(midi, 1);
    write16(midi, 480);
    midi.push_back('M'); midi.push_back('T'); midi.push_back('r'); midi.push_back('k');
    write32(midi, static_cast<uint32_t>(track.size()));
    midi.insert(midi.end(), track.begin(), track.end());

    FILE* file = fopen(outFile, "wb");
    if (!file) { fprintf(stderr, "Error: cannot open %s\n", outFile); return 1; }
    fwrite(midi.data(), 1, midi.size(), file);
    fclose(file);

    int totalPoints = nFreqs * nRes;
    fprintf(stderr, "Wrote %s: %d cutoffs x %d resonances = %d test points (~%ds)\n",
            outFile, nFreqs, nRes, totalPoints, totalPoints + 2);
    fprintf(stderr, "Sweep order: for each cutoff (C1-C9), sweep res 0.0-1.0\n");
    fprintf(stderr, "Cutoff bytes: ");
    for (int f = 0; f < nFreqs; f++)
        fprintf(stderr, "%s=%d%s", freqs[f].name, freqs[f].byte, f < nFreqs-1 ? ", " : "\n");

    return 0;
}
