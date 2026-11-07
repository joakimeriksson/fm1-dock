/* SPDX-License-Identifier: GPL-3.0-only */
/* What Niagara Drift's platform (its app/plat.h, plat_fm1.c) gives Niagara Drift, on the fm1-dock host: the panel, the
 * time and MIDI (modules/lib/hb_fm1.h, the panel in its own order: B_* and EN_*), the keys' pressure (the host's
 * sensing, as HB_EV_KEY_PRESSURE), the lights (its brightness levels as the host's lit / glow / off), the screen (its
 * gfx.c onto the host's fill and blit), the store (the project and banks B, C and the old Presets in the module's
 * banks 1..4) and the audio load (the render timed here). What a module has not: the headphone jack's sense (the
 * Speaker low cut stays as set) and the MASTER pot's overdrive (the host applies MASTER after the render). As
 * modules/fomni/fomni_platform.inc, whose app Niagara Drift grew from. */
#include <stdint.h>
#include "hb_fm1.h"
#include "app.h"                                /* (plat.h: the panel, the store objects) */

void *memcpy(void *d, const void *s, unsigned n);   /* (modules/lib/hb_lib.c) */
void *memset(void *d, int c, unsigned n);

/* ---- the panel: B_FX .. B_OCTUP and EN_SELECT .. EN_K4, from the host's ids */
static const uint8_t ND_BTN[NB] = {HB_BTN_FX, HB_BTN_SEL, HB_BTN_ENV, HB_BTN_LFO, HB_BTN_EDIT, HB_BTN_GLO, HB_BTN_HOME,
                                    HB_BTN_SAVE, HB_BTN_ARP, HB_BTN_SEQ, HB_BTN_PLAY, HB_BTN_REC, HB_BTN_OCT_DOWN,
                                    HB_BTN_OCT_UP};
static const uint8_t ND_ENC[NE] = {HB_ENC_SELECT, HB_ENC_ALGORITHM, HB_ENC_PRESETS, HB_ENC_KNOB1, HB_ENC_KNOB2,
                                    HB_ENC_KNOB3, HB_ENC_KNOB4};

uint32_t plat_ms(void) { return H->ms(); }
/* held; a press that came and went since the last read counts as held for that one read (as FoMni's) */
uint32_t plat_buttons(void)
{
    uint32_t b = 0, i, held = fm1_in.buttons | fm1_input_edges(0);
    for (i = 0; i < NB; i++)
        if (held & 1u << ND_BTN[i])
            b |= 1u << i;
    return b;
}
uint32_t plat_keys(void) { return (fm1_in.notes | fm1_input_note_edges()) & ((1u << NKEYS) - 1u); }
int32_t plat_enc(int role) { return role >= 0 && role < NE ? fm1_enc_take(ND_ENC[role]) : 0; }
/* the engine's clean top (0.8 of its gain): MASTER is the host's, after the render */
#define ND_GAIN 3277u
uint32_t plat_master(void) { return ND_GAIN; }
int plat_headphones(void) { return 0; }

/* ---- the keys' pressure: the host's, 0..127 while held, for the keys it senses (-1 for the others) */
static uint8_t nd_press[NKEYS];
static uint32_t nd_sensed;
static void nd_key_event(uint32_t ev, int32_t arg)  /* from ui() */
{
    uint32_t k = HB_EV_ID(ev);
    if (k >= NKEYS)
        return;
    if (HB_EV_KIND(ev) == HB_EV_KEY_PRESSURE)
        nd_press[k] = (uint8_t)(arg < 0 ? 0 : arg > 127 ? 127 : arg);
    else if (HB_EV_KIND(ev) == HB_EV_KEY_UP)
        nd_press[k] = 0;
}
int plat_pressure(uint32_t k) { return k < NKEYS && (nd_sensed >> k & 1u) ? nd_press[k] : -1; }

/* ---- the lights: a level 0..255 as the host's off (0), glow (2, a dim light) or lit (1) */
static uint8_t nd_sent[NB + NKEYS];
static uint8_t nd_all = 1;
static uint32_t nd_level(uint8_t v) { return v >= 128u ? 1u : v ? 2u : 0u; }
void plat_leds(const uint8_t *buttons, const uint8_t *keys)
{
    uint32_t i;
    for (i = 0; i < NB + NKEYS; i++) {
        uint8_t v = (uint8_t)nd_level(i < NB ? buttons[i] : keys[i - NB]);
        if (nd_all || v != nd_sent[i])
            H->led(i < NB ? ND_BTN[i] : HB_LED_KEY(i - NB), v);
        nd_sent[i] = v;
    }
    nd_all = 0;
}

/* ---- MIDI: USB-MIDI packets (byte 0 the code index, then status, data 1, data 2) */
int plat_midi_in(uint32_t *pkt)
{
    uint8_t st, d1, d2;
    if (!midi_get(&st, &d1, &d2))
        return 0;
    *pkt = (uint32_t)(st >> 4) | (uint32_t)st << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24;
    return 1;
}
void plat_midi_out(uint32_t pkt) { H->midi_send(pkt >> 8 & 0xFFu, pkt >> 16 & 0xFFu, pkt >> 24 & 0xFFu); }

/* ---- the store: OBJ_PROJ, OBJ_BANKB, OBJ_BANKC, OBJ_BANKP in the module's banks 1..4 (8 KiB each); the panel
 * calibration (OBJ_PANEL) is the host's */
static int nd_banks(void) { return HB_HOST_HAS(H, bank_free); }
static uint32_t nd_bank(uint32_t obj)
{
    return obj == OBJ_PROJ ? 1u : obj == OBJ_BANKB ? 2u : obj == OBJ_BANKC ? 3u : obj == OBJ_BANKP ? 4u : 0u;
}
int plat_store_load(uint32_t obj, void *dst, uint32_t max)
{
    if (!nd_bank(obj) || !nd_banks())
        return -1;
    return H->bank_load_n(nd_bank(obj), dst, max);
}
int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    if (!nd_bank(obj) || !nd_banks() || len > PLAT_STORE_MAX)
        return -1;
    return H->bank_save_n(nd_bank(obj), src, len) ? -1 : 0;
}

/* ---- the audio load: the render's own time against its block's (256 frames at 44.1 kHz: 5805 us) */
static volatile uint32_t nd_load;
uint32_t plat_cpu_pct(void) { return nd_load; }
uint32_t plat_xruns(void) { return 0; }

/* ---- the screen: gfx.c's canvas onto the host's (the colours as RGB565 to fill, the canvas already as the panel
 * takes it to blit) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { H->fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, c); }
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px) { H->blit((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px); }
#include "gfx.c"
