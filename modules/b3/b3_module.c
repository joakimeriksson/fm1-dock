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

/* the sound now, to the host (hb_host_t.sound_name, when it has it): the preset and the switches that change the organ's
 * kind ("Gospel PERC OD"); FM-1 Big Screen's Rich plays the closest instrument (percussion, overdrive: other organs) */
static void say_sound(void)
{
    static char last[56];
    char s[56] = "B3 ORGAN";                     /* (the host shows this text as it is: the synth's name first) */
    uint32_t k = 8, i;
    const char *n = P.pgm ? pgm_name(P.pgm) : 0;
    const char *add[3] = {P.perc ? " PERC" : "", P.od ? " OD" : "", P.les == 2u ? " LESLIE FAST" : ""};
    if (n && n[0])
        s[k++] = ' ';
    for (i = 0; n && n[i] && k < 32u; i++)
        s[k++] = n[i];
    for (uint32_t a = 0; a < 3u; a++)
        for (i = 0; add[a][i] && k < sizeof s - 1u; i++)
            s[k++] = add[a][i];
    s[k] = 0;
    for (i = 0; i <= k && s[i] == last[i]; i++)
        ;
    if (i > k)
        return;                                 /* (the same as last time) */
    memcpy(last, s, k + 1u);
    if (H->size >= __builtin_offsetof(hb_host_t, sound_name) + sizeof(void *) && H->sound_name)
        H->sound_name(last);                    /* (the host keeps the pointer: our static copy) */
}

static void draw(uint32_t full)
{
    if (full) {                                 /* (back from the module list: the screen and the LEDs again) */
        drawn_screen = 99;                      /* B3 0.6: a new screen clears and draws every band */
        logo_drawn = 0;
        t_load = 0;
        b3_leds_resend();
    }
    app_loop();
    say_sound();
    hb_key0(H, 53 + 12 * oct);   /* (the SMK-37's outer keys go on from this octave) */
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
