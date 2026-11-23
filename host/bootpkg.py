#!/usr/bin/env python3
"""The installer slot and staging area (fw/boot/install.c, fw/boot/boot.h), host side.

Used two ways:
- host/install_from_stock.py: the stock firmware writes the installer and the staging area
  (erase_end = 0x26000: the whole stock firmware goes);
- the bootloader update package, DFU alt 2 of our bootloader (erase_end = 0x9000: the
  application stays):

    python3 host/bootpkg.py [--board mixer] [--out work/flash/boot_update.bin]

  needs fw/boot/out/install.bin and work/flash/boot_area.bin (host/package_fw.sh boot);
- (mixer) the way back to the stock firmware, also DFU alt 2: a stock app area for 0x1000 plus
  the first stock VM sector, which the installer puts back in place of bootloader, application
  and our VM (fw/boot/boot.h STOCK_VM_EXTRA):

    python3 host/bootpkg.py --board mixer --to-stock work/flash/stock_area_013.bin \
        --vm-sector-from ~/smc_mixer/backups/SMC-Mixer_013_after_install.bin \
        --out work/flash/to_stock.bin
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

# What differs per board (fw/boot/boot.h, BOARD_SMC_MIXER; fw/boot/build.sh BOARD=...).
BOARDS = {
    # name: stock firmware name prefix (SINCO 0x11), STAGE_END, ERASE_STOCK (= stock VM start),
    #       fw/boot build output directory, our VM (as in host/package_fw.sh), the stock VM
    #       sector a stock package may carry (boot.h STOCK_VM_EXTRA), the directory template
    #       (None: jlimage's defaults, which are the pad's)
    "pad":   dict(prefix="SMC-PAD",   stage_end=0x4D000, erase_stock=0x26000, out="fw/boot/out",
                  vm=(0x71000, 0x4000), stock_vm_extra=0, template=None, flash="work/flash"),
    "mixer": dict(prefix="SMC-Mixer", stage_end=0x68000, erase_stock=0x24000, out="fw/boot/out-mixer",
                  vm=(0x77000, 0x4000), stock_vm_extra=0x1000,
                  template=str(ROOT / "dumps/SMC-Mixer_013_area_decrypted.bin"), flash="work/flash/mixer"),
}
BOARD = "pad"
STAGE_END = BOARDS[BOARD]["stage_end"]
ERASE_STOCK = BOARDS[BOARD]["erase_stock"]
VM = BOARDS[BOARD]["vm"]


def select_board(name):
    global BOARD, STAGE_END, ERASE_STOCK, VM
    if name not in BOARDS:
        raise SystemExit(f"unknown board {name!r} ({', '.join(BOARDS)})")
    BOARD, STAGE_END, ERASE_STOCK = name, BOARDS[name]["stage_end"], BOARDS[name]["erase_stock"]
    VM = BOARDS[name]["vm"]


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
    payload = payload or (ROOT / BOARDS[BOARD]["flash"] / "boot_area.bin").read_bytes()
    head = J.read_dir(payload)[0]
    if head.offset != J.AREA_VMA + 0x120 or J.crc16(payload[0x20:head.size]) != head.data_crc:
        raise SystemExit("not an app area for 0x1000 (host/package_fw.sh boot)")
    extra = BOARDS[BOARD]["stock_vm_extra"] if erase_end == ERASE_STOCK else 0
    if len(payload) > erase_end - DEST + extra:     # fw/boot/boot.h DEST_MAX(erase_end)
        raise SystemExit(f"payload {len(payload):#x} bytes does not fit 0x1000..{erase_end + extra:#x}")
    hdr = struct.pack("<7I", STAGE_MAGIC, 1, PAYLOAD_OFF, len(payload),
                      zlib.crc32(payload), DEST, erase_end)
    hdr += struct.pack("<I", zlib.crc32(hdr))
    blob = hdr + b"\xff" * (PAYLOAD_OFF - len(hdr)) + payload
    if STAGE + len(blob) > STAGE_END:
        raise SystemExit("staging area too big")
    return blob


def update_package(template, cfg, erase_end=None, payload=None):
    """DFU alt 2: the plaintext image of INSTALL_DIR..: installer area padded to STAGE, then the
    staging area, padded to whole 4 KB blocks (the bootloader writes one sector per block)."""
    inst = installer_area(template, cfg)
    img = inst + b"\xff" * (STAGE - INSTALL_DIR - len(inst)) + stage(erase_end or ERASE_UPDATE, payload)
    img += b"\xff" * (-len(img) % 0x1000)
    if INSTALL_DIR + len(img) > STAGE_END:
        raise SystemExit(f"package {len(img):#x} bytes runs past {STAGE_END:#x}")
    return img


def to_stock_payload(area_path, vm_from):
    """A stock app area (plaintext, for 0x1000, as host/stock_area.py writes it) padded to the
    stock VM start, plus the first stock VM sector taken from a full dump (the VM is stored
    plaintext, so a physical dump has it as is)."""
    extra = BOARDS[BOARD]["stock_vm_extra"]
    if not extra:
        raise SystemExit(f"--to-stock is not supported for {BOARD}")
    area = Path(area_path).read_bytes()
    if len(area) > ERASE_STOCK - DEST:
        raise SystemExit(f"stock area {len(area):#x} bytes is larger than {ERASE_STOCK - DEST:#x}")
    dump = Path(vm_from).read_bytes()
    if len(dump) != 0x80000:
        raise SystemExit(f"{vm_from}: not a full 512 KB dump")
    vm = dump[ERASE_STOCK:ERASE_STOCK + extra]
    if vm[:4] != b"\x55\xaa\xaa\x55" and vm != b"\xff" * extra:
        raise SystemExit(f"{vm_from}: no VM header at {ERASE_STOCK:#x}")
    return area + b"\xff" * (ERASE_STOCK - DEST - len(area)) + vm


def main():
    ap = argparse.ArgumentParser(description="build the bootloader update package (DFU alt 2)")
    ap.add_argument("--board", default="pad", choices=sorted(BOARDS))
    ap.add_argument("--out", help="default: <board's work dir>/boot_update.bin")
    ap.add_argument("--dump", help="directory template and cfg_tool.bin from a stock dump "
                                   "(default: the board's)")
    ap.add_argument("--to-stock", metavar="AREA", help="(mixer) build the way back to stock "
                    "instead: AREA is a stock app area for 0x1000 (host/stock_area.py)")
    ap.add_argument("--vm-sector-from", metavar="DUMP",
                    help="with --to-stock: full flash dump to take the first stock VM sector from")
    a = ap.parse_args()
    select_board(a.board)
    template, cfg = J.template_and_cfg(a.dump or BOARDS[BOARD]["template"])
    if a.to_stock:
        if not a.vm_sector_from:
            raise SystemExit("--to-stock needs --vm-sector-from")
        pkg = update_package(template, cfg, ERASE_STOCK, to_stock_payload(a.to_stock, a.vm_sector_from))
        what = "back-to-stock package"
    else:
        pkg = update_package(template, cfg)
        what = "bootloader update package"
    a.out = a.out or str(ROOT / BOARDS[BOARD]["flash"] / "boot_update.bin")
    Path(a.out).write_bytes(pkg)
    print(f"{what} written to {a.out} ({len(pkg):#x} bytes)")


if __name__ == "__main__":
    main()
