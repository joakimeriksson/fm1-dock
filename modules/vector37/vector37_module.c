/* SPDX-License-Identifier: GPL-3.0-only */
/* VECTOR37: M. Geisler's Vector37 (github.com/genkigenki/Vector37), an orbiting overtone synth after the Beetlecrab
 * Vector: four corners, each two overtone or wavetable generators, a blend and a filter, mixed by a moon circling a
 * sun along a path; as a fm1-dock module with its FM-1 panel (panel_fm1.c: the knobs in pages, the planets on
 * ALGORITHM, the pads on EDIT and the lowest keys). Its sources are taken by take.py into vector37/ (unchanged but
 * for the edits listed there) and run on fm1-dock's host (v37_platform.inc). Its wavetables are the data pack V37-WT:
 * without it the wavetable sounds are silent and the screen says so. Its own delay and reverb are out (their RAM):
 * the dock's effects play under it (OPTIONS, EFFECT). HOME held 1.5 s alone opens fm1-dock's module list. */
#include "mk_tables.h"
#include "v37_platform.inc"
#include "vector37/libc.c"
#include "vector37/gfx.c"
#include "vector37/synth.h"
#include "vector37/orbit.c"
#include "vector37/engine.c"
#include "vector37/analog.c"
#include "vector37/fx.c"
#include "vector37/mix.c"
#include "vector37/params.c"
#include "vector37/presets.c"
#include "vector37/scenes.c"
#include "vector37/ui.c"
#include "vector37/draw.c"

static uint32_t boot_t0;
static uint8_t booting, wt_said;
static int wt_state;

static void render(int32_t *out, uint32_t n)
{
    static int32_t blk[2u * CTL];
    uint32_t t0 = H->ticks(), b, i, q;
    for (b = 0; b + CTL <= n; b += CTL) {
        synth_block(blk, CTL);
        for (i = 0; i < 2u * CTL; i++)
            out[2u * b + i] += blk[i];
    }
    q = (H->ticks() - t0) * 256u / (n * 1000000u / FS * FM1_TICKS_PER_US);
    cpu_guard(q);                               /* (Vector37's: too slow, fewer voices; mix.c) */
}

static void ui_ev(uint32_t ev, int32_t arg) { hbf_event(ev, arg); }
static void midi(uint32_t st, uint32_t d1, uint32_t d2) { v37_midi_in(st, d1, d2); }
static void note(uint32_t ch, uint32_t key, uint32_t vel) { v37_midi_in((vel ? 0x90u : 0x80u) | (ch & 15u), key, vel); }
static void cc(uint32_t ch, uint32_t num, uint32_t val) { v37_midi_in(0xB0u | (ch & 15u), num, val); }

static void draw(uint32_t full)
{
    if (booting) {                              /* the boot's flight to the planets, a frame a pass (draw.c's) */
        uint32_t t = fm1_ms - boot_t0;
        if (t <= BOOT_MS && !(fm1_in.notes && t > 300u)) {
            boot_frame(t);
            return;
        }
        boot_frame(BOOT_MS);
        boot_handover = 1;
        booting = 0;
        full = 0;
    }
    if (full) {                                 /* (back from the module list: the screen and the LEDs again) */
        lcd_fill(0, 0, 240, 240, C_BG);
        ui.dirty = 1;
        v37_leds_resend();
    }
    if (wt_state && !wt_said) {
        ui_say(wt_state == -1 ? "NO WAVETABLES: " : "WAVETABLES DO NOT FIT: ", "ADD V37-WT");
        wt_said = 1;
    }
    midi_poll();
    ui_input();
    ui_leds();
    draw_frame();
}

static int init(const hb_host_t *h)
{
    H = h;
    wt_state = v37_wt_find();
    persist_boot();                             /* (the settings, the USER bank: the host's store) */
    palette_set(2);
    synth_init();
    ui_init();                                  /* the first sound */
    splash();
    boot_t0 = fm1_ms;
    booting = 1;
    H->log(wt_state ? "vector37: Vector37 " MK_VERSION ", no wavetables (V37-WT)" : "vector37: Vector37 " MK_VERSION);
    return 0;
}

static void release(void) { ev_push(EV_ALLOFF, 0, 0); }

HB_DESC = {HB_ABI, "VECTOR37", init, release, note, cc, render, ui_ev, draw, 0, 0, HB_F_SCREEN | HB_F_PANEL, midi};
