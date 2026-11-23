/* fm1-dock: FLAT-20's firmware/src/flat20.c, split by modules/flat20/take.py: do not edit */
/* SPDX-License-Identifier: GPL-3.0-only */
/* FLAT-20: Korg MS-20 firmware for the M-VAVE FM-1, built on Dinghy (Felucca/SLOOP/OberVave platform).
 * The synth engine is in ms20.h; this file implements the UI, parameter pages, patch panel, presets, and MIDI.
 *
 *   SELECT                 parameter page (VCO 1, VCO 2, MIXER/FM, HIGH PASS, LOW PASS, MG/EG 1, EG 2, MORE, ESP, PATCH PANEL)
 *   ALGORITHM              mod wheel (-5..+5 V, stays where left)
 *   KNOB 1..4              current page parameters (PATCH PANEL: slot, source, dest, turn left to unplug)
 *   HOME/FX/ENV/LFO/EDIT   jump to VCOs, filters, envelopes, MG, or patch panel/ESP
 *   PRESETS / SAVE         select/save one of 26 presets
 *   REC                    momentary switch (shorts to ground while held)
 *   PLAY                   key hold
 *   OCT- / OCT+            octave shift (MS-20 keyboard is C3..C6 at 8')
 *   MIDI IN                notes, pitch bend (±2), CC 1 = wheel upper half, CC 2 = full wheel (64 = center), CC 16..51 = knobs
 *
 * High-note priority, single-trigger. Main loop is integer-only; float is restricted to the audio ISR. */
#if BOARD_SMK37
#error "FLAT-20 is for the FM-1 only: the platform's SMK-37 port is not supported (its board layer is not in this tree)"
#endif
static void app_idle(void);
#define LCD_IDLE app_idle
#include <stdint.h>
static int app_sysex(const uint8_t *p, uint32_t n);
#define APP_SYSEX app_sysex          /* (platform.c: a SysEx from the computer, F0 and F7 left off) */
#define APP_NAME "FLAT-20"
#include "platform.c"
#include "ms20.h"
#include "ms20_link.h"

/* ---- the sound */
static ms20_t syn;
static ms20_patch_t patch;                      /* main-loop working copy; published to audio_patch[] via patch_commit() */
static ms20_patch_t audio_patch[2];
static volatile uint32_t audio_patch_idx;
static ms20_ctl_t ctl;
static volatile uint32_t cpu_peak;              /* the render's time for 256 frames, of 5800 us */
static uint8_t syn_ok;

static void patch_commit(void)
{
    uint32_t next = audio_patch_idx ^ 1u;
    memcpy(&audio_patch[next], &patch, sizeof patch);
#ifdef RING_PUBLISH
    RING_PUBLISH();
#endif
    audio_patch_idx = next;
}

static void app_audio(int32_t *out, uint32_t n)
{
    static float buf[256];
    uint32_t i;
#ifndef DINGHY_HOST
    uint32_t t0 = fm1_ticks(), us;
#endif
    if (n > 256u)
        n = 256u;
    if (!syn_ok) {
        ms20_init(&syn);
        syn_ok = 1;
    }
#ifndef DIAG_NOFLOAT
    ms20_render(&syn, &audio_patch[audio_patch_idx], &ctl, buf, n);
#endif
    for (i = 0; i < n; i++) {
#ifndef DIAG_NOFLOAT
        float x = buf[i] * 30000.f;               /* +-2.5 V at SIGNAL OUT: 0.92 of full scale */
        int32_t v = (int32_t)(x > 32767.f ? 32767.f : x < -32767.f ? -32767.f : x);
#else
        int32_t v = 0;
#endif
        out[2u * i] += v;
#ifdef DIAG_TONE                                /* a 1 kHz integer triangle on the right: is a glitch the engine's or the block's? */
        { static uint32_t tph; int32_t tv; tph += 97391549u; tv = (int32_t)tph >> 17; out[2u * i + 1u] += (tv < 0 ? -tv : tv) - 8192; }
#else
        out[2u * i + 1u] += v;
#endif
    }
#ifndef DINGHY_HOST
    {                                           /* (the platform asks for 32 frames at a time: 8 calls are a half buffer) */
        static uint32_t acc_us, acc_n;
        us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
        acc_us += us;
        acc_n += n;
        if (acc_n >= 256u) {
            if (acc_us > cpu_peak)
                cpu_peak = acc_us;
            acc_us = acc_n = 0;
        }
    }
#endif
}

/* ---- the controls: what the screen calls them and how it shows them */
enum { K_DIAL, K_BI, K_ENUM };                  /* 0.0 .. 10.0; -5.0 .. +5.0; one of four names */
typedef struct { const char *label; uint8_t kind; const char *const *names; } pinfo_t;
static const char *const N_W1[4] = {"TRI", "SAW", "PULSE", "NOISE"};
static const char *const N_W2[4] = {"SAW", "SQR", "PULSE", "RING"};
static const char *const N_S1[4] = {"32'", "16'", "8'", "4'"};
static const char *const N_S2[4] = {"16'", "8'", "4'", "2'"};
static const pinfo_t PINFO[MS_NPARAM] = {
    [P_V1WAVE] = {"WAVE", K_ENUM, N_W1}, [P_V1PW] = {"PW", K_DIAL, 0}, [P_V1SCALE] = {"SCALE", K_ENUM, N_S1},
    [P_PORTA] = {"PORTA", K_DIAL, 0}, [P_V2WAVE] = {"WAVE", K_ENUM, N_W2}, [P_V2PITCH] = {"PITCH", K_BI, 0},
    [P_V2SCALE] = {"SCALE", K_ENUM, N_S2}, [P_TUNE] = {"TUNE", K_BI, 0}, [P_MIX1] = {"VCO 1", K_DIAL, 0},
    [P_MIX2] = {"VCO 2", K_DIAL, 0}, [P_FM_MG] = {"FM MG", K_DIAL, 0}, [P_FM_EG] = {"FM EG1", K_DIAL, 0},
    [P_HP_CUT] = {"CUTOFF", K_DIAL, 0}, [P_HP_PEAK] = {"PEAK", K_DIAL, 0}, [P_HP_MG] = {"MG", K_DIAL, 0},
    [P_HP_EG] = {"EG2", K_DIAL, 0}, [P_LP_CUT] = {"CUTOFF", K_DIAL, 0}, [P_LP_PEAK] = {"PEAK", K_DIAL, 0},
    [P_LP_MG] = {"MG", K_DIAL, 0}, [P_LP_EG] = {"EG2", K_DIAL, 0}, [P_MG_WAVE] = {"MG WAVE", K_DIAL, 0},
    [P_MG_RATE] = {"MG RATE", K_DIAL, 0}, [P_E1_DELAY] = {"E1 DLY", K_DIAL, 0}, [P_E1_ATK] = {"E1 ATK", K_DIAL, 0},
    [P_E1_REL] = {"E1 REL", K_DIAL, 0}, [P_E2_HOLD] = {"E2 HOLD", K_DIAL, 0}, [P_E2_ATK] = {"ATTACK", K_DIAL, 0},
    [P_E2_DEC] = {"DECAY", K_DIAL, 0}, [P_E2_SUS] = {"SUSTAIN", K_DIAL, 0}, [P_E2_REL] = {"RELEASE", K_DIAL, 0},
    [P_VOLUME] = {"VOLUME", K_DIAL, 0}, [P_ESP_CV] = {"CV ADJ", K_BI, 0},
    [P_ESP_LEVEL] = {"LEVEL", K_DIAL, 0}, [P_ESP_LOW] = {"LOW CUT", K_DIAL, 0}, [P_ESP_HIGH] = {"HI CUT", K_DIAL, 0},
    [P_ESP_THR] = {"THRESH", K_DIAL, 0},
};
#define NPAGE 10u
#define PG_PATCH 9u
static const struct { const char *name; uint8_t p[4]; } PAGE[NPAGE] = {
    {"VCO 1", {P_V1WAVE, P_V1PW, P_V1SCALE, P_PORTA}},
    {"VCO 2", {P_V2WAVE, P_V2PITCH, P_V2SCALE, P_TUNE}},
    {"MIXER / FM", {P_MIX1, P_MIX2, P_FM_MG, P_FM_EG}},
    {"HIGH PASS", {P_HP_CUT, P_HP_PEAK, P_HP_MG, P_HP_EG}},
    {"LOW PASS", {P_LP_CUT, P_LP_PEAK, P_LP_MG, P_LP_EG}},
    {"MG / EG 1", {P_MG_WAVE, P_MG_RATE, P_E1_ATK, P_E1_REL}},
    {"EG 2", {P_E2_ATK, P_E2_DEC, P_E2_SUS, P_E2_REL}},
    {"MORE", {P_E1_DELAY, P_E2_HOLD, P_VOLUME, P_ESP_CV}},
    {"EXT SIGNAL PROC", {P_ESP_LEVEL, P_ESP_LOW, P_ESP_HIGH, P_ESP_THR}},
    {"PATCH PANEL", {0, 0, 0, 0}},
};
static const char *const SRC_NAME[MS_NSRC] = {"-", "KBD CV", "KBD TRIG", "MG TRI", "MG RECT", "EG1 OUT", "EG1 REV", "EG2 REV",
    "S&H OUT", "VCA OUT", "WHITE", "PINK", "WHEEL", "SWITCH", "SIGNAL", "PHONES", "ESP AMP", "ESP BPF", "ESP F-V", "ESP ENV",
    "ESP TRIG", "VCO 1 *", "VCO 2 *"};          /* (*: not a jack on an MS-20) */
static const char *const DST_NAME[MS_NDST] = {"-", "TOTAL", "FREQ", "VCO1+2 CV", "VCO2 CV", "EXT SIG", "HP CUTOFF", "LP CUTOFF",
    "INIT GAIN", "TRIG IN", "EG1 TRIG", "S&H IN", "S&H CLOCK", "VCA IN", "VCA CTRL", "ESP IN"};
static const char *jack_name(uint32_t j)
{
    return j == 0u || j >= MS_NJACK ? "-" : j < MS_NSRC ? SRC_NAME[j] : DST_NAME[j - (MS_NSRC - 1u)];
}

/* ---- presets: 26 factory patches until SAVE overwrites a slot (blob 0) */
#define NPRESET 26u
#define W(i) ((i) * 32 + 16)                    /* a four-way switch's position as a stored value */
#define K(x) ((uint8_t)((x) * 12.7f + 0.5f))    /* a knob as its scale reads, 0 .. 10 */
#define BASE [P_V1WAVE] = W(1), [P_V1SCALE] = W(2), [P_V2WAVE] = W(0), [P_V2PITCH] = 64, [P_V2SCALE] = W(1), [P_TUNE] = 64, \
    [P_MIX1] = K(10), [P_LP_CUT] = K(10), [P_MG_WAVE] = K(5), [P_MG_RATE] = K(3), [P_E2_DEC] = K(5), [P_E2_SUS] = K(10), \
    [P_E2_REL] = K(1.5), [P_VOLUME] = K(10), [P_ESP_CV] = K(6.5), [P_ESP_LEVEL] = K(0.2), [P_ESP_LOW] = K(2), \
    [P_ESP_HIGH] = K(4), [P_ESP_THR] = K(3)
static const ms20_patch_t FACTORY[NPRESET] = {
    {.name = "INIT", .p = {BASE}},
    /* The "Flat Beat" bass as GAK's KORG Sound Icons video sets it on an MS-20 mini: one square at 32', the low-pass
     * open with a little PEAK, EG2 with only a DECAY, and one cable: EG2's REV OUT into the low-pass's CUTOFF FREQ, its
     * EG2 / EXT at the top. The envelope upside down: the filter shuts as the note starts and opens as it dies away. */
    {.name = "FLAT BEAT", .p = {BASE, [P_V1WAVE] = W(2), [P_V1PW] = 0, [P_V1SCALE] = W(0), [P_MIX1] = K(9.5), [P_PORTA] = K(2),
        [P_HP_CUT] = K(0.3), [P_LP_PEAK] = K(4.5), [P_LP_EG] = K(10), [P_E2_DEC] = K(1.6), [P_E2_SUS] = 0, [P_E2_REL] = 0},
        .cable = {{MS_JACK_OUT(SRC_EG2_REV), MS_JACK_IN(DST_LP_CUT)}}},
    /* The "Da Funk" lead as the same series sets it: two sawtooths a fourth apart, both PEAKs at 8, and EG1's OUT
     * (0 V rising to +5 V) into the high-pass's CUTOFF FREQ: the high-pass sweeps up into every note. */
    {.name = "DA FUNK", .p = {BASE, [P_V2SCALE] = W(0), [P_V2PITCH] = 97, [P_MIX1] = K(9), [P_MIX2] = K(9), [P_PORTA] = K(2.3),
        [P_HP_CUT] = K(0.5), [P_HP_PEAK] = K(8), [P_HP_EG] = K(9), [P_LP_CUT] = K(9.5), [P_LP_PEAK] = K(8),
        [P_E1_ATK] = K(1.5), [P_E1_REL] = K(5), [P_E2_REL] = K(3)},
        .cable = {{MS_JACK_OUT(SRC_EG1), MS_JACK_IN(DST_HP_CUT)}}},
    /* both filters near singing, EG2 pulling the low-pass up from below (SUSTAIN at 6: the filters get EG2 less that) */
    {.name = "SCREAMER", .p = {BASE, [P_V2WAVE] = W(1), [P_V2PITCH] = 66, [P_MIX2] = K(10), [P_PORTA] = K(3), [P_HP_CUT] = K(3.5),
        [P_HP_PEAK] = K(7.5), [P_LP_CUT] = K(6.5), [P_LP_PEAK] = K(8), [P_LP_EG] = K(7), [P_E2_ATK] = K(0.5), [P_E2_DEC] = K(4),
        [P_E2_SUS] = K(6), [P_E2_REL] = K(3)}},
    /* the high-pass singing along (PEAK just past where it starts), swept by the MG */
    {.name = "HP SQUEAL", .p = {BASE, [P_V1SCALE] = W(1), [P_HP_CUT] = K(4.5), [P_HP_PEAK] = K(9), [P_HP_MG] = K(6),
        [P_MG_RATE] = K(0.8), [P_E2_REL] = K(3)}},
    {.name = "RING BELL", .p = {BASE, [P_V1WAVE] = W(2), [P_MIX1] = 0, [P_V2WAVE] = W(3), [P_V2PITCH] = 98, [P_MIX2] = K(10),
        [P_LP_CUT] = K(7.5), [P_LP_EG] = K(5), [P_E2_DEC] = K(5), [P_E2_SUS] = 0, [P_E2_REL] = K(5)}},
    /* The MS-20's "sample and hold" follows while its clock is high: the MG's rectangle, made narrow by WAVE FORM,
     * lets a moment of noise through and holds it. S&H OUT into TOTAL: the MG / T.EXT knobs now turn it up. */
    {.name = "S&H RANDOM", .p = {BASE, [P_V1WAVE] = W(2), [P_V1PW] = K(3), [P_FM_MG] = K(6.5), [P_LP_CUT] = K(6), [P_LP_PEAK] = K(6),
        [P_LP_MG] = K(5), [P_MG_RATE] = K(3.5), [P_MG_WAVE] = K(9.4)},
        .cable = {{MS_JACK_OUT(SRC_WHITE), MS_JACK_IN(DST_SH_IN)}, {MS_JACK_OUT(SRC_MG_RECT), MS_JACK_IN(DST_SH_CLOCK)}, {MS_JACK_OUT(SRC_SH), MS_JACK_IN(DST_TOTAL)}}},
    /* Vibrato that comes in late: the MG through the modulation VCA into TOTAL. With nothing in its CONTROL INPUT the
     * VCA is opened by EG1: DELAY and ATTACK are how late and how slowly. */
    {.name = "DELAY VIB", .p = {BASE, [P_FM_MG] = K(2.4), [P_MG_RATE] = K(3), [P_LP_CUT] = K(7.5), [P_E1_DELAY] = K(4),
        [P_E1_ATK] = K(4), [P_E1_REL] = K(2), [P_E2_REL] = K(3)},
        .cable = {{MS_JACK_OUT(SRC_MG_TRI), MS_JACK_IN(DST_MVCA_IN)}, {MS_JACK_OUT(SRC_MVCA), MS_JACK_IN(DST_TOTAL)}}},
    /* the headphone output back into EXT SIGNAL IN: the MS-20's own overdrive (VOLUME is how much) */
    {.name = "FEEDBACK", .p = {BASE, [P_V1SCALE] = W(1), [P_V2WAVE] = W(1), [P_V2SCALE] = W(0), [P_MIX1] = K(7), [P_MIX2] = K(7),
        [P_LP_CUT] = K(6), [P_LP_PEAK] = K(6), [P_LP_EG] = K(5), [P_E2_DEC] = K(4), [P_E2_SUS] = K(7), [P_VOLUME] = K(8.5)},
        .cable = {{MS_JACK_OUT(SRC_PHONES), MS_JACK_IN(DST_EXT_IN)}}},
    /* the MG's rectangle into TRIG IN: a trigger is a contact to ground, so the note sounds while the rectangle is low.
     * The keys only choose the pitch. */
    {.name = "AUTO TRIG", .p = {BASE, [P_V1WAVE] = W(2), [P_V1SCALE] = W(1), [P_LP_CUT] = K(4.5), [P_LP_PEAK] = K(6.5),
        [P_LP_EG] = K(7), [P_MG_RATE] = K(4), [P_E2_DEC] = K(2.2), [P_E2_SUS] = 0, [P_E2_REL] = K(1)},
        .cable = {{MS_JACK_OUT(SRC_MG_RECT), MS_JACK_IN(DST_TRIG)}}},
    /* no oscillator: the low-pass sings, EG2 throws its pitch down */
    {.name = "KICK", .p = {BASE, [P_MIX1] = 0, [P_LP_CUT] = K(1.7), [P_LP_PEAK] = K(10), [P_LP_EG] = K(6.5), [P_E2_DEC] = K(2.3),
        [P_E2_SUS] = 0, [P_E2_REL] = K(2.3)}},
    {.name = "WIND", .p = {BASE, [P_V1WAVE] = W(3), [P_HP_CUT] = K(4), [P_LP_CUT] = K(6), [P_LP_PEAK] = K(8.3), [P_LP_MG] = K(6.5),
        [P_MG_RATE] = K(0.2), [P_E2_ATK] = K(4.5), [P_E2_REL] = K(5.5)}},
    /* EG1 / EXT: the VCOs get EG1 as -5 V at rest, so the note climbs to its pitch and falls away after */
    {.name = "EG1 SWOOP", .p = {BASE, [P_V2WAVE] = W(1), [P_V2SCALE] = W(0), [P_MIX2] = K(8), [P_FM_EG] = K(5.5), [P_LP_CUT] = K(7),
        [P_E1_ATK] = K(2.5), [P_E1_REL] = K(3.5), [P_E2_REL] = K(4.5)}},
    /* a plug in VCO 2 CV IN takes VCO 2 off the keyboard: here the wheel is its pitch, in Hz / V (ALGORITHM) */
    {.name = "WHEEL>VCO2", .p = {BASE, [P_MIX1] = K(8.5), [P_MIX2] = K(8.5), [P_LP_CUT] = K(7.5), [P_LP_PEAK] = K(4)},
        .cable = {{MS_JACK_OUT(SRC_WHEEL), MS_JACK_IN(DST_VCO2_CV)}}},
    /* The external signal processor listening to the synthesizer itself: its frequency-to-voltage converter plays
     * VCO 2 from VCO 1's pitch (CV ADJUST tunes it), with the converter's lag. */
    {.name = "F-V TRACK", .p = {BASE, [P_V1WAVE] = W(0), [P_V2WAVE] = W(1), [P_MIX1] = K(7), [P_MIX2] = K(9), [P_LP_CUT] = K(7.5)},
        .cable = {{MS_JACK_OUT(SRC_VCO1), MS_JACK_IN(DST_ESP_IN)}, {MS_JACK_OUT(SRC_ESP_FV), MS_JACK_IN(DST_VCO2_CV)}}},
    /* FLAT ALEX: one pulse at 32', both filters low with some PEAK (the high-pass tuned for a bump around 40-80 Hz), the
     * MG and EG2 sweeping the low-pass nearly all the way, EG2 a long decay to half. MG WAVE is the performance control:
     * 10 (the ramp) wubs, 0 (the sawtooth) snaps the filter open and lets it fall. Bb, A and C; on the last note, hold
     * it and turn MG RATE up. MG and EG2 into the low-pass are steep between 9 and 10: trim them by ear. */
    {.name = "FLAT ALEX", .p = {BASE, [P_V1WAVE] = W(2), [P_V1PW] = 0, [P_V1SCALE] = W(0), [P_MIX1] = K(9.5), [P_MIX2] = 0,
        [P_HP_CUT] = K(1.7), [P_HP_PEAK] = K(4.5), [P_LP_CUT] = K(1.5), [P_LP_PEAK] = K(5), [P_LP_MG] = K(9.5),
        [P_LP_EG] = K(9.5), [P_E2_ATK] = 0, [P_E2_DEC] = K(9.5), [P_E2_SUS] = K(4.5), [P_E2_REL] = 0, [P_MG_RATE] = K(1.7),
        [P_MG_WAVE] = K(10)}},
    /* Visage - "Fade to Grey" (1980): punchy resonant synth-pop bass */
    {.name = "FADE TO GRY", .p = {BASE, [P_V1SCALE] = W(1), [P_V2WAVE] = W(1), [P_V2SCALE] = W(0), [P_V2PITCH] = 65,
        [P_MIX1] = K(9.5), [P_MIX2] = K(8.5), [P_HP_CUT] = K(1.5), [P_HP_PEAK] = K(3.5), [P_LP_CUT] = K(4.5),
        [P_LP_PEAK] = K(4.5), [P_LP_EG] = K(6.5), [P_E2_DEC] = K(2.5), [P_E2_SUS] = K(2.5), [P_E2_REL] = K(1.5)}},
    /* The Normal - "Warm Leatherette" (1978): stark minimal-wave pulse bass */
    {.name = "LEATHERETTE", .p = {BASE, [P_V1SCALE] = W(1), [P_V2WAVE] = W(1), [P_V2SCALE] = W(0), [P_MIX1] = K(9.5),
        [P_MIX2] = K(9.5), [P_HP_CUT] = K(1.8), [P_HP_PEAK] = K(5.5), [P_LP_CUT] = K(3.8), [P_LP_PEAK] = K(7.2),
        [P_LP_EG] = K(8.5), [P_E2_DEC] = K(1.9), [P_E2_SUS] = K(1), [P_E2_REL] = K(1)}},
    /* Fad Gadget - "Collapsing New People" (1984): metallic ring-mod industrial bass/lead */
    {.name = "FAD GADGET", .p = {BASE, [P_V1SCALE] = W(1), [P_V2WAVE] = W(3), [P_V2SCALE] = W(1), [P_V2PITCH] = 83,
        [P_MIX1] = K(8.5), [P_MIX2] = K(6.5), [P_HP_CUT] = K(2.5), [P_HP_PEAK] = K(6), [P_LP_CUT] = K(4.8),
        [P_LP_PEAK] = K(7), [P_LP_EG] = K(8), [P_E2_DEC] = K(2.4), [P_E2_SUS] = K(1.5), [P_E2_REL] = K(1.5)}},
    /* Jean-Michel Jarre - "Equinoxe" / "Magnetic Fields" (1978-81): dual-filter cosmic lead (EG1 -> HPF) */
    {.name = "JARRE SWEEP", .p = {BASE, [P_V2WAVE] = W(2), [P_V2SCALE] = W(2), [P_MIX1] = K(8.5), [P_MIX2] = K(6.5),
        [P_PORTA] = K(2), [P_HP_CUT] = K(1.5), [P_HP_PEAK] = K(5), [P_HP_EG] = K(5.5), [P_LP_CUT] = K(4.2),
        [P_LP_PEAK] = K(7.8), [P_LP_MG] = K(3), [P_LP_EG] = K(6.5), [P_MG_RATE] = K(1.8), [P_E1_ATK] = K(3.5),
        [P_E1_REL] = K(4.5), [P_E2_ATK] = K(1.8), [P_E2_DEC] = K(5.5), [P_E2_SUS] = K(5), [P_E2_REL] = K(4.5)},
        .cable = {{MS_JACK_OUT(SRC_EG1), MS_JACK_IN(DST_HP_CUT)}}},
    /* Aphex Twin - "Didgeridoo" (1992): dual-formant resonant MG-modulated acid drone/bass */
    {.name = "DIDGERIDOO", .p = {BASE, [P_V1WAVE] = W(2), [P_V1PW] = K(7.5), [P_V1SCALE] = W(0), [P_V2SCALE] = W(0),
        [P_MIX1] = K(9), [P_MIX2] = K(8), [P_HP_CUT] = K(3.2), [P_HP_PEAK] = K(8.2), [P_HP_MG] = K(5.5),
        [P_LP_CUT] = K(5.5), [P_LP_PEAK] = K(8.2), [P_LP_MG] = K(4.5), [P_MG_WAVE] = K(3.5), [P_MG_RATE] = K(4.2),
        [P_E2_REL] = K(2.5)}},
    /* Stereolab - "French Disko" (1993): nasal motorik pulse organ/lead */
    {.name = "STEREOLAB", .p = {BASE, [P_V1WAVE] = W(2), [P_V1PW] = K(6.5), [P_V2WAVE] = W(2), [P_V2SCALE] = W(2),
        [P_V2PITCH] = 65, [P_MIX1] = K(8.5), [P_MIX2] = K(7.5), [P_FM_MG] = K(1), [P_HP_CUT] = K(3.8),
        [P_HP_PEAK] = K(5.5), [P_LP_CUT] = K(8.2), [P_LP_PEAK] = K(4), [P_MG_RATE] = K(3.6), [P_E2_DEC] = 0,
        [P_E2_REL] = K(0.8)}},
    /* Portishead - "Sour Times" (1994): theremin-like triangle lead with delayed vibrato */
    {.name = "SOUR TIMES", .p = {BASE, [P_V1WAVE] = W(0), [P_PORTA] = K(5.2), [P_FM_MG] = K(2.2), [P_LP_CUT] = K(6.8),
        [P_LP_PEAK] = K(3.5), [P_MG_RATE] = K(3.2), [P_E1_DELAY] = K(2.5), [P_E1_ATK] = K(3.5), [P_E1_REL] = K(2),
        [P_E2_ATK] = K(2), [P_E2_SUS] = K(9), [P_E2_REL] = K(4.5)},
        .cable = {{MS_JACK_OUT(SRC_MG_TRI), MS_JACK_IN(DST_MVCA_IN)}, {MS_JACK_OUT(SRC_MVCA), MS_JACK_IN(DST_TOTAL)}}},
    /* The Prodigy - "Voodoo People" (1994): screaming rave acid lead */
    {.name = "VOODOO PPL", .p = {BASE, [P_V1WAVE] = W(2), [P_V1PW] = K(6.5), [P_V2PITCH] = 67, [P_MIX1] = K(10),
        [P_MIX2] = K(10), [P_PORTA] = K(2.8), [P_HP_CUT] = K(3), [P_HP_PEAK] = K(8), [P_LP_CUT] = K(5.2),
        [P_LP_PEAK] = K(8.5), [P_LP_EG] = K(7.5), [P_E2_DEC] = K(2.8), [P_E2_SUS] = K(4), [P_E2_REL] = K(1.8)}},
    /* Air - "Sexy Boy" (1998): fuzzy detuned Moon Safari riff */
    {.name = "SEXY BOY", .p = {BASE, [P_V1SCALE] = W(1), [P_V2PITCH] = 66, [P_MIX1] = K(10), [P_MIX2] = K(9),
        [P_PORTA] = K(1.5), [P_HP_CUT] = K(2.2), [P_HP_PEAK] = K(7), [P_LP_CUT] = K(6.5), [P_LP_PEAK] = K(7.5),
        [P_LP_EG] = K(4.5), [P_E2_DEC] = K(3.5), [P_E2_SUS] = K(6), [P_E2_REL] = K(2)}},
    /* Goldfrapp - "Strict Machine" (2003): driving electroclash sub-saw + pulse bass */
    {.name = "STRICT MCH", .p = {BASE, [P_V1SCALE] = W(0), [P_V2WAVE] = W(2), [P_V2SCALE] = W(0), [P_V2PITCH] = 65,
        [P_MIX1] = K(10), [P_MIX2] = K(9), [P_PORTA] = K(1.8), [P_HP_CUT] = K(2), [P_HP_PEAK] = K(7.5),
        [P_LP_CUT] = K(5.8), [P_LP_PEAK] = K(8), [P_LP_EG] = K(6.5), [P_E2_DEC] = K(3), [P_E2_SUS] = K(7),
        [P_E2_REL] = K(1.8)}},
};
static ms20_patch_t bank[NPRESET];
static struct { uint8_t oct, preset; } set = {0, 1};

/* Bank in flash (blob 0): format version followed by NPRESET patches. Format 3 = 26 presets, jack-pair cables. */
#define BANK_FORMAT 3u
static struct { uint32_t format; ms20_patch_t bank[NPRESET]; } store;
/* a saved bank's slot that still holds an empty placeholder ("INIT nn", INIT's knobs, no cables) takes the factory
 * preset that has since been put there (FLAT ALEX went into 16 so); a slot anyone changed and saved stays as it is */
static void bank_fill(void)
{
    static const uint8_t none[MS_NCABLE][2];
    uint32_t i;
    for (i = 1; i < NPRESET; i++)
        if (!memcmp(bank[i].name, "INIT ", 5) && !memcmp(bank[i].p, FACTORY[0].p, sizeof bank[i].p) &&
            !memcmp(bank[i].cable, none, sizeof none) && memcmp(bank[i].name, FACTORY[i].name, sizeof bank[i].name))
            memcpy(&bank[i], &FACTORY[i], sizeof bank[i]);
}
static int bank_load(void)
{
    uint32_t i, c;
    if (!blob_load(0, &store, sizeof store) && store.format == BANK_FORMAT) {
        memcpy(bank, store.bank, sizeof bank);
        bank_fill();
        return 0;
    }
    if (blob_load(0, bank, sizeof bank))
        return -1;
    for (i = 0; i < NPRESET; i++)
        for (c = 0; c < MS_NCABLE; c++) {
            uint32_t s0 = bank[i].cable[c][0], d0 = bank[i].cable[c][1];
            int ok = s0 && s0 < MS_NSRC && d0 && d0 < MS_NDST;
            bank[i].cable[c][0] = ok ? MS_JACK_OUT(s0) : 0u;
            bank[i].cable[c][1] = ok ? MS_JACK_IN(d0) : 0u;
        }
    bank_fill();
    return 0;
}
static void bank_save(void)
{
    store.format = BANK_FORMAT;
    memcpy(store.bank, bank, sizeof bank);
    blob_save(0, &store, sizeof store);
}

static void load_preset(uint32_t i)
{
    memcpy(&patch, &bank[i % NPRESET], sizeof patch);
    patch_commit();
}

/* ---- notes (main loop). The MS-20's keyboard is a chain of resistors read from its top end: the highest key held
 * is the one that counts, and its trigger is "a key is down", so playing legato does not start the envelopes again. */
#define NHELD 16u
static uint8_t held[NHELD], nheld, hold_on;
static int32_t bend;                            /* 1/256 semitone */
static uint32_t last_note = 255;

static void notes_update(void)
{
    uint32_t i;
    if (nheld) {
        last_note = held[0];
        for (i = 1; i < nheld; i++)
            if (held[i] > last_note)
                last_note = held[i];
        ctl.note = (int32_t)last_note * 256 + bend;
        ctl.gate = 1;
    } else {
        if (last_note < 128u)
            ctl.note = (int32_t)last_note * 256 + bend;
        if (!hold_on)
            ctl.gate = 0;
    }
}
static void play(uint32_t note)
{
    uint32_t i, j;
    if (note > 127u)
        return;
    for (i = j = 0; i < nheld; i++)             /* (the same key again: to the top) */
        if (held[i] != note)
            held[j++] = held[i];
    nheld = (uint8_t)j;
    if (nheld == NHELD) {
        for (i = 1; i < NHELD; i++)
            held[i - 1u] = held[i];
        nheld--;
    }
    held[nheld++] = (uint8_t)note;
    notes_update();
}
static void stop(uint32_t note)
{
    uint32_t i, j;
    for (i = j = 0; i < nheld; i++)
        if (held[i] != note)
            held[j++] = held[i];
    nheld = (uint8_t)j;
    notes_update();
}
static int quiet(void) { return !ctl.gate && !syn.e2_st; }

/* ---- the screen: a piece of the MS-20's front panel. White print on black, sections ruled off with their titles, a
 * knob with its scale for each of the FM-1's four knobs, red lamps. The pictures and the lettering are drawn ahead
 * (tools/gen_panel_art.py -> gen/panel_art.h); here, integer only, the pointer, the scales and the rules.
 *   y   0 .. 117   the page: titles, four knobs
 *   y 118 .. 213   a graph of what the page does (or, for a moment, the preset's name when it changes)
 *   y 216 .. 239   the lamps (MG, TRIG, ESP, PEAK, HOLD, EDIT), the octave, the wheel
 * The patch panel's page takes the graph's place for its eight cables. */
#include "panel_art.h"
#define C_INK RGB(228, 226, 218)                /* the panel's print */
#define C_INK2 RGB(138, 138, 132)               /* quieter print: values, hints */
#define C_RULE RGB(120, 120, 116)
#define C_SEL RGB(44, 44, 40)
#define C_OUT RGB(230, 159, 0)                  /* a graph's signal; IN: what went into that stage (Okabe-Ito orange and */
#define C_IN RGB(86, 180, 233)                  /* sky blue: told apart with any of the common colour blindnesses) */
#define C_LOW RGB(230, 50, 35)                  /* the battery, nearly empty (as a lit lamp) */
#define BAND_H 124u
#define COL_W 60
#define GRAPH_Y 124u
#define GRAPH_H 90u
#define STATUS_Y 216u
#define KNOB_Y 63                               /* the knobs' centers in the band */
static const uint16_t CABLE_C[MS_NCABLE] = {    /* the Mac panel's cable colours, slot by slot */
    RGB(242, 199, 31), RGB(217, 64, 46), RGB(46, 122, 217), RGB(64, 173, 79),
    RGB(237, 138, 36), RGB(153, 92, 212), RGB(43, 186, 196), RGB(232, 232, 222)};

static char *put_s(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_u(char *p, uint32_t v)
{
    char t[10];
    uint32_t n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

static void value_text(uint32_t pi, char *b)
{
    uint32_t v = patch.p[pi];
    const pinfo_t *f = &PINFO[pi];
    if (f->kind == K_ENUM) {
        *put_s(b, f->names[(v >> 5) & 3u]) = 0;
    } else if (f->kind == K_BI) {
        int32_t x = ((int32_t)v - 64) * 50 / 63;         /* -5.0 .. +5.0 */
        uint32_t a = (uint32_t)(x < 0 ? -x : x);
        *b++ = x < 0 ? '-' : '+';
        b = put_u(b, a / 10u);
        *b++ = '.';
        *put_u(b, a % 10u) = 0;
    } else {
        uint32_t x = (v * 100u + 63u) / 127u;            /* 0.0 .. 10.0 */
        b = put_u(b, x / 10u);
        *b++ = '.';
        *put_u(b, x % 10u) = 0;
    }
}

/* -- drawing: positions in 1/16 pixel ("q"), a pixel's coverage out of 16 (4 x 4 samples) */
static int32_t isin(int32_t d)                  /* degrees -> Q14 */
{
    d %= 360;
    if (d < 0) d += 360;
    return d <= 90 ? SIN_Q14[d] : d <= 180 ? SIN_Q14[180 - d] : d <= 270 ? -SIN_Q14[d - 180] : -SIN_Q14[360 - d];
}
static void polar(int32_t cx, int32_t cy, int32_t r, int32_t a, int32_t *x, int32_t *y)   /* a: clockwise from up */
{
    *x = cx + (r * isin(a)) / 16384;
    *y = cy - (r * isin(a + 90)) / 16384;
}
static void cv_mix(int32_t x, int32_t y, uint16_t c, uint32_t a)   /* c over the canvas, a / 16 of it */
{
    uint16_t *p;
    uint32_t o, r, g, b;
    y += cv_oy;
    if ((uint32_t)x >= cv_w || (uint32_t)y >= cv_h || !a)
        return;
    p = &cv_px[(uint32_t)y * cv_w + (uint32_t)x];
    if (a >= 16u) { *p = swap16(c); return; }
    o = swap16(*p);
    r = ((c >> 11) * a + (o >> 11) * (16u - a)) >> 4;
    g = (((c >> 5) & 63u) * a + ((o >> 5) & 63u) * (16u - a)) >> 4;
    b = ((c & 31u) * a + (o & 31u) * (16u - a)) >> 4;
    *p = swap16(r << 11 | g << 5 | b);
}
static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    for (; bit; bit >>= 2) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
    }
    return r;
}
/* a stroke from a to b (q), hw (q) either side of it, round ends */
static void cv_seg(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t hw, uint16_t c)
{
    int32_t dx = bx - ax, dy = by - ay, len = (int32_t)isqrt((uint32_t)(dx * dx + dy * dy)), ux, uy, x, y, i, j;
    int32_t x0 = ((ax < bx ? ax : bx) - hw) >> 4, x1 = ((ax > bx ? ax : bx) + hw) >> 4;
    int32_t y0 = ((ay < by ? ay : by) - hw) >> 4, y1 = ((ay > by ? ay : by) + hw) >> 4;
    ux = len ? dx * 1024 / len : 1024;
    uy = len ? dy * 1024 / len : 0;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            uint32_t n = 0;
            for (j = 0; j < 4; j++)
                for (i = 0; i < 4; i++) {
                    int32_t qx = x * 16 + 2 + 4 * i - ax, qy = y * 16 + 2 + 4 * j - ay;
                    int32_t t = (qx * ux + qy * uy) / 1024, e = (qx * uy - qy * ux) / 1024;
                    if (t < 0) e = (int32_t)isqrt((uint32_t)(qx * qx + qy * qy));
                    else if (t > len) { int32_t rx = qx - dx, ry = qy - dy; e = (int32_t)isqrt((uint32_t)(rx * rx + ry * ry)); }
                    if (e < 0) e = -e;
                    n += e <= hw;
                }
            cv_mix(x, y, c, n);
        }
}
static void cv_img(int32_t x, int32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            cv_mix(x + (int32_t)i, y + (int32_t)j, px[j * w + i], 16u);
}
static int32_t text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    return cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}
static void hline(int32_t x, int32_t y, int32_t w, uint16_t c) { cv_rect(x, y, w, 1, c); }
static void vline(int32_t x, int32_t y, int32_t h, uint16_t c) { cv_rect(x, y, 1, h, c); }

/* the switches' and the scales' little pictures, centred on (x, y) in q: as printed on the panel */
enum { M_TRI = 1, M_SAW, M_RAMP, M_SQR, M_PULSE, M_NARROW, M_NOISE, M_PWM };
static void mark(int32_t m, int32_t x, int32_t y)
{
    const int32_t w = 56, h = 42, s = 9;        /* half width, half height, stroke (q) */
    switch (m) {
    case M_TRI: cv_seg(x - w, y + h, x, y - h, s, C_INK); cv_seg(x, y - h, x + w, y + h, s, C_INK); break;
    case M_SAW: cv_seg(x - w, y + h, x - w, y - h, s, C_INK); cv_seg(x - w, y - h, x + w, y + h, s, C_INK); break;
    case M_RAMP: cv_seg(x - w, y + h, x + w, y - h, s, C_INK); cv_seg(x + w, y - h, x + w, y + h, s, C_INK); break;
    case M_SQR: case M_PULSE: case M_NARROW: {
        int32_t f = m == M_SQR ? 0 : m == M_PULSE ? -w * 5 / 10 : -w * 7 / 10;
        cv_seg(x - w, y + h, x - w, y - h, s, C_INK); cv_seg(x - w, y - h, x + f, y - h, s, C_INK);
        cv_seg(x + f, y - h, x + f, y + h, s, C_INK); cv_seg(x + f, y + h, x + w, y + h, s, C_INK);
        break;
    }
    case M_NOISE: {
        static const int8_t ys[8] = {0, -10, 6, -4, 10, -8, 3, 0};
        int32_t i;
        for (i = 0; i < 7; i++)
            cv_seg(x - w + i * 2 * w / 7, y + ys[i] * h / 10, x - w + (i + 1) * 2 * w / 7, y + ys[i + 1] * h / 10, s, C_INK);
        break;
    }
    case M_PWM:                                 /* a pulse whose width moves: an arrow across its top */
        mark(M_PULSE, x, y);
        cv_seg(x - w, y - h - 24, x + w, y - h - 24, 7, C_INK);
        cv_seg(x + w, y - h - 24, x + w - 20, y - h - 40, 7, C_INK);
        cv_seg(x - w, y - h - 24, x - w + 20, y - h - 40, 7, C_INK);
        break;
    }
}

/* -- the controls as the panel prints them */
enum { S_DIAL, S_BI, S_PW, S_MGWAVE, S_SW };    /* the scale around a knob */
typedef struct { const char *l1, *l2; uint8_t scale; int8_t marks[4]; } look_t;   /* marks: a picture, or -1..-4 a name */
static const look_t LOOK[MS_NPARAM] = {
    [P_V1WAVE] = {"WAVE FORM", 0, S_SW, {M_TRI, M_SAW, M_PWM, M_NOISE}}, [P_V1PW] = {"PW", 0, S_PW, {0}},
    [P_V1SCALE] = {"SCALE", 0, S_SW, {-1, -2, -3, -4}}, [P_PORTA] = {"TIME", 0, S_DIAL, {0}},
    [P_V2WAVE] = {"WAVE FORM", 0, S_SW, {M_SAW, M_SQR, M_PULSE, -4}}, [P_V2PITCH] = {"PITCH", 0, S_BI, {0}},
    [P_V2SCALE] = {"SCALE", 0, S_SW, {-1, -2, -3, -4}}, [P_TUNE] = {"", 0, S_BI, {0}},
    [P_MIX1] = {"VCO 1 LEVEL", 0, S_DIAL, {0}}, [P_MIX2] = {"VCO 2 LEVEL", 0, S_DIAL, {0}},
    [P_FM_MG] = {"MG/T.EXT", 0, S_DIAL, {0}}, [P_FM_EG] = {"EG1/EXT", 0, S_DIAL, {0}},
    [P_HP_CUT] = {"CUTOFF", "FREQUENCY", S_DIAL, {0}}, [P_HP_PEAK] = {"PEAK", 0, S_DIAL, {0}},
    [P_HP_MG] = {"MG/T.EXT", 0, S_DIAL, {0}}, [P_HP_EG] = {"EG2/EXT", 0, S_DIAL, {0}},
    [P_LP_CUT] = {"CUTOFF", "FREQUENCY", S_DIAL, {0}}, [P_LP_PEAK] = {"PEAK", 0, S_DIAL, {0}},
    [P_LP_MG] = {"MG/T.EXT", 0, S_DIAL, {0}}, [P_LP_EG] = {"EG2/EXT", 0, S_DIAL, {0}},
    [P_MG_WAVE] = {"WAVE FORM", 0, S_MGWAVE, {0}}, [P_MG_RATE] = {"FREQUENCY", 0, S_DIAL, {0}},
    [P_E1_DELAY] = {"DELAY TIME", 0, S_DIAL, {0}}, [P_E1_ATK] = {"ATTACK", "TIME", S_DIAL, {0}},
    [P_E1_REL] = {"RELEASE", "TIME", S_DIAL, {0}}, [P_E2_HOLD] = {"HOLD TIME", 0, S_DIAL, {0}},
    [P_E2_ATK] = {"ATTACK", "TIME", S_DIAL, {0}}, [P_E2_DEC] = {"DECAY", "TIME", S_DIAL, {0}},
    [P_E2_SUS] = {"SUSTAIN", "LEVEL", S_DIAL, {0}}, [P_E2_REL] = {"RELEASE", "TIME", S_DIAL, {0}},
    [P_VOLUME] = {"VOLUME", 0, S_DIAL, {0}}, [P_ESP_CV] = {"CV ADJUST", 0, S_BI, {0}},
    [P_ESP_LEVEL] = {"SIGNAL", "LEVEL", S_DIAL, {0}}, [P_ESP_LOW] = {"LOW CUT", "FREQ", S_DIAL, {0}},
    [P_ESP_HIGH] = {"HIGH CUT", "FREQ", S_DIAL, {0}}, [P_ESP_THR] = {"THRESHOLD", "LEVEL", S_DIAL, {0}},
};
/* each page's sections: a title (one or two lines, as the panel sets them) over n columns from column c */
typedef struct { const char *t1, *t2; uint8_t c, n; } sect_t;
static const sect_t SECT[NPAGE][4] = {
    {{"VOLTAGE CONTROLLED", "OSCILLATOR 1", 0, 3}, {"PORTAMENTO", 0, 3, 1}},
    {{"VOLTAGE CONTROLLED", "OSCILLATOR 2", 0, 3}, {"MASTER", "TUNE", 3, 1}},
    {{"VCO MIXER", 0, 0, 2}, {"FREQUENCY", "MODULATION", 2, 2}},
    {{"VOLTAGE CONTROLLED", "HIGHPASS FILTER", 0, 4}},
    {{"VOLTAGE CONTROLLED", "LOWPASS FILTER", 0, 4}},
    {{"MODULATION", "GENERATOR", 0, 2}, {"ENVELOPE", "GENERATOR 1", 2, 2}},
    {{"ENVELOPE", "GENERATOR 2", 0, 4}},
    {{"EG 1", 0, 0, 1}, {"EG 2", 0, 1, 1}, {"OUTPUT", 0, 2, 1}, {"F-V", "CONVERTER", 3, 1}},
    {{"EXTERNAL SIGNAL", "PROCESSOR", 0, 4}},
    {{"PATCH PANEL", 0, 0, 4}},
};

/* the battery, top right: three bars at the stock firmware's thresholds (ADC 531 / 561 / 591, not yet checked against
 * a meter), red with none left, a bolt while a USB host powers it (platform.c: batt_raw, batt_ext) */
static uint32_t batt_shown = 3u;                /* 0 .. 3 bars, 4 = USB power */
static uint32_t batt_now(void)
{
    static const int32_t th[3] = {531, 561, 591};
    uint32_t l = batt_shown > 3u ? 3u : batt_shown;
    if (batt_ext()) return 4u;
    if (!batt_raw) return batt_shown;           /* (not read yet) */
    while (l < 3u && batt_raw >= th[l] + 4) l++;   /* a level moves once the reading is 4 past it: the ADC wanders */
    while (l > 0u && batt_raw < th[l - 1u] - 4) l--;
    return l;
}
static void batt_paint(int32_t x0)              /* at x 221 .. 235, y 0 .. 6 of the screen */
{
    int32_t x = 221 - x0, i;
    uint16_t c = batt_shown == 0u ? C_LOW : C_INK;
    cv_rect(x, 0, 13, 1, c); cv_rect(x, 6, 13, 1, c); cv_rect(x, 0, 1, 7, c); cv_rect(x + 12, 0, 1, 7, c);
    cv_rect(x + 13, 2, 2, 3, c);
    if (batt_shown == 4u) {                     /* a bolt */
        int32_t q = x * 16;
        cv_seg(q + 128, 24, q + 84, 60, 10, C_OUT);
        cv_seg(q + 84, 60, q + 124, 56, 10, C_OUT);
        cv_seg(q + 124, 56, q + 80, 92, 10, C_OUT);
    } else {
        for (i = 0; i < (int32_t)batt_shown; i++) cv_rect(x + 2 + 3 * i, 2, 2, 3, c);
    }
}
static void draw_batt(void)
{
    cv_begin(16, 7, C_BLACK);
    batt_paint(221);
    cv_blit(221, 0);
}

/* the page's titles and rules, as seen through a canvas whose left edge is at x0 on the screen */
static void draw_frame(uint32_t pg, int32_t x0)
{
    uint32_t s;
    for (s = 0; s < 4u && SECT[pg][s].t1; s++) {
        const sect_t *t = &SECT[pg][s];
        int32_t x = (int32_t)t->c * COL_W - x0, w = (int32_t)t->n * COL_W, l;
        if (t->c)
            vline(x, 8, (int32_t)BAND_H - 9, C_RULE);
        for (l = 0; l < 2; l++) {
            const char *z = l ? t->t2 : t->t1;
            const felucca_font_t *f = text_w(&FONT_TTL, z ? z : "") > w - 6 ? &FONT_LBL : &FONT_TTL;   /* (a long one, smaller) */
            if (!z) break;
            if (w >= 2 * COL_W) cv_text(x + 5, 8 + 10 * l, f, z, C_INK);
            else text_c(x + w / 2, 8 + 10 * l, f, z, C_INK);
        }
    }
    if (pg != PG_PATCH) hline(-x0, (int32_t)BAND_H - 1, 240, C_RULE);
    batt_paint(x0);
    for (s = 0; s < NPAGE; s++) {               /* where we are: a dot for each page, this one drawn out long */
        int32_t cx = (120 - (int32_t)(NPAGE - 1u) * 5 + (int32_t)s * 10 - x0) * 16 + 8;
        if (s == pg) cv_seg(cx - 56, 3 * 16 + 8, cx + 56, 3 * 16 + 8, 22, C_INK);
        else cv_seg(cx, 3 * 16 + 8, cx, 3 * 16 + 8, 20, C_RULE);
    }
}

/* a knob and its scale, centred on (cx, cy) pixels in the canvas */
static void draw_knob(uint32_t pi, int32_t cx, int32_t cy)
{
    const look_t *lk = &LOOK[pi];
    int32_t qx = cx * 16 + 8, qy = cy * 16 + 8, v = patch.p[pi], a, i, x0, y0, x1, y1;
    char s[12];
    if (lk->scale == S_SW) {                    /* a four-way switch: a mark at each position */
        static const int16_t ang[4] = {-45, -15, 15, 45};
        for (i = 0; i < 4; i++) {
            polar(qx, qy, 248, ang[i], &x0, &y0);
            polar(qx, qy, 296, ang[i], &x1, &y1);
            cv_seg(x0, y0, x1, y1, 8, C_INK);
            polar(qx, qy, 26 * 16, ang[i], &x0, &y0);
            if (lk->marks[i] > 0) mark(lk->marks[i], x0, y0);
            else text_c(x0 >> 4, (y0 >> 4) - 4, &FONT_LBL, PINFO[pi].names[-lk->marks[i] - 1], C_INK);
        }
        a = ang[(v >> 5) & 3];
    } else {                                    /* a pot: ticks at 0 .. 10 over 300 degrees */
        for (i = 0; i <= 10; i++) {
            int32_t t = -150 + 30 * i;
            polar(qx, qy, 248, t, &x0, &y0);
            polar(qx, qy, 288, t, &x1, &y1);
            cv_seg(x0, y0, x1, y1, 7, C_INK);
            if (lk->scale == S_DIAL || lk->scale == S_BI) {
                char *p = s;
                if (lk->scale == S_DIAL) p = put_u(p, (uint32_t)i);
                else { if (i != 5) *p++ = i < 5 ? '-' : '+'; p = put_u(p, (uint32_t)(i < 5 ? 5 - i : i - 5)); }
                *p = 0;
                polar(qx, qy, lk->scale == S_BI ? 24 * 16 : 23 * 16, t, &x0, &y0);
                text_c(x0 >> 4, (y0 >> 4) - 3, &FONT_NUM, s, C_INK);
            }
        }
        if (lk->scale == S_PW) {
            polar(qx, qy, 25 * 16, -150, &x0, &y0); mark(M_SQR, x0 - 16, y0);
            polar(qx, qy, 25 * 16, 150, &x0, &y0); mark(M_NARROW, x0 + 16, y0);
        } else if (lk->scale == S_MGWAVE) {
            polar(qx, qy, 24 * 16, 0, &x0, &y0); mark(M_TRI, x0, y0);
            polar(qx, qy, 25 * 16, -150, &x0, &y0); mark(M_SAW, x0 - 16, y0);
            polar(qx, qy, 25 * 16, 150, &x0, &y0); mark(M_RAMP, x0 + 16, y0);
        }
        a = -150 + (v * 300 + 63) / 127;
    }
    cv_img(cx - KNOB_C, cy - KNOB_C, KNOB_N, KNOB_N, KNOB_PX);
    polar(qx, qy, 3 * 16, a, &x0, &y0);
    polar(qx, qy, (KNOB_R - 1) * 16, a, &x1, &y1);
    cv_seg(x0, y0, x1, y1, 13, C_INK);
}

static void draw_col(uint32_t pg, uint32_t k)   /* one knob's column of the band */
{
    uint32_t pi = PAGE[pg].p[k];
    const look_t *lk = &LOOK[pi];
    char v[12];
    int32_t y = KNOB_Y + 27;
    cv_begin(COL_W, BAND_H, C_BLACK);
    draw_frame(pg, (int32_t)k * COL_W);
    draw_knob(pi, COL_W / 2, KNOB_Y);
    text_c(COL_W / 2, y, &FONT_LBL, lk->l1, C_INK);
    if (lk->l2) text_c(COL_W / 2, y + 10, &FONT_LBL, lk->l2, C_INK);
    value_text(pi, v);
    text_c(COL_W / 2, y + 22, &FONT_LBL, v, C_INK2);
    cv_blit(k * (uint32_t)COL_W, 0);
}

static void draw_patch(uint32_t sel, int32_t y0, uint32_t h)   /* the patch panel: eight cables, the chosen one lit */
{
    uint32_t c;
    cv_begin(240, h, C_BLACK);                  /* (rows y0 .. y0 + h of it: a canvas holds 240 x 64) */
    cv_oy = -y0;
    draw_frame(PG_PATCH, 0);
    cv_text(236 - text_w(&FONT_LBL, "K1 CABLE  K2 K3 ENDS  K4 PULL"), 10, &FONT_LBL, "K1 CABLE  K2 K3 ENDS  K4 PULL", C_INK2);
    for (c = 0; c < MS_NCABLE; c++) {
        uint32_t a = patch.cable[c][0], b = patch.cable[c][1];
        int32_t y = 27 + (int32_t)c * 23, q = (y + 11) * 16 + 8;
        uint16_t col = a || b ? C_INK : C_INK2;
        if (c == sel) cv_rect(2, y, 236, 22, C_SEL);
        text_c(10, y + 7, &FONT_LBL, (const char[]){(char)('1' + c), 0}, C_INK2);
        if (a || b) cv_seg(20 * 16, q, 28 * 16, q, 48, CABLE_C[c]);   /* the cable's colour, as on the Mac panel */
        else cv_seg(20 * 16, q, 28 * 16, q, 10, C_RULE);
        cv_text(36, y + 7, &FONT_TTL, a ? jack_name(a) : b ? "LOOSE" : "-", a ? col : C_INK2);
        cv_seg(124 * 16, q, 138 * 16, q, 9, C_INK2);       /* the panel's arrow */
        cv_seg(138 * 16, q, 133 * 16, q - 56, 9, C_INK2);
        cv_seg(138 * 16, q, 133 * 16, q + 56, 9, C_INK2);
        cv_text(146, y + 7, &FONT_TTL, b ? jack_name(b) : a ? "LOOSE" : "-", b ? col : C_INK2);
    }
    cv_oy = 0;
    cv_blit(0, (uint32_t)y0);
}

static void draw_band(uint32_t pg, uint32_t sel, uint32_t cols)   /* cols: which knob columns (bit k), 15 = all */
{
    uint32_t k;
    if (pg == PG_PATCH) {                       /* (the patch panel takes the graph's place too) */
        for (k = 0; k < 4u; k++)
            draw_patch(sel, (int32_t)(k * (STATUS_Y / 4u)), STATUS_Y / 4u);
        return;
    }
    for (k = 0; k < 4u; k++)
        if (cols >> k & 1u)
            draw_col(pg, k);
}

/* ---- the graph under the knobs: what the page does. The oscillators', the mixer's, the filters' and the ESP's pages:
 * a scope on that signal, straight from the engine (syn.scope). The envelopes' and the MG's: their shapes, worked out
 * from the knobs with the engine's own resistors and capacitors (ms20.h), to scale. */
static int16_t gr_lo[2][240], gr_hi[2][240];    /* each trace's extent in each pixel column (y) */
static uint16_t gr_col[2];
static uint32_t gr_n;                           /* traces */
static const char *gr_l1, *gr_l2, *gr_cl, *gr_cr;   /* captions: top left, top middle; under the graph, left and right */
static uint16_t gr_ccl, gr_ccr;
static uint32_t gr_inline;                      /* the right caption follows the left one (not right-aligned) */
static const char *gr_c2;                       /* the second graph's caption, under its half (C_OUT) */
static char gr_r[24];
static int32_t gr_mark[4], gr_nmark;            /* x of the dashed lines (the key let go) */
static int32_t gr_mid;                          /* a scope's 0 V line, or -1 */
static int16_t snap[MS_SCOPE_N];

static void trace_pt(uint32_t t, int32_t x, int32_t y, int32_t py)   /* a curve's point, joined to the last one */
{
    if (x < 0 || x >= 240) return;
    gr_lo[t][x] = (int16_t)(y < py ? y : py);
    gr_hi[t][x] = (int16_t)(y > py ? y : py);
}
static void trace_clear(void)
{
    uint32_t t, x;
    for (t = 0; t < 2u; t++)
        for (x = 0; x < 240u; x++) gr_lo[t][x] = 1, gr_hi[t][x] = 0;   /* (lo > hi: nothing) */
    gr_n = 0; gr_nmark = 0; gr_mid = -1; gr_l1 = gr_l2 = gr_cl = gr_cr = gr_c2 = 0; gr_inline = 0; gr_r[0] = 0;
}

/* an RC's time constant in microseconds: a pot of rmax ohms on an A taper at knob step v, rmin in series, c in nF */
static uint32_t tau_us(uint32_t v, uint32_t rmax, uint32_t rmin, uint32_t c_nf)
{
    uint32_t r = (TAPER_Q16[v & 127u] * (rmax >> 8) >> 8) + rmin;   /* ohms (32 bits all the way: no 64-bit divide here) */
    return r * (c_nf / 10u) / 100u;
}
static int32_t decay_q12(uint32_t t, uint32_t tau)   /* exp(-t / tau), Q12 */
{
    uint32_t i, k, f;
    while (t > 8388607u) { t >>= 1; tau >>= 1; }   /* (t x 512 in 32 bits) */
    i = tau ? t * 512u / tau : 1u << 20;
    k = i >> 4; f = i & 15u;
    if (k >= 256u) return 0;
    return (int32_t)((EXP_Q12[k] * (16u - f) + EXP_Q12[k + 1u] * f) >> 4);
}
static char *put_time(char *p, uint32_t us)     /* "350 MS", "2.4 S" */
{
    if (us < 1000000u) { p = put_u(p, (us + 500u) / 1000u); return put_s(p, " MS"); }
    us = (us + 50000u) / 100000u;
    p = put_u(p, us / 10u);
    *p++ = '.';
    p = put_u(p, us % 10u);
    return put_s(p, " S");
}

/* EG2 (and its HOLD) across x0 .. x0 + w: attack to 5.07 V (it aims at 8.87 V), decay to SUSTAIN, the key held a while,
 * HOLD, release. Time runs to scale, but for the key held. */
static void graph_eg2(uint32_t t, int32_t x0, int32_t w, int32_t top, int32_t h, int with_hold)
{
    uint32_t ta = tau_us(patch.p[P_E2_ATK], 2000000u, 678u, 6800u), td = tau_us(patch.p[P_E2_DEC], 1000000u, 1400u, 6800u);
    uint32_t tr = tau_us(patch.p[P_E2_REL], 1000000u, 400u, 6800u), sus = patch.p[P_E2_SUS] * 4960u / 127u;   /* mV */
    uint32_t hold = with_hold ? TAPER_Q16[patch.p[P_E2_HOLD]] * (13862000u >> 8) >> 8 : 0u;
    uint32_t a = ta * 847u / 1000u, d = sus < 5070u ? 4u * td : 0u, r = 4u * tr, total = a + d + hold + r, us_px;
    int32_t plateau = w / 6, x, py = top + h, gx;
    us_px = total / (uint32_t)(w - plateau) + 1u;
    gx = x0 + (int32_t)((a + d) / us_px) + plateau;
    for (x = 0; x < w; x++) {
        uint32_t tt, mv;
        int32_t xr = x - (int32_t)((a + d) / us_px), y;
        if (xr < 0) {                           /* attack, decay */
            tt = (uint32_t)x * us_px;
            if (tt < a) mv = 8870u - (uint32_t)(8870 * decay_q12(tt, ta) >> 12);
            else mv = sus + (uint32_t)((int32_t)(5070u - sus) * decay_q12(tt - a, td) >> 12);
        } else if (xr < plateau) {
            mv = sus + (uint32_t)((int32_t)(5070u - sus) * decay_q12(d, td) >> 12);
        } else {                                /* the key let go: HOLD, then the release */
            uint32_t lv = sus + (uint32_t)((int32_t)(5070u - sus) * decay_q12(d, td) >> 12);
            tt = (uint32_t)(xr - plateau) * us_px;
            mv = tt < hold ? lv : (uint32_t)((int32_t)lv * decay_q12(tt - hold, tr) >> 12);
        }
        y = top + h - (int32_t)(mv > 5070u ? 5070u : mv) * h / 5070;
        trace_pt(t, x0 + x, y, x ? py : y);
        py = y;
    }
    gr_mark[gr_nmark++] = gx;
    *put_time(gr_r, total) = 0;
}
/* EG1 across x0 .. x0 + w: DELAY, an attack toward the top while the key is down, release; to scale but the key */
static void graph_eg1(uint32_t t, int32_t x0, int32_t w, int32_t top, int32_t h, int with_delay)
{
    uint32_t ta = tau_us(patch.p[P_E1_ATK], 2000000u, 400u, 3300u), tr = tau_us(patch.p[P_E1_REL], 2000000u, 1067u, 3300u);
    uint32_t dl = with_delay ? TAPER_Q16[patch.p[P_E1_DELAY]] * (9426000u >> 8) >> 8 : 0u;
    uint32_t a = 4u * ta, r = 4u * tr, total = dl + a + r, us_px, top_q12 = 4096u - (uint32_t)decay_q12(a, ta);
    int32_t plateau = w / 6, x, py = top + h;
    us_px = total / (uint32_t)(w - plateau) + 1u;
    for (x = 0; x < w; x++) {
        int32_t xr = x - (int32_t)((dl + a) / us_px), y;
        uint32_t q;
        if (xr < 0) {
            uint32_t tt = (uint32_t)x * us_px;
            q = tt < dl ? 0u : 4096u - (uint32_t)decay_q12(tt - dl, ta);
        } else if (xr < plateau) {
            q = top_q12;
        } else {
            q = (uint32_t)((int32_t)top_q12 * decay_q12((uint32_t)(xr - plateau) * us_px, tr) >> 12);
        }
        y = top + h - (int32_t)q * h / 4096;
        trace_pt(t, x0 + x, y, x ? py : y);
        py = y;
    }
    gr_mark[gr_nmark++] = x0 + (int32_t)((dl + a) / us_px) + plateau;
}
/* the MG: its integrator ramps up through one side of WAVE FORM and down through the other (TRI), and the Schmitt
 * trigger's square follows (RECT); two cycles across x0 .. x0 + w. Also its rate, in Hz. */
static void graph_mg(int32_t x0, int32_t w, int32_t top, int32_t h)
{
    uint32_t v = patch.p[P_MG_RATE], wv = patch.p[P_MG_WAVE];
    uint32_t lo = 48u + v * 10000u / 127u, hi = 2200u + (127u - v) * 10000u / 127u, rth = lo * hi / 12248u + 8200u;
    uint32_t up = wv * 1000000u / 127u + rth, tot = 1000000u + 2u * rth, cyc = (uint32_t)w / 2u, x, hz100;
    int32_t py = top + h, pr = 0, mid = top + h / 2;
    for (x = 0; x < (uint32_t)w; x++) {
        uint32_t ph = (x % cyc) * tot / cyc;   /* where in the cycle, in "ohms" */
        int32_t y = ph < up ? top + h - (int32_t)(ph * (uint32_t)h / up) : top + (int32_t)((ph - up) * (uint32_t)h / (tot - up));
        int32_t yr = ph < up ? mid - h / 4 : mid + h / 4;
        trace_pt(0, x0 + (int32_t)x, y, x ? py : y);
        trace_pt(1, x0 + (int32_t)x, yr, x ? pr : yr);
        py = y;
        pr = yr;
    }
    hz100 = 100000000u / ((tot / 10u) * 4899u / lo);   /* a cycle: 489.9 us x tot / lo */
    {
        char *p = put_s(gr_r, "");
        p = put_u(p, hz100 / 100u);
        *p++ = '.';
        p = put_u(p, hz100 / 10u % 10u);
        *put_s(p, " HZ") = 0;
    }
}

/* the scope: two cycles of the page's signal, held still by the oscillator it comes from. The engine marks where in
 * its record that VCO began each cycle and how fast its phase turns (syn.scope_wrap, scope_inc), as a scope's external
 * trigger would: no guessing from zero crossings, which a resonant filter or two VCOs at once make many of. The
 * sample divider is set from that same period, so two cycles fill 240 .. 960 samples. A second, dimmer trace: what
 * went into the stage (the filters' pages). fs: full scale of each, mV */
static uint32_t graph_scope(int32_t top, int32_t h, const int32_t fs[2], uint32_t trig)
{
    uint32_t w = syn.scope_w, wn = syn.scope_wn, inc = syn.scope_inc, n = MS_SCOPE_N, i, t, x;
    uint32_t div = syn.scope_div ? syn.scope_div : 1u, span = n, start = 0, per, nd = 1;
    int32_t mid = top + h / 2;
    if (trig && inc) {
        per = 0xFFFFFFFFu / inc;                /* a cycle, in samples at 88.2 kHz */
        while (2u * per / nd > 960u && nd < 256u) nd *= 2u;
        if (nd != div) { syn.scope_div = (uint16_t)nd; return 0; }   /* (the record changes pace: next time) */
        span = 2u * per / div;
        if (span < 8u) span = 8u;
        if (span > n) span = n;
        for (i = 0; i < 8u && i < wn; i++) {   /* the newest cycle start with two cycles recorded after it */
            uint32_t s0 = syn.scope_wrap[(wn - 1u - i) & 7u];
            if (w - s0 <= n && w - s0 >= span) { start = n - (w - s0); break; }
        }
        {
            uint32_t hz10 = ((inc >> 12) * 861u) >> 10;   /* 88200 x inc / 2^32, x 10 */
            char *q = put_u(gr_r, hz10 / 10u);
            *q++ = '.';
            q = put_u(q, hz10 % 10u);
            *put_s(q, " HZ") = 0;
        }
    } else {
        syn.scope_div = 4u;                     /* (nothing to hold it by: the last 46 ms as they come) */
        start = 0;
        span = n;
    }
    gr_mid = mid;
    for (t = 0; t < 2u; t++) {
        if (!fs[t]) continue;
        for (i = 0; i < n; i++)
            snap[i] = syn.scope[t][(w + i) & (n - 1u)];   /* the oldest first */
        for (x = 0; x < 240u; x++) {
            uint32_t i0 = start + x * span / 240u, i1 = start + (x + 1u) * span / 240u, j;
            int32_t mn, mx;
            if (i1 <= i0) i1 = i0 + 1u;
            if (i1 > n) i1 = n;
            mn = mx = i0 ? snap[i0 - 1u] : snap[0];
            for (j = i0; j < i1; j++) { if (snap[j] < mn) mn = snap[j]; if (snap[j] > mx) mx = snap[j]; }
            mn = mid - mn * (h / 2) / fs[t];
            mx = mid - mx * (h / 2) / fs[t];
            gr_lo[t][x] = (int16_t)(mx < top ? top : mx > top + h ? top + h : mx);
            gr_hi[t][x] = (int16_t)(mn > top + h ? top + h : mn < top ? top : mn);
        }
    }
    gr_col[0] = C_OUT;
    gr_col[1] = C_IN;
    gr_n = fs[1] ? 2u : 1u;
    return 1;
}

/* each page's scope: OUT, the signal (C_OUT), and IN, what went into that stage (C_IN), their full scales (mV) and
 * captions. (The filters work at a few hundred millivolts inside the KORG35: their scale is twenty times finer.) */
static const struct { uint8_t src[2]; uint16_t fs[2]; const char *name, *scale, *out, *in; } SCOPE[NPAGE] = {
    {{SRC_VCO1, 0}, {5000, 0}, "VCO 1", "+-5 V", "VCO-1 OUT", 0},
    {{SRC_VCO2, 0}, {5000, 0}, "VCO 2", "+-5 V", "VCO-2 OUT", 0},
    {{MS_SCOPE_MIX, 0}, {5000, 0}, "MIXER", "+-5 V", "VCO 1 + VCO 2 -> FILT INPUT", 0},
    {{MS_SCOPE_HP, MS_SCOPE_MIX}, {250, 5000}, "HIGH PASS", "+-0.25 V", "OUT: HP OUTPUT", "IN: FILT INPUT"},
    {{MS_SCOPE_LP, MS_SCOPE_HP}, {250, 250}, "LOW PASS", "+-0.25 V", "OUT: LP OUTPUT", "IN: HP OUTPUT"},
    {{0, 0}, {0, 0}, 0, 0, 0, 0}, {{0, 0}, {0, 0}, 0, 0, 0, 0}, {{0, 0}, {0, 0}, 0, 0, 0, 0},
    {{SRC_ESP_BPF, 0}, {5000, 0}, "ESP", "+-5 V", "ESP BAND PASS OUT", 0},
    {{0, 0}, {0, 0}, 0, 0, 0, 0},
};
#define GRAPH_TOP 11                            /* the traces' area in the strip */
#define GRAPH_HT 64

/* the oscillator a page's scope holds still by: its own on the VCO pages; downstream, VCO 1 unless it is not heard */
static uint32_t scope_trig(uint32_t pg)
{
    if (pg == 0u) return 1u;
    if (pg == 1u) return 2u;
    if (pg >= 2u && pg <= 4u) return patch.p[P_MIX1] || !patch.p[P_MIX2] ? 1u : 2u;
    return 0u;
}

static uint32_t graph_compute(uint32_t pg)      /* 0: nothing new to show */
{
    trace_clear();
    if (SCOPE[pg].src[0]) {
        int32_t fs[2] = {SCOPE[pg].fs[0], SCOPE[pg].fs[1]};
        gr_l1 = SCOPE[pg].name;
        gr_l2 = SCOPE[pg].scale;
        if (SCOPE[pg].in) {                     /* IN on the left, OUT on the right; one trace: its name on the left */
            gr_cl = SCOPE[pg].in; gr_ccl = C_IN;
            gr_cr = SCOPE[pg].out; gr_ccr = C_OUT;
        } else {
            gr_cl = SCOPE[pg].out; gr_ccl = C_OUT;
        }
        return graph_scope(GRAPH_TOP, GRAPH_HT, fs, scope_trig(pg));
    } else if (pg == 5u) {                      /* MG / EG 1 */
        gr_l1 = "MG: TRI AND RECT";
        graph_mg(4, 112, GRAPH_TOP + 4, GRAPH_HT - 8);
        gr_col[0] = C_OUT; gr_col[1] = C_IN; gr_n = 2;
        gr_l2 = "EG 1";
        graph_eg1(0, 128, 108, GRAPH_TOP, GRAPH_HT, 1);
        gr_cl = "MG TRI"; gr_ccl = C_OUT;
        gr_cr = "MG RECT"; gr_ccr = C_IN; gr_inline = 1;
        gr_c2 = "EG-1 OUT";
    } else if (pg == 6u) {                      /* EG 2 */
        gr_l1 = "EG 2";
        graph_eg2(0, 4, 232, GRAPH_TOP, GRAPH_HT, 1);
        gr_col[0] = C_OUT; gr_n = 1;
        gr_cl = "EG-2 OUT"; gr_ccl = C_OUT;
        gr_cr = "- - -  KEY UP"; gr_ccr = C_INK2;
    } else if (pg == 7u) {                      /* MORE: both envelopes with DELAY and HOLD */
        gr_l1 = "EG 1";
        graph_eg1(0, 4, 112, GRAPH_TOP, GRAPH_HT, 1);
        gr_l2 = "EG 2";
        graph_eg2(1, 124, 112, GRAPH_TOP, GRAPH_HT, 1);
        gr_col[0] = gr_col[1] = C_OUT; gr_n = 2;
        gr_r[0] = 0;
        gr_cl = "EG-1 OUT"; gr_ccl = C_OUT;
        gr_c2 = "EG-2 OUT";
    }
    return 1;
}
static void graph_paint(int32_t y0, uint32_t h)   /* rows y0 .. y0 + h of the strip */
{
    uint32_t t, x;
    int32_t i;
    cv_begin(240, h, C_BLACK);
    cv_oy = -y0;
    if (gr_mid >= 0) {                          /* the scope's graticule: 0 V, and dots at the quarters */
        for (x = 0; x < 240u; x += 4u) cv_rect((int32_t)x, gr_mid, 1, 1, C_RULE);
        for (i = 1; i < 4; i++)
            for (x = 0; x <= (uint32_t)GRAPH_HT; x += 4u) cv_rect(i * 60, GRAPH_TOP + (int32_t)x, 1, 1, C_SEL);
    }
    for (i = 0; i < gr_nmark; i++)              /* the key let go */
        for (x = 0; x <= (uint32_t)GRAPH_HT; x += 3u) cv_rect(gr_mark[i], GRAPH_TOP + (int32_t)x, 1, 1, C_INK2);
    for (t = gr_n; t-- > 0;)
        for (x = 0; x < 240u; x++)
            if (gr_lo[t][x] <= gr_hi[t][x])
                cv_rect((int32_t)x, gr_lo[t][x], 1, gr_hi[t][x] - gr_lo[t][x] + 1, gr_col[t]);
    if (gr_l1) cv_text(4, 0, &FONT_LBL, gr_l1, C_INK2);
    if (gr_l2) cv_text(gr_mid >= 0 ? 8 + text_w(&FONT_LBL, gr_l1) : 128, 0, &FONT_LBL, gr_l2, gr_mid >= 0 ? C_RULE : C_INK2);
    if (gr_cl) cv_text(4, GRAPH_TOP + GRAPH_HT + 4, &FONT_NUM, gr_cl, gr_ccl);
    if (gr_c2) cv_text(128, GRAPH_TOP + GRAPH_HT + 4, &FONT_NUM, gr_c2, C_OUT);
    if (gr_cr)                                  /* (the MG's RECT: beside TRI, under its own half) */
        cv_text(gr_inline ? 10 + text_w(&FONT_NUM, gr_cl) : 236 - text_w(&FONT_NUM, gr_cr), GRAPH_TOP + GRAPH_HT + 4, &FONT_NUM, gr_cr, gr_ccr);
    if (gr_r[0]) cv_text(236 - text_w(&FONT_LBL, gr_r), 0, &FONT_LBL, gr_r, C_INK2);
    cv_oy = 0;
    cv_blit(0, GRAPH_Y + (uint32_t)y0);
}
/* the preset's name, for a moment, where the graph goes */
static void flash_paint(const char *what, int32_t y0, uint32_t h)
{
    char s[24], *p = s;
    uint32_t i;
    cv_begin(240, h, C_BLACK);
    cv_oy = -y0;
    text_c(120, 18, &FONT_TTL, what, C_INK2);
    if (set.preset + 1u < 10u) *p++ = '0';
    p = put_u(p, set.preset + 1u);
    *p++ = ' ';
    for (i = 0; i < sizeof patch.name && patch.name[i]; i++) *p++ = patch.name[i];
    *p = 0;
    text_c(120, 38, &FONT_BIG, s, C_INK);
    cv_oy = 0;
    cv_blit(0, GRAPH_Y + (uint32_t)y0);
}
static void draw_graph(uint32_t pg, const char *flash)
{
    if (pg == PG_PATCH) return;
    if (flash) { flash_paint(flash, 0, GRAPH_H / 2u); flash_paint(flash, GRAPH_H / 2u, GRAPH_H / 2u); return; }
    if (!graph_compute(pg)) return;
    graph_paint(0, GRAPH_H / 2u);
    graph_paint(GRAPH_H / 2u, GRAPH_H / 2u);
}

/* ---- the status line: the lamps (the engine's four, HOLD, EDITED), the octave, the wheel */
static const char *const LAMP_NAME[6] = {"MG", "TRIG", "ESP", "PEAK", "HOLD", "EDIT"};
#define LAMP_X(i) (14 + 26 * (int32_t)(i))
#define LAMP_Y (STATUS_Y + 3u)
static void draw_lamp(uint32_t i, uint32_t on)
{
    cv_begin(LAMP_N, LAMP_N, C_BLACK);
    cv_img(0, 0, LAMP_N, LAMP_N, LAMP_PX + (on ? LAMP_N * LAMP_N : 0));
    cv_blit((uint32_t)(LAMP_X(i) - LAMP_N / 2), LAMP_Y);
}
static void draw_status(uint32_t lamps, uint32_t pct)
{
    char s[16], *p = s;
    int32_t oct = (int8_t)set.oct, i, wx;
    cv_begin(240, 240u - STATUS_Y + 1u, C_BLACK);
    cv_oy = 1;                                  /* (from the rule above it, at STATUS_Y - 1) */
    hline(0, -1, 240, C_RULE);
    for (i = 0; i < 6; i++) {
        cv_img(LAMP_X(i) - LAMP_N / 2, 3, LAMP_N, LAMP_N, LAMP_PX + (lamps >> i & 1u ? LAMP_N * LAMP_N : 0));
        text_c(LAMP_X(i), 14, &FONT_NUM, LAMP_NAME[i], C_INK);
    }
    text_c(176, 2, &FONT_NUM, pct >= 80u ? "CPU" : "OCT", C_INK);
    if (pct >= 80u) { p = put_u(p, pct); *p++ = '%'; }
    else { *p++ = oct < 0 ? '-' : oct > 0 ? '+' : ' '; *p++ = (char)('0' + (oct < 0 ? -oct : oct)); }
    *p = 0;
    text_c(176, 11, &FONT_TTL, s, C_INK);
    text_c(215, 2, &FONT_NUM, "WHEEL", C_INK);
    for (i = -2; i <= 2; i++) vline(215 + i * 9, i ? 18 : 16, i ? 3 : 5, C_INK2);
    hline(197, 20, 37, C_RULE);
    wx = 215 + ctl.wheel * 18 / 127;
    cv_rect(wx < 215 ? wx : 215, 13, (wx < 215 ? 215 - wx : wx - 215) + 1, 2, C_INK);
    cv_rect(wx - 1, 11, 3, 6, C_INK);
    cv_oy = 0;
    cv_blit(0, STATUS_Y - 1u);
}

/* ---- MIDI in */
static uint32_t idle_ok, edited, midi_edit;

static void midi_in(void)
{
    uint8_t st, d1, d2;
    while (midi_get(&st, &d1, &d2) != 0) {
        uint32_t ty = st & 0xF0u;
        if (st >= 0xF0u)
            continue;
        if (ty == 0x90u && d2) play(d1);
        else if (ty == 0x80u || ty == 0x90u) stop(d1);
        else if (ty == 0xE0u) {                  /* the bend: +-2 semitones */
            bend = ((int32_t)(d1 | (uint32_t)d2 << 7) - 8192) / 16;
            notes_update();
        } else if (ty == 0xB0u) {
            if (d1 == 1u) { ctl.wheel = d2; midi_edit |= 0x100u; }
            else if (d1 == 2u) { ctl.wheel = ((int32_t)d2 - 64) * 2; midi_edit |= 0x100u; }   /* CC 2: the whole wheel, 64 = 0 V */
            else if (d1 == 64u) ctl.button = d2 >= 64u;
            else if (d1 >= 16u && d1 < 16u + MS_NPARAM) { patch.p[d1 - 16u] = d2 & 127u; patch_commit(); midi_edit = (midi_edit & 0x100u) | 1u; }
            else if (d1 == 123u) { nheld = 0; hold_on = 0; notes_update(); }
        } else if (ty == 0xC0u && d1 < NPRESET) {
            midi_edit = (midi_edit & 0x100u) | (2u + d1);   /* a program change: the preset */
        }
    }
}
/* the panel on the computer patching: a whole new set of cables (ms20_link.h); as a knob moved over MIDI would be */
static int app_sysex(const uint8_t *p, uint32_t n)
{
    if (n < 6u || p[0] != LINK_ID || p[5] != LINK_CABLES)
        return 0;                               /* not ours: the update code's, or nobody's */
    if (!link_cables_take(p, n, patch.cable)) {
        patch_commit();
        midi_edit = (midi_edit & 0x100u) | 1u;  /* (the main loop redraws and marks the sound edited) */
    }
    return 1;
}
static void app_idle(void)
{
    if (idle_ok)
        midi_in();
}

static void turn(uint32_t pi, int32_t e)        /* a knob: e detents */
{
    int32_t v = patch.p[pi];
    if (PINFO[pi].kind == K_ENUM) {
        v = (v >> 5) + e;
        v = v < 0 ? 0 : v > 3 ? 3 : v;
        v = W(v);
    } else {
        v += e * (e > 2 || e < -2 ? 4 : 2);     /* a fast turn goes further */
        v = v < 0 ? 0 : v > 127 ? 127 : v;
    }
    patch.p[pi] = (uint8_t)v;
    patch_commit();
}

/* ---- the panel link (ms20_link.h): the patch to a computer, one way. STATE when something changes (at most every
 * 30 ms while a knob turns) and every second; LIVE (the key, the wheel, the switch, the lamps) when it changes, at most
 * every 33 ms, and every second. A message the USB cannot take now is tried again on the next pass. */
static void link_service(uint32_t now)
{
    static ms20_patch_t sent;
    static link_meta_t sent_m;
    static link_live_t sent_l;
    static uint32_t t_state, t_live, beat_state, beat_live, primed;
    uint8_t b[LINK_MAX];
    link_meta_t m;
    link_live_t l;
    m.preset = set.preset;
    m.flags = (uint8_t)((hold_on ? 1u : 0u) | (edited ? 2u : 0u) | LINK_TAKES_CABLES);
    m.octave = (uint8_t)((int8_t)set.oct + 3);
    l.gate = (uint8_t)(ctl.gate != 0u);
    l.note = (uint8_t)(last_note < 128u ? last_note : 127u);
    l.wheel = ctl.wheel;
    l.button = (uint8_t)(ctl.button != 0u);
    l.lamps = syn.lamp;
    if (!primed || memcmp(&sent, &patch, sizeof patch) || memcmp(&sent_m, &m, sizeof m)) {
        if ((!primed || now - t_state >= 30u) && !sysex_send(b, link_state(b, &patch, &m))) {
            memcpy(&sent, &patch, sizeof patch);
            sent_m = m;
            t_state = beat_state = now;
            primed = 1;
        }
    } else if (now - beat_state >= 1000u && !sysex_send(b, link_state(b, &patch, &m))) {
        beat_state = now;
    }
    if (memcmp(&sent_l, &l, sizeof l)) {
        if (now - t_live >= 33u && !sysex_send(b, link_live(b, &l))) {
            sent_l = l;
            t_live = beat_live = now;
        }
    } else if (now - beat_live >= 1000u && !sysex_send(b, link_live(b, &l))) {
        beat_live = now;
    }
}

/* the patch page's KNOB 2 / KNOB 3: one end of the chosen cable to the next jack (any jack, or none: a loose end). A jack
 * takes one plug, so one already in it comes out (that cable keeps its other end, loose) */
static void plug_end(uint32_t c, uint32_t end, int32_t e)
{
    uint32_t j = (uint32_t)((int32_t)patch.cable[c][end] + e + 4 * (int32_t)MS_NJACK) % MS_NJACK, k, m;
    if (j && j == patch.cable[c][end ^ 1u])          /* (not both ends in one jack: step past it) */
        j = (uint32_t)((int32_t)j + (e > 0 ? 1 : -1) + (int32_t)MS_NJACK) % MS_NJACK;
    patch.cable[c][end] = (uint8_t)j;
    for (k = 0; k < MS_NCABLE; k++)
        for (m = 0; m < 2u; m++)
            if (j && (k != c || m != end) && patch.cable[k][m] == j)
                patch.cable[k][m] = 0;
    patch_commit();
}

/* the splash (assets/splash.png through tools/gen_panel_art.py): runs of a palette colour, drawn 48 rows at a time */
static void draw_splash(void)
{
    uint32_t r = 0, left = 0, y, i;
    uint16_t c = 0;
    for (y = 0; y < 240u; y += 48u) {
        cv_begin(240, 48, C_BLACK);
        for (i = 0; i < 240u * 48u; i++) {
            if (!left) { c = swap16(SPLASH_PAL[SPLASH_RLE[r] & 31u]); left = SPLASH_RLE[r + 1u] + 1u; r += 2u; }
            cv_px[i] = c;
            left--;
        }
        cv_blit(0, y);
    }
}

#define KEY0 53u                                /* key 0 = F3 */
/* fm1-dock: app_main's loop-wide variables, for app_init and app_loop (modules/flat20/take.py) */
static uint32_t prev = 0, k, page = 4, sel = 0, redraw = 7, t_change = 0, set_dirty = 0, bank_dirty = 0, t_cpu = 0, pct_drawn = ~0u;
static uint32_t lamps_drawn = ~0u, lamps, pct = 0, t_graph = 0, flash_until = 0;
static const char *flash = 0;
static uint32_t splash = 1, t_boot, page0, preset0;
static int32_t wheel0;
static uint32_t rec_midi = 0;
static uint8_t key_note[27] = {0};
static void app_init(void)                      /* fm1-dock: app_main up to its loop */
{
    if (settings_load(&set, sizeof set) || (int8_t)set.oct < -3 || (int8_t)set.oct > 3 || set.preset >= NPRESET)
        set.oct = 0, set.preset = 1;
    if (bank_load())
        memcpy(bank, FACTORY, sizeof bank);
    load_preset(set.preset);
    draw_splash();                              /* (it stays until the main loop's first draw: see `splash` below) */
    t_boot = fm1_ms;                            /* (after the saved settings: their preset is not a change) */
    page0 = page;
    preset0 = set.preset;
    wheel0 = ctl.wheel;
}

static void app_loop(void)                      /* fm1-dock: a pass of app_main's loop, from the host's draw() */
{
    do {
        uint32_t keys, down, up, rel, b;
        int32_t oct = (int8_t)set.oct, e;
        uint32_t base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        platform_poll();

        keys = fm1_in.notes;
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {
            if (down >> k & 1u) { key_note[k] = (uint8_t)(base + k); play(key_note[k]); midi_send(0x90u, key_note[k], 100); }
            if (up >> k & 1u) { stop(key_note[k]); midi_send(0x80u, key_note[k], 0); }
        }

        b = fm1_input_edges(&rel);
        if (b & (1u << BTN_OCT_DOWN) && oct > -3) { set.oct = (uint8_t)(oct - 1); set_dirty = 1; t_change = fm1_ms; redraw |= 8u; }
        if (b & (1u << BTN_OCT_UP) && oct < 3) { set.oct = (uint8_t)(oct + 1); set_dirty = 1; t_change = fm1_ms; redraw |= 8u; }
        if (b & (1u << BTN_HOME)) { page = page == 0u ? 1u : page == 1u ? 2u : 0u; redraw |= 3u; }
        if (b & (1u << BTN_FX)) { page = page == 4u ? 3u : 4u; redraw |= 3u; }
        if (b & (1u << BTN_ENV)) { page = page == 6u ? 7u : 6u; redraw |= 3u; }
        if (b & (1u << BTN_LFO)) { page = 5u; redraw |= 3u; }
        if (b & (1u << BTN_EDIT)) { page = page == PG_PATCH ? 8u : PG_PATCH; redraw |= 3u; }
        if (b & (1u << BTN_PLAY)) { hold_on = !hold_on; notes_update(); }
        if (b & (1u << BTN_SAVE)) {
            memcpy(&bank[set.preset], &patch, sizeof patch);
            bank_dirty = 1;
            edited = 0;
            t_change = fm1_ms;
            flash = "SAVED"; flash_until = fm1_ms + 1500u; redraw |= 16u;
        }
        if ((b | rel) & (1u << BTN_REC))        /* (CC 64 does the same from outside) */
            ctl.button = (fm1_in.buttons >> BTN_REC) & 1u;
        (void)rec_midi;

        if ((e = fm1_enc_take(ENC_SELECT)) != 0) {
            page = (uint32_t)((int32_t)page + e + 4 * (int32_t)NPAGE) % NPAGE;
            redraw |= 3u;
        }
        e = fm1_enc_take(ENC_PRESETS);
        if ((midi_edit & 0xFFu) >= 2u) {         /* a program change */
            e = (int32_t)((midi_edit & 0xFFu) - 2u) - (int32_t)set.preset;
            if (!e) { load_preset(set.preset); edited = 0; flash = "PROGRAM"; flash_until = fm1_ms + 1500u; redraw |= 2u; }
            midi_edit &= 0x100u;
        }
        if (e != 0) {
            int32_t p = (int32_t)set.preset + e;
            set.preset = (uint8_t)(p < 0 ? 0 : p >= (int32_t)NPRESET ? (int32_t)NPRESET - 1 : p);
            load_preset(set.preset);
            edited = 0;
            set_dirty = 1;
            t_change = fm1_ms;
            flash = "PROGRAM"; flash_until = fm1_ms + 1500u; redraw |= 2u;
        }
        if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) {   /* the wheel: -5 .. +5 V */
            int32_t wv = ctl.wheel + e * (e > 1 || e < -1 ? 8 : 4);   /* 32 detents each way; a fast turn goes further */
            ctl.wheel = wv < -127 ? -127 : wv > 127 ? 127 : wv;
            redraw |= 8u;
        }
        for (k = 0; k < 4u; k++) {
            if ((e = fm1_enc_take(ENC_KNOB1 + k)) == 0)
                continue;
            if (page != PG_PATCH) {
                turn(PAGE[page].p[k], e);
                flash = 0;                      /* (on with the work: the name gives way to the graph) */
                edited = 1;
                redraw |= 0x100u << k;        /* (just that knob's column) */
                continue;
            } else if (k == 0u) {
                sel = (uint32_t)((int32_t)sel + e + 64) % MS_NCABLE;
                redraw |= 2u;
                continue;
            } else if (k == 1u) {
                plug_end(sel, 0u, e);
            } else if (k == 2u) {
                plug_end(sel, 1u, e);
            } else if (e < 0) {
                patch.cable[sel][0] = patch.cable[sel][1] = 0;
                patch_commit();
            } else {
                continue;
            }
            edited = 1;
            redraw |= 2u;
        }

        midi_in();
        if (midi_edit & 0x100u) { midi_edit &= ~0x100u; redraw |= 8u; }
        if (midi_edit == 1u) {                   /* a CC moved a knob */
            midi_edit = 0;
            edited = 1;
            redraw |= 2u;
        }

        for (k = 0; k < 27u; k++)
            fm1_led_key(KEY_LED(k), ctl.gate && last_note == base + k);
        fm1_led_key(BTN_OCT_DOWN, oct < 0);
        fm1_led_key(BTN_OCT_UP, oct > 0);
        fm1_led_key(BTN_HOME, page < 3u);
        fm1_led_key(BTN_FX, page == 3u || page == 4u);
        fm1_led_key(BTN_LFO, page == 5u);
        fm1_led_key(BTN_ENV, page == 6u || page == 7u);
        fm1_led_key(BTN_EDIT, page >= 8u);
        fm1_led_key(BTN_PLAY, hold_on);
        fm1_led_key(BTN_REC, ctl.button);
        fm1_led_key(BTN_SAVE, edited);

        link_service(fm1_ms);
        syn.scope_src[1] = page == PG_PATCH ? 0u : SCOPE[page].src[1];
        syn.scope_src[0] = page == PG_PATCH ? 0u : SCOPE[page].src[0];
        syn.scope_trig = (uint8_t)scope_trig(page);
        if (splash && (fm1_ms - t_boot >= 1500u || keys || b || page != page0 || set.preset != preset0 ||
                       ctl.wheel != wheel0 || edited)) {
            splash = 0;
            lcd_fill(0, 0, 240, 240, C_BLACK);
            redraw |= 0x1Fu;
            lamps_drawn = ~0u;
        }
        if (splash) {                           /* (what changes meanwhile is drawn when it goes) */
            if (flash) flash_until = fm1_ms + 1500u;
            goto drawn;
        }
        idle_ok = 1;
        if (redraw & 3u) draw_band(page, sel, 15u);
        else if (redraw >> 8 & 15u) draw_band(page, sel, redraw >> 8 & 15u);
        if (redraw & 0xF03u) redraw |= 16u;     /* (the shapes follow the knobs) */
        if (flash && (int32_t)(fm1_ms - flash_until) >= 0) { flash = 0; redraw |= 16u; }
        if (!flash && SCOPE[page].src[0] && fm1_ms - t_graph >= 80u) redraw |= 16u;   /* a scope: some 12 a second */
        if (redraw & 16u) { draw_graph(page, flash); t_graph = fm1_ms; }
        if ((k = batt_now()) != batt_shown) { batt_shown = k; draw_batt(); }
        lamps = (syn.lamp & 15u) | (hold_on ? 16u : 0u) | (edited ? 32u : 0u);
        if (fm1_ms - t_cpu > 500u) {             /* the render's worst time in the last half second, of its 5.8 ms */
            pct = cpu_peak / 58u;
            cpu_peak = 0;
            t_cpu = fm1_ms;
        }
        if ((redraw & 8u) || lamps_drawn == ~0u || (pct >= 80u) != (pct_drawn >= 80u) || (pct >= 80u && pct != pct_drawn)) {
            draw_status(lamps, pct);            /* (the CPU only when it is near its end) */
            pct_drawn = pct;
        } else {
            for (k = 0; k < 6u; k++)
                if ((lamps ^ lamps_drawn) >> k & 1u) draw_lamp(k, lamps >> k & 1u);
        }
        lamps_drawn = lamps;
        redraw = 0;
        idle_ok = 0;
    drawn:

        if ((set_dirty || bank_dirty) && quiet() && fm1_ms - t_change > 2000u) {   /* when silent: an erase stalls the sound */
            if (set_dirty) settings_save(&set, sizeof set);
            if (bank_dirty) bank_save();
            set_dirty = bank_dirty = 0;
        }
    } while (0);
}
