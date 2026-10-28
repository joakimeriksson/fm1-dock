/* SPDX-License-Identifier: GPL-3.0-only */
/* HWSRC: the AC79's sample-rate converter (JL_SRC, 0x14300: CON0..3, IDAT_ADR, IDAT_LEN, ODAT_ADR,
 * ODAT_LEN, FLTB_ADR), one run (or a few in a row) of it, programmed as JieLi's SDK driver does (cpu.a
 * src.c.o, src_resample; fm1-emu's tools/sdk_mmio.py lists it): fm1-emu's tools/hwdsp.py drives this.
 *
 * The driver: CON0 = channels - 1 | phase << 8 (17 bits) | history offset << 26 (5 bits), bit 3 starts,
 * bit 7 reads done, bit 6 clears it; CON1 = out rate << 16 | in rate; CON2 = in channel step << 16 | in
 * sample step, CON3 the same for the output (in samples: the FM-1); IDAT / ODAT_LEN samples a channel.
 * FLTB_ADR: the filter's history, 48 halfwords (src_fltb_t), even. When done, CON0 holds the phase and
 * offset to go on from and IDAT / ODAT_LEN what is left of each (the FM-1). 16-bit samples.
 *
 * argument (u32s): op, then
 *   op 0  the registers
 *   op 1  channels | interleaved << 8 | calls << 12 (runs in a row, each going on from the last; the
 *         input split evenly), in rate, out rate, in samples a channel, out room a channel, gen, amp,
 *         seed, phase0, offset0, out_n (halfwords of output to return), out_off (from where)
 * gen as probes/hwdsp.c's, a sample at a time over all channels in their layout.
 * The result: regs before; for each call con_done, con (after the clear), ilen / olen after; out, hist
 * (the first 48 halfwords after), ticks. The history has room for 8 channels: the chip keeps channel k's
 * at FLTB_ADR + 48 k (the SDK's src_fltb_t only for two). */
#include "probe.h"

#define SRC ((volatile uint32_t *)0x14300u)
#define POLLS 2000000u
#define MAXS 16000u

static int16_t in_b[MAXS] __attribute__((aligned(16)));
static int16_t out_b[MAXS + 64] __attribute__((aligned(16)));
static int16_t hist[24 * 8 + 16] __attribute__((aligned(16)));   /* (channel k's at + 48 k bytes: 8 of them) */

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

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t op = p_arg32(c, 0, 0), i, r[9];
    for (i = 0; i < 9u; i++) r[i] = SRC[i];
    p_words(c, "regs0", r, 9);
    if (op != 1)
        return 0;
    uint32_t f = p_arg32(c, 1, 1), ch = f & 15u, inter = (f >> 8) & 1u, calls = (f >> 12) & 15u;
    uint32_t in_rate = p_arg32(c, 2, 48000), out_rate = p_arg32(c, 3, 48000);
    uint32_t nin = p_arg32(c, 4, 64), nout = p_arg32(c, 5, 64);
    uint32_t gen = p_arg32(c, 6, 0), seed = p_arg32(c, 8, 0), x = seed ? seed : 1u;
    int32_t amp = (int32_t)p_arg32(c, 7, 0);
    uint32_t phase = p_arg32(c, 9, 0), offset = p_arg32(c, 10, 0), out_n = p_arg32(c, 11, 0);
    uint32_t out_off = p_arg32(c, 12, 0);
    if (!ch) ch = 1;
    if (ch > 8u) ch = 8u;                       /* (the history has room for 8) */
    if (!calls) calls = 1;
    if (nin * ch > MAXS) nin = MAXS / ch;
    if (nout * ch > MAXS) nout = MAXS / ch;
    for (i = 0; i < nin * ch; i++) in_b[i] = (int16_t)gen1(gen, amp, seed, i, &x);
    for (i = 0; i < MAXS + 64u; i++) out_b[i] = 0x5A5A;
    for (i = 0; i < 24u * 8u + 16u; i++) hist[i] = 0;

    /* layout: interleaved, a frame after the other; else block, a channel after the other */
    uint32_t in_sp = inter ? ch : 1u, in_ch = inter ? 1u : nin;      /* (in samples, not bytes: the FM-1) */
    uint32_t out_sp = inter ? ch : 1u, out_ch = inter ? 1u : nout;
    uint32_t in_at = 0, out_at = 0, ticks = 0, k;
    uint32_t cd[16], ca[16], il[16], ol[16];
    for (k = 0; k < calls; k++) {
        uint32_t take = k + 1u < calls ? nin / calls : nin - in_at;
        uint32_t t0, polls = 0, done;
        p_irq_off();
        SRC[0] = 0;
        SRC[8] = (uint32_t)hist;
        SRC[0] |= phase << 8;
        SRC[0] |= offset << 26;
        SRC[0] |= ch - 1u;
        SRC[1] = (out_rate << 16) | in_rate;
        SRC[2] = (in_ch << 16) | in_sp;
        SRC[3] = (out_ch << 16) | out_sp;
        SRC[7] = nout - out_at;
        SRC[6] = (uint32_t)(out_b + (inter ? out_at * ch : out_at));
        SRC[5] = take;
        SRC[4] = (uint32_t)(in_b + (inter ? in_at * ch : in_at));
        SRC[0] |= 1u << 6;
        t0 = p_ticks();
        SRC[0] |= 1u << 3;
        while (!((done = SRC[0]) & (1u << 7)) && ++polls < POLLS)
            ;
        ticks += p_ticks() - t0;
        cd[k] = done;
        offset = (SRC[0] >> 26) & 31u;
        phase = (SRC[0] >> 8) & 0x1FFFFu;
        il[k] = SRC[5];
        ol[k] = SRC[7];
        in_at += take - il[k];                  /* (they read back what is left) */
        out_at = nout - ol[k];
        SRC[0] |= 1u << 6;
        ca[k] = SRC[0];
        SRC[0] = 0;
        p_irq_on();
        if (polls >= POLLS)
            break;
    }
    p_words(c, "con_done", cd, k < calls ? k + 1u : calls);
    p_words(c, "con_after", ca, k < calls ? k + 1u : calls);
    p_words(c, "ilen", il, k < calls ? k + 1u : calls);
    p_words(c, "olen", ol, k < calls ? k + 1u : calls);
    p_u32(c, "ticks", ticks);
    if (out_off > MAXS) out_off = MAXS;
    if (out_n > MAXS - out_off) out_n = MAXS - out_off;
    if (out_n > 3000u) out_n = 3000u;
    p_bytes(c, "out", out_b + out_off, 2u * out_n);
    p_bytes(c, "hist", hist, 96);
    return 0;
}
HB_PROBE = {HB_PROBE_MAGIC, "HWSRC", run};
