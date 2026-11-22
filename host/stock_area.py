#!/usr/bin/env python3
"""Plaintext stock app area (flash 0x1000..erase_end) from a vendor .fwsc, as an installer payload.

    python3 host/stock_area.py vendor/SMC-Mixer_013.fwsc --board mixer -o work/flash/stock_area.bin
    uv run --with mido --with python-rtmidi host/install_from_stock.py --board mixer \
        --payload work/flash/stock_area.bin --payload-is-stock

The .fwsc is a JieLi UFW image (host/fwsc.py); its flash.bin entry is the factory image of the
flash from offset 0, the app area SFC-encrypted. We cut 0x1000..erase_end and decrypt it: the
installer (fw/boot/install.c) encrypts it again at 0x1000, so the flash ends up holding exactly
the vendor's bytes. Checks: UFW header and entry CRCs, then the app directory's CRCs.
"""
import argparse
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import bootpkg                                       # noqa: E402
import fwsc                                          # noqa: E402
import jlimage as J                                  # noqa: E402

UFW_HDR, UFW_ENTRY = 0x40, 0x50


def ufw_entries(ufw):
    """(name, type, data) of a UFW image; header and entries are ENC-ciphered (key 0xFFFF)."""
    hdr = bytearray(ufw[:UFW_HDR])
    J.enc_cipher(hdr, 0, UFW_HDR)
    count = struct.unpack_from("<H", hdr, 8)[0]
    chip = hdr[16:64].split(b"\0")[0].decode(errors="replace")
    out = []
    for i in range(count):
        e = bytearray(ufw[UFW_HDR + i * UFW_ENTRY:UFW_HDR + (i + 1) * UFW_ENTRY])
        J.enc_cipher(e, 0, UFW_ENTRY)
        etype, _, dcrc, _, off, size = struct.unpack_from("<HHHHII", e)
        name = e[64:80].split(b"\0")[0].decode(errors="replace")
        data = ufw[off:off + size]
        out.append((name, etype, data, J.crc16(data) == dcrc))
    return chip, out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("fwsc")
    ap.add_argument("--board", default="mixer", choices=sorted(bootpkg.BOARDS))
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    bootpkg.select_board(a.board)

    parsed = fwsc.parse_any(Path(a.fwsc).read_bytes())
    if not parsed:
        sys.exit("not a .fwsc this tool understands")
    name, ver, ufw = parsed
    chip, ents = ufw_entries(ufw)
    print(f"{name} v{ver:03d}, UFW for {chip}: " + ", ".join(f"{n}{'' if ok else '(crc?)'}" for n, _, _, ok in ents))
    if not name.startswith(bootpkg.BOARDS[a.board]["prefix"]):
        sys.exit(f"{name} is not a {a.board} firmware")
    flash = next((d for n, t, d, ok in ents if t == 0 and ok), None)
    if flash is None:
        sys.exit("no flash.bin entry with a good CRC")

    end = bootpkg.ERASE_STOCK
    if len(flash) < end:
        sys.exit(f"flash.bin is {len(flash):#x} bytes, shorter than the stock area end {end:#x}")
    if any(b != 0xFF for b in flash[end:]):
        sys.exit(f"flash.bin has data past {end:#x}: the stock area is bigger than this board's layout says")
    area = J.sfc_encrypt_at(flash[bootpkg.DEST:end], bootpkg.DEST)   # symmetric: decrypts

    d = J.read_dir(area)
    head = d[0]
    if head.offset != J.AREA_VMA + 0x120 or J.crc16(area[0x20:head.size]) != head.data_crc:
        sys.exit("decrypted area does not check out (entry point / area CRC)")
    for e in d[1:3]:
        if J.crc16(area[e.offset:e.offset + e.size]) != e.data_crc:
            sys.exit(f"{e.name}: data CRC mismatch")
    names = [bytes(e.name).split(b"\0")[0].decode(errors="replace") if isinstance(e.name, (bytes, bytearray))
             else e.name for e in d[:3]]
    print("app area: " + ", ".join(f"{n} {e.size:#x}" for n, e in zip(names, d[:3])) + " -- CRCs OK")
    bootpkg.stage(bootpkg.ERASE_STOCK, area)          # the same checks the installer will make
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_bytes(area)
    print(f"wrote {len(area):#x} bytes to {a.out} (installs to {bootpkg.DEST:#x}..{end:#x})")


if __name__ == "__main__":
    main()
