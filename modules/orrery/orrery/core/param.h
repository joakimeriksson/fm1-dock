/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_PARAM_H
#define MD_PARAM_H
#include <stdint.h>
#include "hot.h"

#define MD_API_VERSION 2u /* source contract: current parameter readback added */
#define MD_SAMPLE_RATE 44100u
#define MD_MAX_RENDER_FRAMES 256u
#define MD_TRACKS 5u /* runtime tracks (sequencer sources 0..4); see MD_PROJECT_TRACKS for the v1 wire layout */
#define MD_PROJECT_TRACKS 4u /* tracks the v1 project wire layout holds: six need a schema decision (M5-01) */
#define MD_ALL_TRACK_MASK ((1u << MD_TRACKS) - 1u)
#define MD_MAX_SOURCES 64u
#define MD_MAX_OWNED_NOTES 128u
#define MD_COMMAND_CAPACITY 128u
#define MD_RELEASE_RESERVE 32u
#define MD_BLOCK_EVENT_LIMIT 512u

/* Success is zero; positive byte counts are returned only by encode operations. */
typedef enum {
    MD_OK = 0,
    MD_ERR_ARGUMENT = -1,
    MD_ERR_UNSUPPORTED = -2,
    MD_ERR_RANGE = -3,
    MD_ERR_CAPACITY = -4,
    MD_ERR_VERSION = -5,
    MD_ERR_FORMAT = -6,
    MD_ERR_STATE = -7,
    MD_ERR_NOT_FOUND = -8
} md_status_t;

typedef uint16_t md_param_id_t;
#define MD_PARAM_ID_MAX 0x0fffu /* zero invalid; upper nibble reserved for session target namespace */

enum {
    MD_PARAM_AUTOMATABLE = 1u << 0,
    MD_PARAM_SMOOTH = 1u << 1,
    MD_PARAM_ENUM = 1u << 2,
    MD_PARAM_READ_ONLY = 1u << 3
};
typedef enum {
    MD_UNIT_INTEGER, MD_UNIT_PERCENT, MD_UNIT_SEMITONES, MD_UNIT_CENTS,
    MD_UNIT_MS, MD_UNIT_HZ, MD_UNIT_BPM, MD_UNIT_BOOLEAN, MD_UNIT_Q15
} md_param_unit_t;
typedef struct {
    md_param_id_t id;
    uint16_t flags;
    const char *name;
    int32_t min, max, initial;
    md_param_unit_t unit;
    /* NULL/0 unless ENUM; contiguous integer values index labels from min. */
    const char *const *labels;
    uint16_t label_count;
} md_param_desc_t;

/* These are runtime descriptors, never a disk format. Native integer units are
 * authoritative. Logarithmic/time mappings use an adapter's documented integer
 * conversion and display labels; the UI must not assume linear Hertz spacing. */
typedef struct {
    uint32_t id; /* stable nonzero processor/instrument identity; not registry index */
    uint16_t api_version, settings_version;
    const char *name;
    uint32_t state_bytes, state_alignment;
    const uint32_t *sample_rates;
    uint16_t sample_rate_count;
    uint16_t param_count;
    const md_param_desc_t *params;
    uint16_t max_settings_bytes; /* raw settings payload, excludes project directory */
} md_processor_desc_t;

/* Namespace is session routing, not part of an individual descriptor's local ID.
 * Global targets have exactly one owning sequencer track per project. */
enum {
    MD_TARGET_INSTRUMENT = 0x0000u,
    MD_TARGET_INSERT_1 = 0x1000u,
    MD_TARGET_INSERT_2 = 0x2000u,
    MD_TARGET_MIXER = 0x3000u,
    MD_TARGET_ECHO = 0x4000u,
    MD_TARGET_REVERB = 0x5000u,
    MD_TARGET_MASTER_FILTER = 0x6000u,
    MD_TARGET_GROUP_MASK = 0xf000u,
    MD_TARGET_LOCAL_MASK = 0x0fffu
};
enum { MD_MIX_LEVEL = 1, MD_MIX_PAN, MD_MIX_ECHO_SEND, MD_MIX_REVERB_SEND };
#endif
