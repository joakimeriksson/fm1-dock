/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 npi3pak */
#ifndef MACRO_H
#define MACRO_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MACRO_ENGINES 24
#define MACRO_ROUTES 8
#define MACRO_LFOS 2

enum {
    SRC_OFF, SRC_LFO1, SRC_LFO2, SRC_ENV, SRC_VEL, SRC_NOTE, SRC_MODW, SRC_AFT, SRC_RAND, SRC_GATE,
    SRC_COUNT
};

enum {
    DST_OFF, DST_PITCH, DST_HARM, DST_TIMBRE, DST_MORPH, DST_MODEL, DST_LEVEL, DST_LFO1, DST_LFO2,
    DST_COUNT
};

enum { MODE_TRIG, MODE_DRONE, MODE_GATE, MODES };

enum { LFO_SINE, LFO_TRI, LFO_SAW, LFO_SQUARE, LFO_SH, LFO_SMOOTH, LFO_SHAPES };

typedef struct {
    int8_t src, dst;
    int16_t amount;
} macro_route_t;

typedef struct {
    int16_t engine;
    int16_t harmonics, timbre, morph;
    int16_t transpose;
    int16_t fm_amt, timbre_amt, morph_amt;
    int16_t decay, colour;
    int16_t lfo_rate[MACRO_LFOS], lfo_shape[MACRO_LFOS], lfo_sync[MACRO_LFOS];
    int16_t env_a, env_d, env_s, env_r;
    int16_t blend, spread;
    int16_t mode;
    int16_t glide;
    int16_t reserved[4];
    macro_route_t route[MACRO_ROUTES];
} macro_patch_t;

typedef struct {
    int16_t bend_range;
    int16_t fine;
    int16_t legato;
    int16_t channel;
    int16_t volume;
    int16_t reserved[3];
} macro_global_t;

typedef struct {
    int16_t src[SRC_COUNT];
    int16_t dst[DST_COUNT];
    int16_t engine;
    int16_t note10;
    uint8_t gate;
    uint8_t patched;
} macro_monitor_t;

void macro_init(void);
void macro_render(const macro_patch_t *p, const macro_global_t *g, int32_t *lr, uint32_t frames);
void macro_note_on(uint8_t note, uint8_t velocity);
void macro_note_off(uint8_t note);
void macro_all_notes_off(void);
void macro_controller(uint8_t mod_wheel, uint8_t aftertouch, int16_t bend);
void macro_monitor(macro_monitor_t *m);

#ifdef __cplusplus
}
#endif

#endif
