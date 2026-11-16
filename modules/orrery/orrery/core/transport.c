/* SPDX-License-Identifier: GPL-3.0-only */
/* Fractional remainder carry follows the portable arithmetic seam audited in
 * Melodee mix.c int_clock_units, revision165d177a425944f3d30b0b669255a6ed0050c3a5.
 * Adapted for explicit instance state, BPM Q8, hundredth-tick units and a bounded
 * event-offset query; no upstream function body/platform code is copied here.
 * See work/decisions/001-sequencer-reuse.md and work/evidence/transport.md. */
#include "transport.h"
#include <stddef.h>

#define CHUNK 6u
#define MAX_RATE ((uint64_t)MD_MAX_BPM_Q8 * MD_UNITS_PER_QUARTER)
_Static_assert(CHUNK * MAX_RATE + MD_TRANSPORT_DENOMINATOR - 1u <= UINT32_MAX,
               "transport chunk product must fit 32 bits");
static int position_valid(md_position_t p) {return p.unit<MD_UNITS_PER_QUARTER && !p.reserved;}
static int valid(const md_transport_t *t)
{
    return t && position_valid(t->position) && t->bpm_q8>=MD_MIN_BPM_Q8 && t->bpm_q8<=MD_MAX_BPM_Q8 &&
        t->running<=1u && !t->reserved && t->remainder<MD_TRANSPORT_DENOMINATOR;
}
int md_transport_init(md_transport_t *t, uint16_t tempo)
{
    if (!t) return MD_ERR_ARGUMENT;
    *t=(md_transport_t){0};
    if (tempo<MD_MIN_BPM_Q8 || tempo>MD_MAX_BPM_Q8) return MD_ERR_RANGE;
    t->bpm_q8=tempo;return MD_OK;
}
int md_transport_start(md_transport_t *t, md_position_t p)
{
    if (!valid(t)) return MD_ERR_STATE;
    if (!position_valid(p)) return MD_ERR_RANGE;
    t->position=p;t->remainder=0;t->running=1;return MD_OK;
}
int md_transport_stop(md_transport_t *t)
{
    if (!valid(t)) return MD_ERR_STATE;
    t->running=0;return MD_OK;
}
int md_transport_continue(md_transport_t *t)
{
    if (!valid(t)) return MD_ERR_STATE;
    t->running=1;return MD_OK;
}
int md_transport_seek(md_transport_t *t, md_position_t p)
{
    if (!valid(t) || t->running) return MD_ERR_STATE;
    if (!position_valid(p)) return MD_ERR_RANGE;
    t->position=p;t->remainder=0;return MD_OK;
}
int md_transport_set_tempo(md_transport_t *t, uint16_t tempo)
{
    if (!valid(t)) return MD_ERR_STATE;
    if (tempo<MD_MIN_BPM_Q8 || tempo>MD_MAX_BPM_Q8) return MD_ERR_RANGE;
    /* The denominator does not depend on tempo: remainder retains the exact
     * fractional musical position; only subsequent sample increments change. */
    t->bpm_q8=tempo;return MD_OK;
}
int md_transport_advance(md_transport_t *t, uint32_t frames)
{
    if (!valid(t)) return MD_ERR_STATE;
    if (frames>MD_MAX_RENDER_FRAMES) return MD_ERR_RANGE;
    t->samples+=frames;
    if (!t->running) return MD_OK;
    uint32_t units=t->position.unit, rate=t->bpm_q8*MD_UNITS_PER_QUARTER;
    /* Chunk<=6 proves rate*chunk+remainder fits uint32 at 240BPM Q8. One bounded
     * 32-bit quotient/remainder pair per chunk, none of the target's unavailable
     * general 64-bit divide helpers. No render/DSP call depends on this chunk. */
    while (frames) {
        uint32_t n=frames<CHUNK?frames:CHUNK;
        uint32_t numerator=n*rate+t->remainder;
        units+=numerator/MD_TRANSPORT_DENOMINATOR;
        t->remainder=numerator%MD_TRANSPORT_DENOMINATOR;frames-=n;
    }
    if (units>=MD_UNITS_PER_QUARTER) {units-=MD_UNITS_PER_QUARTER;t->position.quarter++;}
    t->position.unit=(uint16_t)units;return MD_OK;
}
int md_position_add(md_position_t p, uint32_t units, md_position_t *result)
{
    if (!result) return MD_ERR_ARGUMENT;
    if (!position_valid(p)) return MD_ERR_RANGE;
    uint32_t phase=p.unit+units%MD_UNITS_PER_QUARTER;
    p.quarter+=units/MD_UNITS_PER_QUARTER+phase/MD_UNITS_PER_QUARTER;
    p.unit=(uint16_t)(phase%MD_UNITS_PER_QUARTER);*result=p;return MD_OK;
}
int md_transport_offset(const md_transport_t *t, md_position_t target, uint32_t limit, uint32_t *offset)
{
    if (!valid(t) || !t->running) return MD_ERR_STATE;
    if (!offset) return MD_ERR_ARGUMENT;
    if (!position_valid(target) || limit>MD_MAX_RENDER_FRAMES) return MD_ERR_RANGE;
    uint32_t quarters=target.quarter-t->position.quarter;
    if (quarters==0x80000000u) return MD_ERR_RANGE; /* ambiguous modular horizon */
    if (quarters>0x80000000u || (!quarters && target.unit<=t->position.unit)) {*offset=0;return MD_OK;}
    /* <=256 samples cannot cross more than one musical quarter at <=240BPM. */
    if (quarters>1u) return MD_ERR_NOT_FOUND;
    uint32_t units=quarters*MD_UNITS_PER_QUARTER+target.unit-t->position.unit;
    uint64_t goal=(uint64_t)units*MD_TRANSPORT_DENOMINATOR;
    uint32_t rate=t->bpm_q8*MD_UNITS_PER_QUARTER;
    if ((uint64_t)limit*rate+t->remainder<goal) return MD_ERR_NOT_FOUND;
    /* At most9 comparisons. Products fit 64 bits; divide is avoided entirely.
     * This yields ceil((goal-remainder)/rate), exact to the first sample. */
    uint32_t lo=0,hi=limit;
    while (lo<hi) {
        uint32_t mid=lo+(hi-lo)/2u;
        if ((uint64_t)mid*rate+t->remainder>=goal) hi=mid;
        else lo=mid+1u;
    }
    *offset=lo;return MD_OK;
}
uint32_t md_step_straight_units(uint8_t division)
{
    static const uint32_t lengths[]={1200u,2400u,4800u,9600u,19200u,38400u};
    return division<sizeof lengths/sizeof lengths[0]?lengths[division]:0;
}
uint32_t md_step_span_units(uint8_t division, uint8_t swing, uint8_t parity)
{
    uint32_t base=md_step_straight_units(division);
    if (!base || swing<50u || swing>75u || parity>1u) return 0;
    return base*2u*(parity?100u-swing:swing)/100u;
}
