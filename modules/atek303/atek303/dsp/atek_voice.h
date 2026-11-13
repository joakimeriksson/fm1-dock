/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Javier (Animatek)
 *
 * ATEK303 voice for the M-VAVE FM-1: the TB-303 voice of the ATEK303 VCV Rack module
 * (../../../VCV-Rack/ATEK303), in single-precision C with no libm.
 *
 * What it is, piece by piece, and where each piece comes from:
 *   - the Open303 core by Robin Schmidt (MIT, thirdparty/open303 of ATEK303): the envelopes, the
 *     cutoff modulation from the measured mapping, the slide, the 4x oversampling with its
 *     elliptic anti-aliasing filter and the fixed post-filters (allpass, highpass, notch);
 *   - the ATEK oscillator (src/AtekOsc.hpp): polyBLEP ramp and comparator square with the
 *     rounded reset, the slewed edges and the AC-coupling droop;
 *   - the ATEK filter (src/AtekFilter.hpp): the diode ladder with a soft diode in every cell,
 *     the real capacitor ratio and the drive;
 *   - the ATEK module (src/Atek303.cpp) in its "circuit" sound model: decay range of the
 *     schematic, log env mod pot, linear resonance pot, accent accumulation, OTA headroom,
 *     output saturation, slide in the pitch domain, analog drift.
 *
 * Departures from the C++ original, on purpose:
 *   - float instead of double (the FM-1's FPU is single precision); the 12th-order elliptic
 *     filter runs as six biquads (computed from Open303's direct form II coefficients with
 *     scipy's tf2sos, same response within 2e-7), which float can run, where the direct form
 *     can't;
 *   - the drift's random walk uses a xorshift generator and a sum of uniforms instead of
 *     Rack's random::normal();
 *   - no Open303 sequencer and no note list: the voice follows gate, accent and slide as the
 *     VCV module does with its inputs.
 *
 * Threads: at_render() runs in the audio interrupt; the rest is called between blocks (the app
 * posts to it from the main loop through its own queue). */
#pragma once
#include <stdint.h>

/* the seven controls of the 303, 0..1 each. WAVE picks from the oscillator list (AT_OSC_NAME): 0 is SAW, 1 is SQUARE
 * and the combinations of the extra layer lie between, at k / (AT_NOSC - 1) */
enum { AT_TUNING, AT_CUTOFF, AT_RESONANCE, AT_ENVMOD, AT_DECAY, AT_ACCENT, AT_WAVE, AT_NPARAMS };

typedef struct {
    float b0, b1, b2, a1, a2, x1, x2, y1, y2;
} at_biquad_t;

typedef struct {
    float b0, b1, a1, x1, y1;
} at_onepole_t;

typedef struct {
    /* settings */
    float sr;                         /* the voice's sample rate (44100 on the FM-1) */
    float param[AT_NPARAMS];
    int drift;                        /* analog drift: 0 none, 1 subtle (the circuit model's), 2 marked, 3 heavy */
    float sat, ota;                   /* output and OTA saturation thresholds (0: none) */
    float sq_droop_hz, saw_reset_us, saw_droop_hz;
    int auto_legato;                  /* the module's option: a new note while the gate is high slides to it */
    uint32_t unit_seed;               /* which 303: the DAC's per-note error */

    /* gate logic (as the VCV module's inputs) */
    int gate, slide, note_held, held_note;

    /* Open303 state */
    float tuning, cutoff, env_mod, accent, normal_decay, accent_decay;
    float env_scaler, env_offset, accent_gain, osc_freq, pitch_wheel, amp_scaler;
    int note_list;                    /* Open303's note list, as the module uses it: empty or not */
    int idle;
    float slew_coeff, slew_y;         /* the pitch slew limiter (on log Hz) */
    float rc2_coeff, rc2_y;           /* the accent's RC */
    float main_c, main_y, main_init;  /* the filter envelope (pure decay) */
    /* the amp envelope (AnalogEnvelope: attack 0, decay 1230 ms, sustain 0, release by accent) */
    float amp_y, amp_time, amp_inc, amp_decay_c, amp_release_c, amp_decay_ms, amp_release_ms;
    int amp_on;
    at_biquad_t declick, notch, aa[6];
    at_onepole_t hp1, hp2, allpass;

    /* ATEK oscillator */
    float ph, inc, blend, pw, a_lp_saw, a_lp_sq, a_hp_saw, a_hp_sq;
    float lp_saw, lp_sq, hp_saw_y, hp_saw_x, hp_sq_y, hp_sq_x;
    /* the extra layer (the VCV module's "Extra layer" menu): off unless layer_on */
    int layer_on, l_pink;
    float l_sin, l_sin_ratio, l_other, l_other_norm, l_other_ratio, l_noise, l_ring, l_xmod;
    float ph2, ph_sin, pink0, pink1, pink2;
    uint32_t l_rng;
    uint8_t l_idx[8];                 /* the layer's eight indices into the module's tables */
    int osc_idx;                      /* the entry of the oscillator list that WAVE picks, -1 before the first */

    /* ATEK filter */
    float f_cut, f_res, f_drive, f_b0, f_k, f_gcomp, y1, y2, y3, y4, fb_hp, fb_prev, fb_hp_c;

    /* the module's extras */
    float acc_fast, acc_slow, drift_pitch, drift_cut;
    float acc_fast_c, acc_slow_c;
    uint32_t rng;
    uint32_t div;                     /* the module updates its parameters every 16 samples */
} at_voice_t;

void at_init(at_voice_t *v, float sr);

/* the oscillator list that WAVE walks: the 303's two waves at the ends and the extra layer's combinations between */
#define AT_NOSC 14
extern const char *const AT_OSC_NAME[AT_NOSC];
int at_osc_index(float wave);                 /* WAVE's value to its entry */

/* the VCV module's "Fine tuning" menu, an option at a time: the index into its choices, as the module has them
 * (the defaults in brackets: what the circuit model sets) */
enum {
    AT_OPT_PW,          /* pulse width (TM5): 44 47 50 53 56 %  [0] */
    AT_OPT_SQ_DROOP,    /* square droop: none 8 15 30 60 Hz  [4] */
    AT_OPT_SAW_RESET,   /* saw reset corner: 1 3 8 20 us  [3] */
    AT_OPT_SAW_DROOP,   /* saw droop: 0.7 3 8 15 Hz  [3] */
    AT_OPT_DRIVE,       /* filter drive: 0 +6 +12 dB  [1] */
    AT_OPT_SLIDE,       /* slide tau: 60 120 220 ms  [1] */
    AT_OPT_LEGATO,      /* auto-legato: off on  [1] */
    AT_OPT_OTA,         /* OTA saturation: none soft hard  [1] */
    AT_OPT_SAT,         /* output saturation: none soft hard  [1] */
    AT_OPT_DRIFT,       /* analogue drift: none subtle marked heavy  [1] */
    AT_OPT_UNIT,        /* unit (the DAC's error pattern): 1..4  [0] */
    AT_NOPTS
};
extern const uint8_t AT_OPT_N[AT_NOPTS];      /* how many choices each */
extern const uint8_t AT_OPT_DEFAULT[AT_NOPTS];
void at_option(at_voice_t *v, int opt, int idx);   /* between blocks */
void at_set(at_voice_t *v, int param, float value);   /* 0..1 */

/* the gate, the note (MIDI), accent and slide, as the module's GATE, V/OCT, ACC and SLIDE inputs:
 * a gate going up plays the note (sliding if the last gate went down with slide held); a gate going
 * down releases it unless slide is held. */
void at_gate(at_voice_t *v, int gate, int note, int accent, int slide);
void at_panic(at_voice_t *v);

/* n mono samples, about -1..1 (the module's volts / 5) */
void at_render(at_voice_t *v, float *out, uint32_t n);
