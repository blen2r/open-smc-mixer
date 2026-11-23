#!/usr/bin/env bash
# Update the SMC-Mixer's firmware over USB (Linux): ask the running firmware into the
# bootloader (a SysEx on USB-MIDI), write the app area with dfu-util (DFU alt 0),
# wait for the new firmware to come back.
#
#   host/mixer_update.sh [area.bin]     # default: work/flash/mixer/mixer_area.bin
#
# If the bootloader is already up (DFU 1209:0002) it is used as it is. Needs dfu-util and the
# udev rule for 1209:0002 (docs: TAG+="uaccess").
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${1:-$ROOT/work/flash/mixer/mixer_area.bin}"
DFU_ID=1209:0002
die() { echo "!! $*" >&2; exit 1; }
[ -f "$IMAGE" ] || die "no image at $IMAGE (BOARD=mixer host/package_fw.sh mixerapp)"
in_dfu() { dfu-util -l 2>/dev/null | grep -q "\[$DFU_ID\]"; }
wait_for() { local i; for i in $(seq 1 $(($2 * 4))); do "$1" && return 0; sleep 0.25; done; return 1; }
app_up() { amidi -l 2>/dev/null | grep -q 'SMC-Mixer open fw'; }

if ! in_dfu; then
    # the firmware's own way in (fw/smcpad/examples/mixer: SysEx F0 7D 4D 70 F7 on USB-MIDI); no
    # serial port permissions needed
    port="$(aconnect -l | sed -n "s/^client \([0-9]*\): 'SMC-Mixer open fw'.*/\1/p" | head -1)"
    [ -n "$port" ] || die "neither the bootloader nor the firmware is there"
    echo "== asking the firmware (ALSA client $port) into the bootloader"
    amidi -l >/dev/null 2>&1
    hw="$(amidi -l | awk '/SMC-Mixer open fw/ {print $2; exit}')"
    [ -n "$hw" ] || die "no amidi port for the firmware"
    if ! amidi -p "$hw" -S "F0 7D 4D 70 F7" 2>/dev/null; then
        # the raw port is held (the web editor in Chrome, mixer_qa.py): go through the ALSA
        # sequencer instead, which shares it
        echo "   (raw port busy, sending through the ALSA sequencer)"
        py="${MIXER_PY:-$HOME/smc_mixer/fw_work/.venv/bin/python}"
        [ -x "$py" ] || py=python3
        "$py" -c 'import mido; n = next(n for n in mido.get_output_names() if "SMC-Mixer open fw" in n)
mido.open_output(n).send(mido.Message("sysex", data=[0x7D, 0x4D, 0x70]))' 2>&1 | grep -v MidiIn
    fi
    wait_for in_dfu 15 || die "the bootloader did not show up"
fi
echo "== writing $(basename "$IMAGE")"
dfu-util -d "$DFU_ID" -a 0 -D "$IMAGE" 2>&1 | tr '\r' '\n' | grep -E "Download done|Error|error|Done" | sed 's/^/   /'
echo "== waiting for the firmware"
wait_for app_up 30 && echo "   up" || die "the firmware did not come back (in DFU: $(in_dfu && echo yes || echo no))"
