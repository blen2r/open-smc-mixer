/* Bootloader/installer copy of fw/smcpad/include/smcpad_spin.h (which needs the SDK's
 * app_config.h for TCFG_CLOCK_SYS_HZ). The bootloader does not use the SDK and runs on the
 * 24 MHz oscillator, the clock the busy-wait counts were tuned at, so the scale is 1.
 * fw/boot/build.sh puts fw/boot/include ahead of everything else, so fw/smcpad/usb/dcd_br25.c
 * picks this one up when it is built into the bootloader. */
#pragma once

#define SMCPAD_SPIN_SCALE   1
#define SMCPAD_SPIN(n)      ((n) * SMCPAD_SPIN_SCALE)
