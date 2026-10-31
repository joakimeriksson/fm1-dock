/* SPDX-License-Identifier: GPL-3.0-only */
/* GATOR: npi3pak's macro-oscillator synth inspired by Mutable Instruments Plaits (github.com/npi3pak/fm1-gator,
 * 0.6, 92f923e; its DSP is Emilie Gillet's Plaits and stmlib, MIT), as a fm1-dock module: 24 models in 3 banks, the
 * low-pass gate, an envelope, two LFOs and an 8-slot modulation matrix, 16 presets, mono. Its app and voice
 * (gator/app.c, gator/macro.cc and Plaits, taken by take.py, unchanged) on fm1-dock's host (gator_platform.inc).
 * Plaits renders in the host's audio interrupt at the codec's 44.1 kHz family rate (FW_FS48 0, module.cfg), with the
 * FPU as GATOR builds its C++. A key's pressure is GATOR's aftertouch (AFT); its keys play at velocity 100, as on
 * GATOR. HOME held 1.5 s alone opens fm1-dock's module list. */
#include "gator_platform.inc"
#include "app.c"

static int32_t gator_buf[2u * FW_BLOCK];
static void gator_render(int32_t *out, uint32_t n)
{
    const uint32_t t0 = H->ticks();
    uint32_t i, us;
    if (n > FW_BLOCK)
        return;
    app_audio(gator_buf, n);                    /* (Q15, as the host's) */
    for (i = 0; i < 2u * n; i++) {
        int32_t v = out[i] + gator_buf[i];
        out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
    us = (H->ticks() - t0) / 24u;               /* (audio.c's load, which the app shows and guards on) */
    fw_stats.cpu_q8 = (fw_stats.cpu_q8 * 15u + (us * 256u) / (FW_BLOCK * 1000000u / FW_FS)) / 16u;
}

static uint32_t gator_at;                       /* the aftertouch the keys sent last */
static void gator_ui(uint32_t ev, int32_t arg)
{
    const uint32_t ch = global.channel > 0 ? (uint32_t)global.channel - 1u : 0u;   /* (GATOR's MIDI CH, OMNI: 1) */
    if (HB_EV_KIND(ev) == HB_EV_KEY_PRESSURE) { /* a held key's pressure: channel pressure, GATOR's AFT */
        gator_at = (uint32_t)arg & 127u;
        hbf_midi(0xD0u | ch, gator_at, 0);
        return;
    }
    hbf_event(ev, arg);
    if (HB_EV_KIND(ev) == HB_EV_KEY_UP && !fm1_in.notes && gator_at) {
        gator_at = 0;                           /* (the last key up: none) */
        hbf_midi(0xD0u | ch, 0, 0);
    }
}
static void gator_midi(uint32_t st, uint32_t d1, uint32_t d2) { hbf_midi(st, d1, d2); }
static void gator_note(uint32_t ch, uint32_t key, uint32_t vel) { hbf_midi(0x90u | (ch & 15u), key, vel); }
static void gator_cc(uint32_t ch, uint32_t num, uint32_t val) { hbf_midi(0xB0u | (ch & 15u), num, val); }

static uint32_t gator_frame;
static void gator_draw(uint32_t full)
{
    fw_input_t in;
    if (full) {                                 /* (the start, and back from the module list: all of it again) */
        lcd_fill(0, 0, 240, 240, C_BLACK);
        dirty = D_ALL;
        gator_leds_resend();
    }
    if (!full && H->ms() - gator_frame < FW_FRAME_MS)
        return;                                 /* (main.c's loop: a frame every 15 ms) */
    gator_frame = H->ms();
    fm1_ms = gator_frame;
    fw_input_read(&in);
    app_frame(&in);
}

static int gator_init(const hb_host_t *h)
{
    H = h;
    fm1_ms = H->ms();
    app_init();
    H->log("gator: GATOR 0.6 (npi3pak/fm1-gator 92f923e)");
    return 0;
}

static void gator_release(void) { macro_all_notes_off(); }

HB_DESC = {HB_ABI, "GATOR", gator_init, gator_release, gator_note, gator_cc, gator_render, gator_ui, gator_draw, 0, 0,
           HB_F_SCREEN | HB_F_PANEL, gator_midi};
