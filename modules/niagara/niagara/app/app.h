/* SPDX-License-Identifier: GPL-3.0-only */
/* Niagara Drift app: the project (everything that is saved) and the app's entry points. Included by the unity
 * build after gfx.c (drawing helpers in scope) and by the host simulator. */
#pragma once
#include <stdint.h>
#include "plat.h"
#include "../dsp/drone.h"
#include "../dsp/fastmath.h"            /* the UI's few float sums (the pitch in Hz) */

#ifndef OM_VERSION                       /* a release build passes its own (tools/build.py --release) */
#define OM_VERSION "DEV"
#endif
#define PROJ_MAGIC 0x314E5244u           /* "DRN1" */
#define PROJ_FORMAT 31u                  /* 31: Level an audio taper (30's levels to 10 sqrt(v));
                                          * 30: Tuning in each Interval (the interval and Tuning to one index);
                                          * 29: the gesture keys fixed (Weight, Shift, Clean, Wash; 28's latches
                                          * let go, a key may play something else now); 28: Shift's move in cents (27's four to theirs); Noise's top
                                          * 6 dB up (27's values halved);
                                          * 27: the set macros gone (their four values zeroed; now Wander, Age,
                                          * Shift's move); what each gesture key plays; 26: Gesture time (the byte of the macro PRESETS played: 25's reset);
                                          * 25: the gestures' swells shorter (24's reset to the new defaults);
                                          * 24: the gestures Weight, Shift, Converge, Wash (23's gesture values
                                          * reset: Beat and Heat are gone); 23: the effects (delay type, reverb length and damp, pressure; 19-22
                                          * are read and converted); 22: the macros (16 values more a sound; 19-21 are read and converted);
                                          * 21: scenes in banks: A (12) here, B and C in their own objects (v0.2;
                                          * 19 and 20 are read and converted: scenes 13-16 become B1-B4);
                                          * 20: the noise Colour a blend 0..100; 19: the Swell's Depth in dB (18: noise,
                                          * 17: cutoff / resonance 0..1000, 16: Feedback, Character, 15: Movement, 14: tuning,
                                          * speaker, pitch lock, subharmonics, 13: gliding knobs, Scene time, 12: Morph, 11:
                                          * the LFO rate mod, 10: the LFO lock, 9: the filter LFO's destination, 8: the
                                          * effects, 7: the pulse, 6: scenes) */
#define NSCENES 12                       /* a bank: the 12 lower white keys (F3 .. C5, which sense pressure) */
#define NBANKS 4                         /* A, B, C (yours), and the Presets (fixed, in the firmware) */
#define BANK_PRESETS 3
#define PRESET_LOCKED 11                 /* P12, the clean sound */

typedef struct {
    uint32_t magic, format;
    int16_t par[DR_NPARAMS];             /* drone.h parameters */
    uint8_t leds;                        /* key lights on */
    uint8_t bank;                        /* the bank the scene keys play (0 A .. 3 Presets) */
    uint8_t mtime;                       /* GESTURE TIME, 0..100 (PRESETS): every gesture's speed x0.1 .. x10, 50 as set
                                          * (the player's, not a scene's; was the macro PRESETS played, format <= 25) */
    uint8_t gkey_old[4];                 /* unused since 29 (what each gesture key played, 27-28) */
    uint8_t rsv[9];                      /* room to grow: an older, shorter project loads (zeros here) */
    uint16_t scene_used;                 /* bank A: bit n, scene n holds a sound */
    int16_t scene[NSCENES][DR_NPARAMS];  /* bank A: every parameter, recalled but for the room's settings */
} project_t;
_Static_assert(sizeof(project_t) <= PLAT_STORE_MAX, "the project must fit one flash object");

typedef struct {                         /* banks B and C, and the Presets saved over (before 2026-10-19): an object each */
    uint32_t magic, format;
    uint16_t used;
    uint16_t rsv;
    int16_t scene[NSCENES][DR_NPARAMS];
} bank_t;
_Static_assert(sizeof(bank_t) <= PLAT_STORE_MAX, "a bank must fit one flash object");
#define MTIME_DEF 50                     /* Gesture time to start: the gestures' own swells */
#define BANK_MAGIC 0x314B4E42u           /* "BNK1" */

extern project_t proj;

void project_defaults(void);
int project_load(void);                  /* 0 = loaded; else defaults are in place */
int project_save(void);                  /* 0 = saved and verified */
const int16_t *scene_get(int bank, int n);   /* scene n of a bank, or 0 if empty */
const char *preset_name(int n);          /* the Preset's name (P1 "NIAGARA" ..) */
int scene_put(int bank, int n);         /* the sound now into scene n of bank A..C (marks it to save); 0 ok */
void project_apply(void);                /* every value to the engine (boot, load) */

void ui_init(void);
void ui_frame(void);
void ui_input_only(void);
int ui_dirty(void);
void ui_say(const char *a, const char *b);
