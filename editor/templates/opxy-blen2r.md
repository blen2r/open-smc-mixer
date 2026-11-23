# OP-XY - blen2r

For: Teenage Engineering OP-XY. By blen2r.

The mixer as a mixing desk for the OP-XY's 16 tracks, connected over Bluetooth MIDI (the OP-XY finds
"SMC-Mixer" when it scans) or USB. Each OP-XY track listens on the MIDI channel of the same number.

## Pages

Two pages: page 1 = tracks 1-8, page 2 = tracks 9-16. Shift + Fast-forward / Rewind change the page;
the LED above the fader of the new page's number blinks for a second.

## CC mode

| control | sends |
|---|---|
| fader | volume: CC 7 on the strip's channel |
| Shift + fader | pan: CC 10, while Shift is held |
| knob | CC 12 (the track's first parameter) on the strip's channel |
| Mute | toggles mute: CC 9 = 127 / 0; the LED shows it |
| Solo | toggles solo: the other tracks are muted (CC 9) while any track is soloed (the OP-XY has no solo CC) |
| Rec | knobs 1-4 send CC 12-15 (the track's four parameters) of this track; again: back to normal |
| Select | knobs 5-8 send CC 12-15 of this track; again: back to normal |
| Shift + Mute / Solo / Rec / Select | the fader becomes mod wheel (CC 1) / pitch bend / channel pressure / pan (CC 10) for this track; the button blinks; the button again ends it |
| Play | note 94: start / stop |
| other transport and arrow buttons | the stock DAW notes (Pause 93, Record 95, << 46, >> 47, Up 96, Down 97) |

## Notes

- The OP-XY does not send its volumes back, so pickup cannot know where a track's volume is until
  you move its fader once (the LED above a fader is lit while it has not caught its value).
- Shift + Left / Right switch between DAW and CC mode; this template is CC mode.
