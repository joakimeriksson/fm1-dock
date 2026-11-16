/* SPDX-License-Identifier: GPL-3.0-only */
#include "effect.h"
#include "processor_internal.h"

static int descriptor_check(const md_effect_desc_t *d)
{
    if(!d)return MD_ERR_ARGUMENT;
    int status=md_internal_processor_check(&d->processor);
    if(status)return status;
    const uint16_t known=MD_EFFECT_INSERT|MD_EFFECT_BUS;
    if(!d->capabilities || (d->capabilities & ~known))return MD_ERR_ARGUMENT;
    if((d->processor.param_count && !d->get_param) || !d->init || !d->set_param || !d->reset || !d->encode || !d->validate || !d->apply)
        return MD_ERR_ARGUMENT;
    if((d->capabilities & MD_EFFECT_INSERT) && !d->process_replace)return MD_ERR_ARGUMENT;
    if((d->capabilities & MD_EFFECT_BUS) && !d->process_wet)return MD_ERR_ARGUMENT;
    return MD_OK;
}
static int ready(const md_effect_t *e){return e && e->desc && e->state;}
int md_effect_registry_validate(const md_effect_registry_t *registry)
{
    if(!registry || (registry->count && !registry->items))return MD_ERR_ARGUMENT;
    for(uint32_t i=0;i<registry->count;i++){
        int status=descriptor_check(registry->items[i]);if(status)return status;
        for(uint32_t j=0;j<i;j++)
            if(registry->items[j]->processor.id==registry->items[i]->processor.id)return MD_ERR_ARGUMENT;
    }
    return MD_OK;
}
const md_effect_desc_t *md_effect_find(const md_effect_registry_t *registry,uint32_t id)
{
    if(!registry || !id || (registry->count && !registry->items))return NULL;
    for(uint32_t i=0;i<registry->count;i++)
        if(registry->items[i] && registry->items[i]->processor.id==id)return registry->items[i];
    return NULL;
}
int md_effect_init(md_effect_t *e,const md_effect_desc_t *d,void *state,uint32_t bytes,uint32_t rate)
{
    if(!e)return MD_ERR_ARGUMENT;
    *e=(md_effect_t){0};
    int status=descriptor_check(d);if(status)return status;
    status=md_internal_state_check(&d->processor,state,bytes,rate);if(status)return status;
    if(md_internal_overlap(e,sizeof *e,state,d->processor.state_bytes))return MD_ERR_ARGUMENT;
    status=d->init(state,rate);if(status)return status;
    e->desc=d;e->state=state;e->sample_rate=rate;return MD_OK;
}
int md_effect_param(md_effect_t *e,md_param_id_t id,int32_t value)
{
    if(!ready(e))return MD_ERR_STATE;
    int status=md_internal_param_check(&e->desc->processor,id,value);
    return status ? status : e->desc->set_param(e->state,id,value);
}
int md_effect_param_get(const md_effect_t *e,md_param_id_t id,int32_t *value)
{
    if(!ready(e))return MD_ERR_STATE;
    if(!value)return MD_ERR_ARGUMENT;
    if(!id || id>MD_PARAM_ID_MAX)return MD_ERR_RANGE;
    if(md_internal_overlap(value,sizeof *value,e,sizeof *e) ||
        md_internal_overlap(value,sizeof *value,e->state,e->desc->processor.state_bytes))return MD_ERR_ARGUMENT;
    const md_processor_desc_t *processor=&e->desc->processor;
    const md_param_desc_t *param=NULL;
    for(uint32_t p=0;p<processor->param_count;p++)if(processor->params[p].id==id){param=&processor->params[p];break;}
    if(!param)return MD_ERR_NOT_FOUND;
    if(!e->desc->get_param)return MD_ERR_UNSUPPORTED;
    int32_t candidate=0;
    int status=e->desc->get_param(e->state,id,&candidate);
    if(status)return status<0 ? status : MD_ERR_STATE;
    if(candidate<param->min || candidate>param->max)return MD_ERR_STATE;
    *value=candidate;return MD_OK;
}
int md_effect_tempo(md_effect_t *e,uint16_t bpm_q8)
{
    if(!ready(e))return MD_ERR_STATE;
    if(bpm_q8<40u*256u || bpm_q8>240u*256u)return MD_ERR_RANGE;
    if(!e->desc->set_tempo)return MD_ERR_UNSUPPORTED;
    e->desc->set_tempo(e->state,bpm_q8);return MD_OK;
}
static int buffer_valid(const md_effect_t *e,const void *buffer,uint32_t frames)
{
    return ready(e) && buffer && frames && frames<=MD_MAX_RENDER_FRAMES &&
        !md_internal_overlap(buffer,frames*2u*sizeof(int32_t),e->state,e->desc->processor.state_bytes) &&
        !md_internal_overlap(buffer,frames*2u*sizeof(int32_t),e,sizeof *e);
}
void md_effect_process_replace(md_effect_t *e,int32_t *stereo,uint32_t frames)
{
    if(buffer_valid(e,stereo,frames) && (e->desc->capabilities & MD_EFFECT_INSERT))
        e->desc->process_replace(e->state,stereo,frames);
}
void md_effect_process_wet(md_effect_t *e,const int32_t *input,int32_t *wet,uint32_t frames)
{
    if(!buffer_valid(e,input,frames) || !buffer_valid(e,wet,frames) || !(e->desc->capabilities & MD_EFFECT_BUS))
        return;
    uint32_t bytes=frames*2u*sizeof(int32_t);
    if(md_internal_overlap(input,bytes,wet,bytes))return;
    e->desc->process_wet(e->state,input,wet,frames);
}
void md_effect_reset(md_effect_t *e){if(ready(e))e->desc->reset(e->state);}
int md_effect_encode(const md_effect_t *e,uint8_t *dst,uint32_t capacity)
{
    if(!ready(e))return MD_ERR_STATE;
    if(!dst && capacity)return MD_ERR_ARGUMENT;
    int bytes=e->desc->encode(e->state,dst,capacity);
    if(bytes>=0 && ((uint32_t)bytes>capacity || (uint32_t)bytes>e->desc->processor.max_settings_bytes))
        return MD_ERR_STATE;
    return bytes;
}
int md_effect_validate(const md_effect_desc_t *d,uint16_t version,const uint8_t *src,uint32_t bytes)
{
    int status=descriptor_check(d);if(status)return status;
    if(version!=d->processor.settings_version)return MD_ERR_VERSION;
    if(!src && bytes)return MD_ERR_ARGUMENT;
    if(bytes>d->processor.max_settings_bytes)return MD_ERR_CAPACITY;
    return d->validate(src,bytes);
}
int md_effect_apply(md_effect_t *e,uint16_t version,const uint8_t *src,uint32_t bytes)
{
    if(!ready(e))return MD_ERR_STATE;
    int status=md_effect_validate(e->desc,version,src,bytes);
    return status ? status : e->desc->apply(e->state,src,bytes);
}
