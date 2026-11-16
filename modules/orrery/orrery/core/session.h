/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_SESSION_H
#define MD_SESSION_H
#include "commands.h"
#include "mixer.h"
#include "router.h"
#include "module/seq.h"

/* The audio session: one owner of live engine state, transport, sequencer, note
 * ownership and mix. PLAN.md "Audio ownership and event scheduling".
 *
 * Contexts. MAIN functions run in the producer context (fm1-dock main loop: init, ui,
 * note, cc, midi, draw, data). AUDIO functions run in render(). Nothing else may touch
 * the session. Main never writes live state: it queues commands, stages complete edits
 * in buffers audio is not reading, and reads a published snapshot.
 *
 * Block order. render() first takes the emergency-flush requests visible at its start,
 * then drains the commands queued at its start (FIFO, all at offset 0, at most the ring
 * capacity), then applies those flushes, so a flush always follows every command queued
 * before it. Then it iterates the sequencer: tracks render up to each event's offset,
 * the event is applied, rendering continues. Sequencer order at one offset is
 * restore, lock, release, trigger, legato cleanup (module/seq.h). Finally the mixer
 * runs and the transport advances.
 *
 * Parameter targets use decision 002's namespaces: 0x0 instrument, 0x1/0x2 inserts,
 * 0x3 mixer track, 0x4 echo, 0x5 reverb, 0x6 master; others are rejected. Panel/UI
 * parameter commands set the base value and the DSP; sequencer locks set only the DSP,
 * and restores read the base, so a lock never overwrites what the user set.
 *
 * Parallel. With config.parallel and a parallel_track (or several: parallel_mask), a block is routed first and
 * played afterwards. Routed: the sequencer's events of the whole block
 * go through the router on this core with nothing rendered, and what the router sends to an instrument (an event,
 * a reset) is recorded with its offset. Played: the second core takes the recorded operations of its tracks, each
 * between that track's render segments, while this core does the same for the others, both from the block's first
 * frame, and each adds its finished tracks to the mixer on a lane of its own; the buses and the sum follow. The
 * router is done before either plays, so it is never touched by two cores.
 * (Until 2026-11 this core rendered its tracks up to each of their events before the job started: with an event
 * late in a block the two cores ran one after the other.) A full record is played here at once and the routing
 * goes on (parallel_fallbacks counts those blocks). An instrument that refuses a recorded event cannot tell the
 * router any more: a hit over capacity is counted and dropped as before; anything else is a fault, and the track
 * is panicked at the next block's start. A job that times out is not redone (it is not idempotent): the held core
 * may have stopped anywhere in it, so the session panics those tracks on this core (their notes, sustain and
 * engine state cleared; that block's part may be cut short), stops using the second core and counts it.
 * Instruments whose engine has mutable static state must all render on the same core (the Eminent's scratch
 * buffer, the Synthex's render lane): never one on each core at once. Which core plays a track changes nothing
 * that is heard, so with config.parallel_balance the session shares the tracks out itself, block by block, by
 * the time each took (measured as it plays, quick to rise and slow to fall): a part that grows dear, a pad's
 * long chord or a burst from the effects box, does not find the other core idle. With no second core nothing is recorded:
 * each event is applied as it is routed, as ever.
 *
 * Bindings. A track may name an alternate instrument at init; MD_CMD_BIND switches between the two at a
 * block start, after a panic of that track (its notes released, the outgoing instrument reset), and reads the
 * incoming instrument's parameters as the track's new bases. Both instances stay owned by the caller.
 *
 * Releases. A quarter of the ring (MD_RELEASE_RESERVE) is kept for note-offs, source
 * releases, panic, stop and sustain-up. If even a release does not fit, the session
 * raises a coalesced emergency flush for that source, which releases all of that
 * source's notes (and its sustain) at the next block. A refused stop or global panic
 * raises a coalesced stop or panic request the same way. A note-off is therefore never
 * silently lost, and the caller need not retry a release. */

#define MD_SESSION_MAX_PARAMS 48u
#define MD_SESSION_SOUND_BYTES 512u
#define MD_SESSION_SLOTS (MD_TRACKS + MD_TRACKS * MD_MIX_INSERTS + MD_MIX_BUSES + 1u)
#define MD_LIVE_TOKEN_BASE 0x80000000u /* main-allocated live tokens; router tokens stay below */
#define MD_BYPASS_MASTER 0xffu

/* Rendering one track on a second core (the host's c1_run / c1_wait, from render only). run hands fn(arg) to
 * the core and returns 1, or 0 when it cannot (then the session does the job itself); wait returns 0 when
 * the job is done and -1 when it did not end in time (then the session stops using the core). */
typedef struct {
    int (*run)(void (*fn)(uint32_t), uint32_t arg);
    int (*wait)(void);
    uint32_t (*ticks)(void);   /* optional: a free-running counter (any unit, it may wrap), for parallel_balance */
} md_parallel_t;
#define MD_PARALLEL_EVENTS 64u

typedef struct {
    md_instrument_t *instrument[MD_TRACKS];               /* initialised, or NULL */
    md_parallel_t parallel;                               /* optional: {NULL, NULL} renders all on one core */
    uint8_t parallel_track;                               /* the track the second core renders; 0xff none */
    uint8_t parallel_queue;                               /* operations recorded before they are played here; 0 the most */
    uint8_t parallel_mask;                                /* several tracks on the second core (bit t); 0: the one
                                                           * parallel_track */
    uint8_t parallel_balance;                             /* 1: with parallel.ticks, the tracks are shared out anew
                                                           * every block by what each took lately (the dearest
                                                           * first, each to the core with less so far);
                                                           * parallel_mask is where they start. Only when any
                                                           * track may go to either core: no two instances of an
                                                           * engine with static state among them. */
    md_instrument_t *alternate[MD_TRACKS];                /* optional second instrument a track can switch to */
    md_effect_t *insert[MD_TRACKS][MD_MIX_INSERTS];        /* initialised, or NULL */
    md_effect_t *bus[MD_MIX_BUSES];
    md_effect_t *master;
    const md_seq_bank_t *bank;                             /* copied into the session */
    uint16_t bpm_q8;
} md_session_config_t;

typedef struct {
    md_position_t position;
    uint32_t samples;
    uint16_t bpm_q8;
    uint8_t running, mute_mask, solo_mask;
    uint8_t pattern[MD_TRACKS], cursor[MD_TRACKS], queued[MD_TRACKS], requested[MD_TRACKS];
    uint32_t blocks;
    uint32_t commands, command_errors;     /* applied / refused by audio */
    int32_t last_error;                    /* most recent negative status seen by audio */
    uint32_t events, event_errors;         /* sequencer events dispatched / refused */
    uint32_t max_block_events, max_block_commands;
    uint32_t flushes;                      /* emergency source flushes applied */
    uint32_t parallel_blocks, parallel_inline, parallel_fallbacks;   /* jobs on core 1, done here, record full */
    uint8_t parallel_failed;               /* a job timed out: the second core is no longer used */
    uint32_t seq_faults, seq_dropped, seq_base_errors;
    uint32_t owned_notes, max_owned_notes;
    uint32_t rejected_notes, backend_errors;   /* router */
    md_mixer_meters_t meters;
} md_session_snapshot_t;

typedef struct {
    /* shared words: one writer each */
    md_command_ring_t ring;
    /* Coalesced requests: main raises one (request = seen + 1) only when none is
     * pending; audio completes it (seen = request). One writer per array. */
    volatile uint8_t flush_request[MD_MAX_SOURCES];    /* main */
    volatile uint8_t flush_seen[MD_MAX_SOURCES];       /* audio */
    volatile uint8_t control_request[2], control_seen[2]; /* stop, panic: main / audio */
    volatile uint32_t bank_ack, sound_ack;             /* audio writes generations */
    volatile uint32_t snapshot_sequence;               /* audio: odd while writing */
    md_session_snapshot_t snapshot;

    /* staging: main writes while not published, audio reads only after the command */
    md_seq_bank_t bank[2];
    uint8_t sound[MD_SESSION_SOUND_BYTES];
    uint16_t sound_bytes, sound_version;

    /* main only */
    uint32_t bank_generation, sound_generation;
    md_note_token_t live_token;
    uint8_t main_live, staging_open, staged;

    /* audio only (and init) */
    md_instrument_t *instrument[MD_TRACKS];
    md_instrument_t *choice[MD_TRACKS][2];  /* a track's instrument and its alternate */
    md_effect_t *insert[MD_TRACKS][MD_MIX_INSERTS];
    md_effect_t *bus[MD_MIX_BUSES];
    md_effect_t *master;
    md_router_t router;
    md_transport_t clock;
    md_seq_t seq;
    md_mixer_t mixer;
    int32_t base[MD_SESSION_SLOTS][MD_SESSION_MAX_PARAMS];
    int32_t mix_base[MD_TRACKS][MD_MIX_TRACK_PARAM_COUNT];
    uint8_t mute_mask, solo_mask;
    md_session_snapshot_t stats;           /* audio's working copy of the counters */
    md_parallel_t parallel;
    uint8_t parallel_track, par_limit, par_mask;
    uint8_t par_now, par_flushed, fault_mask;             /* this block's tracks for core 1; its record was full;
                                                           * tracks to panic at the next block's start */
    uint32_t par_count, par_frames, par_offset;           /* the record, the block, the event being routed */
    uint32_t cursor[MD_TRACKS];                           /* frames rendered of each track in this block */
    struct { uint32_t offset; uint8_t track, reset; md_event_t event; } par_queue[MD_PARALLEL_EVENTS];
    struct { uint32_t errors, rejected; int32_t last; uint8_t fault; } par_result;   /* the job's, read after it */
    uint32_t played_rejected;                             /* hits over capacity found when played */
    uint8_t par_balance;
    uint32_t track_cost[MD_TRACKS];                       /* ticks a block, lately (each written by the core that
                                                           * played the track) */
    int32_t track_buffer[MD_TRACKS][2u * MD_MAX_RENDER_FRAMES];
} md_session_t;

/* MAIN, before audio starts. Instances must stay valid for the session's lifetime. */
int md_session_init(md_session_t *, const md_session_config_t *);

/* MAIN. Queue any command. Releases may use the reserve. A refused note-off, source
 * release or sustain-up raises an emergency flush of its source; a refused stop or
 * global panic raises a stop or panic request. All still return MD_ERR_CAPACITY. */
int md_session_submit(md_session_t *, const md_command_t *);
/* MAIN. Live note helpers. note_on returns the token the caller keeps for its key-up. */
int md_session_note_on(md_session_t *, uint8_t track, md_source_id_t source, uint8_t key,
                       uint8_t velocity, uint16_t flags, md_note_token_t *token);
int md_session_note_off(md_session_t *, uint8_t track, md_source_id_t source, md_note_token_t token);
int md_session_param(md_session_t *, uint8_t track, uint16_t target, int32_t value);

/* MAIN. Edits. edit_bank returns a staging copy of the live bank, or NULL while a
 * previous publish is unacknowledged. publish validates it and queues the swap; the
 * old live bank becomes editable only after audio acknowledges. stage_sound validates
 * a sound payload for the track's instrument and queues it (one in flight). */
md_seq_bank_t *md_session_edit_bank(md_session_t *);
int md_session_publish_bank(md_session_t *);
int md_session_stage_sound(md_session_t *, uint8_t track, uint16_t version, const uint8_t *src,
                           uint32_t bytes);
/* MAIN. Bits: 1 bank swap pending, 2 sound pending. */
uint32_t md_session_pending(md_session_t *);
/* MAIN. Copy a consistent snapshot; MD_ERR_STATE if audio kept rewriting it. */
int md_session_snapshot(const md_session_t *, md_session_snapshot_t *out);

/* AUDIO. Add frames 1..256 of the mix into out. */
int md_session_render(md_session_t *, int32_t *out, uint32_t frames);

#ifdef MD_SESSION_PROFILE
/* Measurement builds only (tests/stress): stage times of md_session_render by a caller-supplied counter.
 * Stages: 0 flushes and command drain, 1 clearing the track buffers, 2 sequencer events and instrument
 * rendering, 3 mixer and buses, 4 transport and snapshot. [stage][0] sum, [stage][1] worst. */
enum { MD_PROF_DRAIN, MD_PROF_CLEAR, MD_PROF_EVENTS, MD_PROF_MIX, MD_PROF_PUBLISH, MD_PROF_ROUTE, MD_PROF_STAGES };   /* (ROUTE: a
 * parallel block's routing, taken out of EVENTS) */
extern uint32_t (*md_session_ticks)(void);
extern uint32_t md_session_prof[MD_PROF_STAGES][2];
#endif
#endif
