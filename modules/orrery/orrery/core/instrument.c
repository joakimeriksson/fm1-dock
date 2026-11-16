/* SPDX-License-Identifier: GPL-3.0-only */
#include "instrument_internal.h"
#include "processor_internal.h"

static int descriptor_check(const md_instrument_desc_t *d)
{
    if (!d) return MD_ERR_ARGUMENT;
    int status=md_internal_processor_check(&d->processor);
    if (status) return status;
    const uint32_t known=MD_CAP_MONO|MD_CAP_POLY|MD_CAP_PERCUSSION|MD_CAP_VELOCITY|
        MD_CAP_ACCENT|MD_CAP_LEGATO|MD_CAP_SLIDE|MD_CAP_PRESSURE|MD_CAP_BEND;
    if ((d->capabilities & ~known) || !d->max_polyphony ||
        ((d->capabilities & (MD_CAP_MONO|MD_CAP_POLY))==(MD_CAP_MONO|MD_CAP_POLY)) ||
        (!(d->capabilities & (MD_CAP_MONO|MD_CAP_POLY|MD_CAP_PERCUSSION))) ||
        ((d->capabilities & MD_CAP_MONO) && d->max_polyphony!=1u)) return MD_ERR_ARGUMENT;
    if ((d->processor.param_count && !d->get_param) || !d->init || !d->event || !d->render_add || !d->reset ||
        !d->encode || !d->validate || !d->apply) return MD_ERR_ARGUMENT;
    if ((d->preset_count && !d->presets) || (!d->preset_count && d->presets)) return MD_ERR_ARGUMENT;
    for (uint32_t i=0;i<d->preset_count;i++) {
        const md_preset_t *p=&d->presets[i];
        if (!p->id || !p->name || !p->name[0] || (p->settings_bytes && !p->settings) ||
            p->settings_bytes>d->processor.max_settings_bytes) return MD_ERR_ARGUMENT;
        for (uint32_t j=0;j<i;j++) if (d->presets[j].id==p->id) return MD_ERR_ARGUMENT;
        status=d->validate(p->settings,p->settings_bytes);
        if (status) return status;
    }
    if (d->capabilities & MD_CAP_PERCUSSION) {
        if (!d->drum_voice_count || d->drum_voice_count>14u || !d->drum_voices) return MD_ERR_ARGUMENT;
        for (uint32_t i=0;i<d->drum_voice_count;i++) {
            const md_drum_voice_desc_t *v=&d->drum_voices[i];
            if (v->id>=14u || !v->name || !v->name[0] ||
                (v->midi_note>127u && v->midi_note!=255u)) return MD_ERR_RANGE;
            for (uint32_t j=0;j<i;j++) if (d->drum_voices[j].id==v->id) return MD_ERR_ARGUMENT;
        }
    } else if (d->drum_voice_count || d->drum_voices) return MD_ERR_ARGUMENT;
    return MD_OK;
}
static int ready(const md_instrument_t *i) { return i && i->desc && i->state; }
int md_instrument_registry_validate(const md_instrument_registry_t *registry)
{
    if (!registry || (registry->count && !registry->items)) return MD_ERR_ARGUMENT;
    for (uint32_t i=0;i<registry->count;i++) {
        int status=descriptor_check(registry->items[i]);
        if (status) return status;
        for (uint32_t j=0;j<i;j++)
            if (registry->items[j]->processor.id==registry->items[i]->processor.id) return MD_ERR_ARGUMENT;
    }
    return MD_OK;
}
const md_instrument_desc_t *md_instrument_find(const md_instrument_registry_t *registry, uint32_t id)
{
    if (!registry || !id || (registry->count && !registry->items)) return NULL;
    for (uint32_t i=0;i<registry->count;i++)
        if (registry->items[i] && registry->items[i]->processor.id==id) return registry->items[i];
    return NULL;
}
int md_instrument_init(md_instrument_t *i, const md_instrument_desc_t *d, void *state,
                       uint32_t bytes, uint32_t rate)
{
    if (!i) return MD_ERR_ARGUMENT;
    *i=(md_instrument_t){0};
    int status=descriptor_check(d);
    if (status) return status;
    status=md_internal_state_check(&d->processor,state,bytes,rate);
    if (status) return status;
    if (md_internal_overlap(i,sizeof *i,state,d->processor.state_bytes)) return MD_ERR_ARGUMENT;
    status=d->init(state,rate);
    if (status) return status;
    i->desc=d; i->state=state; i->sample_rate=rate;
    return MD_OK;
}
int md_internal_instrument_event_check(const md_instrument_t *i, const md_event_t *e)
{
    if (!ready(i)) return MD_ERR_STATE;
    if (!e || e->reserved) return MD_ERR_ARGUMENT;
    if (e->track>=MD_TRACKS && !((e->kind==MD_EVENT_PANIC || e->kind==MD_EVENT_SOURCE_RELEASE) &&
        e->track==MD_ALL_TRACKS)) return MD_ERR_RANGE;
    if (!md_internal_source_valid(e->source) &&
        !(e->source==MD_SOURCE_NONE && (e->kind==MD_EVENT_PARAM || e->kind==MD_EVENT_PANIC))) return MD_ERR_RANGE;
    uint32_t cap=i->desc->capabilities;
    if (e->kind==MD_EVENT_NOTE_ON || e->kind==MD_EVENT_NOTE_OFF) {
        if (!(cap & (MD_CAP_MONO|MD_CAP_POLY))) return MD_ERR_UNSUPPORTED;
        if (!e->token) return MD_ERR_RANGE;
        if (e->kind==MD_EVENT_NOTE_OFF) return e->flags ? MD_ERR_ARGUMENT : MD_OK;
        if (e->data.note.key>127u || !e->data.note.velocity || e->data.note.velocity>127u) return MD_ERR_RANGE;
        if (e->flags & ~(MD_NOTE_ACCENT|MD_NOTE_LEGATO|MD_NOTE_SLIDE)) return MD_ERR_ARGUMENT;
        if (((e->flags & MD_NOTE_ACCENT) && !(cap & MD_CAP_ACCENT)) ||
            ((e->flags & MD_NOTE_LEGATO) && !(cap & MD_CAP_LEGATO)) ||
            ((e->flags & MD_NOTE_SLIDE) && !(cap & MD_CAP_SLIDE))) return MD_ERR_UNSUPPORTED;
        return MD_OK;
    }
    if (e->flags || (e->token && e->kind!=MD_EVENT_HIT)) return MD_ERR_ARGUMENT;
    if (e->kind==MD_EVENT_HIT) {
        if (!(cap & MD_CAP_PERCUSSION)) return MD_ERR_UNSUPPORTED;
        if (!e->data.hit.velocity || e->data.hit.velocity>127u) return MD_ERR_RANGE;
        for (uint32_t j=0;j<i->desc->drum_voice_count;j++)
            if (i->desc->drum_voices[j].id==e->data.hit.voice) return MD_OK;
        return MD_ERR_NOT_FOUND;
    }
    if (e->kind==MD_EVENT_PARAM) return md_internal_param_check(&i->desc->processor,e->data.param.id,e->data.param.value);
    if (e->kind==MD_EVENT_BEND) {
        if (!(cap & MD_CAP_BEND)) return MD_ERR_UNSUPPORTED;
        return e->data.value<-(48*65536) || e->data.value>48*65536 ? MD_ERR_RANGE : MD_OK;
    }
    if (e->kind==MD_EVENT_PRESSURE) {
        if (!(cap & MD_CAP_PRESSURE)) return MD_ERR_UNSUPPORTED;
        return e->data.value<0 || e->data.value>127 ? MD_ERR_RANGE : MD_OK;
    }
    if (e->kind==MD_EVENT_PANIC) return MD_OK;
    return e->kind==MD_EVENT_SOURCE_RELEASE ? MD_ERR_UNSUPPORTED : MD_ERR_ARGUMENT;
}
int md_instrument_event(md_instrument_t *i, const md_event_t *e)
{
    int status=md_internal_instrument_event_check(i,e);
    if (status) return status;
    if (e->kind==MD_EVENT_PANIC) { i->desc->reset(i->state); return MD_OK; }
    return i->desc->event(i->state,e);
}
int md_instrument_param_get(const md_instrument_t *i,md_param_id_t id,int32_t *value)
{
    if(!ready(i))return MD_ERR_STATE;
    if(!value)return MD_ERR_ARGUMENT;
    if(!id || id>MD_PARAM_ID_MAX)return MD_ERR_RANGE;
    if(md_internal_overlap(value,sizeof *value,i,sizeof *i) ||
        md_internal_overlap(value,sizeof *value,i->state,i->desc->processor.state_bytes))return MD_ERR_ARGUMENT;
    const md_processor_desc_t *processor=&i->desc->processor;
    const md_param_desc_t *param=NULL;
    for(uint32_t p=0;p<processor->param_count;p++)if(processor->params[p].id==id){param=&processor->params[p];break;}
    if(!param)return MD_ERR_NOT_FOUND;
    if(!i->desc->get_param)return MD_ERR_UNSUPPORTED;
    int32_t candidate=0;
    int status=i->desc->get_param(i->state,id,&candidate);
    if(status)return status<0 ? status : MD_ERR_STATE;
    if(candidate<param->min || candidate>param->max)return MD_ERR_STATE;
    *value=candidate;return MD_OK;
}
void md_instrument_render_add(md_instrument_t *i, int32_t *out, uint32_t frames)
{
    if (ready(i) && out && frames && frames<=MD_MAX_RENDER_FRAMES) i->desc->render_add(i->state,out,frames);
}
void md_instrument_reset(md_instrument_t *i) { if (ready(i)) i->desc->reset(i->state); }
int md_sound_encode(const md_instrument_t *i, uint8_t *dst, uint32_t capacity)
{
    if (!ready(i)) return MD_ERR_STATE;
    if (!dst && capacity) return MD_ERR_ARGUMENT;
    int bytes=i->desc->encode(i->state,dst,capacity);
    if (bytes>=0 && ((uint32_t)bytes>capacity || (uint32_t)bytes>i->desc->processor.max_settings_bytes)) return MD_ERR_STATE;
    return bytes;
}
int md_sound_validate(const md_instrument_desc_t *d, uint16_t version, const uint8_t *src, uint32_t bytes)
{
    int status=descriptor_check(d);
    if (status) return status;
    if (version!=d->processor.settings_version) return MD_ERR_VERSION;
    if (!src && bytes) return MD_ERR_ARGUMENT;
    if (bytes>d->processor.max_settings_bytes) return MD_ERR_CAPACITY;
    return d->validate(src,bytes);
}
int md_sound_apply(md_instrument_t *i, uint16_t version, const uint8_t *src, uint32_t bytes)
{
    if (!ready(i)) return MD_ERR_STATE;
    int status=md_sound_validate(i->desc,version,src,bytes);
    return status ? status : i->desc->apply(i->state,src,bytes);
}
