/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Joakim Eriksson */
/* CHORUS, an effect of fm1-dock (hb_module.h, "Effects"): each side through a delay line of 12 ms that a slow
 * triangle sweeps by up to 6 ms more, a quarter turn apart left and right, beside the sound itself.
 * RATE 0.1 to 3 Hz, DEPTH the sweep, MIX how much of the line. 8 KiB of the effect's RAM. */
#include "hb_module.h"
#define SW_FRAMES 2048u
#include "fx_sweep.h"

static int32_t val[3];
static void set(uint32_t k, int32_t v)
{
    sw_coef_t *c = &sw_next;
    if (k < 3u) val[k] = v;
    c->inc = sw_inc(100u + 2900u * (uint32_t)val[0] / 100u);
    c->base = 530 << 8;
    c->depth = (int32_t)(265u * (uint32_t)val[1] / 100u) << 8;
    c->fb = 0;
    c->wet = val[2] * 65536 / 100;
    c->dry = 65536 - c->wet / 4;
    __asm__ volatile("" ::: "memory");         /* (the values stored before sw_new) */
    sw_new = 1;
}
static int init(void) { return 0; }

static const hb_fx_param_t PARAMS[] = {
    {"RATE", "%", 0, 100, 30, 1}, {"DEPTH", "%", 0, 100, 50, 1}, {"MIX", "%", 0, 100, 50, 1}};
HB_FX = {HB_FX_ABI, "CHORUS", 3, PARAMS, init, set, sw_run};
