/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Andrew GG — Roland S-1 parity engine */
/* S1Vave (Roland S-1 Clone) Parameter Table and Page Definitions.
 * Aligned with Roland AIRA Compact S-1 specifications (System v1.02)
 * and mapped 1:1 to MIDI Control Changes. */

#ifndef OBV_PARAMS_H
#define OBV_PARAMS_H

#include <stdint.h>

enum {
    /* 0..3: OSC CORE (EDIT Page 1) */
    P_RANGE,            /* Range: 0=64', 1=32', 2=16', 3=8', 4=4', 5=2' [CC 14] */
    P_PULSE_LVL,        /* Square/Pulse Level: 0..127 [CC 19] */
    P_SAW_LVL,          /* Saw Level: 0..127 [CC 20] */
    P_SUB_LVL,          /* Sub Level: 0..127 [CC 21] */

    /* 4..7: OSC SUB & NOISE (EDIT Page 2) */
    P_SUB_TYPE,         /* Sub Octave Type: 0=-2o.A, 1=-2oc, 2=-1oc [CC 22] */
    P_NOISE_LVL,        /* Noise Level: 0..127 [CC 23] */
    P_NOISE_MODE,       /* Noise Mode: 0=White, 1=Pink [CC 78 when Riser OFF] */
    P_FINE_TUNE,        /* Fine Tune: 0..127 (-64..+63 semitones) [CC 76] */

    /* 8..9: PWM Config */
    P_PWM_SRC,          /* PWM Source: 0=Env, 1=Manual (NAn), 2=LFO [CC 16] */
    P_PWM_DEPTH,        /* PWM Depth / Pulse Width: 0..127 [CC 15] */

    /* 10..11: OSC DRAW (EDIT Page 3) */
    P_DRAW_SW,          /* Draw Switch: 0=OFF, 1=STEP, 2=SLOPE [CC 107] */
    P_DRAW_MULT,        /* Draw Multiply: 0..31 (1.0x to 32.0x) [CC 102] */

    /* 12..13: OSC CHOP (EDIT Page 4) */
    P_CHOP_OVTN,        /* Chop Overtone: 0..200 [CC 103] */
    P_CHOP_COMB,        /* Chop Comb: 0..31 (1.0 to 32.0) [CC 104] */

    /* 14..20: FILTER (EDIT Pages 6 & 7) */
    P_CUTOFF,           /* Filter Cutoff: 0..127 [CC 74] */
    P_RESO,             /* Filter Resonance: 0..127 [CC 71] */
    P_FENV_AMT,         /* Filter Env Depth: 0..127 [CC 24] */
    P_FLFO_AMT,         /* Filter LFO Depth: 0..127 [CC 25] */
    P_KEYTRK,           /* Filter Keyboard Follow: 0..127 (0..100%) [CC 26] */
    P_FLT_BEND,         /* Filter Bend Sensitivity: 0..127 [CC 27] */
    P_FLT_COLOR,        /* Filter Character: 0=S-1 Clean, 1=Warm Midnight [CC 108] */
    P_TONE,             /* Overall Tone Tilt: 0..127 (-64..+63) [CC 109] */
    P_WARMTH,           /* Analog Warmth / Bass Retention: 0..127 [CC 110] */

    /* 20..25: ENVELOPE (ENV Pages 1 & 2) */
    P_ATTACK,           /* Attack Time: 0..127 [CC 73] */
    P_DECAY,            /* Decay Time: 0..127 [CC 75] */
    P_SUSTAIN,          /* Sustain Level: 0..127 [CC 30] */
    P_RELEASE,          /* Release Time: 0..127 [CC 72] */
    P_AMP_MODE,         /* Amp Env Mode: 0=Gate, 1=Env [CC 28] */
    P_ENV_TRG,          /* Env Trigger Mode: 0=Gate, 1=Gate+Trig, 2=LFO [CC 29] */

    /* 26..33: LFO (LFO Page 1) */
    P_LFO_RATE,         /* LFO Rate: 0..127 [CC 3] */
    P_LFO_WAVE,         /* LFO Waveform: 0=Saw, 1=InvSaw, 2=Tri, 3=Sqr, 4=Rnd, 5=Noise [CC 12] */
    P_LFO_SYNC,         /* LFO Sync to Tempo: 0=OFF, 1=ON [CC 106] */
    P_LFO_TRIG,         /* LFO Key Trigger: 0=OFF, 1=ON [CC 105] */
    P_LFO_MODE,         /* LFO Mode: 0=Normal, 1=Fast [CC 79] */
    P_LFO_PITCH,        /* OSC Pitch LFO Depth: 0..127 [CC 13] */
    P_MOD_DEPTH,        /* LFO Mod Depth: 0..127 [CC 1 / CC 17] */
    P_OSC_BEND,         /* OSC Bend Sensitivity: 0..127 [CC 18] */

    /* 34..37: RISER EFFECT (LFO Page 2) */
    P_RISER_MODE,       /* Riser Mode: 0=OFF, 1=Sync, 2=Quiv, 3=QvPn [CC 78] */
    P_RISER_RESO,       /* Riser Resonance: 0..100 */
    P_RISER_SHAPE,      /* Riser Shape: 0..100 (0=saw, 100=square) */
    P_RISER_LVL,        /* Riser Level: 0..100 */

    /* 38..41: DELAY (FX Page 1) */
    P_DLY_LVL,          /* Delay Level: 0..127 [CC 92] */
    P_DLY_TIME,         /* Delay Time: 0..127 [CC 90] */
    P_DLY_FB,           /* Delay Feedback: 0..127 */
    P_DLY_SYNC,         /* Delay Sync: 0=OFF, 1=ON */

    /* 42..46: REVERB & CHORUS (FX Page 2) */
    P_REV_LVL,          /* Reverb Level: 0..127 [CC 91] */
    P_REV_TIME,         /* Reverb Time: 0..127 [CC 89] */
    P_REV_TYPE,         /* Reverb Type: 0=Amb, 1=Room, 2=Hall1, 3=Hall2, 4=Plate, 5=Spring, 6=Mod */
    P_REV_PRE,          /* Reverb Pre-Delay: 0..100 ms */
    P_CHO_MODE,         /* Chorus Mode: 0=OFF, 1=Type1, 2=Type2, 3=Type3, 4=Type4 [CC 93] */

    /* 47..52: VOICE & CONTROLLER (GLO Page 1) */
    P_POLY_MODE,        /* Poly Mode: 0=Mono, 1=Unison, 2=Poly, 3=Chord [CC 80] */
    P_PORTA_MODE,       /* Portamento Mode: 0=OFF, 1=ON, 2=Auto [CC 31] */
    P_PORTA_TIME,       /* Portamento Time: 0..127 [CC 5] */
    P_PORTA_SW,         /* Portamento Switch: 0=OFF, 1=ON [CC 65] */
    P_TRANSPOSE,        /* Key Transpose: 4..124 (-60..+60 semitones, center 64) [CC 77] */
    P_VOLUME,           /* Master Volume: 0..127 [CC 7] */

    /* 53..58: CHORD CONFIG */
    P_CHORD_SW2,        /* Chord Voice 2 SW: 0=OFF, 1=ON [CC 81] */
    P_CHORD_SW3,        /* Chord Voice 3 SW: 0=OFF, 1=ON [CC 82] */
    P_CHORD_SW4,        /* Chord Voice 4 SW: 0=OFF, 1=ON [CC 83] */
    P_CHORD_KS2,        /* Chord Voice 2 Shift: 0..24 (-12..+12) [CC 85] */
    P_CHORD_KS3,        /* Chord Voice 3 Shift: 0..24 (-12..+12) [CC 86] */
    P_CHORD_KS4,        /* Chord Voice 4 Shift: 0..24 (-12..+12) [CC 87] */

    /* 59..64: ADV - stereo, phaser & the lo-fi/distortion slot (SEL button pages) */
    P_SPREAD,           /* Stereo Spread: 0..100 (0 = mono) [CC 41] */
    P_DETUNE,           /* Stereo Detune: 0..50 cents (extra on top of the stock S-1 unison) [CC 42] */
    P_PHASER_MIX,       /* Phaser Mix: 0..100 (0 = off); sweep follows the LFO rate [CC 43] */
    P_FX_MODE,          /* FX Slot Mode: 0=Rate Reduce, 1=Distortion [CC 45] */
    P_FX_AMT,           /* Slot Amount (shared): 0..100 (0 = off) [CC 46] */
    P_FX_MIX,           /* Slot Mix (shared): 0..100 (dry..wet) [CC 47] */

    NPARAM              /* 65 total parameters */
};

#define PATCH_NAME 10u
#define PATCH_BYTES 80u                         /* NPARAM values, the name at NPARAM, the rest spare */
#if NPARAM + PATCH_NAME > PATCH_BYTES
#error "the patch outgrew PATCH_BYTES"
#endif

#define NPROG 99u                               /* 1-64 factory sounds, 65-99 INIT; user saves over them */
#define NFACT 16u                               /* fm1-dock: its own bank (s1v_patches.h) */

/* Display and Value Kinds */
enum {
    K_NUM, K_BIP, K_ONOFF, K_RANGE, K_SUBTYPE, K_NOISEM, K_PWMS,
    K_DRAWSW, K_MULT, K_OVTN, K_COMB, K_ENVTRG, K_AMP, K_LFOWAVE,
    K_RISERM, K_CHOM, K_REVM, K_PMODE, K_PORTAM, K_CUT, K_MS, K_HZ, K_PCT, K_SEMI,
    K_DDIV, K_FLTCOL,
    K_FXMODE
};

/* Named constants for parameters */
enum { RANGE_64, RANGE_32, RANGE_16, RANGE_8, RANGE_4, RANGE_2 };
enum { SUB_2OA, SUB_2OC, SUB_1OC };
enum { PWM_ENV, PWM_MANUAL, PWM_LFO };
enum { DSW_OFF, DSW_STEP, DSW_SLOPE };
enum { NOISE_PINK, NOISE_WHITE };
enum { AMP_GATE, AMP_ENV };
enum { TRIG_LFO, TRIG_GATE, TRIG_GATE_TRIG };
enum { LFO_SAW, LFO_INV_SAW, LFO_TRI, LFO_SQR, LFO_RND, LFO_NOISE };
enum { RISER_OFF, RISER_SYNC, RISER_QUIV, RISER_QVPAN };
enum { POLY_MONO, POLY_UNISON, POLY_POLY, POLY_CHORD, NPOLY_MODE };
enum { PORTA_OFF, PORTA_ON, PORTA_AUTO };
enum { CHO_OFF, CHO_TYPE1, CHO_TYPE2, CHO_TYPE3, CHO_TYPE4 };
enum { REV_AMB, REV_ROOM, REV_HALL1, REV_HALL2, REV_PLATE, REV_SPRING, REV_MOD };

typedef struct {
    const char *name;                           /* 7 characters at most (the knob label) */
    uint8_t max, def, kind, cc;                 /* cc: fixed MIDI CC, 0 = taken from table */
} pdef_t;

static const pdef_t PDEF[NPARAM] = {
    /* OSC CORE */
    [P_RANGE]       = {"RANGE",   5,   RANGE_8,     K_RANGE,   14},
    [P_PULSE_LVL]   = {"SQR LVL", 127, 100,         K_NUM,     19},
    [P_SAW_LVL]     = {"SAW LVL", 127, 110,         K_NUM,     20},
    [P_SUB_LVL]     = {"SUB LVL", 127, 80,          K_NUM,     21},

    /* OSC SUB & NOISE */
    [P_SUB_TYPE]    = {"SUB TYP", 2,   SUB_1OC,     K_SUBTYPE, 22},
    [P_NOISE_LVL]   = {"NOISE",   127, 0,           K_NUM,     23},
    [P_NOISE_MODE]  = {"NOI MOD", 1,   NOISE_PINK,  K_NOISEM,  78},
    [P_FINE_TUNE]   = {"FINE",    127, 64,          K_BIP,     76},

    /* PWM */
    [P_PWM_SRC]     = {"PWM SRC", 2,   PWM_MANUAL,  K_PWMS,    16},
    [P_PWM_DEPTH]   = {"PW/DUTY", 127, 64,          K_PCT,     15},

    /* OSC DRAW */
    [P_DRAW_SW]     = {"DRAW SW", 2,   DSW_OFF,     K_DRAWSW,  107},
    [P_DRAW_MULT]   = {"DRW MLT", 31,  0,           K_MULT,    102},

    /* OSC CHOP */
    [P_CHOP_OVTN]   = {"OVRTONE", 200, 0,           K_OVTN,    103},
    [P_CHOP_COMB]   = {"COMB",    31,  0,           K_COMB,    104},

    /* FILTER */
    [P_CUTOFF]      = {"CUTOFF",  127, 85,          K_CUT,     74},
    [P_RESO]        = {"RESO",    127, 25,          K_NUM,     71},
    [P_FENV_AMT]    = {"ENV AMT", 127, 0,           K_NUM,     24},
    [P_FLFO_AMT]    = {"FLT LFO", 127, 0,           K_NUM,     25},
    [P_KEYTRK]      = {"KEY TRK", 127, 64,          K_PCT,     26},
    [P_FLT_BEND]    = {"FLT BND", 24,  2,           K_NUM,     27},
    [P_FLT_COLOR]   = {"FLT COL", 1,   0,           K_FLTCOL,  108},
    [P_TONE]        = {"TONE",    127, 64,          K_BIP,     109},
    [P_WARMTH]      = {"WARMTH",  127, 64,          K_PCT,     110},

    /* ENVELOPE */
    [P_ATTACK]      = {"ATTACK",  127, 5,           K_MS,      73},
    [P_DECAY]       = {"DECAY",   127, 65,          K_MS,      75},
    [P_SUSTAIN]     = {"SUSTAIN", 127, 80,          K_NUM,     30},
    [P_RELEASE]     = {"RELEASE", 127, 45,          K_MS,      72},
    [P_AMP_MODE]    = {"AMP MOD", 1,   AMP_ENV,     K_AMP,     28},
    [P_ENV_TRG]     = {"ENV TRG", 2,   TRIG_GATE_TRIG, K_ENVTRG, 29},

    /* LFO */
    [P_LFO_RATE]    = {"RATE",    127, 60,          K_HZ,      3},
    [P_LFO_WAVE]    = {"WAVE",    5,   LFO_TRI,     K_LFOWAVE, 12},
    [P_LFO_SYNC]    = {"SYNC",    1,   0,           K_ONOFF,   106},
    [P_LFO_TRIG]    = {"KEY TRG", 1,   0,           K_ONOFF,   105},
    [P_LFO_MODE]    = {"FAST/N",  1,   0,           K_ONOFF,   79},
    [P_LFO_PITCH]   = {"PITCH",   127, 0,           K_NUM,     13},
    [P_MOD_DEPTH]   = {"MOD DPT", 127, 0,           K_NUM,     17},
    [P_OSC_BEND]    = {"OSC BND", 24,  2,           K_NUM,     18},

    /* RISER */
    [P_RISER_MODE]  = {"RSR MOD", 3,   RISER_OFF,   K_RISERM,  0},
    [P_RISER_RESO]  = {"RSR RES", 100, 30,          K_NUM,     0},
    [P_RISER_SHAPE] = {"RSR SHP", 100, 0,           K_NUM,     0},
    [P_RISER_LVL]   = {"RSR LVL", 100, 80,          K_NUM,     0},

    /* DELAY */
    [P_DLY_LVL]     = {"DLY LVL", 127, 0,           K_NUM,     92},
    [P_DLY_TIME]    = {"DLY TIM", 127, 14,          K_DDIV,    90},
    [P_DLY_FB]      = {"DLY FB",  127, 50,          K_NUM,     0},
    [P_DLY_SYNC]    = {"DLY SYN", 1,   1,           K_ONOFF,   0},

    /* REVERB & CHORUS */
    [P_REV_LVL]     = {"REV LVL", 127, 0,           K_NUM,     91},
    [P_REV_TIME]    = {"REV TIM", 127, 100,         K_NUM,     89},
    [P_REV_TYPE]    = {"REV TYP", 6,   REV_SPRING,  K_REVM,    0},
    [P_REV_PRE]     = {"PRE-DLY", 100, 20,          K_MS,      0},
    [P_CHO_MODE]    = {"CHORUS",  4,   CHO_OFF,     K_CHOM,    93},

    /* VOICE & GLOBAL */
    [P_POLY_MODE]   = {"VOICING", NPOLY_MODE - 1, POLY_POLY, K_PMODE, 80},
    [P_PORTA_MODE]  = {"PORTA M", 2,   PORTA_OFF,   K_PORTAM,  31},
    [P_PORTA_TIME]  = {"PORTA T", 127, 30,          K_MS,      5},
    [P_PORTA_SW]    = {"PORTA",   1,   0,           K_ONOFF,   65},
    [P_TRANSPOSE]   = {"TRANSP",  124, 64,          K_SEMI,    77},
    [P_VOLUME]      = {"VOLUME",  127, 100,         K_NUM,     7},

    /* CHORD CONFIG */
    [P_CHORD_SW2]   = {"V2 SW",   1,   1,           K_ONOFF,   81},
    [P_CHORD_SW3]   = {"V3 SW",   1,   1,           K_ONOFF,   82},
    [P_CHORD_SW4]   = {"V4 SW",   1,   1,           K_ONOFF,   83},
    [P_CHORD_KS2]   = {"V2 SHFT", 24,  12,          K_SEMI,    85},
    [P_CHORD_KS3]   = {"V3 SHFT", 24,  12,          K_SEMI,    86},
    [P_CHORD_KS4]   = {"V4 SHFT", 24,  12,          K_SEMI,    87},

    /* ADV: stereo, phaser & the lo-fi/distortion slot (defaults = 0: stock S-1 behaviour) */
    [P_SPREAD]      = {"SPREAD",  100, 0,           K_NUM,     41},
    [P_DETUNE]      = {"DETUNE",  50,  0,           K_NUM,     42},
    [P_PHASER_MIX]  = {"PHR MIX", 100, 0,           K_NUM,     0},
    [P_FX_MODE]     = {"FX MODE", 1,   0,           K_FXMODE,  45},
    [P_FX_AMT]      = {"FX AMT",  100, 0,           K_NUM,     46},
    [P_FX_MIX]      = {"FX MIX",  100, 50,          K_NUM,     47},
};

static const char *const RANGE_NAME[6] = {"64'", "32'", "16'", "8'", "4'", "2'"};
static const char *const SUB_NAME[3] = {"-2o.A", "-2oc", "-1oc"};
static const char *const PWMS_NAME[3] = {"ENV", "MAN", "LFO"};
static const char *const DRAWSW_NAME[3] = {"OFF", "STEP", "SLOPE"};
static const char *const NOISEM_NAME[2] = {"PINK", "WHITE"};
static const char *const ENVTRG_NAME[3] = {"LFO", "GATE", "G+TRG"};
static const char *const AMP_NAME[2] = {"GATE", "ENV"};
static const char *const LFOWAVE_NAME[6] = {"SAW", "IN.SAW", "TRI", "SQR", "RND", "NOISE"};
static const char *const RISER_NAME[4] = {"OFF", "SYNC", "QUIV", "QV.PAN"};
static const char *const CHORUS_NAME[5] = {"OFF", "TYPE 1", "TYPE 2", "TYPE 3", "TYPE 4"};
static const char *const REVERB_NAME[7] = {"AMB", "ROOM", "HALL 1", "HALL 2", "PLATE", "SPRING", "MOD"};
static const char *const POLY_NAME[NPOLY_MODE] = {"MONO", "UNISON", "POLY", "CHORD"};
static const char *const PORTA_NAME[3] = {"OFF", "ON", "AUTO"};
static const char *const FLTCOL_NAME[2] = {"S-1", "WARM"};
static const char *const FXMODE_NAME[2] = {"REDUCE", "DIST"};

/* Roland S-1 LFO Tempo Sync 31 Note Divisions */
static const char *const LFO_SYNC_NAME[31] = {
    "8_1", "6_1", "8_1t", "4_1", "3_1", "4_1t", "2_1", "1d", "2_1t", "1_1",
    "2d",  "1t",  "1_2",  "4d",  "2t",  "1_4",  "8d",  "4t", "1_8",  "16d",
    "8t",  "1_16", "32d", "16t", "1_32", "64d", "32t", "1_64", "128d", "64t", "128"
};
static const uint8_t LFO_SYNC_RATIO[31][2] = {
    {32, 1}, {24, 1}, {64, 3}, {16, 1}, {12, 1}, {32, 3}, {8, 1}, {6, 1},
    {16, 3}, {4, 1},  {3, 1},  {8, 3},  {2, 1},  {3, 2},  {4, 3}, {1, 1},
    {3, 4},  {2, 3},  {1, 2},  {3, 8},  {1, 3},  {1, 4},  {3, 16}, {1, 6},
    {1, 8},  {3, 32}, {1, 12}, {1, 16}, {3, 64}, {1, 24}, {1, 32}
};

/* Arpeggiator Types & Rates (Roland S-1) */
enum { ARP_UP, ARP_DOWN, ARP_UP_DOWN, ARP_UP_2OCT, ARP_DOWN_2OCT, ARP_UP_DOWN_2OCT, ARP_RANDOM, ARP_RANDOM_2OCT, NARP_TYPE };
static const char *const ARP_TYPE_NAME[NARP_TYPE] = {"UP", "DOWN", "UP.DW", "UP.2", "DW.2", "U.D.2", "RAND", "RND.2"};

enum { AR_4, AR_8, AR_16, AR_32, AR_8T, AR_16T, AR_32T, NARP_RATE };
static const char *const ARP_RATE_NAME[NARP_RATE] = {"1/4", "1/8", "1/16", "1/32", "8T", "16T", "32T"};

/* Step Sequencer Divisions (Pattern Scale) */
enum { D_4, D_8, D_16, D_32, D_8T, D_16T, NDIV };
static const char *const DIV_NAME[NDIV] = {"1/4", "1/8", "1/16", "1/32", "1/8T", "1/16T"};
static const uint8_t DIV_PER_QUARTER[NDIV] = {1, 2, 4, 8, 3, 6};

/* Delay Sync Divisions - Roland S-1 16 note lengths */
enum {
    DD_128, DD_64T, DD_128D, DD_64, DD_32T, DD_64D, DD_32, DD_16T,
    DD_32D, DD_16,  DD_8T,   DD_16D, DD_8,  DD_4T,  DD_8D,  DD_4,
    NDDIV = 16
};
static const char *const DDIV_NAME[NDDIV] = {
    "1/128", "64T", "128D", "1/64", "32T", "64D", "1/32", "16T",
    "32D",   "1/16", "8T",  "16D",  "1/8",  "4T",   "8D",   "1/4"
};
static const uint8_t DDIV_NUM[NDDIV] = {1,  1,  3,  1,  1,  3, 1, 1, 3,  1, 1, 3, 1, 2, 3, 1};
static const uint8_t DDIV_DEN[NDDIV] = {32, 24, 64, 16, 12, 32, 8, 6, 16, 4, 3, 8, 2, 3, 4, 1};

/* The Page Structure (4 knobs per page, -1 = no parameter) */
typedef struct { const char *title; int8_t p[4]; } page_t;

enum {
    PG_PERFORM,     /* HOME Button: Filter Cutoff, Reso, Env Depth, Porta Time */

    /* EDIT Button Pages (1..6) */
    PG_OSC_CORE,    /* Range, Square Level, Saw Level, Sub Level */
    PG_OSC_PWM,     /* PWM Depth, PWM Source, Square Level, LFO Rate */
    PG_OSC_SUB,     /* Sub Type, Noise Level, Noise Mode, Fine Tune */
    PG_OSC_DRAW,    /* Draw Switch, Multiply, Step, Amplitude */
    PG_OSC_CHOP,    /* Chop Overtone, Comb, -1, -1 */
    PG_FILTER,      /* Cutoff, Reso, Filter LFO, Key Follow */

    /* ENV Button Pages (1..2) */
    PG_ADSR,        /* Attack, Decay, Sustain, Release */
    PG_ENV_MOD,     /* Env Trig Mode, Amp Mode, PWM Source, PWM Depth */

    /* LFO Button Pages (1..2) */
    PG_LFO,         /* Rate, Waveform, LFO Sync, LFO Mode */
    PG_LFO_MOD,     /* Pitch Depth, Filter LFO Depth, Mod Depth, Key Trigger */

    /* FX Button Pages (1..3) */
    PG_FX_MIX,      /* FX Mix: Chorus Mode, Delay Level, Reverb Level, Delay Time */
    PG_DELAY,       /* Delay Level, Delay Time, Feedback, Sync */
    PG_REVERB,      /* Reverb Level, Reverb Time, Reverb Type, Chorus Mode */

    /* GLO Button Pages (1..2) */
    PG_VOICE,       /* Poly Mode, Porta Mode, Transpose, Volume */
    PG_GLOBAL,      /* Tempo, MIDI Ch, Master Prob, Pattern Scale (drawn by hand) */

    PG_ARP,         /* Arpeggiator Page: Mode, Hold, Type, Rate */
    PG_ARP_CFG,     /* Arpeggiator Config: Note Priority */
    PG_SEQ,         /* Sequencer view (drawn by hand) */

    /* SEL Button Pages (1..2): the advanced controls */
    PG_ADV1,        /* Spread, Detune, -1, -1 */
    PG_ADV2,        /* FX Mode, FX Amount, FX Mix, -1 */
    NPAGE
};

static const page_t PAGE[NPAGE] = {
    [PG_PERFORM]    = {"PERFORM",   {P_CUTOFF, P_RESO, P_FENV_AMT, P_PORTA_TIME}},
    [PG_OSC_CORE]   = {"OSC CORE",  {P_RANGE, P_PULSE_LVL, P_SAW_LVL, P_SUB_LVL}},
    [PG_OSC_PWM]    = {"OSC PWM",   {P_PWM_DEPTH, P_PWM_SRC, P_PULSE_LVL, P_LFO_RATE}},
    [PG_OSC_SUB]    = {"SUB/NOISE", {P_SUB_TYPE, P_NOISE_LVL, P_NOISE_MODE, P_FINE_TUNE}},
    [PG_OSC_DRAW]   = {"OSC DRAW",  {P_DRAW_SW, P_DRAW_MULT, -1, -1}},
    [PG_OSC_CHOP]   = {"OSC CHOP",  {P_CHOP_OVTN, P_CHOP_COMB, -1, -1}},
    [PG_FILTER]     = {"FILTER",    {P_CUTOFF, P_RESO, P_FLFO_AMT, P_KEYTRK}},
    [PG_ADSR]       = {"ENVELOPE",  {P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE}},
    [PG_ENV_MOD]    = {"ENV / PWM", {P_ENV_TRG, P_AMP_MODE, P_PWM_SRC, P_PWM_DEPTH}},
    [PG_LFO]        = {"LFO",       {P_LFO_RATE, P_LFO_WAVE, P_LFO_SYNC, P_LFO_MODE}},
    [PG_LFO_MOD]    = {"LFO MOD",   {P_LFO_PITCH, P_FLFO_AMT, P_PWM_DEPTH, P_LFO_TRIG}},
    [PG_FX_MIX]     = {"FX MIX",    {P_CHO_MODE, P_DLY_LVL, P_REV_LVL, P_DLY_TIME}},
    [PG_DELAY]      = {"DELAY",     {P_DLY_LVL, P_DLY_TIME, P_DLY_FB, P_DLY_SYNC}},
    [PG_REVERB]     = {"REVERB",    {P_REV_LVL, P_REV_TIME, P_REV_TYPE, P_REV_PRE}},
    [PG_VOICE]      = {"VOICE",     {P_POLY_MODE, P_PORTA_MODE, P_TRANSPOSE, P_VOLUME}},
    [PG_GLOBAL]     = {"GLOBAL",    {-1, -1, -1, -1}},
    [PG_ARP]        = {"ARPEGGIO",  {-1, -1, -1, -1}},
    [PG_ARP_CFG]    = {"ARP CONFIG",{-1, -1, -1, -1}},
    [PG_SEQ]        = {"SEQUENCER", {-1, -1, -1, -1}},
    [PG_ADV1]       = {"STEREO",    {P_SPREAD, P_DETUNE, -1, -1}},
    [PG_ADV2]       = {"LOFI/ECO",  {P_FX_MODE, P_FX_AMT, P_FX_MIX, -1}},
};

typedef struct { uint8_t v[PATCH_BYTES]; } patch_t;

static void patch_defaults(patch_t *p)
{
    uint32_t i;
    for (i = 0; i < NPARAM; i++) p->v[i] = PDEF[i].def;
    for (; i < PATCH_BYTES; i++) p->v[i] = ' ';
}

typedef struct { const char *name; const uint8_t *set; } factory_t;
static void patch_from_factory(patch_t *p, const factory_t *f)
{
    const uint8_t *s = f->set;
    uint32_t i;
    patch_defaults(p);
    while (s[0] != 255u) { p->v[s[0]] = s[1]; s += 2; }
    p->v[P_TRANSPOSE] = 64;
    for (i = 0; i < PATCH_NAME && f->name[i]; i++) p->v[NPARAM + i] = (uint8_t)f->name[i];
}

#endif /* OBV_PARAMS_H */
