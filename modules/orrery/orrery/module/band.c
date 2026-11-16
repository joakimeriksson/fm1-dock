/* SPDX-License-Identifier: GPL-3.0-only */
#include "band.h"
#include "instruments/electronic_fx.h"
#include "arp_presets.h"   /* fm1-dock: the engine of this tree (take.py) */
#include "mp_engine.h"   /* fm1-dock: the engine of this tree (take.py) */
#include <string.h>

/* ARP 1601 gates as percent of a sixteenth (arp_engine.c: SHORT a quarter, LONG three quarters). */
#define GATE_SHORT 25u
#define GATE_LONG 75u
static const uint8_t CHORD_QUALITY[MD_BAND_CHORD_KINDS] = {
    0, MD_CHORD_MINOR, MD_CHORD_MAJOR, MD_CHORD_MINOR7, MD_CHORD_MAJOR7, MD_CHORD_SUS2, MD_CHORD_SUS4, MD_CHORD_FIFTH};

static void empty(md_seq_pattern_t *p, uint8_t mode, uint8_t length, uint8_t division)
{
    memset(p, 0, sizeof *p);
    p->meta = (md_pattern_meta_t){.length = length, .division = division, .swing = 50, .mode = mode};
    for (uint32_t k = 0; k < MD_STEPS_PER_PATTERN; k++) p->step[k].probability = 100;
}
static uint32_t seq_length(const arp_seq_t *s)
{
    uint32_t n = 0;
    while (n < ARP_STEPS && s->step[n].gate != ARP_END) n++;
    return n ? n : 1u;
}
static void bass(md_seq_pattern_t *p, const arp_seq_t *s, int8_t transpose, md_band_report_t *r, int count)
{
    uint32_t n = seq_length(s);
    empty(p, MD_PATTERN_MONO, (uint8_t)n, MD_DIV_16TH);
    p->meta.transpose = transpose;
    for (uint32_t k = 0; k < n; k++) {
        const arp_step_t *a = &s->step[k];
        md_step_t *t = &p->step[k];
        if (a->gate == ARP_REST) continue;
        t->payload = a->note;
        t->velocity = 100;
        t->gate = (uint8_t)(a->gate == ARP_SHORT ? GATE_SHORT : GATE_LONG);
        if (a->accent) t->flags |= MD_STEP_ACCENT;
        if (a->gate == ARP_TIE) {
            /* TIE: no new attack. To the same pitch it is our TIE; to a new pitch the 2600 moves
             * legato, which v1 expresses as SLIDE (a short glide where the patch has none). */
            const arp_step_t *prev = &s->step[(k + n - 1u) % n];
            if (prev->note == a->note) t->flags |= MD_STEP_TIE;
            else { t->flags |= MD_STEP_SLIDE; if (count) r->ties_as_slides++; }
        }
    }
}
/* A chord's voicing: the root where it is, the tones over it in one octave (from `floor`, the lowest root of the
 * range, an octave up), whatever the root: a tone two chords share stays where it is and the others move a step,
 * as a player's hands do, instead of every chord the same shape moved bodily (seq.h: MD_CHORD_RAISE). */
static uint16_t voiced(uint32_t root, uint32_t floor, uint8_t quality)
{
    static const uint8_t IV[MD_CHORD_COUNT][4] = {{0, 4, 7, 12}, {0, 3, 7, 12}, {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 2, 7, 12},
                                                  {0, 5, 7, 12}, {0, 7, 12, 19}};   /* (seq.c's) */
    uint16_t v = (uint16_t)(root | (uint16_t)(quality << 8));
    if (quality == MD_CHORD_FIFTH) return v;         /* (open as it is: raised, its fifth would land on its twelfth) */
    for (uint32_t i = 1; i < 4u; i++) if (root + IV[quality][i] < floor + 12u) v |= MD_CHORD_RAISE(i);
    return v;
}
static void chord(md_seq_pattern_t *p, const md_band_chord_t *c, uint8_t octave, uint8_t bars)
{
    empty(p, MD_PATTERN_CHORD, bars, MD_DIV_BAR);
    if (c->kind == MD_BAND_CHORD_NONE || c->kind >= MD_BAND_CHORD_KINDS) return;
    uint16_t root = (uint16_t)(36u + 12u * (octave > 4u ? 4u : octave) + c->root % 12u);
    for (uint32_t k = 0; k < bars; k++) {
        md_step_t *t = &p->step[k];
        t->payload = voiced(root, root - c->root % 12u, CHORD_QUALITY[c->kind]);
        t->velocity = 100;
        t->gate = 100;
        if (k) t->flags = MD_STEP_TIE;  /* held across the link's bars: one attack */
    }
}
/* Mini Pops rebuild() (mp_engine.c) onto the v1 grid: an eighth-note machine step is two sixteenths,
 * a triplet-eighth step 8/3 thirty-seconds. Off-grid source times move to the nearest grid step. */
typedef struct { uint32_t num, den; uint8_t division; } grid_t;
static void place(md_seq_pattern_t *p, const grid_t *g, uint32_t voice, uint32_t pos100, md_band_report_t *r)
{
    uint32_t scaled = pos100 * g->num;                  /* steps x (100 * den) */
    uint32_t at = (scaled + 50u * g->den) / (100u * g->den);
    uint32_t exact = at * 100u * g->den;
    uint32_t diff = exact > scaled ? exact - scaled : scaled - exact;
    if (diff) {
        r->drum_moved++;
        /* in quarters: one grid step is 1/4 (sixteenth) or 1/8 (thirty-second) of a quarter */
        uint32_t quarter_steps = g->division == MD_DIV_16TH ? 4u : 8u;
        uint32_t unit = 100u * g->den * quarter_steps;
        uint32_t permille = (diff * 1000u + unit - 1u) / unit;   /* rounded up: never understate */
        if (permille > r->drum_error_permille) r->drum_error_permille = (uint16_t)permille;
    }
    at %= p->meta.length;
    md_step_t *t = &p->step[at];
    if (t->payload & (1u << voice)) r->drum_merged++;
    t->payload |= (uint16_t)(1u << voice);
    t->velocity = 127;
    t->gate = 100;
    r->drum_hits++;
}
static void drums(md_seq_pattern_t *p, const md_band_settings_t *s, md_band_report_t *r)
{
    uint32_t len = 16, div = 2;
    for (uint32_t k = 0; k < MP_KEYS; k++)
        if (s->rhythm_keys >> k & 1u) {
            const mp_rhythm_t *rh = mp_rhythm(s->rhythm_section, k);
            if (rh->len == 12) len = 12;
            if (rh->div == 3) div = 3;
        }
    /* div 2: machine step = eighth = 2 sixteenths, or 4 thirty-seconds when a key has hits off the
     * sixteenth grid (the late triplet eighths of Swing and Latin Swing: the finer grid halves their
     * error and 16 x 4 still fits 64 steps); div 3: triplet eighth = 8/3 thirty-seconds */
    uint32_t fine = 0;
    for (uint32_t k = 0; k < MP_KEYS; k++)
        if (s->rhythm_keys >> k & 1u) {
            const mp_rhythm_t *rh = mp_rhythm(s->rhythm_section, k);
            for (uint32_t i = 0; i < rh->n; i++) fine |= MP_HITS[rh->first + i].pos100 % 50u != 0u;
        }
    grid_t g = div == 3 ? (grid_t){8u, 3u, MD_DIV_32ND} : fine ? (grid_t){4u, 1u, MD_DIV_32ND} : (grid_t){2u, 1u, MD_DIV_16TH};
    uint32_t length = (len * g.num + g.den / 2u) / g.den;
    empty(p, MD_PATTERN_DRUM, (uint8_t)length, g.division);
    for (uint32_t k = 0; k < MP_KEYS; k++)
        if (s->rhythm_keys >> k & 1u) {
            const mp_rhythm_t *rh = mp_rhythm(s->rhythm_section, k);
            for (uint32_t i = 0; i < rh->n; i++)
                place(p, &g, MP_HITS[rh->first + i].voice, MP_HITS[rh->first + i].pos100, r);
        }
    const mp_extra_t *ex = &MP_EXTRA[len == 16 ? 0 : div == 2 ? 1 : 2];
    if (s->extras & 1u) for (uint32_t i = 0; i < len; i++) place(p, &g, MP_TA, 100u * i, r);
    if (s->extras & 4u) for (uint32_t i = 0; i < ex->qu_n; i++) place(p, &g, MP_QU, MP_QU_POS[ex->qu_first + i], r);
    if (s->extras & 2u) for (uint32_t i = 0; i < ex->gu_n; i++) place(p, &g, MP_GU, MP_GU_POS[ex->gu_first + i][0], r);
}

/* AMBIENT: the eight chords of s->chord, two bars each (two eight-bar links); the root between F2 and E3 (at
 * the strings' octave 1; each octave more or less moves it twelve), a chord of kind NONE a two-bar rest */
static uint32_t amb_root(const md_band_settings_t *s, uint32_t i)
{
    int32_t note = 41 + (int32_t)((s->chord[i].root % 12u + 7u) % 12u) + 12 * ((int32_t)s->chord_octave - 1);
    return (uint32_t)(note < 36 ? note + 12 : note > 72 ? note - 12 : note);
}
static uint32_t amb_tones(uint8_t kind, uint8_t *iv)   /* the chord's tones within an octave */
{
    static const uint8_t T[MD_CHORD_COUNT][4] = {{0, 4, 7}, {0, 3, 7}, {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 2, 7}, {0, 5, 7}, {0, 7}};
    static const uint8_t N[MD_CHORD_COUNT] = {3, 3, 4, 4, 3, 3, 2};
    for (uint32_t i = 0; i < N[kind]; i++) iv[i] = T[kind][i];
    return N[kind];
}
/* AMBIENT's lead phrases (originals): over each two-bar chord a few notes on its tones, four motifs in turn, eighths
 * at the tempo, each note held (tied) so the echo and the hall carry it. A ladder index j is tone j % n of the chord,
 * j / n octaves up. The styles (s->phrase):
 *   1 HARP        long notes two octaves over the bass that rise, answer, climb, settle (made for the laser harp)
 *   2 LONG TONES  one or two notes a chord an octave lower, joined: for a sound that swells (brass, a choir, a violin)
 *   3 BELLS       a few short notes high up, off the beat, and room between: for a sound that rings (vibes, chimes)
 *   4 CALLS       three rising notes, six bars of quiet, three in answer, quiet again: a signal far off */
static void lead_phrase(const md_band_settings_t *s, md_seq_bank_t *b)
{
    static const uint8_t MOTIF[MD_BAND_PHRASES][4][5][3] = {   /* {step, length, ladder index (99: a ladder top)}, length 0 ends */
        {{{0, 3, 99}, {3, 3, 2}, {6, 2, 1}, {8, 8, 0}, {0, 0, 0}},
         {{0, 2, 1}, {2, 2, 2}, {4, 4, 99}, {8, 4, 100}, {12, 4, 2}},
         {{0, 6, 100}, {6, 2, 99}, {8, 3, 2}, {11, 5, 99}, {0, 0, 0}},
         {{0, 4, 2}, {4, 4, 1}, {8, 8, 0}, {0, 0, 0}, {0, 0, 0}}},
        {{{0, 10, 2}, {10, 6, 1}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
         {{0, 13, 99}, {13, 3, 2}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
         {{0, 6, 1}, {6, 10, 2}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
         {{0, 14, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}}},
        {{{2, 2, 99}, {7, 2, 2}, {12, 3, 100}, {0, 0, 0}, {0, 0, 0}},
         {{4, 2, 2}, {10, 3, 1}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
         {{0, 2, 100}, {3, 2, 99}, {6, 2, 2}, {11, 4, 1}, {0, 0, 0}},
         {{6, 2, 2}, {12, 4, 99}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}}},
        {{{0, 2, 0}, {2, 2, 1}, {4, 9, 2}, {0, 0, 0}, {0, 0, 0}},     /* (the ladder's first three: a bare fifth's are root, fifth, octave) */
         {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
         {{0, 2, 3}, {2, 2, 2}, {4, 9, 0}, {0, 0, 0}, {0, 0, 0}},
         {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}}}};
    static const uint8_t UP[MD_BAND_PHRASES] = {24, 12, 12, 24};   /* over the chord's root (F2 .. E3 at the strings' octave 1; the
                                                                    * sounds that ring speak an octave or three over their key) */
    static const uint8_t VEL[MD_BAND_PHRASES][2] = {{110, 96}, {100, 92}, {104, 84}, {100, 90}};   /* a motif's first note, the rest */
    uint32_t style = (uint32_t)(s->phrase - 1u) % MD_BAND_PHRASES;
    for (uint32_t p = 0; p < MD_PATTERNS_PER_TRACK; p++) {
        uint32_t half = p & 1u;
        md_seq_pattern_t *ld = &b->pattern[MD_BAND_LEAD][p];
        empty(ld, MD_PATTERN_MONO, 64, MD_DIV_8TH);
        for (uint32_t chord = 0; chord < 4u; chord++) {
            uint32_t i = half * 4u + chord;
            uint8_t iv[4], kind = s->chord[i].kind;
            if (kind == MD_BAND_CHORD_NONE || kind >= MD_BAND_CHORD_KINDS) continue;
            uint32_t n = amb_tones(CHORD_QUALITY[kind], iv), base = amb_root(s, i) + UP[style];
            if (base > 79u) base -= 12u;
            if (s->phrase_shift >= -24 && s->phrase_shift <= 24) base = (uint32_t)((int32_t)base + s->phrase_shift);
            for (uint32_t m = 0; m < 5u; m++) {
                const uint8_t *t = MOTIF[style][i % 4u][m];
                if (!t[1]) break;
                uint32_t j = t[2] >= 99u ? n + t[2] - 99u : t[2];
                uint32_t key = base + 12u * (j / n) + iv[j % n];
                for (uint32_t k = 0; k < t[1]; k++)
                    ld->step[chord * 16u + t[0] + k] = (md_step_t){.payload = (uint16_t)key, .velocity = VEL[style][m != 0],
                        .gate = 100, .flags = (uint8_t)(k ? MD_STEP_TIE : 0u), .probability = 100};
            }
        }
    }
}
/* The effect sequence. A degree is a step of the home key's pentatonic from the C4 octave. */
uint32_t md_band_fx_key(const md_band_settings_t *s, uint32_t degree)
{
    static const uint8_t PENTA[2][5] = {{0, 3, 5, 7, 10}, {0, 2, 4, 7, 9}};
    uint32_t root = 0, major = 0;
    if (degree >= MD_BAND_FX_DEGREES) degree = MD_BAND_FX_DEGREES - 1u;
    for (uint32_t k = 0; s && k < MD_BAND_LINKS; k++)
        if (s->chord[k].kind != MD_BAND_CHORD_NONE && s->chord[k].kind < MD_BAND_CHORD_KINDS) {
            root = s->chord[k].root % 12u;
            major = s->chord[k].kind == MD_BAND_CHORD_MAJ || s->chord[k].kind == MD_BAND_CHORD_MAJ7;
            break;
        }
    return 60u + root + 12u * (degree / 5u) + PENTA[major][degree % 5u];
}
/* The events of one 64-eighth pattern (first: its first eighth in the sequence, span: the sequence's length) onto its
 * steps, each a one-shot at its kit sound's own strength; lock[] takes the kit sound of each step, 0 none. */
/* (a PING is the quietest recipe, a DROP and a BUBBLE the loudest: electronic_fx_test's peaks. A tone that lasts
 * seconds is heard well under a droplet's peak, and noise needs all it can get) */
uint32_t md_band_fx_velocity(uint32_t sound)
{
    static const uint8_t VELOCITY[MD_FX_RECIPES] = {50, 127, 46, 66, 56,   /* DROP PING BUBBLE CHIRP METAL */
                                                    50, 14, 24, 22, 18,    /* ZAP WHISTLE GULL TWITTER BLEEPS */
                                                    80, 127, 127};         /* WHOOSH SURF THUNDER */
    return sound && sound <= MD_FX_RECIPES ? VELOCITY[sound - 1u] : 0u;
}
static void fx_events(const md_band_settings_t *s, md_seq_pattern_t *fx, uint8_t *lock, uint32_t first, uint32_t span)
{
    for (uint32_t k = 0; k < MD_BAND_FX_EVENTS; k++) {
        const md_band_fx_event_t *e = &s->fx[k];
        uint32_t at = e->at % span;
        if (!e->sound || e->sound > MD_FX_RECIPES || !e->chance || e->how || at < first || at >= first + 64u) continue;
        at -= first;
        if (lock[at]) continue;                  /* the first in the list has the eighth */
        fx->step[at] = (md_step_t){.payload = (uint16_t)md_band_fx_key(s, e->degree), .velocity = (uint8_t)md_band_fx_velocity(e->sound),
                                   .gate = 50, .probability = (uint8_t)(e->chance > 100u ? 100u : e->chance)};
        lock[at] = e->sound;
    }
}
static void fx_locks(md_seq_bank_t *b, uint32_t pattern, const uint8_t *lock)   /* in step order, patterns in order */
{
    for (uint32_t e = 0; e < 64u; e++)
        if (lock[e] && b->lock_count < MD_MAX_LOCKS)
            b->locks[b->lock_count++] = (md_lock_t){.step = (uint16_t)((MD_BAND_FX * 4u + pattern) * 64u + e),
                                                    .target = MD_FX_KIT, .value = (int16_t)lock[e]};
}
static void ambient(const md_band_settings_t *s, md_seq_bank_t *b, md_band_chain_t *c, md_band_report_t *r)
{
    c->links = 2; c->bars[0] = c->bars[1] = 8; c->total_bars = 16;
    for (uint32_t p = 0; p < MD_PATTERNS_PER_TRACK; p++) {
        uint32_t half = p & 1u;                  /* patterns 0/2 the first eight bars, 1/3 the second */
        md_seq_pattern_t *ch = &b->pattern[MD_BAND_CHORDS][p], *bs = &b->pattern[MD_BAND_BASS][p];
        md_seq_pattern_t *fx = &b->pattern[MD_BAND_FX][p];   /* the lead itself is left to the player */
        uint8_t lock[64] = {0};                  /* the kit sound locked on a step */
        empty(ch, MD_PATTERN_CHORD, 8, MD_DIV_BAR);
        empty(bs, MD_PATTERN_MONO, 8, MD_DIV_BAR);
        empty(fx, MD_PATTERN_MONO, 64, MD_DIV_8TH);
        for (uint32_t k = 0; k < 8u; k++) {      /* chords and a soft bass: one a bar, the second bar tied */
            uint32_t i = half * 4u + k / 2u, root = amb_root(s, i);
            uint8_t kind = s->chord[i].kind, tie = (uint8_t)(k & 1u ? MD_STEP_TIE : 0u);
            if (kind == MD_BAND_CHORD_NONE || kind >= MD_BAND_CHORD_KINDS) continue;
            ch->step[k] = (md_step_t){.payload = voiced(root, root - (s->chord[i].root % 12u + 7u) % 12u, CHORD_QUALITY[kind]), .velocity = 80,
                                      .gate = 100, .flags = tie, .probability = 100};
            bs->step[k] = (md_step_t){.payload = (uint16_t)(root - 12u), .velocity = 90, .gate = 100,
                                      .flags = tie, .probability = 100};
        }
        /* the effects box, one track: the run (the base sound, a glassy ping) up each chord's second bar */
        if (s->amb_arp)
            for (uint32_t chord = 0; chord < 4u; chord++) {
                uint32_t i = half * 4u + chord;
                uint8_t iv[4], kind = s->chord[i].kind;
                if (kind == MD_BAND_CHORD_NONE || kind >= MD_BAND_CHORD_KINDS) continue;
                uint32_t n = amb_tones(CHORD_QUALITY[kind], iv);
                for (uint32_t k = 0; k < 8u; k++) {
                    uint32_t key = amb_root(s, i) + 12u + 12u * (k / n) + iv[k % n];
                    if (key > 96u) key -= 12u;
                    fx->step[chord * 16u + 8u + k] = (md_step_t){.payload = (uint16_t)key, .velocity = 70,
                                                                 .gate = 60, .probability = 85};
                }
            }
        fx_events(s, fx, lock, 64u * half, MD_BAND_FX_SPAN);   /* the effect sequence over it */
        if (p < 2u) fx_locks(b, p, lock);        /* the chain plays patterns 0 and 1 */
    }
    if (s->phrase) lead_phrase(s, b);
    drums(&b->pattern[MD_BAND_DRUMS][0], s, r);   /* soft drums: the rhythm keys, as BAND (none: no drums) */
    for (uint32_t p = 1; p < MD_PATTERNS_PER_TRACK; p++) b->pattern[MD_BAND_DRUMS][p] = b->pattern[MD_BAND_DRUMS][0];
}
int md_band_build(const md_band_settings_t *s, md_seq_bank_t *b, md_band_chain_t *c, md_band_report_t *r)
{
    if (!s || !b || !c || !r) return MD_ERR_ARGUMENT;
    memset(b, 0, sizeof *b);
    memset(c, 0, sizeof *c);
    memset(r, 0, sizeof *r);
    b->seed = 0x1976u;
    for (uint32_t i = 0; i < 3; i++) b->global_owner[i] = MD_AUTOMATION_UNOWNED;
    for (uint32_t t = 0; t < MD_TRACKS; t++)        /* every track valid and silent until a scene fills it */
        for (uint32_t p = 0; p < MD_PATTERNS_PER_TRACK; p++) empty(&b->pattern[t][p], MD_PATTERN_MONO, 16, MD_DIV_16TH);
    if (s->scene == MD_BAND_SCENE_AMBIENT) { ambient(s, b, c, r); return md_seq_bank_validate(b); }
    const arp_seq_t *q = &ARP_SEQS[s->bass_seq % ARP_N_SEQS];
    c->links = q->n_chain ? (uint8_t)(q->n_chain < MD_PATTERNS_PER_TRACK ? q->n_chain : MD_PATTERNS_PER_TRACK) : 1u;
    if (q->n_chain > MD_PATTERNS_PER_TRACK) r->links_dropped = (uint8_t)(q->n_chain - MD_PATTERNS_PER_TRACK);
    for (uint32_t k = 0; k < MD_PATTERNS_PER_TRACK; k++) {
        uint32_t link = k < c->links ? k : 0u;
        uint8_t bars = q->n_chain ? (q->bars[link] ? q->bars[link] : 1u) : 1u;
        if (k < c->links) { c->bars[k] = bars; c->total_bars = (uint8_t)(c->total_bars + bars); }
        bass(&b->pattern[MD_BAND_BASS][k], q, q->n_chain ? q->tr[link] : 0, r, k < c->links);
        empty(&b->pattern[MD_BAND_LEAD][k], MD_PATTERN_MONO, 16, MD_DIV_16TH);
        /* a single link holds its chord four bars at a time */
        chord(&b->pattern[MD_BAND_CHORDS][k], &s->chord[link], s->chord_octave, c->links == 1u ? 4u : bars);
        if (k == 0) drums(&b->pattern[MD_BAND_DRUMS][0], s, r);
        else b->pattern[MD_BAND_DRUMS][k] = b->pattern[MD_BAND_DRUMS][0];
    }
    {   /* the effect sequence as one eight-bar pattern (it is not linked: it loops under the links) */
        md_seq_pattern_t *fx = &b->pattern[MD_BAND_FX][0];
        uint8_t lock[64] = {0};
        empty(fx, MD_PATTERN_MONO, 64, MD_DIV_8TH);
        fx_events(s, fx, lock, 0, 64u);
        fx_locks(b, 0, lock);
    }
    return md_seq_bank_validate(b);
}

uint32_t md_band_link_at(const md_band_chain_t *c, uint32_t bar)
{
    if (!c || c->links <= 1u || !c->total_bars) return 0;
    uint32_t at = bar % c->total_bars;
    for (uint32_t k = 0; k < c->links; k++) {
        if (at < c->bars[k]) return k;
        at -= c->bars[k];
    }
    return 0;
}
int md_band_next_link(const md_band_chain_t *c, uint32_t bar, uint8_t pattern, uint8_t queued, uint8_t requested)
{
    if (!c || c->links <= 1u) return -1;
    /* A pending launch is never replaced: the snapshot's bar can already be the new one while the launch for
     * that bar line is still due (a bar line exactly on a block boundary belongs to the next block), so a
     * "stale" launch is indistinguishable from one about to land. Wait for it; the next pass schedules on. */
    (void)requested;
    if (queued) return -1;
    uint32_t next = md_band_link_at(c, bar + 1u);
    return next == pattern ? -1 : (int)next;
}
