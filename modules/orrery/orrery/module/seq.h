/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_SEQ_H
#define MD_SEQ_H
#include "core/event.h"
#include "core/transport.h"

typedef struct {md_pattern_meta_t meta;md_step_t step[MD_STEPS_PER_PATTERN];} md_seq_pattern_t;
typedef struct {
    md_seq_pattern_t pattern[MD_TRACKS][MD_PATTERNS_PER_TRACK];
    md_lock_t locks[MD_MAX_LOCKS];
    uint32_t seed;
    uint8_t lock_count, global_owner[3];
} md_seq_bank_t;
typedef struct {
    void *context;
    md_note_token_t (*token)(void *context); /* serialized router allocator, no host/allocation */
    int (*base_value)(void *context,uint8_t track,uint16_t target,int32_t *value);
} md_seq_callbacks_t;
enum {
    MD_SEQ_RESTORE, MD_SEQ_LOCK, MD_SEQ_RELEASE, MD_SEQ_TRIGGER, MD_SEQ_LEGATO_CLEANUP
};
typedef struct {md_timed_event_t timed;uint8_t priority;} md_seq_event_t;
typedef struct {uint16_t target;uint8_t active;int32_t base;} md_seq_lock_state_t;
typedef struct {
    md_position_t next, launch_at;
    uint64_t visits;
    md_note_token_t last[4];
    md_seq_lock_state_t lock;
    uint8_t last_count, pattern, cursor, muted, queued, requested;
} md_seq_track_t;
typedef struct {
    md_position_t off;
    md_note_token_t token;
    uint8_t used,track,cleanup;
} md_seq_note_t;
/* At one timestamp: <=128 offs, <=56 triggers, <=8 locks, <=12 source flushes. */
#define MD_SEQ_STEP_LIMIT (MD_TRACKS * 4u) /* normal block <=8 visits incl launches */
#define MD_SEQ_BATCH_MAX (MD_MAX_OWNED_NOTES + MD_TRACKS*(MD_DRUM_VOICES+5u))
typedef struct {
    const md_seq_bank_t *bank; /* immutable during an iteration; never retained from a temporary */
    md_seq_callbacks_t callbacks;
    md_seq_track_t track[MD_TRACKS];
    md_seq_note_t notes[MD_MAX_OWNED_NOTES];
    md_transport_t clock; /* block-start snapshot, advanced by session not sequencer */
    md_event_t batch[MD_SEQ_BATCH_MAX];
    uint8_t priority[MD_SEQ_BATCH_MAX];
    uint16_t order[MD_SEQ_BATCH_MAX];
    uint32_t offset,frames,batch_count,batch_index,emitted,visited;
    uint32_t dropped_notes,base_errors,overflows;
    uint8_t running,flush_mask,iterating;
} md_seq_t;

int md_seq_bank_validate(const md_seq_bank_t *);
int md_seq_init(md_seq_t *,const md_seq_bank_t *,md_seq_callbacks_t);
int md_seq_start(md_seq_t *,md_position_t position);
int md_seq_stop(md_seq_t *);
int md_seq_continue(md_seq_t *);
int md_seq_mute(md_seq_t *,uint8_t track,uint32_t mute);
/* Quantized: next strictly-future global4/4bar. Immediate: next hundredth-tick
 * boundary (<=7 samples at40BPM), avoiding render-block quantization.
 * Stopped selection changes immediately without a queued launch. */
int md_seq_launch(md_seq_t *,const md_transport_t *,uint8_t track,uint8_t pattern,uint32_t immediate);
int md_seq_begin(md_seq_t *,const md_transport_t *,uint32_t frames);
/* Returns1/event,0/drained,negative/error. Caller drains before advancing clock
 * or editing state. Merge by offset then priority, preserving returned order for
 * ties. LEGATO cleanup deliberately follows its new NOTE_ON at the same sample.
 * On unexpected iterator error the session must release all four seq sources.
 * No instrument/host calls, allocation, drawing or project storage in this unit. */
int md_seq_next(md_seq_t *,md_seq_event_t *);
#endif
