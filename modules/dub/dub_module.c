/* SPDX-License-Identifier: GPL-3.0-only */
/* DUB-1: sbegueria's dub techno machine (github.com/sbegueria/fm1-dub, 501c4cf; its files in dub/ unchanged but for
 * RAM, see take.py), a fork of AMB-1, as a fm1-dock module: chord stabs dissolving in a tape echo over a steady kick,
 * pings, a bass, drums and a noise machine that play themselves in eight pieces, with a dub mixer, sirens and a plate.
 * Its app on fm1-dock's host (dub_platform.inc: its platform calls); its engine renders in the host's audio interrupt,
 * as on its own firmware (one core), with the FPU as DUB-1 builds it (module.cfg). HOME held 1.5 s alone opens
 * fm1-dock's module list. */
#include "dub_platform.inc"
#include "project.c"
#include "ui.c"

#define DUB_BLOCK 256u
static int32_t dub_buf[2u * DUB_BLOCK];
static void dub_render(int32_t *out, uint32_t n)
{
    uint32_t t0 = H->ticks(), i;
    if (n != DUB_BLOCK)
        return;
    for (i = 0; i < (uint32_t)EN_PROF_N; i++)
        en_prof[i] = 0;                     /* (this block's stages, from nothing) */
    en_render(dub_buf, n, 4096u);           /* (the host applies MASTER) */
    for (i = 0; i < 2u * n; i++) {          /* DUB-1's 24 bits into the host's Q15, added */
        int32_t v = out[i] + (dub_buf[i] >> 8);
        out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
    dub_load(H->ticks() - t0);
}

static void dub_ui(uint32_t ev, int32_t arg) { hbf_event(ev, arg); }
static void dub_midi(uint32_t st, uint32_t d1, uint32_t d2) { hbf_midi(st, d1, d2); }
static void dub_note(uint32_t ch, uint32_t key, uint32_t vel) { hbf_midi(0x90u | (ch & 15u), key, vel); }
static void dub_cc(uint32_t ch, uint32_t num, uint32_t val) { hbf_midi(0xB0u | (ch & 15u), num, val); }

/* the launch screen first, a frame each 55 ms as main_fm1.c plays it (there while the FM-1 starts), then the UI */
static uint32_t dub_last_frame, dub_splash;
static void dub_draw(uint32_t full)
{
    uint32_t now = H->ms();
    if (dub_splash < UI_SPLASH_N) {
        if (full || now - dub_last_frame >= 55u) {
            dub_last_frame = now;
            ui_splash((int)dub_splash++);
            if (dub_splash == UI_SPLASH_N) {
                ui_init();
                dub_lights(1);
            }
        }
        return;
    }
    if (full) {                             /* (back from the module list: the whole screen again, as it was, and
                                             * the lights; each band's picture differs from what it was drawn for) */
        lcd_fill(0, 0, 240, 240, K_BG);
        ui.sig[0] = ~ui.sig[0];
        ui.sig[1] = ~ui.sig[1];
        ui.sig[2] = ~ui.sig[2];
        dub_lights(1);
    }
    if (now - dub_last_frame >= 16u) {      /* ~60 frames a second at most, input in between (its main loop) */
        dub_last_frame = now;
        ui_frame();
    } else {
        ui_input_only();
    }
}

static int dub_init(const hb_host_t *h)
{
    H = h;
    en_init();
    project_load();                         /* defaults when nothing (or another format) is saved */
    project_apply();
    H->log("dub: DUB-1 (sbegueria/fm1-dub 501c4cf)");
    return 0;
}

static void dub_release(void)
{
    en_play(0);
    en_panic();
}

HB_DESC = {HB_ABI, "DUB-1", dub_init, dub_release, dub_note, dub_cc, dub_render, dub_ui, dub_draw, 0, 0,
           HB_F_SCREEN | HB_F_PANEL, dub_midi};
