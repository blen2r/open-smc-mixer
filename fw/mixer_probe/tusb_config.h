/* TinyUSB configuration of the mixer probe: one USB MIDI interface, polled. */
#ifndef MIXER_PROBE_TUSB_CONFIG_H
#define MIXER_PROBE_TUSB_CONFIG_H

#define CFG_TUSB_MCU                OPT_MCU_NONE
#define CFG_TUSB_OS                 OPT_OS_NONE
#define CFG_TUSB_DEBUG              0
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))

#define SMCPAD_DCD_BR25             1
#define TUP_DCD_ENDPOINT_MAX        2           /* EP0 + EP1 (MIDI in and out) */

#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE      64

#define CFG_TUD_MIDI                1
#define CFG_TUD_MIDI_RX_BUFSIZE     64
#define CFG_TUD_MIDI_TX_BUFSIZE     256

#define CFG_TUD_DFU                 0
#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_VENDOR              0

#endif
