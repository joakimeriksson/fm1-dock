/* SPDX-License-Identifier: GPL-3.0-only */
/* The Mini Pops engine (mp_engine.h): js/minipops-processor.js in integers, sample for sample in its
 * structure. Signals are Q20 (1.0 = 1 << 20: a voice's unit hit), the resonators' coefficients Q28, the
 * bandpass's Q30, levels Q16. */
#include "mp_engine.h"

/* MP_HOT: the render, the one function that runs every block; it calls nothing. A build that runs it from RAM
 * (out of the flash cache's way: ORRERY, with three more engines beside it) names its section here.
 * MP_IN: a helper the render uses, inlined into it whatever the compiler thinks (JieLi's clang 4 keeps a plain
 * static out of a function in another section, and a call from RAM does not reach the flash). */
#ifndef MP_HOT
#define MP_HOT
#endif
#define MP_IN static inline __attribute__((always_inline))

const char *const MP_VOICE_NAMES[MP_VOICES] = { "BD", "SD1", "SD2", "HH", "CY1", "CY2", "CB", "CO", "LB", "SB", "CL", "QU", "GU", "TA" };

#define Q20 (1 << 20)
#define Q16 65536
#define DC_K 1071644672                         /* 0.998, Q30: the drums' DC blocker */
#define BLOCK_MUL(a, b, sh) ((int32_t)(((int64_t)(a) * (b)) >> (sh)))

/* dsp-lib's fastTanh: x (27 + x^2) / (27 + 9 x^2), +-1 past +-3. In Q15 inside (one 32-bit division) */
MP_IN int32_t mp_tanh(int32_t x_q20)
{
    int32_t x = x_q20 >> 5, x2;
    int64_t num;
    int32_t den;
    if (x <= -3 * 32768) return -Q20;
    if (x >= 3 * 32768) return Q20;
    x2 = (int32_t)(((int64_t)x * x) >> 15);
    num = (int64_t)x * ((27 << 15) + x2);                               /* Q30 */
    den = (27 << 15) + 9 * x2;                                          /* Q15, >= 27 << 15 */
    return (int32_t)((num >> 8) / (den >> 8)) * 32;                     /* (both >> 8: the quotient Q15 fits) */
}

MP_IN int32_t res_run(int32_t *y1, int32_t *y2, int32_t x, int32_t a1, int32_t a2, int32_t gain)
{
    int32_t y = (int32_t)(((int64_t)x * gain + (int64_t)a1 * *y1 + (int64_t)a2 * *y2) >> 28);
    *y2 = *y1;
    *y1 = y;
    return y;
}
MP_IN int32_t svf_run(mp_voice_t *v, const mp_spec_t *s, int32_t x)
{
    int32_t v3 = x - v->ic2;
    int32_t v1 = (int32_t)(((int64_t)s->b_a1 * v->ic1 + (int64_t)s->b_a2 * v3) >> 30);
    int32_t v2 = v->ic2 + (int32_t)(((int64_t)s->b_a2 * v->ic1 + (int64_t)s->b_a3 * v3) >> 30);
    v->ic1 = 2 * v1 - v->ic1;
    v->ic2 = 2 * v2 - v->ic2;
    return v1;
}
MP_IN int32_t noise(mp_voice_t *v)      /* -1..1, Q20 (the web engine's LCG) */
{
    v->seed = v->seed * 1664525u + 1013904223u;
    return (int32_t)(v->seed >> 11) - Q20;
}

MP_IN void voice_trigger(mp_voice_t *v, int32_t vel, int32_t gate)
{
    v->active = 1;
    v->n = 0;
    v->vel = vel;
    v->env = 1 << 30;
    v->gate = gate;
    v->pulse = 0;
    v->lp = 0;                                  /* (the resonators are not reset: a drum struck while ringing adds to it) */
}

MP_IN int32_t voice_run(mp_voice_t *v, const mp_spec_t *s)
{
    int32_t imp = v->n == 0 ? Q20 : 0, out = 0, end;
    switch (s->kind) {
    case MP_DRUM: {
        int32_t y = res_run(&v->y1, &v->y2, imp, s->r_a1, s->r_a2, s->r_gain), sat, blocked;
        /* the transistor stage after the network clips one half harder: a little second harmonic, and
         * a DC term the blocker removes */
        sat = mp_tanh(BLOCK_MUL(y, s->drive, 16) + BLOCK_MUL(BLOCK_MUL(y, y, 20), s->c2, 16));
        if (s->mix) sat += BLOCK_MUL(res_run(&v->y1b, &v->y2b, imp, s->r2_a1, s->r2_a2, s->r2_gain), s->mix, 16);
        blocked = sat - v->dcx + BLOCK_MUL(v->dcy, DC_K, 30);
        v->dcx = sat;
        v->dcy = blocked;
        v->lp += (imp - v->lp) >> 2;            /* the trigger pulse itself leaks through as a short tick */
        out = blocked + BLOCK_MUL(v->lp, s->click, 16);
        break;
    }
    case MP_BELL: {
        int32_t y = BLOCK_MUL(res_run(&v->y1, &v->y2, imp, s->r_a1, s->r_a2, s->r_gain), 49152, 16) +
                    BLOCK_MUL(res_run(&v->y1b, &v->y2b, imp, s->r2_a1, s->r2_a2, s->r2_gain), s->mix, 16);
        out = mp_tanh(BLOCK_MUL(y, s->drive, 16));
        break;
    }
    case MP_NOISE: {
        int32_t x = BLOCK_MUL(noise(v), v->env, 30);
        out = svf_run(v, s, x);
        if (s->mix) out += BLOCK_MUL(res_run(&v->y1, &v->y2, imp, s->r_a1, s->r_a2, s->r_gain), s->mix, 16);
        v->env = BLOCK_MUL(v->env, s->env_mul, 30);
        break;
    }
    case MP_RATCHET: {
        int32_t tick = 0, running = s->life ? 1 : v->gate > 0;
        if (running && v->pulse <= 0) {
            tick = v->env >> 10;
            /* a jawbone's rattle is not a metronome: the ticks wander by a part of their spacing */
            v->pulse = s->jitter ? s->period + (int32_t)(((int64_t)s->period * s->jitter * noise(v) + ((int64_t)1 << 35)) >> 36) : s->period;
        }
        v->pulse--;
        if (v->gate > 0) v->gate--;
        v->lp += BLOCK_MUL(tick - v->lp, s->tick_k, 30);   /* the tick is softer than a one-sample impulse */
        out = svf_run(v, s, s->use_lp ? v->lp : tick);
        v->env = BLOCK_MUL(v->env, s->env_mul, 30);
        break;
    }
    default: break;
    }
    v->n++;
    end = s->kind == MP_RATCHET && !s->life
        ? v->gate <= 0 && v->n > (uint32_t)s->period * 2u && v->pulse < -(MP_SR / 50)
        : v->n > (uint32_t)s->life;
    if (end) v->active = 0;
    return BLOCK_MUL(BLOCK_MUL(out, v->vel, 16), s->gain, 16);
}

/* ---- the rhythm box */

void mp_init(mp_t *m)
{
    uint32_t i, *w = (uint32_t *)m;
    for (i = 0; i < sizeof *m / 4; i++) w[i] = 0;
    for (i = 0; i < MP_VOICES; i++) m->v[i].seed = MP_SPEC[i].seed;
    m->req_tempo = 120;
    m->req_volume = 80;
    m->req_balance = 50;
    m->req_seq = 1;
}
void mp_set_keys(mp_t *m, uint32_t keys, uint32_t section) { m->req_keys = (uint16_t)(keys & 0x3FFu); m->req_section = section != 0; m->req_seq++; }
void mp_set_tempo(mp_t *m, uint32_t bpm) { m->req_tempo = (uint16_t)(bpm < 20u ? 20u : bpm > 300u ? 300u : bpm); m->req_seq++; }
void mp_set_level(mp_t *m, uint32_t volume, uint32_t balance)
{
    m->req_volume = (uint8_t)(volume > 100u ? 100u : volume);
    m->req_balance = (uint8_t)(balance > 100u ? 100u : balance);
    m->req_seq++;
}
void mp_set_extra(mp_t *m, uint32_t qu, uint32_t gu, uint32_t ta)
{
    m->req_extra[0] = (uint8_t)(qu > 100u ? 100u : qu);
    m->req_extra[1] = (uint8_t)(gu > 100u ? 100u : gu);
    m->req_extra[2] = (uint8_t)(ta > 100u ? 100u : ta);
    m->req_seq++;
}
void mp_play(mp_t *m, uint32_t on) { m->req_play = on != 0; m->req_delay = 0; m->req_seq++; }
void mp_play_at(mp_t *m, uint32_t delay) { m->req_play = 1; m->req_delay = (uint16_t)delay; m->req_seq++; }
uint32_t mp_take_hits(mp_t *m) { uint32_t h = m->hit_mask; m->hit_mask = 0; return h; }

MP_IN void add_hit(mp_t *m, uint32_t voice, uint32_t pos100, uint32_t gate100)
{
    uint32_t i;
    if (pos100 >= 100u * m->len) return;
    for (i = 0; i < m->n_hits; i++)             /* keys pressed together OR their pulses, as the diode matrix does */
        if (m->hits[i].voice == voice && m->hits[i].pos100 == pos100 && !m->hits[i].gate100 == !gate100)
            return;
    if (m->n_hits >= MP_MAX_HITS) return;
    m->hits[m->n_hits].voice = (uint8_t)voice;
    m->hits[m->n_hits].pos100 = (uint16_t)pos100;
    m->hits[m->n_hits].gate100 = (uint16_t)gate100;
    m->n_hits++;
}
/* the rhythms of the keys down merged into one hit list; a 12-step key puts the counter in 12-step mode
 * for all of them, a triplet key in triplets */
MP_IN void rebuild(mp_t *m)
{
    uint32_t k, i, len = 16, div = 2, grid;
    const mp_extra_t *ex;
    for (k = 0; k < MP_KEYS; k++)
        if (m->keys >> k & 1u) {
            const mp_rhythm_t *r = mp_rhythm(m->section, k);
            if (r->len == 12) len = 12;
            if (r->div == 3) div = 3;
        }
    m->len = (uint8_t)len;
    m->div = (uint8_t)div;
    m->n_hits = 0;
    for (k = 0; k < MP_KEYS; k++)
        if (m->keys >> k & 1u) {
            const mp_rhythm_t *r = mp_rhythm(m->section, k);
            for (i = 0; i < r->n; i++) add_hit(m, MP_HITS[r->first + i].voice, MP_HITS[r->first + i].pos100, 0);
        }
    grid = len == 16 ? 0 : div == 2 ? 1 : 2;
    ex = &MP_EXTRA[grid];
    for (i = 0; i < len; i++) add_hit(m, MP_TA, 100u * i, 0);
    for (i = 0; i < ex->qu_n; i++) add_hit(m, MP_QU, MP_QU_POS[ex->qu_first + i], 0);
    for (i = 0; i < ex->gu_n; i++) add_hit(m, MP_GU, MP_GU_POS[ex->gu_first + i][0], MP_GU_POS[ex->gu_first + i][1]);
    m->sps_q8 = (uint32_t)MP_SR * 60u * 256u / ((uint32_t)m->tempo * div);
    if (m->step >= len) m->step = 0;
}

MP_IN void take_settings(mp_t *m)
{
    uint32_t seq = m->req_seq, play;
    int32_t bal;
    if (seq == m->seq) return;
    m->seq = seq;
    m->keys = m->req_keys;
    m->section = m->req_section;
    m->tempo = m->req_tempo;
    bal = (int32_t)m->req_balance * Q16 / 100;
    m->lo_g = 85197 - BLOCK_MUL(39322, bal, 16);   /* 1.3 - 0.6 bal: the lows down as the slider rises */
    m->hi_g = 26214 + BLOCK_MUL(78643, bal, 16);   /* 0.4 + 1.2 bal: the highs up */
    m->vol = (int32_t)m->req_volume * 36045 / 100; /* volume x 0.55 */
    m->extra[0] = (int32_t)m->req_extra[0] * Q16 / 100;
    m->extra[1] = (int32_t)m->req_extra[1] * Q16 / 100;
    m->extra[2] = (int32_t)m->req_extra[2] * Q16 / 100;
    rebuild(m);
    play = m->req_play;
    if (play && !m->playing) {                  /* started: from the first step, now */
        m->step = 0;
        m->counter_q8 = (int32_t)m->req_delay << 8;
        m->n_pend = 0;
        m->steps = 0;
    }
    if (!play) m->n_pend = 0;
    m->playing = (uint8_t)play;
}

MP_IN void hit(mp_t *m, uint32_t voice, int32_t gate)
{
    int32_t vel = Q16;
    if (voice == MP_QU || voice == MP_GU || voice == MP_TA) {   /* the additional rhythms at their slider's level */
        vel = m->extra[voice == MP_QU ? 0 : voice == MP_GU ? 1 : 2];
        if (vel <= 65) return;
    }
    if (m->req_volume) voice_trigger(&m->v[voice], vel, gate);   /* (at no volume nothing is struck: a rhythm turned all
                                                          * the way down cost what a loud one does) */
    m->hit_mask |= 1u << voice;
}
void mp_trigger(mp_t *m, uint32_t voice, int32_t vel)
{
    if (voice >= MP_VOICES) return;
    if (m->req_volume) voice_trigger(&m->v[voice], vel, voice == MP_GU ? MP_SR / 4 : 0);
    m->hit_mask |= 1u << voice;
}

/* this step's hits; hits between steps go into the pending list */
MP_IN void step_hits(mp_t *m)
{
    uint32_t i;
    for (i = 0; i < m->n_hits; i++) {
        const mp_mhit_t *h = &m->hits[i];
        uint32_t whole = h->pos100 / 100u, frac = h->pos100 % 100u;
        int32_t gate;
        if (whole != m->step) continue;
        gate = h->gate100 ? (int32_t)((h->gate100 * m->sps_q8 / 100u + 128u) >> 8) : 0;
        if (!frac) hit(m, h->voice, gate);
        else if (m->n_pend < MP_PENDING) {
            uint32_t k = m->n_pend++;
            m->pend_voice[k] = h->voice;
            m->pend_at[k] = (int32_t)((frac * m->sps_q8 / 100u + 128u) >> 8);
            m->pend_gate[k] = gate;
        }
    }
}

MP_HOT void mp_render(mp_t *m, int32_t *out, uint32_t n)
{
    uint32_t i, k, v, any = 0;
    take_settings(m);
    for (v = 0; v < MP_VOICES; v++) any |= m->v[v].active;
    if (!m->playing && !any && !m->n_pend && !m->bal_lp)
        return;                                 /* silent and stopped: nothing to do this block */
    for (i = 0; i < n; i++) {
        int32_t mix = 0, y, s;
        /* hits booked for between the steps: counted down before this sample's step is read, so a hit
         * booked N samples ahead lands N samples later */
        for (k = 0; k < m->n_pend; k++)
            if (--m->pend_at[k] <= 0) {
                uint32_t last;
                hit(m, m->pend_voice[k], m->pend_gate[k]);
                any = 1;
                last = --m->n_pend;
                m->pend_voice[k] = m->pend_voice[last];
                m->pend_at[k] = m->pend_at[last];
                m->pend_gate[k] = m->pend_gate[last];
                k--;
            }
        if (m->playing) {
            if (m->counter_q8 <= 0) {
                step_hits(m);
                any = 1;
                m->counter_q8 += (int32_t)m->sps_q8;
                m->step = (uint8_t)((m->step + 1u) % m->len);
                m->steps++;
            }
            m->counter_q8 -= 256;
        }
        if (any) {                              /* (the fourteen are looked at only while one may sound) */
            any = 0;
            for (v = 0; v < MP_VOICES; v++)
                if (m->v[v].active) { mix += voice_run(&m->v[v], &MP_SPEC[v]); any = 1; }
        }
        if (!mix && !m->bal_lp) continue;       /* silent: nothing to add (the output stage has a division) */
        /* tone balance: a one-pole split at 800 Hz, lows down and highs up as the slider rises */
        m->bal_lp += BLOCK_MUL(mix - m->bal_lp, MP_KBAL, 30);
        y = BLOCK_MUL(m->bal_lp, m->lo_g, 16) + BLOCK_MUL(mix - m->bal_lp, m->hi_g, 16);
        s = mp_tanh(BLOCK_MUL(y, m->vol, 16)) >> 5;   /* the machine is mono; its output stage rounds the peaks */
        out[2 * i] += s;
        out[2 * i + 1] += s;
    }
}
