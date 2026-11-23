#!/usr/bin/env bash
# Package the firmware into a flashable image and the full image to verify against, so that
# `sudo host/flash_fw.sh` has everything it needs.
#
#   host/package_fw.sh          # fw/smcpad alone at 0x1000 (built with the default TEXT_BASE)
#   host/package_fw.sh boot     # bootloader (fw/boot) at 0x1000 + fw/smcpad at 0x9000
#                               # (fw/smcpad built with TEXT_BASE=0x1E08120)
#   BOARD=mixer host/package_fw.sh bootonly   # the mixer's bootloader area + update package
#
# Produces, in work/flash/:
#   fw_area.bin          the app area as the CPU sees it (plaintext; the application's in
#                        boot mode, boot_area.bin is the bootloader's)
#   fw_area_raw.bin      what gets written at 0x1000, SFC-encrypted where it must be
#   expected_with_fw.bin the whole 512 KB flash as it must read back afterwards
#   boot_update.bin      (boot) the bootloader update package, host/dfu_flash.sh --bootloader
#
# Layout with the bootloader: both directories declare the VM at 0x71000 (0x4000 bytes), so the
# SFC decrypts everything below it -- bootloader and application -- in every boot phase, and
# the application has 0x9000..0x71000 (416 KB).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
MODE="${1:-solo}"

APP_DIR=0x9000
# BOARD=pad (default) or mixer (docs/mixer-hardware.md): the mixer's factory areas sit higher,
# so its VM is at 0x77000 (fw/boot/boot.h APP_AREA_END), its directory template comes from its
# own stock firmware, and its files go to work/flash/mixer/.
BOARD="${BOARD:-pad}"
case "$BOARD" in
    pad)   VM_BASE=0x71000; BOOT_OUT=fw/boot/out;       FLASH=work/flash;       TEMPLATE="" ;;
    mixer) VM_BASE=0x77000; BOOT_OUT=fw/boot/out-mixer; FLASH=work/flash/mixer
           TEMPLATE="--dump dumps/SMC-Mixer_013_area_decrypted.bin" ;;
    *)     echo "unknown BOARD=$BOARD (pad or mixer)" >&2; exit 2 ;;
esac
mkdir -p "$FLASH"
VM="--vm-base $VM_BASE --vm-size 0x4000 --prct-size $VM_BASE"

case "$MODE" in
    solo)
        python3 host/check_appbin.py
        python3 host/jlimage.py build --out work/flash/fw_area.bin --raw work/flash/fw_area_raw.bin
        ;;
    bootonly)
        # just the bootloader's area (BOARD=mixer: for host/install_from_stock.py --board mixer)
        python3 host/jlimage.py build --app $BOOT_OUT/boot.bin --area-base 0x1000 $VM $TEMPLATE \
            --out $FLASH/boot_area.bin --raw $FLASH/boot_area_raw.bin
        python3 host/bootpkg.py --board "$BOARD"   # $FLASH/boot_update.bin (DFU alt 2)
        exit 0
        ;;
    boot)
        [ "$BOARD" = pad ] || { echo "boot mode: pad only for now (BOARD=mixer: bootonly)" >&2; exit 2; }
        TEXT_BASE=$(printf '0x%X' $((0x1E00120 + APP_DIR - 0x1000))) python3 host/check_appbin.py
        python3 host/jlimage.py build --app fw/boot/out/boot.bin --area-base 0x1000 $VM \
            --out work/flash/boot_area.bin --raw work/flash/boot_area_raw.bin
        python3 host/jlimage.py build --area-base $APP_DIR $VM \
            --out work/flash/fw_area.bin --raw work/flash/app_area_raw.bin
        python3 - "$APP_DIR" <<'EOF'
import pathlib, sys
app_dir = int(sys.argv[1], 0)
boot = pathlib.Path("work/flash/boot_area_raw.bin").read_bytes()
app = pathlib.Path("work/flash/app_area_raw.bin").read_bytes()
assert 0x1000 + len(boot) <= app_dir, f"the bootloader area ({len(boot):#x}) runs into {app_dir:#x}"
img = boot + b"\xff" * (app_dir - 0x1000 - len(boot)) + app
pathlib.Path("work/flash/fw_area_raw.bin").write_bytes(img)
print(f"bootloader {0x1000:#x}..{0x1000 + len(boot):#x}, application {app_dir:#x}..{app_dir + len(app):#x}")
EOF
        python3 host/bootpkg.py                     # work/flash/boot_update.bin (DFU alt 2)
        ;;
    *)  echo "usage: [BOARD=pad|mixer] $0 [solo|boot|bootonly]" >&2; exit 2 ;;
esac

python3 - <<'EOF'
import pathlib

orig = pathlib.Path("dumps/original_physical_full.bin").read_bytes()
raw = pathlib.Path("work/flash/fw_area_raw.bin").read_bytes()
AREA = 0x1000
END = max(0x26000, AREA + (len(raw) + 0xFFF) // 0x1000 * 0x1000)   # what flash_fw.sh erases
assert len(orig) == 0x80000, len(orig)
assert AREA + len(raw) <= 0x71000, f"the image ({len(raw):#x}) runs into the VM"

img = bytearray(orig)
img[AREA:END] = b"\xff" * (END - AREA)
img[AREA:AREA + len(raw)] = raw
out = pathlib.Path("work/flash/expected_with_fw.bin")
out.write_bytes(img)
print(f"expected image written to {out} "
      f"(written {AREA:#x}..{AREA + len(raw):#x}, erased up to {END:#x})")
EOF

echo
echo "ready: sudo host/flash_fw.sh"
