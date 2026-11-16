/* SPDX-License-Identifier: GPL-3.0-only */
#include "commands.h"
#include <string.h>

void md_ring_init(md_command_ring_t *r)
{
    if (r) memset(r, 0, sizeof *r);
}
uint32_t md_ring_used(const md_command_ring_t *r)
{
    uint32_t head = r->head, tail = r->tail;
    return head - tail; /* free-running indices: unsigned difference survives wrap */
}
int md_ring_push(md_command_ring_t *r, const md_command_t *c, int release)
{
    if (!r || !c) return MD_ERR_ARGUMENT;
    uint32_t head = r->head;
    uint32_t tail = r->tail;
    MD_FENCE(); /* read the consumer's tail before reusing the slot it freed */
    uint32_t used = head - tail;
    uint32_t limit = release ? MD_COMMAND_CAPACITY : MD_COMMAND_CAPACITY - MD_RELEASE_RESERVE;
    if (used >= limit) {
        if (release) r->release_refused++;
        else r->rejected++;
        return MD_ERR_CAPACITY;
    }
    r->slot[head & (MD_COMMAND_CAPACITY - 1u)] = *c;
    MD_FENCE(); /* payload before publication */
    r->head = head + 1u;
    if (used + 1u > r->high_water) r->high_water = used + 1u;
    return MD_OK;
}
int md_ring_pop(md_command_ring_t *r, md_command_t *out)
{
    if (!r || !out) return 0;
    uint32_t tail = r->tail;
    uint32_t head = r->head;
    if (head == tail) return 0;
    MD_FENCE(); /* see the payload published with head */
    *out = r->slot[tail & (MD_COMMAND_CAPACITY - 1u)];
    MD_FENCE(); /* finish reading before the producer may reuse the slot */
    r->tail = tail + 1u;
    return 1;
}
