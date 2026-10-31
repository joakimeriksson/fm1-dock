/* SPDX-License-Identifier: GPL-3.0-only */
/* FIMBA: jadamsowers' FiMba-1 (github.com/jadamsowers/fm1-fimba, 0485ed0; its files in fimba/ unchanged but for
 * take.py's), a kalimba by modal synthesis, as a fm1-dock module: tines on the white keys, chords, sharps or
 * performance moves on the black ones, bodies, buzzers and the sound hole's wah, grains, a ping-pong delay, a plate,
 * tape and lo-fi, mbira patterns. Its app on fm1-dock's host (fimba_platform.inc: its platform calls); its engine
 * renders in the host's audio interrupt, as on its own firmware (one core), with the FPU as FiMba-1 builds it
 * (module.cfg). HOME held 1.5 s alone opens fm1-dock's module list. */
#include "fimba_platform.inc"
#include "project.c"
#include "ui.c"

#define KM_BLOCK 256u
static int32_t km_buf[2u * KM_BLOCK];
static void fimba_render(int32_t *out, uint32_t n)
{
    uint32_t t0 = H->ticks(), i;
    if (n != KM_BLOCK)
        return;
    km_render(km_buf, n, 4096u);            /* MASTER full (the host applies it) */
    for (i = 0; i < 2u * n; i++) {          /* FiMba-1's 24 bits into the host's Q15, added */
        int32_t v = out[i] + (km_buf[i] >> 8);
        out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
    om_load = (H->ticks() - t0) * 100u / (5805u * 24u);
}

static void fimba_ui(uint32_t ev, int32_t arg)
{
    fimba_key_force(ev, arg);
    if (HB_EV_KIND(ev) != HB_EV_KEY_PRESSURE)
        hbf_event(ev, arg);
}
static void fimba_midi(uint32_t st, uint32_t d1, uint32_t d2) { hbf_midi(st, d1, d2); }
static void fimba_note(uint32_t ch, uint32_t key, uint32_t vel) { hbf_midi(0x90u | (ch & 15u), key, vel); }
static void fimba_cc(uint32_t ch, uint32_t num, uint32_t val) { hbf_midi(0xB0u | (ch & 15u), num, val); }

/* a band, a strip at a time (fimba_platform.inc): drawn again for each strip while the first one found it changed */
static void strips(void (*draw)(void), uint32_t *sig)
{
    uint32_t was = *sig;
    for (cv_top = 0;; *sig = was) {
        cv_drawn = 0;
        draw();
        if (!cv_drawn || (cv_top += FIMBA_CV_ROWS) >= cv_band_h)
            break;
    }
    cv_top = 0;
}
/* ui.c's ui_frame, the bands in strips */
static void fimba_frame(void)
{
    input();
    autosave();
    strips(draw_header, &ui.sig[0]);
    strips(draw_main, &ui.sig[1]);
    strips(draw_knobs, &ui.sig[2]);
    leds();
    ui.frame++;
}

static uint32_t om_last_frame;
static void fimba_draw(uint32_t full)
{
    if (full) {                             /* (back from the module list: the whole screen and the lights again) */
        lcd_fill(0, 0, 240, 240, K_BG);
        ui.sig[0] ^= 1u;
        ui.sig[1] ^= 1u;
        ui.sig[2] ^= 1u;
        om_lights(1);
    }
    if (H->ms() - om_last_frame >= 16u) {   /* ~60 frames a second at most, input in between (its main loop) */
        om_last_frame = H->ms();
        fimba_frame();
    } else {
        ui_input_only();
    }
}

static int fimba_init(const hb_host_t *h)
{
    H = h;
    km_init();
    project_load();                         /* defaults when nothing (or another format) is saved */
    project_apply();
    ui_init();
    om_lights(1);
    H->log("fimba: FiMba-1 (jadamsowers/fm1-fimba 0485ed0)");
    return 0;
}

static void fimba_release(void) { km_panic(); }

HB_DESC = {HB_ABI, "FIMBA", fimba_init, fimba_release, fimba_note, fimba_cc, fimba_render, fimba_ui, fimba_draw, 0, 0,
           HB_F_SCREEN | HB_F_PANEL, fimba_midi};
