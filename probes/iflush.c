/* SPDX-License-Identifier: GPL-3.0-only */
/* IFLUSH: ways to drop flash lines from the instruction cache, tried on a real unit (fm1-dock #87: code written
 * where other code ran since power-on runs as a mix of the old and the new). Run after such a write; the module
 * before the probe is started again after it, and says what ran.
 * argument: [way u32][address u32][bytes u32]   ways: 1 iflush a 32-byte line, 2 iflush every 4 bytes,
 * 3 flushinv + iflush every 4 bytes, 4 btbclr, 5 flush + flushinv + iflush every 4 bytes and btbclr */
#include "probe.h"

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t way = p_arg32(c, 0, 1), at = p_arg32(c, 1, 0), n = p_arg32(c, 2, 0), a, step = way == 1u ? 32u : 4u, k = 0;
    p_irq_off();
    for (a = at & ~3u; a < at + n; a += step, k++) {
        __asm__ volatile("csync" ::: "memory");
        if (way == 5u) __asm__ volatile("flush [%0]" :: "r"(a) : "memory");
        if (way == 3u || way == 5u) __asm__ volatile("flushinv [%0]" :: "r"(a) : "memory");
        if (way != 4u) __asm__ volatile("iflush [%0]" :: "r"(a) : "memory");
        __asm__ volatile("csync" ::: "memory");
    }
    if (way == 4u || way == 5u) __asm__ volatile("btbclr\n\tcsync" ::: "memory");
    p_irq_on();
    p_u32(c, "way", way);
    p_hex(c, "at", at);
    p_u32(c, "lines", k);
    return 0;
}

HB_PROBE = {HB_PROBE_MAGIC, "IFLUSH", run};
