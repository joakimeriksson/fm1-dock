/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Joakim Eriksson */
/* The master EQ: three bands (a LOW shelf, a MID peak, a HIGH shelf: tools/eq_table.py) on what the module
 * renders, before MASTER, so the speaker and USB audio get the same. The AC79's hardware EQ (JL_EQ, 0x1E000;
 * fm1-dock's probes/hweq.c measured it on an FM-1, fm1-emu's hweq.rs models it) filters the block beside the
 * CPU, which starts it and waits: six sections over 256 frames, ~0.2 ms (3 % of a block).
 *
 * It runs in the module's render (app_audio), so its time is the module's in the render stats and the overload
 * check. The main loop sets it (eq_set: on, the gains); the audio ISR writes the sections into the EQ's RAM before
 * its next run, and clears their state when it starts from silence (switched on, another module). A run that
 * does not finish stops the EQ for good (eq_failed: that block and the rest play unfiltered). */
#include "eq_table.h"
#include "fm1_eq.h"

#define EQ_POLLS 200000u                        /* (a run is done in ~5 000 polls; this many: it is stuck) */

static int32_t eq_in[2u * HALF_FRAMES] __attribute__((aligned(16)));   /* the module's block, the EQ's input */
static uint32_t eq_flt[8] __attribute__((aligned(16)));   /* FLT_ADR (slots past 20: none here), as the SDK sets it */
static uint8_t eq_idx[EQ_BANDS];                /* the gains as the ISR takes them: dB + EQ_DB_MAX */
static volatile uint8_t eq_on, eq_new, eq_clear, eq_failed;

static void eq_set(uint32_t on, const int8_t *db)   /* the main loop's */
{
    uint32_t b;
    for (b = 0; b < EQ_BANDS; b++)
        eq_idx[b] = (uint8_t)(db[b] + EQ_DB_MAX);
    if (on && !eq_on)
        eq_clear = 1;                           /* (from silence) */
    eq_new = 1;                                 /* (after the gains: a block that took half of them takes them again) */
    eq_on = (uint8_t)on;
}

static void eq_audio(int32_t *o, uint32_t n)    /* the audio ISR's: o, n frames interleaved, filtered in place */
{
    uint32_t i, j, polls = 0;
    if (!eq_on || eq_failed)
        return;
    if (eq_new || eq_clear) {
        eq_new = 0;
        for (j = 0; j < 2u * EQ_BANDS; j++)    /* the left channel's sections, then the right's */
            fm1_eq_slot(j, EQ_TABLE[j % EQ_BANDS][eq_idx[j % EQ_BANDS]], eq_clear);
        eq_clear = 0;
    }
    for (i = 0; i < 2u * n; i++)
        eq_in[i] = o[i];
    fm1_eq_start_s32x2(eq_in, o, n, EQ_BANDS, eq_flt);
    while (!fm1_eq_done())
        if (++polls >= EQ_POLLS) {
            fm1_eq_stop();
            eq_failed = 1;
            for (i = 0; i < 2u * n; i++)
                o[i] = eq_in[i];
            return;
        }
    fm1_eq_finish();
}
