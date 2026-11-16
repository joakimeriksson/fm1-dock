/* SPDX-License-Identifier: GPL-3.0-only */
#include "router.h"
#include "instrument_internal.h"
#include "processor_internal.h"

static void flush_track(md_router_t *r, uint8_t track)
{
    for (uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++) {
        if (r->notes[i].used && r->notes[i].track==track) r->notes[i]=(md_owned_note_t){0};
        if (r->voices[i].used && r->voices[i].track==track) r->voices[i]=(md_router_voice_t){0};
    }
    for (uint32_t i=0;i<MD_MAX_SOURCES;i++) r->sustain[track][i]=0;
    if (r->sink) (void)r->sink(r->sink_context,track,NULL);
    else md_instrument_reset(r->track[track]);
}
static int dispatch(md_router_t *r, uint8_t track, const md_event_t *e)
{
    int status=r->sink ? r->sink(r->sink_context,track,e) : md_instrument_event(r->track[track],e);
    if (status==MD_OK) return MD_OK;
    /* A hybrid instrument may have every physical slot occupied by held
     * pitches. Its transient HIT admission can fail without a processor fault;
     * preserve those owners and let the caller report the dropped hit. */
    if (e->kind==MD_EVENT_HIT && status==MD_ERR_CAPACITY) {
        r->rejected_notes++;return MD_ERR_CAPACITY;
    }
    r->backend_errors++; flush_track(r,track); return MD_ERR_STATE;
}
static int token_used(const md_router_t *r, md_note_token_t token)
{
    for (uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++)
        if ((r->notes[i].used && r->notes[i].token==token) ||
            (r->voices[i].used && r->voices[i].token==token)) return 1;
    return 0;
}
static int voice_find(const md_router_t *r, uint8_t track, uint8_t key, int mono)
{
    for (uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++)
        if (r->voices[i].used && r->voices[i].track==track && (mono || r->voices[i].key==key)) return (int)i;
    return -1;
}
static int free_voice(const md_router_t *r)
{
    for (uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++) if (!r->voices[i].used) return (int)i;
    return -1;
}
static md_event_t on_event(const md_owned_note_t *n)
{
    md_event_t e={0}; e.kind=MD_EVENT_NOTE_ON; e.track=n->track;
    e.source=n->source; e.token=n->token; e.flags=n->flags;
    e.data.note.key=n->key; e.data.note.velocity=n->velocity;
    return e;
}
static int off_voice(md_router_t *r, uint32_t index)
{
    md_router_voice_t v=r->voices[index];
    md_event_t e={0}; e.kind=MD_EVENT_NOTE_OFF; e.track=v.track; e.source=v.source; e.token=v.token;
    int status=dispatch(r,v.track,&e);
    if (!status) r->voices[index]=(md_router_voice_t){0};
    return status;
}
static int new_voice(md_router_t *r, uint32_t index, const md_owned_note_t *n)
{
    md_event_t e=on_event(n);
    int status=dispatch(r,n->track,&e);
    if (!status) r->voices[index]=(md_router_voice_t){.token=n->token,.source=n->source,
        .used=1,.track=n->track,.key=n->key};
    return status;
}
static const md_owned_note_t *mono_top(const md_router_t *r, uint8_t track)
{
    const md_owned_note_t *top=NULL;
    for (uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++) {
        const md_owned_note_t *n=&r->notes[i];
        if (!n->used || n->track!=track) continue;
        int live=n->source>=MD_SOURCE_PANEL;
        int top_live=top && top->source>=MD_SOURCE_PANEL;
        if (!top || live>top_live || (live==top_live && n->order>top->order)) top=n;
    }
    return top;
}
static int reconcile(md_router_t *r, uint8_t track)
{
    if (r->track[track]->desc->capabilities & MD_CAP_MONO) {
        const md_owned_note_t *top=mono_top(r,track);
        int old=voice_find(r,track,0,1);
        if (!top) return old>=0 ? off_voice(r,(uint32_t)old) : MD_OK;
        if (old>=0 && r->voices[old].key==top->key) return MD_OK;
        md_owned_note_t selected=*top; /* fault recovery may clear the note book */
        int slot=old>=0 ? old : free_voice(r);
        if (slot<0) return MD_ERR_CAPACITY;
        if (old>=0 && (selected.flags & (MD_NOTE_LEGATO|MD_NOTE_SLIDE))) {
            /* Mono legato replaces the effective physical token, then releases
             * the stale token; adapters must treat stale physical offs as no-op. */
            md_router_voice_t previous=r->voices[old];
            int status=new_voice(r,(uint32_t)slot,&selected);
            if (status) return status;
            md_event_t off={0}; off.kind=MD_EVENT_NOTE_OFF;off.track=track;
            off.source=previous.source;off.token=previous.token;
            return dispatch(r,track,&off);
        }
        if (old>=0) { int status=off_voice(r,(uint32_t)old); if (status) return status; }
        return new_voice(r,(uint32_t)slot,&selected);
    }
    /* Keep a shared physical token until the last logical owner at its pitch
     * leaves, including when the original owner of that token has gone. */
    for (uint32_t v=0;v<MD_MAX_OWNED_NOTES;v++) {
        if (!r->voices[v].used || r->voices[v].track!=track) continue;
        int keep=0;
        for (uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++)
            if (r->notes[i].used && r->notes[i].track==track && r->notes[i].key==r->voices[v].key) {keep=1;break;}
        if (!keep) { int status=off_voice(r,v); if (status) return status; }
    }
    return MD_OK;
}
int md_router_init(md_router_t *r, md_instrument_t *const tracks[MD_TRACKS])
{
    if (!r || !tracks) return MD_ERR_ARGUMENT;
    if (md_internal_overlap(r,sizeof *r,tracks,MD_TRACKS*sizeof *tracks)) return MD_ERR_ARGUMENT;
    for (uint32_t i=0;i<MD_TRACKS;i++) {
        if (!tracks[i]) continue;
        if (!tracks[i]->desc || !tracks[i]->state) return MD_ERR_STATE;
        if (md_internal_overlap(r,sizeof *r,tracks[i],sizeof *tracks[i]) ||
            md_internal_overlap(r,sizeof *r,tracks[i]->state,tracks[i]->desc->processor.state_bytes)) return MD_ERR_ARGUMENT;
        for (uint32_t j=0;j<i;j++) if (tracks[j] &&
            (tracks[i]==tracks[j] || md_internal_overlap(tracks[i]->state,tracks[i]->desc->processor.state_bytes,
                tracks[j]->state,tracks[j]->desc->processor.state_bytes))) return MD_ERR_ARGUMENT;
    }
    *r=(md_router_t){0};
    for (uint32_t i=0;i<MD_TRACKS;i++) r->track[i]=tracks[i];
    return MD_OK;
}
md_note_token_t md_router_next_token(md_router_t *r)
{
    if (!r) return MD_TOKEN_NONE;
    /* At most 128 logical plus 128 physical tokens can be outstanding. */
    for (uint32_t i=0;i<=2u*MD_MAX_OWNED_NOTES;i++) {
        r->next_token++;
        if (!r->next_token) r->next_token++;
        if (!token_used(r,r->next_token)) return r->next_token;
    }
    return MD_TOKEN_NONE;
}
uint32_t md_router_owned_count(const md_router_t *r)
{
    uint32_t count=0;
    if (r) for (uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++) count+=r->notes[i].used!=0;
    return count;
}
static int release_source(md_router_t *r, uint8_t track, uint16_t source)
{
    r->sustain[track][source]=0;
    for (uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++)
        if (r->notes[i].used && r->notes[i].track==track && r->notes[i].source==source)
            r->notes[i]=(md_owned_note_t){0};
    return r->track[track] ? reconcile(r,track) : MD_OK;
}
int md_router_sustain(md_router_t *r, uint8_t track, md_source_id_t source, uint32_t down)
{
    if (!r || track>=MD_TRACKS || !md_internal_source_valid(source) || down>1u) return MD_ERR_ARGUMENT;
    if (!r->track[track]) return MD_ERR_NOT_FOUND;
    r->sustain[track][source]=(uint8_t)down;
    if (down) return MD_OK;
    for (uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++)
        if (r->notes[i].used && r->notes[i].track==track && r->notes[i].source==source && r->notes[i].released)
            r->notes[i]=(md_owned_note_t){0};
    return reconcile(r,track);
}
int md_router_event(md_router_t *r, const md_event_t *e)
{
    if (!r || !e || e->reserved) return MD_ERR_ARGUMENT;
    if (e->kind==MD_EVENT_SOURCE_RELEASE || e->kind==MD_EVENT_PANIC) {
        if (e->flags || e->token || (e->track>=MD_TRACKS && e->track!=MD_ALL_TRACKS) ||
            (!md_internal_source_valid(e->source) && !(e->kind==MD_EVENT_PANIC && e->source==MD_SOURCE_NONE)))
            return MD_ERR_ARGUMENT;
        int result=MD_OK;
        for (uint8_t t=0;t<MD_TRACKS;t++) if (e->track==MD_ALL_TRACKS || e->track==t) {
            if (e->kind==MD_EVENT_PANIC) flush_track(r,t);
            else {int status=release_source(r,t,e->source);if (status) result=status;}
        }
        return result;
    }
    if (e->track>=MD_TRACKS) return MD_ERR_RANGE;
    int note_index=-1;
    if (e->kind==MD_EVENT_NOTE_OFF) {
        if (!e->token || !md_internal_source_valid(e->source)) return MD_ERR_RANGE;
        if (e->flags) return MD_ERR_ARGUMENT;
        for (uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++)
            if (r->notes[i].used && r->notes[i].token==e->token) {note_index=(int)i;break;}
        /* A valid late off remains harmless after panic/unbinding/replacement,
         * before consulting the new destination's pitched capability. */
        if (note_index<0) return MD_OK;
        if (r->notes[note_index].source!=e->source || r->notes[note_index].track!=e->track)
            return MD_ERR_ARGUMENT;
    }
    if (!r->track[e->track]) return MD_ERR_NOT_FOUND;
    int status=md_internal_instrument_event_check(r->track[e->track],e);
    if (status) return status;
    if (e->kind==MD_EVENT_NOTE_ON) {
        if (token_used(r,e->token)) return MD_ERR_STATE;
        int slot=-1;
        for (uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++) if (!r->notes[i].used) {slot=(int)i;break;}
        if (slot<0) {r->rejected_notes++;return MD_ERR_CAPACITY;}
        int mono=(r->track[e->track]->desc->capabilities & MD_CAP_MONO)!=0;
        int physical=voice_find(r,e->track,e->data.note.key,mono);
        if (!mono && physical<0) {
            uint32_t count=0;
            for (uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++) count+=r->voices[i].used && r->voices[i].track==e->track;
            if (count>=r->track[e->track]->desc->max_polyphony || free_voice(r)<0) {
                r->rejected_notes++; return MD_ERR_CAPACITY;
            }
        }
        if (r->order==UINT64_MAX) return MD_ERR_CAPACITY; /* explicit lifetime limit, never wrong priority */
        r->notes[slot]=(md_owned_note_t){.order=++r->order,.token=e->token,.source=e->source,
            .flags=e->flags,.used=1,.track=e->track,.key=e->data.note.key,.velocity=e->data.note.velocity};
        if (mono) return reconcile(r,e->track);
        return physical>=0 ? MD_OK : new_voice(r,(uint32_t)free_voice(r),&r->notes[slot]);
    }
    if (e->kind==MD_EVENT_NOTE_OFF) {
        if (r->sustain[e->track][e->source]) r->notes[note_index].released=1;
        else r->notes[note_index]=(md_owned_note_t){0};
        return reconcile(r,e->track);
    }
    return dispatch(r,e->track,e);
}
