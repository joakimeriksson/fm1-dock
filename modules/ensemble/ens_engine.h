/* SPDX-License-Identifier: GPL-3.0-only */
/* ENSEMBLE: a string / choir / dark-pad engine with a built-in stereo multi-tap ensemble, ported from Caravel 0.11.0
 * (bonszaimusic, GPL-3.0-only: firmware/src/eng_ensemble.c), with the DSP blocks it uses from Felucca-Engines
 * (Leo Kuroshita / Hugelton Instruments, MIT: src/dsp.c) and the parts of Caravel's voice.c it relies on (the ADSR,
 * the filter envelope, the velocity). The voice and the ensemble are Caravel's code line for line; what is new is
 * the frame around them: six voices of its own, its own envelope, the stereo out. Not from fm1-harbor: take.py does
 * not copy this one, it lives here.
 *
 * A voice: the registers 16' / 8' / 4' (a triangle an octave down, a band-limited saw, a saw an octave up), a second
 * detuned pulse for the dark end, a breath noise for the choir, then one trapezoidal state-variable filter whose
 * cutoff, damping and low-pass / band-pass mix follow MORPH: STRINGS (0) .. CHOIR (64) .. DARK (127). The ensemble,
 * after the voices: three delay taps a channel (6.0 / 8.5 / 11.0 ms left, 6.8 / 10.1 / 12.7 ms right) read from one
 * ring, each moved by a slow (0.2 .. 1.6 Hz) and a fast (6 .. 8.6 Hz) LFO; the width is the preset's. */
#ifndef ENS_ENGINE_H
#define ENS_ENGINE_H
#include <stdint.h>
#include "ens_tables.h"

#define ENS_VOICES 6u                           /* Caravel's cap for this engine */
#define ENS_N_PRESETS 14u
#define ENS_NT 6u                               /* the taps: 0..2 left, 3..5 right */
#define ENS_RING 1024u                          /* 23.2 ms */
enum { ENS_REG, ENS_MORPH, ENS_TONE, ENS_DRIFT, ENS_DEPTH, ENS_RATE, ENS_TEXT, ENS_MIX };
enum { ENS_ATK, ENS_DEC, ENS_SUS, ENS_REL };

typedef struct {
    const char *name;
    uint8_t e[8];                               /* REG MORPH TONE DRIFT ENS RATE TEXT MIX, 0..127 */
    uint8_t env[4];                             /* ATK DEC SUS REL, 0..127 */
    int8_t fenv;                                /* ENV -> FILTER (-64..63) */
    uint8_t width;                              /* the stereo width (Caravel's SPRD), 0 mono .. 127 */
} ens_preset_t;
typedef struct {
    uint32_t ph[3], age;
    int32_t s[8];                               /* the filter (0, 1), the noise (2), the drift (3..5), the calibration (7) */
    int32_t env, env_out;                       /* the ADSR (Q24), the last control tick's amplitude (Q15) */
    uint8_t stage, key, vel, gate;              /* stage 0 off, 1 attack, 2 decay / sustain, 3 release */
} ens_voice_t;
typedef struct {
    uint8_t e[8], env[4], width;
    int8_t fenv;
    ens_voice_t v[ENS_VOICES];
    uint32_t age, tail;
    int32_t ring[ENS_RING];
    uint32_t w, slow[ENS_NT], fast[2], live;
    int32_t delay[ENS_NT];
    int32_t mono[ENS_CTL], side[ENS_CTL];
} ens_t;

extern const ens_preset_t ENS_PRESETS[ENS_N_PRESETS];
void ens_init(ens_t *e);                        /* silent, VINTAGE STRINGS */
void ens_load(ens_t *e, uint32_t preset);       /* a factory preset's controls, envelope and width; the notes go on */
void ens_note_on(ens_t *e, uint8_t key, uint8_t vel);
void ens_note_off(ens_t *e, uint8_t key);
void ens_all_off(ens_t *e);                     /* silence at once, the ensemble's ring cleared */
void ens_render(ens_t *e, int32_t *stereo, uint32_t frames);   /* adds interleaved stereo, about Q15 */
#endif
