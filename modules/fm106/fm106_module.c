/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-106: PatttF's Roland Juno-106 for the FM-1 (github.com/PatttF/fm-106, db59d58; its DSP after
 * ultramaster_kr106, its dual-core audio after Melodee), a 6-voice polysynth with 128 factory presets, user
 * presets, the BBD chorus, insert effects, an arpeggiator and a step sequencer, as a fm1-dock module: its
 * own app (fm106/fm106.c, taken by take.py) on fm1-dock's host (fm106/platform.c, from fm106_platform.inc).
 * Its voices render in the host's audio interrupt, voices 3..5 on the second core when the host lends it
 * (c1_run), else all six on core 0; its panel, screen and MIDI are the host's. HOME held 1.5 s alone opens
 * fm1-dock's module list. */
#include "fm106/fm106.c"

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
        g_dirty_screen = 1;
        fm106_leds_resend();
    }
    fm106_app_frame();
}

static int init(const hb_host_t *h)
{
    H = h;
    audio_worker_online = HB_HOST_HAS(H, c1_wait);
    fm106_app_init();
    fm106_leds_resend();
    H->log("fm106: FM-106 " APP_VER);
    return 0;
}

static void release(void) { fm106_all_notes_off(&g_synth); }   /* (the host has stopped its render) */

HB_DESC = {HB_ABI, "FM-106", init, release, note, cc, render, ui, draw, 0, 0, HB_F_SCREEN | HB_F_PANEL, midi};
