#!/usr/bin/env bash
# Build the SMC-PAD bootloader (fw/boot) with the JieLi pi32v2 toolchain in Docker.
# Output: fw/boot/out/boot.{elf,bin,dis,map} (the bootloader, app area at 0x1000) and
#         fw/boot/out/install.{elf,bin,dis,map} (the stock -> ours installer, fw/boot/install.c)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TOOLCHAIN="$(ls -d "$ROOT"/tools/jl-toolchain/jieli-linux-toolchains-*/ | head -1)"
# BOARD=pad (default) or mixer: the layout in boot.h (BOARD_SMC_MIXER), output in out/ or out-mixer/
BOARD="${BOARD:-pad}"
case "$BOARD" in
    pad)   DEFS="";                  OUT=out ;;
    mixer) DEFS="-DBOARD_SMC_MIXER"; OUT=out-mixer ;;
    *)     echo "unknown BOARD=$BOARD (pad or mixer)" >&2; exit 2 ;;
esac
mkdir -p "$ROOT/fw/boot/$OUT"

docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" -e DEFS="$DEFS" -e OUT="$OUT" \
    -v "$TOOLCHAIN:/opt/jieli:ro" -v "$ROOT/fw:/fw" -v "$ROOT/third_party/tinyusb:/tinyusb:ro" \
    jl-build bash -c '
        set -e
        T=/opt/jieli/pi32v2/bin
        cd /fw/boot
        # -enable-ipra=false: the JieLi clang otherwise trusts the body of a weak default
        # across the call (docs/bootloader.md, "Pitfalls")
        cc() {
            $T/clang -target pi32v2 -mcpu=r3 -integrated-as -Oz -fno-common -ffreestanding \
                -ffunction-sections -fdata-sections -Wall -Wno-unused-function \
                -mllvm -enable-ipra=false \
                $DEFS -I. -Iinclude -I/tinyusb/src -c "$1" -o "$OUT/$(basename "$1" .c).o"
        }
        # link <name> <script> <sources...>: $OUT/<name>.elf, .bin (.start then .ram), .dis, .map
        link() {
            name=$1 script=$2; shift 2
            objs=
            for s in "$@"; do
                [ -f "$s" ] || continue
                cc "$s"; objs="$objs $OUT/$(basename "$s" .c).o"
            done
            $T/ld -T $script --gc-sections $objs -o $OUT/$name.elf -Map $OUT/$name.map
            $T/objcopy -O binary -j .start $OUT/$name.elf $OUT/$name.start.bin
            $T/objcopy -O binary -j .ram $OUT/$name.elf $OUT/$name.ram.bin
            cat $OUT/$name.start.bin $OUT/$name.ram.bin > $OUT/$name.bin
            $T/objdump -d $OUT/$name.elf > $OUT/$name.dis
            echo "$name:"; $T/objdump -h $OUT/$name.elf | grep -E "start|ram|bss"
        }
        link boot boot.ld boot.c dfu.c flash.c jlfs.c usb_desc.c libc.c /fw/smcpad/usb/dcd_br25.c \
            /tinyusb/src/tusb.c /tinyusb/src/common/tusb_fifo.c /tinyusb/src/device/usbd.c \
            /tinyusb/src/device/usbd_control.c /tinyusb/src/class/dfu/dfu_device.c
        link install install.ld install.c flash.c jlfs.c libc.c
    '
ls -l "$ROOT/fw/boot/$OUT/boot.bin" "$ROOT/fw/boot/$OUT/install.bin"
