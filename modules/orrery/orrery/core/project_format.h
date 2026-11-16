/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_PROJECT_FORMAT_H
#define MD_PROJECT_FORMAT_H
#include "param.h"

#define MD_PROJECT_VERSION 1u
#define MD_PATTERNS_PER_TRACK 4u
#define MD_STEPS_PER_PATTERN 64u
#define MD_MAX_SECTIONS 32u
#define MD_MAX_LOCKS 48u
#define MD_PROJECT_HEADER_BYTES 128u
#define MD_PROJECT_TRACK_BYTES 32u
#define MD_PROJECT_PATTERN_BYTES 8u
#define MD_PROJECT_STEP_BYTES 6u
#define MD_PROJECT_SECTION_BYTES 16u
#define MD_PROJECT_LOCK_BYTES 6u
#define MD_PROJECT_SOUND_BYTES 512u
#define MD_PROJECT_EFFECT_BYTES 256u
#define MD_PROJECT_BANK_LIMIT 8176u

enum {
    MD_PROJECT_TRACK_OFFSET = MD_PROJECT_HEADER_BYTES,
    MD_PROJECT_PATTERN_OFFSET = MD_PROJECT_TRACK_OFFSET + MD_PROJECT_TRACKS * MD_PROJECT_TRACK_BYTES,
    MD_PROJECT_STEP_OFFSET = MD_PROJECT_PATTERN_OFFSET + MD_PROJECT_TRACKS * MD_PATTERNS_PER_TRACK * MD_PROJECT_PATTERN_BYTES,
    MD_PROJECT_SECTION_OFFSET = MD_PROJECT_STEP_OFFSET + MD_PROJECT_TRACKS * MD_PATTERNS_PER_TRACK * MD_STEPS_PER_PATTERN * MD_PROJECT_STEP_BYTES,
    MD_PROJECT_LOCK_OFFSET = MD_PROJECT_SECTION_OFFSET + MD_MAX_SECTIONS * MD_PROJECT_SECTION_BYTES,
    MD_PROJECT_SOUND_OFFSET = MD_PROJECT_LOCK_OFFSET + MD_MAX_LOCKS * MD_PROJECT_LOCK_BYTES,
    MD_PROJECT_EFFECT_OFFSET = MD_PROJECT_SOUND_OFFSET + MD_PROJECT_SOUND_BYTES,
    MD_PROJECT_BYTES = MD_PROJECT_EFFECT_OFFSET + MD_PROJECT_EFFECT_BYTES
};
_Static_assert(MD_PROJECT_BYTES == 8096u, "v1 wire budget changed: review schema/plan");
_Static_assert(MD_PROJECT_BYTES <= MD_PROJECT_BANK_LIMIT, "project exceeds primary bank");

enum { MD_PATTERN_MONO, MD_PATTERN_CHORD, MD_PATTERN_DRUM };
enum { MD_DIV_32ND, MD_DIV_16TH, MD_DIV_8TH, MD_DIV_QUARTER, MD_DIV_HALF, MD_DIV_BAR };
enum {
    MD_STEP_TIE = 1u << 0, MD_STEP_ACCENT = 1u << 1, MD_STEP_SLIDE = 1u << 2,
    MD_STEP_FLAGS = MD_STEP_TIE | MD_STEP_ACCENT | MD_STEP_SLIDE
};
enum {
    MD_CHORD_MAJOR, MD_CHORD_MINOR, MD_CHORD_MAJOR7, MD_CHORD_MINOR7,
    MD_CHORD_SUS2, MD_CHORD_SUS4, MD_CHORD_FIFTH, MD_CHORD_COUNT
};
/* a chord step's voicing: tone i (1..3 of the quality's four) an octave up; the root never moves */
#define MD_CHORD_RAISE(i) ((uint16_t)(1u << (10u + (i))))
#define MD_PATTERN_HOLD 0xfeu
#define MD_PATTERN_REST 0xffu
#define MD_AUTOMATION_UNOWNED 0xffu
#define MD_DRUM_VOICES 14u

/* Logical model fields only: explicit byte encoders follow in M5-01. Never memcpy
 * these runtime structs into project storage, even when sizeof happens to match. */
typedef struct {
    uint16_t payload; /* mono: pitch 0..127; chord: pitch | quality<<8 | MD_CHORD_RAISE bits; drum: 14-hit mask */
    uint8_t velocity; /* 1..127 for sounding steps */
    uint8_t gate; /* 0 rest, otherwise 1..200 percent of unswung duration */
    uint8_t flags, probability; /* probability 0..100 */
} md_step_t;
typedef struct {
    uint8_t length, division, swing; /* 1..64, MD_DIV_*, 50..75 */
    int8_t transpose; /* -48..48, pitched modes only */
    uint8_t mode, flags; /* flags zero in v1 */
    uint16_t reserved;
} md_pattern_meta_t;
typedef struct {
    uint8_t pattern[MD_PROJECT_TRACKS]; /* v1 sections address the first four tracks */
    uint8_t bars; /* 1..64 */
    int8_t transpose; /* -48..48 */
    uint8_t mute_mask, flags; /* low four bits, flags zero */
    uint8_t reserved[8];
} md_section_t;
typedef struct {
    uint16_t step; /* ((track*4 + pattern)*64 + index), 0..MD_TRACKS*256-1 */
    uint16_t target; /* session namespace | local parameter ID */
    int16_t value;
} md_lock_t;
_Static_assert(sizeof(md_step_t) == MD_PROJECT_STEP_BYTES, "unexpected step model padding");
_Static_assert(sizeof(md_pattern_meta_t) == MD_PROJECT_PATTERN_BYTES, "unexpected pattern padding");
_Static_assert(sizeof(md_section_t) == MD_PROJECT_SECTION_BYTES, "unexpected section padding");
_Static_assert(sizeof(md_lock_t) == MD_PROJECT_LOCK_BYTES, "unexpected lock padding");

/* Header/track/directory offsets, reserved-byte rules, checksums and sound-pool
 * ownership are specified in decision 002; fields are not a C packing ABI. */
#endif
