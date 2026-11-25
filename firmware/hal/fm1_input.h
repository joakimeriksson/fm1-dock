/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* FM-1 input HAL: key/button/encoder matrix and LEDs.
 *
 * One 11-column x 6-row diode matrix behind a 2x74HC595 chain (PA4 SER, PA3
 * SRCLK, PA1 RCLK), bit-banged and polled; rows PA0, PA5..PA8, PB7 with
 * pull-ups (low = closed). LED lines PH6/PH9/PA9/PA10 light the LED on the
 * same column, row PA7/PA8/PA5/PA6 respectively.
 *
 *   fm1_input_init();
 *   polled:  for (;;) { fm1_input_scan(); ... }
 *   IRQ:     call fm1_input_tick() from a ~10 kHz timer ISR; it advances one
 *            column per call (rows are sampled one tick after the column was
 *            latched, LEDs stay lit in between) and processes a frame every
 *            FM1_NCOL ticks. The main loop reads fm1_in.notes / buttons and
 *            takes edges/steps with fm1_input_edges() / fm1_enc_take().
 *
 * fm1_input_scan() runs one full frame (11 columns, ~0.6 ms) and calls
 * FM1_INPUT_IDLE() while it waits.
 * Keys/buttons (as Felucca 1.0): debounced as soon as their column is read. A press counts after
 * FM1_DEB_PRESS frames closed in a row (1.1-2.2 ms: the matrix has diodes and no ghosting, so a closed
 * sample is a closed key; two in a row keep one stray sample from playing a note), a release after
 * FM1_DEB_RELEASE frames open in a row (~9 ms): a contact bouncing open on the way down, or chattering
 * on the way up, never ends a note early or plays it twice.
 * Encoders: quadrature decoder (every frame's state, + = clockwise) with detent counting, as
 * Felucca 1.0 reworked it (its #23, "knobs skipping or jumping"): an FM-1 detent is one full
 * quadrature cycle (4 transitions) and the knob rests in one state, the one seen at power-on
 * (relearned only after FM1_REST_FRAMES, ~1 s, parked elsewhere). Steps are emitted on arriving
 * back at it, the net transitions rounded to whole cycles (>= 2 counts one: a lost transition or
 * two is forgiven; a two-state jump counts on in the direction of travel). One click = one step
 * at any speed; bounce and back-and-forth cancel out. Never a second rest state: a knob held
 * mid-click taught the complement as a rest and every click counted twice, and short mid-click
 * pauses of a slow turn taught the mid states and the knob went dead (the 0.9 learner).
 * fm1_enc_take() returns the steps.
 * LEDs: set fm1_led[col] (packed row bits, bit1 PA5..bit4 PA8); they are lit while that column is
 * selected (one tick, ~95 us a frame). Two dim layers (fm1_input_tick only), after Felucca 1.0.1 (its
 * #35: the eye is logarithmic, an LED lit 1/4 or 1/6 of the time reads as nearly lit): a short pulse on
 * every frame (~910 Hz, no flicker) at the start of the column's next tick, riding on the 595 shift of
 * the next column (its outputs change only at the latch, so the pulse stays on column p and the key
 * read before it is unchanged). fm1_led_dim[col]: the glow (landmarks, notes under tiles), FM1_GLOW_NS
 * (~1/24 of a lit LED); fm1_led_bg[col]: the backlight (menu LIGHTS), fm1_led_bg_ns, shorter. TIMER4
 * times the pulses bit by bit; only a pulse longer than the whole shift waits for the rest. An LED in
 * several layers takes the brightest. fm1_led_key/btn helpers address them by id.
 */
#pragma once
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_gpio.h"
#include "fm1_cc.h"
#include "fm1_adc.h"

/* Analog rows (Velo, Charles Vestal's charlesvestal/fm1-velocity, from CS-1's fm1_input.h; on with
 * fm1_an.con): the tick also starts an
 * ADC conversion on row PA7 (even frames) or PA8 (odd frames) of the column it has just latched,
 * and takes the result at the next tick, before the rows are read, so every key on those two rows
 * is sampled once per two frames (~2.2 ms): the voltage across the key's carbon pill against the
 * row's pull-up, 1023 open, falling as the pill is pressed harder. FM1_INPUT_ANALOG(col, rowbit, v,
 * driven) gets each sample (rowbit 3 = PA7, 4 = PA8; the key is FM1_KEYMAP[rowbit][col]). The
 * MASTER pot takes the slot of (odd frame, column 0) and lands in fm1_an.master (the main loop
 * must not use fm1_adc_read while the scan owns the converter).
 * Drive: with fm1_an.drive set (1..4), a sample for which FM1_INPUT_DRIVE(col, rowbit) is nonzero
 * is taken with the row driven high instead of pulled up, at strength drive - 1 (HD0 bit 0, HD bit
 * 1: the pin's driver is then the divider's upper half and spreads a seated key's range); the
 * row is an input with its pull-up again when the result is taken. CS-1's probe of the top row
 * and its converter experiments are not here. */
#ifndef FM1_INPUT_ANALOG
#define FM1_INPUT_ANALOG(col, rowbit, v, driven) ((void)0)
#endif
#ifndef FM1_INPUT_DRIVE
#define FM1_INPUT_DRIVE(col, rowbit) 0
#endif

#ifndef FM1_INPUT_IDLE
#define FM1_INPUT_IDLE() ((void)0)
#endif
#ifndef FM1_LED_US
#define FM1_LED_US 40u           /* LED on-time per column (brightness vs scan rate) */
#endif
#define FM1_DEB_PRESS 2u          /* frames closed in a row: a press (a frame = 11 ticks, ~1.1 ms) */
#define FM1_DEB_RELEASE 8u        /* frames open in a row: a release (~9 ms) */
#define FM1_SETTLE_US 10u
#define FM1_REST_FRAMES 900u      /* ~1 s still off the detent state: that is the detent (power-on) */
#define FM1_ENC_GO 20u            /* a 2-state jump from the detent goes on the way of a click this recent (~22 ms) */
#define FM1_NCOL 11u
#define FM1_NKEY 41u              /* ids: 0..13 buttons, 14..40 note keys */
#define FM1_NENC 7u


/* key id at (physical column, packed row bit), -1 = none */
static const int8_t FM1_KEYMAP[6][FM1_NCOL] = {
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},          /* PA0: encoders */
    { 5, 11,  4, 10,  3,  9,  2,  8, -1, -1, -1},          /* PA5 */
    {34, 35, 36, 37, 38, 40, 39, 13,  7,  6, 12},          /* PA6 */
    {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},          /* PA7 */
    { 0,  1, 15, 14, 17, 16, 19, 18, 20, 21, 22},          /* PA8 */
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},          /* PB7: encoder 6 */
};
/* encoder i: A at (col, row bit), B at (col, row bit) */
static const uint8_t FM1_ENC[FM1_NENC][4] = {
    {0, 0, 1, 0}, {2, 0, 3, 0}, {8, 1, 9, 1}, {8, 0, 9, 0}, {6, 0, 7, 0}, {4, 0, 5, 0}, {0, 5, 1, 5},
};
enum { FM1_BTN_OCT_DOWN = 0, FM1_BTN_OCT_UP = 1 };

static const int8_t FM1_LED_PORT[6] = {-1, FM1_PA, FM1_PA, FM1_PH, FM1_PH, -1};
static const uint8_t FM1_LED_BIT[6] = {0, 9, 10, 6, 9, 0};

static volatile struct {
    uint32_t notes;              /* debounced: bit n = note key n (0 = F3 .. 26 = G5) */
    uint32_t buttons;            /* debounced: bit i = button i (0..13) */
    uint32_t pressed, released;  /* button edges since the last fm1_input_edges() */
    uint32_t notes_pressed;      /* note-key press edges since the last fm1_input_note_edges() */
    uint8_t raw[FM1_NCOL];       /* last frame, packed rows, 1 = closed */
    uint8_t cnt[FM1_NKEY];
    uint8_t enc_prev[FM1_NENC], enc_last[FM1_NENC];
    uint8_t enc_rest[FM1_NENC];  /* the detent state (0..3) */
    uint16_t enc_still[FM1_NENC]; /* frames since the last state change */
    int8_t enc_sub[FM1_NENC];    /* net transitions since the last rest state */
    int16_t enc_steps[FM1_NENC]; /* + = clockwise */
    int8_t enc_dir[FM1_NENC];    /* the last click's direction, and its frame */
    uint32_t enc_dfr[FM1_NENC];
    uint32_t frames;
} fm1_in;
static uint8_t fm1_led[FM1_NCOL];
static volatile struct {
    uint16_t con;                /* the conversion's clock / start delay (FM1_ADC_CON); 0 = analog off */
    uint8_t pend, col, rowbit;   /* a conversion runs for (col, rowbit); rowbit 0 = the pot */
    uint16_t master;             /* the MASTER pot, 0..1023 */
    uint8_t master_ok;           /* (one taken since the scan began) */
    uint16_t timeouts;           /* conversions not done after a tick (the settings too slow) */
    uint8_t drive;               /* 0 off; 1..4: driven samples (FM1_INPUT_DRIVE) at strength drive - 1 */
    uint8_t driven;              /* the pending conversion's row is driven high (restore at the take) */
    uint8_t phase;               /* frame phase: 0 PA7, 1 PA8 */
} fm1_an;
#ifndef FM1_GLOW_NS
#define FM1_GLOW_NS 4000u        /* the glow pulse a frame (ns); a lit LED ~95 us: ~1/24 the brightness */
#endif
#define FM1__NS_T(ns) (((uint32_t)(ns) * FM1_TICKS_PER_US + 500u) / 1000u)   /* ns -> TIMER4 ticks */
static uint8_t fm1_led_dim[FM1_NCOL];   /* same layout as fm1_led: the glow */
static uint8_t fm1_led_bg[FM1_NCOL];    /* same layout: the backlight (labels readable in the dark) */
static volatile uint16_t fm1_led_bg_ns; /* the backlight pulse a frame (ns), 0 = off (menu LIGHTS) */

static void fm1__led_lines(uint32_t rowmask)
{
    uint32_t r;
    for (r = 1; r < 5u; r++) {
        if (rowmask & (1u << r))
            FM1_PR(FM1_LED_PORT[r], FM1_OUT) |= 1u << FM1_LED_BIT[r];
        else
            FM1_PR(FM1_LED_PORT[r], FM1_OUT) &= ~(1u << FM1_LED_BIT[r]);
    }
}

static void fm1__sr_bit(uint32_t w, uint32_t i)  /* bit i of w (msb first) into the 595 */
{
    if (w & (0x8000u >> i))
        FM1_PR(FM1_PA, FM1_OUT) |= 1u << 4;
    else
        FM1_PR(FM1_PA, FM1_OUT) &= ~(1u << 4);
    FM1_PR(FM1_PA, FM1_OUT) |= 1u << 3;         /* read-modify-write per edge: each SFR write is */
    FM1_PR(FM1_PA, FM1_OUT) &= ~(1u << 3);      /* far slower than the 595 needs */
}
static void fm1__sr_latch(void)                 /* the outputs change here only */
{
    FM1_PR(FM1_PA, FM1_OUT) |= 1u << 1;
    FM1_PR(FM1_PA, FM1_OUT) &= ~(1u << 1);
}
static void fm1__sr_word(uint32_t w)
{
    uint32_t i;
    for (i = 0; i < 16u; i++)
        fm1__sr_bit(w, i);
    fm1__sr_latch();
}

static uint32_t fm1__rows(void)
{
    uint32_t a = FM1_PR(FM1_PA, FM1_IN), b = FM1_PR(FM1_PB, FM1_IN);
    return (~((a & 1u) | ((a >> 4) & 0x1Eu) | ((b >> 2) & 0x20u))) & 0x3Fu;
}

static void fm1__wait(uint32_t us)
{
    uint32_t t0 = fm1_ticks(), span = us * FM1_TICKS_PER_US;
    while ((uint32_t)(fm1_ticks() - t0) < span)
        FM1_INPUT_IDLE();
}

static void fm1_input_init(void)
{
    static const uint8_t LEDP[4][2] = {{FM1_PH, 6}, {FM1_PH, 9}, {FM1_PA, 9}, {FM1_PA, 10}};
    const uint32_t rows_a = (1u << 0) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8);
    const uint32_t sr = (1u << 1) | (1u << 3) | (1u << 4), row_b = 1u << 7;
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        uint32_t p = LEDP[i][0], m = 1u << LEDP[i][1];
        FM1_PR(p, FM1_DIE) |= m;
        FM1_PR(p, FM1_OUT) &= ~m;
        FM1_PR(p, FM1_DIR) &= ~m;
        FM1_PR(p, FM1_HD0) |= m;
        FM1_PR(p, FM1_HD) |= m;
    }
    FM1_PR(FM1_PA, FM1_DIE) |= rows_a;
    FM1_PR(FM1_PA, FM1_DIR) |= rows_a;
    FM1_PR(FM1_PA, FM1_PD) &= ~rows_a;
    FM1_PR(FM1_PA, FM1_PU) |= rows_a;
    FM1_PR(FM1_PB, FM1_DIE) |= row_b;
    FM1_PR(FM1_PB, FM1_DIR) |= row_b;
    FM1_PR(FM1_PB, FM1_PD) &= ~row_b;
    FM1_PR(FM1_PB, FM1_PU) |= row_b;
    FM1_PR(FM1_PA, FM1_DIE) |= sr;
    FM1_PR(FM1_PA, FM1_PU) &= ~sr;
    FM1_PR(FM1_PA, FM1_PD) &= ~sr;
    FM1_PR(FM1_PA, FM1_OUT) &= ~sr;
    FM1_PR(FM1_PA, FM1_DIR) &= ~sr;
    fm1__sr_word(0xFFFFu);
    for (i = 0; i < FM1_NENC; i++)
        fm1_in.enc_prev[i] = fm1_in.enc_last[i] = 0xFF;   /* seeded by the first frame */
}

static void fm1__key(uint32_t id, uint32_t closed)
{
    volatile uint8_t *c = &fm1_in.cnt[id];
    uint32_t note = id >= 14u, bit = note ? 1u << (id - 14u) : 1u << id;
    uint32_t on = ((note ? fm1_in.notes : fm1_in.buttons) & bit) != 0u;
    if (closed == on) {                            /* agrees with the state: start over */
        *c = 0;
        return;
    }
    if (++*c < (on ? FM1_DEB_RELEASE : FM1_DEB_PRESS))
        return;
    *c = 0;
    if (note) {
        if (!on)
            fm1_in.notes_pressed |= bit;
        fm1_in.notes ^= bit;
    } else {
        fm1_in.buttons ^= bit;
        if (!on)
            fm1_in.pressed |= bit;
        else
            fm1_in.released |= bit;
    }
}

static void fm1__keys(uint32_t p)                  /* the keys of column p, just read */
{
    uint32_t r, raw = fm1_in.raw[p];
    for (r = 1; r < 5u; r++)
        if (FM1_KEYMAP[r][p] >= 0)
            fm1__key((uint32_t)FM1_KEYMAP[r][p], (raw >> r) & 1u);
}

static void fm1__frame(void);

static void fm1_input_scan(void)
{
    uint32_t p;
    for (p = 0; p < FM1_NCOL; p++) {
        fm1__led_lines(0);
        fm1__sr_word(0xFFFFu ^ (1u << p) ^ (p < 2u ? 1u << (11u + p) : 0u));
        fm1__wait(FM1_SETTLE_US);
        fm1_in.raw[p] = (uint8_t)fm1__rows();
        fm1__keys(p);
        fm1__led_lines(fm1_led[p]);
        fm1__wait(FM1_LED_US);
    }
    fm1__led_lines(0);
    fm1__frame();
}

static void fm1__frame(void)
{
    uint32_t e;                                    /* (the keys: fm1__keys, as each column is read) */
    for (e = 0; e < FM1_NENC; e++) {               /* quadrature decoder + detents */
        const uint8_t *m = FM1_ENC[e];
        uint32_t cur = ((fm1_in.raw[m[0]] >> m[1]) & 1u) << 1 | ((fm1_in.raw[m[2]] >> m[3]) & 1u);
        uint32_t idx;
        volatile int8_t *sub = &fm1_in.enc_sub[e];
        if (cur != fm1_in.enc_last[e]) {
            fm1_in.enc_last[e] = (uint8_t)cur;
            fm1_in.enc_still[e] = 0;              /* (no 2-sample filter: every frame's state counts, */
        }                                          /* Vector37 #126; it lost whole clicks of a turn) */
        if (fm1_in.enc_prev[e] == 0xFF) {          /* first frame: the knob rests here */
            fm1_in.enc_prev[e] = (uint8_t)cur;
            fm1_in.enc_rest[e] = (uint8_t)cur;
        }
        if (fm1_in.enc_still[e] < 0xFFFFu && ++fm1_in.enc_still[e] == FM1_REST_FRAMES &&
            cur != fm1_in.enc_rest[e]) {
            fm1_in.enc_rest[e] = (uint8_t)cur;     /* parked ~1 s off the detent state (held at power-on): */
            *sub = 0;                              /* that is the detent. Never a second state (see top) */
        }
        if (cur == fm1_in.enc_prev[e])
            continue;
        idx = (uint32_t)fm1_in.enc_prev[e] << 2 | cur;
        if ((0x4182u >> idx) & 1u)
            (*sub)++;
        else if ((0x2814u >> idx) & 1u)
            (*sub)--;
        else {                                     /* two states in one sample: a fast turn, the way it */
            int32_t d = *sub > 0 ? 1 : *sub < 0 ? -1 : /* was going (on the detent: a click's of the last */
                        (uint32_t)(fm1_in.frames - fm1_in.enc_dfr[e]) <= FM1_ENC_GO ? fm1_in.enc_dir[e] : 0;
            *sub = (int8_t)(*sub + 2 * d);         /* FM1_ENC_GO frames, else nothing; Vector37) */
        }
        fm1_in.enc_prev[e] = (uint8_t)cur;
        if (*sub > 100 || *sub < -100)
            *sub = 0;                              /* (never off the detent that long) */
        if (cur == fm1_in.enc_rest[e]) {           /* back on the detent: whole cycles, a lost transition */
            int32_t n = *sub < 0 ? -*sub : *sub;   /* or two forgiven (one click = 4 transitions) */
            n = n >= 2 ? (n + 2) / 4 : 0;
            fm1_in.enc_steps[e] = (int16_t)(fm1_in.enc_steps[e] + (*sub < 0 ? -n : n));
            if (n) {
                fm1_in.enc_dir[e] = (int8_t)(*sub < 0 ? -1 : 1);
                fm1_in.enc_dfr[e] = fm1_in.frames;
            }
            *sub = 0;
        }
    }
    fm1_in.frames++;
}

/* the drive (analog rows): rowbit 3 = PA7, 4 = PA8; strength 0..3 in HD0 / HD */
static void fm1__row_drive(uint32_t rowbit, uint32_t strength)
{
    uint32_t m = 1u << (rowbit == 3u ? 7 : 8);
    if (strength & 1u)
        FM1_PR(FM1_PA, FM1_HD0) |= m;
    else
        FM1_PR(FM1_PA, FM1_HD0) &= ~m;
    if (strength & 2u)
        FM1_PR(FM1_PA, FM1_HD) |= m;
    else
        FM1_PR(FM1_PA, FM1_HD) &= ~m;
    FM1_PR(FM1_PA, FM1_OUT) |= m;
    FM1_PR(FM1_PA, FM1_DIR) &= ~m;
}
static void fm1__row_release(uint32_t rowbit)
{
    uint32_t m = 1u << (rowbit == 3u ? 7 : 8);
    FM1_PR(FM1_PA, FM1_DIR) |= m;                  /* an input with its pull-up again */
    FM1_PR(FM1_PA, FM1_OUT) &= ~m;
    FM1_PR(FM1_PA, FM1_HD0) &= ~m;
    FM1_PR(FM1_PA, FM1_HD) &= ~m;
}
/* the conversion started last tick (on column p): its result, the row as it was */
static void fm1__an_take(void)
{
    uint32_t driven = fm1_an.driven;
    fm1_an.pend = 0;
    if (driven) {
        fm1_an.driven = 0;
        fm1__row_release(fm1_an.rowbit);
    }
    if (fm1_adc_done()) {
        uint32_t v = fm1_adc_take();
        if (fm1_an.rowbit)
            FM1_INPUT_ANALOG(fm1_an.col, fm1_an.rowbit, v, driven);
        else {
            fm1_an.master = (uint16_t)v;
            fm1_an.master_ok = 1;
        }
    } else {
        fm1_adc_abort();
        fm1_an.timeouts++;
    }
}
/* column n is latched now: sample its PA7 / PA8 key (or the pot) until the next tick */
static void fm1__an_start(uint32_t n)
{
    uint32_t odd = fm1_an.phase, ch = odd ? FM1_ADC_ROW_PA8 : FM1_ADC_ROW_PA7, rowbit = odd ? 4u : 3u;
    if (odd && n == 0u) {
        ch = FM1_ADC_MASTER;
        rowbit = 0;
    }
    fm1_an.col = (uint8_t)n;
    fm1_an.rowbit = (uint8_t)rowbit;
    if (rowbit && fm1_an.drive && FM1_INPUT_DRIVE(n, rowbit)) {
        fm1__row_drive(rowbit, fm1_an.drive - 1u);
        fm1_an.driven = 1;
    }
    fm1_adc_start(ch, fm1_an.con);
    fm1_an.pend = 1;
}

/* one column per call, from a timer ISR (see top). The dim pulses of column p ride on the shift of
 * column n: the lines go lit | glow | backlight of p, each layer ends when its time is up (TIMER4,
 * checked after every bit), and the latch comes with the lines dark. */
static uint8_t fm1__tick_col;
static void fm1_input_tick(void)
{
    uint32_t p = fm1__tick_col, n = p + 1u == FM1_NCOL ? 0u : p + 1u, i;
    uint32_t w = 0xFFFFu ^ (1u << n) ^ (n < 2u ? 1u << (11u + n) : 0u);
    uint32_t lit = fm1_led[p], a = fm1_led_dim[p] & ~lit, b = fm1_led_bg[p] & ~lit & ~a;
    uint32_t ta = a ? FM1__NS_T(FM1_GLOW_NS) : 0u, tb = b ? FM1__NS_T(fm1_led_bg_ns) : 0u;
    uint32_t tmax = ta > tb ? ta : tb;
    if (fm1_an.pend)
        fm1__an_take();                            /* (a driven row an input again before it is read) */
    fm1__led_lines(0);
    fm1_in.raw[p] = (uint8_t)fm1__rows();          /* column p has been latched one tick (the lines dark) */
    if (tmax) {
        uint32_t t0 = fm1_ticks(), cur = lit | a | (tb ? b : 0u), on, d;
        fm1__led_lines(cur);                       /* (the 595 still drives column p) */
        for (i = 0; i < 16u || cur; i++) {         /* the shift; then wait if the pulse is longer */
            if (i < 16u)
                fm1__sr_bit(w, i);
            d = fm1_ticks() - t0;
            on = d < tmax ? lit | (d < ta ? a : 0u) | (d < tb ? b : 0u) : 0u;
            if (on != cur) {
                fm1__led_lines(on);
                cur = on;
            }
        }
        fm1__sr_latch();                           /* column n, the lines dark */
    } else {
        fm1__sr_word(w);
    }
    fm1__led_lines(fm1_led[n]);
    fm1__tick_col = (uint8_t)n;
    fm1__keys(p);                                  /* its keys now: no wait for the frame's end */
    if (n == 0u) {
        fm1__frame();
        fm1_an.phase ^= 1u;
    }
    if (fm1_an.con)
        fm1__an_start(n);
}

/* main-loop critical section against fm1_input_tick (main loop only: it
 * re-enables interrupts unconditionally) */
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

/* detent steps turned since the last call, + = clockwise */
static int32_t fm1_enc_take(uint32_t e)
{
    uint32_t k = fm1__lock();
    int32_t s = fm1_in.enc_steps[e];
    fm1_in.enc_steps[e] = 0;
    fm1__unlock(k);
    return s;
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

/* LED of key id (button 0..13 or note key 14..40) */
static void fm1_led_key(uint32_t id, int on)
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id) {
                if (on)
                    fm1_led[p] |= (uint8_t)(1u << r);
                else
                    fm1_led[p] &= (uint8_t)~(1u << r);
            }
}
