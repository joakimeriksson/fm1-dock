/* SPDX-License-Identifier: GPL-3.0-only */
#include "seq.h"
#include <stddef.h>

static const uint8_t chord[MD_CHORD_COUNT][4]={
    {0,4,7,12},{0,3,7,12},{0,4,7,11},{0,3,7,10},{0,2,7,12},{0,5,7,12},{0,7,12,19}
};
static int pos_valid(md_position_t p){return !p.reserved && p.unit<MD_UNITS_PER_QUARTER;}
int md_seq_bank_validate(const md_seq_bank_t *b)
{
    if(!b)return MD_ERR_ARGUMENT;
    if(b->lock_count>MD_MAX_LOCKS)return MD_ERR_CAPACITY;
    for(uint32_t i=0;i<3;i++)if(b->global_owner[i]>=MD_TRACKS && b->global_owner[i]!=MD_AUTOMATION_UNOWNED)return MD_ERR_RANGE;
    for(uint32_t t=0;t<MD_TRACKS;t++)for(uint32_t p=0;p<MD_PATTERNS_PER_TRACK;p++){
        const md_seq_pattern_t *pat=&b->pattern[t][p];const md_pattern_meta_t *m=&pat->meta;
        if(!m->length || m->length>64 || m->division>MD_DIV_BAR || m->swing<50 || m->swing>75 ||
            m->transpose < -48 || m->transpose>48 || m->mode>MD_PATTERN_DRUM || m->flags || m->reserved ||
            (m->mode==MD_PATTERN_DRUM && m->transpose))return MD_ERR_RANGE;
        for(uint32_t k=0;k<MD_STEPS_PER_PATTERN;k++){
            const md_step_t *s=&pat->step[k];
            if(s->gate>200 || s->velocity>127 || (s->gate && !s->velocity) || s->probability>100 ||
                (s->flags & ~MD_STEP_FLAGS) ||
                ((s->flags & (MD_STEP_TIE|MD_STEP_SLIDE))==(MD_STEP_TIE|MD_STEP_SLIDE)) ||
                (!s->gate && (s->flags & (MD_STEP_TIE|MD_STEP_SLIDE))))return MD_ERR_RANGE;
            if(m->mode==MD_PATTERN_DRUM){
                if((s->payload & 0xc000u) || (s->flags & (MD_STEP_TIE|MD_STEP_SLIDE)))return MD_ERR_RANGE;
            }else if(m->mode==MD_PATTERN_MONO){if(s->payload>127)return MD_ERR_RANGE;}
            else if((s->payload & 255u)>127 || ((s->payload>>8)&7u)>=MD_CHORD_COUNT || (s->payload & 0xc000u) || (s->flags & MD_STEP_SLIDE))return MD_ERR_RANGE;
        }
    }
    for(uint32_t i=0;i<b->lock_count;i++){
        const md_lock_t *l=&b->locks[i];uint32_t group=l->target>>12;
        if(l->step>=MD_TRACKS*MD_PATTERNS_PER_TRACK*MD_STEPS_PER_PATTERN ||
            (i && b->locks[i-1].step>=l->step) || !(l->target & MD_TARGET_LOCAL_MASK) || group>6)return MD_ERR_RANGE;
        if(group>=4 && b->global_owner[group-4]!=l->step/(MD_PATTERNS_PER_TRACK*MD_STEPS_PER_PATTERN))return MD_ERR_RANGE;
    }
    return MD_OK;
}
int md_seq_init(md_seq_t *s,const md_seq_bank_t *b,md_seq_callbacks_t callbacks)
{
    if(!s || !callbacks.token)return MD_ERR_ARGUMENT;
    int status=md_seq_bank_validate(b);if(status)return status;
    if(b->lock_count && !callbacks.base_value)return MD_ERR_ARGUMENT;
    *s=(md_seq_t){0};s->bank=b;s->callbacks=callbacks;return MD_OK;
}
static int idle(const md_seq_t *s){return s && s->bank && !s->iterating;}
int md_seq_start(md_seq_t *s,md_position_t p)
{
    if(!idle(s))return MD_ERR_STATE;
    if(!pos_valid(p))return MD_ERR_RANGE;
    s->running=1;s->flush_mask=MD_ALL_TRACK_MASK;
    for(uint32_t t=0;t<MD_TRACKS;t++){
        s->track[t].next=p;s->track[t].cursor=0;s->track[t].visits=0;s->track[t].queued=0;
    }
    return MD_OK;
}
int md_seq_stop(md_seq_t *s)
{
    if(!idle(s))return MD_ERR_STATE;
    s->running=0;s->flush_mask=MD_ALL_TRACK_MASK;return MD_OK;
}
int md_seq_continue(md_seq_t *s)
{
    if(!idle(s))return MD_ERR_STATE;
    s->running=1;return MD_OK;
}
int md_seq_mute(md_seq_t *s,uint8_t track,uint32_t mute)
{
    if(!idle(s))return MD_ERR_STATE;
    if(track>=MD_TRACKS || mute>1)return MD_ERR_RANGE;
    if(mute && !s->track[track].muted)s->flush_mask|=(uint8_t)(1u<<track);
    s->track[track].muted=(uint8_t)mute;return MD_OK;
}
int md_seq_launch(md_seq_t *s,const md_transport_t *clock,uint8_t track,uint8_t pattern,uint32_t immediate)
{
    if(!idle(s))return MD_ERR_STATE;
    if(!clock || !pos_valid(clock->position) || track>=MD_TRACKS || pattern>=MD_PATTERNS_PER_TRACK || immediate>1)return MD_ERR_RANGE;
    md_transport_t check=*clock;if(md_transport_advance(&check,0))return MD_ERR_STATE;
    md_seq_track_t *t=&s->track[track];
    if(!s->running || !clock->running){
        t->pattern=pattern;t->cursor=0;t->visits=0;t->next=clock->position;t->queued=0;
        s->flush_mask|=(uint8_t)(1u<<track);return MD_OK;
    }
    t->launch_at=clock->position;
    if(immediate){if(clock->remainder)md_position_add(t->launch_at,1,&t->launch_at);}
    else{t->launch_at.quarter+=4u-t->launch_at.quarter%4u;t->launch_at.unit=0;}
    t->queued=1;t->requested=pattern;return MD_OK;
}
int md_seq_begin(md_seq_t *s,const md_transport_t *clock,uint32_t frames)
{
    if(!idle(s))return MD_ERR_STATE;
    if(!clock || !frames || frames>MD_MAX_RENDER_FRAMES)return MD_ERR_RANGE;
    md_transport_t check=*clock;
    if(md_transport_advance(&check,0))return MD_ERR_STATE;
    s->clock=*clock;s->frames=frames;s->batch_count=0;s->batch_index=0;s->emitted=0;s->visited=0;s->offset=0;s->iterating=1;
    return MD_OK;
}
static int add(md_seq_t *s,md_event_t e,uint8_t priority)
{
    if(s->batch_count>=MD_SEQ_BATCH_MAX)return MD_ERR_CAPACITY;
    s->batch[s->batch_count]=e;s->priority[s->batch_count++]=priority;return MD_OK;
}
static md_event_t param(uint8_t track,uint16_t target,int32_t value)
{
    md_event_t e={.kind=MD_EVENT_PARAM,.track=track,.source=track};e.data.param.id=target;e.data.param.value=value;return e;
}
static int restore(md_seq_t *s,uint8_t track)
{
    md_seq_lock_state_t *l=&s->track[track].lock;if(!l->active)return MD_OK;
    int32_t base=l->base;
    if(!s->callbacks.base_value || s->callbacks.base_value(s->callbacks.context,track,l->target,&base)){
        s->base_errors++;base=l->base; /* bounded fallback avoids leaving the old lock sticky */
    }
    int status=add(s,param(track,l->target,base),MD_SEQ_RESTORE);if(!status)l->active=0;return status;
}
static int flush_notes(md_seq_t *s,uint8_t track)
{
    md_event_t e={.kind=MD_EVENT_SOURCE_RELEASE,.track=track,.source=track};
    int status=add(s,e,MD_SEQ_RELEASE);if(status)return status;
    for(uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++)if(s->notes[i].used && s->notes[i].track==track)s->notes[i]=(md_seq_note_t){0};
    s->track[track].last_count=0;return MD_OK;
}
static int flush(md_seq_t *s,uint8_t track)
{
    int status=restore(s,track);return status?status:flush_notes(s,track);
}
static int due(const md_seq_t *s,md_position_t p,uint32_t offset)
{
    uint32_t at;return md_transport_offset(&s->clock,p,s->frames,&at)==MD_OK && at<=offset;
}
static int earliest(const md_seq_t *s,uint32_t *offset)
{
    if(s->flush_mask){*offset=0;return 1;}
    if(!s->running || !s->clock.running)return 0;
    uint32_t at,min=s->frames;
    for(uint32_t t=0;t<MD_TRACKS;t++){
        if(md_transport_offset(&s->clock,s->track[t].next,s->frames,&at)==MD_OK && at<min)min=at;
        if(s->track[t].queued && md_transport_offset(&s->clock,s->track[t].launch_at,s->frames,&at)==MD_OK && at<min)min=at;
    }
    for(uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++)if(s->notes[i].used &&
        md_transport_offset(&s->clock,s->notes[i].off,s->frames,&at)==MD_OK && at<min)min=at;
    if(min==s->frames)return 0;*offset=min;return 1;
}
static uint32_t chance(uint32_t seed,uint32_t track,uint32_t pattern,uint64_t visit)
{
    uint32_t x=seed ^ ((track+1u)*0x9e3779b9u) ^ ((pattern+1u)*0x85ebca6bu) ^ (uint32_t)visit ^ (uint32_t)(visit>>32);
    x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return (x^(x>>16))%100u;
}
static const md_lock_t *find_lock(const md_seq_t *s,uint8_t track,uint8_t step)
{
    uint32_t ref=(track*MD_PATTERNS_PER_TRACK+s->track[track].pattern)*MD_STEPS_PER_PATTERN+step;
    for(uint32_t i=0;i<s->bank->lock_count;i++)if(s->bank->locks[i].step==ref)return &s->bank->locks[i];
    return NULL;
}
static int latest(const md_seq_track_t *t,md_note_token_t token)
{
    for(uint32_t i=0;i<t->last_count;i++)if(t->last[i]==token)return 1;return 0;
}
typedef struct {
    md_step_t step;
    md_position_t on,off;
    uint8_t mode,trigger;
    int8_t transpose;
} step_plan_t;
static int prepare(md_seq_t *s,uint8_t track,uint32_t offset,step_plan_t *plan)
{
    if(s->visited>=MD_SEQ_STEP_LIMIT)return MD_ERR_CAPACITY;s->visited++;
    md_seq_track_t *t=&s->track[track];
    const md_seq_pattern_t *p=&s->bank->pattern[track][t->pattern];
    if(t->cursor>=p->meta.length)t->cursor=0;
    uint8_t cursor=t->cursor;
    const md_step_t *step=&p->step[cursor];md_position_t on=t->next;
    uint32_t span=md_step_span_units(p->meta.division,p->meta.swing,(uint8_t)(t->visits&1u));
    md_position_add(on,span,&t->next);
    t->cursor++;if(t->cursor>=p->meta.length)t->cursor=0;
    int play=chance(s->bank->seed,track,t->pattern,t->visits)<step->probability;t->visits++;
    int status=restore(s,track);if(status)return status;
    const md_lock_t *lock=find_lock(s,track,cursor);
    if(lock && !t->muted){
        int32_t base;
        if(!s->callbacks.base_value || s->callbacks.base_value(s->callbacks.context,track,lock->target,&base))s->base_errors++;
        else{
            status=add(s,param(track,lock->target,lock->value),MD_SEQ_LOCK);if(status)return status;
            t->lock=(md_seq_lock_state_t){.target=lock->target,.active=1,.base=base};
        }
    }
    if(t->muted)return MD_OK;
    if(!play || !step->gate)return flush_notes(s,track);
    md_position_t off;md_position_add(on,md_step_straight_units(p->meta.division)*step->gate/100u,&off);
    const md_step_t *next=&p->step[t->cursor];
    /* A following tie or slide must have a predecessor even when its original
     * gate was short. Chance/rest at that next visit explicitly releases it. */
    if(next->gate && (next->flags & (MD_STEP_TIE|MD_STEP_SLIDE))){
        uint32_t gate=md_step_straight_units(p->meta.division)*step->gate/100u;
        if(gate<span)off=t->next;
    }
    if(step->flags & MD_STEP_TIE){
        for(uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++)if(s->notes[i].used && s->notes[i].track==track && latest(t,s->notes[i].token))
            s->notes[i].off=off;
        return MD_OK;
    }
    int slide=p->meta.mode==MD_PATTERN_MONO && (step->flags & MD_STEP_SLIDE);
    if(p->meta.mode==MD_PATTERN_MONO && !slide){status=flush_notes(s,track);if(status)return status;}
    int predecessor=0;
    if(slide)for(uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++)if(s->notes[i].used && s->notes[i].track==track){
        s->notes[i].off=on;s->notes[i].cleanup=1;predecessor=1;
    }
    md_step_t planned=*step;if(slide && !predecessor)planned.flags&=(uint8_t)~MD_STEP_SLIDE;
    *plan=(step_plan_t){.step=planned,.on=on,.off=off,.mode=p->meta.mode,.trigger=1,.transpose=p->meta.transpose};
    (void)offset;return MD_OK;
}
static int token_exists(const md_seq_t *s,md_note_token_t token)
{
    for(uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++)if(s->notes[i].used && s->notes[i].token==token)return 1;return 0;
}
static int trigger_note(md_seq_t *s,uint8_t track,const step_plan_t *p,int32_t key)
{
    if(key<0 || key>127){s->dropped_notes++;return MD_OK;}
    int slot=-1;for(uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++)if(!s->notes[i].used){slot=(int)i;break;}
    if(slot<0){s->dropped_notes++;return MD_OK;}
    md_note_token_t token=s->callbacks.token(s->callbacks.context);
    if(!token || token_exists(s,token)){s->dropped_notes++;return MD_OK;}
    md_event_t e={.kind=MD_EVENT_NOTE_ON,.track=track,.source=track,.token=token};
    e.data.note.key=(uint8_t)key;e.data.note.velocity=p->step.velocity;
    if(p->step.flags & MD_STEP_ACCENT)e.flags|=MD_NOTE_ACCENT;
    if(p->step.flags & MD_STEP_SLIDE)e.flags|=MD_NOTE_SLIDE;
    int status=add(s,e,MD_SEQ_TRIGGER);if(status)return status;
    s->notes[slot]=(md_seq_note_t){.off=p->off,.token=token,.used=1,.track=track};
    md_seq_track_t *t=&s->track[track];if(t->last_count<4)t->last[t->last_count++]=token;return MD_OK;
}
static int triggers(md_seq_t *s,uint8_t track,const step_plan_t *p)
{
    if(!p->trigger)return MD_OK;s->track[track].last_count=0;
    if(p->mode==MD_PATTERN_DRUM){
        for(uint16_t v=0;v<MD_DRUM_VOICES;v++)if(p->step.payload & (1u<<v)){
            md_event_t e={.kind=MD_EVENT_HIT,.track=track,.source=track};
            e.data.hit.voice=v;e.data.hit.velocity=(p->step.flags & MD_STEP_ACCENT)?127:p->step.velocity;
            int status=add(s,e,MD_SEQ_TRIGGER);if(status)return status;
        }
        return MD_OK;
    }
    int32_t root=(int32_t)(p->step.payload & 255u)+(int32_t)p->transpose;
    if(p->mode==MD_PATTERN_MONO)return trigger_note(s,track,p,root);
    for(uint32_t i=0;i<4;i++){
        /* MD_CHORD_RAISE: tone i (1..3) an octave up, an open voicing (the root stays) */
        int32_t up=i && (p->step.payload & (1u<<(10u+i)))?12:0;
        int status=trigger_note(s,track,p,root+chord[(p->step.payload>>8)&7u][i]+up);if(status)return status;
    }
    return MD_OK;
}
static int build_batch(md_seq_t *s,uint32_t offset)
{
    step_plan_t plans[MD_TRACKS]={0};s->batch_count=0;s->batch_index=0;s->offset=offset;
    for(uint8_t t=0;t<MD_TRACKS;t++)if(s->flush_mask & (1u<<t)){int status=flush(s,t);if(status)return status;}
    s->flush_mask=0;
    if(s->running && s->clock.running)for(uint8_t t=0;t<MD_TRACKS;t++){
        md_seq_track_t *track=&s->track[t];
        if(track->queued && due(s,track->launch_at,offset)){
            int status=flush(s,t);if(status)return status;
            track->pattern=track->requested;track->cursor=0;track->visits=0;track->next=track->launch_at;track->queued=0;
        }
        if(due(s,track->next,offset)){int status=prepare(s,t,offset,&plans[t]);if(status)return status;}
    }
    for(uint32_t i=0;i<MD_MAX_OWNED_NOTES;i++)if(s->notes[i].used && due(s,s->notes[i].off,offset)){
        md_seq_note_t *n=&s->notes[i];md_event_t e={.kind=MD_EVENT_NOTE_OFF,.track=n->track,.source=n->track,.token=n->token};
        int status=add(s,e,n->cleanup?MD_SEQ_LEGATO_CLEANUP:MD_SEQ_RELEASE);if(status)return status;*n=(md_seq_note_t){0};
    }
    for(uint8_t t=0;t<MD_TRACKS;t++){int status=triggers(s,t,&plans[t]);if(status)return status;}
    uint32_t index=0;
    for(uint8_t priority=MD_SEQ_RESTORE;priority<=MD_SEQ_LEGATO_CLEANUP;priority++)
        for(uint32_t i=0;i<s->batch_count;i++)if(s->priority[i]==priority)s->order[index++]=(uint16_t)i;
    return MD_OK;
}
int md_seq_next(md_seq_t *s,md_seq_event_t *out)
{
    if(!s || !out)return MD_ERR_ARGUMENT;if(!s->iterating)return 0;
    while(s->batch_index==s->batch_count){
        uint32_t offset;if(!earliest(s,&offset)){s->iterating=0;return 0;}
        int status=build_batch(s,offset);
        if(status){s->overflows++;s->iterating=0;s->running=0;s->flush_mask=MD_ALL_TRACK_MASK;return status;}
    }
    if(s->emitted>=MD_BLOCK_EVENT_LIMIT){s->overflows++;s->iterating=0;s->running=0;s->flush_mask=MD_ALL_TRACK_MASK;return MD_ERR_CAPACITY;}
    uint32_t index=s->order[s->batch_index++];
    *out=(md_seq_event_t){.timed={.offset=s->offset,.event=s->batch[index]},.priority=s->priority[index]};
    s->emitted++;return 1;
}
