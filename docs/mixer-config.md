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
| knob | 8 | `K1..K8` | endless encoders, no push switch; LED ring/indicator driven by the host |
| fader | 8 | `F1..F8` | not motorized; F1 is the leftmost (no master fader) |
| track button | 8 x 4 | `T1.1..T8.4` | column x row, top row = 1; have LEDs |
| global button | 11 | `G1..G11` | bottom row, left to right: Play, Pause, Record, Rewind, Fast-forward, `<<`, `>>`, Up, Down, Left, Right |
| oval button | 2 | `O1`, `O2` | top right: O1 = Bluetooth, O2 = **Shift** (neither sends MIDI) |
| fader LED | 8 | `L1..L8` | under each knob / above each fader; stock: **blinks** while fader position != host value (tight tolerance), off when they match |

## DAW mode defaults (stock fw 013, captured 2026-11-22)

Mackie Control layout. The stock firmware sends everything identically on **both** USB MIDI ports
(`SMC-Mixer-Private` and `SMC-Mixer-Master`); all on channel 1 unless noted.

| control | message |
|---|---|
| K1..K8 | CC 16..23, relative sign-magnitude: `1` = one step right, `65` = one step left (MCU V-Pot) |
| F1..F8 | touch: Note 104..111 on (127) / off when the fader starts / stops moving; value: **pitch bend on channel 1..8**, -8192..8191 |
| T*n*.1 (top row) | Note 16+*n*-1 (MCU Mute 1..8) |
| T*n*.2 | Note 8+*n*-1 (MCU Solo) |
| T*n*.3 | Note 0+*n*-1 (MCU Rec arm) |
| T*n*.4 (bottom row) | Note 24+*n*-1 (MCU Select) |
| Play / Pause / Record | Note 94 / 93 / 95 (MCU Play / Stop / Record) |
| Rewind / Fast-forward | Note 91 / 92 |
| `<<` / `>>` | Note 46 / 47 (MCU Fader bank left / right) |
| Up / Down / Left / Right | Note 96 / 97 / 98 / 99 (MCU cursor keys) |
| Shift, Bluetooth | nothing |

Buttons: Note on velocity 127 on press, Note off on release.
Shift has no effect in stock DAW mode (Shift + Up / Down send the same as Up / Down). Shift + Left /
Right switches DAW / CC mode silently (no MIDI); the lit arrow shows the mode.
LEDs (track buttons, knob indicators) are driven by the host (MCU feedback), not locally.

## Resolution order

For a control event on page `p` (1..8), with Shift held or not, the firmware takes the **first
layer that defines the control**:

1. page `p`, Shift layer (only when Shift is held)
2. page `p`, normal layer
3. all pages, Shift layer (only when Shift is held)
4. all pages, normal layer
5. **DAW default** (what the stock DAW mode sends, see the table above)

A mapping can also be **"disabled"** (send nothing), which is different from "not defined" (inherit).

Reserved, not remappable: **Shift** itself, **Shift + Rewind / Shift + Fast-forward** (page down /
up) and **Shift + Left / Shift + Right** (mode switch, as in the stock firmware, so existing users
are not surprised). Plain Rewind / FF and the arrows keep their DAW meaning unless overridden;
`<<` / `>>` are the DAW's bank buttons. Shift + Up / Down is free (remappable).

### Modes

Shift + Left / Right selects the mode, like stock (the lit arrow shows which). Proposed set (TBD):

- **custom**: DAW base + overrides, pages, Shift layer (the normal mode).
- **plain DAW**: overrides ignored, exactly the stock DAW behaviour (useful to check whether an
  issue comes from the config).

Mode kept across power cycles; the change is shown with an LED pattern (TBD).

## Pages

- 8 pages, current page kept across power cycles.
- Shift + Fast-forward: page + 1, Shift + Rewind: page - 1 (no wrap-around -- TBD).
  (Stock Shift + Rewind / FF most likely send the same as Rewind / FF, as Shift + Up / Down do -- TBD.)
- On a page change the fader LED `Lp` **blinks fast** for ~1 s with the other fader LEDs off,
  then the LEDs go back to what they showed (mismatch lights, button LEDs of the new page).
  Blinking is free for this because our mismatch light is solid.
- The firmware remembers, per page, the last value it sent for each absolute control, so coming back
  to a page continues from there.

### Faders after a page change: jump or pickup

Faders are not motorized, so after a page change a fader's position and the parameter's last value
differ. Setting `fader_takeover`:

- `jump`: send the fader position immediately (the value jumps).
- `pickup`: send nothing until the fader crosses the stored value for this page, then follow it.

A **global** value, overridable **per page and per fader mapping** (`inherit` | `jump` | `pickup`).
Knobs are endless encoders and do not need it.

### Fader mismatch LED

Each fader has an LED above it (the lights under the knobs). Unlike the stock DAW mode (which blinks), it is **solid** while
the fader's physical position differs from the value it should have, and turns off the moment the fader reaches
the value, so you can see exactly when you've hit it (a blinking LED makes that ambiguous):

- **DAW default mapping:** the target is the value the host sends back (MCU pitch bend on the
  fader's channel), as in stock.
- **Our mappings (pages / Shift):** the target is the last value stored for that page and mapping.
  With `pickup` the LED goes off once the fader picks the value up; with `jump` it is only lit
  until the fader is moved.
- Setting `fader_led`: `mismatch` (default) | `off`. TBD: tolerance, and whether the LED can show
  the direction (blink rate / brightness) if the hardware allows it.

The page-change blink (above) uses the same LEDs and takes priority for its ~1 s.

### Step-sequencer pages (later)

Idea from the user (2026-11-22, reference: "Take a look inside Richie Hawtin's DJ setup", Native
Instruments, YouTube `g7Wvq8PN-84`, around 8:00 -- not watched yet, details to confirm): any of the 8
pages can be a **step sequencer** instead of a mapping page, each with its own MIDI channel(s).

Sketch, all TBD:

- Page type `mapping` (default) | `sequencer`. A sequencer page has its own output channel, and
  possibly one channel / note per row.
- The 8 x 4 track buttons are the grid: e.g. 4 rows (instruments / notes) x 8 steps, or one row of
  32 steps; button LEDs show the steps that are set, the playing step runs across as a moving light.
- Faders / knobs per step or per row: velocity, note, gate length, probability (TBD).
- Clock: MIDI clock from the host over USB (follows the DAW's tempo and start/stop); maybe an
  internal clock as a fallback.
- Sequences keep running while another page is shown, so several pages can play at once on
  different channels.
- Storage: patterns next to the config (their own flash sectors), edited on the device and in the
  web editor.

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
- Global settings (fader takeover, fader mismatch LED, page LED blink).
- Live MIDI monitor, JSON import / export, a simulated device for working without the hardware.

## Open questions

- Which LEDs sit above the faders; which LEDs the host can drive with MCU feedback.
- Which modes Shift + Left / Right should select.
- DAW banking with plain arrows in our firmware: reproduce the stock messages exactly (TBD).
