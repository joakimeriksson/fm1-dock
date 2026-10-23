/* SPDX-License-Identifier: GPL-3.0-only */
/* A Scintilla patch in integers. It mirrors synthex-web's patch.ts field for field; every continuous
 * value is the TypeScript value in Q16 (x 65536): seconds stay seconds, Hz stay Hz, cents stay
 * cents, levels stay 0..1. Enums are small integers in the TypeScript order. That one rule is what
 * lets tools/gen_presets.mjs convert the factory bank mechanically. */
#ifndef SC_PATCH_H
#define SC_PATCH_H
#include <stdint.h>

#define SC_Q16(x) ((int32_t)((x) * 65536))

enum { SC_WAVE_TRI, SC_WAVE_SAW, SC_WAVE_SQR, SC_WAVE_SIN, SC_WAVE_NOISE };
enum { SC_FLT_LP24, SC_FLT_LP12, SC_FLT_BP12, SC_FLT_BP6, SC_FLT_HP12 };
enum { SC_LFO_TRI, SC_LFO_SQR, SC_LFO_SH, SC_LFO_RND, SC_LFO_RAMP, SC_LFO_SQRU };
enum { SC_GLIDE_OFF, SC_GLIDE_GLIDE, SC_GLIDE_PORTA };
enum { SC_KEY_POLY, SC_KEY_MONO, SC_KEY_UNISON };
enum { SC_VM_SINGLE, SC_VM_SPLIT, SC_VM_DOUBLE };
enum { SC_DLY_STANDARD, SC_DLY_TAPE, SC_DLY_PINGPONG };
enum { SC_REV_PLATE, SC_REV_ROOM, SC_REV_HALL, SC_REV_GALACTIC };

/* modMatrix, in patch.ts's ModSlot order */
enum {
    SC_MM_L1_P1, SC_MM_L1_P2, SC_MM_L1_CUT, SC_MM_L1_PW1, SC_MM_L1_PW2, SC_MM_L1_AMP,
    SC_MM_L2_P1, SC_MM_L2_P2, SC_MM_L2_CUT, SC_MM_L2_PW1, SC_MM_L2_PW2, SC_MM_L2_AMP,
    SC_MM_E1_CUT, SC_MM_E1_P2,
    SC_MM_JX_L1, SC_MM_JX_L2,
    SC_MM_JY_CUT, SC_MM_JY_LFOFILT,
    SC_MM_VEL_AMP, SC_MM_VEL_CUT, SC_MM_VEL_E1,
    SC_MM_N
};

typedef struct { int32_t a, d, s, r; } sc_adsr_t;

typedef struct {
    int8_t shape, sync;
    int32_t rate, delay, depth_a, depth_b;
} sc_lfo_p_t;

typedef struct {                        /* LayerPatch */
    int8_t  o1_wave, o1_oct;
    int32_t o1_pwm, o1_glide_amt, o1_glide_speed;
    int8_t  o2_wave, o2_oct, o2_sync;
    int32_t o2_detune, o2_pwm, o2_glide_amt, o2_glide_speed;
    int32_t mix_o1, mix_o2, mix_noise;
    int8_t  noise_pink, ring;
    int32_t cross, cross2;              /* OSC1 -> OSC2 PWM, OSC2 -> OSC1 PWM */
    int8_t  flt_mode;
    int32_t cutoff, reso, env_amt, key_track;
    sc_adsr_t env_f, env_a;
    sc_lfo_p_t lfo[2];
    int32_t mm[SC_MM_N];
    int32_t glide_time, glide_amt;
    int8_t  glide_mode, glide_o1, glide_o2;
    int8_t  key_assign, multi_trigger;
    int32_t pan;
} sc_layer_t;

typedef struct { int8_t on, mode; int32_t mix, rate, depth; } sc_chorus_p_t;
typedef struct { int8_t on, mode; int32_t time, feedback, tone, spread, mix; } sc_delay_p_t;
typedef struct { int8_t on, algo; int32_t size, damping, mix; } sc_reverb_p_t;
typedef struct { sc_chorus_p_t chorus; sc_delay_p_t delay; sc_reverb_p_t reverb; } sc_fx_p_t;

typedef struct {                        /* Patch */
    char name[28];
    sc_layer_t layer[2];                /* upper, lower */
    int8_t  voice_mode, split;
    sc_fx_p_t fx;                       /* both layers use it (fxLower is not in the factory bank) */
    int32_t tune, fine_tune, volume, unison_detune;
    int8_t  polyphony, limiter;
} sc_patch_t;

#endif
