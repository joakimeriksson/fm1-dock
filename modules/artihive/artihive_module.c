/* SPDX-License-Identifier: GPL-3.0-only */
/* ARTIHIVE: KHAV's ARTIHIVE 2.0.3 (github.com/KHAVld/ARTIHIVE, ed53f4b), an eight-voice drone and polyphonic
 * synthesizer on four interacting FM pairs, as a fm1-dock module: its own app (artihive/artihive.c, split by
 * take.py: app_init, app_loop) on fm1-dock's host (artihive/platform.c, from artihive_platform.inc). Its voices,
 * delay and compressor render in the host's audio interrupt (as on Dinghy, one core); its panel, screen and MIDI
 * are the host's. HOME held 1.5 s alone opens fm1-dock's module list. */
#include "artihive/artihive.c"

static void render(int32_t *out, uint32_t n)
{
    if (n == HALF_FRAMES)
        app_audio(out, n);
}

static void ah_ui(uint32_t ev, int32_t arg) { hbf_event(ev, arg); }
static void midi(uint32_t st, uint32_t d1, uint32_t d2) { hbf_midi(st, d1, d2); }
static void note(uint32_t ch, uint32_t key, uint32_t vel) { hbf_midi(0x90u | (ch & 15u), key, vel); }
static void cc(uint32_t ch, uint32_t num, uint32_t val) { hbf_midi(0xB0u | (ch & 15u), num, val); }

static void draw(uint32_t full)
{
    if (full) {                                 /* (back from the module list: the screen and the LEDs again) */
        draw_panel_bg();
        draw_header();
        draw_page();
        draw_body();
        draw_voices();
        draw_status();
        ah_leds_resend();
    }
    app_loop();
}

static int init(const hb_host_t *h)
{
    H = h;
    app_init();
    ah_leds_resend();
    H->log("artihive: ARTIHIVE " APP_VER);
    return 0;
}

static void release(void)                       /* (its voices off) */
{
    latch_mask = touch_mask = 0;
    gates();
}

HB_DESC = {HB_ABI, "ARTIHIVE", init, release, note, cc, render, ah_ui, draw, 0, 0, HB_F_SCREEN | HB_F_PANEL, midi};
