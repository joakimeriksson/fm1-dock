/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Joakim Eriksson */
/* A swept delay line for an effect (hb_module.h, "Effects"): two lines, left and right, read at a delay that a
 * slow triangle moves, a quarter turn apart left and right, with linear interpolation between samples, and fed
 * back into themselves if asked. The CHORUS (some 12 ms, no feedback) and the FLANGER (0.5 ms and up, fed back)
 * are this with other numbers. The file that includes it says SW_FRAMES (a power of two: the lines' length).
 *
 * set() writes sw_next and then sw_new; run() takes it between two blocks. Integers only. */
#include <stdint.h>

typedef struct {
    uint32_t inc;                               /* the sweep's phase a sample (2^32 a turn) */
    int32_t base, depth;                        /* the delay and its sweep, samples Q8 */
    int32_t fb, wet, dry;                       /* Q16 */
} sw_coef_t;
static int16_t sw_buf[2 * SW_FRAMES];           /* frame j's left and right at 2 j, 2 j + 1 */
static sw_coef_t sw_c, sw_next;
static volatile uint32_t sw_new;
static uint32_t sw_w, sw_ph;

static inline int32_t sw_sat(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }
static inline int32_t sw_q16(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }
/* a rate in mHz as the phase a sample: Hz 2^32 / 44100 */
static inline uint32_t sw_inc(uint32_t mhz) { return mhz * 97u + mhz * 39u / 100u; }

static void sw_run(int32_t *o, uint32_t n)
{
    int16_t *b = sw_buf;
    uint32_t i, w = sw_w, ph = sw_ph, inc;
    int32_t base, depth, fb, wet, dry;
    if (sw_new) {
        sw_c = sw_next;
        sw_new = 0;
    }
    inc = sw_c.inc; base = sw_c.base; depth = sw_c.depth; fb = sw_c.fb; wet = sw_c.wet; dry = sw_c.dry;
    for (i = 0; i < n; i++, w++) {
        uint32_t c;
        for (c = 0; c < 2u; c++) {              /* left, then right a quarter turn on */
            uint32_t p = ph + (c << 30), t = p >> 15, di, a;
            int32_t d, s0, s1, y, x = o[2 * i + c];
            if (t > 65535u) t = 131071u - t;    /* the triangle, 0..65535 */
            d = base + (int32_t)(((int64_t)depth * (int32_t)t) >> 16);
            di = (uint32_t)d >> 8;
            a = (uint32_t)d & 255u;
            s0 = b[(((w - di) & (SW_FRAMES - 1u)) << 1) | c];
            s1 = b[(((w - di - 1u) & (SW_FRAMES - 1u)) << 1) | c];
            y = s0 + (((s1 - s0) * (int32_t)a) >> 8);
            b[((w & (SW_FRAMES - 1u)) << 1) | c] = (int16_t)sw_sat(x + sw_q16(y, fb));
            o[2 * i + c] = sw_sat(sw_q16(x, dry) + sw_q16(y, wet));
        }
        ph += inc;
    }
    sw_w = w;
    sw_ph = ph;
}
