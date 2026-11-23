#!/usr/bin/env python3
"""Talk to the SMC-Mixer probe firmware (fw/mixer_probe) over USB MIDI.

    python3 host/mixer_probe.py watch            # print key / knob / fader events as they come
    python3 host/mixer_probe.py leds-index       # light the stock LED indices 0..52 one by one
    python3 host/mixer_probe.py leds-pos         # light the raw positions phase*16+seg one by one
    python3 host/mixer_probe.py led N [--pos]    # light one LED (stock index, or raw position)
    python3 host/mixer_probe.py all on|off       # every LED position
    python3 host/mixer_probe.py dfu              # reboot into the bootloader (DFU mode)

Events (see fw/mixer_probe/probe.c): Note On/Off channels 1..4 = matrix sense row 0..3,
note = column; CC on channel 6 = fader ADC channel and its 7-bit value. Key codes are printed
as the stock firmware writes them (column << 4 | row, docs/mixer-hardware.md) with what the
stock settings say sits there.
"""
import argparse
import sys
import time

import mido

# docs/mixer-hardware.md: key code (physical column << 4 | row) -> control, confirmed 2026-11-22.
# The stock firmware numbers columns one higher (its scan stores each sample one step late).
STRIPS = [("03", "13", "23", "33"), ("A3", "93", "83", "73"), ("B3", "C3", "D3", "E3"),
          ("02", "12", "22", "32"), ("A2", "92", "82", "72"), ("B2", "C2", "D2", "E2"),
          ("51", "41", "31", "21"), ("81", "91", "A1", "B1")]
ROWS = ("Mute (top)", "Solo", "Rec", "Track Stop (bottom)")
NAMES = {int(code, 16): f"strip {s + 1} {ROWS[r]}" for s, codes in enumerate(STRIPS)
         for r, code in enumerate(codes)}
for code, name in zip(("43", "53", "63", "F3", "42", "52", "62", "F2", "01", "11", "C1", "71", "61"),
                      ("Play", "Pause", "Record", "Rewind", "Fast-forward", "<<", ">>", "Up", "Down",
                       "Left", "Right", "Shift", "Bluetooth")):
    NAMES[int(code, 16)] = name
ENCODER_COLS = {c: c // 2 + 1 for c in range(16)}                  # knob n = columns 2n-2, 2n-1
ADC_PIN = {0: "PA0", 1: "PA1", 2: "PA3", 3: "PA4", 4: "PA6", 10: "PC2", 11: "PC4", 12: "PC5"}


def ports():
    ins = [n for n in mido.get_input_names() if "SMC-Mixer probe" in n]
    outs = [n for n in mido.get_output_names() if "SMC-Mixer probe" in n]
    if not ins or not outs:
        sys.exit("SMC-Mixer probe not found (is the probe firmware running?)\n"
                 f"inputs: {mido.get_input_names()}")
    return mido.open_input(ins[0]), mido.open_output(outs[0])


def describe(msg):
    if msg.type in ("note_on", "note_off"):
        row, col = msg.channel, msg.note
        code = col << 4 | row
        down = msg.type == "note_on" and msg.velocity > 0
        what = NAMES.get(code, "?")
        if row == 0:
            what = f"knob {ENCODER_COLS[col]} contact"
        return f"key {code:02X}  col {col:2d} row {row}  {'DOWN' if down else 'up  '}  {what}"
    if msg.type == "control_change" and msg.channel == 5:
        return f"fader ADC ch {msg.control:2d} ({ADC_PIN.get(msg.control, '?')})  value {msg.value:3d}"
    return str(msg)


def led(out, n, on, pos=False):
    ch = 15 if pos else 14
    out.send(mido.Message("note_on" if on else "note_off", channel=ch, note=n, velocity=127 if on else 0))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("cmd", choices=["watch", "leds-index", "leds-pos", "led", "all", "dfu"])
    ap.add_argument("arg", nargs="?")
    ap.add_argument("--pos", action="store_true", help="led: raw position instead of stock index")
    ap.add_argument("--step", type=float, default=1.5, help="seconds per LED in the sweeps")
    a = ap.parse_args()
    inp, out = ports()

    if a.cmd == "watch":
        print("watching (Ctrl-C to stop)", flush=True)
        for msg in inp:
            print(time.strftime("%H:%M:%S"), describe(msg), flush=True)
    elif a.cmd in ("leds-index", "leds-pos"):
        pos = a.cmd == "leds-pos"
        for n in range(64 if pos else 53):
            if pos:
                label = f"position {n:2d} (phase {n >> 4}, segment {n & 15})"
            else:
                label = f"stock LED index {n:2d}"
            print(time.strftime("%H:%M:%S"), label, flush=True)
            led(out, n, True, pos)
            time.sleep(a.step)
            led(out, n, False, pos)
        print("done")
    elif a.cmd == "led":
        led(out, int(a.arg), True, a.pos)
    elif a.cmd == "all":
        out.send(mido.Message("control_change", channel=15, control=126, value=127 if a.arg == "on" else 0))
    elif a.cmd == "dfu":
        out.send(mido.Message("control_change", channel=15, control=127, value=127))
        print("rebooting into the bootloader")


if __name__ == "__main__":
    main()
