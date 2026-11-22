# SMC-Mixer: mapping model, config format and editor protocol (draft)

Status: **draft**, 2026-11-22. Nothing here is implemented yet. Items marked **TBD** wait for the
capture of the stock firmware's DAW mode (fw `SMC-Mixer_013`) or for the hardware map.

## Goal

The stock firmware has two modes: **DAW mode** (fixed, Mackie-Control-style, not editable) and
**CC mode** (every control remapped by hand in Midi Suite). Ours has one model in which **DAW mode is
the base and anything can be overridden**, per page and per Shift layer:

- Controls that already do the right thing in DAW mode (Play, Stop, ...) need no configuration.
- **Pages:** 8 parameter pages; knobs and faders (and anything else) can map to something different
  on each page.
- **Shift layer:** every control can send something else while Shift is held.
- Edited in a **web editor** (WebMIDI), no Midi Suite / Wine needed.

## Controls

| group | count | id range | notes |
|---|---|---|---|
| knob | 8 | `K1..K8` | endless encoders (Midi Suite has a "Speed" setting) -- TBD: push switch? |
| fader | 9 | `F1..F9` | not motorized; F1 is the leftmost (TBD: master?) |
| track button | 8 x 4 | `T1.1..T8.4` | column x row, top row = 1; have LEDs |
| global button | 11 + 2 | `G1..G13` | bottom row left to right, then the two ovals top right -- TBD names |
| LED | TBD | | "the light above every fader" -- TBD which LEDs those are |

Shift and the arrow buttons are among the global buttons (TBD which).

## Resolution order

For a control event on page `p` (1..8), with Shift held or not, the firmware takes the **first
layer that defines the control**:

1. page `p`, Shift layer (only when Shift is held)
2. page `p`, normal layer
3. all pages, Shift layer (only when Shift is held)
4. all pages, normal layer
5. **DAW default** (what the stock DAW mode sends: table TBD from the capture)

A mapping can also be **"disabled"** (send nothing), which is different from "not defined" (inherit).

Reserved, not remappable: **Shift** itself and **Shift + Left / Shift + Right** (page down / up).
Plain Left / Right keep their DAW meaning (bank) unless overridden.

## Pages

- 8 pages, current page kept across power cycles.
- Shift + Right: page + 1, Shift + Left: page - 1 (no wrap-around -- TBD).
- On a page change the LED above fader `p` blinks for ~1 s (TBD: which LED, rate), then the LEDs go
  back to what they showed (DAW feedback or the button LEDs of the new page).
- The firmware remembers, per page, the last value it sent for each absolute control, so coming back
  to a page continues from there.

### Faders after a page change: jump or pickup

Faders are not motorized, so after a page change a fader's position and the parameter's last value
differ. Setting `fader_takeover`:

- `jump`: send the fader position immediately (the value jumps).
- `pickup`: send nothing until the fader crosses the stored value for this page, then follow it.

A **global** value, overridable **per page and per fader mapping** (`inherit` | `jump` | `pickup`).
Knobs are endless encoders and do not need it.

## Mapping

| field | values | applies to |
|---|---|---|
| `type` | `inherit`, `disabled`, `cc`, `note`, `pc`, `pitchbend`, `daw` | all |
| `channel` | 1..16 | all but inherit/disabled |
| `number` | 0..127 (CC / note / program) | cc, note, pc |
| `min`, `max` | 0..127 (0..16383 for pitchbend) | absolute controls, button values |
| `encoder` | `absolute`, `relative-2c`, `relative-bin`, `relative-sb` (TBD list) + `speed` | knobs |
| `button` | `momentary`, `toggle`, `trigger` | buttons |
| `led` | `follow` (lit while on), `off`, `on`, `daw` (DAW feedback) | buttons with LEDs |
| `takeover` | `inherit`, `jump`, `pickup` | faders |

`daw` sends the control's DAW-mode message even on a page/layer where it would otherwise be
overridden (useful in the Shift layer: "Shift + this knob = the DAW's pan").

## Storage on the device (sparse)

Most controls inherit, so only overrides are stored: a header + a list of entries.

```
header   magic "SMXC", u16 version, u16 n_entries, u8 globals[...], u32 crc32
entry    u8 control, u8 layer, u8 type, u8 channel, u8 number, u8 flags, u16 min, u16 max  (10 B)
         layer: bits 0..3 page (0 = all pages, 1..8), bit 4 shift
```

62 controls x 18 layers would be ~11 KB if every slot were set; real configs are far smaller.
Lives in its own flash sectors of our firmware's area (TBD with the layout of our bootloader/app on
the Mixer), written double-buffered so a power cut never loses the config.

## Editor protocol (SysEx)

Separate from the stock SINCO protocol. Manufacturer ID `7D` (non-commercial / educational),
then a device byte and a command; 8-bit payloads 7-bit packed like SINCO (LSB first).

```
F0 7D 4D <cmd> <seq> <payload, 7-bit packed> F7       4D = "M" (Mixer)
```

| cmd | direction | meaning |
|---|---|---|
| `01` | host -> dev | hello: reply firmware version, config version, control count, page |
| `02` | host -> dev | read config block (offset, length <= 256) |
| `03` | host -> dev | write config block into the RAM copy |
| `04` | host -> dev | commit: check CRC, store to flash, apply |
| `05` | host -> dev | revert RAM copy to the stored config |
| `06` | host -> dev | factory reset (empty override list) |
| `10` | dev -> host | event: control moved / page changed (editor's live view, opt-in) |
| `7F` | dev -> host | reply: status + data |

## Web editor

- Static page in this repo (`editor/`), WebMIDI (Chrome / Edge / Chromium; SysEx permission).
- Midi-Suite-like device drawing; click a control to edit it.
- **Page selector 1..8 + "all pages"**, **Normal / Shift** toggle; every control shows where its
  current value comes from (this layer / inherited from ... / DAW default).
- Global settings (fader takeover, page LED blink).
- Live MIDI monitor, JSON import / export, a simulated device for working without the hardware.

## Open questions

- DAW-mode defaults per control (capture of fw 013 DAW mode).
- Button names, which buttons are Shift / Left / Right; which LEDs sit above the faders.
- DAW banking with plain arrows in our firmware: reproduce the stock messages exactly (TBD).
