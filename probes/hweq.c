/* SPDX-License-Identifier: GPL-3.0-only */
/* HWEQ: the AC79's hardware EQ (JL_EQ, 0x1E000: CON0..3, DATAI_ADR, DATAO_ADR, DATA_LEN, FLT_ADR; its RAM
 * at 0x1E800), one run of it, programmed as JieLi's SDK driver does (cpu.a eq.c.o, audio_hw_eq_ch_start;
 * fm1-emu's tools/sdk_mmio.py lists it): fm1-emu's tools/hwdsp.py drives this and keeps the results.
 *
 * The driver: a (channel, section) slot is 8 floats, 5 coefficients then 3 words of the section's state,
 * channel after channel, from the EQ's RAM (0x1E800) on, then at FLT_ADR. CON0 = (sections - 1) << 16 |
 * (out channels - 1) << 8 | out format << 6 | in format << 4 | run mode << 2 (formats: 0 short, 1 int,
 * 2 float), then bit 30 (clear done), bit 15 (its interrupt: not here, nothing would take it) and bits 0-1
 * (start); bit 31 reads done. CON1 = out channel step << 16 | in channel step, CON2 = the same times
 * (channels - 1), CON3 = out sample step << 16 | in sample step (bytes); DATA_LEN = frames - 1.
 *
 * argument (u32s): op, then
 *   op 0  the registers and the first words of the EQ's RAM
 *   op 1  a run: fmt = sections | in << 8 | out << 10 | run << 12 | interleaved in << 14 | out << 15 |
 *         channels << 16 | out channels << 20 | slots in FLT_ADR only << 24, frames, gen, amp, seed,
 *         out_n (words of output to return), con0 extra bits, nslots | given << 8 | marker << 16 (slot
 *         i is given slot i % given, given 0: nslots; with marker m > 0, slot m - 1 is given slot 1 and
 *         every other given slot 0), then given x 8 words (the slots). Slots past the EQ's RAM (26 of
 *         them, its 208 words) go on at FLT_ADR, as the driver's ring does.
 * gen as probes/hwdsp.c's, per sample (short: halfwords; float: the value converted); the input is
 * in its channels' layout (block: channel after channel; interleaved: frame after frame).
 * The result: regs before, con_done (CON0 when done), polls, ticks, out (out_n words), slots (the slots'
 * words after the run: their state). */
/* hbmod-cflags: -mcpu=r3 */
#include "probe.h"

#define EQ ((volatile uint32_t *)0x1E000u)
#define EQ_RAM ((volatile uint32_t *)0x1E800u)
#define EQ_RAM_WORDS 208u
#define POLLS 2000000u
#define BUF 4096u

static int32_t in_b[BUF] __attribute__((aligned(16)));
static int32_t out_b[BUF + 64] __attribute__((aligned(16)));
static uint32_t flt[8 * 64] __attribute__((aligned(16)));

static int32_t gen1(uint32_t gen, int32_t amp, uint32_t p, uint32_t i, uint32_t *x)
{
    switch (gen) {
    case 1: return i == p ? amp : 0;
    case 2: return amp;
    case 3: *x ^= *x << 13; *x ^= *x >> 17; *x ^= *x << 5;
            return amp ? (int32_t)((*x >> 1) % (2u * (uint32_t)amp + 1u)) - amp : 0;
    case 4: return (int32_t)i * amp + (int32_t)p;
    case 5: *x ^= *x << 13; *x ^= *x >> 17; *x ^= *x << 5; return (int32_t)*x;
    default: return 0;
    }
}

static void regs(hb_probe_ctx_t *c)
{
    uint32_t r[8], i;
    for (i = 0; i < 8u; i++) r[i] = EQ[i];
    p_words(c, "regs0", r, 8);
}

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t op = p_arg32(c, 0, 0), i;
    regs(c);
    if (op == 0) {
        uint32_t w[32];
        for (i = 0; i < 32u; i++) w[i] = EQ_RAM[i];
        p_words(c, "ram", w, 32);
        return 0;
    }
    uint32_t fmt = p_arg32(c, 1, 0), n = p_arg32(c, 2, 64);
    uint32_t nsec = fmt & 63u, inm = (fmt >> 8) & 3u, outm = (fmt >> 10) & 3u, runm = (fmt >> 12) & 3u;
    uint32_t dim = (fmt >> 14) & 1u, dom = (fmt >> 15) & 1u, ch = (fmt >> 16) & 15u, och = (fmt >> 20) & 15u;
    uint32_t cpu_only = (fmt >> 24) & 1u;
    uint32_t gen = p_arg32(c, 3, 0), seed = p_arg32(c, 5, 0), out_n = p_arg32(c, 6, 0), extra = p_arg32(c, 7, 0);
    int32_t amp = (int32_t)p_arg32(c, 4, 0);
    uint32_t nslots = p_arg32(c, 8, 0) & 0xFFu, given = (p_arg32(c, 8, 0) >> 8) & 0xFFu, x = seed ? seed : 1u;
    uint32_t marker = (p_arg32(c, 8, 0) >> 16) & 0xFFu;
    uint32_t ib = inm ? 4u : 2u, ob = outm ? 4u : 2u;
    if (!ch) ch = 1;
    if (!och) och = ch;
    if (nslots > 64u) nslots = 64u;
    if (!given || given > nslots) given = nslots;
    if (given > 8u) given = 8u;
    if (n * ch * ib > BUF * 4u) n = BUF * 4u / (ch * ib);

    /* the input, in its layout: sample s of channel k at (block) k * n + s, (interleaved) s * ch + k */
    for (i = 0; i < n * ch; i++) {
        int32_t v = gen1(gen, amp, seed, i, &x);
        if (inm == 0) ((int16_t *)in_b)[i] = (int16_t)v;
        else if (inm == 1) in_b[i] = v;
        else ((float *)in_b)[i] = (float)v;
    }
    for (i = 0; i < BUF + 64u; i++) out_b[i] = 0x5A5A5A5A;

    /* the slots: in the EQ's RAM (as the driver's first sections), or all at FLT_ADR */
    for (i = 0; i < EQ_RAM_WORDS; i++) EQ_RAM[i] = 0;
    for (i = 0; i < 8u * 64u; i++) flt[i] = 0;
    for (i = 0; i < 8u * nslots; i++) {
        uint32_t g = marker ? (i / 8u == marker - 1u ? 1u : 0u) : i / 8u % given;
        uint32_t w = p_arg32(c, 9u + g * 8u + i % 8u, 0);
        if (cpu_only) flt[i] = w;
        else if (i < EQ_RAM_WORDS) EQ_RAM[i] = w;
        else flt[i - EQ_RAM_WORDS] = w;
    }

    uint32_t in_step = (dim ? ch : 1u) * ib, out_step = (dom ? och : 1u) * ob;
    uint32_t chl_in = runm == 1u ? 0u : dim ? ib : in_step * n;
    uint32_t chl_out = dom ? ob : out_step * n;
    uint32_t con0 = ((och - 1u) << 8) | (runm << 2) | (inm << 4) | (outm << 6) | ((nsec - 1u) << 16) | extra;
    uint32_t t0, t1, polls = 0, done;

    p_irq_off();
    EQ[0] = 0;
    EQ[7] = (uint32_t)flt;
    EQ[0] = con0;
    EQ[1] = (chl_out << 16) | chl_in;
    EQ[2] = ((chl_out << 16) * (och - 1u)) | (chl_in * (ch - 1u));
    EQ[3] = (out_step << 16) | in_step;
    EQ[4] = (uint32_t)in_b;
    EQ[5] = (uint32_t)out_b;
    EQ[6] = n - 1u;
    EQ[0] |= 1u << 30;
    t0 = p_ticks();
    EQ[0] |= 3u;
    while (!((done = EQ[0]) & (1u << 31)) && ++polls < POLLS)
        ;
    t1 = p_ticks();
    EQ[0] |= 1u << 30;
    EQ[0] &= ~1u;
    p_irq_on();

    p_hex(c, "con0", con0);
    p_hex(c, "con_done", done);
    p_hex(c, "con_after", EQ[0]);
    p_u32(c, "polls", polls < POLLS ? polls + 1u : 0u);
    p_u32(c, "ticks", t1 - t0);
    if (out_n > BUF + 64u) out_n = BUF + 64u;
    if (out_n > 1500u) out_n = 1500u;
    p_words(c, "out", (const uint32_t *)out_b, out_n);
    {
        uint32_t w[64], k = nslots < 8u ? 8u * nslots : 64u;
        for (i = 0; i < k; i++) w[i] = cpu_only ? flt[i] : EQ_RAM[i];
        p_words(c, "slots", w, k);
        for (i = 0; i < 64u; i++) w[i] = flt[i];
        p_words(c, "flt", w, 64);
    }
    EQ[0] = 0;
    return 0;
}
HB_PROBE = {HB_PROBE_MAGIC, "HWEQ", run};
