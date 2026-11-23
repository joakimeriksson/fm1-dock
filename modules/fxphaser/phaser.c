/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Joakim Eriksson */
/* PHASER, an effect of fm1-dock (hb_module.h, "Effects"): each side through four all-pass filters whose corner a slow
 * triangle sweeps (from about 0.5 to 6 kHz), a quarter turn apart left and right, the last one fed back into the
 * first. Added to the clean sound the sweep leaves notches that move. RATE 0.05 to 5 Hz, DEPTH how far it sweeps,
 * FEEDBACK how sharp the notches are, MIX how much is heard. About 5 % of a block. */
#include "hb_module.h"

#define STAGES 4

static volatile uint32_t inc;                   /* the LFO's phase a sample (2^32 a turn) */
static volatile int32_t half;                   /* Q15: how far the coefficient moves either side of its centre */
static volatile int32_t fb;                     /* Q15 */
static volatile int32_t wetg, dryg;             /* Q16: what the sum is scaled by */
static int32_t val[4];
static uint32_t ph;
static int32_t zs[2][STAGES], last[2];

#define CENTRE 22282                            /* Q15 0.68 */

static int init(void)
{
    int c, s;
    for (c = 0; c < 2; c++) { last[c] = 0; for (s = 0; s < STAGES; s++) zs[c][s] = 0; }
    ph = 0;
    return 0;
}

static void set(uint32_t k, int32_t v)          /* the main loop's */
{
    uint32_t m16, nrm;
    if (k < 4u) val[k] = v;
    inc = (uint32_t)(5 + 495 * val[0] * val[0] / 10000) * 974u;     /* 0.05 .. 5 Hz, slow at the bottom */
    half = val[1] * 8847 / 100;                                     /* 0.27 of the range at 100 % */
    fb = val[2] * 27853 / 100;                                      /* up to 0.85 */
    m16 = (uint32_t)val[3] * 65536u / 100u;
    nrm = (0x80000000u / (65536u + m16)) << 1;                      /* 65536 / (1 + m): the sum keeps its level */
    dryg = (int32_t)nrm;
    wetg = (int32_t)(((uint64_t)nrm * m16) >> 16);
}

static void run(int32_t *lr, uint32_t n)        /* the audio interrupt's */
{
    int32_t f = fb, dg = dryg, wg = wetg, hf = half;
    uint32_t i, p = ph;
    for (i = 0; i < n; i++, p += inc) {
        int c;
        for (c = 0; c < 2; c++) {
            uint32_t t = (p + (c ? 0x40000000u : 0u)) >> 15;
            int32_t x = lr[2 * i + c], y, a, s;
            if (t > 65535u) t = 131071u - t;                        /* a triangle, 0..65535 */
            a = CENTRE + (int32_t)(((int64_t)hf * ((int32_t)t - 32768)) >> 15);   /* (the products below are rounded, not floored: a floor leaves a DC in the loop) */
            y = x + (int32_t)(((int64_t)f * last[c] + 16384) >> 15);
            if (y > 65535) y = 65535;
            if (y < -65535) y = -65535;
            for (s = 0; s < STAGES; s++) {          /* y = a x + z;  z = x - a y */
                int32_t o = (int32_t)(((int64_t)a * y + 16384) >> 15) + zs[c][s];
                zs[c][s] = y - (int32_t)(((int64_t)a * o + 16384) >> 15);
                y = o;
            }
            last[c] = y;
            y = (int32_t)(((int64_t)x * dg + (int64_t)y * wg + 32768) >> 16);
            lr[2 * i + c] = y > 32767 ? 32767 : y < -32767 ? -32767 : y;
        }
    }
    ph = p;
}

static const hb_fx_param_t PARAMS[] = {
    {"RATE", "%", 0, 100, 25, 1}, {"DEPTH", "%", 0, 100, 70, 1}, {"FEEDBACK", "%", 0, 100, 40, 1}, {"MIX", "%", 0, 100, 50, 1}};
HB_FX = {HB_FX_ABI, "PHASER", 4, PARAMS, init, set, run};
