#!/usr/bin/env python3
"""
Generate iPlug2 MakePreset() calls from:
  1. Factory_Patches.pat  — 128 Juno-106 SysEx factory patches (text format)
  2. david_churcher_patches — UltraMaster binary patch file

.pat format (per line):
  18 hex bytes separated by spaces, comma, patch name
  Bytes 0-15: continuous parameters (0-127)
  Byte 16: Switches 1 — see JUNO-106 CONNECTION.html
  Byte 17: Switches 2

Byte 16 (Switches 1):
  bit 0: 16' on/off              (mutually exclusive octave range)
  bit 1: 8'  on/off
  bit 2: 4'  on/off
  bit 3: Pulse on/off
  bit 4: Sawtooth on/off
  bit 5: 0 => Chorus on          (inverted!)
  bit 6: 0 => Chorus level II, 1 => level I

Byte 17 (Switches 2):
  bit 0: 0 => DCO PWM = LFO, 1 => MAN
  bit 1: 0 => VCF Polarity +, 1 => - (inverted)
  bit 2: 0 => VCA ENV, 1 => GATE
  bit 3+4: 00 => HPF=3, 01 => HPF=2, 10 => HPF=1, 11 => HPF=0

iPlug2 EParams order (KR106.h):
  kBenderDco, kBenderVcf, kArpRate, kLfoRate, kLfoDelay,
  kDcoLfo, kDcoPwm, kDcoSub, kDcoNoise, kHpfFreq,
  kVcfFreq, kVcfRes, kVcfEnv, kVcfLfo, kVcfKbd,
  kVcaLevel, kEnvA, kEnvD, kEnvS, kEnvR,
  kTranspose, kHold, kArpeggio, kDcoPulse, kDcoSaw,
  kDcoSubSw, kChorusOff, kChorusI, kChorusII,
  kOctTranspose, kArpMode, kArpRange, kLfoMode, kPwmMode,
  kVcfEnvInv, kVcaMode, kBender, kTuning
"""

import struct
import math
import os


# ── Helpers ──────────────────────────────────────────────────────────────────

def clamp(v, lo, hi):
    return max(lo, min(hi, v))



def linear(n, lo, hi):
    return lo + (hi - lo) * clamp(n, 0.0, 1.0)


def exp_shape(n, lo, hi):
    """IParam::ShapeExp — exponential mapping."""
    return lo * math.pow(hi / lo, clamp(n, 0.0, 1.0))


def pow_curve(n, lo, hi, power=3.0):
    """IParam::ShapePowCurve(p) — power-curve mapping."""
    return lo + (hi - lo) * math.pow(clamp(n, 0.0, 1.0), power)


# ── .pat file reader (Juno-106 SysEx text format) ───────────────────────────

def read_pat_patches(path):
    """Read SysEx text patches: '18 hex bytes,Name' per line."""
    with open(path, 'rb') as f:
        lines = f.read().decode('latin-1').splitlines()

    patches = []
    for line in lines:
        line = line.strip()
        if not line:
            continue
        parts = line.split(',', 1)
        hex_part = parts[0].strip()
        name = parts[1].strip() if len(parts) > 1 else 'Unnamed'

        b = [int(x, 16) for x in hex_part.split()]
        if len(b) != 18:
            continue

        # Continuous params (bytes 0-15): raw 7-bit values (0-127)
        # SysEx bytes are raw ADC from 50KB linear pots.
        # Kept as integers — C++ converts to 0.0-1.0 at load time.
        n = list(b[:16])

        # --- Switches 1 (byte 16) ---
        sw1 = b[16]
        oct_bits = sw1 & 0x07
        pulse   = (sw1 >> 3) & 1
        saw     = (sw1 >> 4) & 1
        chorus_off_bit = (sw1 >> 5) & 1  # 0 = chorus ON (inverted)
        chorus_lvl_bit = (sw1 >> 6) & 1  # 1 = level I, 0 = level II

        # Octave: kOctTranspose 0=16'(down), 1=8'(normal), 2=4'(up)
        if oct_bits & 0x04:       # 4'
            oct_transpose = 2
        elif oct_bits & 0x02:     # 8'
            oct_transpose = 1
        else:                     # 16' or default
            oct_transpose = 0

        # Chorus
        if chorus_off_bit:
            chorus_I, chorus_II = 0, 0
        elif chorus_lvl_bit:
            chorus_I, chorus_II = 1, 0
        else:
            chorus_I, chorus_II = 0, 1

        # --- Switches 2 (byte 17) ---
        sw2 = b[17]
        pwm_man     = sw2 & 1             # 0=LFO, 1=MAN
        vcf_env_inv = (sw2 >> 1) & 1      # 0=positive, 1=negative
        vca_mode    = (sw2 >> 2) & 1      # 0=ENV, 1=GATE
        hpf_bits    = (sw2 >> 3) & 0x03   # 00=3, 01=2, 10=1, 11=0
        hpf = 3 - hpf_bits

        # Sub switch: no separate switch in SysEx; infer from sub level
        sub_sw = 1 if n[15] > 0 else 0

        patches.append({
            'name': name,
            'lfo_rate': n[0], 'lfo_delay': n[1], 'dco_lfo': n[2],
            'dco_pwm': n[3], 'dco_noise': n[4], 'vcf_frq': n[5],
            'vcf_res': n[6], 'vcf_env': n[7], 'vcf_lfo': n[8],
            'vcf_kbd': n[9], 'volume': n[10], 'env_attack': n[11],
            'env_decay': n[12], 'env_sustain': n[13], 'env_release': n[14],
            'dco_sub': n[15],
            'dco_pulse_switch': pulse, 'dco_saw_switch': saw,
            'dco_sub_switch': sub_sw,
            'chorus_I_switch': chorus_I, 'chorus_II_switch': chorus_II,
            'octave_transpose': oct_transpose,
            'vcf_env_invert': vcf_env_inv, 'vca_mode': vca_mode,
            'hpf_frq': hpf, 'dco_pwm_mod': pwm_man,
            # Not stored in SysEx — defaults
            'bender_dco': 0.0, 'bender_vcf': 0.0,
            'arpeggio_rate': 0.0, 'lfo_mode': 0.0,
            'arpeggio_switch': 0.0, 'arpeggio_mode': 0.0,
            'arpeggio_range': 0.0, 'lfo_trigger': 0.0,
        })
    return patches


# ── UltraMaster binary reader ───────────────────────────────────────────────

HEADER_SIZE = 4096
NUM_PATCHES = 100
PATCH_FORMAT = '<i34d64s'
PATCH_SIZE = struct.calcsize(PATCH_FORMAT)
assert PATCH_SIZE == 340

PATCH_FIELDS = [
    'used', 'bender_dco', 'bender_vcf', 'lfo_trigger', 'volume',
    'octave_transpose', 'arpeggio_switch', 'arpeggio_mode', 'arpeggio_range',
    'arpeggio_rate', 'lfo_rate', 'lfo_delay', 'lfo_mode', 'dco_lfo',
    'dco_pwm', 'dco_pwm_mod', 'dco_pulse_switch', 'dco_saw_switch',
    'dco_sub_switch', 'dco_sub', 'dco_noise', 'hpf_frq', 'vcf_frq',
    'vcf_res', 'vcf_env_invert', 'vcf_env', 'vcf_lfo', 'vcf_kbd',
    'vca_mode', 'env_attack', 'env_decay', 'env_sustain', 'env_release',
    'chorus_I_switch', 'chorus_II_switch', 'name'
]


def read_binary_patches(path):
    """Read UltraMaster binary patch file."""
    with open(path, 'rb') as f:
        header_data = f.read(HEADER_SIZE)
    magic = struct.unpack_from('<I', header_data, 0)[0]
    if magic != 0xAFABCEEE:
        raise ValueError(f"{path}: bad magic 0x{magic:08X}")

    patches = []
    with open(path, 'rb') as f:
        f.seek(HEADER_SIZE)
        for _ in range(NUM_PATCHES):
            raw = f.read(PATCH_SIZE)
            values = struct.unpack(PATCH_FORMAT, raw)
            p = dict(zip(PATCH_FIELDS, values))
            p['name'] = p['name'].split(b'\x00')[0].decode('ascii', errors='replace')
            patches.append(p)
    return patches


# ── Parameter conversion ────────────────────────────────────────────────────

def pat_patch_to_params(p):
    """Convert a .pat patch dict to iPlug2 MakePreset() parameter values."""
    chorus_I = p['chorus_I_switch']
    chorus_II = p['chorus_II_switch']
    chorus_off = 1 if (chorus_I == 0 and chorus_II == 0) else 0

    return [
        0,                                             # kBenderDco (not stored)
        0,                                             # kBenderVcf (not stored)
        120,                                           # kArpRate (default BPM)
        p['lfo_rate'],                                 # kLfoRate (raw 7-bit)
        p['lfo_delay'],                                # kLfoDelay
        p['dco_lfo'],                                  # kDcoLfo
        p['dco_pwm'],                                  # kDcoPwm
        p['dco_sub'],                                  # kDcoSub
        p['dco_noise'],                                # kDcoNoise
        p['hpf_frq'],                                  # kHpfFreq (0-3 switch)
        p['vcf_frq'],                                  # kVcfFreq (raw 7-bit)
        p['vcf_res'],                                  # kVcfRes
        p['vcf_env'],                                  # kVcfEnv
        p['vcf_lfo'],                                  # kVcfLfo
        p['vcf_kbd'],                                  # kVcfKbd
        p['volume'],                                   # kVcaLevel
        p['env_attack'],                               # kEnvA (raw 7-bit)
        p['env_decay'],                                # kEnvD
        p['env_sustain'],                              # kEnvS
        p['env_release'],                              # kEnvR
        0,                                             # kTranspose (default off)
        0,                                             # kHold (default off)
        0,                                             # kArpeggio (default off)
        p['dco_pulse_switch'],                         # kDcoPulse
        p['dco_saw_switch'],                           # kDcoSaw
        p['dco_sub_switch'],                           # kDcoSubSw
        chorus_off,                                    # kChorusOff
        chorus_I,                                      # kChorusI
        chorus_II,                                     # kChorusII
        p['octave_transpose'],                         # kOctTranspose (0=4', 1=8', 2=16')
        0,                                             # kArpMode (default)
        0,                                             # kArpRange (default)
        0,                                             # kLfoMode (default)
        p['dco_pwm_mod'],                              # kPwmMode (0=LFO, 1=MAN)
        p['vcf_env_invert'],                           # kVcfEnvInv
        p['vca_mode'],                                 # kVcaMode
        0,                                             # kBender (not stored)
        0,                                             # kTuning (not stored)
        1,                                             # kPower (always on)
        2,                                             # kPortaMode (default Poly)
        0,                                             # kPortaRate (default 0)
        0,                                             # kTransposeOffset (default 0)
        0,                                             # kBenderLfo (default 0)
        1,                                             # kAdsrMode (1=Juno-106)
    ]


def to7bit(v):
    """Convert 0.0-1.0 float to 7-bit integer (0-127)."""
    return int(round(clamp(v, 0.0, 1.0) * 127))


def binary_patch_to_params(p):
    """Convert a UltraMaster binary patch dict to parameter values (ints)."""
    def b(v): return 1 if v > 0.5 else 0

    chorus_off = b(1.0 - max(p['chorus_I_switch'], p['chorus_II_switch']))

    # Juno-6 binary: dco_pwm_mod was 0.0=LFO or 2.0=MAN (no ENV on Juno-6)
    pwm_mode = 0 if p['dco_pwm_mod'] < 0.5 else 1

    return [
        to7bit(p['bender_dco']),                       # kBenderDco
        to7bit(p['bender_vcf']),                       # kBenderVcf
        120,                                           # kArpRate (default BPM)
        to7bit(p['lfo_rate']),                         # kLfoRate
        to7bit(p['lfo_delay']),                        # kLfoDelay
        to7bit(p['dco_lfo']),                          # kDcoLfo
        to7bit(p['dco_pwm']),                          # kDcoPwm
        to7bit(p['dco_sub']),                          # kDcoSub
        to7bit(p['dco_noise']),                        # kDcoNoise
        int(round(p['hpf_frq'] / 0.25)),              # kHpfFreq (0-3)
        to7bit(p['vcf_frq']),                          # kVcfFreq
        to7bit(p['vcf_res']),                          # kVcfRes
        to7bit(p['vcf_env']),                          # kVcfEnv
        to7bit(p['vcf_lfo']),                          # kVcfLfo
        to7bit(p['vcf_kbd']),                          # kVcfKbd
        to7bit(p['volume']),                           # kVcaLevel
        to7bit(p['env_attack']),                       # kEnvA
        to7bit(p['env_decay']),                        # kEnvD
        to7bit(p['env_sustain']),                      # kEnvS
        to7bit(p['env_release']),                      # kEnvR
        0,                                             # kTranspose
        0,                                             # kHold
        b(p['arpeggio_switch']),                       # kArpeggio
        b(p['dco_pulse_switch']),                      # kDcoPulse
        b(p['dco_saw_switch']),                        # kDcoSaw
        b(p['dco_sub_switch']),                        # kDcoSubSw
        chorus_off,                                    # kChorusOff
        b(p['chorus_I_switch']),                       # kChorusI
        b(p['chorus_II_switch']),                      # kChorusII
        2 - int(round(clamp(p['octave_transpose'], 0.0, 2.0))),  # kOctTranspose
        int(round(clamp(p['arpeggio_mode'], 0.0, 2.0))),         # kArpMode
        int(round(clamp(p['arpeggio_range'], 0.0, 2.0))),        # kArpRange
        int(round(clamp(p['lfo_mode'], 0.0, 1.0))),              # kLfoMode
        pwm_mode,                                      # kPwmMode
        int(round(clamp(p['vcf_env_invert'], 0.0, 1.0))),        # kVcfEnvInv
        int(round(clamp(p['vca_mode'], 0.0, 1.0))),              # kVcaMode
        0,                                             # kBender (not stored)
        0,                                             # kTuning (not stored)
        1,                                             # kPower (always on)
        2,                                             # kPortaMode (default Poly)
        0,                                             # kPortaRate (default 0)
        0,                                             # kTransposeOffset (default 0)
        0,                                             # kBenderLfo (default 0)
        1,                                             # kAdsrMode (1=Juno-106)
    ]


# ── Output ───────────────────────────────────────────────────────────────────

def generate(pat_files, binary_files, out_path):
    all_presets = []
    seen_names = set()

    for path, label in pat_files:
        patches = read_pat_patches(path)
        for idx, p in enumerate(patches):
            name = p['name'].strip()
            if not name:
                continue
            # Prefix with Juno-106 bank/patch label: A11-A88, B11-B88
            bank = 'A' if idx < 64 else 'B'
            local = idx if idx < 64 else idx - 64
            patch_label = f'{bank}{local // 8 + 1}{local % 8 + 1}'
            name = f'{patch_label} {name}'
            orig = name
            suffix = 2
            while name in seen_names:
                name = f'{orig} ({suffix})'
                suffix += 1
            seen_names.add(name)
            params = pat_patch_to_params(p)
            all_presets.append((name, params))

    for path, label, name_factory in binary_files:
        patches = read_binary_patches(path)
        factory_idx = 0
        for p in patches:
            if not p['used']:
                continue
            name = p['name'].strip()
            if not name or name.lower().startswith('blank'):
                continue
            if name in ('Undescribed', 'Untitled', 'Undescribed patch'):
                name = name_factory(factory_idx)
                factory_idx += 1
            orig = name
            suffix = 2
            while name in seen_names:
                name = f'{orig} ({suffix})'
                suffix += 1
            seen_names.add(name)
            params = binary_patch_to_params(p)
            all_presets.append((name, params))

    # Output JUCE struct format
    lines = [
        '#pragma once',
        '',
        f'// Auto-generated by tools/preset-gen/gen_presets.py — do not edit by hand.',
        f'// {len(all_presets)} factory presets. Slider values are raw 7-bit (0-127);',
        '// C++ constructor converts to 0.0-1.0 at load time.',
        '',
        'struct KR106FactoryPreset {',
        '    const char* name;',
        '    int values[44];',
        '};',
        '',
        'static constexpr int kNumFactoryPresets = ' + str(len(all_presets)) + ';',
        '',
        'static const KR106FactoryPreset kFactoryPresets[] = {',
    ]
    for name, params in all_presets:
        safe_name = name.replace('\\', '\\\\').replace('"', '\\"')
        args = ', '.join(str(v) for v in params)
        lines.append(f'    {{"{safe_name}", {{{args}}}}},')
    lines.append('};')

    content = '\n'.join(lines) + '\n'
    with open(out_path, 'w') as f:
        f.write(content)
    print(f'Wrote {len(all_presets)} presets to {out_path}')


if __name__ == '__main__':
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(os.path.dirname(script_dir))
    pat_files = [
        (f'{script_dir}/Factory_Patches.pat', 'factory'),
    ]
    binary_files = [
        # (f'{script_dir}/david_churcher_patches', 'churcher', lambda i: f'Churcher {i+1:02d}'),
    ]
    out = f'{project_root}/Source/KR106_Presets_JUCE.h'
    generate(pat_files, binary_files, out)
