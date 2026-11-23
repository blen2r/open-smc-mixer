<h1 align="center">open-smc-mixer</h1>

<p align="center"><b>Open firmware for the M-Vave SMC-Mixer</b><br>
DAW and CC modes, pages, a Shift layer, device templates and a web editor, for an 8-fader MIDI controller</p>

<p align="center">
<a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT"></a>
<img src="https://img.shields.io/badge/chip-JieLi%20AC636N%20(BR25)-555" alt="JieLi AC636N">
<img src="https://img.shields.io/badge/USB-MIDI%20%C2%B7%20DFU-555" alt="USB">
<img src="https://img.shields.io/badge/Bluetooth-BLE--MIDI-555" alt="BLE-MIDI">
</p>

> **No maintenance, no support.** This is one person's firmware for their own SMC-Mixer, shared
> as it is. Issues are off. Pull requests may be reviewed when there is time, which may be never;
> pull requests that add [device templates](editor/templates/README.md) are the most welcome.
> This is not meant to be the place for every SMC-Mixer firmware: fork it and make your own on
> top of it, that is what it is for.
>
> Replacing your mixer's firmware is at your own risk. The way back to the stock firmware exists
> but has only been tested in simulation (see [Back to stock](#back-to-stock)).

It is a fork of [fedepaj/open-smc-pad](https://github.com/fedepaj/open-smc-pad), the open
firmware for the SMC-PAD (same chip family), whose bootloader, runtime, USB and BLE work this is
built on. The SMC-PAD parts are still in this tree, untouched; for them, see the original
repository.

## Features

- **DAW mode**, the default: the stock firmware's Mackie Control layout (faders as pitch bend
  with touch notes, encoders as V-Pots, buttons as notes, LEDs driven by the host).
- **CC mode**: each strip is a track on its own MIDI channel. What the faders, encoders and strip
  buttons send comes from a **profile** you set in the editor or load from a **template**:
  - fader: a CC (volume), Shift + fader: another (pan);
  - Mute toggles a CC; Solo mutes the other tracks (for devices without a solo CC);
  - Rec / Select: two focus groups, each turning any of the 8 encoders to CCs of that track
    (both buttons blink when the groups share an encoder and one takes over);
  - Shift + Mute / Solo / Rec / Select turn a fader into mod wheel / pitch bend / pressure / pan.
- **Shift + Left / Right** switch DAW / CC mode, as on the stock firmware (the lit arrow shows the
  mode while Shift is held). The mode is kept across power cycles.
- **Pages**: up to 8, Shift + Rewind / Fast-forward; each page says which tracks the strips are.
- **Mappings** for any control: CC, note, program change, pitch bend, channel pressure or nothing;
  per mode, per page or all pages, with or without Shift; buttons momentary, toggle or trigger;
  value ranges.
- **Faders**: jump or pickup (global, or per mapping); the LED above a fader stays lit while the
  fader is not at its value and goes off when it gets there. Noise filtering so a resting fader
  stays quiet.
- **Encoders**: absolute values kept on the mixer, with acceleration you can tune or turn off.
- **Values coming back** from the device (CC, pitch bend, pressure) update the mixer, without
  feedback loops; optionally remembered across power cycles.
- **Bluetooth MIDI** (the mixer is a BLE-MIDI peripheral, "SMC-Mixer") and USB-MIDI at once.
- **Web editor** (Chrome): every setting and mapping, templates with search and favourites, a
  live monitor of what the controls send.
- **Updates over USB** from Linux in one command; a bootloader that catches a crashing firmware.

## Use

### Install (once, from the stock firmware)

Linux, a USB cable, and a source build (there are no release binaries). You need Docker and the
JieLi toolchain, see [fw/README.md](fw/README.md#building); Python 3 with
[uv](https://docs.astral.sh/uv/) (or `mido` and `python-rtmidi` installed); `dfu-util`.

```sh
git clone --recursive https://github.com/blen2r/open-smc-mixer && cd open-smc-mixer

BOARD=mixer fw/boot/build.sh                                        # bootloader + installer
BOARD=mixer host/package_fw.sh bootonly
BOARD=mixer SMCPAD_TINYUSB=1 TEXT_BASE=0x1E08120 fw/build.sh        # the mixer firmware
BOARD=mixer host/package_fw.sh mixerapp                             # -> work/flash/mixer/mixer_area.bin

uv run --with mido --with python-rtmidi host/install_from_stock.py --board mixer \
    --app work/flash/mixer/mixer_area.bin
```

The script talks to the stock firmware over USB-MIDI, writes an installer and the bootloader
into free flash (every byte read back), and asks you to switch the mixer off and on once. The
installer replaces the stock firmware with the bootloader, which comes up as a USB DFU device;
the script then writes the firmware. If anything does not check out, the stock firmware simply
boots again. [docs/bootloader.md](docs/bootloader.md) has the whole story. (At the very end the
script may say the application did not come back: its check is macOS-only. Look for
"SMC-Mixer open fw" in `amidi -l`.)

So that `dfu-util` can reach the bootloader without sudo, add a udev rule once:

```sh
echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0002", TAG+="uaccess"' |
    sudo tee /etc/udev/rules.d/50-smc-mixer-dfu.rules && sudo udevadm control --reload
```

### Update

```sh
BOARD=mixer SMCPAD_TINYUSB=1 TEXT_BASE=0x1E08120 fw/build.sh && BOARD=mixer host/package_fw.sh mixerapp
host/mixer_update.sh
```

`package_fw.sh` only packages what `fw/build.sh` compiled: run both. `mixer_update.sh` asks the
running firmware into the bootloader (a SysEx message), writes the firmware and waits for it to
come back. Your settings stay. By hand: hold **Shift + Bluetooth + Play** for 2 s, or switch the
mixer on holding **Shift + Play**, then `dfu-util -d 1209:0002 -a 0 -D work/flash/mixer/mixer_area.bin`.

### The editor

```sh
host/mixer_editor.sh
```

serves the editor on `http://localhost:8770` and opens it in Chrome (Web MIDI needs Chrome,
Edge or Chromium). **Connect (USB)** reads the mixer's configuration; change things, then **Save
to mixer**. **Simulate** works without a mixer. [editor/README.md](editor/README.md) explains it.

### Templates

A template is CC mode set up for one device, by one person: `OP-XY - blen2r` is the one this
firmware was made for (the Teenage Engineering OP-XY over Bluetooth). In the editor: Settings →
**Choose a template…**. Each template comes with a page saying how it is mapped
([editor/templates/opxy-blen2r.md](editor/templates/opxy-blen2r.md)).

To make your own: set CC mode up in the editor, **Save as template…**, write its `.md`, drop both
in `editor/templates/local/` (yours, ignored by git) or `editor/templates/` (to share by pull
request). [editor/templates/README.md](editor/templates/README.md) has the steps and the naming
rule.

### Back to stock

The bootloader accepts a package that puts the stock firmware back
([host/bootpkg.py](host/bootpkg.py) `--to-stock`, built from the vendor's `.fwsc` and a dump of
your mixer's first VM sector). It has been checked byte for byte in simulation
(`host/mixer_install_sim.py`) but **not yet run on a mixer**.

## Hacking on it

- **[QA.md](QA.md)**: how to check a change on the hardware before you publish it (a guided
  script does most of it). Please do, before you share a firmware.
- **[CLAUDE.md](CLAUDE.md)**: the orientation for coding agents (and people): where things are,
  how to build, test and keep QA.md current.
- [docs/mixer-config.md](docs/mixer-config.md): modes, profile, mappings, the configuration
  format and the editor's SysEx protocol.
- [docs/mixer-hardware.md](docs/mixer-hardware.md): every pin, key and LED of the mixer.

| | |
|---|---|
| [`fw/smcpad/examples/mixer/`](fw/smcpad/examples/mixer) | the mixer firmware: modes, mappings, MIDI, BLE-MIDI, configuration |
| [`fw/smcpad/arduino/smcmixer_hw.c`](fw/smcpad/arduino/smcmixer_hw.c) | the mixer's hardware: key matrix, LEDs, encoders, faders |
| [`fw/boot/`](fw/boot) | the bootloader and the installer (`BOARD=mixer`) |
| [`editor/`](editor) | the web editor and its templates |
| [`host/`](host) | install, update, the editor server, the QA script, configuration CLI |

## Credits

- [fedepaj/open-smc-pad](https://github.com/fedepaj/open-smc-pad) by Federico Paglioni and its
  contributors: the bootloader, the install-from-stock path, the runtime, the USB and BLE work
  this firmware stands on.
- [kagaimiq](https://github.com/kagaimiq): jl-uboot-tool, jielie, ghidra-jieli and the
  documentation that made the JieLi chips approachable.
- [JieLi](https://github.com/Jieli-Tech): the AC63 SDK (Apache 2.0).
- [TinyUSB](https://github.com/hathach/tinyusb).

Not affiliated with M-Vave, JieLi or Teenage Engineering. "SMC-Mixer", "M-Vave" and "OP-XY" are
names of their owners, used here to say what this firmware runs on and talks to.

## License

MIT, [LICENSE](LICENSE). Parts derived from the JieLi SDK are Apache 2.0
([fw/NOTICE](fw/NOTICE)).
