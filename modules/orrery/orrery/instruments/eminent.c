/* SPDX-License-Identifier: GPL-3.0-only */
/* Version1 payload: reg,attack,release,ensemble,phaser,color,reserved0,volume.
 * Musical reverb is a session send, not an engine parameter. */
#include "eminent.h"
static const uint32_t rates[]={MD_SAMPLE_RATE};
static const char *const registrations[]={"STRINGS","CELLO","VIOLIN","FULL"};
static const uint8_t preset_bytes[][MD_EMINENT_SETTINGS_BYTES]={
    {0,45,55,70,25,1,0,80},{1,45,55,70,25,1,0,80},
    {2,45,55,70,25,1,0,80},{3,45,55,70,25,1,0,80}
};
static const md_preset_t presets[]={
    {1,"STRINGS",preset_bytes[0],8},{2,"CELLO",preset_bytes[1],8},
    {3,"VIOLIN",preset_bytes[2],8},{4,"FULL",preset_bytes[3],8}
};
static const md_param_desc_t params[]={
    {.id=MD_EMINENT_REGISTRATION,.flags=MD_PARAM_ENUM|MD_PARAM_AUTOMATABLE,.name="REGISTRATION",
     .min=0,.max=3,.initial=0,.unit=MD_UNIT_INTEGER,.labels=registrations,.label_count=4},
    {.id=MD_EMINENT_ATTACK,.flags=MD_PARAM_AUTOMATABLE,.name="ATTACK",.min=0,.max=100,.initial=45,.unit=MD_UNIT_PERCENT},
    {.id=MD_EMINENT_RELEASE,.flags=MD_PARAM_AUTOMATABLE,.name="RELEASE",.min=0,.max=100,.initial=55,.unit=MD_UNIT_PERCENT},
    {.id=MD_EMINENT_ENSEMBLE,.flags=MD_PARAM_AUTOMATABLE,.name="ENSEMBLE",.min=0,.max=100,.initial=70,.unit=MD_UNIT_PERCENT},
    {.id=MD_EMINENT_PHASER,.flags=MD_PARAM_AUTOMATABLE,.name="PHASER",.min=0,.max=100,.initial=25,.unit=MD_UNIT_PERCENT},
    {.id=MD_EMINENT_COLOR,.flags=MD_PARAM_AUTOMATABLE,.name="COLOR",.min=0,.max=1,.initial=1,.unit=MD_UNIT_BOOLEAN},
    {.id=MD_EMINENT_LEVEL,.flags=MD_PARAM_AUTOMATABLE,.name="LEVEL",.min=0,.max=100,.initial=80,.unit=MD_UNIT_PERCENT}
};
static const size_t offsets[]={offsetof(em_patch_t,reg),offsetof(em_patch_t,attack),offsetof(em_patch_t,release),
    offsetof(em_patch_t,ensemble),offsetof(em_patch_t,phaser),offsetof(em_patch_t,color),offsetof(em_patch_t,volume)};
static int validate(const uint8_t *src,uint32_t bytes)
{
    if(!src)return MD_ERR_ARGUMENT;
    if(bytes!=MD_EMINENT_SETTINGS_BYTES)return MD_ERR_FORMAT;
    if(src[0]>=EM_N_REG || src[5]>1u || src[6])return MD_ERR_RANGE;
    for(uint32_t i=1;i<5;i++)if(src[i]>100u)return MD_ERR_RANGE;
    return src[7]<=100u ? MD_OK : MD_ERR_RANGE;
}
static int apply(void *state,const uint8_t *src,uint32_t bytes)
{
    if(!state)return MD_ERR_ARGUMENT;
    int result=validate(src,bytes);if(result)return result;
    em_patch_t patch={.reg=src[0],.attack=src[1],.release=src[2],.ensemble=src[3],
        .phaser=src[4],.color=src[5],.reverb=0,.volume=src[7]};
    em_set_patch(&((md_eminent_state_t *)state)->engine,&patch);return MD_OK;
}
static int encode(const void *state,uint8_t *dst,uint32_t capacity)
{
    int result=md_adapter_encode_check(state,dst,capacity,MD_EMINENT_SETTINGS_BYTES);if(result)return result;
    const em_patch_t *p=&((const md_eminent_state_t *)state)->engine.p;
    dst[0]=p->reg;dst[1]=p->attack;dst[2]=p->release;dst[3]=p->ensemble;
    dst[4]=p->phaser;dst[5]=p->color;dst[6]=0;dst[7]=p->volume;return MD_EMINENT_SETTINGS_BYTES;
}
static int init(void *state,uint32_t rate)
{
    if(!state)return MD_ERR_ARGUMENT;
    if(rate!=MD_SAMPLE_RATE)return MD_ERR_UNSUPPORTED;
    md_eminent_state_t *s=state;memset(s,0,sizeof *s);em_init(&s->engine);
    return apply(s,preset_bytes[0],MD_EMINENT_SETTINGS_BYTES);
}
static void reset(void *state)
{
    md_eminent_state_t *s=state;em_patch_t patch=s->engine.p;
    memset(s,0,sizeof *s);em_init(&s->engine);em_set_patch(&s->engine,&patch);
}
static int get_param(const void *state,md_param_id_t id,int32_t *value)
{
    if(!state || !value)return MD_ERR_ARGUMENT;
    if(id<1u || id>sizeof params/sizeof params[0])return MD_ERR_NOT_FOUND;
    *value=*((const uint8_t *)&((const md_eminent_state_t *)state)->engine.p+offsets[id-1u]);return MD_OK;
}
static int event(void *state,const md_event_t *e)
{
    if(!state || !e)return MD_ERR_ARGUMENT;
    md_eminent_state_t *s=state;
    if(e->kind==MD_EVENT_PARAM){
        if(e->flags)return MD_ERR_UNSUPPORTED;
        uint32_t id=e->data.param.id;
        if(id<1u || id>sizeof params/sizeof params[0])return MD_ERR_NOT_FOUND;
        if(e->data.param.value<params[id-1u].min || e->data.param.value>params[id-1u].max)return MD_ERR_RANGE;
        em_patch_t patch=s->engine.p;
        *((uint8_t *)&patch+offsets[id-1u])=(uint8_t)e->data.param.value;
        em_set_patch(&s->engine,&patch);return MD_OK;
    }
    if(e->kind==MD_EVENT_PANIC){reset(s);return MD_OK;}
    if(e->kind!=MD_EVENT_NOTE_ON && e->kind!=MD_EVENT_NOTE_OFF)return MD_ERR_UNSUPPORTED;
    int result=md_adapter_note_check(e);if(result)return result;
    int found=md_adapter_voice_find(s->voices,s->voice_count,e->token);
    if(e->kind==MD_EVENT_NOTE_OFF){
        if(found>=0){em_note_off(&s->engine,s->voices[found].key);md_adapter_voice_remove(s->voices,&s->voice_count,(uint32_t)found);}
        return MD_OK;
    }
    if(found>=0)return MD_ERR_STATE;
    for(uint32_t i=0;i<s->voice_count;i++)if(s->voices[i].key==e->data.note.key)return MD_ERR_STATE;
    if(s->voice_count==EM_VOICES)return MD_ERR_CAPACITY;
    s->voices[s->voice_count++]=(md_adapter_voice_t){e->token,e->data.note.key};
    em_note_on(&s->engine,e->data.note.key,e->data.note.velocity);return MD_OK;
}
MD_HOT static void render(void *state,int32_t *out,uint32_t frames){em_render(&((md_eminent_state_t *)state)->engine,out,frames);}
const md_instrument_desc_t MD_EMINENT={
    .processor={.id=MD_INSTRUMENT_EMINENT,.api_version=MD_API_VERSION,.settings_version=MD_EMINENT_SETTINGS_VERSION,
        .name="EMINENT",.state_bytes=sizeof(md_eminent_state_t),.state_alignment=_Alignof(md_eminent_state_t),
        .sample_rates=rates,.sample_rate_count=1,.params=params,.param_count=sizeof params/sizeof params[0],
        .max_settings_bytes=MD_EMINENT_SETTINGS_BYTES},
    .capabilities=MD_CAP_POLY,.max_polyphony=EM_VOICES,.presets=presets,.preset_count=4,
    .init=init,.event=event,.get_param=get_param,.render_add=render,.reset=reset,.encode=encode,.validate=validate,.apply=apply
};
