#!/usr/bin/env python3
"""The installer slot and staging area (fw/boot/install.c, fw/boot/boot.h), host side.

Used two ways:
- host/install_from_stock.py: the stock firmware writes the installer and the staging area
  (erase_end = 0x26000: the whole stock firmware goes);
- the bootloader update package, DFU alt 2 of our bootloader (erase_end = 0x9000: the
  application stays):

    python3 host/bootpkg.py [--out work/flash/boot_update.bin]

  needs fw/boot/out/install.bin and work/flash/boot_area.bin (host/package_fw.sh boot).
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import jlimage as J                                  # noqa: E402

INSTALL_DIR = 0x41000           # fw/boot/boot.h
STAGE = 0x43000
STAGE_MAGIC = 0x49434D53        # "SMCI"
PAYLOAD_OFF = 0x1000            # from STAGE
DEST = 0x1000
ERASE_UPDATE = 0x9000           # APP_DIR
VM = (0x71000, 0x4000)          # as in host/package_fw.sh

# What differs per board (fw/boot/boot.h, BOARD_SMC_MIXER; fw/boot/build.sh BOARD=...).
BOARDS = {
    # name: stock firmware name prefix (SINCO 0x11), STAGE_END, ERASE_STOCK (= stock VM start),
    #       fw/boot build output directory
    "pad":   dict(prefix="SMC-PAD",   stage_end=0x4D000, erase_stock=0x26000, out="fw/boot/out"),
    "mixer": dict(prefix="SMC-Mixer", stage_end=0x68000, erase_stock=0x24000, out="fw/boot/out-mixer"),
}
BOARD = "pad"
STAGE_END = BOARDS[BOARD]["stage_end"]
ERASE_STOCK = BOARDS[BOARD]["erase_stock"]


def select_board(name):
    global BOARD, STAGE_END, ERASE_STOCK
    if name not in BOARDS:
        raise SystemExit(f"unknown board {name!r} ({', '.join(BOARDS)})")
    BOARD, STAGE_END, ERASE_STOCK = name, BOARDS[name]["stage_end"], BOARDS[name]["erase_stock"]


def installer_area(template, cfg):
    """The installer as an app area for INSTALL_DIR. The uboot maps the directory sector 0
    points at to CPU (entry & ~0xFFFF), so the code runs at 0x1E00120 like the stock firmware."""
    code = (ROOT / BOARDS[BOARD]["out"] / "install.bin").read_bytes()
    area = J.build_app_area(code, cfg, template, entry_vma=J.AREA_VMA + 0x120, vm=VM, prct_size=VM[0])
    if INSTALL_DIR + len(area) > STAGE:
        raise SystemExit(f"installer area too big ({len(area):#x})")
    return area


def stage(erase_end, payload=None):
    """Staging header + the payload app area for 0x1000 (plaintext; the installer encrypts it):
    the bootloader's, or any area linked for 0x1000 such as a stock firmware's."""
    payload = payload or (ROOT / "work/flash/boot_area.bin").read_bytes()
    head = J.read_dir(payload)[0]
    if head.offset != J.AREA_VMA + 0x120 or J.crc16(payload[0x20:head.size]) != head.data_crc:
        raise SystemExit("not an app area for 0x1000 (host/package_fw.sh boot)")
    if len(payload) > erase_end - DEST:             # fw/boot/boot.h DEST_MAX(erase_end)
        raise SystemExit(f"payload {len(payload):#x} bytes does not fit 0x1000..{erase_end:#x}")
    hdr = struct.pack("<7I", STAGE_MAGIC, 1, PAYLOAD_OFF, len(payload),
                      zlib.crc32(payload), DEST, erase_end)
    hdr += struct.pack("<I", zlib.crc32(hdr))
    blob = hdr + b"\xff" * (PAYLOAD_OFF - len(hdr)) + payload
    if STAGE + len(blob) > STAGE_END:
        raise SystemExit("staging area too big")
    return blob


def update_package(template, cfg):
    """DFU alt 2: the plaintext image of INSTALL_DIR..: installer area padded to STAGE, then the
    staging area, padded to whole 4 KB blocks (the bootloader writes one sector per block)."""
    inst = installer_area(template, cfg)
    img = inst + b"\xff" * (STAGE - INSTALL_DIR - len(inst)) + stage(ERASE_UPDATE)
    return img + b"\xff" * (-len(img) % 0x1000)


def main():
    ap = argparse.ArgumentParser(description="build the bootloader update package (DFU alt 2)")
    ap.add_argument("--out", default=str(ROOT / "work/flash/boot_update.bin"))
    ap.add_argument("--dump", help="directory template and cfg_tool.bin from a stock dump "
                                   "(default: jlimage's defaults, the same values)")
    a = ap.parse_args()
    template, cfg = J.template_and_cfg(a.dump)
    pkg = update_package(template, cfg)
    Path(a.out).write_bytes(pkg)
    print(f"bootloader update package written to {a.out} ({len(pkg):#x} bytes)")


if __name__ == "__main__":
    main()
