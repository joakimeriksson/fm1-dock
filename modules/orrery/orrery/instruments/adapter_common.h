/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_ADAPTER_COMMON_H
#define MD_ADAPTER_COMMON_H
#include "core/instrument.h"
#include <stddef.h>
#include <string.h>

/* Physical voice book only. Logical source arbitration belongs to core/router. */
typedef struct { md_note_token_t token; uint8_t key; } md_adapter_voice_t;
static inline int md_adapter_note_check(const md_event_t *event)
{
    if (event->flags) return MD_ERR_UNSUPPORTED;
    if (!event->token) return MD_ERR_RANGE;
    if (event->kind==MD_EVENT_NOTE_ON &&
        (event->data.note.key>127u || !event->data.note.velocity || event->data.note.velocity>127u))
        return MD_ERR_RANGE;
    return MD_OK;
}
static inline int md_adapter_voice_find(const md_adapter_voice_t *voices, uint32_t count, md_note_token_t token)
{
    for (uint32_t i=0;i<count;i++) if (voices[i].token==token) return (int)i;
    return -1;
}
static inline void md_adapter_voice_remove(md_adapter_voice_t *voices, uint32_t *count, uint32_t index)
{
    for (uint32_t i=index+1u;i<*count;i++) voices[i-1u]=voices[i];
    voices[--*count]=(md_adapter_voice_t){0};
}
static inline int md_adapter_encode_check(const void *state, uint8_t *dst, uint32_t capacity, uint32_t bytes)
{
    if (!state || !dst) return MD_ERR_ARGUMENT;
    return capacity<bytes ? MD_ERR_CAPACITY : MD_OK;
}
#endif
