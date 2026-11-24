#!/usr/bin/env python3
"""Update the SMC-Mixer's firmware over USB, on Linux, macOS or Windows: ask the running firmware
into the bootloader (a SysEx on USB-MIDI), write the app area with dfu-util (DFU alt 0), wait for
the new firmware to come back. Your settings stay.

    python3 host/mixer_update.py [area.bin]       # default: work/flash/mixer/mixer_area.bin

Needs Python 3 with mido + python-rtmidi (or: uv run --with mido --with python-rtmidi ...) and
dfu-util on the PATH. If the bootloader is already up (USB DFU 1209:0002) it is used as it is.

Linux: host/mixer_update.sh does the same without Python packages (and is the tested path).
macOS: brew install dfu-util. Windows: dfu-util from its releases, and once, Zadig to give the
bootloader (1209:0002, it shows up as a DFU device after the first "into the bootloader") the
WinUSB driver. On Windows a MIDI port can only be open in one program: disconnect the web editor
first. Linux is tested; macOS and Windows are not (README.md).
"""
import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DFU_ID = "1209:0002"
PORT = "SMC-Mixer open fw"


def die(msg):
    sys.exit(f"!! {msg}")


def in_dfu(dfu):
    r = subprocess.run([dfu, "-l"], capture_output=True, text=True)
    return f"[{DFU_ID}]" in r.stdout


def wait_for(test, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if test():
            return True
        time.sleep(0.25)
    return False


def mixer_port(mido):
    try:
        return next((n for n in mido.get_output_names() if PORT in n), None)
    except Exception:                       # a MIDI backend hiccup while the device re-enumerates
        return None


def port_appears():
    """The firmware's port, listed by a fresh process: some MIDI backends (CoreMIDI) do not see
    devices that appear while a process runs."""
    r = subprocess.run([sys.executable, "-c", "import mido; print('\\n'.join(mido.get_output_names()))"],
                       capture_output=True, text=True)
    return PORT in r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", nargs="?", default=str(ROOT / "work/flash/mixer/mixer_area.bin"))
    args = ap.parse_args()
    image = Path(args.image)
    if not image.is_file():
        die(f"no image at {image} (BOARD=mixer host/package_fw.sh mixerapp)")
    dfu = shutil.which("dfu-util")
    if not dfu:
        die("dfu-util not found (Linux: your package manager; macOS: brew install dfu-util; "
            "Windows: https://dfu-util.sourceforge.net/releases/, on the PATH)")
    try:
        import mido
    except ImportError:
        die("needs mido + python-rtmidi (pip install mido python-rtmidi, or run it with "
            "uv run --with mido --with python-rtmidi python host/mixer_update.py)")

    if not in_dfu(dfu):
        name = mixer_port(mido)
        if not name:
            die(f"neither the bootloader nor the firmware (\"{PORT}\") is there")
        print(f"== asking the firmware ({name}) into the bootloader")
        try:
            with mido.open_output(name) as out:
                out.send(mido.Message("sysex", data=[0x7D, 0x4D, 0x70]))
        except Exception as e:
            die(f"could not open {name}: {e}\n   (Windows: disconnect the web editor first, a MIDI port "
                "can only be open in one program there)")
        if not wait_for(lambda: in_dfu(dfu), 15):
            die("the bootloader did not show up (Windows: does it have the WinUSB driver? see the top of "
                "this file)")

    print(f"== writing {image.name}")
    r = subprocess.run([dfu, "-d", DFU_ID, "-a", "0", "-D", str(image)], capture_output=True, text=True)
    for line in (r.stdout + r.stderr).replace("\r", "\n").splitlines():
        if any(k in line for k in ("Download done", "Error", "error", "Done")):
            print("   " + line.strip())
    if r.returncode != 0:
        die(f"dfu-util failed (exit {r.returncode})")

    print("== waiting for the firmware")
    if not wait_for(port_appears, 30):
        die(f"the firmware did not come back (in DFU: {'yes' if in_dfu(dfu) else 'no'})")
    print("   up")


if __name__ == "__main__":
    main()
