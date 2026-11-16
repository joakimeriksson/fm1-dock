/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_MINIPOPS_H
#define MD_MINIPOPS_H
#include "core/instrument.h"
#include "mp_engine.h"   /* fm1-dock: the engine of this tree (take.py) */
#include "builtin_ids.h"
#define MD_MINIPOPS_SETTINGS_VERSION 2u /* 2: the three extra-rhythm levels */
#define MD_MINIPOPS_SETTINGS_BYTES 5u
/* QUIJADA/GUIRO/TAMBOURINE scale hits on those voices, as the machine's sliders do; 0 mutes them. */
enum { MD_MINIPOPS_LEVEL=1, MD_MINIPOPS_BALANCE, MD_MINIPOPS_QUIJADA, MD_MINIPOPS_GUIRO, MD_MINIPOPS_TAMBOURINE };
typedef struct { mp_t engine; } md_minipops_state_t;
extern const md_instrument_desc_t MD_MINIPOPS;
#endif
