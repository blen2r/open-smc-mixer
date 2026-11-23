# QA

How to check a change before you flash it on someone else's mixer or publish it. A firmware that
was not checked on the hardware should not be shared. Three steps, in this order: offline checks,
the guided hardware run, the manual checks. Record the run at the bottom.

## 1. Offline (no mixer)

```sh
python3 host/selftest.py
```

Templates valid and indexed, the editor and the Python tools agreeing byte for byte on the
configuration format, the firmware header matching. Then build the firmware, which also checks
the header's size at compile time:

```sh
BOARD=mixer SMCPAD_TINYUSB=1 TEXT_BASE=0x1E08120 fw/build.sh && BOARD=mixer host/package_fw.sh mixerapp
```

Editor changes: open `http://localhost:8770/?sim` (`host/mixer_editor.sh`, then add `?sim`), which
runs the editor against a simulated mixer, and click through what you changed. The browser
console must stay free of errors.

## 2. Guided hardware run

Flash the build (`host/mixer_update.sh`), keep any Bluetooth device (the OP-XY) **disconnected**
(the checks send test messages), no strip soloed, no Rec / Track stop focus lit. Then:

```sh
python3 host/mixer_qa.py              # with mido + python-rtmidi (uv run --with mido --with python-rtmidi ...)
```

1. It backs your configuration up to `work/qa_backup_<time>.json` (restored at the end; on a rerun
   it finds your last real backup instead of backing up its own test configuration).
2. **Editor round trip.** You import `host/qa/qa_config.json` in the editor, make six listed
   changes (in both modes), save; the script reads the configuration back and compares it with
   what it should be.
3. **46 hardware checks.** One action at a time ("turn encoder 2 a lot to the right"); the script
   checks every MIDI message the mixer sends on USB, and knows which control you touched and on
   which page (wrong control, wrong page and stray messages are reported as such). A few ask
   yes / no about an LED. Covered:
   - encoders: CC, value ranges, program change, pitch bend, disabled, the CC profile's encoder CC;
   - focus groups: Rec focus on chosen encoders, an empty slot left alone, Track stop sharing an
     encoder (both buttons blink, Rec goes off), focus off;
   - faders: CC, pressure, Shift layer, pickup with the LED, page-only mappings and ranges;
   - buttons: toggle, momentary, trigger, program change, base explicitly, mappings removed and
     added in the editor, the profile's mute;
   - pages: Shift + Rewind / Fast-forward, per-page track bases, the editor's page count, the page
     LED while Shift is held (steady, and in bursts when that fader has a value to pick up);
   - modes: Shift + Left / Right, the arrow LEDs, DAW mode's Mackie messages, per-mode mappings,
     the mode stored in the configuration;
   - no stray messages anywhere (a noisy fader fails every check it is noisy in).

`--no-editor` loads the expected configuration directly; `--from N` resumes at check N.

## 3. Manual checks

Not automated (they need the device, a power cycle, or your eyes):

| # | check | how |
|---|---|---|
| M1 | mode kept across power | Shift + Right (CC), switch off and on: still CC (Shift held: Right lit). Same with Left. |
| M2 | Bluetooth | connect the OP-XY (or any BLE-MIDI host): faders, Mute, Play reach it; the BT LED is steady while connected |
| M3 | template picker | editor: CC mode profile card → Choose a template…: search filters, ★ keeps favourites on top and "★ only" works, "How it's mapped" opens the `.md`, Use + Save to mixer applies it (check a fader on the device) |
| M4 | save as template | Save as template… downloads a `.json` and a `.md`; in `editor/templates/local/` + `host/templates_index.py` they show up as "(local)" |
| M5 | factory reset | editor: Factory reset → DAW mode, profile "OP-XY - blen2r", no mappings |
| M6 | update keeps settings | `host/mixer_update.sh`, then the editor shows the same configuration |
| M7 | resting faders | 30 s without touching anything, the editor's monitor stays empty (also while pressing buttons) |
| M8 | values remembered | "Remember" on: move a fader, wait 6 s, power cycle; with pickup its LED shows the remembered value |
| M9 | page while Shift | CC mode, pickup on, page 1. Hold Shift, move fader 1 to the middle (pan), let go of Shift, move fader 1 to the bottom (volume). Hold Shift: the LED above fader 1 blinks in bursts with solid in between (page 1, pan to pick up). Shift + fader 1 back to the middle: it blinks steadily. Shift + Fast-forward: now the LED above fader 2 blinks |
| M10 | restore | Backup…, then Restore… that file: same configuration; Restore… a template `.json`: applied as a template (DAW mappings and Settings unchanged) |

## Keeping this file current

Every change that adds or changes behaviour adds its check: to `host/mixer_qa.py` (and
`host/qa/qa_config.json`) when it sends MIDI the script can see, or to the manual table above.
Remove checks for behaviour that is gone. A configuration format change also updates
`host/selftest.py`'s expectations.

## Runs

| date | firmware (commit) | result | notes |
|---|---|---|---|
| 2026-11-23 | before modes / templates | 28 / 28 guided | after the fader noise fix |
| 2026-11-23 | 07211bd (modes, templates) | all good (36 guided + manual) | first run with DAW / CC modes and templates |
| 2026-11-23 | d12f4c8 (focus groups, encoders, format v3) | all good (45 guided) | v2 settings converted on the update |
