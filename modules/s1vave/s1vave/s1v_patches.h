/* SPDX-License-Identifier: GPL-3.0-only */
/* S1VAVE in fm1-dock: its factory bank, fm1-dock's own (in place of S1Vave's obv_patches.h, which holds Roland's
 * S-1 factory presets: modules/s1vave/take.py). Sixteen sounds on S1Vave's parameters; programs 17..99 are INIT,
 * for the user's. Each is (parameter, value) pairs ending in X; a parameter not named keeps PDEF's default. */
#ifndef OBV_PATCHES_H
#define OBV_PATCHES_H

#define X 255u

static const uint8_t PF_01[] = {P_POLY_MODE, POLY_POLY, P_PULSE_LVL, 0, P_SAW_LVL, 127, P_SUB_LVL, 30, P_CUTOFF, 78,
    P_RESO, 18, P_FENV_AMT, 40, P_ATTACK, 0, P_DECAY, 60, P_SUSTAIN, 90, P_RELEASE, 40, P_AMP_MODE, AMP_ENV,
    P_CHO_MODE, CHO_TYPE2, P_REV_LVL, 30, P_REV_TYPE, REV_HALL1, X};                         /* POLY SAW */
static const uint8_t PF_02[] = {P_POLY_MODE, POLY_MONO, P_RANGE, RANGE_16, P_PULSE_LVL, 0, P_SAW_LVL, 127,
    P_SUB_LVL, 90, P_SUB_TYPE, SUB_2OA, P_CUTOFF, 34, P_RESO, 50, P_FENV_AMT, 90, P_ATTACK, 0, P_DECAY, 45,
    P_SUSTAIN, 20, P_RELEASE, 10, P_AMP_MODE, AMP_ENV, P_PORTA_MODE, PORTA_AUTO, P_PORTA_TIME, 20, X};   /* MONO BASS */
static const uint8_t PF_03[] = {P_POLY_MODE, POLY_MONO, P_PULSE_LVL, 127, P_SAW_LVL, 0, P_SUB_LVL, 0, P_PWM_SRC,
    PWM_MANUAL, P_PWM_DEPTH, 64, P_CUTOFF, 28, P_RESO, 100, P_FENV_AMT, 110, P_ATTACK, 0, P_DECAY, 40,
    P_SUSTAIN, 0, P_RELEASE, 12, P_AMP_MODE, AMP_ENV, P_PORTA_MODE, PORTA_AUTO, P_PORTA_TIME, 30, P_DLY_LVL, 30, X};   /* ACID LINE */
static const uint8_t PF_04[] = {P_POLY_MODE, POLY_POLY, P_PULSE_LVL, 127, P_SAW_LVL, 0, P_SUB_LVL, 20, P_PWM_SRC,
    PWM_LFO, P_PWM_DEPTH, 80, P_LFO_RATE, 40, P_LFO_WAVE, LFO_TRI, P_CUTOFF, 70, P_RESO, 10, P_FENV_AMT, 20,
    P_ATTACK, 70, P_DECAY, 80, P_SUSTAIN, 100, P_RELEASE, 80, P_AMP_MODE, AMP_ENV, P_CHO_MODE, CHO_TYPE3,
    P_REV_LVL, 60, P_REV_TYPE, REV_HALL2, X};                                                 /* PWM PAD */
static const uint8_t PF_05[] = {P_POLY_MODE, POLY_POLY, P_PULSE_LVL, 0, P_SAW_LVL, 127, P_SUB_LVL, 40, P_CUTOFF, 45,
    P_RESO, 8, P_FENV_AMT, 80, P_ATTACK, 20, P_DECAY, 60, P_SUSTAIN, 90, P_RELEASE, 30, P_AMP_MODE, AMP_ENV,
    P_REV_LVL, 25, P_REV_TYPE, REV_ROOM, X};                                                  /* BRASS */
static const uint8_t PF_06[] = {P_POLY_MODE, POLY_UNISON, P_PULSE_LVL, 60, P_SAW_LVL, 127, P_SUB_LVL, 0, P_DETUNE, 20,
    P_SPREAD, 60, P_CUTOFF, 90, P_RESO, 30, P_FENV_AMT, 30, P_ATTACK, 0, P_DECAY, 70, P_SUSTAIN, 100,
    P_RELEASE, 30, P_AMP_MODE, AMP_ENV, P_PORTA_MODE, PORTA_ON, P_PORTA_SW, 1, P_PORTA_TIME, 20, P_DLY_LVL, 40, X};   /* UNISON LEAD */
static const uint8_t PF_07[] = {P_POLY_MODE, POLY_CHORD, P_CHORD_SW2, 1, P_CHORD_SW3, 1, P_CHORD_SW4, 0,
    P_CHORD_KS2, 19, P_CHORD_KS3, 24, P_PULSE_LVL, 0, P_SAW_LVL, 127, P_SUB_LVL, 0, P_CUTOFF, 60, P_RESO, 20,
    P_FENV_AMT, 90, P_ATTACK, 0, P_DECAY, 40, P_SUSTAIN, 0, P_RELEASE, 30, P_AMP_MODE, AMP_ENV, P_DLY_LVL, 50,
    P_REV_LVL, 30, X};                                                                       /* CHORD STAB */
static const uint8_t PF_08[] = {P_POLY_MODE, POLY_POLY, P_RANGE, RANGE_4, P_PULSE_LVL, 110, P_SAW_LVL, 0, P_SUB_LVL, 0,
    P_CHOP_OVTN, 160, P_CHOP_COMB, 6, P_CUTOFF, 100, P_FENV_AMT, 20, P_ATTACK, 0, P_DECAY, 90, P_SUSTAIN, 0,
    P_RELEASE, 80, P_AMP_MODE, AMP_ENV, P_REV_LVL, 50, P_REV_TYPE, REV_PLATE, X};             /* CHOP BELL */
static const uint8_t PF_09[] = {P_POLY_MODE, POLY_POLY, P_DRAW_SW, DSW_SLOPE, P_DRAW_MULT, 2, P_PULSE_LVL, 90,
    P_SAW_LVL, 60, P_SUB_LVL, 0, P_CUTOFF, 100, P_RESO, 10, P_ATTACK, 10, P_DECAY, 60, P_SUSTAIN, 100,
    P_RELEASE, 50, P_AMP_MODE, AMP_ENV, P_CHO_MODE, CHO_TYPE1, X};                            /* DRAW WAVE */
static const uint8_t PF_10[] = {P_POLY_MODE, POLY_MONO, P_RANGE, RANGE_16, P_PULSE_LVL, 0, P_SAW_LVL, 30,
    P_SUB_LVL, 127, P_SUB_TYPE, SUB_1OC, P_CUTOFF, 25, P_RESO, 0, P_FENV_AMT, 30, P_ATTACK, 0, P_DECAY, 70,
    P_SUSTAIN, 80, P_RELEASE, 10, P_AMP_MODE, AMP_ENV, X};                                    /* SUB BASS */
static const uint8_t PF_11[] = {P_POLY_MODE, POLY_POLY, P_PULSE_LVL, 0, P_SAW_LVL, 0, P_SUB_LVL, 0, P_NOISE_LVL, 127,
    P_CUTOFF, 20, P_RESO, 110, P_FLFO_AMT, 90, P_LFO_RATE, 20, P_LFO_WAVE, LFO_TRI, P_KEYTRK, 127,
    P_ATTACK, 60, P_SUSTAIN, 127, P_RELEASE, 90, P_AMP_MODE, AMP_ENV, P_REV_LVL, 70, P_REV_TYPE, REV_HALL2, X};   /* NOISE SWEEP */
static const uint8_t PF_12[] = {P_POLY_MODE, POLY_POLY, P_PULSE_LVL, 0, P_SAW_LVL, 127, P_SUB_LVL, 10, P_CUTOFF, 75,
    P_RESO, 5, P_FENV_AMT, 10, P_ATTACK, 80, P_DECAY, 60, P_SUSTAIN, 110, P_RELEASE, 90, P_AMP_MODE, AMP_ENV,
    P_CHO_MODE, CHO_TYPE4, P_SPREAD, 80, P_REV_LVL, 50, P_REV_TYPE, REV_HALL1, X};             /* STRINGS */
static const uint8_t PF_13[] = {P_POLY_MODE, POLY_POLY, P_PULSE_LVL, 90, P_SAW_LVL, 0, P_SUB_LVL, 20, P_PWM_SRC,
    PWM_MANUAL, P_PWM_DEPTH, 30, P_CUTOFF, 70, P_RESO, 5, P_FENV_AMT, 50, P_ATTACK, 0, P_DECAY, 70, P_SUSTAIN, 40,
    P_RELEASE, 40, P_AMP_MODE, AMP_ENV, P_CHO_MODE, CHO_TYPE1, X};                            /* SYNTH KEYS */
static const uint8_t PF_14[] = {P_POLY_MODE, POLY_MONO, P_RANGE, RANGE_16, P_PULSE_LVL, 0, P_SAW_LVL, 127,
    P_SUB_LVL, 70, P_CUTOFF, 40, P_RESO, 80, P_FLFO_AMT, 100, P_LFO_RATE, 60, P_LFO_WAVE, LFO_TRI,
    P_ATTACK, 0, P_SUSTAIN, 127, P_RELEASE, 15, P_AMP_MODE, AMP_ENV, X};                      /* WOBBLE */
static const uint8_t PF_15[] = {P_POLY_MODE, POLY_MONO, P_PULSE_LVL, 40, P_SAW_LVL, 127, P_SUB_LVL, 30, P_CUTOFF, 80,
    P_RESO, 25, P_FENV_AMT, 40, P_FX_MODE, 1, P_FX_AMT, 40, P_FX_MIX, 60, P_ATTACK, 0, P_SUSTAIN, 110,
    P_RELEASE, 25, P_AMP_MODE, AMP_ENV, P_PORTA_MODE, PORTA_AUTO, P_PORTA_TIME, 25, P_DLY_LVL, 35, X};   /* DIST LEAD */
static const uint8_t PF_16[] = {P_POLY_MODE, POLY_POLY, P_PULSE_LVL, 127, P_SAW_LVL, 0, P_SUB_LVL, 0, P_PWM_SRC,
    PWM_MANUAL, P_PWM_DEPTH, 90, P_FX_MODE, 0, P_FX_AMT, 50, P_FX_MIX, 70, P_CUTOFF, 85, P_ATTACK, 0,
    P_DECAY, 45, P_SUSTAIN, 0, P_RELEASE, 30, P_AMP_MODE, AMP_ENV, P_DLY_LVL, 45, X};          /* LOFI PLUCK */

static const uint8_t PF_INIT[] = {X};
#undef X

static const factory_t FACT_INIT = {"INIT", PF_INIT};
static const factory_t FACTORY[NFACT] = {
    {"POLY SAW", PF_01}, {"MONO BASS", PF_02}, {"ACID LINE", PF_03}, {"PWM PAD", PF_04},
    {"BRASS", PF_05}, {"UNISON LD", PF_06}, {"CHORD STAB", PF_07}, {"CHOP BELL", PF_08},
    {"DRAW WAVE", PF_09}, {"SUB BASS", PF_10}, {"NOISE SWP", PF_11}, {"STRINGS", PF_12},
    {"SYNTH KEYS", PF_13}, {"WOBBLE", PF_14}, {"DIST LEAD", PF_15}, {"LOFI PLUCK", PF_16},
};

static const uint8_t FACT_TEMPO[NFACT] = {120, 120, 128, 100, 120, 125, 122, 110, 120, 120, 90, 100, 120, 140, 125, 118};

typedef struct {
    int16_t draw[16];
    uint16_t chop[4];
} fact_wave_t;

#define FW_TRI {-25600, -19200, -12800, -6400, 0, 6400, 12800, 19200, 25600, 19200, 12800, 6400, 0, -6400, -12800, -19200}
#define FW_ALL {65535u, 65535u, 65535u, 65535u}
static const fact_wave_t FACT_WAVE[NFACT] = {   /* the draw table and the chop mask: a triangle, every slice */
    {FW_TRI, FW_ALL}, {FW_TRI, FW_ALL}, {FW_TRI, FW_ALL}, {FW_TRI, FW_ALL},
    {FW_TRI, FW_ALL}, {FW_TRI, FW_ALL}, {FW_TRI, FW_ALL}, {FW_TRI, FW_ALL},
    {{-25600, -25600, 25600, 25600, -12800, 12800, 25600, 0, -25600, 0, 19200, -19200, 6400, -6400, 25600, -25600}, FW_ALL},
    {FW_TRI, FW_ALL}, {FW_TRI, FW_ALL}, {FW_TRI, FW_ALL},
    {FW_TRI, FW_ALL}, {FW_TRI, FW_ALL}, {FW_TRI, FW_ALL}, {FW_TRI, FW_ALL},
};
#undef FW_TRI
#undef FW_ALL

#endif
