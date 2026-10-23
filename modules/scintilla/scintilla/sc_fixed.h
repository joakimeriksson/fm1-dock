/* SPDX-License-Identifier: GPL-3.0-only */
/* Fixed-point helpers. The FM-1 (JieLi AC791N, pi32v2) has no FPU and no libgcc: no float, no
 * 64-bit division. 64-bit multiplication and shifts are fine.
 *
 * Formats used across the engine:
 *   audio      Q20 in int32 (1.0 = 2^20): headroom for a resonant 24 dB filter (peaks of ~150)
 *   phase      uint32, 2^32 = one cycle; an increment per sample is the frequency
 *   pitch      MIDI note number in Q16
 *   envelopes  Q30 (1.0 = 2^30)
 *   params     Q16, as sc_patch.h */
#ifndef SC_FIXED_H
#define SC_FIXED_H
#include <stdint.h>

#define SC_FS     44100
#define SC_ONE16  65536
#define SC_ONE20  (1 << 20)
#define SC_ONE30  (1 << 30)

static inline int32_t sc_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int32_t sc_mul(int32_t a, int32_t b, int sh) { return (int32_t)(((int64_t)a * b) >> sh); }

/* floor(n / d) for a quotient that fits 32 bits (n < d * 2^32), by restoring division: no
 * __udivdi3. Only at note and parameter events, never per sample. */
static inline uint32_t sc_udiv64(uint64_t n, uint32_t d)
{
    uint64_t r = n >> 32;
    uint32_t lo = (uint32_t)n, q = 0;
    if (d == 0) return 0xFFFFFFFFu;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((lo >> i) & 1u);
        q <<= 1;
        if (r >= d) { r -= d; q |= 1u; }
    }
    return q;
}

/* seconds (Q16) -> samples (Q8) at SC_FS */
static inline uint32_t sc_samples_q8(int32_t t_q16)
{
    if (t_q16 <= 0) return 0;
    return (uint32_t)(((uint64_t)(uint32_t)t_q16 * SC_FS) >> 8);
}

/* e^(-1/N) in Q30 for N samples (Q8), N >= ~4: a Taylor series in x = 1/N, error < 1e-5 there */
static inline int32_t sc_exp_neg_inv(uint32_t n_q8)
{
    if (n_q8 < 256) n_q8 = 256;
    /* every power is in [0, 2^30): divide them as 32-bit values (a 64-bit one is __udivdi3) */
    uint32_t x = sc_udiv64((uint64_t)1 << 38, n_q8);         /* 1/N, Q30 */
    uint32_t x2 = (uint32_t)(((uint64_t)x * x) >> 30), x3 = (uint32_t)(((uint64_t)x2 * x) >> 30);
    uint32_t x4 = (uint32_t)(((uint64_t)x3 * x) >> 30), x5 = (uint32_t)(((uint64_t)x4 * x) >> 30);
    return (int32_t)(SC_ONE30 - x + x2 / 2 - x3 / 6 + x4 / 24 - x5 / 120);
}

/* xorshift32, bit for bit the generator synthex-web uses (WhiteNoise, Lfo) */
static inline uint32_t sc_xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
/* ... as audio: (int32)x / 2^31, in Q20 */
static inline int32_t sc_rand20(uint32_t *s) { return (int32_t)sc_xorshift(s) >> 11; }

#endif
