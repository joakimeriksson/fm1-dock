/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Andrew GG — Roland S-1 parity engine */
/* S1Vave DSP building blocks, all fixed point (no FPU, no 64-bit division on the chip).
 * Oscillators: naive saw / pulse with polyBLEP corrections applied as discontinuity events, one sample late:
 * a jump of height H at fraction t before the current sample (t = 0..32767 ~ 0..1 sample) adds
 * H/2 * t^2 to the previous sample and takes H/2 * (1-t)^2 from the current one. A natural wrap and a
 * hard-sync reset are the same event, so one piece of code serves both (as the plan in CLAUDE.md). */
#include "obv_tables.h"

static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* tanh(x / 32768 * 2) * 32767, interpolated: x = +-32768 -> +-0.76 full scale, +-65536 -> +-0.96 */
static inline __attribute__((always_inline)) int32_t softclip(int32_t x)
{
    int32_t a = x < 0 ? -x : x, i = a >> 8, y;
    if (i >= 256)
        y = TANH_Q15[256];
    else
        y = TANH_Q15[i] + (((TANH_Q15[i + 1] - TANH_Q15[i]) * (a & 255)) >> 8);
    return x < 0 ? -y : y;
}

static inline uint32_t xorshift(uint32_t *st)
{
    uint32_t s = *st;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *st = s;
    return s;
}

/* Q8 semitones (0 = MIDI 0, 8.18 Hz) -> phase step per frame; 12 octave values x 256 fine steps */
static inline uint32_t pitch_inc(int32_t p)
{
    uint32_t n, o;
    p = clampi(p, 0, 131 * 256 + 255);
    n = (uint32_t)p >> 8;
    o = n / 12u;
    return (uint32_t)(((uint64_t)INC_C9[n - o * 12u] * FINE_Q16[p & 255]) >> (26u - o));
}

/* ---- oscillators */
typedef struct {
    uint32_t ph;                                /* phase, wraps */
    int32_t prev;                               /* the previous sample, still open to a correction */
} osc_t;

#define OSC_SAW 1u
#define OSC_PLS 2u

static inline __attribute__((always_inline)) int32_t osc_naive(uint32_t ph, uint32_t pw, uint32_t wave)   /* -98304 .. 98303 */
{
    int32_t v = 0;
    if (wave & OSC_SAW)
        v = (int32_t)(ph >> 16) - 32768;
    if (wave & OSC_PLS)                         /* saw(ph) - saw(ph + pw): a pulse with mean 0 */
        v += (int32_t)(ph >> 16) - (int32_t)((ph + pw) >> 16);
    return v;
}

/* the event: a jump of H (any size up to +-200000) at t (0..32767) before the current sample */
static inline __attribute__((always_inline)) void osc_event(int32_t *prev, int32_t *cur, int32_t h, int32_t t)
{
    int32_t u = 32767 - t, h16 = h >> 4;
    *prev += (h16 * ((t * t) >> 15)) >> 12;   /* H/2 * t^2: h16 = H/16, t^2 in Q15, so >> 12 */
    *cur -= (h16 * ((u * u) >> 15)) >> 12;
}

/* one sample: advance by inc (d = inc >> 15, hw = the height of a wrap of ph: -65536 per saw component,
 * both precomputed per block), return the delayed output; o->prev then holds this sample's naive value (the
 * X-MOD source). reset_t >= 0: a hard-sync reset to phase 0 happened reset_t before the current sample
 * (0..32767 = 0..1 sample). Inlined by force: it is the inner loop. */
static inline __attribute__((always_inline)) int32_t osc_step(osc_t *o, uint32_t inc, uint32_t d, uint32_t pw,
                                                              uint32_t wave, int32_t hw, int32_t reset_t)
{
    uint32_t old = o->ph, ph = old + inc;
    int32_t cur, out = o->prev;
    if (reset_t < 0 || !d) {
        cur = osc_naive(ph, pw, wave);
        if (ph < inc && d)                      /* wrapped: the saw(s) drop */
            osc_event(&out, &cur, hw, (int32_t)(ph / d));
        if ((wave & OSC_PLS) && ph + pw < inc && d)   /* the subtracted saw wraps: the pulse rises */
            osc_event(&out, &cur, 65536, (int32_t)((ph + pw) / d));
    } else {
        uint32_t at = ph - d * (uint32_t)reset_t;   /* the phase at the reset instant */
        int32_t t;
        ph = d * (uint32_t)reset_t;             /* reset, then reset_t on */
        cur = osc_naive(ph, pw, wave);
        if (at < old) {                         /* ph wrapped on the way to the reset */
            t = (int32_t)(at / d) + reset_t;
            osc_event(&out, &cur, hw, t > 32767 ? 32767 : t);
        }
        if ((wave & OSC_PLS) && at + pw < old + pw) {   /* ph + pw wrapped on the way to the reset */
            t = (int32_t)((at + pw) / d) + reset_t;
            osc_event(&out, &cur, 65536, t > 32767 ? 32767 : t);
        }
        osc_event(&out, &cur, osc_naive(0, pw, wave) - osc_naive(at, pw, wave), reset_t);
    }
    o->ph = ph;
    o->prev = cur;
    return out;
}

/* ---- trapezoidal state-variable filter (A. Simper), coefficients per block in Q13 */
typedef struct { int32_t a1, a2, a3, k; } svf_coef_t;
typedef struct { int32_t ic1, ic2; } svf_state_t;

/* cut: Q8 semitones (0..136 << 8); k: damping Q12 (2.0 = 8192; ~0 self-oscillates) */
static inline void svf_coef(svf_coef_t *c, int32_t cut, int32_t k)
{
    int32_t i, g, den;
    cut = clampi(cut, 0, 136 << 8);
    i = cut >> 8;
    g = SVF_G[i];
    if (i < 136)
        g += ((SVF_G[i + 1] - g) * (cut & 255)) >> 8;
    den = 4096 + ((g * (g + k)) >> 12);
    if (den < 64) den = 64;
    c->a1 = (int32_t)((4096u << 13) / (uint32_t)den);
    c->a2 = (c->a1 * g) >> 12;
    c->a3 = (c->a2 * g) >> 12;
    c->k = k;
}

/* one sample; *lp, *bp, *hp out. The band-pass state passes a soft saturation: resonance stays warm and bounded
 * (self-oscillation settles instead of running away). Signals within +-150000 keep the products in 32 bits. */
static inline __attribute__((always_inline)) void svf_step(const svf_coef_t *c, svf_state_t *s, int32_t in, int32_t *lp, int32_t *bp, int32_t *hp)
{
    int32_t v3 = in - s->ic2;
    int32_t v1 = (c->a1 * s->ic1 + c->a2 * v3) >> 13;
    int32_t v2 = s->ic2 + ((c->a2 * s->ic1 + c->a3 * v3) >> 13);
    int32_t n1 = 2 * v1 - s->ic1;
    s->ic1 = (uint32_t)(n1 + 24576) < 49152u ? n1 : softclip(n1 >> 1) << 1;
    s->ic2 = 2 * v2 - s->ic2;
    *lp = v2;
    *bp = v1;
    *hp = in - ((c->k * v1) >> 12) - v2;
}

static inline __attribute__((always_inline)) int32_t svf_lp(const svf_coef_t *c, svf_state_t *s, int32_t in)   /* the plain stage (24 dB's second) */
{
    int32_t v3 = in - s->ic2;
    int32_t v1 = (c->a1 * s->ic1 + c->a2 * v3) >> 13;
    int32_t v2 = s->ic2 + ((c->a2 * s->ic1 + c->a3 * v3) >> 13);
    int32_t n1 = 2 * v1 - s->ic1;
    s->ic1 = (uint32_t)(n1 + 24576) < 49152u ? n1 : softclip(n1 >> 1) << 1;
    s->ic2 = 2 * v2 - s->ic2;
    return v2;
}

/* ---- Roland S-1 Extensions: OSC DRAW, OSC CHOP, Pink Noise */

/* OSC DRAW: 16-step user wavetable lookup with Multiply (1.0x to 32.0x in Q8) and Step/Slope interpolation */
enum { DRAW_OFF, DRAW_STEP, DRAW_SLOPE };

static inline __attribute__((always_inline)) int32_t osc_draw_sample(const int16_t *draw_table, uint32_t ph, uint32_t mult_q8, uint32_t mode)
{
    uint32_t eff_ph = (uint32_t)(((uint64_t)ph * mult_q8) >> 8);
    uint32_t idx = (eff_ph >> 28) & 15u;
    if (mode == DRAW_STEP) {
        /* Soften the step edge to prevent wideband aliasing clicks */
        uint32_t sub = (eff_ph >> 20) & 255u; /* 0..255 within slice */
        if (sub < 32u) {
            uint32_t prev_idx = (idx + 15u) & 15u;
            int32_t v_prev = draw_table[prev_idx];
            int32_t v_curr = draw_table[idx];
            uint32_t t = sub << 3; /* 0..248 (Q8) */
            return v_prev + (int32_t)(((int64_t)(v_curr - v_prev) * t) >> 8);
        }
        return (int32_t)draw_table[idx];
    } else { /* DRAW_SLOPE */
        uint32_t next_idx = (idx + 1u) & 15u;
        uint32_t frac = (eff_ph >> 12) & 0xFFFFu; /* Q16 */
        /* Cubic smoothstep interpolation S(t) = 3t^2 - 2t^3 to round sharp vertex corners */
        int32_t t = (int32_t)(frac >> 1); /* Q15: 0..32767 */
        int32_t t2 = (t * t) >> 15;       /* Q15 */
        int32_t smooth_t = (int32_t)(((int64_t)t2 * (98304 - 2 * t)) >> 16); /* Q15: 0..32768 */
        int32_t v0 = draw_table[idx];
        int32_t v1 = draw_table[next_idx];
        return v0 + (int32_t)(((int64_t)(v1 - v0) * smooth_t) >> 15);
    }
}

/* OSC CHOP: Overtone gain calculation and 16-step bitmask slicing */
static inline int32_t chop_calc_masked_gain(uint32_t ovtn)
{
    if (ovtn == 0) return 32767;
    if (ovtn <= 100u) {
        int32_t amt = (int32_t)ovtn * 327 + ((int32_t)ovtn * 67) / 100;
        return 32767 - amt;
    } else {
        uint32_t diff = ovtn - 100u;
        int32_t amt = (int32_t)diff * 327 + ((int32_t)diff * 67) / 100;
        return -amt;
    }
}

static inline __attribute__((always_inline)) int32_t osc_chop_sample(int32_t sample, uint32_t ph, uint16_t mask, uint32_t comb_q8, int32_t g_masked)
{
    if (g_masked == 32767) return sample; /* Dry / passthrough */
    uint32_t chop_ph = (uint32_t)(((uint64_t)ph * comb_q8) >> 8);
    uint32_t slice = (chop_ph >> 28) & 15u;
    uint32_t next_slice = (slice + 1u) & 15u;
    int32_t g_curr = (mask & (1u << slice)) ? 32767 : g_masked;
    int32_t g_next = (mask & (1u << next_slice)) ? 32767 : g_masked;
    uint32_t pos = (chop_ph >> 20) & 255u;
    int32_t g = g_curr;
    if (g_curr != g_next && pos >= 224u) {
        /* Soft linear ramp over the final 32/256 of the slice */
        uint32_t t = (pos - 224u) << 3; /* 0..248 in Q8 */
        g = g_curr + (int32_t)(((int64_t)(g_next - g_curr) * t) >> 8);
    }
    return (int32_t)(((int64_t)sample * g) >> 15);
}

/* Pink Noise Generator: 3-pole Gardner IIR filter + 2-pole Roland analog RC warmer */
typedef struct {
    int32_t b0, b1, b2;
    int32_t lp1, lp2;
} pink_noise_t;

static inline __attribute__((always_inline)) int32_t pink_noise_step(pink_noise_t *pn, int32_t white)
{
    /* Clean Gardner pinking poles scaled to guarantee zero internal clipping */
    pn->b0 = (int32_t)(((int64_t)pn->b0 * 32691 + (int64_t)white * 464) >> 15);
    pn->b1 = (int32_t)(((int64_t)pn->b1 * 31555 + (int64_t)white * 1400) >> 15);
    pn->b2 = (int32_t)(((int64_t)pn->b2 * 18677 + (int64_t)white * 4400) >> 15);
    int32_t out = pn->b0 + pn->b1 + pn->b2;
    /* Gentle 2-pole lowpass matching Roland S-1 / SH-101 analog transistor RC roll-off */
    pn->lp1 += (int32_t)(((int64_t)(out - pn->lp1) * 24000) >> 15);
    pn->lp2 += (int32_t)(((int64_t)(pn->lp1 - pn->lp2) * 28000) >> 15);
    return pn->lp2;
}

