#!/usr/bin/env python3
"""Generate a MIDI file that tests every Juno-106 SysEx parameter mapping.

Sends an APR (init patch) then changes one parameter at a time via IPR,
plays a note, and waits. The resulting audio recording can be analyzed
to confirm each SysEx control maps to the correct parameter on hardware.

Usage: python3 sysex_param_test.py [output.mid]
  Default output: sysex_param_test.mid

Play this into a real Juno-106 via MIDI and record the audio output.
Each segment should produce a distinct, predictable sound change.
"""

import sys
from midiutil import MIDIFile

output = sys.argv[1] if len(sys.argv) > 1 else "sysex_param_test.mid"

# Juno-106 SysEx format:
# IPR (Individual Parameter): F0 41 32 00 cc vv F7
# APR (All Parameter):        F0 41 30 00 pp [16 sliders] [sw1] [sw2] F7
# Switch bytes:
#   sw1: oct 8' (0x02), pulse on (0x08), saw on (0x10), chorus off (0x20)
#   sw2: PWM manual (0x01), env+ (0x00), VCA env (0x00), HPF flat (0x18)

CHANNEL = 0  # MIDI channel 0
NOTE = 60    # C4
VELOCITY = 127
BPM = 120

# SysEx control numbers and their parameter names
# From the Juno-106 service manual / our firmware analysis
# Each entry: (ctrl, name, init_val, test_val, extra_setup, extra_teardown)
# extra_setup/teardown are lists of (ctrl, val) pairs to make the param audible.
SYSEX_PARAMS = [
    (0x00, "LFO Rate",    0,   127,  [(0x02, 127)],           [(0x02, 0)]),   # need DCO LFO to hear rate
    (0x01, "LFO Delay",   0,   127,  [(0x00, 80), (0x02, 127)], [(0x00, 0), (0x02, 0)]),  # need rate+depth
    (0x02, "DCO LFO",     0,   127,  [(0x00, 80)],            [(0x00, 0)]),   # need LFO rate to hear depth
    (0x03, "DCO PWM",     64,  127,  [],                       []),
    (0x04, "DCO Noise",   0,   127,  [],                       []),
    (0x05, "VCF Freq",    64,  0,    [],                       []),            # fully closed
    (0x06, "VCF Res",     0,   127,  [],                       []),
    (0x07, "VCF Env",     0,   127,  [(0x0D, 0), (0x0C, 40)],  [(0x0D, 127), (0x0C, 64)]),  # drop sustain, short decay so env sweeps
    (0x08, "VCF LFO",     0,   127,  [(0x00, 80)],            [(0x00, 0)]),   # need LFO rate to hear filter wobble
    (0x09, "VCF KBD",     0,   127,  [],                       []),
    (0x0A, "VCA Level",   64,  127,  [],                       []),
    (0x0B, "ENV Attack",  0,   127,  [],                       []),
    (0x0C, "ENV Decay",   64,  127,  [],                       []),
    (0x0D, "ENV Sustain", 127, 0,    [],                       []),
    (0x0E, "ENV Release", 40,  127,  [],                       []),
    (0x0F, "DCO Sub",     0,   127,  [],                       []),
]

# Init patch: a simple saw wave, mid cutoff, no modulation
INIT_SLIDERS = [0] * 16  # all zero
INIT_SLIDERS[0x03] = 64   # DCO PWM at center
INIT_SLIDERS[0x05] = 64   # VCF Freq mid
INIT_SLIDERS[0x0A] = 64   # VCA Level center
INIT_SLIDERS[0x0B] = 0    # Attack instant
INIT_SLIDERS[0x0C] = 64   # Decay medium
INIT_SLIDERS[0x0D] = 127  # Sustain full
INIT_SLIDERS[0x0E] = 40   # Release short

# sw1: oct 8' (0x02) + saw on (0x10) + chorus off (0x20) = 0x32
INIT_SW1 = 0x32
# sw2: PWM manual (0x01) + env+ (0x00) + VCA env (0x00) + HPF flat (0x18) = 0x19
# bit 0x04: 0=ENV, 1=GATE
INIT_SW2 = 0x19


def make_apr(patch_num=0):
    """Build a 24-byte APR SysEx message."""
    data = [0xF0, 0x41, 0x30, CHANNEL, patch_num]
    data.extend(INIT_SLIDERS)
    data.extend([INIT_SW1, INIT_SW2])
    data.append(0xF7)
    return bytes(data)


def make_ipr(control, value):
    """Build a 7-byte IPR SysEx message."""
    return bytes([0xF0, 0x41, 0x32, CHANNEL, control, value, 0xF7])


midi = MIDIFile(1, eventtime_is_ticks=False)
midi.addTempo(0, 0, BPM)
midi.addTrackName(0, 0, "KR106 SysEx Param Test")

t = 0.0  # time in beats
GAP = 0.5       # gap before SysEx (beats)
SYSEX_GAP = 0.25  # gap after SysEx before note (let CPU process)
NOTE_DUR = 2.0  # note duration (beats)
TAIL = 1.5      # silence after note-off

# --- Send APR to set init patch ---
midi.addSysEx(0, t, 0x41, make_apr()[2:-1])  # midiutil wraps with F0/F7
t += 1.0  # let it settle

# --- Play init patch reference note ---
midi.addNote(0, CHANNEL, NOTE, t, NOTE_DUR, VELOCITY)
t += NOTE_DUR + TAIL

# --- Test each parameter ---
for ctrl, name, init_val, test_val, setup, teardown in SYSEX_PARAMS:
    # Send extra setup params first (e.g. LFO rate for modulation tests)
    for s_ctrl, s_val in setup:
        ipr = make_ipr(s_ctrl, s_val)
        midi.addSysEx(0, t, 0x41, ipr[2:-1])
        t += SYSEX_GAP

    # Send IPR to change the parameter under test
    ipr_payload = make_ipr(ctrl, test_val)
    midi.addSysEx(0, t, 0x41, ipr_payload[2:-1])
    t += SYSEX_GAP

    # Play a note to hear the change
    midi.addNote(0, CHANNEL, NOTE, t, NOTE_DUR, VELOCITY)
    t += NOTE_DUR + TAIL

    # Reset parameter back to init value
    reset_payload = make_ipr(ctrl, init_val)
    midi.addSysEx(0, t, 0x41, reset_payload[2:-1])
    t += SYSEX_GAP

    # Tear down extra params
    for s_ctrl, s_val in teardown:
        ipr = make_ipr(s_ctrl, s_val)
        midi.addSysEx(0, t, 0x41, ipr[2:-1])
        t += SYSEX_GAP
    t += GAP

# --- Final: send APR again to confirm full reset ---
midi.addSysEx(0, t, 0x41, make_apr()[2:-1])
t += 1.0
midi.addNote(0, CHANNEL, NOTE, t, NOTE_DUR, VELOCITY)
t += NOTE_DUR + TAIL

total_seconds = t / BPM * 60
print(f"Generated {output}")
print(f"  {len(SYSEX_PARAMS)} parameters tested")
print(f"  Total duration: {total_seconds:.0f} seconds ({t:.1f} beats at {BPM} BPM)")
print(f"  Structure: APR init -> reference note -> 16x (IPR change, note, IPR reset) -> APR reset -> final note")
print(f"  Expected: first and last notes should sound identical (init patch)")
print(f"  Each test note should differ from init in exactly one way:")
for ctrl, name, init_val, test_val, setup, teardown in SYSEX_PARAMS:
    extra = f"  (with {', '.join(f'0x{c:02X}={v}' for c,v in setup)})" if setup else ""
    print(f"    0x{ctrl:02X} {name}: {init_val} -> {test_val}{extra}")

with open(output, "wb") as f:
    midi.writeFile(f)
