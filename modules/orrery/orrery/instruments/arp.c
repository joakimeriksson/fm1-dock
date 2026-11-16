/* SPDX-License-Identifier: GPL-3.0-only */
/* Adapter and explicit version-1 sound payload. Preset values are derived from
 * the imported GPL-3.0-only engines/arp_presets.h; engine files remain unchanged.
 * Wire order is documented by fields[]; names/history/1601 state are not stored.
 * Byte33 is reserved zero: musical reverb belongs to the session. */
#include "arp.h"

static const uint32_t rates[]={MD_SAMPLE_RATE};
typedef struct { size_t offset; uint8_t min,max; } field_t;
static const field_t fields[MD_ARP_SETTINGS_BYTES]={
    {offsetof(arp_patch_t,vco1_wave),0,2},
    {offsetof(arp_patch_t,vco1_oct),0,4},
    {offsetof(arp_patch_t,vco1_pw),5,95},
    {offsetof(arp_patch_t,vco1_lvl),0,100},
    {offsetof(arp_patch_t,vco2_wave),0,2},
    {offsetof(arp_patch_t,vco2_oct),0,4},
    {offsetof(arp_patch_t,vco2_detune),0,100},
    {offsetof(arp_patch_t,vco2_lvl),0,100},
    {offsetof(arp_patch_t,noise_lvl),0,100},
    {offsetof(arp_patch_t,ring_lvl),0,100},
    {offsetof(arp_patch_t,sync),0,1},
    {offsetof(arp_patch_t,sub_lvl),0,100},
    {offsetof(arp_patch_t,cutoff),0,100},
    {offsetof(arp_patch_t,reso),0,100},
    {offsetof(arp_patch_t,env_amt),0,100},
    {offsetof(arp_patch_t,kbd),0,100},
    {offsetof(arp_patch_t,lfo_vcf),0,100},
    {offsetof(arp_patch_t,sh_vcf),0,100},
    {offsetof(arp_patch_t,drive),0,100},
    {offsetof(arp_patch_t,glide),0,100},
    {offsetof(arp_patch_t,a1),0,100},
    {offsetof(arp_patch_t,d1),0,100},
    {offsetof(arp_patch_t,s1),0,100},
    {offsetof(arp_patch_t,r1),0,100},
    {offsetof(arp_patch_t,a2),0,100},
    {offsetof(arp_patch_t,d2),0,100},
    {offsetof(arp_patch_t,s2),0,100},
    {offsetof(arp_patch_t,r2),0,100},
    {offsetof(arp_patch_t,lfo_wave),0,3},
    {offsetof(arp_patch_t,lfo_rate),0,100},
    {offsetof(arp_patch_t,lfo_pitch),0,100},
    {offsetof(arp_patch_t,lfo_pw),0,100},
    {offsetof(arp_patch_t,volume),0,100},
    {offsetof(arp_patch_t,reverb),0,0},
    {offsetof(arp_patch_t,accent),0,100},
    {offsetof(arp_patch_t,env_pitch),0,100},
};
static const uint8_t preset_bytes[10][MD_ARP_SETTINGS_BYTES]={
    {0,2,50,80,0,2,56,70,0,0,0,50,38,12,40,50,0,0,25,0,8,50,25,25,12,60,70,25,0,30,0,0,80,0,25,0},
    {1,2,40,80,0,2,53,60,0,0,0,20,36,42,50,30,0,0,25,0,6,52,10,30,8,60,25,40,0,30,0,20,80,0,35,0},
    {1,2,50,70,1,2,50,60,0,0,0,40,42,28,30,40,18,0,25,0,6,48,0,25,8,56,10,30,0,10,0,0,80,0,35,0},
    {0,2,50,80,0,2,58,80,0,0,0,0,60,25,30,60,0,0,35,35,8,50,60,30,10,30,90,30,0,45,3,0,75,0,30,0},
    {0,2,50,0,0,3,50,100,0,0,1,0,70,10,0,50,0,0,35,0,0,55,0,40,6,60,60,40,0,30,0,0,75,0,30,60},
    {1,2,50,70,2,2,50,60,0,0,0,0,40,55,10,30,0,60,25,0,20,40,80,50,20,40,80,50,3,55,0,0,75,0,0,0},
    {1,3,50,50,0,2,70,50,60,50,0,0,55,40,70,30,0,0,45,0,0,25,0,15,4,25,0,10,0,30,0,0,80,0,60,0},
    {0,2,50,75,1,2,54,55,0,0,0,10,48,45,55,40,12,0,30,0,6,52,10,30,8,55,15,38,0,8,0,0,80,0,30,0},
    {2,1,50,80,0,1,50,15,0,0,0,25,28,8,22,50,0,0,20,0,8,45,40,25,14,55,60,25,0,30,0,0,85,0,25,0},
    {0,2,50,80,0,2,45,80,0,0,0,0,50,25,38,40,14,0,30,0,6,48,20,30,10,44,50,35,0,12,0,30,80,0,35,0},
};
static const md_preset_t presets[]={
    {1u,"WARM BASS",preset_bytes[0],MD_ARP_SETTINGS_BYTES},
    {2u,"PLUCK SEQ",preset_bytes[1],MD_ARP_SETTINGS_BYTES},
    {3u,"GLASS PLUCK",preset_bytes[2],MD_ARP_SETTINGS_BYTES},
    {4u,"ARP LEAD",preset_bytes[3],MD_ARP_SETTINGS_BYTES},
    {5u,"LASER SYNC",preset_bytes[4],MD_ARP_SETTINGS_BYTES},
    {6u,"S&H SPACE",preset_bytes[5],MD_ARP_SETTINGS_BYTES},
    {7u,"PERCUSSIVE",preset_bytes[6],MD_ARP_SETTINGS_BYTES},
    {8u,"SEQ BASS",preset_bytes[7],MD_ARP_SETTINGS_BYTES},
    {9u,"SUB BASS",preset_bytes[8],MD_ARP_SETTINGS_BYTES},
    {10u,"TWIN SAWS",preset_bytes[9],MD_ARP_SETTINGS_BYTES},
};
static const char *const waveforms[]={"SAW","PULSE","TRIANGLE"};
static const char *const octaves[]={"-2","-1","0","+1","+2"};
static const char *const lfo_waveforms[]={"TRIANGLE","SQUARE","SAW","SAMPLE HOLD"};
static const md_param_desc_t params[]={
    {.id=MD_ARP_CUTOFF,.flags=MD_PARAM_AUTOMATABLE,.name="CUTOFF",.min=0,.max=100,.initial=38,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_RESONANCE,.flags=MD_PARAM_AUTOMATABLE,.name="RESONANCE",.min=0,.max=100,.initial=12,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_GLIDE,.flags=MD_PARAM_AUTOMATABLE,.name="GLIDE",.min=0,.max=100,.initial=0,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_LEVEL,.flags=MD_PARAM_AUTOMATABLE,.name="LEVEL",.min=0,.max=100,.initial=80,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_FILTER_ENV,.flags=MD_PARAM_AUTOMATABLE,.name="FILTER ENV",.min=0,.max=100,.initial=40,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_DRIVE,.flags=MD_PARAM_AUTOMATABLE,.name="DRIVE",.min=0,.max=100,.initial=25,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_FILTER_ATTACK,.flags=MD_PARAM_AUTOMATABLE,.name="FILTER ATTACK",.min=0,.max=100,.initial=8,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_FILTER_DECAY,.flags=MD_PARAM_AUTOMATABLE,.name="FILTER DECAY",.min=0,.max=100,.initial=50,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_AMP_ATTACK,.flags=MD_PARAM_AUTOMATABLE,.name="AMP ATTACK",.min=0,.max=100,.initial=12,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_AMP_RELEASE,.flags=MD_PARAM_AUTOMATABLE,.name="AMP RELEASE",.min=0,.max=100,.initial=25,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_OSC1_LEVEL,.flags=MD_PARAM_AUTOMATABLE,.name="OSC1 LEVEL",.min=0,.max=100,.initial=80,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_OSC2_LEVEL,.flags=MD_PARAM_AUTOMATABLE,.name="OSC2 LEVEL",.min=0,.max=100,.initial=70,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_OSC1_WAVE,.flags=MD_PARAM_AUTOMATABLE|MD_PARAM_ENUM,.name="OSC1 WAVE",.min=0,.max=2,.initial=0,.unit=MD_UNIT_INTEGER,.labels=waveforms,.label_count=3},
    {.id=MD_ARP_OSC1_OCTAVE,.flags=MD_PARAM_AUTOMATABLE|MD_PARAM_ENUM,.name="OSC1 OCTAVE",.min=0,.max=4,.initial=2,.unit=MD_UNIT_INTEGER,.labels=octaves,.label_count=5},
    {.id=MD_ARP_OSC1_PULSE_WIDTH,.flags=MD_PARAM_AUTOMATABLE,.name="OSC1 PULSE WIDTH",.min=5,.max=95,.initial=50,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_OSC2_WAVE,.flags=MD_PARAM_AUTOMATABLE|MD_PARAM_ENUM,.name="OSC2 WAVE",.min=0,.max=2,.initial=0,.unit=MD_UNIT_INTEGER,.labels=waveforms,.label_count=3},
    {.id=MD_ARP_OSC2_OCTAVE,.flags=MD_PARAM_AUTOMATABLE|MD_PARAM_ENUM,.name="OSC2 OCTAVE",.min=0,.max=4,.initial=2,.unit=MD_UNIT_INTEGER,.labels=octaves,.label_count=5},
    {.id=MD_ARP_OSC2_DETUNE,.flags=MD_PARAM_AUTOMATABLE,.name="OSC2 DETUNE",.min=0,.max=100,.initial=56,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_NOISE_LEVEL,.flags=MD_PARAM_AUTOMATABLE,.name="NOISE LEVEL",.min=0,.max=100,.initial=0,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_RING_LEVEL,.flags=MD_PARAM_AUTOMATABLE,.name="RING LEVEL",.min=0,.max=100,.initial=0,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_SYNC,.flags=MD_PARAM_AUTOMATABLE,.name="SYNC",.min=0,.max=1,.initial=0,.unit=MD_UNIT_BOOLEAN},
    {.id=MD_ARP_SUB_LEVEL,.flags=MD_PARAM_AUTOMATABLE,.name="SUB LEVEL",.min=0,.max=100,.initial=50,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_KEYBOARD_TRACK,.flags=MD_PARAM_AUTOMATABLE,.name="KEYBOARD TRACK",.min=0,.max=100,.initial=50,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_LFO_FILTER,.flags=MD_PARAM_AUTOMATABLE,.name="LFO FILTER",.min=0,.max=100,.initial=0,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_SAMPLE_HOLD_FILTER,.flags=MD_PARAM_AUTOMATABLE,.name="SAMPLE HOLD FILTER",.min=0,.max=100,.initial=0,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_FILTER_SUSTAIN,.flags=MD_PARAM_AUTOMATABLE,.name="FILTER SUSTAIN",.min=0,.max=100,.initial=25,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_FILTER_RELEASE,.flags=MD_PARAM_AUTOMATABLE,.name="FILTER RELEASE",.min=0,.max=100,.initial=25,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_AMP_DECAY,.flags=MD_PARAM_AUTOMATABLE,.name="AMP DECAY",.min=0,.max=100,.initial=60,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_AMP_SUSTAIN,.flags=MD_PARAM_AUTOMATABLE,.name="AMP SUSTAIN",.min=0,.max=100,.initial=70,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_LFO_WAVE,.flags=MD_PARAM_AUTOMATABLE|MD_PARAM_ENUM,.name="LFO WAVE",.min=0,.max=3,.initial=0,.unit=MD_UNIT_INTEGER,.labels=lfo_waveforms,.label_count=4},
    {.id=MD_ARP_LFO_RATE,.flags=MD_PARAM_AUTOMATABLE,.name="LFO RATE",.min=0,.max=100,.initial=30,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_LFO_PITCH,.flags=MD_PARAM_AUTOMATABLE,.name="LFO PITCH",.min=0,.max=100,.initial=0,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_LFO_PULSE_WIDTH,.flags=MD_PARAM_AUTOMATABLE,.name="LFO PULSE WIDTH",.min=0,.max=100,.initial=0,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_ACCENT_AMOUNT,.flags=MD_PARAM_AUTOMATABLE,.name="ACCENT AMOUNT",.min=0,.max=100,.initial=25,.unit=MD_UNIT_PERCENT},
    {.id=MD_ARP_ENVELOPE_PITCH,.flags=MD_PARAM_AUTOMATABLE,.name="ENVELOPE PITCH",.min=0,.max=100,.initial=0,.unit=MD_UNIT_PERCENT},
};
static const size_t param_offsets[]={offsetof(arp_patch_t,cutoff),offsetof(arp_patch_t,reso),offsetof(arp_patch_t,glide),offsetof(arp_patch_t,volume),offsetof(arp_patch_t,env_amt),offsetof(arp_patch_t,drive),offsetof(arp_patch_t,a1),offsetof(arp_patch_t,d1),offsetof(arp_patch_t,a2),offsetof(arp_patch_t,r2),offsetof(arp_patch_t,vco1_lvl),offsetof(arp_patch_t,vco2_lvl),offsetof(arp_patch_t,vco1_wave),offsetof(arp_patch_t,vco1_oct),offsetof(arp_patch_t,vco1_pw),offsetof(arp_patch_t,vco2_wave),offsetof(arp_patch_t,vco2_oct),offsetof(arp_patch_t,vco2_detune),offsetof(arp_patch_t,noise_lvl),offsetof(arp_patch_t,ring_lvl),offsetof(arp_patch_t,sync),offsetof(arp_patch_t,sub_lvl),offsetof(arp_patch_t,kbd),offsetof(arp_patch_t,lfo_vcf),offsetof(arp_patch_t,sh_vcf),offsetof(arp_patch_t,s1),offsetof(arp_patch_t,r1),offsetof(arp_patch_t,d2),offsetof(arp_patch_t,s2),offsetof(arp_patch_t,lfo_wave),offsetof(arp_patch_t,lfo_rate),offsetof(arp_patch_t,lfo_pitch),offsetof(arp_patch_t,lfo_pw),offsetof(arp_patch_t,accent),offsetof(arp_patch_t,env_pitch)};
static int validate(const uint8_t *src,uint32_t bytes)
{
    if (!src) return MD_ERR_ARGUMENT;
    if (bytes!=MD_ARP_SETTINGS_BYTES) return MD_ERR_FORMAT;
    for (uint32_t i=0;i<bytes;i++) if(src[i]<fields[i].min || src[i]>fields[i].max) return MD_ERR_RANGE;
    return MD_OK;
}
static int apply(void *state,const uint8_t *src,uint32_t bytes)
{
    if (!state) return MD_ERR_ARGUMENT;
    int result=validate(src,bytes);if(result) return result;
    md_arp_state_t *s=state;arp_patch_t patch=s->engine.patch;
    for(uint32_t i=0;i<bytes;i++) *((uint8_t *)&patch+fields[i].offset)=src[i];
    arp_set_patch(&s->engine,&patch);return MD_OK;
}
static int encode(const void *state,uint8_t *dst,uint32_t capacity)
{
    int result=md_adapter_encode_check(state,dst,capacity,MD_ARP_SETTINGS_BYTES);if(result)return result;
    const md_arp_state_t *s=state;
    for(uint32_t i=0;i<MD_ARP_SETTINGS_BYTES;i++) dst[i]=*((const uint8_t *)&s->engine.patch+fields[i].offset);
    return MD_ARP_SETTINGS_BYTES;
}
static int init(void *state,uint32_t rate)
{
    if(!state)return MD_ERR_ARGUMENT;
    if(rate!=MD_SAMPLE_RATE)return MD_ERR_UNSUPPORTED;
    md_arp_state_t *s=state;memset(s,0,sizeof *s);arp_init(&s->engine);
    return apply(s,preset_bytes[0],MD_ARP_SETTINGS_BYTES);
}
static void reset(void *state)
{
    md_arp_state_t *s=state;arp_patch_t patch=s->engine.patch;
    memset(s,0,sizeof *s);arp_init(&s->engine);arp_set_patch(&s->engine,&patch);
}
static int get_param(const void *state,md_param_id_t id,int32_t *value)
{
    if(!state || !value)return MD_ERR_ARGUMENT;
    if(id<1u || id>sizeof params/sizeof params[0])return MD_ERR_NOT_FOUND;
    *value=*((const uint8_t *)&((const md_arp_state_t *)state)->engine.patch+param_offsets[id-1u]);return MD_OK;
}
static int event(void *state,const md_event_t *e)
{
    if(!state || !e)return MD_ERR_ARGUMENT;
    md_arp_state_t *s=state;
    if(e->kind==MD_EVENT_PARAM){
        if(e->flags)return MD_ERR_UNSUPPORTED;
        uint32_t id=e->data.param.id;
        if(id<1u || id>sizeof params/sizeof params[0])return MD_ERR_NOT_FOUND;
        if(e->data.param.value<params[id-1u].min || e->data.param.value>params[id-1u].max)return MD_ERR_RANGE;
        arp_patch_t patch=s->engine.patch;
        *((uint8_t *)&patch+param_offsets[id-1u])=(uint8_t)e->data.param.value;
        arp_set_patch(&s->engine,&patch);return MD_OK;
    }
    if(e->kind==MD_EVENT_PANIC){reset(s);return MD_OK;}
    if(e->kind!=MD_EVENT_NOTE_ON && e->kind!=MD_EVENT_NOTE_OFF)return MD_ERR_UNSUPPORTED;
    /* Explicit performance path (M1-05): accent/legato/slide are flags, never velocity. */
    const uint16_t known=MD_NOTE_ACCENT|MD_NOTE_LEGATO|MD_NOTE_SLIDE;
    if(e->flags & ~known)return MD_ERR_UNSUPPORTED;
    md_event_t plain=*e;plain.flags=0;
    int result=md_adapter_note_check(&plain);if(result)return result;
    if(e->kind==MD_EVENT_NOTE_OFF){
        if(e->flags)return MD_ERR_ARGUMENT;
        if(e->token==s->voice.token){arp_release(&s->engine);s->voice=(md_adapter_voice_t){0};}
        return MD_OK; /* stale physical off after a legato/slide replacement: no-op */
    }
    uint32_t joined=(e->flags & (MD_NOTE_LEGATO|MD_NOTE_SLIDE))!=0;
    /* A held voice is replaced only by a legato/slide transition (router: new on, then old off). */
    if(s->voice.token && !joined)return MD_ERR_CAPACITY;
    uint32_t flags=((e->flags & MD_NOTE_ACCENT)?ARP_PLAY_ACCENT:0u)|((e->flags & MD_NOTE_LEGATO)?ARP_PLAY_LEGATO:0u)|
        ((e->flags & MD_NOTE_SLIDE)?ARP_PLAY_SLIDE:0u);
    s->voice=(md_adapter_voice_t){e->token,e->data.note.key};
    arp_play(&s->engine,e->data.note.key,flags);return MD_OK;
}
MD_HOT static void render(void *state,int32_t *out,uint32_t frames){arp_render(&((md_arp_state_t *)state)->engine,out,frames);}
const md_instrument_desc_t MD_ARP2600={
    .processor={.id=MD_INSTRUMENT_ARP2600,.api_version=MD_API_VERSION,.settings_version=MD_ARP_SETTINGS_VERSION,
        .name="ARP 2600",.state_bytes=sizeof(md_arp_state_t),.state_alignment=_Alignof(md_arp_state_t),
        .sample_rates=rates,.sample_rate_count=1,.params=params,.param_count=sizeof params/sizeof params[0],
        .max_settings_bytes=MD_ARP_SETTINGS_BYTES},
    .capabilities=MD_CAP_MONO|MD_CAP_ACCENT|MD_CAP_LEGATO|MD_CAP_SLIDE,.max_polyphony=1,.presets=presets,.preset_count=10,
    .init=init,.event=event,.get_param=get_param,.render_add=render,.reset=reset,.encode=encode,.validate=validate,.apply=apply
};
