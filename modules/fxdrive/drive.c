/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Joakim Eriksson */
/* DRIVE, an effect of fm1-dock (hb_module.h, "Effects"): an overdrive that goes to a distortion. Each side is lifted
 * by DRIVE (1x to 20x, gently at first: the lower half is an overdrive), shaped by a smooth cubic that flattens out at the top (1.5 x - 0.5 x^3: no hard corner, so
 * it bites without the fizz of a clamp), then darkened by TONE (a one-pole low-pass, 0.9 to 20 kHz) and set by LEVEL.
 * MIX blends it with the clean sound. No RAM to speak of. */
#include "hb_module.h"

static volatile int32_t gain;                   /* Q8: the lift before the shaper */
static volatile int32_t lp;                     /* Q16: the tone's one-pole coefficient */
static volatile int32_t level;                  /* Q16 */
static volatile int32_t wet;                    /* Q16: how much of the shaped sound */
static int32_t val[4];
static int32_t z[2];                            /* the tone's state, a side each */

static int init(void) { z[0] = z[1] = 0; return 0; }

static void set(uint32_t k, int32_t v)          /* the main loop's */
{
    if (k < 4u) val[k] = v;
    gain = 256 + val[0] * val[0] * 4864 / 10000;            /* 1x .. 20x, slow at the bottom */
    lp = 8000 + (val[1] * val[1]) * 52000 / 10000;          /* .. nearly open at 100 % */
    level = val[2] * 65536 / 100;
    wet = val[3] * 65536 / 100;
}

static inline int32_t shape(int32_t x, int32_t g)
{
    int32_t t = (int32_t)(((int64_t)x * g) >> 8), x2, x3;
    if (t > 32767) t = 32767;
    if (t < -32767) t = -32767;
    x2 = (t * t) >> 15;
    x3 = (x2 * t) >> 15;
    return (3 * t - x3) >> 1;                   /* 1.5 t - 0.5 t^3: +-32767 at the ends, flat there */
}

static void run(int32_t *lr, uint32_t n)        /* the audio interrupt's */
{
    int32_t g = gain, a = lp, lv = level, w = wet, ch = 0;
    uint32_t i;
    for (i = 0; i < 2 * n; i++, ch ^= 1) {
        int32_t x = lr[i], y = shape(x, g), o;
        z[ch] += (int32_t)(((int64_t)(y - z[ch]) * a) >> 16);
        y = (int32_t)(((int64_t)z[ch] * lv) >> 16);
        o = x + (int32_t)(((int64_t)(y - x) * w) >> 16);
        lr[i] = o > 32767 ? 32767 : o < -32767 ? -32767 : o;
    }
}

static const hb_fx_param_t PARAMS[] = {
    {"DRIVE", "%", 0, 100, 40, 1}, {"TONE", "%", 0, 100, 60, 1}, {"LEVEL", "%", 0, 100, 50, 1}, {"MIX", "%", 0, 100, 100, 1}};
HB_FX = {HB_FX_ABI, "DRIVE", 4, PARAMS, init, set, run};
