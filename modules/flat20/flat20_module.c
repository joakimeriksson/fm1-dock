/* SPDX-License-Identifier: GPL-3.0-only */
/* FLAT-20: thenewwazoo's FLAT-20 (github.com/thenewwazoo/FLAT-20, 0c96ea5, 0.3.2; GPL-3.0), a Korg MS-20 modelled part
 * by part from the service manual: two VCOs, the KORG35 high- and low-pass filters, the modulation generator, two
 * envelopes, the external signal processor and the patch panel, 26 presets, as a fm1-dock module: its own app
 * (flat20/flat20.c, split by take.py: app_init, app_loop) on fm1-dock's host (flat20/platform.c, from
 * flat20_platform.inc). Its engine renders in the host's audio interrupt (as on Dinghy, one core, with the FPU as
 * FLAT-20 builds it: module.cfg); its panel, screen and MIDI are the host's. HOME held 1.5 s alone opens fm1-dock's
 * module list. Not here: the panel link to FLAT-20's Mac app. */
#include "flat20/flat20.c"

static void render(int32_t *out, uint32_t n)
{
    if (n == 256u)
        app_audio(out, n);
}

static void ui(uint32_t ev, int32_t arg) { hbf_event(ev, arg); }
static void midi(uint32_t st, uint32_t d1, uint32_t d2) { hbf_midi(st, d1, d2); }
static void note(uint32_t ch, uint32_t key, uint32_t vel) { hbf_midi(0x90u | (ch & 15u), key, vel); }
static void cc(uint32_t ch, uint32_t num, uint32_t val) { hbf_midi(0xB0u | (ch & 15u), num, val); }

static void draw(uint32_t full)
{
    if (full && !splash) {                      /* (back from the module list: the screen and the LEDs again) */
        lcd_fill(0, 0, 240, 240, C_BLACK);
        redraw |= 0x1Fu;
        lamps_drawn = ~0u;
        batt_shown = ~0u;
    }
    app_loop();
}

static int init(const hb_host_t *h)
{
    H = h;
    app_init();
    H->log("flat20: FLAT-20 " APP_VER);
    return 0;
}

static void release(void)                       /* (its voice off) */
{
    nheld = 0;
    hold_on = 0;
    notes_update();
}

HB_DESC = {HB_ABI, "FLAT-20", init, release, note, cc, render, ui, draw, 0, 0, HB_F_SCREEN | HB_F_PANEL, midi};
