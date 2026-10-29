/* SPDX-License-Identifier: GPL-3.0-only */
/* HWDSP: the AC79's FFT / vector unit (JL_FFT, 0x1EE0400: CON, CADR, CONST, TEST1), one job a run, on
 * inputs made from a seed so the computer can make them again (fm1-emu's tools/hwdsp.py drives it and
 * keeps what comes back as the emulator's reference).
 *
 * The SDK's driver (apps/common/jl_math/jl_fft.c): a job is a descriptor in RAM, CADR its address;
 * CON = (nbatch << 8) | (1 << 6) clear | (vector << 3) | (ie << 2) | (1 << 0) en starts it; bit 7 is
 * done, written 1 to bit 6 clears it. FFT descriptor {con, iadr, oadr, 0}; the vector one {con, xadr,
 * yadr, zadr, config0 = q << 24 | nloop << 12 | nlen, config1 = z_inc << 20 | y_inc << 10 | x_inc,
 * config2, 0}, and CONST its constant.
 *
 * argument (u32s): op, then
 *   op 0  the registers as they are (no job)
 *   op 1  FFT:    fft_con, gen, amp, p, out_off, out_n          (gen: the input, below; in 2N words)
 *   op 2  vector: vector_con, config0, config1, config2, const, xgen, xamp, xp, ygen, yamp, yp,
 *                 zgen, zamp, zp, out_off, out_n                (x, y, z: 2048 words each)
 * gen 0 zeros; 1 an impulse of amp at word p; 2 amp everywhere; 3 xorshift32 from seed p, each word
 * (x >> 1) % (2 amp + 1) - amp; 4 the word index times amp, plus p; 5 xorshift32 from p, raw.
 * The result: con0 / cadr0 / const0 / test1_0 (before), con / con_done (after, before the clear),
 * polls (the done wait, 0: timed out), ticks (TIMER4, 24 MHz, the job alone), out (out_n words from
 * out_off of the output: the FFT's oadr, the vector's z), and for the vector x and y's first words
 * again (did it write them). */
#include "probe.h"

#define FFT_CON   (*(volatile uint32_t *)0x1EE0400u)
#define FFT_CADR  (*(volatile uint32_t *)0x1EE0404u)
#define FFT_CONST (*(volatile uint32_t *)0x1EE0408u)
#define FFT_TEST1 (*(volatile uint32_t *)0x1EE040Cu)
#define WORDS 2048u
#define POLLS 2000000u

static int32_t xb[WORDS + 8] __attribute__((aligned(16)));
static int32_t yb[WORDS + 8] __attribute__((aligned(16)));
static int32_t zb[WORDS + 8] __attribute__((aligned(16)));
static uint32_t desc[8] __attribute__((aligned(16)));

static void fill(int32_t *b, uint32_t gen, int32_t amp, uint32_t p)
{
    uint32_t i, x = p ? p : 1u;
    for (i = 0; i < WORDS + 8u; i++) {
        int32_t v = 0;
        switch (gen) {
        case 1: v = i == p ? amp : 0; break;
        case 2: v = amp; break;
        case 3: x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                v = amp ? (int32_t)((x >> 1) % (2u * (uint32_t)amp + 1u)) - amp : 0; break;
        case 4: v = (int32_t)i * amp + (int32_t)p; break;
        case 5: x ^= x << 13; x ^= x >> 17; x ^= x << 5; v = (int32_t)x; break;
        default: break;
        }
        b[i] = v;
    }
}

static void regs(hb_probe_ctx_t *c)
{
    p_hex(c, "con0", FFT_CON);
    p_hex(c, "cadr0", FFT_CADR);
    p_hex(c, "const0", FFT_CONST);
    p_hex(c, "test1_0", FFT_TEST1);
}

/* start a job (CON as the driver writes it), wait for done, clear */
static void job(hb_probe_ctx_t *c, uint32_t vector)
{
    uint32_t t0, t1, n = 0, con, done;
    p_irq_off();
    FFT_CADR = (uint32_t)desc;
    t0 = p_ticks();
    FFT_CON = (1u << 8) | (1u << 6) | (vector << 3) | (1u << 0);
    while (!((done = FFT_CON) & (1u << 7)) && ++n < POLLS)
        ;
    t1 = p_ticks();
    con = FFT_CON;
    FFT_CON |= 1u << 6;
    if (n >= POLLS)
        FFT_CON = 0;                            /* (stuck: stop it, so it does not go on writing) */
    p_irq_on();
    p_hex(c, "con_done", done);
    p_hex(c, "con", con);
    p_hex(c, "con_cleared", FFT_CON);
    p_u32(c, "polls", n < POLLS ? n + 1u : 0u);
    p_u32(c, "ticks", t1 - t0);
}

static void out(hb_probe_ctx_t *c, const int32_t *b, uint32_t off, uint32_t n)
{
    if (off > WORDS + 8u) off = WORDS + 8u;
    if (n > WORDS + 8u - off) n = WORDS + 8u - off;
    if (n > 1900u) n = 1900u;                   /* (8 KiB of output) */
    p_words(c, "out", (const uint32_t *)(b + off), n);
}

static int32_t run(hb_probe_ctx_t *c)
{
    uint32_t op = p_arg32(c, 0, 0);
    regs(c);
    if (op == 1) {                              /* FFT */
        uint32_t con = p_arg32(c, 1, 0), nfft = (con >> 16) & 0xFFFu;
        /* (complex: 2N words in and out; real: N + 2: within the buffers) */
        if ((con & 2u) ? nfft + 2u > WORDS + 8u : 2u * nfft > WORDS + 8u) {
            p_text(c, "error", "N over the buffers");
            return -1;
        }
        fill(xb, p_arg32(c, 2, 0), (int32_t)p_arg32(c, 3, 0), p_arg32(c, 4, 0));
        fill(zb, 0, 0, 0);
        desc[0] = con;
        desc[1] = (uint32_t)xb;
        desc[2] = (con & 1u) ? (uint32_t)xb : (uint32_t)zb;   /* FFT_SAME_ADR: in place */
        desc[3] = 0;
        job(c, 0);
        out(c, (con & 1u) ? xb : zb, p_arg32(c, 5, 0), p_arg32(c, 6, 0));
    } else if (op == 2) {                       /* vector */
        uint32_t con = p_arg32(c, 1, 0), c0 = p_arg32(c, 2, 0), c1 = p_arg32(c, 3, 0);
        uint32_t nlen = c0 & 0xFFFu, nloop = (c0 >> 12) & 0xFFFu, k, ok = 1;
        if (!nloop) nloop = 1;
        /* (each of x, y, z: the last loop's start (increments in bytes) plus nlen elements, two a
         * complex one, at its width, within its buffer) */
        for (k = 0; k < 3u; k++) {
            uint32_t w = (con >> (6u + 2u * k)) & 3u, inc = (c1 >> (10u * k)) & 0x3FFu;
            if ((nloop - 1u) * inc + 2u * nlen * (1u << w) > 4u * (WORDS + 8u)) ok = 0;
        }
        if (!ok) {
            p_text(c, "error", "nlen / nloop / increments over the buffers");
            return -1;
        }
        fill(xb, p_arg32(c, 6, 0), (int32_t)p_arg32(c, 7, 0), p_arg32(c, 8, 0));
        fill(yb, p_arg32(c, 9, 0), (int32_t)p_arg32(c, 10, 0), p_arg32(c, 11, 0));
        fill(zb, p_arg32(c, 12, 0), (int32_t)p_arg32(c, 13, 0), p_arg32(c, 14, 0));
        desc[0] = p_arg32(c, 1, 0);
        desc[1] = (uint32_t)xb;
        desc[2] = (uint32_t)yb;
        desc[3] = (uint32_t)zb;
        desc[4] = p_arg32(c, 2, 0);
        desc[5] = p_arg32(c, 3, 0);
        desc[6] = p_arg32(c, 4, 0);
        desc[7] = 0;
        FFT_CONST = p_arg32(c, 5, 0);
        job(c, 1);
        out(c, zb, p_arg32(c, 15, 0), p_arg32(c, 16, 0));
        p_words(c, "x_head", (const uint32_t *)xb, 4);
        p_words(c, "y_head", (const uint32_t *)yb, 4);
    }
    return 0;
}
HB_PROBE = {HB_PROBE_MAGIC, "HWDSP", run};
