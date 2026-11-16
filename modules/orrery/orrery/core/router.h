/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_ROUTER_H
#define MD_ROUTER_H
#include "instrument.h"

typedef struct {
    uint64_t order;
    md_note_token_t token;
    md_source_id_t source;
    uint16_t flags;
    uint8_t used, track, key, velocity, released;
} md_owned_note_t;
typedef struct {
    md_note_token_t token;
    md_source_id_t source;
    uint8_t used, track, key;
} md_router_voice_t;
/* Where the router's calls to an instrument go, when set: the event for the track's instrument (NULL: its reset),
 * taken as done. The session records them while it routes a block and plays them afterwards, each track's in
 * order between its render segments (so a second core can play some tracks: the router is done by then). */
typedef int (*md_router_sink_t)(void *context, uint8_t track, const md_event_t *event);
typedef struct {
    md_instrument_t *track[MD_TRACKS];
    md_owned_note_t notes[MD_MAX_OWNED_NOTES];
    md_router_voice_t voices[MD_MAX_OWNED_NOTES];
    uint8_t sustain[MD_TRACKS][MD_MAX_SOURCES];
    uint64_t order;
    md_note_token_t next_token;
    uint32_t rejected_notes, backend_errors; /* rejected_notes includes transient HIT admission */
    md_router_sink_t sink;                   /* NULL (init): the instruments are called directly */
    void *sink_context;
} md_router_t;

/* NULL track slots are valid unbound destinations. Init rejects aliased engine
 * state/bindings. Instances must already be initialized and outlive the router.
 * Calls occur in one serialized/audio context. Re-init is only safe after panic. */
int md_router_init(md_router_t *, md_instrument_t *const tracks[MD_TRACKS]);
int md_router_event(md_router_t *, const md_event_t *);
int md_router_sustain(md_router_t *, uint8_t track, md_source_id_t source, uint32_t down);
md_note_token_t md_router_next_token(md_router_t *);
uint32_t md_router_owned_count(const md_router_t *);
/* Capacity/bad input is rejected without changing notes or DSP. A backend failure
 * after validated admission is a processor fault: reset/flush that track, count
 * the fault and return ERR_STATE. Exception: nonmutating HIT/CAPACITY is normal
 * hybrid-voice admission failure; count/drop that hit and preserve held owners. Effects are outside this router: session panic
 * must also reset mixer/effect histories. */
#endif
