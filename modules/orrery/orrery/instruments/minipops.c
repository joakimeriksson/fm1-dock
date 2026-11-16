/* SPDX-License-Identifier: GPL-3.0-only */
/* Direct percussion, no internal rhythm playback. Settings version 2: level, balance, then the
 * quijada, guiro and tambourine levels (kept in the engine's req_extra, which mp_trigger ignores). */
#include "minipops.h"
#include "adapter_common.h"
static const uint32_t rates[]={MD_SAMPLE_RATE};
static const uint8_t preset_bytes[]={80,50,70,70,70};
static const md_preset_t presets[]={{1,"MINIPOPS 7",preset_bytes,MD_MINIPOPS_SETTINGS_BYTES}};
static const md_drum_voice_desc_t voices[]={
    {MP_BD,"BASS DRUM",36},{MP_SD1,"SNARE 1",38},{MP_SD2,"SNARE 2",40},{MP_HH,"HI HAT",42},
    {MP_CY1,"CYMBAL 1",49},{MP_CY2,"CYMBAL 2",57},{MP_CB,"COWBELL",56},{MP_CO,"CONGA",64},
    {MP_LB,"LOW BONGO",67},{MP_SB,"SMALL BONGO",69},{MP_CL,"CLAVES",75},
    {MP_QU,"QUIJADA",58},{MP_GU,"GUIRO",73},{MP_TA,"TAMBOURINE",54}
};
static const md_param_desc_t params[]={
    {.id=MD_MINIPOPS_LEVEL,.flags=MD_PARAM_AUTOMATABLE,.name="LEVEL",.min=0,.max=100,.initial=80,.unit=MD_UNIT_PERCENT},
    {.id=MD_MINIPOPS_BALANCE,.flags=MD_PARAM_AUTOMATABLE,.name="BALANCE",.min=0,.max=100,.initial=50,.unit=MD_UNIT_PERCENT},
    {.id=MD_MINIPOPS_QUIJADA,.flags=MD_PARAM_AUTOMATABLE,.name="QUIJADA",.min=0,.max=100,.initial=70,.unit=MD_UNIT_PERCENT},
    {.id=MD_MINIPOPS_GUIRO,.flags=MD_PARAM_AUTOMATABLE,.name="GUIRO",.min=0,.max=100,.initial=70,.unit=MD_UNIT_PERCENT},
    {.id=MD_MINIPOPS_TAMBOURINE,.flags=MD_PARAM_AUTOMATABLE,.name="TAMBOURINE",.min=0,.max=100,.initial=70,.unit=MD_UNIT_PERCENT}
};
static int validate(const uint8_t *src,uint32_t bytes)
{
    if(!src)return MD_ERR_ARGUMENT;
    if(bytes!=MD_MINIPOPS_SETTINGS_BYTES)return MD_ERR_FORMAT;
    for(uint32_t i=0;i<MD_MINIPOPS_SETTINGS_BYTES;i++)if(src[i]>100u)return MD_ERR_RANGE;
    return MD_OK;
}
static int apply(void *state,const uint8_t *src,uint32_t bytes)
{
    if(!state)return MD_ERR_ARGUMENT;
    int result=validate(src,bytes);if(result)return result;
    mp_t *e=&((md_minipops_state_t *)state)->engine;
    mp_set_level(e,src[0],src[1]);mp_set_extra(e,src[2],src[3],src[4]);return MD_OK;
}
static int encode(const void *state,uint8_t *dst,uint32_t capacity)
{
    int result=md_adapter_encode_check(state,dst,capacity,MD_MINIPOPS_SETTINGS_BYTES);if(result)return result;
    const mp_t *e=&((const md_minipops_state_t *)state)->engine;
    dst[0]=e->req_volume;dst[1]=e->req_balance;dst[2]=e->req_extra[0];dst[3]=e->req_extra[1];dst[4]=e->req_extra[2];
    return MD_MINIPOPS_SETTINGS_BYTES;
}
static int init(void *state,uint32_t rate)
{
    if(!state)return MD_ERR_ARGUMENT;
    if(rate!=MD_SAMPLE_RATE)return MD_ERR_UNSUPPORTED;
    md_minipops_state_t *s=state;memset(s,0,sizeof *s);mp_init(&s->engine);
    return apply(s,preset_bytes,MD_MINIPOPS_SETTINGS_BYTES);
}
static void reset(void *state)
{
    md_minipops_state_t *s=state;
    uint8_t bytes[]={s->engine.req_volume,s->engine.req_balance,s->engine.req_extra[0],s->engine.req_extra[1],s->engine.req_extra[2]};
    memset(s,0,sizeof *s);mp_init(&s->engine);(void)apply(s,bytes,sizeof bytes);
}
static int get_param(const void *state,md_param_id_t id,int32_t *value)
{
    if(!state || !value)return MD_ERR_ARGUMENT;
    const mp_t *engine=&((const md_minipops_state_t *)state)->engine;
    if(id==MD_MINIPOPS_LEVEL)*value=engine->req_volume;
    else if(id==MD_MINIPOPS_BALANCE)*value=engine->req_balance;
    else if(id>=MD_MINIPOPS_QUIJADA && id<=MD_MINIPOPS_TAMBOURINE)*value=engine->req_extra[id-MD_MINIPOPS_QUIJADA];
    else return MD_ERR_NOT_FOUND;
    return MD_OK;
}
static int event(void *state,const md_event_t *e)
{
    if(!state || !e)return MD_ERR_ARGUMENT;
    md_minipops_state_t *s=state;
    if(e->flags)return MD_ERR_UNSUPPORTED;
    if(e->kind==MD_EVENT_PARAM){
        uint32_t id=e->data.param.id;
        if(id<MD_MINIPOPS_LEVEL || id>MD_MINIPOPS_TAMBOURINE)return MD_ERR_NOT_FOUND;
        if(e->data.param.value<0 || e->data.param.value>100)return MD_ERR_RANGE;
        if(id>=MD_MINIPOPS_QUIJADA){
            uint8_t x[3]={s->engine.req_extra[0],s->engine.req_extra[1],s->engine.req_extra[2]};
            x[id-MD_MINIPOPS_QUIJADA]=(uint8_t)e->data.param.value;
            mp_set_extra(&s->engine,x[0],x[1],x[2]);return MD_OK;
        }
        mp_set_level(&s->engine,id==MD_MINIPOPS_LEVEL ? (uint32_t)e->data.param.value : s->engine.req_volume,
            id==MD_MINIPOPS_BALANCE ? (uint32_t)e->data.param.value : s->engine.req_balance);
        return MD_OK;
    }
    if(e->kind==MD_EVENT_PANIC){reset(s);return MD_OK;}
    if(e->kind!=MD_EVENT_HIT)return MD_ERR_UNSUPPORTED;
    if(e->data.hit.voice>=MP_VOICES || !e->data.hit.velocity || e->data.hit.velocity>127u)return MD_ERR_RANGE;
    int32_t vel=(int32_t)e->data.hit.velocity*65536/127;
    if(e->data.hit.voice>=MP_QU){ /* the extra rhythms at their slider's level; muted at zero */
        vel=vel*s->engine.req_extra[e->data.hit.voice-MP_QU]/100;
        if(!vel)return MD_OK;
    }
    mp_trigger(&s->engine,e->data.hit.voice,vel);return MD_OK;
}
MD_HOT static void render(void *state,int32_t *out,uint32_t frames){mp_render(&((md_minipops_state_t *)state)->engine,out,frames);}
const md_instrument_desc_t MD_MINIPOPS={
    .processor={.id=MD_INSTRUMENT_MINIPOPS,.api_version=MD_API_VERSION,.settings_version=MD_MINIPOPS_SETTINGS_VERSION,
        .name="MINIPOPS",.state_bytes=sizeof(md_minipops_state_t),.state_alignment=_Alignof(md_minipops_state_t),
        .sample_rates=rates,.sample_rate_count=1,.params=params,.param_count=5,.max_settings_bytes=MD_MINIPOPS_SETTINGS_BYTES},
    .capabilities=MD_CAP_PERCUSSION|MD_CAP_VELOCITY,.max_polyphony=MP_VOICES,.presets=presets,.preset_count=1,
    .drum_voice_count=MP_VOICES,.drum_voices=voices,
    .init=init,.event=event,.get_param=get_param,.render_add=render,.reset=reset,.encode=encode,.validate=validate,.apply=apply
};
