/* SPDX-License-Identifier: GPL-3.0-only */
/* MONK-1: Charles Vestal's twelve singers (github.com/charlesvestal/fm1-monk, a9d368a; its files in monk/, see take.py
 * for the little that changed and why), MonkSynth's formant voice (Jonathan Taylor, MIT) with the characters of its
 * iOS port, as a fm1-dock module: one voice of up to ten in unison, the vowel on ALGORITHM, the singer on PRESETS,
 * velocity and pressure from the FM-1's own keys. Its app on fm1-dock's host (monk_platform.inc: its platform calls);
 * its voice renders in the host's audio interrupt, as on its own firmware (one core), with the FPU as MONK-1 builds it
 * (module.cfg). HOME held 1.5 s alone opens fm1-dock's module list. */
#include "monk_platform.inc"
#include "project.c"
#include "inflate.c"
#include "face.c"
#include "ui.c"

#define MK_BLOCK 256u
static int32_t mk_buf[2u * MK_BLOCK];
static void monk_mod_render(int32_t *out, uint32_t n)
{
    uint32_t i;
    if (n != MK_BLOCK)
        return;
    monk_render(mk_buf, n, 4096u);          /* (the host applies MASTER) */
    for (i = 0; i < 2u * n; i++) {          /* MONK-1's 24 bits into the host's Q15, added */
        int32_t v = out[i] + (mk_buf[i] >> 8);
        out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
}

/* the keys: a sensed key's landing (its velocity) and lift go to MONK-1's sense queue, the others to its panel read */
static void monk_mod_ui(uint32_t ev, int32_t arg)
{
    uint32_t k = HB_EV_ID(ev);
    if (HB_EV_KIND(ev) == HB_EV_KEY_DOWN && arg > 0 && k < SN_NKEYS) {
        mk_sdown |= 1u << k;
        mk_sense_ev(EV_ON, k, (uint32_t)arg);
    } else if (HB_EV_KIND(ev) == HB_EV_KEY_UP && k < SN_NKEYS && (mk_sdown >> k & 1u)) {
        mk_sdown &= ~(1u << k);
        mk_sense_ev(EV_OFF, k, 0);
    } else if (HB_EV_KIND(ev) != HB_EV_KEY_PRESSURE) {   /* (MONK-1 reads its pressure itself: ui.c pressure()) */
        hbf_event(ev, arg);
    }
}
static void monk_mod_midi(uint32_t st, uint32_t d1, uint32_t d2) { hbf_midi(st, d1, d2); }
static void monk_mod_note(uint32_t ch, uint32_t key, uint32_t vel) { hbf_midi(0x90u | (ch & 15u), key, vel); }
static void monk_mod_cc(uint32_t ch, uint32_t num, uint32_t val) { hbf_midi(0xB0u | (ch & 15u), num, val); }

static uint32_t mk_last_frame;
static void monk_mod_draw(uint32_t full)
{
    if (full) {                             /* (back from the module list: the whole screen and the lights again) */
        ui.scene = 0xFF;
        mk_lights(1);
    }
    mk_sense_sync();
    if (H->ms() - mk_last_frame >= 16u) {   /* ~60 frames a second at most, input in between (its main loop) */
        mk_last_frame = H->ms();
        ui_frame();
    } else {
        ui_input_only();
    }
}

static int monk_mod_init(const hb_host_t *h)
{
    H = h;
    monk_init();
    sense_setup();                          /* (DRIVE 2, the curve and the lightest press to the host's sensing) */
    project_load();                         /* defaults when nothing (or another format) is saved */
    project_apply();
    ui_init();
    mk_lights(1);
    H->log("monk: MONK-1 (charlesvestal/fm1-monk a9d368a)");
    return 0;
}

static void monk_mod_release(void)
{
    all_off();
    if (ui.dirty)                           /* (what its autosave had not saved yet) */
        project_save();
}

HB_DESC = {HB_ABI, "MONK-1", monk_mod_init, monk_mod_release, monk_mod_note, monk_mod_cc, monk_mod_render, monk_mod_ui,
           monk_mod_draw, 0, 0, HB_F_SCREEN | HB_F_PANEL, monk_mod_midi};
