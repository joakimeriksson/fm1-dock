/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Joakim Eriksson */
/* DELAY, an effect of fm1-dock (hb_module.h, "Effects"): one line of both sides' sum at half the sample rate (a
 * lowpass before it, so the repeats are darker, as a tape's), 30 to 500 ms, fed back, heard on both sides beside
 * the sound. TIME, FEEDBACK (up to 85 %), MIX. 21.5 KiB of the effect's RAM: 11025 samples, half a second.
 *
 * set() writes d_next and then d_new; run() takes it between two blocks. Integers only. */
#include "hb_module.h"

#define D_N 11025u                              /* the line: 0.5 s at 22.05 kHz */
typedef struct { uint32_t len; int32_t fb, wet, dry; } d_coef_t;
static int16_t d_buf[D_N];
static d_coef_t d_c, d_next;
static volatile uint32_t d_new;
static uint32_t d_w, d_odd;
static int32_t d_lp, d_y0, d_y1, val[3];

static inline int32_t sat(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }
static inline int32_t q16(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }

static void set(uint32_t k, int32_t v)
{
    d_coef_t *c = &d_next;
    if (k < 3u) val[k] = v;
    c->len = (uint32_t)val[0] * 2205u / 100u;   /* ms at 22.05 kHz */
    if (c->len < 1u) c->len = 1u;
    if (c->len > D_N - 1u) c->len = D_N - 1u;
    c->fb = val[1] * 65536 / 100;
    c->wet = val[2] * 65536 / 100;
    c->dry = 65536 - c->wet / 4;
    __asm__ volatile("" ::: "memory");
    d_new = 1;
}
static int init(void) { return 0; }

static void run(int32_t *o, uint32_t n)
{
    uint32_t i, w = d_w, len;
    int32_t lp = d_lp, y0 = d_y0, y1 = d_y1, fb, wet, dry;
    if (d_new) {
        d_c = d_next;
        d_new = 0;
    }
    len = d_c.len; fb = d_c.fb; wet = d_c.wet; dry = d_c.dry;
    for (i = 0; i < n; i++) {
        int32_t l = o[2 * i], r = o[2 * i + 1], y;
        lp += ((((l + r) >> 1) - lp) * 39321) >> 16;            /* about 6 kHz: the line runs at half the rate */
        if (!d_odd) {                           /* a sample of the line every other frame */
            uint32_t rd = w >= len ? w - len : w + D_N - len;
            y0 = y1;
            y1 = d_buf[rd];
            d_buf[w] = (int16_t)sat(lp + q16(y1, fb));
            if (++w == D_N) w = 0;
            y = (y0 + y1) >> 1;                 /* (the frame between two of its samples) */
        } else {
            y = y1;
        }
        d_odd ^= 1u;
        o[2 * i] = sat(q16(l, dry) + q16(y, wet));
        o[2 * i + 1] = sat(q16(r, dry) + q16(y, wet));
    }
    d_w = w; d_lp = lp; d_y0 = y0; d_y1 = y1;
}

static const hb_fx_param_t PARAMS[] = {
    {"TIME", "ms", 30, 500, 185, 5}, {"FEEDBACK", "%", 0, 85, 35, 1}, {"MIX", "%", 0, 100, 30, 1}};
HB_FX = {HB_FX_ABI, "DELAY", 3, PARAMS, init, set, run};
