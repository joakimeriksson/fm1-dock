/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The organ's reverb and Leslie, after setBfree (b_reverb/reverb.c, b_whirl/whirl.c), in integers. Included by
 * organ_core.h. The tables come from tools/gen_leslie.py (organ_leslie.h).
 *
 * The chain is setBfree's: tone generator -> overdrive -> reverb -> Leslie. Both work on one block of mono
 * samples in Q15 (reverb in place, the Leslie makes the stereo pair). Inside they use Q19 (4 bits more, so that
 * the long chain of filters and delay lines does not turn into noise).
 *
 * The big delay lines go in the pool (ORGAN_POOL: the firmware puts them there; zeroed at boot). */
#include "organ_leslie.h"

#ifndef ORGAN_POOL
#define ORGAN_POOL
#endif

/* ---- reverb: four feedback combs in parallel, three all-pass in series, a low-pass, a little feedback */
static int32_t rv_dl[RV_TOTAL] ORGAN_POOL;
static uint32_t rv_ix[7];                       /* where each line is now (0 .. RV_LEN - 1) */
static int32_t rv_fb, rv_lp;                    /* feedback to the input, the low-pass's memory */

#define RV_G 23170                              /* 1 / sqrt(2), Q15: every comb and all-pass */
#define RV_IN 3277                              /* the input gain 0.1, Q15 */
#define RV_FBK (-492)                           /* the feedback -0.015, Q15 */

static void reverb_run(int32_t *x, uint32_t n, uint32_t mix)   /* mix 0..127: setBfree's reverb.mix (wet = mix, dry = 1 - mix) */
{
    int32_t wet = (int32_t)(mix * 258u), dry = 32768 - wet;
    uint32_t i, j;
    for (i = 0; i < n; i++) {
        int32_t xo = x[i] << 4, xin = rv_fb + (int32_t)(((int64_t)xo * RV_IN) >> 15), xa = 0, y;
        for (j = 0; j < 4u; j++) {              /* combs: y out, x + g y in */
            int32_t *p = &rv_dl[RV_OFF[j] + rv_ix[j]];
            y = *p;
            *p = xin + (int32_t)(((int64_t)y * RV_G) >> 15);
            if (++rv_ix[j] >= RV_LEN[j]) rv_ix[j] = 0;
            xa += y;
        }
        for (; j < 7u; j++) {                   /* all-pass: g (xa + y) in, y - xa on */
            int32_t *p = &rv_dl[RV_OFF[j] + rv_ix[j]];
            y = *p;
            *p = (int32_t)(((int64_t)(xa + y) * RV_G) >> 15);
            if (++rv_ix[j] >= RV_LEN[j]) rv_ix[j] = 0;
            xa = y - xa;
        }
        y = (xa + rv_lp) >> 1;
        rv_lp = y;
        rv_fb = (int32_t)(((int64_t)xa * RV_FBK) >> 15);
        x[i] = (int32_t)(((int64_t)wet * y + (int64_t)dry * xo) >> 19);   /* Q19 -> Q15 */
    }
}

/* ---- Leslie: horn and drum turn, each sound reaches the left and the right microphone on several paths
 * (direct, first reflection, second reflection), delayed by the rotor's distance to the microphone (the Doppler
 * shift) and, for the horn, filtered by its angle (it is a beam). The drum gets the whole signal, the horn the
 * part above about 300 Hz, the drum's output is low-passed (setBfree). */
#ifndef LES_REFL2
#define LES_REFL2 1                             /* second reflections as well (needed to match setBfree; 0 saves about 30 % of the Leslie) */
#endif
#define LS_N 512u                               /* the delay lines to the microphones, in samples */
#define LS_M (LS_N - 1u)

typedef struct { int32_t x1, x2, y1, y2; } bq_t; /* y in Q27 */

static int32_t ls_hl[LS_N] ORGAN_POOL, ls_hr[LS_N] ORGAN_POOL, ls_dl[LS_N] ORGAN_POOL, ls_dr[LS_N] ORGAN_POOL;   /* Q19 */
static uint32_t ls_pos, ls_hph, ls_dph, ls_hinc, ls_dinc;   /* output position; the angles (of 2^32) and their speed per frame */
static int32_t ls_h0[4], ls_h1[4], ls_h2[4];    /* history of the three horn signals: the last four samples, Q14 */
static int32_t ls_z[4];                         /* the reflection filters (0.4 x + 0.4 z) */
static bq_t ls_ha, ls_hb, ls_dfl, ls_dfr;       /* horn filters A and B; the drum's, left and right */

static int32_t bq_run(bq_t *s, const int32_t *c, int32_t x)   /* Q19 in, Q19 out */
{
    int64_t a = ((int64_t)c[0] * x + (int64_t)c[1] * s->x1 + (int64_t)c[2] * s->x2) * 256 - (int64_t)c[3] * s->y1 - (int64_t)c[4] * s->y2;
    int32_t yh = (int32_t)(a >> 28);
    s->x2 = s->x1; s->x1 = x; s->y2 = s->y1; s->y1 = yh;
    return yh >> 8;
}

/* the sound x lands in a delay line, where the microphone will read it t / 16 samples from now (Q4 samples) */
static void ls_put(int32_t *buf, int32_t x, int32_t t)
{
    uint32_t r = ls_pos + (uint32_t)(t >> 4);
    int32_t q = (x * (t & 15)) >> 4;
    buf[r & LS_M] += x - q;
    buf[(r + 1u) & LS_M] += q;
}

/* one horn path: the delay by the angle, the angle-dependent filter of the last five samples */
static void ls_horn(int32_t *buf, uint32_t ph, uint32_t sp, const uint16_t *disp, const int16_t *bw, int32_t xq, const int32_t *h)
{
    uint32_t ix = ph >> 22, fr = (ph >> 6) & 0xFFFFu, k = ((ph + (1u << 21)) >> 22) & 1023u;
    int32_t d0 = disp[ix], d1 = disp[(ix + 1u) & 1023u], xa;
    const int16_t *b = bw + 5u * k;
    xa = (b[0] * xq + b[1] * h[0] + b[2] * h[1] + b[3] * h[2] + b[4] * h[3]) >> 10;   /* Q15 x Q14 -> Q19 */
    ls_put(buf, xa, (int32_t)sp + d0 + (((d1 - d0) * (int32_t)fr) >> 16));
}

static void ls_drum(int32_t *buf, uint32_t ph, uint32_t sp, const uint16_t *disp, int32_t x)
{
    uint32_t ix = ph >> 22, fr = (ph >> 6) & 0xFFFFu;
    int32_t d0 = disp[ix], d1 = disp[(ix + 1u) & 1023u];
    ls_put(buf, x, (int32_t)sp + d0 + (((d1 - d0) * (int32_t)fr) >> 16));
}

static void ls_hist(int32_t *h, int32_t xq)
{
    h[3] = h[2]; h[2] = h[1]; h[1] = h[0]; h[0] = xq;
}

static uint32_t ls_speed(uint32_t inc, uint32_t target, int32_t acc, int32_t dec)   /* one block of the speed's slow change */
{
    int32_t diff = (int32_t)target - (int32_t)inc, step;
    if (diff > -81 && diff < 81) return target; /* 0.05 rpm: arrived */
    step = (int32_t)(((int64_t)diff * (diff > 0 ? acc : dec) + 32768) >> 16);
    if (!step) step = diff > 0 ? 1 : -1;
    return (uint32_t)((int32_t)inc + step);
}

static int32_t ls_refl(uint32_t k, int32_t x)     /* the reflection filters: 0.4 x + 0.4 (the last x) */
{
    int32_t y = (int32_t)(((int64_t)x * 13107 + (int64_t)ls_z[k] * 13107) >> 15);
    ls_z[k] = x;
    return y;
}

static void leslie_run(const int32_t *in, int32_t *outl, int32_t *outr, uint32_t n, uint32_t speed)   /* speed 0 stop, 1 slow, 2 fast */
{
    uint32_t i;
    ls_hinc = ls_speed(ls_hinc, speed == 2u ? LES_HORN_FAST : speed == 1u ? LES_HORN_SLOW : 0u, LES_HORN_ACC, LES_HORN_DEC);
    ls_dinc = ls_speed(ls_dinc, speed == 2u ? LES_DRUM_FAST : speed == 1u ? LES_DRUM_SLOW : 0u, LES_DRUM_ACC, LES_DRUM_DEC);
    for (i = 0; i < n; i++) {
        int32_t x = in[i] << 4, xh, xq, leak, l, r, y;
        uint32_t o = ls_pos;
        /* the horn: its filters, the direct sound to the left and right microphone, then the reflections */
        xh = bq_run(&ls_hb, LES_HB, bq_run(&ls_ha, LES_HA, x));
        leak = (int32_t)(((int64_t)xh * LES_LEAK) >> 15);
        xq = xh >> 5;
        ls_horn(ls_hl, ls_hph + LES_HPH[0], LES_HSP[0], LES_HN_FWD, LES_BBW, xq, ls_h0);
        ls_horn(ls_hr, ls_hph + LES_HPH[1], LES_HSP[1], LES_HN_BWD, LES_BFW, xq, ls_h0);
        ls_hist(ls_h0, xq);
        xh = ls_refl(0, xh);
        xq = xh >> 5;
        ls_horn(ls_hl, ls_hph + LES_HPH[2], LES_HSP[2], LES_HN_BWD, LES_BFW, xq, ls_h1);
        ls_horn(ls_hr, ls_hph + LES_HPH[3], LES_HSP[3], LES_HN_FWD, LES_BBW, xq, ls_h1);
        ls_hist(ls_h1, xq);
#if LES_REFL2
        xh = ls_refl(1, xh);
        xq = xh >> 5;
        ls_horn(ls_hl, ls_hph + LES_HPH[4], LES_HSP[4], LES_HN_FWD, LES_BBW, xq, ls_h2);
        ls_horn(ls_hr, ls_hph + LES_HPH[5], LES_HSP[5], LES_HN_BWD, LES_BFW, xq, ls_h2);
        ls_hist(ls_h2, xq);
#endif
        /* the drum: the whole signal, only delayed */
        ls_drum(ls_dl, ls_dph + LES_DPH[0], LES_DSP[0], LES_DR_FWD, x);
        ls_drum(ls_dr, ls_dph + LES_DPH[1], LES_DSP[1], LES_DR_BWD, x);
        x = ls_refl(2, x);
        ls_drum(ls_dl, ls_dph + LES_DPH[2], LES_DSP[2], LES_DR_BWD, x);
        ls_drum(ls_dr, ls_dph + LES_DPH[3], LES_DSP[3], LES_DR_FWD, x);
#if LES_REFL2
        x = ls_refl(3, x);
        ls_drum(ls_dl, ls_dph + LES_DPH[4], LES_DSP[4], LES_DR_FWD, x);
        ls_drum(ls_dr, ls_dph + LES_DPH[5], LES_DSP[5], LES_DR_BWD, x);
#endif
        /* what reaches the microphones now: the drum through its low-pass, the horn, the leak */
        y = bq_run(&ls_dfl, LES_DF, ls_dl[o]);
        l = y + (int32_t)(((int64_t)ls_hl[o] * LES_HORN_LEVEL) >> 15) + leak;
        y = bq_run(&ls_dfr, LES_DF, ls_dr[o]);
        r = y + (int32_t)(((int64_t)ls_hr[o] * LES_HORN_LEVEL) >> 15) + leak;
        ls_hl[o] = ls_hr[o] = ls_dl[o] = ls_dr[o] = 0;
        ls_pos = (o + 1u) & LS_M;
        ls_hph += ls_hinc;
        ls_dph += ls_dinc;
        outl[i] = l >> 4;                       /* Q19 -> Q15 */
        outr[i] = r >> 4;
    }
}
