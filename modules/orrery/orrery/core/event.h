/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_EVENT_H
#define MD_EVENT_H
#include "param.h"

typedef uint16_t md_source_id_t;
typedef uint32_t md_note_token_t;
#define MD_TOKEN_NONE 0u
#define MD_SOURCE_NONE 0xffffu
/* NONE is legal only for PARAM/PANIC from non-note UI/system operations. */
#define MD_ALL_TRACKS 0xffu
#define MD_SOURCE_PANEL 7u /* after the sequencer sources 0..MD_TRACKS-1 */
#define MD_SOURCE_MIDI_BASE 8u /* base + port*16 + channel, for up to three ports */
/* Sequencer sources are track indices 0..MD_TRACKS-1; up to 6 and 56..63 reserved. */

typedef enum {
    MD_EVENT_NOTE_ON = 1, MD_EVENT_NOTE_OFF, MD_EVENT_HIT,
    MD_EVENT_PARAM, MD_EVENT_BEND, MD_EVENT_PRESSURE,
    MD_EVENT_SOURCE_RELEASE, MD_EVENT_PANIC
} md_event_kind_t;
enum {
    MD_NOTE_ACCENT = 1u << 0,
    MD_NOTE_LEGATO = 1u << 1,
    MD_NOTE_SLIDE = 1u << 2
};
typedef struct {
    md_event_kind_t kind;
    uint8_t track; /* 0..3; ALL_TRACKS only for source release/panic */
    uint8_t reserved;
    uint16_t flags;
    md_source_id_t source;
    md_note_token_t token;
    union {
        struct { uint8_t key, velocity; } note; /* note-on velocity 1..127; off uses token */
        struct { uint16_t voice; uint8_t velocity; } hit; /* one-shot; token may be zero */
        struct { md_param_id_t id; int32_t value; } param;
        int32_t value; /* bend: signed Q16 semitones +/-48; pressure: 0..127 */
    } data;
} md_event_t;
typedef struct {
    uint32_t offset; /* 0..n-1 in current block; n is deferred to next block */
    md_event_t event;
} md_timed_event_t;

/* Logical note-on tokens are nonzero and unique among all outstanding notes.
 * Duplicate token admission fails without mutation; allocation skips live tokens
 * on wrap. The router resolves sources/mono priority/duplicate pitches BEFORE
 * processor callbacks. Adapters receive resolved voice events, not logical key
 * ownership. NOTE_OFF's token identifies the existing voice; callers initialize
 * unused data to zero. Stale logical offs are successful no-ops in the router.
 * SOURCE_RELEASE is router-only; panic resets instruments AND effect history.
 * Panel/MIDI note-on velocity zero must be converted into a logical note-off by
 * the input mapper, which retains key-to-token mappings across routing changes.
 * Events have no pointers and are not serialized as raw runtime structs. */
#endif
