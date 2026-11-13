/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Javier (Animatek)
 *
 * The ATEK303 SEQ's pattern generator, in C: a port of src/AcidPattern.hpp, AcidCorpus.hpp and the parts of
 * src/AcidGen.hpp it uses (../../VCV-Rack/UZZ-VCV-RACK, the ATEK303 SEQ of 64 steps). Same algorithm, same streams, same numbers:
 * a seed gives the same pattern here as in VCV Rack (tests/pattern_*.c* check it bit for bit).
 *
 * The pattern (v4 layout; v5 of the generator, UZZ-VCV-RACK's: tuned on a corpus of acid lines, 64 steps) lives in two layers:
 *   - time:  REST, NOTE or TIE per step (a tie holds the previous note, no new attack);
 *   - pitch: one event per NOTE, in order: scale degree, octave (-2..+2), accent, slide out.
 * acid_render() turns it into the per-step grid the sequencer plays. Degrees, not semitones:
 * the scale and the root are applied at the output, so they can change without losing the line. */
#pragma once
#include <stdint.h>

#define ACID_MAX_STEPS 64                /* as the ATEK303 SEQ in VCV Rack: 4 pages of 16 */
#define ACID_SCALES_LEN 8

typedef struct {
    const char *name;                 /* the long name */
    const char *short_name;           /* for the screen */
    int n;
    int8_t s[12];
} acid_scale_t;
extern const acid_scale_t ACID_SCALES[ACID_SCALES_LEN];
extern const char *const ACID_NOTE_NAMES[12];

/* the character knobs, all 0..1 except steps and scale */
typedef struct {
    int steps;                        /* 1..64 */
    int scale;                        /* index in ACID_SCALES */
    float density, accent, slide, range, tie;
} acid_params_t;

enum { ACID_REST = 0, ACID_NOTE = 1, ACID_TIE = 2 };

typedef struct {
    int8_t degree, octave;
    uint8_t accent, slide_out;
} acid_pitch_t;

typedef struct {
    uint32_t seed;
    uint8_t algorithm_version;
    uint8_t time_length;
    uint8_t pitch_length;
    uint8_t time[ACID_MAX_STEPS];
    acid_pitch_t pitch[ACID_MAX_STEPS];
} acid_pattern_t;

/* the rendered grid (AcidGen's fields) */
typedef struct {
    int8_t deg[ACID_MAX_STEPS], oct[ACID_MAX_STEPS];
    uint8_t gate[ACID_MAX_STEPS], accent[ACID_MAX_STEPS], slide[ACID_MAX_STEPS], tie[ACID_MAX_STEPS];
} acid_grid_t;

enum { ACID_MUT_TIME = 1, ACID_MUT_PITCH = 2, ACID_MUT_ARTICULATION = 3 };

void acid_params_default(acid_params_t *p);    /* the SEQ module's knob defaults */
int acid_semi_of(int degree, int octave, int scale);
void acid_clear(acid_pattern_t *pat);
void acid_sanitize(acid_pattern_t *pat, int scale);
/* the same, but keep_slides keeps every slide flag as it is (the editor: a slide into a rest or into the same
 * note is the player's to set; the sequencer decides what it does) */
void acid_sanitize_ex(acid_pattern_t *pat, int scale, int keep_slides);
void acid_render(const acid_pattern_t *pat, acid_grid_t *out);
/* returns the contour picked (0 pedal, 1 rise, 2 fall, 3 zigzag, 4 scatter) */
int acid_generate(acid_pattern_t *pat, const acid_params_t *p, uint32_t seed);
/* a grid back into the pattern (AcidPatternV4::importRendered); the editor's way in */
void acid_import(acid_pattern_t *pat, const acid_grid_t *in, int length, uint32_t seed, uint8_t version);
/* a burst of deterministic mutations on one layer (2 for time and pitch, 3 for articulation in the
 * module); *last gets the last mutation index used. 0 = nothing changed (pattern untouched). */
int acid_mutate_burst(acid_pattern_t *pat, int layer, uint32_t first_index, int operations, int scale,
                      uint32_t *last);
/* the same within the steps [lo, hi) (a page; clipped to the pattern's length) */
int acid_mutate_burst_range(acid_pattern_t *pat, int layer, uint32_t first_index, int operations, int scale,
                            uint32_t *last, int lo, int hi);
