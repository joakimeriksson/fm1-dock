/* SPDX-License-Identifier: GPL-3.0-only */
#include "effects.h"

static const md_effect_desc_t *const items[] = {&MD_FX_FILTER, &MD_FX_PHASER, &MD_FX_ECHO, &MD_FX_REVERB};
const md_effect_registry_t MD_FX_REGISTRY = {items, 4};
