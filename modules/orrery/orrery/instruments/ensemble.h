/* SPDX-License-Identifier: GPL-3.0-only */
/* Caravel's ENSEMBLE (engines/ens_engine.*) as an instrument: its fourteen factory pads, strings to choir to dark,
 * each with its stereo ensemble. A preset sets every control; ATTACK, RELEASE, ENS (the taps' depth) and TONE then
 * move from it. */
#ifndef MD_ENSEMBLE_H
#define MD_ENSEMBLE_H
#include "core/instrument.h"
#include "engines/ens_engine.h"
#include "adapter_common.h"
#include "builtin_ids.h"
#define MD_ENSEMBLE_SETTINGS_VERSION 1u
#define MD_ENSEMBLE_SETTINGS_BYTES 6u   /* preset, attack, release, ens, tone, level */
#define MD_ENSEMBLE_TOKENS 16u
enum { MD_ENSEMBLE_PRESET = 1, MD_ENSEMBLE_ATTACK, MD_ENSEMBLE_RELEASE, MD_ENSEMBLE_ENS, MD_ENSEMBLE_TONE, MD_ENSEMBLE_LEVEL };
typedef struct {
    ens_t engine; uint8_t preset, level; uint32_t voice_count;
    md_adapter_voice_t voices[MD_ENSEMBLE_TOKENS];
} md_ensemble_state_t;
extern const md_instrument_desc_t MD_ENSEMBLE;
#endif
