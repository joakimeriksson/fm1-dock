/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Javier (Animatek)
 *
 * ATEK303's effects: drive, chorus / flanger, ping-pong delay, plate reverb. See fx.h. */
#include "fx.h"
#include "fastmath.h"

#if defined PLAT_HOST || defined FX_HOST
#define FX_POOL
#else
#define FX_POOL                        /* fm1-dock: one RAM region (take.py) */
#endif

const char *const FX_DRIVE_NAME[FX_DRIVE_TYPES] = {"OFF", "OVERDRIVE", "RAT", "FUZZ"};
const char *const FX_MOD_NAME[FX_MOD_TYPES] = {"CHORUS", "FLANGER"};
const char *const FX_DLY_NAME[FX_DLY_TIMES] = {"1/16", "1/8T", "1/8", "1/8.", "1/4", "1/4."};
static const float DLY_STEPS[FX_DLY_TIMES] = {1.0f, 4.0f / 3.0f, 2.0f, 3.0f, 4.0f, 6.0f};   /* in sixteenths */

static float sr = 44100.0f;
static float par[FX_NPARAMS];
static float sixteenth = 0.12f;

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline float flush(float x) { return fm_fabsf(x) < 1e-15f ? 0.0f : x; }
static inline float onepole_c(float hz) { return 1.0f - fm_expf(-FM_TWO_PI * hz / sr); }

/* ------------------------------------------------------------------ drive --- */
/* The clippers and their antiderivatives (ADAA, first order: y = (F(x) - F(x1)) / (x - x1)). */
static inline float soft_f(float x) { return x / fm_sqrtf(1.0f + x * x); }        /* algebraic sigmoid */
static inline float soft_F(float x) { return fm_sqrtf(1.0f + x * x); }
static inline float hard_f(float x) { return clampf(x, -1.0f, 1.0f); }
static inline float hard_F(float x)
{
    float a = fm_fabsf(x);
    return a <= 1.0f ? 0.5f * x * x : a - 0.5f;
}
static struct {
    float x1, pre_hp, pre_x, tone_y, tone_y2, dc_x, dc_y, gain_s, tone_c, pre_c;
} dr;
static inline float adaa(float x, int hard)
{
    float d = x - dr.x1, y;
    if (fm_fabsf(d) < 1e-4f) {
        float m = 0.5f * (x + dr.x1);
        y = hard ? hard_f(m) : soft_f(m);
    } else
        y = hard ? (hard_F(x) - hard_F(dr.x1)) / d : (soft_F(x) - soft_F(dr.x1)) / d;
    dr.x1 = x;
    return y;
}
static inline float drive_sample(float x, int type)
{
    float g = dr.gain_s, y, bias;
    if (type == 2) {                           /* RAT: the op-amp's pre-emphasis (lows less amplified) */
        dr.pre_hp += dr.pre_c * (x - dr.pre_hp);
        x = 0.35f * dr.pre_hp + (x - dr.pre_hp);
        y = adaa(g * x, 1);
    } else if (type == 3) {                    /* FUZZ: lopsided: one half clips sooner */
        bias = 0.25f;
        y = adaa(g * x + bias, 1) - hard_f(bias);
    } else {                                   /* OVERDRIVE: soft, a little asymmetric */
        bias = 0.12f;
        y = adaa(g * x + bias, 0) - soft_f(bias);
    }
    /* the DC the asymmetry leaves, out (5 Hz) */
    {
        float hp = y - dr.dc_x + 0.9993f * dr.dc_y;
        dr.dc_x = y;
        dr.dc_y = flush(hp);
        y = hp;
    }
    /* TONE: two one-poles, 700 Hz .. 12 kHz */
    dr.tone_y += dr.tone_c * (y - dr.tone_y);
    dr.tone_y2 += dr.tone_c * (dr.tone_y - dr.tone_y2);
    return dr.tone_y2;
}

/* ------------------------------------------------------------ chorus/flanger --- */
#define MOD_N 1024u                            /* 23 ms */
static float mod_buf[2][MOD_N] FX_POOL;
static struct {
    uint32_t w;
    float ph, fb[2];
} md;
static inline float mod_read(const float *b, float delay)   /* delay in samples, linear */
{
    float pos = (float)md.w - delay;
    int32_t i;
    float f;
    while (pos < 0.0f)
        pos += (float)MOD_N;
    i = (int32_t)pos;
    f = pos - (float)i;
    return b[(uint32_t)i & (MOD_N - 1u)] * (1.0f - f) + b[(uint32_t)(i + 1) & (MOD_N - 1u)] * f;
}

/* -------------------------------------------------------------------- delay --- */
#define DLY_N 26460u                           /* 0.6 s each side, 16-bit */
static int16_t dly_buf[2][DLY_N] FX_POOL;
static struct {
    uint32_t w;
    float len, lp[2], hp[2];
} dl;
static inline float dly_read(const int16_t *b, float delay)
{
    float pos = (float)dl.w - delay;
    int32_t i;
    float f;
    if (pos < 0.0f)
        pos += (float)DLY_N;
    i = (int32_t)pos;
    f = pos - (float)i;
    {
        uint32_t a = (uint32_t)i % DLY_N, b2 = (a + 1u) % DLY_N;
        return ((float)b[a] * (1.0f - f) + (float)b[b2] * f) * (1.0f / 16384.0f);
    }
}
static inline int16_t to16(float x)
{
    x = clampf(x * 16384.0f, -32767.0f, 32767.0f);
    return (int16_t)x;
}
float fx_delay_seconds(void)
{
    int t = (int)par[FX_DLY_TIME];
    float s = DLY_STEPS[t < 0 ? 0 : t >= FX_DLY_TIMES ? FX_DLY_TIMES - 1 : t] * sixteenth;
    return fm_minf(s, (float)(DLY_N - 4u) / sr);
}

/* ------------------------------------------------------------------- reverb --- */
/* Dattorro's plate, his lengths (at 29761 Hz) scaled to 44.1 kHz */
#define RS(n) ((uint32_t)((float)(n) * 1.48181f + 0.5f))
enum { R_PRE, R_IN1, R_IN2, R_IN3, R_IN4, R_AP1L, R_D1L, R_AP2L, R_D2L, R_AP1R, R_D1R, R_AP2R, R_D2R, R_NLINES };
#define PRE_MAX 2205u                          /* 50 ms */
#define EXC 24u                                /* the tank modulation's excursion, in samples */
static uint32_t r_len[R_NLINES], r_off[R_NLINES], r_pos[R_NLINES];
#define RV_N (PRE_MAX + 33400u)              /* the lines below add up to 35588 */
#ifdef FX_RV_Q                                 /* the plate's lines in 16 bits, FX_RV_Q a unit (a smaller build) */
typedef int16_t rv_t;
static inline float rv_ld(rv_t v) { return (float)v * (1.0f / (float)FX_RV_Q); }
static inline rv_t rv_st(float x)
{
    x *= (float)FX_RV_Q;
    return (rv_t)(x > 32767.0f ? 32767.0f : x < -32767.0f ? -32767.0f : x + (x < 0.0f ? -0.5f : 0.5f));
}
#else                                          /* (our own build: floats, as before) */
typedef float rv_t;
#define rv_ld(v) (v)
#define rv_st(x) (x)
#endif
static rv_t rv_buf[RV_N] FX_POOL;
static uint8_t rv_ok;
static struct {
    float bw, lp_l, lp_r, damp_c, decay, fb_l, fb_r, ph;
} rv;
static inline rv_t *rline(int k) { return &rv_buf[r_off[k]]; }
static inline float rtap(int k, uint32_t d)   /* the sample written d ago */
{
    uint32_t n = r_len[k], p = r_pos[k];
    return rv_ld(rline(k)[(p + n - (d % n)) % n]);
}
static inline void rput(int k, float x)
{
    rline(k)[r_pos[k]] = rv_st(x);
    if (++r_pos[k] >= r_len[k])
        r_pos[k] = 0;
}
static inline float rap(int k, float x, float g)   /* Schroeder allpass on line k, full length */
{
    float d = rv_ld(rline(k)[r_pos[k]]), v = x - g * d;   /* (the oldest sample: written len ago) */
    rput(k, flush(v));
    return d + g * v;
}
static inline float rap_mod(int k, float x, float g, float delay)   /* the tank's modulated allpass */
{
    uint32_t n = r_len[k];
    float pos = (float)r_pos[k] - delay, f, d, v;
    int32_t i;
    while (pos < 0.0f)
        pos += (float)n;
    i = (int32_t)pos;
    f = pos - (float)i;
    d = rv_ld(rline(k)[(uint32_t)i % n]) * (1.0f - f) + rv_ld(rline(k)[(uint32_t)(i + 1) % n]) * f;
    v = x - g * d;
    rput(k, flush(v));
    return d + g * v;
}
static void reverb_init(void)
{
    static const uint32_t L[R_NLINES] = {PRE_MAX, 142, 107, 379, 277, 672, 4453, 1800, 3720, 908, 4217, 2656, 3163};
    uint32_t off = 0;
    int k;
    for (k = 0; k < R_NLINES; k++) {
        r_len[k] = k == R_PRE ? PRE_MAX : RS(L[k]) + (k == R_AP1L || k == R_AP1R ? EXC + 2u : 0u);
        r_off[k] = off;
        r_pos[k] = 0;
        off += r_len[k];
    }
    rv_ok = off <= RV_N;                      /* (never false with these lengths: a guard for edits) */
}

/* -------------------------------------------------------------------- API --- */
void fx_init(float rate)
{
    int i;
    sr = rate;
    for (i = 0; i < FX_NPARAMS; i++)
        par[i] = 0.0f;
    par[FX_DRIVE] = 0.5f;
    par[FX_TONE] = 0.6f;
    par[FX_LEVEL] = 0.6f;
    par[FX_MOD_RATE] = 0.3f;
    par[FX_MOD_DEPTH] = 0.5f;
    par[FX_DLY_TIME] = 3.0f;                   /* 1/8 dotted: the acid classic */
    par[FX_DLY_FEEDBACK] = 0.45f;
    par[FX_DLY_TONE] = 0.5f;
    par[FX_REV_DECAY] = 0.6f;
    par[FX_REV_DAMP] = 0.4f;
    par[FX_REV_PRE] = 0.2f;
    reverb_init();
    dr.x1 = 0.0f;
    dl.len = fx_delay_seconds() * sr;
    rv.bw = 0.9995f;
}

void fx_set(int p, float v)
{
    if (p >= 0 && p < FX_NPARAMS)
        par[p] = v;
}

void fx_tempo(float s) { sixteenth = clampf(s, 0.02f, 2.0f); }

void fx_process(const float *in, float *outl, float *outr, uint32_t n)
{
    uint32_t i;
    int dtype = (int)par[FX_DRIVE_TYPE], mtype = (int)par[FX_MOD_TYPE];
    float level = 1.2f * par[FX_LEVEL] * par[FX_LEVEL];      /* 0.6: about the dry level */
    float mmix = par[FX_MOD_MIX], dmix = par[FX_DLY_MIX], rmix = par[FX_REV_MIX];
    float fb = 0.95f * par[FX_DLY_FEEDBACK];
    float dlp = onepole_c(800.0f * fm_exp2f(4.0f * par[FX_DLY_TONE]));   /* 800 Hz .. 12.8 kHz */
    float dhp = onepole_c(120.0f);
    float target = fx_delay_seconds() * sr;
    float rate = 0.05f * fm_exp2f(7.0f * par[FX_MOD_RATE]);              /* 0.05 .. 6.4 Hz */
    float inc = rate / sr;
    float base = mtype ? 0.0006f * sr : 0.007f * sr;
    float depth = (mtype ? 0.004f : 0.004f) * sr * par[FX_MOD_DEPTH];
    float mfb = mtype ? 0.6f : 0.0f;
    float decay = 0.25f + 0.65f * par[FX_REV_DECAY];         /* up to 0.9: long, never endless */
    float damp = onepole_c(1500.0f * fm_exp2f(3.5f * (1.0f - par[FX_REV_DAMP])));   /* 1.5 .. 17 kHz */
    uint32_t pre = (uint32_t)(par[FX_REV_PRE] * (float)(PRE_MAX - 1u));
    dr.gain_s = 1.0f + (dtype == 3 ? 120.0f : dtype == 2 ? 60.0f : 12.0f) * par[FX_DRIVE] * par[FX_DRIVE];
    dr.tone_c = onepole_c(700.0f * fm_exp2f(4.1f * par[FX_TONE]));
    dr.pre_c = onepole_c(300.0f);
    if (dtype < 0 || dtype >= FX_DRIVE_TYPES)
        dtype = 0;

    for (i = 0; i < n; i++) {
        float x = in[i], l, r;
        /* DRIVE */
        if (dtype)
            x = drive_sample(x, dtype) * level;
        l = r = x;
        /* MOD */
        if (mmix > 0.0f) {
            float s = fm_sin_turns(md.ph), c = fm_sin_turns(md.ph + 0.25f);
            float wl = mod_read(mod_buf[0], base + depth * (0.5f + 0.5f * s));
            float wr = mod_read(mod_buf[1], base + depth * (0.5f + 0.5f * c));
            mod_buf[0][md.w] = flush(x + mfb * wl);
            mod_buf[1][md.w] = flush(x + mfb * wr);
            md.w = (md.w + 1u) & (MOD_N - 1u);
            md.ph += inc;
            if (md.ph >= 1.0f)
                md.ph -= 1.0f;
            l = x + mmix * (wl - x * (mtype ? 0.0f : 0.5f));
            r = x + mmix * (wr - x * (mtype ? 0.0f : 0.5f));
        }
        /* DELAY: ping-pong; the left line feeds the right and back; the input enters on the left */
        if (dmix > 0.0f) {
            float a, b, mono = 0.5f * (l + r);
            dl.len += (target - dl.len) * 0.0004f;          /* turning TIME glides, like tape */
            a = dly_read(dly_buf[0], dl.len);
            b = dly_read(dly_buf[1], dl.len);
            /* the loop's tone: darker every pass, no mud */
            dl.lp[0] += dlp * (b - dl.lp[0]);
            dl.hp[0] += dhp * (dl.lp[0] - dl.hp[0]);
            dl.lp[1] += dlp * (a - dl.lp[1]);
            dl.hp[1] += dhp * (dl.lp[1] - dl.hp[1]);
            dly_buf[0][dl.w] = to16(mono + fb * (dl.lp[0] - dl.hp[0]));
            dly_buf[1][dl.w] = to16(fb * (dl.lp[1] - dl.hp[1]));
            if (++dl.w >= DLY_N)
                dl.w = 0;
            l += dmix * a;
            r += dmix * b;
        }
        /* REVERB */
        if (rmix > 0.0f && rv_ok) {
            float t, xl, xr, ex, yl = 0.0f, yr = 0.0f;
            rput(R_PRE, 0.5f * (l + r));
            t = rtap(R_PRE, pre + 1u);
            rv.lp_l += rv.bw * (t - rv.lp_l);          /* bandwidth */
            t = rv.lp_l;
            t = rap(R_IN1, t, 0.75f);
            t = rap(R_IN2, t, 0.75f);
            t = rap(R_IN3, t, 0.625f);
            t = rap(R_IN4, t, 0.625f);
            ex = (float)EXC * 0.5f * (1.0f + fm_sin_turns(rv.ph));
            rv.ph += 1.0f / sr;                        /* 1 Hz */
            if (rv.ph >= 1.0f)
                rv.ph -= 1.0f;
            /* left half of the tank */
            xl = rap_mod(R_AP1L, t + decay * rv.fb_r, -0.70f, (float)RS(672) + ex);
            rput(R_D1L, xl);
            xl = rtap(R_D1L, r_len[R_D1L]);
            rv.lp_r += damp * (xl - rv.lp_r);          /* damping */
            xl = flush(rv.lp_r) * decay;
            xl = rap(R_AP2L, xl, 0.50f);
            rput(R_D2L, xl);
            /* right half */
            xr = rap_mod(R_AP1R, t + decay * rv.fb_l, -0.70f, (float)RS(908) + ((float)EXC - ex));
            rput(R_D1R, xr);
            xr = rtap(R_D1R, r_len[R_D1R]);
            rv.damp_c += damp * (xr - rv.damp_c);
            xr = flush(rv.damp_c) * decay;
            xr = rap(R_AP2R, xr, 0.50f);
            rput(R_D2R, xr);
            rv.fb_l = rtap(R_D2L, r_len[R_D2L]);
            rv.fb_r = rtap(R_D2R, r_len[R_D2R]);
            /* the output taps (Dattorro's table) */
            yl = rtap(R_D1R, RS(266)) + rtap(R_D1R, RS(2974)) - rtap(R_AP2R, RS(1913)) + rtap(R_D2R, RS(1996))
                 - rtap(R_D1L, RS(1990)) - rtap(R_AP2L, RS(187)) - rtap(R_D2L, RS(1066));
            yr = rtap(R_D1L, RS(353)) + rtap(R_D1L, RS(3627)) - rtap(R_AP2L, RS(1228)) + rtap(R_D2L, RS(2673))
                 - rtap(R_D1R, RS(2111)) - rtap(R_AP2R, RS(335)) - rtap(R_D2R, RS(121));
            l += rmix * 0.5f * yl;
            r += rmix * 0.5f * yr;
        }
        outl[i] = l;
        outr[i] = r;
    }
}
