/* SPDX-License-Identifier: GPL-3.0-only */
/* Resonant low-pass / band-pass / high-pass: a trapezoidal state-variable filter (the
 * zero-delay form after A. Simper's "Linear Trapezoidal Integrated SVF"), stable for
 * every coefficient it can be given. Coefficients are Q24 and recomputed every
 * MD_FX_CONTROL samples from smoothed cutoff and resonance; band-pass is normalised to
 * unity peak. Resonance 0..100 maps k = 2 .. 0.1 (Q 0.5 .. 10). */
#include "fx_common.h"
#include <string.h>

static const uint32_t rates[] = {MD_SAMPLE_RATE};
static const char *const types[] = {"LOWPASS", "BANDPASS", "HIGHPASS"};
static const md_param_desc_t params[] = {
    {MD_FILTER_TYPE, MD_PARAM_AUTOMATABLE | MD_PARAM_ENUM, "Type", 0, 2, 0, MD_UNIT_INTEGER, types, 3},
    {MD_FILTER_CUTOFF, MD_PARAM_AUTOMATABLE | MD_PARAM_SMOOTH, "Cutoff", 0, 990, 990, MD_UNIT_INTEGER, NULL, 0},
    {MD_FILTER_RESONANCE, MD_PARAM_AUTOMATABLE | MD_PARAM_SMOOTH, "Resonance", 0, 100, 0, MD_UNIT_PERCENT, NULL, 0},
};
#define COUNT 3u
#define ONE (1 << 24)
#define SCALE 256
#define INPUT_LIMIT (1 << 16)   /* +6 dB over full scale; the mix saturates there anyway */
#define STATE_LIMIT (1 << 29)   /* input 2^24 scaled, x10 resonance peak, with margin */

static void coefficients(md_fx_filter_t *f)
{
    int32_t g = fx_tan_q24(f->cut_q8);
    f->k = 2 * ONE - f->res_q8 * 1245;                  /* 1.9 / (100 * 256) in Q24 */
    int64_t d = ONE + (((int64_t)g * (g + f->k)) >> 24);  /* 1 + g (g + k), Q24, >= 1 */
    f->a1 = (int32_t)fx_udiv64((uint64_t)1 << 48, (uint32_t)d);
    f->a2 = fx_mul(g, f->a1, 24);
    f->a3 = fx_mul(g, f->a2, 24);
}
static void snap(md_fx_filter_t *f)
{
    f->cut_q8 = f->cutoff * 256;
    f->res_q8 = f->resonance * 256;
    coefficients(f);
}
static int init(void *state, uint32_t rate)
{
    (void)rate;
    md_fx_filter_t *f = state;
    memset(f, 0, sizeof *f);
    f->cutoff = 990;
    snap(f);
    return MD_OK;
}
static int set_param(void *state, md_param_id_t id, int32_t v)
{
    md_fx_filter_t *f = state;
    if (id == MD_FILTER_TYPE) f->type = v;
    else if (id == MD_FILTER_CUTOFF) f->cutoff = v;
    else if (id == MD_FILTER_RESONANCE) f->resonance = v;
    else return MD_ERR_NOT_FOUND;
    return MD_OK;
}
static int get_param(const void *state, md_param_id_t id, int32_t *v)
{
    const md_fx_filter_t *f = state;
    if (id == MD_FILTER_TYPE) *v = f->type;
    else if (id == MD_FILTER_CUTOFF) *v = f->cutoff;
    else if (id == MD_FILTER_RESONANCE) *v = f->resonance;
    else return MD_ERR_NOT_FOUND;
    return MD_OK;
}
static void process(void *state, int32_t *x, uint32_t frames)
{
    md_fx_filter_t *f = state;
    for (uint32_t i = 0; i < frames; i++) {
        if (f->phase == 0) {
            int32_t cut = fx_smooth(f->cut_q8, f->cutoff * 256, 5), res = fx_smooth(f->res_q8, f->resonance * 256, 5);
            if (cut != f->cut_q8 || res != f->res_q8) { f->cut_q8 = cut; f->res_q8 = res; coefficients(f); }
        }
        f->phase = (f->phase + 1u) & (MD_FX_CONTROL - 1u);
        for (uint32_t c = 0; c < 2; c++) {
            /* Internal scale 2^8 above Q15, and the integrator updates carry their rounding
             * remainder (error feedback), so tiny low-cutoff increments are not lost: the
             * low-pass of DC converges exactly instead of stopping in a dead band. */
            int32_t v0 = fx_clamp(x[2 * i + c], -INPUT_LIMIT, INPUT_LIMIT) * SCALE;
            int32_t v3 = v0 - f->ic2[c];
            int64_t s1 = (int64_t)f->a1 * f->ic1[c] + (int64_t)f->a2 * v3 + f->e1[c];
            int64_t s2 = (int64_t)f->a2 * f->ic1[c] + (int64_t)f->a3 * v3 + f->e2[c];
            int32_t v1 = (int32_t)(s1 >> 24), t2 = (int32_t)(s2 >> 24);
            f->e1[c] = (int32_t)(s1 - (int64_t)v1 * ONE);   /* 0 .. 2^24 - 1 */
            f->e2[c] = (int32_t)(s2 - (int64_t)t2 * ONE);
            int32_t v2 = f->ic2[c] + t2;
            f->ic1[c] = fx_clamp(2 * v1 - f->ic1[c], -STATE_LIMIT, STATE_LIMIT);
            f->ic2[c] = fx_clamp(2 * v2 - f->ic2[c], -STATE_LIMIT, STATE_LIMIT);
            int32_t band = fx_mulz(f->k, v1, 24);
            int32_t y = f->type == MD_FILTER_LOWPASS ? v2 : f->type == MD_FILTER_BANDPASS ? band : v0 - band - v2;
            x[2 * i + c] = y >= 0 ? y >> 8 : -((-y) >> 8);   /* back to Q15, toward zero */
        }
    }
}
static void reset(void *state)
{
    md_fx_filter_t *f = state;
    memset(f->ic1, 0, sizeof f->ic1);
    memset(f->ic2, 0, sizeof f->ic2);
    memset(f->e1, 0, sizeof f->e1);
    memset(f->e2, 0, sizeof f->e2);
    f->phase = 0;
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
const md_effect_desc_t MD_FX_FILTER = {
    .processor = {.id = MD_FX_FILTER_ID, .api_version = MD_API_VERSION, .settings_version = 1, .name = "FILTER",
                  .state_bytes = sizeof(md_fx_filter_t), .state_alignment = _Alignof(md_fx_filter_t),
                  .sample_rates = rates, .sample_rate_count = 1, .param_count = COUNT, .params = params,
                  .max_settings_bytes = 2u * COUNT},
    .capabilities = MD_EFFECT_INSERT, .init = init, .set_param = set_param, .get_param = get_param,
    .process_replace = process, .reset = reset, .encode = encode, .validate = validate, .apply = apply,
};
