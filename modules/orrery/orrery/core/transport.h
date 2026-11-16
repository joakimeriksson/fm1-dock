/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_TRANSPORT_H
#define MD_TRANSPORT_H
#include "project_format.h"

#define MD_PPQN 96u
/* Hundredths of one musical tick make all v1 integer gate/swing percentages
 * exact, rather than rounding their durations to the coarse 96-PPQN grid. */
#define MD_UNITS_PER_TICK 100u
#define MD_UNITS_PER_QUARTER (MD_PPQN * MD_UNITS_PER_TICK)
#define MD_TRANSPORT_DENOMINATOR (MD_SAMPLE_RATE * 60u * 256u)
#define MD_MIN_BPM_Q8 (40u * 256u)
#define MD_MAX_BPM_Q8 (240u * 256u)

typedef struct {
    uint32_t quarter; /* wrapping quarter-note count; compare within half its range */
    uint16_t unit; /* 0..9599 within quarter; independent of sample counter */
    uint16_t reserved;
} md_position_t;
typedef struct {
    md_position_t position;
    uint32_t samples; /* wrapping wall-clock sample count, advances even stopped */
    uint32_t remainder; /* fractional musical unit numerator, common denominator */
    uint16_t bpm_q8;
    uint8_t running, reserved;
} md_transport_t;

int md_transport_init(md_transport_t *, uint16_t bpm_q8);
int md_transport_start(md_transport_t *, md_position_t position);
int md_transport_stop(md_transport_t *);
int md_transport_continue(md_transport_t *);
int md_transport_seek(md_transport_t *, md_position_t position); /* stopped only */
int md_transport_set_tempo(md_transport_t *, uint16_t bpm_q8); /* preserve phase */
int md_transport_advance(md_transport_t *, uint32_t frames); /* 0..256 */
int md_position_add(md_position_t position, uint32_t units, md_position_t *result);
/* Earliest integer sample offset reaching a target, 0..limit inclusive. Past
 * targets return 0; targets beyond limit return NOT_FOUND. Offset==block length
 * belongs to NEXT block. Stopped returns STATE. No 64-bit division is used.
 * Target comparisons require a horizon less than 2^31 quarter notes. */
int md_transport_offset(const md_transport_t *, md_position_t target, uint32_t limit,
                         uint32_t *offset);
/* Invalid division/swing/parity returns zero. Parity advances across odd-length
 * pattern wraps; even visit delays the following off-step, odd visit catches up. */
uint32_t md_step_straight_units(uint8_t division);
uint32_t md_step_span_units(uint8_t division, uint8_t swing, uint8_t visit_parity);
#endif
