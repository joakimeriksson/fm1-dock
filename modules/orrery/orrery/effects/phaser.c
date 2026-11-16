/* SPDX-License-Identifier: GPL-3.0-only */
/* Slow phaser: four first-order allpasses per channel whose break frequency a triangle
 * LFO sweeps between about 200 Hz and 4 kHz (depth scales the sweep around 1.4 kHz),
 * feedback from the chain's output to its input, and a mix where 100 % is the classic
 * half dry plus half allpassed. The right channel's LFO runs a quarter cycle ahead. */
#include "fx_common.h"
#include <string.h>

static const uint32_t rates[] = {MD_SAMPLE_RATE};
static const md_param_desc_t params[] = {
    {MD_PHASER_RATE, MD_PARAM_AUTOMATABLE, "Rate", 1, 500, 30, MD_UNIT_INTEGER, NULL, 0}, /* centi-Hz */
    {MD_PHASER_DEPTH, MD_PARAM_AUTOMATABLE | MD_PARAM_SMOOTH, "Depth", 0, 100, 70, MD_UNIT_PERCENT, NULL, 0},
    {MD_PHASER_FEEDBACK, MD_PARAM_AUTOMATABLE | MD_PARAM_SMOOTH, "Feedback", -90, 90, 40, MD_UNIT_PERCENT, NULL, 0},
    {MD_PHASER_MIX, MD_PARAM_AUTOMATABLE | MD_PARAM_SMOOTH, "Mix", 0, 100, 100, MD_UNIT_PERCENT, NULL, 0},
};
#define COUNT 4u
#define ONE (1 << 24)
#define CENTRE (548 * 256)   /* cutoff units Q8: 20 * 2^5.48 = 893 Hz ... sweep 200 Hz .. 4 kHz */
#define HALF_SPAN 216        /* cutoff units */
#define LAST_LIMIT (1 << 23) /* (not 1 << 22: -2^22 is 0xffc00000, which hbmod rejects as a chip-ROM address) */

/* allpass a = (g - 1) / (g + 1), Q24, from the LFO phase */
static int32_t coefficient(const md_fx_phaser_t *p, uint32_t lfo)
{
    uint32_t t = lfo < 0x80000000u ? lfo : ~lfo;          /* triangle 0 .. 2^31 - 1 */
    int32_t q15 = (int32_t)(t >> 15) - 32768;             /* -32768 .. 32767 */
    int32_t v = CENTRE + q15 * HALF_SPAN / 128 * p->depth_q8 / 25600;
    int32_t g = fx_tan_q24(v);
    int64_t num = (int64_t)(g - ONE) * ONE;
    uint32_t den = (uint32_t)(g + ONE);
    return num >= 0 ? (int32_t)fx_udiv64((uint64_t)num, den) : -(int32_t)fx_udiv64((uint64_t)(-num), den);
}
static void control(md_fx_phaser_t *p)
{
    p->fb_q15 = fx_smooth(p->fb_q15, p->feedback * 327, 5);
    p->mix_q15 = fx_smooth(p->mix_q15, p->mix * 327, 5);
    p->depth_q8 = fx_smooth(p->depth_q8, p->depth * 256, 5);
    p->a[0] = coefficient(p, p->lfo);
    p->a[1] = coefficient(p, p->lfo + 0x40000000u);
}
static void snap(md_fx_phaser_t *p)
{
    p->inc = (uint32_t)p->rate * 974u;                   /* 2^32 / (100 * 44100) = 973.9 */
    p->fb_q15 = p->feedback * 327;
    p->mix_q15 = p->mix * 327;
    p->depth_q8 = p->depth * 256;
    control(p);
}
static int init(void *state, uint32_t rate)
{
    (void)rate;
    md_fx_phaser_t *p = state;
    memset(p, 0, sizeof *p);
    p->rate = 30; p->depth = 70; p->feedback = 40; p->mix = 100;
    snap(p);
    return MD_OK;
}
static int set_param(void *state, md_param_id_t id, int32_t v)
{
    md_fx_phaser_t *p = state;
    if (id == MD_PHASER_RATE) { p->rate = v; p->inc = (uint32_t)v * 974u; }
    else if (id == MD_PHASER_DEPTH) p->depth = v;
    else if (id == MD_PHASER_FEEDBACK) p->feedback = v;
    else if (id == MD_PHASER_MIX) p->mix = v;
    else return MD_ERR_NOT_FOUND;
    return MD_OK;
}
static int get_param(const void *state, md_param_id_t id, int32_t *v)
{
    const md_fx_phaser_t *p = state;
    if (id == MD_PHASER_RATE) *v = p->rate;
    else if (id == MD_PHASER_DEPTH) *v = p->depth;
    else if (id == MD_PHASER_FEEDBACK) *v = p->feedback;
    else if (id == MD_PHASER_MIX) *v = p->mix;
    else return MD_ERR_NOT_FOUND;
    return MD_OK;
}
static void process(void *state, int32_t *x, uint32_t frames)
{
    md_fx_phaser_t *p = state;
    for (uint32_t i = 0; i < frames; i++) {
        if (p->phase == 0) control(p);
        p->phase = (p->phase + 1u) & (MD_FX_CONTROL - 1u);
        p->lfo += p->inc;
        for (uint32_t c = 0; c < 2; c++) {
            int32_t dry = fx_in(x[2 * i + c]);
            int32_t u = dry + fx_mulz(p->last[c], p->fb_q15, 15);
            for (uint32_t s = 0; s < MD_PHASER_STAGES; s++) {
                int32_t y = fx_mulz(p->a[c], u - p->y1[c][s], 24) + p->x1[c][s];
                p->x1[c][s] = u;
                p->y1[c][s] = y;
                u = y;
            }
            p->last[c] = fx_clamp(u, -LAST_LIMIT, LAST_LIMIT);
            x[2 * i + c] = dry + fx_mulz(p->last[c] - dry, p->mix_q15 >> 1, 15);
        }
    }
}
static void reset(void *state)
{
    md_fx_phaser_t *p = state;
    memset(p->x1, 0, sizeof p->x1);
    memset(p->y1, 0, sizeof p->y1);
    memset(p->last, 0, sizeof p->last);
    p->lfo = 0;
    p->phase = 0;
    control(p);
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
    snap(state);
    return MD_OK;
}
const md_effect_desc_t MD_FX_PHASER = {
    .processor = {.id = MD_FX_PHASER_ID, .api_version = MD_API_VERSION, .settings_version = 1, .name = "PHASER",
                  .state_bytes = sizeof(md_fx_phaser_t), .state_alignment = _Alignof(md_fx_phaser_t),
                  .sample_rates = rates, .sample_rate_count = 1, .param_count = COUNT, .params = params,
                  .max_settings_bytes = 2u * COUNT},
    .capabilities = MD_EFFECT_INSERT, .init = init, .set_param = set_param, .get_param = get_param,
    .process_replace = process, .reset = reset, .encode = encode, .validate = validate, .apply = apply,
};
