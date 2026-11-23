/*
 * SMC-Mixer configuration: storage, resolution, and the editor's SysEx protocol
 * (docs/mixer-config.md, "Editor protocol"):
 *
 *   F0 7D 4D <cmd> <seq> <data, 8-to-7 packed> F7          host -> device
 *   F0 7D 4D 7F <seq> <status> <data, packed> F7           device -> host (reply)
 *
 * Packing: each group of up to 7 data bytes is sent as one byte holding their top bits (bit i =
 * bit 7 of byte i) followed by the 7 bytes' low 7 bits.
 *
 *   01 hello        -> fw version u16, config version u16, max bytes u16, controls u8, pages u8,
 *                      current page u8, current length u16, current mode u8
 *   02 read         offset u16, length u8 (<= 128)  -> those bytes of the active configuration
 *   03 write        offset u16, bytes               -> into the staging copy
 *   04 commit       length u16                      -> checked (magic, version, size, CRC),
 *                                                      stored, made active
 *   05 revert                                       -> staging = active
 *   06 factory                                      -> defaults, stored, active
 *   10 events       on u8                           -> device sends 10 <control> <value u16>
 *                                                      <page> <shift> for every move / press
 * Status: 0 ok, 1 bad request, 2 bad configuration, 3 storage failed.
 */
#include "smcpad_usb_cfg.h"
#if SMC_MIXER
#include "system/includes.h"
#include "smcpad_rt.h"
#include "mixer_config.h"

#define FW_VERSION          0x0002
#define STORE_KEY_CONFIG_V1 1               /* version 1, 2432 bytes: read once, converted */
#define STORE_KEY_CONFIG    2
#define V1_HEADER           32
#define V1_MAX_BYTES        (V1_HEADER + MCFG_MAX_ENTRIES * MCFG_ENTRY)

_Static_assert(sizeof(mcfg_header_t) == MCFG_HEADER, "mcfg_header_t must match the 64-byte header");
_Static_assert(sizeof(mcfg_entry_t) == MCFG_ENTRY, "mcfg_entry_t must be 10 bytes");

static u8 active[MCFG_MAX_BYTES] __attribute__((aligned(4)));
static u8 staging[MCFG_MAX_BYTES] __attribute__((aligned(4)));
static u8 index_map[MCTL_COUNT][MCFG_LAYERS];   /* entry + 1, 0 = none */
const mcfg_header_t *mcfg = (const mcfg_header_t *)active;
u8 mcfg_events_on;
extern int mixer_current_page(void);            /* 1..8 */

/* ------------------------------------------------------------------ helpers */

static u32 crc32_update(u32 crc, const u8 *p, u32 n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
        }
    }
    return ~crc;
}

static u32 blob_len(const u8 *b)
{
    const mcfg_header_t *h = (const mcfg_header_t *)b;

    return MCFG_HEADER + (u32)h->n_entries * MCFG_ENTRY;
}

/* CRC over a header of `hl` bytes whose CRC field (at hl - 4) counts as 0, then n entries */
static u32 crc_of(const u8 *b, u32 hl, u32 n)
{
    u8 head[MCFG_HEADER];

    memcpy(head, b, hl);
    memset(head + hl - 4, 0, 4);
    return crc32_update(crc32_update(0, head, hl), b + hl, n * MCFG_ENTRY);
}

static u32 blob_crc(const u8 *b)
{
    return crc_of(b, MCFG_HEADER, ((const mcfg_header_t *)b)->n_entries);
}

static int blob_ok(const u8 *b, u32 len)
{
    const mcfg_header_t *h = (const mcfg_header_t *)b;

    if (h->magic != MCFG_MAGIC || h->version != MCFG_VERSION || h->n_entries > MCFG_MAX_ENTRIES ||
        blob_len(b) != len || h->pages < 1 || h->pages > MCFG_PAGES || h->mode > MODE_CC ||
        h->crc32 != blob_crc(b)) {
        return 0;
    }
    for (u32 i = 0; i < h->n_entries; i++) {
        const mcfg_entry_t *e = (const mcfg_entry_t *)(b + MCFG_HEADER + i * MCFG_ENTRY);

        if (e->control >= MCTL_COUNT || (e->layer & 0xF) > MCFG_PAGES || (e->layer & ~0x3F) ||
            e->type >= MT_COUNT || e->channel > 15) {
            return 0;
        }
    }
    return 1;
}

/* CC mode's profile, as editor/templates/opxy-blen2r.json */
static void profile_defaults(mcfg_header_t *h)
{
    static const char name[] = "OP-XY - blen2r";

    for (int p = 0; p < MCFG_PAGES; p++) {
        h->track_base[p] = (p & 1) ? 8 : 0;         /* tracks 1-8, 9-16, 1-8, ... */
    }
    h->cc_fader = 7;                                /* volume */
    h->cc_fader_shift = 10;                         /* pan */
    h->cc_mute = 9;
    h->cc_knob = 12;
    h->cc_focus = 12;                               /* the OP-XY's track parameters, CC 12-15 */
    h->cc_flags = MCC_SOLO_MUTES | MCC_SPECIAL;
    memset(h->name, 0, sizeof(h->name));
    memcpy(h->name, name, sizeof(name) - 1);
}

static void defaults(u8 *b)
{
    mcfg_header_t *h = (mcfg_header_t *)b;

    memset(b, 0, MCFG_MAX_BYTES);
    h->magic = MCFG_MAGIC;
    h->version = MCFG_VERSION;
    h->n_entries = 0;
    h->mode = MODE_DAW;                             /* as the stock firmware */
    h->fader_takeover = 1;
    h->fader_led = 1;
    h->follow_incoming = 1;
    h->remember_values = 1;
    h->accel = 1;
    h->accel_ms_x3 = 6;
    h->accel_ms_x2 = 12;
    h->pages = 2;
    profile_defaults(h);
    h->crc32 = blob_crc(b);
}

static u32 layer_index(int page, int shift, int daw)
{
    return (u32)(daw * (MCFG_PAGES + 1) * 2 + page * 2 + shift);
}

static void build_index(void)
{
    memset(index_map, 0, sizeof(index_map));
    for (u32 i = 0; i < mcfg->n_entries; i++) {
        const mcfg_entry_t *e = (const mcfg_entry_t *)(active + MCFG_HEADER + i * MCFG_ENTRY);
        u32 layer = layer_index(e->layer & 0xF, (e->layer >> 4) & 1, (e->layer >> 5) & 1);

        index_map[e->control][layer] = (u8)(i + 1);     /* a later duplicate wins */
    }
}

const mcfg_entry_t *mcfg_resolve(int control, int page, int shift)
{
    static const u8 order_shift[4][2] = { { 1, 1 }, { 1, 0 }, { 0, 1 }, { 0, 0 } };  /* (page?, shift) */

    if (control < 0 || control >= MCTL_COUNT) {
        return NULL;
    }
    for (int k = 0; k < 4; k++) {
        int pg = order_shift[k][0] ? page : 0;
        int sh = order_shift[k][1];
        u8 i;

        if (sh && !shift) {
            continue;
        }
        if (order_shift[k][0] && (page < 1 || page > MCFG_PAGES)) {
            continue;
        }
        i = index_map[control][layer_index(pg, sh, mcfg->mode == MODE_DAW)];
        if (i) {
            const mcfg_entry_t *e = (const mcfg_entry_t *)(active + MCFG_HEADER + (i - 1) * MCFG_ENTRY);

            return e->type == MT_BASE ? NULL : e;
        }
    }
    return NULL;
}

static int store_active(void)
{
    return smcpad_store_write(STORE_KEY_CONFIG, active, MCFG_MAX_BYTES) == MCFG_MAX_BYTES;
}

/* A version 1 configuration (in staging, as read from the old key) into version 2 (in active).
 * base OP-XY -> CC mode with the OP-XY profile; base DAW -> DAW mode, the mappings DAW ones. */
static int convert_v1(const u8 *v1, u8 *out)
{
    const u32 magic = v1[0] | v1[1] << 8 | v1[2] << 16 | (u32)v1[3] << 24;
    u16 version = v1[4] | v1[5] << 8, n = v1[6] | v1[7] << 8;
    u32 crc = v1[28] | v1[29] << 8 | v1[30] << 16 | (u32)v1[31] << 24;
    mcfg_header_t *h = (mcfg_header_t *)out;

    if (magic != MCFG_MAGIC || version != 1 || n > MCFG_MAX_ENTRIES || crc != crc_of(v1, V1_HEADER, n)) {
        return 0;
    }
    defaults(out);
    h->mode = v1[8] ? MODE_CC : MODE_DAW;
    memcpy(&h->fader_takeover, v1 + 9, 16);         /* takeover .. track_base[8]: same places */
    h->n_entries = n;
    memcpy(out + MCFG_HEADER, v1 + V1_HEADER, n * MCFG_ENTRY);
    if (h->mode == MODE_DAW) {
        for (u32 i = 0; i < n; i++) {
            out[MCFG_HEADER + i * MCFG_ENTRY + 1] |= MCFG_LAYER_DAW;
        }
    }
    h->crc32 = blob_crc(out);
    return blob_ok(out, blob_len(out));
}

void mcfg_begin(void)
{
    if (smcpad_store_read(STORE_KEY_CONFIG, active, MCFG_MAX_BYTES) != MCFG_MAX_BYTES ||
        !blob_ok(active, blob_len(active) <= MCFG_MAX_BYTES ? blob_len(active) : 0)) {
        memset(staging, 0, sizeof(staging));
        if (smcpad_store_read(STORE_KEY_CONFIG_V1, staging, V1_MAX_BYTES) == V1_MAX_BYTES &&
            convert_v1(staging, active)) {
            store_active();
        } else {
            defaults(active);
        }
    }
    memcpy(staging, active, MCFG_MAX_BYTES);
    build_index();
}

void mcfg_set_mode(int mode)
{
    mcfg_header_t *h = (mcfg_header_t *)active;

    if (mode == h->mode || (mode != MODE_DAW && mode != MODE_CC)) {
        return;
    }
    h->mode = (u8)mode;
    h->crc32 = blob_crc(active);
    memcpy(staging, active, MCFG_HEADER);
    store_active();
}

/* ------------------------------------------------------------------ protocol */

static u32 unpack7(const u8 *in, u32 n, u8 *out, u32 max)
{
    u32 o = 0;

    for (u32 i = 0; i < n; ) {
        u8 msb = in[i++];

        for (u32 k = 0; k < 7 && i < n && o < max; k++, i++) {
            out[o++] = in[i] | (((msb >> k) & 1) << 7);
        }
    }
    return o;
}

static u32 pack7(const u8 *in, u32 n, u8 *out)
{
    u32 o = 0;

    for (u32 i = 0; i < n; i += 7) {
        u32 g = n - i < 7 ? n - i : 7;
        u8 msb = 0;

        for (u32 k = 0; k < g; k++) {
            msb |= ((in[i + k] >> 7) & 1) << k;
        }
        out[o++] = msb;
        for (u32 k = 0; k < g; k++) {
            out[o++] = in[i + k] & 0x7F;
        }
    }
    return o;
}

static void reply(u8 seq, u8 status, const u8 *data, u32 n)
{
    u8 msg[8 + 160];
    u32 o = 0;

    msg[o++] = 0xF0; msg[o++] = 0x7D; msg[o++] = 0x4D; msg[o++] = 0x7F;
    msg[o++] = seq & 0x7F;
    msg[o++] = status;
    if (n > 128) {
        n = 128;
    }
    o += pack7(data, n, msg + o);
    msg[o++] = 0xF7;
    mixer_sysex_send(msg, o);
}

void mcfg_event(int control, int value, int page, int shift)
{
    u8 d[5] = { (u8)control, (u8)(value >> 8), (u8)value, (u8)page, (u8)shift };
    u8 msg[16];
    u32 o = 0;

    if (!mcfg_events_on) {
        return;
    }
    msg[o++] = 0xF0; msg[o++] = 0x7D; msg[o++] = 0x4D; msg[o++] = 0x10; msg[o++] = 0;
    o += pack7(d, sizeof(d), msg + o);
    msg[o++] = 0xF7;
    mixer_sysex_send(msg, o);
}

int mcfg_sysex(const u8 *msg, u32 len)
{
    u8 d[160], r[16];
    u32 n;
    u8 cmd, seq;

    if (len < 6 || msg[0] != 0xF0 || msg[1] != 0x7D || msg[2] != 0x4D || msg[len - 1] != 0xF7) {
        return 0;
    }
    cmd = msg[3];
    seq = msg[4];
    n = unpack7(msg + 5, len - 6, d, sizeof(d));

    switch (cmd) {
    case 0x01: {
        u32 l = blob_len(active);

        r[0] = FW_VERSION & 0xFF; r[1] = FW_VERSION >> 8;
        r[2] = MCFG_VERSION; r[3] = 0;
        r[4] = MCFG_MAX_BYTES & 0xFF; r[5] = MCFG_MAX_BYTES >> 8;
        r[6] = MCTL_COUNT; r[7] = MCFG_PAGES; r[8] = (u8)mixer_current_page();
        r[9] = l & 0xFF; r[10] = l >> 8;
        r[11] = mcfg->mode;
        reply(seq, 0, r, 12);
        return 1;
    }
    case 0x02: {
        u32 off = n >= 3 ? d[0] | d[1] << 8 : 0, cnt = n >= 3 ? d[2] : 0;

        if (n < 3 || cnt > 128 || off + cnt > MCFG_MAX_BYTES) {
            reply(seq, 1, NULL, 0);
        } else {
            reply(seq, 0, active + off, cnt);
        }
        return 1;
    }
    case 0x03: {
        u32 off = n >= 2 ? d[0] | d[1] << 8 : 0;

        if (n < 2 || off + (n - 2) > MCFG_MAX_BYTES) {
            reply(seq, 1, NULL, 0);
        } else {
            memcpy(staging + off, d + 2, n - 2);
            reply(seq, 0, NULL, 0);
        }
        return 1;
    }
    case 0x04: {
        u32 l = n >= 2 ? d[0] | d[1] << 8 : 0;

        if (l < MCFG_HEADER || l > MCFG_MAX_BYTES || !blob_ok(staging, l)) {
            reply(seq, 2, NULL, 0);
            return 1;
        }
        memset(staging + l, 0, MCFG_MAX_BYTES - l);
        memcpy(active, staging, MCFG_MAX_BYTES);
        build_index();
        mixer_config_changed();
        reply(seq, store_active() ? 0 : 3, NULL, 0);
        return 1;
    }
    case 0x05:
        memcpy(staging, active, MCFG_MAX_BYTES);
        reply(seq, 0, NULL, 0);
        return 1;
    case 0x06:
        defaults(active);
        memcpy(staging, active, MCFG_MAX_BYTES);
        build_index();
        mixer_config_changed();
        reply(seq, store_active() ? 0 : 3, NULL, 0);
        return 1;
    case 0x10:
        mcfg_events_on = n >= 1 && d[0];
        reply(seq, 0, NULL, 0);
        return 1;
    }
    return 0;
}

#endif /* SMC_MIXER */
