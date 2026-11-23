/*
 * SMC-Mixer application, first version: the OP-XY profile of docs/mixer-config.md, built in
 * (the configurable model and the web editor come next). Everything goes out on USB-MIDI and
 * BLE-MIDI at once.
 *
 *   strip n (page 1: tracks 1..8, page 2: tracks 9..16) = OP-XY track / MIDI channel
 *   fader       volume, CC 7 (pickup: it takes over only once it reaches the track's value; the
 *               LED above it is lit while it has not)
 *   Mute        toggles mute, CC 9 = 127 / 0
 *   Solo        toggles solo: the other tracks of the page are muted (CC 9) while any is soloed
 *   Rec         focus group A: knobs 1-4 send CC 12-15 of this track (again: off)
 *   Select      focus group B: knobs 5-8 send CC 12-15 of this track (again: off)
 *   knob k      default CC 12 on channel k (absolute: the mixer keeps the value)
 *   Play        note 94 (as the stock DAW mode, which the OP-XY follows); its LED toggles
 *   Shift + Mute / Solo / Rec / Select   fader special mode: mod wheel / pitch bend / channel
 *               pressure / pan (CC 10), toggles; the button's LED blinks while it is on
 *   Shift + fader (no special mode)      pan (CC 10), while Shift is held
 *   Shift + Rewind / Fast-forward        page 1 / 2; the LED above fader <page> blinks fast
 *   Shift + Bluetooth + Play, held 2 s   the bootloader (DFU), for updates
 *   SysEx F0 7D 4D 70 F7 on USB-MIDI       the same, for host/mixer_update.sh (editor protocol
 *                                          of docs/mixer-config.md: 7D = non-commercial, 4D = "M")
 */
#include "smcpad_usb_cfg.h"
#if SMC_MIXER
#include "system/includes.h"
#include "smcpad_rt.h"
#include "smcmixer_hw.h"
#include "mixer_app.h"

#define NUM_STRIPS      8
#define NUM_PAGES       2
#define UNKNOWN         0xFF

enum { FM_VOLUME, FM_MODWHEEL, FM_PITCHBEND, FM_PRESSURE, FM_PAN, FM_COUNT };

/* ------------------------------------------------------------------ state */

static u8 page;                                     /* 0..NUM_PAGES-1 */
static u8 mute[16], solo[16], sent_mute[16];        /* per track */
static s8 focus_a = -1, focus_b = -1;               /* track, -1 = none */
static u8 fader_mode[16];                           /* per track: FM_* */
static u8 playing;
static u32 page_shown_at;                           /* ms; the page LED blinks after a change */
static u32 boot_combo_since;

/* last value sent per target, UNKNOWN until the first: what pickup compares with */
static u8 cc_val[16][128];
static u8 pb_val[16], at_val[16];
static u8 knob_val[16][4];                          /* CC 12..15 */

/* per fader: what it controls now, and whether it has picked that value up */
typedef struct { u8 kind, ch, num; } target_t;      /* kind 0 CC, 1 pitch bend, 2 pressure */
static target_t fader_target[NUM_STRIPS];
static u8 picked[NUM_STRIPS];
static s8 fader_side[NUM_STRIPS];                   /* sign of (position - value) when not picked */
static u8 fader_pos7[NUM_STRIPS];
static u16 fader_raw[NUM_STRIPS];

static int track_of(int strip)
{
    return page * NUM_STRIPS + strip;
}

/* ------------------------------------------------------------------ MIDI out */

static void midi_send(u8 status, u8 d1, u8 d2)
{
    u8 msg[3] = { status, d1, d2 };
    u32 len = ((status & 0xE0) == 0xC0) ? 2 : 3;
    u8 pkt[4] = { (u8)(status >> 4), status, d1, d2 };

    if (len == 2) {
        pkt[3] = 0;
    }
    smcpad_usbmidi_write(pkt);
    mixer_blemidi_send(msg, len);
}

static void send_cc(int ch, int num, int v)
{
    midi_send(0xB0 | (ch & 15), num & 127, v & 127);
    cc_val[ch & 15][num & 127] = (u8)v;
}

/* ------------------------------------------------------------------ mute / solo */

static void update_mutes(void)
{
    int any_solo = 0;

    for (int t = 0; t < 16; t++) {
        any_solo |= solo[t];
    }
    for (int t = 0; t < 16; t++) {
        u8 m = mute[t] || (any_solo && !solo[t]);

        if (m != sent_mute[t]) {
            sent_mute[t] = m;
            send_cc(t, 9, m ? 127 : 0);
        }
    }
}

/* ------------------------------------------------------------------ faders */

static u8 target_value(const target_t *t)
{
    return t->kind == 0 ? cc_val[t->ch][t->num] : t->kind == 1 ? pb_val[t->ch] : at_val[t->ch];
}

static void fader_retarget(int f)
{
    int t = track_of(f);
    target_t n = { 0, (u8)t, 7 };

    switch (fader_mode[t]) {
    case FM_MODWHEEL: n.num = 1; break;
    case FM_PITCHBEND: n.kind = 1; break;
    case FM_PRESSURE: n.kind = 2; break;
    case FM_PAN: n.num = 10; break;
    default:
        if (smcmixer_button(SMCMIXER_BTN_SHIFT)) {
            n.num = 10;                     /* Shift + fader: pan while Shift is held */
        }
        break;
    }
    if (n.kind != fader_target[f].kind || n.ch != fader_target[f].ch || n.num != fader_target[f].num) {
        u8 v = target_value(&n);

        fader_target[f] = n;
        picked[f] = v == UNKNOWN || v == fader_pos7[f];
        fader_side[f] = v == UNKNOWN ? 0 : fader_pos7[f] > v ? 1 : -1;
    }
}

static void fader_send(int f)
{
    target_t *t = &fader_target[f];
    u8 v = fader_pos7[f];

    if (t->kind != 1 && v == target_value(t)) {
        return;                             /* the same value again: nothing to say */
    }

    switch (t->kind) {
    case 0:
        send_cc(t->ch, t->num, v);
        break;
    case 1: {
        u32 b = (u32)fader_raw[f] * 16383 / 1023;

        midi_send(0xE0 | t->ch, b & 127, (b >> 7) & 127);
        pb_val[t->ch] = v;
        break;
    }
    default:
        midi_send(0xD0 | t->ch, v, 0);
        at_val[t->ch] = v;
        break;
    }
}

static void fader_moved(int f, int raw)
{
    u8 v = (u8)((raw * 128) >> 10);

    fader_raw[f] = (u16)raw;
    fader_pos7[f] = v > 127 ? 127 : v;
    fader_retarget(f);
    if (!picked[f]) {
        u8 want = target_value(&fader_target[f]);
        s8 side = fader_pos7[f] > want ? 1 : fader_pos7[f] < want ? -1 : 0;

        if (side == 0 || side != fader_side[f]) {
            picked[f] = 1;                  /* reached or crossed the value */
        } else {
            return;
        }
    }
    fader_send(f);
}

static void faders_retarget_all(void)
{
    for (int f = 0; f < NUM_STRIPS; f++) {
        fader_retarget(f);
    }
}

/* ------------------------------------------------------------------ knobs */

/* Acceleration: steps that come quickly count more (a fast spin covers the range in under a
 * turn), slow ones stay single for fine control. */
static int knob_accel(int k, int steps)
{
    static u32 last[SMCMIXER_NUM_ENCODERS];
    u32 now = smcpad_millis(), dt = now - last[k];

    last[k] = now;
    /* edges ~15 ms apart are a moderate pace (half a turn a second): no boost there */
    return steps * (dt < 6 ? 3 : dt < 12 ? 2 : 1);
}

static void knob_turned(int k, int steps)
{
    int ch, slot;

    steps = knob_accel(k, steps);

    if (k < 4 && focus_a >= 0) {
        ch = focus_a, slot = k;
    } else if (k >= 4 && focus_b >= 0) {
        ch = focus_b, slot = k - 4;
    } else {
        ch = k, slot = 0;                   /* default: CC 12 on channel k + 1 */
    }
    int v = knob_val[ch][slot] + steps;

    v = v < 0 ? 0 : v > 127 ? 127 : v;
    if (v != knob_val[ch][slot]) {
        knob_val[ch][slot] = (u8)v;
        send_cc(ch, 12 + slot, v);
    }
}

/* ------------------------------------------------------------------ buttons */

static void strip_button(int strip, int row, int down)
{
    int t = track_of(strip);

    if (!down) {
        return;
    }
    static const u8 mode_of_row[4] = { FM_MODWHEEL, FM_PITCHBEND, FM_PRESSURE, FM_PAN };
    u8 m = mode_of_row[row];

    /* Shift + the button: the fader's special mode on / off. The button alone while its mode
     * is on (its LED blinking) also just ends the mode. */
    if (smcmixer_button(SMCMIXER_BTN_SHIFT) || fader_mode[t] == m) {
        fader_mode[t] = fader_mode[t] == m ? FM_VOLUME : m;
        fader_retarget(strip);
        return;
    }
    switch (row) {
    case SMCMIXER_ROW_MUTE:
        mute[t] ^= 1;
        update_mutes();
        break;
    case SMCMIXER_ROW_SOLO:
        solo[t] ^= 1;
        update_mutes();
        break;
    case SMCMIXER_ROW_REC:
        focus_a = focus_a == t ? -1 : t;
        break;
    case SMCMIXER_ROW_SELECT:
        focus_b = focus_b == t ? -1 : t;
        break;
    }
}

static void set_page(int p)
{
    if (p < 0 || p >= NUM_PAGES) {
        return;
    }
    page = (u8)p;
    page_shown_at = smcpad_millis() | 1;
    faders_retarget_all();
}

static void button(int b, int down)
{
    int shift = smcmixer_button(SMCMIXER_BTN_SHIFT);

    if (b < 32) {
        strip_button(b / 4, b % 4, down);
        return;
    }
    switch (b) {
    case SMCMIXER_BTN_PLAY:
        if (shift) {
            break;
        }
        midi_send(down ? 0x90 : 0x80, 94, down ? 127 : 0);
        if (down) {
            playing ^= 1;
        }
        break;
    case SMCMIXER_BTN_REWIND:
        if (shift && down) {
            set_page(page - 1);
        } else if (!shift) {
            midi_send(down ? 0x90 : 0x80, 91, down ? 127 : 0);
        }
        break;
    case SMCMIXER_BTN_FF:
        if (shift && down) {
            set_page(page + 1);
        } else if (!shift) {
            midi_send(down ? 0x90 : 0x80, 92, down ? 127 : 0);
        }
        break;
    case SMCMIXER_BTN_SHIFT:
        faders_retarget_all();              /* Shift + fader = pan */
        break;
    default: {
        /* the rest as the stock DAW mode */
        static const u8 daw_note[] = { 94, 93, 95, 91, 92, 46, 47, 96, 97, 98, 99 };

        if (b >= 32 && b <= 42 && !shift) {
            midi_send(down ? 0x90 : 0x80, daw_note[b - 32], down ? 127 : 0);
        }
        break;
    }
    }
}

/* ------------------------------------------------------------------ LEDs */

static void leds_update(void)
{
    u32 now = smcpad_millis();
    int blink = (now / 250) & 1, fast = (now / 60) & 1;
    int page_blink = page_shown_at && now - page_shown_at < 1000;

    for (int s = 0; s < NUM_STRIPS; s++) {
        int t = track_of(s);
        u8 m = fader_mode[t];

        smcmixer_led_set(SMCMIXER_BTN_STRIP(s, 0), m == FM_MODWHEEL ? blink : sent_mute[t]);
        smcmixer_led_set(SMCMIXER_BTN_STRIP(s, 1), m == FM_PITCHBEND ? blink : solo[t]);
        smcmixer_led_set(SMCMIXER_BTN_STRIP(s, 2), m == FM_PRESSURE ? blink : focus_a == t);
        smcmixer_led_set(SMCMIXER_BTN_STRIP(s, 3), m == FM_PAN ? blink : focus_b == t);
        if (page_blink) {
            smcmixer_led_set(SMCMIXER_LED_FADER(s), s == page && fast);
        } else {
            smcmixer_led_set(SMCMIXER_LED_FADER(s), !picked[s]);
        }
    }
    smcmixer_led_set(SMCMIXER_BTN_PLAY, playing);
    smcmixer_led_set(SMCMIXER_BTN_SHIFT, smcmixer_button(SMCMIXER_BTN_SHIFT));
    smcmixer_led_set(SMCMIXER_BTN_BLUETOOTH, mixer_blemidi_connected() ? 1 : blink && (now / 1000) % 3 == 0);
}

/* ------------------------------------------------------------------ SysEx from USB-MIDI */

static u8 sysex[32];
static u32 sysex_len;

static void sysex_done(void)
{
    static const u8 enter_boot[] = { 0xF0, 0x7D, 0x4D, 0x70, 0xF7 };

    if (sysex_len == sizeof(enter_boot) && !memcmp(sysex, enter_boot, sizeof(enter_boot))) {
        smcpad_enter_bootloader();
    }
}

/* USB-MIDI event packets: CIN 4 = SysEx start / continue (3 bytes), 5 / 6 / 7 = SysEx ends
 * with 1 / 2 / 3 bytes. */
static void usbmidi_in(const u8 pkt[4])
{
    u8 cin = pkt[0] & 0x0F;
    u32 n = cin == 4 || cin == 7 ? 3 : cin == 6 ? 2 : cin == 5 ? 1 : 0;

    if (!n) {
        return;
    }
    if (pkt[1] == 0xF0) {
        sysex_len = 0;
    }
    for (u32 i = 0; i < n && sysex_len < sizeof(sysex); i++) {
        sysex[sysex_len++] = pkt[1 + i];
    }
    if (cin != 4) {
        sysex_done();
        sysex_len = 0;
    }
}

/* ------------------------------------------------------------------ main */

void mixer_app_run(void)
{
    smcmixer_event_t ev;
    u8 msg[3], pkt[4];

    memset(cc_val, UNKNOWN, sizeof(cc_val));
    memset(pb_val, UNKNOWN, sizeof(pb_val));
    memset(at_val, UNKNOWN, sizeof(at_val));
    memset(knob_val, 64, sizeof(knob_val));
    for (int f = 0; f < NUM_STRIPS; f++) {
        fader_target[f].kind = 0xFF;        /* forces the first retarget */
    }
    smcmixer_hw_begin();
    mixer_blemidi_begin();
    faders_retarget_all();

    for (;;) {
        while (smcmixer_event_get(&ev)) {
            switch (ev.type) {
            case SMCMIXER_EV_BUTTON_DOWN:
            case SMCMIXER_EV_BUTTON_UP:
                button(ev.index, ev.type == SMCMIXER_EV_BUTTON_DOWN);
                break;
            case SMCMIXER_EV_ENCODER:
                knob_turned(ev.index, ev.value);
                break;
            case SMCMIXER_EV_FADER:
                fader_moved(ev.index, ev.value);
                break;
            }
        }
        while (mixer_blemidi_read(msg)) {
        }                                   /* nothing to do with input yet */
        while (smcpad_usbmidi_read(pkt)) {
            usbmidi_in(pkt);
        }
        /* Shift + Bluetooth + Play held 2 s: into the bootloader for an update */
        if (smcmixer_button(SMCMIXER_BTN_SHIFT) && smcmixer_button(SMCMIXER_BTN_BLUETOOTH) &&
            smcmixer_button(SMCMIXER_BTN_PLAY)) {
            if (!boot_combo_since) {
                boot_combo_since = smcpad_millis() | 1;
            } else if (smcpad_millis() - boot_combo_since > 2000) {
                smcpad_enter_bootloader();
            }
        } else {
            boot_combo_since = 0;
        }
        leds_update();
        mixer_blemidi_poll();
        smcpad_usbmidi_flush();
        smcpad_yield();                     /* USB, the 1200-baud touch, the boot counter; the
                                             * BT stack's tasks have the higher priority */
    }
}

#endif /* SMC_MIXER */
