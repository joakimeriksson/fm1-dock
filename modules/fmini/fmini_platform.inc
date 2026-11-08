/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (Felucca's voice.c, as FMini-1 2.2.0 carries
 * it: the code below is taken from it for one part) */
/* FMINI-1 on fm1-dock: what FMini-1's host gives its FMINI engine (Felucca's core.h, voice.c and mod.c, as FMini-1
 * 2.2.0 builds them with FELUCCA_FMINI_ONLY), for one part. The engine (fmini/eng_fmini.c) and Felucca-Engines' dsp.c
 * are included as they are; this is the slice of the host they read:
 *   - the part (track_t: its parameters, voices, LFO, mod wheel, the filter contour's RELEASE) and its voices (voice_t,
 *     felucca_engine.h's);
 *   - the amplitude contour as FMini-1's env_tick (its attack charges toward a level above full and ends in a corner,
 *     the Minimoog's; the DECAY falls to the SUSTAIN; the RELEASE its own time), its steps from the tables (ENV SYNC
 *     off: no tempo here);
 *   - glide (FMini-1's: RATE mode, a step every 12th block), the LFO (RATE, WAVE, FADE; TRIG NOTE: it restarts with a
 *     fresh phrase), the pitch bend smoothed, the common modulation (LFO -> PITCH / AMP, the contour -> PITCH, ENV ->
 *     FILTER), velocity as loudness only (FMini-1 since 1.7.3);
 *   - the voice modes: MONO (each key retriggers, glide when GLIDE is set), LEGATO (FMini-1's MONO presets: glide and
 *     one contour between held keys), POLY (12 voices; a key's own voice reused, else a free one, else the oldest
 *     released, else the oldest held but the lowest), the note priority LOW / HIGH / LAST (FMini-1's GLO setting).
 * Not here: the modulation matrix, SPREAD, ENV SYNC and the LFO's tempo SYNC, the voice budget across parts, the
 * engine switch's fade, TRANSPOSE. Runs in the audio interrupt, but fmini_set_patch (the main loop's). */

#define NPART 1u
#define NTRK 1u
#define NVOICE 12u                              /* FMini-1's POLY 12 */
#define FELUCCA_FMINI_ONLY 1
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#include "fmini/felucca_engine.h"
#include "fmini/fmini_tables.h"
#include "fmini/dsp.c"                         /* (by its folder: probes/ has a dsp.c too) */

#define F_MCUT 14                               /* (FMini-1's core.h: the CUTOFF and EMPHASIS dials' formats) */
#define F_MEMP 15
enum {                                          /* the part's parameters (FMini-1's core.h, those the engine and this read) */
    P_LEVEL, P_ATK, P_DEC, P_SUS, P_REL, P_ED_FLT, P_ED_PIT, P_LRATE, P_LWAVE, P_LFADE,
    P_LD_PIT, P_LD_FLT, P_LD_AMP, P_LPOL, P_LTRIG, P_VOICE, P_GLIDE, P_GLMODE,
    P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7, P_COUNT
};
enum { V_POLY, V_MONO, V_LEGATO };              /* P_VOICE */
typedef struct track {
    int16_t p[P_COUNT];
    voice_t v[NVOICE];
    uint8_t eng_req;                            /* (always ENGI_FMINI) */
    uint8_t mw;                                 /* the mod wheel (CC 1) */
    uint8_t frel;                               /* the filter contour's RELEASE: bit 7 set = its own (low 7 bits), else the DECAY's */
    uint8_t nmono, mono_note;
    uint8_t mono_stack[8];
    int32_t lfo_val, lfo_fade;
    uint32_t lfo_ph, lfo_rnd;
} track_t;
#define FMINI_FREL(t) (((t)->frel & 0x80) ? ((t)->frel & 127) : ((t)->p[P_E4] & 127))
static track_t trk[NTRK];
static struct { uint32_t sel; int16_t g[1]; } song;   /* (the engine's tempo bytes: no song here) */
#define G_BPM 0
static int32_t glo_tune_c;                      /* (FMini-1's GLO TUNING, cents: A 440 here) */
static uint8_t glo_note_prio = 1;               /* NOTE PRIORITY: 0 LAST, 1 LOW (FMini-1's default), 2 HIGH */
#define fm1_ms (H->ms())

#include "fmini/eng_fmini.c"

/* FMini-1's bank (fmini_bank.c, not taken): a slot's patch is the factory's here (the module's own user slots are
 * fmini_module.c's) */
static void fmini_slot_patch(uint32_t s, uint8_t *dst) { memcpy(dst, FMINI_FACTORY[s % FMINI_NFACTORY], FG_SIZE); }
static void fb_names_init(void) {}

/* ---- voice.c's slice */
static uint32_t vage, glide_blk;
static int32_t lfo_seed = 0x2545F491;           /* the LFO's S&H (xorshift) */
static int32_t bend_q8, bend_target;            /* the bend, Q8 semitones: smoothed (~6 ms, an exact landing) */
static uint32_t esync_k[3];                     /* ATK's step, DEC's and REL's coefficient (Q16) this block */
#define GLIDE_SLOW 12u                          /* FMini-1: the glide moves every 12th block */

static int32_t lfo_wave(track_t *t, uint32_t ph)
{
    switch (t->p[P_LWAVE]) {
    case 1: return osc_tri(ph);
    case 2: return (int32_t)(ph >> 16) - 32768;
    case 3: return ph < 0x80000000u ? 32767 : -32767;
    case 4: return (int32_t)(t->lfo_rnd >> 16) - 32768;
    default: return osc_sine(ph);
    }
}
static void track_lfo_tick(track_t *t)
{
    uint32_t old = t->lfo_ph;
    t->lfo_ph += LFO_INC[t->p[P_LRATE] & 127];
    if (t->lfo_ph < old)
        t->lfo_rnd = noise32(&lfo_seed);        /* S&H: a new value each cycle */
    t->lfo_val = lfo_wave(t, t->lfo_ph);
    if (t->p[P_LPOL])
        t->lfo_val = (t->lfo_val + 32768) >> 1;
    if (t->lfo_fade < 32767) {
        int32_t step = (int32_t)(ENV_LIN[t->p[P_LFADE] & 127] >> 9);
        t->lfo_fade = t->p[P_LFADE] ? clamp(t->lfo_fade + (step ? step : 1), 0, 32767) : 32767;
    }
}

static void glide_set(track_t *t, voice_t *v, int glide)
{
    if (!glide || !v->pitch_cur) {
        v->pitch_cur = v->pitch16;
        v->gstep = 0;
        return;
    }
    if (t->p[P_GLMODE]) {
        int32_t d = v->pitch16 - v->pitch_cur, ticks = 1 + t->p[P_GLIDE] * t->p[P_GLIDE] / 8;
        d = d < 0 ? -d : d;
        v->gstep = d / ticks > 0 ? d / ticks : 1;
    } else {
        v->gstep = 0;
    }
}

static void voice_start(track_t *t, voice_t *v, uint32_t note, uint32_t vel, int glide)
{
    const engine_t *e = &ENG_FMINI;
    int sounding = v->active && v->stage != 0;
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2];
    int32_t s0 = v->s[0], s1 = v->s[1], s4 = v->s[4], s5 = v->s[5], s6 = v->s[6], s7 = v->s[7];
    v->note = (uint8_t)note;
    v->vel = (uint8_t)vel;
    v->gate = 1;
    v->active = 1;
    v->stage = 1;
    v->age = ++vage;
    v->pitch16 = (int32_t)note * 16;
    glide_set(t, v, glide);
    if (!sounding)
        v->env = v->env_out = 0;                /* sounding: the attack starts from the current level */
    e->note_on(t, v);
    if (sounding) {                             /* retrigger / steal: phases and the kept states stay (no click) */
        v->ph[0] = ph0;
        v->ph[1] = ph1;
        v->ph[2] = ph2;
        if (e->keep & 0x01u) v->s[0] = s0;
        if (e->keep & 0x02u) v->s[1] = s1;
        if (e->keep & 0x10u) v->s[4] = s4;
        if (e->keep & 0x20u) v->s[5] = s5;
        if (e->keep & 0x40u) v->s[6] = s6;
        if (e->keep & 0x80u) v->s[7] = s7;
    }
}

static uint32_t poly_cap = NVOICE;               /* fmini_module.c: 12 with the second core, 8 on one */
static uint32_t poly_n(void)
{
    uint32_t n = ENG_FMINI.poly && ENG_FMINI.poly < NVOICE ? ENG_FMINI.poly : NVOICE;
    return n < poly_cap ? n : poly_cap;
}

static voice_t *voice_alloc(track_t *t, uint32_t note)
{
    uint32_t i, np = poly_n(), best = np, low = np;
    for (i = 0; i < np; i++)                    /* the key's own voice */
        if (t->v[i].active && t->v[i].note == note)
            return &t->v[i];
    for (i = 0; i < np; i++)                    /* a free one */
        if (!t->v[i].active)
            return &t->v[i];
    for (i = 0; i < np; i++)                    /* the oldest released */
        if (!t->v[i].gate && (best == np || t->v[i].age < t->v[best].age))
            best = i;
    if (best < np)
        return &t->v[best];
    for (i = 0; i < np; i++)                    /* the lowest held note (the bass) is kept */
        if (low == np || t->v[i].note < t->v[low].note)
            low = i;
    for (i = 0; i < np; i++)                    /* the oldest held but that */
        if (i != low && (best == np || t->v[i].age < t->v[best].age))
            best = i;
    return &t->v[best == np ? 0 : best];
}

static void mono_play(track_t *t, uint32_t note, uint32_t vel, int retrig, int glide)
{
    voice_t *v = &t->v[0];
    v->fine = 0;
    if (retrig || !v->active || !v->gate) {
        voice_start(t, v, note, vel, glide);
    } else {                                    /* legato: the new pitch, the same contours */
        v->note = (uint8_t)note;
        v->pitch16 = (int32_t)note * 16;
        if (vel > v->vel)
            v->vel = (uint8_t)vel;
        glide_set(t, v, glide);
    }
    t->mono_note = (uint8_t)note;
}
static uint32_t mono_pick(const track_t *t)
{
    uint32_t i, n;
    if (!t->nmono)
        return 0;
    n = t->mono_stack[t->nmono - 1u];
    for (i = 0; i < t->nmono; i++) {
        if (glo_note_prio == 1 && t->mono_stack[i] < n) n = t->mono_stack[i];
        if (glo_note_prio == 2 && t->mono_stack[i] > n) n = t->mono_stack[i];
    }
    return n;
}
static void mono_remove(track_t *t, uint32_t note)
{
    uint32_t j, k = 0;
    for (j = 0; j < t->nmono; j++)
        if (t->mono_stack[j] != note)
            t->mono_stack[k++] = t->mono_stack[j];
    t->nmono = (uint8_t)k;
}

static void trk_note_on(track_t *t, uint32_t note, uint32_t vel)
{
    uint32_t any = 0, i, mode = (uint32_t)t->p[P_VOICE];
    for (i = 0; i < NVOICE; i++)
        any |= t->v[i].gate;
    if (!any) {                                 /* a fresh phrase: the LFO restarts (TRIG NOTE) and fades in */
        if (!t->p[P_LTRIG])
            t->lfo_ph = 0;
        t->lfo_fade = 0;
    }
    if (mode == V_POLY) {
        voice_t *v = voice_alloc(t, note);
        t->nmono = 0;
        t->mono_note = 0;
        v->fine = 0;
        voice_start(t, v, note, vel, t->p[P_GLIDE] != 0);
        return;
    }
    {
        uint32_t sounding = t->mono_note && t->v[0].active && t->v[0].gate, want;
        mono_remove(t, note);
        if (t->nmono >= 8u)
            mono_remove(t, t->mono_stack[0]);
        t->mono_stack[t->nmono++] = (uint8_t)note;
        want = mono_pick(t);
        if (sounding && want == t->mono_note)
            return;                             /* LOW / HIGH priority: this key does not win */
        if (mode == V_MONO)
            mono_play(t, want, vel, 1, t->p[P_GLIDE] != 0);
        else
            mono_play(t, want, vel, !sounding, sounding && t->p[P_GLIDE] != 0);
    }
}

static void trk_note_off(track_t *t, uint32_t note)
{
    uint32_t i, mode = (uint32_t)t->p[P_VOICE];
    mono_remove(t, note);
    if (mode != V_POLY && t->mono_note == note) {
        uint32_t next = mono_pick(t);
        if (next) {                             /* back to a held key, legato */
            mono_play(t, next, t->v[0].vel, 0, t->p[P_GLIDE] != 0);
        } else {
            if (t->v[0].gate) {
                t->v[0].gate = 0;
                t->v[0].stage = 3;
            }
            t->mono_note = 0;
        }
    }
    for (i = 0; i < NVOICE; i++)
        if (t->v[i].note == note && t->v[i].gate) {
            t->v[i].gate = 0;
            t->v[i].stage = 3;
        }
}

static void trk_all_off(track_t *t)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++) {
        t->v[i].gate = 0;
        t->v[i].stage = t->v[i].active ? 3 : 0;
    }
    t->nmono = 0;
    t->mono_note = 0;
}

/* one control tick of the amplitude contour (FMini-1's env_tick); Q15 */
static int32_t env_tick(track_t *t, voice_t *v)
{
    const int16_t *p = t->p;
    int32_t sus = (int32_t)p[P_SUS] << 17;
    switch (v->stage) {
    case 1:
        {   /* the Minimoog's capacitor charges toward a level above full: a concave rise that ends in a corner */
            int32_t d = (int32_t)(((int64_t)((5 << 22) - v->env) * (int32_t)(esync_k[0] >> 1) * 824) >> 32);
            v->env += d < 1 ? 1 : d;
        }
        if (v->env >= (1 << 24)) {
            v->env = 1 << 24;
            v->stage = 2;
        }
        break;
    case 2:
        v->env += mulq16(sus - v->env, esync_k[1]);
        break;
    case 3:
        v->env -= mulq16(v->env, esync_k[2]);
        if (v->env < (1 << 12)) {
            v->env = 0;
            v->stage = 0;
            v->active = 0;
        }
        break;
    default:
        v->env = 0;
        break;
    }
    return v->env >> 9;
}

/* the block's part-wide work (track_render's head, on core 0): the bend, the LFO, the contour steps, the engine's block */
static int32_t blk_lfo, blk_bend16, blk_bend_fine;
static void part_head(track_t *t)
{
    int32_t d = bend_target - bend_q8, bend;
    bend_q8 += d > 0 ? (d < 4 ? d : (d + 3) / 4) : (d > -4 ? d : (d - 3) / 4);
    bend = bend_q8;
    blk_bend16 = bend >= 0 ? bend / 16 : -((-bend + 15) / 16);
    blk_bend_fine = (bend - blk_bend16 * 16) * 2367 / 2560;
    track_lfo_tick(t);
    blk_lfo = mulq15(t->lfo_val, t->lfo_fade);
    esync_k[0] = ENV_LIN[t->p[P_ATK] & 127];
    esync_k[1] = ENV_EXP[t->p[P_DEC] & 127];
    esync_k[2] = ENV_EXP[t->p[P_REL] & 127];
    glide_blk++;
    ENG_FMINI.block(t);
}

/* a voice's control tick (track_render's loop): its contours, glide and the block's modulation; 0: nothing to render */
static int voice_prep(track_t *t, voice_t *v, vmod_t *m)
{
    const int16_t *p = t->p;
    int32_t env, pitch, lfo = blk_lfo;
    env = env_tick(t, v);
    m->envq15 = env;
    m->amp1 = mulq15(env, v->vel * 258);        /* velocity: loudness only */
    if (p[P_LD_AMP])
        m->amp1 = mulq15(m->amp1, 32767 - mulq15(p[P_LPOL] ? lfo : (lfo + 32768) >> 1, p[P_LD_AMP] * 258));
    m->amp0 = v->env_out;
    v->env_out = m->amp1;
    if (v->pitch_cur != v->pitch16 && glide_blk % GLIDE_SLOW == 0u) {
        int32_t d = v->pitch16 - v->pitch_cur;
        int32_t st = v->gstep ? v->gstep : (int32_t)(ENV_LIN[(p[P_GLIDE] ? p[P_GLIDE] : 40) & 127] >> 14);
        if (st < 1)
            st = 1;
        v->pitch_cur += d > 0 ? (d < st ? d : st) : (-d < st ? d : -st);
    }
    if (!env && !m->amp0 && v->stage == 2)
        return 0;                               /* held at a silent sustain */
    pitch = v->pitch_cur + blk_bend16 + ((lfo * p[P_LD_PIT] * 3) >> 15) + ((m->envq15 * p[P_ED_PIT] * 3) >> 15);
    m->pitch16 = clamp(pitch, 0, 2047);
    m->inc = pitch_inc((uint32_t)m->pitch16);
    m->fine = v->fine + blk_bend_fine;
    if (m->fine)
        m->inc += (uint32_t)((int32_t)(m->inc >> 12) * m->fine);
    m->cutoff = ((lfo * p[P_LD_FLT]) >> 7) + ((m->envq15 * p[P_ED_FLT]) >> 7);
    m->shape = 64 << 8;
    return 1;
}
