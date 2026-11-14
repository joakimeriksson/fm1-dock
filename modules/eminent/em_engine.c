/* SPDX-License-Identifier: GPL-3.0-only */
/* The Eminent 310 string ensemble (em_engine.h): the voices, the formant, the ensemble, the phaser. */
#include "em_engine.h"

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

static uint32_t tab(const uint32_t *t, uint32_t p)   /* p 0..100 over eleven points */
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
    for (i = 0; i < EM_VOICES; i++)             /* the same key again: that voice; else a silent one, the quietest
                                                 * released, the oldest */
        if (e->v[i].gate && e->v[i].note == note) { pick = i; break; }
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
static inline int32_t saw(uint32_t ph, uint32_t inc)
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

void em_render(em_t *e, int32_t *out, uint32_t n)
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
    static int32_t mono[256];
    if (n > 256u) n = 256u;
    for (i = 0; i < n; i++) mono[i] = 0;
    /* the voices */
    for (k = 0; k < EM_VOICES; k++) {
        em_voice_t *v = &e->v[k];
        uint32_t ph8 = v->ph8, ph4 = v->ph4, ph16 = v->ph16, inc = v->inc;
        int32_t env = v->env;
        if (!v->gate && !env) continue;
        nv++;
        for (i = 0; i < n; i++) {
            int32_t s;
            if (v->gate) { env += att; if (env > Q30) env = Q30; }
            else { env -= MUL(env, rel, 30); if (env < 2048) env = 0; }
            ph16 += inc >> 1; ph8 += inc; ph4 += inc << 1;
            s = MUL(saw(ph8, inc), l8, 15);
            if (l4) s += MUL(saw(ph4, inc << 1), l4, 15);
            if (l16) s += MUL(saw(ph16, inc >> 1), l16, 15);
            mono[i] += MUL(s, env >> 15, 15);
        }
        v->ph8 = ph8; v->ph4 = ph4; v->ph16 = ph16; v->env = env;
    }
    if (!nv && !e->svf_bp && !e->lp_y) {        /* silent: the lines empty too after a block of nothing */
        for (i = 0; i < 3u; i++) { uint32_t j; for (j = 0; j < EM_LINE; j++) e->line[i][j] = 0; }
        return;
    }
    for (i = 0; i < n; i++) {
        int32_t x = MUL(mono[i], gain, 16), hp, bp, y, l, r, wl, wr;
        uint32_t ch;
        /* the formant: a high-pass at 250 Hz, a peak at 2.2 kHz, a roll-off from 6.5 kHz */
        hp = x - e->hp_x + MUL(e->hp_y, 31621, 15);
        e->hp_x = x; e->hp_y = hp;
        e->svf_lo += MUL(e->svf_bp, 10229, 15);
        bp = e->svf_bp + MUL(hp - e->svf_lo - MUL(e->svf_bp, 16384, 15), 10229, 15);   /* Q 2 */
        e->svf_bp = bp;
        y = hp + MUL(bp, 40000, 15);
        e->lp_y += MUL(y - e->lp_y, 19789, 15);
        y = e->lp_y;
        /* the ensemble: three lines, each 5 ms give or take 1.4 ms at 0.6 Hz and 0.15 ms at 6 Hz, a third of a
         * cycle apart; left the first and half the second, right the third and half the second; no dry */
        e->w = (e->w + 1u) % EM_LINE;
        e->line[0][e->w] = e->line[1][e->w] = e->line[2][e->w] = y;
        e->lfo_slow += 58434u;                  /* 0.6 Hz */
        e->lfo_fast += 584338u;                 /* 6 Hz */
        {
            int32_t tap[3];
            for (k = 0; k < 3u; k++) {
                uint32_t ps = e->lfo_slow + k * 1431655765u, pf = e->lfo_fast + k * 1431655765u;
                int32_t ts = (int32_t)(ps >> 16) - 32768, tf = (int32_t)(pf >> 16) - 32768;   /* Q15 saws */
                int32_t tri_s = (ts < 0 ? -ts : ts) * 2 - Q15, tri_f = (tf < 0 ? -tf : tf) * 2 - Q15;
                int32_t d_q8 = 220 * 256 + MUL(MUL(tri_s, 62 * 256, 15) + MUL(tri_f, 7 * 256, 15), depth, 16);
                uint32_t di = (uint32_t)(d_q8 >> 8), fr = (uint32_t)(d_q8 & 255);
                uint32_t r0 = (e->w + EM_LINE - di) % EM_LINE, r1 = (r0 + EM_LINE - 1u) % EM_LINE;
                tap[k] = e->line[k][r0] + (int32_t)(((int64_t)(e->line[k][r1] - e->line[k][r0]) * (int32_t)fr) >> 8);
            }
            l = MUL(tap[0] + (tap[1] >> 1), 28000, 15) + MUL(y, dry_mix, 15);
            r = MUL(tap[2] + (tap[1] >> 1), 28000, 15) + MUL(y, dry_mix, 15);
        }
        /* the phaser: four all-pass stages a channel, the sweep a triangle, the right channel's upside down */
        if (prate) {
            e->lfo_ph += prate;
            for (ch = 0; ch < 2u; ch++) {
                uint32_t ph = ch ? e->lfo_ph + 0x80000000u : e->lfo_ph;
                int32_t t = (int32_t)(ph >> 16) - 32768, tri = t < 0 ? -t : t;   /* 0 .. Q15: a triangle a cycle */
                uint32_t pos = (uint32_t)(tri > 32767 ? 32767 : tri) * 32u, pi = pos >> 15, pf = pos & 32767u;   /* (pi 0 .. 31) */
                int32_t a = (int32_t)AP[pi] + (int32_t)(((int32_t)AP[pi + 1 > 32 ? 32 : pi + 1] - (int32_t)AP[pi]) * (int32_t)pf >> 15);
                int32_t in = (ch ? r : l), s = in + MUL(e->fb[ch], fbk, 15), st;
                for (k = 0; k < 4u; k++) {      /* y = x1 - a x + a y1: a quarter turn at the table's frequency (with
                                                 * the signs the other way round it sat at 19 kHz and over, and the
                                                 * notches with it: nothing to hear) */
                    st = e->ap_x[ch][k] - MUL(s, a, 15) + MUL(e->ap_y[ch][k], a, 15);
                    e->ap_x[ch][k] = s;
                    e->ap_y[ch][k] = st;
                    s = st;
                }
                e->fb[ch] = s;
                if (ch) wr = (r + s) >> 1; else wl = (l + s) >> 1;
            }
            l = wl; r = wr;
        }
        out[2 * i] += l;
        out[2 * i + 1] += r;
    }
}
