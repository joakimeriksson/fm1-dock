/* SPDX-License-Identifier: GPL-3.0-only */
/* Tape echo bus, ported from fm1-harbor firmware/src/hb_echo.c (GPL-3.0-only, the host's
 * seqdock echo) to a caller-owned instance with the effect API's span rules:
 *  - the same quarter-rate line (11.025 kHz, 12288 samples, 1.09 s), feedback law
 *    (100 % = 0.85), one-pole tone table (800 Hz .. 5 kHz) inside the loop, right tap
 *    20 ms behind, and linear interpolation of the taps back to the full rate;
 *  - streamed one frame at a time with a group phase, so spans need not be multiples of
 *    four and output does not depend on how the caller splits them;
 *  - stereo input mixed to mono; wet-only output;
 *  - delay changes glide (a quarter line sample per line sample) instead of jumping,
 *    as tape varispeed does, and the read is interpolated between line samples;
 *    feedback and tone move by a one-pole once a group (about 3 ms);
 *  - the tone and feedback round so that a tail decays to exactly zero. */
#include "fx_common.h"
#include <string.h>

static const uint32_t rates[] = {MD_SAMPLE_RATE};
static const char *const syncs[] = {"MS", "1/16", "1/8", "1/8.", "1/4", "1/4.", "1/2"};
static const md_param_desc_t params[] = {
    {MD_ECHO_SYNC, MD_PARAM_AUTOMATABLE | MD_PARAM_ENUM, "Sync", 0, 6, 4, MD_UNIT_INTEGER, syncs, 7},
    {MD_ECHO_TIME, MD_PARAM_AUTOMATABLE | MD_PARAM_SMOOTH, "Time", 20, 1090, 375, MD_UNIT_MS, NULL, 0},
    {MD_ECHO_FEEDBACK, MD_PARAM_AUTOMATABLE | MD_PARAM_SMOOTH, "Feedback", 0, 100, 40, MD_UNIT_PERCENT, NULL, 0},
    {MD_ECHO_TONE, MD_PARAM_AUTOMATABLE | MD_PARAM_SMOOTH, "Tone", 0, 100, 50, MD_UNIT_PERCENT, NULL, 0},
};
#define COUNT 4u
#define RATE (MD_SAMPLE_RATE / MD_ECHO_DIV)
#define MAX_T (MD_ECHO_LEN - MD_ECHO_SPREAD - 8u)
#define GLIDE 64 /* Q8 line samples per line sample */
/* 1 - e^(-2 pi fc / rate), fc 800 Hz .. 5 kHz in tenths, Q15 (hb_echo.c EC_TONE) */
static const uint16_t TONE[11] = {11998, 13815, 15788, 17888, 20069, 22270, 24415, 26420, 28202, 29695, 30857};
static const uint8_t SIXTEENTHS[7] = {0, 1, 2, 3, 4, 6, 8};

static void targets(md_fx_echo_t *e)
{
    uint32_t bpm_q8 = e->bpm_q8 ? e->bpm_q8 : 120u * 256u;
    uint32_t ms = e->sync ? 15000u * SIXTEENTHS[e->sync] * 256u / bpm_q8 : (uint32_t)e->time_ms;
    uint32_t t = ms * RATE / 1000u;
    if (t > MAX_T) t = MAX_T;
    if (t < 8u) t = 8u;
    e->target_q8 = (int32_t)(t << 8);
    e->gain_target = e->feedback * 278;
    uint32_t k = (uint32_t)e->tone / 10u, r = (uint32_t)e->tone % 10u;
    e->k_target = TONE[k] + (k < 10u ? (int32_t)((TONE[k + 1] - TONE[k]) * r / 10u) : 0);
}
static void snap(md_fx_echo_t *e)
{
    e->gain = e->gain_target;
    e->k = e->k_target;
    e->delay_q8 = e->target_q8;
}
static int init(void *state, uint32_t rate)
{
    (void)rate;
    md_fx_echo_t *e = state;
    memset(e, 0, sizeof *e);
    e->sync = 4; e->time_ms = 375; e->feedback = 40; e->tone = 50; e->bpm_q8 = 120u * 256u;
    targets(e);
    snap(e);
    return MD_OK;
}
static int set_param(void *state, md_param_id_t id, int32_t v)
{
    md_fx_echo_t *e = state;
    if (id == MD_ECHO_SYNC) e->sync = v;
    else if (id == MD_ECHO_TIME) e->time_ms = v;
    else if (id == MD_ECHO_FEEDBACK) e->feedback = v;
    else if (id == MD_ECHO_TONE) e->tone = v;
    else return MD_ERR_NOT_FOUND;
    targets(e);
    return MD_OK;
}
static int get_param(const void *state, md_param_id_t id, int32_t *v)
{
    const md_fx_echo_t *e = state;
    if (id == MD_ECHO_SYNC) *v = e->sync;
    else if (id == MD_ECHO_TIME) *v = e->time_ms;
    else if (id == MD_ECHO_FEEDBACK) *v = e->feedback;
    else if (id == MD_ECHO_TONE) *v = e->tone;
    else return MD_ERR_NOT_FOUND;
    return MD_OK;
}
static void set_tempo(void *state, uint16_t bpm_q8)
{
    md_fx_echo_t *e = state;
    e->bpm_q8 = bpm_q8;
    targets(e);
}
/* the line value `delay` (Q8) line samples behind the write index */
MD_INLINE int32_t tap(const md_fx_echo_t *e, int32_t delay_q8)
{
    uint32_t d = (uint32_t)delay_q8 >> 8, fr = (uint32_t)delay_q8 & 255u;
    uint32_t at = e->w + MD_ECHO_LEN - d;         /* d < MD_ECHO_LEN - 1: one wrap at most */
    if (at >= MD_ECHO_LEN) at -= MD_ECHO_LEN;
    uint32_t before = at ? at - 1u : MD_ECHO_LEN - 1u;
    int32_t a = e->line[at];
    int32_t b = e->line[before];
    return a + (((b - a) * (int32_t)fr) >> 8);
}
/* (not MD_HOT: the 16 KiB of RAM are full of dearer loops, and this one is 2 % of a block) */
static void process(void *state, const int32_t *in, int32_t *wet, uint32_t frames)
{
    md_fx_echo_t *e = state;
    for (uint32_t i = 0; i < frames; i++) {
        if (e->sub == 0) { /* a new group: the taps it fades towards */
            int32_t d = e->target_q8 - e->delay_q8;
            e->delay_q8 += d > GLIDE ? GLIDE : d < -GLIDE ? -GLIDE : d;
            e->pl = e->yl;
            e->pr = e->yr;
            e->yl = tap(e, e->delay_q8);
            e->yr = tap(e, e->delay_q8 + (int32_t)(MD_ECHO_SPREAD << 8));
        }
        int32_t k = e->sub + 1;
        wet[2 * i] = e->pl + (((e->yl - e->pl) * k) >> 2);
        wet[2 * i + 1] = e->pr + (((e->yr - e->pr) * k) >> 2);
        e->acc += (fx_in(in[2 * i]) + fx_in(in[2 * i + 1])) >> 1;
        if (++e->sub == MD_ECHO_DIV) { /* the group's input into the tape, through the tone */
            e->gain = fx_smooth(e->gain, e->gain_target, 5);   /* ~3 ms, once a group */
            e->k = fx_smooth(e->k, e->k_target, 5);
            int32_t x = fx_clamp(e->acc >> 2, -32767, 32767);
            e->acc = 0;
            e->sub = 0;
            e->lp = fx_step(e->lp, x + fx_mulz(e->yl, e->gain, 15), e->k, 15);
            e->line[e->w] = (int16_t)fx_clamp(e->lp, -32767, 32767);
            if (++e->w == MD_ECHO_LEN) e->w = 0;
        }
    }
}
static void reset(void *state)
{
    md_fx_echo_t *e = state;
    memset(e->line, 0, sizeof e->line);
    e->acc = e->lp = e->yl = e->yr = e->pl = e->pr = 0;
    e->w = 0;
    e->sub = 0;
    snap(e);
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
    md_fx_echo_t *e = state;
    for (uint32_t i = 0; i < COUNT; i++) (void)set_param(e, params[i].id, fx_settings_value(src, i));
    snap(e);
    return MD_OK;
}
const md_effect_desc_t MD_FX_ECHO = {
    .processor = {.id = MD_FX_ECHO_ID, .api_version = MD_API_VERSION, .settings_version = 1, .name = "ECHO",
                  .state_bytes = sizeof(md_fx_echo_t), .state_alignment = _Alignof(md_fx_echo_t),
                  .sample_rates = rates, .sample_rate_count = 1, .param_count = COUNT, .params = params,
                  .max_settings_bytes = 2u * COUNT},
    .capabilities = MD_EFFECT_BUS, .init = init, .set_param = set_param, .get_param = get_param,
    .set_tempo = set_tempo, .process_wet = process, .reset = reset, .encode = encode, .validate = validate,
    .apply = apply,
};
