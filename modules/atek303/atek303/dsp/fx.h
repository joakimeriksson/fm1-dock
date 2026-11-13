/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Javier (Animatek)
 *
 * ATEK303's effects for the FM-1, the classic acid chain, mono in, stereo out:
 *
 *   DRIVE   a pedal after the 303: OVERDRIVE (soft, asymmetric), RAT (op-amp gain into hard diode
 *           clipping, with its pre-emphasis) or FUZZ (huge gain, lopsided). Antiderivative
 *           anti-aliasing on the clipper, so a square through it doesn't fizz; TONE and LEVEL after.
 *   MOD     CHORUS (two voices of a modulated 7 ms delay, left and right a quarter turn apart) or
 *           FLANGER (a short modulated delay fed back).
 *   DELAY   ping-pong, synced to the tempo (1/16 .. 1/4 dotted), each repeat darker (a low-pass in
 *           the loop) and without mud (a high-pass); turning TIME bends the pitch like tape.
 *   REVERB  Dattorro's plate (J. Dattorro, "Effect Design Part 1", JAES 1997), with its modulated
 *           tank and a pre-delay.
 *
 * All of it single precision, no libm (fastmath.h). The buffers live in the device's .pool RAM.
 * fx_process() runs in the audio interrupt; fx_set() and fx_tempo() between blocks. */
#pragma once
#include <stdint.h>

enum {
    FX_DRIVE_TYPE, FX_DRIVE, FX_TONE, FX_LEVEL,          /* type: 0 off, 1 overdrive, 2 rat, 3 fuzz */
    FX_MOD_TYPE, FX_MOD_RATE, FX_MOD_DEPTH, FX_MOD_MIX,  /* type: 0 chorus, 1 flanger */
    FX_DLY_TIME, FX_DLY_FEEDBACK, FX_DLY_TONE, FX_DLY_MIX,   /* time: 0..5, see FX_DLY_NAME */
    FX_REV_DECAY, FX_REV_DAMP, FX_REV_PRE, FX_REV_MIX,
    FX_NPARAMS
};
#define FX_DRIVE_TYPES 4
#define FX_MOD_TYPES 2
#define FX_DLY_TIMES 6
extern const char *const FX_DRIVE_NAME[FX_DRIVE_TYPES];
extern const char *const FX_MOD_NAME[FX_MOD_TYPES];
extern const char *const FX_DLY_NAME[FX_DLY_TIMES];

void fx_init(float sr);
void fx_set(int param, float value);   /* knobs 0..1; the types and TIME as their index */
void fx_tempo(float sixteenth_s);      /* the sequencer's step, for the delay's sync */
float fx_delay_seconds(void);          /* the delay time it plays (clamped to the buffer) */
void fx_process(const float *in, float *l, float *r, uint32_t n);
