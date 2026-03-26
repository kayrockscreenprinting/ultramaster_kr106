# KR-106 Tools

Standalone command-line tools for testing, calibrating, and rendering the KR-106 DSP engine. All tools are header-only C++17 with no JUCE dependency -- they include the DSP headers directly.

## Building

Each directory has its own Makefile:

```bash
cd tools/vcf-analyze && make
cd tools/preset-midi && make
cd tools/render-midi && make
cd tools/vcf-sweep && make
```

## Tools

### render-midi/

**render_midi** -- Offline MIDI renderer. Plays a MIDI file through the full KR-106 DSP engine (6 voices, 4x oversampled VCF, chorus, HPF) and writes a normalized 24-bit stereo WAV.

```bash
./render_midi input.mid output.wav 44100
```

### vcf-analyze/

VCF calibration pipeline. Generate a test MIDI file, render it, then analyze the WAV.

**gen_filter_test** -- Generates a MIDI file that sweeps noise through the VCF at 9 cutoffs (C1-C9) x 11 resonance levels (0.0-1.0), 1s per step. Noise-only patch, gate mode VCA, no envelope. Ignore the first ~100ms of each step for steady-state analysis.

**vcf_analyze** -- Reads a rendered WAV file and measures the filter response at each test point. Reports -3dB frequency, passband gain, resonance peak, prominence, and rolloff slope. CSV output.

```bash
./gen_filter_test filter_test.mid
cd ../render-midi && ./render_midi ../vcf-analyze/filter_test.mid ../vcf-analyze/filter_test.wav
cd ../vcf-analyze && ./vcf_analyze filter_test.wav > results.csv
```

### preset-midi/

**gen_preset_midi** -- Generates MIDI files that program a Juno-106 via SysEx and play test notes. For A/B comparison between hardware and plugin.

```bash
./gen_preset_midi 0            # single preset (A11 Brass)
./gen_preset_midi all          # 128 individual files
./gen_preset_midi batch        # 16 bank files (8 presets each)
```

Each file sends SysEx for all parameters (no Program Change), plays a C major chord with full attack/release envelope timing, then a quick C2-C6 keyboard survey.

### vcf-sweep/

Standalone VCF unit tests. Each generates a 24-bit mono WAV or CSV output. These test the VCF in isolation without the full signal chain.

**TODO:** Convert these to MIDI file generators (like gen_filter_test) so the same tests can be run on real hardware via render_midi for A/B comparison, rather than only testing the model against itself.

| Tool | Description |
|------|-------------|
| **vcf_sweep** | Sweeps VCF slider 0-1. Optional noise input and resonance on command line. |
| **vcf_noise** | White noise through VCF at fixed frequencies and resonance steps. |
| **vcf_quick** | Self-oscillation pitch/amplitude check at 9 frequencies. CSV output. |
| **vcf_res_sweep** | White noise at fixed cutoff, resonance swept 0-1. |
| **vcf_thd** | Self-oscillation THD measurement via Goertzel DFT. |
| **vcf_peaks** | Compares saw passthrough peak vs self-oscillation peak at 8 octaves. |
| **vcf_cutoff** | Measures actual -3dB cutoff vs target at 10 frequencies x 6 resonance levels. |

### preset-gen/

Factory preset conversion utilities. Converts original patch data to the C++ arrays in `KR106_Presets_JUCE.h`.

### osc-compare/

Oscillator comparison tool (polyBLEP vs wavetable).

### adsr-calibration/

ADSR envelope timing measurements and curve fitting data from Juno-6 hardware.
