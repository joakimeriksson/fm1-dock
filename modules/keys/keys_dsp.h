/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Joakim Eriksson */
/* KEYS' three instruments, one voice each, in float (the FPU: module.cfg), at the host's 44117.6 Hz:
 *   CLAV   a Karplus-Strong string after Plaits' (keys_mi.h: its excitation, one period of filtered noise): a delay
 *          line, a Thiran all-pass for the fraction, two first-order all-passes for the stiffness and a one-pole
 *          loss; the two pickups as combs on the string (taps at their places), a resonant SVF (low-pass, or
 *          band-pass swept by an envelope: the wah); the key let go lowers the yarn damper (fast, dark decay).
 *   E.PIANO  modal: eight decaying sines (the tine's and the tone bar's modes) struck by a hammer whose contact
 *          time falls with the velocity (the higher modes get more of a hard hit), summed as the tine's
 *          displacement into a magnetic pickup's curve (a soft clip, off centre: the bark grows with the
 *          amplitude), its change (the coil's voltage) and a tone low-pass; the key let go damps the modes.
 *   PIANO  two strings a few cents apart, each a delay line with a Thiran fraction, up to four first-order all-passes
 *          for the dispersion (their coefficient found at the note-on so a high partial sits where the key's
 *          stiffness B puts it) and a one-pole loss. The two meet at the bridge, which moves with their sum and
 *          takes it from each: what they do together dies in seconds, what they do against each other rings for
 *          half a minute (a piano's two decays). A felt hammer strikes them a seventh along: a mass on a spring
 *          that stiffens as it is pressed (F = K u^p, with the felt's hysteresis), run against the string's own
 *          motion while it touches, so a hard key is brighter by itself and a string still ringing is struck as
 *          it moves. The string, bridge and hammer values by key are those of C. Otey's waveguide piano
 *          (large.stanford.edu/courses/2007/ph210/otey2) as S. Takano's pico_piano (MIT) re-implements them; the
 *          code here is KEYS' own single-loop string. What is heard is the bridge's velocity; the board's
 *          radiation and its resonances are the part's, after the voices (keys_module.c).
 * Each voice is tuned at its note-on: the delay is what is left of the period after the phase delays of the loop's
 * filters at the fundamental. No libm: exp2, sin, atan and sqrt below. A string's delay line is KD_LEN floats; a
 * note below SR / KD_LEN plays an octave up. */
#ifndef KEYS_DSP_H
#define KEYS_DSP_H

#include <stdint.h>
#include "keys_mi.h"

#define KD_SR 44117.647f
#define KD_LEN 1024u
#define KD_MASK (KD_LEN - 1u)
#define KD_CTL 32u                              /* frames between control updates (envelopes, filters) */
#define KD_NM 8                                 /* E.PIANO's modes */
#define KD_NAP 4                                /* PIANO's dispersion all-passes */

enum { FAM_CLAV, FAM_EP, FAM_PIANO, NFAM };

/* ---- maths (note-on only, but for kd_exp2 in the control updates) */
static inline float kd_exp2(float x)            /* 2^x, x in -126..126, ~2e-5 */
{
    union { float f; int32_t i; } u;
    int32_t i = (int32_t)x;
    float f;
    if (x < (float)i)
        i--;
    f = x - (float)i;
    u.i = (i + 127) << 23;
    return u.f * (1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f +
                  f * (0.0013334f + f * 0.0001540f))))));
}
static inline float kd_sin(float x)             /* x in -pi..pi */
{
    float x2;
    if (x > 0.5f * KM_PI) x = KM_PI - x;
    else if (x < -0.5f * KM_PI) x = -KM_PI - x;
    x2 = x * x;
    return x * (1.0f - x2 * (1.0f / 6.0f) * (1.0f - x2 * (1.0f / 20.0f) * (1.0f - x2 * (1.0f / 42.0f) *
                (1.0f - x2 * (1.0f / 72.0f) * (1.0f - x2 * (1.0f / 110.0f))))));
}
static inline float kd_cos(float x) { return kd_sin(0.5f * KM_PI - x); }   /* x in 0..pi */
static inline float kd_atan(float x)            /* ~1e-5 */
{
    float a = x < 0.0f ? -x : x, a2, r;
    int inv = a > 1.0f;
    if (inv) a = 1.0f / a;
    a2 = a * a;
    r = a * (0.99997726f + a2 * (-0.33262347f + a2 * (0.19354346f + a2 * (-0.11643287f + a2 * (0.05265332f -
             a2 * 0.01172120f)))));
    if (inv) r = 0.5f * KM_PI - r;
    return x < 0.0f ? -r : r;
}
static inline float kd_sqrt(float x)
{
    union { float f; int32_t i; } u;
    float y;
    int k;
    if (x <= 0.0f)
        return 0.0f;
    u.f = x;
    u.i = 0x5F3759DF - (u.i >> 1);              /* 1 / sqrt(x), refined */
    y = u.f;
    for (k = 0; k < 3; k++)
        y = y * (1.5f - 0.5f * x * y * y);
    return x * y;
}
static inline float kd_clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* phase delays at w (radians a sample) of a first-order all-pass (a + z^-1) / (1 + a z^-1), and of the one-pole
 * low-pass y = (1 - b) x + b y[-1]; and the low-pass's gain there */
static inline float kd_pd_ap(float a, float w)
{
    return 1.0f - 2.0f * kd_atan(a * kd_sin(w) / (1.0f + a * kd_cos(w))) / w;
}
static inline float kd_pd_lp(float b, float w)
{
    return kd_atan(b * kd_sin(w) / (1.0f - b * kd_cos(w))) / w;
}
static inline float kd_gain_lp(float b, float w)
{
    return (1.0f - b) / kd_sqrt(1.0f - 2.0f * b * kd_cos(w) + b * b);
}
/* the gain a period that gives t60 seconds at f0 Hz */
static inline float kd_t60_gain(float t60, float f0) { return kd_exp2(-9.965784f / (t60 * f0)); }
/* the one-pole's b for a cutoff fc Hz */
static inline float kd_lp_b(float fc)
{
    float w = 2.0f * KM_PI * kd_clamp(fc, 10.0f, 0.45f * KD_SR) / KD_SR;
    return kd_exp2(-1.442695f * w);             /* e^-w */
}

/* ---- a delay-line string: the integer delay, a Thiran all-pass for the fraction */
typedef struct {
    float *d;
    uint32_t w, n;                              /* write index, integer delay */
    float tha, thx, thy;                        /* the Thiran all-pass: its coefficient and states */
} kd_line_t;

/* the delay `l` (samples, at w0) less what the loop's filters take, `fix`: integer part and Thiran fraction */
static void kd_line_tune(kd_line_t *s, float l, float w0)
{
    float d;
    int k;
    if (l < 2.5f) l = 2.5f;
    s->n = (uint32_t)(l - 0.5f);
    if (s->n > KD_LEN - 4u) s->n = KD_LEN - 4u;
    d = l - (float)s->n;                        /* 0.5 .. 1.5: the all-pass's stable, flat range */
    s->tha = (1.0f - d) / (1.0f + d);
    for (k = 0; k < 2; k++) {                   /* its phase delay at w0, not at DC: corrected */
        float e = kd_pd_ap(s->tha, w0) - d, dd = d - e;
        dd = kd_clamp(dd, 0.3f, 1.7f);
        s->tha = (1.0f - dd) / (1.0f + dd);
    }
}
static inline float kd_line_read(kd_line_t *s)
{
    float x = s->d[(s->w - s->n) & KD_MASK];
    float y = s->tha * (x - s->thy) + s->thx;
    s->thx = x;
    s->thy = y;
    return y;
}
static inline void kd_line_write(kd_line_t *s, float v)
{
    s->d[s->w] = v;
    s->w = (s->w + 1u) & KD_MASK;
}
/* a fresh string: silence in what the loop reads before it writes again (the n samples behind w; the pickups' taps
 * are within them), not the whole line: a note-on in the audio interrupt stays short. (w may be what another
 * instrument left in the voice: in range first) */
static void kd_line_clear(kd_line_t *s)
{
    uint32_t i;
    s->w &= KD_MASK;
    for (i = 1; i <= s->n + 2u; i++)
        s->d[(s->w - i) & KD_MASK] = 0.0f;
    s->thx = s->thy = 0.0f;
}

/* ---- what a note is played with: the preset's and the knobs' (0..1) */
typedef struct {
    float tone, decay, pick, vol;               /* KNOB1..4 */
    float x[4];                                 /* the family's own (keys_module.c's presets) */
} kd_par_t;

/* ---- CLAV */
typedef struct {
    kd_line_t s;
    float sa, sx1, sy1, sx2, sy2;               /* the stiffness: two all-passes, one coefficient */
    float b, gb, b_off, gb_off, y;              /* the loss: y = gb x + b y (held, damped) */
    uint32_t p1, p2;                            /* the pickups' taps */
    float m2;                                   /* the second pickup: +1 in phase, -1 out, 0 none */
    uint32_t exc, rng;                          /* noise left to put in, its generator */
    float eamp;
    km_svf_t ef;                                /* the excitation's filter */
    km_svf_t f;                                 /* the resonant filter */
    float fbase, fenv, fq, env, edec, bp;       /* its cutoff (normalised), the envelope's depth, ... */
} kd_clav_t;

/* x[0] the second pickup (-1..1), x[1] resonance (q), x[2] envelope depth (octaves), x[3] band-pass 1 / low-pass 0 */
static void kd_clav_on(kd_clav_t *c, float f0, float vn, float key, const kd_par_t *p, int fresh)
{
    float w0, t60, l, g, fc;
    if (fresh) {
        c->sx1 = c->sy1 = c->sx2 = c->sy2 = c->y = 0.0f;
        km_svf_reset(&c->ef);
        km_svf_reset(&c->f);
    }
    while (f0 < KD_SR / (float)(KD_LEN - 8u))
        f0 *= 2.0f;
    w0 = 2.0f * KM_PI * f0 / KD_SR;
    c->sa = -0.42f;
    t60 = (0.5f + 8.0f * p->decay * p->decay) * kd_exp2(-(f0 - 261.6f) * (1.0f / 1400.0f));
    fc = f0 * kd_exp2(2.0f + 4.5f * p->tone + 1.0f * vn);
    c->b = kd_lp_b(fc);
    g = kd_t60_gain(t60, f0) / kd_gain_lp(c->b, w0);
    c->gb = (g < 0.99995f ? g : 0.99995f) * (1.0f - c->b);
    c->b_off = kd_lp_b(f0 * 2.5f);              /* the yarn: 70 ms, dark */
    g = kd_t60_gain(0.07f, f0) / kd_gain_lp(c->b_off, w0);
    c->gb_off = (g < 0.999f ? g : 0.999f) * (1.0f - c->b_off);
    l = KD_SR / f0 - 2.0f * kd_pd_ap(c->sa, w0) - kd_pd_lp(c->b, w0);
    kd_line_tune(&c->s, l, w0);
    if (fresh)
        kd_line_clear(&c->s);
    /* the pickups: at 4 .. 40 % of the string (the comb's notches at multiples of 1 / pos), the second at 0.55 of it */
    l = KD_SR / f0;
    c->p1 = (uint32_t)(l * (0.04f + 0.36f * p->pick)) + 1u;
    c->p2 = (uint32_t)(l * (0.04f + 0.36f * p->pick) * 0.55f) + 1u;
    c->m2 = p->x[0];
    /* the excitation: Plaits' (keys_mi.h), the velocity as its accent */
    c->exc = (uint32_t)(KD_SR / f0);
    c->rng = c->rng * 69069u + (uint32_t)(f0 * 16.0f) + 1u;
    km_svf_set(&c->ef, km_excite_cutoff(f0 / KD_SR, 0.35f + 0.6f * p->tone, vn, kd_exp2), 0.5f);
    /* (the level evened out across the keys: the pickups and the filter give a high string more) */
    c->eamp = (0.12f + 0.88f * vn * vn) * kd_exp2(-(kd_clamp(key, 36.0f, 96.0f) - 60.0f) * (1.0f / 19.0f));
    /* the filter: its cutoff from the tone, opened by the velocity and the envelope */
    if (p->x[3] > 0.5f)                         /* the wah: a band from 250 Hz (a pedal's travel), not by the key */
        c->fbase = 250.0f * kd_exp2(2.0f * p->tone) / KD_SR;
    else
        c->fbase = kd_clamp(f0 * kd_exp2(1.5f + 3.5f * p->tone) / KD_SR, 60.0f / KD_SR, 0.45f);
    c->fq = 0.7f + 5.0f * p->x[1];
    c->fenv = p->x[2] * (0.4f + 0.6f * vn);
    c->bp = p->x[3];
    c->env = 1.0f;
    c->edec = kd_exp2(-(float)KD_CTL / (KD_SR * (0.08f + 0.35f * p->decay)));
}

/* n frames (a multiple of KD_CTL, or the last part) into out, added at gain g; gate 0: damped. Returns the peak */
static float kd_clav_run(kd_clav_t *c, int gate, float g, float *out, uint32_t n)
{
    uint32_t i, j, rng = c->rng;
    float pk = 0.0f, b = gate ? c->b : c->b_off, gb = gate ? c->gb : c->gb_off;
    float sa = c->sa, sx1 = c->sx1, sy1 = c->sy1, sx2 = c->sx2, sy2 = c->sy2, y = c->y, m2 = c->m2;
    for (j = 0; j < n; j += KD_CTL) {
        uint32_t k = n - j < KD_CTL ? n - j : KD_CTL;
        float fc = c->fbase * kd_exp2(c->fenv * c->env);
        km_svf_set(&c->f, fc < 0.45f ? fc : 0.45f, c->fq);
        c->env *= c->edec;
        for (i = 0; i < k; i++) {
            float x = kd_line_read(&c->s), s, v, o, lp, bp;
            s = sa * (x - sy1) + sx1;           /* the stiffness */
            sx1 = x; sy1 = s;
            x = sa * (s - sy2) + sx2;
            sx2 = s; sy2 = x;
            y = gb * x + b * y;                 /* the loss */
            v = y;
            if (c->exc) {
                c->exc--;
                v += c->eamp * km_svf_lp(&c->ef, 2.0f * km_rand_float(&rng) - 1.0f);
            }
            kd_line_write(&c->s, v);
            o = v - c->s.d[(c->s.w - c->p1) & KD_MASK];
            o += m2 * (v - c->s.d[(c->s.w - c->p2) & KD_MASK]);
            km_svf_lp_bp(&c->f, o, &lp, &bp);
            o = c->bp > 0.5f ? bp : lp;
            out[j + i] += g * o;
            o = o < 0.0f ? -o : o;
            if (o > pk) pk = o;
        }
    }
    c->sx1 = sx1; c->sy1 = sy1; c->sx2 = sx2; c->sy2 = sy2; c->y = y;
    c->rng = rng;
    return pk;
}

/* ---- E.PIANO */
typedef struct {
    float y1[KD_NM], y2[KD_NM];                 /* each mode's last two values */
    float c[KD_NM], r2[KD_NM];                  /* y = c y1 - r2 y2: c = 2 r cos w, r2 = r^2 */
    float cos2[KD_NM], t60[KD_NM];              /* 2 cos w, each mode's t60 while held */
    float off_t60;                              /* damped */
    int nm, damped;
    float drive, bias, bias0, last, lpb, lp;
} kd_ep_t;

/* the modes: ratio to f0, level, t60 against the fundamental's; the tine's own above the tone bar's harmonics
 * (a cantilever's 1 : 6.27 : 17.55 : 34.39) */
static const float EP_RATIO[KD_NM] = {1.0f, 1.0f, 2.0f, 3.0f, 6.267f, 9.42f, 17.55f, 34.39f};
static const float EP_LEVEL[KD_NM] = {1.0f, 0.10f, 0.05f, 0.02f, 0.20f, 0.03f, 0.07f, 0.025f};
static const float EP_T60[KD_NM] = {1.0f, 0.7f, 0.35f, 0.25f, 0.15f, 0.08f, 0.025f, 0.012f};
/* the tine's own modes fit no interval: a chime at the strike, not a tone under the note. At most this long (s), however
 * long the note is (0.15 of a low note's t60 was over a second, and chords sounded out of tune: #103) */
static const float EP_CHIME[KD_NM] = {0.0f, 0.0f, 0.0f, 0.0f, 0.60f, 0.20f, 0.08f, 0.05f};

static void kd_ep_set_r(kd_ep_t *e, int damped)
{
    int m;
    for (m = 0; m < e->nm; m++) {
        float t = damped ? e->off_t60 * EP_T60[m] * 3.0f : e->t60[m];
        float r;
        if (damped && t > e->t60[m]) t = e->t60[m];
        r = kd_exp2(-9.965784f / (t * KD_SR));  /* 10^(-3 / (t SR)) */
        e->c[m] = e->cos2[m] * r;
        e->r2[m] = r * r;
    }
    e->damped = damped;
}

/* x[0] the bell (the tine's modes), x[1] the hammer's hardness, x[2] the bark (the pickup off centre), x[3] the
 * tone bar's beat (Hz) */
static void kd_ep_on(kd_ep_t *e, float f0, float vn, const kd_par_t *p, int fresh)
{
    int m, nm = 0;
    float t1 = (1.5f + 12.0f * p->decay * p->decay) * kd_exp2(-(f0 - 220.0f) * (1.0f / 900.0f));
    float fc = 250.0f * kd_exp2(2.0f * p->x[1] + 3.0f * vn + 1.0f * p->tone);   /* the hammer's corner */
    float amp = 0.06f + 0.94f * vn * vn;
    if (fresh)
        for (m = 0; m < KD_NM; m++)
            e->y1[m] = e->y2[m] = 0.0f;
    for (m = 0; m < KD_NM; m++) {
        float f = f0 * EP_RATIO[m], w, a, r, h;
        if (m == 1) f += p->x[3];               /* the tone bar, a little off the tine */
        if (f > 0.42f * KD_SR)
            break;
        w = 2.0f * KM_PI * f / KD_SR;
        e->cos2[m] = 2.0f * kd_cos(w);
        e->t60[m] = t1 * EP_T60[m];
        if (m >= 4 && e->t60[m] > EP_CHIME[m]) e->t60[m] = EP_CHIME[m];
        h = f / fc;
        a = EP_LEVEL[m] * (m >= 4 ? 0.3f + 1.7f * p->x[0] : 1.0f) * amp / (1.0f + h * h);
        r = kd_exp2(-9.965784f / (e->t60[m] * KD_SR));
        /* struck at rest: a decaying sine from 0, y[-1] = 0, y[-2] = -a sin w / r (added to what still rings) */
        e->y2[m] += -a * kd_sin(w) / r;
        nm = m + 1;
    }
    for (m = nm; m < KD_NM; m++)
        e->y1[m] = e->y2[m] = 0.0f;
    e->nm = nm;
    e->off_t60 = 0.12f;
    kd_ep_set_r(e, 0);
    e->drive = 0.7f + 1.3f * p->pick;
    e->bias0 = 0.08f + 0.5f * p->x[2] * (0.5f + p->pick);
    e->bias = e->bias0 - e->bias0 * e->bias0 * e->bias0 * (1.0f / 3.0f);
    e->lpb = kd_lp_b(1200.0f * kd_exp2(3.5f * p->tone));
    if (fresh)
        e->last = e->lp = 0.0f;
}

static float kd_ep_run(kd_ep_t *e, int gate, float g, float *out, uint32_t n)
{
    uint32_t i;
    int m, nm = e->nm;
    float pk = 0.0f, last = e->last, lp = e->lp, lpb = e->lpb, drive = e->drive, b0 = e->bias0, bias = e->bias;
    if ((!gate) != e->damped)
        kd_ep_set_r(e, !gate);
    for (i = 0; i < n; i++) {
        float x = 0.0f, u, o;
        for (m = 0; m < nm; m++) {
            float y = e->c[m] * e->y1[m] - e->r2[m] * e->y2[m];
            e->y2[m] = e->y1[m];
            e->y1[m] = y;
            x += y;
        }
        /* the pickup: the field's curve around the tine (a cubic soft clip, off centre), its change the coil's
         * voltage (a first difference, with some of the level kept) */
        u = drive * x + b0;
        u = u > 1.0f ? 1.0f : u < -1.0f ? -1.0f : u;
        u = u - u * u * u * (1.0f / 3.0f) - bias;
        o = 1.6f * u - 1.2f * last;
        last = u;
        lp = o + lpb * (lp - o);
        out[i] += g * lp;
        o = lp < 0.0f ? -lp : lp;
        if (o > pk) pk = o;
    }
    e->last = last;
    e->lp = lp;
    return pk;
}

/* ---- PIANO */
static inline float kd_log2(float x)            /* x > 0, ~1e-5 */
{
    union { float f; int32_t i; } u;
    float y, y2;
    int32_t e;
    u.f = x;
    e = ((u.i >> 23) & 255) - 127;
    u.i = (u.i & 0x007FFFFF) | 0x3F800000;      /* 1 .. 2 */
    y = (u.f - 1.0f) / (u.f + 1.0f);
    y2 = y * y;
    return (float)e + 2.885390f * y * (1.0f + y2 * (0.3333333f + y2 * (0.2f + y2 * 0.1428571f)));
}
static inline float kd_pow(float a, float b) { return kd_exp2(b * kd_log2(a)); }
/* the felt's force: 2^(l2k + p log2(u)), u > 0, with short polynomials (~0.1 %): it is worked out every sample the
 * hammer touches the string, sixteen hammers at once in a chord */
static inline float kd_felt(float u, float p, float l2k)
{
    union { float f; int32_t i; } v;
    float m, x, f;
    int32_t e, i;
    v.f = u;
    e = ((v.i >> 23) & 255) - 127;
    v.i = (v.i & 0x007FFFFF) | 0x3F800000;
    m = v.f - 1.0f;
    x = l2k + p * ((float)e + m * (1.4421478f + m * (-0.7020801f + m * (0.3673458f - m * 0.1078226f))));
    i = (int32_t)x;
    if (x < (float)i)
        i--;
    if (i < -126)
        return 0.0f;
    f = x - (float)i;
    v.i = ((i > 126 ? 126 : i) + 127) << 23;
    return v.f * (1.0f + f * (0.6935327f + f * (0.2334681f + f * 0.0725858f)));
}

typedef struct {
    kd_line_t s[2];
    float apa;                                  /* the dispersion's coefficient */
    int nap;
    float ax[2][KD_NAP], ay[2][KD_NAP];
    float b, gb[2], b_off, gb_off[2], y[2];
    float rho;                                  /* the bridge: what it takes of the strings' sum, a period */
    uint32_t tap[2];                            /* the hammer's other meeting with the string: the wave back from the bridge */
    /* the hammer: its velocity (m/s), the felt's compression (m), K u^p / 2Z last time, the force / 2Z now */
    int hon, hsub;
    uint32_t hn;
    float hv, hu, hup, hf, hk, hp, hc2, hc3, hdt;
    float og;                                   /* the bridge's velocity to the output */
    uint32_t live;                              /* samples a fresh strike is still on its way round the string */
} kd_piano_t;

#define KP_E 200e9f                             /* steel: Young's modulus (Pa), density (kg / m^3) */
#define KP_RHO 7850.0f
#define KP_ZB 4000.0f                           /* the bridge's impedance (kg / s) */

/* x[0] the strings' detune (cents), x[1] stiffness (x the key's B), x[2] -, x[3] - */
static void kd_piano_on(kd_piano_t *pn, float f0, float vn, float key, const kd_par_t *p, int fresh)
{
    int k, i, nphys, kk;
    float lr, kr, len, r, rl, ten, z, bb, rc, zb, c1, c3, g, a1, bq, w1, wk, want, lo, hi, dsc, n0, m, kf, pp;
    if (fresh) {
        for (k = 0; k < 2; k++) {
            pn->y[k] = 0.0f;
            for (i = 0; i < KD_NAP; i++)
                pn->ax[k][i] = pn->ay[k][i] = 0.0f;
        }
    }
    while (f0 < KD_SR / (float)(KD_LEN - 8u)) {
        f0 *= 2.0f;
        key += 12.0f;
    }
    /* the string by its key: length, radius, tension, wave impedance, stiffness B; one, two or three strings a key
     * (the laws of C. Otey's waveguide piano, as S. Takano's pico_piano has them: see the head of this file) */
    lr = 0.6931472f * kd_log2(f0 / 27.5f);
    if (lr < 0.0f) lr = 0.0f;
    kr = kd_clamp(lr * 0.1989924f, 0.0f, 1.0f);
    len = 0.04f + 1.4f / (1.0f + kd_exp2(1.442695f * (-3.4f + 1.4f * lr)));
    r = 0.002f * kd_pow(1.0f + 0.6f * lr, -1.4f);
    rl = KM_PI * r * r * KP_RHO;
    ten = (2.0f * len * f0) * (2.0f * len * f0) * rl;
    z = kd_sqrt(ten * rl);
    rc = r < 0.0006f ? r : 0.0006f;
    bb = (KM_PI * KM_PI * KM_PI) * KP_E * (rc * rc * rc * rc) / (4.0f * len * len * ten) * (0.5f + p->x[1]);
    nphys = f0 < 47.6f ? 1 : f0 < 84.8f ? 2 : 3;
    /* DECAY: the bridge heavier and the string's own loss less, x 1/4 .. x 2.2 around the preset's */
    dsc = kd_exp2(2.5f * (p->decay - 0.6f));
    zb = KP_ZB * dsc;
    pn->rho = 0.5f * (float)nphys * 2.0f * z / ((float)nphys * z + zb);   /* (two strings stand for nphys) */
    /* the string's loss: 1 - c1 / f0 a period at every frequency, and a one-pole for what the high partials lose
     * more (c3; TONE) */
    c1 = 0.25f / dsc;
    c3 = 5.85f * kd_exp2(3.0f * (0.55f - p->tone));
    g = 1.0f - c1 / f0;
    bq = 4.0f * c3 + f0;
    a1 = (-bq + kd_sqrt(bq * bq - 16.0f * c3 * c3)) / (4.0f * c3);
    pn->b = -a1;
    pn->b_off = kd_lp_b(f0 * 3.0f);
    if (key >= 88.0f)                           /* (no dampers at the top) */
        pn->b_off = pn->b;
    /* the dispersion: the all-passes' coefficient that puts partial kk where B says, against the fundamental
     * (what the loop must be shorter by there, in samples) */
    n0 = KD_SR / f0;
    kk = (int)(0.30f * KD_SR / f0);
    if (kk > 10) kk = 10;
    pn->nap = key < 92.0f ? KD_NAP : 2;
    pn->apa = 0.0f;
    if (kk >= 2) {
        float fk = (float)kk * f0 * kd_sqrt((1.0f + bb * (float)(kk * kk)) / (1.0f + bb));
        w1 = 2.0f * KM_PI * f0 / KD_SR;
        wk = 2.0f * KM_PI * fk / KD_SR;
        want = n0 - (float)kk * KD_SR / fk;     /* the loop's delay at the fundamental less that at partial kk */
        lo = -0.97f; hi = 0.0f;
        for (i = 0; i < 18; i++) {
            float a = 0.5f * (lo + hi);
            float d = (float)pn->nap * (kd_pd_ap(a, w1) - kd_pd_ap(a, wk));
            if (d > want) lo = a; else hi = a;
        }
        pn->apa = 0.5f * (lo + hi);
    } else
        pn->nap = 0;
    for (k = 0; k < 2; k++) {
        float f = f0 * kd_exp2((k ? 0.5f : -0.5f) * p->x[0] * (1.0f / 1200.0f));
        float w = 2.0f * KM_PI * f / KD_SR, l, gg;
        uint32_t hp;
        pn->gb[k] = g * (1.0f - pn->b);
        gg = kd_t60_gain(key < 88.0f ? 0.25f : 30.0f, f) / kd_gain_lp(pn->b_off, w);
        pn->gb_off[k] = (gg < 0.99995f ? gg : 0.99995f) * (1.0f - pn->b_off);
        l = KD_SR / f - (float)pn->nap * kd_pd_ap(pn->apa, w) - kd_pd_lp(pn->b, w);
        kd_line_tune(&pn->s[k], l, w);
        if (fresh)
            kd_line_clear(&pn->s[k]);
        hp = (uint32_t)(KD_SR / f * (1.0f / 7.0f) + 0.5f);          /* struck at a seventh: there and back to the near end */
        pn->tap[k] = pn->s[k].n > hp + 1u ? pn->s[k].n - hp : 1u;
    }
    /* the hammer: its mass, the felt's stiffness K and exponent p (harder and steeper up the keyboard; KNOB3), the
     * felt's hysteresis; the velocity the key gives it */
    m = 0.06f - 0.058f * kd_pow(kr > 1e-6f ? kr : 1e-6f, 0.1f);
    /* (at the top a period is a dozen samples and that hammer would lie on the string for three or four of them:
     * what it leaves then depends on the moment it lets go, not on how hard it came. Lighter there, to two periods) */
    if (key > 84.0f)
        m *= kd_exp2(-(key - 84.0f) * (1.0f / 8.0f));
    pp = 2.0f + kr;
    kf = 40.0f * kd_pow(0.7e-3f, -pp) * kd_exp2(1.5f + 3.0f * (p->pick - 0.47f));
    pn->hsub = kr < 0.7f ? 1 : 2;               /* (the felt is stiff at the top: two steps a sample there) */
    pn->hdt = 1.0f / (KD_SR * (float)pn->hsub);
    pn->hk = kd_log2(kf / (2.0f * z));          /* (as a power of two: kd_felt) */
    pn->hp = pp;
    pn->hc2 = 0.1e-4f * kr / pn->hdt;
    pn->hc3 = pn->hdt * 2.0f * z / m;
    pn->hv = 0.25f + 5.5f * vn * vn;
    pn->hu = pn->hup = pn->hf = 0.0f;
    pn->hon = 1;
    pn->hn = 0;
    pn->og = 80.0f;
    pn->live = 2u * KD_LEN;
}

static float kd_piano_run(kd_piano_t *pn, int gate, float g, float *out, uint32_t n)
{
    uint32_t i;
    int k, j, nap = pn->nap;
    float pk = 0.0f, a = pn->apa, b = gate ? pn->b : pn->b_off, rho = pn->rho, og = g * pn->og;
    for (i = 0; i < n; i++) {
        float y[2], vb, o;
        for (k = 0; k < 2; k++) {
            float x = kd_line_read(&pn->s[k]), *ax = pn->ax[k], *ay = pn->ay[k];
            for (j = 0; j < nap; j++) {
                float t = a * (x - ay[j]) + ax[j];
                ax[j] = x;
                ay[j] = t;
                x = t;
            }
            pn->y[k] = (gate ? pn->gb[k] : pn->gb_off[k]) * x + b * pn->y[k];
            y[k] = pn->y[k];
        }
        vb = rho * (y[0] + y[1]);               /* the bridge moves with the strings' sum, and takes it from each */
        y[0] -= vb;
        y[1] -= vb;
        if (pn->hon) {
            /* the string's velocity where the hammer is: the wave from the near end and the one back from the bridge */
            kd_line_t *s0 = &pn->s[0], *s1 = &pn->s[1];
            float *d0 = &s0->d[(s0->w - pn->tap[0]) & KD_MASK], *d1 = &s1->d[(s1->w - pn->tap[1]) & KD_MASK];
            float vin = 0.5f * (y[0] - *d0 + y[1] - *d1), f = pn->hf;
            for (j = 0; j < pn->hsub; j++) {
                float up;
                pn->hu += (pn->hv - vin - f) * pn->hdt;
                up = pn->hu > 0.0f ? kd_felt(pn->hu, pn->hp, pn->hk) : 0.0f;
                f = up + pn->hc2 * (up - pn->hup);
                if (f < 0.0f) f = 0.0f;
                pn->hv -= f * pn->hc3;
                pn->hup = up;
            }
            pn->hf = f;
            /* its force into both waves; the felt does not meet the two strings alike, so they start a little
             * apart: what the bridge cannot take from them is the long tail */
            y[0] += 1.12f * f; y[1] += 0.88f * f;
            *d0 -= 1.12f * f; *d1 -= 0.88f * f;
            /* it has left when it is off the string and going back (off it and still coming, it meets it again) */
            if ((pn->hu <= 0.0f && pn->hv <= 0.0f) || ++pn->hn > (uint32_t)(0.02f * KD_SR))
                pn->hon = 0;
        }
        kd_line_write(&pn->s[0], y[0]);
        kd_line_write(&pn->s[1], y[1]);
        o = og * vb;
        out[i] += o;
        o = o < 0.0f ? -o : o;
        if (o > pk) pk = o;
    }
    /* a low string's first wave is longer on its way than a block is: silence at the bridge for a block or two is
     * not a voice that has died away (the part ends a voice by its peak: before this a key under G2 was cut off
     * a block after its hammer) */
    if (pn->live) {
        pn->live = pn->live > n ? pn->live - n : 0u;
        if (pk < 1e-3f) pk = 1e-3f;
    }
    return pk;
}

#endif
