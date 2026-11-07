/* SPDX-License-Identifier: GPL-3.0-only */
/* B3 ORGAN: R-Jeb's tonewheel organ for the FM-1 (github.com/R-Jeb/setBfree-fm1, v0.2 7014954), modelled on setBfree
 * (91 wheels, nine drawbars, setBfree's tapering and leakage, key click, vibrato and chorus, percussion, overdrive,
 * reverb and a Leslie; its 110 programs and default controllers), as a fm1-dock module: its own app (b3/b3.c, split
 * by take.py: app_init, app_loop) on fm1-dock's host (b3/platform.c, from b3_platform.inc). The organ renders in the
 * host's audio interrupt (as on Dinghy, one core); its panel, screen and MIDI are the host's. HOME held 1.5 s alone
 * opens fm1-dock's module list. */
#include "b3/b3.c"

static void render(int32_t *out, uint32_t n)
{
    if (n == HALF_FRAMES)
        app_audio(out, n);
}

static void ui(uint32_t ev, int32_t arg) { hbf_event(ev, arg); }
static void midi(uint32_t st, uint32_t d1, uint32_t d2) { hbf_midi(st, d1, d2); }
static void note(uint32_t ch, uint32_t key, uint32_t vel) { hbf_midi(0x90u | (ch & 15u), key, vel); }
static void cc(uint32_t ch, uint32_t num, uint32_t val) { hbf_midi(0xB0u | (ch & 15u), num, val); }

static void draw(uint32_t full)
{
    if (full) {                                 /* (back from the module list: the screen and the LEDs again) */
        lcd_fill(0, 0, 240, 240, C_BLACK);
        draw_text_box(0, 4, 120, &FONT_S, "B3 ORGAN v" APP_VER, C_ACC, 0);
        for (i = 0; i < NB; i++)
            drawn_db[i] = 99;
        status_key = ~0u;
        t_load = 0;
        b3_leds_resend();
    }
    app_loop();
}

static int init(const hb_host_t *h)
{
    H = h;
    app_init();
    b3_leds_resend();
    H->log("b3: B3 ORGAN " APP_VER);
    return 0;
}

static void release(void) { pk_lo = pk_hi = mk_lo = mk_hi = 0; }   /* (the host has stopped its render: no key held) */

HB_DESC = {HB_ABI, "B3 ORGAN", init, release, note, cc, render, ui, draw, 0, 0, HB_F_SCREEN | HB_F_PANEL, midi};
