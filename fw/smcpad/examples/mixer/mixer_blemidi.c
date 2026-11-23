/*
 * SMC-Mixer BLE-MIDI (Apple / MMA "MIDI over Bluetooth LE" 1.0), peripheral: the central (an
 * OP-XY, a computer, a phone) scans for "SMC-Mixer" and connects, as with the stock firmware.
 * Based on the pad console's (examples/smcpad_test/smcpad_blemidi.c), plus batching: a message
 * goes out at once when the stack's queue is empty, otherwise it joins the next packet (each
 * with its own timestamp), which leaves as soon as the previous one is gone (mixer_blemidi_poll
 * from the main loop) -- the pad's BLEMIDI library does the same (docs/bluetooth.md).
 *
 *   service        03B80E5A-EDE8-4B33-A751-6CE34EC4C700
 *   MIDI I/O char  7772E5DB-3868-4112-A1A9-F2669D106BF3  read (empty) | write w/o response | notify
 */
#include "smcpad_usb_cfg.h"
#if SMC_MIXER
#include "system/includes.h"
#include "app_config.h"
#include "smcpad_rt.h"
#include "mixer_app.h"

#if TCFG_USER_BLE_ENABLE

#define BLEMIDI_NAME            "SMC-Mixer"

#define H_GAP_NAME_VALUE        0x0003
#define H_MIDI_VALUE            0x0006

static const uint8_t blemidi_profile[] = {
    /* 0x0001 PRIMARY_SERVICE 1800 (GAP) */
    0x0a, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x28, 0x00, 0x18,
    /* 0x0002 CHARACTERISTIC 2a00 (device name) READ | DYNAMIC -> 0x0003 */
    0x0d, 0x00, 0x02, 0x00, 0x02, 0x00, 0x03, 0x28, 0x02, 0x03, 0x00, 0x00, 0x2a,
    /* 0x0003 VALUE 2a00 READ | DYNAMIC */
    0x08, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00, 0x2a,
    /* 0x0004 PRIMARY_SERVICE 03B80E5A-EDE8-4B33-A751-6CE34EC4C700 (MIDI) */
    0x18, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x28,
    0x00, 0xc7, 0xc4, 0x4e, 0xe3, 0x6c, 0x51, 0xa7, 0x33, 0x4b, 0xe8, 0xed, 0x5a, 0x0e, 0xb8, 0x03,
    /* 0x0005 CHARACTERISTIC 7772E5DB-... READ | WRITE_WITHOUT_RESPONSE | NOTIFY -> 0x0006 */
    0x1b, 0x00, 0x02, 0x00, 0x05, 0x00, 0x03, 0x28, 0x16, 0x06, 0x00,
    0xf3, 0x6b, 0x10, 0x9d, 0x66, 0xf2, 0xa9, 0xa1, 0x12, 0x41, 0x68, 0x38, 0xdb, 0xe5, 0x72, 0x77,
    /* 0x0006 VALUE 7772E5DB-... READ | WRITE_WITHOUT_RESPONSE | NOTIFY | DYNAMIC | UUID128 */
    0x16, 0x00, 0x16, 0x03, 0x06, 0x00,
    0xf3, 0x6b, 0x10, 0x9d, 0x66, 0xf2, 0xa9, 0xa1, 0x12, 0x41, 0x68, 0x38, 0xdb, 0xe5, 0x72, 0x77,
    /* 0x0007 CLIENT_CHARACTERISTIC_CONFIGURATION READ | WRITE | DYNAMIC */
    0x0a, 0x00, 0x0a, 0x01, 0x07, 0x00, 0x02, 0x29, 0x00, 0x00,
    /* end */
    0x00, 0x00,
};

static volatile u16 con_handle;
static volatile u8 notify_on;

/* ------------------------------------------------------------------ incoming */

static u32 midi_len(u8 status)
{
    if (status < 0xF0) {
        return (status & 0xE0) == 0xC0 ? 2 : 3;
    }
    return status == 0xF2 ? 3 : (status == 0xF1 || status == 0xF3) ? 2 : 1;
}

/* Runs in the BT stack's task: complete messages go to a small queue the main loop empties. */
#define RXQ_LEN     32
static u8 rxq[RXQ_LEN][3];
static volatile u8 rxq_head, rxq_tail;

static void rx_push(const u8 *msg, u32 len)
{
    u8 next = (rxq_head + 1) % RXQ_LEN;

    if (next == rxq_tail) {
        return;
    }
    rxq[rxq_head][0] = msg[0];
    rxq[rxq_head][1] = len > 1 ? msg[1] : 0;
    rxq[rxq_head][2] = len > 2 ? msg[2] : 0;
    rxq_head = next;
}

int mixer_blemidi_read(u8 msg[3])
{
    if (rxq_tail == rxq_head) {
        return 0;
    }
    memcpy(msg, rxq[rxq_tail], 3);
    rxq_tail = (rxq_tail + 1) % RXQ_LEN;
    return 1;
}

static void blemidi_parse(const u8 *p, u32 n)
{
    u8 status = 0, msg[3];
    u32 i = 1, have = 0;

    if (n < 2 || (p[0] & 0xC0) != 0x80) {
        return;
    }
    while (i < n) {
        u8 b = p[i++];

        if (b & 0x80) {                             /* timestamp */
            if (i < n && (p[i] & 0x80)) {           /* ... then a status byte */
                u8 st = p[i++];

                if (st >= 0xF8) {
                    rx_push(&st, 1);
                    continue;
                }
                status = (st == 0xF0 || st == 0xF7) ? 0 : st;   /* SysEx: skipped */
                have = 0;
                if (status && midi_len(status) == 1) {
                    rx_push(&status, 1);
                    status = 0;
                }
            }
            continue;
        }
        if (!status) {
            continue;
        }
        if (!have) {
            msg[have++] = status;
        }
        msg[have++] = b;
        if (have == midi_len(status)) {
            rx_push(msg, have);
            have = 0;
        }
    }
}

/* ------------------------------------------------------------------ outgoing, batched */

#define PKT_MAX     64

static u8 pkt[PKT_MAX];
static u32 pkt_len;
static u32 pkt_t0;                          /* ms of the packet's first message */

static u32 pkt_limit(void)
{
    u32 mtu = smcpad_ble_mtu();
    u32 lim = mtu > 3 ? mtu - 3 : 20;

    return lim < PKT_MAX ? lim : PKT_MAX;
}

static int pkt_flush(void)
{
    if (!pkt_len) {
        return 1;
    }
    if (!smcpad_ble_notify(H_MIDI_VALUE, pkt, pkt_len)) {
        return 0;
    }
    pkt_len = 0;
    return 1;
}

void mixer_blemidi_send(const u8 *msg, u32 len)
{
    u32 ts = smcpad_millis() & 0x1FFF;

    if (!con_handle || !notify_on || len == 0 || len > 3) {
        return;
    }
    /* a packet spans < 128 ms of timestamps and fits the MTU */
    if (pkt_len && (pkt_len + 1 + len > pkt_limit() || smcpad_millis() - pkt_t0 > 100)) {
        if (!pkt_flush()) {
            pkt_len = 0;                    /* the stack refused: drop the old batch */
        }
    }
    if (!pkt_len) {
        pkt[0] = 0x80 | (ts >> 7);
        pkt_len = 1;
        pkt_t0 = smcpad_millis();
    }
    pkt[pkt_len++] = 0x80 | (ts & 0x7F);
    memcpy(pkt + pkt_len, msg, len);
    pkt_len += len;
    if (smcpad_ble_queued() == 0) {
        pkt_flush();
    }
}

void mixer_blemidi_poll(void)
{
    if (pkt_len && con_handle && notify_on && smcpad_ble_queued() == 0) {
        pkt_flush();
    }
}

int mixer_blemidi_connected(void)
{
    return con_handle && notify_on;
}

/* ------------------------------------------------------------------ GATT */

static uint16_t att_read(uint16_t handle, uint16_t offset, uint8_t *buf, uint16_t size)
{
    const char *name = BLEMIDI_NAME;
    u16 len = strlen(name);

    if (handle != H_GAP_NAME_VALUE || offset >= len) {
        return 0;
    }
    if (!buf) {
        return len;
    }
    if (size > len - offset) {
        size = len - offset;
    }
    memcpy(buf, name + offset, size);
    return size;
}

static void att_write(uint16_t handle, uint16_t offset, const uint8_t *data, uint16_t len)
{
    if (handle == H_MIDI_VALUE) {
        blemidi_parse(data, len);
    }
}

static void ble_event(int event, uint16_t handle, uint16_t value)
{
    switch (event) {
    case SMCPAD_BLE_CONNECTED:
        con_handle = handle;
        notify_on = 0;
        pkt_len = 0;
        break;
    case SMCPAD_BLE_DISCONNECTED:
        con_handle = 0;
        notify_on = 0;
        pkt_len = 0;
        break;
    case SMCPAD_BLE_SUBSCRIBE:
        notify_on = value & 1;
        if (notify_on) {
            smcpad_ble_set_conn_interval(6, 12);   /* 7.5-15 ms, as BLE-MIDI asks */
        }
        break;
    }
}

static const smcpad_ble_callbacks_t callbacks = {
    .read = att_read,
    .write = att_write,
    .event = ble_event,
};

void mixer_blemidi_begin(void)
{
    static const u8 midi_uuid_le[16] = {
        0x00, 0xc7, 0xc4, 0x4e, 0xe3, 0x6c, 0x51, 0xa7, 0x33, 0x4b, 0xe8, 0xed, 0x5a, 0x0e, 0xb8, 0x03,
    };
    u8 adv[3 + 18], rsp[2 + sizeof(BLEMIDI_NAME) - 1];

    adv[0] = 2;                                 /* flags: general discoverable, no BR/EDR */
    adv[1] = 0x01;
    adv[2] = 0x06;
    adv[3] = 17;                                /* complete list of 128-bit service UUIDs */
    adv[4] = 0x07;
    memcpy(adv + 5, midi_uuid_le, 16);
    rsp[0] = sizeof(rsp) - 1;                   /* complete local name */
    rsp[1] = 0x09;
    memcpy(rsp + 2, BLEMIDI_NAME, sizeof(rsp) - 2);
    smcpad_ble_set_name(BLEMIDI_NAME);
    smcpad_ble_set_profile(blemidi_profile, sizeof(blemidi_profile));
    smcpad_ble_set_adv(adv, sizeof(adv), rsp, sizeof(rsp), 48, 1);
    smcpad_ble_begin(&callbacks);
    smcpad_ble_advertise(1);
}

#else

void mixer_blemidi_begin(void) {}
void mixer_blemidi_send(const u8 *msg, u32 len) {}
void mixer_blemidi_poll(void) {}
int mixer_blemidi_read(u8 msg[3]) { return 0; }
int mixer_blemidi_connected(void) { return 0; }

#endif /* TCFG_USER_BLE_ENABLE */
#endif /* SMC_MIXER */
