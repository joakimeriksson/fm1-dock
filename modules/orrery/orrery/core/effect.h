/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_EFFECT_H
#define MD_EFFECT_H
#include "param.h"

enum { MD_EFFECT_INSERT = 1u << 0, MD_EFFECT_BUS = 1u << 1 };
typedef struct md_effect_desc {
    md_processor_desc_t processor;
    uint16_t capabilities;
    int (*init)(void *state, uint32_t sample_rate);
    int (*set_param)(void *state, md_param_id_t id, int32_t value);
    /* Required when param_count>0; pure owning-context current-value query. */
    int (*get_param)(const void *state, md_param_id_t id, int32_t *value);
    /* BPM Q8, range 40*256..240*256; called on change, not as another clock. */
    void (*set_tempo)(void *state, uint16_t bpm_q8);
    /* INSERT: replace all requested frames in-place, never reads future frames. */
    void (*process_replace)(void *state, int32_t *stereo, uint32_t frames);
    /* BUS: distinct non-overlapping buffers, replace output with wet-only signal. */
    void (*process_wet)(void *state, const int32_t *input, int32_t *wet, uint32_t frames);
    void (*reset)(void *state); /* clear delay/filter history, retain parameters */
    int (*encode)(const void *state, uint8_t *dst, uint32_t capacity);
    int (*validate)(const uint8_t *src, uint32_t bytes);
    int (*apply)(void *state, const uint8_t *src, uint32_t bytes);
} md_effect_desc_t;
typedef struct {
    const md_effect_desc_t *desc;
    void *state;
    uint32_t sample_rate;
} md_effect_t;
typedef struct {
    const md_effect_desc_t *const *items;
    uint16_t count;
} md_effect_registry_t;

int md_effect_registry_validate(const md_effect_registry_t *);
const md_effect_desc_t *md_effect_find(const md_effect_registry_t *, uint32_t id);
int md_effect_init(md_effect_t *, const md_effect_desc_t *, void *state,
                   uint32_t state_bytes, uint32_t sample_rate);
int md_effect_param(md_effect_t *, md_param_id_t id, int32_t value);
int md_effect_param_get(const md_effect_t *, md_param_id_t id, int32_t *value);
int md_effect_tempo(md_effect_t *, uint16_t bpm_q8);
void md_effect_process_replace(md_effect_t *, int32_t *stereo, uint32_t frames);
void md_effect_process_wet(md_effect_t *, const int32_t *input, int32_t *wet, uint32_t frames);
void md_effect_reset(md_effect_t *);
int md_effect_encode(const md_effect_t *, uint8_t *dst, uint32_t capacity);
int md_effect_validate(const md_effect_desc_t *, uint16_t version, const uint8_t *, uint32_t bytes);
int md_effect_apply(md_effect_t *, uint16_t version, const uint8_t *, uint32_t bytes);

/* Common descriptor/init/settings/error/render rules match instrument.h.
 * Required process callback follows capability bits; the other may be NULL.
 * Optional tempo callback may be NULL (tempo returns UNSUPPORTED).
 * Mixer owns bypass crossfades, send/return gains and tail policy, not these DSP
 * functions. Muting input feeds zero while the bus drains normally; stopped
 * transport does not reset tails. Panic/project replacement does reset them.
 * Buffers use interleaved stereo Q15 in int32_t; no dry signal in bus output.
 * Runtime processor state and pools are never shared among slots or buses.
 * Readback follows instrument.h: audio/stopped owner only, readonly readable,
 * output unchanged on error, current/effective value rather than model base. */
#endif
