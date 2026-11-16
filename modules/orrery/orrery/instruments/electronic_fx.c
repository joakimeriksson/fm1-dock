/* SPDX-License-Identifier: GPL-3.0-only */
/* Electronic percussion: independent fixed-point generator, not a bus effect.
 * Sine/phase tables are mathematical values generated from sin() and equal
 * temperament at44100Hz. Audio uses integers only; no per-sample division wider
 * than32bits, allocation, host calls or cross-instance mutable state.
 * Version1 wire: recipe,pitch,decayLE16,signedSweep,sweepMsLE16,tone,resonance,
 * modDepth,level,random,seedLE32. Version2 adds wind,windColour (18 bytes) and spread (19); a version-2 sound of 16
 * bytes loads with the wind off, of 16 or 18 in the middle; version-1 sounds are refused (the version must match;
 * none were ever stored). See electronic_fx.h for the recipes and the snapshot/kit semantics. */
#include "electronic_fx.h"
#include "adapter_common.h"
#define Q30 1073741824u
static const uint32_t rates[]={MD_SAMPLE_RATE};
static const int16_t sine_quarter[65]={
    0,804,1608,2410,3212,4011,4808,5602,6393,7179,7962,8739,9512,
    10278,11039,11793,12539,13279,14010,14732,15446,16151,16846,17530,18204,18868,
    19519,20159,20787,21403,22005,22594,23170,23731,24279,24811,25329,25832,26319,
    26790,27245,27683,28105,28510,28898,29268,29621,29956,30273,30571,30852,31113,
    31356,31580,31785,31971,32137,32285,32412,32521,32609,32678,32728,32757,32767,
};
static const uint32_t note_inc[128]={
    796254u,843601u,893765u,946911u,1003217u,1062871u,1126073u,1193033u,
    1263974u,1339134u,1418763u,1503127u,1592507u,1687203u,1787529u,1893821u,
    2006434u,2125742u,2252146u,2386065u,2527948u,2678268u,2837526u,3006254u,
    3185015u,3374406u,3575058u,3787642u,4012867u,4251485u,4504291u,4772130u,
    5055896u,5356535u,5675051u,6012507u,6370030u,6748811u,7150117u,7575285u,
    8025735u,8502970u,9008582u,9544261u,10111792u,10713070u,11350103u,12025015u,
    12740059u,13497623u,14300233u,15150569u,16051469u,17005939u,18017165u,19088521u,
    20223584u,21426141u,22700205u,24050030u,25480119u,26995246u,28600467u,30301139u,
    32102938u,34011878u,36034330u,38177043u,40447168u,42852281u,45400411u,48100060u,
    50960238u,53990491u,57200933u,60602278u,64205876u,68023757u,72068660u,76354085u,
    80894335u,85704563u,90800821u,96200119u,101920476u,107980983u,114401866u,121204555u,
    128411753u,136047513u,144137319u,152708170u,161788671u,171409126u,181601643u,192400238u,
    203840952u,215961966u,228803732u,242409110u,256823506u,272095026u,288274639u,305416341u,
    323577341u,342818251u,363203285u,384800477u,407681904u,431923931u,457607465u,484818220u,
    513647012u,544190053u,576549277u,610832681u,647154683u,685636503u,726406571u,769600953u,
    815363807u,863847862u,915214929u,969636441u,1027294024u,1088380105u,1153098554u,1221665363u,
};
#define LE16(v) (uint8_t)((v)&255),(uint8_t)((v)>>8)
/* recipe, pitch, decay ms, sweep semitones, sweep ms, tone, resonance, mod depth, level, random, seed; the wind off,
 * its colour 50, the spread 0 */
#define PRESET(recipe,pitch,decay,sweep,sweep_ms,tone,resonance,mod,level,random,seed) \
    {recipe,pitch,LE16(decay),(uint8_t)(sweep),LE16(sweep_ms),tone,resonance,mod,level,random,seed,0,0,0,0,50,0}
static const uint8_t preset_bytes[MD_FX_RECIPES][MD_ELECTRONIC_FX_SETTINGS_BYTES]={
    PRESET(MD_FX_DROP,72,450,24,35,65,65,0,80,0,1),
    PRESET(MD_FX_PING,84,1200,0,80,75,85,0,70,0,2),
    PRESET(MD_FX_BUBBLE,55,700,-12,150,50,70,55,80,30,3),
    PRESET(MD_FX_CHIRP,96,300,-36,45,85,40,35,65,0,4),
    PRESET(MD_FX_METAL,88,1600,0,120,85,50,85,70,0,5),
    PRESET(MD_FX_ZAP,84,420,36,800,85,0,0,70,0,6),          /* three octaves down, still falling as it dies */
    PRESET(MD_FX_WHISTLE,84,2600,17,120,70,0,45,70,0,7),       /* an octave and a fourth down in two and a half seconds */
    PRESET(MD_FX_GULL,91,750,7,70,80,0,30,70,0,8),          /* up a fifth in 70 ms, then down past the key */
    PRESET(MD_FX_TWITTER,96,1500,12,250,90,0,44,70,0,9),    /* fifteen chirps a second, a second and a half */
    PRESET(MD_FX_BLEEPS,84,1300,24,30,75,0,28,70,0,10),     /* eleven steps a second over two octaves */
    PRESET(MD_FX_WHOOSH,60,2600,30,1300,100,80,0,70,0,11),  /* two and a half octaves up, loudest half way */
    PRESET(MD_FX_SURF,67,4000,24,1500,100,20,0,70,0,12),    /* a second and a half in, two and a half out */
    PRESET(MD_FX_THUNDER,60,3800,-24,40,100,10,70,80,0,13)  /* the crack, then two octaves down into the roll */
};
#define P(r,name) {(r)+1u,name,preset_bytes[r],MD_ELECTRONIC_FX_SETTINGS_BYTES}
static const md_preset_t presets[]={
    P(MD_FX_DROP,"DROP"),P(MD_FX_PING,"PING"),P(MD_FX_BUBBLE,"BUBBLE"),P(MD_FX_CHIRP,"CHIRP"),P(MD_FX_METAL,"METAL"),
    P(MD_FX_ZAP,"ZAP"),P(MD_FX_WHISTLE,"WHISTLE"),P(MD_FX_GULL,"GULL"),P(MD_FX_TWITTER,"TWITTER"),P(MD_FX_BLEEPS,"BLEEPS"),
    P(MD_FX_WHOOSH,"WHOOSH"),P(MD_FX_SURF,"SURF"),P(MD_FX_THUNDER,"THUNDER")
};
#undef P
static const md_drum_voice_desc_t kit[]={
    {MD_FX_DROP,"DROP",72},{MD_FX_PING,"PING",84},{MD_FX_BUBBLE,"BUBBLE",55},
    {MD_FX_CHIRP,"CHIRP",96},{MD_FX_METAL,"METAL",88},{MD_FX_ZAP,"ZAP",84},{MD_FX_WHISTLE,"WHISTLE",84},
    {MD_FX_GULL,"GULL",91},{MD_FX_TWITTER,"TWITTER",96},{MD_FX_BLEEPS,"BLEEPS",84},{MD_FX_WHOOSH,"WHOOSH",60},
    {MD_FX_SURF,"SURF",67},{MD_FX_THUNDER,"THUNDER",60}
};
static const char *const recipes[]={"DROP","PING","BUBBLE","CHIRP","METAL","ZAP","WHISTLE","GULL","TWITTER","BLEEPS",
    "WHOOSH","SURF","THUNDER"};
/* how far a recipe travels left to right over its length, percent of the spread (0: it stays where it lands) */
static const uint8_t travel[MD_FX_RECIPES]={0,0,0,0,0,70,50,30,0,0,100,80,20};
static const md_param_desc_t params[]={
    {.id=MD_FX_RECIPE,.name="RECIPE",.min=0,.max=MD_FX_RECIPES-1,.initial=0,.flags=MD_PARAM_ENUM|MD_PARAM_AUTOMATABLE,.unit=MD_UNIT_INTEGER,.labels=recipes,.label_count=MD_FX_RECIPES},
    {.id=MD_FX_BASE_PITCH,.name="KIT PITCH",.min=0,.max=127,.initial=72,.flags=MD_PARAM_AUTOMATABLE,.unit=MD_UNIT_INTEGER},
    {.id=MD_FX_DECAY_MS,.name="DECAY",.min=20,.max=4000,.initial=450,.flags=MD_PARAM_AUTOMATABLE,.unit=MD_UNIT_MS},
    {.id=MD_FX_SWEEP_SEMITONES,.name="PITCH SWEEP",.min=-48,.max=48,.initial=24,.flags=MD_PARAM_AUTOMATABLE,.unit=MD_UNIT_SEMITONES},
    {.id=MD_FX_SWEEP_MS,.name="SWEEP TIME",.min=5,.max=4000,.initial=35,.flags=MD_PARAM_AUTOMATABLE,.unit=MD_UNIT_MS},
    {.id=MD_FX_TONE,.name="TONE",.min=0,.max=100,.initial=65,.flags=MD_PARAM_AUTOMATABLE,.unit=MD_UNIT_PERCENT},
    {.id=MD_FX_RESONANCE,.name="RESONANCE",.min=0,.max=90,.initial=65,.flags=MD_PARAM_AUTOMATABLE,.unit=MD_UNIT_PERCENT},
    {.id=MD_FX_MOD_DEPTH,.name="MOD DEPTH",.min=0,.max=100,.initial=0,.flags=MD_PARAM_AUTOMATABLE,.unit=MD_UNIT_PERCENT},
    {.id=MD_FX_LEVEL,.name="LEVEL",.min=0,.max=100,.initial=80,.flags=MD_PARAM_AUTOMATABLE,.unit=MD_UNIT_PERCENT},
    {.id=MD_FX_RANDOM,.name="RANDOM",.min=0,.max=100,.initial=0,.flags=MD_PARAM_AUTOMATABLE,.unit=MD_UNIT_PERCENT},
    {.id=MD_FX_WIND,.name="WIND",.min=0,.max=100,.initial=0,.flags=MD_PARAM_AUTOMATABLE,.unit=MD_UNIT_PERCENT},
    {.id=MD_FX_WIND_COLOUR,.name="WIND COLOUR",.min=0,.max=100,.initial=50,.flags=MD_PARAM_AUTOMATABLE,.unit=MD_UNIT_PERCENT},
    {.id=MD_FX_KIT,.name="KIT SOUND",.min=0,.max=MD_FX_RECIPES,.initial=0,.flags=MD_PARAM_AUTOMATABLE,.unit=MD_UNIT_INTEGER},
    {.id=MD_FX_SPREAD,.name="SPREAD",.min=0,.max=100,.initial=0,.flags=MD_PARAM_AUTOMATABLE,.unit=MD_UNIT_PERCENT}
};
static uint16_t u16(const uint8_t *p){return (uint16_t)((uint16_t)p[0]|(uint16_t)p[1]<<8);}
static uint32_t u32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int validate(const uint8_t *p,uint32_t bytes)
{
    if(!p)return MD_ERR_ARGUMENT;
    if(bytes!=16u && bytes!=18u && bytes!=19u)return MD_ERR_FORMAT;
    if(bytes>=18u && (p[16]>100u || p[17]>100u))return MD_ERR_RANGE;
    if(bytes==19u && p[18]>100u)return MD_ERR_RANGE;
    int32_t sweep=p[4]<=127u ? p[4] : (int32_t)p[4]-256;
    if(p[0]>=MD_FX_RECIPES || p[1]>127u || u16(p+2)<20u || u16(p+2)>4000u ||
        sweep < -48 || sweep > 48 || u16(p+5)<5u || u16(p+5)>4000u ||
        p[7]>100u || p[8]>90u || p[9]>100u || p[10]>100u || p[11]>100u)return MD_ERR_RANGE;
    return MD_OK;
}
static int apply(void *state,const uint8_t *p,uint32_t bytes)
{
    if(!state)return MD_ERR_ARGUMENT;
    int result=validate(p,bytes);if(result)return result;
    md_electronic_fx_state_t *s=state;
    s->settings=(md_electronic_fx_settings_t){.recipe=p[0],.pitch=p[1],.decay_ms=u16(p+2),
        .sweep=(int8_t)(p[4]<=127u ? p[4] : (int32_t)p[4]-256),.sweep_ms=u16(p+5),
        .tone=p[7],.resonance=p[8],.mod_depth=p[9],.level=p[10],.random=p[11],.seed=u32(p+12),
        .wind=bytes>=18u ? p[16] : 0,.wind_colour=bytes>=18u ? p[17] : 50,.spread=bytes==19u ? p[18] : 0};
    s->rng=s->settings.seed;return MD_OK;
}
static int encode(const void *state,uint8_t *p,uint32_t capacity)
{
    int result=md_adapter_encode_check(state,p,capacity,19);if(result)return result;
    const md_electronic_fx_settings_t *s=&((const md_electronic_fx_state_t *)state)->settings;
    p[0]=s->recipe;p[1]=s->pitch;p[2]=(uint8_t)s->decay_ms;p[3]=(uint8_t)(s->decay_ms>>8);
    p[4]=(uint8_t)s->sweep;p[5]=(uint8_t)s->sweep_ms;p[6]=(uint8_t)(s->sweep_ms>>8);
    p[7]=s->tone;p[8]=s->resonance;p[9]=s->mod_depth;p[10]=s->level;p[11]=s->random;
    for(uint32_t i=0;i<4;i++)p[12+i]=(uint8_t)(s->seed>>(8u*i));
    p[16]=s->wind;p[17]=s->wind_colour;p[18]=s->spread;return 19;
}
static int init(void *state,uint32_t rate)
{
    if(!state)return MD_ERR_ARGUMENT;
    if(rate!=MD_SAMPLE_RATE)return MD_ERR_UNSUPPORTED;
    memset(state,0,sizeof(md_electronic_fx_state_t));((md_electronic_fx_state_t *)state)->wind_rng=0x9e3779b9u;return apply(state,preset_bytes[0],19);
}
static void reset(void *state)
{
    md_electronic_fx_state_t *s=state;md_electronic_fx_settings_t settings=s->settings;
    memset(s,0,sizeof *s);s->settings=settings;s->rng=settings.seed;s->wind_rng=0x9e3779b9u; /* (the kit sound off) */
}
MD_INLINE uint32_t random_next(uint32_t *seed){*seed=*seed*1664525u+1013904223u;return *seed;}
MD_INLINE int32_t sine(uint32_t phase)
{
    uint32_t quadrant=phase>>30,position=phase&0x3fffffffu;
    if(quadrant&1u)position=0x40000000u-position;
    uint32_t index=position>>24,fraction=(position>>16)&255u;
    int32_t y=sine_quarter[index];
    if(index<64u)y+=(sine_quarter[index+1u]-y)*(int32_t)fraction/256;
    return quadrant>=2u ? -y : y;
}
MD_INLINE uint32_t pitch_increment(int32_t pitch_q16)
{
    if(pitch_q16<=0)return note_inc[0];
    if(pitch_q16>=127*65536)return note_inc[127];
    uint32_t note=(uint32_t)pitch_q16>>16,fraction=(uint32_t)pitch_q16&65535u;
    return note_inc[note]+(uint32_t)(((uint64_t)(note_inc[note+1u]-note_inc[note])*fraction)>>16);
}
static uint32_t decay_coefficient(uint32_t ms)
{
    uint32_t samples=ms*MD_SAMPLE_RATE/1000u;
    return (Q30/samples)*7u; /* amplitude approximately -60dB at selected duration */
}
static int voice_slot(const md_electronic_fx_state_t *s)
{
    int oldest=-1;
    for(uint32_t i=0;i<MD_ELECTRONIC_FX_VOICES;i++){
        if(s->voices[i].held)continue;
        if(!s->voices[i].active)return (int)i;
        if(oldest<0 || s->voices[i].order<s->voices[oldest].order)oldest=(int)i;
    }
    return oldest;
}
MD_INLINE void place(md_electronic_fx_voice_t *v,int32_t pan)   /* a balance: the far side falls, the near one stays */
{
    if(pan<0)pan=0;else if(pan>32768)pan=32768;
    v->gain_l=pan<=16384 ? 32768 : 2*(32768-pan);v->gain_r=pan>=16384 ? 32768 : 2*pan;
}
/* m: the macros the voice takes (the sound's, or a recipe's own for a kit sound); level and random are the sound's */
static int trigger(md_electronic_fx_state_t *s,const md_electronic_fx_settings_t *m,uint8_t recipe,uint8_t key,uint8_t velocity,md_note_token_t token)
{
    int slot=voice_slot(s);if(slot<0 || s->order==UINT64_MAX)return MD_ERR_CAPACITY;
    uint32_t seed=random_next(&s->rng);
    int32_t cents=((int32_t)(seed>>16)%601-300)*(int32_t)s->settings.random/100;
    md_electronic_fx_voice_t *v=&s->voices[slot];
    *v=(md_electronic_fx_voice_t){.token=token,.order=++s->order,.amp=Q30,.pitch_env=Q30,
        .decay_k=decay_coefficient(m->decay_ms),.pitch_k=decay_coefficient(m->sweep_ms),
        .rng=seed,.base_q16=(int32_t)key*65536+cents*65536/100,.sweep_q16=(int32_t)m->sweep*65536,
        .gain_q15=(uint16_t)((uint32_t)velocity*s->settings.level*32767u/12700u),
        .ping_damp=16+(int32_t)((100u-m->resonance)*(100u-m->resonance)*(100u-m->resonance)*65u/1000u),
        .recipe=recipe,.tone=m->tone,.resonance=m->resonance,
        .mod_depth=m->mod_depth,.active=1,.held=token!=MD_TOKEN_NONE};
    if(recipe>=MD_FX_WHISTLE){   /* shaped: the swell for the sweep's time (nine tenths of the length at most), then the fall */
        uint32_t span=(uint32_t)m->decay_ms*(MD_SAMPLE_RATE/100u)/10u,rise=(uint32_t)m->sweep_ms*(MD_SAMPLE_RATE/100u)/10u;
        if(rise>span-span/10u)rise=span-span/10u;
        v->shaped=1;v->rising=1;v->amp=0;v->lin=Q30;v->up_k=Q30/rise;v->down_k=Q30/(span-rise);
        uint32_t peak=(rise<<8)/(span>>7);                    /* (of 32768, near enough) */
        v->peak_q15=(uint16_t)(peak>29491u ? 29491u : peak);
        if(recipe==MD_FX_TWITTER || recipe==MD_FX_BLEEPS){
            v->step_len=MD_SAMPLE_RATE/(4u+m->mod_depth/4u);
            v->step_on=recipe==MD_FX_TWITTER ? v->step_len*11u/20u : v->step_len*4u/5u;
            v->glide_k=recipe==MD_FX_TWITTER ? (int32_t)(5u*65536u/v->step_on) : 0;   /* each chirp up a fourth */
            v->step_q16=INT32_MAX;                            /* (no step before the first) */
        }else if(recipe>=MD_FX_WHOOSH)
            v->ping_damp=1200+(int32_t)(100u-m->resonance)*290;   /* the noise's filter: Q about 1 .. 8 */
    }
    {   /* its place: across and travelling, or where the seed puts it; the middle with no spread */
        int32_t spread=s->settings.spread,across=(int32_t)travel[recipe]*spread;
        if(across){
            int32_t direction=seed&1u ? 1 : -1;
            v->pan=16384-direction*(across*16384/10000);v->pan_move=direction*(across*32768/10000);
        }else v->pan=16384+((int32_t)(seed>>8&0xffffu)-32768)*spread/200;
        place(v,v->pan);
    }
    return MD_OK;
}
void md_electronic_fx_macros(uint32_t recipe,int16_t macros[6])
{
    const uint8_t *p=preset_bytes[recipe%MD_FX_RECIPES];
    macros[0]=(int16_t)u16(p+2);macros[1]=(int16_t)(p[4]<=127u ? p[4] : (int32_t)p[4]-256);macros[2]=(int16_t)u16(p+5);
    macros[3]=p[7];macros[4]=p[8];macros[5]=p[9];
}
static int trigger_kit(md_electronic_fx_state_t *s,uint8_t recipe,uint8_t key,uint8_t velocity)
{
    const uint8_t *p=preset_bytes[recipe];
    md_electronic_fx_settings_t own={.decay_ms=u16(p+2),.sweep=(int8_t)(p[4]<=127u ? p[4] : (int32_t)p[4]-256),
        .sweep_ms=u16(p+5),.tone=p[7],.resonance=p[8],.mod_depth=p[9]};
    return trigger(s,&own,recipe,key,velocity,MD_TOKEN_NONE);
}
static int set_param(md_electronic_fx_state_t *s,md_param_id_t id,int32_t value)
{
    if(id<1u || id>sizeof params/sizeof params[0])return MD_ERR_NOT_FOUND;
    if(value<params[id-1u].min || value>params[id-1u].max)return MD_ERR_RANGE;
    switch(id){
    case MD_FX_RECIPE:s->settings.recipe=(uint8_t)value;break;
    case MD_FX_BASE_PITCH:s->settings.pitch=(uint8_t)value;break;
    case MD_FX_DECAY_MS:s->settings.decay_ms=(uint16_t)value;break;
    case MD_FX_SWEEP_SEMITONES:s->settings.sweep=(int8_t)value;break;
    case MD_FX_SWEEP_MS:s->settings.sweep_ms=(uint16_t)value;break;
    case MD_FX_TONE:s->settings.tone=(uint8_t)value;break;
    case MD_FX_RESONANCE:s->settings.resonance=(uint8_t)value;break;
    case MD_FX_MOD_DEPTH:s->settings.mod_depth=(uint8_t)value;break;
    case MD_FX_LEVEL:s->settings.level=(uint8_t)value;break;
    case MD_FX_RANDOM:s->settings.random=(uint8_t)value;break;
    case MD_FX_WIND:s->settings.wind=(uint8_t)value;break;
    case MD_FX_KIT:s->kit=(uint8_t)value;break;
    case MD_FX_SPREAD:s->settings.spread=(uint8_t)value;break;
    default:s->settings.wind_colour=(uint8_t)value;break;
    }
    return MD_OK;
}
static int get_param(const void *state,md_param_id_t id,int32_t *value)
{
    if(!state || !value)return MD_ERR_ARGUMENT;
    const md_electronic_fx_settings_t *s=&((const md_electronic_fx_state_t *)state)->settings;
    switch(id){
    case MD_FX_RECIPE:*value=s->recipe;break;
    case MD_FX_BASE_PITCH:*value=s->pitch;break;
    case MD_FX_DECAY_MS:*value=s->decay_ms;break;
    case MD_FX_SWEEP_SEMITONES:*value=s->sweep;break;
    case MD_FX_SWEEP_MS:*value=s->sweep_ms;break;
    case MD_FX_TONE:*value=s->tone;break;
    case MD_FX_RESONANCE:*value=s->resonance;break;
    case MD_FX_MOD_DEPTH:*value=s->mod_depth;break;
    case MD_FX_LEVEL:*value=s->level;break;
    case MD_FX_RANDOM:*value=s->random;break;
    case MD_FX_WIND:*value=s->wind;break;
    case MD_FX_WIND_COLOUR:*value=s->wind_colour;break;
    case MD_FX_KIT:*value=((const md_electronic_fx_state_t *)state)->kit;break;
    case MD_FX_SPREAD:*value=s->spread;break;
    default:return MD_ERR_NOT_FOUND;
    }
    return MD_OK;
}
static int event(void *state,const md_event_t *e)
{
    if(!state || !e)return MD_ERR_ARGUMENT;
    if(e->flags)return MD_ERR_UNSUPPORTED;
    md_electronic_fx_state_t *s=state;
    if(e->kind==MD_EVENT_PARAM)return set_param(s,e->data.param.id,e->data.param.value);
    if(e->kind==MD_EVENT_PANIC){reset(s);return MD_OK;}
    if(e->kind==MD_EVENT_HIT){
        if(e->data.hit.voice>=MD_FX_RECIPES || !e->data.hit.velocity || e->data.hit.velocity>127u)return MD_ERR_RANGE;
        return trigger(s,&s->settings,(uint8_t)e->data.hit.voice,s->settings.pitch,e->data.hit.velocity,MD_TOKEN_NONE);
    }
    if(e->kind!=MD_EVENT_NOTE_ON && e->kind!=MD_EVENT_NOTE_OFF)return MD_ERR_UNSUPPORTED;
    int result=md_adapter_note_check(e);if(result)return result;
    int found=-1;
    for(uint32_t i=0;i<MD_ELECTRONIC_FX_VOICES;i++)if(s->voices[i].token==e->token){found=(int)i;break;}
    if(e->kind==MD_EVENT_NOTE_OFF){
        if(found>=0){s->voices[found].held=0;s->voices[found].token=0;s->voices[found].releasing=1;}
        return MD_OK;
    }
    if(found>=0)return MD_ERR_STATE;
    if(s->kit)return trigger_kit(s,(uint8_t)(s->kit-1u),e->data.note.key,e->data.note.velocity); /* its off finds nothing */
    return trigger(s,&s->settings,s->settings.recipe,e->data.note.key,e->data.note.velocity,e->token);
}
MD_INLINE int32_t clamp_signal(int32_t value)
{
    return value < -262144 ? -262144 : value > 262144 ? 262144 : value;
}
#define RELEASE_K ((Q30/(30u*MD_SAMPLE_RATE/1000u))*7u)   /* a released note: 30 ms */
#define FLOOR (Q30>>12)   /* a voice is over 72 dB down (it was 120: a second more of every voice, costing and unheard) */
MD_INLINE int32_t voice_sample(md_electronic_fx_voice_t *v)
{
    if(v->amp<FLOOR || v->age>=MD_SAMPLE_RATE*8u){v->active=0;return 0;}
    int32_t pitch=v->base_q16+(int32_t)(((int64_t)v->sweep_q16*v->pitch_env)>>30);
    if(v->recipe==MD_FX_BUBBLE)
        pitch+=sine(v->mod_phase)*(int32_t)v->mod_depth*2/25;
    uint32_t increment=pitch_increment(pitch);
    int32_t carrier=sine(v->phase),sample=carrier;
    if(v->recipe==MD_FX_PING){
        uint32_t safe_inc=increment>0x20000000u ? 0x20000000u : increment;
        int32_t g=2*sine(safe_inc>>1),damp=v->ping_damp;
        int32_t excitation=v->age ? 0 : 32767;
        if(v->age<32u)excitation+=(int32_t)(random_next(&v->rng)>>17)-16384;
        v->filter_low=clamp_signal(v->filter_low+(int32_t)(((int64_t)g*v->filter_band)>>15));
        int32_t high=excitation-v->filter_low-(int32_t)(((int64_t)damp*v->filter_band)>>15);
        v->filter_band=clamp_signal(v->filter_band+(int32_t)(((int64_t)g*high)>>15));
        sample=v->filter_band;
    }else if(v->recipe==MD_FX_METAL){
        int32_t mod=sine(v->mod_phase);
        carrier=sine(v->phase+(uint32_t)((int64_t)mod*v->mod_depth*655));
        int32_t ring=(int32_t)(((int64_t)carrier*mod)>>15);
        sample=carrier+(ring-carrier)*(int32_t)v->mod_depth/100;
    }else if(v->recipe==MD_FX_CHIRP){
        sample=(carrier*2+sine(v->phase*2u))/3;
    }else if(v->recipe==MD_FX_ZAP && increment<0x2aaaaaaau){   /* bright: two more partials while they fit */
        sample=(carrier*3+sine(v->phase*2u)*2+sine(v->phase*3u))/5;
    }
    int32_t tone_k=327+(int32_t)v->tone*262;
    v->tone_low+= (int32_t)(((int64_t)(sample-v->tone_low)*tone_k)>>15);
    sample=v->tone_low;
    if(sample>32767)sample=32767;else if(sample < -32767)sample=-32767;
    sample=(int32_t)(((int64_t)sample*v->amp)>>30);
    sample=(int32_t)(((int64_t)sample*v->gain_q15)>>15);
    v->phase+=increment;
    v->mod_phase+=v->recipe==MD_FX_BUBBLE ? 1168699u : increment+increment/2u;
    uint32_t k=v->releasing ? RELEASE_K : v->decay_k;
    uint32_t decrement=(uint32_t)(((uint64_t)v->amp*k)>>30);
    v->amp-=decrement ? decrement : 1u;
    if(v->pitch_env){decrement=(uint32_t)(((uint64_t)v->pitch_env*v->pitch_k)>>30);v->pitch_env-=decrement ? decrement : 1u;}
    v->age++;return sample;
}
/* The shaped recipes. The envelope first: the swell is a line up to full, the fall a line down, squared. Then the
 * source by recipe; w is how far the fall has come and a the envelope, both 0 .. 32768. */
static const int8_t step_notes[7]={0,7,12,19,24,31,36};   /* fifths and octaves: in key above any note of the scale */
MD_INLINE int32_t shaped_sample(md_electronic_fx_voice_t *v)
{
    if(v->age>=MD_SAMPLE_RATE*8u){v->active=0;return 0;}
    if(v->releasing){
        if(v->amp<FLOOR){v->active=0;return 0;}
        v->amp-=(uint32_t)(((uint64_t)v->amp*RELEASE_K)>>30)+1u;
    }else if(v->rising){
        v->amp+=v->up_k;if(v->amp>=Q30){v->amp=Q30;v->rising=0;}
    }else{
        if(v->lin<=v->down_k){v->active=0;return 0;}
        v->lin-=v->down_k;v->amp=(uint32_t)(((uint64_t)v->lin*v->lin)>>30);
    }
    int32_t w=(int32_t)((Q30-v->lin)>>15),a=(int32_t)(v->amp>>15),sample,pitch;
    switch(v->recipe){
    case MD_FX_WHISTLE:
        pitch=v->base_q16+(int32_t)(((int64_t)v->sweep_q16*(32768-w))>>15);
        pitch+=(int32_t)(((int64_t)sine(v->mod_phase)*2*(int32_t)v->mod_depth/100*w)>>15);   /* a semitone at most */
        v->mod_phase+=584350u;                                /* 6 Hz */
        sample=sine(v->phase);v->phase+=pitch_increment(pitch);
        break;
    case MD_FX_GULL:{
        int32_t contour=v->rising ? a : 32768-(int32_t)(((int64_t)w*w*7)>>17);   /* up with the swell, then 1 - 1.75 w^2 */
        pitch=v->base_q16+(int32_t)(((int64_t)v->sweep_q16*contour)>>15);
        pitch+=(int32_t)(((int64_t)sine(v->mod_phase)*4*(int32_t)v->mod_depth/100*w)>>15);   /* two semitones at most */
        v->mod_phase+=2532183u;                               /* 26 Hz: the cry roughens as it falls */
        sample=(sine(v->phase)*2+sine(v->phase*2u))/3;v->phase+=pitch_increment(pitch);
        break;
    }
    case MD_FX_TWITTER:case MD_FX_BLEEPS:{
        if(!v->step_count){                                   /* a new step: another note of the table, or a rest */
            uint32_t r=random_next(&v->rng),n=1,k;
            int32_t range=v->sweep_q16<0 ? -(v->sweep_q16>>16) : v->sweep_q16>>16,next;
            while(n<7u && step_notes[n]<=range)n++;
            k=(r>>20)%n;next=(v->sweep_q16<0 ? -step_notes[k] : step_notes[k])*65536;
            if(next==v->step_q16 && n>1u)next=(v->sweep_q16<0 ? -step_notes[(k+1u)%n] : step_notes[(k+1u)%n])*65536;
            v->rest=v->step_q16!=INT32_MAX && (r>>28)<(v->recipe==MD_FX_TWITTER ? 5u : 2u);
            v->step_q16=next;v->step_count=v->step_len;v->glide=0;
        }
        int on=!v->rest && v->step_len-v->step_count<v->step_on;
        v->step_count--;
        v->gate+=((on ? 32767 : 0)-v->gate)>>7;               /* three milliseconds: no click */
        uint32_t increment=pitch_increment(v->base_q16+v->step_q16+v->glide);
        v->glide+=v->glide_k;
        sample=sine(v->phase);
        if(v->recipe==MD_FX_BLEEPS && increment<0x2aaaaaaau)sample=(sample*3+sine(v->phase*3u))/4;   /* hollow */
        sample=(sample*v->gate)>>15;v->phase+=increment;
        break;
    }
    default:{                                                 /* WHOOSH, SURF, THUNDER: noise through a filter */
        int32_t noise=(int32_t)(random_next(&v->rng)>>17)-16384;
        if(!(v->age&15u)){                                    /* where the filter is, sixteen samples at a time */
            int32_t fall=32768-w,to;
            if(v->recipe==MD_FX_WHOOSH)                       /* over the whole length */
                to=v->rising ? (a*(int32_t)v->peak_q15)>>15 : (int32_t)v->peak_q15+((w*(32768-(int32_t)v->peak_q15))>>15);
            else if(v->recipe==MD_FX_SURF)to=a;               /* with the envelope */
            else to=32768-((fall*fall)>>15);                  /* THUNDER: fast at first */
            pitch=v->base_q16+(int32_t)(((int64_t)v->sweep_q16*to)>>15);
            uint32_t increment=pitch_increment(pitch);
            v->svf_g=2*sine((increment>0x20000000u ? 0x20000000u : increment)>>1);
            /* a low-pass lets through less the lower it sits: THUNDER is made up by the square root of that, as
             * the increment of half the way down from C8 (x1 at C8, x8 at C2, x11 at most) */
            if(v->recipe==MD_FX_THUNDER){
                v->makeup=(int32_t)(pitch_increment((108*65536-pitch)/2+60*65536)/6220u);
                if(v->makeup>45056)v->makeup=45056;
            }
        }
        v->filter_low=clamp_signal(v->filter_low+(int32_t)(((int64_t)v->svf_g*v->filter_band)>>15));
        int32_t high=noise-v->filter_low-(int32_t)(((int64_t)v->ping_damp*v->filter_band)>>15);
        v->filter_band=clamp_signal(v->filter_band+(int32_t)(((int64_t)v->svf_g*high)>>15));
        if(v->recipe==MD_FX_WHOOSH)sample=v->filter_band;
        else if(v->recipe==MD_FX_SURF)sample=v->filter_band+(v->filter_low>>2);   /* hiss more than rumble */
        else{
            if(!(v->age&2047u))v->roll_target=(int32_t)(random_next(&v->rng)>>17);   /* a new height 21 times a second */
            v->roll+=(v->roll_target-v->roll)>>10;
            sample=(int32_t)(((int64_t)v->filter_low*v->makeup*3)>>13);
            if(sample>32767)sample=32767;else if(sample < -32767)sample=-32767;
            sample=(sample*(32768-(int32_t)v->mod_depth*(32768-v->roll)/100))>>15;
        }
        break;
    }
    }
    int32_t tone_k=327+(int32_t)v->tone*262;
    v->tone_low+=(int32_t)(((int64_t)(sample-v->tone_low)*tone_k)>>15);
    sample=v->tone_low;
    if(sample>32767)sample=32767;else if(sample < -32767)sample=-32767;
    sample=(int32_t)(((int64_t)sample*v->amp)>>30);
    sample=(int32_t)(((int64_t)sample*v->gain_q15)>>15);
    v->age++;return sample;
}
/* the wind: the gain moves a 1/32768 of the way a sample (time constant about 0.74 s: full to silence in about
 * four seconds), reaching its target exactly; the band-pass centre is set every 16
 * samples from the colour (G3 .. G6) and an 11 s LFO (an octave either side) */
#define WIND_DAMP 12000
MD_INLINE int32_t wind_sample(md_electronic_fx_state_t *s)
{
    int32_t target=(int32_t)s->settings.wind*327*256;
    if(!target && s->wind_gain<256){s->wind_gain=0;return 0;}
    int32_t step=(target-s->wind_gain)>>15;    /* the last 1/32768 of the way: one a sample, either way */
    if(!step && target!=s->wind_gain)step=target>s->wind_gain ? 1 : -1;
    s->wind_gain+=step;
    if(!(s->wind_count++&15u)){
        s->wind_lfo+=16u*8853u;
        int32_t pitch=(55+(int32_t)s->settings.wind_colour*36/100)*65536+sine(s->wind_lfo)*24;
        uint32_t inc=pitch_increment(pitch);
        s->wind_g=2*sine((inc>0x20000000u ? 0x20000000u : inc)>>1);
    }
    int32_t noise=(int32_t)(random_next(&s->wind_rng)>>17)-16384;
    s->wind_low=clamp_signal(s->wind_low+(int32_t)(((int64_t)s->wind_g*s->wind_band)>>15));
    int32_t high=noise-s->wind_low-(int32_t)(((int64_t)WIND_DAMP*s->wind_band)>>15);
    s->wind_band=clamp_signal(s->wind_band+(int32_t)(((int64_t)s->wind_g*high)>>15));
    int32_t out=s->wind_band+(s->wind_low>>2);
    if(out>32767)out=32767;else if(out < -32767)out=-32767;
    return (int32_t)(((int64_t)out*(s->wind_gain>>8))>>15);
}
MD_HOT static void render(void *state,int32_t *stereo,uint32_t frames)
{
    md_electronic_fx_state_t *s=state;
    for(uint32_t i=0;i<frames;i++){
        int32_t left=wind_sample(s),right=left;               /* the wind in the middle */
        for(uint32_t k=0;k<MD_ELECTRONIC_FX_VOICES;k++){
            md_electronic_fx_voice_t *v=&s->voices[k];
            if(!v->active)continue;
            if(v->pan_move && !(v->age&15u)){                 /* travelling: how far it has come, 0 .. 32768 */
                int32_t to=!v->shaped ? 32768-(int32_t)(v->pitch_env>>15) :
                    v->rising ? ((int32_t)(v->amp>>15)*(int32_t)v->peak_q15)>>15 :
                    (int32_t)v->peak_q15+(((int32_t)((Q30-v->lin)>>15)*(32768-(int32_t)v->peak_q15))>>15);
                place(v,v->pan+(int32_t)(((int64_t)v->pan_move*to)>>15));
            }
            int32_t sample=v->shaped ? shaped_sample(v) : voice_sample(v);
            left+=(sample*v->gain_l)>>15;right+=(sample*v->gain_r)>>15;
        }
        stereo[2u*i]+=left;stereo[2u*i+1u]+=right;
    }
}
const md_instrument_desc_t MD_ELECTRONIC_FX={
    .processor={.id=MD_INSTRUMENT_ELECTRONIC_FX,.api_version=MD_API_VERSION,.settings_version=MD_ELECTRONIC_FX_SETTINGS_VERSION,
        .name="ELECTRONIC FX",.state_bytes=sizeof(md_electronic_fx_state_t),.state_alignment=_Alignof(md_electronic_fx_state_t),
        .sample_rates=rates,.sample_rate_count=1,.params=params,.param_count=sizeof params/sizeof params[0],.max_settings_bytes=19},
    .capabilities=MD_CAP_POLY|MD_CAP_PERCUSSION|MD_CAP_VELOCITY,.max_polyphony=MD_ELECTRONIC_FX_VOICES,
    .presets=presets,.preset_count=MD_FX_RECIPES,.drum_voice_count=MD_FX_RECIPES,.drum_voices=kit,
    .init=init,.event=event,.get_param=get_param,.render_add=render,.reset=reset,.encode=encode,.validate=validate,.apply=apply
};
