/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_ELECTRONIC_FX_H
#define MD_ELECTRONIC_FX_H
#include "core/instrument.h"
#include "builtin_ids.h"
#define MD_ELECTRONIC_FX_SETTINGS_VERSION 2u
#define MD_ELECTRONIC_FX_SETTINGS_BYTES 19u /* v2: v1's 16, the wind's two, the spread; 16 and 18 still load */
#define MD_ELECTRONIC_FX_VOICES 8u
/* The recipes. The first six are struck: they start at full and die away. From WHISTLE on they are shaped: they swell
 * for SWEEP TIME and fall away to the end of DECAY (the whole length), and SWEEP is what moves meanwhile:
 *   ZAP      a bright tone falling SWEEP semitones onto the key: a shot
 *   WHISTLE  a tone gliding down SWEEP semitones to the key (up if negative), MOD DEPTH a vibrato that grows
 *   GULL     a cry: up SWEEP semitones with the swell, then falling past the key; MOD DEPTH its roughness
 *   TWITTER  quick rising chirps on random steps (fifths and octaves within SWEEP), some left out: an electronic
 *            bird; MOD DEPTH the chirps' rate (4 .. 29 a second)
 *   BLEEPS   a hollow tone stepping at random through fifths and octaves within SWEEP: sample and hold; MOD DEPTH
 *            the steps' rate
 *   WHOOSH   noise through a resonant band that travels SWEEP semitones up from the key over the whole length
 *   SURF     noise through a low-pass that opens SWEEP semitones above the key with the swell and closes again: a wave
 *   THUNDER  a crack of noise at the key that falls SWEEP semitones into a rumble, MOD DEPTH how much it rolls */
enum { MD_FX_DROP,MD_FX_PING,MD_FX_BUBBLE,MD_FX_CHIRP,MD_FX_METAL,MD_FX_ZAP,MD_FX_WHISTLE,MD_FX_GULL,MD_FX_TWITTER,
       MD_FX_BLEEPS,MD_FX_WHOOSH,MD_FX_SURF,MD_FX_THUNDER,MD_FX_RECIPES };
enum {
    MD_FX_RECIPE=1,MD_FX_BASE_PITCH,MD_FX_DECAY_MS,MD_FX_SWEEP_SEMITONES,
    MD_FX_SWEEP_MS,MD_FX_TONE,MD_FX_RESONANCE,MD_FX_MOD_DEPTH,MD_FX_LEVEL,MD_FX_RANDOM,
    MD_FX_WIND,MD_FX_WIND_COLOUR,
    MD_FX_KIT,  /* 0 off; 1..13: each NOTE_ON plays that recipe (DROP..THUNDER) as a one-shot with the recipe's own
                 * factory macros at the note's key, whatever the base sound: what a sequence locks on a step. Not a
                 * setting */
    MD_FX_SPREAD /* the stereo width, percent: 0 everything in the middle (mono, as before); above it each new sound
                 * takes a place of its own left to right, and ZAP, WHISTLE, GULL, WHOOSH, SURF and THUNDER travel across */
};
typedef struct {
    uint16_t decay_ms,sweep_ms;
    uint8_t recipe,pitch,tone,resonance,mod_depth,level,random;
    uint8_t wind,wind_colour;   /* the wind layer: its level (0 off) and where it sits, low to high */
    uint8_t spread;
    int8_t sweep;
    uint32_t seed;
} md_electronic_fx_settings_t;
typedef struct {
    md_note_token_t token;uint64_t order;
    uint32_t phase,mod_phase,amp,pitch_env,decay_k,pitch_k,rng,age;
    int32_t base_q16,sweep_q16,filter_low,filter_band,tone_low,ping_damp;
    /* the shaped recipes: the fall's line (Q30 down to 0), the swell's and the fall's steps a sample */
    uint32_t lin,up_k,down_k;
    uint32_t step_len,step_on,step_count;      /* TWITTER, BLEEPS: a step's samples, its sounding part, what is left */
    int32_t step_q16,glide,glide_k,gate;       /* the step's pitch, the chirp's rise, the gate (smoothed) */
    int32_t svf_g,makeup,roll,roll_target;     /* the noise recipes' filter, THUNDER's level and its rolling */
    int32_t pan,pan_move,gain_l,gain_r;        /* the place (0 left .. 32768 right), its travel, the two gains (Q15) */
    uint16_t gain_q15,peak_q15;                /* (peak: where the swell ends, of the length) */
    uint8_t recipe,tone,resonance,mod_depth,active,held,releasing,shaped,rising,rest;
} md_electronic_fx_voice_t;
typedef struct {
    md_electronic_fx_settings_t settings;
    uint32_t rng;uint64_t order;
    md_electronic_fx_voice_t voices[MD_ELECTRONIC_FX_VOICES];
    /* the wind: noise through a band-pass swept by its own slow LFO, swelling to its level (not a voice) */
    uint32_t wind_rng,wind_lfo;
    int32_t wind_low,wind_band,wind_gain,wind_g;
    uint32_t wind_count;
    uint8_t kit;   /* MD_FX_KIT: performance state, not in the sound's bytes */
} md_electronic_fx_state_t;
/* The wind is a continuous layer beside the voices: WIND sets its level (it swells there with a time constant of about
 * 0.74 s, and from full fades to silence in about four seconds at 0), WIND COLOUR its centre; it takes no voice, is not a note, and follows parameter
 * changes and locks at once. Settings are snapshotted per trigger: locks shape the new event without
 * rewriting already-ringing tails. HIT recipe overrides only recipe selection;
 * all other current sound macros/base pitch apply. NOTE_ON uses the selected
 * recipe and key. Hits can steal transient/released tails, never held pitches. With KIT set a NOTE_ON is a
 * one-shot of that recipe with its factory decay, sweep, tone, resonance and modulation (level and random stay the
 * sound's): it rings out whatever the note's length, takes no token and can be stolen as a hit can.
 * A shaped recipe's note swells and falls by itself too; its note-off cuts it short (30 ms). */
extern const md_instrument_desc_t MD_ELECTRONIC_FX;
/* A recipe's factory macros, as its kit sound plays: decay ms, sweep semitones, sweep ms, tone, resonance, mod depth. */
void md_electronic_fx_macros(uint32_t recipe,int16_t macros[6]);
#endif
