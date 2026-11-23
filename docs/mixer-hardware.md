# SMC-Mixer hardware map

What the firmware needs to know about the board: which pin does what, how the keys, encoders,
LEDs and faders are wired. Confirmed on the device with the probe firmware (`fw/mixer_probe`,
2026-11-22). Driven by `fw/smcpad/arduino/smcmixer_hw.c`.

The design is a close relative of the SMC-PAD: a JieLi BR25 (AC636N) in QFN32, the same 16-column
74HC595 key scan fed by SPI DMA, but **four** sense rows, LED segments on the 595 chain, and **no**
analog mux (faders on direct ADC pins).

## Pin map

| Pin | Role |
|---|---|
| PB6, PB7 | 74HC595 chain clock / data (SPI, DMA of 4 bytes per scan step) |
| PB4 | 595 latch (RCLK) |
| PB5 | 595 /OE (driven low = enabled) |
| PB0..PB3 | key-matrix sense rows 0..3 (input, pull-up) |
| PA2, PA5, PC1, PC3 | LED **commons** (phases 0..3, active low) |
| PC5, PC4, PC2, PA6, PA4, PA3, PA1, PA0 | faders 1..8 (analog in; ADC channels `0xC, 0xB, 0xA, 4, 3, 2, 1, 0`). **Never drive these pins**: they sit on the fader wipers |
| USBDP/USBDM | USB |
| PD0..PD4 | internal SPI flash |

All 22 user GPIOs of a QFN32 BR25 are accounted for (PA0-6, PB0-7, PC1-5).

## 595 chain (32 bits per scan step)

Four cascaded 595s. Each of the 16 steps shifts out 4 bytes: the low 16 bits are the **LED
segment** lines (active low) for the current LED phase, the high 16 bits the **column select**,
one-hot-low. The key scan and the LEDs share the chain: the segment word rides in the same
transfer as the column select.

## Keys

16 columns x 4 rows = 64 positions; key code = `column << 4 | row`, where column is the 595 column
driven when the key reads pressed (a pressed key reads **0**).

| control | key codes |
|---|---|
| strips 1..8, top to bottom (Mute, Solo, Rec, Track Stop) | 1: `03 13 23 33`, 2: `A3 93 83 73`, 3: `B3 C3 D3 E3`, 4: `02 12 22 32`, 5: `A2 92 82 72`, 6: `B2 C2 D2 E2`, 7: `51 41 31 21`, 8: `81 91 A1 B1` |
| Play, Pause, Record, Rewind, Fast-forward | `43 53 63 F3 42` |
| `<<`, `>>`, Up, Down, Left, Right | `52 62 F2 01 11 C1` |
| **Shift** (lower oval), Bluetooth (upper oval) | `71`, `61` |

Measured: strip 1 and 8 buttons, all bottom-row buttons, both ovals; the rest follow the same
pattern.

## Encoders

8 quadrature pairs, all on **row 0**: encoder *n* = columns `2n-2` / `2n-1` (encoder 1 = columns
0/1 ... encoder 8 = 14/15). No push switches. Measured: encoders 1 and 8.

## LEDs

4 phases (commons PA2, PA5, PC1, PC3) x 16 segments (595 outputs) = 64 positions, **53 LEDs**.
By index: 0..31 strip buttons (strip 1 top to bottom, then strip 2, ...), 32..36 Play, Pause,
Record, Rewind, Fast-forward, 37..42 `<<`, `>>`, Up, Down, Left, Right, 43..44 the two ovals,
**45..52 the fader LEDs from right to left** (45 = above fader 8, 52 = above fader 1). All 53
checked one by one.

Where each LED sits, `segment << 4 | phase`, by index:

```
80 90 A0 B0 40 30 20 10 50 81 91 A1 31 21 11 01 41 51 82 92 22 12 02 D2 32 42 52
83 03 D3 C3 B3 C0 D0 00 B1 C1 D1 A2 B2 C2 93 A3 23 13 53 43 63 33 70 60 62 61
```

Multiplexing: one phase at a time; at each phase change all commons go high (off) and the
segments to `0xFFFF`, then the phase's common goes low with its segment word.

## Faders

8 direct ADC inputs (pins above), 10-bit, full travel 0..1023 (0..127 after scaling). Measured:
faders 1 and 8. A resting fader wobbles by about 16 raw counts: `smcmixer_hw.c` filters it (24
raw counts to start moving or turn back, then every 8).
