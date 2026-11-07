/* SPDX-License-Identifier: GPL-3.0-only */
/* Niagara Drift's effects: the drones -> a stereo DELAY -> a REVERB, with a dry/wet mix for each. Included by
 * drone.c (one compilation unit); uses its parameters, its LFO and its helpers. Float only, no libm
 * (fastmath.h). They run while the drone is stopped too, so tails ring out.
 *
 * The design of both is ChoralRoot's (Quixotic7's ChoralRootFM1, GPL-3.0-only: firmware/src/cr_fx.c and
 * docs/FX.md), after the Moog Matriarch's delay and a Jot / Stautner-Puckette feedback delay network; written
 * anew here in float for this engine (ChoralRoot's is fixed point). LICENSING.md has the row.
 *
 * DELAY: two lines, L and R (int16, 743 ms each, in the .pool RAM). Each side reads between two samples at a
 * time that glides to its target (~50 ms: turning TIME bends the repeats' pitch, as tape does) plus the WOW (a
 * slow and a faster sine, the same on both sides) and the LFO's tap (+-25 % of the time). The loop: the read ->
 * a soft clip at the REPEATS gain -> a low-pass (the type's tone) -> a low cut (~80 Hz) -> back in with the
 * input. Past ~85 Repeats runs away into self-oscillation, held by the clip. FOUR TYPES, each a set of the
 * mode, the right side's time against the left's, the tone and the wow (gliding from one to the next):
 *   Tape       both sides alike (mono), warm, a gentle wow: 0.1's delay
 *   Wide       stereo, the right at 3/4 of the left's time, a little brighter, a light wow
 *   Ping-pong  the input into the left; left feeds right and right left
 *   Clean      bright, no wow (just tuning stays still)
 *
 * REVERB: a feedback delay network of eight lines mixed by an 8 x 8 Hadamard matrix, behind a low cut, a
 * pre-delay and four all-pass diffusers, at half the rate (22.05 kHz, between two half-band filters). Each
 * line: a read at its length (SIZE; four of them swing on slow sines, MOD), a low-pass (DAMP) and the gain that
 * gives every line the same decay time (LENGTH: 0.3 .. 40 s, 100 "Inf": a freeze; at Inf the damping is out of
 * the loop, so what's in it stays). Out: four lines a side with alternating signs. FOUR TYPES set the size,
 * pre-delay, swing, low cut and width: Room, Hall, Cathedral, Chorus hall (a wide swing). A new type glides its
 * lines to their new lengths: no fade, no clearing. Stopped, an Inf tail fades over about twice the Fade time. */

#ifdef OM_HOST
#define DR_POOL
#else
#define DR_POOL                        /* fm1-dock: one RAM region (take.py) */
#endif

static uint32_t fx_quiet;                /* blocks the effects' output has been silent (dr_render rests them) */

/* ------------------------------------------------------------- delay --- */
#define DLY_N 16384u                     /* fm1-dock (take.py): 371 ms, its longest time 270 ms (743, 450) */
#define DLY_MASK (DLY_N - 1u)
static int16_t dly_buf[2][DLY_N] DR_POOL;
static uint32_t dly_w;
static float dly_t = 0.177f * DR_SR;    /* the left's time, in samples, smoothed */
volatile float dr_dly_ms;               /* ... in ms, each block (the host's probe) */
static float dly_lp[2], dly_hp[2], dly_olp[2];
static float dly_ph1, dly_ph2;           /* the wow's two sines */
enum { DT_TAPE, DT_WIDE, DT_PING, DT_CLEAN, DT_NTYPES };
typedef struct {
    float spread, tone, out, wow, ping;  /* R's time / L's; the loop's low-pass k; the wet's low-pass k; the wow,
                                          * samples of swing; ping-pong 0 .. 1 */
    float gain;                          /* the wet's level: the types as loud as each other (measured on a drone) */
} dly_type_t;
static const dly_type_t DLY_TYPE[DT_NTYPES] = {
    {1.00f, 0.39f, 0.51f, 30.0f, 0.0f, 1.00f},   /* TAPE: ~3.5 kHz in the loop, ~5 kHz out, 0.7 ms of wow */
    {0.75f, 0.57f, 0.70f, 14.0f, 0.0f, 1.15f},   /* WIDE: ~6 kHz, ~8.5 kHz */
    {1.00f, 0.50f, 0.66f, 14.0f, 1.0f, 1.66f},   /* PING-PONG: each repeat on one side (3 dB of it) */
    {1.00f, 0.86f, 1.00f, 0.0f, 0.0f, 1.08f},    /* CLEAN: ~14 kHz, nothing out */
};
static dly_type_t dly_c = {1.00f, 0.39f, 0.51f, 30.0f, 0.0f, 1.00f};   /* the type as it glides */

static float dly_ms(int v) { return fm_minf(70.0f * fm_exp2f((float)v * 0.026845f), 270.0f); }   /* fm1-dock (take.py): to 270 ms */

static inline float dly_read(const int16_t *b, float d)   /* d samples back, linearly interpolated */
{
    float p = (float)dly_w - d, f;
    int32_t i;
    if (p < 0.0f)
        p += (float)DLY_N;
    i = (int32_t)p;
    f = p - (float)i;
    return ((float)b[(uint32_t)i & DLY_MASK] * (1.0f - f) + (float)b[(uint32_t)(i + 1) & DLY_MASK] * f) *
           (1.0f / 16384.0f);
}

/* ------------------------------------------------------------ reverb --- */
enum { RV_ROOM, RV_HALL, RV_CATHEDRAL, RV_CHORUS_HALL, RV_NTYPES };
typedef struct {
    float size, pre_ms, mod, lowcut, width;   /* the lines' lengths x; pre-delay; the swing, samples at 22.05 kHz;
                                               * the input's low cut, Hz; 0 mono .. 1 */
    float gain;                               /* the wet's level: the types as loud as each other (measured) */
} rv_type_t;
static const rv_type_t RV_TYPE[RV_NTYPES] = {
    {0.35f, 4.0f, 2.0f, 120.0f, 0.8f, 0.80f},    /* ROOM: small, close */
    {0.75f, 18.0f, 5.0f, 90.0f, 0.9f, 0.93f},    /* HALL */
    {1.00f, 42.0f, 7.0f, 60.0f, 0.85f, 1.00f},   /* CATHEDRAL: the largest, the furthest */
    {0.85f, 24.0f, 18.0f, 90.0f, 1.0f, 1.45f},   /* CHORUS HALL: a hall whose lines swing wide (they cancel a little) */
};
#define RV_FS (DR_SR * 0.5f)
/* the eight lines at size 1, samples at 22.05 kHz (57 .. 125 ms; primes, ~1.12 from one to the next:
 * ChoralRoot's), each in its own stretch of one array with room for the swing and the interpolator */
static const uint16_t RV_LEN[8] = {1259, 1433, 1607, 1801, 2003, 2221, 2477, 2767};
#define RV_GUARD 24u
#define RV_CAP (1259u + 1433u + 1607u + 1801u + 2003u + 2221u + 2477u + 2767u + 8u * RV_GUARD)
static float rv_mem[RV_CAP] DR_POOL;
#define RV_PRE_N 2048u                   /* the pre-delay: 92 ms at 22.05 kHz */
static float rv_pre[RV_PRE_N] DR_POOL;
static const uint16_t RV_AP[4] = {113, 173, 263, 397};   /* the diffusers: 5.1 .. 18 ms */
#define RV_AP_CAP (113u + 173u + 263u + 397u)
static float rv_apm[RV_AP_CAP] DR_POOL;
typedef struct { float x[4], y[4]; } hb_t;   /* a half-band filter: two paths of two first-order all-passes */
static struct {
    float *line[8], *ap[4];
    uint32_t cap[8], pos[8], app[4], pw;
    float cur[8];                        /* each line's length now (gliding to its size) */
    float d[8], dd[8], pdd;              /* this block: each line's read now and its step a tick (the glide and
                                          * the swing, worked out once a block), the pre-delay's step */
    float lp[8], g[8], dk, ig;           /* the damping's states, the lines' gains, damp's k, the input gain */
    float h0, h1, lk;                    /* the low cut (two poles) and its k */
    float pre, mod, width, gain;         /* the pre-delay now (samples), the swing, the width, the level: gliding */
    float mph[4];                        /* the four swings' phases */
    hb_t dn, up[2];
    float hold[2];                       /* an odd block's last sample: the wet of the one before */
    int ready;
} rv;
_Static_assert((RV_PRE_N & (RV_PRE_N - 1u)) == 0, "a power of two: a mask");

static void rv_layout(void)
{
    float *m = rv_mem, *a = rv_apm;
    uint32_t i;
    for (i = 0; i < 8u; i++) {
        rv.line[i] = m;
        rv.cap[i] = RV_LEN[i] + RV_GUARD;
        m += rv.cap[i];
    }
    for (i = 0; i < 4u; i++) {
        rv.ap[i] = a;
        a += RV_AP[i];
    }
}

/* the half-band all-passes (ChoralRoot's tools/fx_halfband.py: -54 dB above 12.5 kHz, flat to 9.5 kHz) */
#define HB_A0 (199.0f / 2048.0f)
#define HB_A1 (1264.0f / 2048.0f)
#define HB_A2 (695.0f / 2048.0f)
#define HB_A3 (1785.0f / 2048.0f)
static inline float hb_ap(hb_t *h, int k, float a, float x)
{
    float y = a * (x - h->y[k]) + h->x[k];
    h->x[k] = x;
    h->y[k] = y;
    return y;
}

static float rv_rt60(int v)              /* LENGTH -> seconds; 100: Inf (0) */
{
    return v >= 100 ? 0.0f : 0.3f * fm_exp2f((float)v * 0.070607f);   /* 0.3 * (40/0.3)^(v/99) */
}
static float onepole_k(float hz, float fs) { return 1.0f - fm_exp2f(-6.2831853f * 1.442695f * hz / fs); }

static float fx_g[5];                            /* the repeats' and mixes' gains where the last block ended */
/* REPEATS: the loop's gain, 1.15 at 100 before its losses (the soft clip, the tone, the 80 Hz cut); over 85,
 * where the screen says "runaway", a little more, so the top sings into runaway on a low drone too */
static float rep_g(float v) { return v * 0.0115f + fm_maxf(v - 85.0f, 0.0f) * 0.006f; }
static void fx_gains(void)                       /* ... where they're set: no ramp from nothing */
{
    fx_g[0] = rep_g(pfs[P_DLY_REPEATS]);
    fx_g[1] = fm_sqrtf(1.0f - pfs[P_DLY_MIX] * 0.01f);
    fx_g[2] = fm_sqrtf(pfs[P_DLY_MIX] * 0.01f);
    fx_g[3] = fm_sqrtf(1.0f - pfs[P_REV_MIX] * 0.01f);
    fx_g[4] = fm_sqrtf(pfs[P_REV_MIX] * 0.01f);
}

/* the reverb's coefficients for this block: the gains from LENGTH (Inf while playing; stopped, a freeze lets go
 * over about twice the Fade time, at least 1.5 s), damp's k, the input gain; the type's pre-delay, swing and width glide */
static void rv_coefs(float ease)
{
    const rv_type_t *t = &RV_TYPE[(unsigned)par[P_REV_TYPE] < RV_NTYPES ? par[P_REV_TYPE] : RV_CATHEDRAL];
    float rt = rv_rt60(par[P_REV_LEN]), fsec, e;
    int inf = rt <= 0.0f;
    uint32_t i;
    if (inf && fade_target <= 0.0f) {   /* stopped or stopping: a freeze lets go, over about twice the Fade */
        fsec = fm_maxf(2.0f * 0.05f * fm_exp2f((float)par[P_FADE] * 0.102288f), 1.5f);
        rt = fsec;
        inf = 0;
    }
    for (i = 0; i < 8u; i++) {           /* g = 10^(-3 L / (RT60 fs)) */
        float g = inf ? 0.99988f : fm_exp2f(-9.965784f * rv.cur[i] / (rt * RV_FS));
        rv.g[i] += (g - rv.g[i]) * ease;
    }
    {
        float v = (float)par[P_REV_DAMP], k = v <= 0.0f || inf ? 1.0f
                                                    : onepole_k(12000.0f * fm_exp2f(-v * 0.0418f), RV_FS);
        rv.dk += (k - rv.dk) * ease;
    }
    e = 1.0f - rv.g[4] * rv.g[4];        /* the input's gain: (1 - g^2)^(1/4), so a long tail isn't louder */
    rv.ig = fm_sqrtf(fm_sqrtf(fm_maxf(e, 0.0f)));
    rv.lk = onepole_k(t->lowcut, RV_FS);
    rv.mod += (t->mod - rv.mod) * ease;
    rv.width += (t->width - rv.width) * ease;
    rv.gain += (t->gain - rv.gain) * ease;
}

static void rv_clear(void)
{
    uint32_t i;
    rv_layout();
    for (i = 0; i < RV_CAP; i++)
        rv_mem[i] = 0.0f;
    for (i = 0; i < RV_AP_CAP; i++)
        rv_apm[i] = 0.0f;
    for (i = 0; i < RV_PRE_N; i++)
        rv_pre[i] = 0.0f;
    {
        const rv_type_t *t = &RV_TYPE[(unsigned)par[P_REV_TYPE] < RV_NTYPES ? par[P_REV_TYPE] : RV_CATHEDRAL];
        for (i = 0; i < 8u; i++) {
            rv.cur[i] = rv.d[i] = (float)(uint32_t)((float)RV_LEN[i] * t->size);
            rv.pos[i] = 0;
            rv.lp[i] = 0.0f;
            rv.g[i] = 0.0f;
        }
        rv.pre = t->pre_ms * 0.001f * RV_FS;
        rv.mod = t->mod;
        rv.width = t->width;
        rv.gain = t->gain;
    }
    for (i = 0; i < 4u; i++)
        rv.app[i] = 0;
    rv.pw = 0;
    rv.h0 = rv.h1 = 0.0f;
    rv.dk = 1.0f;
    {
        static const hb_t z;
        rv.dn = rv.up[0] = rv.up[1] = z;
    }
    rv.hold[0] = rv.hold[1] = 0.0f;
    rv_coefs(1.0f);
    rv.ready = 1;
}

/* this block's ramps, nt ticks: each line's length glides to its size (at most 0.05 samples a tick), the odd four
 * swing (their slow sines worked out at the block's ends), the pre-delay glides (a quarter of a sample a tick) */
static void rv_block(uint32_t nt, float dt)
{
    const rv_type_t *t = &RV_TYPE[(unsigned)par[P_REV_TYPE] < RV_NTYPES ? par[P_REV_TYPE] : RV_CATHEDRAL];
    static const float MR[4] = {0.31f, 0.43f, 0.57f, 0.71f};   /* the swings, Hz */
    const float inv = nt ? 1.0f / (float)nt : 0.0f, lim = 0.05f * (float)nt;
    uint32_t i;
    for (i = 0; i < 4u; i++)
        rv.mph[i] = advance(rv.mph[i], MR[i] * dt);
    for (i = 0; i < 8u; i++) {
        float tl = (float)(uint32_t)((float)RV_LEN[i] * t->size), e;
        rv.cur[i] += fm_clampf(tl - rv.cur[i], -lim, lim);
        e = rv.cur[i] + ((i & 1u) ? rv.mod * fm_sin_turns(rv.mph[i >> 1]) : 0.0f);
        e = fm_clampf(e, 2.0f, (float)(rv.cap[i] - 2u));
        rv.d[i] = fm_clampf(rv.d[i], 2.0f, (float)(rv.cap[i] - 2u));
        rv.dd[i] = (e - rv.d[i]) * inv;
    }
    rv.pdd = fm_clampf(t->pre_ms * 0.001f * RV_FS - rv.pre, -0.25f * (float)nt, 0.25f * (float)nt) * inv;
}

/* the soft limit in the lines: itself up to 2, a gentle knee above, 4 at most (a freeze with Feedback) */
static inline float rv_sat(float v)
{
    float a = fm_fabsf(v);
    if (a <= 2.0f)
        return v;
    a = fm_minf(2.0f + (a - 2.0f) * 0.25f, 4.0f);
    return v < 0.0f ? -a : a;
}

/* one sample of the network at 22.05 kHz: x in, the wet out in *ol, *or_ */
static inline void rv_tick(float x, float *ol, float *or_)
{
    float r[8], v[8];
    uint32_t i;
    /* the low cut, two poles, then the pre-delay (gliding: a quarter of a sample a sample) */
    rv.h0 += (x - rv.h0) * rv.lk;
    x -= rv.h0;
    rv.h1 += (x - rv.h1) * rv.lk;
    x -= rv.h1;
    rv_pre[rv.pw] = x;
    rv.pre += rv.pdd;
    {
        float p = (float)rv.pw - rv.pre, f;
        int32_t k;
        if (p < 0.0f)
            p += (float)RV_PRE_N;
        k = (int32_t)p;
        f = p - (float)k;
        x = rv_pre[(uint32_t)k & (RV_PRE_N - 1u)] * (1.0f - f) + rv_pre[(uint32_t)(k + 1) & (RV_PRE_N - 1u)] * f;
    }
    rv.pw = (rv.pw + 1u) & (RV_PRE_N - 1u);
    for (i = 0; i < 4u; i++) {           /* the diffusers: all-passes, g = 5/8 */
        float *b = rv.ap[i], z = b[rv.app[i]], u = x - 0.625f * z;
        b[rv.app[i]] = u;
        if (++rv.app[i] >= RV_AP[i])
            rv.app[i] = 0;
        x = z + 0.625f * u;
    }
    x *= rv.ig;
    /* the lines' reads: each at its length, a ramp over the block (rv_block) */
    for (i = 0; i < 8u; i++) {
        float p, f;
        uint32_t n = rv.cap[i], k;
        p = (float)rv.pos[i] - rv.d[i];
        rv.d[i] += rv.dd[i];
        if (p < 0.0f)
            p += (float)n;
        k = (uint32_t)p;
        f = p - (float)k;
        r[i] = rv.line[i][k] * (1.0f - f) + rv.line[i][k + 1u < n ? k + 1u : 0u] * f;
    }
    {
        float l = r[0] - r[1] + r[4] - r[5], rr = r[2] - r[3] + r[6] - r[7], m = (l + rr) * 0.5f;
        float s = (l - rr) * 0.5f * rv.width;
        *ol = m + s;
        *or_ = m - s;
    }
    for (i = 0; i < 8u; i++) {           /* the damping and the gains (x 1/sqrt 8: the Hadamard's scale) */
        rv.lp[i] += (r[i] - rv.lp[i]) * rv.dk;
        rv.lp[i] = fm_flush(rv.lp[i]);
        v[i] = rv.lp[i] * rv.g[i] * 0.35355339f;
    }
    {   /* Hadamard: three stages of butterflies */
        float a0 = v[0] + v[1], a1 = v[0] - v[1], a2 = v[2] + v[3], a3 = v[2] - v[3];
        float a4 = v[4] + v[5], a5 = v[4] - v[5], a6 = v[6] + v[7], a7 = v[6] - v[7];
        float b0 = a0 + a2, b1 = a1 + a3, b2 = a0 - a2, b3 = a1 - a3;
        float b4 = a4 + a6, b5 = a5 + a7, b6 = a4 - a6, b7 = a5 - a7;
        v[0] = b0 + b4; v[1] = b1 + b5; v[2] = b2 + b6; v[3] = b3 + b7;
        v[4] = b0 - b4; v[5] = b1 - b5; v[6] = b2 - b6; v[7] = b3 - b7;
    }
    for (i = 0; i < 8u; i++) {           /* the writes */
        rv.line[i][rv.pos[i]] = rv_sat(v[i] + x);
        if (++rv.pos[i] >= rv.cap[i])
            rv.pos[i] = 0;
    }
}

static void fx_clear(void)
{
    uint32_t k;
    for (k = 0; k < DLY_N; k++)
        dly_buf[0][k] = dly_buf[1][k] = 0;
    dly_lp[0] = dly_lp[1] = dly_hp[0] = dly_hp[1] = dly_olp[0] = dly_olp[1] = 0.0f;
    dly_t = dly_ms(par[P_DLY_TIME]) * 0.001f * DR_SR;    /* at its time, no glide from an old one */
    dly_c = DLY_TYPE[(unsigned)par[P_DLY_TYPE] < DT_NTYPES ? par[P_DLY_TYPE] : DT_TAPE];
    rv_clear();
    fx_gains();
    fx_quiet = 0;
}

/* resting (stopped and silent): the delay's time and type go straight to their knobs */
static void fx_rest(void)
{
    dly_t = dly_ms(par[P_DLY_TIME]) * 0.001f * DR_SR;
    dly_c = DLY_TYPE[(unsigned)par[P_DLY_TYPE] < DT_NTYPES ? par[P_DLY_TYPE] : DT_TAPE];
    fx_gains();
}

#define RV_WET 0.55f                     /* the wet's level: 0.1's reverb's at the same mix, 1 dB under (its peaks) */

/* the effects, in place on the stereo block l, r (n frames) */
static void fx_run(float *l, float *r, uint32_t n)
{
    /* the mixes and the repeats as they glide (pfs), and in a straight line across the block from where the
     * last one ended: read raw (par), each detent was a step, a click on a soft sound */
    /* stopped or stopping, the repeats can't run away: they ring out (QA 2026-10-20: at 90+ the howl went on after
     * PLAY, with "PLAY to start" on the screen) */
    const float ms = dly_ms(par[P_DLY_TIME]),
                fbg1 = fade_target <= 0.0f ? fm_minf(rep_g(pfs[P_DLY_REPEATS]), 0.85f) : rep_g(pfs[P_DLY_REPEATS]);
    const float dm = pfs[P_DLY_MIX] * 0.01f, dry1e = fm_sqrtf(1.0f - dm), wet1e = fm_sqrtf(dm);
    const float rm = pfs[P_REV_MIX] * 0.01f, dry2e = fm_sqrtf(1.0f - rm), wet2e = fm_sqrtf(rm);
    const float dn = 1.0f / (float)(n ? n : 1u);
    float fbg = fx_g[0], dry1 = fx_g[1], wet1 = fx_g[2], dry2 = fx_g[3], wet2 = fx_g[4];
    const float dfb = (fbg1 - fbg) * dn, dd1 = (dry1e - dry1) * dn, dw1 = (wet1e - wet1) * dn;
    const float dd2 = (dry2e - dry2) * dn, dw2 = (wet2e - wet2) * dn;
    const float rstep = 1.0f / (morph_s() * DR_SR);    /* its LFO route switched: crossfade (Morph) */
    const float damt = depth_amt(par[P_DLY_LFO_DEPTH]) * 0.25f * pfs[P_MOVE] * 0.02f;   /* x the MOVEMENT */
    const dly_type_t *dt = &DLY_TYPE[(unsigned)par[P_DLY_TYPE] < DT_NTYPES ? par[P_DLY_TYPE] : DT_TAPE];
    const float ease = fm_minf((float)n * (1.0f / (0.25f * DR_SR)), 1.0f);   /* a type's glide: ~0.25 s */
    static float sl[BLK], sr[BLK];       /* the reverb's send (mono, in sl) and its wet, a block */
    float pgain;
    int mono = 0;
    uint32_t j;
    if (!rv.ready)
        rv_clear();
    if ((uint8_t)par[P_DLY_LFO] != dly_rt) {
        dly_rp = dly_rt;
        dly_rt = (uint8_t)par[P_DLY_LFO];
        dly_rw = 0.0f;
    }
    float sp = dly_c.spread, dsp;        /* the right side's spread across the block, sample by sample (QA
                                          * 2026-10-20: stepped each block, Wide crackled as it came and went) */
    dly_c.spread += (dt->spread - dly_c.spread) * ease;
    dly_c.tone += (dt->tone - dly_c.tone) * ease;
    dly_c.out += (dt->out - dly_c.out) * ease;
    dly_c.wow += (dt->wow - dly_c.wow) * ease;
    dly_c.ping += (dt->ping - dly_c.ping) * ease;
    dly_c.gain += (dt->gain - dly_c.gain) * ease;
    pgain = dly_c.gain;
    if (dt->spread >= 1.0f && dt->ping <= 0.0f && dly_c.spread > 0.9995f && dly_c.ping < 0.0005f) {
        dly_c.spread = 1.0f;             /* settled on a mono type */
        dly_c.ping = 0.0f;
        mono = 1;
    }
    dsp = (dly_c.spread - sp) * dn;
    rv_coefs(ease);
    dr_dly_ms = dly_t * (1000.0f / DR_SR);
    /* DELAY */
    for (j = 0; j < n; j++) {
        float x, yl, yr, fl, fr, t, wow, pp = dly_c.ping;
        sp += dsp;
        fbg += dfb;
        dry1 += dd1;
        wet1 += dw1;
        x = (l[j] + r[j]) * 0.5f;
        {
            float lfo = lfo_at(dly_rt, j / CR);
            if (dly_rw < 1.0f) {
                dly_rw = fm_minf(dly_rw + rstep, 1.0f);
                lfo = lfo_at(dly_rp, j / CR) + (lfo - lfo_at(dly_rp, j / CR)) * dly_rw;
            }
            t = ms * 0.001f * DR_SR * (1.0f + damt * lfo);
            dly_t += (t - dly_t) * 0.00045f;                       /* ~50 ms: tape-like */
        }
        if (!(j % CR)) {
            dly_ph1 = advance(dly_ph1, 0.55f * (float)CR / DR_SR);
            dly_ph2 = advance(dly_ph2, 3.3f * (float)CR / DR_SR);
        }
        wow = dly_c.wow * (0.75f * fm_sin_turns(dly_ph1) + 0.25f * fm_sin_turns(dly_ph2));
        yl = dly_read(dly_buf[0], fm_clampf(dly_t + wow + dly_c.wow, 4.0f, (float)(DLY_N - 4u)));
        dly_olp[0] += (yl - dly_olp[0]) * dly_c.out;               /* the wet out */
        /* the loops: soft clip, the tone, no rumble (~80 Hz taken away) */
        dly_lp[0] += (sat(yl * fbg) - dly_lp[0]) * dly_c.tone;
        dly_hp[0] += (dly_lp[0] - dly_hp[0]) * 0.0114f;
        fl = dly_lp[0] - dly_hp[0];
        if (mono) {                      /* Tape, Clean (settled): the right is the left, worked out once */
            yr = yl;
            dly_olp[1] = dly_olp[0];
            dly_lp[1] = dly_lp[0];
            dly_hp[1] = dly_hp[0];
            fr = fl;
        } else {
            yr = dly_read(dly_buf[1], fm_clampf(dly_t * sp + wow + dly_c.wow, 4.0f, (float)(DLY_N - 4u)));
            dly_olp[1] += (yr - dly_olp[1]) * dly_c.out;
            dly_lp[1] += (sat(yr * fbg) - dly_lp[1]) * dly_c.tone;
            dly_hp[1] += (dly_lp[1] - dly_hp[1]) * 0.0114f;
            fr = dly_lp[1] - dly_hp[1];
        }
        {   /* stereo: each side feeds itself; ping-pong: the input into L, L into R, R into L */
            float wl = x + fl * (1.0f - pp) + fr * pp, wr = x * (1.0f - pp) + fr * (1.0f - pp) + fl * pp;
            dly_buf[0][dly_w] = (int16_t)(fm_clampf(wl, -1.99f, 1.99f) * 16384.0f);
            dly_buf[1][dly_w] = (int16_t)(fm_clampf(wr, -1.99f, 1.99f) * 16384.0f);
            dly_w = (dly_w + 1u) & DLY_MASK;
        }
        l[j] = l[j] * dry1 + dly_olp[0] * wet1 * pgain;
        r[j] = r[j] * dry1 + dly_olp[1] * wet1 * pgain;
        sl[j] = (l[j] + r[j]) * 0.5f;    /* the reverb's send */
    }
    /* REVERB at the half rate: the send down, the network, the wet up */
    rv_block(n / 2u, (float)n / DR_SR);
    for (j = 0; j + 1u < n; j += 2u) {
        float a = sl[j], b = sl[j + 1u], x, ol, or_;
        x = (hb_ap(&rv.dn, 1, HB_A1, hb_ap(&rv.dn, 0, HB_A0, b)) + hb_ap(&rv.dn, 3, HB_A3, hb_ap(&rv.dn, 2, HB_A2, a))) * 0.5f;
        rv_tick(x, &ol, &or_);
        sl[j] = hb_ap(&rv.up[0], 1, HB_A1, hb_ap(&rv.up[0], 0, HB_A0, ol));
        sl[j + 1u] = hb_ap(&rv.up[0], 3, HB_A3, hb_ap(&rv.up[0], 2, HB_A2, ol));
        sr[j] = hb_ap(&rv.up[1], 1, HB_A1, hb_ap(&rv.up[1], 0, HB_A0, or_));
        sr[j + 1u] = hb_ap(&rv.up[1], 3, HB_A3, hb_ap(&rv.up[1], 2, HB_A2, or_));
    }
    if (n & 1u) {                        /* (never at the block sizes in use: the wet held a sample) */
        sl[n - 1u] = rv.hold[0];
        sr[n - 1u] = rv.hold[1];
    }
    if (n >= 2u) {
        rv.hold[0] = sl[n - 1u];
        rv.hold[1] = sr[n - 1u];
    }
    for (j = 0; j < n; j++) {
        dry2 += dd2;
        wet2 += dw2;
        l[j] = l[j] * dry2 + sl[j] * RV_WET * rv.gain * wet2;
        r[j] = r[j] * dry2 + sr[j] * RV_WET * rv.gain * wet2;
    }
    fx_g[0] = fbg1;
    fx_g[1] = dry1e;
    fx_g[2] = wet1e;
    fx_g[3] = dry2e;
    fx_g[4] = wet2e;
}
