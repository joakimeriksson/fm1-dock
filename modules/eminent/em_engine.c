/* SPDX-License-Identifier: GPL-3.0-only */
/* The Eminent 310 string ensemble (em_engine.h): the voices, the formant, the ensemble, the phaser. */
#include "em_engine.h"

/* EM_HOT: the render, the one function that runs every block; it calls nothing. A build that runs it from RAM
 * (out of the flash cache's way: ORRERY, with three more engines beside it) names its section here.
 * EM_IN: a helper the render uses, inlined into it whatever the compiler thinks (JieLi's clang 4 keeps a plain
 * static out of a function in another section, and a call from RAM does not reach the flash). */
#ifndef EM_HOT
#define EM_HOT
#endif
#define EM_IN static inline __attribute__((always_inline))

#define Q15 32768
#define Q30 1073741824
#define MUL(a, b, s) ((int32_t)(((int64_t)(a) * (b)) >> (s)))

/* the envelope: the attack a Q30 step a sample (20 ms .. 1.5 s, exponential in the knob), the release a one-pole's
 * coefficient (50 ms .. 3 s); the phaser's rate 0.05 .. 1.5 Hz as a phase step; the all-pass coefficient over the
 * sweep 300 Hz .. 3 kHz, 33 steps; each table over the knob's 0 .. 100 in tens, interpolated */
static const uint32_t ATT[11] = {1217394, 790542, 513356, 333360, 216475, 140573, 91284, 59277, 38493, 24996, 16232};
static const uint32_t REL[11] = {486847, 323304, 214693, 142566, 94670, 62864, 41744, 27719, 18406, 12222, 8116};
static const uint32_t PHR[11] = {4870, 6842, 9614, 13509, 18982, 26672, 37477, 52659, 73993, 103968, 146087};
static const uint16_t AP[33] = {31397, 31296, 31189, 31074, 30952, 30820, 30679, 30529, 30367, 30195, 30011, 29814, 29604,
                                29380, 29141, 28885, 28613, 28323, 28015, 27686, 27337, 26965, 26570, 26151, 25706, 25235,
                                24736, 24207, 23648, 23058, 22434, 21775, 21081};
static const uint32_t INC_C9[12] = {            /* MIDI 120..131: f 2^32 / 44100 */
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};

EM_IN uint32_t tab(const uint32_t *t, uint32_t p)   /* p 0..100 over eleven points */
{
    uint32_t k = p >= 100u ? 9u : p / 10u, r = p >= 100u ? 10u : p % 10u;
    return t[k] + (uint32_t)(((int64_t)((int32_t)t[k + 1] - (int32_t)t[k]) * (int32_t)r) / 10);
}

void em_init(em_t *e)
{
    uint32_t i, *w = (uint32_t *)e;
    for (i = 0; i < sizeof *e / 4; i++) w[i] = 0;
    e->p.reg = EM_STRINGS; e->p.attack = 45; e->p.release = 55; e->p.ensemble = 70; e->p.phaser = 25; e->p.color = 1;
    e->p.reverb = 30; e->p.volume = 80;
}
void em_set_patch(em_t *e, const em_patch_t *p)
{
    if (p != &e->p) e->p = *p;
    if (e->p.reg >= EM_N_REG) e->p.reg = EM_STRINGS;
}
uint32_t em_held(const em_t *e)
{
    uint32_t i, n = 0;
    for (i = 0; i < EM_VOICES; i++) n += e->v[i].gate;
    return n;
}
void em_note_on(em_t *e, uint32_t note, uint32_t vel)
{
    uint32_t i, pick = EM_VOICES, oldest = 0xFFFFFFFFu, quiet = EM_VOICES;
    int32_t low = Q30;
    (void)vel;
    if (note > 127u) return;
    for (i = 0; i < EM_VOICES; i++)             /* the same key again: that voice, held or still ringing (a
                                                 * divide-down key opens the one oscillator it has: a second voice
                                                 * for a ringing note doubled it, and a band's repeated chords kept
                                                 * all sixteen running); else a silent one, the quietest released,
                                                 * the oldest */
        if ((e->v[i].gate || e->v[i].env) && e->v[i].note == note) { pick = i; break; }
    if (pick == EM_VOICES)
        for (i = 0; i < EM_VOICES; i++) if (!e->v[i].gate && !e->v[i].env) { pick = i; break; }
    if (pick == EM_VOICES)
        for (i = 0; i < EM_VOICES; i++) if (!e->v[i].gate && e->v[i].env < low) { low = e->v[i].env; quiet = i; }
    if (pick == EM_VOICES && quiet < EM_VOICES) pick = quiet;
    if (pick == EM_VOICES)
        for (i = 0; i < EM_VOICES; i++) if (e->v[i].age < oldest) { oldest = e->v[i].age; pick = i; }
    {
        em_voice_t *v = &e->v[pick];
        uint32_t oct = (131u - note) / 12u;     /* C9's octave down to the note's */
        v->note = (uint8_t)note;
        v->gate = 1;
        v->age = ++e->ages;
        v->inc = INC_C9[note % 12u] >> oct;        /* the note's row of C9's octave (120 is a C: before, the row was
                                                    * one up: every key a semitone sharp, a B the C eleven below) */
    }
}
void em_note_off(em_t *e, uint32_t note)
{
    uint32_t i;
    for (i = 0; i < EM_VOICES; i++) if (e->v[i].gate && e->v[i].note == note) e->v[i].gate = 0;
}
void em_all_off(em_t *e)
{
    uint32_t i;
    for (i = 0; i < EM_VOICES; i++) e->v[i].gate = 0;
}

/* a sawtooth with the step rounded (PolyBLEP): ph the phase, inc its step; Q15 */
EM_IN int32_t saw(uint32_t ph, uint32_t inc)
{
    int32_t s = (int32_t)(ph >> 16) - 32768, c = 0;
    if (ph < inc) {                             /* just after the drop: t in 0..1 of the step */
        int32_t x = (int32_t)(((uint64_t)ph << 15) / inc);          /* Q15 */
        c = -((2 * x) - ((x * x) >> 15) - Q15);
    } else if (0u - ph < inc) {                 /* just before it */
        int32_t x = -(int32_t)(((uint64_t)(0u - ph) << 15) / inc);
        c = -(((x * x) >> 15) + 2 * x + Q15);
    }
    return s + c;
}

/* One footage of a key into acc (the first set, the others added): the saw at its level, n samples; the phase
 * after them comes back. Between two drops the saw is a line, so the samples go in runs: plain ones up to the
 * step (how many is one division), the two at the step with the rounding. A sample is under 2^16 and a level at
 * most Q15, so their product fits 32 bits. */
EM_IN uint32_t foot(int32_t *acc, uint32_t n, uint32_t ph, uint32_t inc, int32_t lvl, int add)
{
    int32_t *a = acc, *end = acc + n;
    if (!inc) inc = 1u;                         /* (no key has it: the division below) */
    while (a < end) {
        uint32_t next = ph + inc;               /* this sample's phase */
        if (next >= inc && 0u - next >= inc) {  /* no drop within a step of it, nor of the run samples after it */
            uint32_t run = (0u - inc - next) / inc + 1u;
            if (run > (uint32_t)(end - a)) run = (uint32_t)(end - a);
            do {
                int32_t x;
                ph += inc;
                x = (((int32_t)(ph >> 16) - 32768) * lvl) >> 15;
                if (add) *a++ += x; else *a++ = x;
            } while (--run);
        } else {
            int32_t x;
            ph = next;
            x = (saw(ph, inc) * lvl) >> 15;
            if (add) *a++ += x; else *a++ = x;
        }
    }
    return ph;
}

EM_HOT void em_render(em_t *e, int32_t *out, uint32_t n)
{
    const em_patch_t *p = &e->p;
    uint32_t i, k, nv = 0;
    int32_t att = (int32_t)tab(ATT, p->attack), rel = (int32_t)tab(REL, p->release);
    int32_t l16 = p->reg == EM_CELLO || p->reg == EM_FULL ? Q15 : 0;
    int32_t l8 = p->reg == EM_VIOLIN ? Q15 / 2 : Q15;
    int32_t l4 = p->reg == EM_STRINGS ? Q15 * 3 / 4 : p->reg == EM_VIOLIN || p->reg == EM_FULL ? Q15 : 0;
    int32_t gain = (int32_t)p->volume * 655 / 16;  /* a handful of keys, two footages each, under full scale */
    int32_t depth = (int32_t)p->ensemble * 655, dry_mix = p->ensemble ? 0 : Q15;
    uint32_t prate = p->phaser ? tab(PHR, p->phaser) : 0;
    int32_t fbk = p->color ? Q15 / 2 : 0;
    static int32_t mono[256], acc[256], wet_l[256], wet_r[256];   /* the keys' sum; one key's footages before its
                                                 * envelope; the two sides after the ensemble */
    if (n > 256u) n = 256u;
    for (i = 0; i < n; i++) mono[i] = 0;
    /* the voices: a pass a footage into acc, then the envelope into the sum. (One loop over everything a sample
     * was 75 instructions a key a sample on the FM-1, most of them the stack: sixteen keys were half a block.
     * The same numbers come out.) */
    for (k = 0; k < EM_VOICES; k++) {
        em_voice_t *v = &e->v[k];
        uint32_t ph8 = v->ph8, ph4 = v->ph4, ph16 = v->ph16, inc = v->inc;
        int32_t env = v->env;
        if (!v->gate && !env) continue;
        nv++;
        ph8 = foot(acc, n, ph8, inc, l8, 0);
        if (l4) ph4 = foot(acc, n, ph4, inc << 1, l4, 1); else ph4 += (inc << 1) * n;
        if (l16) ph16 = foot(acc, n, ph16, inc >> 1, l16, 1); else ph16 += (inc >> 1) * n;
        if (v->gate && env == Q30)              /* held and full: the key as it is */
            for (i = 0; i < n; i++) mono[i] += acc[i];
        else if (v->gate)
            for (i = 0; i < n; i++) {
                env += att; if (env > Q30) env = Q30;
                mono[i] += MUL(acc[i], env >> 15, 15);
            }
        else
            for (i = 0; i < n; i++) {
                env -= MUL(env, rel, 30); if (env < 2048) env = 0;
                mono[i] += MUL(acc[i], env >> 15, 15);
            }
        v->ph8 = ph8; v->ph4 = ph4; v->ph16 = ph16; v->env = env;
    }
    if (!nv && !e->svf_bp && !e->lp_y) {        /* silent: the lines empty too after a block of nothing */
        for (i = 0; i < 3u; i++) { uint32_t j; for (j = 0; j < EM_LINE; j++) e->line[i][j] = 0; }
        return;
    }
    /* the rest in passes as well, each with what it needs in registers (one loop over all of it was 560
     * instructions a frame): the formant, the ensemble, the phaser a channel, the output. The same numbers. */
    {   /* the formant: a high-pass at 250 Hz, a peak at 2.2 kHz, a roll-off from 6.5 kHz; mono becomes its output */
        int32_t hp_x = e->hp_x, hp_y = e->hp_y, lo = e->svf_lo, band = e->svf_bp, lp = e->lp_y;
        for (i = 0; i < n; i++) {
            int32_t x = MUL(mono[i], gain, 16), hp, y;
            hp = x - hp_x + MUL(hp_y, 31621, 15);
            hp_x = x; hp_y = hp;
            lo += MUL(band, 10229, 15);
            band = band + MUL(hp - lo - MUL(band, 16384, 15), 10229, 15);   /* Q 2 */
            y = hp + MUL(band, 40000, 15);
            lp += MUL(y - lp, 19789, 15);
            mono[i] = lp;
        }
        e->hp_x = hp_x; e->hp_y = hp_y; e->svf_lo = lo; e->svf_bp = band; e->lp_y = lp;
    }
    {   /* the ensemble: three taps, each 5 ms give or take 1.4 ms at 0.6 Hz and 0.15 ms at 6 Hz, a third of a
         * cycle apart; left the first and half the second, right the third and half the second; no dry.
         * (The three lines held the same samples: one is written and read.) */
        uint32_t w = e->w, slow = e->lfo_slow, fast = e->lfo_fast;
        int32_t *line = e->line[0];
        for (i = 0; i < n; i++) {
            int32_t y = mono[i], tap[3];
            w = (w + 1u) % EM_LINE;
            line[w] = y;
            slow += 58434u;                     /* 0.6 Hz */
            fast += 584338u;                    /* 6 Hz */
            for (k = 0; k < 3u; k++) {
                uint32_t ps = slow + k * 1431655765u, pf = fast + k * 1431655765u;
                int32_t ts = (int32_t)(ps >> 16) - 32768, tf = (int32_t)(pf >> 16) - 32768;   /* Q15 saws */
                int32_t tri_s = (ts < 0 ? -ts : ts) * 2 - Q15, tri_f = (tf < 0 ? -tf : tf) * 2 - Q15;
                int32_t d_q8 = 220 * 256 + MUL(((tri_s * (62 * 256)) >> 15) + ((tri_f * (7 * 256)) >> 15), depth, 16);   /* (a
                                                 * triangle is at most Q15: those two products fit 32 bits) */
                uint32_t di = (uint32_t)(d_q8 >> 8), fr = (uint32_t)(d_q8 & 255);
                uint32_t r0 = (w + EM_LINE - di) % EM_LINE, r1 = (r0 + EM_LINE - 1u) % EM_LINE;
                tap[k] = line[r0] + (int32_t)(((int64_t)(line[r1] - line[r0]) * (int32_t)fr) >> 8);
            }
            wet_l[i] = MUL(tap[0] + (tap[1] >> 1), 28000, 15) + MUL(y, dry_mix, 15);
            wet_r[i] = MUL(tap[2] + (tap[1] >> 1), 28000, 15) + MUL(y, dry_mix, 15);
        }
        e->w = w; e->lfo_slow = slow; e->lfo_fast = fast;
    }
    /* the phaser: four all-pass stages a channel, the sweep a triangle, the right channel's upside down */
    if (prate) {
        uint32_t ch;
        for (ch = 0; ch < 2u; ch++) {
            int32_t *io = ch ? wet_r : wet_l;
            int32_t x0 = e->ap_x[ch][0], x1 = e->ap_x[ch][1], x2 = e->ap_x[ch][2], x3 = e->ap_x[ch][3];
            int32_t y0 = e->ap_y[ch][0], y1 = e->ap_y[ch][1], y2 = e->ap_y[ch][2], y3 = e->ap_y[ch][3], fb = e->fb[ch];
            uint32_t sweep = e->lfo_ph;
            for (i = 0; i < n; i++) {
                uint32_t ph;
                int32_t t, tri, a, in, s, st;
                uint32_t pos, pi, pf;
                sweep += prate;
                ph = ch ? sweep + 0x80000000u : sweep;
                t = (int32_t)(ph >> 16) - 32768; tri = t < 0 ? -t : t;   /* 0 .. Q15: a triangle a cycle */
                pos = (uint32_t)(tri > 32767 ? 32767 : tri) * 32u; pi = pos >> 15; pf = pos & 32767u;   /* (pi 0 .. 31) */
                a = (int32_t)AP[pi] + (int32_t)(((int32_t)AP[pi + 1 > 32 ? 32 : pi + 1] - (int32_t)AP[pi]) * (int32_t)pf >> 15);
                in = io[i]; s = in + MUL(fb, fbk, 15);
                /* y = x1 - a x + a y1 a stage: a quarter turn at the table's frequency (with the signs the other
                 * way round it sat at 19 kHz and over, and the notches with it: nothing to hear) */
                st = x0 - MUL(s, a, 15) + MUL(y0, a, 15); x0 = s; y0 = st; s = st;
                st = x1 - MUL(s, a, 15) + MUL(y1, a, 15); x1 = s; y1 = st; s = st;
                st = x2 - MUL(s, a, 15) + MUL(y2, a, 15); x2 = s; y2 = st; s = st;
                st = x3 - MUL(s, a, 15) + MUL(y3, a, 15); x3 = s; y3 = st; s = st;
                fb = s;
                io[i] = (in + s) >> 1;
            }
            e->ap_x[ch][0] = x0; e->ap_x[ch][1] = x1; e->ap_x[ch][2] = x2; e->ap_x[ch][3] = x3;
            e->ap_y[ch][0] = y0; e->ap_y[ch][1] = y1; e->ap_y[ch][2] = y2; e->ap_y[ch][3] = y3; e->fb[ch] = fb;
        }
        e->lfo_ph += prate * n;
    }
    for (i = 0; i < n; i++) {
        out[2 * i] += wet_l[i];
        out[2 * i + 1] += wet_r[i];
    }
}
