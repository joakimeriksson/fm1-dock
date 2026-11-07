/* SPDX-License-Identifier: GPL-3.0-only */
/* ARTIHIVE 1.1 DSP helpers, written for the FM-1's integer-only pi32v2 core.
 * Independent implementation: the oscillator is a two-edge PolyBLEP
 * pulse with a leaky-integrated triangle, and the delay stores linear 12-bit
 * PCM, two samples in three bytes. The public functions are static so the
 * same file is shared by the firmware and the host unit tests. */
#ifndef ARTIHIVE_DSP_H
#define ARTIHIVE_DSP_H
#include <stdint.h>
#include "artihive_tables.h"

#ifndef DFRAMES                                  /* fm1-dock: settable (modules/artihive/take.py) */
#define DFRAMES 96000u                 /* 2.177 s capacity per line at 44100 Hz */
#endif
#define DPACKED (DFRAMES * 3u / 2u)    /* 144000 bytes per line, not 192000 */
#define BLEP_FMAX 1932735283u          /* floor(0.45 * 2^32): correction width cap */
#define BLEP_FMIN 131072u              /* reciprocal stays inside uint32_t */
#ifndef DELAY_MAX_FRAMES                                  /* fm1-dock: settable (modules/artihive/take.py) */
#define DELAY_MAX_FRAMES 89938u          /* 2039.4 ms: preserves the old TIME=127 maximum */
#endif

/* Quadratic TIME law: one sample at 0, fine comb/resonator control at the bottom,
 * and the old 2.039 s maximum at 127. Called from the control/UI side, not the ISR. */
static uint32_t delay_frames(uint32_t time)
{
    if (time > 127u) time = 127u;
    return 1u + (time * time * (DELAY_MAX_FRAMES - 1u)) / (127u * 127u);
}

/* floor(log2(x)), x != 0. Five predictable steps, no CLZ library call. */
static uint32_t log2_u32(uint32_t x)
{
    uint32_t e = 0;
    if (x >= 65536u) { x >>= 16; e += 16; }
    if (x >= 256u)   { x >>= 8;  e += 8; }
    if (x >= 16u)    { x >>= 4;  e += 4; }
    if (x >= 4u)     { x >>= 2;  e += 2; }
    if (x >= 2u)     e += 1;
    return e;
}

/* round(2^48 / x) for the audio range, as a Q16 reciprocal of x/2^32.
 * A 256-entry mantissa table plus one linear interpolation step. The caps
 * only affect sub-audio instantaneous rates and extreme FM above 0.45 Fs. */
static uint32_t blep_recip(uint32_t x)
{
    uint32_t e, m, f, r;
    if (x < BLEP_FMIN) x = BLEP_FMIN;
    if (x > BLEP_FMAX) x = BLEP_FMAX;
    e = log2_u32(x);
    if (e >= 8u) {
        m = (x >> (e - 8u)) & 255u;
        f = (e >= 24u) ? ((x >> (e - 24u)) & 65535u)
                       : ((x << (24u - e)) & 65535u);
    } else {
        m = (x << (8u - e)) & 255u;
        f = 0;
    }
    r = BLEP_RECIP[m] + (int32_t)((int32_t)BLEP_RECIP[m + 1] - (int32_t)BLEP_RECIP[m]) * (int32_t)f / 65536;
    return r << (32u - e);
}

/* One PolyBLEP residue, Q16, for a rising saw discontinuity at phase zero.
 * dt is the capped |phase step|; inv is its Q16 reciprocal. */
static int32_t blep(uint32_t ph, uint32_t dt, uint32_t inv)
{
    uint32_t x, d;
    int32_t y;
    if (ph < dt) {
        x = (uint32_t)(((uint64_t)ph * inv) >> 32);
        if (x > 65536u) x = 65536u;
        d = 65536u - x;
        y = -(int32_t)((uint32_t)(((uint64_t)d * d) >> 16));
    } else if (ph > 0xFFFFFFFFu - dt) {
        x = (uint32_t)(((uint64_t)(ph + dt) * inv) >> 32);
        if (x > 65536u) x = 65536u;
        y = (int32_t)(((uint64_t)x * x) >> 16);
    } else return 0;
    return y;
}

/* Band-limited triangle -> square morph. ph is the effective (FM/PM) phase;
 * step is its actual per-sample delta, so the correction follows audio-rate
 * modulation rather than the nominal TUNE. *integ is the triangle's leaky
 * integrator state. morph is Q15, 0 = triangle, 32768 = square. */
static int32_t bl_voice(uint32_t ph, uint32_t step, uint32_t morph, int32_t *integ)
{
    uint32_t mag = (step & 0x80000000u) ? (0u - step) : step;
    uint32_t dt = mag > BLEP_FMAX ? BLEP_FMAX : mag;
    uint32_t inv = blep_recip(dt);
    int32_t b0 = blep(ph, dt, inv), b1 = blep(ph + 0x80000000u, dt, inv);
    int32_t sq, tr, y;
    if (step & 0x80000000u) { b0 = -b0; b1 = -b1; }
    sq = ((ph & 0x80000000u) ? -65536 : 65534) + b0 - b1;
    sq >>= 1;
    if (sq > 49151) sq = 49151;          /* keep the bandlimited Gibbs overshoot */
    if (sq < -49151) sq = -49151;
    /* A 1.35 Hz leak keeps the integrator inside int32 without eating bass.
     * The square is halved before integration; the >>14 below restores 2*f. */
    *integ += (sq >> 1) - (*integ >> 15);
    tr = (int32_t)(((int64_t)*integ * (int32_t)(dt >> 16)) >> 14);
    if (tr > 49151) tr = 49151;
    if (tr < -49151) tr = -49151;
    y = tr + (int32_t)(((int64_t)(sq - tr) * (int32_t)morph) >> 15);
    return y;
}

/* Linear 12-bit offset-binary PCM. Two samples share three bytes:
 * [a7..a0] [b3..b0 | a11..a8] [b11..b4]. No companding noise modulation;
 * the existing DC blockers remove the half-LSB offset. */
static int32_t d12_read(const uint8_t *p, uint32_t i)
{
    uint32_t b = (i >> 1) * 3u, u;
    if (i & 1u) u = (uint32_t)(p[b + 1] >> 4) | ((uint32_t)p[b + 2] << 4);
    else        u = (uint32_t)p[b] | ((uint32_t)(p[b + 1] & 15u) << 8);
    return (int32_t)(u << 4) - 32768;
}

static void d12_write(uint8_t *p, uint32_t i, int32_t x)
{
    uint32_t b = (i >> 1) * 3u, u;
    if (x < -32768) x = -32768;
    if (x > 32767) x = 32767;
    u = (uint32_t)(x + 32768 + 8) >> 4;   /* round, not floor: no half-LSB DC in the loop */
    if (u > 4095u) u = 4095u;
    if (i & 1u) {
        p[b + 1] = (uint8_t)((p[b + 1] & 15u) | ((u & 15u) << 4));
        p[b + 2] = (uint8_t)(u >> 4);
    } else {
        p[b] = (uint8_t)u;
        p[b + 1] = (uint8_t)((p[b + 1] & 240u) | ((u >> 8) & 15u));
    }
}

/* Interpolated delay read, time in Q8 frames (96000*256 fits uint32).
 * Explicit ring wrap: 96000 is not a power of two. */
static int32_t d12_tap(const uint8_t *p, uint32_t wr, uint32_t delay_q8)
{
    uint32_t d = delay_q8 >> 8, frac = delay_q8 & 255u;
    uint32_t i = wr + DFRAMES - d, j, f = 0;
    int32_t a, b;
    if (frac) { i--; f = 256u - frac; }
    if (i >= DFRAMES) i -= DFRAMES;
    j = i + 1u;
    if (j == DFRAMES) j = 0;
    a = d12_read(p, i);
    if (i & 1u) b = d12_read(p, j);
    else {
        /* Even index: both nibbles of the same 3-byte group, one load path. */
        uint32_t base = (i >> 1) * 3u;
        uint32_t u = (uint32_t)(p[base + 1] >> 4) | ((uint32_t)p[base + 2] << 4);
        b = (int32_t)(u << 4) - 32768;
    }
    return a + (((b - a) * (int32_t)f) >> 8);
}

/* The sensor meter. The envelope lives in Q31 (1.1 gave it sub-count steps for the
 * 8 s tail) while the drone floor and the bar itself are Q15. Scaling the raw Q31
 * value by the bar height overflowed int32: the height wrapped between full scale
 * and negative numbers, so the rule beside the pad jumped up and down during every
 * release. Convert first, clamp, and only then scale. */
static int32_t env_bar(int32_t env_q31, int32_t hold_q15, int32_t max_h)
{
    int32_t e = env_q31 >> 16;
    if (e < hold_q15) e = hold_q15;
    if (e < 0) e = 0;
    if (e > 32767) e = 32767;
    return (e * max_h) >> 15;
}

/* DC blocker with a Q8 fractional state. The plain integer form used before kept
 * its state at whole counts, so the feedback rounded back to the same value and any
 * residual under ~960 counts could never decay: the output sat at a constant offset
 * after the tail had ended. With eight fractional bits the state always walks to zero. */
static int32_t dc_block(int32_t x, int32_t *state)
{
    int32_t target = x * 256, d = target - *state; /* multiplication is defined for negative x; signed << was not */
    /* step towards the target with a ceiling, never a floor: an arithmetic shift alone
     * stalls at state=-1 and leaves one count of DC on the output forever. */
    if (d > 0)      *state += (d + 255) >> 8;
    else if (d < 0) *state -= ((-d) + 255) >> 8;
    return x - ((*state + 128) >> 8);
}
#endif
