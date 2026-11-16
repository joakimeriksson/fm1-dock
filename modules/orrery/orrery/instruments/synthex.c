/* SPDX-License-Identifier: GPL-3.0-only */
/* Version1 payload: patch (0..79, the factory bank's order), level (0..100). The engine runs in the audio context
 * (events come through the session's render), so it is driven directly, not through its event queue. The factory
 * patches are the Synthex's 25-byte records (engines/sc_presets_data.h), decoded when one is loaded; each patch's
 * own reverb is switched off (the session's reverb serves every part). */
#include "synthex.h"
#include "engines/sc_presets.h"
_Static_assert(SC_N_PRESETS == MD_SYNTHEX_PATCHES, "the factory bank");
static const char *const NAMES[MD_SYNTHEX_PATCHES]={"Chunky Synth","Wah brass","Fat Sound","Metallic","Pan flutes","Tremolo piano","Chorale","Brass I","Brass II","Brass III","Double basses","Ceilos","Violins","Church organ","Reed organ","Electronic organ","High pass sweep","Filtered brass","Filtered chorus","Filtered strings","Metallic piano","Pianet","Funky Clav.","Clavichord I","Clavichord II","Clavinet","Electric piano I","Electric piano II","Chimes","Vibes","Sweep 1","Sweep 2","Take-off","Landing","Wash of sound","Fast decay","Ring mod.","Phased sweep","Bass pluck","Whistle","Cathedral choir","Sync. Harmonic Sweep","Filtered Choir","Harsh Clavinet","Echo","Double Bass - Nylon Guitar","Sync. 1","Sync. 2","De-tuned pipe organ","Normal Clavinet","Harsh lead","Solo Violin","Bass Guitar","Sync. 3","Glide Strings","Hammond 1 (John Lord)","Phased Choir","Brash Brass","Brushed Cymbals","Hammond 2 (Emerson)","Plink","Wah Ring Mod.","Metallic Clavinet","Bass Drum - tom-toms","Hammond 3 (Brooker T)","Plucked brass","Snare Drum","Distant Strings","Watery Grave","High Portamento","Electric Piano","Oriental Fourths","Wood Blocks","Marimba","Harp","Filtered brass","Deep. Sync.","Mini Moog Bass","Mini Moog Lead","Steel Drums"};
static const uint32_t rates[]={MD_SAMPLE_RATE};
static const char *const *const labels=NAMES;
static const uint8_t preset_bytes[MD_SYNTHEX_PATCHES][MD_SYNTHEX_SETTINGS_BYTES]={{0,100},{1,100},{2,100},{3,100},{4,100},{5,100},{6,100},{7,100},{8,100},{9,100},{10,100},{11,100},{12,100},{13,100},{14,100},{15,100},{16,100},{17,100},{18,100},{19,100},{20,100},{21,100},{22,100},{23,100},{24,100},{25,100},{26,100},{27,100},{28,100},{29,100},{30,100},{31,100},{32,100},{33,100},{34,100},{35,100},{36,100},{37,100},{38,100},{39,100},{40,100},{41,100},{42,100},{43,100},{44,100},{45,100},{46,100},{47,100},{48,100},{49,100},{50,100},{51,100},{52,100},{53,100},{54,100},{55,100},{56,100},{57,100},{58,100},{59,100},{60,100},{61,100},{62,100},{63,100},{64,100},{65,100},{66,100},{67,100},{68,100},{69,100},{70,100},{71,100},{72,100},{73,100},{74,100},{75,100},{76,100},{77,100},{78,100},{79,100}};
static const md_preset_t presets[]={
    {1,"Chunky Synth",preset_bytes[0],2},
    {2,"Wah brass",preset_bytes[1],2},
    {3,"Fat Sound",preset_bytes[2],2},
    {4,"Metallic",preset_bytes[3],2},
    {5,"Pan flutes",preset_bytes[4],2},
    {6,"Tremolo piano",preset_bytes[5],2},
    {7,"Chorale",preset_bytes[6],2},
    {8,"Brass I",preset_bytes[7],2},
    {9,"Brass II",preset_bytes[8],2},
    {10,"Brass III",preset_bytes[9],2},
    {11,"Double basses",preset_bytes[10],2},
    {12,"Ceilos",preset_bytes[11],2},
    {13,"Violins",preset_bytes[12],2},
    {14,"Church organ",preset_bytes[13],2},
    {15,"Reed organ",preset_bytes[14],2},
    {16,"Electronic organ",preset_bytes[15],2},
    {17,"High pass sweep",preset_bytes[16],2},
    {18,"Filtered brass",preset_bytes[17],2},
    {19,"Filtered chorus",preset_bytes[18],2},
    {20,"Filtered strings",preset_bytes[19],2},
    {21,"Metallic piano",preset_bytes[20],2},
    {22,"Pianet",preset_bytes[21],2},
    {23,"Funky Clav.",preset_bytes[22],2},
    {24,"Clavichord I",preset_bytes[23],2},
    {25,"Clavichord II",preset_bytes[24],2},
    {26,"Clavinet",preset_bytes[25],2},
    {27,"Electric piano I",preset_bytes[26],2},
    {28,"Electric piano II",preset_bytes[27],2},
    {29,"Chimes",preset_bytes[28],2},
    {30,"Vibes",preset_bytes[29],2},
    {31,"Sweep 1",preset_bytes[30],2},
    {32,"Sweep 2",preset_bytes[31],2},
    {33,"Take-off",preset_bytes[32],2},
    {34,"Landing",preset_bytes[33],2},
    {35,"Wash of sound",preset_bytes[34],2},
    {36,"Fast decay",preset_bytes[35],2},
    {37,"Ring mod.",preset_bytes[36],2},
    {38,"Phased sweep",preset_bytes[37],2},
    {39,"Bass pluck",preset_bytes[38],2},
    {40,"Whistle",preset_bytes[39],2},
    {41,"Cathedral choir",preset_bytes[40],2},
    {42,"Sync. Harmonic Sweep",preset_bytes[41],2},
    {43,"Filtered Choir",preset_bytes[42],2},
    {44,"Harsh Clavinet",preset_bytes[43],2},
    {45,"Echo",preset_bytes[44],2},
    {46,"Double Bass - Nylon Guitar",preset_bytes[45],2},
    {47,"Sync. 1",preset_bytes[46],2},
    {48,"Sync. 2",preset_bytes[47],2},
    {49,"De-tuned pipe organ",preset_bytes[48],2},
    {50,"Normal Clavinet",preset_bytes[49],2},
    {51,"Harsh lead",preset_bytes[50],2},
    {52,"Solo Violin",preset_bytes[51],2},
    {53,"Bass Guitar",preset_bytes[52],2},
    {54,"Sync. 3",preset_bytes[53],2},
    {55,"Glide Strings",preset_bytes[54],2},
    {56,"Hammond 1 (John Lord)",preset_bytes[55],2},
    {57,"Phased Choir",preset_bytes[56],2},
    {58,"Brash Brass",preset_bytes[57],2},
    {59,"Brushed Cymbals",preset_bytes[58],2},
    {60,"Hammond 2 (Emerson)",preset_bytes[59],2},
    {61,"Plink",preset_bytes[60],2},
    {62,"Wah Ring Mod.",preset_bytes[61],2},
    {63,"Metallic Clavinet",preset_bytes[62],2},
    {64,"Bass Drum - tom-toms",preset_bytes[63],2},
    {65,"Hammond 3 (Brooker T)",preset_bytes[64],2},
    {66,"Plucked brass",preset_bytes[65],2},
    {67,"Snare Drum",preset_bytes[66],2},
    {68,"Distant Strings",preset_bytes[67],2},
    {69,"Watery Grave",preset_bytes[68],2},
    {70,"High Portamento",preset_bytes[69],2},
    {71,"Electric Piano",preset_bytes[70],2},
    {72,"Oriental Fourths",preset_bytes[71],2},
    {73,"Wood Blocks",preset_bytes[72],2},
    {74,"Marimba",preset_bytes[73],2},
    {75,"Harp",preset_bytes[74],2},
    {76,"Filtered brass",preset_bytes[75],2},
    {77,"Deep. Sync.",preset_bytes[76],2},
    {78,"Mini Moog Bass",preset_bytes[77],2},
    {79,"Mini Moog Lead",preset_bytes[78],2},
    {80,"Steel Drums",preset_bytes[79],2}
};
static const md_param_desc_t params[]={
    {.id=MD_SYNTHEX_PATCH,.flags=MD_PARAM_ENUM,.name="PATCH",.min=0,.max=MD_SYNTHEX_PATCHES-1,.initial=0,
     .unit=MD_UNIT_INTEGER,.labels=labels,.label_count=MD_SYNTHEX_PATCHES},
    {.id=MD_SYNTHEX_LEVEL,.flags=MD_PARAM_AUTOMATABLE,.name="LEVEL",.min=0,.max=100,.initial=100,.unit=MD_UNIT_PERCENT}
};
int md_synthex_find(const char *name)
{
    for(uint32_t i=0;i<MD_SYNTHEX_PATCHES;i++){         /* (no strcmp in a module: compared here) */
        const char *a=NAMES[i],*b=name;
        while(*a && *a==*b){a++;b++;}
        if(*a==*b)return (int)i;
    }
    return -1;
}
const char *md_synthex_name(uint32_t patch){return patch<MD_SYNTHEX_PATCHES ? NAMES[patch] : "";}
static void decode(sc_patch_t *p,uint32_t patch)   /* a factory record into a patch, its own reverb off */
{
    sc_preset_load(patch,p);
    p->fx.reverb.on=0;
}
static void load(md_synthex_state_t *s,uint32_t patch)   /* a new patch: the old one's voices cut, then reprepared */
{
    sc_all_off(&s->engine);
    decode(&s->engine.patch,patch);
    sc_reprep(&s->engine);
    s->patch=(uint8_t)patch;
}
static int validate(const uint8_t *src,uint32_t bytes)
{
    if(!src)return MD_ERR_ARGUMENT;
    if(bytes!=MD_SYNTHEX_SETTINGS_BYTES)return MD_ERR_FORMAT;
    return src[0]<MD_SYNTHEX_PATCHES && src[1]<=100u ? MD_OK : MD_ERR_RANGE;
}
static int apply(void *state,const uint8_t *src,uint32_t bytes)
{
    if(!state)return MD_ERR_ARGUMENT;
    int result=validate(src,bytes);if(result)return result;
    md_synthex_state_t *s=state;load(s,src[0]);s->level=src[1];return MD_OK;
}
static int encode(const void *state,uint8_t *dst,uint32_t capacity)
{
    int result=md_adapter_encode_check(state,dst,capacity,MD_SYNTHEX_SETTINGS_BYTES);if(result)return result;
    const md_synthex_state_t *s=state;dst[0]=s->patch;dst[1]=s->level;return MD_SYNTHEX_SETTINGS_BYTES;
}
static int init(void *state,uint32_t rate)
{
    if(!state)return MD_ERR_ARGUMENT;
    if(rate!=MD_SAMPLE_RATE)return MD_ERR_UNSUPPORTED;
    md_synthex_state_t *s=state;memset(s,0,sizeof *s);
    decode(&s->staged,0);sc_init(&s->engine,&s->staged);s->level=100;return MD_OK;
}
static void reset(void *state)
{
    md_synthex_state_t *s=state;uint8_t patch=s->patch,level=s->level;
    decode(&s->staged,patch);sc_init(&s->engine,&s->staged);
    s->patch=patch;s->level=level;s->voice_count=0;memset(s->voices,0,sizeof s->voices);
    s->dc_x[0]=s->dc_x[1]=s->dc_y[0]=s->dc_y[1]=0;
}
static int get_param(const void *state,md_param_id_t id,int32_t *value)
{
    if(!state || !value)return MD_ERR_ARGUMENT;
    const md_synthex_state_t *s=state;
    if(id==MD_SYNTHEX_PATCH){*value=s->patch;return MD_OK;}
    if(id==MD_SYNTHEX_LEVEL){*value=s->level;return MD_OK;}
    return MD_ERR_NOT_FOUND;
}
static int event(void *state,const md_event_t *e)
{
    if(!state || !e)return MD_ERR_ARGUMENT;
    md_synthex_state_t *s=state;
    if(e->kind==MD_EVENT_PARAM){
        if(e->flags)return MD_ERR_UNSUPPORTED;
        uint32_t id=e->data.param.id;int32_t v=e->data.param.value;
        if(id<1u || id>sizeof params/sizeof params[0])return MD_ERR_NOT_FOUND;
        if(v<params[id-1u].min || v>params[id-1u].max)return MD_ERR_RANGE;
        if(id==MD_SYNTHEX_PATCH){if((uint32_t)v!=s->patch)load(s,(uint32_t)v);}
        else s->level=(uint8_t)v;
        return MD_OK;
    }
    if(e->kind==MD_EVENT_PANIC){reset(s);return MD_OK;}
    if(e->kind!=MD_EVENT_NOTE_ON && e->kind!=MD_EVENT_NOTE_OFF)return MD_ERR_UNSUPPORTED;
    int result=md_adapter_note_check(e);if(result)return result;
    int found=md_adapter_voice_find(s->voices,s->voice_count,e->token);
    if(e->kind==MD_EVENT_NOTE_OFF){
        if(found>=0){sc_note_off(&s->engine,s->voices[found].key);md_adapter_voice_remove(s->voices,&s->voice_count,(uint32_t)found);}
        return MD_OK;
    }
    if(found>=0)return MD_ERR_STATE;
    for(uint32_t i=0;i<s->voice_count;i++)if(s->voices[i].key==e->data.note.key)return MD_ERR_STATE;
    if(s->voice_count==MD_SYNTHEX_TOKENS)return MD_ERR_CAPACITY;
    s->voices[s->voice_count++]=(md_adapter_voice_t){e->token,e->data.note.key};
    sc_note_on(&s->engine,e->data.note.key,(int32_t)e->data.note.velocity*516);return MD_OK;
}
static void render(void *state,int32_t *out,uint32_t frames)
{
    md_synthex_state_t *s=state;
    int32_t g=(int32_t)s->level*328;            /* Q15 */
    md_clear32(s->scratch,2u*frames);
    sc_render(&s->engine,s->scratch,frames);
    /* the outputs AC-coupled, as the instrument's are: a one-pole high-pass near 10 Hz a channel. The engine's
     * ring modulator leaves an offset (the laser harp: 6 % of full scale, 93 % of its energy under 40 Hz) that
     * took the mix's headroom and its level; y in Q8 so the pole's leak keeps the low bits. */
    for(uint32_t i=0;i<2u*frames;i++){
        uint32_t c=i&1u;int32_t x=s->scratch[i];
        int32_t y=(int32_t)((((int64_t)s->dc_y[c]*32721)>>15)+(int64_t)(x-s->dc_x[c])*256);
        s->dc_x[c]=x;s->dc_y[c]=y;
        out[i]+=(int32_t)((((int64_t)(y>>8))*g)>>15);
    }
}
const md_instrument_desc_t MD_SYNTHEX={
    .processor={.id=MD_INSTRUMENT_SYNTHEX,.api_version=MD_API_VERSION,.settings_version=MD_SYNTHEX_SETTINGS_VERSION,
        .name="SYNTHEX",.state_bytes=sizeof(md_synthex_state_t),.state_alignment=_Alignof(md_synthex_state_t),
        .sample_rates=rates,.sample_rate_count=1,.params=params,.param_count=sizeof params/sizeof params[0],
        .max_settings_bytes=MD_SYNTHEX_SETTINGS_BYTES},
    .capabilities=MD_CAP_POLY|MD_CAP_VELOCITY,.max_polyphony=SC_VOICES,.presets=presets,.preset_count=MD_SYNTHEX_PATCHES,
    .init=init,.event=event,.get_param=get_param,.render_add=render,.reset=reset,.encode=encode,.validate=validate,.apply=apply
};
