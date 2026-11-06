/* SPDX-License-Identifier: GPL-3.0-only */
/* Original 25-byte panel records for PRESETS; software extension for MEMORIES.
 * Decode on recall; runtime patches keep exact Q16 and independently editable layers. */
#ifndef SC_PRESETS_H
#define SC_PRESETS_H
#include <stddef.h>
#include "sc_patch.h"
#include "sc_presets_data.h"
_Static_assert(sizeof(sc_patch_t) == 620 && sizeof(sc_layer_t) == 256 &&
               offsetof(sc_patch_t, layer) == 28 && offsetof(sc_patch_t, voice_mode) == 540,
               "update the preset extension codec for this patch layout");

static const char *sc_preset_name(unsigned index)
{
    return SC_PRESET_NAMES + SC_PRESET_NAME_OFF[index % SC_N_PRESETS];
}

/* Rational rounding, ties to even; all numerators fit int32. */
static int32_t sc_rom_round(int32_t n, int32_t d)
{
    int32_t sign = n < 0 ? -1 : 1;
    uint32_t a = (uint32_t)(n < 0 ? -n : n), q = a / (uint32_t)d, r = a % (uint32_t)d;
    if (r * 2u > (uint32_t)d || (r * 2u == (uint32_t)d && (q & 1u))) q++;
    return sign * (int32_t)q;
}

/* Decimal -> Q16, Math.round as in gen_presets.mjs (negative ties toward +inf). */
static int32_t sc_rom_q(int32_t n, int32_t scale)
{
    int32_t v = n * 65536;
    return v >= 0 ? (v + scale / 2) / scale : -((-v + (scale - 1) / 2) / scale);
}

static unsigned sc_rom_semis(unsigned value)
{
    static const uint8_t thresholds[13] = {0, 5, 21, 45, 71, 96, 120, 146, 171, 195, 217, 235, 250};
    unsigned i, semis = 0;
    for (i = 1; i < 13; i++) if (value >= thresholds[i]) semis = i;
    return semis;
}

static void sc_rom_load(unsigned index, sc_patch_t *p)
{
    static const uint8_t widths[40] = {
        8,8,8,8,8,8,8,8,8, 7,7,7,7,7,7,7,7, 4,4,4,4,4,4,4,4,4,
        6, 3,3,3,3, 2,2,2,2,2,2,2,2,2
    };
    static const uint8_t wave[5] = { SC_WAVE_TRI, SC_WAVE_SAW, SC_WAVE_SQR, SC_WAVE_SQR, SC_WAVE_SQR };
    static const uint8_t filter[4] = { SC_FLT_LP24, SC_FLT_BP6, SC_FLT_BP12, SC_FLT_HP12 };
    static const uint8_t lfo[4] = { SC_LFO_TRI, SC_LFO_RAMPDN, SC_LFO_RAMP, SC_LFO_SQR };
    const uint8_t *record = SC_PRESET_RECORDS[index];
    uint8_t v[40];
    unsigned i, k, bit = 0;
    for (i = 0; i < 40; i++) {
        unsigned value = 0;
        for (k = 0; k < widths[i]; k++, bit++)
            value |= ((record[24 - bit / 8] >> (bit % 8)) & 1u) << k;
        if ((i >= 9 && i < 26) || i >= 34) value <<= 8 - widths[i];
        v[i] = (uint8_t)value;
    }
    sc_layer_t *L = &p->layer[0];
    L->o1_wave = (int8_t)wave[v[29]]; L->o1_oct = (int8_t)(v[27] - 2);
    L->o2_wave = (int8_t)wave[v[30]]; L->o2_oct = (int8_t)(v[28] - 2);
    L->o2_sync = (int8_t)((v[35] & 128u) != 0);
    L->o1_glide_speed = L->o2_glide_speed = 3277;
    int32_t pwm1 = sc_rom_round(v[11] * 1000, 254), pwm2 = sc_rom_round(v[12] * 1000, 254);
    if (pwm1 < 50) pwm1 = 50;
    if (pwm1 > 950) pwm1 = 950;
    if (pwm2 < 50) pwm2 = 50;
    if (pwm2 > 950) pwm2 = 950;
    L->o1_pwm = v[29] >= 3 ? sc_rom_q(pwm1, 1000) : 32768;
    L->o2_pwm = v[30] >= 3 ? sc_rom_q(pwm2, 1000) : 32768;
    L->o2_detune = sc_rom_q(1000 * ((int32_t)sc_rom_semis(v[16]) - (int32_t)sc_rom_semis(v[15])) +
                              sc_rom_round(((int32_t)v[0] - 128) * 125, 32), 10);
    L->mix_o1 = sc_rom_q(sc_rom_round(v[17] * 35, 12), 1000);
    L->mix_o2 = sc_rom_q(sc_rom_round(v[18] * 35, 12), 1000);
    L->mix_noise = (v[39] & 192u) ? sc_rom_q(sc_rom_round(v[19] * 35, 12), 1000) : 0;
    L->noise_pink = (int8_t)((v[39] & 128u) != 0);
    L->ring = (int8_t)((v[37] & 192u) != 0);
    L->cross = v[30] == 4 ? 11469 : 0; L->cross2 = v[29] == 4 ? 11469 : 0;
    L->flt_mode = (int8_t)filter[v[32]];
    L->cutoff = sc_rom_q(sc_rom_round((-25000 + 580 * (255 - (int32_t)v[5])) * 12750 +
                                     971616 * (int32_t)v[6], 1224000), 1000);
    L->key_track = sc_rom_q(sc_rom_round(80968 * (int32_t)v[6], 25500), 1000);
    L->reso = sc_rom_q(sc_rom_round(v[10] * 1000, 254), 1000);
    int32_t env = (v[20] >> 4) <= 3 && (v[22] >> 4) > 6 ? SC_ROM_ENV_LONG_MILLI[v[7]] : SC_ROM_ENV_MILLI[v[7]];
    L->env_amt = sc_rom_q((v[35] & 64u) ? -env : env, 1000);
    L->env_f.a = sc_rom_q(SC_ROM_ATTACK_MS[v[20] >> 4], 1000);
    L->env_f.d = sc_rom_q(index == 32 ? 3300 : SC_ROM_ENV_MS[v[22] >> 4], 1000); /* calibrated Take-off */
    L->env_f.s = sc_rom_q(sc_rom_round(v[8] * 1000, 255), 1000);
    L->env_f.r = sc_rom_q((v[38] & 128u) ? SC_ROM_ENV_MS[v[24] >> 4] : 10, 1000);
    L->env_a.a = sc_rom_q(SC_ROM_ATTACK_MS[v[21] >> 4], 1000);
    L->env_a.d = sc_rom_q(SC_ROM_ENV_MS[v[23] >> 4], 1000);
    L->env_a.s = sc_rom_q(sc_rom_round(v[9] * 1000, 254), 1000);
    L->env_a.r = sc_rom_q((v[38] & 128u) ? SC_ROM_ENV_MS[v[25] >> 4] : 10, 1000);
    L->lfo[0].shape = (int8_t)lfo[v[31]];
    L->lfo[0].rate = sc_rom_q(SC_ROM_RATE_CHZ[v[3]], 100);
    L->lfo[0].delay = sc_rom_q(SC_ROM_DELAY_CS[v[4]], 100);
    L->lfo[0].depth_a = sc_rom_q(sc_rom_round(v[1] * 1000, 255), 1000);
    L->lfo[0].depth_b = sc_rom_q(sc_rom_round(v[2] * 1000, 255), 1000);
    L->lfo[1].rate = 26214; L->lfo[1].depth_a = L->lfo[1].depth_b = 65536;
    int32_t pitch = sc_rom_q(SC_ROM_PITCH_1E4[v[1]], 10000);
    if (v[26] & 1u) L->mm[SC_MM_L1_P1] = pitch;
    if (v[26] & 2u) L->mm[SC_MM_L1_P2] = pitch;
    if (v[26] & 4u) L->mm[SC_MM_L1_PW1] = 32768;
    if (v[26] & 32u) L->mm[SC_MM_L1_PW2] = 32768;
    if (v[26] & 16u) L->mm[SC_MM_L1_CUT] = 32768;
    if (v[26] & 8u) L->mm[SC_MM_L1_AMP] = 24248;
    L->glide_o1 = L->glide_o2 = L->multi_trigger = 1;
    L->glide_mode = v[34] & 128u ? SC_GLIDE_PORTA : v[34] & 64u ? SC_GLIDE_GLIDE : SC_GLIDE_OFF;
    if (L->glide_mode) {
        L->glide_time = sc_rom_q(SC_ROM_GLIDE_MS[v[13] >> 1], 1000);
        L->glide_amt = ((int32_t)(v[14] >> 2) - 32) * 65536;
        L->glide_o1 = (int8_t)((v[36] & 128u) != 0);
        L->glide_o2 = (int8_t)((v[36] & 64u) != 0);
    }
    p->split = 60;
    p->fx.chorus.on = (int8_t)(v[33] != 0); p->fx.chorus.mode = v[33] ? (int8_t)v[33] : 2;
    p->fx.chorus.mix = p->fx.chorus.rate = 32768; p->fx.chorus.depth = 39322;
    p->fx.delay.time = 16384; p->fx.delay.feedback = p->fx.delay.spread = 19661;
    p->fx.delay.tone = 32768; p->fx.delay.mix = 13107;
    p->fx.reverb.size = p->fx.reverb.damping = 32768; p->fx.reverb.mix = 13107;
    p->volume = 52429; p->unison_detune = 9830; p->polyphony = 8; p->limiter = 1;
}

static void sc_preset_load(unsigned index, sc_patch_t *p)
{
    unsigned i, k;
    uint8_t *bytes = (uint8_t *)p;
    index %= SC_N_PRESETS;
    for (i = 0; i < sizeof *p; i++) bytes[i] = 0;
    if (index < 40) sc_rom_load(index, p);
    else {
        const uint8_t *record = SC_PRESET_RECORDS[index];
        unsigned bit = 0;
        for (i = 0; i < 84; i++) {
            unsigned first = SC_MEM_PALETTE_OFF[i], count = SC_MEM_PALETTE_OFF[i + 1] - first;
            unsigned width = 0, code = 0;
            while ((1u << width) < count) width++;
            for (k = 0; k < width; k++, bit++) code |= ((record[bit / 8] >> (bit % 8)) & 1u) << k;
            uint32_t value = SC_MEM_VALUES[first + code];
            unsigned offset = i < 64 ? 28 + 4 * i : 540 + 4 * (i - 64);
            for (k = 0; k < 4; k++) bytes[offset + k] = (uint8_t)(value >> (8 * k));
        }
    }
    p->layer[1] = p->layer[0];
    const char *name = sc_preset_name(index);
    for (i = 0; name[i] && i < sizeof p->name - 1; i++) p->name[i] = name[i];
}
#endif
