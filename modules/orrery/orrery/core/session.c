/* SPDX-License-Identifier: GPL-3.0-only */
#include "session.h"
#include "processor_internal.h"
#include "hot.h"
#include <string.h>

/* ---- parameter targets -------------------------------------------------------- */

enum { TARGET_INSTRUMENT, TARGET_EFFECT, TARGET_MIXER };
typedef struct {
    int kind;
    uint32_t slot;                 /* base table row for instruments/effects */
    md_instrument_t *instrument;
    md_effect_t *effect;
    const md_param_desc_t *param;
    uint32_t index;                /* parameter index in its descriptor */
    md_param_id_t local;
} target_t;

static int find_param(const md_processor_desc_t *d, md_param_id_t id, target_t *t)
{
    for (uint32_t i = 0; i < d->param_count; i++)
        if (d->params[i].id == id) { t->param = &d->params[i]; t->index = i; return MD_OK; }
    return MD_ERR_NOT_FOUND;
}
static int decode(const md_session_t *s, uint8_t track, uint16_t target, target_t *t)
{
    *t = (target_t){0};
    if (track >= MD_TRACKS) return MD_ERR_RANGE;
    t->local = (md_param_id_t)(target & MD_TARGET_LOCAL_MASK);
    if (!t->local) return MD_ERR_RANGE;
    switch (target & MD_TARGET_GROUP_MASK) {
    case MD_TARGET_INSTRUMENT:
        if (!(t->instrument = s->instrument[track])) return MD_ERR_NOT_FOUND;
        t->kind = TARGET_INSTRUMENT; t->slot = track;
        return find_param(&t->instrument->desc->processor, t->local, t);
    case MD_TARGET_INSERT_1:
    case MD_TARGET_INSERT_2: {
        uint32_t slot = (target & MD_TARGET_GROUP_MASK) == MD_TARGET_INSERT_1 ? 0u : 1u;
        t->effect = s->insert[track][slot];
        t->slot = MD_TRACKS + track * MD_MIX_INSERTS + slot;
        break;
    }
    case MD_TARGET_MIXER:
        if (t->local > MD_MIX_TRACK_PARAM_COUNT) return MD_ERR_NOT_FOUND;
        t->kind = TARGET_MIXER; t->index = t->local - 1u; t->param = &md_mixer_track_params[t->index];
        return MD_OK;
    case MD_TARGET_ECHO:
    case MD_TARGET_REVERB: {
        uint32_t bus = (target & MD_TARGET_GROUP_MASK) == MD_TARGET_ECHO ? 0u : 1u;
        t->effect = s->bus[bus];
        t->slot = MD_TRACKS + MD_TRACKS * MD_MIX_INSERTS + bus;
        break;
    }
    case MD_TARGET_MASTER_FILTER:
        t->effect = s->master; t->slot = MD_SESSION_SLOTS - 1u;
        break;
    default:
        return MD_ERR_RANGE;
    }
    if (!t->effect) return MD_ERR_NOT_FOUND;
    t->kind = TARGET_EFFECT;
    return find_param(&t->effect->desc->processor, t->local, t);
}
static int32_t *base_cell(md_session_t *s, uint8_t track, const target_t *t)
{
    return t->kind == TARGET_MIXER ? &s->mix_base[track][t->index] : &s->base[t->slot][t->index];
}
/* Is a sequencer lock holding this target now? Global groups (echo, reverb, master)
 * have one owning track, so any track's lock on them counts. */
static int locked(const md_session_t *s, uint8_t track, uint16_t target)
{
    int global = (target & MD_TARGET_GROUP_MASK) >= MD_TARGET_ECHO;
    for (uint8_t t = 0; t < MD_TRACKS; t++) {
        const md_seq_lock_state_t *l = &s->seq.track[t].lock;
        if (l->active && l->target == target && (global || t == track)) return 1;
    }
    return 0;
}
/* UI edits (set_base) move the base and the DSP, but stay underneath an active lock
 * (decision 002) until its restore applies them. Locks and restores move only the DSP. */
static int apply_param(md_session_t *s, uint8_t track, uint16_t target, int32_t value,
                       md_source_id_t source, int set_base)
{
    target_t t;
    int status = decode(s, track, target, &t);
    if (status) return status;
    if (value < t.param->min || value > t.param->max) return MD_ERR_RANGE;
    if (t.param->flags & MD_PARAM_READ_ONLY) return MD_ERR_UNSUPPORTED;
    if (set_base && locked(s, track, target)) {
        *base_cell(s, track, &t) = value;
        return MD_OK;
    }
    if (t.kind == TARGET_INSTRUMENT) {
        md_event_t e = {.kind = MD_EVENT_PARAM, .track = track, .source = source};
        e.data.param.id = t.local;
        e.data.param.value = value;
        status = md_router_event(&s->router, &e);
    } else if (t.kind == TARGET_EFFECT) {
        status = md_effect_param(t.effect, t.local, value);
    } else {
        status = md_mixer_track_param(&s->mixer, track, t.local, value);
    }
    if (!status && set_base) *base_cell(s, track, &t) = value;
    return status;
}
static int base_value(void *context, uint8_t track, uint16_t target, int32_t *value)
{
    md_session_t *s = context;
    target_t t;
    int status = decode(s, track, target, &t);
    if (status) return status;
    *value = *base_cell(s, track, &t);
    return MD_OK;
}
static md_note_token_t seq_token(void *context)
{
    return md_router_next_token(&((md_session_t *)context)->router);
}
static int read_bases(md_session_t *s, uint32_t slot, const md_instrument_t *inst, const md_effect_t *fx)
{
    const md_processor_desc_t *d = inst ? &inst->desc->processor : &fx->desc->processor;
    if (d->param_count > MD_SESSION_MAX_PARAMS) return MD_ERR_CAPACITY;
    for (uint32_t i = 0; i < d->param_count; i++) {
        int status = inst ? md_instrument_param_get(inst, d->params[i].id, &s->base[slot][i])
                          : md_effect_param_get(fx, d->params[i].id, &s->base[slot][i]);
        if (status) return status;
    }
    return MD_OK;
}

/* ---- init (main, before audio) ------------------------------------------------- */

int md_session_init(md_session_t *s, const md_session_config_t *c)
{
    if (!s || !c || !c->bank) return MD_ERR_ARGUMENT;
    int status = md_seq_bank_validate(c->bank);
    if (status) return status;
    memset(s, 0, sizeof *s);
    md_ring_init(&s->ring);
    s->bank[0] = *c->bank;
    s->bank[1] = *c->bank;
    s->live_token = MD_LIVE_TOKEN_BASE;
    memcpy(s->instrument, c->instrument, sizeof s->instrument);
    for (uint32_t t = 0; t < MD_TRACKS; t++) {
        s->choice[t][0] = c->instrument[t];
        s->choice[t][1] = c->alternate[t];
        if (c->alternate[t] && (!c->alternate[t]->desc || !c->alternate[t]->state || c->alternate[t] == c->instrument[t]))
            return MD_ERR_ARGUMENT;
        for (uint32_t u = 0; c->alternate[t] && u < MD_TRACKS; u++)   /* no state shared with any other instance */
            if ((c->instrument[u] && c->instrument[u]->state == c->alternate[t]->state) ||
                (u != t && c->alternate[u] && c->alternate[u]->state == c->alternate[t]->state)) return MD_ERR_ARGUMENT;
    }
    memcpy(s->insert, c->insert, sizeof s->insert);
    memcpy(s->bus, c->bus, sizeof s->bus);
    s->master = c->master;
    if ((status = md_router_init(&s->router, s->instrument))) return status;
    if ((status = md_transport_init(&s->clock, c->bpm_q8))) return status;
    if ((status = md_mixer_init(&s->mixer))) return status;
    for (uint32_t t = 0; t < MD_TRACKS; t++)
        for (uint32_t k = 0; k < MD_MIX_INSERTS; k++)
            if ((status = md_mixer_set_insert(&s->mixer, t, k, s->insert[t][k]))) return status;
    for (uint32_t b = 0; b < MD_MIX_BUSES; b++)
        if ((status = md_mixer_set_bus(&s->mixer, b, s->bus[b]))) return status;
    if ((status = md_mixer_set_master(&s->mixer, s->master))) return status;
    if ((status = md_mixer_tempo(&s->mixer, c->bpm_q8))) return status;
    if ((status = md_mixer_reset(&s->mixer))) return status;
    for (uint32_t t = 0; t < MD_TRACKS; t++) {
        if (s->instrument[t] && (status = read_bases(s, t, s->instrument[t], NULL))) return status;
        for (uint32_t k = 0; k < MD_MIX_INSERTS; k++)
            if (s->insert[t][k] && (status = read_bases(s, MD_TRACKS + t * MD_MIX_INSERTS + k, NULL, s->insert[t][k])))
                return status;
        for (uint32_t p = 0; p < MD_MIX_TRACK_PARAM_COUNT; p++)
            (void)md_mixer_track_param_get(&s->mixer, t, md_mixer_track_params[p].id, &s->mix_base[t][p]);
    }
    for (uint32_t b = 0; b < MD_MIX_BUSES; b++)
        if (s->bus[b] && (status = read_bases(s, MD_TRACKS * (1u + MD_MIX_INSERTS) + b, NULL, s->bus[b]))) return status;
    if (s->master && (status = read_bases(s, MD_SESSION_SLOTS - 1u, NULL, s->master))) return status;
    md_seq_callbacks_t callbacks = {s, seq_token, base_value};
    if ((status = md_seq_init(&s->seq, &s->bank[0], callbacks))) return status;
    s->parallel = c->parallel;
    s->parallel_track = c->parallel_track < MD_TRACKS && c->parallel.run && c->parallel.wait ? c->parallel_track : 0xffu;
    s->par_limit = c->parallel_queue && c->parallel_queue < MD_PARALLEL_EVENTS ? c->parallel_queue : MD_PARALLEL_EVENTS;
    s->par_mask = (uint8_t)(c->parallel_mask ? c->parallel_mask & MD_ALL_TRACK_MASK :
                            s->parallel_track < MD_TRACKS ? 1u << s->parallel_track : 0u);
    if (!c->parallel.run || !c->parallel.wait) s->par_mask = 0;
    s->par_balance = c->parallel_balance && c->parallel.ticks && s->par_mask;
    s->stats.bpm_q8 = c->bpm_q8;
    s->snapshot = s->stats;
    return MD_OK;
}

/* ---- main side ------------------------------------------------------------------ */

static int is_release(const md_command_t *c)
{
    if (c->kind == MD_CMD_STOP) return 1;
    if (c->kind == MD_CMD_SUSTAIN) return c->value == 0;
    if (c->kind != MD_CMD_EVENT) return 0;
    return c->event.kind == MD_EVENT_NOTE_OFF || c->event.kind == MD_EVENT_SOURCE_RELEASE ||
           c->event.kind == MD_EVENT_PANIC;
}
/* Raise a coalesced request: at most one pending, so the counter cannot wrap back to
 * "nothing pending" however many releases are refused before audio runs. */
static void raise_request(volatile uint8_t *request, const volatile uint8_t *seen)
{
    uint8_t done = *seen;
    MD_FENCE();
    if (*request != done) return; /* already pending */
    *request = (uint8_t)(done + 1u);
}
static void request_flush(md_session_t *s, md_source_id_t source)
{
    if (source < MD_MAX_SOURCES) raise_request(&s->flush_request[source], &s->flush_seen[source]);
}
int md_session_submit(md_session_t *s, const md_command_t *c)
{
    if (!s || !c) return MD_ERR_ARGUMENT;
    if (c->kind == MD_CMD_BANK || c->kind == MD_CMD_SOUND) return MD_ERR_ARGUMENT; /* use the edit calls */
    int release = is_release(c);
    int status = md_ring_push(&s->ring, c, release);
    if (status != MD_ERR_CAPACITY || !release) return status;
    if (c->kind == MD_CMD_STOP) raise_request(&s->control_request[0], &s->control_seen[0]);
    else if (c->kind == MD_CMD_SUSTAIN) request_flush(s, c->event.source);
    else if (c->event.kind == MD_EVENT_PANIC && c->event.track == MD_ALL_TRACKS)
        raise_request(&s->control_request[1], &s->control_seen[1]);
    else if (c->event.kind != MD_EVENT_PANIC) request_flush(s, c->event.source);
    /* a refused per-track panic only returns the error: its caller retries */
    return status;
}
int md_session_note_on(md_session_t *s, uint8_t track, md_source_id_t source, uint8_t key,
                       uint8_t velocity, uint16_t flags, md_note_token_t *token)
{
    if (!s || !token) return MD_ERR_ARGUMENT;
    md_note_token_t next = s->live_token + 1u;
    if (next < MD_LIVE_TOKEN_BASE) next = MD_LIVE_TOKEN_BASE + 1u; /* stay in the live range */
    md_command_t c = {.kind = MD_CMD_EVENT};
    c.event = (md_event_t){.kind = MD_EVENT_NOTE_ON, .track = track, .flags = flags, .source = source, .token = next};
    c.event.data.note.key = key;
    c.event.data.note.velocity = velocity;
    int status = md_session_submit(s, &c);
    if (status) return status;
    s->live_token = next;
    *token = next;
    return MD_OK;
}
int md_session_note_off(md_session_t *s, uint8_t track, md_source_id_t source, md_note_token_t token)
{
    md_command_t c = {.kind = MD_CMD_EVENT};
    c.event = (md_event_t){.kind = MD_EVENT_NOTE_OFF, .track = track, .source = source, .token = token};
    return md_session_submit(s, &c);
}
int md_session_param(md_session_t *s, uint8_t track, uint16_t target, int32_t value)
{
    md_command_t c = {.kind = MD_CMD_EVENT};
    c.event = (md_event_t){.kind = MD_EVENT_PARAM, .track = track, .source = MD_SOURCE_NONE};
    c.event.data.param.id = target;
    c.event.data.param.value = value;
    return md_session_submit(s, &c);
}
uint32_t md_session_pending(md_session_t *s)
{
    if (!s) return 0;
    uint32_t bank_ack = s->bank_ack, sound_ack = s->sound_ack;
    MD_FENCE();
    uint32_t pending = 0;
    if (bank_ack != s->bank_generation) pending |= 1u;
    else if (s->staged) { s->main_live = (uint8_t)(1u - s->main_live); s->staged = 0; }
    if (sound_ack != s->sound_generation) pending |= 2u;
    return pending;
}
md_seq_bank_t *md_session_edit_bank(md_session_t *s)
{
    if (!s || (md_session_pending(s) & 1u)) return NULL;
    md_seq_bank_t *staging = &s->bank[1u - s->main_live];
    if (!s->staging_open) {
        *staging = s->bank[s->main_live]; /* both are read-only to audio except the live one, which it only reads */
        s->staging_open = 1;
    }
    return staging;
}
int md_session_publish_bank(md_session_t *s)
{
    if (!s) return MD_ERR_ARGUMENT;
    if (!s->staging_open || (md_session_pending(s) & 1u)) return MD_ERR_STATE;
    uint8_t index = (uint8_t)(1u - s->main_live);
    int status = md_seq_bank_validate(&s->bank[index]);
    if (status) return status;
    md_command_t c = {.kind = MD_CMD_BANK, .arg = index, .value = s->bank_generation + 1u};
    MD_FENCE(); /* the whole bank before the command that publishes it */
    status = md_ring_push(&s->ring, &c, 0);
    if (status) return status;
    s->bank_generation++;
    s->staging_open = 0;
    s->staged = 1;
    return MD_OK;
}
int md_session_stage_sound(md_session_t *s, uint8_t track, uint16_t version, const uint8_t *src,
                           uint32_t bytes)
{
    if (!s || track >= MD_TRACKS || (!src && bytes)) return MD_ERR_ARGUMENT;
    if (bytes > MD_SESSION_SOUND_BYTES) return MD_ERR_CAPACITY;
    if (!s->instrument[track]) return MD_ERR_NOT_FOUND; /* bindings are fixed after init */
    if (md_session_pending(s) & 2u) return MD_ERR_STATE;
    int status = md_sound_validate(s->instrument[track]->desc, version, src, bytes);
    if (status) return status;
    if (bytes) memcpy(s->sound, src, bytes);
    s->sound_bytes = (uint16_t)bytes;
    s->sound_version = version;
    md_command_t c = {.kind = MD_CMD_SOUND, .track = track, .value = s->sound_generation + 1u};
    MD_FENCE();
    status = md_ring_push(&s->ring, &c, 0);
    if (status) return status;
    s->sound_generation++;
    return MD_OK;
}
int md_session_snapshot(const md_session_t *s, md_session_snapshot_t *out)
{
    if (!s || !out) return MD_ERR_ARGUMENT;
    for (uint32_t attempt = 0; attempt < 16u; attempt++) {
        uint32_t before = s->snapshot_sequence;
        MD_FENCE();
        if (before & 1u) continue;
        md_session_snapshot_t copy = s->snapshot;
        MD_FENCE();
        if (s->snapshot_sequence == before) { *out = copy; return MD_OK; }
    }
    return MD_ERR_STATE;
}

/* ---- audio side ----------------------------------------------------------------- */

static void note_error(md_session_t *s, int status)
{
    if (status < 0) s->stats.last_error = status;
}
static int release_sequencer_sources(md_session_t *s)
{
    int result = MD_OK;
    for (uint8_t t = 0; t < MD_TRACKS; t++) {
        md_event_t e = {.kind = MD_EVENT_SOURCE_RELEASE, .track = MD_ALL_TRACKS, .source = t};
        int status = md_router_event(&s->router, &e);
        if (status) result = status;
    }
    return result;
}
static int panic_all(md_session_t *s)
{
    (void)md_seq_stop(&s->seq);
    (void)md_transport_stop(&s->clock);
    md_event_t e = {.kind = MD_EVENT_PANIC, .track = MD_ALL_TRACKS, .source = MD_SOURCE_NONE};
    int status = md_router_event(&s->router, &e);
    int mixed = md_mixer_panic(&s->mixer);
    return status ? status : mixed;
}
static int mode_changed(const md_seq_bank_t *a, const md_seq_bank_t *b, uint8_t track)
{
    for (uint32_t p = 0; p < MD_PATTERNS_PER_TRACK; p++)
        if (a->pattern[track][p].meta.mode != b->pattern[track][p].meta.mode) return 1;
    return 0;
}
static int swap_bank(md_session_t *s, const md_command_t *c)
{
    if (c->arg > 1u) return MD_ERR_RANGE;
    const md_seq_bank_t *next = &s->bank[c->arg];
    /* A track whose pattern modes changed releases its sequenced notes: mute then
     * unmute schedules exactly that track's restore and source flush. */
    for (uint8_t t = 0; t < MD_TRACKS; t++)
        if (mode_changed(s->seq.bank, next, t) && !s->seq.track[t].muted) {
            (void)md_seq_mute(&s->seq, t, 1);
            (void)md_seq_mute(&s->seq, t, 0);
        }
    s->seq.bank = next; /* published between iterations, never during one */
    MD_FENCE();
    s->bank_ack = c->value;
    return MD_OK;
}
static int bind(md_session_t *s, const md_command_t *c)
{
    if (c->track >= MD_TRACKS || c->value > 1u) return MD_ERR_RANGE;
    md_instrument_t *next = s->choice[c->track][c->value];
    if (!next) return MD_ERR_NOT_FOUND;
    if (next == s->instrument[c->track]) return MD_OK;
    if (s->instrument[c->track]) {
        md_event_t panic = {.kind = MD_EVENT_PANIC, .track = c->track, .source = MD_SOURCE_NONE};
        (void)md_router_event(&s->router, &panic);   /* every owner released, the outgoing sound cut */
    }
    s->instrument[c->track] = next;
    s->router.track[c->track] = next;                /* the router holds no note on this track now */
    return read_bases(s, c->track, next, NULL);
}
static int apply_sound(md_session_t *s, const md_command_t *c)
{
    int status = MD_ERR_NOT_FOUND;
    md_instrument_t *inst = c->track < MD_TRACKS ? s->instrument[c->track] : NULL;
    if (inst) {
        md_event_t panic = {.kind = MD_EVENT_PANIC, .track = c->track, .source = MD_SOURCE_NONE};
        (void)md_router_event(&s->router, &panic); /* release every owner, cut the old sound */
        status = md_sound_apply(inst, s->sound_version, s->sound, s->sound_bytes);
        if (!status) status = read_bases(s, c->track, inst, NULL);
    }
    MD_FENCE();
    s->sound_ack = c->value; /* acknowledged either way: the staging slot is free again */
    return status;
}
static int bypass(md_session_t *s, const md_command_t *c)
{
    if (c->value > 1u) return MD_ERR_RANGE;
    if (c->track == MD_BYPASS_MASTER) return md_mixer_master_bypass(&s->mixer, (int)c->value);
    return md_mixer_insert_bypass(&s->mixer, c->track, c->arg, (int)c->value);
}
static int apply_command(md_session_t *s, const md_command_t *c)
{
    switch (c->kind) {
    case MD_CMD_EVENT:
        if (c->event.kind == MD_EVENT_PARAM)
            return apply_param(s, c->event.track, c->event.data.param.id, c->event.data.param.value,
                               c->event.source, 1);
        if (c->event.kind == MD_EVENT_PANIC && c->event.track == MD_ALL_TRACKS) return panic_all(s);
        return md_router_event(&s->router, &c->event);
    case MD_CMD_SUSTAIN:
        return c->value > 1u ? MD_ERR_RANGE : md_router_sustain(&s->router, c->track, c->event.source, c->value);
    case MD_CMD_START: {
        md_position_t at = {.quarter = c->value};
        int status = md_transport_start(&s->clock, at);
        return status ? status : md_seq_start(&s->seq, at);
    }
    case MD_CMD_STOP: {
        int status = md_seq_stop(&s->seq);
        int clock = md_transport_stop(&s->clock);
        return status ? status : clock;
    }
    case MD_CMD_CONTINUE: {
        int status = md_transport_continue(&s->clock);
        return status ? status : md_seq_continue(&s->seq);
    }
    case MD_CMD_TEMPO: {
        if (c->value > 0xffffu) return MD_ERR_RANGE;
        int status = md_transport_set_tempo(&s->clock, (uint16_t)c->value);
        return status ? status : md_mixer_tempo(&s->mixer, (uint16_t)c->value);
    }
    case MD_CMD_MUTE: {
        if (c->track >= MD_TRACKS || c->value > 1u) return MD_ERR_RANGE;
        int status = md_seq_mute(&s->seq, c->track, c->value);
        if (!status) status = md_mixer_mute(&s->mixer, c->track, (int)c->value);
        if (!status) s->mute_mask = (uint8_t)((s->mute_mask & ~(1u << c->track)) | (c->value << c->track));
        return status;
    }
    case MD_CMD_SOLO: {
        if (c->track >= MD_TRACKS || c->value > 1u) return MD_ERR_RANGE;
        int status = md_mixer_solo(&s->mixer, c->track, (int)c->value);
        if (!status) s->solo_mask = (uint8_t)((s->solo_mask & ~(1u << c->track)) | (c->value << c->track));
        return status;
    }
    case MD_CMD_LAUNCH:
        if (c->value > 1u) return MD_ERR_RANGE;
        return md_seq_launch(&s->seq, &s->clock, c->track, c->arg, c->value);
    case MD_CMD_RETURN_LEVEL:
        return md_mixer_return_level(&s->mixer, c->arg, (int32_t)c->value);
    case MD_CMD_BYPASS:
        return bypass(s, c);
    case MD_CMD_BANK:
        return swap_bank(s, c);
    case MD_CMD_SOUND:
        return apply_sound(s, c);
    case MD_CMD_BIND:
        return bind(s, c);
    default:
        return MD_ERR_ARGUMENT;
    }
}
/* Each track renders up to the next event that touches it, so an event on one track does not split the
 * others' render calls (engines update coefficients per call: splits cost CPU, not correctness). */
static void render_track(md_session_t *s, uint32_t t, uint32_t *cursor, uint32_t to)
{
    if (to > cursor[t] && s->instrument[t])
        md_instrument_render_add(s->instrument[t], s->track_buffer[t] + 2u * cursor[t], to - cursor[t]);
    if (to > cursor[t]) cursor[t] = to;
}
/* the tracks whose instrument an event reaches: one, all, or none (mixer and effect targets) */
static uint32_t touched(const md_event_t *e)
{
    if (e->kind == MD_EVENT_PARAM && (e->data.param.id & MD_TARGET_GROUP_MASK) != MD_TARGET_INSTRUMENT) return 0;
    if (e->track == MD_ALL_TRACKS) return (1u << MD_TRACKS) - 1u;
    return e->track < MD_TRACKS ? 1u << e->track : 0u;
}
static int dispatch(md_session_t *s, const md_event_t *e)
{
    if (e->kind == MD_EVENT_PARAM) /* lock or restore: DSP only, the base stays */
        return apply_param(s, e->track, e->data.param.id, e->data.param.value, e->source, 0);
    return md_router_event(&s->router, e);
}
static void publish(md_session_t *s)
{
    md_session_snapshot_t *st = &s->stats;
    st->position = s->clock.position;
    st->samples = s->clock.samples;
    st->bpm_q8 = s->clock.bpm_q8;
    st->running = s->clock.running;
    st->mute_mask = s->mute_mask;
    st->solo_mask = s->solo_mask;
    for (uint32_t t = 0; t < MD_TRACKS; t++) {
        st->pattern[t] = s->seq.track[t].pattern;
        st->cursor[t] = s->seq.track[t].cursor;
        st->queued[t] = s->seq.track[t].queued;
        st->requested[t] = s->seq.track[t].requested;
    }
    st->seq_dropped = s->seq.dropped_notes;
    st->seq_base_errors = s->seq.base_errors;
    st->owned_notes = md_router_owned_count(&s->router);
    if (st->owned_notes > st->max_owned_notes) st->max_owned_notes = st->owned_notes;
    st->rejected_notes = s->router.rejected_notes + s->played_rejected;
    st->backend_errors = s->router.backend_errors;
    md_mixer_meters(&s->mixer, &st->meters);
    uint32_t sequence = s->snapshot_sequence + 1u;
    s->snapshot_sequence = sequence;      /* odd: writing */
    MD_FENCE();
    s->snapshot = *st;
    MD_FENCE();
    s->snapshot_sequence = sequence + 1u; /* even: complete */
}
#ifdef MD_SESSION_PROFILE
uint32_t (*md_session_ticks)(void);
uint32_t md_session_prof[MD_PROF_STAGES][2];
static uint32_t prof_t;
static void prof_mark(uint32_t stage)
{
    if (!md_session_ticks) return;
    uint32_t now = md_session_ticks(), d = now - prof_t;
    prof_t = now;
    if (stage < MD_PROF_STAGES) {
        md_session_prof[stage][0] += d;
        if (d > md_session_prof[stage][1]) md_session_prof[stage][1] = d;
    }
}
#define PROF(stage) prof_mark(stage)
#else
#define PROF(stage) ((void)0)
#endif
/* ---- routed first, played afterwards (session.h "Parallel") -------------------------------------------- */

typedef struct { uint32_t errors, rejected; int32_t last; uint8_t fault; } played_t;
/* one recorded operation: its track rendered up to it, then the instrument's event or reset */
static void op_play(md_session_t *s, uint32_t i, uint32_t *cursor, played_t *p)
{
    uint32_t t = s->par_queue[i].track;
    md_instrument_t *inst = s->instrument[t];
    if (!inst) return;
    render_track(s, t, cursor, s->par_queue[i].offset);
    if (s->par_queue[i].reset) { md_instrument_reset(inst); return; }
    int status = md_instrument_event(inst, &s->par_queue[i].event);
    if (status == MD_OK) return;
    p->errors++;
    p->last = status;
    if (s->par_queue[i].event.kind == MD_EVENT_HIT && status == MD_ERR_CAPACITY) p->rejected++;   /* dropped, as ever */
    else p->fault |= (uint8_t)(1u << t);          /* the router took it as done: the track is panicked next block */
}
/* a core's share of a block: the recorded operations of its tracks (mine) in order, each track rendered to the
 * block's end and added to the mixer on the core's lane; with a counter, what each track took */
static void play(md_session_t *s, uint32_t mine, uint32_t lane, uint32_t *cursor, played_t *p)
{
    uint32_t (*ticks)(void) = s->par_balance ? s->parallel.ticks : NULL;
    uint32_t cost[MD_TRACKS] = {0}, t0 = 0;
    for (uint32_t i = 0; i < s->par_count; i++) {
        uint32_t t = s->par_queue[i].track;
        if (!(mine >> t & 1u)) continue;
        if (ticks) t0 = ticks();
        op_play(s, i, cursor, p);
        if (ticks) cost[t] += ticks() - t0;
    }
    for (uint32_t t = 0; t < MD_TRACKS; t++) {
        if (!(mine >> t & 1u)) continue;
        if (ticks) t0 = ticks();
        render_track(s, t, cursor, s->par_frames);
        if (s->instrument[t]) (void)md_mixer_add_track_lane(&s->mixer, t, s->track_buffer[t], lane);
        if (ticks) {                              /* up at once, down a quarter of the way a block */
            uint32_t now = cost[t] + (ticks() - t0), was = s->track_cost[t];
            s->track_cost[t] = now >= was ? now : was - ((was - now + 3u) >> 2);
        }
    }
}
/* the tracks for the second core this block: the dearest first, each to the core with less so far */
static uint32_t balance(const md_session_t *s, uint32_t start)
{
    uint32_t order[MD_TRACKS], load[2] = {0, 0}, mask = 0, any = 0;
    for (uint32_t t = 0; t < MD_TRACKS; t++) {
        uint32_t k = t;
        for (; k && s->track_cost[order[k - 1u]] < s->track_cost[t]; k--) order[k] = order[k - 1u];
        order[k] = t;
        any |= s->track_cost[t];
    }
    if (!any) return start;                       /* nothing measured yet */
    for (uint32_t k = 0; k < MD_TRACKS; k++) {
        uint32_t t = order[k], core = load[1] < load[0];
        if (!s->instrument[t]) continue;
        load[core] += s->track_cost[t];
        mask |= core << t;
    }
    return mask;
}
static void played(md_session_t *s, const played_t *p)
{
    s->stats.event_errors += p->errors;
    if (p->errors) note_error(s, p->last);
    s->played_rejected += p->rejected;
    s->fault_mask |= p->fault;
}
/* the router's sink while a block is routed: the operation recorded at the event's offset. A full record is
 * played here first, every track up to its operations (the block is serial up to there). */
static int op_sink(void *context, uint8_t track, const md_event_t *e)
{
    md_session_t *s = context;
    if (s->par_count >= s->par_limit) {
        played_t p = {0};
        for (uint32_t i = 0; i < s->par_count; i++) op_play(s, i, s->cursor, &p);
        played(s, &p);
        s->par_count = 0;
        s->par_flushed = 1;
    }
    s->par_queue[s->par_count].offset = s->par_offset;
    s->par_queue[s->par_count].track = track;
    s->par_queue[s->par_count].reset = !e;
    if (e) s->par_queue[s->par_count].event = *e;
    s->par_count++;
    return MD_OK;
}
/* the second core's job: its tracks' operations in order and their segments, to the block's end (one session
 * at a time). It reads the record and the cursors, writes its tracks' buffers and its result. */
static md_session_t *par_session;
static void par_job(uint32_t arg)
{
    md_session_t *s = par_session;
    uint32_t cursor[MD_TRACKS], mine = s->par_now;
    played_t p = {0};
    (void)arg;
    for (uint32_t t = 0; t < MD_TRACKS; t++) cursor[t] = mine >> t & 1u ? s->cursor[t] : 0u;   /* (its own only) */
    play(s, mine, 1, cursor, &p);
    s->par_result.errors = p.errors;
    s->par_result.rejected = p.rejected;
    s->par_result.last = p.last;
    s->par_result.fault = p.fault;
}
int md_session_render(md_session_t *s, int32_t *out, uint32_t frames)
{
    PROF(MD_PROF_STAGES);   /* start */
    if (!s || !out) return MD_ERR_ARGUMENT;
    if (!frames || frames > MD_MAX_RENDER_FRAMES) return MD_ERR_RANGE;
    md_session_snapshot_t *st = &s->stats;

    /* Flush requests seen now cover only commands already queued: take them first,
     * apply them after the drain. */
    uint8_t flush[MD_MAX_SOURCES], control[2];
    for (uint32_t i = 0; i < MD_MAX_SOURCES; i++) flush[i] = s->flush_request[i];
    control[0] = s->control_request[0];
    control[1] = s->control_request[1];
    MD_FENCE();
    for (uint32_t t = 0; s->fault_mask && t < MD_TRACKS; t++) if (s->fault_mask >> t & 1u) {
        /* an instrument refused an event the router had taken as done (the last block, played): as the router
         * does with a backend fault, its notes and the instrument cleared */
        md_event_t panic = {.kind = MD_EVENT_PANIC, .track = (uint8_t)t, .source = MD_SOURCE_NONE};
        (void)md_router_event(&s->router, &panic);
        s->router.backend_errors++;
    }
    s->fault_mask = 0;
    uint32_t commands = 0;
    md_command_t c;
    while (commands < MD_COMMAND_CAPACITY && md_ring_pop(&s->ring, &c)) {
        commands++;
        int status = apply_command(s, &c);
        if (status) { st->command_errors++; note_error(s, status); }
        else st->commands++;
    }
    if (commands > st->max_block_commands) st->max_block_commands = commands;
    for (uint32_t i = 0; i < MD_MAX_SOURCES; i++) {
        if (flush[i] == s->flush_seen[i]) continue;
        md_event_t e = {.kind = MD_EVENT_SOURCE_RELEASE, .track = MD_ALL_TRACKS, .source = (md_source_id_t)i};
        int status = md_router_event(&s->router, &e); /* also clears that source's sustain */
        if (status) note_error(s, status);
        st->flushes++;
        MD_FENCE();
        s->flush_seen[i] = flush[i];
    }
    if (control[0] != s->control_seen[0]) {
        (void)md_seq_stop(&s->seq);
        (void)md_transport_stop(&s->clock);
        st->flushes++;
        MD_FENCE();
        s->control_seen[0] = control[0];
    }
    if (control[1] != s->control_seen[1]) {
        int status = panic_all(s);
        if (status) note_error(s, status);
        st->flushes++;
        MD_FENCE();
        s->control_seen[1] = control[1];
    }

    PROF(MD_PROF_DRAIN);
    for (uint32_t t = 0; t < MD_TRACKS; t++) md_clear32(s->track_buffer[t], 2u * frames);
    PROF(MD_PROF_CLEAR);
    int status = md_seq_begin(&s->seq, &s->clock, frames);
    if (status) { note_error(s, status); return status; }
    if ((status = md_mixer_begin(&s->mixer, frames))) { note_error(s, status); return status; }   /* (each core adds
                                                             * its tracks as it finishes them) */
    uint32_t events = 0;
    uint32_t par = s->par_mask && !st->parallel_failed ? s->par_mask : 0u;   /* the tracks core 1 renders (0: one core) */
    for (uint32_t t = 0; t < MD_TRACKS; t++) s->cursor[t] = 0;
    md_seq_event_t e;
    if (par) {
        /* routed: every event of the block through the router, its instrument calls recorded; nothing rendered */
        s->par_count = 0;
        s->par_frames = frames;
        s->par_flushed = 0;
        s->router.sink = op_sink;
        s->router.sink_context = s;
        while ((status = md_seq_next(&s->seq, &e)) > 0) {
            s->par_offset = e.timed.offset < frames ? e.timed.offset : frames;
            events++;
            int applied = dispatch(s, &e.timed.event);
            if (applied) { st->event_errors++; note_error(s, applied); }
        }
        if (status < 0) {
            /* The sequencer stopped itself and scheduled a flush; release now as well (at the block's end). */
            st->seq_faults++;
            note_error(s, status);
            s->par_offset = frames;
            (void)release_sequencer_sources(s);
        }
        s->router.sink = NULL;
        PROF(MD_PROF_ROUTE);
        /* played: core 1 its tracks, this one the rest, both from the block's start */
        played_t own = {0};
        if (s->par_balance) par = balance(s, par);
        s->par_now = (uint8_t)par;
        par_session = s;
        int taken = s->parallel.run(par_job, 0);
        play(s, MD_ALL_TRACK_MASK & ~par, 0, s->cursor, &own);
        played(s, &own);
        if (!taken) { par_job(0); st->parallel_inline++; }
        if (taken && s->parallel.wait()) {
            /* held, possibly mid-event: not redone; its tracks panicked here (core 1 is stopped), then one core */
            for (uint32_t t = 0; t < MD_TRACKS; t++) if (par >> t & 1u) {
                md_event_t panic = {.kind = MD_EVENT_PANIC, .track = (uint8_t)t, .source = MD_SOURCE_NONE};
                (void)md_router_event(&s->router, &panic);
            }
            st->parallel_failed = 1;
        } else {
            played_t theirs = {s->par_result.errors, s->par_result.rejected, s->par_result.last, s->par_result.fault};
            played(s, &theirs);
            if (taken) st->parallel_blocks++;
        }
        if (s->par_flushed) st->parallel_fallbacks++;
    } else {
        while ((status = md_seq_next(&s->seq, &e)) > 0) {
            uint32_t at = e.timed.offset < frames ? e.timed.offset : frames;
            uint32_t mask = touched(&e.timed.event);
            events++;
            for (uint32_t t = 0; t < MD_TRACKS; t++) if (mask >> t & 1u) render_track(s, t, s->cursor, at);
            int applied = dispatch(s, &e.timed.event);
            if (applied) { st->event_errors++; note_error(s, applied); }
        }
        if (status < 0) {
            /* The sequencer stopped itself and scheduled a flush; release now as well. */
            st->seq_faults++;
            note_error(s, status);
            for (uint32_t t = 0; t < MD_TRACKS; t++) render_track(s, t, s->cursor, frames);
            (void)release_sequencer_sources(s);
        }
        for (uint32_t t = 0; t < MD_TRACKS; t++) {
            render_track(s, t, s->cursor, frames);
            if (s->instrument[t]) (void)md_mixer_add_track(&s->mixer, t, s->track_buffer[t]);
        }
    }
    st->events += events;
    if (events > st->max_block_events) st->max_block_events = events;

    PROF(MD_PROF_EVENTS);
    if ((status = md_mixer_finish(&s->mixer, out))) { note_error(s, status); return status; }
    PROF(MD_PROF_MIX);
    if ((status = md_transport_advance(&s->clock, frames))) { note_error(s, status); return status; }
    st->blocks++;
    publish(s);
    PROF(MD_PROF_PUBLISH);
    return MD_OK;
}
