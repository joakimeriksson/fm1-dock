/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Joakim Eriksson */
/* The AC79's hardware EQ (JL_EQ = 0x1E000: CON0..3, DATAI_ADR, DATAO_ADR, DATA_LEN, FLT_ADR; its RAM at
 * 0x1E800), programmed as JieLi's SDK driver does (cpu.a eq.c.o) and as probes/hweq.c measured it on an FM-1.
 * It runs biquad sections over a block beside the CPU. A (channel, section) slot j = channel x sections +
 * section is 8 words: the f32 coefficients [g, c1, c2, c3, c4], H(z) = g (1 + c4 z^-1 + c2 z^-2) /
 * (1 - c3 z^-1 - c1 z^-2), then 3 words of the section's state, which a run writes back; slots 0..19 are
 * in the EQ's RAM, the rest at FLT_ADR.
 *
 *   fm1_eq_slot(j, c, clear)        slot j's coefficients (5 words), its state zeroed with clear: with the EQ off
 *                                   (fm1_eq_stop): while CON0 has bit 1 the CPU's writes to its RAM are lost, and
 *                                   its reads come back as anything (an FM-1, 2026-10-30)
 *   fm1_eq_start_s32x2(in, out, n, sections, flt)   a run over n interleaved stereo s32 frames
 *   fm1_eq_done()                   the run has ended
 *   fm1_eq_finish() / fm1_eq_stop() after a run / to stop one (it writes nothing more)
 */
#pragma once
#include <stdint.h>
#include "fm1_cc.h"

#define FM1_EQ_REG(n) (*(volatile uint32_t *)(0x1E000u + 4u * (n)))
#define FM1_EQ_RAM(n) (*(volatile uint32_t *)(0x1E800u + 4u * (n)))
#define FM1_EQ_RAM_SLOTS 20u

FM1_INLINE void fm1_eq_slot(uint32_t j, const uint32_t *c, uint32_t clear)
{
    uint32_t k;
    for (k = 0; k < 5u; k++)
        FM1_EQ_RAM(8u * j + k) = c[k];
    if (clear)
        FM1_EQ_RAM(8u * j + 5u) = FM1_EQ_RAM(8u * j + 6u) = FM1_EQ_RAM(8u * j + 7u) = 0;
}

FM1_INLINE void fm1_eq_start_s32x2(const int32_t *in, int32_t *out, uint32_t n, uint32_t sections, uint32_t *flt)
{
    FM1_EQ_REG(0) = 0;
    FM1_EQ_REG(7) = (uint32_t)flt;
    FM1_EQ_REG(0) = (sections - 1u) << 16 | 1u << 8 | 1u << 6 | 1u << 4;   /* 2 out channels, s32 out, s32 in */
    FM1_EQ_REG(1) = 4u << 16 | 4u;              /* the channel step, out and in: interleaved, the next word */
    FM1_EQ_REG(2) = 4u << 16 | 4u;              /* (times channels - 1) */
    FM1_EQ_REG(3) = 8u << 16 | 8u;              /* the sample step: a frame */
    FM1_EQ_REG(4) = (uint32_t)in;
    FM1_EQ_REG(5) = (uint32_t)out;
    FM1_EQ_REG(6) = n - 1u;
    FM1_EQ_REG(0) |= 1u << 30;                  /* (done cleared) */
    FM1_EQ_REG(0) |= 3u;
}

FM1_INLINE uint32_t fm1_eq_done(void) { return FM1_EQ_REG(0) >> 31; }

FM1_INLINE void fm1_eq_finish(void)
{
    FM1_EQ_REG(0) |= 1u << 30;
    FM1_EQ_REG(0) &= ~1u;
}

FM1_INLINE void fm1_eq_stop(void) { FM1_EQ_REG(0) = 0; }
