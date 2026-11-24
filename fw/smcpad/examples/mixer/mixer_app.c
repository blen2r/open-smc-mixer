/*
 * SMC-Mixer application: a base profile with the user's mappings on top (docs/mixer-config.md).
 *
 * For every control event the configuration is asked first (mixer_config.c: page + Shift, page,
 * all pages + Shift, all pages); only when nothing is mapped does the base profile act:
 *
 * Two modes, as the stock firmware; Shift + Left = DAW, Shift + Right = CC (the lit arrow shows
 * the mode while Shift is held; the mode is stored). Each mode has its own mappings.
 *
 * CC mode: the profile in the configuration (mixer_config.h, cc_*; the editor's templates, e.g.
 * "OP-XY - blen2r"). Strip n of page p sends on MIDI channel channel_base[p] + n:
 *   fader       CC cc_fader (takeover per setting; the LED above it is lit while it has not
 *               picked its value up); Shift + fader: CC cc_fader_shift while Shift is held
 *   Mute        toggles mute, CC cc_mute = 127 / 0
 *   Solo        (MCC_SOLO_MUTES) toggles solo: the other tracks are muted while any is soloed;
 *               (MCC_SOLO_CC) toggles CC cc_solo = 127 / 0 instead
 *   Rec         focus group Rec: every encoder k with a CC in focus[0][k] sends that CC on this
 *               track's channel (the others stay as they are); again: off
 *   Track Stop  the same with focus[1]. If the two groups share an encoder, only one can be on:
 *               turning one on blinks both buttons fast for FOCUS_CONFLICT_MS, then the other
 *               goes off and this one on
 *   encoder k   CC cc_encoder on channel channel_base + k (absolute: the mixer keeps the value)
 *   Play        MIDI Start / Stop, alternately (LED = playing; a Start / Stop received sets it)
 *   Pause / Record / Rewind / FF / << / >> / arrows: the stock DAW notes
 *   Shift + Mute / Solo / Rec / Track Stop   fader special mode, fader_special[row]: the strip's
 *               fader sends that CC / pitch bend / channel pressure instead, toggles; the button's
 *               LED blinks while it is on; the button alone also ends it. MCFG_NONE: Shift + the
 *               button does what the button does
 * DAW mode: the stock DAW mode (Mackie Control): encoders CC 16-23 relative, faders pitch bend 1-8
 *   with touch notes 104-111, buttons notes; LEDs follow the host.
 * Always:
 *   Shift + Rewind / Fast-forward        page down / up; the LED above fader <page> blinks fast
 *                                        (and keeps blinking while Shift is held)
 *   Shift + Bluetooth + Play, held 2 s   the bootloader (DFU), for updates
 *   SysEx F0 7D 4D ... on USB-MIDI       the editor (mixer_config.c), 70 = bootloader,
 *                                        71 = echo diagnostics
 */
#include "smcpad_usb_cfg.h"
#if SMC_MIXER
#include "system/includes.h"
#include "smcpad_rt.h"
#include "smcmixer_hw.h"
#include "mixer_app.h"
#include "mixer_config.h"

#define NUM_STRIPS      8
#define UNKNOWN         0xFF
#define SHIFT_HELD()    smcmixer_button(SMCMIXER_BTN_SHIFT)

extern void mcfg_event(int control, int value, int page, int shift);

/* A timestamp that is never 0 (0 = "never"). Not `millis() | 1`: that can be 1 ms in the
 * future, and `now - stamp` then wraps to ~4e9 -- every "within N ms" test fails. */
static u32 stamp(void)
{
    u32 t = smcpad_millis();

    return t ? t : 1;
}

/* ------------------------------------------------------------------ state */


static u8 page;                                     /* 0-based */
static u8 mute[16], solo[16], sent_mute[16];        /* CC mode: per track */
static s8 focus[2] = { -1, -1 };                    /* CC mode: Rec / Track Stop group: track, -1 = none */
#define FOCUS_CONFLICT_MS 500
static struct {
    u32 at;                                         /* 0 = nothing waiting */
    u8 group, track;
} focus_wait;                                       /* a group waiting for the other to go off */
static u8 fader_mode[16];                           /* CC mode: per track, 0 = normal, row + 1 = special */
static u8 playing;
static u32 page_shown_at;
static u32 boot_combo_since;
static u8 btn_toggle[SMCMIXER_NUM_BUTTONS];         /* mapped toggle buttons: state */
static const mcfg_entry_t *btn_press_entry[SMCMIXER_NUM_BUTTONS];   /* what a press used */
static u8 daw_led[SMCMIXER_NUM_BUTTONS];            /* DAW: LEDs as the host sets them */

/* known values (last sent or received), UNKNOWN until then: what pickup compares with */
static u8 cc_val[16][128];
static u8 pb_val[16], at_val[16];

int mixer_current_page(void)
{
    return page + 1;
}

static int track_of(int strip)
{
    return (mcfg->channel_base[page] + strip) & 15;
}

/* ------------------------------------------------------------------ MIDI out */

static void midi_send(u8 status, u8 d1, u8 d2)
{
    u8 msg[3] = { status, d1, d2 };
    u32 len = ((status & 0xE0) == 0xC0) ? 2 : 3;
    u8 pkt[4] = { (u8)(status >> 4), status, d1, len == 2 ? 0 : d2 };

    smcpad_usbmidi_write(pkt);
    mixer_blemidi_send(msg, len);
}

/* a one-byte real-time message (MIDI Start FA, Stop FC, ...) on USB and BLE */
static void midi_realtime(u8 b)
{
    u8 pkt[4] = { 0x0F, b, 0, 0 };

    smcpad_usbmidi_write(pkt);
    mixer_blemidi_send(&b, 1);
}

void mixer_sysex_send(const u8 *d, u32 n)           /* whole message, F0 .. F7, USB only */
{
    for (u32 i = 0; i < n; i += 3) {
        u32 left = n - i;
        u8 pkt[4] = { 0x04, d[i], left > 1 ? d[i + 1] : 0, left > 2 ? d[i + 2] : 0 };

        if (left <= 3) {
            pkt[0] = (u8)(0x04 + left);     /* CIN 5 / 6 / 7: ends with 1 / 2 / 3 bytes */
        }
        smcpad_usbmidi_write(pkt);
    }
}

/* ------------------------------------------------------------------ remembered values */
#define STORE_KEY_VALUES    0
#define STORE_MAGIC         0x4D585632u     /* "MXV2" */
#define SAVE_AFTER_MS       5000

typedef struct {
    u32 magic;
    u8 cc[16][16];                          /* CC 0..15 of each channel (mod, volume, mute, pan, 12-15) */
    u8 pb[16], at[16];
} saved_values_t;

static u32 values_dirty_at;

static void values_changed(void)
{
    if (mcfg->remember_values) {
        values_dirty_at = stamp();
    }
}

static void values_load(void)
{
    saved_values_t v;

    if (!mcfg->remember_values ||
        smcpad_store_read(STORE_KEY_VALUES, &v, sizeof(v)) != sizeof(v) || v.magic != STORE_MAGIC) {
        return;
    }
    for (int ch = 0; ch < 16; ch++) {
        memcpy(cc_val[ch], v.cc[ch], 16);
        pb_val[ch] = v.pb[ch];
        at_val[ch] = v.at[ch];
    }
}

static void values_save_if_due(void)
{
    saved_values_t v;

    if (!values_dirty_at || smcpad_millis() - values_dirty_at < SAVE_AFTER_MS) {
        return;
    }
    values_dirty_at = 0;
    v.magic = STORE_MAGIC;
    for (int ch = 0; ch < 16; ch++) {
        memcpy(v.cc[ch], cc_val[ch], 16);
        v.pb[ch] = pb_val[ch];
        v.at[ch] = at_val[ch];
    }
    smcpad_store_write(STORE_KEY_VALUES, &v, sizeof(v));
}

static void send_cc(int ch, int num, int v)
{
    midi_send(0xB0 | (ch & 15), num & 127, v & 127);
    cc_val[ch & 15][num & 127] = (u8)v;
    values_changed();
}

/* ------------------------------------------------------------------ echoes
 * Incoming values for a control we are sending on right now are our own echo (MIDI thru, BLE
 * latency): within ECHO_MS of our last send and inside the range we sent since, ignored. */
#define ECHO_MS         1500
typedef struct { u32 at; u8 lo, hi; } echo_t;

static void echo_note(echo_t *e, u8 v)
{
    u32 now = stamp();

    if (!e->at || now - e->at >= ECHO_MS) {
        e->lo = e->hi = v;
    }
    e->lo = v < e->lo ? v : e->lo;
    e->hi = v > e->hi ? v : e->hi;
    e->at = now;
}

static int echo_of(const echo_t *e, u8 v)
{
    return e->at && smcpad_millis() - e->at < ECHO_MS && v >= e->lo && v <= e->hi;
}

/* ------------------------------------------------------------------ targets
 * What a fader or knob controls right now: kind, channel, number, value range. */
enum { TK_NONE, TK_CC, TK_PITCHBEND, TK_PRESSURE, TK_NOTE, TK_PROGRAM, TK_DAW_FADER, TK_DAW_KNOB };

typedef struct { u8 kind, ch, num, lo, hi, pickup, accel; } target_t;

static u8 target_value(const target_t *t)
{
    switch (t->kind) {
    case TK_CC: return cc_val[t->ch][t->num];
    case TK_PITCHBEND: case TK_DAW_FADER: return pb_val[t->ch];
    case TK_PRESSURE: return at_val[t->ch];
    default: return UNKNOWN;
    }
}

static int same_target(const target_t *a, const target_t *b)
{
    return a->kind == b->kind && a->ch == b->ch && a->num == b->num;
}

/* does an incoming message (type 0xB0 / 0xE0 / 0xD0) concern this target? */
static int is_target(const target_t *t, u8 type, u8 ch, u8 num)
{
    if (t->ch != ch) {
        return 0;
    }
    return (type == 0xB0 && t->kind == TK_CC && t->num == num) ||
           (type == 0xE0 && (t->kind == TK_PITCHBEND || t->kind == TK_DAW_FADER)) ||
           (type == 0xD0 && t->kind == TK_PRESSURE);
}

static target_t target_from_entry(const mcfg_entry_t *e)
{
    target_t t = { TK_NONE, e->channel, e->number, e->min, e->max, 0, 0 };

    switch (e->type) {
    case MT_CC: t.kind = TK_CC; break;
    case MT_NOTE: t.kind = TK_NOTE; break;
    case MT_PROGRAM: t.kind = TK_PROGRAM; break;
    case MT_PITCHBEND: t.kind = TK_PITCHBEND; break;
    case MT_PRESSURE: t.kind = TK_PRESSURE; break;
    default: t.kind = TK_NONE; break;
    }
    if (t.hi < t.lo) {
        u8 x = t.lo; t.lo = t.hi; t.hi = x;
    }
    if (t.lo == t.hi && t.lo == 0) {
        t.hi = 127;
    }
    return t;
}

/* ------------------------------------------------------------------ mute / solo (OP-XY) */

static void update_mutes(void)
{
    int any_solo = 0;

    if (mcfg->cc_mute & MCFG_NONE) {
        return;
    }
    for (int t = 0; t < 16; t++) {
        any_solo |= solo[t] && (mcfg->cc_flags & MCC_SOLO_MUTES);
    }
    for (int t = 0; t < 16; t++) {
        u8 m = mute[t] || (any_solo && !solo[t]);

        if (m != sent_mute[t]) {
            sent_mute[t] = m;
            send_cc(t, mcfg->cc_mute, m ? 127 : 0);
        }
    }
}

/* ------------------------------------------------------------------ faders */

static target_t fader_target[NUM_STRIPS];
static u8 picked[NUM_STRIPS];
static s8 fader_side[NUM_STRIPS];
static u8 fader_pos7[NUM_STRIPS];
static u16 fader_raw[NUM_STRIPS];
static u8 fader_seen[NUM_STRIPS];
static echo_t fader_echo[NUM_STRIPS];
static u32 daw_touch_at[NUM_STRIPS];

static target_t fader_resolve(int f)
{
    const mcfg_entry_t *e = mcfg_resolve(MCTL_FADER(f), page + 1, SHIFT_HELD());
    target_t n = { TK_CC, (u8)track_of(f), mcfg->cc_fader, 0, 127, mcfg->fader_takeover, 0 };

    if (e) {
        u8 tk = e->flags & MF_TAKEOVER_MASK;

        n = target_from_entry(e);
        n.pickup = tk == MF_TAKEOVER_JUMP ? 0 : tk == MF_TAKEOVER_PICKUP ? 1 : mcfg->fader_takeover;
        return n;
    }
    if (mcfg->mode == MODE_DAW) {
        n.kind = TK_DAW_FADER;
        n.ch = (u8)f;
        n.num = 0;
        n.pickup = 0;                       /* a DAW moves to the fader; the LED shows the host */
        return n;
    }
    if (fader_mode[n.ch]) {                 /* a special mode (Shift + a strip button) */
        u8 sp = mcfg->fader_special[fader_mode[n.ch] - 1];

        if (sp == MCFG_FM_PITCHBEND) {
            n.kind = TK_PITCHBEND;
            return n;
        }
        if (sp == MCFG_FM_PRESSURE) {
            n.kind = TK_PRESSURE;
            return n;
        }
        if (!(sp & MCFG_NONE)) {
            n.num = sp;
            return n;
        }
    }
    if (n.num & MCFG_NONE) {
        n.kind = TK_NONE;
        return n;
    }
    if (SHIFT_HELD() && !(mcfg->cc_fader_shift & MCFG_NONE)) {
        n.num = mcfg->cc_fader_shift;       /* Shift + fader (pan for the OP-XY) while held */
    }
    return n;
}

static u8 scaled(const target_t *t, u8 pos7)
{
    return (u8)(t->lo + (pos7 * (t->hi - t->lo) + 63) / 127);
}

static void fader_retarget(int f)
{
    target_t n = fader_resolve(f);

    if (!same_target(&n, &fader_target[f]) || n.lo != fader_target[f].lo || n.hi != fader_target[f].hi) {
        u8 v = target_value(&n), here = scaled(&n, fader_pos7[f]);

        fader_target[f] = n;
        picked[f] = !n.pickup || v == UNKNOWN || v == here || !fader_seen[f];
        fader_side[f] = v == UNKNOWN ? 0 : here > v ? 1 : -1;
    } else {
        fader_target[f].pickup = n.pickup;
    }
}

static void faders_retarget_all(void)
{
    for (int f = 0; f < NUM_STRIPS; f++) {
        fader_retarget(f);
    }
}

static void fader_send(int f)
{
    target_t *t = &fader_target[f];
    u8 v = scaled(t, fader_pos7[f]);

    switch (t->kind) {
    case TK_CC:
        if (v == cc_val[t->ch][t->num]) {
            return;
        }
        echo_note(&fader_echo[f], v);
        send_cc(t->ch, t->num, v);
        break;
    case TK_PITCHBEND:
    case TK_DAW_FADER: {
        u32 b = (u32)fader_raw[f] * 16383 / 1023;

        if (t->kind == TK_DAW_FADER) {
            if (!daw_touch_at[f]) {
                midi_send(0x90, 104 + f, 127);      /* Mackie fader touch */
            }
            daw_touch_at[f] = stamp();
        }
        if (t->lo != 0 || t->hi != 127) {
            b = (u32)v * 16383 / 127;
        }
        echo_note(&fader_echo[f], v);
        midi_send(0xE0 | t->ch, b & 127, (b >> 7) & 127);
        pb_val[t->ch] = v;
        values_changed();
        break;
    }
    case TK_PRESSURE:
        if (v == at_val[t->ch]) {
            return;
        }
        echo_note(&fader_echo[f], v);
        midi_send(0xD0 | t->ch, v, 0);
        at_val[t->ch] = v;
        values_changed();
        break;
    case TK_NOTE:
        midi_send(0x90 | t->ch, t->num, v ? v : 1);
        midi_send(0x80 | t->ch, t->num, 0);
        break;
    case TK_PROGRAM:
        midi_send(0xC0 | t->ch, v, 0);
        break;
    }
}

static void fader_moved(int f, int raw)
{
    u8 p = (u8)((raw * 128) >> 10);

    fader_raw[f] = (u16)raw;
    fader_pos7[f] = p > 127 ? 127 : p;
    if (!fader_seen[f]) {
        fader_seen[f] = 1;                  /* the first reading: just where it rests */
        fader_target[f].kind = 0xFF;
        fader_retarget(f);
        return;
    }
    fader_retarget(f);
    if (fader_target[f].kind == TK_NONE) {
        return;
    }
    if (!picked[f]) {
        u8 want = target_value(&fader_target[f]), here = scaled(&fader_target[f], fader_pos7[f]);
        s8 side = here > want ? 1 : here < want ? -1 : 0;

        if (side == 0 || side != fader_side[f]) {
            picked[f] = 1;                  /* reached or crossed the value */
        } else {
            return;
        }
    }
    fader_send(f);
}

/* ------------------------------------------------------------------ knobs */

static target_t knob_target[SMCMIXER_NUM_ENCODERS];
static echo_t knob_echo[SMCMIXER_NUM_ENCODERS];
static u16 knob_pb[16];                             /* pitch bend knobs: 14-bit position */
static u8 knob_prog[16];

static target_t knob_resolve(int k)
{
    const mcfg_entry_t *e = mcfg_resolve(MCTL_KNOB(k), page + 1, SHIFT_HELD());
    target_t n = { TK_CC, 0, mcfg->cc_encoder, 0, 127, 0, mcfg->accel };

    if (e) {
        u8 a = e->flags & MF_ACCEL_MASK;

        n = target_from_entry(e);
        n.accel = a == MF_ACCEL_OFF ? 0 : a == MF_ACCEL_ON ? 1 : mcfg->accel;
        return n;
    }
    if (mcfg->mode == MODE_DAW) {
        n.kind = TK_DAW_KNOB;
        n.num = 16 + k;
        n.accel = 0;
        return n;
    }
    n.ch = (u8)track_of(k);                 /* default: CC cc_encoder of the strip's track */
    for (int g = 0; g < 2; g++) {
        if (focus[g] >= 0 && !(mcfg->focus[g][k] & MCFG_NONE)) {
            n.ch = (u8)focus[g], n.num = mcfg->focus[g][k];
            break;
        }
    }
    if (n.num & MCFG_NONE) {
        n.kind = TK_NONE;
    }
    return n;
}

static int knob_accel(int k, int steps, int on)
{
    static u32 last[SMCMIXER_NUM_ENCODERS];
    u32 now = smcpad_millis(), dt = now - last[k];

    last[k] = now;
    if (!on) {
        return steps;
    }
    return steps * (dt < mcfg->accel_ms_x3 ? 3 : dt < mcfg->accel_ms_x2 ? 2 : 1);
}

static void knob_turned(int k, int steps)
{
    target_t t = knob_resolve(k);

    knob_target[k] = t;
    steps = knob_accel(k, steps, t.accel);
    switch (t.kind) {
    case TK_DAW_KNOB: {
        int n = steps < 0 ? -steps : steps;

        n = n > 63 ? 63 : n;
        midi_send(0xB0, t.num, steps < 0 ? 64 + n : n);    /* Mackie V-Pot, sign-magnitude */
        break;
    }
    case TK_CC:
    case TK_PRESSURE: {
        u8 *val = t.kind == TK_CC ? &cc_val[t.ch][t.num] : &at_val[t.ch];
        int v = (*val == UNKNOWN ? (t.lo + t.hi) / 2 : *val) + steps;

        v = v < t.lo ? t.lo : v > t.hi ? t.hi : v;
        if (v != *val) {
            echo_note(&knob_echo[k], (u8)v);
            if (t.kind == TK_CC) {
                send_cc(t.ch, t.num, v);
            } else {
                midi_send(0xD0 | t.ch, v, 0);
                *val = (u8)v;
                values_changed();
            }
        }
        break;
    }
    case TK_PITCHBEND: {
        int v = (int)knob_pb[t.ch] + steps * 128;

        v = v < 0 ? 0 : v > 16383 ? 16383 : v;
        knob_pb[t.ch] = (u16)v;
        midi_send(0xE0 | t.ch, v & 127, (v >> 7) & 127);
        pb_val[t.ch] = (u8)(v >> 7);
        echo_note(&knob_echo[k], (u8)(v >> 7));
        break;
    }
    case TK_PROGRAM: {
        int v = knob_prog[t.ch] + (steps > 0 ? 1 : -1);

        v = v < t.lo ? t.lo : v > t.hi ? t.hi : v;
        if (v != knob_prog[t.ch]) {
            knob_prog[t.ch] = (u8)v;
            midi_send(0xC0 | t.ch, v, 0);
        }
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ buttons */

static const u8 daw_note_of_button[SMCMIXER_NUM_BUTTONS] = {
    16, 8, 0, 24,  17, 9, 1, 25,  18, 10, 2, 26,  19, 11, 3, 27,
    20, 12, 4, 28,  21, 13, 5, 29,  22, 14, 6, 30,  23, 15, 7, 31,
    94, 93, 95, 91, 92, 46, 47, 96, 97, 98, 99, 0xFF, 0xFF,
};

/* Start / Stop, alternately: what devices (the OP-XY) start and stop on */
static void transport_toggle(void)
{
    playing ^= 1;
    midi_realtime(playing ? 0xFA : 0xFC);
}

static void mapped_button(int b, const mcfg_entry_t *e, int down)
{
    u8 mode = e->flags & MF_BUTTON_MASK;
    u8 lo = e->min, hi = e->max ? e->max : 127;
    int on;

    if (e->type == MT_TRANSPORT) {
        if (down) {
            transport_toggle();
        }
        return;
    }

    if (mode == MF_TOGGLE) {
        if (!down) {
            return;
        }
        btn_toggle[b] ^= 1;
        on = btn_toggle[b];
    } else if (mode == MF_TRIGGER) {
        if (!down) {
            return;
        }
        on = 1;
    } else {
        on = down;
    }
    switch (e->type) {
    case MT_CC:
        send_cc(e->channel, e->number, on ? hi : lo);
        break;
    case MT_NOTE:
        if (on) {
            midi_send(0x90 | e->channel, e->number, hi);
        } else {
            midi_send(0x80 | e->channel, e->number, 0);
        }
        if (mode == MF_TRIGGER) {
            midi_send(0x80 | e->channel, e->number, 0);
        }
        break;
    case MT_PROGRAM:
        if (on) {
            midi_send(0xC0 | e->channel, e->number, 0);
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ focus groups (CC mode) */

static int focus_used(int g)
{
    for (int k = 0; k < 8; k++) {
        if (!(mcfg->focus[g][k] & MCFG_NONE)) {
            return 1;
        }
    }
    return 0;
}

static int focus_overlap(void)
{
    for (int k = 0; k < 8; k++) {
        if (!(mcfg->focus[0][k] & MCFG_NONE) && !(mcfg->focus[1][k] & MCFG_NONE)) {
            return 1;
        }
    }
    return 0;
}

/* the waiting group takes over: the other goes off */
static void focus_finish(void)
{
    focus[!focus_wait.group] = -1;
    focus[focus_wait.group] = (s8)focus_wait.track;
    focus_wait.at = 0;
}

static void focus_tick(void)
{
    if (focus_wait.at && smcpad_millis() - focus_wait.at >= FOCUS_CONFLICT_MS) {
        focus_finish();
    }
}

/* Rec (g = 0) / Track Stop (g = 1) of track t pressed */
static void focus_press(int g, int t)
{
    if (focus_wait.at) {                    /* pressed during a blink: finish it first */
        focus_finish();
    }
    if (!focus_used(g)) {
        return;
    }
    if (focus[g] == t) {
        focus[g] = -1;
    } else if (focus[!g] >= 0 && focus_overlap()) {
        focus_wait.group = (u8)g;
        focus_wait.track = (u8)t;
        focus_wait.at = stamp();
    } else {
        focus[g] = (s8)t;
    }
}

/* the LED of Rec / Track Stop of track t: on while focused; both buttons of a conflict blink fast */
static int focus_led(int g, int t, int fast)
{
    if (focus_wait.at && ((g == focus_wait.group && t == focus_wait.track) || (g != focus_wait.group && t == focus[g]))) {
        return fast;
    }
    return focus[g] == t;
}

static void cc_strip_button(int strip, int row, int down)
{
    int t = track_of(strip);
    u8 m = (u8)(row + 1);

    if (!down) {
        return;
    }
    /* Shift + the button: the fader's special mode on / off; the button alone while its mode is
     * on (its LED blinking) also just ends the mode */
    if (fader_mode[t] == m || (SHIFT_HELD() && mcfg->fader_special[row] != MCFG_NONE)) {
        fader_mode[t] = fader_mode[t] == m ? 0 : m;
        fader_retarget(strip);
        return;
    }
    switch (row) {
    case SMCMIXER_ROW_MUTE: mute[t] ^= 1; update_mutes(); break;
    case SMCMIXER_ROW_SOLO:
        if (mcfg->cc_flags & MCC_SOLO_MUTES) {
            solo[t] ^= 1;
            update_mutes();
        } else if ((mcfg->cc_flags & MCC_SOLO_CC) && !(mcfg->cc_solo & MCFG_NONE)) {
            solo[t] ^= 1;                   /* the device's own solo; the LED shows it */
            send_cc(t, mcfg->cc_solo, solo[t] ? 127 : 0);
        }
        break;
    case SMCMIXER_ROW_REC:
    case SMCMIXER_ROW_STOP:
        focus_press(row - SMCMIXER_ROW_REC, t);
        break;
    }
}

static void set_page(int p)
{
    if (p < 0 || p >= mcfg->pages) {
        return;
    }
    page = (u8)p;
    page_shown_at = stamp();
    faders_retarget_all();
}

static void button(int b, int down)
{
    int shift = SHIFT_HELD();
    const mcfg_entry_t *e;

    mcfg_event(MCTL_BUTTON(b), down, page + 1, shift);
    if (b == SMCMIXER_BTN_SHIFT) {
        faders_retarget_all();              /* the Shift layer, Shift + fader */
        return;
    }
    if (shift && (b == SMCMIXER_BTN_LEFT || b == SMCMIXER_BTN_RIGHT)) {  /* reserved: the mode */
        if (down) {
            mcfg_set_mode(b == SMCMIXER_BTN_LEFT ? MODE_DAW : MODE_CC);
            mixer_config_changed();
        }
        btn_press_entry[b] = NULL;
        return;
    }
    if (shift && (b == SMCMIXER_BTN_REWIND || b == SMCMIXER_BTN_FF)) {    /* reserved: pages */
        if (down) {
            set_page(page + (b == SMCMIXER_BTN_FF ? 1 : -1));
        }
        btn_press_entry[b] = NULL;
        return;
    }
    /* a release goes where its press went, even if Shift or the page changed in between */
    e = down ? mcfg_resolve(MCTL_BUTTON(b), page + 1, shift) : btn_press_entry[b];
    if (down) {
        btn_press_entry[b] = e;
    }
    if (e) {
        mapped_button(b, e, down);
        return;
    }
    if (mcfg->mode == MODE_CC && b < 32) {
        cc_strip_button(b / 4, b % 4, down);
        return;
    }
    if (mcfg->mode == MODE_CC && b == SMCMIXER_BTN_PLAY) {    /* CC mode: Start / Stop */
        if (down) {
            transport_toggle();
        }
        return;
    }
    if (b == SMCMIXER_BTN_PLAY && down) {
        playing ^= 1;
    }
    if (daw_note_of_button[b] != 0xFF) {
        midi_send(down ? 0x90 : 0x80, daw_note_of_button[b], down ? 127 : 0);
    }
}

/* ------------------------------------------------------------------ LEDs */

static void leds_update(void)
{
    u32 now = smcpad_millis();
    int blink = (now / 250) & 1, fast = (now / 60) & 1;
    int page_blink = page_shown_at && now - page_shown_at < 1000;
    int shift = SHIFT_HELD();

    for (int b = 0; b < SMCMIXER_NUM_BUTTONS; b++) {
        const mcfg_entry_t *e;
        int on = 0;

        if (b == SMCMIXER_BTN_SHIFT) {
            on = shift;
        } else if (shift && (b == SMCMIXER_BTN_LEFT || b == SMCMIXER_BTN_RIGHT)) {
            on = (b == SMCMIXER_BTN_RIGHT) == (mcfg->mode == MODE_CC);     /* the mode */
        } else if (b == SMCMIXER_BTN_BLUETOOTH) {
            on = mixer_blemidi_connected() ? 1 : blink && (now / 1000) % 3 == 0;
        } else if ((e = mcfg_resolve(MCTL_BUTTON(b), page + 1, shift)) != NULL) {
            on = e->type == MT_TRANSPORT ? playing
                 : (e->flags & MF_BUTTON_MASK) == MF_TOGGLE ? btn_toggle[b] : smcmixer_button(b);
        } else if (mcfg->mode == MODE_CC && b < 32) {
            int s = b / 4, row = b % 4, t = track_of(s);

            if (fader_mode[t] == row + 1) {
                on = blink;
            } else {
                on = row == 0 ? sent_mute[t] : row == 1 ? solo[t] : focus_led(row - 2, t, fast);
            }
        } else if (mcfg->mode == MODE_CC && b == SMCMIXER_BTN_PLAY) {
            on = playing;
        } else {
            on = daw_led[b];
        }
        smcmixer_led_set(b, on);
    }
    for (int s = 0; s < NUM_STRIPS; s++) {
        int on;

        if (page_blink) {
            on = s == page && fast;
        } else if (!mcfg->fader_led) {
            on = 0;
        } else if (fader_target[s].kind == TK_DAW_FADER) {
            u8 v = pb_val[s];

            on = v != UNKNOWN && (v > fader_pos7[s] + 2 || v + 2 < fader_pos7[s]);
        } else {
            on = !picked[s];
        }
        /* Shift held: the LED above fader <page> blinks fast (the page). If that fader also has a
         * value to pick up, the blinking comes in bursts with the LED solid in between. */
        if (shift && !page_blink && s == page) {
            on = (on && ((now / 400) & 1)) ? 1 : fast;
        }
        smcmixer_led_set(SMCMIXER_LED_FADER(s), on);
    }
}

/* ------------------------------------------------------------------ MIDI in */

static struct {
    u32 n_in, n_echo, n_accept;
} dbg;

static void midi_in(u8 status, u8 d1, u8 d2)
{
    u8 type = status & 0xF0, ch = status & 0x0F;
    u8 val = type == 0xD0 ? d1 & 127 : d2 & 127;

    if (status == 0xFA || status == 0xFB || status == 0xFC) {     /* the device started / stopped */
        playing = status != 0xFC;
        return;
    }

    /* DAW: the host sets the button LEDs with notes */
    if (mcfg->mode == MODE_DAW && (type == 0x90 || type == 0x80) && ch == 0) {
        for (int b = 0; b < SMCMIXER_NUM_BUTTONS; b++) {
            if (daw_note_of_button[b] == (d1 & 127)) {
                daw_led[b] = type == 0x90 && d2;
            }
        }
        return;
    }
    if (type != 0xB0 && type != 0xE0 && type != 0xD0) {
        return;
    }
    if (!mcfg->follow_incoming && !(mcfg->mode == MODE_DAW && type == 0xE0)) {
        return;
    }
    dbg.n_in++;
    for (int f = 0; f < NUM_STRIPS; f++) {
        if (is_target(&fader_target[f], type, ch, d1 & 127) && echo_of(&fader_echo[f], val)) {
            dbg.n_echo++;
            return;
        }
    }
    for (int k = 0; k < SMCMIXER_NUM_ENCODERS; k++) {
        if (is_target(&knob_target[k], type, ch, d1 & 127) && echo_of(&knob_echo[k], val)) {
            dbg.n_echo++;
            return;
        }
    }
    dbg.n_accept++;
    if (type == 0xB0) {
        cc_val[ch][d1 & 127] = val;
    } else if (type == 0xE0) {
        pb_val[ch] = val;
        knob_pb[ch] = (u16)((d2 & 127) << 7 | (d1 & 127));
    } else {
        at_val[ch] = val;
    }
    values_changed();
    for (int f = 0; f < NUM_STRIPS; f++) {
        target_t *t = &fader_target[f];

        if (t->pickup && is_target(t, type, ch, d1 & 127)) {
            u8 here = scaled(t, fader_pos7[f]);

            picked[f] = val == here;
            fader_side[f] = here > val ? 1 : -1;
        }
    }
}

/* ------------------------------------------------------------------ SysEx from USB-MIDI */

static u8 sysex[200];
static u32 sysex_len;

static void dbg_reply(void)
{
    u8 b[16], *p = b;

    *p++ = 0xF0; *p++ = 0x7D; *p++ = 0x4D; *p++ = 0x71;
    for (int k = 0; k < 3; k++) {
        u32 v = k == 0 ? dbg.n_in : k == 1 ? dbg.n_echo : dbg.n_accept;

        *p++ = (v >> 7) & 127;
        *p++ = v & 127;
    }
    *p++ = 0xF7;
    mixer_sysex_send(b, p - b);
}

static void sysex_done(void)
{
    static const u8 enter_boot[] = { 0xF0, 0x7D, 0x4D, 0x70, 0xF7 };
    static const u8 get_dbg[] = { 0xF0, 0x7D, 0x4D, 0x71, 0xF7 };

    if (sysex_len == sizeof(enter_boot) && !memcmp(sysex, enter_boot, sizeof(enter_boot))) {
        smcpad_enter_bootloader();
    }
    if (sysex_len == sizeof(get_dbg) && !memcmp(sysex, get_dbg, sizeof(get_dbg))) {
        dbg_reply();
        return;
    }
    mcfg_sysex(sysex, sysex_len);
}

/* USB-MIDI event packets: CIN 4 = SysEx start / continue (3 bytes), 5 / 6 / 7 = SysEx ends
 * with 1 / 2 / 3 bytes; 8..E channel messages. */
static void usbmidi_in(const u8 pkt[4])
{
    u8 cin = pkt[0] & 0x0F;
    u32 n = cin == 4 || cin == 7 ? 3 : cin == 6 ? 2 : cin == 5 ? 1 : 0;

    if (cin >= 0x8 && cin <= 0xE) {
        midi_in(pkt[1], pkt[2], pkt[3]);
        return;
    }
    if (cin == 0xF && pkt[1] >= 0xF8) {    /* a real-time byte (Start / Stop / clock) */
        midi_in(pkt[1], 0, 0);
        return;
    }
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

/* ------------------------------------------------------------------ configuration */

void mixer_config_changed(void)
{
    if (page >= mcfg->pages) {
        page = 0;
    }
    memset(btn_toggle, 0, sizeof(btn_toggle));
    focus[0] = focus[1] = -1;               /* the groups may have changed */
    memset(fader_mode, 0, sizeof(fader_mode));  /* and the special modes */
    focus_wait.at = 0;
    for (int f = 0; f < NUM_STRIPS; f++) {
        fader_target[f].kind = 0xFF;        /* forces a fresh retarget */
    }
    faders_retarget_all();
}

/* ------------------------------------------------------------------ main */

void mixer_app_run(void)
{
    smcmixer_event_t ev;
    u8 msg[3], pkt[4];

    memset(cc_val, UNKNOWN, sizeof(cc_val));
    memset(pb_val, UNKNOWN, sizeof(pb_val));
    memset(at_val, UNKNOWN, sizeof(at_val));
    for (int c = 0; c < 16; c++) {
        knob_pb[c] = 8192;
    }
    mcfg_begin();
    values_load();
    for (int f = 0; f < NUM_STRIPS; f++) {
        fader_target[f].kind = 0xFF;
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
                mcfg_event(MCTL_KNOB(ev.index), ev.value & 0xFFFF, page + 1, SHIFT_HELD());
                knob_turned(ev.index, ev.value);
                break;
            case SMCMIXER_EV_FADER:
                mcfg_event(MCTL_FADER(ev.index), ev.value, page + 1, SHIFT_HELD());
                fader_moved(ev.index, ev.value);
                break;
            }
        }
        while (mixer_blemidi_read(msg)) {
            midi_in(msg[0], msg[1], msg[2]);
        }
        while (smcpad_usbmidi_read(pkt)) {
            usbmidi_in(pkt);
        }
        /* DAW fader touch: released 300 ms after the last move */
        for (int f = 0; f < NUM_STRIPS; f++) {
            if (daw_touch_at[f] && smcpad_millis() - daw_touch_at[f] > 300) {
                daw_touch_at[f] = 0;
                midi_send(0x80, 104 + f, 0);
            }
        }
        /* Shift + Bluetooth + Play held 2 s: into the bootloader for an update */
        if (SHIFT_HELD() && smcmixer_button(SMCMIXER_BTN_BLUETOOTH) && smcmixer_button(SMCMIXER_BTN_PLAY)) {
            if (!boot_combo_since) {
                boot_combo_since = stamp();
            } else if (smcpad_millis() - boot_combo_since > 2000) {
                smcpad_enter_bootloader();
            }
        } else {
            boot_combo_since = 0;
        }
        focus_tick();
        leds_update();
        values_save_if_due();
        mixer_blemidi_poll();
        smcpad_usbmidi_flush();
        smcpad_yield();
    }
}

#endif /* SMC_MIXER */
