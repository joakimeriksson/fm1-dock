/* SPDX-License-Identifier: GPL-3.0-only */
/* One Scintilla voice in fixed point: see sc_voice.h. Each block below names the TypeScript it
 * ports (synthex-web, packages/engine/src/dsp/). */
#include "sc_voice.h"
#include "sc_fixed.h"
#include "sc_tables.h"

#ifdef SC_TRACE                                 /* fm1/trace: the voice's insides, sample by sample */
#define SC_TRACE_N 400
#define SC_TRACE_K 12
uint32_t sc_trace[SC_TRACE_N][SC_TRACE_K];
uint32_t sc_trace_i;
#define T(k, x) do { if (sc_trace_i < SC_TRACE_N) sc_trace[sc_trace_i][k] = (uint32_t)(x); } while (0)
#define T_NEXT() (sc_trace_i++)
#else
#define T(k, x) ((void)0)
#define T_NEXT() ((void)0)
#endif

enum { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

#define MAX_DT 1932735283u                      /* 0.45 of a cycle: VoiceCore's dt clamp */
#define PW_LO  3277                             /* 0.05, Q16 */
#define PW_HI  62259                            /* 0.95 */

/* ---- pitch and cutoff tables ---------------------------------------------------------------- */

static const uint8_t SC_DIV12[144] = {
#define D12(o) o, o, o, o, o, o, o, o, o, o, o, o
    D12(0), D12(1), D12(2), D12(3), D12(4), D12(5), D12(6), D12(7), D12(8), D12(9), D12(10), D12(11)
#undef D12
};

/* MIDI note (Q16) -> phase increment. One octave of table (notes 132..144) shifted down. */
uint32_t sc_pitch_inc(int32_t n)
{
    if (n < 0) n = 0;
    if (n >= (144 << 16)) n = (144 << 16) - 1;
    uint32_t u = (uint32_t)n;
    uint32_t oct = SC_DIV12[u >> 16];           /* a table: the compiler makes u / (12 << 16) a divide */
    uint32_t r = u - oct * (12u << 16);
    uint32_t i = r >> 10, fr = r & 1023u;
    uint32_t a = SC_PITCH_T[i], b = SC_PITCH_T[i + 1];
    return (a + (uint32_t)(((uint64_t)(b - a) * fr) >> 10)) >> (11u - oct);
}

/* cutoff note (Q16, 0..132) -> g = tan(pi fc / fs), Q28 */
static uint32_t cut_g(int32_t n)
{
    uint32_t u = (uint32_t)n, i = u >> 13, fr = u & 8191u;
    uint32_t a = SC_CUT_G[i], b = SC_CUT_G[i + 1];
    return a + (uint32_t)(((uint64_t)(b - a) * fr) >> 13);
}

/* ---- prep: what a layer's voices derive from its patch (VoiceCore's per-sample constants) ----- */

static uint32_t env_inc(int32_t dist_q30, int32_t t_q16)       /* dist / (max(t, 1e-4) * fs) */
{
    uint32_t n = sc_samples_q8(t_q16 < 7 ? 7 : t_q16);          /* 1e-4 s = 6.55 in Q16 */
    return sc_udiv64((uint64_t)(uint32_t)dist_q30 << 8, n);
}

static int32_t q16_to_q30(int32_t v) { return v >= SC_ONE16 ? SC_ONE30 : v <= 0 ? 0 : v << 14; }

void sc_prep(sc_prep_t *p, const sc_layer_t *L, const sc_perf_t *jp)
{
    const int32_t *m = L->mm;
    p->L = L;
    /* Adsr: attack 1/(a fs), decay (1 - s)/(d fs); release is set at note-off from its level */
    p->fs = q16_to_q30(L->env_f.s);
    p->as = q16_to_q30(L->env_a.s);
    p->fa_inc = env_inc(SC_ONE30, L->env_f.a);
    p->fd_inc = env_inc(SC_ONE30 - p->fs, L->env_f.d);
    p->aa_inc = env_inc(SC_ONE30, L->env_a.a);
    p->ad_inc = env_inc(SC_ONE30 - p->as, L->env_a.d);
    p->fr_q8 = sc_samples_q8(L->env_f.r < 7 ? 7 : L->env_f.r);
    p->ar_q8 = sc_samples_q8(L->env_a.r < 7 ? 7 : L->env_a.r);

    /* Lfo: dt = rate / fs; the delay ramp reaches 1 after `delay` seconds */
    for (int k = 0; k < 2; k++) {
        const sc_lfo_p_t *lp = &L->lfo[k];
        uint32_t rate = lp->rate < 0 ? 0 : (uint32_t)lp->rate;
        p->lfo_inc[k] = (uint32_t)(((uint64_t)rate * 398915783u) >> 28);   /* 2^44 / 44100 */
        p->lfo_dly_inc[k] = lp->delay > 0 ? env_inc(SC_ONE30, lp->delay) : 0;
        p->lfo_joy[k] = SC_ONE16 + sc_mul(m[k ? SC_MM_JX_L2 : SC_MM_JX_L1], jp->joy_x, 16);
    }

    p->semi1 = L->o1_oct * (12 << 16);
    p->semi2 = L->o2_oct * (12 << 16) + sc_mul(L->o2_detune, 655, 16);   /* cents / 100 (655 = 0.01 Q16) */

    /* the stick, as VoiceCore's joyPitch: up bends pitch when held left, LFO2 into pitch held left */
    int32_t left = SC_ONE16 - jp->joy_x;                                 /* 1 - joyX */
    p->joy_pitch = sc_mul(sc_mul(sc_mul(jp->joy_y, jp->bend_osc, 16), 7 * left, 16), SC_ONE16 / 2, 16);
    int32_t neg_x = jp->joy_x < 0 ? -jp->joy_x : 0;
    int32_t joy_l2 = 2 * sc_mul(neg_x, jp->lfo2_osc, 16);

    int32_t a1 = L->lfo[0].depth_a, a2 = L->lfo[1].depth_a, b1 = L->lfo[0].depth_b, b2 = L->lfo[1].depth_b;
    p->kp1[0] = sc_mul(12 * m[SC_MM_L1_P1], a1, 16);
    p->kp1[1] = sc_mul(12 * m[SC_MM_L2_P1], a2, 16) + joy_l2;
    p->kp2[0] = sc_mul(12 * m[SC_MM_L1_P2], a1, 16);
    p->kp2[1] = sc_mul(12 * m[SC_MM_L2_P2], a2, 16) + joy_l2;
    p->kp2e = 24 * m[SC_MM_E1_P2];

    /* PWM: 0.45 per unit (29491 = 0.45 Q16) */
    p->kw1[0] = sc_mul(sc_mul(m[SC_MM_L1_PW1], a1, 16), 29491, 16);
    p->kw1[1] = sc_mul(sc_mul(m[SC_MM_L2_PW1], a2, 16), 29491, 16);
    p->kw2[0] = sc_mul(sc_mul(m[SC_MM_L1_PW2], a1, 16), 29491, 16);
    p->kw2[1] = sc_mul(sc_mul(m[SC_MM_L2_PW2], a2, 16), 29491, 16);
    p->cross = sc_mul(L->cross, 29491, 16);
    p->cross2 = sc_mul(L->cross2, 29491, 16);
    p->pw1 = L->o1_pwm;
    p->pw2 = L->o2_pwm;

    p->ka[0] = sc_mul(m[SC_MM_L1_AMP], b1, 16);
    p->ka[1] = sc_mul(m[SC_MM_L2_AMP], b2, 16);

    /* cutoff (VoiceCore.updateCoeffs), in semitones: the static part and the per-source scales */
    int32_t right = SC_ONE16 + jp->joy_x;                                /* 1 + joyX */
    int32_t pos_x = jp->joy_x > 0 ? jp->joy_x : 0;
    p->c_base = (24 << 16) + 96 * L->cutoff
              + 48 * sc_mul(m[SC_MM_JY_CUT], jp->joy_y, 16)
              + sc_mul(60 * sc_mul(jp->joy_y, jp->bend_filt, 16), right / 2, 16);
    p->kc[0] = 60 * sc_mul(m[SC_MM_L1_CUT], b1, 16);
    p->kc[1] = 60 * sc_mul(m[SC_MM_L2_CUT], b2, 16) + 60 * sc_mul(pos_x, jp->lfo2_filt, 16);
    p->kc_env = 60 * L->env_amt;
    p->kc_env1 = 96 * m[SC_MM_E1_CUT];
    p->kc_vel = 60 * m[SC_MM_VEL_CUT];
    p->kc_key = L->key_track;

    /* Q = 0.5 + res^2 * 24.5; the SVF takes k = 1/Q */
    int32_t r2 = sc_mul(L->reso, L->reso, 16);
    uint32_t q = (uint32_t)(SC_ONE16 / 2 + sc_mul(r2, SC_Q16(24.5), 16));
    p->k_q28 = (int32_t)sc_udiv64((uint64_t)1 << 44, q);
    p->atten = SC_ONE16 - sc_mul(L->reso, SC_Q16(0.6), 16);

    p->mix1 = L->mix_o1;
    p->mix2 = L->mix_o2;
    p->mixn = L->mix_noise;

    /* glide: portamento 1 - e^(-1/(max(t,1e-4) fs)); swoop decay e^(-1/(max(s,1e-3) fs)) */
    p->glide_coef = SC_ONE30 - sc_exp_neg_inv(sc_samples_q8(L->glide_time < 7 ? 7 : L->glide_time));
#define SPEED(s) sc_exp_neg_inv(sc_samples_q8((s) < 66 ? 66 : (s)))        /* 1e-3 s = 65.5 */
    p->gdc1 = SPEED(L->o1_glide_speed);
    p->gdc2 = SPEED(L->o2_glide_speed);
    p->gdc_panel = SPEED(L->glide_time);
#undef SPEED

    int sq1 = L->o1_wave == SC_WAVE_SQR, sq2 = L->o2_wave == SC_WAVE_SQR;
    p->use = 0;
    for (int k = 0; k < 2; k++)
        if (p->kp1[k] || p->kp2[k] || p->kc[k] || p->ka[k] || (sq1 && p->kw1[k]) || (sq2 && p->kw2[k]))
            p->use |= k ? SC_USE_L2 : SC_USE_L1;
    if (p->kp1[0] || p->kp1[1]) p->use |= SC_USE_P1MOD;
    if (p->kp2[0] || p->kp2[1] || p->kp2e) p->use |= SC_USE_P2MOD;
    if (p->ka[0] || p->ka[1]) p->use |= SC_USE_AMP;
    if (p->mixn || L->o2_wave == SC_WAVE_NOISE) p->use |= SC_USE_NOISE;
}

/* ---- building blocks ------------------------------------------------------------------------ */

/* Adsr.step: linear segments */
static inline int32_t env_step(sc_env_t *e, uint32_t a_inc, uint32_t d_inc, int32_t s)
{
    switch (e->stage) {
    case ENV_ATTACK:
        e->level += (int32_t)a_inc;
        if (e->level >= SC_ONE30) { e->level = SC_ONE30; e->stage = ENV_DECAY; }
        return e->level;
    case ENV_DECAY:
        e->level -= (int32_t)d_inc;
        if (e->level <= s) { e->level = s; e->stage = ENV_SUSTAIN; }
        return e->level;
    case ENV_SUSTAIN:
        return s;
    case ENV_RELEASE:
        e->level -= (int32_t)e->rel_inc;
        if (e->level <= 0) { e->level = 0; e->stage = ENV_IDLE; }
        return e->level;
    default:
        return 0;
    }
}

static void env_off(sc_env_t *e, uint32_t r_q8)
{
    if (e->stage == ENV_IDLE) return;
    e->rel_inc = sc_udiv64((uint64_t)(uint32_t)e->level << 8, r_q8);
    e->stage = ENV_RELEASE;
}

/* env_step over m samples, each value into out[] (Adsr.step's segments as tight loops) */
static void env_run(sc_env_t *e, uint32_t ai, uint32_t di, int32_t s, int32_t *out, uint32_t m)
{
    int32_t lv = e->level;
    uint32_t i = 0;
    while (i < m) {
        switch (e->stage) {
        case ENV_ATTACK:
            while (i < m) {
                lv += (int32_t)ai;
                if (lv >= SC_ONE30) { out[i++] = lv = SC_ONE30; e->stage = ENV_DECAY; break; }
                out[i++] = lv;
            }
            break;
        case ENV_DECAY:
            while (i < m) {
                lv -= (int32_t)di;
                if (lv <= s) { out[i++] = lv = s; e->stage = ENV_SUSTAIN; break; }
                out[i++] = lv;
            }
            break;
        case ENV_SUSTAIN:
            while (i < m) out[i++] = s;
            break;
        case ENV_RELEASE:
            while (i < m) {
                lv -= (int32_t)e->rel_inc;
                if (lv <= 0) { out[i++] = lv = 0; e->stage = ENV_IDLE; break; }
                out[i++] = lv;
            }
            break;
        default:
            while (i < m) out[i++] = 0;
            break;
        }
    }
    e->level = lv;
}

/* the same m steps without their values: each linear segment in one jump */
static void env_skip(sc_env_t *e, uint32_t ai, uint32_t di, int32_t s, uint32_t m)
{
    while (m) {
        int32_t lv = e->level;
        uint32_t step, to;                      /* steps until the segment ends (it ends on that step) */
        switch (e->stage) {
        case ENV_ATTACK:  step = ai; to = (uint32_t)(SC_ONE30 - lv); break;
        case ENV_DECAY:   step = di; to = (uint32_t)(lv - s); break;
        case ENV_RELEASE: step = e->rel_inc; to = (uint32_t)lv; break;
        default:          return;               /* sustain and idle do not move */
        }
        uint32_t j = step ? (to + step - 1) / step : 0xFFFFFFFFu;
        if (e->stage != ENV_DECAY && j == 0) j = 1;
        if (e->stage == ENV_DECAY && lv <= s) j = 1;
        if (j > m) {                            /* the segment goes on past these m samples */
            e->level = e->stage == ENV_ATTACK ? lv + (int32_t)(step * m) : lv - (int32_t)(step * m);
            return;
        }
        m -= j;
        if (e->stage == ENV_ATTACK) { e->level = SC_ONE30; e->stage = ENV_DECAY; }
        else if (e->stage == ENV_DECAY) { e->level = s; e->stage = ENV_SUSTAIN; }
        else { e->level = 0; e->stage = ENV_IDLE; }
    }
}

/* Lfo.step: Q20 output, scaled by the delay ramp */
static inline int32_t lfo_step(sc_lfo_t *l, uint32_t inc, int shape, uint32_t dly_inc)
{
    int32_t raw;
    uint32_t ph = l->ph;
    switch (shape) {
    case SC_LFO_TRI: {
        int32_t p20 = (int32_t)(ph >> 12);
        raw = ph < 0x80000000u ? 4 * p20 - SC_ONE20 : 3 * SC_ONE20 - 4 * p20;
        break;
    }
    case SC_LFO_SQR:
    case SC_LFO_SQRU: raw = ph < 0x80000000u ? SC_ONE20 : -SC_ONE20; break;
    case SC_LFO_SH:   raw = l->sh; break;
    case SC_LFO_RND:  raw = l->sh + (int32_t)(((int64_t)(l->sh_next - l->sh) * (ph >> 2)) >> 30); break;
    default:          raw = (int32_t)(ph >> 11) - SC_ONE20; break;           /* ramp */
    }
    l->smooth += (raw - l->smooth) >> 1;
    l->ph = ph + inc;
    if (l->ph < ph) { l->sh = l->sh_next; l->sh_next = sc_rand20(&l->rng); }
    if (!dly_inc) return l->smooth;
    if (l->ramp < SC_ONE30) {
        l->ramp += (int32_t)dly_inc;
        if (l->ramp >= SC_ONE30) return l->smooth;
        return sc_mul(l->smooth, l->ramp, 30);
    }
    return l->smooth;
}

static void lfo_trigger(sc_lfo_t *l)
{
    l->ph = 0;
    l->ramp = 0;
    l->sh = sc_rand20(&l->rng);
    l->sh_next = sc_rand20(&l->rng);
}

/* a / b in Q16 for 0 <= a < b, by a 32-bit division on both scaled to 16 bits */
static inline int32_t frac16(uint32_t a, uint32_t b)
{
    while (b >= 0x10000u) { a >>= 1; b >>= 1; }
    return (int32_t)((a << 16) / b);
}

/* polyBlep(t, dt), Q20 */
static inline int32_t blep(uint32_t t, uint32_t dt)
{
    if (t < dt) {
        int32_t x = frac16(t, dt);                                   /* 2x - x^2 - 1 */
        return (2 * x - (int32_t)(((int64_t)x * x) >> 16) - SC_ONE16) * 16;
    }
    uint32_t u = 0u - t;                                             /* 1 - t */
    if (u < dt) {
        int32_t x = frac16(u, dt);                                   /* x = -(t - 1)/dt: x^2 - 2x + 1 */
        return ((int32_t)(((int64_t)x * x) >> 16) - 2 * x + SC_ONE16) * 16;
    }
    return 0;
}

static inline void advance(sc_osc_t *o, uint32_t dt)
{
    uint32_t ph = o->ph;
    o->ph = ph + dt;
    o->wrapped = o->ph < ph;
}

static inline int32_t osc_saw(sc_osc_t *o, uint32_t dt)
{
    int32_t s = (int32_t)(o->ph >> 11) - SC_ONE20 - blep(o->ph, dt);
    advance(o, dt);
    return s;
}

static inline int32_t osc_square(sc_osc_t *o, uint32_t dt, uint32_t pw)
{
    int32_t s = o->ph < pw ? SC_ONE20 : -SC_ONE20;
    s += blep(o->ph, dt);
    s -= blep(o->ph + (0u - pw), dt);                                /* phase + (1 - pwm), wrapped */
    advance(o, dt);
    return s;
}

/* PolyBlepTriangle: a leaky integrator (0.999) of a band-limited square, gain 4 dt */
static inline int32_t osc_triangle(sc_osc_t *o, uint32_t dt)
{
    int32_t sq = osc_square(o, dt, 0x80000000u);
    o->z = sc_mul(o->z, 1072668082, 30) + (int32_t)(((int64_t)sq * dt) >> 30);
    return o->z;
}

static inline int32_t osc_sine(sc_osc_t *o, uint32_t dt)
{
    uint32_t i = o->ph >> 22, fr = (o->ph >> 6) & 0xFFFFu;
    int32_t a = SC_SINE[i], b = SC_SINE[i + 1];
    advance(o, dt);
    return a * 32 + (int32_t)(((int64_t)(b - a) * fr) >> 11);
}

static inline int32_t osc_tick(sc_osc_t *o, int wave, uint32_t dt, uint32_t pw, uint32_t *noise)
{
    switch (wave) {
    case SC_WAVE_SAW:   return osc_saw(o, dt);
    case SC_WAVE_SQR:   return osc_square(o, dt, pw);
    case SC_WAVE_TRI:   return osc_triangle(o, dt);
    case SC_WAVE_SIN:   return osc_sine(o, dt);
    default:            return sc_rand20(noise);   /* noise: the oscillator does not run (no wrap) */
    }
}

static void osc_reset(sc_osc_t *o) { o->ph = 0; o->z = 0; o->wrapped = 0; }

/* PinkNoise: Voss-McCartney, 16 bands, the sum / 8 */
static void pink_init(sc_pink_t *p, uint32_t seed)
{
    p->w = seed ? seed : 1u;
    p->sum = 0;
    for (int i = 0; i < 16; i++) { p->bands[i] = sc_rand20(&p->w); p->sum += p->bands[i]; }
    p->counter = 1;
}

static inline int32_t pink_next(sc_pink_t *p)
{
    uint32_t c = p->counter;
    int bit = 0;
    while (!(c & 1u) && bit < 15) { c >>= 1; bit++; }
    p->sum -= p->bands[bit];
    p->bands[bit] = sc_rand20(&p->w);
    p->sum += p->bands[bit];
    p->counter++;
    return p->sum >> 3;
}

/* ZdfSvf in Andy Simper's form (the same filter, coefficients that fit Q30): v1 = bp, v2 = lp */
static inline void svf_step(sc_svf_t *f, int32_t in, int32_t a1, int32_t a2, int32_t a3,
                            int32_t *lp, int32_t *bp)
{
    int32_t v3 = in - f->ic2;
    int32_t v1 = (int32_t)(((int64_t)a1 * f->ic1 + (int64_t)a2 * v3) >> 30);
    int32_t v2 = f->ic2 + (int32_t)(((int64_t)a2 * f->ic1 + (int64_t)a3 * v3) >> 30);
    f->ic1 = 2 * v1 - f->ic1;
    f->ic2 = 2 * v2 - f->ic2;
    *lp = v2;
    *bp = v1;
}

/* ---- the voice ------------------------------------------------------------------------------ */

void sc_voice_init(sc_voice_t *v, uint32_t seed)
{
    uint8_t *b = (uint8_t *)v;
    for (uint32_t i = 0; i < sizeof *v; i++) b[i] = 0;
    v->noise1 = seed ? seed : 1u;
    v->noise2 = 0xdeadbeefu;                    /* VoiceCore's noiseW2 and PinkNoise seeds */
    pink_init(&v->pink, 0xfeedfaceu);
    v->lfo1.rng = v->lfo2.rng = 0x9e3779b1u;
    v->cur = v->target = (int64_t)60 << 32;
    v->n1 = v->n2 = INT32_MIN;                  /* no cached pitch yet */
    v->vel = SC_ONE16;
    v->a1 = SC_ONE30;                           /* filled in at the first block */
}

void sc_voice_note_on(sc_voice_t *v, const sc_prep_t *p, int32_t note, int32_t vel_q16)
{
    const sc_layer_t *L = p->L;
    int from_idle = v->env2.stage == ENV_IDLE;
    v->target = (int64_t)note << 32;
    if (!(L->glide_mode == SC_GLIDE_PORTA && !from_idle)) v->cur = v->target;
    v->vel = vel_q16;
    if (from_idle || L->multi_trigger) { v->env1.stage = ENV_ATTACK; v->env2.stage = ENV_ATTACK; }
    if (L->lfo[0].sync) lfo_trigger(&v->lfo1);
    if (L->lfo[1].sync) lfo_trigger(&v->lfo2);
    if (!L->o2_sync) osc_reset(&v->o1);
    osc_reset(&v->o2);

    v->off1 = L->o1_glide_amt * 256;
    v->off2 = L->o2_glide_amt * 256;
    v->c1 = p->gdc1;
    v->c2 = p->gdc2;
    if (L->glide_mode == SC_GLIDE_GLIDE) {
        if (L->glide_o1) { v->off1 += L->glide_amt * 256; v->c1 = p->gdc_panel; }
        if (L->glide_o2) { v->off2 += L->glide_amt * 256; v->c2 = p->gdc_panel; }
    }
}

void sc_voice_note_off(sc_voice_t *v, const sc_prep_t *p)
{
    env_off(&v->env1, p->fr_q8);
    env_off(&v->env2, p->ar_q8);
}

/* VoiceCore.updateCoeffs */
static void update_coeffs(sc_voice_t *v, const sc_prep_t *p)
{
    int32_t e1 = v->env1.level;
    int64_t c = p->c_base;
    c += ((int64_t)v->l1 * p->kc[0] + (int64_t)v->l2 * p->kc[1]) >> 20;
    c += ((int64_t)(v->vel - SC_ONE16 / 2) * p->kc_vel) >> 16;
    c += ((int64_t)e1 * p->kc_env) >> 30;
    c += ((int64_t)(e1 - SC_ONE30 / 2) * p->kc_env1) >> 30;
    c += ((int64_t)((int32_t)(v->cur >> 16) - (60 << 16)) * p->kc_key) >> 16;
    int32_t cn = c < 0 ? 0 : c > (132 << 16) ? (132 << 16) : (int32_t)c;

    /* a1 = 1 / (1 + g (g + k)), a2 = g a1, a3 = g a2 */
    uint32_t g = cut_g(cn);
    uint64_t d28 = ((uint64_t)1 << 28) + (((uint64_t)g * (g + (uint32_t)p->k_q28)) >> 28);
    v->a1 = (int32_t)sc_udiv64((uint64_t)1 << 54, (uint32_t)(d28 >> 4));
    v->a2 = (int32_t)(((int64_t)v->a1 * g) >> 28);
    v->a3 = (int32_t)(((int64_t)v->a2 * g) >> 28);
}

/* Everything per sample, in VoiceCore's order: the path for OSC2 -> OSC1 cross-mod, where each
 * oscillator's pulse width needs the other's last sample and the stages below cannot be split. */
static void render_general(sc_voice_t *v, const sc_prep_t *p, int32_t *out, uint32_t n)
{
    const sc_layer_t *L = p->L;
    const int w1 = L->o1_wave, w2 = L->o2_wave, mode = L->flt_mode;
    const int sh1 = L->lfo[0].shape, sh2 = L->lfo[1].shape;
    const uint32_t use = p->use;
    const int32_t vgain = (SC_ONE16 + sc_mul(L->mm[SC_MM_VEL_AMP], v->vel - SC_ONE16, 16)) / 2;   /* velAmp * 0.5 */

    for (uint32_t i = 0; i < n; i++) {
        if (v->block == 0) update_coeffs(v, p);
        if (++v->block == SC_COEFF_BLOCK) v->block = 0;

        if (v->cur != v->target)                /* Q30 coefficient x the distance in Q24 -> Q32 */
            v->cur += ((int64_t)p->glide_coef * (int32_t)((v->target - v->cur) >> 8)) >> 22;

        int32_t e1 = env_step(&v->env1, p->fa_inc, p->fd_inc, p->fs);
        int32_t e2 = env_step(&v->env2, p->aa_inc, p->ad_inc, p->as);
        int32_t l1 = 0, l2 = 0;
        if (use & SC_USE_L1) {
            l1 = lfo_step(&v->lfo1, p->lfo_inc[0], sh1, p->lfo_dly_inc[0]);
            if (p->lfo_joy[0] != SC_ONE16) l1 = sc_mul(l1, p->lfo_joy[0], 16);
            v->l1 = l1;
        }
        if (use & SC_USE_L2) {
            l2 = lfo_step(&v->lfo2, p->lfo_inc[1], sh2, p->lfo_dly_inc[1]);
            if (p->lfo_joy[1] != SC_ONE16) l2 = sc_mul(l2, p->lfo_joy[1], 16);
            v->l2 = l2;
        }

        if (v->off1) v->off1 = (int32_t)(((int64_t)v->off1 * v->c1 + (1 << 29)) >> 30);   /* rounded: no drift */
        if (v->off2) v->off2 = (int32_t)(((int64_t)v->off2 * v->c2 + (1 << 29)) >> 30);

        int32_t note = (int32_t)(v->cur >> 16) + v->bend + p->joy_pitch;
        int32_t n1 = note + p->semi1 + (v->off1 >> 8);
        int32_t n2 = note + p->semi2 + (v->off2 >> 8);
        if (use & SC_USE_P1MOD)
            n1 += (int32_t)(((int64_t)l1 * p->kp1[0] + (int64_t)l2 * p->kp1[1]) >> 20);
        if (use & SC_USE_P2MOD)
            n2 += (int32_t)(((int64_t)l1 * p->kp2[0] + (int64_t)l2 * p->kp2[1]) >> 20)
                + (int32_t)(((int64_t)(e1 - SC_ONE30 / 2) * p->kp2e) >> 30);
        if (n1 != v->n1) {                      /* the table only when the pitch moved */
            v->n1 = n1;
            v->dt1 = sc_pitch_inc(n1);
            if (v->dt1 > MAX_DT) v->dt1 = MAX_DT;
        }
        if (n2 != v->n2) {
            v->n2 = n2;
            v->dt2 = sc_pitch_inc(n2);
            if (v->dt2 > MAX_DT) v->dt2 = MAX_DT;
        }
        uint32_t dt1 = v->dt1, dt2 = v->dt2;

        /* pulse widths: only a square uses one (the triangle's square is fixed at 0.5) */
        uint32_t pw1 = 0x80000000u, pw2 = 0x80000000u;
        if (w1 == SC_WAVE_SQR)
            pw1 = (uint32_t)sc_clamp(p->pw1 + (int32_t)(((int64_t)l1 * p->kw1[0] + (int64_t)l2 * p->kw1[1]
                                     + (int64_t)v->prev2 * p->cross2) >> 20), PW_LO, PW_HI) << 16;
        if (w2 == SC_WAVE_SQR)
            pw2 = (uint32_t)sc_clamp(p->pw2 + (int32_t)(((int64_t)l1 * p->kw2[0] + (int64_t)l2 * p->kw2[1]
                                     + (int64_t)v->prev1 * p->cross) >> 20), PW_LO, PW_HI) << 16;

        int32_t s1 = osc_tick(&v->o1, w1, dt1, pw1, &v->noise1);
        if (L->o2_sync && w1 != SC_WAVE_NOISE && v->o1.wrapped) v->o2.ph = 0;
        int32_t s2 = osc_tick(&v->o2, w2, dt2, pw2, &v->noise2);
        /* white noise shares OSC2's generator, as noiseW2 does */
        int32_t nz = 0;
        if (use & SC_USE_NOISE) nz = L->noise_pink ? pink_next(&v->pink) : sc_rand20(&v->noise2);

        int32_t s2m = L->ring ? sc_mul(s1, s2, 20) : s2;
        int32_t mixed = (int32_t)(((int64_t)s1 * p->mix1 + (int64_t)s2m * p->mix2 + (int64_t)nz * p->mixn) >> 16);
        mixed = sc_mul(mixed, p->atten, 16);

        int32_t lp, bp, y;
        svf_step(&v->f1, mixed, v->a1, v->a2, v->a3, &lp, &bp);
        switch (mode) {
        case SC_FLT_LP24: { int32_t bp2; svf_step(&v->f2, lp, v->a1, v->a2, v->a3, &y, &bp2); break; }
        case SC_FLT_LP12: y = lp; break;
        case SC_FLT_HP12: y = mixed - sc_mul(p->k_q28, bp, 28) - lp; break;
        case SC_FLT_BP12: y = 2 * bp; break;
        default:          y = bp; break;
        }
        T(0, e1); T(1, e2); T(2, l1); T(3, l2); T(4, dt1); T(5, dt2); T(6, s1); T(7, s2);
        T(8, mixed); T(9, y); T(10, v->a1); T(11, v->f1.ic1);
        T_NEXT();

        int32_t amp = e2 >> 14;                                              /* Q16 */
        if (use & SC_USE_AMP)
            amp = (int32_t)(((int64_t)amp * (SC_ONE16 + (int32_t)(((int64_t)l1 * p->ka[0] + (int64_t)l2 * p->ka[1]) >> 20))) >> 16);
        amp = sc_mul(amp, vgain, 16);
        out[i] += sc_mul(y, amp, 16);

        v->prev1 = s1;
        v->prev2 = s2;
    }
}

/* ---- the staged render ---------------------------------------------------------------------- */
/* The same arithmetic as render_general, sample for sample, in passes over the block: control
 * (envelopes, LFOs, pitch, pulse width, gain), OSC1, OSC2, then mix + filter. Each pass is a tight
 * loop whose state fits the registers; one loop doing everything spilled ~120 values per sample. */
static int32_t  S_amp[256], S_s1[256], S_s2[256], S_nz[256], S_e1[256], S_l1[256], S_l2[256];
static const int32_t S_zero[256];              /* an LFO that is not routed reads as 0 */
static uint32_t S_dt1[256], S_dt2[256], S_pw1[256];
static int64_t  S_pw2[256];                     /* OSC2's PWM sum before OSC1's cross-mod term */
static uint8_t  S_wrap[256];

/* one oscillator over the block, its waveform a constant so each case compiles to its own loop.
 * reset: OSC1's wraps (hard sync) or 0. pwacc / xs / xk / pwbase: OSC2's pulse width with OSC1 ->
 * OSC2 cross-mod (else pw[], else 50 %). nz: OSC2 as noise shares its generator with the mixer's
 * white noise, drawn in VoiceCore's order (OSC2's sample first). */
typedef struct {
    const uint32_t *dt, *pw;
    const int64_t *pwacc;
    const int32_t *xs;
    int32_t xk, pwbase;
    uint32_t *noise;
    int32_t *s, *nz;
    uint8_t *wrap;
    const uint8_t *reset;
} osc_io_t;

static inline __attribute__((always_inline))
void osc_loop(sc_osc_t *o, const int wave, const osc_io_t *io, uint32_t n)
{
    uint32_t ph = o->ph;
    int32_t z = o->z;
    uint8_t w = o->wrapped;
    const uint32_t *dt = io->dt;
    int32_t *s = io->s;
    uint8_t *wrap = io->wrap;
    const uint8_t *reset = io->reset;
    if (wave == SC_WAVE_NOISE) {                /* the phase does not run */
        for (uint32_t i = 0; i < n; i++) {
            s[i] = sc_rand20(io->noise);
            if (io->nz) io->nz[i] = sc_rand20(io->noise);
            if (wrap) wrap[i] = 0;
        }
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t d = dt[i];
        if (reset && reset[i]) ph = 0;
        int32_t y;
        if (wave == SC_WAVE_SAW) {
            y = (int32_t)(ph >> 11) - SC_ONE20 - blep(ph, d);
        } else if (wave == SC_WAVE_SQR) {
            uint32_t q;
            if (io->pwacc)                      /* OSC2 with OSC1 -> OSC2 cross-mod */
                q = (uint32_t)sc_clamp(io->pwbase + (int32_t)((io->pwacc[i] + (int64_t)io->xs[i] * io->xk) >> 20),
                                       PW_LO, PW_HI) << 16;
            else
                q = io->pw ? io->pw[i] : 0x80000000u;
            y = (ph < q ? SC_ONE20 : -SC_ONE20) + blep(ph, d) - blep(ph + (0u - q), d);
        } else if (wave == SC_WAVE_TRI) {
            int32_t sq = (ph < 0x80000000u ? SC_ONE20 : -SC_ONE20) + blep(ph, d) - blep(ph + 0x80000000u, d);
            z = sc_mul(z, 1072668082, 30) + (int32_t)(((int64_t)sq * d) >> 30);
            y = z;
        } else {
            uint32_t k = ph >> 22, fr = (ph >> 6) & 0xFFFFu;
            int32_t a = SC_SINE[k], b = SC_SINE[k + 1];
            y = a * 32 + (int32_t)(((int64_t)(b - a) * fr) >> 11);
        }
        s[i] = y;
        uint32_t np = ph + d;
        w = np < ph;
        if (wrap) wrap[i] = w;
        ph = np;
    }
    o->ph = ph;
    o->z = z;
    o->wrapped = w;
}

static void osc_run(sc_osc_t *o, int wave, const osc_io_t *io, uint32_t n)
{
    switch (wave) {
    case SC_WAVE_SAW: osc_loop(o, SC_WAVE_SAW, io, n); break;
    case SC_WAVE_SQR: osc_loop(o, SC_WAVE_SQR, io, n); break;
    case SC_WAVE_TRI: osc_loop(o, SC_WAVE_TRI, io, n); break;
    case SC_WAVE_SIN: osc_loop(o, SC_WAVE_SIN, io, n); break;
    default:          osc_loop(o, SC_WAVE_NOISE, io, n); break;
    }
}

/* mix, filter and gain over [a, b), in passes over S_mix: one loop doing all of it held more
 * values than pi32v2 has registers (15 spills a sample); each pass here fits */
typedef struct { int32_t m1, m2, mn, at, k; int ring; } mix_t;
static int32_t S_mix[256];

static void svf_pass(sc_svf_t *f, const int32_t *c, int32_t *x, int out_bp, uint32_t a, uint32_t b)
{
    const int32_t a1 = c[0], a2 = c[1], a3 = c[2];
    int32_t ic1 = f->ic1, ic2 = f->ic2;
    for (uint32_t i = a; i < b; i++) {
        int32_t v3 = x[i] - ic2;
        int32_t v1 = (int32_t)(((int64_t)a1 * ic1 + (int64_t)a2 * v3) >> 30);
        int32_t v2 = ic2 + (int32_t)(((int64_t)a2 * ic1 + (int64_t)a3 * v3) >> 30);
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        x[i] = out_bp ? v1 : v2;
    }
    f->ic1 = ic1;
    f->ic2 = ic2;
}

static void mix_run(sc_voice_t *v, int mode, const mix_t *m, const int32_t *c, int32_t *out, uint32_t a, uint32_t b)
{
    const int32_t m1 = m->m1, m2 = m->m2, mn = m->mn, at = m->at;
    int32_t *x = S_mix;
    if (mn)                                     /* the noise joins the same sum before its shift */
        for (uint32_t i = a; i < b; i++) {
            int64_t acc = (int64_t)S_s1[i] * m1 + (int64_t)(m->ring ? sc_mul(S_s1[i], S_s2[i], 20) : S_s2[i]) * m2
                        + (int64_t)S_nz[i] * mn;
            x[i] = (int32_t)(acc >> 16);
        }
    else if (m->ring)
        for (uint32_t i = a; i < b; i++)
            x[i] = (int32_t)(((int64_t)S_s1[i] * m1 + (int64_t)sc_mul(S_s1[i], S_s2[i], 20) * m2) >> 16);
    else
        for (uint32_t i = a; i < b; i++)
            x[i] = (int32_t)(((int64_t)S_s1[i] * m1 + (int64_t)S_s2[i] * m2) >> 16);
    for (uint32_t i = a; i < b; i++) x[i] = sc_mul(x[i], at, 16);

    switch (mode) {
    case SC_FLT_LP24:
        svf_pass(&v->f1, c, x, 0, a, b);
        svf_pass(&v->f2, c, x, 0, a, b);
        break;
    case SC_FLT_LP12:
        svf_pass(&v->f1, c, x, 0, a, b);
        break;
    case SC_FLT_HP12: {                         /* hp = in - k bp - lp: needs the input and both outputs */
        const int32_t a1 = c[0], a2 = c[1], a3 = c[2], k = m->k;
        int32_t ic1 = v->f1.ic1, ic2 = v->f1.ic2;
        for (uint32_t i = a; i < b; i++) {
            int32_t in = x[i], v3 = in - ic2;
            int32_t v1 = (int32_t)(((int64_t)a1 * ic1 + (int64_t)a2 * v3) >> 30);
            int32_t v2 = ic2 + (int32_t)(((int64_t)a2 * ic1 + (int64_t)a3 * v3) >> 30);
            ic1 = 2 * v1 - ic1;
            ic2 = 2 * v2 - ic2;
            x[i] = in - sc_mul(k, v1, 28) - v2;
        }
        v->f1.ic1 = ic1;
        v->f1.ic2 = ic2;
        break;
    }
    case SC_FLT_BP12:
        svf_pass(&v->f1, c, x, 1, a, b);
        for (uint32_t i = a; i < b; i++) x[i] *= 2;
        break;
    default:
        svf_pass(&v->f1, c, x, 1, a, b);
        break;
    }
    for (uint32_t i = a; i < b; i++) out[i] += sc_mul(x[i], S_amp[i], 16);
}

void sc_voice_render(sc_voice_t *v, const sc_prep_t *p, int32_t *out, uint32_t n)
{
    if (v->env2.stage == ENV_IDLE) return;
#ifndef SC_TEST_GENERAL                         /* tests/run.sh: the general path for every patch */
    if (p->cross2 || n > 256)
#endif
    { render_general(v, p, out, n); return; }
    const sc_layer_t *L = p->L;
    const int w1 = L->o1_wave, w2 = L->o2_wave, mode = L->flt_mode;
    const int sh1 = L->lfo[0].shape, sh2 = L->lfo[1].shape;
    const uint32_t use = p->use;
    const int32_t vgain = (SC_ONE16 + sc_mul(L->mm[SC_MM_VEL_AMP], v->vel - SC_ONE16, 16)) / 2;   /* velAmp * 0.5 */

    /* the filter's coefficient sets and where each starts (one per COEFF_BLOCK, at most 3 here) */
    uint32_t seg_at[4];
    int32_t seg_c[4][3];
    uint32_t nseg = 1;
    seg_at[0] = 0;
    seg_c[0][0] = v->a1; seg_c[0][1] = v->a2; seg_c[0][2] = v->a3;

    /* 1. control, a segment at a time: segments end where VoiceCore recomputes the coefficients,
     * and each starts with that update, in the state the samples before it left */
    const int sq1 = w1 == SC_WAVE_SQR, sq2 = w2 == SC_WAVE_SQR;
    for (uint32_t i0 = 0; i0 < n; ) {
        if (v->block == 0) {
            update_coeffs(v, p);
            uint32_t k = i0 == 0 ? 0 : nseg++;
            seg_at[k] = i0;
            seg_c[k][0] = v->a1; seg_c[k][1] = v->a2; seg_c[k][2] = v->a3;
        }
        uint32_t m = SC_COEFF_BLOCK - v->block;
        if (m > n - i0) m = n - i0;
        uint32_t i1 = i0 + m;
        v->block = (v->block + m) % SC_COEFF_BLOCK;

        env_run(&v->env2, p->aa_inc, p->ad_inc, p->as, S_amp + i0, m);
        if (use & SC_USE_P2MOD) env_run(&v->env1, p->fa_inc, p->fd_inc, p->fs, S_e1 + i0, m);
        else env_skip(&v->env1, p->fa_inc, p->fd_inc, p->fs, m);
        if (use & SC_USE_L1) {
            for (uint32_t i = i0; i < i1; i++) {
                int32_t l = lfo_step(&v->lfo1, p->lfo_inc[0], sh1, p->lfo_dly_inc[0]);
                S_l1[i] = p->lfo_joy[0] != SC_ONE16 ? sc_mul(l, p->lfo_joy[0], 16) : l;
            }
            v->l1 = S_l1[i1 - 1];
        }
        if (use & SC_USE_L2) {
            for (uint32_t i = i0; i < i1; i++) {
                int32_t l = lfo_step(&v->lfo2, p->lfo_inc[1], sh2, p->lfo_dly_inc[1]);
                S_l2[i] = p->lfo_joy[1] != SC_ONE16 ? sc_mul(l, p->lfo_joy[1], 16) : l;
            }
            v->l2 = S_l2[i1 - 1];
        }
        const int32_t *l1 = (use & SC_USE_L1) ? S_l1 : S_zero, *l2 = (use & SC_USE_L2) ? S_l2 : S_zero;

        /* pitch: per sample while something moves it, else once */
        if ((use & (SC_USE_P1MOD | SC_USE_P2MOD)) || v->cur != v->target || v->off1 || v->off2) {
            for (uint32_t i = i0; i < i1; i++) {
                if (v->cur != v->target)
                    v->cur += ((int64_t)p->glide_coef * (int32_t)((v->target - v->cur) >> 8)) >> 22;
                if (v->off1) v->off1 = (int32_t)(((int64_t)v->off1 * v->c1 + (1 << 29)) >> 30);
                if (v->off2) v->off2 = (int32_t)(((int64_t)v->off2 * v->c2 + (1 << 29)) >> 30);
                int32_t note = (int32_t)(v->cur >> 16) + v->bend + p->joy_pitch;
                int32_t n1 = note + p->semi1 + (v->off1 >> 8);
                int32_t n2 = note + p->semi2 + (v->off2 >> 8);
                if (use & SC_USE_P1MOD)
                    n1 += (int32_t)(((int64_t)l1[i] * p->kp1[0] + (int64_t)l2[i] * p->kp1[1]) >> 20);
                if (use & SC_USE_P2MOD)
                    n2 += (int32_t)(((int64_t)l1[i] * p->kp2[0] + (int64_t)l2[i] * p->kp2[1]) >> 20)
                        + (int32_t)(((int64_t)(S_e1[i] - SC_ONE30 / 2) * p->kp2e) >> 30);
                if (n1 != v->n1) { v->n1 = n1; v->dt1 = sc_pitch_inc(n1); if (v->dt1 > MAX_DT) v->dt1 = MAX_DT; }
                if (n2 != v->n2) { v->n2 = n2; v->dt2 = sc_pitch_inc(n2); if (v->dt2 > MAX_DT) v->dt2 = MAX_DT; }
                S_dt1[i] = v->dt1;
                S_dt2[i] = v->dt2;
            }
        } else {
            int32_t note = (int32_t)(v->cur >> 16) + v->bend + p->joy_pitch;
            int32_t n1 = note + p->semi1, n2 = note + p->semi2;
            if (n1 != v->n1) { v->n1 = n1; v->dt1 = sc_pitch_inc(n1); if (v->dt1 > MAX_DT) v->dt1 = MAX_DT; }
            if (n2 != v->n2) { v->n2 = n2; v->dt2 = sc_pitch_inc(n2); if (v->dt2 > MAX_DT) v->dt2 = MAX_DT; }
            uint32_t d1 = v->dt1, d2 = v->dt2;
            for (uint32_t i = i0; i < i1; i++) { S_dt1[i] = d1; S_dt2[i] = d2; }
        }

        if (sq1)
            for (uint32_t i = i0; i < i1; i++)
                S_pw1[i] = (uint32_t)sc_clamp(p->pw1 + (int32_t)(((int64_t)l1[i] * p->kw1[0] + (int64_t)l2[i] * p->kw1[1]) >> 20),
                                              PW_LO, PW_HI) << 16;
        if (sq2)
            for (uint32_t i = i0; i < i1; i++) S_pw2[i] = (int64_t)l1[i] * p->kw2[0] + (int64_t)l2[i] * p->kw2[1];

        if (use & SC_USE_AMP)
            for (uint32_t i = i0; i < i1; i++) {
                int32_t amp = (int32_t)(((int64_t)(S_amp[i] >> 14) * (SC_ONE16 + (int32_t)(((int64_t)l1[i] * p->ka[0] + (int64_t)l2[i] * p->ka[1]) >> 20))) >> 16);
                S_amp[i] = sc_mul(amp, vgain, 16);
            }
        else
            for (uint32_t i = i0; i < i1; i++) S_amp[i] = sc_mul(S_amp[i] >> 14, vgain, 16);
        i0 = i1;
    }

    /* 2. OSC1, keeping its wraps when OSC2 syncs to it; 3. OSC2. xs: OSC1's previous sample, for the
     * OSC1 -> OSC2 PWM cross-mod (S_s1 shifted by one, its first from the last block) */
    static int32_t S_prev1[256];
    int sync = L->o2_sync && w1 != SC_WAVE_NOISE;
    int need_nz = (use & SC_USE_NOISE) && !L->noise_pink;
    int nz_in_osc2 = need_nz && w2 == SC_WAVE_NOISE;
    osc_io_t io1 = { S_dt1, S_pw1, 0, 0, 0, 0, &v->noise1, S_s1, 0, sync ? S_wrap : 0, 0 };
    osc_io_t io2 = { S_dt2, 0, 0, 0, 0, 0, &v->noise2, S_s2, nz_in_osc2 ? S_nz : 0, 0, sync ? S_wrap : 0 };
    S_prev1[0] = v->prev1;
    osc_run(&v->o1, w1, &io1, n);
    if (w2 == SC_WAVE_SQR) {
        for (uint32_t i = 1; i < n; i++) S_prev1[i] = S_s1[i - 1];
        io2.pwacc = S_pw2;
        io2.xs = S_prev1;
        io2.xk = p->cross;
        io2.pwbase = p->pw2;
    }
    osc_run(&v->o2, w2, &io2, n);
    if (use & SC_USE_NOISE) {
        if (L->noise_pink) for (uint32_t i = 0; i < n; i++) S_nz[i] = pink_next(&v->pink);
        else if (!nz_in_osc2) for (uint32_t i = 0; i < n; i++) S_nz[i] = sc_rand20(&v->noise2);
    }
    v->prev1 = S_s1[n - 1];
    v->prev2 = S_s2[n - 1];

    /* 4. mix, filter, gain, per coefficient segment */
    mix_t m = { p->mix1, p->mix2, (use & SC_USE_NOISE) ? p->mixn : 0, p->atten, p->k_q28, L->ring };
    seg_at[nseg] = n;
    for (uint32_t g = 0; g < nseg; g++) mix_run(v, mode, &m, seg_c[g], out, seg_at[g], seg_at[g + 1]);
}
