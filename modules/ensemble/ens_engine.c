/* SPDX-License-Identifier: GPL-3.0-only */
/* The ENSEMBLE engine (ens_engine.h). ens_voice and ens_post are Caravel's ens_render and ens_post (eng_ensemble.c,
 * bonszaimusic, GPL-3.0-only); the inline helpers are Felucca-Engines' dsp.c (MIT); env_tick and the voice start
 * follow Caravel's voice.c. Their tables come from tools/gen_ens_tables.py, made from the same formulas. */
#include "ens_engine.h"

#define ENS_TAIL_BLOCKS 28u                     /* control ticks the taps run on after the last voice: 20 ms > the longest tap */
#define ENS_WET_Q16 26000u                      /* the three taps' mean (1/3) with 1.19 x make-up */
#define ENS_MID_Q16 (ENS_WET_Q16 >> 1)          /* stereo: the mean of both channels' three taps */
#define ENS_VOICE_FS 24000                      /* Felucca's per-voice level */
#define ENS_OUT(x) (((x) * 5) >> 5)             /* the out: -16 dB, Caravel's track and master stages, so a pad sits where
                                                 * ORRERY' Eminent does (tests/native/ensemble_test.c) */
static const int32_t ENS_BASE[ENS_NT] = {265, 375, 485, 300, 445, 560};   /* 6.0 8.5 11.0 ms; 6.8 10.1 12.7 ms */

/* ---- Felucca-Engines' DSP blocks (src/dsp.c, MIT) */
static inline int32_t mulq15(int32_t a, int32_t b) { return (a * b) >> 15; }
static inline int32_t mulq16(int32_t a, uint32_t k)
{
    return (int32_t)(((a >> 16) * (int32_t)k) + (int32_t)(((uint32_t)(a & 0xFFFF) * k) >> 16));
}
static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int32_t sine_i(uint32_t ph)
{
    uint32_t i = ph >> 22;
    int32_t a = ENS_SINE[i], b = ENS_SINE[(i + 1u) & 1023u];
    return a + (((b - a) * (int32_t)((ph >> 7) & 0x7FFFu)) >> 15);
}
static inline int32_t blep(uint32_t ph, uint32_t inc)   /* polyBLEP residual (Q15) around a wrap */
{
    uint32_t d = inc >> 15;
    int32_t x;
    if (!d) return 0;
    if (ph < inc) {
        x = (int32_t)(ph / d);
        return x + x - ((x * x) >> 15) - 32768;
    }
    if (ph > 0xFFFFFFFFu - inc) {
        x = -(int32_t)((0xFFFFFFFFu - ph) / d);
        return ((x * x) >> 15) + x + x + 32768;
    }
    return 0;
}
static inline int32_t osc_saw(uint32_t ph, uint32_t inc) { return ((int32_t)(ph >> 16) - 32768) - blep(ph, inc); }
/* (Felucca's osc_pulse is (osc_saw(ph, inc) - osc_saw(ph + pw, inc)) >> 1: ens_voice makes it of two saw runs) */
static inline int32_t osc_tri(uint32_t ph)
{
    int32_t s = (int32_t)(ph >> 15);
    return s < 65536 ? s - 32768 : 98303 - s;
}
static inline int32_t softclip(int32_t x)
{
    int32_t a = x < 0 ? -x : x, i = a >> 8, y;
    if (i >= 256) y = ENS_TANH[256];
    else y = ENS_TANH[i] + (((ENS_TANH[i + 1] - ENS_TANH[i]) * (a & 255)) >> 8);
    return x < 0 ? -y : y;
}
static inline uint32_t noise32(int32_t *st)
{
    uint32_t s = (uint32_t)*st;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    *st = (int32_t)s;
    return s;
}
typedef struct { int32_t a1, a2, a3; } tsvf_t;
static inline void tsvf_coef_k(tsvf_t *c, int32_t cut, int32_t k)   /* cut: 0..127 << 8, k: damping Q12 */
{
    int32_t i, g, den;
    cut = clamp(cut, 0, 127 << 8);
    i = cut >> 8;
    g = ENS_SVF_G[i];
    if (i < 127) g += ((ENS_SVF_G[i + 1] - g) * (cut & 255)) >> 8;
    den = 4096 + ((g * (g + k)) >> 12);
    c->a1 = (int32_t)((4096u << 13) / (uint32_t)den);
    c->a2 = (c->a1 * g) >> 12;
    c->a3 = (c->a2 * g) >> 12;
}
static inline int32_t soft_knee(int32_t y, int32_t k)
{
    int32_t a = y < 0 ? -y : y;
    if (a <= k) return y;
    a = k + (softclip((a - k) * 2) >> 1);
    return y < 0 ? -a : a;
}

/* ---- the presets: Caravel's ENS_PRESETS and ENS_WIDTH (the design intent of each is in its ensemble-engine.md) */
const ens_preset_t ENS_PRESETS[ENS_N_PRESETS] = {
    {"VINTAGE STRINGS", {64, 0, 80, 40, 70, 50, 10, 100}, {60, 90, 112, 80}, 0, 127},
    {"GLASS CHOIR", {40, 64, 60, 50, 60, 40, 15, 90}, {80, 90, 115, 95}, 0, 127},
    {"DUSK ORCHESTRA", {100, 40, 70, 60, 85, 35, 30, 100}, {75, 95, 115, 90}, 0, 127},
    {"ABYSS PAD", {80, 110, 40, 90, 75, 20, 90, 95}, {100, 100, 120, 105}, 0, 127},
    {"PROFILE A", {120, 15, 80, 60, 70, 71, 31, 126}, {85, 90, 112, 95}, 0, 127},
    {"STRING MACHINE", {64, 0, 127, 25, 100, 22, 0, 127}, {85, 90, 115, 95}, 0, 112},
    {"BREATH CHOIR", {64, 64, 90, 35, 40, 45, 127, 100}, {90, 95, 115, 100}, 0, 96},
    {"DEEP DRONE", {127, 0, 0, 60, 80, 8, 0, 110}, {105, 100, 120, 115}, 0, 112},
    {"GLASS SHIMMER", {64, 127, 127, 15, 10, 60, 127, 100}, {55, 70, 60, 100}, 0, 0},
    {"SLOW SWELL", {90, 10, 70, 40, 80, 10, 0, 100}, {120, 100, 120, 112}, 40, 127},
    {"ROUGH PULSE", {127, 127, 110, 50, 70, 110, 127, 127}, {50, 70, 70, 60}, 0, 0},
    {"SOLO DRIFT", {70, 20, 95, 127, 25, 60, 0, 100}, {60, 90, 112, 70}, 0, 0},   /* (MONO in Caravel; poly here) */
    {"HUGE ENSEMBLE", {64, 0, 110, 50, 127, 6, 0, 127}, {70, 90, 115, 100}, 0, 96},
    {"RUNNING PAD", {40, 30, 100, 40, 127, 127, 0, 127}, {95, 90, 115, 100}, 0, 127},
};

void ens_load(ens_t *e, uint32_t preset)
{
    const ens_preset_t *p = &ENS_PRESETS[preset % ENS_N_PRESETS];
    for (uint32_t k = 0; k < 8u; k++) e->e[k] = p->e[k];
    for (uint32_t k = 0; k < 4u; k++) e->env[k] = p->env[k];
    e->fenv = p->fenv;
    e->width = p->width;
}
void ens_all_off(ens_t *e)
{
    uint8_t *b = (uint8_t *)e->v;
    for (uint32_t k = 0; k < sizeof e->v; k++) b[k] = 0;
    for (uint32_t k = 0; k < ENS_RING; k++) e->ring[k] = 0;
    e->live = 0; e->tail = 0;
}
void ens_init(ens_t *e)
{
    uint8_t *b = (uint8_t *)e;
    for (uint32_t k = 0; k < sizeof *e; k++) b[k] = 0;
    ens_load(e, 0);
}

/* Caravel's ens_note_on, inside its voice_start: a sounding voice (a retrigger or a steal) keeps its phases, its
 * filter and its drift, and its attack starts from the level it has */
void ens_note_on(ens_t *e, uint8_t key, uint8_t vel)
{
    ens_voice_t *v = 0;
    uint32_t i, lane;
    for (i = 0; i < ENS_VOICES && !v; i++)
        if (e->v[i].stage && e->v[i].key == key) v = &e->v[i];       /* the same key again */
    for (i = 0; i < ENS_VOICES && !v; i++)
        if (!e->v[i].stage) v = &e->v[i];
    if (!v) {                                   /* the oldest released one, else the oldest */
        ens_voice_t *best = 0;
        for (i = 0; i < ENS_VOICES; i++)
            if (!e->v[i].gate && (!best || e->v[i].age < best->age)) best = &e->v[i];
        for (i = 0; i < ENS_VOICES && !best; i++)
            if (!best || e->v[i].age < best->age) best = &e->v[i];
        if (!best) {
            best = &e->v[0];
            for (i = 1; i < ENS_VOICES; i++) if (e->v[i].age < best->age) best = &e->v[i];
        }
        v = best;
    }
    lane = (uint32_t)(v - e->v);
    {
        int sounding = v->stage != 0;
        uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2];
        int32_t s0 = v->s[0], s1 = v->s[1], s4 = v->s[4], s5 = v->s[5], s6 = v->s[6], s7 = v->s[7];
        uint32_t seed = 0x1B873593u ^ ((lane + 1u) * 0x9E3779B9u);
        v->key = key; v->vel = vel; v->gate = 1; v->stage = 1; v->age = ++e->age;
        if (!sounding) { v->env = 0; v->env_out = 0; }
        v->ph[1] = v->ph[0] + 0x55555555u;
        v->ph[2] = v->ph[0];
        v->s[0] = v->s[1] = v->s[4] = 0;
        v->s[2] = (int32_t)(seed ^ (v->age * 0x85EBCA6Bu));
        if (!v->s[2]) v->s[2] = 1;
        v->s[3] = 0;
        v->s[5] = (int32_t)(noise32(&v->s[2]) >> 16) - 32768;
        v->s[6] = 0;
        v->s[7] = (int32_t)(seed >> 24) - 128;
        if (sounding) {                         /* (engine keep 0xF3: s0 s1 s4 s5 s6 s7) */
            v->ph[0] = ph0; v->ph[1] = ph1; v->ph[2] = ph2;
            v->s[0] = s0; v->s[1] = s1; v->s[4] = s4; v->s[5] = s5; v->s[6] = s6; v->s[7] = s7;
        }
    }
}
void ens_note_off(ens_t *e, uint8_t key)
{
    for (uint32_t i = 0; i < ENS_VOICES; i++)
        if (e->v[i].gate && e->v[i].key == key) { e->v[i].gate = 0; e->v[i].stage = 3; }
}

/* n samples of osc_saw from ph, a step inc a sample: into a (SAW_SET), off a (SAW_SUB), into a and times g into
 * x (SAW_BOTH), or times g onto x (SAW_ADD). Between two wraps the saw is a line with no residual, so the samples
 * go in runs: how many until the wrap comes within a step is one division, and only the two at the wrap take the
 * residual. The same numbers as n calls. */
enum { SAW_SET, SAW_SUB, SAW_BOTH, SAW_ADD };
static inline __attribute__((always_inline)) void saws(int32_t *a, int32_t *x, uint32_t n, uint32_t ph, uint32_t inc,
                                                       int32_t g, int kind)
{
    uint32_t i = 0, run;
#define SAW_PUT(S) do { int32_t s_ = (S); \
        if (kind == SAW_SET) a[i] = s_; else if (kind == SAW_SUB) a[i] -= s_; \
        else if (kind == SAW_BOTH) { a[i] = s_; x[i] = s_ * g; } else x[i] += s_ * g; } while (0)
    while (i < n) {
        if (!(inc >> 15)) run = n - i;                                  /* (blep gives nothing at such a step) */
        else if (ph >= inc && ph <= 0xFFFFFFFFu - inc) run = (0xFFFFFFFFu - inc - ph) / inc + 1u;
        else { SAW_PUT(osc_saw(ph, inc)); ph += inc; i++; continue; }
        if (run > n - i) run = n - i;
        for (; run; run--, i++, ph += inc) SAW_PUT((int32_t)(ph >> 16) - 32768);
    }
#undef SAW_PUT
}

/* one control tick of the ADSR (Caravel's env_tick); Q15 */
static int32_t env_tick(const ens_t *e, ens_voice_t *v)
{
    int32_t sus = (int32_t)e->env[ENS_SUS] << 17;
    switch (v->stage) {
    case 1:
        v->env += (int32_t)ENS_ENV_LIN[e->env[ENS_ATK] & 127];
        if (v->env >= (1 << 24)) { v->env = 1 << 24; v->stage = 2; }
        break;
    case 2:
        v->env += mulq16(sus - v->env, ENS_ENV_EXP[e->env[ENS_DEC] & 127]);
        break;
    case 3:
        v->env -= mulq16(v->env, ENS_ENV_EXP[e->env[ENS_REL] & 127]);
        if (v->env < (1 << 12)) { v->env = 0; v->stage = 0; }
        break;
    default:
        v->env = 0;
        break;
    }
    return v->env >> 9;
}

/* Caravel's ens_render: one voice's n samples added to out (mono). inc: the note's phase increment, cutoff: the
 * filter's modulation (1/256 of a cutoff index), amp0 / amp1: the amplitude ramp over the n samples (Q15) */
static void ens_voice(const ens_t *e, ens_voice_t *v, int32_t *out, uint32_t n, uint32_t inc, int32_t pitch16,
                      int32_t mcut, int32_t amp0, int32_t amp1)
{
    int32_t reg = e->e[ENS_REG], morph = e->e[ENS_MORPH], tone = e->e[ENS_TONE], drift = e->e[ENS_DRIFT], tex = e->e[ENS_TEXT];
    int32_t wS = morph < 64 ? (64 - morph) * 512 : 0, wD = morph > 64 ? (morph - 64) * 520 : 0;
    int32_t wC = 32768 - (morph > 64 ? morph - 64 : 64 - morph) * 512;
    int32_t g4 = (reg < 64 ? reg : 64) * 160, g16 = reg > 64 ? (reg - 64) * 160 : 0, g8 = 14000;
    int32_t ic1 = v->s[0], ic2 = v->s[1], q20, cut, k, gl, gb, knee, rough, breath;
    int32_t astep = (amp1 - amp0) / (int32_t)n, amp = amp0;
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2], i;
    uint32_t pw = (uint32_t)(64 * 256) << 17;    /* the shape at rest (no LFO, no ENV > SHAPE) */
    uint32_t inc0, inc1, inc2;
    int32_t x[ENS_CTL], sa[ENS_CTL];            /* the oscillators' sum; the 8' saw alone */
    tsvf_t f;
    wS = clamp(wS, 0, 32767); wD = clamp(wD, 0, 32767); wC = clamp(wC, 0, 32767);
    if (++v->s[3] >= 1024) {                    /* a new drift target about every 0.7 s, then a one-pole glide to it */
        v->s[3] = 0;
        v->s[5] = (int32_t)(noise32(&v->s[2]) >> 16) - 32768;
    }
    v->s[4] += (v->s[5] - v->s[4]) / 256;
    q20 = (v->s[4] * drift / 127) / 8 + v->s[7] * drift / 127 * 3;
    inc0 = inc + (uint32_t)((int32_t)(inc >> 11) * q20 / 512);
    inc1 = inc0 + (inc0 >> 6);                  /* the second oscillator: about 27 cents up */
    inc2 = inc0 >> 1;
    cut = (mulq15(wS, (62 + tone * 57 / 127) << 8) + mulq15(wC, (58 + tone * 32 / 127) << 8)
           + mulq15(wD, (44 + tone * 51 / 127) << 8)) + mcut;
    cut += mulq15(wS + wD, clamp(pitch16 - 768, -768, 768) * 5);   /* a little key tracking */
    k = mulq15(wS, 6000) + mulq15(wC, 1400) + mulq15(wD, 2600);
    k = clamp(k, 900, 8192);
    tsvf_coef_k(&f, cut, k);
    gl = mulq15(wS + wD, 32767) + mulq15(wC, 11000);
    gb = mulq15(wC, 29000);
    knee = 16000 - tex * 60;
    rough = mulq15(wD, 8192 + tex * 160);
    breath = mulq15(wC, 2200 + tex * 12);
    /* In passes, each with a few values in registers (Caravel's one loop over everything was 250 instructions a
     * sample here, a quarter of them the stack): the oscillators one at a time into x, in runs between a saw's
     * steps; then the filter. The same numbers. */
    saws(sa, x, n, ph0, inc0, g8, SAW_BOTH);                            /* the 8' saw, kept for the pulse's product */
    if (g4) saws(0, x, n, ph0 << 1, inc0 << 1, g4, SAW_ADD);              /* the 4' */
    if (g16) { uint32_t ph = ph2; for (i = 0; i < n; i++, ph += inc2) x[i] += osc_tri(ph) * g16; }
    for (i = 0; i < n; i++) x[i] >>= 15;
    if (rough) {                                /* (64 bits: a saw at its wrap and the pulse reach 2 x full scale, which
                                                 * Caravel's 32-bit product wraps into a click) */
        int32_t pu[ENS_CTL];
        saws(pu, 0, n, ph1, inc1, 0, SAW_SET);
        saws(pu, 0, n, ph1 + pw, inc1, 0, SAW_SUB);
        for (i = 0; i < n; i++) x[i] += mulq15((int32_t)(((int64_t)sa[i] * (pu[i] >> 1)) >> 15), rough);
    }
    if (breath) {
        int32_t seed = v->s[2];
        for (i = 0; i < n; i++) x[i] += (((int32_t)(noise32(&seed) >> 17) - 16384) * breath) >> 15;
        v->s[2] = seed;
    }
    {
        const int32_t a1 = f.a1, a2 = f.a2, a3 = f.a3;
        for (i = 0; i < n; i++) {
            int32_t v3 = x[i] - ic2, bp, lp, y;
            bp = (a1 * ic1 + a2 * v3) >> 13;
            lp = ic2 + ((a2 * ic1 + a3 * v3) >> 13);
            ic1 = clamp(2 * bp - ic1, -150000, 150000);
            ic2 = clamp(2 * lp - ic2, -150000, 150000);
            y = (int32_t)(((int64_t)lp * gl + (int64_t)((bp * k) >> 12) * gb) >> 15);   /* (64 bits: lp reaches 150000, gl
                                                 * 32930 at the dark end, where Caravel's 32-bit products wrap) */
            amp += astep;
            out[i] += mulq15(mulq15(soft_knee(y, knee) * 2, amp), ENS_VOICE_FS) * 2;
        }
    }
    ph0 += inc0 * n; ph1 += inc1 * n; ph2 += inc2 * n;
    v->ph[0] = ph0; v->ph[1] = ph1; v->ph[2] = ph2;
    v->s[0] = ic1; v->s[1] = ic2;
}

/* Caravel's ens_post: b (the voices' mono mix, n <= ENS_CTL) in place; with a width, sd = left - right */
static void ens_post(ens_t *s, int32_t *b, uint32_t n, int32_t *sd)
{
    uint32_t j, i;
    int32_t depth = s->e[ENS_DEPTH], mix = s->e[ENS_MIX] * 516, tgt[ENS_NT], step[ENS_NT];
    uint32_t slow_inc = 19500u + (uint32_t)s->e[ENS_RATE] * 1075u;     /* 0.2 .. 1.6 Hz */
    uint32_t fast_inc = 584000u + (uint32_t)s->e[ENS_RATE] * 2000u;    /* 6 .. 8.6 Hz */
    uint32_t slow_inc_b = (slow_inc * 165u) >> 7, fast_inc_b = (fast_inc * 150u) >> 7;
    if (!s->live) {                             /* the slow LFOs 120 degrees apart, the right group 60 on */
        for (j = 0; j < ENS_NT; j++) s->slow[j] = (j % 3u) * 0x55555555u + (j / 3u) * 0x2AAAAAAAu;
        s->fast[1] = 0x2AAAAAAAu;
    }
    for (j = 0; j < ENS_NT; j++) {
        int32_t lfo = (sine_i(s->slow[j]) * 3 + sine_i(s->fast[j / 3u] + (j % 3u) * 0x55555555u)) >> 2;
        s->slow[j] += (j < 3u ? slow_inc : slow_inc_b) * n;
        tgt[j] = (ENS_BASE[j] << 8) + ((lfo * depth) >> 7);
    }
    s->fast[0] += fast_inc * n;
    s->fast[1] += fast_inc_b * n;
    if (!s->live) {
        for (j = 0; j < ENS_NT; j++) s->delay[j] = tgt[j];
        s->live = 1;
    }
    for (j = 0; j < ENS_NT; j++) step[j] = (tgt[j] - s->delay[j]) / (int32_t)n;
    {   /* The block into the ring first, then a pass a tap (one loop over six taps was 212 instructions a sample,
         * with a product for each tap's place). A tap reads at most 688 samples behind its own, and the block is
         * ENS_CTL long: nothing it wants is overwritten yet. The same numbers. */
        int32_t suml[ENS_CTL], sumr[ENS_CTL];
        const uint32_t w0 = s->w, taps = sd ? ENS_NT : 3u;
        int32_t *const ring = s->ring;
        for (i = 0; i < n; i++) { ring[(w0 + i) & (ENS_RING - 1u)] = b[i]; suml[i] = 0; sumr[i] = 0; }
        for (j = 0; j < taps; j++) {
            int32_t d = s->delay[j], st = step[j], *sum = j < 3u ? suml : sumr;
            for (i = 0; i < n; i++) {
                uint32_t idx0, idx1;
                int32_t r0, r1;
                d += st;
                idx0 = (w0 + i - ((uint32_t)d >> 8)) & (ENS_RING - 1u); idx1 = (idx0 - 1u) & (ENS_RING - 1u);
                r0 = ring[idx0]; r1 = ring[idx1];
                sum[i] += r0 + (((r1 - r0) * (d & 255)) >> 8);
            }
        }
        s->w = (w0 + n) & (ENS_RING - 1u);
        if (sd)
            for (i = 0; i < n; i++) {
                int32_t dry = b[i];
                b[i] = dry + mulq16(mulq16(suml[i] + sumr[i], ENS_MID_Q16) - dry, (uint32_t)mix);
                sd[i] = mulq16(mulq16(suml[i] - sumr[i], ENS_WET_Q16), (uint32_t)mix);
            }
        else
            for (i = 0; i < n; i++) {
                int32_t dry = b[i];
                b[i] = dry + mulq16(mulq16(suml[i], ENS_WET_Q16) - dry, (uint32_t)mix);
            }
    }
    for (j = 0; j < ENS_NT; j++) s->delay[j] += step[j] * (int32_t)n;
}

void ens_render(ens_t *e, int32_t *stereo, uint32_t frames)
{
    while (frames) {
        uint32_t n = frames < ENS_CTL ? frames : ENS_CTL, i, any = 0;
        int32_t w = e->width;
        for (i = 0; i < n; i++) e->mono[i] = 0;
        for (i = 0; i < ENS_VOICES; i++) {      /* a control tick, then the voice's n samples */
            ens_voice_t *v = &e->v[i];
            int32_t env, amp1;
            if (!v->stage) continue;
            env = env_tick(e, v);
            amp1 = mulq15(env, v->vel * 258);
            {
                int32_t mcut = (env * e->fenv) >> 7;
                if (v->vel > 110) mcut += (env * 24) >> 7;   /* accent opens the filter with the env */
                ens_voice(e, v, e->mono, n, ENS_NOTE_INC[v->key & 127], (int32_t)v->key * 16, mcut, v->env_out, amp1);
            }
            v->env_out = amp1;
            any = 1;
        }
        if (any) e->tail = ENS_TAIL_BLOCKS;
        else if (e->tail) e->tail--;
        else { e->live = 0; stereo += 2u * n; frames -= n; continue; }   /* silent: the ensemble rests too */
        if (w) {
            ens_post(e, e->mono, n, e->side);
            for (i = 0; i < n; i++) {
                int32_t d = (e->side[i] * w) >> 8;
                stereo[2u * i] += ENS_OUT(e->mono[i] + d);
                stereo[2u * i + 1u] += ENS_OUT(e->mono[i] - d);
            }
        } else {
            ens_post(e, e->mono, n, 0);
            for (i = 0; i < n; i++) { int32_t y = ENS_OUT(e->mono[i]); stereo[2u * i] += y; stereo[2u * i + 1u] += y; }
        }
        stereo += 2u * n; frames -= n;
    }
}
