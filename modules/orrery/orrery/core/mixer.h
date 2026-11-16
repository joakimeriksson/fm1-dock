/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_MIXER_H
#define MD_MIXER_H
#include "effect.h"

/* Fixed v1 graph (PLAN.md "Mixer and effects", decision 002):
 *
 *   track input -> insert 1 -> insert 2 -> level/pan/mute -> dry mix
 *                                              |-> echo send   -> echo bus   -> return --|
 *                                              |-> reverb send -> reverb bus -> return --|
 *   dry mix + returns -> master insert (bypassed by default) -> saturate -> output (added)
 *
 * Sends are stereo, post-insert and post-level/pan/mute. Returns never feed a send,
 * so there is no bus feedback. Bus processors produce wet-only output and the mixer
 * owns every gain, so the dry signal reaches the output exactly once.
 *
 * Gains are Q23 inside and change through a per-sample one-pole smoother, so output
 * is identical for every render partition. Pan is a balance law: centre is unity on
 * both sides and moving away attenuates the opposite side linearly to zero.
 *
 * Every internal signal is clamped to +-MD_MIX_LIMIT (+48 dB over Q15 full scale) at
 * each stage boundary, and products use 64-bit multiplies (no division), so the
 * worst input and any processor output cannot wrap. The output is saturated to Q15
 * and counted as a clip before it is added to the caller's buffer; the caller keeps
 * that buffer within int32 headroom, as for instrument render_add. Q15 32767 means
 * exact unity for levels, sends and returns.
 *
 * Effect instances are caller-owned and initialised. One instance may occupy one
 * slot only. Configuration calls belong to the audio owner and are rejected inside a
 * span (between begin and finish). A slot whose bypass ramp has reached zero stops
 * processing, and it is reset before it is heard again, so stale history never
 * returns. Replacing an audible insert cuts the old one: bypass it and wait for
 * md_mixer_insert_idle before swapping to avoid a click. */

#define MD_MIX_INSERTS 2u
#define MD_MIX_LIMIT (1 << 24)
#define MD_MIX_UNITY_Q23 (1 << 23)
#define MD_MIX_SMOOTH_SHIFT 8u /* one-pole time constant 256 samples, about 5.8 ms */

enum { MD_MIX_BUS_ECHO, MD_MIX_BUS_REVERB, MD_MIX_BUSES };
#define MD_MIX_LANES 2u /* sums of their own, so two cores can add tracks at once (md_mixer_add_track_lane) */

typedef struct {
    md_effect_t *effect;
    int32_t wet, wet_target; /* Q23 crossfade, 0 = dry */
    uint8_t bypass, active;
} md_mixer_slot_t;

typedef struct {
    md_mixer_slot_t insert[MD_MIX_INSERTS];
    int32_t level, pan, send[MD_MIX_BUSES]; /* native parameter values */
    int32_t gain[4], gain_target[4];        /* Q23: left, right, echo send, reverb send */
    uint8_t mute, solo, added;
} md_mixer_track_t;

typedef struct {
    int32_t track_peak[MD_TRACKS];    /* post-fader, absolute, internal scale */
    int32_t return_peak[MD_MIX_BUSES];
    int32_t master_peak;              /* before output saturation */
    uint32_t clipped;                 /* output samples saturated to Q15 */
    uint32_t clamped;                 /* internal samples held at +-MD_MIX_LIMIT */
} md_mixer_meters_t;

typedef struct {
    md_mixer_track_t track[MD_TRACKS];
    md_effect_t *bus[MD_MIX_BUSES];
    int32_t return_level[MD_MIX_BUSES];
    int32_t return_gain[MD_MIX_BUSES], return_target[MD_MIX_BUSES];
    md_mixer_slot_t master;
    md_mixer_meters_t meters;
    uint32_t frames;
    uint16_t tempo_q8;
    uint8_t in_span, any_solo;
    int32_t dry[2u * MD_MAX_RENDER_FRAMES];
    int32_t send[MD_MIX_BUSES][2u * MD_MAX_RENDER_FRAMES];
    int32_t wet[2u * MD_MAX_RENDER_FRAMES];
    int32_t xfade[2u * MD_MAX_RENDER_FRAMES];
    int32_t silent[2u * MD_MAX_RENDER_FRAMES];
    /* the second lane: its own sums, crossfade scratch and clamp count, joined to the first in finish */
    int32_t dry1[2u * MD_MAX_RENDER_FRAMES];
    int32_t send1[MD_MIX_BUSES][2u * MD_MAX_RENDER_FRAMES];
    int32_t xfade1[2u * MD_MAX_RENDER_FRAMES];
    uint32_t clamped1;
    uint8_t lane1;                    /* something was added on it in this span */
} md_mixer_t;

/* Track parameters use the MD_TARGET_MIXER local IDs from param.h. */
extern const md_param_desc_t md_mixer_track_params[4];
#define MD_MIX_TRACK_PARAM_COUNT 4u

/* Defaults: unity level, centre pan, no sends, unity returns, empty slots and buses. */
int md_mixer_init(md_mixer_t *);
int md_mixer_track_param(md_mixer_t *, uint32_t track, md_param_id_t id, int32_t value);
int md_mixer_track_param_get(const md_mixer_t *, uint32_t track, md_param_id_t id, int32_t *value);
int md_mixer_mute(md_mixer_t *, uint32_t track, int on);
int md_mixer_solo(md_mixer_t *, uint32_t track, int on);
int md_mixer_return_level(md_mixer_t *, uint32_t bus, int32_t q15);
int md_mixer_return_level_get(const md_mixer_t *, uint32_t bus, int32_t *q15);

/* NULL empties a slot. Inserts and master need MD_EFFECT_INSERT, buses MD_EFFECT_BUS.
 * A new insert/master starts from dry and fades in unless bypassed. */
int md_mixer_set_insert(md_mixer_t *, uint32_t track, uint32_t slot, md_effect_t *);
int md_mixer_insert_bypass(md_mixer_t *, uint32_t track, uint32_t slot, int bypass);
int md_mixer_insert_idle(const md_mixer_t *, uint32_t track, uint32_t slot); /* 1, 0 or error */
int md_mixer_set_bus(md_mixer_t *, uint32_t bus, md_effect_t *);
int md_mixer_set_master(md_mixer_t *, md_effect_t *); /* bypassed until md_mixer_master_bypass(0) */
int md_mixer_master_bypass(md_mixer_t *, int bypass);
int md_mixer_master_idle(const md_mixer_t *);
/* Forwards 40..240 BPM Q8 to every assigned effect and to effects assigned later. */
int md_mixer_tempo(md_mixer_t *, uint16_t bpm_q8);

/* Both are refused with MD_ERR_STATE inside a span.
 * Panic: clear every assigned effect's history; gains keep moving smoothly. */
int md_mixer_panic(md_mixer_t *);
/* Project replacement: panic, settle every gain and crossfade, clear meters. */
int md_mixer_reset(md_mixer_t *);

/* One span: begin, add each rendered track at most once (the buffer is processed in
 * place by its inserts), then finish adds the saturated mix into out. A track that
 * was not added is processed as silence so its inserts and gains stay in time. */
int md_mixer_begin(md_mixer_t *, uint32_t frames);
int md_mixer_add_track(md_mixer_t *, uint32_t track, int32_t *stereo);
/* The same on a lane (0 .. MD_MIX_LANES - 1; md_mixer_add_track is lane 0). Two callers may add different tracks
 * at the same time when each keeps to its own lane: a track's state, inserts and meter are its own, and the sums,
 * the scratch and the clamp count are the lane's. The result is the same on any lane. */
int md_mixer_add_track_lane(md_mixer_t *, uint32_t track, int32_t *stereo, uint32_t lane);
int md_mixer_finish(md_mixer_t *, int32_t *out);

void md_mixer_meters(const md_mixer_t *, md_mixer_meters_t *out);
void md_mixer_clear_meters(md_mixer_t *);
#endif
