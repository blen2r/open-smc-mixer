# CLAUDE.md

Orientation for coding agents (and people) working on this repository: custom firmware for the
M-Vave SMC-Mixer, a fork of fedepaj/open-smc-pad. Read [README.md](README.md) first for what it
does; this file is how to change it without breaking someone's mixer.

The person you are helping may have no firmware experience. Explain what you are about to do to
their device in plain words, and walk them through checking it (see "QA duty").

## Map

| path | what |
|---|---|
| `fw/smcpad/examples/mixer/mixer_app.c` | the firmware's behaviour: modes, CC profile, mappings, faders, encoders, focus groups, buttons, LEDs, MIDI in |
| `fw/smcpad/examples/mixer/mixer_config.{h,c}` | the configuration: binary format (header comment), storage in the VM, resolution, the editor's SysEx protocol |
| `fw/smcpad/examples/mixer/mixer_blemidi.c` | BLE-MIDI peripheral |
| `fw/smcpad/arduino/smcmixer_hw.{h,c}` | hardware: 74HC595 key/LED matrix, encoders, fader ADC + noise filter, timebase |
| `fw/boot/` | bootloader + installer (`BOARD=mixer`); rarely touched |
| `editor/` | web editor: `config.js` (format, shared with node), `device.js` (Web MIDI + simulator), `app.js` (UI) |
| `editor/templates/` | device templates (`.json` + `.md`), `index.json` written by `host/templates_index.py`; `local/` is git-ignored |
| `host/` | `mixer_update.sh` / `mixer_update.py` (flash: Linux / any OS), `mixer_editor.sh` / `mixer_editor.py` (serve editor), `mixer_cfg.py` (config CLI), `mixer_qa.py` + `qa/` (hardware QA), `selftest.py` (offline), `templates_index.py` |
| `docs/mixer-config.md`, `docs/mixer-hardware.md` | the spec and the pin map |
| `QA.md` | how changes are checked; **keep it current** |

Everything else (`arduino/`, `fw/smcpad` outside the mixer files, `docs/` pad documents) is the
upstream SMC-PAD project; leave it alone unless the change is about it.

## Build, flash, test

```sh
python3 host/selftest.py                                                    # offline, always
BOARD=mixer SMCPAD_TINYUSB=1 TEXT_BASE=0x1E08120 fw/build.sh                # compile (Docker)
BOARD=mixer host/package_fw.sh mixerapp                                     # package
host/mixer_update.sh                                                        # flash over USB
python3 host/mixer_qa.py                                                    # guided hardware QA
```

- `package_fw.sh mixerapp` **only packages** `fw/smcpad/out/app.bin`; it does not compile. Always
  run `fw/build.sh` first, or you flash the previous build and test nothing.
- The JieLi toolchain is not redistributable; see `fw/README.md#building`.
- The editor: `host/mixer_editor.sh` (port 8770, no-cache headers). `?sim` = simulated mixer.
  Chrome holds the mixer's raw MIDI port while the editor is connected; tools go through the ALSA
  sequencer (mido), which shares it.

Platforms: Linux is the tested one. `mixer_update.py`, `mixer_editor.py` and the QA script are
meant to work on macOS and Windows (README.md, "Use"); keep them free of Linux-only tools (`amidi`,
`aconnect`), and say "untested" for anything you could not run there.

## Safety rules (the user's hardware)

- Ask before any flash write the user has not asked for. `host/mixer_update.sh` writes only the
  application area and keeps settings; that is the normal path once the user agreed to update.
- **Never** write the bootloader, sector 0, or run the install / back-to-stock packages without the
  user's explicit go-ahead, after showing which flash ranges change. A bad bootloader write can
  need a Raspberry Pi Pico to recover.
- Never drive the fader pins (PC5 PC4 PC2 PA6 PA4 PA3 PA1 PA0): they sit on the fader wipers.
- Never commit flash dumps, vendor firmware (`.fwsc`), or anything under `dumps/`, `vendor/`,
  `work/`, `re/bin`; `.gitignore` covers them, check `git status` anyway.
- Do not push or open pull requests unless asked.

## Changing the configuration format

The format lives in four places that must agree: `mixer_config.h` (+ `.c`), `editor/config.js`,
`host/mixer_cfg.py`, and `docs/mixer-config.md`. Bump `MCFG_VERSION`, convert the previous
version in `mcfg_begin()` (see `convert_v1`: the user's settings must survive an update), update
`host/qa/qa_config.json` and `normalizeConfig()` / `normalize()` for older exports, and run
`host/selftest.py`, which compares the editor's and Python's bytes.

## Pitfalls already paid for

- Timestamps: use `stamp()` in `mixer_app.c`, never `millis() | 1` (can be 1 ms in the future, and
  every "within N ms" test then fails).
- Echo suppression: incoming values for a control we just sent are our own echo; keep the
  `echo_note()` / `echo_of()` calls when adding new outgoing paths, or devices that echo will make
  faders jump.
- Remembered values are saved 5 s after the last change, not on every change (flash wear).
- Faders: a resting fader wobbles ~16 raw counts; the filter in `smcmixer_hw.c` needs 24 counts to
  start or turn back. Don't lower it without the QA run's stray-message checks.
- `pgrep -f pattern` matches its own shell; use `pgrep -f "patter[n]"`.
- The mixer's replies reach every client of its MIDI port; tools drain stale input before a
  command (`mixer_cfg.py`).

## QA duty

- Every behaviour change adds or updates its check in `host/mixer_qa.py` (+ `host/qa/qa_config.json`)
  or in QA.md's manual table, in the same change. QA.md must describe what is actually checked.
- Before telling the user a change is done: run `host/selftest.py`, build, and give the user the
  QA steps for what changed (which guided checks, which manual ones), in plain words. Report what
  passed and what was not tested; never claim hardware behaviour you did not see verified.
- Before the user publishes a firmware or a template: the full QA run, recorded in QA.md's "Runs".
  Help them not publish something broken.

## Templates

A template = CC mode's profile + pages + track bases + follow_incoming + CC-mode mappings, plus
`name` / `device` / `author` / `description`. Names contain ` - <author>` ("OP-XY - blen2r"),
28 ASCII characters at most. Each needs a `.md` saying how it is mapped. After adding one:
`python3 host/templates_index.py`. See `editor/templates/README.md`.

## Style

Comments say why, in plain sentences; the firmware's comments describe the behaviour a user sees.
Keep the editor dependency-free (plain JS, no build step).
