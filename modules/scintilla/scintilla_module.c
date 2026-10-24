/* SPDX-License-Identifier: GPL-3.0-only */
/* Scintilla: the 1981 Elka Synthex, as synthex-web plays it, in integer C (github.com/joakimeriksson/
 * scintilla-fm1) as a harbor module: its own app, scintilla/scintilla.c, unchanged (built with HB_MODULE,
 * which takes sc_harbor.h for its platform), with its engine, 80 factory sounds, pages and MIDI. The host
 * runs its app_init() at init and its app_loop() from draw(); the engine renders in the host's audio
 * interrupt. What a module cannot have: nothing it uses (it runs on one core). HOME held 1.5 s alone opens
 * harbor's module list (a short HOME is Scintilla's HOME page). take.sh brings the sources in. */
#include "sc_harbor.h"
#include "scintilla/scintilla.c"

static void render(int32_t *out, uint32_t n) { app_audio(out, n); }
static uint8_t clip127(int32_t v) { return (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v); }
static void ui(uint32_t ev, int32_t arg)         /* the host senses the keys: their velocity and pressure */
{
    uint32_t id = HB_EV_ID(ev);
    if (HB_EV_KIND(ev) == HB_EV_KEY_PRESSURE) {  /* (Scintilla's aftertouch: the stick pushed left) */
        if (id < 27u) key_press[id] = clip127(arg);
        return;
    }
    if (HB_EV_KIND(ev) == HB_EV_KEY_DOWN && id < 27u)
        key_vel[id] = arg > 0 ? clip127(arg) : 100u;   /* (0: a key the host does not sense) */
    hbf_event(ev, arg);
}
static void midi(uint32_t st, uint32_t d1, uint32_t d2) { hbf_midi(st, d1, d2); }
static void draw(uint32_t full)
{
    if (full) redraw_all();                     /* (back from the module list: the screen again) */
    app_loop();
}
static void note(uint32_t ch, uint32_t key, uint32_t vel) { hbf_midi((vel ? 0x90u : 0x80u) | (ch & 15u), key, vel); }
static void cc(uint32_t ch, uint32_t num, uint32_t val) { hbf_midi(0xB0u | (ch & 15u), num, val); }

static int init(const hb_host_t *h)
{
    H = h;
    app_init();
    H->log("scintilla: Scintilla");
    return 0;
}

static void release(void)                       /* (switched back to, it starts again from init) */
{
    uint32_t i;
    for (i = 0; i < KEY_LED(27); i++)           /* its LEDs off: the buttons (OCT-, the pages) and the keys */
        H->led(i, 0);
}

HB_DESC = {HB_ABI, "SCINTILLA", init, release, note, cc, render, ui, draw, 0, 0, HB_F_SCREEN | HB_F_PANEL, midi};
