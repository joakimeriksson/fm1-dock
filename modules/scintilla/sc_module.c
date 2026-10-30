/* SPDX-License-Identifier: GPL-3.0-only */
/* Scintilla: the Elka Synthex engine of synthex-web, ported to integers (joakimeriksson/scintilla-fm1),
 * as a fm1-dock module: its own app (app/scintilla.c, from take.py, built with -DHB_MODULE: its platform
 * is sc_harbor.h) with its 80 presets, pages, joystick, SETTINGS and MIDI. The host runs app_init() at
 * init and app_loop() from draw(); the engine renders in the host's audio interrupt. The host senses the
 * keys: ui() hands a key's velocity and pressure to the app (key_vel, key_press: its TOUCH and VELOCITY
 * settings use them). HOME held 1.5 s alone opens fm1-dock's module list (a short HOME is Scintilla's). */
#include "app/scintilla.c"

static void render(int32_t *out, uint32_t n)
{
    if (n == HALF_FRAMES)
        app_audio(out, n);
}
static void ui(uint32_t ev, int32_t arg)
{
    uint32_t k = HB_EV_ID(ev);
    if (k < 27u) {
        if (HB_EV_KIND(ev) == HB_EV_KEY_PRESSURE) {
            key_press[k] = (uint8_t)arg;
            return;
        }
        if (HB_EV_KIND(ev) == HB_EV_KEY_DOWN)
            key_vel[k] = (uint8_t)(arg > 0 ? arg : 100);   /* (a key whose force is not sensed: 100, as Scintilla's) */
        else if (HB_EV_KIND(ev) == HB_EV_KEY_UP)
            key_press[k] = 0;
    }
    hbf_event(ev, arg);
}
static void midi(uint32_t st, uint32_t d1, uint32_t d2) { hbf_midi(st, d1, d2); }
static void draw(uint32_t full)
{
    if (full)                                   /* (back from the module list: everything again) */
        redraw_all();
    app_loop();
}
static void note(uint32_t ch, uint32_t key, uint32_t vel)
{
    (void)ch;
    if (vel) note_on(key, vel);
    else note_off(key);
}
static void cc(uint32_t ch, uint32_t num, uint32_t val) { hbf_midi(0xB0u | (ch & 15u), num, val); }

static int init(const hb_host_t *h)
{
    H = h;
    app_init();
    H->log("scintilla: " APP_NAME);
    return 0;
}
static void release(void)
{
    sc_post(&syn, SC_EV_ALL_OFF, 0, 0);
}

HB_DESC = {HB_ABI, "SCINTILLA", init, release, note, cc, render, ui, draw, 0, 0, HB_F_SCREEN | HB_F_PANEL, midi};
