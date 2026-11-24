/*
 * SMC-Mixer configuration: the model of docs/mixer-config.md, stored in the VM and edited over
 * USB-MIDI SysEx by the web editor (editor/).
 *
 * Binary layout, version 4 (little endian; the editor builds and parses the same bytes,
 * editor/config.js):
 *
 *   header, 84 bytes
 *     0  "SMXC"                magic
 *     4  u16 version           4
 *     6  u16 n_entries
 *     8  u8  mode              0 = DAW (stock DAW mode, Mackie), 1 = CC. The mode the mixer is in:
 *                              Shift + Left / Right change it and store it.
 *     9  u8  fader_takeover    0 = jump, 1 = pickup
 *    10  u8  fader_led         0 = off, 1 = lit while a fader does not match its value
 *    11  u8  follow_incoming   0 / 1: incoming CC / pitch bend / pressure set the known value
 *    12  u8  remember_values   0 / 1: known values kept across power cycles
 *    13  u8  accel             0 = off, 1 = on
 *    14  u8  accel_ms_x3       encoder steps closer than this (ms) count x3
 *    15  u8  accel_ms_x2       ... x2
 *    16  u8  pages             1..8
 *    17  u8  channel_base[8]   per page: strip s sends on MIDI channel channel_base + s
 *   CC mode's profile (what a control does when nothing is mapped):
 *    25  u8  cc_fader          fader: this CC on the strip's channel
 *    26  u8  cc_fader_shift    Shift + fader: this CC (MCFG_NONE: the same as without Shift)
 *    27  u8  cc_mute           Mute buttons toggle this CC 127 / 0 (MCFG_NONE: they send nothing)
 *    28  u8  cc_encoder        encoder: this CC on its strip's channel (MCFG_NONE: nothing)
 *    29  u8  cc_solo           with MCC_SOLO_CC: Solo toggles this CC 127 / 0 on the strip's channel
 *                              (version 2: cc_focus, see focus below)
 *    30  u8  cc_flags          MCC_SOLO_MUTES: Solo mutes the other tracks (with cc_mute; wins
 *                              over MCC_SOLO_CC); MCC_SOLO_CC: Solo sends cc_solo; neither: nothing
 *                              (0x02 was MCC_SPECIAL up to version 3, see fader_special)
 *    31  u8  reserved
 *    32  char name[28]         the template the profile came from ("OP-XY - blen2r"), NUL padded
 *    60  u8  focus[2][8]       the focus groups, [0] = Rec, [1] = Track Stop: Rec on a strip makes
 *                              encoder k send CC focus[0][k] on that strip's channel, for every k
 *                              that is not MCFG_NONE (the others stay as they are); again: off.
 *                              A group of only MCFG_NONE: the button sends nothing.
 *    76  u8  fader_special[4]  Shift + Mute / Solo / Rec / Track Stop: what that strip's fader
 *                              sends until the button is pressed again: a CC (0..127),
 *                              MCFG_FM_PITCHBEND, MCFG_FM_PRESSURE, or MCFG_NONE (Shift + the
 *                              button does what the button does without Shift)
 *    80  u32 crc32             (zlib) over the header with this field 0, then the entries
 *   entries, 10 bytes each, at 84
 *     0  u8  control           MCTL_* below
 *     1  u8  layer             bits 0..3 page (0 = all pages, 1..8), bit 4 = Shift layer,
 *                              bit 5 = DAW mode (0: the mapping is for CC mode)
 *     2  u8  type              MT_*
 *     3  u8  channel           0..15
 *     4  u8  number            CC / note / program
 *     5  u8  flags             MF_*
 *     6  u8  min               value sent for "off" / the low end
 *     7  u8  max               value sent for "on" / the high end
 *     8  u8  reserved[2]
 *
 * Versions 2 (64-byte header: cc_focus at 29, one CC for both groups, no focus[], crc32 at 60, up
 * to 240 entries) and 3 (80-byte header, crc32 at 76; MCC_SPECIAL = mod wheel / pitch bend /
 * pressure / pan) are read once and converted. All are stored as MCFG_MAX_BYTES under one key.
 */
#ifndef MIXER_CONFIG_H
#define MIXER_CONFIG_H

#include <stdint.h>

#define MCFG_MAGIC          0x43584D53u     /* "SMXC" little endian */
#define MCFG_VERSION        4
#define MCFG_HEADER         84
#define MCFG_ENTRY          10
#define MCFG_MAX_ENTRIES    238
#define MCFG_MAX_BYTES      2464            /* stored size, as version 2: header + entries fit */
#define MCFG_PAGES          8
#define MCFG_LAYERS         ((MCFG_PAGES + 1) * 2 * 2)  /* (all pages + 8) x (normal, shift) x mode */
#define MCFG_LAYER_SHIFT    0x10
#define MCFG_LAYER_DAW      0x20
#define MCFG_NONE           0x80            /* "no CC" in the profile fields */
#define MCFG_FM_PITCHBEND   0x81            /* fader_special: pitch bend */
#define MCFG_FM_PRESSURE    0x82            /* fader_special: channel pressure */
#define MCC_SOLO_MUTES      0x01
#define MCC_SOLO_CC         0x04
#define MCFG_NAME_LEN       28

/* controls */
#define MCTL_KNOB(k)        (k)             /* 0..7, the encoders */
#define MCTL_FADER(f)       (8 + (f))       /* 8..15 */
#define MCTL_BUTTON(b)      (16 + (b))      /* 16..60, buttons in smcmixer_hw.h order */
#define MCTL_COUNT          61

enum {
    MT_DISABLED = 0,        /* send nothing */
    MT_CC,
    MT_NOTE,
    MT_PROGRAM,
    MT_PITCHBEND,
    MT_PRESSURE,
    MT_BASE,                /* what the base profile does here (explicitly, above a broader layer) */
    MT_TRANSPORT,           /* buttons: MIDI Start / Stop, alternately (LED = playing); fields unused */
    MT_COUNT
};

#define MF_BUTTON_MASK      0x03            /* buttons: */
#define MF_MOMENTARY        0x00            /*   max on press, min on release */
#define MF_TOGGLE           0x01            /*   max / min on alternate presses, LED = state */
#define MF_TRIGGER          0x02            /*   max on press only */
#define MF_TAKEOVER_MASK    0x0C            /* faders: */
#define MF_TAKEOVER_INHERIT 0x00
#define MF_TAKEOVER_JUMP    0x04
#define MF_TAKEOVER_PICKUP  0x08
#define MF_ACCEL_MASK       0x30            /* encoders: */
#define MF_ACCEL_INHERIT    0x00
#define MF_ACCEL_OFF        0x10
#define MF_ACCEL_ON         0x20

enum { MODE_DAW = 0, MODE_CC = 1 };

typedef struct {
    uint8_t control, layer, type, channel, number, flags, min, max, rsv[2];
} mcfg_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t version, n_entries;
    uint8_t mode, fader_takeover, fader_led, follow_incoming, remember_values;
    uint8_t accel, accel_ms_x3, accel_ms_x2, pages;
    uint8_t channel_base[MCFG_PAGES];
    uint8_t cc_fader, cc_fader_shift, cc_mute, cc_encoder, cc_solo, cc_flags, rsv1;
    char name[MCFG_NAME_LEN];
    uint8_t focus[2][8];                    /* [0] = Rec, [1] = Track Stop */
    uint8_t fader_special[4];               /* Shift + Mute / Solo / Rec / Track Stop */
    uint32_t crc32;
} __attribute__((packed)) mcfg_header_t;

/* the active configuration */
extern const mcfg_header_t *mcfg;

void mcfg_begin(void);                      /* load from the VM, or the defaults */
/* The entry for a control on page (1..8) with Shift held or not, in the current mode, by the
 * resolution order: page + shift, page, all pages + shift, all pages. NULL: the mode's profile. */
const mcfg_entry_t *mcfg_resolve(int control, int page, int shift);
/* switch DAW / CC mode and store it */
void mcfg_set_mode(int mode);
/* SysEx of the editor protocol (F0 7D 4D ... F7), whole message. Returns 1 if it was ours. */
int mcfg_sysex(const uint8_t *msg, uint32_t len);
/* called when a committed configuration replaced the active one */
extern void mixer_config_changed(void);
/* SysEx out, USB only (mixer_app.c) */
extern void mixer_sysex_send(const uint8_t *msg, uint32_t len);

#endif
