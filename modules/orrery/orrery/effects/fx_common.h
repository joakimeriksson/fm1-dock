/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_FX_COMMON_H
#define MD_FX_COMMON_H
#include "effects.h"
#include "fx_tables.h"

MD_INLINE int32_t fx_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
MD_INLINE int32_t fx_in(int32_t v) { return fx_clamp(v, -MD_FX_INPUT_LIMIT, MD_FX_INPUT_LIMIT); }
/* Multiply then arithmetic shift, as the engines' MUL. */
MD_INLINE int32_t fx_mul(int32_t a, int32_t b, int sh) { return (int32_t)(((int64_t)a * b) >> sh); }
/* Same, rounding toward zero, so integer decays reach exactly zero (no -1 limit cycle). */
MD_INLINE int32_t fx_mulz(int32_t a, int32_t b, int sh)
{
    int64_t p = (int64_t)a * b;
    return (int32_t)(p >= 0 ? p >> sh : -((-p) >> sh));
}
/* One-pole step current += (target - current) * k (k Q15/Q16 by sh), with the increment
 * rounded away from zero but never past the target, so it lands exactly on target. */
MD_INLINE int32_t fx_step(int32_t current, int32_t target, int32_t k, int sh)
{
    int64_t d = (int64_t)target - current;
    if (!d) return current;
    int64_t m = d < 0 ? -d : d;
    int64_t s = (m * k + (((int64_t)1 << sh) - 1)) >> sh;
    if (s > m) s = m;
    return (int32_t)(current + (d < 0 ? -s : s));
}
/* Symmetric one-pole step toward target; snaps when the step rounds to zero. */
MD_INLINE int32_t fx_smooth(int32_t current, int32_t target, int shift)
{
    int32_t d = target - current;
    int32_t step = d >= 0 ? d >> shift : -((-d) >> shift);
    return step ? current + step : target;
}
/* floor(n / d) for a quotient that fits 32 bits, without a 64-bit division helper
 * (restoring division, as fm1-harbor's hb_fx.c). Control rate only. */
static inline uint32_t fx_udiv64(uint64_t n, uint32_t d)
{
    uint64_t r = n >> 32;
    uint32_t lo = (uint32_t)n, q = 0;
    if (!d) return 0xffffffffu;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((lo >> i) & 1u);
        q <<= 1;
        if (r >= d) { r -= d; q |= 1u; }
    }
    return q;
}
/* tan(pi f / fs) Q24 for cutoff units Q8 (f = 20 Hz * 2^(v / 100)), linear between entries. */
static inline int32_t fx_tan_q24(int32_t v_q8)
{
    if (v_q8 < 0) v_q8 = 0;
    if (v_q8 >= 1000 * 256) return MD_FX_TAN_Q24[100];
    uint32_t pos = (uint32_t)v_q8 / 10u;   /* Q8 index in table steps */
    uint32_t i = pos >> 8, fr = pos & 255u;
    return MD_FX_TAN_Q24[i] + (int32_t)(((int64_t)(MD_FX_TAN_Q24[i + 1] - MD_FX_TAN_Q24[i]) * fr) >> 8);
}

/* Generic settings: every parameter as little-endian int16 in descriptor order. */
static inline int fx_settings_validate(const md_param_desc_t *p, uint32_t count, const uint8_t *src, uint32_t bytes)
{
    if (bytes != 2u * count) return MD_ERR_FORMAT;
    if (!src) return MD_ERR_ARGUMENT;
    for (uint32_t i = 0; i < count; i++) {
        int32_t v = (int16_t)(uint16_t)(src[2 * i] | (src[2 * i + 1] << 8));
        if (v < p[i].min || v > p[i].max) return MD_ERR_RANGE;
    }
    return MD_OK;
}
static inline int fx_settings_encode(const md_param_desc_t *p, uint32_t count, const void *state,
                                     int (*get)(const void *, md_param_id_t, int32_t *),
                                     uint8_t *dst, uint32_t capacity)
{
    if (capacity < 2u * count) return MD_ERR_CAPACITY;
    for (uint32_t i = 0; i < count; i++) {
        int32_t v = 0;
        int status = get(state, p[i].id, &v);
        if (status) return status;
        dst[2 * i] = (uint8_t)((uint32_t)v & 255u);
        dst[2 * i + 1] = (uint8_t)(((uint32_t)v >> 8) & 255u);
    }
    return (int)(2u * count);
}
static inline int32_t fx_settings_value(const uint8_t *src, uint32_t i)
{
    return (int16_t)(uint16_t)(src[2 * i] | (src[2 * i + 1] << 8));
}
#endif
