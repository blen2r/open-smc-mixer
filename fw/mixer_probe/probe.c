/* SMC-Mixer probe: a bare-metal application (behind fw/boot, at flash 0x9000) to map the
 * mixer's hardware from the outside -- the bring-up console's job on the pad, over USB MIDI so
 * that no driver or terminal is needed (host/mixer_probe.py).
 *
 * It reports, as MIDI to the host:
 *   - every change of the key matrix, raw: Note On (pressed) / Note Off (released), channel
 *     row + 1 (sense rows PB0..PB3 = channels 1..4), note = column 0..15. Knobs are quadrature
 *     pairs on row 0, so turning one shows two columns of channel 1 changing;
 *   - the faders, raw: CC on channel 6, controller = ADC channel (0..12, the SDK's AD_CH_*),
 *     value = 10-bit result >> 3, sent when it moves by more than a few counts;
 * and takes from the host:
 *   - Note On/Off channel 16, note 0..63: LED position phase * 16 + segment on / off;
 *   - Note On/Off channel 15, note 0..52: LED by the stock firmware's index (its table below);
 *   - CC 126 on channel 16: value 127 all 64 positions on, 0 all off;
 *   - CC 127 on channel 16, value 127: reboot into the bootloader's DFU mode.
 *
 * Hardware (docs/mixer-hardware.md): four cascaded 74HC595, 32 bits per transfer shifted a
 * byte at a time MSB first on PB7 (data) / PB6 (clock), latched by PB4, /OE = PB5 low. Bits
 * 16..31 select the key column (one-hot low), bits 0..15 are the LED segments (active low).
 * Sense rows PB0..PB3 (pull-ups, a pressed key reads 0). LED commons PA2, PA5, PC1, PC3
 * (phases 0..3, active low). Faders on PA0, PA1, PA3, PA4, PA6, PC2, PC4, PC5 (ADC).
 *
 * Runs polled with interrupts off, from flash, like the bootloader's DFU mode runs from RAM.
 */
#include <string.h>
#include "tusb.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define REG32(a)            (*(volatile u32 *)(a))
#define PORTA(o)            REG32(0x1E5000u + (o))
#define PORTB(o)            REG32(0x1E5040u + (o))
#define PORTC(o)            REG32(0x1E5080u + (o))
#define P_OUT               0x00
#define P_IN                0x04
#define P_DIR               0x08                /* 1 = input */
#define P_DIE               0x0C
#define P_PU                0x10
#define P_PD                0x14

#define ADC_CON             REG32(0x1E3100u)
#define ADC_RES             REG32(0x1E3104u)

#define T0_CON              REG32(0x1E0400u)
#define T0_CNT              REG32(0x1E0404u)
#define T0_PRD              REG32(0x1E0408u)
#define T_EN                (1u << 0)
#define T_SRC_OSC           (1u << 3)
#define T_PCLR              (1u << 14)
#define T_PND               (1u << 15)

#define USB_CON0            REG32(0x1E1800u)
#define USB_IO_CON0         REG32(0x1E5100u)

/* fw/boot/boot.h: the contract with the bootloader */
#define BOOT_MAGIC_ADDR     0x31AE0u
#define BOOT_MAGIC_DFU      0xB007DF00u
#define BOOT_COUNT          ((volatile u32 *)0x31AD8u)

#define ROM_CHIP_RESET      ((void (*)(u32, u32))0x117fccu)
#define ROM_IRQ_DISABLE     ((void (*)(void))0x117d44u)
#define ROM_P33_OR_1BYTE    ((void (*)(u32, u32))0x117d54u)

#define SR_PINS             ((1u << 4) | (1u << 5) | (1u << 6) | (1u << 7))
#define SENSE_PINS          0xFu
#define COM_A_PINS          ((1u << 2) | (1u << 5))     /* phases 0, 1 */
#define COM_C_PINS          ((1u << 1) | (1u << 3))     /* phases 2, 3 */
#define FADER_A_PINS        ((1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 6))
#define FADER_C_PINS        ((1u << 2) | (1u << 4) | (1u << 5))

/* ADC channels of the fader pins (SDK adc_api.h AD_CH_*): PA0 0, PA1 1, PA3 2, PA4 3, PA6 4,
 * PC2 10, PC4 11, PC5 12 -- the stock firmware's slots 0..7 are 12, 11, 10, 4, 3, 2, 1, 0. */
static const u8 adc_channels[8] = { 0, 1, 2, 3, 4, 10, 11, 12 };

/* LED index -> segment << 4 | phase (docs/mixer-hardware.md, "LEDs"). */
static const u8 stock_led[53] = {
    0x80, 0x90, 0xA0, 0xB0, 0x40, 0x30, 0x20, 0x10, 0x50, 0x81, 0x91, 0xA1, 0x31, 0x21, 0x11,
    0x01, 0x41, 0x51, 0x82, 0x92, 0x22, 0x12, 0x02, 0xD2, 0x32, 0x42, 0x52, 0x83, 0x03, 0xD3,
    0xC3, 0xB3, 0xC0, 0xD0, 0x00, 0xB1, 0xC1, 0xD1, 0xA2, 0xB2, 0xC2, 0x93, 0xA3, 0x23, 0x13,
    0x53, 0x43, 0x63, 0x33, 0x70, 0x60, 0x62, 0x61,
};

static u32 now_ms;
static u16 led_on[4];                   /* per phase, bit = segment, 1 = lit */
static u8 keys_now[16];                 /* debounced, bit r = row r pressed */
static u8 keys_seen[16];                /* last raw sweep */
static u8 keys_count[16][4];
static u16 adc_sent[8];

static void wdt_kick(void)
{
    ROM_P33_OR_1BYTE(0x80, 0x40);       /* the SDK's wdt_clear() */
}

static u32 millis(void)
{
    if (T0_CON & T_PND) {
        T0_CON |= T_PCLR;
        now_ms++;
    }
    return now_ms;
}

uint32_t tusb_time_millis_api(void)
{
    return millis();
}

static void spin(u32 n)
{
    for (volatile u32 i = 0; i < n; i++) {
    }
}

/* ------------------------------------------------------------------------ 595 chain */

static void sr_write32(u32 v)
{
    u32 out = PORTB(P_OUT) & ~((1u << 6) | (1u << 7));

    for (int byte = 0; byte < 4; byte++) {
        u32 b = (v >> (8 * byte)) & 0xFF;

        for (int bit = 7; bit >= 0; bit--) {
            u32 d = ((b >> bit) & 1) << 7;

            PORTB(P_OUT) = out | d;                 /* clock low, data */
            PORTB(P_OUT) = out | d | (1u << 6);     /* clock high: shift */
        }
    }
    PORTB(P_OUT) = out;
    PORTB(P_OUT) = out | (1u << 4);                 /* latch */
    PORTB(P_OUT) = out;
}

static u32 column_bits(int c)
{
    return 1u << (c < 8 ? 24 + c : 16 + c - 8);
}

static void commons_off(void)
{
    PORTA(P_OUT) |= COM_A_PINS;
    PORTC(P_OUT) |= COM_C_PINS;
}

static void common_on(int phase)
{
    switch (phase) {
    case 0: PORTA(P_OUT) &= ~(1u << 2); break;
    case 1: PORTA(P_OUT) &= ~(1u << 5); break;
    case 2: PORTC(P_OUT) &= ~(1u << 1); break;
    default: PORTC(P_OUT) &= ~(1u << 3); break;
    }
}

static void hw_init(void)
{
    /* 595 chain outputs, /OE low */
    PORTB(P_PU) &= ~SR_PINS;
    PORTB(P_PD) &= ~SR_PINS;
    PORTB(P_OUT) &= ~SR_PINS;
    PORTB(P_DIR) &= ~SR_PINS;
    /* sense rows: inputs with pull-ups */
    PORTB(P_PD) &= ~SENSE_PINS;
    PORTB(P_PU) |= SENSE_PINS;
    PORTB(P_DIE) |= SENSE_PINS;
    PORTB(P_DIR) |= SENSE_PINS;
    /* LED commons: outputs, all off (high) */
    PORTA(P_PU) &= ~COM_A_PINS;
    PORTA(P_PD) &= ~COM_A_PINS;
    PORTC(P_PU) &= ~COM_C_PINS;
    PORTC(P_PD) &= ~COM_C_PINS;
    commons_off();
    PORTA(P_DIR) &= ~COM_A_PINS;
    PORTC(P_DIR) &= ~COM_C_PINS;
    /* faders: analog inputs, never driven */
    PORTA(P_DIR) |= FADER_A_PINS;
    PORTC(P_DIR) |= FADER_C_PINS;
    PORTA(P_PU) &= ~FADER_A_PINS;
    PORTA(P_PD) &= ~FADER_A_PINS;
    PORTC(P_PU) &= ~FADER_C_PINS;
    PORTC(P_PD) &= ~FADER_C_PINS;
    PORTA(P_DIE) &= ~FADER_A_PINS;
    PORTC(P_DIE) &= ~FADER_C_PINS;
    sr_write32(0xFFFFFFFFu);
}

/* ------------------------------------------------------------------------ MIDI */

static void midi3(u8 status, u8 d1, u8 d2)
{
    u8 pkt[4] = { (u8)(status >> 4), status, d1, d2 };     /* cable 0, CIN = status high nibble */

    if (tud_midi_mounted()) {
        tud_midi_packet_write(pkt);
    }
}

static void led_set(int phase, int segment, int on)
{
    if (on) {
        led_on[phase & 3] |= 1u << (segment & 15);
    } else {
        led_on[phase & 3] &= ~(1u << (segment & 15));
    }
}

static __attribute__((noreturn)) void reboot_to_dfu(void)
{
    REG32(BOOT_MAGIC_ADDR) = BOOT_MAGIC_DFU;
    commons_off();
    USB_CON0 = 0;                                   /* off the bus, the host sees a detach */
    USB_IO_CON0 &= ~((1u << 4) | (1u << 5) | (1u << 6) | (1u << 7));
    for (u32 t = millis(); millis() - t < 200; ) {
        wdt_kick();
    }
    ROM_CHIP_RESET(0, 0);
    for (;;) {
    }
}

static void midi_in(void)
{
    u8 pkt[4];

    while (tud_midi_available() && tud_midi_packet_read(pkt)) {
        u8 st = pkt[1] & 0xF0, ch = pkt[1] & 0x0F, d1 = pkt[2], d2 = pkt[3];
        int on = st == 0x90 && d2 > 0;

        if ((st == 0x90 || st == 0x80) && ch == 15 && d1 < 64) {
            led_set(d1 >> 4, d1 & 15, on);
        } else if ((st == 0x90 || st == 0x80) && ch == 14 && d1 < sizeof(stock_led)) {
            led_set(stock_led[d1] & 0xF, stock_led[d1] >> 4, on);
        } else if (st == 0xB0 && ch == 15 && d1 == 126) {
            for (int p = 0; p < 4; p++) {
                led_on[p] = d2 ? 0xFFFF : 0;
            }
        } else if (st == 0xB0 && ch == 15 && d1 == 127 && d2 == 127) {
            reboot_to_dfu();
        }
    }
}

/* ------------------------------------------------------------------------ scanning */

/* One sweep of the 16 columns with the LEDs of `phase` lit. */
static void sweep(int phase)
{
    u32 seg = (~(u32)led_on[phase]) & 0xFFFF;

    commons_off();
    sr_write32(0xFFFF0000u | seg);                  /* new segments, no column */
    common_on(phase);
    for (int c = 0; c < 16; c++) {
        sr_write32((0xFFFF0000u & ~column_bits(c)) | seg);
        spin(40);                                   /* the pull-ups win through the diodes */
        keys_seen[c] = ~PORTB(P_IN) & SENSE_PINS;
    }
    sr_write32(0xFFFF0000u | seg);
}

/* A key changes state after 3 sweeps that agree. */
static void keys_update(void)
{
    for (int c = 0; c < 16; c++) {
        for (int r = 0; r < 4; r++) {
            int raw = (keys_seen[c] >> r) & 1, cur = (keys_now[c] >> r) & 1;

            if (raw == cur) {
                keys_count[c][r] = 0;
            } else if (++keys_count[c][r] >= 3) {
                keys_count[c][r] = 0;
                keys_now[c] ^= 1u << r;
                midi3(raw ? 0x90 | r : 0x80 | r, (u8)c, raw ? 127 : 0);
            }
        }
    }
}

static u32 adc_read(u32 ch)
{
    ADC_CON = 0;
    ADC_CON = 0b110 | (1u << 12) | (ch & 0xF) << 8 | (1u << 3) | (1u << 6);   /* SDK adc_sample() */
    ADC_CON |= 1u << 4;                             /* enable */
    ADC_CON |= 1u << 6;                             /* start */
    for (u32 i = 0; i < 20000; i++) {
        if (ADC_CON & (1u << 7)) {                  /* pending: done */
            u32 v = ADC_RES & 0x3FF;

            ADC_CON = 0;
            return v;
        }
    }
    ADC_CON = 0;
    return 0xFFFF;                                  /* no answer */
}

static void faders_update(int slot)
{
    u32 v = adc_read(adc_channels[slot]);

    if (v == 0xFFFF) {
        return;
    }
    if (adc_sent[slot] == 0xFFFF || (v > adc_sent[slot] ? v - adc_sent[slot] : adc_sent[slot] - v) > 6) {
        adc_sent[slot] = (u16)v;
        midi3(0xB5, adc_channels[slot], (u8)(v >> 3));
    }
}

/* ------------------------------------------------------------------------ main */

void probe_main(void)
{
    tusb_rhport_init_t init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    int phase = 0, slot = 0, confirmed = 0;

    ROM_IRQ_DISABLE();
    wdt_kick();
    T0_CON = T_PCLR;
    T0_CNT = 0;
    T0_PRD = 24000;
    T0_CON = T_EN | T_SRC_OSC | T_PCLR;
    for (int i = 0; i < 8; i++) {
        adc_sent[i] = 0xFFFF;
    }
    hw_init();
    tusb_init(0, &init);

    for (;;) {
        wdt_kick();
        dcd_int_handler(0);
        tud_task();
        midi_in();

        sweep(phase);
        phase = (phase + 1) & 3;
        if (phase == 0) {
            keys_update();
            faders_update(slot);
            slot = (slot + 1) & 7;
        }
        if (!confirmed && millis() > 3000) {        /* running: no crash loop (fw/boot) */
            BOOT_COUNT[0] = 0;
            BOOT_COUNT[1] = 0;
            confirmed = 1;
        }
    }
}

/* ------------------------------------------------------------------------ entry */

extern u32 data_load[], data_begin[], data_end[], bss_begin[], bss_end[], stack_top[];

__attribute__((naked, noreturn, noinline))
static void switch_stack(u32 sp_top)
{
    __asm__ volatile("sp = r0\n\t"
                     "ssp = r0\n\t"
                     "goto probe_start");
}

__attribute__((used, noreturn, noinline))
void probe_start(void)
{
    volatile u32 *src = data_load, *dst = data_begin;

    while (dst < data_end) {
        *dst++ = *src++;
    }
    for (dst = bss_begin; dst < bss_end; ) {
        *dst++ = 0;
    }
    probe_main();
    for (;;) {
    }
}

/* The bootloader jumps to the first byte of app.bin (probe.ld puts .entry there). */
__attribute__((section(".entry"), used, noreturn))
void probe_entry(void)
{
    switch_stack((u32)stack_top);
}
