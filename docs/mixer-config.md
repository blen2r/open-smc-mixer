# SMC-Mixer: mapping model, config format and editor protocol (draft)

Status: implemented, 2026-11-23, except where a section says "later" or **TBD**. The binary format
is authoritative in `fw/smcpad/examples/mixer/mixer_config.h`; this document explains the model.

## Goal

The stock firmware has two modes: **DAW mode** (fixed, Mackie-Control-style, not editable) and
**CC mode** (every control remapped by hand in Midi Suite). Ours keeps the two modes (Shift + Left
= DAW, Shift + Right = CC, DAW by default) and lets anything be overridden in either, per page and
per Shift layer:

- **DAW mode** sends what the stock DAW mode sends (table below) unless a DAW-mode mapping says
  otherwise.
- **CC mode** has a **profile** (which CCs the faders, encoders and strip buttons send, see "CC mode
  profile") that a **template** sets for a device, plus CC-mode mappings on top.
- **Pages:** 8 parameter pages; encoders and faders (and anything else) can map to something different
  on each page.
- **Shift layer:** every control can send something else while Shift is held.
- Edited in a **web editor** (WebMIDI), no Midi Suite / Wine needed.

## Controls

| group | count | id range | notes |
|---|---|---|---|
| encoder | 8 | `E1..E8` | endless rotary encoders ("knobs"), no push switch; LED ring/indicator driven by the host |
| fader | 8 | `F1..F8` | not motorized; F1 is the leftmost (no master fader) |
| track button | 8 x 4 | `T1.1..T8.4` | column x row, top row = 1; have LEDs |
| global button | 11 | `G1..G11` | bottom row, left to right: Play, Pause, Record, Rewind, Fast-forward, `<<`, `>>`, Up, Down, Left, Right |
| oval button | 2 | `O1`, `O2` | top right: O1 = Bluetooth, O2 = **Shift** (neither sends MIDI) |
| fader LED | 8 | `L1..L8` | under each encoder / above each fader; stock: **blinks** while fader position != host value (tight tolerance), off when they match |

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
| T*n*.4 (bottom row) | Note 24+*n*-1 (MCU Select; the button is Track stop) |
| Play / Pause / Record | Note 94 / 93 / 95 (MCU Play / Stop / Record) |
| Rewind / Fast-forward | Note 91 / 92 |
| `<<` / `>>` | Note 46 / 47 (MCU Fader bank left / right) |
| Up / Down / Left / Right | Note 96 / 97 / 98 / 99 (MCU cursor keys) |
| Shift, Bluetooth | nothing |

Buttons: Note on velocity 127 on press, Note off on release.
Shift has no effect in stock DAW mode (Shift + Up / Down send the same as Up / Down). Shift + Left /
Right switches DAW / CC mode silently (no MIDI); the lit arrow shows the mode.
LEDs (track buttons, encoder indicators) are driven by the host (MCU feedback), not locally.

## Resolution order

For a control event on page `p` (1..8), with Shift held or not, the firmware takes the **first
layer that defines the control**:

1. page `p`, Shift layer (only when Shift is held)
2. page `p`, normal layer
3. all pages, Shift layer (only when Shift is held)
4. all pages, normal layer
5. **the mode itself**: DAW mode's stock messages (the table above), or CC mode's profile

Mappings belong to one mode (layer bit 5); the other mode does not see them.

A mapping can also be **"disabled"** (send nothing), which is different from "not defined" (inherit).

Reserved, not remappable: **Shift** itself, **Shift + Rewind / Shift + Fast-forward** (page down /
up) and **Shift + Left / Shift + Right** (mode switch, as in the stock firmware, so existing users
are not surprised). Plain Rewind / FF and the arrows keep their DAW meaning unless overridden;
`<<` / `>>` are the DAW's bank buttons. Shift + Up / Down is free (remappable).

### Modes

Shift + Left = **DAW**, Shift + Right = **CC**, like stock: while Shift is held the arrow of the
current mode is lit. The mode is a field of the stored configuration (`mode`), written when it
changes, so it is kept across power cycles; the editor shows and sets it too (and on Save keeps a
mode switched on the mixer since it was read). Factory default: DAW.

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
Encoders are endless and do not need it.

### Fader mismatch LED

Each fader has an LED above it (the lights under the encoders). Unlike the stock DAW mode (which blinks), it is **solid** while
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

## CC mode profile

What CC mode does with a control nothing is mapped on (configuration fields `cc_*`, editor:
Settings → CC mode profile). Strip *n* of page *p* = track / MIDI channel `track_base[p] + n`.

| field | control | does |
|---|---|---|
| `cc_fader` | fader | this CC on the strip's channel |
| `cc_fader_shift` | Shift + fader | this CC while Shift is held (none: as without Shift) |
| `cc_mute` | Mute | toggles this CC 127 / 0; LED = muted (none: Mute sends nothing) |
| `solo_mutes` | Solo | toggles solo by muting (`cc_mute`) the other tracks while any is soloed |
| `cc_encoder` | encoder | this CC on the strip's channel (absolute, the mixer keeps the value; none: nothing) |
| `focus_rec` | Rec | focus group: 8 slots, one per encoder, each a CC or none. Rec on a strip makes every encoder with a CC send it on that strip's track; the others stay as they are; again: off. All none: Rec sends nothing |
| `focus_stop` | Track stop | the same, a second group |
| `special_modes` | Shift + Mute / Solo / Rec / Track stop | the fader's special modes (below) |
| | transport, arrows | the stock DAW notes |

**Focus groups.** One strip per group at a time (Rec on another strip moves it); the button's LED
is lit while its group is on that strip. Both groups can be on together when they use different
encoders (the OP-XY template: Rec = encoders 1-4, Track stop = 5-8). When they share an encoder only
one can be on: turning one on blinks both buttons fast for half a second, then the other goes off.
Encoders with their own mapping are not affected by focus.

`name` records the template the profile came from (28 ASCII characters); the editor clears it
when the profile is edited by hand. The factory profile is the "OP-XY - blen2r" template's.

### Templates

`editor/templates/<name>.json` (shared) and `editor/templates/local/` (personal, git-ignored), each
with a `<name>.md` describing the mapping; `host/templates_index.py` checks them and writes the
`index.json` lists the editor reads. A template carries the profile, `pages`, `track_base`,
`follow_incoming` and CC-mode mappings; applying one replaces exactly those (DAW mode and the
personal settings stay). Names contain ` - <author>`. See `editor/templates/README.md`.

## OP-XY (the template "OP-XY - blen2r", 2026-11-22)

The mixer drives a Teenage Engineering OP-XY **over Bluetooth MIDI** (the OP-XY scans and connects:
the mixer is the BLE peripheral, as the stock firmware); one MIDI channel per OP-XY
track (16). Official OP-XY CC table (guide section 23, "midi cc reference table"): track volume
CC 7, track mute CC 9, track pan CC 10, all on channels 1-16; no solo CC. The user uses CC 12-15
for a track's 4 parameters (the table itself lists "track parameters" as CC 46 -- to check).

Strip *n* = OP-XY track / channel *n*; page 2 covers tracks 9-16. As a profile: `cc_fader` 7,
`cc_fader_shift` 10, `cc_mute` 9, `solo_mutes`, `cc_encoder` 12, `special_modes`, `focus_rec`
encoders 1-4 = CC 12-15, `focus_stop` encoders 5-8 = CC 12-15.

| control | does |
|---|---|
| fader *n* | volume: CC 7, channel *n* (pickup + mismatch LED as everywhere) |
| Mute *n* | toggles mute: CC 9 = 127 / 0 on channel *n*; LED = muted |
| Solo *n* | toggles solo -- the OP-XY has none, so the mixer does it: it mutes (CC 9) the other tracks and restores them when solo goes off; LED = soloed (suggestion, TBD) |
| Play | play / stop toggle (the stock DAW note 94 worked on the OP-XY; whether to send MIDI Start / Stop instead is TBD) |
| encoder *k* | by default its own mapping from the editor; if none: CC 12 on channel *k*. Encoders are relative, the OP-XY wants absolute values: the mixer keeps the value per (channel, CC) and sends it |
| Rec *n* | **focus group Rec**: encoders 1-4 send CC 12-15 on channel *n*. One strip at a time (Rec on another strip moves it); LED lit while focused; pressing it again goes back to the default mappings |
| Track stop *n* | **focus group Track stop**: same for encoders 5-8 |

### Values coming back (implemented, 2026-11-22)

A CC / pitch bend / channel pressure the mixer **receives** (BLE or USB) becomes that control's
known value: a fader on it has to pick it up again and its LED shows the mismatch; an encoder
continues from it. Input never triggers output (no loops); a value we are sending on that very
control right now (within 1.5 s, inside the range we just sent) is taken as our own echo and
ignored. The values are also kept across power cycles (saved ~5 s after the last change).

- Editor: **"follow incoming values"** on / off (global; per mapping later if needed).

### Sync with the OP-XY (later; the user runs a custom OP-XY firmware)

The user will add to the OP-XY: answer a request for all current values, and send them by
itself on pairing, program change and project change. On the mixer side:

- a request on connect (and from a button combo / the editor): SysEx in our protocol, e.g.
  `F0 7D 4D 20 F7` ("send me your values") -- final bytes TBD with the OP-XY side;
- the answer can simply be the ordinary CCs (volume, mute, pan, CC 12-15 per track): they go
  through "values coming back" above, so pickup and the LEDs follow;
- mute state should follow too (CC 9 in sets the Mute LED) -- TBD with solo-by-muting.

### Fader special modes (Shift + strip button), toggles, LED blinks while active

| combo | fader *n* then sends (channel *n*) |
|---|---|
| Shift + Mute | mod wheel (CC 1) |
| Shift + Solo | pitch bend (14 bit; center detent behaviour TBD) |
| Shift + Rec | suggestion: channel pressure (aftertouch) |
| Shift + Track stop | suggestion: pan (CC 10, latched) |
| Shift + move a fader (no button) | suggestion: pan (CC 10) while Shift is held, momentary |

Shift + fader can also be mapped **per fader** in the editor, like a macro control: e.g. Shift +
fader 1 = CC 13 on channel 12 (a parameter of track 12 at hand from any strip). A per-fader Shift
mapping replaces the momentary-pan default for that fader only.

**"Applies to all pages"** (editor toggle on any mapping): stores it in the *all pages* layer
(layer 0, see the resolution order) instead of the current page, so a macro set once works on
every page; a page can still override it.

Pressing the same combo again returns the fader to normal (volume). Each mode keeps its own
value for pickup / mismatch LED.

These are behaviours more than plain mappings (focus groups, solo-by-muting, special modes): the
mapping model gets a few "behaviour" types for them, configurable in the editor, with this
profile as the default layout.

### Step-sequencer pages (later)

Idea from the user (2026-11-22, inspired by "Take a look inside Richie Hawtin's DJ setup", Native
Instruments, YouTube `g7Wvq8PN-84`, around 8:00). Kept simple: any of the 8 pages can be a
**step sequencer** instead of a mapping page, with its own MIDI channel.

- Page type `mapping` (default) | `sequencer`; a sequencer page has an output channel.
- **8 steps, one per strip:**
  - **Mute** button of strip *n*: step *n* on / off (its LED shows it);
  - **fader** *n*: the note step *n* plays.
- **Step parameters on the same faders while a button row is held:**
  - hold **Solo** + move the fader: velocity;
  - hold **Rec** + move the fader: length (gate);
  - hold **Track stop** + move the fader: probability.
- The faders are not motorized, so every view (note / velocity / length / probability) reuses the
  **fader mismatch LED** (solid while the fader is not at the step's stored value, off once it is)
  and the **pickup** behaviour: a fader only changes a value after it has reached it, so switching
  views or pages never overwrites values by accident.
- Sequences keep running while another page is shown, so several sequencer pages can play at once
  on different channels.
- Clock: MIDI clock from the host over USB (follows the DAW's tempo and start / stop).

TBD: does holding *any* Solo (Rec, Track stop) switch all 8 faders to that parameter, or only that
strip's button for that strip's fader (the first lets you set several steps at once); note range
of a fader (all 128 notes, or a range / scale); how the playing step is shown (e.g. a moving LED
on the Rec or Track stop row); what the encoders do on a sequencer page (pass through to the mapping,
or per-step extras); pattern length other than 8 (e.g. chaining two pages).

## Mapping

| field | values | applies to |
|---|---|---|
| `type` | `inherit`, `disabled`, `cc`, `note`, `pc`, `pitchbend`, `daw` | all |
| `channel` | 1..16 | all but inherit/disabled |
| `number` | 0..127 (CC / note / program) | cc, note, pc |
| `min`, `max` | 0..127 (0..16383 for pitchbend) | absolute controls, button values |
| `encoder` | `absolute`, `relative-2c`, `relative-bin`, `relative-sb` (TBD list) + `speed` | encoders |
| `accel` | `inherit`, `off`, or a curve (steps closer than 6 / 12 ms count x3 / x2 by default, editable) | encoders; also a global setting |
| `button` | `momentary`, `toggle`, `trigger` | buttons |
| `led` | `follow` (lit while on), `off`, `on`, `daw` (DAW feedback) | buttons with LEDs |
| `takeover` | `inherit`, `jump`, `pickup` | faders |

`daw` sends the control's DAW-mode message even on a page/layer where it would otherwise be
overridden (useful in the Shift layer: "Shift + this encoder = the DAW's pan").

## Storage on the device (sparse)

Most controls inherit, so only overrides are stored: an 80-byte header (settings, CC profile,
template name, focus groups, CRC) + up to 238 entries of 10 bytes, in the VM (key 2, 2464 bytes
whatever the version). Layout: `mixer_config.h`. Entry layer: bits 0..3 page (0 = all pages),
bit 4 Shift, bit 5 DAW mode. Version 2 (64-byte header, one `cc_focus` CC for both groups, up to
240 entries) is converted on the first boot of a firmware that knows version 3: `cc_focus` *c*
became Rec = encoders 1-4 on *c*..*c*+3, Track stop = encoders 5-8 on the same. (Version 1, key 1,
was converted to version 2 by the firmwares of that time; version 3 firmwares no longer read it.)
The editor and `host/mixer_cfg.py` convert older exports and templates the same way (`cc_knob`
became `cc_encoder`).

## Editor protocol (SysEx)

**Implemented (2026-11-22)**, as below with these details: 8-bit data is sent 8-to-7 packed (one
byte with the top bits of up to 7 bytes, then those bytes' low 7 bits); the binary layout of the
configuration is in `fw/smcpad/examples/mixer/mixer_config.h` (version 3: header 80 bytes,
entries 10 bytes, up to 238); hello also returns the current mode; `70` enters the bootloader, `71`
returns echo diagnostics; `host/mixer_cfg.py` and the editor (`editor/`) speak it.

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
- **Mode bar DAW / CC**, **page selector 1..8 + "all pages"**, **Normal / Shift** toggle; every
  control shows where its current value comes from (this layer / inherited from ... / the mode).
- Settings: the mixer's mode, the CC profile, **Choose a template…** (search, favourites, the
  template's `.md`), **Save as template…** (downloads the `.json` and a started `.md`).
- Global settings (fader takeover, fader mismatch LED, page LED blink, encoder acceleration: on / off
  and its curve, overridable per encoder).
- Live MIDI monitor, JSON import / export, a simulated device for working without the hardware.

## Open questions

- Which LEDs sit above the faders; which LEDs the host can drive with MCU feedback.
- DAW banking with plain arrows in our firmware: reproduce the stock messages exactly (TBD).
