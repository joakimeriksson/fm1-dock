/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * SMK-37 Pro port: Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* The SMK-37's panel for fm1-dock (BOARD_SMK37, in place of fm1_input.h and fm1_adc.h): the reference port's
 * scan (genkigenki's fm1-to-smk37, SLOOP's hal/smk_input.h), shown to the host as an FM-1 with extras.
 *
 *   as the FM-1   fm1_in.notes: the middle 27 keys (F3..G5); fm1_in.buttons / fm1_input_edges: the FM-1's
 *                 button ids 0..13 by meaning (PATCH = HOME, PARA = EDIT, CHORD = ENV, NOTE REPEAT = LFO,
 *                 SCALE = SEL, GLOBE = GLO, BT = SAVE, the others by name), then the SMK's own 14..17 (KNOB BANK,
 *                 FADER BANK, <, >); fm1_enc_take: the FM-1's encoder ids (K2 SELECT, K3 ALGORITHM, K4 PRESETS,
 *                 K5..K8 KNOB1..4); fm1_adc_read(FM1_ADC_MASTER): K1 as the MASTER pot; fm1_led_key: the
 *                 buttons' LEDs by the FM-1's ids
 *   the extras    smk_key_vel(k): a middle key's velocity (two contacts, the unit's factory curves);
 *                 smk_ext_take(): the outer keys (C3..E3, G#5..C6) and the 16 pads, with their velocity, in order;
 *                 fm1_in.fader[4] (0..127), fm1_in.wheel[2] (raw), the main loop's to read
 *
 * Hardware (fm1-to-smk37 docs/01; stock v016):
 *  - 4 x 74HC595 (32 bits) shifted by SPI2 group C (PA3 CLK, PA4 DO; IOMAP_CON1 bit 17), latched by PA1, PA0 low
 *    (/OE). Word bits 31..16: column select, active-low one-hot (col 0..7 = bits 31..24, col 8..15 = bits 23..16);
 *    bits 15..0: the button LEDs (active low) and the analog mux address (bits 3..5).
 *  - 8 return lines with pull-ups, low = closed: PA2, PA5, PA6, PA7, PA9, PH6, PH8, PH9 = rows 0..7. Rows 0..4: the
 *    keybed (2 contacts a key), rows 5..6: 18 buttons, row 7: 8 encoders.
 *  - The button LEDs: 19 positions in two groups, commons on the second USB PHY's pads as GPIO (SFR 0x16A0C bits
 *    17 / 18, low = group on).
 *  - SARADC: ch1 = mux A (PA8, pads 0..7), ch8 = mux B (PC9, pads 8..15), ch2 = mux C (PA10: faders, wheels, pedal,
 *    battery); the 3 muxes share the address from the 595 word.
 * Timing: fm1_input_tick() runs from the 10 kHz TIMER5 ISR. Each tick reads the rows of the column latched one tick
 * ago, latches the word shifted during the last tick, starts the next SPI2 DMA (4 bytes, ~3 us) and one ADC
 * conversion (read at the next tick). A frame = 16 ticks = 1.6 ms. Analog: 8 mux addresses x 3 channels = 24 ticks.
 * Key velocity: the stock two-contact algorithm (T = 10 + the time between contact 1 and contact 2 in 0.1 ms) on the
 * 5-point curves of the unit's calibration (flash 0xC1000, read only; defaults {5,640,800,920,980}). */
#pragma once
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_gpio.h"
#include "fm1_cc.h"

#ifndef FM1_INPUT_IDLE
#define FM1_INPUT_IDLE() ((void)0)
#endif

#define SMK_SPI2_CON (*(volatile uint32_t *)0x11E00u)
#define SMK_SPI2_BAUD (*(volatile uint32_t *)0x11E04u)
#define SMK_SPI2_ADR (*(volatile uint32_t *)0x11E0Cu)
#define SMK_SPI2_CNT (*(volatile uint32_t *)0x11E10u)
#define SMK_IOMAP_CON1 (*(volatile uint32_t *)0x51020u)
#define SMK_USBIO_CON (*(volatile uint32_t *)0x16A0Cu)   /* second USB PHY pads as GPIO (LED commons) */
#define SMK_ADC_CON (*(volatile uint32_t *)0x13100u)
#define SMK_ADC_RES (*(volatile uint32_t *)0x13104u)
#define SMK_WLA_CON0 (*(volatile uint32_t *)0x11900u)

#define SMK_NCOL 16u
#define SMK_NBTN 18u
#define SMK_NKEY 37u              /* the keybed, C3..C6 */
#define SMK_NENC 8u
#define SMK_NPAD 16u
#define SMK_NLED 19u
#define SMK_KEY_F3 5u             /* keybed key 5 = F3: keys 5..31 are the FM-1's 27 note keys */
#define FM1_NENC 7u               /* the FM-1's encoders: SELECT, ALGORITHM, KNOB1..4, PRESETS */
#define FM1_NBTN 14u
#define SMK_DEB_RELEASE 4u        /* frames open in a row: a button release (~6 ms) */
#define SMK_DEB_PRESS 2u          /* frames closed in a row: a button press (~3 ms) */
#define SMK_KEY_REL 3u            /* frames with contact 1 open: a key release (~5 ms) */
#define SMK_ENC_DIV 4             /* decoder counts a step: a detent is a whole quadrature cycle */
enum { FM1_BTN_OCT_DOWN = 0, FM1_BTN_OCT_UP = 1 };
enum { SMK_BTN_KNOB_BANK = 14, SMK_BTN_FADER_BANK = 15, SMK_BTN_LEFT = 16, SMK_BTN_RIGHT = 17 };   /* fm1_in.buttons bits */

/* ---------------------------------------------------------------- tables --- */
/* 595 word per column: active-low one-hot select (stock table) */
#define SMK_COLW(c) (~((c) < 8u ? 0x01000000u << (c) : 0x00010000u << ((c) - 8u)))
/* keybed key k: (first contact, second contact) matrix codes, col << 4 | row (stock table) */
static const uint8_t SMK_KEYC[SMK_NKEY][2] = {
    {0x10, 0x52}, {0x20, 0x72}, {0x30, 0x92}, {0x40, 0xb2}, {0x50, 0xd2}, {0x60, 0xf2}, {0x70, 0xe2},
    {0x80, 0xc2}, {0x90, 0xa2}, {0xa0, 0x82}, {0xc0, 0x62}, {0xd0, 0x42}, {0xe0, 0x22}, {0xf0, 0x12},
    {0xb0, 0x32}, {0x11, 0x33}, {0x21, 0x43}, {0x31, 0x53}, {0x41, 0x63}, {0x51, 0x73}, {0x61, 0x83},
    {0x71, 0x93}, {0x81, 0xa3}, {0x91, 0xb3}, {0xa1, 0xc3}, {0xb1, 0xd3}, {0xc1, 0xe3}, {0xd1, 0x23},
    {0xe1, 0x13}, {0xf1, 0x14}, {0x64, 0x24}, {0x74, 0x34}, {0x84, 0x44}, {0x94, 0x54}, {0xa4, 0xd4},
    {0xb4, 0xf4}, {0xc4, 0xe4},
};
/* the SMK's buttons in the stock order (printed names, confirmed on a unit): 0 KNOB BANK, 1 FADER BANK, 2 <, 3 >,
 * 4 OCTAVE-, 5 OCTAVE+, 6 ARP, 7 NOTE REPEAT, 8 SCALE, 9 CHORD, 10 GLOBE, 11 BT, 12 PATCH, 13 PARA, 14 FX, 15 SEQ,
 * 16 SEQ PLAY, 17 SEQ REC: their matrix codes, and the FM-1 id each one is (the SMK LED i belongs to button i) */
static const uint8_t SMK_BTNC[SMK_NBTN] = {0x26, 0x76, 0x96, 0x56, 0x66, 0x36, 0x75, 0x65, 0x25,
                                            0x45, 0x15, 0x86, 0x95, 0x55, 0x85, 0x35, 0x16, 0x46};
static const uint8_t SMK_BTN_FM1[SMK_NBTN] = {14, 15, 16, 17, 0, 1, 10, 5, 3, 4, 7, 9, 8, 6, 2, 11, 12, 13};
/* encoder K1..K8: A and B matrix codes (stock table), and the FM-1 encoder each one is (K1: the MASTER pot) */
static const uint8_t SMK_ENCC[SMK_NENC][2] = {{0x97, 0xa7}, {0x37, 0x47}, {0x17, 0x27}, {0xb7, 0xc7},
                                               {0x57, 0x67}, {0x77, 0x87}, {0xd7, 0xe7}, {0xf7, 0x07}};
#define SMK_ENC_MASTER 0xFFu
static const uint8_t SMK_ENC_FM1[SMK_NENC] = {SMK_ENC_MASTER, 0, 1, 6, 2, 3, 4, 5};
/* LED i (= button i): group << 4 | bit in the 595 low half (stock table). SEQ PLAY's LED is two-coloured: 16 its
 * green, 18 its red (unused); SEQ REC's red is 0x1b (confirmed on a unit) */
static const uint8_t SMK_LEDC[SMK_NLED] = {0x0c, 0x12, 0x00, 0x10, 0x0d, 0x1d, 0x02, 0x01, 0x09, 0x08,
                                           0x0b, 0x0a, 0x11, 0x19, 0x18, 0x1a, 0x1c, 0x1b, 0x1e};
/* analog: mux address -> pad on mux A (ch1); mux B (ch8) = the same + 8 (stock table) */
static const uint8_t SMK_PADMUX[8] = {1, 2, 3, 0, 4, 7, 5, 6};
/* mux C (ch2): address 0..3 -> fader 1,2,3,0; 4, 5 wheels; 6 pedal; 7 battery */
static const uint8_t SMK_FADMUX[4] = {1, 2, 3, 0};
enum { SMK_ADC_PADA = 1, SMK_ADC_PADB = 8, SMK_ADC_MUXC = 2 };

#define SMK_TRIM_FLASH 0xC1000u   /* USRTRIM: the factory calibration record ("trimdat"), read only */
#define SMK_TRIM_SIZE 0x22Cu
/* 5-point velocity curves (+0x08 16 pads x 5 u16, +0xA8 37 keys x 5 u16) */
static uint16_t smk_trim_pad[SMK_NPAD][5], smk_trim_key[SMK_NKEY][5];

/* ------------------------------------------------------------------ state --- */
static volatile struct {
    uint32_t notes;              /* bit n = note key n (0 = F3 .. 26 = G5): keybed keys 5..31 */
    uint32_t buttons;            /* debounced, by FM-1 id (0..13) and the SMK's 14..17 */
    uint32_t pressed, released;  /* button edges since the last fm1_input_edges() */
    uint32_t notes_pressed;      /* note-key press edges since the last fm1_input_note_edges() */
    uint8_t key_vel[SMK_NKEY];   /* MIDI velocity 1..127 of each key's last press */
    uint8_t raw[SMK_NCOL];       /* last frame, rows packed, 1 = closed */
    uint8_t cnt[SMK_NBTN];       /* button debounce */
    uint8_t ks_stab[SMK_NKEY], ks_rel[SMK_NKEY];
    uint32_t ks_t1[SMK_NKEY];    /* fm1_ticks() when contact 1 closed */
    uint8_t enc_last[SMK_NENC], enc_hist[SMK_NENC];
    int16_t enc_cnt[SMK_NENC];   /* transitions, + = B leads */
    int16_t enc_steps[SMK_NENC]; /* by SMK encoder (K1..K8) */
    uint16_t pad_base[SMK_NPAD], pad_peak[SMK_NPAD];
    uint8_t pad_st[SMK_NPAD];    /* 0 idle, 1..SMK_PAD_PEAKN collecting the peak, 0x80 on */
    uint8_t pad_low[SMK_NPAD];
    uint8_t pad_press[SMK_NPAD]; /* pressure 0..127 while on */
    uint16_t fad_acc[4];         /* faders: 0..1023 x 16, IIR */
    uint16_t fad_hist[4][2];     /* the two readings before: a median of 3 drops a lone spike */
    uint8_t fader[4];            /* 0..127, faders 1..4 left to right */
    uint16_t wheel[2], pedal, batt;   /* raw 0..1023: pitch, mod (mux C addresses 4, 5), pedal, battery */
    uint32_t adc_ok, adc_miss;   /* conversions read / not finished within a tick */
    uint32_t frames;
} fm1_in;

/* the extras, in order: SMK_EXT_KEY + keybed key (an outer key) or SMK_EXT_PAD + pad (0..7 the top row, 8..15 the
 * bottom row, left to right) << 8 | velocity (0: released) */
#define SMK_EXT_KEY 0x00u
#define SMK_EXT_PAD 0x40u
#define SMK_EXTQ 32u
static volatile uint16_t smk_extq[SMK_EXTQ];
static volatile uint8_t smk_extq_w, smk_extq_r;

static uint8_t fm1_led[SMK_NLED];     /* 1 = lit (LED i = SMK button i) */
static volatile uint32_t smk_ledw[2]; /* per LED group: 0xFFFF0000 | low half (LED lines active low) */
static volatile uint32_t smk_muxw = 0xFFFFFFC7u;  /* AND mask over the word: the mux address (bits 3..5) */
static uint8_t smk_col, smk_grp, smk_slot, smk_adc_slot = 0xFF;

/* fm1_adc.h's stand-ins (platform.c): the battery from mux C; the FM-1's MASTER pot is K1 here, a virtual pot
 * (0..1023, 24 a detent, from 724: the FM-1 knob's master_q12 2048) */
enum { FM1_ADC_BATT = 3, FM1_ADC_MASTER = 4 };
static int32_t smk_master = 724;
static volatile struct { uint16_t con, master; uint8_t master_ok, drive; } fm1_an;   /* (no analog rows: con stays 0) */

/* ------------------------------------------------------------------ pins --- */
static void smk__pin_in_pu(uint32_t port, uint32_t m)
{
    FM1_PR(port, FM1_DIE) |= m;
    FM1_PR(port, FM1_DIR) |= m;
    FM1_PR(port, FM1_PD) &= ~m;
    FM1_PR(port, FM1_PU) |= m;
}
static void smk__pin_out(uint32_t port, uint32_t m, int v)
{
    FM1_PR(port, FM1_DIE) |= m;
    FM1_PR(port, FM1_PU) &= ~m;
    FM1_PR(port, FM1_PD) &= ~m;
    if (v)
        FM1_PR(port, FM1_OUT) |= m;
    else
        FM1_PR(port, FM1_OUT) &= ~m;
    FM1_PR(port, FM1_DIR) &= ~m;
}
static void smk__pin_analog(uint32_t port, uint32_t m)
{
    FM1_PR(port, FM1_DIE) &= ~m;
    FM1_PR(port, FM1_DIR) |= m;
    FM1_PR(port, FM1_PD) &= ~m;
    FM1_PR(port, FM1_PU) &= ~m;
}
FM1_INLINE void smk__common(uint32_t grp_on)    /* 0 / 1: that group's common low, the other high; 2: both off */
{
    uint32_t v = SMK_USBIO_CON | 0x1000Fu;      /* (as stock: GPIO mode bits on every write) */
    v |= (1u << 17) | (1u << 18);
    if (grp_on < 2u)
        v &= ~(1u << (17u + grp_on));
    SMK_USBIO_CON = v;
}

FM1_INLINE uint32_t smk__rows(void)             /* 1 = closed */
{
    uint32_t a = FM1_PR(FM1_PA, FM1_IN), h = FM1_PR(FM1_PH, FM1_IN), r;
    r = ((a >> 2) & 1u) | ((a >> 4) & 0x0Eu) | ((a >> 5) & 0x10u) |   /* PA2; PA5..7; PA9 */
        ((h >> 1) & 0x20u) | ((h >> 2) & 0xC0u);                      /* PH6; PH8, PH9 */
    return ~r & 0xFFu;
}

FM1_INLINE void smk__shift(uint32_t col)        /* start the DMA of column col's word */
{
    static uint32_t w;
    w = SMK_COLW(col) & smk_ledw[smk_grp] & smk_muxw;
    SMK_SPI2_CON |= 0x4000u;
    SMK_SPI2_ADR = (uint32_t)(uintptr_t)&w;
    SMK_SPI2_CNT = 4u;
}

FM1_INLINE void smk__latch(void)
{
    FM1_PR(FM1_PA, FM1_OUT) |= 1u << 1;
    FM1_PR(FM1_PA, FM1_OUT) &= ~(1u << 1);
}

/* -------------------------------------------------------------- velocity --- */
static uint32_t smk_curve(const uint16_t *t, uint32_t x)   /* stock curve: 5 points -> 1..1023 */
{
    uint32_t i = 0, v;
    while (i < 5u && x > t[i])
        i++;
    if (i == 0)
        return 1;
    if (i == 5u)
        return 1023;
    v = (i - 1u) * 256u + ((x - t[i - 1u]) << 8) / (uint32_t)(t[i] - t[i - 1u]);
    return v ? v : 1u;
}
FM1_INLINE uint8_t smk_vel7(uint32_t v10) { return (uint8_t)(v10 >> 3 ? v10 >> 3 : 1u); }

static void smk_trim_defaults(void)
{
    static const uint16_t P[5] = {50, 550, 720, 850, 950}, K[5] = {5, 640, 800, 920, 980};
    uint32_t i, j;
    for (i = 0; i < SMK_NPAD; i++)
        for (j = 0; j < 5u; j++)
            smk_trim_pad[i][j] = P[j];
    for (i = 0; i < SMK_NKEY; i++)
        for (j = 0; j < 5u; j++)
            smk_trim_key[i][j] = K[j];
}

/* the factory calibration record (SMK_TRIM_SIZE bytes, "trimdat\0" at +0x224), read from flash 0xC1000; ignored
 * unless the marker and strictly rising curves check out. Returns 1 if taken. */
static int smk_trim_load(const uint8_t *rec)
{
    static const char M[8] = "trimdat";
    uint32_t i, j, ok = 1;
    const uint8_t *p;
    for (i = 0; i < 8u; i++)
        ok &= rec[0x224u + i] == (uint8_t)M[i];
    for (i = 0; ok && i < SMK_NPAD + SMK_NKEY; i++) {
        p = rec + 8u + i * 10u;
        for (j = 1; j < 5u; j++)
            ok &= (p[2u * j] | p[2u * j + 1u] << 8) > (p[2u * j - 2u] | p[2u * j - 1u] << 8);
    }
    if (!ok)
        return 0;
    for (i = 0; i < SMK_NPAD + SMK_NKEY; i++)
        for (j = 0; j < 5u; j++) {
            uint16_t v = (uint16_t)(rec[8u + i * 10u + 2u * j] | rec[9u + i * 10u + 2u * j] << 8);
            if (i < SMK_NPAD)
                smk_trim_pad[i][j] = v;
            else
                smk_trim_key[i - SMK_NPAD][j] = v;
        }
    return 1;
}

/* ------------------------------------------------------------------ init --- */
static void fm1_input_init(void)
{
    uint32_t i;
    smk_trim_defaults();
    smk__pin_out(FM1_PA, (1u << 0) | (1u << 1), 0);              /* PA0 /OE low, PA1 latch low */
    smk__pin_out(FM1_PA, (1u << 3) | (1u << 4), 0);              /* SPI2 CLK, DO */
    smk__pin_in_pu(FM1_PA, (1u << 2) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 9));
    smk__pin_in_pu(FM1_PH, (1u << 6) | (1u << 8) | (1u << 9));
    smk__pin_analog(FM1_PA, (1u << 8) | (1u << 10));             /* ADC ch1, ch2 */
    smk__pin_analog(FM1_PC, 1u << 9);                            /* ADC ch8 (PC9 never an SPI1 clock) */
    smk_ledw[0] = smk_ledw[1] = 0xFFFFFFFFu;
    smk_muxw = 0xFFFFFFC7u;                                      /* mux address 0 (bits 3..5) */
    smk__common(2);
    SMK_IOMAP_CON1 = (SMK_IOMAP_CON1 & ~0x30000u) | 0x20000u;    /* SPI2 group C: PA3 / PA4 */
    SMK_SPI2_CON = 0x4020u;                                      /* master, TX, no IRQ */
    SMK_SPI2_BAUD = 5u;
    SMK_SPI2_CON |= 1u;
    for (i = 0; i < SMK_NENC; i++)
        fm1_in.enc_last[i] = 0xFF;
    for (i = 0; i < SMK_NPAD; i++)
        fm1_in.pad_base[i] = 100u;                               /* (stock init) */
    smk_col = 0;
    smk__shift(0);                                               /* the first word: latched on the first tick */
}

/* ------------------------------------------------------------------ frame --- */
FM1_INLINE uint32_t smk__closed(uint32_t code) { return (fm1_in.raw[code >> 4] >> (code & 15u)) & 1u; }

static void smk__extq(uint32_t code, uint32_t vel)
{
    uint8_t w = smk_extq_w;
    if ((uint8_t)(w + 1u) % SMK_EXTQ == smk_extq_r)
        return;
    smk_extq[w] = (uint16_t)(code << 8 | vel);
    smk_extq_w = (uint8_t)((w + 1u) % SMK_EXTQ);
}

static void smk__key_event(uint32_t k, uint32_t vel)     /* vel 0 = release */
{
    if (vel)
        fm1_in.key_vel[k] = (uint8_t)vel;
    if (k >= SMK_KEY_F3 && k < SMK_KEY_F3 + 27u) {
        uint32_t bit = 1u << (k - SMK_KEY_F3);
        if (vel) {
            fm1_in.notes |= bit;
            fm1_in.notes_pressed |= bit;
        } else {
            fm1_in.notes &= ~bit;
        }
    } else {
        smk__extq(SMK_EXT_KEY + k, vel);
    }
}

static void smk__keys(uint32_t now)
{
    uint32_t k;
    for (k = 0; k < SMK_NKEY; k++) {
        uint32_t s = smk__closed(SMK_KEYC[k][0]) | smk__closed(SMK_KEYC[k][1]) << 1, st = fm1_in.ks_stab[k];
        if (s == 2u)                                      /* contact 2 without 1: treat as 1 then 2 */
            s = 3u;
        if (s == st) {
            fm1_in.ks_rel[k] = 0;
            continue;
        }
        if (s == 0u) {                                    /* contact 1 open: a release after a few frames */
            if (++fm1_in.ks_rel[k] < SMK_KEY_REL)
                continue;
            fm1_in.ks_rel[k] = 0;
            fm1_in.ks_stab[k] = 0;
            if (st == 3u)
                smk__key_event(k, 0);
            continue;
        }
        fm1_in.ks_rel[k] = 0;
        if (st == 0u)
            fm1_in.ks_t1[k] = now;                        /* contact 1 made (or both in one frame) */
        if (s == 3u && st != 3u) {                        /* contact 2 made: the note, timed from contact 1 */
            uint32_t T = 10u + (now - fm1_in.ks_t1[k]) / (100u * FM1_TICKS_PER_US);   /* stock 0.1 ms frames */
            uint32_t x = T <= 0x3FEu ? 0x3FFu - T : 1u;
            smk__key_event(k, smk_vel7(smk_curve(smk_trim_key[k], x)));
        }
        if (s == 1u && st == 3u)                          /* 3 -> 1: still down */
            continue;
        fm1_in.ks_stab[k] = (uint8_t)s;
    }
}

static void smk__buttons(void)
{
    uint32_t i;
    for (i = 0; i < SMK_NBTN; i++) {
        uint32_t closed = smk__closed(SMK_BTNC[i]), bit = 1u << SMK_BTN_FM1[i], on = (fm1_in.buttons & bit) != 0u;
        volatile uint8_t *c = &fm1_in.cnt[i];
        if (closed == on) {
            *c = 0;
            continue;
        }
        if (++*c < (on ? SMK_DEB_RELEASE : SMK_DEB_PRESS))
            continue;
        *c = 0;
        fm1_in.buttons ^= bit;
        if (!on)
            fm1_in.pressed |= bit;
        else
            fm1_in.released |= bit;
    }
}

static void smk__encoders(void)                    /* stock decoder: 3-state window, 4 counts a cycle */
{
    uint32_t e;
    for (e = 0; e < SMK_NENC; e++) {
        uint32_t cur = smk__closed(SMK_ENCC[e][0]) | smk__closed(SMK_ENCC[e][1]) << 1, h;
        if (fm1_in.enc_last[e] == 0xFF) {           /* first frame */
            fm1_in.enc_last[e] = (uint8_t)cur;
            fm1_in.enc_hist[e] = (uint8_t)(cur * 0x15u);
            continue;
        }
        if (cur != fm1_in.enc_last[e]) {            /* must hold for two frames */
            fm1_in.enc_last[e] = (uint8_t)cur;
            continue;
        }
        if (cur == (fm1_in.enc_hist[e] & 3u))
            continue;
        h = ((uint32_t)fm1_in.enc_hist[e] << 2 | cur) & 0x3Fu;
        fm1_in.enc_hist[e] = (uint8_t)h;
        if (h == 0x07u || h == 0x1Eu || h == 0x21u || h == 0x38u)
            fm1_in.enc_cnt[e]--;                     /* A leads */
        else if (h == 0x0Bu || h == 0x12u || h == 0x2Du || h == 0x34u)
            fm1_in.enc_cnt[e]++;                     /* B leads */
        if (fm1_in.enc_cnt[e] >= SMK_ENC_DIV || fm1_in.enc_cnt[e] <= -SMK_ENC_DIV) {   /* one step a detent */
            int32_t s = fm1_in.enc_cnt[e] / SMK_ENC_DIV;
            fm1_in.enc_steps[e] = (int16_t)(fm1_in.enc_steps[e] + s);
            fm1_in.enc_cnt[e] = (int16_t)(fm1_in.enc_cnt[e] - s * SMK_ENC_DIV);
        }
    }
}

static void smk__leds(void)                        /* the next frame shows the other group */
{
    uint32_t i, w[2] = {0xFFFFu, 0xFFFFu};
    for (i = 0; i < SMK_NLED; i++)
        if (fm1_led[i])
            w[SMK_LEDC[i] >> 4] &= ~(1u << (SMK_LEDC[i] & 15u));
    smk_ledw[0] = 0xFFFF0000u | w[0] | 0x38u;       /* (bits 3..5: the mux address, ANDed in) */
    smk_ledw[1] = 0xFFFF0000u | w[1] | 0x38u;
    smk_grp ^= 1u;
}

static void smk__frame(void)
{
    smk__keys(fm1_ticks());
    smk__buttons();
    smk__encoders();
    smk__leds();
    fm1_in.frames++;
}

/* ---------------------------------------------------------------- analog --- */
#define SMK_PAD_ON 24u            /* above the baseline: a hit starts */
#define SMK_PAD_OFF 12u           /* below base + this: released */
#define SMK_PAD_PEAKN 3u          /* samples (2.4 ms apart) for the peak */

static void smk__pad(uint32_t p, uint32_t raw)
{
    uint32_t base = fm1_in.pad_base[p], st = fm1_in.pad_st[p];
    if (st == 0u) {
        if (raw > base + SMK_PAD_ON) {
            fm1_in.pad_st[p] = 1;
            fm1_in.pad_peak[p] = (uint16_t)raw;
        } else if (base > raw) {
            fm1_in.pad_base[p] = (uint16_t)(base - 1u);     /* the baseline follows (stock) */
        } else if (base < smk_trim_pad[p][0]) {
            fm1_in.pad_base[p] = (uint16_t)(base + 2u);
        }
        return;
    }
    if (st < 0x80u) {                                       /* the peak of the first samples */
        if (raw > fm1_in.pad_peak[p])
            fm1_in.pad_peak[p] = (uint16_t)raw;
        if (++st < SMK_PAD_PEAKN) {
            fm1_in.pad_st[p] = (uint8_t)st;
            return;
        }
        fm1_in.pad_st[p] = 0x80;
        fm1_in.pad_low[p] = 0;
        smk__extq(SMK_EXT_PAD + p, smk_vel7(smk_curve(smk_trim_pad[p], fm1_in.pad_peak[p])));
        return;
    }
    fm1_in.pad_press[p] = (uint8_t)(raw <= 0x200u ? raw >> 4 : (raw - 0x200u) / 4u + 32u > 127u ? 127u
                                                                      : (raw - 0x200u) / 4u + 32u);
    if (raw < base + SMK_PAD_OFF) {
        if (++fm1_in.pad_low[p] >= 2u) {
            fm1_in.pad_st[p] = 0;
            fm1_in.pad_press[p] = 0;
            smk__extq(SMK_EXT_PAD + p, 0);
        }
    } else {
        fm1_in.pad_low[p] = 0;
    }
}

static void smk__analog(uint32_t slot, uint32_t raw)
{
    uint32_t a = slot / 3u, ch = slot % 3u;
    if (ch == 0u) {
        smk__pad(SMK_PADMUX[a], raw);
    } else if (ch == 1u) {
        smk__pad(SMK_PADMUX[a] + 8u, raw);
    } else if (a < 4u) {
        uint32_t f = SMK_FADMUX[a], h0 = fm1_in.fad_hist[f][0], h1 = fm1_in.fad_hist[f][1], m, v;
        int32_t acc = fm1_in.fad_acc[f];
        fm1_in.fad_hist[f][1] = (uint16_t)h0;
        fm1_in.fad_hist[f][0] = (uint16_t)raw;
        m = raw > h0 ? (h0 > h1 ? h0 : raw > h1 ? h1 : raw) : (raw > h1 ? raw : h0 > h1 ? h1 : h0);   /* median */
        acc += ((int32_t)m * 16 - acc) / 4;                  /* x16 IIR (signed: it falls as well as rises) */
        fm1_in.fad_acc[f] = (uint16_t)acc;
        v = (uint32_t)acc / 16u / 8u;
        if (v > 127u)
            v = 127u;
        if (v > fm1_in.fader[f] + 1u || v + 1u < fm1_in.fader[f] || (v != fm1_in.fader[f] && (v == 0u || v == 127u)))
            fm1_in.fader[f] = (uint8_t)v;                    /* +-1 dead band, the ends reachable */
    } else if (a < 6u) {
        fm1_in.wheel[a - 4u] = (uint16_t)(fm1_in.wheel[a - 4u] ? fm1_in.wheel[a - 4u] + ((int32_t)raw - fm1_in.wheel[a - 4u]) / 4
                                                              : raw);
    } else if (a == 6u) {
        fm1_in.pedal = (uint16_t)raw;
    } else {
        fm1_in.batt = (uint16_t)raw;
    }
}

FM1_INLINE void smk__adc_start(uint32_t ch)
{
    SMK_ADC_CON = 0;
    if (SMK_WLA_CON0 & (1u << 14))
        SMK_WLA_CON0 &= ~(1u << 14);
    /* the stock SMK timing (0x407D without IE): start delay 4 x 8 ADC clocks, divider 0b101; done in ~40 us */
    SMK_ADC_CON = 0x400Du | ((ch & 0xFu) << 8);
    SMK_ADC_CON |= 0x10u;
    SMK_ADC_CON |= 0x40u;
}

/* ------------------------------------------------------------------- tick --- */
/* one column per call, from the 10 kHz timer ISR (the stock ISR's order, see the top) */
static void fm1_input_tick(void)
{
    static const uint8_t CH[3] = {SMK_ADC_PADA, SMK_ADC_PADB, SMK_ADC_MUXC};
    uint32_t col = smk_col, next, s, ns;
    if (smk_adc_slot != 0xFF) {                              /* the conversion of the last tick */
        if (SMK_ADC_CON & 0x80u) {
            fm1_in.adc_ok++;
            smk__analog(smk_adc_slot, SMK_ADC_RES & 0x3FFu);
        } else {
            fm1_in.adc_miss++;
        }
        SMK_ADC_CON = 0x40u;
        SMK_ADC_CON = 0;
        smk_adc_slot = 0xFF;
    }
    fm1_in.raw[col] = (uint8_t)smk__rows();
    if (col == 0u)                                           /* a new frame: its LED group */
        smk__common(smk_grp);
    smk__latch();                                            /* the word shifted last tick */
    next = (col + 1u) & 15u;
    smk_col = (uint8_t)next;
    s = smk_slot;                                            /* this slot: its mux address was latched now */
    ns = s + 1u == 24u ? 0u : s + 1u;
    smk__adc_start(CH[s % 3u]);
    smk_adc_slot = (uint8_t)s;
    smk_slot = (uint8_t)ns;
    smk_muxw = 0xFFFFFFC7u | (ns / 3u) << 3;                 /* the next slot's address goes out with the next word */
    if (next == 0u)
        smk__frame();                                        /* (before the shift: column 0 carries the new LED group) */
    smk__shift(next);
}

/* one full frame, polled (the rescue's look at the keys, power-on); calls FM1_INPUT_IDLE() while it waits */
static void fm1_input_scan(void)
{
    uint32_t i;
    for (i = 0; i < SMK_NCOL; i++) {
        uint32_t t0 = fm1_ticks();
        fm1_input_tick();
        while ((uint32_t)(fm1_ticks() - t0) < 100u * FM1_TICKS_PER_US)
            FM1_INPUT_IDLE();
    }
}

/* ------------------------------------------------------- main-loop API --- */
static inline uint32_t fm1__lock(void)
{
    __asm__ volatile("cli" ::: "memory");
    return 0;
}
static inline void fm1__unlock(uint32_t v)
{
    (void)v;
    __asm__ volatile("csync\n\tsti" ::: "memory");
}

static int32_t smk__enc_take(uint32_t i)        /* SMK encoder i (K1..K8 = 0..7) */
{
    uint32_t k = fm1__lock();
    int32_t s = fm1_in.enc_steps[i];
    fm1_in.enc_steps[i] = 0;
    fm1__unlock(k);
    return s;
}

static int32_t fm1_enc_take(uint32_t e)         /* the FM-1's encoder e (ENC_SELECT ..) */
{
    uint32_t i;
    for (i = 0; i < SMK_NENC; i++)
        if (SMK_ENC_FM1[i] == e)
            return smk__enc_take(i);
    return 0;
}

static uint32_t fm1_input_edges(uint32_t *released)
{
    uint32_t k = fm1__lock();
    uint32_t p = fm1_in.pressed;
    if (released)
        *released = fm1_in.released;
    fm1_in.pressed = fm1_in.released = 0;
    fm1__unlock(k);
    return p;
}

static uint32_t fm1_input_note_edges(void)
{
    uint32_t k = fm1__lock();
    uint32_t p = fm1_in.notes_pressed;
    fm1_in.notes_pressed = 0;
    fm1__unlock(k);
    return p;
}

static uint32_t smk_key_vel(uint32_t k)         /* note key k's (0 = F3) last velocity, 1..127 */
{
    uint32_t v = k < 27u ? fm1_in.key_vel[SMK_KEY_F3 + k] : 0u;
    return v ? v : 100u;
}

/* the next extra (outer key or pad: SMK_EXT_* + its index << 8 | velocity, 0 = released), -1 if none */
static int32_t smk_ext_take(void)
{
    int32_t v = -1;
    if (smk_extq_r != smk_extq_w) {
        v = smk_extq[smk_extq_r];
        smk_extq_r = (uint8_t)((smk_extq_r + 1u) % SMK_EXTQ);
    }
    return v;
}

/* the LED of the FM-1's button id (0..13); the FM-1's note keys (14 + k) have none on the SMK */
static void fm1_led_key(uint32_t id, int on)
{
    uint32_t i;
    for (i = 0; id < FM1_NBTN && i < SMK_NBTN; i++)
        if (SMK_BTN_FM1[i] == id)
            fm1_led[i] = (uint8_t)(on != 0);
}

static void fm1_adc_init(void) {}
static int32_t fm1_adc_read(uint32_t ch)
{
    if (ch == FM1_ADC_BATT)
        return fm1_in.batt ? (int32_t)fm1_in.batt : -1;
    if (ch == FM1_ADC_MASTER) {
        smk_master += smk__enc_take(0) * 24;
        smk_master = smk_master < 0 ? 0 : smk_master > 1023 ? 1023 : smk_master;
        return smk_master;
    }
    return -1;
}
