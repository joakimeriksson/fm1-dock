/* SPDX-License-Identifier: GPL-3.0-only */
/* One Scintilla voice: a port of synthex-web's VoiceCore (packages/engine/src/dsp/voice-core.ts) to
 * fixed point. VoiceCore is the reference: tools/ref_render.mjs renders it, host/sc_render.c renders
 * this, and tests/run.sh compares the two. Keep the order of operations in sc_voice_render() the same
 * as VoiceCore.render(); where they differ on purpose, the comment says so. */
#ifndef SC_VOICE_H
#define SC_VOICE_H
#include <stdint.h>
#include "sc_patch.h"

/* The joystick performance section (panel controls, not patch memory), Q16. */
typedef struct {
    int32_t joy_x, joy_y;                       /* -1..1 */
    int32_t bend_osc, bend_filt, lfo2_osc, lfo2_filt;   /* the four sliders, 0..1 */
} sc_perf_t;

/* What the voices of one layer derive from its patch and the performance controls: computed in
 * the main loop when either changes (sc_prep), read by the audio interrupt. */
typedef struct {
    const sc_layer_t *L;
    uint32_t fa_inc, fd_inc, aa_inc, ad_inc;    /* envelope attack / decay steps, Q30 per sample */
    int32_t  fs, as;                            /* sustain levels, Q30 */
    int32_t  fr_mul, ar_mul;                    /* release: per-sample multiplier, Q30 (20 dB per r) */
    uint32_t lfo_inc[2], lfo_dly_inc[2];        /* phase step; delay ramp step (0: no delay) */
    int32_t  lfo_joy[2];                        /* 1 + joyXToLfoNDepth * joyX, Q16 */
    int32_t  semi1, semi2;                      /* octave (and OSC2's detune) in semitones, Q16 */
    int32_t  joy_pitch;                         /* the stick's static pitch bend, semitones Q16 */
    int32_t  kp1[2], kp2[2], kp2e;              /* pitch per unit LFO1/LFO2 (and ENV1 for OSC2), Q16 */
    int32_t  kw1[2], kw2[2], cross, cross2;     /* PWM per unit LFO and per unit cross-mod source, Q16 */
    int32_t  pw1, pw2;                          /* static pulse widths, Q16 */
    int32_t  ka[2];                             /* amplitude per unit LFO, Q16 */
    int32_t  c_base, kc[2], kc_env, kc_env1, kc_vel, kc_key;   /* cutoff, semitones Q16 */
    int32_t  k_q28, atten;                      /* the SVF's 1/Q; input attenuation (Q16) */
    int32_t  mix1, mix2, mixn;
    int32_t  glide_coef;                        /* portamento step, Q30 */
    int32_t  gdc1, gdc2, gdc_panel;             /* per-oscillator and panel glide decay, Q30 */
    uint32_t use;                               /* SC_USE_*: which per-sample paths this patch needs */
} sc_prep_t;

/* sc_prep_t.use: work the render loop skips when a patch does not route it. Skipping is exact: a
 * term that is not computed is one that would have added zero. (A skipped LFO does not advance, so
 * if a route to it is switched on mid-note, its phase is not what VoiceCore's would be.) */
#define SC_USE_L1     1u                        /* LFO1 reaches pitch, PWM, cutoff or amplitude */
#define SC_USE_L2     2u
#define SC_USE_P1MOD  4u                        /* OSC1's pitch moves per sample (LFOs) */
#define SC_USE_P2MOD  8u                        /* OSC2's (LFOs, ENV1) */
#define SC_USE_AMP    16u                       /* an LFO on amplitude */
#define SC_USE_NOISE  32u                       /* the noise generator is heard (or OSC2 shares it) */

void sc_prep(sc_prep_t *p, const sc_layer_t *L, const sc_perf_t *perf);

typedef struct { int8_t stage; int32_t level; int32_t rel_mul; } sc_env_t;
typedef struct { uint32_t ph, rng; int32_t sh, sh_next, smooth, ramp; } sc_lfo_t;
typedef struct { uint32_t ph; int32_t pend, tau; uint8_t after, wrapped; } sc_osc_t;   /* tau: a pending sync, Q16, or -1; pend / after: the residual owed to the sample after a sync */
typedef struct { int32_t ic1, ic2; } sc_svf_t;
typedef struct { uint32_t w, counter; int32_t bands[16], sum; } sc_pink_t;

typedef struct {
    sc_osc_t o1, o2;
    uint32_t noise1, noise2;
    sc_pink_t pink;
    sc_env_t env1, env2;
    sc_lfo_t lfo1, lfo2;
    sc_svf_t f1, f2;
    int64_t cur, target;                        /* pitch, Q32: a slow portamento's steps are far below Q16 */
    int32_t vel;                                /* 0..1, Q16 */
    int32_t bend;                               /* this voice's bend (pitch bend or unison detune), Q16 */
    int32_t off1, off2, c1, c2;                 /* glide pitch offsets (Q24) and their decay (Q30) */
    int32_t l1, l2;                             /* the last LFO outputs, for the block-rate cutoff */
    int32_t prev1, prev2;                       /* the last oscillator outputs, for cross-mod */
    uint32_t block;                             /* samples into the coefficient block */
    int32_t a1, a2, a3;                         /* SVF coefficients, Q30 */
    int32_t cn;                                 /* the cutoff (semitones Q16) they were computed for */
    uint32_t amp_gate;                          /* samples until the amp envelope starts (SC_AMP_ONSET_DELAY) */
    int32_t n1, n2;                             /* the pitches dt1 / dt2 were computed for */
    uint32_t dt1, dt2;
} sc_voice_t;

#define SC_COEFF_BLOCK 128                      /* as VoiceCore's COEFF_BLOCK */
/* VoiceCore's CUTOFF_STEP: between block boundaries the coefficients are recomputed as soon as the
 * cutoff has moved half a semitone from the note they were computed for (a 2 ms filter attack is
 * shorter than a block; holding the old coefficients through it put a click on Take-off) */
#define SC_CUTOFF_STEP (1 << 15)
/* VoiceCore's AMP_ONSET_DELAY, 1.5 ms: the amp envelope starts this many samples after note-on,
 * the filter envelope at once (the hardware's VCA opens after the filter envelope's fastest attack) */
#define SC_AMP_ONSET_DELAY 66

void sc_voice_init(sc_voice_t *v, uint32_t seed);
void sc_voice_note_on(sc_voice_t *v, const sc_prep_t *p, int32_t note, int32_t vel_q16);
void sc_voice_note_off(sc_voice_t *v, const sc_prep_t *p);
static inline int sc_voice_idle(const sc_voice_t *v) { return v->env2.stage == 0 && v->amp_gate == 0; }
/* add n samples of this voice (audio Q20, mono) into out */
void sc_voice_render(sc_voice_t *v, const sc_prep_t *p, int32_t *out, uint32_t n);

uint32_t sc_pitch_inc(int32_t note_q16);

#endif
