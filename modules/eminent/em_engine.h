/* SPDX-License-Identifier: GPL-3.0-only */
/* The Eminent 310's string section, in integers: the bed under Jarre's Oxygène and Equinoxe, a divide-down
 * string ensemble through the Electro-Harmonix Small Stone. Paraphonic (every key, 16 at once here), each key
 * sawtooth footages (16', 8', 4' by the registration) with the ensemble's slow swell and long release; the sum
 * through the string formant (a high-pass, a peak near 2 kHz, a roll-off), the three-line ensemble chorus
 * (0.6 and 6 Hz, the Solina / Eminent circuit, no dry) and the four-stage phaser, swept slowly, with its
 * colour (feedback). Q15 stereo into an interleaved block, as every module. */
#ifndef EM_ENGINE_H
#define EM_ENGINE_H
#include <stdint.h>

#define EM_SR 44100
#define EM_VOICES 16
#define EM_LINE 512                             /* an ensemble line: 11.6 ms */
enum { EM_STRINGS, EM_CELLO, EM_VIOLIN, EM_FULL, EM_N_REG };   /* 8'+4', 16'+8', 4' (8' under), 16'+8'+4' */

typedef struct {
    uint8_t reg;                                /* EM_* */
    uint8_t attack, release;                    /* 0..100: 20 ms .. 1.5 s; 50 ms .. 3 s */
    uint8_t ensemble;                           /* 0 off, else the chorus depth */
    uint8_t phaser;                             /* 0 off, else the sweep's rate: 0.05 .. 1.5 Hz */
    uint8_t color;                              /* the phaser's feedback, 0 / 1 */
    uint8_t reverb;                             /* the host's reverb mix, percent */
    uint8_t volume;                             /* percent */
} em_patch_t;

typedef struct {
    uint32_t ph16, ph8, ph4, inc;               /* the footages' phases; the 8' step a sample */
    int32_t env;                                /* Q30 */
    uint32_t age;
    uint8_t note, gate;
} em_voice_t;

typedef struct {
    em_patch_t p;
    em_voice_t v[EM_VOICES];
    uint32_t ages;
    int32_t hp_x, hp_y, lp_y, svf_lo, svf_bp;   /* the formant */
    int32_t line[3][EM_LINE];                   /* the ensemble */
    uint32_t w, lfo_slow, lfo_fast;
    int32_t ap_x[2][4], ap_y[2][4], fb[2];      /* the phaser, a channel each */
    uint32_t lfo_ph;
} em_t;

void em_init(em_t *e);
void em_set_patch(em_t *e, const em_patch_t *p);
void em_note_on(em_t *e, uint32_t note, uint32_t vel);
void em_note_off(em_t *e, uint32_t note);
void em_all_off(em_t *e);
uint32_t em_held(const em_t *e);                /* keys down */
void em_render(em_t *e, int32_t *out, uint32_t n);

#endif
