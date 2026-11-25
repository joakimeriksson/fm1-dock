/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Joakim Eriksson */
/* TRIO, an effect of fm1-dock (hb_module.h, "Effects"): a chorus, a flanger or a delay, one at a time, by its
 * first value, MODE. The three that are also effects of their own (modules/fxchorus, fxflanger, fxdelay), here in
 * one add-on and one buffer: only one of them runs.
 *
 *   CHORUS   each side through a line of 12 ms swept by up to 6 ms more, a quarter turn apart left and right
 *   FLANGER  the same line at 0.5 ms, swept by up to 5 ms more and fed back (55 %)
 *   DELAY    both sides' sum at half the sample rate, a lowpass before it, 30 to 500 ms, fed back up to 85 %
 *
 * Its other three values are RATE, DEPTH and MIX, or with the delay TIME, FEEDBACK and MIX: label() and show()
 * say which, and the time in ms. 21.5 KiB of the effect's RAM (the delay's line; the other two use its start).
 *
 * set() writes t_next and then t_new; run() takes it between two blocks, and empties the buffer when the mode
 * is another. Integers only. */
#include "hb_module.h"

enum { M_CHORUS, M_FLANGER, M_DELAY };
#define N 11025u                                /* the delay's line: 0.5 s at 22.05 kHz */
#define SW 2047u                                /* the swept lines: 2048 frames, L R, in the buffer's first 4096 */
typedef struct {
    uint32_t mode, inc, len;                    /* the sweep's phase a sample; the delay's length */
    int32_t base, depth, fb, wet, dry;          /* samples Q8; Q16 */
} coef_t;
static int16_t buf[N];
static coef_t c_run, c_next;
static volatile uint32_t t_new;
static uint32_t w, ph, odd, mode_was = ~0u;
static int32_t lp, y0, y1, val[4];

static inline int32_t sat(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }
static inline int32_t q16(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }
static uint32_t ms_of(int32_t v) { return 30u + 470u * (uint32_t)v / 100u; }

static void set(uint32_t k, int32_t v)
{
    coef_t *c = &c_next;
    uint32_t mhz;
    if (k < 4u) val[k] = v;
    c->mode = (uint32_t)val[0];
    c->wet = val[3] * 65536 / 100;
    c->dry = 65536 - c->wet / 4;
    if (c->mode == M_DELAY) {
        c->len = ms_of(val[1]) * 2205u / 100u;
        if (c->len > N - 1u) c->len = N - 1u;
        c->fb = val[2] * 55705 / 100;           /* up to 85 % */
    } else {
        mhz = c->mode == M_FLANGER ? 50u + 1950u * (uint32_t)val[1] / 100u : 100u + 2900u * (uint32_t)val[1] / 100u;
        c->inc = mhz * 97u + mhz * 39u / 100u;  /* Hz 2^32 / 44100 */
        c->base = (c->mode == M_FLANGER ? 22 : 530) << 8;
        c->depth = (int32_t)((c->mode == M_FLANGER ? 220u : 265u) * (uint32_t)val[2] / 100u) << 8;
        c->fb = c->mode == M_FLANGER ? 36045 : 0;
    }
    __asm__ volatile("" ::: "memory");         /* (the values stored before t_new) */
    t_new = 1;
}
static int init(void) { return 0; }

static void run(int32_t *o, uint32_t n)
{
    uint32_t i;
    int32_t fb, wet, dry;
    if (t_new) {
        c_run = c_next;
        t_new = 0;
    }
    if (c_run.mode != mode_was) {               /* another of the three: its line starts empty */
        mode_was = c_run.mode;
        for (i = 0; i < N; i++) buf[i] = 0;
        w = odd = 0;
        lp = y0 = y1 = 0;
    }
    fb = c_run.fb; wet = c_run.wet; dry = c_run.dry;
    if (c_run.mode == M_DELAY) {
        const uint32_t len = c_run.len;
        for (i = 0; i < n; i++) {
            int32_t l = o[2 * i], r = o[2 * i + 1], y;
            lp += q16(((l + r) >> 1) - lp, 39321);        /* about 6 kHz: the line runs at half the rate */
            if (!odd) {
                uint32_t rd = w >= len ? w - len : w + N - len;
                y0 = y1;
                y1 = buf[rd];
                buf[w] = (int16_t)sat(lp + q16(y1, fb));
                if (++w == N) w = 0;
                y = (y0 + y1) >> 1;
            } else {
                y = y1;
            }
            odd ^= 1u;
            o[2 * i] = q16(l, dry) + q16(y, wet);    /* (not clamped: the host does, after MASTER) */
            o[2 * i + 1] = q16(r, dry) + q16(y, wet);
        }
    } else {
        const int32_t base = c_run.base, depth = c_run.depth;
        const uint32_t inc = c_run.inc;
        for (i = 0; i < n; i++, w++) {
            uint32_t c;
            for (c = 0; c < 2u; c++) {          /* left, then right a quarter turn on */
                uint32_t p = ph + (c << 30), t = p >> 15, di, a;
                int32_t d, s0, s1, y, x = o[2 * i + c];
                if (t > 65535u) t = 131071u - t;
                d = base + (int32_t)(((int64_t)depth * (int32_t)t) >> 16);
                di = (uint32_t)d >> 8;
                a = (uint32_t)d & 255u;
                s0 = buf[(((w - di) & SW) << 1) | c];
                s1 = buf[(((w - di - 1u) & SW) << 1) | c];
                y = s0 + (((s1 - s0) * (int32_t)a) >> 8);
                buf[((w & SW) << 1) | c] = (int16_t)sat(x + q16(y, fb));
                o[2 * i + c] = q16(x, dry) + q16(y, wet);
            }
            ph += inc;
        }
    }
}

/* the page's words: what a value is called in this mode, and how it reads */
static const char *label(uint32_t k)
{
    static const char *const L[2][4] = {{"MODE", "RATE", "DEPTH", "MIX"}, {"MODE", "TIME", "FEEDBACK", "MIX"}};
    return k < 4u ? L[val[0] == M_DELAY][k] : 0;
}
static void put(char *o, uint32_t v, const char *unit)
{
    char t[6];
    uint32_t n = 0;
    do t[n++] = (char)('0' + v % 10u); while ((v /= 10u) != 0u);
    while (n) *o++ = t[--n];
    while (*unit) *o++ = *unit++;
    *o = 0;
}
static void show(uint32_t k, int32_t v, char *out)
{
    static const char *const MODE[3] = {"CHORUS", "FLANGER", "DELAY"};
    if (k == 0) {
        const char *s = MODE[v < 0 || v > 2 ? 0 : v];
        while (*s) *out++ = *s++;
        *out = 0;
    } else if (k == 1 && val[0] == M_DELAY) {
        put(out, ms_of(v), " ms");
    } else {
        put(out, (uint32_t)v, " %");
    }
}

static const hb_fx_param_t PARAMS[] = {
    {"MODE", "", 0, 2, 0, 1}, {"RATE", "%", 0, 100, 30, 1}, {"DEPTH", "%", 0, 100, 50, 1}, {"MIX", "%", 0, 100, 50, 1}};
HB_FX = {HB_FX_ABI, "TRIO", 4, PARAMS, init, set, run, label, show};
