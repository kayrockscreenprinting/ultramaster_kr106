# TODO

## Bugs

### Hold + Arp note not released on Hold-off
**Steps to reproduce:**
1. Enable Hold
2. Click Middle C → note sounds
3. Enable Arp → arp arpegiates C
4. Disable Arp → arp stops, Middle C held
5. Disable Hold → **Middle C keeps sounding** (key released visually but audio continues)

**Normal Hold-off works fine. The bug only occurs after an Arp enable/disable cycle.**

Likely in the interaction between `SetParam(kArpeggio)` restore-as-held logic and
`ReleaseHeldNotes()` in `KR106_DSP.h`. `mHeldNotes` may not be populated correctly
or NoteOff is not reaching the synth voice when hold is toggled off after arp was used.
