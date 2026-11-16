/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_INSTRUMENT_INTERNAL_H
#define MD_INSTRUMENT_INTERNAL_H
#include "instrument.h"
/* Pure validation shared by public dispatch and router admission. Does not call
 * the backend or walk descriptor registries in the audio path. */
int md_internal_instrument_event_check(const md_instrument_t *, const md_event_t *);
#endif
