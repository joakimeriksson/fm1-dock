/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_INSTRUMENT_H
#define MD_INSTRUMENT_H
#include "event.h"

enum {
    MD_CAP_MONO = 1u << 0, MD_CAP_POLY = 1u << 1,
    MD_CAP_PERCUSSION = 1u << 2, MD_CAP_VELOCITY = 1u << 3,
    MD_CAP_ACCENT = 1u << 4, MD_CAP_LEGATO = 1u << 5,
    MD_CAP_SLIDE = 1u << 6, MD_CAP_PRESSURE = 1u << 7,
    MD_CAP_BEND = 1u << 8
};
typedef struct {
    uint32_t id; /* stable preset ID, not its display index */
    const char *name;
    const uint8_t *settings;
    uint16_t settings_bytes;
} md_preset_t;
typedef struct {
    uint16_t id; /* stable kit ID, 0..13 in v1 drum masks */
    const char *name;
    uint8_t midi_note; /* 0..127, or 255 when no note mapping exists */
} md_drum_voice_desc_t;
typedef struct md_instrument_desc {
    md_processor_desc_t processor;
    uint32_t capabilities;
    uint16_t max_polyphony, preset_count;
    const md_preset_t *presets;
    uint16_t drum_voice_count;
    const md_drum_voice_desc_t *drum_voices;
    int (*init)(void *state, uint32_t sample_rate);
    int (*event)(void *state, const md_event_t *event);
    /* Required when param_count>0; pure read of current native control value. */
    int (*get_param)(const void *state, md_param_id_t id, int32_t *value);
    /* Add into caller buffer, interleaved stereo Q15 in int32_t; never clear it. */
    void (*render_add)(void *state, int32_t *stereo, uint32_t frames);
    void (*reset)(void *state); /* cut notes/history, retain sound settings */
    int (*encode)(const void *state, uint8_t *dst, uint32_t capacity);
    int (*validate)(const uint8_t *src, uint32_t bytes); /* no mutation */
    int (*apply)(void *state, const uint8_t *src, uint32_t bytes);
} md_instrument_desc_t;
typedef struct {
    const md_instrument_desc_t *desc;
    void *state; /* caller-owned, aligned, stable until instance is released */
    uint32_t sample_rate;
} md_instrument_t;
typedef struct {
    const md_instrument_desc_t *const *items;
    uint16_t count;
} md_instrument_registry_t;

/* Explicit registry context keeps tests/new hosts independent of global arrays.
 * Unknown ID returns NULL; validation rejects duplicate IDs and invalid schemas. */
int md_instrument_registry_validate(const md_instrument_registry_t *);
const md_instrument_desc_t *md_instrument_find(const md_instrument_registry_t *, uint32_t id);
int md_instrument_init(md_instrument_t *, const md_instrument_desc_t *,
                       void *state, uint32_t state_bytes, uint32_t sample_rate);
int md_instrument_event(md_instrument_t *, const md_event_t *);
int md_instrument_param_get(const md_instrument_t *, md_param_id_t id, int32_t *value);
void md_instrument_render_add(md_instrument_t *, int32_t *stereo, uint32_t frames);
void md_instrument_reset(md_instrument_t *);
int md_sound_encode(const md_instrument_t *, uint8_t *dst, uint32_t capacity);
int md_sound_validate(const md_instrument_desc_t *, uint16_t settings_version,
                      const uint8_t *src, uint32_t bytes);
int md_sound_apply(md_instrument_t *, uint16_t settings_version,
                   const uint8_t *src, uint32_t bytes);

/* Initialization validates descriptor, capacity, rate and power-of-two alignment
 * before calling init. Failure leaves the handle uninitialized; init must not
 * allocate. validate must check the complete settings payload before apply; apply
 * cannot fail after successful validation for the same descriptor/payload. Invalid
 * event/range/unsupported capability errors do not mutate processor state.
 * Render is valid for frames 1..256 only. No callback may allocate, wait, call a
 * host function or hardware register. settings encode is stopped/non-audio work;
 * validated bounded apply occurs at a stopped/audio-safe handoff, never per sample.
 * Registry descriptors and preset bytes are immutable and outlive all instances.
 * Runtime wrappers do not perform logical source arbitration: use the router.
 * Parameter readback runs only in the audio/stopped owner context. The wrapper
 * commits output only after successful in-range callback return; readonly values
 * are readable. UI reads published model snapshots, never live DSP. Readback is
 * CURRENT/effective state, not a saved base value underneath an active lock. */
#endif
