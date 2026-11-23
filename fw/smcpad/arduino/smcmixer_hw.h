/*
 * SMC-Mixer hardware service (smcmixer_hw.c, the mixer's counterpart of smcpad_hw.c; built
 * when SMC_MIXER is set, fw/build.sh BOARD=mixer). Map: docs/mixer-hardware.md.
 *
 * Numbering (the stock firmware's panel order):
 *   buttons  0..31  track strips: 4 * strip + row (strip 0..7 left to right, row 0 Mute (top),
 *                   1 Solo, 2 Rec, 3 Track Stop (bottom))
 *            32..36 Play, Pause, Record, Rewind, Fast-forward
 *            37..42 <<, >>, Up, Down, Left, Right
 *            43     Bluetooth (upper oval), 44 Shift (lower oval)
 *   LEDs     0..44  the buttons' LEDs, same numbers (37..42 have LEDs the stock firmware drives
 *                   itself, they light just the same)
 *            SMCMIXER_LED_FADER(f): the LED above fader f (0..7)
 *   encoders 0..7 = knobs 1..8, faders 0..7 = faders 1..8
 */
#ifndef SMCMIXER_HW_H
#define SMCMIXER_HW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMCMIXER_NUM_BUTTONS    45
#define SMCMIXER_NUM_LEDS       53
#define SMCMIXER_NUM_ENCODERS   8
#define SMCMIXER_NUM_FADERS     8

#define SMCMIXER_BTN_STRIP(s, row)  ((s) * 4 + (row))
enum {
    SMCMIXER_ROW_MUTE = 0, SMCMIXER_ROW_SOLO, SMCMIXER_ROW_REC, SMCMIXER_ROW_STOP,
};
enum {
    SMCMIXER_BTN_PLAY = 32, SMCMIXER_BTN_PAUSE, SMCMIXER_BTN_RECORD, SMCMIXER_BTN_REWIND,
    SMCMIXER_BTN_FF, SMCMIXER_BTN_BANK_LEFT, SMCMIXER_BTN_BANK_RIGHT, SMCMIXER_BTN_UP,
    SMCMIXER_BTN_DOWN, SMCMIXER_BTN_LEFT, SMCMIXER_BTN_RIGHT, SMCMIXER_BTN_BLUETOOTH,
    SMCMIXER_BTN_SHIFT,
};
/* the fader LEDs run right to left: 45 is above fader 8 */
#define SMCMIXER_LED_FADER(f)   (52 - (f))

enum {
    SMCMIXER_EV_BUTTON_DOWN = 1,    /* index: button */
    SMCMIXER_EV_BUTTON_UP,
    SMCMIXER_EV_ENCODER,            /* index: encoder; value: steps, + = clockwise */
    SMCMIXER_EV_FADER,              /* index: fader; value: 0..1023 */
};

typedef struct {
    uint8_t type;
    uint8_t index;
    int16_t value;
} smcmixer_event_t;

void smcmixer_hw_begin(void);
int smcmixer_event_get(smcmixer_event_t *ev);      /* 1 when one was waiting */
int smcmixer_button(int button);                    /* 1 = held */
int32_t smcmixer_encoder(int encoder);              /* steps since start */
uint16_t smcmixer_fader(int fader);                 /* 0..1023, smoothed */
uint64_t smcmixer_matrix_raw(void);                 /* bit (column * 4 + row): closed */
void smcmixer_led_set(int led, int on);
int smcmixer_led(int led);
void smcmixer_led_raw(int pos, int on);             /* pos = phase * 16 + segment */

#ifdef __cplusplus
}
#endif

#endif
