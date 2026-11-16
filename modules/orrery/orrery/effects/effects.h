/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_EFFECTS_H
#define MD_EFFECTS_H
#include "core/effect.h"

/* Local musical effects (PLAN.md "Mixer and effects", decision 006). Each is a
 * caller-owned instance behind core/effect.h; every one takes spans of 1..256 frames and
 * produces identical output for any partition, because all control-rate work happens
 * at fixed sample phases of the instance, not per call. Integer only; no division per
 * sample (control-rate 32-bit divisions and a restoring 64/32 division only); 64-bit
 * multiplies as in the engines. Inputs are clamped (MD_FX_INPUT_LIMIT, the filter
 * +6 dB), so the mixer's +48 dB headroom cannot overflow them.
 *
 * Settings: each parameter in descriptor order as a little-endian int16 (version 1). */

#define MD_FX_FILTER_ID 0x00020001u
#define MD_FX_PHASER_ID 0x00020002u
#define MD_FX_ECHO_ID 0x00020003u
#define MD_FX_REVERB_ID 0x00020004u
#define MD_FX_INPUT_LIMIT (1 << 20)   /* +30 dB over Q15 full scale */
#define MD_FX_CONTROL 8u              /* control-rate period in samples, a power of two */

/* Filter: insert and master sweep. Cutoff units: f = 20 Hz * 2^(v / 100), 0..990. */
enum { MD_FILTER_TYPE = 1, MD_FILTER_CUTOFF, MD_FILTER_RESONANCE };
enum { MD_FILTER_LOWPASS, MD_FILTER_BANDPASS, MD_FILTER_HIGHPASS };
typedef struct {
    int32_t type, cutoff, resonance;     /* parameter values */
    int32_t cut_q8, res_q8;              /* smoothed, Q8 parameter units */
    int32_t a1, a2, a3, k;               /* Q24 coefficients */
    int32_t ic1[2], ic2[2], e1[2], e2[2]; /* integrator states (Q23) and rounding carries */
    uint32_t phase;
} md_fx_filter_t;

/* Phaser: four first-order allpasses per channel swept by a triangle LFO, right channel
 * a quarter cycle ahead. Rate in centi-Hz. */
enum { MD_PHASER_RATE = 1, MD_PHASER_DEPTH, MD_PHASER_FEEDBACK, MD_PHASER_MIX };
#define MD_PHASER_STAGES 4u
typedef struct {
    int32_t rate, depth, feedback, mix;
    int32_t fb_q15, mix_q15, depth_q8;   /* smoothed */
    uint32_t lfo, inc;
    int32_t a[2];                        /* allpass coefficient per channel, Q24 */
    int32_t x1[2][MD_PHASER_STAGES], y1[2][MD_PHASER_STAGES], last[2];
    uint32_t phase;
} md_fx_phaser_t;

/* Tape echo, after fm1-harbor's firmware/src/hb_echo.c: a quarter-rate line, a one-pole
 * tone inside the feedback loop, the right tap 20 ms behind. Sync 0 is free time in ms;
 * 1..6 are 1/16, 1/8, 1/8 dotted, 1/4, 1/4 dotted, 1/2 of the tempo. Time changes glide. */
enum { MD_ECHO_SYNC = 1, MD_ECHO_TIME, MD_ECHO_FEEDBACK, MD_ECHO_TONE };
#define MD_ECHO_DIV 4u
#define MD_ECHO_LEN 12288u
#define MD_ECHO_SPREAD 220u
typedef struct {
    int32_t sync, time_ms, feedback, tone;
    uint16_t bpm_q8, sub;
    int32_t gain, k;                     /* feedback Q15, tone Q15: smoothed once a group */
    int32_t gain_target, k_target;
    int32_t target_q8, delay_q8;         /* delay in line samples, Q8 */
    int32_t acc, lp, yl, yr, pl, pr;     /* group input sum, tone state, current and previous taps */
    uint32_t w;
    int16_t line[MD_ECHO_LEN];
} md_fx_echo_t;

/* Reverb: four lines in a Hadamard loop (the decay and level laws of fm1-harbor's firmware/src/hb_fx.c, itself
 * Scintilla's at half the lines), run at half the sample rate so the same memory holds twice the time (268 ms of
 * line: twice the resonances a hertz), two of the taps moved slowly (a hall's tail drifts instead of ringing on fixed
 * pitches), the input through two allpasses a channel (a struck sound arrives as a cluster, not as slaps) and a
 * lowpass inside the loop (the top of the tail dies first). */
enum { MD_REVERB_ALGORITHM = 1, MD_REVERB_SIZE, MD_REVERB_DAMPING };
enum { MD_REVERB_ROOM, MD_REVERB_PLATE, MD_REVERB_HALL, MD_REVERB_CATHEDRAL };
#define MD_REVERB_LINES 4u
#define MD_REVERB_LEN 2048u
#define MD_REVERB_AP 256u                /* an input allpass's line (two a channel) */
#define MD_REVERB_SPAN 256u              /* frames a pass (a longer call is taken in pieces) */
typedef struct {
    int32_t algorithm, size, damping;
    int32_t g[MD_REVERB_LINES], a, wet;  /* Q30 line gains, Q16 output lowpass and level */
    int32_t loop;                        /* the lowpass in the loop, thirty-seconds */
    int32_t lpl, lpr;
    int32_t lp[MD_REVERB_LINES];         /* the loop's lowpasses */
    int32_t hold_l, hold_r;              /* the frame waiting for its pair */
    int32_t prev_l, prev_r, cur_l, cur_r;   /* the last two half-rate outputs */
    uint32_t w, odd, lfo1, lfo2;
    uint16_t at[4];                      /* the allpasses' places */
    int32_t tick[2][MD_REVERB_SPAN / 2u + 1u];   /* a span's half-rate frames: in, then out */
    int16_t ap[4][MD_REVERB_AP];
    int16_t line[MD_REVERB_LEN * MD_REVERB_LINES];
} md_fx_reverb_t;

extern const md_effect_desc_t MD_FX_FILTER, MD_FX_PHASER, MD_FX_ECHO, MD_FX_REVERB;
extern const md_effect_registry_t MD_FX_REGISTRY;
#endif
