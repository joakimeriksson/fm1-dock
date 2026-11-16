/* SPDX-License-Identifier: GPL-3.0-only */
/* Version1 payload: preset (0..13, Caravel's order), attack, release, ens, tone (0..127, as the engine's controls),
 * level (0..100). The engine runs in the audio context (events come through the session's render). */
#include "ensemble.h"
static const uint32_t rates[]={MD_SAMPLE_RATE};
static const char *const NAMES[ENS_N_PRESETS]={"VINTAGE STRINGS","GLASS CHOIR","DUSK ORCHESTRA","ABYSS PAD","PROFILE A",
    "STRING MACHINE","BREATH CHOIR","DEEP DRONE","GLASS SHIMMER","SLOW SWELL","ROUGH PULSE","SOLO DRIFT","HUGE ENSEMBLE","RUNNING PAD"};
static const uint8_t preset_bytes[ENS_N_PRESETS][MD_ENSEMBLE_SETTINGS_BYTES]={
    {0,60,80,70,80,100},{1,80,95,60,60,100},{2,75,90,85,70,100},{3,100,105,75,40,100},{4,85,95,70,80,100},
    {5,85,95,100,127,100},{6,90,100,40,90,100},{7,105,115,80,0,100},{8,55,100,10,127,100},{9,120,112,80,70,100},
    {10,50,60,70,110,100},{11,60,70,25,95,100},{12,70,100,127,110,100},{13,95,100,127,100,100}};
static const md_preset_t presets[]={
    {1,"VINTAGE STRINGS",preset_bytes[0],6},{2,"GLASS CHOIR",preset_bytes[1],6},{3,"DUSK ORCHESTRA",preset_bytes[2],6},
    {4,"ABYSS PAD",preset_bytes[3],6},{5,"PROFILE A",preset_bytes[4],6},{6,"STRING MACHINE",preset_bytes[5],6},
    {7,"BREATH CHOIR",preset_bytes[6],6},{8,"DEEP DRONE",preset_bytes[7],6},{9,"GLASS SHIMMER",preset_bytes[8],6},
    {10,"SLOW SWELL",preset_bytes[9],6},{11,"ROUGH PULSE",preset_bytes[10],6},{12,"SOLO DRIFT",preset_bytes[11],6},
    {13,"HUGE ENSEMBLE",preset_bytes[12],6},{14,"RUNNING PAD",preset_bytes[13],6}
};
static const md_param_desc_t params[]={
    {.id=MD_ENSEMBLE_PRESET,.flags=MD_PARAM_ENUM,.name="PRESET",.min=0,.max=ENS_N_PRESETS-1,.initial=0,
     .unit=MD_UNIT_INTEGER,.labels=NAMES,.label_count=ENS_N_PRESETS},
    {.id=MD_ENSEMBLE_ATTACK,.flags=MD_PARAM_AUTOMATABLE,.name="ATTACK",.min=0,.max=127,.initial=60,.unit=MD_UNIT_INTEGER},
    {.id=MD_ENSEMBLE_RELEASE,.flags=MD_PARAM_AUTOMATABLE,.name="RELEASE",.min=0,.max=127,.initial=80,.unit=MD_UNIT_INTEGER},
    {.id=MD_ENSEMBLE_ENS,.flags=MD_PARAM_AUTOMATABLE,.name="ENS",.min=0,.max=127,.initial=70,.unit=MD_UNIT_INTEGER},
    {.id=MD_ENSEMBLE_TONE,.flags=MD_PARAM_AUTOMATABLE,.name="TONE",.min=0,.max=127,.initial=80,.unit=MD_UNIT_INTEGER},
    {.id=MD_ENSEMBLE_LEVEL,.flags=MD_PARAM_AUTOMATABLE,.name="LEVEL",.min=0,.max=200,.initial=100,.unit=MD_UNIT_PERCENT}
};
static int validate(const uint8_t *src,uint32_t bytes)
{
    if(!src)return MD_ERR_ARGUMENT;
    if(bytes!=MD_ENSEMBLE_SETTINGS_BYTES)return MD_ERR_FORMAT;
    if(src[0]>=ENS_N_PRESETS || src[5]>200u)return MD_ERR_RANGE;
    for(uint32_t k=1;k<5u;k++)if(src[k]>127u)return MD_ERR_RANGE;
    return MD_OK;
}
static int apply(void *state,const uint8_t *src,uint32_t bytes)
{
    if(!state)return MD_ERR_ARGUMENT;
    int result=validate(src,bytes);if(result)return result;
    md_ensemble_state_t *s=state;ens_t *e=&s->engine;
    ens_load(e,src[0]);s->preset=src[0];
    e->env[ENS_ATK]=src[1];e->env[ENS_REL]=src[2];e->e[ENS_DEPTH]=src[3];e->e[ENS_TONE]=src[4];s->level=src[5];
    return MD_OK;
}
static int encode(const void *state,uint8_t *dst,uint32_t capacity)
{
    int result=md_adapter_encode_check(state,dst,capacity,MD_ENSEMBLE_SETTINGS_BYTES);if(result)return result;
    const md_ensemble_state_t *s=state;const ens_t *e=&s->engine;
    dst[0]=s->preset;dst[1]=e->env[ENS_ATK];dst[2]=e->env[ENS_REL];dst[3]=e->e[ENS_DEPTH];dst[4]=e->e[ENS_TONE];dst[5]=s->level;
    return MD_ENSEMBLE_SETTINGS_BYTES;
}
static int init(void *state,uint32_t rate)
{
    if(!state)return MD_ERR_ARGUMENT;
    if(rate!=MD_SAMPLE_RATE)return MD_ERR_UNSUPPORTED;
    md_ensemble_state_t *s=state;memset(s,0,sizeof *s);
    ens_init(&s->engine);s->level=100;return MD_OK;
}
static void reset(void *state)
{
    md_ensemble_state_t *s=state;
    ens_all_off(&s->engine);s->voice_count=0;memset(s->voices,0,sizeof s->voices);
}
static int get_param(const void *state,md_param_id_t id,int32_t *value)
{
    if(!state || !value)return MD_ERR_ARGUMENT;
    const md_ensemble_state_t *s=state;const ens_t *e=&s->engine;
    switch(id){
    case MD_ENSEMBLE_PRESET:*value=s->preset;return MD_OK;
    case MD_ENSEMBLE_ATTACK:*value=e->env[ENS_ATK];return MD_OK;
    case MD_ENSEMBLE_RELEASE:*value=e->env[ENS_REL];return MD_OK;
    case MD_ENSEMBLE_ENS:*value=e->e[ENS_DEPTH];return MD_OK;
    case MD_ENSEMBLE_TONE:*value=e->e[ENS_TONE];return MD_OK;
    case MD_ENSEMBLE_LEVEL:*value=s->level;return MD_OK;
    default:return MD_ERR_NOT_FOUND;
    }
}
static int event(void *state,const md_event_t *ev)
{
    if(!state || !ev)return MD_ERR_ARGUMENT;
    md_ensemble_state_t *s=state;ens_t *e=&s->engine;
    if(ev->kind==MD_EVENT_PARAM){
        if(ev->flags)return MD_ERR_UNSUPPORTED;
        uint32_t id=ev->data.param.id;int32_t v=ev->data.param.value;
        if(id<1u || id>sizeof params/sizeof params[0])return MD_ERR_NOT_FOUND;
        if(v<params[id-1u].min || v>params[id-1u].max)return MD_ERR_RANGE;
        switch(id){
        case MD_ENSEMBLE_PRESET:ens_load(e,(uint32_t)v);s->preset=(uint8_t)v;break;   /* the notes go on in the new sound */
        case MD_ENSEMBLE_ATTACK:e->env[ENS_ATK]=(uint8_t)v;break;
        case MD_ENSEMBLE_RELEASE:e->env[ENS_REL]=(uint8_t)v;break;
        case MD_ENSEMBLE_ENS:e->e[ENS_DEPTH]=(uint8_t)v;break;
        case MD_ENSEMBLE_TONE:e->e[ENS_TONE]=(uint8_t)v;break;
        default:s->level=(uint8_t)v;break;
        }
        return MD_OK;
    }
    if(ev->kind==MD_EVENT_PANIC){reset(s);return MD_OK;}
    if(ev->kind!=MD_EVENT_NOTE_ON && ev->kind!=MD_EVENT_NOTE_OFF)return MD_ERR_UNSUPPORTED;
    int result=md_adapter_note_check(ev);if(result)return result;
    int found=md_adapter_voice_find(s->voices,s->voice_count,ev->token);
    if(ev->kind==MD_EVENT_NOTE_OFF){
        if(found>=0){ens_note_off(e,s->voices[found].key);md_adapter_voice_remove(s->voices,&s->voice_count,(uint32_t)found);}
        return MD_OK;
    }
    if(found>=0)return MD_ERR_STATE;
    for(uint32_t i=0;i<s->voice_count;i++)if(s->voices[i].key==ev->data.note.key)return MD_ERR_STATE;
    if(s->voice_count==MD_ENSEMBLE_TOKENS)return MD_ERR_CAPACITY;
    s->voices[s->voice_count++]=(md_adapter_voice_t){ev->token,ev->data.note.key};
    ens_note_on(e,ev->data.note.key,ev->data.note.velocity);return MD_OK;
}
static void render(void *state,int32_t *out,uint32_t frames)
{
    md_ensemble_state_t *s=state;
    if(s->level==100u){ens_render(&s->engine,out,frames);return;}
    /* (another level, to 200: twice, for the pads that are quiet: the engine adds into a block of its own, then scaled) */
    int32_t buf[2u*ENS_CTL],g=(int32_t)s->level*328;
    while(frames){
        uint32_t n=frames<ENS_CTL ? frames : ENS_CTL;
        md_clear32(buf,2u*n);
        ens_render(&s->engine,buf,n);
        for(uint32_t i=0;i<2u*n;i++)out[i]+=(buf[i]>>4)*g>>11;
        out+=2u*n;frames-=n;
    }
}
const md_instrument_desc_t MD_ENSEMBLE={
    .processor={.id=MD_INSTRUMENT_ENSEMBLE,.api_version=MD_API_VERSION,.settings_version=MD_ENSEMBLE_SETTINGS_VERSION,
        .name="ENSEMBLE",.state_bytes=sizeof(md_ensemble_state_t),.state_alignment=_Alignof(md_ensemble_state_t),
        .sample_rates=rates,.sample_rate_count=1,.params=params,.param_count=sizeof params/sizeof params[0],
        .max_settings_bytes=MD_ENSEMBLE_SETTINGS_BYTES},
    .capabilities=MD_CAP_POLY|MD_CAP_VELOCITY,.max_polyphony=ENS_VOICES,.presets=presets,.preset_count=ENS_N_PRESETS,
    .init=init,.event=event,.get_param=get_param,.render_add=render,.reset=reset,.encode=encode,.validate=validate,.apply=apply
};
