/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Joakim Eriksson */
/* FLANGER, an effect of fm1-dock (hb_module.h, "Effects"): each side through a delay line of 0.5 ms that a slow
 * triangle sweeps by up to 5 ms more, a quarter turn apart left and right, fed back into itself, beside the sound.
 * RATE 0.05 to 2 Hz, DEPTH the sweep, FEEDBACK how much of the line goes round, MIX how much of it is heard.
 * 2 KiB of the effect's RAM. */
#include "hb_module.h"
#define SW_FRAMES 512u
#include "fx_sweep.h"

static int32_t val[4];
static void set(uint32_t k, int32_t v)
{
    sw_coef_t *c = &sw_next;
    if (k < 4u) val[k] = v;
    c->inc = sw_inc(50u + 1950u * (uint32_t)val[0] / 100u);
    c->base = 22 << 8;
    c->depth = (int32_t)(220u * (uint32_t)val[1] / 100u) << 8;
    c->fb = val[2] * 65536 / 100;
    c->wet = val[3] * 65536 / 100;
    c->dry = 65536 - c->wet / 4;
    __asm__ volatile("" ::: "memory");
    sw_new = 1;
}
static int init(void) { return 0; }

static const hb_fx_param_t PARAMS[] = {
    {"RATE", "%", 0, 100, 20, 1}, {"DEPTH", "%", 0, 100, 60, 1}, {"FEEDBACK", "%", 0, 90, 55, 1}, {"MIX", "%", 0, 100, 50, 1}};
HB_FX = {HB_FX_ABI, "FLANGER", 4, PARAMS, init, set, sw_run};
