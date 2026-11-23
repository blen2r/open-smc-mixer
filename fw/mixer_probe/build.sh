#!/usr/bin/env bash
# Build the SMC-Mixer probe (fw/mixer_probe) with the JieLi pi32v2 toolchain in Docker.
# Output: fw/mixer_probe/out/probe.{elf,bin,dis,map}; then
#   BOARD=mixer host/package_fw.sh probe   -> work/flash/mixer/probe_area.bin (DFU alt 0)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TOOLCHAIN="$(ls -d "$ROOT"/tools/jl-toolchain/jieli-linux-toolchains-*/ | head -1)"
mkdir -p "$ROOT/fw/mixer_probe/out"

docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" \
    -v "$TOOLCHAIN:/opt/jieli:ro" -v "$ROOT/fw:/fw" -v "$ROOT/third_party/tinyusb:/tinyusb:ro" \
    jl-build bash -c '
        set -e
        T=/opt/jieli/pi32v2/bin
        cd /fw/mixer_probe
        objs=
        for s in probe.c usb_desc.c /fw/boot/libc.c /fw/smcpad/usb/dcd_br25.c \
                 /tinyusb/src/tusb.c /tinyusb/src/common/tusb_fifo.c /tinyusb/src/device/usbd.c \
                 /tinyusb/src/device/usbd_control.c /tinyusb/src/class/midi/midi_device.c; do
            [ -f "$s" ] || continue          # (usbd_control.c: older TinyUSB only)
            o=out/$(basename "$s" .c).o
            $T/clang -target pi32v2 -mcpu=r3 -integrated-as -Os -fno-common -ffreestanding \
                -ffunction-sections -fdata-sections -Wall -Wno-unused-function \
                -mllvm -enable-ipra=false \
                -I. -I/fw/boot/include -I/tinyusb/src -c "$s" -o "$o"
            objs="$objs $o"
        done
        $T/ld -T probe.ld --gc-sections $objs -o out/probe.elf -Map out/probe.map
        $T/objcopy -O binary -j .text out/probe.elf out/probe.text.bin
        $T/objcopy -O binary -j .data out/probe.elf out/probe.data.bin
        cat out/probe.text.bin out/probe.data.bin > out/probe.bin
        $T/objdump -d out/probe.elf > out/probe.dis
        $T/objdump -h out/probe.elf | grep -E "text|data|bss"
    '
ls -l "$ROOT/fw/mixer_probe/out/probe.bin"
