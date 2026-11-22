/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Joakim Eriksson */
/* TREMOLO, an effect of fm1-dock (hb_module.h, "Effects"), and the developer's guide's example of one: the level
 * of both sides moved by a slow triangle. RATE in tenths of a Hz, DEPTH how far down it goes. No RAM to speak of. */
#include "hb_module.h"

static volatile uint32_t inc;                   /* the triangle's phase a sample (2^32 a turn) */
static volatile int32_t depth;                  /* Q16 */
static uint32_t ph;

static int init(void) { return 0; }

static void set(uint32_t k, int32_t v)          /* the main loop's: a value turned (or kept from before) */
{
    if (k == 0) inc = (uint32_t)v * 9739u;      /* v / 10 Hz: 2^32 / 44100 a Hz */
    else depth = v * 65536 / 100;
}

static void run(int32_t *lr, uint32_t n)        /* the audio interrupt's: n frames, L R, in place */
{
    uint32_t i;
    for (i = 0; i < n; i++, ph += inc) {
        uint32_t t = ph >> 15;
        int32_t g;
        if (t > 65535u) t = 131071u - t;        /* a triangle, 0..65535 */
        g = 65536 - (int32_t)(((int64_t)depth * (int32_t)t) >> 16);
        lr[2 * i] = (int32_t)(((int64_t)lr[2 * i] * g) >> 16);
        lr[2 * i + 1] = (int32_t)(((int64_t)lr[2 * i + 1] * g) >> 16);
    }
}

static const hb_fx_param_t PARAMS[] = {
    {"RATE", "/10 Hz", 1, 100, 40, 1},          /* name, unit, from, to, where it starts, a detent's step */
    {"DEPTH", "%", 0, 100, 60, 1}};
HB_FX = {HB_FX_ABI, "TREMOLO", 2, PARAMS, init, set, run};
