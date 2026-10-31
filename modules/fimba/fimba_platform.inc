/* SPDX-License-Identifier: GPL-3.0-only */
/* What FiMba-1's platform (its app/plat.h, plat_fm1.c: FoMni's) gives FiMba-1, on the fm1-dock host: the panel, the
 * time and MIDI (modules/lib/hb_fm1.h, the panel in FiMba-1's own order: B_* and EN_*), the lights (lit, and the dim
 * light of the key under the lowest tine), the keys' force (a pluck's velocity), the screen (FiMba-1's gfx.c onto the
 * host's fill and blit, drawn in strips), the store (the project in the module's first bank) and the audio load (the
 * render timed here). As modules/fomni/fomni_platform.inc, which FiMba-1's grew from. */
#include <stdint.h>
#include "hb_fm1.h"
#define OM_VERSION "0.6.1"                      /* (FiMba-1's VERSION, on its Setup page) */
#include "app.h"                                /* (plat.h: the panel, the store objects) */

void *memcpy(void *d, const void *s, unsigned n);   /* (modules/lib/hb_lib.c) */
void *memset(void *d, int c, unsigned n);

/* ---- the panel: B_FX .. B_OCTUP and EN_SELECT .. EN_K4, from the host's ids */
static const uint8_t OM_BTN[NB] = {HB_BTN_FX, HB_BTN_SEL, HB_BTN_ENV, HB_BTN_LFO, HB_BTN_EDIT, HB_BTN_GLO, HB_BTN_HOME,
                                    HB_BTN_SAVE, HB_BTN_ARP, HB_BTN_SEQ, HB_BTN_PLAY, HB_BTN_REC, HB_BTN_OCT_DOWN,
                                    HB_BTN_OCT_UP};
static const uint8_t OM_ENC[NE] = {HB_ENC_SELECT, HB_ENC_ALGORITHM, HB_ENC_PRESETS, HB_ENC_KNOB1, HB_ENC_KNOB2,
                                    HB_ENC_KNOB3, HB_ENC_KNOB4};

uint32_t plat_ms(void) { return H->ms(); }
/* held, as plat_fm1.c reads the panel; a press that came and went since the last read (the host hands the module
 * events, FiMba-1 polls) counts as held for that one read, so its release is seen at the next */
uint32_t plat_buttons(void)
{
    uint32_t b = 0, i, held = fm1_in.buttons | fm1_input_edges(0);
    for (i = 0; i < NB; i++)
        if (held & 1u << OM_BTN[i])
            b |= 1u << i;
    return b;
}
uint32_t plat_keys(void) { return (fm1_in.notes | fm1_input_note_edges()) & ((1u << NKEYS) - 1u); }
int32_t plat_enc(int role) { return role >= 0 && role < NE ? fm1_enc_take(OM_ENC[role]) : 0; }

/* ---- the keys' force: a sensed key's velocity (0: not sensed) scales FiMba-1's own (100 a tine, less in a roll);
 * its pressure covers the sound hole, as MIDI channel pressure does in FiMba-1 */
static uint8_t km_key_vel[NKEYS], km_key_press[NKEYS], km_press_sent;
static int fimba_key_vel(int k, int vel)
{
    int v = k >= 0 && k < NKEYS && km_key_vel[k] ? vel * km_key_vel[k] / 100 : vel;
    return v < 1 ? 1 : v > 127 ? 127 : v;
}
static void fimba_key_force(uint32_t ev, int32_t arg)
{
    uint32_t k = HB_EV_ID(ev), i, p = 0;
    if (k >= NKEYS)
        return;
    if (HB_EV_KIND(ev) == HB_EV_KEY_DOWN)
        km_key_vel[k] = (uint8_t)(arg > 0 && arg < 128 ? arg : 0);
    else if (HB_EV_KIND(ev) == HB_EV_KEY_PRESSURE)
        km_key_press[k] = (uint8_t)(arg <= 0 ? 0 : arg > 127 ? 127 : arg);
    else if (HB_EV_KIND(ev) == HB_EV_KEY_UP)
        km_key_press[k] = 0;
    else
        return;
    for (i = 0; i < NKEYS; i++)
        p = km_key_press[i] > p ? km_key_press[i] : p;
    if (p != km_press_sent) {
        km_press_sent = (uint8_t)p;
        km_hole(HOLE_PRESS, (int)p);
    }
}

/* ---- the lights: what changed goes to the host (lit 1, dim 2) */
static uint32_t om_lit_b, om_lit_k, om_dim_k, om_sent_b = ~0u, om_sent_k = ~0u, om_sent_dim = ~0u;
static void om_lights(int all)
{
    uint32_t i;
    for (i = 0; i < NB; i++) {
        uint32_t m = 1u << i;
        if (all || ((om_lit_b ^ om_sent_b) & m))
            H->led(OM_BTN[i], (om_lit_b & m) ? 1u : 0u);
    }
    for (i = 0; i < NKEYS; i++) {
        uint32_t m = 1u << i;
        if (all || (((om_lit_k ^ om_sent_k) | (om_dim_k ^ om_sent_dim)) & m))
            H->led(HB_LED_KEY(i), (om_lit_k & m) ? 1u : (om_dim_k & m) ? 2u : 0u);
    }
    om_sent_b = om_lit_b;
    om_sent_k = om_lit_k;
    om_sent_dim = om_dim_k;
}
void plat_leds(uint32_t buttons, uint32_t keys, uint32_t dim_keys)
{
    om_lit_b = buttons & ((1u << NB) - 1u);
    om_lit_k = keys & ((1u << NKEYS) - 1u);
    om_dim_k = dim_keys & ((1u << NKEYS) - 1u);
    om_lights(0);
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

/* ---- the store: the project (OBJ_PROJ) in the module's first bank (8 KiB); the panel calibration is the host's */
static int om_banks(void) { return HB_HOST_HAS(H, bank_free); }
int plat_store_load(uint32_t obj, void *dst, uint32_t max)
{
    if (obj != OBJ_PROJ || !om_banks())
        return -1;
    return H->bank_load_n(1u, dst, max);
}
int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    if (obj != OBJ_PROJ || !om_banks() || len > PLAT_STORE_MAX)
        return -1;
    return H->bank_save_n(1u, src, len) ? -1 : 0;
}

/* ---- the audio load: the render's own time against its block's (256 frames at 44.1 kHz: 5805 us); no xruns here
 * (the host counts its overruns) */
static volatile uint32_t om_load;
uint32_t plat_cpu_pct(void) { return om_load; }
uint32_t plat_xruns(void) { return 0; }

/* ---- the screen: gfx.c's canvas onto the host's (the colours as RGB565 to fill, the canvas already as the panel
 * takes it to blit) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { H->fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, c); }
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px) { H->blit((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px); }
#define FIMBA_CV_ROWS 8u                        /* the canvas: 240 x 8 (FiMba-1's 240 x 144, 68 KiB, does not fit
                                                 * beside its engine's 266 KiB) */
#include "gfx.c"

/* ---- the bands in strips: FiMba-1 draws a band (the header's 28 rows, the kalimba's 144, the knobs' 68) into a canvas
 * as tall; here fimba_frame() has it drawn once a strip, cv_top rows down, all of it clipped to the strip (gfx.c's
 * cv_oy, cv_y0 and cv_y1, as its bands) and put on the screen there */
static uint32_t cv_top, cv_band_h, cv_drawn;
static void fimba_cv_begin(uint32_t w, uint32_t h, uint16_t bg)
{
    cv_begin(w, h, bg);                         /* (as many rows as the strip has) */
    cv_band_h = h;
    cv_drawn = 1;
    if (cv_h > h - cv_top)
        cv_h = h - cv_top;
    cv_oy = -(int32_t)cv_top;
    cv_y0 = 0;
    cv_y1 = (int32_t)cv_h;
}
static void fimba_cv_blit(uint32_t x, uint32_t y) { cv_blit(x, y + cv_top); }
/* text wholly above or below the strip: only where it ends (cv_text would test each of its pixels) */
static int32_t fimba_cv_text(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    y += cv_oy;
    if (y >= cv_y1 || y + (int32_t)f->h <= cv_y0)
        return x + text_w(f, s);
    return cv_text(x, y - cv_oy, f, s, c);
}
#define cv_begin fimba_cv_begin
#define cv_blit fimba_cv_blit
#define cv_text fimba_cv_text
