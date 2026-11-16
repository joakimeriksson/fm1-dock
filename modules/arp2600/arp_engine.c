/* SPDX-License-Identifier: GPL-3.0-only */
/* The ARP 2600 engine (arp_engine.h). The oscillators are PolyBLEP band-limited (a two-sample polynomial at
 * each edge); the ladder is four one-pole stages with the feedback to the first and a soft clip on the input,
 * run twice a sample; the envelopes are the analog curves (an attack that aims past the top, exponential
 * decays). Levels Q20, phases 32-bit, pitch Q16 notes. */
#include "arp_engine.h"

/* ARP_HOT: the render, the one function that runs every block; it calls nothing. A build that runs it from RAM
 * (out of the flash cache's way: ORRERY, with three more engines beside it) names its section here.
 * ARP_IN: a helper the render uses, inlined into it whatever the compiler thinks (JieLi's clang 4 keeps a plain
 * static out of a function in another section, and a call from RAM does not reach the flash). */
#ifndef ARP_HOT
#define ARP_HOT
#endif
#define ARP_IN static inline __attribute__((always_inline))

#define Q20 (1 << 20)
#define Q16 65536
#define Q30 (1 << 30)
#define MUL(a, b, sh) ((int32_t)(((int64_t)(a) * (b)) >> (sh)))

static const uint32_t EXP2_T[65] = {           /* 2^(i / 64), Q16 */
    65536u, 66250u, 66971u, 67700u, 68438u, 69183u, 69936u, 70698u, 71468u, 72246u,
    73032u, 73828u, 74632u, 75444u, 76266u, 77096u, 77936u, 78785u, 79642u, 80510u,
    81386u, 82273u, 83169u, 84074u, 84990u, 85915u, 86851u, 87796u, 88752u, 89719u,
    90696u, 91684u, 92682u, 93691u, 94711u, 95743u, 96785u, 97839u, 98905u, 99982u,
    101070u, 102171u, 103283u, 104408u, 105545u, 106694u, 107856u, 109031u, 110218u, 111418u,
    112631u, 113858u, 115098u, 116351u, 117618u, 118899u, 120194u, 121502u, 122825u, 124163u,
    125515u, 126882u, 128263u, 129660u, 131072u,
};
/* 2^(y / 65536) in Q16, for y in -30 .. +14 octaves */
ARP_IN int32_t exp2_q16(int32_t y)
{
    int32_t ip = y >> 16;
    uint32_t f = (uint32_t)y & 0xFFFFu, i = f >> 10, fr = f & 1023u;
    uint32_t v = EXP2_T[i] + (uint32_t)(((uint64_t)(EXP2_T[i + 1] - EXP2_T[i]) * fr) >> 10);
    if (ip >= 14) return 0x7FFFFFFF;
    return ip >= 0 ? (int32_t)(v << ip) : ip < -30 ? 0 : (int32_t)(v >> -ip);
}
/* dsp-lib's fastTanh on Q20: x (27 + x^2) / (27 + 9 x^2), +-1 past +-3 */
ARP_IN int32_t tanh_q20(int32_t x_q20)
{
    int32_t x = x_q20 >> 5, x2, den;
    int64_t num;
    if (x <= -3 * 32768) return -Q20;
    if (x >= 3 * 32768) return Q20;
    x2 = (int32_t)(((int64_t)x * x) >> 15);
    num = (int64_t)x * ((27 << 15) + x2);
    den = (27 << 15) + 9 * x2;
    return (int32_t)((num >> 8) / (den >> 8)) * 32; /* bounded Q15 quotient; signed shift would be undefined */
}
/* a one-pole's coefficient for a time constant of t milliseconds (Q16 ms): 1 - e^(-1 / (t fs)), Q30 */
static int32_t k_of_ms(int32_t ms_q16)
{
    /* 1 / (t fs) = 65536 / (ms_q16 * 44.1); e^-x = 2^(-x / ln 2) = 2^(-1.4427 x) */
    uint32_t samples = (((uint32_t)ms_q16 >> 8) * 441u / 10u) >> 8;   /* t fs (no 64-bit division) */
    int32_t x;
    if (samples < 2u) return Q30;
    x = (int32_t)(94548u / samples);            /* 1.4427 / samples, Q16 (94548 = 1.4427 * 65536) */
    return Q30 - (exp2_q16(-x) << 14);
}
/* a time parameter 0..100 to milliseconds Q16: 1 ms .. 10 s, exponential */
static int32_t ms_of_param(uint32_t p) { return exp2_q16((int32_t)p * 8710) ; }   /* 2^(p * 13.29 / 100) ms */
/* a frequency in Hz (Q16) from a MIDI note (Q16): 440 2^((n - 69) / 12) */
ARP_IN int32_t hz_of_note(int32_t note_q16) { return MUL(exp2_q16((note_q16 - 69 * Q16) / 12), 440 * Q16, 16); }
ARP_IN uint32_t inc_of_hz(int32_t hz_q16)     /* a phase increment: hz 2^32 / fs = hz_q16 * (2^32 / fs) / 2^16 */
{
    return (uint32_t)(((uint64_t)(uint32_t)hz_q16 * 97391u) >> 16);   /* 2^32 / 44100 = 97391.5 */
}

static void env_set(arp_env_t *e, uint32_t a, uint32_t d, uint32_t s, uint32_t r)
{
    e->ka = k_of_ms(ms_of_param(a) / 3);        /* (the attack aims at 1.25: a third of the time to reach 1) */
    e->kd = k_of_ms(ms_of_param(d) / 3);
    e->kr = k_of_ms(ms_of_param(r) / 3);
    e->sus = (int32_t)(s * (Q30 / 100));
}
ARP_IN int32_t env_run(arp_env_t *e)
{
    switch (e->stage) {
    case 1:
        e->level += MUL((int32_t)(Q30 * 1.25) - e->level, e->ka, 30);
        if (e->level >= Q30) { e->level = Q30; e->stage = 2; }
        break;
    case 2:
        e->level += MUL(e->sus - e->level, e->kd, 30);
        if (e->level - e->sus < 1024) e->stage = 3;
        break;
    case 3: e->level = e->sus; break;
    case 4:
        e->level -= MUL(e->level, e->kr, 30);
        if (e->level < 256) { e->level = 0; e->stage = 0; }
        break;
    default: break;
    }
    return e->level;
}

/* PolyBLEP: the residual at an edge, t the phase in cycles (Q32 as uint32), dt the increment */
ARP_IN int32_t blep(uint32_t t, uint32_t dt)
{
    if (t < dt) {                               /* just after the edge: t/dt in Q15 */
        int32_t x = (int32_t)(t / (dt >> 15 ? dt >> 15 : 1u));
        if (x > 32767) x = 32767;
        return (x * 2 - ((x * x) >> 15) - 32768) * 32;   /* 2x - x^2 - 1, Q20 */
    }
    if (t > ~dt) {                              /* just before it */
        int32_t x = (int32_t)((~t) / (dt >> 15 ? dt >> 15 : 1u));
        if (x > 32767) x = 32767;
        x = -x;
        return (((x * x) >> 15) + x * 2 + 32768) * 32;   /* x^2 + 2x + 1 */
    }
    return 0;
}
ARP_IN int32_t osc(uint32_t ph, uint32_t inc, uint32_t wave, uint32_t pw_q32)
{
    int32_t saw = (int32_t)(ph >> 11) - Q20;    /* 2 t - 1, Q20 */
    switch (wave) {
    case ARP_SAW: return saw - blep(ph, inc);
    case ARP_PULSE: {
        int32_t p = ph < pw_q32 ? Q20 : -Q20;
        return p + blep(ph, inc) - blep(ph + (0xFFFFFFFFu - pw_q32) + 1u, inc);
    }
    default: {                                  /* the triangle: folded, its corners soft enough at these pitches */
        int32_t t = saw < 0 ? -saw : saw;
        return (t << 1) - Q20;
    }
    }
}

void arp_init(arp_t *a)
{
    uint32_t i, *w = (uint32_t *)a;
    for (i = 0; i < sizeof *a / 4; i++) w[i] = 0;
    a->seed = 0x2545F491u;
    a->note_q16 = a->target_q16 = 48 * Q16;
    a->seq.tempo = 120;
    a->sps_q8 = (uint32_t)ARP_SR * 60u * 256u / (120u * 4u);
}

static void prep(arp_t *a)                      /* what the patch decides once */
{
    const arp_patch_t *p = &a->patch;
    env_set(&a->env1, p->a1, p->d1, p->s1, p->r1);
    env_set(&a->env2, p->a2, p->d2, p->s2, p->r2);
    a->glide_k = p->glide ? k_of_ms(ms_of_param(p->glide) * 2 / 3) : Q30;   /* 0 .. ~6 s to settle */
    a->slide_k = p->glide ? a->glide_k : k_of_ms(20 * Q16);                 /* a slide: 95 % in ~60 ms */
    /* the LFO: 0.05 .. 40 Hz: 0.05 2^(p / 10.36) */
    a->lfo_inc = inc_of_hz(MUL(exp2_q16((int32_t)p->lfo_rate * 6326), 3277, 16));   /* 0.05 Hz in Q16 = 3277 */
    a->k_res = (int32_t)p->reso * 2752;         /* 0 .. 4.2, Q16 */
}
void arp_set_patch(arp_t *a, const arp_patch_t *p)
{
    if (p != &a->patch) a->patch = *p;
    prep(a);
}
static uint32_t seq_len(const arp_t *a)        /* steps to the END, or sixteen */
{
    uint32_t n = 0;
    while (n < ARP_STEPS && a->seq.step[n].gate != ARP_END) n++;
    return n ? n : 1u;
}
void arp_set_seq(arp_t *a, const arp_seq_t *s)
{
    uint32_t k, bars = 0;
    if (s != &a->seq) a->seq = *s;
    if (a->seq.n_chain > ARP_CHAIN) a->seq.n_chain = ARP_CHAIN;
    for (k = 0; k < a->seq.n_chain; k++) bars += a->seq.bars[k] ? a->seq.bars[k] : 1u;
    a->chain_total = (bars ? bars : 4u) * seq_len(a);
    if (a->link >= a->seq.n_chain) { a->link = 0; a->bar = 0; }
    arp_set_tempo(a, a->seq.tempo);
}
void arp_set_tempo(arp_t *a, uint32_t bpm)
{
    bpm = bpm < 40u ? 40u : bpm > 240u ? 240u : bpm;
    a->seq.tempo = (uint16_t)bpm;
    a->sps_q8 = (uint32_t)ARP_SR * 60u * 256u / (bpm * 4u);
}
void arp_run(arp_t *a, uint32_t on) { a->req_running = on != 0; a->req_delay = 0; a->req_seq++; }
void arp_run_at(arp_t *a, uint32_t delay) { a->req_running = 1; a->req_delay = delay; a->req_seq++; }

ARP_IN void voice_on(arp_t *a, uint32_t note, uint32_t accent, uint32_t retrig, uint32_t slide)
{
    a->target_q16 = (int32_t)note * Q16;
    a->sliding = (uint8_t)(slide != 0);
    if (!a->patch.glide && !slide) a->note_q16 = a->target_q16;   /* (the portamento is on the CV: from wherever it was) */
    if (retrig || !a->gate) {
        a->env1.stage = 1;
        a->env2.stage = 1;
    }
    a->accent_q16 = accent ? (int32_t)a->patch.accent * 655 : 0;
    a->gate = 1;
}
ARP_IN void voice_off(arp_t *a)
{
    a->gate = 0;
    if (a->env1.stage && a->env1.stage != 4) a->env1.stage = 4;
    if (a->env2.stage && a->env2.stage != 4) a->env2.stage = 4;
}
void arp_note_on(arp_t *a, uint32_t note, uint32_t vel)
{
    uint32_t i;
    (void)vel;
    if (note > 127u) return;
    if (a->running) {                           /* the keys transpose the sequence: C3 is where it is written */
        a->transpose = (int8_t)((int32_t)note - 48);
        return;
    }
    for (i = 0; i < a->n_held; i++) if (a->held[i] == note) break;
    if (i == a->n_held) {
        if (a->n_held == 16u) { for (i = 1; i < 16u; i++) a->held[i - 1] = a->held[i]; a->n_held = 15; }
        a->held[a->n_held++] = (uint8_t)note;
    }
    voice_on(a, note, 0, 1, 0);
}
void arp_note_off(arp_t *a, uint32_t note)
{
    uint32_t i, j;
    for (i = 0; i < a->n_held; i++)
        if (a->held[i] == note) {
            for (j = i + 1; j < a->n_held; j++) a->held[j - 1] = a->held[j];
            a->n_held--;
            break;
        }
    if (a->running) return;
    if (a->n_held) voice_on(a, a->held[a->n_held - 1], 0, 0, 0);   /* back to the key still down, legato */
    else voice_off(a);
}
void arp_all_off(arp_t *a) { a->n_held = 0; voice_off(a); }
void arp_play(arp_t *a, uint32_t note, uint32_t flags)
{
    uint32_t joined = a->gate && (flags & (ARP_PLAY_LEGATO | ARP_PLAY_SLIDE));
    if (note > 127u) return;
    voice_on(a, note, flags & ARP_PLAY_ACCENT, !joined, joined && (flags & ARP_PLAY_SLIDE));
}
void arp_release(arp_t *a) { voice_off(a); }
int arp_position(const arp_t *a) { return a->running ? a->pos : -1; }
int arp_link(const arp_t *a) { return a->running && a->seq.n_chain ? a->link : -1; }

/* the sequencer's step: the gate on or off */
ARP_IN void step_start(arp_t *a)
{
    const arp_step_t *s, *nx;
    uint32_t len, tied;
    if (a->pos >= ARP_STEPS || a->seq.step[a->pos].gate == ARP_END) a->pos = 0;
    if (a->pos == 0 && a->steps) {              /* a new bar: the pattern's next link once its bars are played */
        a->bar++;
        if (a->seq.n_chain) {
            if (a->bar >= (a->seq.bars[a->link] ? a->seq.bars[a->link] : 1u)) {
                a->bar = 0;
                if (++a->link >= a->seq.n_chain) a->link = 0;
            }
        } else if (a->bar >= 4u) {
            a->bar = 0;
        }
        if (a->bar == 0 && a->link == 0) a->chain_step = 0;   /* (the pattern from its start: the sweep too) */
    }
    a->chain_tr = a->seq.n_chain ? a->seq.tr[a->link] : 0;
    s = &a->seq.step[a->pos];
    nx = &a->seq.step[a->pos + 1u < ARP_STEPS && a->seq.step[a->pos + 1u].gate != ARP_END ? a->pos + 1u : 0];
    tied = nx->gate == ARP_TIE;                 /* the next step holds this note on: the gate stays up into it */
    switch (s->gate) {
    case ARP_REST: voice_off(a); a->gate_left = 0; break;
    case ARP_SHORT: len = a->sps_q8 >> 10; goto on;             /* a quarter of the step */
    case ARP_LONG: len = (a->sps_q8 >> 8) * 3 / 4; goto on;
    case ARP_TIE:                               /* no new attack; held on, or let go at three quarters */
        voice_on(a, (uint32_t)((int32_t)s->note + a->transpose + a->chain_tr), s->accent, 0, 0);
        a->gate_left = tied ? -1 : (int32_t)((a->sps_q8 >> 8) * 3 / 4);
        break;
    default: len = 0; break;
    on:
        voice_on(a, (uint32_t)((int32_t)s->note + a->transpose + a->chain_tr), s->accent, 1, 0);
        a->gate_left = tied ? -1 : (int32_t)len;
        break;
    }
    a->steps++;
    a->chain_step++;
    a->pos++;
}

ARP_HOT void arp_render(arp_t *a, int32_t *out, uint32_t n)
{
    const arp_patch_t *p = &a->patch;
    uint32_t i, seq;
    int32_t cut_base, kbd, env_amt, lfo_vcf, sh_vcf, drive, lvl1, lvl2, lvln, lvlr, lvls, vol, pw1_base, lfo_pw, lfo_pitch, env_pitch;
    uint32_t pw1;
    /* the settings taken up between two blocks */
    seq = a->req_seq;
    if (seq != a->seq_done) {
        a->seq_done = seq;
        if (a->req_running && !a->running) {
            a->running = 1; a->pos = 0; a->counter_q8 = (int32_t)(a->req_delay << 8); a->gate_left = 0; a->transpose = 0; a->steps = 0;
            a->bar = a->link = 0; a->chain_step = 0; a->chain_tr = 0;
            a->n_held = 0;
        } else if (!a->req_running && a->running) {
            a->running = 0;
            voice_off(a);
        }
    }
    /* the sweep: a triangle over the pattern (its steps, and the way into this one), 0 .. 48 notes peak to peak */
    a->sweep_q16 = 0;
    if (a->running && a->seq.sweep && a->chain_total) {
        int32_t sps = (int32_t)(a->sps_q8 >> 8), c = a->counter_q8 >> 8;
        uint32_t frac = sps > 0 && c > 0 && c <= sps ? (uint32_t)Q16 - (uint32_t)((c << 16) / sps) : (uint32_t)Q16;
        uint32_t pos = (a->chain_step % a->chain_total << 16) + frac, ph = pos * 2u / a->chain_total;
        int32_t tri = ph < (uint32_t)Q16 ? (int32_t)ph : 2 * Q16 - (int32_t)ph;   /* 0 .. 1 .. 0 */
        a->sweep_q16 = MUL(tri - Q16 / 2, (int32_t)a->seq.sweep * (48 * Q16 / 100), 16);
    }
    /* this block's constants from the patch */
    cut_base = (20 * Q16) + (int32_t)p->cutoff * (110 * Q16 / 100) + a->sweep_q16;   /* notes 20 .. 130, Q16 */
    kbd = (int32_t)p->kbd * 655;                                            /* Q16 */
    env_amt = (int32_t)p->env_amt * (96 * Q16 / 100);                       /* 0 .. 96 notes */
    lfo_vcf = (int32_t)p->lfo_vcf * (48 * Q16 / 100);
    sh_vcf = (int32_t)p->sh_vcf * (48 * Q16 / 100);
    drive = Q16 / 4 + (int32_t)p->drive * 983;                              /* 0.25 .. 1.75, on half the mix */
    lvl1 = (int32_t)p->vco1_lvl * 655; lvl2 = (int32_t)p->vco2_lvl * 655;
    lvln = (int32_t)p->noise_lvl * 655; lvlr = (int32_t)p->ring_lvl * 655; lvls = (int32_t)p->sub_lvl * 655;
    vol = (int32_t)p->volume * 1310;                                        /* (2: the mix's half made up) */
    pw1_base = (int32_t)(p->vco1_pw < 5u ? 5u : p->vco1_pw > 95u ? 95u : p->vco1_pw) * 655;   /* Q16 */
    lfo_pw = (int32_t)p->lfo_pw * 295;                                      /* up to 0.45 */
    lfo_pitch = (int32_t)p->lfo_pitch * (12 * Q16 / 100);                   /* up to 12 semitones */
    env_pitch = (int32_t)p->env_pitch * (48 * Q16 / 100);                   /* ADSR 1 to VCO 2: up to 48 */

    for (i = 0; i < n; i++) {
        int32_t lfo, pitch1, pitch2, y, s, env1, env2, cut_note, mix, o1, o2, sub, ring, noise, fb, x, kres;
        uint32_t ph1, ph2;
        /* the sequencer */
        if (a->running) {
            if (a->counter_q8 <= 0) {
                step_start(a);
                a->counter_q8 += (int32_t)a->sps_q8;
            }
            a->counter_q8 -= 256;
            if (a->gate_left > 0 && --a->gate_left == 0) voice_off(a);
        }
        /* the LFO, and the sample-and-hold at its rate */
        {
            uint32_t ph = a->phl;
            a->phl += a->lfo_inc;
            if (a->phl < ph) {                  /* wrapped: a new sample held */
                a->seed = a->seed * 1664525u + 1013904223u;
                a->sh_out = (int32_t)(a->seed >> 11) - Q20;
            }
            switch (p->lfo_wave) {
            case ARP_LFO_TRI: { int32_t t = (int32_t)(ph >> 11) - Q20; a->lfo_out = ((t < 0 ? -t : t) << 1) - Q20; break; }
            case ARP_LFO_SQ: a->lfo_out = ph < 0x80000000u ? Q20 : -Q20; break;
            case ARP_LFO_SAW: a->lfo_out = Q20 - (int32_t)(ph >> 11); break;
            default: a->lfo_out = a->sh_out; break;
            }
            lfo = a->lfo_out;
        }
        /* the envelopes */
        env1 = env_run(&a->env1);
        env2 = env_run(&a->env2);
        /* the pitch: the glide, the octaves, the detune, the LFO; ADSR 1 on VCO 2 */
        a->note_q16 += MUL(a->target_q16 - a->note_q16, a->sliding ? a->slide_k : a->glide_k, 30);
        pitch1 = a->note_q16 + ((int32_t)p->vco1_oct - 2) * 12 * Q16 + MUL(lfo, lfo_pitch, 20);
        pitch2 = a->note_q16 + ((int32_t)p->vco2_oct - 2) * 12 * Q16 + ((int32_t)p->vco2_detune - 50) * 655 + MUL(lfo, lfo_pitch, 20)
               + MUL(env1 >> 14, env_pitch, 16);
        if ((i & 7u) == 0) {                    /* the increments every eight samples: the division is dear */
            a->inc1 = inc_of_hz(hz_of_note(pitch1));
            a->inc2 = inc_of_hz(hz_of_note(pitch2));
        }
        ph1 = a->ph1; ph2 = a->ph2;
        a->ph1 += a->inc1;
        if (p->sync && a->ph1 < ph1) a->ph2 = 0; else a->ph2 += a->inc2;
        a->ph3 += a->inc2 >> 1;
        pw1 = (uint32_t)(pw1_base + MUL(lfo, lfo_pw, 20)) << 16;
        o1 = osc(ph1, a->inc1, p->vco1_wave, pw1);
        o2 = osc(ph2, a->inc2, p->vco2_wave, 0x80000000u);
        sub = a->ph3 < 0x80000000u ? Q20 : -Q20;
        ring = MUL(o1, o2, 20);
        a->seed = a->seed * 1664525u + 1013904223u;
        noise = (int32_t)(a->seed >> 11) - Q20;
        mix = MUL(o1, lvl1, 16) + MUL(o2, lvl2, 16) + MUL(sub, lvls, 16) + MUL(ring, lvlr, 16) + MUL(noise, lvln, 16);
        /* the filter: its cutoff this sample in notes, then the ladder twice */
        cut_note = cut_base + MUL(a->note_q16 - 60 * Q16, kbd, 16) + MUL(env1 >> 14, env_amt + a->accent_q16 * 24, 16)
                 + MUL(lfo >> 4, lfo_vcf, 16) + MUL(a->sh_out >> 4, sh_vcf, 16);
        if ((i & 7u) == 0) {
            int32_t hz = hz_of_note(cut_note > 136 * Q16 ? 136 * Q16 : cut_note < 10 * Q16 ? 10 * Q16 : cut_note);
            /* g = 1 - e^(-2 pi fc / (2 fs)) = 1 - 2^(-fc 2 pi / (2 fs ln 2)) = 1 - 2^(-fc * 0.00010278); the exponent
             * in Q16 is fc * 6.736, and hz is Q16 itself: hz * 441413 / 2^32 */
            int32_t xq = MUL(hz, 441413, 32);
            a->g = Q30 - (exp2_q16(-xq) << 14);
            if (a->g > (int32_t)(Q30 * 0.92)) a->g = (int32_t)(Q30 * 0.92);
        }
        x = MUL(mix, drive, 17);
        kres = a->k_res;
        {
            int32_t g = a->g, h;
            for (h = 0; h < 2; h++) {
                fb = x - MUL(a->s4, kres, 16);
                fb = tanh_q20(fb);
                a->s1 += MUL(fb - a->s1, g, 30);
                a->s2 += MUL(a->s1 - a->s2, g, 30);
                a->s3 += MUL(a->s2 - a->s3, g, 30);
                a->s4 += MUL(a->s3 - a->s4, g, 30);
            }
        }
        y = a->s4;
        /* the amplifier, the accent, the volume; a DC block (the ladder's resonance leaves an offset); the
         * output stage rounds the peaks */
        s = MUL(MUL(y, env2 >> 10, 20), vol + (a->accent_q16 >> 1), 16);
        {
            int32_t b = s - a->dc_x + MUL(a->dc_y, 1071644672, 30);
            a->dc_x = s; a->dc_y = b; s = b;
        }
        s = tanh_q20(s) >> 5;
        out[2 * i] += s;
        out[2 * i + 1] += s;
    }
}
