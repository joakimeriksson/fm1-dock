/* SPDX-License-Identifier: GPL-3.0-only */
/* ATEK303: Animatek's acid machine (github.com/animatek/FM1-Animatek, its atek303/ on fm1-base; GPL-3.0-or-later, its
 * files in atek303/ unchanged but for a module's RAM, see take.py) as a fm1-dock module: the ATEK303 voice from VCV
 * Rack (Open303 with a diode ladder and a modelled oscillator, 14 oscillators on WAVE), its acid pattern generator (64
 * steps, a step editor), drive, chorus / flanger, a synced ping-pong delay and a plate reverb, 32 presets. Its app on
 * fm1-dock's host (atek303_platform.inc: fm1-base's platform calls); its engine renders in the host's audio interrupt,
 * as on fm1-base (one core), with the FPU as fm1-base builds it (module.cfg). HOME held 1.5 s alone opens fm1-dock's
 * module list. */
#include "atek303_platform.inc"
#include "app.c"

static int32_t atm_buf[2u * APP_BLOCK];
static volatile uint32_t atm_load;
static void atm_render(int32_t *out, uint32_t n)
{
    uint32_t t0 = H->ticks(), i;
    if (n != APP_BLOCK)
        return;
    app_render(atm_buf, n, 4096u);           /* (the host applies MASTER) */
    for (i = 0; i < 2u * n; i++) {          /* fm1-base's 24 bits into the host's Q15, added */
        int32_t v = out[i] + (atm_buf[i] >> 8);
        out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
    atm_load = (H->ticks() - t0) * 100u / (5805u * 24u);
}

static void atm_ui(uint32_t ev, int32_t arg)
{
    atm_key_event(ev, arg);
    hbf_event(ev, arg);
}
static void atm_midi(uint32_t st, uint32_t d1, uint32_t d2) { hbf_midi(st, d1, d2); }
static void atm_note(uint32_t ch, uint32_t key, uint32_t vel) { hbf_midi(0x90u | (ch & 15u), key, vel); }
static void atm_cc(uint32_t ch, uint32_t num, uint32_t val) { hbf_midi(0xB0u | (ch & 15u), num, val); }

static uint32_t atm_last_frame;
static void atm_draw(uint32_t full)
{
    if (full) {                             /* (back from the module list: the whole screen and the lights again) */
        app_ui_init();
        atm_lights(1);
    }
    if (H->ms() - atm_last_frame >= 16u) {   /* ~60 frames a second at most, input in between (fm1-base's loop) */
        atm_last_frame = H->ms();
        app_frame();
    } else {
        app_input_only();
    }
}

static int atm_init(const hb_host_t *h)
{
    H = h;
    app_init();
    app_ui_init();
    atm_lights(1);
    H->log("atek303: ATEK303 1.2 (animatek/FM1-Animatek 9b82ef2)");
    return 0;
}

static void atm_release(void) { app_panic(); }

HB_DESC = {HB_ABI, "ATEK303", atm_init, atm_release, atm_note, atm_cc, atm_render, atm_ui, atm_draw, 0, 0,
           HB_F_SCREEN | HB_F_PANEL, atm_midi};
