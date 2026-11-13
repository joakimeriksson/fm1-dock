// Copyright 2014-2016 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// KEYS' parts of Mutable Instruments' stmlib and Plaits (pichenettes/eurorack f4cfa76), ported to C for fm1-dock's
// KEYS module by Joakim Eriksson, 2026, under the same MIT licence (modules/keys/LICENSE-MIT-Plaits):
//   - stmlib/dsp/filter.h: the SVF (Svf: set_f_q with OnePole::tan<FREQUENCY_FAST>, the low-pass and band-pass
//     outputs) and the DC blocker (DCBlocker);
//   - stmlib/utils/random.h: the generator (Random::GetWord, GetFloat);
//   - plaits/dsp/physical_modelling/string_voice.cc: a string's excitation (StringVoice::Render, not sustained):
//     white noise for one period of the note through a low-pass SVF (q 0.5) whose cutoff is 4 x f0 moved by
//     (brightness (2 - brightness) - 0.5) x 72 semitones, brightness raised by the accent;
//   - plaits/dsp/physical_modelling/resonator.cc: the stiffness stretch of a mode series (NthHarmonicCompensation's
//     recurrence), here km_stretch.
// Changes: C structs and functions in place of the classes; frequencies normalised to the sample rate as in Plaits;
// the semitone ratio from the caller's exp2 (no lookup tables).

#ifndef KEYS_MI_H
#define KEYS_MI_H

#include <stdint.h>

#define KM_PI 3.14159265f

/* stmlib Svf: the TPT state-variable filter (Zavalishin's), coefficients g = tan(pi f), r = 1 / q, h */
typedef struct {
    float g, r, h;
    float s1, s2;
} km_svf_t;

/* stmlib OnePole::tan<FREQUENCY_FAST>: tan(pi f), coefficients fitted for 16 Hz .. 16 kHz */
static inline float km_tan_fast(float f)
{
    const float a = 3.260e-01f * 31.00627668f;   /* x pi^3 */
    const float b = 1.823e-01f * 306.01968478f;  /* x pi^5 */
    float f2 = f * f;
    return f * (KM_PI + f2 * (a + b * f2));
}

static inline void km_svf_set(km_svf_t *s, float f, float q)
{
    if (f > 0.497f)
        f = 0.497f;
    s->g = km_tan_fast(f);
    s->r = 1.0f / q;
    s->h = 1.0f / (1.0f + s->r * s->g + s->g * s->g);
}

static inline void km_svf_reset(km_svf_t *s) { s->s1 = s->s2 = 0.0f; }

/* one sample; *lp and *bp out */
static inline void km_svf_lp_bp(km_svf_t *s, float in, float *lp, float *bp)
{
    float hp = (in - s->r * s->s1 - s->g * s->s1 - s->s2) * s->h;
    float b = s->g * hp + s->s1;
    float l;
    s->s1 = s->g * hp + b;
    l = s->g * b + s->s2;
    s->s2 = s->g * b + l;
    *lp = l;
    *bp = b;
}

static inline float km_svf_lp(km_svf_t *s, float in)
{
    float lp, bp;
    km_svf_lp_bp(s, in, &lp, &bp);
    return lp;
}

/* stmlib DCBlocker: y = pole y + x - x[-1] */
typedef struct {
    float pole, x, y;
} km_dc_t;

static inline void km_dc_init(km_dc_t *d, float pole)
{
    d->pole = pole;
    d->x = d->y = 0.0f;
}

static inline float km_dc(km_dc_t *d, float in)
{
    float y = d->y * d->pole + in - d->x;
    d->x = in;
    d->y = y;
    return y;
}

/* stmlib Random */
static inline uint32_t km_rand_word(uint32_t *st)
{
    *st = *st * 1664525u + 1013904223u;
    return *st;
}

static inline float km_rand_float(uint32_t *st)
{
    return (float)(km_rand_word(st) >> 8) * (1.0f / 16777216.0f);   /* (24 bits: exact in a float) */
}

/* Plaits StringVoice: the excitation filter's cutoff (normalised), for f0 (normalised), brightness 0..1 and the
 * accent 0..1; exp2 is the caller's */
static inline float km_excite_cutoff(float f0, float brightness, float accent, float (*exp2)(float))
{
    float c;
    brightness += 0.25f * accent * (1.0f - brightness);
    c = 4.0f * f0 * exp2((brightness * (2.0f - brightness) - 0.5f) * 72.0f * (1.0f / 12.0f));
    return c < 0.499f ? c : 0.499f;
}

/* Plaits Resonator: the n-th mode's stretch for a stiffness (n from 1), NthHarmonicCompensation's recurrence */
static inline float km_stretch(int n, float stiffness)
{
    float stretch = 1.0f;
    int i;
    for (i = 0; i < n - 1; ++i) {
        stretch += stiffness;
        stiffness *= stiffness < 0.0f ? 0.93f : 0.98f;
    }
    return stretch;
}

#endif
