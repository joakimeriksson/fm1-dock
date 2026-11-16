/* SPDX-License-Identifier: GPL-3.0-only */
/* Reverb bus. It began as a port of fm1-harbor firmware/src/hb_fx.c (GPL-3.0-only; itself
 * Scintilla's eight-line reverb, joakimeriksson/scintilla-fm1 engine/sc_synth.c, at half
 * the lines) and keeps its lines (1051, 1327, 1613, 1907 samples) in a Hadamard loop, its
 * input taps and signs, its decay law per algorithm and size, and its level law. Measured
 * as it was, a long hall rang on some sixty fixed pitches between 200 and 1200 Hz with
 * 50 dB less between them (134 ms of line for four seconds of decay). So, in this one:
 *  - the loop runs at half the sample rate (a frame and its neighbour averaged in, the
 *    output drawn between two results): the lines hold 268 ms, and it costs less;
 *  - lines 1 and 3 are read at a place that moves a third of a millisecond, slowly (0.31
 *    and 0.47 Hz, two cents of pitch at most): the resonances drift, the tail is a wash;
 *  - the input goes through two allpasses a channel first (5 .. 9 ms): a drop or a drum
 *    reaches the lines as a cluster;
 *  - a lowpass in the loop: the top of the tail dies first (Damping), the output's own
 *    lowpass above it;
 *  - any span length gives the same output (the pairing is kept across calls), and
 *    everything rounds toward zero, so a tail ends in exact silence. */
#include "fx_common.h"
#include <string.h>

static const uint32_t rates[] = {MD_SAMPLE_RATE};
static const char *const algorithms[] = {"ROOM", "PLATE", "HALL", "CATHEDRAL"};
static const md_param_desc_t params[] = {
    {MD_REVERB_ALGORITHM, MD_PARAM_AUTOMATABLE | MD_PARAM_ENUM, "Type", 0, 3, 2, MD_UNIT_INTEGER, algorithms, 4},
    {MD_REVERB_SIZE, MD_PARAM_AUTOMATABLE, "Size", 0, 100, 50, MD_UNIT_PERCENT, NULL, 0},
    {MD_REVERB_DAMPING, MD_PARAM_AUTOMATABLE, "Damping", 0, 100, 40, MD_UNIT_PERCENT, NULL, 0},
};
#define COUNT 3u
#define MASK (MD_REVERB_LEN * MD_REVERB_LINES - 1u)
#define SH 7 /* lines hold Q13 (Q20 >> 7) */
#define Q16(x) ((int32_t)((x) * 65536.0 + 0.5))
#define ONE16 65536
static const uint16_t D[MD_REVERB_LINES] = {1051, 1327, 1613, 1907};
/* input taps P = 67, 307, 547, 887 and signs +L +R -L -R are written out in process() */
static const int32_t BASE[4] = {Q16(0.6), Q16(0.3), Q16(1.2), Q16(3.0)};
static const int32_t MAXS[4] = {Q16(2.5), Q16(1.4), Q16(4.5), Q16(9.0)};
static const int32_t BRIGHT[4] = {Q16(0.85), Q16(0.65), Q16(0.55), Q16(0.35)};
static const uint32_t EXP2[65] = { /* 2^(i / 64), Q16 */
    65536u, 66250u, 66971u, 67700u, 68438u, 69183u, 69936u, 70698u, 71468u, 72246u,
    73032u, 73828u, 74632u, 75444u, 76266u, 77096u, 77936u, 78785u, 79642u, 80510u,
    81386u, 82273u, 83169u, 84074u, 84990u, 85915u, 86851u, 87796u, 88752u, 89719u,
    90696u, 91684u, 92682u, 93691u, 94711u, 95743u, 96785u, 97839u, 98905u, 99982u,
    101070u, 102171u, 103283u, 104408u, 105545u, 106694u, 107856u, 109031u, 110218u, 111418u,
    112631u, 113858u, 115098u, 116351u, 117618u, 118899u, 120194u, 121502u, 122825u, 124163u,
    125515u, 126882u, 128263u, 129660u, 131072u,
};
static int32_t exp2_q16(int32_t y) /* 2^(y / 65536), Q16; y <= 0 */
{
    int32_t ip = y >> 16;
    uint32_t f = (uint32_t)y & 0xffffu, i = f >> 10, fr = f & 1023u;
    uint32_t v = EXP2[i] + (uint32_t)(((uint64_t)(EXP2[i + 1] - EXP2[i]) * fr) >> 10);
    return ip >= 0 ? (int32_t)(v << ip) : ip < -30 ? 0 : (int32_t)(v >> -ip);
}
static uint32_t isqrt64(uint64_t x)
{
    uint64_t r = 0, b = (uint64_t)1 << 62;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; } else r >>= 1;
        b >>= 2;
    }
    return (uint32_t)r;
}
/* hb_fx.c fx_prep at mix 100 % */
static void prepare(md_fx_reverb_t *r)
{
    int32_t sz = r->size * 65536 / 100, dp = r->damping * 65536 / 100;
    int al = (int)r->algorithm;
    int32_t sec = BASE[al] + fx_mul(sz, MAXS[al] - BASE[al], 16);
    uint32_t len = (uint32_t)(((uint64_t)(uint32_t)sec * 22050u) >> 16);   /* the decay time in the loop's samples */
    uint32_t len44 = 2u * len;
    for (uint32_t i = 0; i < MD_REVERB_LINES; i++) {
        uint32_t y = fx_udiv64((uint64_t)653118u * D[i], len); /* log2(1000) d / N, Q16 */
        r->g[i] = (int32_t)(((int64_t)exp2_q16(-(int32_t)y) * 536870912) >> 16); /* x 1/2, Q30 */
    }
    r->loop = (Q16(0.975) - dp / 2 + 1024) >> 11;   /* the loop's lowpass, in thirty-seconds: the top's decay, 31 (hardly) .. 15 */
    int32_t fc = fx_mul(fx_mul(20000, ONE16 - dp, 16), BRIGHT[al], 16) + 200;
    r->a = ONE16 - exp2_q16(-(int32_t)(((int64_t)fc * 882827) >> 16));
    uint32_t ratio = fx_udiv64((uint64_t)(uint32_t)(2 * ONE16 - r->a) << 16, (uint32_t)r->a);
    uint32_t early = fx_udiv64((uint64_t)len44 << 32, len44 + 12286u);
    r->wet = fx_mul(fx_mul(8400, (int32_t)isqrt64((uint64_t)ratio << 16), 16), (int32_t)isqrt64(early), 16);
}
static int init(void *state, uint32_t rate)
{
    (void)rate;
    md_fx_reverb_t *r = state;
    memset(r, 0, sizeof *r);
    r->lfo2 = 0x40000000u;                       /* a quarter apart */
    r->algorithm = MD_REVERB_HALL; r->size = 50; r->damping = 40;
    prepare(r);
    return MD_OK;
}
static int set_param(void *state, md_param_id_t id, int32_t v)
{
    md_fx_reverb_t *r = state;
    if (id == MD_REVERB_ALGORITHM) r->algorithm = v;
    else if (id == MD_REVERB_SIZE) r->size = v;
    else if (id == MD_REVERB_DAMPING) r->damping = v;
    else return MD_ERR_NOT_FOUND;
    prepare(r);
    return MD_OK;
}
static int get_param(const void *state, md_param_id_t id, int32_t *v)
{
    const md_fx_reverb_t *r = state;
    if (id == MD_REVERB_ALGORITHM) *v = r->algorithm;
    else if (id == MD_REVERB_SIZE) *v = r->size;
    else if (id == MD_REVERB_DAMPING) *v = r->damping;
    else return MD_ERR_NOT_FOUND;
    return MD_OK;
}
/* Branch-free rounding toward zero (so a tail decays to exact silence instead of a -1 limit cycle). */
MD_INLINE int32_t shr0_32(int32_t v, int sh) { return (v + ((v >> 31) & ((1 << sh) - 1))) >> sh; }
MD_INLINE int32_t shr0_64(int64_t p, int sh) { return (int32_t)((p + ((p >> 63) & (((int64_t)1 << sh) - 1))) >> sh); }
MD_INLINE int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v); }
/* line k's slot that an input entering now reaches (D - P samples behind the write) and its tap (D behind) */
#define IN_AT(k) ((t4 + (k) + 4u * (MD_REVERB_LEN + P_##k - D_##k)) & MASK)
#define TAP(k) ((int32_t)b[(t4 + (k) + 4u * (MD_REVERB_LEN - D_##k)) & MASK] * (1 << SH))
#define D_0 1051u
#define D_1 1327u
#define D_2 1613u
#define D_3 1907u
#define P_0 67u
#define P_1 307u
#define P_2 547u
#define P_3 887u
#define LP_X 4 /* the lowpasses keep 4 more bits than the lines, so plain shifts settle */
/* the input allpasses' lengths (half-rate samples: 5.1, 7.8 ms left; 5.8, 8.7 ms right) and gain 0.55 */
#define AP_0 113u
#define AP_1 173u
#define AP_2 127u
#define AP_3 191u
#define AP_G 141                        /* of 256 */
/* a moved tap: `off` (Q8 samples, 0 .. 2 x depth) further back than D - depth, between two slots of line k */
#define MOD_DEPTH_1 (6 * 256)
#define MOD_DEPTH_3 (8 * 256)
#define LFO_1 133083u                   /* 0.31 Hz and 0.47 Hz at 22050 ticks a second */
#define LFO_2 201771u
MD_INLINE int32_t lfo(uint32_t phase)   /* a parabola a half: 0 .. 65536, smooth where it turns */
{
    int32_t t = (int32_t)phase >> 16, h = t < 0 ? -t : t;
    int32_t y = (h * (32768 - h)) >> 13;            /* 0 .. 32768 */
    return 32768 + (t < 0 ? -y : y);
}
MD_INLINE int32_t moved(const int16_t *b, uint32_t t4, uint32_t k, uint32_t d, int32_t depth, int32_t wave)
{
    uint32_t off = (uint32_t)((depth * wave) >> 15);            /* 0 .. 2 x depth, Q8 */
    uint32_t at = t4 + k + 4u * (MD_REVERB_LEN - d + (uint32_t)(depth >> 8) - (off >> 8));
    int32_t fr = (int32_t)(off & 255u);
    int32_t near = b[at & MASK], far = b[(at - 4u) & MASK];
    return (near * (256 - fr) + far * fr) / 2;                   /* Q13 x 256 -> Q20 */
}
MD_INLINE int32_t allpass(int16_t *line, uint32_t at, int32_t x)   /* one sample through an allpass (Q13) */
{
    int32_t d = line[at];
    int32_t y = d - shr0_32(x * AP_G, 8);       /* (x within 2^17, y within 2^18: 32 bits hold the products) */
    line[at] = sat16(x + shr0_32(y * AP_G, 8));
    return y;
}
/* the loop, n half-rate frames: tick[0], tick[1] hold the input (Q13) and take the output (Q20) */
MD_HOT static void loop(md_fx_reverb_t *r, uint32_t n)
{
    int16_t *b = r->line;
    int32_t *tl = r->tick[0], *tr = r->tick[1];
    const int32_t g0 = r->g[0], g1 = r->g[1], g2 = r->g[2], g3 = r->g[3], k = r->loop, level = r->wet;
    int32_t f0 = r->lp[0], f1 = r->lp[1], f2 = r->lp[2], f3 = r->lp[3];
    uint32_t w = r->w, p1 = r->lfo1, p2 = r->lfo2;
    uint32_t i0 = r->at[0], i1 = r->at[1], i2 = r->at[2], i3 = r->at[3];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t4 = 4u * w;
        int32_t ql = allpass(r->ap[1], i1, allpass(r->ap[0], i0, tl[i]));
        int32_t qr = allpass(r->ap[3], i3, allpass(r->ap[2], i2, tr[i]));
        if (++i0 == AP_0) i0 = 0;
        if (++i1 == AP_1) i1 = 0;
        if (++i2 == AP_2) i2 = 0;
        if (++i3 == AP_3) i3 = 0;
        b[IN_AT(0)] = sat16(b[IN_AT(0)] + ql);                   /* the signs: +L +R -L -R (hb_fx.c FX_S) */
        b[IN_AT(1)] = sat16(b[IN_AT(1)] + qr);
        b[IN_AT(2)] = sat16(b[IN_AT(2)] - ql);
        b[IN_AT(3)] = sat16(b[IN_AT(3)] - qr);
        int32_t y0 = TAP(0), y1 = moved(b, t4, 1u, D_1, MOD_DEPTH_1, lfo(p1));
        int32_t y2 = TAP(2), y3 = moved(b, t4, 3u, D_3, MOD_DEPTH_3, lfo(p2));
        int32_t a0 = y0 + y1, a1 = y0 - y1, a2 = y2 + y3, a3 = y2 - y3;
        int32_t h0 = a0 + a2, h1 = a1 + a3;
        /* the loop's lowpasses, a thirty-second a step (Q20, within 2^25: 32 bits; what they leave unsettled is under
         * a line's own step, so the tail still ends in silence) */
        f0 += ((h0 - f0) * k) >> 5;
        f1 += ((h1 - f1) * k) >> 5;
        f2 += ((a0 - a2 - f2) * k) >> 5;
        f3 += ((a1 - a3 - f3) * k) >> 5;
        b[(t4 + 0u) & MASK] = sat16(shr0_64((int64_t)f0 * g0, 30 + SH));
        b[(t4 + 1u) & MASK] = sat16(shr0_64((int64_t)f1 * g1, 30 + SH));
        b[(t4 + 2u) & MASK] = sat16(shr0_64((int64_t)f2 * g2, 30 + SH));
        b[(t4 + 3u) & MASK] = sat16(shr0_64((int64_t)f3 * g3, 30 + SH));
        tl[i] = shr0_64((int64_t)h0 * level, 21 - LP_X);        /* the level here, half as often: Q20 x Q16 -> Q15 and LP_X */
        tr[i] = shr0_64((int64_t)h1 * level, 21 - LP_X);
        w = (w + 1u) & (MD_REVERB_LEN - 1u); p1 += LFO_1; p2 += LFO_2;
    }
    r->lp[0] = f0; r->lp[1] = f1; r->lp[2] = f2; r->lp[3] = f3;
    r->w = w; r->at[0] = (uint16_t)i0; r->at[1] = (uint16_t)i1; r->at[2] = (uint16_t)i2; r->at[3] = (uint16_t)i3;
    r->lfo1 = p1; r->lfo2 = p2;
}
/* (in the flash: the loop is the dear half and the 16 KiB of RAM hold dearer loops than these two) */
static void span(md_fx_reverb_t *r, const int32_t *in, int32_t *wet, uint32_t frames)
{
    int32_t *tl = r->tick[0], *tr = r->tick[1];
    const int32_t a = r->a;
    int32_t hl = r->hold_l, hr = r->hold_r;
    uint32_t odd = r->odd, n = 0, i;
    for (i = 0; i < frames; i++) {               /* in: a frame and the one after it, averaged (Q15 -> Q13) */
        int32_t xl = in[2 * i], xr = in[2 * i + 1];
        xl = xl > (1 << 18) ? (1 << 18) : xl < -(1 << 18) ? -(1 << 18) : xl;
        xr = xr > (1 << 18) ? (1 << 18) : xr < -(1 << 18) ? -(1 << 18) : xr;
        if ((odd + i) & 1u) { tl[n] = shr0_32(hl + xl, 3); tr[n] = shr0_32(hr + xr, 3); n++; }
        else { hl = xl; hr = xr; }
    }
    r->hold_l = hl; r->hold_r = hr;
    if (n) MD_FAR(loop)(r, n);
    {   /* out: between the last two results on the frame that waits, the newer on the frame that runs the loop */
        int32_t pl = r->prev_l, pr = r->prev_r, cl = r->cur_l, cr = r->cur_r, lpl = r->lpl, lpr = r->lpr;
        for (i = 0, n = 0; i < frames; i++) {
            int32_t ol, or_;
            if ((odd + i) & 1u) { ol = cl; or_ = cr; pl = cl; pr = cr; cl = tl[n]; cr = tr[n]; n++; }
            else { ol = (pl + cl) / 2; or_ = (pr + cr) / 2; }
            lpl += (int32_t)(((int64_t)(ol - lpl) * a) >> 16);
            lpr += (int32_t)(((int64_t)(or_ - lpr) * a) >> 16);
            wet[2 * i] = shr0_32(lpl, LP_X);
            wet[2 * i + 1] = shr0_32(lpr, LP_X);
        }
        r->prev_l = pl; r->prev_r = pr; r->cur_l = cl; r->cur_r = cr; r->lpl = lpl; r->lpr = lpr;
    }
    r->odd = (odd + frames) & 1u;
}
static void process(void *state, const int32_t *in, int32_t *wet, uint32_t frames)
{
    md_fx_reverb_t *r = state;
    while (frames) {
        uint32_t n = frames > MD_REVERB_SPAN ? MD_REVERB_SPAN : frames;
        span(r, in, wet, n);
        in += 2u * n; wet += 2u * n; frames -= n;
    }
}
static void reset(void *state)
{
    md_fx_reverb_t *r = state;
    memset(r->line, 0, sizeof r->line);
    memset(r->ap, 0, sizeof r->ap);
    memset(r->lp, 0, sizeof r->lp);
    r->lpl = r->lpr = r->hold_l = r->hold_r = r->prev_l = r->prev_r = r->cur_l = r->cur_r = 0;
    r->w = r->odd = r->lfo1 = 0;
    memset(r->at, 0, sizeof r->at);
    r->lfo2 = 0x40000000u;                       /* a quarter apart */
}
static int encode(const void *state, uint8_t *dst, uint32_t capacity)
{
    return fx_settings_encode(params, COUNT, state, get_param, dst, capacity);
}
static int validate(const uint8_t *src, uint32_t bytes) { return fx_settings_validate(params, COUNT, src, bytes); }
static int apply(void *state, const uint8_t *src, uint32_t bytes)
{
    int status = validate(src, bytes);
    if (status) return status;
    for (uint32_t i = 0; i < COUNT; i++) (void)set_param(state, params[i].id, fx_settings_value(src, i));
    return MD_OK;
}
const md_effect_desc_t MD_FX_REVERB = {
    .processor = {.id = MD_FX_REVERB_ID, .api_version = MD_API_VERSION, .settings_version = 1, .name = "REVERB",
                  .state_bytes = sizeof(md_fx_reverb_t), .state_alignment = _Alignof(md_fx_reverb_t),
                  .sample_rates = rates, .sample_rate_count = 1, .param_count = COUNT, .params = params,
                  .max_settings_bytes = 2u * COUNT},
    .capabilities = MD_EFFECT_BUS, .init = init, .set_param = set_param, .get_param = get_param,
    .process_wet = process, .reset = reset, .encode = encode, .validate = validate, .apply = apply,
};
