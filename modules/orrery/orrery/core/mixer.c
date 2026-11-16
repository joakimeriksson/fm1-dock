/* SPDX-License-Identifier: GPL-3.0-only */
#include "mixer.h"
#include "processor_internal.h"
#include "hot.h"
#include <string.h>

const md_param_desc_t md_mixer_track_params[4] = {
    {MD_MIX_LEVEL, MD_PARAM_AUTOMATABLE | MD_PARAM_SMOOTH, "Level", 0, 32767, 32767, MD_UNIT_Q15, NULL, 0},
    {MD_MIX_PAN, MD_PARAM_AUTOMATABLE | MD_PARAM_SMOOTH, "Pan", -32767, 32767, 0, MD_UNIT_Q15, NULL, 0},
    {MD_MIX_ECHO_SEND, MD_PARAM_AUTOMATABLE | MD_PARAM_SMOOTH, "Echo send", 0, 32767, 0, MD_UNIT_Q15, NULL, 0},
    {MD_MIX_REVERB_SEND, MD_PARAM_AUTOMATABLE | MD_PARAM_SMOOTH, "Reverb send", 0, 32767, 0, MD_UNIT_Q15, NULL, 0},
};

MD_INLINE int32_t magnitude(int32_t x) { return x < 0 ? -x : x; } /* only for clamped values */
/* Any int32 x, gain Q23 within 0..2^23: product within 2^54 and |result| <= |x|, so it fits int32.
 * Arithmetic right shift, as the engines' MUL macro relies on. */
MD_INLINE int32_t gain_apply(int32_t x, int32_t gain_q23)
{
    return (int32_t)(((int64_t)x * gain_q23) >> 23);
}
/* Symmetric one-pole step; snaps when the remaining distance rounds to zero. */
MD_INLINE int32_t smooth(int32_t current, int32_t target)
{
    int32_t d = target - current;
    int32_t step = d >= 0 ? d >> MD_MIX_SMOOTH_SHIFT : -((-d) >> MD_MIX_SMOOTH_SHIFT);
    return step ? current + step : target;
}
/* Q15 32767 is treated as exact unity so a full-scale chain is bit-transparent. */
static int32_t unit(int32_t q15) { return q15 >= 32767 ? 32768 : q15; }
static int32_t q15_to_q23(int32_t q15) { return unit(q15) * 256; }

static void track_targets(md_mixer_t *m, md_mixer_track_t *t)
{
    int audible = !t->mute && (!m->any_solo || t->solo);
    int32_t left = unit(t->pan <= 0 ? 32767 : 32767 - t->pan);
    int32_t right = unit(t->pan >= 0 ? 32767 : 32767 + t->pan);
    /* Q15 * Q15 = Q30, >> 7 = Q23; at most 32768*32768 >> 7 = 2^23. */
    t->gain_target[0] = audible ? (unit(t->level) * left) >> 7 : 0;
    t->gain_target[1] = audible ? (unit(t->level) * right) >> 7 : 0;
    t->gain_target[2] = q15_to_q23(t->send[MD_MIX_BUS_ECHO]);
    t->gain_target[3] = q15_to_q23(t->send[MD_MIX_BUS_REVERB]);
}
static void all_targets(md_mixer_t *m)
{
    m->any_solo = 0;
    for (uint32_t i = 0; i < MD_TRACKS; i++) if (m->track[i].solo) m->any_solo = 1;
    for (uint32_t i = 0; i < MD_TRACKS; i++) track_targets(m, &m->track[i]);
}
static void slot_target(md_mixer_slot_t *s) { s->wet_target = s->bypass ? 0 : MD_MIX_UNITY_Q23; }

int md_mixer_init(md_mixer_t *m)
{
    if (!m) return MD_ERR_ARGUMENT;
    memset(m, 0, sizeof *m);
    for (uint32_t i = 0; i < MD_TRACKS; i++) m->track[i].level = 32767;
    for (uint32_t b = 0; b < MD_MIX_BUSES; b++) {
        m->return_level[b] = 32767;
        m->return_gain[b] = m->return_target[b] = q15_to_q23(32767);
    }
    m->master.bypass = 1;
    all_targets(m);
    for (uint32_t i = 0; i < MD_TRACKS; i++)
        memcpy(m->track[i].gain, m->track[i].gain_target, sizeof m->track[i].gain);
    return MD_OK;
}

static int track_ok(const md_mixer_t *m, uint32_t track) { return m && track < MD_TRACKS; }
static const md_param_desc_t *track_param(md_param_id_t id)
{
    for (uint32_t i = 0; i < MD_MIX_TRACK_PARAM_COUNT; i++)
        if (md_mixer_track_params[i].id == id) return &md_mixer_track_params[i];
    return NULL;
}
int md_mixer_track_param(md_mixer_t *m, uint32_t track, md_param_id_t id, int32_t value)
{
    if (!track_ok(m, track)) return MD_ERR_ARGUMENT;
    const md_param_desc_t *p = track_param(id);
    if (!p) return MD_ERR_NOT_FOUND;
    if (value < p->min || value > p->max) return MD_ERR_RANGE;
    md_mixer_track_t *t = &m->track[track];
    switch (id) {
    case MD_MIX_LEVEL: t->level = value; break;
    case MD_MIX_PAN: t->pan = value; break;
    case MD_MIX_ECHO_SEND: t->send[MD_MIX_BUS_ECHO] = value; break;
    default: t->send[MD_MIX_BUS_REVERB] = value; break;
    }
    track_targets(m, t);
    return MD_OK;
}
int md_mixer_track_param_get(const md_mixer_t *m, uint32_t track, md_param_id_t id, int32_t *value)
{
    if (!track_ok(m, track) || !value) return MD_ERR_ARGUMENT;
    const md_mixer_track_t *t = &m->track[track];
    switch (id) {
    case MD_MIX_LEVEL: *value = t->level; return MD_OK;
    case MD_MIX_PAN: *value = t->pan; return MD_OK;
    case MD_MIX_ECHO_SEND: *value = t->send[MD_MIX_BUS_ECHO]; return MD_OK;
    case MD_MIX_REVERB_SEND: *value = t->send[MD_MIX_BUS_REVERB]; return MD_OK;
    default: return MD_ERR_NOT_FOUND;
    }
}
int md_mixer_mute(md_mixer_t *m, uint32_t track, int on)
{
    if (!track_ok(m, track)) return MD_ERR_ARGUMENT;
    m->track[track].mute = on != 0;
    track_targets(m, &m->track[track]);
    return MD_OK;
}
int md_mixer_solo(md_mixer_t *m, uint32_t track, int on)
{
    if (!track_ok(m, track)) return MD_ERR_ARGUMENT;
    m->track[track].solo = on != 0;
    all_targets(m);
    return MD_OK;
}
int md_mixer_return_level(md_mixer_t *m, uint32_t bus, int32_t q15)
{
    if (!m || bus >= MD_MIX_BUSES) return MD_ERR_ARGUMENT;
    if (q15 < 0 || q15 > 32767) return MD_ERR_RANGE;
    m->return_level[bus] = q15;
    m->return_target[bus] = q15_to_q23(q15);
    return MD_OK;
}
int md_mixer_return_level_get(const md_mixer_t *m, uint32_t bus, int32_t *q15)
{
    if (!m || bus >= MD_MIX_BUSES || !q15) return MD_ERR_ARGUMENT;
    *q15 = m->return_level[bus];
    return MD_OK;
}

/* One instance (or one state block) may occupy one slot; skip the slot being replaced. */
static int effect_in_use(const md_mixer_t *m, const md_effect_t *e, const void *except)
{
    const md_effect_t *used[MD_TRACKS * MD_MIX_INSERTS + MD_MIX_BUSES + 1];
    const void *where[MD_TRACKS * MD_MIX_INSERTS + MD_MIX_BUSES + 1];
    uint32_t n = 0;
    for (uint32_t t = 0; t < MD_TRACKS; t++)
        for (uint32_t s = 0; s < MD_MIX_INSERTS; s++) {
            used[n] = m->track[t].insert[s].effect; where[n++] = &m->track[t].insert[s];
        }
    for (uint32_t b = 0; b < MD_MIX_BUSES; b++) { used[n] = m->bus[b]; where[n++] = &m->bus[b]; }
    used[n] = m->master.effect; where[n++] = &m->master;
    for (uint32_t i = 0; i < n; i++)
        if (used[i] && where[i] != except &&
            (used[i] == e || md_internal_overlap(used[i]->state, used[i]->desc->processor.state_bytes,
                                                 e->state, e->desc->processor.state_bytes))) return 1;
    return 0;
}
static int assignable(const md_mixer_t *m, const md_effect_t *e, uint16_t capability, const void *slot)
{
    if (m->in_span) return MD_ERR_STATE;
    if (!e) return MD_OK;
    if (!e->desc || !e->state) return MD_ERR_STATE;
    if (!(e->desc->capabilities & capability)) return MD_ERR_UNSUPPORTED;
    if (md_internal_overlap(e, sizeof *e, m, sizeof *m) ||
        md_internal_overlap(e->state, e->desc->processor.state_bytes, m, sizeof *m)) return MD_ERR_ARGUMENT;
    if (effect_in_use(m, e, slot)) return MD_ERR_ARGUMENT;
    return MD_OK;
}
static void adopt(md_mixer_t *m, md_effect_t *e)
{
    md_effect_reset(e);
    if (m->tempo_q8) (void)md_effect_tempo(e, m->tempo_q8); /* UNSUPPORTED is fine */
}
static int slot_assign(md_mixer_t *m, md_mixer_slot_t *s, md_effect_t *e)
{
    int status = assignable(m, e, MD_EFFECT_INSERT, s);
    if (status) return status;
    if (e) adopt(m, e);
    s->effect = e;
    s->wet = 0;
    s->active = 0;
    slot_target(s);
    return MD_OK;
}
int md_mixer_set_insert(md_mixer_t *m, uint32_t track, uint32_t slot, md_effect_t *e)
{
    if (!track_ok(m, track) || slot >= MD_MIX_INSERTS) return MD_ERR_ARGUMENT;
    return slot_assign(m, &m->track[track].insert[slot], e);
}
int md_mixer_insert_bypass(md_mixer_t *m, uint32_t track, uint32_t slot, int bypass)
{
    if (!track_ok(m, track) || slot >= MD_MIX_INSERTS) return MD_ERR_ARGUMENT;
    if (m->in_span) return MD_ERR_STATE;
    m->track[track].insert[slot].bypass = bypass != 0;
    slot_target(&m->track[track].insert[slot]);
    return MD_OK;
}
int md_mixer_insert_idle(const md_mixer_t *m, uint32_t track, uint32_t slot)
{
    if (!track_ok(m, track) || slot >= MD_MIX_INSERTS) return MD_ERR_ARGUMENT;
    return !m->track[track].insert[slot].active;
}
int md_mixer_set_bus(md_mixer_t *m, uint32_t bus, md_effect_t *e)
{
    if (!m || bus >= MD_MIX_BUSES) return MD_ERR_ARGUMENT;
    int status = assignable(m, e, MD_EFFECT_BUS, &m->bus[bus]);
    if (status) return status;
    if (e) adopt(m, e);
    m->bus[bus] = e;
    return MD_OK;
}
int md_mixer_set_master(md_mixer_t *m, md_effect_t *e)
{
    if (!m) return MD_ERR_ARGUMENT;
    return slot_assign(m, &m->master, e);
}
int md_mixer_master_bypass(md_mixer_t *m, int bypass)
{
    if (!m) return MD_ERR_ARGUMENT;
    if (m->in_span) return MD_ERR_STATE;
    m->master.bypass = bypass != 0;
    slot_target(&m->master);
    return MD_OK;
}
int md_mixer_master_idle(const md_mixer_t *m) { return m ? !m->master.active : MD_ERR_ARGUMENT; }

static void each_effect(md_mixer_t *m, void (*fn)(md_mixer_t *, md_effect_t *))
{
    for (uint32_t t = 0; t < MD_TRACKS; t++)
        for (uint32_t s = 0; s < MD_MIX_INSERTS; s++)
            if (m->track[t].insert[s].effect) fn(m, m->track[t].insert[s].effect);
    for (uint32_t b = 0; b < MD_MIX_BUSES; b++) if (m->bus[b]) fn(m, m->bus[b]);
    if (m->master.effect) fn(m, m->master.effect);
}
static void tempo_one(md_mixer_t *m, md_effect_t *e) { (void)md_effect_tempo(e, m->tempo_q8); }
static void reset_one(md_mixer_t *m, md_effect_t *e) { (void)m; md_effect_reset(e); }
int md_mixer_tempo(md_mixer_t *m, uint16_t bpm_q8)
{
    if (!m) return MD_ERR_ARGUMENT;
    if (bpm_q8 < 40u * 256u || bpm_q8 > 240u * 256u) return MD_ERR_RANGE;
    if (m->in_span) return MD_ERR_STATE;
    m->tempo_q8 = bpm_q8;
    each_effect(m, tempo_one);
    return MD_OK;
}
int md_mixer_panic(md_mixer_t *m)
{
    if (!m) return MD_ERR_ARGUMENT;
    if (m->in_span) return MD_ERR_STATE;
    each_effect(m, reset_one);
    return MD_OK;
}
static void settle(md_mixer_slot_t *s)
{
    s->wet = s->wet_target;
    s->active = s->effect && s->wet_target;
}
int md_mixer_reset(md_mixer_t *m)
{
    int status = md_mixer_panic(m);
    if (status) return status;
    for (uint32_t t = 0; t < MD_TRACKS; t++) {
        memcpy(m->track[t].gain, m->track[t].gain_target, sizeof m->track[t].gain);
        for (uint32_t s = 0; s < MD_MIX_INSERTS; s++) settle(&m->track[t].insert[s]);
    }
    for (uint32_t b = 0; b < MD_MIX_BUSES; b++) m->return_gain[b] = m->return_target[b];
    settle(&m->master);
    md_mixer_clear_meters(m);
    return MD_OK;
}

/* ---- the loops over the frames (MD_HOT, hot.h): each keeps what it counts in registers and calls nothing */

/* n samples held within the limit; how many were over */
MD_HOT static uint32_t mix_clamp(int32_t *p, uint32_t n)
{
    uint32_t over = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t x = p[i];
        if (x > MD_MIX_LIMIT) { p[i] = MD_MIX_LIMIT; over++; }
        else if (x < -MD_MIX_LIMIT) { p[i] = -MD_MIX_LIMIT; over++; }
    }
    return over;
}
/* One frame of a track: its gains, the clamp, into the dry sum and the sends it has, its peak.
 * |sample x gain| <= |sample| < 2^31: the product fits int64 and the result int32, then it is clamped.
 * Each track adds at most 2^24 per accumulator: the tracks together stay below 2^27.
 * A silent send costs nothing (its gain is exactly zero once settled). */
#define MIX_FRAME() do { \
        int32_t l = gain_apply(buf[i], g0), r = gain_apply(buf[i + 1], g1); \
        if (l > MD_MIX_LIMIT) { l = MD_MIX_LIMIT; clamped++; } else if (l < -MD_MIX_LIMIT) { l = -MD_MIX_LIMIT; clamped++; } \
        if (r > MD_MIX_LIMIT) { r = MD_MIX_LIMIT; clamped++; } else if (r < -MD_MIX_LIMIT) { r = -MD_MIX_LIMIT; clamped++; } \
        dry[i] += l; \
        dry[i + 1] += r; \
        if (g2) { echo[i] += gain_apply(l, g2); echo[i + 1] += gain_apply(r, g2); } \
        if (g3) { reverb[i] += gain_apply(l, g3); reverb[i + 1] += gain_apply(r, g3); } \
        if (magnitude(l) > peak) peak = magnitude(l); \
        if (magnitude(r) > peak) peak = magnitude(r); \
    } while (0)
MD_HOT static void mix_track(md_mixer_t *m, md_mixer_track_t *t, const int32_t *buf, uint32_t index, uint32_t lane)
{
    const uint32_t n = 2u * m->frames;
    int32_t *const dry = lane ? m->dry1 : m->dry;
    int32_t *const echo = lane ? m->send1[MD_MIX_BUS_ECHO] : m->send[MD_MIX_BUS_ECHO];
    int32_t *const reverb = lane ? m->send1[MD_MIX_BUS_REVERB] : m->send[MD_MIX_BUS_REVERB];
    int32_t g0 = t->gain[0], g1 = t->gain[1], g2 = t->gain[2], g3 = t->gain[3];
    const int32_t t0 = t->gain_target[0], t1 = t->gain_target[1], t2 = t->gain_target[2], t3 = t->gain_target[3];
    int32_t peak = m->meters.track_peak[index];
    uint32_t clamped = 0, i;
    if (g0 != t0 || g1 != t1 || g2 != t2 || g3 != t3) {   /* moving: every gain a step a frame, for the block */
        for (i = 0; i < n; i += 2) {
            g0 = smooth(g0, t0); g1 = smooth(g1, t1); g2 = smooth(g2, t2); g3 = smooth(g3, t3);
            MIX_FRAME();
        }
        t->gain[0] = g0; t->gain[1] = g1; t->gain[2] = g2; t->gain[3] = g3;
    } else
        for (i = 0; i < n; i += 2) MIX_FRAME();
    m->meters.track_peak[index] = peak;
    if (lane) m->clamped1 += clamped; else m->meters.clamped += clamped;
}
/* the second lane's sums into the first's */
MD_HOT static void mix_join(md_mixer_t *m)
{
    const uint32_t n = 2u * m->frames;
    for (uint32_t i = 0; i < n; i++) {
        m->dry[i] += m->dry1[i];
        m->send[MD_MIX_BUS_ECHO][i] += m->send1[MD_MIX_BUS_ECHO][i];
        m->send[MD_MIX_BUS_REVERB][i] += m->send1[MD_MIX_BUS_REVERB][i];
    }
}
/* a bus's wet signal into the dry sum, at its return level */
MD_HOT static void mix_return(md_mixer_t *m, uint32_t b)
{
    const uint32_t n = 2u * m->frames;
    const int32_t *const wet = m->wet, target = m->return_target[b];
    int32_t *const dry = m->dry;
    int32_t gain = m->return_gain[b], peak = m->meters.return_peak[b];
    uint32_t clamped = 0;
    const int moving = gain != target;
    for (uint32_t i = 0; i < n; i += 2) {
        if (moving) gain = smooth(gain, target);
        for (uint32_t c = 0; c < 2; c++) {
            int32_t v = wet[i + c];
            if (v > MD_MIX_LIMIT) { v = MD_MIX_LIMIT; clamped++; } else if (v < -MD_MIX_LIMIT) { v = -MD_MIX_LIMIT; clamped++; }
            v = gain_apply(v, gain);
            dry[i + c] += v;
            if (magnitude(v) > peak) peak = magnitude(v);
        }
    }
    m->return_gain[b] = gain;
    m->meters.return_peak[b] = peak;
    m->meters.clamped += clamped;
}
/* the sum into out, saturated: clamped here unless the master did it (clamp: one pass fewer, the same result) */
MD_HOT static void mix_out(md_mixer_t *m, int32_t *out, int clamp_here)
{
    const uint32_t n = 2u * m->frames;
    const int32_t *const dry = m->dry;
    int32_t peak = m->meters.master_peak;
    uint32_t clamped = 0, clipped = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t v = dry[i];
        if (clamp_here) {
            if (v > MD_MIX_LIMIT) { v = MD_MIX_LIMIT; clamped++; } else if (v < -MD_MIX_LIMIT) { v = -MD_MIX_LIMIT; clamped++; }
        }
        if (magnitude(v) > peak) peak = magnitude(v);
        if (v > 32767) { v = 32767; clipped++; }
        else if (v < -32767) { v = -32767; clipped++; }
        out[i] += v;
    }
    m->meters.master_peak = peak;
    m->meters.clamped += clamped;
    m->meters.clipped += clipped;
}

/* In-place insert with bypass crossfade. An idle slot (bypass ramp at zero) is
 * skipped and reset when it is enabled again, so dormant history cannot return. */
static void slot_process(md_mixer_t *m, md_mixer_slot_t *s, int32_t *buf, uint32_t frames, uint32_t lane)
{
    int32_t *const xfade = lane ? m->xfade1 : m->xfade;
    uint32_t *const clamped = lane ? &m->clamped1 : &m->meters.clamped;
    if (!s->effect) return;
    if (!s->active) {
        if (!s->wet_target) return;
        md_effect_reset(s->effect);
        s->active = 1;
    }
    uint32_t n = 2u * frames;
    int fading = s->wet != MD_MIX_UNITY_Q23 || s->wet_target != MD_MIX_UNITY_Q23;
    if (fading) memcpy(xfade, buf, n * sizeof *buf);
    md_effect_process_replace(s->effect, buf, frames);
    *clamped += MD_FAR(mix_clamp)(buf, n);
    if (!fading) return;
    for (uint32_t i = 0; i < n; i += 2) {
        s->wet = smooth(s->wet, s->wet_target);
        for (uint32_t c = 0; c < 2; c++) {
            int32_t dry = xfade[i + c];
            buf[i + c] = dry + gain_apply(buf[i + c] - dry, s->wet);
        }
    }
    if (!s->wet && !s->wet_target) s->active = 0;
}

static void track_process(md_mixer_t *m, uint32_t index, int32_t *buf, uint32_t lane)
{
    md_mixer_track_t *t = &m->track[index];
    uint32_t n = 2u * m->frames;
    /* With an insert that will run, its input is clamped first; otherwise the clamp moves after the gain (one pass
     * fewer): a gain of at most unity cannot enlarge a sample, so the result is the same within the limit. Beyond
     * it (|sample| > 2^24, a faulty instrument) the gain now applies before the clamp: 2^25 at half level gives
     * 2^24 unclamped instead of 2^23 and one clamp count. Still bounded: |product| <= |sample| < 2^31. */
    int inserts = 0;
    for (uint32_t s = 0; s < MD_MIX_INSERTS; s++)
        inserts |= t->insert[s].effect && (t->insert[s].active || t->insert[s].wet_target);
    if (inserts) {
        uint32_t over = MD_FAR(mix_clamp)(buf, n);
        if (lane) m->clamped1 += over; else m->meters.clamped += over;
        for (uint32_t s = 0; s < MD_MIX_INSERTS; s++) slot_process(m, &t->insert[s], buf, m->frames, lane);
    }
    MD_FAR(mix_track)(m, t, buf, index, lane);
    t->added = 1;
}
static int track_idle(const md_mixer_track_t *t)
{
    for (uint32_t s = 0; s < MD_MIX_INSERTS; s++) if (t->insert[s].effect) return 0;
    for (uint32_t g = 0; g < 4; g++) if (t->gain[g] != t->gain_target[g]) return 0;
    return 1; /* silence through settled gains changes no state */
}

int md_mixer_begin(md_mixer_t *m, uint32_t frames)
{
    if (!m) return MD_ERR_ARGUMENT;
    if (m->in_span) return MD_ERR_STATE;
    if (!frames || frames > MD_MAX_RENDER_FRAMES) return MD_ERR_RANGE;
    md_clear32(m->dry, 2u * frames);
    for (uint32_t b = 0; b < MD_MIX_BUSES; b++) md_clear32(m->send[b], 2u * frames);
    for (uint32_t t = 0; t < MD_TRACKS; t++) m->track[t].added = 0;
    m->lane1 = 0;
    m->clamped1 = 0;
    m->frames = frames;
    m->in_span = 1;
    return MD_OK;
}
int md_mixer_add_track_lane(md_mixer_t *m, uint32_t track, int32_t *stereo, uint32_t lane)
{
    if (!track_ok(m, track) || !stereo || lane >= MD_MIX_LANES) return MD_ERR_ARGUMENT;
    if (!m->in_span || m->track[track].added) return MD_ERR_STATE;
    if (md_internal_overlap(stereo, 2u * m->frames * sizeof *stereo, m, sizeof *m)) return MD_ERR_ARGUMENT;
    if (lane && !m->lane1) {              /* the lane's first track of the span: its sums start here */
        md_clear32(m->dry1, 2u * m->frames);
        for (uint32_t b = 0; b < MD_MIX_BUSES; b++) md_clear32(m->send1[b], 2u * m->frames);
        m->lane1 = 1;
    }
    track_process(m, track, stereo, lane);
    return MD_OK;
}
int md_mixer_add_track(md_mixer_t *m, uint32_t track, int32_t *stereo) { return md_mixer_add_track_lane(m, track, stereo, 0); }
static void bus_process(md_mixer_t *m, uint32_t b)
{
    if (!m->bus[b]) { m->return_gain[b] = m->return_target[b]; return; }
    m->meters.clamped += MD_FAR(mix_clamp)(m->send[b], 2u * m->frames);
    md_effect_process_wet(m->bus[b], m->send[b], m->wet, m->frames);
    MD_FAR(mix_return)(m, b);
}
int md_mixer_finish(md_mixer_t *m, int32_t *out)
{
    if (!m || !out) return MD_ERR_ARGUMENT;
    if (!m->in_span) return MD_ERR_STATE;
    uint32_t n = 2u * m->frames;
    if (md_internal_overlap(out, n * sizeof *out, m, sizeof *m)) return MD_ERR_ARGUMENT;
    for (uint32_t t = 0; t < MD_TRACKS; t++) {
        if (m->track[t].added || track_idle(&m->track[t])) continue;
        md_clear32(m->silent, n);
        track_process(m, t, m->silent, 0);
    }
    if (m->lane1) {
        MD_FAR(mix_join)(m);
        m->meters.clamped += m->clamped1;
    }
    for (uint32_t b = 0; b < MD_MIX_BUSES; b++) bus_process(m, b);
    /* dry + returns: at most 4 * 2^24 + 2 * 2^24 < 2^27, clamped before the master (in the output loop itself
     * when the master slot is idle: one pass fewer, the same result). */
    int master = m->master.effect && (m->master.active || m->master.wet_target);
    if (master) {
        m->meters.clamped += MD_FAR(mix_clamp)(m->dry, n);
        slot_process(m, &m->master, m->dry, m->frames, 0);
    }
    MD_FAR(mix_out)(m, out, !master);
    m->in_span = 0;
    return MD_OK;
}

void md_mixer_meters(const md_mixer_t *m, md_mixer_meters_t *out)
{
    if (m && out) *out = m->meters;
}
void md_mixer_clear_meters(md_mixer_t *m)
{
    if (m) memset(&m->meters, 0, sizeof m->meters);
}
