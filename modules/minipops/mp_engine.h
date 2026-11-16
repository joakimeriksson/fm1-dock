/* SPDX-License-Identifier: GPL-3.0-only */
/* The Korg Mini Pops 7 (1973) as an integer engine: the web engine of ai-smarthome/audio/software-synth
 * (js/minipops-processor.js, the reference) ported for the FM-1. Ten keys carrying two rhythms each,
 * an A/B lever, keys pressed together combine their rhythms; the three additional rhythms (quijada,
 * guiro, tambourine) at a level; fourteen tone-generator voices built from the SR-95 service manual's
 * output-level table and fitted to recordings. Its tables (mp_tables.h) come from the web engine
 * (take.mjs); tests/minipops_check.py renders both and compares them.
 *
 * Integers only: signals Q20, coefficients Q28 / Q30, one 32-bit division a sample at most (the
 * saturation), none 64-bit. The settings (mp_set_*) may come from another context than mp_render (the
 * main loop and the audio interrupt): they are taken up at the start of the next block. */
#ifndef MP_ENGINE_H
#define MP_ENGINE_H
#include <stdint.h>
#include "mp_tables.h"

#define MP_KEYS 10
#define MP_PENDING 64

typedef struct {
    int32_t y1, y2;                             /* a resonator */
    int32_t y1b, y2b;                           /* the second one */
    int32_t ic1, ic2;                           /* the bandpass */
    int32_t env;                                /* Q30 */
    int32_t vel;                                /* Q16 */
    int32_t gate, pulse;                        /* samples: the guiro's scrape left; until the next tick */
    int32_t lp, dcx, dcy;                       /* Q20 */
    uint32_t seed, n;
    uint8_t active;
} mp_voice_t;

typedef struct { uint8_t voice; uint16_t pos100, gate100; } mp_mhit_t;

typedef struct {
    /* the settings as last set (any context), taken up by mp_render */
    volatile uint32_t req_seq;
    uint16_t req_keys;                          /* bit k: key k down */
    uint8_t req_section;                        /* 0 A, 1 B */
    uint16_t req_tempo;                         /* 20..300 */
    uint8_t req_volume, req_balance;            /* percent */
    uint8_t req_extra[3];                       /* quijada, guiro, tambourine: percent */
    uint8_t req_play;                           /* 1 running; a change from 0 starts at step 0 */
    uint16_t req_delay;                         /* samples into the block the start waits (mp_play_at) */
    /* mp_render's own */
    uint32_t seq;
    uint16_t keys;
    uint8_t section, playing, len, div;
    uint16_t tempo;
    int32_t lo_g, hi_g, vol;                    /* Q16 */
    int32_t extra[3];                           /* Q16 */
    uint16_t n_hits;
    mp_mhit_t hits[MP_MAX_HITS];                /* the keys down, merged, plus the additional rhythms */
    uint32_t sps_q8;                            /* samples a step, Q8 */
    int32_t counter_q8;                         /* samples to the next step, Q8 */
    uint8_t step;
    uint8_t n_pend;
    uint8_t pend_voice[MP_PENDING];
    int32_t pend_at[MP_PENDING], pend_gate[MP_PENDING];
    int32_t bal_lp;
    volatile uint32_t hit_mask;                 /* the voices struck since mp_take_hits (bit = voice) */
    volatile uint32_t steps;                    /* steps played since the start (the position: steps % len) */
    mp_voice_t v[MP_VOICES];
} mp_t;

void mp_init(mp_t *m);                          /* silent, stopped, 120 bpm, volume 80, balance 50, no key */
void mp_set_keys(mp_t *m, uint32_t keys, uint32_t section);
void mp_set_tempo(mp_t *m, uint32_t bpm);
void mp_set_level(mp_t *m, uint32_t volume_pct, uint32_t balance_pct);
void mp_set_extra(mp_t *m, uint32_t quijada_pct, uint32_t guiro_pct, uint32_t tambourine_pct);
void mp_play(mp_t *m, uint32_t on);
void mp_play_at(mp_t *m, uint32_t delay);       /* from step 0, delay samples into the next block (the host aligns it) */
/* one voice struck by hand (the render context only: a pad, a keyboard), vel Q16 */
void mp_trigger(mp_t *m, uint32_t voice, int32_t vel);
/* the audio interrupt: add n stereo frames (L R, Q15; the machine is mono) into out */
void mp_render(mp_t *m, int32_t *out, uint32_t n);
uint32_t mp_take_hits(mp_t *m);
/* the rhythm a key carries: section 0 A, 1 B; key 0..9 */
static inline const mp_rhythm_t *mp_rhythm(uint32_t section, uint32_t key) { return &MP_RHYTHM[(section ? 10u : 0u) + (key % 10u)]; }
extern const char *const MP_VOICE_NAMES[MP_VOICES];

#endif
