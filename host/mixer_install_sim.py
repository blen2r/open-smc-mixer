#!/usr/bin/env python3
"""Simulate the mixer's install round trip on a flash image, before doing it on the device.

Models, step for step, what fw/boot does with a package (installer slot + staging area):
host/install_from_stock.py or DFU alt 2 writes it (the installer part SFC-encrypted, the staging
area plaintext) and points sector 0 at the installer; fw/boot/install.c then checks the
staging area, erases, writes the payload (encrypted below the stock VM start), erases the rest
of the stock VM on a stock package, and erases the slot, staging area and diagnostics page.

    python3 host/mixer_install_sim.py --dump ~/smc_mixer/backups/SMC-Mixer_013_after_install.bin

Checks:
  1. stock -> our bootloader (host/install_from_stock.py --board mixer with
     work/flash/mixer/boot_area.bin): bootloader at 0x1000, the rest of 0x1000..0x7B000 blank
     but for nothing, the factory areas (0x7B000..) untouched;
  2. our bootloader -> stock (DFU alt 2, work/flash/mixer/to_stock.bin): the whole flash equal
     to the dump again, byte for byte.
Sector 0 is not modelled (its boot pointer goes to the installer and back to 0x1000).
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import bootpkg                                       # noqa: E402
import jlimage as J                                  # noqa: E402

SECTOR = 0x1000
VM_END = 0x7B000                                     # fw/boot/boot.h (mixer)
APP_AREA_END = 0x77000


def erase(img, a):
    img[a:a + SECTOR] = b"\xff" * SECTOR


def write_package(img, pkg):
    """What the writer does: installer area encrypted, staging area as is (DFU alt 2 encrypts
    INSTALL_DIR..STAGE; host/install_from_stock.py does the same with sfc_encrypt_at)."""
    inst = pkg[:bootpkg.STAGE - bootpkg.INSTALL_DIR]
    stage = pkg[bootpkg.STAGE - bootpkg.INSTALL_DIR:]
    assert bootpkg.INSTALL_DIR + len(pkg) <= bootpkg.STAGE_END, "package too big"
    img[bootpkg.INSTALL_DIR:bootpkg.STAGE] = J.sfc_encrypt_at(inst, bootpkg.INSTALL_DIR)
    img[bootpkg.STAGE:bootpkg.STAGE + len(stage)] = stage


def area_check(img, base, max_size, encrypted):
    head = bytes(img[base:base + 0x20])
    if encrypted:
        head = J.sfc_encrypt_at(head, base)
    size = struct.unpack_from("<I", head, 8)[0]
    if J.crc16(head[2:32]) != struct.unpack_from("<H", head, 0)[0]:
        return 0
    if struct.unpack_from("<I", head, 4)[0] != 0x1E00120 or not 0x120 < size <= max_size:
        return 0
    body = bytes(img[base:base + size])
    if encrypted:
        body = J.sfc_encrypt_at(body, base)
    return size if J.crc16(body[0x20:size]) == struct.unpack_from("<H", head, 2)[0] else 0


def stage_check(img):
    """fw/boot/jlfs.c stage_check, mixer constants."""
    s = bootpkg.STAGE
    magic, ver, poff, plen, pcrc, dest, erase_end, hcrc = struct.unpack_from("<8I", img, s)
    extra = 0x1000 if erase_end == bootpkg.ERASE_STOCK else 0
    ok = (magic == bootpkg.STAGE_MAGIC and ver == 1 and zlib.crc32(img[s:s + 28]) == hcrc and
          dest == bootpkg.DEST and erase_end in (bootpkg.ERASE_STOCK, bootpkg.ERASE_UPDATE) and
          0 < plen <= erase_end - bootpkg.DEST + extra and s + poff + plen <= bootpkg.STAGE_END)
    if not ok:
        raise SystemExit("stage_check: header refused")
    if zlib.crc32(img[s + poff:s + poff + plen]) != pcrc:
        raise SystemExit("stage_check: payload CRC")
    if not area_check(img, s + poff, plen, False):
        raise SystemExit("stage_check: payload is not an app area for 0x1000")
    return poff, plen, erase_end


def dfu_manifest_check(img):
    """fw/boot/dfu.c boot_pkg_manifest: an installer for the slot and a good staging area."""
    if not area_check(img, bootpkg.INSTALL_DIR, bootpkg.STAGE - bootpkg.INSTALL_DIR, True):
        raise SystemExit("manifest: no installer in the slot")
    stage_check(img)


def run_installer(img):
    """fw/boot/install.c, mixer build."""
    poff, plen, erase_end = stage_check(img)
    dest = bootpkg.DEST
    erase_to = max(erase_end, dest + plen)
    for a in range(dest, erase_to, SECTOR):
        erase(img, a)
    payload = bytes(img[bootpkg.STAGE + poff:bootpkg.STAGE + poff + plen])
    for off in range(0, plen, SECTOR):
        chunk = payload[off:off + SECTOR]
        if dest + off < bootpkg.ERASE_STOCK:
            chunk = J.sfc_encrypt_at(chunk, dest + off)
        img[dest + off:dest + off + len(chunk)] = chunk
        img[dest + off + len(chunk):dest + off + SECTOR] = b"\xff" * (SECTOR - len(chunk))
    if erase_end == bootpkg.ERASE_STOCK:
        for a in range(erase_to, VM_END, SECTOR):
            if not bootpkg.INSTALL_DIR <= a < bootpkg.STAGE_END:
                erase(img, a)
    for a in range(bootpkg.INSTALL_DIR, bootpkg.STAGE_END + SECTOR, SECTOR):   # DIAG_END
        erase(img, a)


def diff_ranges(a, b, lo, hi):
    out, start = [], None
    for x in range(lo, hi, SECTOR):
        same = a[x:x + SECTOR] == b[x:x + SECTOR]
        if not same and start is None:
            start = x
        if same and start is not None:
            out.append((start, x)); start = None
    if start is not None:
        out.append((start, hi))
    return ", ".join(f"{s:#x}..{e:#x}" for s, e in out) or "none"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dump", required=True, help="full 512 KB SINCO dump of the stock mixer")
    ap.add_argument("--flash", default=str(ROOT / "work/flash/mixer"))
    a = ap.parse_args()
    bootpkg.select_board("mixer")
    flash = Path(a.flash)
    dump = Path(a.dump).read_bytes()
    assert len(dump) == 0x80000
    # A SINCO dump reads the app area through the SFC, i.e. decrypted; the model works on the
    # physical bytes, so encrypt it back (the VM and everything above are stored plaintext).
    stock = bytearray(dump)
    stock[0x1000:bootpkg.ERASE_STOCK] = J.sfc_encrypt_at(dump[0x1000:bootpkg.ERASE_STOCK], 0x1000)
    stock = bytes(stock)

    # 1. stock -> bootloader (the package host/install_from_stock.py writes: installer from the
    #    device's own directory template; here the board's template, same entries)
    template, cfg = J.template_and_cfg(bootpkg.BOARDS["mixer"]["template"])
    boot_area = (flash / "boot_area.bin").read_bytes()
    pkg = bootpkg.update_package(template, cfg, bootpkg.ERASE_STOCK, boot_area)
    img = bytearray(stock)
    write_package(img, pkg)
    run_installer(img)
    raw = (flash / "boot_area_raw.bin").read_bytes()
    assert img[0x1000:0x1000 + len(raw)] == raw, "bootloader not where expected"
    rest = img[0x1000 + len(raw):VM_END]
    assert rest == b"\xff" * len(rest), "something left between the bootloader and 0x7B000"
    assert img[VM_END:] == stock[VM_END:], "factory areas changed"
    assert img[:0x1000] == stock[:0x1000]
    print("1. stock -> bootloader: bootloader at 0x1000, rest blank to 0x7B000, factory areas kept")

    # a stand-in application + our VM in use, so that going back has something to clear
    for x in range(0x9000, 0x40000, SECTOR):
        img[x:x + SECTOR] = bytes([x >> 12 & 0xFF]) * SECTOR
    img[APP_AREA_END:APP_AREA_END + SECTOR] = b"\x5a" * SECTOR

    # 2. bootloader -> stock, DFU alt 2
    pkg = (flash / "to_stock.bin").read_bytes()
    erase_all = range(bootpkg.INSTALL_DIR, bootpkg.STAGE_END, SECTOR)
    for x in erase_all:                     # dfu.c writes each block with erase + program
        erase(img, x)
    write_package(img, pkg)
    dfu_manifest_check(img)
    run_installer(img)
    print("2. bootloader -> stock: differences from the dump:", diff_ranges(img, stock, 0, 0x80000))
    if img != stock:
        raise SystemExit("FAIL: the round trip does not give the dump back")
    print("OK: byte for byte the dump again")


if __name__ == "__main__":
    main()
