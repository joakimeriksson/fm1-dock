/* SPDX-License-Identifier: GPL-3.0-only */
/* S1VAVE: Andrew GG's S1Vave 2.0.1 (github.com/AndrewGG88/S1Vave, e837018; GPL-3.0-only, grown from zvenson's
 * OberVave), an 8-voice synth after the SH-101 and the S-1 (mono, unison, poly, chord; draw and chop; a
 * 4-pole filter, chorus, delay, reverb and a step sequencer), as a fm1-dock module: its own app
 * (s1vave/s1vave.c, split by take.py: app_init, app_loop) on fm1-dock's host (s1vave/platform.c, from
 * s1v_platform.inc), with fm1-dock's own factory bank (s1v_patches.h) and no logo (take.py). Its voices, effects and sequencer render in the host's audio interrupt (as on
 * Dinghy, one core); its panel, screen and MIDI are the host's. HOME held 1.5 s alone opens fm1-dock's
 * module list. */
#include "s1vave/s1vave.c"

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
        dirty_hdr = dirty_page = dirty_tags = 1;
        leds_q = ~0u;
        obv_leds_resend();
    }
    app_loop();
    hb_key0(H, (int32_t)KEY0 + 12 * set.oct);   /* (the SMK-37's outer keys go on from this octave) */
}

static int init(const hb_host_t *h)
{
    H = h;
    app_init();
    obv_leds_resend();
    H->log("s1vave: S1Vave " APP_VER);
    return 0;
}

static void release(void) { ctl_panic = 1; }    /* (its voices off) */

HB_DESC = {HB_ABI, "S1VAVE", init, release, note, cc, render, ui, draw, 0, 0, HB_F_SCREEN | HB_F_PANEL, midi};
