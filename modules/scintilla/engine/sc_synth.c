/* SPDX-License-Identifier: GPL-3.0-only */
/* Voice allocation (synth.ts layerNoteOn / layerNoteOff, voice.ts VoicePool), the per-layer chain
 * and the master stage. */
#include "sc_synth.h"
#include "sc_fixed.h"
#include "sc_tables.h"

static const uint16_t INV_SQRT[17] = { 65535, 65535, 46341, 37837, 32768, 29309, 26755, 24770, 23170,
                                       21845, 20724, 19760, 18919, 18176, 17515, 16921, 16384 };

void sc_perf_default(sc_perf_t *p)
{
    p->joy_x = p->joy_y = 0;
    p->bend_osc = p->bend_filt = SC_Q16(0.3);
    p->lfo2_osc = p->lfo2_filt = SC_Q16(0.5);
}

static int npoly(const sc_synth_t *s)
{
    int n = s->patch.polyphony;
    return n < 1 ? 1 : n > SC_VOICES ? SC_VOICES : n;
}

void sc_reprep(sc_synth_t *s)
{
    for (int k = 0; k < 2; k++) sc_prep(&s->layer[k].prep, &s->patch.layer[k], &s->perf);
    sc_reverb_prep(&s->rev, &s->patch.fx.reverb);
}

void sc_init(sc_synth_t *s, const sc_patch_t *patch)
{
    uint8_t *b = (uint8_t *)s;
    for (uint32_t i = 0; i < sizeof *s; i++) b[i] = 0;
    s->patch = *patch;
    sc_perf_default(&s->perf);
    for (int k = 0; k < 2; k++) {
        sc_layer_state_t *ls = &s->layer[k];
        for (int i = 0; i < SC_VOICES; i++) {
            sc_voice_init(&ls->v[i], 0x2545F491u * (uint32_t)(k * SC_VOICES + i + 1));
            ls->note[i] = -1;
        }
    }
    sc_reprep(s);
}

/* ---- voice allocation ----------------------------------------------------------------------- */

static int allocate(sc_layer_state_t *ls, int nv, int note, uint32_t now)
{
    int idle = -1, rel = -1, play = -1;
    for (int i = 0; i < nv; i++) {
        if (ls->note[i] == -1 && idle < 0) idle = i;
        if (ls->released[i] != 0 && ls->released[i] != 0xFFFFFFFFu)
            if (rel < 0 || ls->released[i] < ls->released[rel]) rel = i;
        if (ls->released[i] == 0xFFFFFFFFu)
            if (play < 0 || ls->started[i] < ls->started[play]) play = i;
    }
    int c = idle >= 0 ? idle : rel >= 0 ? rel : play >= 0 ? play : 0;
    ls->note[c] = (int16_t)note;
    ls->started[c] = now;
    ls->released[c] = 0xFFFFFFFFu;
    return c;
}

static void layer_on(sc_synth_t *s, int k, int note, int32_t vel)
{
    sc_layer_state_t *ls = &s->layer[k];
    const sc_layer_t *L = &s->patch.layer[k];
    int nv = npoly(s);
    uint32_t now = ++s->clock;
    if (ls->nheld < sizeof ls->held) ls->held[ls->nheld++] = (uint8_t)note;

    switch (L->key_assign) {
    case SC_KEY_MONO:
        ls->note[0] = (int16_t)note;
        ls->started[0] = now;
        ls->released[0] = 0xFFFFFFFFu;
        sc_voice_note_on(&ls->v[0], &ls->prep, note, vel);
        break;
    case SC_KEY_UNISON: {
        /* fan out over all voices, detuned evenly over +-unisonDetune * 25 cents */
        int32_t cents = sc_mul(s->patch.unison_detune, 25 << 16, 16);   /* Q16 cents */
        for (int i = 0; i < nv; i++) {
            int32_t t = nv == 1 ? 0 : (i * 2 * SC_ONE16) / (nv - 1) - SC_ONE16;     /* 32-bit division only */
            ls->note[i] = (int16_t)note;
            ls->started[i] = now;
            ls->released[i] = 0xFFFFFFFFu;
            ls->v[i].bend = sc_mul(sc_mul(t, cents, 16), 655, 16);    /* cents / 100 */
            sc_voice_note_on(&ls->v[i], &ls->prep, note, vel);
        }
        break;
    }
    default: {
        int c = allocate(ls, nv, note, now);
        sc_voice_note_on(&ls->v[c], &ls->prep, note, vel);
        break;
    }
    }
}

static void layer_off(sc_synth_t *s, int k, int note)
{
    sc_layer_state_t *ls = &s->layer[k];
    const sc_layer_t *L = &s->patch.layer[k];
    uint32_t now = ++s->clock;
    for (int i = (int)ls->nheld - 1; i >= 0; i--)
        if (ls->held[i] == note) {
            for (uint32_t j = (uint32_t)i; j + 1 < ls->nheld; j++) ls->held[j] = ls->held[j + 1];
            ls->nheld--;
            break;
        }

    switch (L->key_assign) {
    case SC_KEY_MONO:
        if (ls->nheld > 0) {                    /* last-note priority, retriggered at velocity 1 */
            int next = ls->held[ls->nheld - 1];
            ls->note[0] = (int16_t)next;
            sc_voice_note_on(&ls->v[0], &ls->prep, next, SC_ONE16);
        } else {
            ls->note[0] = -1;
            ls->released[0] = now;
            sc_voice_note_off(&ls->v[0], &ls->prep);
        }
        break;
    case SC_KEY_UNISON:
        if (ls->nheld == 0)
            for (int i = 0; i < SC_VOICES; i++) sc_voice_note_off(&ls->v[i], &ls->prep);
        break;
    default:
        for (int i = 0; i < npoly(s); i++)
            if (ls->note[i] == note && ls->released[i] == 0xFFFFFFFFu) {
                ls->released[i] = now;
                sc_voice_note_off(&ls->v[i], &ls->prep);
                break;
            }
        break;
    }
}

/* which layers a key plays: single = upper, double = both, split at splitPoint */
static int route(const sc_synth_t *s, int note)
{
    switch (s->patch.voice_mode) {
    case SC_VM_DOUBLE: return 3;
    case SC_VM_SPLIT:  return note >= s->patch.split ? 1 : 2;
    default:           return 1;
    }
}

void sc_note_on(sc_synth_t *s, int note, int32_t vel)
{
    int r = route(s, note);
    if (r & 1) layer_on(s, 0, note, vel);
    if (r & 2) layer_on(s, 1, note, vel);
}

void sc_note_off(sc_synth_t *s, int note)
{
    int r = route(s, note);
    if (r & 1) layer_off(s, 0, note);
    if (r & 2) layer_off(s, 1, note);
}

void sc_all_off(sc_synth_t *s)
{
    uint32_t now = ++s->clock;
    for (int k = 0; k < 2; k++) {
        sc_layer_state_t *ls = &s->layer[k];
        ls->nheld = 0;
        for (int i = 0; i < SC_VOICES; i++)
            if (ls->released[i] == 0xFFFFFFFFu) {
                ls->released[i] = now;
                sc_voice_note_off(&ls->v[i], &ls->prep);
            }
    }
}

void sc_pitch_bend(sc_synth_t *s, int32_t semis)
{
    /* as synth.ts: the bend replaces every voice's own (a unison detune too) */
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < SC_VOICES; i++) s->layer[k].v[i].bend = semis;
}

int sc_post(sc_synth_t *s, uint8_t type, uint8_t note, int32_t val)
{
    uint32_t w = s->q_w;
    if (w - s->q_r >= SC_QUEUE) return -1;
    sc_event_t *e = &s->q[w % SC_QUEUE];
    e->type = type;
    e->note = note;
    e->val = val;
    s->q_w = w + 1;                             /* publish after the event is written */
    return 0;
}

static void drain(sc_synth_t *s)
{
    while (s->q_r != s->q_w) {
        sc_event_t *e = &s->q[s->q_r % SC_QUEUE];
        switch (e->type) {
        case SC_EV_NOTE_ON:  sc_note_on(s, e->note, e->val); break;
        case SC_EV_NOTE_OFF: sc_note_off(s, e->note); break;
        case SC_EV_ALL_OFF:  sc_all_off(s); break;
        case SC_EV_BEND:     sc_pitch_bend(s, e->val); break;
        case SC_EV_PATCH:    sc_reprep(s); break;
        case SC_EV_PERF:     for (int k = 0; k < 2; k++) sc_prep(&s->layer[k].prep, &s->patch.layer[k], &s->perf); break;
        }
        s->q_r++;
    }
}

/* ---- the chain ------------------------------------------------------------------------------ */

/* StereoChorus: two taps around 12 ms, swept by one sine in opposite phase; dry 1 - mix/2, wet mix.
 * Its mode presets: (rate Hz, depth s) = (0.30, 0.002), (0.55, 0.004), (0.90, 0.007), each scaled
 * by (0.5 + the patch's rate / depth). */
static void chorus(sc_chorus_t *c, const sc_chorus_p_t *p, const int32_t *in, int32_t *l, int32_t *r, uint32_t n)
{
    static const int32_t RATE[3] = { SC_Q16(0.30), SC_Q16(0.55), SC_Q16(0.90) };
    static const int32_t DEPTH[3] = { SC_Q16(0.002), SC_Q16(0.004), SC_Q16(0.007) };
    if (!p->on) {
        for (uint32_t i = 0; i < n; i++) l[i] = r[i] = in[i];
        return;
    }
    int m = p->mode < 1 ? 0 : p->mode > 3 ? 2 : p->mode - 1;
    int32_t rate = sc_mul(RATE[m], SC_ONE16 / 2 + p->rate, 16);
    uint32_t inc = (uint32_t)(((uint64_t)(uint32_t)rate * 398915783u) >> 28);
    int32_t depth = sc_mul(DEPTH[m], SC_ONE16 / 2 + p->depth, 16) * SC_FS;          /* samples, Q16 */
    const int32_t base = SC_Q16(0.012 * SC_FS);
    int32_t dry = SC_ONE16 - p->mix / 2, wet = p->mix;

    for (uint32_t i = 0; i < n; i++) {
        int32_t x = in[i];
        c->buf[c->w] = x;
        uint32_t si = c->ph >> 22, fr = (c->ph >> 6) & 0xFFFFu;
        int32_t sn = SC_SINE[si] * 2 + (int32_t)(((int64_t)(SC_SINE[si + 1] - SC_SINE[si]) * fr) >> 15);  /* Q16 */
        c->ph += inc;
        int32_t dm = sc_mul(depth, sn, 16);
        int32_t tap[2];
        for (int k = 0; k < 2; k++) {
            int32_t d = base + (k ? -dm : dm);                         /* Q16 samples */
            uint32_t di = (uint32_t)d >> 16, f = (uint32_t)d & 0xFFFFu;
            int32_t a = c->buf[(c->w - di) & (SC_CHORUS_LEN - 1)];
            int32_t b = c->buf[(c->w - di - 1) & (SC_CHORUS_LEN - 1)];
            tap[k] = a + (int32_t)(((int64_t)(b - a) * f) >> 16);
        }
        c->w = (c->w + 1) & (SC_CHORUS_LEN - 1);
        int32_t d0 = sc_mul(x, dry, 16);
        l[i] = d0 + sc_mul(tap[0], wet, 16);
        r[i] = d0 + sc_mul(tap[1], wet, 16);
    }
}

/* ---- the limiter --------------------------------------------------------------------------- */
/* The app ends in a DynamicsCompressorNode (threshold -3 dB, knee 6, ratio 12, attack 3 ms, release
 * 150 ms). This is that curve as a soft-knee gain computer in log2 units, on a peak detector, its
 * gain decided every 16 samples and ramped in between, with the makeup the Web Audio spec gives a
 * compressor ((1 / curve(0 dBFS))^0.6: +1.65 dB). Like Chrome's, the detector hears the audio
 * SC_LIM_LOOK samples before the gain is applied to it, so the 3 ms attack is done when a peak
 * arrives; a hard ceiling stays behind it for what still gets through. */
#define LIM_SUB     16
#define LIM_T       (-32656)                    /* -3 dB in log2 units, Q16 (-0.4983) */
#define LIM_W       65312                       /* 6 dB, Q16 (0.9966) */
#define LIM_INV2W   32880                       /* 1 / 2W, Q16 */
#define LIM_MAKEUP  17961                       /* +1.65 dB, Q16 (0.2741) */
#define LIM_ATT     58071                       /* e^(-16 / (0.003 fs)), Q16 */
#define LIM_REL     65378                       /* e^(-16 / (0.150 fs)), Q16 */

static int32_t log2_q16(uint32_t x)             /* x > 0 */
{
    int n = 0;
    uint32_t t = x;
    if (t >= 1u << 16) { t >>= 16; n += 16; }
    if (t >= 1u << 8) { t >>= 8; n += 8; }
    if (t >= 1u << 4) { t >>= 4; n += 4; }
    if (t >= 1u << 2) { t >>= 2; n += 2; }
    if (t >= 2u) n += 1;
    uint32_t m = n >= 30 ? x >> (n - 30) : x << (30 - n);       /* [2^30, 2^31) */
    uint32_t f = m - (1u << 30), i = f >> 24, fr = (f >> 8) & 0xFFFFu;
    return (n << 16) + (int32_t)(SC_LOG2_T[i] + (uint32_t)(((uint64_t)(SC_LOG2_T[i + 1] - SC_LOG2_T[i]) * fr) >> 16));
}

static int32_t exp2_q16(int32_t y)              /* 2^(y / 65536), Q16; y <= 0 here */
{
    int32_t ip = y >> 16;                       /* floor */
    uint32_t f = (uint32_t)y & 0xFFFFu, i = f >> 10, fr = f & 1023u;
    uint32_t v = SC_EXP2_T[i] + (uint32_t)(((uint64_t)(SC_EXP2_T[i + 1] - SC_EXP2_T[i]) * fr) >> 10);
    return ip >= 0 ? (int32_t)(v << ip) : ip < -30 ? 0 : (int32_t)(v >> -ip);
}

/* ---- the reverb ---------------------------------------------------------------------------- */
/* The app's reverb (fx/reverb.ts) is a ConvolverNode: its impulse is white noise through a one-pole
 * lowpass (the damping), decaying to -60 dB at its end (size: the algorithm's seconds), with a few
 * single-sample early reflections; Chrome scales an impulse to an rms of -58 dB. Wet = mix, dry =
 * 1 - mix / 2. Nine seconds of convolution is far beyond the FM-1, so this is a feedback delay
 * network with the same statistics: the same decay time (each line loses 60 dB over the size), the
 * same lowpass on its output, the same energy (the gain below), and L R uncorrelated. The early
 * reflections are left out: single samples at about the level of one sample of the tail.
 * tools/reverb_check.py measures all of it against the app's impulses. One reverb takes both layers
 * (the app has one per layer, with the same settings). */
static const uint16_t REV_D[SC_REV_LINES] = { 1031, 1171, 1301, 1433, 1559, 1693, 1811, 1949 };
/* the input enters each line this many samples before it comes out (not a whole line late) */
static const uint16_t REV_P[SC_REV_LINES] = { 67, 191, 307, 421, 547, 659, 773, 887 };
/* and with these signs: they sum to 0 against both output rows, so a mono input's first pass out of
 * the lines does not pile up at low frequencies (+3 to 5 dB below 160 Hz with all +), and the matrix
 * spreads them over four lines */
static const int8_t REV_S[SC_REV_LINES] = { 1, -1, -1, -1, -1, 1, 1, 1 };
/* RECIPES: base and longest seconds, brightness (Q16) */
static const int32_t REV_BASE[4] = { SC_Q16(0.6), SC_Q16(0.3), SC_Q16(1.2), SC_Q16(3.0) };
static const int32_t REV_MAX[4] = { SC_Q16(2.5), SC_Q16(1.4), SC_Q16(4.5), SC_Q16(9.0) };
static const int32_t REV_BRIGHT[4] = { SC_Q16(0.85), SC_Q16(0.65), SC_Q16(0.55), SC_Q16(0.35) };

static uint32_t isqrt64(uint64_t x)
{
    uint64_t r = 0, b = (uint64_t)1 << 62;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; } else r >>= 1;
        b >>= 2;
    }
    return (uint32_t)r;
}

void sc_reverb_prep(sc_reverb_t *r, const sc_reverb_p_t *p)
{
    int al = p->algo < 0 ? 0 : p->algo > 3 ? 3 : p->algo;
    if (p->on && !r->on) {                      /* switched on: no old tail */
        int32_t *b = &r->buf[0][0];
        for (uint32_t i = 0; i < SC_REV_LEN * SC_REV_LINES; i++) b[i] = 0;
        r->lpl = r->lpr = 0;
    }
    r->on = p->on;
    /* the decay: 10^(-3 d / N) = 2^(-log2(1000) d / N) a pass, N = the size in samples */
    int32_t sec = REV_BASE[al] + sc_mul(sc_clamp(p->size, 0, SC_ONE16), REV_MAX[al] - REV_BASE[al], 16);
    uint32_t len = (uint32_t)(((uint64_t)(uint32_t)sec * SC_FS) >> 16);
    for (int i = 0; i < SC_REV_LINES; i++) {
        uint32_t y = sc_udiv64((uint64_t)653118u * REV_D[i], len);                 /* Q16 */
        r->g[i] = (int32_t)(((int64_t)exp2_q16(-(int32_t)y) * 379625062) >> 16);   /* * 1/sqrt(8), Q30 */
    }
    /* the lowpass: cutoff 20000 (1 - damping) brightness + 200 Hz, a = 1 - e^(-2 pi fc / fs) */
    int32_t fc = sc_mul(sc_mul(20000, SC_ONE16 - sc_clamp(p->damping, 0, SC_ONE16), 16), REV_BRIGHT[al], 16) + 200;
    r->a = SC_ONE16 - exp2_q16(-(int32_t)(((int64_t)fc * 882827) >> 16));
    /* the energy: the lines' outputs are near independent, each with the state's energy over the
     * 11948 samples it holds, so a unit impulse on L and R (8 into the lines) gives the outputs
     * 8 * 8 * N / (2 ln 1000 * 11948), a / (2 - a) of that through the lowpass; Chrome's is
     * (10^(-58/20))^2 N. The first pass out of the lines comes before the energy is spread, which
     * adds about 1.037 (N + 12286) / N (fitted by tools/reverb_check.py). The gain:
     * 0.0628 sqrt((2 - a) / a) sqrt(N / (N + 12286)) */
    uint32_t ratio = sc_udiv64((uint64_t)(uint32_t)(2 * SC_ONE16 - r->a) << 16, (uint32_t)r->a);   /* Q16 */
    uint32_t early = sc_udiv64((uint64_t)len << 32, len + 12286u);                       /* Q32, < 1 */
    int32_t gain = sc_mul(sc_mul(4115, (int32_t)isqrt64((uint64_t)ratio << 16), 16), (int32_t)isqrt64(early), 16);
    r->wet = sc_mul(sc_clamp(p->mix, 0, SC_ONE16), gain, 16);
    r->dry = SC_ONE16 - sc_clamp(p->mix, 0, SC_ONE16) / 2;
}

/* In passes, as the voice is (one loop of everything spilled most of it on the pi32v2): the input,
 * the loop, the lowpass and the mix. The input may go in first for the whole block: a sample enters
 * a line REV_P samples before it is read, and REV_D - REV_P (>= 964) samples after the slot was last
 * written, so for n <= 256 this is the per-sample order, bit for bit. The lines are interleaved, so
 * one base and one mask reach them all: line k's slot j is word ((8 j + k) & REV_MASK). */
#define REV_MASK (SC_REV_LEN * SC_REV_LINES - 1)
#define REV_RD(k, d) b[(t8 + (k) - 8 * (d)) & REV_MASK]
#define REV_FB(k, h) o[k] = (int32_t)(((int64_t)(h) * g[k] + (1 << 29)) >> 30)
void sc_reverb_run(sc_reverb_t *r, int32_t *L, int32_t *R, uint32_t n)
{
    static int32_t ol[256], orr[256];
    int32_t *b = &r->buf[0][0];
    const int32_t *g = r->g;
    uint32_t w = r->w;

    /* the input: L into the even lines, R into the odd, with REV_S's signs */
#define REV_IN(k, x) b[(u + (k) + 8 * (REV_P[k] - REV_D[k])) & REV_MASK] += REV_S[k] > 0 ? (x) : -(x)
    for (uint32_t i = 0, u = 8 * w; i < n; i++, u += 8) {
        int32_t xl = L[i], xr = R[i];
        REV_IN(0, xl); REV_IN(1, xr); REV_IN(2, xl); REV_IN(3, xr);
        REV_IN(4, xl); REV_IN(5, xr); REV_IN(6, xl); REV_IN(7, xr);
    }
#undef REV_IN
    /* the loop: the lines' ends through the 8x8 Hadamard matrix (three butterfly passes), whose first
     * two rows are also the outputs, so L and R are uncorrelated */
    for (uint32_t i = 0, t8 = 8 * w; i < n; i++, t8 += 8) {
        int32_t y0 = REV_RD(0, 1031), y1 = REV_RD(1, 1171), y2 = REV_RD(2, 1301), y3 = REV_RD(3, 1433);
        int32_t a0 = y0 + y1, a1 = y0 - y1, a2 = y2 + y3, a3 = y2 - y3;
        int32_t c0 = a0 + a2, c1 = a1 + a3, c2 = a0 - a2, c3 = a1 - a3;
        int32_t y4 = REV_RD(4, 1559), y5 = REV_RD(5, 1693), y6 = REV_RD(6, 1811), y7 = REV_RD(7, 1949);
        int32_t a4 = y4 + y5, a5 = y4 - y5, a6 = y6 + y7, a7 = y6 - y7;
        int32_t c4 = a4 + a6, c5 = a5 + a7, c6 = a4 - a6, c7 = a5 - a7;
        int32_t *o = &b[t8 & REV_MASK];
        ol[i] = c0 + c4;
        orr[i] = c1 + c5;
        REV_FB(0, c0 + c4); REV_FB(4, c0 - c4); REV_FB(1, c1 + c5); REV_FB(5, c1 - c5);
        REV_FB(2, c2 + c6); REV_FB(6, c2 - c6); REV_FB(3, c3 + c7); REV_FB(7, c3 - c7);
    }
    r->w = w + n;
    /* the lowpass (the app's damping), then wet and dry */
    int32_t lpl = r->lpl, lpr = r->lpr, a = r->a, wet = r->wet, dry = r->dry;
    for (uint32_t i = 0; i < n; i++) {
        lpl += sc_mul(ol[i] - lpl, a, 16);
        lpr += sc_mul(orr[i] - lpr, a, 16);
        L[i] = sc_mul(L[i], dry, 16) + sc_mul(lpl, wet, 16);
        R[i] = sc_mul(R[i], dry, 16) + sc_mul(lpr, wet, 16);
    }
    r->lpl = lpl; r->lpr = lpr;
}
#undef REV_MASK
#undef REV_RD
#undef REV_FB

/* L R in Q20, in place: the gain they get, then Q15 */
static void limit(sc_limiter_t *lm, int32_t *L, int32_t *R, uint32_t n)
{
    if (lm->gain == 0) lm->gain = SC_ONE16;
    for (uint32_t a = 0; a < n; a += LIM_SUB) {
        uint32_t b = a + LIM_SUB < n ? a + LIM_SUB : n;
        int32_t pk = 0;
        for (uint32_t i = a; i < b; i++) {
            int32_t l = L[i] < 0 ? -L[i] : L[i], r = R[i] < 0 ? -R[i] : R[i];
            if (l > pk) pk = l;
            if (r > pk) pk = r;
        }
        pk >>= 5;                               /* Q15 */
        int32_t c = pk > lm->env ? LIM_ATT : LIM_REL;
        lm->env = pk + sc_mul(lm->env - pk, c, 16);
        int32_t g = LIM_MAKEUP;
        if (lm->env > 0) {
            int32_t lv = log2_q16((uint32_t)lm->env) - (15 << 16);  /* level, log2 of full scale */
            int32_t d = lv - LIM_T, out;
            if (2 * d < -LIM_W) out = lv;
            else if (2 * d <= LIM_W) {          /* the knee: L + (1/R - 1)(d + W/2)^2 / 2W */
                int32_t e = d + LIM_W / 2;
                out = lv - sc_mul(sc_mul(sc_mul(e, e, 16), SC_Q16(11.0 / 12.0), 16), LIM_INV2W, 16);
            } else out = LIM_T + d / 12;
            g += out - lv;
        }
        int32_t g1 = exp2_q16(g), g0 = lm->gain, m = (int32_t)(b - a);
        for (uint32_t i = a; i < b; i++) {
            int32_t gi = g0 + (g1 - g0) * (int32_t)(i - a + 1) / m;
            uint32_t k = lm->w;
            int32_t l = lm->dl[k], r = lm->dr[k];
            lm->dl[k] = L[i];
            lm->dr[k] = R[i];
            lm->w = (k + 1) % SC_LIM_LOOK;
            L[i] = sc_mul(l, gi, 16) >> 5;
            R[i] = sc_mul(r, gi, 16) >> 5;
        }
        lm->gain = g1;
    }
}

void sc_render(sc_synth_t *s, int32_t *out, uint32_t n)
{
    static int32_t L[256], R[256], l2[256], r2[256];
    if (n > 256) n = 256;
    drain(s);
    int nv = npoly(s);
    int32_t vol = sc_mul(s->patch.volume, INV_SQRT[nv], 16);        /* the layer mixer and the master */
    int layers = s->patch.voice_mode == SC_VM_SINGLE ? 1 : 2;

    for (uint32_t i = 0; i < n; i++) L[i] = R[i] = 0;
    for (int k = 0; k < layers; k++) {
        sc_layer_state_t *ls = &s->layer[k];
        int32_t *bus = s->bus[k];
        for (uint32_t i = 0; i < n; i++) bus[i] = 0;
        for (int i = 0; i < SC_VOICES; i++)
            if (!sc_voice_idle(&ls->v[i])) sc_voice_render(&ls->v[i], &ls->prep, bus, n);
        chorus(&ls->chorus, &s->patch.fx.chorus, bus, l2, r2, n);
        for (uint32_t i = 0; i < n; i++) { L[i] += l2[i]; R[i] += r2[i]; }
    }
    if (s->rev.on) sc_reverb_run(&s->rev, L, R, n);
    /* master volume, the limiter (as the patch's master.limiter), Q15 with a hard ceiling */
    for (uint32_t i = 0; i < n; i++) { L[i] = sc_mul(L[i], vol, 16); R[i] = sc_mul(R[i], vol, 16); }
    if (s->patch.limiter) limit(&s->lim, L, R, n);
    else for (uint32_t i = 0; i < n; i++) { L[i] >>= 5; R[i] >>= 5; }
    for (uint32_t i = 0; i < n; i++) {
        out[2 * i]     += sc_clamp(L[i], -32767, 32767);
        out[2 * i + 1] += sc_clamp(R[i], -32767, 32767);
    }
}
