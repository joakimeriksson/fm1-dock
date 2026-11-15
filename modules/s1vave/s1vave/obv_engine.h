/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Andrew GG — Roland S-1 parity engine */
/* S1Vave Voice Architecture & Sound Engine: 8-Voice Polyphonic Roland S-1 Emulation.
 * Audio ISR (44.1 kHz, 256 frames). Fixed-point integer DSP for JieLi AC791N RISC MCU. */

#ifndef OBV_ENGINE_H
#define OBV_ENGINE_H

#ifndef RING_PUBLISH
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#endif

#define NV 8u                                   /* 8-voice polyphony matching extended architecture */
#define NHELD 16u
#define ENV_ONE (1 << 24)
#define LEVEL_FLOOR 2048                        /* -78 dB: frees releasing voice */

enum { E_IDLE, E_ATT, E_DEC, E_SUS, E_REL };
typedef struct { uint32_t stage; int32_t level; } env_t;   /* level Q24 */

typedef struct {
    osc_t o_sqr;                                /* Square / Pulse / DRAW oscillator */
    osc_t o_saw;                                /* Sawtooth oscillator */
    osc_t o_sub;                                /* Sub-oscillator */
    pink_noise_t pn;                            /* Pink noise state */
    uint32_t noise;                             /* PRNG seed per voice */
    env_t fe, ae;                               /* Filter and amp envelopes */
    svf_state_t f1, f2;                         /* 4-pole cascaded SVF stages */
    svf_coef_t c1, c2;
    uint32_t inc, pw;                           /* Phase step & pulse width */
    int32_t amp0, amp1;                         /* Q15 amplitude slew */
    int32_t pitch16, target16;                  /* Q16 semitones: glide */
    int32_t det8;                               /* Unison / chord offset in Q8 */
    int32_t gl, gr;                             /* Stereo pan gains Q12 */
    int32_t drift;                              /* Analog vintage drift */
    int32_t noise_lp;                           /* White noise lowpass state */
    uint32_t lfo_blk;                           /* Blocks since note-on */
    int32_t lfo_fade;                           /* Q15 */
    uint32_t seq;                               /* Allocation age */
    uint8_t note, vel, gate;
    uint8_t part;                               /* 0: Sound A, 1: Track B sound */
    uint8_t chord_idx;                          /* 0=Voice 1, 1=Voice 2, 2=Voice 3, 3=Voice 4 */
} voice_t;

/* Two sounds: Sound A (main) and Track B's sound */
static patch_t live, live_b, live_bq;
volatile uint32_t engine_ready;                 /* set after engine_init(); ISR outputs silence until then */
static volatile uint32_t ctl_bq_swap;
static uint32_t b_own;
static voice_t vc[NV];
static uint32_t vseq, rr;

/* Sequencer accessor forward declarations */
static uint32_t seq_b_own(void);
static uint32_t seq_b_dry(void);
static void seq_tick(void);
static inline const int16_t *seq_draw_table(void);
static inline const uint16_t *seq_chop_masks(void);

/* Note Event FIFO */
typedef struct { uint8_t type, note, vel, part; } nev_t;
#define NQ 64u
static nev_t nq[NQ];
static volatile uint32_t nq_w, nq_r;

static void note_event_p(uint32_t part, uint32_t on, uint32_t note, uint32_t vel)
{
    if (nq_w - nq_r >= NQ) return;
    nq[nq_w % NQ].part = (uint8_t)part;
    nq[nq_w % NQ].type = (uint8_t)on;
    nq[nq_w % NQ].note = (uint8_t)note;
    nq[nq_w % NQ].vel = (uint8_t)vel;
    RING_PUBLISH();
    nq_w++;
}
static void note_event(uint32_t on, uint32_t note, uint32_t vel) { note_event_p(0, on, note, vel); }

/* Realtime Controllers */
static volatile int32_t ctl_bend;               /* -8192..8191 */
static volatile uint32_t ctl_sustain, ctl_hold, ctl_cc7 = 127;
static volatile int32_t ctl_vib;
static volatile int32_t ctl_tune8;              /* Master tune, Q8 */

/* ADV stereo (P_SPREAD / P_DETUNE): applied at note-on only - voice_start already carries
 * pan and det8, the per-sample path is untouched. Defaults 0 = the stock Roland S-1. */
#define SPREAD64()  ((int32_t)pv[P_SPREAD] * 64 / 100)     /* 0..64 pan width */
#define DETUNE8()   ((int32_t)pv[P_DETUNE] * 256 / 100)    /* cents -> Q8 */
static volatile uint32_t ctl_tempo = 120, ctl_ext_quarter;
static volatile uint32_t ctl_arp_restart, ctl_panic;
static volatile uint32_t ctl_arp_on;
static volatile uint32_t ctl_arp_type;
static volatile uint32_t ctl_arp_rate = 2;      /* 1/16 */
static volatile uint32_t ctl_note_pri;          /* 0=Last, 1=Low */

/* UI Telemetry (ISR -> Main Loop) */
static volatile int32_t ui_vlevel[NV];
static volatile uint8_t ui_vnote[NV];
static volatile uint32_t ui_render_us, ui_arp_step;
#define SCOPE_N 240u
static int16_t scope[SCOPE_N];
static volatile uint32_t scope_state, scope_stride = 4;

/* Held Notes List */
static struct { uint8_t note, vel, phys; } held[2][NHELD];
static uint32_t nheld[2];
static uint32_t rnd_state = 0x9E3779B9u;

/* Synthesis Part State */
typedef struct {
    const patch_t *p;
    uint32_t lfo_ph, lfo_rnd_a, lfo_rnd_b;
    int32_t lfo_val, lfo_noi_sm;
    int32_t last_pitch16;
    int32_t fenv_amt, keytrk, lfo_pd, lfo_fd, wheel_q, bend8, flt_bend8;
    int32_t lvl_sqr, lvl_saw, lvl_sub, lvl_noi, reso_k1, reso_k2, vint, sus_f, sus_a, gain_q12;
    int32_t cut_sm, reso_sm;
    uint32_t pw_base, pwm_src, sub_type, noise_mode, draw_sw, draw_mult_q8;
    uint32_t chop_ovtn, chop_comb_q8;
    int32_t g_chop_masked;
    uint32_t poly_mode, porta_mode, glide_k;
    int32_t fine_tune8, range_semi8, transp_semi8;
} part_t;

static int32_t lfo_shape(const part_t *g, uint32_t ph, uint32_t shape);
static part_t pt[2] = {{&live, 0, 0, 0, 0, 60 << 16}, {&live, 0, 0, 0, 0, 60 << 16}};

static void parts_point(void)
{
    uint32_t was = b_own;
    b_own = seq_b_own();
    pt[1].p = b_own ? &live_b : &live;
    if (b_own && !was) {
        pt[1].cut_sm = (int32_t)CUTOFF_TAPER_WARM[live_b.v[P_CUTOFF] & 127u];
        pt[1].reso_sm = live_b.v[P_RESO] << 8;
    }
}

static void held_remove(uint32_t p, uint32_t note)
{
    uint32_t i, j;
    for (i = j = 0; i < nheld[p]; i++)
        if (held[p][i].note != note) held[p][j++] = held[p][i];
    nheld[p] = j;
}

/* Vintage card offsets for 8 voices (Roland ACB voice circuit modeling) */
static const int16_t VINT_TUNE8[NV] = {0, 0, 0, 0, 0, 0, 0, 0};
static const int16_t VINT_CUT8[NV]  = {0, 0, 0, 0, 0, 0, 0, 0};
static const int16_t VINT_RESO8[NV] = {0, 0, 0, 0, 0, 0, 0, 0};
static const int8_t  VINT_ENV[NV]   = {0, 0, 0, 0, 0, 0, 0, 0};
static const int32_t VINT_PW8[NV]   = {0, 0, 0, 0, 0, 0, 0, 0};

/* Vintage warmth card offsets (Oct 19 Midnight Tone) */
static const int16_t VINT_TUNE8_WARM[NV] = {-12, 10, -8, 12, -10, 8, -6, 10};
static const int16_t VINT_RESO8_WARM[NV] = {0, 0, 0, 0, 0, 0, 0, 0};
static const int8_t  VINT_ENV_WARM[NV]   = {4, -3, 5, -4, 3, -2, 4, -3};
static const int32_t VINT_PW8_WARM[NV]   = {-0x01800000, 0x01200000, -0x00D00000, 0x01A00000, -0x01000000, 0x01400000, -0x00A00000, 0x01600000};
static const int8_t  RANGE_SEMITONES[6] = {-36, -24, -12, 0, 12, 24};

static void voice_pan(voice_t *v, int32_t pan)
{
    uint32_t pos = (uint32_t)(clampi(pan, -64, 64) + 64);
    v->gl = PAN_Q12[128u - pos];
    v->gr = PAN_Q12[pos];
}

static void voice_start(voice_t *v, uint32_t part, uint32_t note, uint32_t vel, int32_t det8, int32_t pan, uint8_t c_idx)
{
    const uint8_t *pv = pt[part].p->v;
    v->part = (uint8_t)part;
    v->note = (uint8_t)note;
    v->vel = (uint8_t)vel;
    v->gate = 1;
    v->seq = ++vseq;
    v->det8 = det8;
    v->chord_idx = c_idx;
    v->target16 = (int32_t)note << 16;
    v->pitch16 = (pv[P_PORTA_MODE] == PORTA_ON && pv[P_PORTA_TIME]) ? pt[part].last_pitch16 : v->target16;
    v->fe.stage = E_ATT;
    v->ae.stage = E_ATT;
    v->lfo_blk = 0;
    v->lfo_fade = 32767;
    /* Free-running voice phase jitter while keeping saw, pulse, and sub strictly phase-locked */
    uint32_t jitter = xorshift(&v->noise) & 0x1FFFFFFFu;
    v->o_sqr.ph += jitter;
    v->o_saw.ph = v->o_sqr.ph;
    v->o_sub.ph = (pv[P_SUB_TYPE] % 3u == SUB_1OC) ? (v->o_sqr.ph >> 1) : (v->o_sqr.ph >> 2);
    if (pv[P_RESO] > 110) {
        v->f2.ic1 += (int32_t)pv[P_RESO] * 64;
    }
    voice_pan(v, pan);
}

/* Voice allocation for 4 polyphonic voices */
static uint32_t voice_alloc(uint32_t part)
{
    uint32_t i, v, pick = 0, best = 0xFFFFFFFFu;
    (void)part;
    /* 1. Prefer completely idle voices */
    for (i = 0; i < NV; i++) {
        v = (rr + i) % NV;
        if (vc[v].ae.stage == E_IDLE) { rr = (v + 1u) % NV; return v; }
    }
    /* 2. Prefer released voices */
    for (v = 0; v < NV; v++) {
        if (!vc[v].gate && vc[v].seq < best) { best = vc[v].seq; pick = v; }
    }
    /* 3. Steal oldest active voice */
    if (best == 0xFFFFFFFFu) {
        for (v = 0; v < NV; v++) {
            if (vc[v].seq < best) { best = vc[v].seq; pick = v; }
        }
    }
    rr = (pick + 1u) % NV;
    return pick;
}

static void voice_trigger(uint32_t part, uint32_t note, uint32_t vel)
{
    const uint8_t *pv = pt[part].p->v;
    uint32_t mode = pv[P_POLY_MODE] % NPOLY_MODE;

    if (mode == POLY_POLY) {
        uint32_t v = voice_alloc(part);
        voice_start(&vc[v], part, note, vel,
                        (vseq & 1) ? DETUNE8() : -DETUNE8(),
                        (vseq & 1) ? SPREAD64() : -SPREAD64(), 0);
    } else if (mode == POLY_MONO) {
        voice_t *v = &vc[0];
        int legato = v->gate;
        if (legato) {
            v->note = (uint8_t)note;
            v->vel = (uint8_t)vel;
            v->target16 = (int32_t)note << 16;
            if (pv[P_PORTA_MODE] == PORTA_OFF) v->pitch16 = v->target16;
            if (pv[P_ENV_TRG] == TRIG_GATE_TRIG) {
                v->fe.stage = E_ATT;
                v->ae.stage = E_ATT;
            }
        } else {
            voice_start(v, part, note, vel, 0, 0, 0);
        }
    } else if (mode == POLY_UNISON) {
        /* 4 voices stacked with symmetric detuning (stock Roland S-1: centered, detune 22 Q8) */
        int32_t det = 22 + DETUNE8();
        int32_t det_offsets[4] = {-det, -det / 3, det / 3, det};
        int32_t sp = SPREAD64();
        int32_t pan_offsets[4] = {-sp, -sp / 3, sp / 3, sp};
        int legato = vc[0].gate;
        if (legato) {
            for (uint32_t i = 0; i < 4; i++) {
                vc[i].note = (uint8_t)note;
                vc[i].vel = (uint8_t)vel;
                vc[i].target16 = (int32_t)note << 16;
                if (pv[P_PORTA_MODE] == PORTA_OFF) vc[i].pitch16 = vc[i].target16;
                if (pv[P_ENV_TRG] == TRIG_GATE_TRIG) {
                    vc[i].fe.stage = E_ATT;
                    vc[i].ae.stage = E_ATT;
                }
            }
        } else {
            uint32_t base_ph = vc[0].o_sqr.ph;
            for (uint32_t i = 0; i < 4; i++) {
                voice_start(&vc[i], part, note, vel, det_offsets[i], pan_offsets[i], (uint8_t)i);
                uint32_t u_ph = base_ph + i * 0x40000000u;
                vc[i].o_sqr.ph = u_ph;
                vc[i].o_saw.ph = u_ph;
                uint32_t u_sub = (pv[P_SUB_TYPE] % 3u == SUB_1OC) ? (u_ph >> 1) : (u_ph >> 2);
                vc[i].o_sub.ph = u_sub + i * 0x40000000u;
            }
            for (uint32_t i = 4; i < NV; i++) {
                vc[i].gate = 0;
            }
        }
    } else if (mode == POLY_CHORD) {
        /* Chord mode: 4 voices (Voice 1: base note, Voices 2..4: user shifted notes, centered) */
        int legato = vc[0].gate;
        int32_t shifts[4] = {
            0,
            pv[P_CHORD_SW2] ? (int32_t)pv[P_CHORD_KS2] - 12 : 0,
            pv[P_CHORD_SW3] ? (int32_t)pv[P_CHORD_KS3] - 12 : 0,
            pv[P_CHORD_SW4] ? (int32_t)pv[P_CHORD_KS4] - 12 : 0
        };
        if (legato) {
            for (uint32_t i = 0; i < 4; i++) {
                int active = (i == 0) || (i == 1 && pv[P_CHORD_SW2]) || (i == 2 && pv[P_CHORD_SW3]) || (i == 3 && pv[P_CHORD_SW4]);
                if (active) {
                    uint32_t cn = (uint32_t)clampi((int32_t)note + shifts[i], 0, 127);
                    vc[i].note = (uint8_t)cn;
                    vc[i].vel = (uint8_t)vel;
                    vc[i].target16 = (int32_t)cn << 16;
                    if (pv[P_PORTA_MODE] == PORTA_OFF) vc[i].pitch16 = vc[i].target16;
                    if (pv[P_ENV_TRG] == TRIG_GATE_TRIG) {
                        vc[i].fe.stage = E_ATT;
                        vc[i].ae.stage = E_ATT;
                    }
                } else {
                    vc[i].gate = 0;
                }
            }
        } else {
            int32_t sp = SPREAD64();
            int32_t pan_offsets[4] = {-sp, -sp / 3, sp / 3, sp};
            voice_start(&vc[0], part, note, vel, 0, pan_offsets[0], 0);
            for (uint32_t i = 1; i < 4; i++) {
                int active = (i == 1 && pv[P_CHORD_SW2]) || (i == 2 && pv[P_CHORD_SW3]) || (i == 3 && pv[P_CHORD_SW4]);
                if (active) {
                    uint32_t cn = (uint32_t)clampi((int32_t)note + shifts[i], 0, 127);
                    voice_start(&vc[i], part, cn, vel, 0, pan_offsets[i], (uint8_t)i);
                } else {
                    vc[i].gate = 0;
                }
            }
            for (uint32_t i = 4; i < NV; i++) {
                vc[i].gate = 0;
            }
        }
    }
    pt[part].last_pitch16 = (int32_t)note << 16;
    if (pv[P_LFO_TRIG]) {
        pt[part].lfo_ph = 0;
        pt[part].lfo_val = lfo_shape(&pt[part], 0, pv[P_LFO_WAVE]);
    }
}

static void voice_release(uint32_t part, uint32_t note, uint32_t from_arp)
{
    const uint8_t *pv = pt[part].p->v;
    uint32_t v, mode = pv[P_POLY_MODE];

    if ((mode == POLY_MONO || mode == POLY_UNISON || mode == POLY_CHORD) && !from_arp) {
        if (nheld[part]) {
            /* Fallback to lowest held note (Lo-pri) or last note (Last-pri) */
            uint8_t fallback = held[part][0].note;
            if (ctl_note_pri == 1) {
                for (uint32_t i = 1; i < nheld[part]; i++) {
                    if (held[part][i].note < fallback) fallback = held[part][i].note;
                }
            } else {
                fallback = held[part][nheld[part] - 1u].note;
            }
            if (mode == POLY_MONO) {
                vc[0].note = fallback;
                vc[0].target16 = (int32_t)fallback << 16;
                if (pv[P_PORTA_MODE] == PORTA_OFF) vc[0].pitch16 = vc[0].target16;
            } else if (mode == POLY_UNISON) {
                for (uint32_t i = 0; i < 4; i++) {
                    vc[i].note = fallback;
                    vc[i].target16 = (int32_t)fallback << 16;
                    if (pv[P_PORTA_MODE] == PORTA_OFF) vc[i].pitch16 = vc[i].target16;
                }
            } else if (mode == POLY_CHORD) {
                int32_t shifts[4] = {
                    0,
                    pv[P_CHORD_SW2] ? (int32_t)pv[P_CHORD_KS2] - 12 : 0,
                    pv[P_CHORD_SW3] ? (int32_t)pv[P_CHORD_KS3] - 12 : 0,
                    pv[P_CHORD_SW4] ? (int32_t)pv[P_CHORD_KS4] - 12 : 0
                };
                for (uint32_t i = 0; i < 4; i++) {
                    int active = (i == 0) || (i == 1 && pv[P_CHORD_SW2]) || (i == 2 && pv[P_CHORD_SW3]) || (i == 3 && pv[P_CHORD_SW4]);
                    if (active) {
                        uint32_t cn = (uint32_t)clampi((int32_t)fallback + shifts[i], 0, 127);
                        vc[i].note = (uint8_t)cn;
                        vc[i].target16 = (int32_t)cn << 16;
                        if (pv[P_PORTA_MODE] == PORTA_OFF) vc[i].pitch16 = vc[i].target16;
                    }
                }
            }
            pt[part].last_pitch16 = (int32_t)fallback << 16;
        } else {
            if (mode == POLY_MONO) {
                vc[0].gate = 0;
            } else {
                for (v = 0; v < NV; v++) vc[v].gate = 0;
            }
        }
        return;
    }
    for (v = 0; v < NV; v++) {
        if (vc[v].gate && vc[v].note == note && vc[v].part == part) vc[v].gate = 0;
    }
}

static void voices_release_all(void)
{
    for (uint32_t v = 0; v < NV; v++) vc[v].gate = 0;
}

static void voices_release_part(uint32_t part)
{
    for (uint32_t v = 0; v < NV; v++)
        if (vc[v].part == part) vc[v].gate = 0;
}

/* Metronome Click */
static volatile uint32_t ctl_click, ctl_count, ui_seq_count;
static int32_t click_amp;
static uint32_t click_ph, click_inc;
static void click_start(uint32_t accent)
{
    click_amp = accent ? 9000 : 6000;
    click_ph = 0;
    click_inc = accent ? 233739600u : 155826400u;
}

/* Arpeggiator (Roland S-1 Arpeggio engine) */
typedef struct { uint32_t cnt, period, gate_at, pos, dir, sounding, note, was_on; } arp_t;
static arp_t arp[2];

#define AQ 32u
static uint8_t arp_q[AQ][3];
static volatile uint32_t arp_qw, arp_qr;
static void arp_post(uint32_t p, uint32_t on, uint32_t note)
{
    if (arp_qw - arp_qr >= AQ) return;
    arp_q[arp_qw % AQ][0] = (uint8_t)on;
    arp_q[arp_qw % AQ][1] = (uint8_t)note;
    arp_q[arp_qw % AQ][2] = (uint8_t)p;
    RING_PUBLISH();
    arp_qw++;
}

static void arp_stop_note(uint32_t p)
{
    arp_t *a = &arp[p];
    if (a->sounding) { voice_release(p, a->note, 1); a->sounding = 0; arp_post(p, 0, a->note); }
}

static void arp_step(uint32_t p)
{
    arp_t *a = &arp[p];
    uint8_t sorted[NHELD];
    uint32_t i, j, n = nheld[p];
    if (!n) return;
    for (i = 0; i < n; i++) sorted[i] = held[p][i].note;
    for (i = 1; i < n; i++) {
        uint8_t t = sorted[i];
        for (j = i; j && sorted[j - 1u] > t; j--) sorted[j] = sorted[j - 1u];
        sorted[j] = t;
    }
    uint32_t type = ctl_arp_type % NARP_TYPE;
    uint32_t oct = (type == ARP_UP_2OCT || type == ARP_DOWN_2OCT || type == ARP_UP_DOWN_2OCT || type == ARP_RANDOM_2OCT) ? 2u : 1u;
    uint32_t total = n * oct;
    uint32_t k;

    if (type == ARP_RANDOM || type == ARP_RANDOM_2OCT) {
        rnd_state = rnd_state * 1664525u + 1013904223u;
        k = (rnd_state >> 16) % total;
    } else if (type == ARP_UP || type == ARP_UP_2OCT) {
        k = a->pos % total;
        a->pos++;
    } else if (type == ARP_DOWN || type == ARP_DOWN_2OCT) {
        k = (total - 1u) - (a->pos % total);
        a->pos++;
    } else { /* ARP_UP_DOWN or ARP_UP_DOWN_2OCT */
        uint32_t cycle = total > 1u ? (total * 2u - 2u) : 1u;
        uint32_t step = a->pos % cycle;
        if (step < total) k = step;
        else k = total - 2u - (step - total);
        a->pos++;
    }

    uint32_t idx = k % n;
    uint32_t oct_shift = k / n;
    a->note = (uint32_t)clampi((int32_t)sorted[idx] + 12 * (int32_t)oct_shift, 0, 127);
    for (i = 0; i < n; i++)
        if (held[p][i].note == sorted[idx]) break;
    voice_trigger(p, a->note, i < n ? held[p][i].vel : 100u);
    arp_post(p, 1, a->note);
    a->sounding = 1;
    ui_arp_step++;
}

static void arp_tick(uint32_t p)
{
    if (!ctl_arp_on) return;
    arp_t *a = &arp[p];
    uint32_t q = ctl_ext_quarter ? ctl_ext_quarter : 60u * FS / clampi(ctl_tempo, 40, 240);
    if (!nheld[p]) { arp_stop_note(p); a->pos = 0; a->cnt = 0xFFFFFFFFu; return; }
    static const uint8_t ARP_DIV[7] = {1, 2, 4, 8, 3, 6, 12};
    uint32_t div = ARP_DIV[ctl_arp_rate < 7u ? ctl_arp_rate : 2u];
    a->period = q / div;
    if (a->period < CTL) a->period = CTL;
    a->gate_at = (a->period * 64u) >> 7;
    if (a->cnt == 0xFFFFFFFFu || a->cnt + CTL >= a->period) {
        a->cnt = a->cnt == 0xFFFFFFFFu ? 0 : a->cnt + CTL - a->period;
        arp_stop_note(p);
        arp_step(p);
    } else {
        a->cnt += CTL;
        if (a->sounding && a->cnt >= a->gate_at) arp_stop_note(p);
    }
}

/* Event processing */
static uint32_t sus_was;
static inline uint32_t part_sus(uint32_t p) { return ctl_sustain || (ctl_hold >> p & 1u); }

static void ev_note_on(uint32_t p, uint32_t note, uint32_t vel)
{
    if (part_sus(p) && nheld[p] > 0) {
        uint32_t any_phys = 0;
        for (uint32_t i = 0; i < nheld[p]; i++) {
            if (held[p][i].phys) { any_phys = 1; break; }
        }
        if (!any_phys) {
            nheld[p] = 0;
            if (ctl_arp_on) {
                arp_stop_note(p);
                ctl_arp_restart = 1;
            }
        }
    }
    held_remove(p, note);
    if (nheld[p] == NHELD) {
        for (uint32_t i = 1; i < NHELD; i++) held[p][i - 1u] = held[p][i];
        nheld[p]--;
    }
    held[p][nheld[p]].note = (uint8_t)note;
    held[p][nheld[p]].vel = (uint8_t)vel;
    held[p][nheld[p]].phys = 1;
    nheld[p]++;
    if (!ctl_arp_on) {
        uint32_t pn = note;
        if (ctl_note_pri == 1) { /* Low note priority */
            for (uint32_t i = 0; i < nheld[p]; i++)
                if (held[p][i].note < pn) pn = held[p][i].note;
        }
        voice_trigger(p, pn, vel);
    }
}

static void ev_note_off(uint32_t p, uint32_t note)
{
    for (uint32_t i = 0; i < nheld[p]; i++)
        if (held[p][i].note == note) held[p][i].phys = 0;
    if (part_sus(p)) return;
    held_remove(p, note);
    if (!ctl_arp_on) voice_release(p, note, 0);
}

static void ev_sustain_up(uint32_t p)
{
    uint8_t gone[NHELD];
    uint32_t ng = 0, j = 0;
    for (uint32_t i = 0; i < nheld[p]; i++) {
        if (held[p][i].phys) held[p][j++] = held[p][i];
        else gone[ng++] = held[p][i].note;
    }
    nheld[p] = j;
    for (uint32_t i = 0; i < ng; i++) voice_release(p, gone[i], 0);
}

static void parts_off(uint32_t p)
{
    nheld[p] = 0;
    arp_stop_note(p);
    voices_release_part(p);
}

static void engine_events(void)
{
    uint32_t sus = part_sus(0) | (part_sus(1) << 1);
    if (ctl_panic) { ctl_panic = 0; parts_off(0); parts_off(1); voices_release_all(); }
    if (sus_was & ~sus & 1u) ev_sustain_up(0);
    if (sus_was & ~sus & 2u) ev_sustain_up(1);
    sus_was = sus;
    while (nq_r != nq_w) {
        nev_t e = nq[nq_r % NQ];
        uint32_t p = e.part && b_own;
        nq_r++;
        if (e.type) ev_note_on(p, e.note, e.vel);
        else ev_note_off(p, e.note);
    }
}

/* ADSR Envelope Progression */
static inline void env_tick(env_t *e, uint32_t a, uint32_t d, int32_t sus, uint32_t r, int32_t gate, int32_t vint)
{
    int32_t k, target;
    if (gate && (e->stage == E_IDLE || e->stage >= E_REL)) e->stage = E_ATT;
    if (!gate && e->stage && e->stage < E_REL) e->stage = E_REL;
    switch (e->stage) {
    case E_ATT:
        k = (int32_t)ENV_K_ATT[a & 127u];
        k += (int32_t)(((int64_t)k * vint) >> 7);
        target = ENV_ONE + (ENV_ONE * 3 >> 3);
        e->level += (int32_t)(((int64_t)(target - e->level) * k) >> 24);
        if (e->level >= ENV_ONE) { e->level = ENV_ONE; e->stage = E_DEC; }
        break;
    case E_DEC:
        k = (int32_t)ENV_K_DEC[d & 127u];
        k += (int32_t)(((int64_t)k * vint) >> 7);
        e->level += (int32_t)(((int64_t)(sus - e->level) * k) >> 24) - 16;
        if (e->level <= sus + 64) { e->level = sus; e->stage = E_SUS; }
        break;
    case E_SUS:
        e->level = sus;
        break;
    case E_REL:
        k = (int32_t)ENV_K_REL[r & 127u];
        k += (int32_t)(((int64_t)k * vint) >> 7);
        e->level += (int32_t)(((int64_t)(0 - e->level) * k) >> 24) - 32;
        if (e->level < LEVEL_FLOOR) { e->level = 0; e->stage = E_IDLE; }
        break;
    default:
        e->level = 0;
    }
}

/* LFO Waveform Generator */
static int32_t lfo_shape(const part_t *g, uint32_t ph, uint32_t shape)
{
    switch (shape) {
    case LFO_TRI: {
        int32_t p = (int32_t)((ph + 0x40000000u) >> 15);
        return p < 65536 ? p - 32768 : 98303 - p;
    }
    case LFO_SAW:     return (int32_t)(ph >> 16) - 32768;
    case LFO_INV_SAW: return 32767 - (int32_t)(ph >> 16);
    case LFO_SQR:     return ph < 0x80000000u ? 32767 : -32767;
    case LFO_RND:     return (int32_t)(g->lfo_rnd_b >> 16) - 32768;
    default:          /* LFO_NOISE: static high-speed continuous noise generator matching Roland S-1 */
        return (int32_t)(xorshift(&rnd_state) >> 16) - 32768;
    }
}

static void lfo_tick(part_t *g)
{
    uint32_t inc;
    if (g->p->v[P_LFO_SYNC]) {
        uint32_t slot = g->p->v[P_LFO_RATE];
        if (slot >= 31u) slot = (slot * 30u) / 127u;
        uint32_t num = LFO_SYNC_RATIO[slot][0];
        uint32_t den = LFO_SYNC_RATIO[slot][1];
        inc = (uint32_t)((ctl_tempo * (25971u * den)) / num);
    } else {
        uint32_t rate = g->p->v[P_LFO_RATE] & 127u;
        inc = LFO_INC[rate];
        if (g->p->v[P_LFO_MODE]) inc = (inc * 11u) >> 2; /* Fast mode extends to ~100 Hz (matching Roland S-1) */
    }
    uint32_t old = g->lfo_ph;
    g->lfo_ph += inc;
    if (g->lfo_ph < old) {
        g->lfo_rnd_a = g->lfo_rnd_b;
        g->lfo_rnd_b = xorshift(&rnd_state);
        /* Roland S-1 ENV TRG = LFO: retrigger attack phase at each LFO cycle */
        if (g->p->v[P_ENV_TRG] == TRIG_LFO) {
            uint32_t part_idx = (g == &pt[1]) ? 1u : 0u;
            for (uint32_t v = 0; v < NV; v++) {
                if (vc[v].gate && vc[v].part == part_idx) {
                    vc[v].fe.stage = E_ATT;
                    vc[v].ae.stage = E_ATT;
                }
            }
        }
    }

    g->lfo_val = lfo_shape(g, g->lfo_ph, g->p->v[P_LFO_WAVE]);
    /* Smooth noise LFO for PWM modulation (analog slew-rate limiting on S-1) */
    if (g->p->v[P_LFO_WAVE] == LFO_NOISE) {
        g->lfo_noi_sm += (g->lfo_val - g->lfo_noi_sm) >> 4;
    } else {
        g->lfo_noi_sm = g->lfo_val;
    }
}

static int32_t vol_q12;

/* Derive parameter values once per 16-frame block */
static void block_globals(part_t *g)
{
    const uint8_t *pv = g->p->v;
    int32_t d;

    g->range_semi8 = (int32_t)RANGE_SEMITONES[pv[P_RANGE] % 6u] << 8;
    g->fine_tune8 = ((int32_t)pv[P_FINE_TUNE] - 64) * 48;
    g->transp_semi8 = ((int32_t)pv[P_TRANSPOSE] - 64) << 8;

    g->lvl_sqr = pv[P_PULSE_LVL];
    g->lvl_saw = pv[P_SAW_LVL];
    g->lvl_sub = ((uint32_t)pv[P_SUB_LVL] * 7u) >> 2; /* 1.75x low-end punch */
    g->lvl_noi = (uint32_t)pv[P_NOISE_LVL] * 6u;      /* 6.0x noise presence (+3.5 dB boost to match Roland S-1) */

    g->sub_type = pv[P_SUB_TYPE] % 3u;
    g->noise_mode = pv[P_NOISE_MODE] % 2u;

    g->pwm_src = pv[P_PWM_SRC];
    g->pw_base = ((uint32_t)pv[P_PWM_DEPTH] << 25) + (1u << 24);

    g->draw_sw = pv[P_DRAW_SW];
    g->draw_mult_q8 = ((uint32_t)pv[P_DRAW_MULT] + 1u) << 8;

    g->chop_ovtn = pv[P_CHOP_OVTN];
    g->chop_comb_q8 = ((uint32_t)pv[P_CHOP_COMB] + 1u) << 8;
    g->g_chop_masked = chop_calc_masked_gain(g->chop_ovtn);

    /* When filter cutoff is in the bottom 25% range (cut_val < 32), the filter attenuates
     * the effective envelope depth (scaling from 75 at cut_val=0 to simulate Roland S-1 closed filter behavior).
     * Above 25% (cut_val >= 32), envelope amount is completely unreduced, matching the original curve. */
    int32_t fa = (int32_t)pv[P_FENV_AMT];
    uint32_t cut_val = pv[P_CUTOFF] & 127u;
    if (cut_val < 32u) {
        int32_t scale = 75 + (int32_t)((52u * cut_val) >> 5);
        fa = (fa * scale) / 127;
    }
    if (fa > 80) fa = 80 + ((fa - 80) * 6) / 8;
    g->fenv_amt = fa;
    /* Key tracking: scaled to Q8 semitones (127 -> 256 = 100% 1V/oct chromatic follow) */
    g->keytrk = ((int32_t)pv[P_KEYTRK] << 8) / 127;

    uint32_t pd_idx = pv[P_LFO_PITCH] & 127u;
    g->lfo_pd = (int32_t)LFO_PITCH_DEPTH[pd_idx];
    d = pv[P_FLFO_AMT];  g->lfo_fd = (d * 38) + ((d * d * 70) >> 7);
    g->wheel_q = pv[P_MOD_DEPTH];

    g->bend8 = (ctl_bend * pv[P_OSC_BEND] * 256) >> 13;
    g->flt_bend8 = (ctl_bend * pv[P_FLT_BEND] * 256) >> 13;

    int32_t target_cut = (int32_t)CUTOFF_TAPER_WARM[pv[P_CUTOFF] & 127u];
    g->cut_sm += (target_cut - g->cut_sm) >> 6;
    /* Resonance: 2x calibration matching S-1 hardware resonance strength */
    int32_t reso_scaled = (int32_t)pv[P_RESO] * 2;
    if (reso_scaled > 127) reso_scaled = 127;
    g->reso_sm += ((reso_scaled << 8) - g->reso_sm) >> 6;
    g->reso_k1 = 8192 - g->reso_sm * 8400 / (127 * 256);
    g->reso_k2 = 8192;

    int32_t sus_raw = pv[P_SUSTAIN];
    /* Roland analog slider convex taper calibrated to S-1 (e.g. 25 -> 43) */
    int32_t sus_adj = sus_raw + ((sus_raw * (127 - sus_raw) * 29) >> 12);
    g->sus_f = sus_adj * (ENV_ONE / 127);
    g->sus_a = sus_adj * (ENV_ONE / 127);

    d = pv[P_VOLUME];
    /* Roland S-1 Patch Volume: quadratic audio taper matching Roland hardware */
    g->gain_q12 = (int32_t)((d * d) >> 2);

    g->poly_mode = pv[P_POLY_MODE] % NPOLY_MODE;
    g->porta_mode = pv[P_PORTA_MODE] % 3u;
    g->glide_k = GLIDE_K[pv[P_PORTA_TIME] & 127u];
}

static inline uint32_t pw_mod(uint32_t base, int32_t mod)
{
    int64_t p = (int64_t)base + ((int64_t)mod << 8);
    if (p < (1 << 26)) p = 1 << 26;
    if (p > (int64_t)0xFFFFFFFFu - (1 << 26)) p = (int64_t)0xFFFFFFFFu - (1 << 26);
    return (uint32_t)p;
}

static void voice_block(voice_t *v, uint32_t card)
{
    const part_t *g = &pt[v->part];
    const uint8_t *pv = g->p->v;
    int32_t lfo = g->lfo_val, p8, cut, fenv, a;
    uint32_t glide_k = g->glide_k;

    /* Glide / Portamento slew */
    if (v->pitch16 != v->target16) {
        if (!glide_k || g->porta_mode == PORTA_OFF) {
            v->pitch16 = v->target16;
        } else {
            int32_t diff = v->target16 - v->pitch16;
            v->pitch16 += (int32_t)(((int64_t)diff * glide_k) >> 16);
            if (diff > -64 && diff < 64) v->pitch16 = v->target16;
        }
    }

    /* Envelopes */
    int32_t v_env = VINT_ENV_WARM[card];
    /* Decay: 1.08x calibration matching S-1 hardware decay time */
    int32_t dec_scaled = ((int32_t)pv[P_DECAY] * 115) / 106;
    if (dec_scaled > 127) dec_scaled = 127;
    /* Release: 1.25x calibration matching S-1 hardware release time */
    int32_t rel_scaled = ((int32_t)pv[P_RELEASE] * 5) / 4;
    if (rel_scaled > 127) rel_scaled = 127;
    env_tick(&v->fe, pv[P_ATTACK], dec_scaled, g->sus_f, rel_scaled, v->gate, v_env);
    env_tick(&v->ae, pv[P_ATTACK], dec_scaled, g->sus_a, rel_scaled, v->gate, v_env);
    fenv = v->fe.level >> 9;

    /* Analog thermal drift wandering filter (~1.5s time constant) */
    v->drift += (((int32_t)(xorshift(&v->noise) >> 16) - 32768) - v->drift) >> 8;

    /* Pitch calculation with analog card detuning & slow thermal drift */
    int32_t v_tune = VINT_TUNE8_WARM[card];
    p8 = (v->pitch16 >> 8) + g->range_semi8 + g->fine_tune8 + g->transp_semi8 + g->bend8 + ctl_tune8
        + v->det8 + v_tune + (v->drift >> 11);
    int32_t lfo_pd = g->lfo_pd;
    if (pv[P_LFO_WAVE] == LFO_NOISE) {
        /* Noise pitch modulation is handled per-sample in voice_render */
        lfo_pd = 0;
    }
    p8 += (lfo * lfo_pd) >> 15;
    if (ctl_vib) {
        p8 += (lfo * ctl_vib) >> 14;   /* CC 1 Mod Wheel vibrato */
    }
    v->inc = pitch_inc(p8);

    /* PWM / Square Wave Duty Cycle (Phase) Calculation */
    if (g->pwm_src == PWM_LFO) {
        /* PWM always modulated by triangle LFO (S-1 behavior: waveform selector is pitch-only) */
        int32_t pwm_lfo = lfo_shape(g, g->lfo_ph, LFO_TRI);
        int64_t p = (int64_t)0x80000000u + (((int64_t)pwm_lfo * (int32_t)pv[P_PWM_DEPTH]) * 464);
        if (p < (1 << 26)) p = 1 << 26;
        if (p > (int64_t)0xFFFFFFFFu - (1 << 26)) p = (int64_t)0xFFFFFFFFu - (1 << 26);
        v->pw = (uint32_t)p;
    } else if (g->pwm_src == PWM_ENV) {
        /* Envelope modulates pulse width symmetrically from 50% square wave */
        int64_t p = (int64_t)0x80000000u - (((int64_t)fenv * (int32_t)pv[P_PWM_DEPTH]) * 464);
        if (p < (1 << 26)) p = 1 << 26;
        if (p > (int64_t)0xFFFFFFFFu - (1 << 26)) p = (int64_t)0xFFFFFFFFu - (1 << 26);
        v->pw = (uint32_t)p;
    } else {
        /* Manual Duty Cycle: On Roland S-1, 0 = 50% square wave, 127 = ~5% narrow pulse */
        uint32_t delta = (uint32_t)pv[P_PWM_DEPTH] * 0x00E80000u;
        v->pw = 0x80000000u - delta;
    }
    int32_t v_pw = VINT_PW8_WARM[card];
    v->pw += (uint32_t)v_pw; /* Natural analog pulse width variance per voice */

    /* Filter cutoff calculation (calibrated to Roland S-1 curve) */
    int32_t env_mult = 244;
    int32_t env_sweep = (fenv * g->fenv_amt * env_mult) >> 15;
    cut = g->cut_sm + env_sweep
        + ((int32_t)v->note - 60) * (int32_t)g->keytrk + ((lfo * g->lfo_fd) >> 15)
        + g->flt_bend8;
    cut = clampi(cut, 0, 136 << 8);
    svf_coef(&v->c1, cut, g->reso_k1);
    svf_coef(&v->c2, cut, 8192);

    /* Amplitude calculation */
    v->amp0 = v->amp1;
    if (pv[P_AMP_MODE] == AMP_GATE) {
        a = v->gate ? 32767 : 0;
    } else {
        a = v->ae.level >> 9;
    }
    /* S-1 Parity Velocity Sensitivity: scales VCA amplitude with ~18 dB musical dynamic range */
    int32_t vel = v->vel ? (int32_t)v->vel : 100;
    int32_t vel_q15 = 4096 + (vel * 225);
    a = (a * vel_q15) >> 15;
    v->amp1 = (a * g->gain_q12) >> 12;
    ui_vlevel[card] = a;
    ui_vnote[card] = v->note;
}

/* Program change smooth de-click ramp: 5.3ms fade-out -> swap patch at zero -> 5.3ms fade-in */
static volatile int32_t patch_fade_q15 = 32768;
static volatile int32_t patch_fade_step = 0;
static volatile uint32_t patch_pending = 0xFFFFFFFFu;
static volatile uint8_t patch_pending_valid = 0;
static int32_t blk_fade_f0 = 32768, blk_fade_df = 0;
static void apply_program(uint32_t n);

/* Render audio samples of one voice */
static void voice_render(voice_t *v, int32_t *out)
{
    uint32_t i, inc = v->inc, pw = v->pw, d = inc >> 15;
    int32_t amp0 = v->amp0, da = v->amp1 - v->amp0, gl = v->gl, gr = v->gr;
    const part_t *g = &pt[v->part];
    int32_t l_sqr = g->lvl_sqr, l_saw = g->lvl_saw, l_sub = g->lvl_sub, l_noi = g->lvl_noi;
    uint32_t sub_t = g->sub_type, draw_sw = g->draw_sw, draw_mult = g->draw_mult_q8;
    uint32_t comb_q8 = g->chop_comb_q8;
    const int16_t *draw_table = seq_draw_table();
    const uint16_t *chop_mask = seq_chop_masks();

    /* Noise LFO pitch modulation using smoothed noise (prevents aliasing leakage) */
    int32_t noise_pd = 0;
    int32_t noise_val = 0;
    if (g->p->v[P_LFO_WAVE] == LFO_NOISE) {
        noise_pd = (g->lfo_pd * 255) >> 8;
        noise_val = g->lfo_noi_sm;
    }

    /* Roland S-1 1 kHz high-frequency roll-off & hardware gating for OSC CHOP */
    int chop_active = (chop_mask[0] != 0xFFFFu || chop_mask[1] != 0xFFFFu ||
                       chop_mask[2] != 0xFFFFu || chop_mask[3] != 0xFFFFu);
    int32_t g_masked = (chop_active && g->chop_ovtn > 0) ? g->g_chop_masked : 32767;
    if (g_masked != 32767 && inc > 87652000u) {
        if (inc >= 107130000u) {
            g_masked = 32767; /* 100% dry above 1.1 kHz */
        } else {
            /* Smooth linear crossfade from g_masked to 32767 (900 Hz to 1100 Hz) */
            uint32_t fade = (uint32_t)(((uint64_t)(inc - 87652000u) * 32767u) / (107130000u - 87652000u));
            g_masked += (int32_t)(((int64_t)(32767 - g_masked) * fade) >> 15);
        }
    }

    for (i = 0; i < CTL; i++) {
        int32_t s_sqr, s_saw, s_sub, s_noi, mix, lp, bp, hp, y, amp;

        /* Smoothed noise pitch modulation for LFO_NOISE */
        uint32_t inc_i = inc, d_i = d;
        if (noise_pd) {
            int32_t delta_p8 = (noise_val * noise_pd) >> 15;
            /* Convert Q8 semitones to phase increment delta: inc * delta_p8 * ln(2) / 3072 */
            int32_t delta_inc = (int32_t)(((int64_t)inc * delta_p8 * 3783) >> 24);
            int32_t inc_signed = (int32_t)inc + delta_inc;
            inc_i = (uint32_t)inc_signed;
            d_i = inc_i >> 15;
        }

        /* 1. Square / Pulse / DRAW */
        if (draw_sw != DSW_OFF) {
            v->o_sqr.ph += inc_i;
            s_sqr = osc_draw_sample(draw_table, v->o_sqr.ph, draw_mult, draw_sw);
        } else {
            s_sqr = osc_step(&v->o_sqr, inc_i, d_i, pw, OSC_PLS, -65536, -1);
        }

        /* 2. Sawtooth */
        s_saw = osc_step(&v->o_saw, inc_i, d_i, 0, OSC_SAW, -65536, -1);

        /* 3. Sub-Oscillator: -1oc (50% sqr), -2oc (50% sqr), -2o.A (25% pulse) */
        uint32_t inc_sub = (sub_t == SUB_1OC) ? (inc_i >> 1) : (inc_i >> 2);
        uint32_t d_sub = inc_sub >> 15;
        uint32_t pw_sub = (sub_t == SUB_2OA) ? 0x40000000u : 0x80000000u;
        s_sub = osc_step(&v->o_sub, inc_sub, d_sub, pw_sub, OSC_PLS, -65536, -1);

        /* 4. Noise (Calibrated to Roland S-1 hardware spectral character) */
        if (g->noise_mode == NOISE_PINK) {
            s_noi = pink_noise_step(&v->pn, (int32_t)(xorshift(&v->noise) >> 16) - 32768);
        } else {
            int32_t w = (int32_t)(xorshift(&v->noise) >> 16) - 32768;
            /* One-pole lowpass matching S-1 analog bandwidth (~4 kHz roll-off) */
            v->noise_lp += (w - v->noise_lp) >> 1;
            s_noi = (v->noise_lp * 180) >> 8;
        }

        /* 5. OSC CHOP Slicing (Anti-Aliased & synced to fundamental) */
        if (g_masked != 32767) {
            s_sqr = osc_chop_sample(s_sqr, v->o_sqr.ph, chop_mask[0], comb_q8, g_masked);
            s_saw = osc_chop_sample(s_saw, v->o_saw.ph, chop_mask[1], comb_q8, g_masked);
            s_sub = osc_chop_sample(s_sub, v->o_saw.ph, chop_mask[2], comb_q8, g_masked);
            s_noi = osc_chop_sample(s_noi, v->o_saw.ph, chop_mask[3], comb_q8, g_masked);
            /* Level compensation matching S-1 CHOP gain staging (0.65x) */
            s_sqr = (s_sqr * 21294) >> 15;
            s_saw = (s_saw * 21294) >> 15;
            s_sub = (s_sub * 21294) >> 15;
            s_noi = (s_noi * 21294) >> 15;
        }

        /* 6. Composite Voice Mixer */
        mix = (s_sqr * l_sqr + s_saw * l_saw + s_sub * l_sub + s_noi * l_noi) >> 7;
        mix = clampi(mix, -65535, 65535);

        /* 7. Roland 4-Pole Cascaded SVF with authentic OTA resonance saturation */
        svf_step(&v->c1, &v->f1, mix, &lp, &bp, &hp);
        y = svf_lp(&v->c2, &v->f2, lp);

        /* 8. Amplifier Envelope and Panning */
        amp = amp0 + ((da * (int32_t)i) >> 4);
        if (blk_fade_f0 < 32768 || blk_fade_df != 0) {
            int32_t f = blk_fade_f0 + ((blk_fade_df * (int32_t)i) >> 4);
            if (f < 0) f = 0;
            if (f > 32768) f = 32768;
            amp = (int32_t)(((int64_t)amp * f) >> 15);
        }
        y = (y * amp) >> 16;
        out[2u * i]      += (y * gl) >> 12;
        out[2u * i + 1u] += (y * gr) >> 12;
    }
}

static int32_t bus_b[2u * HALF_FRAMES];

static void engine_audio(int32_t *out, uint32_t n)
{
    uint32_t b, v, i, was_own = b_own, dry, bus = b_own;
    static int32_t sc_prev;
    static uint32_t sc_i, sc_sub;

    fx_block(&live, ctl_ext_quarter ? ctl_ext_quarter : 60u * FS / clampi(ctl_tempo, 40, 240));
    vol_q12 = (int32_t)((ctl_cc7 * ctl_cc7) >> 2);

    for (v = 0; v < NV; v++)
        if (vc[v].part && vc[v].ae.stage != E_IDLE) bus = 1;
    if (bus)
        for (i = 0; i < 2u * n; i++) bus_b[i] = 0;

    for (b = 0; b < n; b += CTL) {
        parts_point();
        if (was_own && !b_own) parts_off(1);
        engine_events();
        if (ctl_arp_restart) {
            ctl_arp_restart = 0;
            arp[0].pos = arp[1].pos = 0;
            arp[0].cnt = arp[1].cnt = 0xFFFFFFFFu;
        }
        arp_tick(0);
        if (b_own) arp_tick(1);
        seq_tick();
        parts_point();
        if (was_own && !b_own) parts_off(1);
        was_own = b_own;
        lfo_tick(&pt[0]);
        if (b_own) lfo_tick(&pt[1]);
        block_globals(&pt[0]);
        if (b_own) block_globals(&pt[1]);

        /* Update program change fade ramp */
        int32_t f0 = patch_fade_q15;
        if (patch_fade_step < 0) {
            patch_fade_q15 += patch_fade_step;
            if (patch_fade_q15 <= 0) {
                patch_fade_q15 = 0;
                f0 = 0;
                if (patch_pending_valid) {
                    apply_program(patch_pending);
                    patch_pending_valid = 0;
                    for (uint32_t v_idx = 0; v_idx < NV; v_idx++) {
                        if (vc[v_idx].gate) {
                            vc[v_idx].ae.stage = E_ATT;
                            vc[v_idx].fe.stage = E_ATT;
                            vc[v_idx].ae.level = 0;
                            vc[v_idx].fe.level = 0;
                            vc[v_idx].amp0 = 0;
                            vc[v_idx].amp1 = 0;
                        } else {
                            vc[v_idx].ae.stage = E_IDLE;
                            vc[v_idx].amp0 = vc[v_idx].amp1 = 0;
                        }
                    }
                }
                patch_fade_step = 2048; /* Fade back up over 16 blocks (5.3ms) */
            }
        } else if (patch_fade_step > 0) {
            patch_fade_q15 += patch_fade_step;
            if (patch_fade_q15 >= 32768) {
                patch_fade_q15 = 32768;
                patch_fade_step = 0;
            }
        }
        blk_fade_f0 = f0;
        blk_fade_df = patch_fade_q15 - f0;

        for (v = 0; v < NV; v++) {
            if (vc[v].ae.stage == E_IDLE && !vc[v].gate) {
                if (ui_vlevel[v]) { ui_vlevel[v] = 0; vc[v].amp0 = vc[v].amp1 = 0; }
                /* Free-running oscillator phase advance */
                uint32_t step = vc[v].inc * CTL;
                vc[v].o_sqr.ph += step;
                vc[v].o_saw.ph = vc[v].o_sqr.ph;
                uint32_t step_sub = (pt[vc[v].part].sub_type == SUB_1OC) ? (step >> 1) : (step >> 2);
                vc[v].o_sub.ph += step_sub;
                continue;
            }
            voice_block(&vc[v], v);
            voice_render(&vc[v], (vc[v].part && bus ? bus_b : out) + 2u * b);
        }
    }

    dry = bus ? seq_b_dry() : 0u;
    for (i = 0; bus && i < 2u * n; i++) {
        int32_t x = bus_b[i], d = (x * (int32_t)dry) >> 7;
        out[i] += x - d;
        bus_b[i] = d;
    }

    /* Output Stage with soft saturation */
    for (i = 0; i < n; i++) {
        int32_t l = softclip((clampi(out[2u * i], -262144, 262143) >> 2) * vol_q12 >> 10);
        int32_t r = softclip((clampi(out[2u * i + 1u], -262144, 262143) >> 2) * vol_q12 >> 10);
        int32_t m = (l + r) >> 1;
        out[2u * i] = l;
        out[2u * i + 1u] = r;

        if (scope_state == 1u) {
            if (sc_prev < 0 && m >= 0) { scope_state = 2; sc_i = 0; sc_sub = 0; }
        } else if (scope_state == 2u) {
            if (!sc_sub++) scope[sc_i++] = (int16_t)m;
            if (sc_sub >= scope_stride) sc_sub = 0;
            if (sc_i >= SCOPE_N) scope_state = 3;
        }
        sc_prev = m;
    }

    fx_process(out, n);

    if (dry) {
        for (i = 0; i < 2u * n; i++)
            out[i] = sat16(out[i] + softclip((clampi(bus_b[i], -262144, 262143) >> 2) * vol_q12 >> 10));
    }

    /* Metronome click */
    for (i = 0; i < n && click_amp; i++) {
        int32_t c;
        click_ph += click_inc;
        c = (click_ph & 0x80000000u) ? click_amp : -click_amp;
        out[2u * i]      += c;
        out[2u * i + 1u] += c;
        click_amp -= (click_amp >> 8) + 1;
        if (click_amp < 64) click_amp = 0;
    }
}

static int engine_quiet(void)
{
    for (uint32_t v = 0; v < NV; v++)
        if (vc[v].ae.stage != E_IDLE || vc[v].gate) return 0;
    return nq_r == nq_w;
}

static void engine_init(void)
{
    for (uint32_t v = 0; v < NV; v++) {
        vc[v].noise = 0x12345678u * (v + 1u) + 0x9E3779B9u;
        uint32_t ph = xorshift(&vc[v].noise) + v * 0x40000000u;
        vc[v].o_sqr.ph = ph;
        vc[v].o_saw.ph = ph;
        vc[v].o_sub.ph = ph >> 1;
        vc[v].o_sub.prev = 0;
        vc[v].inc = pitch_inc((60 + (int32_t)v * 3) << 8);
        vc[v].pn.b0 = vc[v].pn.b1 = vc[v].pn.b2 = 0;
        vc[v].gl = vc[v].gr = 2896;
    }
    pt[0].lfo_rnd_a = xorshift(&rnd_state);
    pt[0].lfo_rnd_b = xorshift(&rnd_state);
    pt[1].lfo_rnd_a = 0x6A09E667u;
    pt[1].lfo_rnd_b = 0xBB67AE85u;
    fx_init();
    engine_ready = 1;
}

#endif /* OBV_ENGINE_H */
