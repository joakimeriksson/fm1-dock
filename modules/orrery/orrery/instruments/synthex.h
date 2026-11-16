/* SPDX-License-Identifier: GPL-3.0-only */
/* The Elka Synthex (Scintilla's engine, engines/sc_*) as an instrument: its 80 factory patches, "Ring mod." the
 * laser harp among them; each patch's own stereo chorus and limiter; the reverb is the session's. */
#ifndef MD_SYNTHEX_H
#define MD_SYNTHEX_H
#include "synthex_config.h"
#include "core/instrument.h"
#include "engines/sc_synth.h"
#include "adapter_common.h"
#include "builtin_ids.h"
#define MD_SYNTHEX_SETTINGS_VERSION 1u
#define MD_SYNTHEX_SETTINGS_BYTES 2u    /* patch, level */
#define MD_SYNTHEX_PATCHES 80u
#define MD_SYNTHEX_TOKENS 16u
enum { MD_SYNTHEX_PATCH = 1, MD_SYNTHEX_LEVEL };
typedef struct {
    sc_synth_t engine;
    uint8_t patch, level;
    uint32_t voice_count;
    md_adapter_voice_t voices[MD_SYNTHEX_TOKENS];
    int32_t scratch[2u * 256u];                 /* a render span before its level */
    int32_t dc_x[2], dc_y[2];                   /* the output's DC blocker, a channel each (x Q0, y Q8) */
    sc_patch_t staged;                          /* a factory record decoded, for sc_init */
} md_synthex_state_t;
extern const md_instrument_desc_t MD_SYNTHEX;
/* the patch called name (exact), or -1 */
int md_synthex_find(const char *name);
const char *md_synthex_name(uint32_t patch);
#endif
