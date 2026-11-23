/*
 * SMC-Mixer hardware service: the mixer's counterpart of smcpad_hw.c (built instead of it when
 * SMC_MIXER is set). Everything with timing runs in the background; docs/mixer-hardware.md has
 * the map, measured with fw/mixer_probe.
 *
 * TIMER3 at 4 kHz:
 *   - one 32-bit transfer into the four 74HC595 per tick (bit-banged: PB7 data, PB6 clock, PB4
 *     latch, /OE PB5 low): the high 16 bits select a key column (one-hot low), the low 16 are
 *     the LED segments of the current LED phase (active low). Each tick first reads the sense
 *     rows PB0..PB3 for the column latched by the previous tick (250 us of settling), then
 *     latches the next: a whole pass of the 16 x 4 matrix every 4 ms, after which encoders
 *     are decoded and buttons debounced;
 *   - the LEDs: 4 phases (commons PA2, PA5, PC1, PC3, active low), one per 4 ticks.
 * SAR ADC, interrupt-chained: the 8 faders (direct ADC pins), each conversion's interrupt
 *   stores it and starts the next; every 64 rounds the PMU (bandgap, VBAT) as on the pad.
 * The fader pins are never driven: they sit on the fader wipers.
 */
#include "smcpad_usb_cfg.h"
#if SMC_MIXER
#include "system/includes.h"
#include "asm/gpio.h"
#include "asm/hwi.h"
#include "asm/adc_api.h"
#include "asm/clock.h"
#include "smcpad_rt.h"
#include "smcmixer_hw.h"

#define REG32(a)            (*(volatile u32 *)(a))
#define PORT_OUT(b)         REG32((b) + 0x00)
#define PORT_IN(b)          REG32((b) + 0x04)
#define PORTA               0x1E5000u
#define PORTB               0x1E5040u
#define PORTC               0x1E5080u

/* ------------------------------------------------------------------ tables (docs/mixer-hardware.md) */

/* button (panel order) -> physical key code: driven column << 4 | sense row */
static const u8 button_code[SMCMIXER_NUM_BUTTONS] = {
    0x03, 0x13, 0x23, 0x33,  0xA3, 0x93, 0x83, 0x73,  0xB3, 0xC3, 0xD3, 0xE3,  0x02, 0x12, 0x22, 0x32,
    0xA2, 0x92, 0x82, 0x72,  0xB2, 0xC2, 0xD2, 0xE2,  0x51, 0x41, 0x31, 0x21,  0x81, 0x91, 0xA1, 0xB1,
    0x43, 0x53, 0x63, 0xF3, 0x42,               /* play, pause, record, rewind, ff */
    0x52, 0x62, 0xF2, 0x01, 0x11, 0xC1,         /* <<, >>, up, down, left, right */
    0x61, 0x71,                                 /* bluetooth, shift */
};
/* LED index -> segment << 4 | phase (docs/mixer-hardware.md, "LEDs") */
static const u8 led_pos[SMCMIXER_NUM_LEDS] = {
    0x80, 0x90, 0xA0, 0xB0, 0x40, 0x30, 0x20, 0x10, 0x50, 0x81, 0x91, 0xA1, 0x31, 0x21, 0x11,
    0x01, 0x41, 0x51, 0x82, 0x92, 0x22, 0x12, 0x02, 0xD2, 0x32, 0x42, 0x52, 0x83, 0x03, 0xD3,
    0xC3, 0xB3, 0xC0, 0xD0, 0x00, 0xB1, 0xC1, 0xD1, 0xA2, 0xB2, 0xC2, 0x93, 0xA3, 0x23, 0x13,
    0x53, 0x43, 0x63, 0x33, 0x70, 0x60, 0x62, 0x61,
};
/* fader 1..8 -> ADC channel */
static const u8 fader_ch[SMCMIXER_NUM_FADERS] = {
    AD_CH_PC5, AD_CH_PC4, AD_CH_PC2, AD_CH_PA6, AD_CH_PA4, AD_CH_PA3, AD_CH_PA1, AD_CH_PA0,
};

/* ------------------------------------------------------------------ events */

#define EVQ_LEN     200                 /* 8 faders swept at once: ~70 events / 10 ms */
static smcmixer_event_t evq[EVQ_LEN];
static volatile u8 evq_head, evq_tail;

static void ev_push(u8 type, u8 index, s16 value)
{
    u8 next = (evq_head + 1) % EVQ_LEN;

    if (next == evq_tail) {
        return;                             /* full: drop the newest */
    }
    evq[evq_head].type = type;
    evq[evq_head].index = index;
    evq[evq_head].value = value;
    evq_head = next;
}

int smcmixer_event_get(smcmixer_event_t *ev)
{
    if (evq_tail == evq_head) {
        return 0;
    }
    *ev = evq[evq_tail];
    evq_tail = (evq_tail + 1) % EVQ_LEN;
    return 1;
}

/* ------------------------------------------------------------------ key matrix + LEDs */

static volatile u64 matrix_now;             /* bit (col * 4 + row): closed */
static u64 matrix_acc;
static u8 scan_col;                         /* the column latched right now */
static u8 led_phase, led_ticks;
static volatile u16 led_seg[4];             /* per phase: segment bits lit */
static u8 led_state[SMCMIXER_NUM_LEDS];
static u64 buttons_state, buttons_candidate;
static u8 buttons_stable;
static volatile s32 encoder_count[SMCMIXER_NUM_ENCODERS];
static u8 encoder_ab[SMCMIXER_NUM_ENCODERS];
static s8 encoder_sub[SMCMIXER_NUM_ENCODERS];

static void sr_write32(u32 v)
{
    u32 out = PORT_OUT(PORTB) & ~((1u << 6) | (1u << 7));

    for (int byte = 0; byte < 4; byte++) {
        u32 b = (v >> (8 * byte)) & 0xFF;

        for (int bit = 7; bit >= 0; bit--) {
            u32 d = ((b >> bit) & 1) << 7;

            PORT_OUT(PORTB) = out | d;
            PORT_OUT(PORTB) = out | d | (1u << 6);
        }
    }
    PORT_OUT(PORTB) = out;
    PORT_OUT(PORTB) = out | (1u << 4);      /* latch */
    PORT_OUT(PORTB) = out;
}

static u32 column_bits(u32 c)
{
    return 1u << (c < 8 ? 24 + c : 16 + c - 8);
}

static void commons_off(void)
{
    PORT_OUT(PORTA) |= (1u << 2) | (1u << 5);
    PORT_OUT(PORTC) |= (1u << 1) | (1u << 3);
}

static void common_on(u32 phase)
{
    switch (phase) {
    case 0: PORT_OUT(PORTA) &= ~(1u << 2); break;
    case 1: PORT_OUT(PORTA) &= ~(1u << 5); break;
    case 2: PORT_OUT(PORTC) &= ~(1u << 1); break;
    default: PORT_OUT(PORTC) &= ~(1u << 3); break;
    }
}

/* Quadrature: (previous AB << 2 | new AB) -> step */
static const s8 quad_step[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };

static void matrix_pass_done(u64 m)
{
    u64 b = 0;

    matrix_now = m;
    for (int e = 0; e < SMCMIXER_NUM_ENCODERS; e++) {
        /* knob e: columns 2e (A) and 2e + 1 (B), row 0 */
        u8 ab = ((m >> (8 * e)) & 1) | (((m >> (8 * e + 4)) & 1) << 1);
        s8 step = quad_step[(encoder_ab[e] << 2) | ab];

        encoder_ab[e] = ab;
        if (step) {
            /* smooth knobs (no detents): every quadrature edge is a step, ~360 degrees for
             * 128 of them; the application adds acceleration */
            encoder_sub[e] += step;
            if (encoder_sub[e] >= 1 || encoder_sub[e] <= -1) {
                s8 d = encoder_sub[e] > 0 ? 1 : -1;

                encoder_sub[e] = 0;
                encoder_count[e] += d;
                ev_push(SMCMIXER_EV_ENCODER, (u8)e, d);
            }
        }
    }
    for (int i = 0; i < SMCMIXER_NUM_BUTTONS; i++) {
        u32 code = button_code[i];

        if (m & ((u64)1 << ((code >> 4) * 4 + (code & 0xF)))) {
            b |= (u64)1 << i;
        }
    }
    if (b != buttons_candidate) {           /* debounce: the same for two passes (8 ms) */
        buttons_candidate = b;
        buttons_stable = 0;
    } else if (!buttons_stable) {
        u64 changed = b ^ buttons_state;

        buttons_stable = 1;
        for (int i = 0; i < SMCMIXER_NUM_BUTTONS; i++) {
            if (changed & ((u64)1 << i)) {
                ev_push((b & ((u64)1 << i)) ? SMCMIXER_EV_BUTTON_DOWN : SMCMIXER_EV_BUTTON_UP, (u8)i, 0);
            }
        }
        buttons_state = b;
    }
}

static volatile u32 hw_ticks, hw_ticks_hi;  /* TIMER3 periods (250 us) since smcmixer_hw_begin() */
static u8 hw_running;

___interrupt
static void hw_tick_isr(void)
{
    u32 rows, seg, new_phase = 0;

    JL_TIMER3->CON |= BIT(14);              /* clear pending */
    if (++hw_ticks == 0) {
        hw_ticks_hi++;
    }

    rows = ~PORT_IN(PORTB) & 0xF;           /* the column latched by the previous tick */
    matrix_acc |= (u64)rows << (scan_col * 4);
    if (++scan_col == 16) {
        scan_col = 0;
        matrix_pass_done(matrix_acc);
        matrix_acc = 0;
    }
    if (++led_ticks == 4) {
        led_ticks = 0;
        led_phase = (led_phase + 1) & 3;
        new_phase = 1;
        commons_off();
    }
    seg = led_seg[led_phase];
    sr_write32((0xFFFF0000u & ~column_bits(scan_col)) | (~seg & 0xFFFFu));
    if (new_phase && seg) {
        common_on(led_phase);
    }
}

void smcmixer_led_raw(int pos, int on)
{
    if (pos < 0 || pos >= 64) {
        return;
    }
    if (on) {
        led_seg[pos >> 4] |= 1u << (pos & 15);
    } else {
        led_seg[pos >> 4] &= ~(1u << (pos & 15));
    }
}

void smcmixer_led_set(int led, int on)
{
    if (led < 0 || led >= SMCMIXER_NUM_LEDS) {
        return;
    }
    led_state[led] = on ? 1 : 0;
    smcmixer_led_raw((led_pos[led] & 0xF) * 16 + (led_pos[led] >> 4), on);
}

int smcmixer_led(int led)
{
    return led >= 0 && led < SMCMIXER_NUM_LEDS && led_state[led];
}

int smcmixer_button(int b)
{
    return b >= 0 && b < SMCMIXER_NUM_BUTTONS && (buttons_state & ((u64)1 << b));
}

int32_t smcmixer_encoder(int e)
{
    return e >= 0 && e < SMCMIXER_NUM_ENCODERS ? encoder_count[e] : 0;
}

uint64_t smcmixer_matrix_raw(void)
{
    return matrix_now;
}

/* ------------------------------------------------------------------ faders (ADC chain) */

/* A fader reports a move past a threshold that depends on what it is doing: a resting fader
 * wobbles (fader 2 by up to ~16 counts peak to peak, more when the case is knocked or another
 * fader moves), so starting to move, or turning back, takes FADER_START counts; a fader already
 * moving that way reports every FADER_STEP counts, about one 7-bit value. "Moving": it reported
 * within FADER_REST_TICKS. A wobble is a string of reversals, so it stays under FADER_START. */
#define FADER_STEP          8
#define FADER_START         24
#define FADER_END           10              /* within this of an end: snap to it */
#define FADER_REST_TICKS    1200            /* 300 ms of TIMER3 ticks */

static volatile u16 fader_avg[SMCMIXER_NUM_FADERS];     /* x 8 */
static u16 fader_sent[SMCMIXER_NUM_FADERS];
static u8 fader_init[SMCMIXER_NUM_FADERS];
static s8 fader_dir[SMCMIXER_NUM_FADERS];
static u32 fader_at[SMCMIXER_NUM_FADERS];
static u8 adc_pos;
static volatile u32 adc_rounds;

static void adc_start(u32 f)
{
    u32 con = 0b110;                        /* clock divider, as the SDK's adc_sample() */

    con |= 0x6u << 12;                      /* start delay (x 8 ADC clocks) */
    con |= (u32)fader_ch[f] << 8;
    con |= BIT(3) | BIT(5) | BIT(6);        /* as adc_sample(): ?, interrupt enable, clear */
    JL_ADC->CON = con;
    JL_ADC->CON |= BIT(4);                  /* enable */
    JL_ADC->CON |= BIT(6);                  /* start */
}

static void fader_sample(u32 f, u32 v)
{
    u32 avg, thr, r;
    int d;
    s8 dir;

    if (!fader_init[f]) {
        fader_init[f] = 1;
        fader_avg[f] = (u16)(v * 8);
        fader_sent[f] = (u16)v;
        fader_at[f] = hw_ticks - FADER_REST_TICKS;
        ev_push(SMCMIXER_EV_FADER, (u8)f, (s16)v);
        return;
    }
    avg = fader_avg[f] - fader_avg[f] / 8 + v;      /* IIR, 1/8 */
    fader_avg[f] = (u16)avg;
    avg /= 8;
    d = (int)avg - (int)fader_sent[f];
    dir = d > 0 ? 1 : -1;
    thr = (dir == fader_dir[f] && hw_ticks - fader_at[f] < FADER_REST_TICKS) ? FADER_STEP : FADER_START;
    if (avg <= FADER_END) {
        r = 0;                              /* the ends are reachable, and a fader there stays */
    } else if (avg >= 1023 - FADER_END) {
        r = 1023;
    } else if ((u32)(d < 0 ? -d : d) > thr) {
        r = avg;
    } else {
        return;
    }
    if (r == fader_sent[f]) {
        return;
    }
    fader_dir[f] = r > fader_sent[f] ? 1 : -1;
    fader_at[f] = hw_ticks;
    fader_sent[f] = (u16)r;
    ev_push(SMCMIXER_EV_FADER, (u8)f, (s16)r);
}

uint16_t smcmixer_fader(int f)
{
    return f >= 0 && f < SMCMIXER_NUM_FADERS ? fader_avg[f] / 8 : 0;
}

/* Power, as on the pad: every 64 rounds the bandgap reference and VBAT / 4. */
static const u32 pmu_ch[2] = { AD_CH_LDOREF, AD_CH_VBAT };
static volatile u16 pmu_raw[2];
static u8 pmu_pos;

static void adc_start_pmu(u32 ch)
{
    adc_pmu_detect_en(ch >> 20);
    JL_ADC->CON = 0b110 | (0xFu << 12) | (AD_CH_MUX << 8) | BIT(3) | BIT(5) | BIT(6);
    JL_ADC->CON |= BIT(4);
    JL_ADC->CON |= BIT(6);
}

___interrupt
static void hw_adc_isr(void)
{
    u32 v = JL_ADC->RES;
    u32 pos = adc_pos;

    JL_ADC->CON = BIT(6);                   /* clear pending, stop */
    JL_ADC->CON = 0;
    if (pmu_pos) {
        pmu_raw[pmu_pos - 1] = (u16)v;
        if (pmu_pos < ARRAY_SIZE(pmu_ch)) {
            adc_start_pmu(pmu_ch[pmu_pos++]);
            return;
        }
        pmu_pos = 0;
        adc_pmu_detect_en(AD_CH_WVDD >> 20);
        adc_start(0);
        return;
    }
    fader_sample(pos, v & 0x3FF);
    pos = (pos + 1) % SMCMIXER_NUM_FADERS;
    adc_pos = (u8)pos;
    if (pos == 0 && (++adc_rounds & 63) == 0) {
        pmu_pos = 1;
        adc_start_pmu(pmu_ch[0]);
        return;
    }
    adc_start(pos);
}

uint32_t smcpad_battery_mv(void)
{
    u32 ref = pmu_raw[0], val = pmu_raw[1];

    return ref ? adc_value_to_voltage(ref, val) * 4 : 0;
}

/* ------------------------------------------------------------------ time (as smcpad_hw.c) */
#define TICK_US     250u
#define TICK_CNT    (24000000u / 4000u)

uint32_t smcpad_millis(void)
{
    extern unsigned long jiffies_half_msec(void);
    u32 lo, hi;

    if (!hw_running) {
        return jiffies_half_msec() >> 1;
    }
    do {
        hi = hw_ticks_hi;
        lo = hw_ticks;
    } while (hi != hw_ticks_hi);
    return lo >> 2 | hi << 30;
}

uint32_t smcpad_micros(void)
{
    extern unsigned long jiffies_half_msec(void);
    u32 t, c, pend;

    if (!hw_running) {
        return jiffies_half_msec() * 500u;
    }
    do {
        t = hw_ticks;
        c = JL_TIMER3->CNT;
        pend = JL_TIMER3->CON & BIT(15);
    } while (t != hw_ticks);
    if (pend && c < TICK_CNT / 2) {
        t++;
    }
    return t * TICK_US + c / 24u;
}

void smcpad_delay_us(uint32_t us)
{
    uint32_t start;

    if (us <= 50 && hw_running) {
        u32 last, elapsed = 0, target = us * 24;

        local_irq_disable();
        last = JL_TIMER3->CNT;
        while (elapsed < target) {
            u32 c = JL_TIMER3->CNT;

            elapsed += c >= last ? c - last : c + TICK_CNT - last;
            last = c;
        }
        local_irq_enable();
        return;
    }
    start = smcpad_micros();
    while (smcpad_micros() - start < us) {
    }
}

/* ------------------------------------------------------------------ start */

void smcmixer_hw_begin(void)
{
    static u8 started;
    static const u32 sr_pins[] = { IO_PORTB_04, IO_PORTB_05, IO_PORTB_06, IO_PORTB_07 };
    static const u32 sense_pins[] = { IO_PORTB_00, IO_PORTB_01, IO_PORTB_02, IO_PORTB_03 };
    static const u32 com_pins[] = { IO_PORTA_02, IO_PORTA_05, IO_PORTC_01, IO_PORTC_03 };
    static const u32 fader_pins[] = { IO_PORTA_00, IO_PORTA_01, IO_PORTA_03, IO_PORTA_04,
                                      IO_PORTA_06, IO_PORTC_02, IO_PORTC_04, IO_PORTC_05 };

    if (started) {
        return;
    }
    started = 1;

    for (u32 i = 0; i < ARRAY_SIZE(fader_pins); i++) {      /* first: never driven */
        gpio_set_direction(fader_pins[i], 1);
        gpio_set_die(fader_pins[i], 0);
        gpio_set_pull_up(fader_pins[i], 0);
        gpio_set_pull_down(fader_pins[i], 0);
    }
    for (u32 i = 0; i < ARRAY_SIZE(com_pins); i++) {        /* LED commons off (high) */
        gpio_set_pull_up(com_pins[i], 0);
        gpio_set_pull_down(com_pins[i], 0);
        gpio_direction_output(com_pins[i], 1);
    }
    for (u32 i = 0; i < ARRAY_SIZE(sr_pins); i++) {         /* 595s: low, /OE on */
        gpio_set_pull_up(sr_pins[i], 0);
        gpio_set_pull_down(sr_pins[i], 0);
        gpio_direction_output(sr_pins[i], 0);
    }
    for (u32 i = 0; i < ARRAY_SIZE(sense_pins); i++) {      /* sense rows: pull-ups */
        gpio_set_direction(sense_pins[i], 1);
        gpio_set_die(sense_pins[i], 1);
        gpio_set_pull_down(sense_pins[i], 0);
        gpio_set_pull_up(sense_pins[i], 1);
    }
    sr_write32(0xFFFFFFFFu);

    /* the matrix / LED tick: TIMER3, 24 MHz oscillator, 250 us */
    scan_col = 0;
    sr_write32((0xFFFF0000u & ~column_bits(0)) | 0xFFFFu);
    request_irq(IRQ_TIME3_IDX, 2, hw_tick_isr, 0);
    JL_TIMER3->CON = BIT(14);
    JL_TIMER3->CNT = 0;
    JL_TIMER3->PRD = TICK_CNT;
    JL_TIMER3->CON = BIT(0) | BIT(3) | BIT(14);
    hw_running = 1;

    /* the faders: take the ADC over from the SDK's scan (occupy mode stops it), then chain */
    adc_enter_occupy_mode(AD_CH_PC5);
    for (u32 n = 0; (JL_ADC->CON & BIT(4)) && n < 100000; n++) {
    }
    request_irq(IRQ_SARADC_IDX, 2, hw_adc_isr, 0);
    adc_pos = 0;
    adc_start(0);
}

#endif /* SMC_MIXER */
