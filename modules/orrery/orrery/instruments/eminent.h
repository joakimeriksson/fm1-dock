/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_EMINENT_H
#define MD_EMINENT_H
#include "core/instrument.h"
#include "em_engine.h"   /* fm1-dock: the engine of this tree (take.py) */
#include "adapter_common.h"
#include "builtin_ids.h"
#define MD_EMINENT_SETTINGS_VERSION 1u
#define MD_EMINENT_SETTINGS_BYTES 8u
enum {
    MD_EMINENT_REGISTRATION=1, MD_EMINENT_ATTACK, MD_EMINENT_RELEASE,
    MD_EMINENT_ENSEMBLE, MD_EMINENT_PHASER, MD_EMINENT_COLOR, MD_EMINENT_LEVEL
};
typedef struct {
    em_t engine; uint32_t voice_count;
    md_adapter_voice_t voices[EM_VOICES];
} md_eminent_state_t;
extern const md_instrument_desc_t MD_EMINENT;
#endif
