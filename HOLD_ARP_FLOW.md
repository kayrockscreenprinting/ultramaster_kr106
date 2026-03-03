# Hold + Arp Interaction Flow (GUI Keyboard Only)

Scope: only mouse clicks on the GUI keyboard, the HOLD button, and the ARP button.
Ignores: external MIDI, QWERTY keyboard, host transport.

---

## Key Data Structures

| Where | Name | Type | Purpose |
|-------|------|------|---------|
| KR106Controls.h | `mKeys[61]` | bool array | Visual key state (what's lit on screen) |
| KR106Controls.h | `mPressedKey` | int | Currently mouse-down key (-1 if none) |
| KR106.h | `mKeyboardHeld` | bitset<128> | Audio-thread mirror of visually held notes |
| KR106.h | `mMidiForKeyboard` | queue | Audio→UI: MIDI msgs to update `mKeys` display |
| KR106.h | `mForceRelease` | queue | UI→Audio: individual note releases (bypasses hold) |
| KR106.h | `mHoldOff` | atomic bool | UI→Audio: signal that Hold just turned off |
| KR106_DSP.h | `mHold` | bool | DSP's hold state |
| KR106_DSP.h | `mHeldNotes` | bitset<128> | Notes whose NoteOff was suppressed by hold |
| KR106_DSP.h | `mKeysDown` | bitset<128> | Physical key state (for arp seeding) |
| KR106Arpeggiator.h | `mHeldNotes` | vector<int> | Arp's sorted note pool (what it arpeggites) |
| KR106Arpeggiator.h | `mLastNote` | int | Currently sounding arp note |

---

## Thread Boundary

```
UI THREAD                              AUDIO THREAD
───────────                            ────────────
Mouse handlers                         ProcessBlock()
  SendNoteOn/Off → SendMidiMsgFromUI ─→ ProcessMidiMsg()
  ForceReleaseNote → mForceRelease ───→ drained in ProcessBlock
  OnParamChange (kHold off) → mHoldOff → drained in ProcessBlock

                                        mMidiForKeyboard ──→ OnIdle() → SetNoteFromMidi()
```

---

## Scenario 1: Hold OFF, Arp OFF — Click a key

### MouseDown
1. `KR106KeyboardControl::OnMouseDown` — `mKeys[key] = true`, calls `SendNoteOn(key)`
2. `SendMidiMsgFromUI` delivers NoteOn to audio thread → `ProcessMidiMsg`
3. `ProcessMidiMsg`: `mKeysDown.set(note)`, arp not enabled, hold not on → `SendToSynth(note, true, 127)` — voice plays
4. `ProcessMidiMsg`: `mKeyboardHeld.set(note)`, pushes NoteOn to `mMidiForKeyboard`
5. `OnIdle` drains queue → `SetNoteFromMidi(note, true)` — but `mKeys` was already set in step 1, so no visual change

### MouseUp
1. `OnMouseUp`: hold is off → `mKeys[key] = false`, calls `SendNoteOff(key)`
2. `ProcessMidiMsg`: `mKeysDown.reset(note)`, hold is off → `SendToSynth(note, false, 0)` — voice releases
3. `ProcessMidiMsg`: `mKeyboardHeld.reset(note)`, pushes NoteOff to `mMidiForKeyboard`
4. `OnIdle` drains queue → `SetNoteFromMidi(note, false)` — `mKeys` already cleared in step 1

**Result**: note sounds while mouse is down, stops on release. Clean.

---

## Scenario 2: Hold ON, Arp OFF — Click a key

### MouseDown
1. `OnMouseDown`: `mKeys[key] = true`, calls `SendNoteOn(key)`
2. `ProcessMidiMsg`: `mKeysDown.set(note)`, arp off, hold on but it's a NoteOn → `SendToSynth(note, true, 127)` — voice plays
3. `mKeyboardHeld.set(note)`, pushes NoteOn to `mMidiForKeyboard`

### MouseUp
1. `OnMouseUp`: hold is ON → **does NOT clear** `mKeys[key]` (key stays visually lit)
2. Calls `SendNoteOff(key)` anyway
3. `ProcessMidiMsg`: `mKeysDown.reset(note)`, isNoteOff + `mHold` is true → **suppressed**: `mHeldNotes.set(note)`, returns (no SendToSynth)
4. NoteOff is also suppressed from `mMidiForKeyboard` (the `if (mDSP.mHold)` branch at line 296)
5. `mKeyboardHeld` stays set for this note

**Result**: voice sustains indefinitely, key stays lit. The NoteOff went to `mHeldNotes` so it can be cleaned up later.

---

## Scenario 3: Hold ON, Arp OFF — Click a HELD key (force release)

### MouseDown on already-lit key
1. `OnMouseDown`: `holdOn && mKeys[key]` is true → enters force-release branch
2. `mKeys[key] = false` (visual off immediately)
3. Calls `ForceReleaseNote(key + kMinNote)` → pushes to `mForceRelease` queue
4. Sets `mHoldRelease = true`, `mPressedKey = -1`

### Next ProcessBlock
1. Drains `mForceRelease` → calls `mDSP.ForceRelease(noteNum)`
2. `ForceRelease`: `mHeldNotes.reset(note)`, arp not enabled → `SendToSynth(note, false)` — voice releases
3. `mKeyboardHeld.reset(note)` — prevents spurious NoteOff when hold later turns off

### MouseUp
1. `mPressedKey` is -1 → nothing happens

**Result**: single held note is released. Clean.

---

## Scenario 4: Hold ON, then Hold OFF — releasing all held notes

### User clicks Hold button OFF
1. `OnParamChange(kHold)`: hold is now false → sets `mHoldOff = true`
2. `SetParam(kHold)` in DSP: `mHold = false`, calls `ReleaseHeldNotes()`
3. `ReleaseHeldNotes`: iterates `mHeldNotes`, for each: `SendToSynth(note, false)` — voices release
4. `mHeldNotes.reset()`

### Next ProcessBlock (keyboard visual cleanup)
1. `mHoldOff.exchange(false)` returns true
2. Iterates `mKeyboardHeld`: for each set bit, pushes NoteOff to `mMidiForKeyboard`
3. `mKeyboardHeld.reset()`
4. `OnIdle` drains queue → `SetNoteFromMidi(note, false)` → `mKeys[key] = false` — keys go dark

**Result**: all held voices release, all held keys go dark. Clean.

---

## Scenario 5: Hold ON, Arp ON — Click a key

### MouseDown
1. `OnMouseDown`: `mKeys[key] = true`, calls `SendNoteOn(key)`
2. `ProcessMidiMsg`: `mKeysDown.set(note)`, arp IS enabled + isNoteOn → `mArp.NoteOn(note)`, **returns** (doesn't reach SendToSynth)
3. `mKeyboardHeld.set(note)`, pushes NoteOn to `mMidiForKeyboard`

### Arp takes over
- `Arpeggiator::NoteOn`: inserts note into `mArp.mHeldNotes` (sorted vector)
- If first note: sets `mPhase = 1.0` (immediate trigger on next Process)
- `Arpeggiator::Process` (called each block): steps through sequence, calls `SendToSynth(note, true/false)` via lambdas

### MouseUp
1. `OnMouseUp`: hold ON → `mKeys[key]` stays lit
2. `SendNoteOff(key)`
3. `ProcessMidiMsg`: `mKeysDown.reset(note)`, arp enabled + isNoteOff + `mHold` is true → `mHeldNotes.set(note)`, **returns** (note stays in arp pool)

**Result**: note is added to arp sequence, stays there after mouse-up because hold suppresses NoteOff from reaching `mArp.NoteOff`. Key stays visually lit.

---

## Scenario 6: Hold ON, Arp ON — Click a HELD key (force release from arp)

### MouseDown on already-lit key
1. Same as Scenario 3: `mKeys[key] = false`, `ForceReleaseNote(key + kMinNote)` → queued

### Next ProcessBlock
1. Drains `mForceRelease` → `mDSP.ForceRelease(noteNum)`
2. `ForceRelease`: `mHeldNotes.reset(note)`, arp IS enabled → `mArp.NoteOff(note)`
3. `Arpeggiator::NoteOff`: removes note from `mArp.mHeldNotes` vector
4. If that was the last note: arp sequence empties → `mArp.Process` releases `mLastNote` on next block

**Result**: note removed from arp pool. If it was the only note, arp stops.

---

## Scenario 7: Arp ON, Hold ON — then Arp OFF

### User clicks Arp button OFF
1. `SetParam(kArpeggio)`: `mArp.mEnabled` was true, now false
2. Releases current arp note: `SendToSynth(mArp.mLastNote, false)` — voice releases
3. Hold is on → restores arp's notes as held notes:
   - For each note in `mArp.mHeldNotes`: sends NoteOn to synth, sets `mHeldNotes`
4. `mArp.Reset()` — clears arp state

**Result**: arp stops, but notes transition to plain hold (voices sustain as chords). Keys stay lit.

---

## Scenario 8: Arp OFF, Hold ON (with held notes) — then Arp ON

### User clicks Arp button ON
1. `SetParam(kArpeggio)`: `mArp.mEnabled` was false, now true
2. Seeds arp: `toSeed = mKeysDown | mHeldNotes`
3. For each seeded note: `mArp.NoteOn(note)` + sends NoteOff to synth (releases the chord voice)
4. `mHeldNotes.reset()` — hold tracking cleared, arp now owns the notes

**Result**: held chord transitions to arpeggiated sequence. Keys stay lit.

---

## Scenario 9: Hold ON, Arp ON — then Hold OFF

### User clicks Hold button OFF
1. `OnParamChange(kHold)`: sets `mHoldOff = true`
2. `SetParam(kHold)` in DSP: `mHold = false`, calls `ReleaseHeldNotes()`
3. `ReleaseHeldNotes`: iterates `mHeldNotes`, arp IS enabled → calls `mArp.NoteOff(note)` for each
4. `mHeldNotes.reset()`

### Audio thread (keyboard visual cleanup)
5. `mHoldOff` → pushes NoteOff for each `mKeyboardHeld` bit → `mKeyboardHeld.reset()`
6. `OnIdle` → `SetNoteFromMidi(note, false)` → keys go dark

### But wait — what about notes still physically down?
- `mKeysDown` tracks physical state. If a key is still mouse-down when hold turns off, `mKeysDown` still has it set.
- But the mouse is NOT down (this is a button click on HOLD, not the keyboard). So `mKeysDown` was already cleared on the previous MouseUp.
- **However**: `mKeysDown.reset(note)` happened in `ProcessMidiMsg` on the NoteOff, even though the note stayed in the arp via `mHeldNotes`. So `mKeysDown` is empty.

**Result**: all `mHeldNotes` get removed from arp via `NoteOff`. If those were the only notes in the arp, the arp's `mHeldNotes` vector empties, and on the next `Process()` call it releases `mLastNote`. Arp effectively stops. Keys go dark.

---

## Summary: State ownership

```
                    Hold OFF              Hold ON
                    ────────              ───────
Arp OFF             Synth voices          Synth voices (sustained)
                    NoteOff → release     NoteOff → mHeldNotes (suppressed)

Arp ON              Arp owns notes        Arp owns notes
                    NoteOff → mArp.NoteOff   NoteOff → mHeldNotes (stays in arp)
```

When transitioning:
- **Hold OFF→ON**: no immediate effect (future NoteOffs get suppressed)
- **Hold ON→OFF**: `ReleaseHeldNotes()` releases from synth or arp
- **Arp OFF→ON**: seeds from `mKeysDown | mHeldNotes`, NoteOff to synth, notes go to arp
- **Arp ON→OFF**: if hold on, restores as held chords; if hold off, `mArp.Reset()` stops arp
