/*
 * SMC-Mixer configuration: the model of docs/mixer-config.md, stored in the VM and edited over
 * USB-MIDI SysEx by the web editor (editor/).
 *
 * Binary layout (little endian; the editor builds and parses the same bytes, editor/config.js):
 *
 *   header, 32 bytes
 *     0  "SMXC"                magic
 *     4  u16 version           1
 *     6  u16 n_entries
 *     8  u8  base              0 = DAW (stock DAW mode), 1 = OP-XY profile
 *     9  u8  fader_takeover    0 = jump, 1 = pickup
 *    10  u8  fader_led         0 = off, 1 = lit while a fader does not match its value
 *    11  u8  follow_incoming   0 / 1: incoming CC / pitch bend / pressure set the known value
 *    12  u8  remember_values   0 / 1: known values kept across power cycles
 *    13  u8  accel             0 = off, 1 = on
 *    14  u8  accel_ms_x3       knob steps closer than this (ms) count x3
 *    15  u8  accel_ms_x2       ... x2
 *    16  u8  pages             1..8
 *    17  u8  track_base[8]     per page: strip s controls track / channel track_base + s
 *    25  u8  reserved[3]
 *    28  u32 crc32             (zlib) over the header with this field 0, then the entries
 *   entries, 10 bytes each, at 32
 *     0  u8  control           MCTL_* below
 *     1  u8  layer             bits 0..3 page (0 = all pages, 1..8), bit 4 = Shift layer
 *     2  u8  type              MT_*
 *     3  u8  channel           0..15
 *     4  u8  number            CC / note / program
 *     5  u8  flags             MF_*
 *     6  u8  min               value sent for "off" / the low end
 *     7  u8  max               value sent for "on" / the high end
 *     8  u8  reserved[2]
 */
#ifndef MIXER_CONFIG_H
#define MIXER_CONFIG_H

#include <stdint.h>

#define MCFG_MAGIC          0x43584D53u     /* "SMXC" little endian */
#define MCFG_VERSION        1
#define MCFG_HEADER         32
#define MCFG_ENTRY          10
#define MCFG_MAX_ENTRIES    240
#define MCFG_MAX_BYTES      (MCFG_HEADER + MCFG_MAX_ENTRIES * MCFG_ENTRY)
#define MCFG_PAGES          8
#define MCFG_LAYERS         ((MCFG_PAGES + 1) * 2)  /* (all pages + 8) x (normal, shift) */

/* controls */
#define MCTL_KNOB(k)        (k)             /* 0..7 */
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
#define MF_ACCEL_MASK       0x30            /* knobs: */
#define MF_ACCEL_INHERIT    0x00
#define MF_ACCEL_OFF        0x10
#define MF_ACCEL_ON         0x20

enum { BASE_DAW = 0, BASE_OPXY = 1 };

typedef struct {
    uint8_t control, layer, type, channel, number, flags, min, max, rsv[2];
} mcfg_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t version, n_entries;
    uint8_t base, fader_takeover, fader_led, follow_incoming, remember_values;
    uint8_t accel, accel_ms_x3, accel_ms_x2, pages;
    uint8_t track_base[MCFG_PAGES];
    uint8_t rsv[3];
    uint32_t crc32;
} __attribute__((packed)) mcfg_header_t;

/* the active configuration */
extern const mcfg_header_t *mcfg;

void mcfg_begin(void);                      /* load from the VM, or the defaults */
/* The entry for a control on page (1..8) with Shift held or not, by the resolution order:
 * page + shift, page, all pages + shift, all pages. NULL: the base profile. */
const mcfg_entry_t *mcfg_resolve(int control, int page, int shift);
/* SysEx of the editor protocol (F0 7D 4D ... F7), whole message. Returns 1 if it was ours. */
int mcfg_sysex(const uint8_t *msg, uint32_t len);
/* called when a committed configuration replaced the active one */
extern void mixer_config_changed(void);
/* SysEx out, USB only (mixer_app.c) */
extern void mixer_sysex_send(const uint8_t *msg, uint32_t len);

#endif
