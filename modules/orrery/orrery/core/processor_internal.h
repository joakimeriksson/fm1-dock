/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_PROCESSOR_INTERNAL_H
#define MD_PROCESSOR_INTERNAL_H
#include "event.h"
#include <stddef.h>

static inline int md_internal_source_valid(uint16_t source)
{
    return source < MD_TRACKS || source == MD_SOURCE_PANEL || (source >= 8u && source < 56u);
}
static inline int md_internal_overlap(const void *a, uint32_t an, const void *b, uint32_t bn)
{
    uintptr_t x=(uintptr_t)a, y=(uintptr_t)b;
    return x <= y ? y-x < an : x-y < bn;
}
static inline int md_internal_processor_check(const md_processor_desc_t *d)
{
    const uint16_t flags=MD_PARAM_AUTOMATABLE|MD_PARAM_SMOOTH|MD_PARAM_ENUM|MD_PARAM_READ_ONLY;
    if (!d || !d->id || !d->name || !d->name[0]) return MD_ERR_ARGUMENT;
    if (d->api_version != MD_API_VERSION || !d->settings_version) return MD_ERR_VERSION;
    if (!d->state_bytes || !d->state_alignment ||
        (d->state_alignment & (d->state_alignment-1u))) return MD_ERR_ARGUMENT;
    if (!d->sample_rate_count || !d->sample_rates ||
        (d->param_count && !d->params) || (!d->param_count && d->params)) return MD_ERR_ARGUMENT;
    for (uint32_t i=0;i<d->sample_rate_count;i++) {
        if (!d->sample_rates[i]) return MD_ERR_RANGE;
        for (uint32_t j=0;j<i;j++) if (d->sample_rates[j]==d->sample_rates[i]) return MD_ERR_ARGUMENT;
    }
    for (uint32_t i=0;i<d->param_count;i++) {
        const md_param_desc_t *p=&d->params[i];
        if (!p->id || p->id>MD_PARAM_ID_MAX || !p->name || !p->name[0] ||
            (p->flags & ~flags) || p->min>p->max || p->initial<p->min || p->initial>p->max ||
            (uint32_t)p->unit>MD_UNIT_Q15 ||
            ((p->flags & MD_PARAM_READ_ONLY) && (p->flags & MD_PARAM_AUTOMATABLE))) return MD_ERR_RANGE;
        for (uint32_t j=0;j<i;j++) if (d->params[j].id==p->id) return MD_ERR_ARGUMENT;
        if (p->flags & MD_PARAM_ENUM) {
            int64_t count=(int64_t)p->max-(int64_t)p->min+1;
            if (!p->labels || count != p->label_count) return MD_ERR_RANGE;
            for (uint32_t j=0;j<p->label_count;j++)
                if (!p->labels[j] || !p->labels[j][0]) return MD_ERR_ARGUMENT;
        } else if (p->labels || p->label_count) return MD_ERR_ARGUMENT;
    }
    return MD_OK;
}
static inline int md_internal_state_check(const md_processor_desc_t *d, void *state,
                                          uint32_t bytes, uint32_t rate)
{
    if (!state || ((uintptr_t)state & (d->state_alignment-1u))) return MD_ERR_ARGUMENT;
    if (bytes < d->state_bytes) return MD_ERR_CAPACITY;
    for (uint32_t i=0;i<d->sample_rate_count;i++) if (rate==d->sample_rates[i]) return MD_OK;
    return MD_ERR_UNSUPPORTED;
}
static inline int md_internal_param_check(const md_processor_desc_t *d, uint16_t id, int32_t value)
{
    if (!id || id>MD_PARAM_ID_MAX) return MD_ERR_RANGE;
    for (uint32_t i=0;i<d->param_count;i++) if (d->params[i].id==id) {
        if (d->params[i].flags & MD_PARAM_READ_ONLY) return MD_ERR_UNSUPPORTED;
        return value<d->params[i].min || value>d->params[i].max ? MD_ERR_RANGE : MD_OK;
    }
    return MD_ERR_NOT_FOUND;
}
#endif
