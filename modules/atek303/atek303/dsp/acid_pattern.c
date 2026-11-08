/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Javier (Animatek)
 *
 * The ATEK303 SEQ's pattern generator and mutator (src/AcidPattern.hpp, src/AcidGen.hpp), in C.
 * Names and order follow the C++ so the two read side by side; every random draw happens in the
 * same order on the same stream, which is what keeps a seed's pattern identical. */
#include "acid_pattern.h"
#include "fastmath.h"                              /* fm_powf: the same 61,440 test lines as std::pow */

const acid_scale_t ACID_SCALES[ACID_SCALES_LEN] = {
    {"Acid (pent. menor)", "ACID", 5, {0, 3, 5, 7, 10}},
    {"Menor natural", "MEN", 7, {0, 2, 3, 5, 7, 8, 10}},
    {"Frigia", "FRIG", 7, {0, 1, 3, 5, 7, 8, 10}},
    {"Menor armonica", "ARM", 7, {0, 2, 3, 5, 7, 8, 11}},
    {"Dorica", "DOR", 7, {0, 2, 3, 5, 7, 9, 10}},
    {"Blues", "BLUE", 6, {0, 3, 5, 6, 7, 10}},
    {"Mayor", "MAY", 7, {0, 2, 4, 5, 7, 9, 11}},
    {"Cromatica", "CROM", 12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
};
const char *const ACID_NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

enum { C_PEDAL, C_RISE, C_FALL, C_ZIGZAG, C_SCATTER, CONTOUR_LEN };

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static float fminf_(float a, float b) { return a < b ? a : b; }
static float fmaxf_(float a, float b) { return a > b ? a : b; }
static float clamp01(float x) { return fmaxf_(0.0f, fminf_(1.0f, x)); }
static int iabs(int x) { return x < 0 ? -x : x; }
static long lround_(float x) { return x >= 0.0f ? (long)(x + 0.5f) : -(long)(-x + 0.5f); }   /* std::lround */

void acid_params_default(acid_params_t *p)
{
    p->steps = ACID_MAX_STEPS;
    p->scale = 0;
    p->density = 0.65f;
    p->accent = 0.6f;
    p->slide = 0.5f;
    p->range = 0.45f;
    p->tie = 0.4f;
}

int acid_semi_of(int d, int o, int scale)
{
    const acid_scale_t *sc = &ACID_SCALES[scale];
    int i = (d % sc->n + sc->n) % sc->n;
    return sc->s[i] + 12 * (o + (d - i) / sc->n);
}

/* AcidCorpus.hpp: frequencies measured over a corpus of acid lines (140 bars of 16 steps, 1928 notes); aggregate
 * numbers, not the music */
static const float PITCH_CLASS_WEIGHT[12] = {5.38f, 1.38f, 1.23f, 1.73f, 1.12f, 1.81f,
                                             1.00f, 1.46f, 1.73f, 1.19f, 2.19f, 1.08f};
#define VOCAB_MAX 7
static const float VOCAB_SIZE_CDF[VOCAB_MAX] = {0.143f, 0.264f, 0.457f, 0.636f, 0.772f, 0.851f, 1.000f};
static const float SLIDE_BY_LEAP[13] = {0.074f, 0.274f, 0.348f, 0.217f, 0.525f, 0.307f, 0.406f,
                                        0.241f, 0.307f, 0.226f, 0.280f, 0.320f, 0.391f};
#define TIE_RATE 0.072f
#define ACCENT_CELL_START 1.05f
#define ACCENT_CELL_REST 0.95f
static float slide_base(int leap)
{
    if (leap < 0)
        leap = -leap;
    return SLIDE_BY_LEAP[leap > 12 ? 12 : leap];
}
static float semi_weight(int semi) { return PITCH_CLASS_WEIGHT[((semi % 12) + 12) % 12]; }

/* ------------------------------------------------------------- the pattern --- */
static void pitch_zero(acid_pitch_t *p) { p->degree = p->octave = 0; p->accent = p->slide_out = 0; }

void acid_clear(acid_pattern_t *pat)
{
    int i;
    pat->seed = 1u;
    pat->algorithm_version = 5;
    pat->time_length = ACID_MAX_STEPS;
    pat->pitch_length = 1;
    for (i = 0; i < ACID_MAX_STEPS; i++) {
        pat->time[i] = ACID_REST;
        pitch_zero(&pat->pitch[i]);
    }
    pat->time[0] = ACID_NOTE;
}

void acid_sanitize(acid_pattern_t *pat, int scale) { acid_sanitize_ex(pat, scale, 0); }

void acid_sanitize_ex(acid_pattern_t *pat, int scale, int keep_slides)
{
    int i, s, e, notes = 0, active = 0, old_len, si;
    int event_step[ACID_MAX_STEPS];
    pat->time_length = (uint8_t)imax(1, imin(pat->time_length, ACID_MAX_STEPS));
    for (i = 0; i < pat->time_length; i++) {
        if (pat->time[i] > ACID_TIE)
            pat->time[i] = ACID_REST;
        if (pat->time[i] == ACID_TIE && !active)
            pat->time[i] = ACID_REST;
        if (pat->time[i] == ACID_NOTE) {
            active = 1;
            notes++;
        } else if (pat->time[i] == ACID_REST)
            active = 0;
    }
    for (i = pat->time_length; i < ACID_MAX_STEPS; i++)
        pat->time[i] = ACID_REST;
    if (notes == 0) {
        pat->time[0] = ACID_NOTE;
        notes = 1;
    }
    old_len = imax(0, imin(pat->pitch_length, ACID_MAX_STEPS));
    for (i = old_len; i < notes; i++)
        pitch_zero(&pat->pitch[i]);
    pat->pitch_length = (uint8_t)notes;
    for (i = 0; i < pat->pitch_length; i++) {
        pat->pitch[i].degree = (int8_t)imax(-24, imin(pat->pitch[i].degree, 24));
        pat->pitch[i].octave = (int8_t)imax(-2, imin(pat->pitch[i].octave, 2));
    }
    for (i = pat->pitch_length; i < ACID_MAX_STEPS; i++)
        pitch_zero(&pat->pitch[i]);
    si = imax(0, imin(scale, ACID_SCALES_LEN - 1));
    for (i = 0; i < ACID_MAX_STEPS; i++)
        event_step[i] = -1;
    e = 0;
    for (s = 0; s < pat->time_length && e < pat->pitch_length; s++)
        if (pat->time[s] == ACID_NOTE)
            event_step[e++] = s;
    /* a slide needs the next step to attack; into the same pitch it is legato (v5: allowed) */
    for (i = 0; !keep_slides && i < pat->pitch_length; i++) {
        int st;
        if (!pat->pitch[i].slide_out)
            continue;
        st = event_step[i];
        if (st < 0 || pat->time[(st + 1) % pat->time_length] != ACID_NOTE)
            pat->pitch[i].slide_out = 0;
    }
}

void acid_render(const acid_pattern_t *pat, acid_grid_t *out)
{
    int s, event = 0, have = 0;
    int8_t last_d = pat->pitch_length ? pat->pitch[0].degree : 0;
    int8_t last_o = pat->pitch_length ? pat->pitch[0].octave : 0;
    for (s = 0; s < ACID_MAX_STEPS; s++) {
        out->gate[s] = out->accent[s] = out->slide[s] = out->tie[s] = 0;
        if (s < pat->time_length && pat->time[s] == ACID_NOTE && event < pat->pitch_length) {
            const acid_pitch_t *p = &pat->pitch[event++];
            last_d = p->degree;
            last_o = p->octave;
            have = 1;
            out->gate[s] = 1;
            out->accent[s] = p->accent;
            out->slide[s] = p->slide_out;
        } else if (s < pat->time_length && pat->time[s] == ACID_TIE && have) {
            out->gate[s] = 1;
            out->tie[s] = 1;
        }
        out->deg[s] = last_d;
        out->oct[s] = last_o;
    }
}

/* ------------------------------------------------------------------ the RNG --- */
typedef struct { uint32_t state; } rng_t;
static void rng_init(rng_t *r, uint32_t seed) { r->state = seed ? seed : 1u; }
static uint32_t rng_next(rng_t *r)
{
    r->state ^= r->state << 13;
    r->state ^= r->state >> 17;
    r->state ^= r->state << 5;
    return r->state;
}
static float rng_unit(rng_t *r) { return (float)(rng_next(r) >> 8) * (1.0f / 16777216.0f); }
static uint32_t rng_mix(uint32_t seed, uint32_t stream)
{
    uint32_t z = seed ^ stream ^ 0x9e3779b9u;
    z ^= z >> 16;
    z *= 0x85ebca6bu;
    z ^= z >> 13;
    z *= 0xc2b2ae35u;
    z ^= z >> 16;
    return z ? z : 1u;
}
#define S_STYLE 0x5354594cu
#define S_TIME 0x54494d45u
#define S_PITCH 0x50495443u
#define S_OCTAVE 0x4f435441u
#define S_ACCENT 0x41434345u
#define S_SLIDE 0x534c4944u

/* -------------------------------------------------------------- generation --- */
static float activation_threshold(float u, int cell_start)
{
    float floor_ = cell_start ? 0.30f : 0.05f, slope = cell_start ? 0.70f : 0.80f;
    return u <= floor_ ? 0.0f : (u - floor_) / slope;
}
static int choose_vocab_size(float u, int scale_size)
{
    int n = VOCAB_MAX, i;
    for (i = 0; i < VOCAB_MAX; i++)
        if (u < VOCAB_SIZE_CDF[i]) {
            n = i + 1;
            break;
        }
    return n < scale_size ? n : scale_size;
}
static void sort_ints(int *v, int n)               /* std::sort on a few distinct ints */
{
    int i, j;
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && v[j - 1] > v[j]; j--) {
            int t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
}
static int build_vocab(int *voc, int scale, int want, rng_t *rng)
{
    const acid_scale_t *sc = &ACID_SCALES[scale];
    uint8_t taken[12] = {0};
    int nv = 1, d;
    voc[0] = 0;
    taken[0] = 1;
    while (nv < want && nv < 12) {
        float total = 0.0f, r;
        for (d = 1; d < sc->n; d++)
            if (!taken[d])
                total += semi_weight(sc->s[d]);
        if (total <= 0.0f)
            break;
        r = rng_unit(rng) * total;
        for (d = 1; d < sc->n; d++) {
            if (taken[d])
                continue;
            r -= semi_weight(sc->s[d]);
            if (r <= 0.0f) {
                taken[d] = 1;
                voc[nv++] = d;
                break;
            }
        }
    }
    sort_ints(voc, nv);
    return nv;
}
static int pick_contour(rng_t *rng)
{
    static const float W[CONTOUR_LEN] = {0.22f, 0.18f, 0.15f, 0.26f, 0.19f};
    float r = rng_unit(rng);
    int i;
    for (i = 0; i < CONTOUR_LEN; i++) {
        r -= W[i];
        if (r <= 0.0f)
            return i;
    }
    return C_PEDAL;
}
static void fold_pitch_range(acid_pattern_t *pat, int scale, int limit)
{
    int pass, i;
    for (pass = 0; pass < 6; pass++) {
        int lo = 999, hi = -999, mid, move_high, from, delta, moved = 0;
        for (i = 0; i < pat->pitch_length; i++) {
            int v = acid_semi_of(pat->pitch[i].degree, pat->pitch[i].octave, scale);
            lo = imin(lo, v);
            hi = imax(hi, v);
        }
        if (hi - lo <= limit)
            return;
        mid = (hi + lo) / 2;
        move_high = (hi - mid) >= (mid - lo);
        from = move_high ? hi : lo;
        delta = move_high ? -1 : 1;
        for (i = 0; i < pat->pitch_length; i++) {
            acid_pitch_t *p = &pat->pitch[i];
            int o;
            if (acid_semi_of(p->degree, p->octave, scale) != from)
                continue;
            o = p->octave + delta;
            if (o < -2 || o > 2)
                continue;
            p->octave = (int8_t)o;
            moved = 1;
        }
        if (!moved)
            return;
    }
}

int acid_generate(acid_pattern_t *pat, const acid_params_t *params, uint32_t seed)
{
    int length = imax(1, imin(params->steps, ACID_MAX_STEPS));
    int scale = imax(0, imin(params->scale, ACID_SCALES_LEN - 1));
    float density = clamp01(params->density), range = clamp01(params->range);
    float accent_amount = clamp01(params->accent), slide_amount = clamp01(params->slide);
    float tie_amount = clamp01(params->tie), tie_chance;
    float accent_curve = (accent_amount > 0.0f ? 0.90f * fm_powf(accent_amount, 1.27f) : 0.0f);
    rng_t style, time_rng, pitch_rng, octave_rng, accent_rng, slide_rng;
    int cell, contour, want_vocab, forced = 0, s, i, candidate_count = 0, vocab_length, event;
    float activation[ACID_MAX_STEPS];
    uint8_t tie_plan[ACID_MAX_STEPS] = {0};
    int candidate_at[ACID_MAX_STEPS], vocab[12], cell_vocab[4] = {0}, vocab_shift[ACID_MAX_STEPS] = {0};
    int event_at[ACID_MAX_STEPS];
    acid_pitch_t candidates[ACID_MAX_STEPS];

    rng_init(&style, rng_mix(seed, S_STYLE));
    rng_init(&time_rng, rng_mix(seed, S_TIME));
    rng_init(&pitch_rng, rng_mix(seed, S_PITCH));
    rng_init(&octave_rng, rng_mix(seed, S_OCTAVE));
    rng_init(&accent_rng, rng_mix(seed, S_ACCENT));
    rng_init(&slide_rng, rng_mix(seed, S_SLIDE));
    acid_clear(pat);
    pat->seed = seed ? seed : 1u;
    pat->algorithm_version = 5;
    pat->time_length = (uint8_t)length;
    cell = rng_unit(&style) < 0.25f ? 3 : 4;
    contour = pick_contour(&style);
    want_vocab = choose_vocab_size(rng_unit(&style), ACID_SCALES[scale].n);

    for (s = 0; s < length; s++) {
        activation[s] = activation_threshold(rng_unit(&time_rng), s % cell == 0);
        if (activation[s] < activation[forced])
            forced = s;
    }
    tie_chance = fminf_(0.90f, 2.77f * TIE_RATE * (tie_amount / 0.4f));
    for (s = 1; s < length; s++) {
        float chain = tie_plan[s - 1] ? 0.35f : 1.0f;
        /* (the C++ evaluates the order test first and draws only when it holds) */
        tie_plan[s] = activation[s - 1] <= activation[s] && rng_unit(&time_rng) < tie_chance * chain;
    }
    for (s = 0; s < length; s++) {
        int on = activation[s] <= density || s == forced;
        if (!on)
            pat->time[s] = ACID_REST;
        else if (s > 0 && tie_plan[s])
            pat->time[s] = ACID_TIE;
        else
            pat->time[s] = ACID_NOTE;
    }
    if (pat->time[forced] == ACID_TIE)
        pat->time[forced] = ACID_NOTE;

    for (i = 0; i < ACID_MAX_STEPS; i++)
        candidate_at[i] = -1;
    for (s = 0; s < length; s++) {
        int can_appear = activation[s] <= 1.0f || s == forced;
        int planned_tie = s > 0 && tie_plan[s] && s != forced;
        if (can_appear && !planned_tie)
            candidate_at[s] = candidate_count++;
    }
    pat->pitch_length = (uint8_t)candidate_count;
    vocab_length = build_vocab(vocab, scale, want_vocab, &pitch_rng);
    switch (contour) {
    case C_PEDAL:
        for (i = 0; i < cell; i++)
            cell_vocab[i] = (i == 0 || rng_unit(&pitch_rng) < 0.45f)
                                ? 0 : (int)(rng_unit(&pitch_rng) * (float)vocab_length) % vocab_length;
        break;
    case C_RISE:
    case C_FALL: {
        int a, b;
        for (i = 0; i < cell; i++)
            cell_vocab[i] = (int)(rng_unit(&pitch_rng) * (float)vocab_length) % vocab_length;
        for (a = 0; a < cell; a++)
            for (b = a + 1; b < cell; b++)
                if (contour == C_RISE ? (vocab[cell_vocab[b]] < vocab[cell_vocab[a]])
                                      : (vocab[cell_vocab[b]] > vocab[cell_vocab[a]])) {
                    int t = cell_vocab[a];
                    cell_vocab[a] = cell_vocab[b];
                    cell_vocab[b] = t;
                }
        if (rng_unit(&pitch_rng) < 0.6f)
            cell_vocab[0] = 0;
        break;
    }
    case C_ZIGZAG: {
        int top = vocab_length > 1
                      ? 1 + (int)(rng_unit(&pitch_rng) * (float)(vocab_length - 1)) % (vocab_length - 1) : 0;
        for (i = 0; i < cell; i++)
            cell_vocab[i] = i % 2 ? top : 0;
        break;
    }
    default:
        cell_vocab[0] = (int)(rng_unit(&pitch_rng) * (float)vocab_length) % vocab_length;
        for (i = 1; i < cell; i++)
            cell_vocab[i] = rng_unit(&pitch_rng) < 0.40f
                                ? cell_vocab[i - 1] : (int)(rng_unit(&pitch_rng) * (float)vocab_length) % vocab_length;
        break;
    }
    {
        int rep;
        for (rep = 1; rep * cell < ACID_MAX_STEPS; rep++)
            if (vocab_length > 1 && rng_unit(&pitch_rng) < 0.45f) {
                static const int SHIFT[6] = {1, -1, 1, -1, 2, -2};
                vocab_shift[rep] = SHIFT[(int)(rng_unit(&pitch_rng) * 6.0f) % 6];
            }
    }
    for (i = 0; i < ACID_MAX_STEPS; i++) {
        int cell_pos = i % cell, rep = i / cell, vi, degree;
        float jump, acc_chance;
        acid_pitch_t *p = &pat->pitch[i];
        vi = (cell_vocab[cell_pos] + vocab_shift[rep]) % vocab_length;
        if (vi < 0)
            vi += vocab_length;
        degree = vocab[vi];
        if (rep > 0 && rng_unit(&pitch_rng) < 0.18f)
            degree = vocab[(int)(rng_unit(&pitch_rng) * (float)vocab_length) % vocab_length];
        p->degree = (int8_t)degree;
        jump = range * (cell_pos == 0 ? 0.55f : 0.38f);
        if (rng_unit(&octave_rng) < jump) {
            float two = fmaxf_(0.0f, fminf_(1.0f, (range - 0.35f) / 0.65f)) * 0.45f;
            int amount = rng_unit(&octave_rng) < two ? 2 : 1;
            p->octave = (int8_t)(rng_unit(&octave_rng) < 0.55f ? amount : -amount);
        }
        acc_chance = cell_pos == 0 ? fminf_(0.95f, ACCENT_CELL_START * accent_curve) : fminf_(0.90f, ACCENT_CELL_REST * accent_curve);
        p->accent = rng_unit(&accent_rng) < acc_chance;
        p->slide_out = 0;
    }
    fold_pitch_range(pat, scale, 12 + (int)lround_(range * 24.0f));
    for (i = 0; i < ACID_MAX_STEPS; i++)
        candidates[i] = pat->pitch[i];
    pat->pitch_length = 0;
    for (s = 0; s < length; s++) {
        int c;
        if (pat->time[s] != ACID_NOTE)
            continue;
        c = candidate_at[s];
        if (c >= 0 && c < candidate_count)
            pat->pitch[pat->pitch_length++] = candidates[c];
    }
    for (i = pat->pitch_length; i < ACID_MAX_STEPS; i++)
        pitch_zero(&pat->pitch[i]);
    for (i = 0; i < ACID_MAX_STEPS; i++)
        event_at[i] = -1;
    event = 0;
    for (s = 0; s < length; s++)
        if (pat->time[s] == ACID_NOTE)
            event_at[s] = event++;
    for (s = 0; s < length; s++) {
        int here_e = event_at[s], next_step = (s + 1) % length, leap;
        float base;
        acid_pitch_t *here;
        const acid_pitch_t *next;
        if (here_e < 0 || event_at[next_step] < 0)
            continue;
        here = &pat->pitch[here_e];
        next = &pat->pitch[event_at[next_step]];
        leap = iabs(acid_semi_of(next->degree, next->octave, scale) - acid_semi_of(here->degree, here->octave, scale));
        if (leap == 0)
            continue;
        base = slide_base(leap);            /* v5: the big leaps slide more, the octave most */
        here->slide_out = rng_unit(&slide_rng) < fminf_(0.95f, base * 2.0f * slide_amount);
    }
    acid_sanitize(pat, scale);
    return contour;
}

/* ---------------------------------------------------------------- mutation --- */
static int same(const acid_pattern_t *a, const acid_pattern_t *b)
{
    int i;
    if (a->seed != b->seed || a->algorithm_version != b->algorithm_version || a->time_length != b->time_length
        || a->pitch_length != b->pitch_length)
        return 0;
    for (i = 0; i < a->time_length; i++)
        if (a->time[i] != b->time[i])
            return 0;
    for (i = 0; i < a->pitch_length; i++)
        if (a->pitch[i].degree != b->pitch[i].degree || a->pitch[i].octave != b->pitch[i].octave
            || a->pitch[i].accent != b->pitch[i].accent || a->pitch[i].slide_out != b->pitch[i].slide_out)
            return 0;
    return 1;
}
static int event_index_at_step(const acid_pattern_t *pat, int wanted)
{
    int e = 0, s;
    for (s = 0; s < pat->time_length; s++) {
        if (pat->time[s] != ACID_NOTE)
            continue;
        if (s == wanted)
            return e;
        e++;
    }
    return -1;
}
static void remove_pitch(acid_pattern_t *pat, int e)
{
    int i;
    if (e < 0 || e >= pat->pitch_length)
        return;
    for (i = e; i + 1 < pat->pitch_length; i++)
        pat->pitch[i] = pat->pitch[i + 1];
    pat->pitch_length--;
    pitch_zero(&pat->pitch[pat->pitch_length]);
}
static void insert_pitch(acid_pattern_t *pat, int e, acid_pitch_t value)
{
    int i;
    if (pat->pitch_length >= ACID_MAX_STEPS)
        return;
    e = imax(0, imin(e, pat->pitch_length));
    for (i = pat->pitch_length; i > e; i--)
        pat->pitch[i] = pat->pitch[i - 1];
    pat->pitch[e] = value;
    pat->pitch_length++;
}
/* the events whose attacks fall in [lo, hi): a page's */
static int events_in_range(const acid_pattern_t *pat, int lo, int hi, int *out)
{
    int found = 0, ev = 0, s;
    for (s = 0; s < pat->time_length; s++) {
        if (pat->time[s] != ACID_NOTE)
            continue;
        if (s >= lo && s < hi && found < ACID_MAX_STEPS)
            out[found++] = ev;
        ev++;
    }
    return found;
}
static int mutate_time(acid_pattern_t *pat, rng_t *rng, int lo, int hi)
{
    int length = pat->time_length, attempt, s, i, span = hi - lo;
    if (span <= 0)
        return 0;
    for (attempt = 0; attempt < 48; attempt++) {
        int from = lo + (int)(rng_next(rng) % (uint32_t)span);
        int to = lo + (int)(rng_next(rng) % (uint32_t)span);
        int after = (from + 1) % length;
        if (from == to || pat->time[from] != ACID_NOTE || pat->time[to] != ACID_REST || pat->time[after] == ACID_TIE)
            continue;
        pat->time[from] = ACID_REST;
        pat->time[to] = ACID_NOTE;
        return 1;
    }
    for (s = imax(1, lo); s < hi; s++) {
        int e = 0, prev;
        if (pat->time[s] != ACID_TIE)
            continue;
        for (i = 0; i < s; i++)
            if (pat->time[i] == ACID_NOTE)
                e++;
        prev = imax(0, e - 1);
        insert_pitch(pat, e, pat->pitch[prev]);
        pat->time[s] = ACID_NOTE;
        return 1;
    }
    for (s = imax(1, lo); s < hi; s++) {
        int e;
        if (pat->time[s] != ACID_NOTE || pat->time[s - 1] == ACID_REST)
            continue;
        e = event_index_at_step(pat, s);
        pat->time[s] = ACID_TIE;
        remove_pitch(pat, e);
        return 1;
    }
    return 0;
}
static int mutate_pitch(acid_pattern_t *pat, rng_t *rng, int lo, int hi)
{
    acid_pitch_t *p;
    int cand[ACID_MAX_STEPS], n;
    if (!pat->pitch_length)
        return 0;
    n = events_in_range(pat, lo, hi, cand);
    if (n <= 0)
        return 0;
    p = &pat->pitch[cand[rng_next(rng) % (uint32_t)n]];
    if (p->octave >= 2)
        p->octave--;
    else if (p->octave <= -2)
        p->octave++;
    else
        p->octave += (rng_next(rng) & 1u) ? 1 : -1;
    return 1;
}
static int mutate_articulation(acid_pattern_t *pat, rng_t *rng, int scale, int lo, int hi)
{
    int cand[ACID_MAX_STEPS], n, span = hi - lo;
    if (!pat->pitch_length)
        return 0;
    n = events_in_range(pat, lo, hi, cand);
    if (n <= 0 || span <= 0)
        return 0;
    if (rng_next(rng) & 1u) {
        int event_at[ACID_MAX_STEPS], i, s, attempt, ev = 0;
        for (i = 0; i < ACID_MAX_STEPS; i++)
            event_at[i] = -1;
        for (s = 0; s < pat->time_length; s++)
            if (pat->time[s] == ACID_NOTE)
                event_at[s] = ev++;
        for (attempt = 0; attempt < span; attempt++) {
            int st = lo + (int)(rng_next(rng) % (uint32_t)span), next = (st + 1) % pat->time_length;
            int a = event_at[st], b = event_at[next], here, there;
            if (a < 0 || b < 0)
                continue;
            here = acid_semi_of(pat->pitch[a].degree, pat->pitch[a].octave, scale);
            there = acid_semi_of(pat->pitch[b].degree, pat->pitch[b].octave, scale);
            if (here == there)
                continue;
            pat->pitch[a].slide_out = (uint8_t)!pat->pitch[a].slide_out;
            return 1;
        }
    }
    {
        int e = cand[rng_next(rng) % (uint32_t)n];
        pat->pitch[e].accent = (uint8_t)!pat->pitch[e].accent;
    }
    return 1;
}
static int mutate(acid_pattern_t *pat, int layer, uint32_t index, int scale, int lo, int hi)
{
    acid_pattern_t before;
    uint32_t tag = layer == ACID_MUT_TIME ? 0x4d54494du : layer == ACID_MUT_PITCH ? 0x4d504954u : 0x4d415254u;
    rng_t rng;
    int changed;
    lo = imax(0, imin(lo, pat->time_length));
    hi = imax(lo, imin(hi, pat->time_length));
    if (hi <= lo)
        return 0;
    before = *pat;
    rng_init(&rng, rng_mix(pat->seed ^ (0x9e3779b9u * index), tag));
    if (layer == ACID_MUT_TIME)
        changed = mutate_time(pat, &rng, lo, hi);
    else if (layer == ACID_MUT_PITCH)
        changed = mutate_pitch(pat, &rng, lo, hi);
    else
        changed = mutate_articulation(pat, &rng, scale, lo, hi);
    acid_sanitize(pat, scale);
    return changed && !same(&before, pat);
}

int acid_mutate_burst(acid_pattern_t *pat, int layer, uint32_t first, int operations, int scale, uint32_t *last)
{
    return acid_mutate_burst_range(pat, layer, first, operations, scale, last, 0, ACID_MAX_STEPS);
}
int acid_mutate_burst_range(acid_pattern_t *pat, int layer, uint32_t first, int operations, int scale, uint32_t *last,
                            int lo, int hi)
{
    acid_pattern_t before = *pat;
    int wanted = imax(1, operations), applied = 0, attempt;
    *last = first;
    for (attempt = 0; attempt < 32; attempt++) {
        uint32_t candidate = first + (uint32_t)attempt;
        *last = candidate;
        if (mutate(pat, layer, candidate, scale, lo, hi))
            applied++;
        if (applied >= wanted && !same(&before, pat))
            return 1;
    }
    *pat = before;
    return 0;
}

/* --------------------------------------------------------------- import --- */
/* AcidPatternV4::importRendered: a grid (edited by hand) back into the two layers. Pitches under a
 * REST had no attack and are dropped, as in the C++. */
void acid_import(acid_pattern_t *pat, const acid_grid_t *in, int length, uint32_t seed, uint8_t version)
{
    int s, active = 0;
    acid_clear(pat);
    pat->seed = seed ? seed : 1u;
    pat->algorithm_version = version;
    pat->time_length = (uint8_t)imax(1, imin(length, ACID_MAX_STEPS));
    pat->pitch_length = 0;
    for (s = 0; s < pat->time_length; s++) {
        acid_pitch_t *p;
        if (!in->gate[s]) {
            pat->time[s] = ACID_REST;
            active = 0;
            continue;
        }
        if (in->tie[s] && active) {
            pat->time[s] = ACID_TIE;
            continue;
        }
        pat->time[s] = ACID_NOTE;
        active = 1;
        p = &pat->pitch[pat->pitch_length++];
        p->degree = in->deg[s];
        p->octave = in->oct[s];
        p->accent = in->accent[s];
        p->slide_out = in->slide[s];
    }
}
