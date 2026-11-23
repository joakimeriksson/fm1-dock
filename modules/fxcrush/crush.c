/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Joakim Eriksson */
/* CRUSH, an effect of fm1-dock (hb_module.h, "Effects"): a bit crusher. BITS keeps the top 2 to 16 bits of each side
 * (rounded to the nearest step, so silence stays silent), RATE holds each value for 1 to 40 frames (44.1 kHz / RATE),
 * MIX blends it with the clean sound. The grit of an early sampler; the sample-rate part folds high notes back
 * down on purpose. No RAM to speak of. */
#include "hb_module.h"

static volatile int32_t shift;                  /* 16 - BITS */
static volatile int32_t hold;                   /* frames a value is kept */
static volatile int32_t wet;                    /* Q16 */
static int32_t val[3];
static int32_t held[2], count;

static int init(void) { held[0] = held[1] = 0; count = 0; return 0; }

static void set(uint32_t k, int32_t v)          /* the main loop's */
{
    if (k < 3u) val[k] = v;
    shift = 16 - val[0];
    hold = val[1];
    wet = val[2] * 65536 / 100;
}

static void run(int32_t *lr, uint32_t n)        /* the audio interrupt's */
{
    int32_t s = shift, h = hold, w = wet, c = count;
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t l = lr[2 * i], r = lr[2 * i + 1];
        if (c == 0) {                           /* a new value: the nearest step (zero is one: no DC when idle) */
            held[0] = ((l + ((1 << s) >> 1)) >> s) << s;
            held[1] = ((r + ((1 << s) >> 1)) >> s) << s;
        }
        if (++c >= h) c = 0;
        l += (int32_t)(((int64_t)(held[0] - l) * w) >> 16);
        r += (int32_t)(((int64_t)(held[1] - r) * w) >> 16);
        lr[2 * i] = l > 32767 ? 32767 : l < -32767 ? -32767 : l;
        lr[2 * i + 1] = r > 32767 ? 32767 : r < -32767 ? -32767 : r;
    }
    count = c;
}

static const hb_fx_param_t PARAMS[] = {
    {"BITS", "bit", 2, 16, 8, 1}, {"RATE", "frames", 1, 40, 4, 1}, {"MIX", "%", 0, 100, 100, 1}};
HB_FX = {HB_FX_ABI, "CRUSH", 3, PARAMS, init, set, run};
