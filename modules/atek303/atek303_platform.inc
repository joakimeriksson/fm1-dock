/* SPDX-License-Identifier: GPL-3.0-only */
/* What fm1-base's platform (plat.h, its device/unity.c) gives ATEK303, on the fm1-dock host: the panel, the time and
 * MIDI (modules/lib/hb_fm1.h, the panel in fm1-base's own order: B_* and EN_*), the keys' velocity (the host's sensing,
 * in place of fm1-base's lib/touch.c), the lights (lit, the glow of the unlit buttons, and the keys' middle level as
 * the glow), the screen (fm1-base's gfx.c onto the host's fill and blit) and the store (its five objects in the
 * module's banks 1..5). What a module has not: the screen reader over USB (its ann_* calls do nothing), the battery's
 * reading (the gauge hidden), the backup over USB (fm1-dock's own banks are the module's), the splash. */
#include <stdint.h>
#include "hb_fm1.h"
#include "app.h"                                /* (plat.h, and the store objects: OBJ_SETTINGS ...) */

void *memcpy(void *d, const void *s, unsigned n);   /* (modules/lib/hb_lib.c) */
void *memset(void *d, int c, unsigned n);

/* ---- the panel: fm1-base's B_FX .. B_OCTUP and EN_SELECT .. EN_K4, from the host's ids */
static const uint8_t ATM_BTN[NB] = {HB_BTN_FX, HB_BTN_SEL, HB_BTN_ENV, HB_BTN_LFO, HB_BTN_EDIT, HB_BTN_GLO, HB_BTN_HOME,
                                   HB_BTN_SAVE, HB_BTN_ARP, HB_BTN_SEQ, HB_BTN_PLAY, HB_BTN_REC, HB_BTN_OCT_DOWN,
                                   HB_BTN_OCT_UP};
static const uint8_t ATM_ENC[NE] = {HB_ENC_SELECT, HB_ENC_ALGORITHM, HB_ENC_PRESETS, HB_ENC_KNOB1, HB_ENC_KNOB2,
                                   HB_ENC_KNOB3, HB_ENC_KNOB4};

uint32_t plat_ms(void) { return H->ms(); }
/* held, as fm1-base reads the panel; a press that came and went since the last read (the host hands the module events,
 * ATEK303 polls) counts as held for that one read, so its release is seen at the next */
uint32_t plat_buttons(void)
{
    uint32_t b = 0, i, held = fm1_in.buttons | fm1_input_edges(0);
    for (i = 0; i < NB; i++)
        if (held & 1u << ATM_BTN[i])
            b |= 1u << i;
    return b;
}
uint32_t plat_keys(void) { return fm1_in.notes | fm1_input_note_edges(); }
int32_t plat_enc(int role) { return role >= 0 && role < NE ? fm1_enc_take(ATM_ENC[role]) : 0; }

/* ---- the keys' velocity: a sensed key's comes with its press (the host's sensing, 0 when not sensed: 100, as
 * fm1-base gives a key it does not read); touch.c's state calls on it */
static uint8_t atm_vel[NKEYS];
static void atm_key_event(uint32_t ev, int32_t arg)
{
    uint32_t k = HB_EV_ID(ev);
    if (HB_EV_KIND(ev) == HB_EV_KEY_DOWN && k < NKEYS)
        atm_vel[k] = (uint8_t)(arg > 0 && arg < 128 ? arg : 100);
}
static void touch_on(int on) { (void)on; }
static void touch_poll(void) {}
static uint32_t touch_keys(void) { return plat_keys(); }
static uint8_t touch_vel(uint32_t k) { return k < NKEYS && atm_vel[k] ? atm_vel[k] : 100u; }

/* ---- the lights: what changed goes to the host (lit 1, the glow 2: the unlit buttons, and the keys at the middle
 * level, the saved presets in the list) */
static uint32_t atm_lit_b, atm_lit_k, atm_mid_k, atm_glow, atm_sent_b = ~0u, atm_sent_k = ~0u, atm_sent_mid = ~0u,
                atm_sent_glow = ~0u;
static void atm_lights(int all)
{
    uint32_t i;
    for (i = 0; i < NB; i++) {
        uint32_t m = 1u << i, v = (atm_lit_b & m) ? 1u : atm_glow ? 2u : 0u;
        uint32_t was = (atm_sent_b & m) ? 1u : atm_sent_glow ? 2u : 0u;
        if (all || v != was)
            H->led(ATM_BTN[i], v);
    }
    for (i = 0; i < NKEYS; i++) {
        uint32_t m = 1u << i, v = (atm_lit_k & m) ? 1u : (atm_mid_k & m) ? 2u : 0u;
        uint32_t was = (atm_sent_k & m) ? 1u : (atm_sent_mid & m) ? 2u : 0u;
        if (all || v != was)
            H->led(HB_LED_KEY(i), v);
    }
    atm_sent_b = atm_lit_b;
    atm_sent_k = atm_lit_k;
    atm_sent_mid = atm_mid_k;
    atm_sent_glow = atm_glow;
}
void plat_leds(uint32_t buttons, uint32_t keys)
{
    atm_lit_b = buttons & ((1u << NB) - 1u);
    atm_lit_k = keys;
    atm_lights(0);
}
void plat_glow(int on)
{
    atm_glow = on != 0;
    atm_lights(0);
}
void plat_lights_mid(uint32_t buttons, uint32_t keys)
{
    (void)buttons;
    atm_mid_k = keys & ~atm_lit_k;
    atm_lights(0);
}

/* ---- MIDI: USB-MIDI packets (byte 0 the code index, then status, data 1, data 2); out to the host's (USB and
 * BLE-MIDI, as fm1-dock sends) */
int plat_midi_in(uint32_t *pkt, int *port)
{
    uint8_t st, d1, d2;
    if (!midi_get(&st, &d1, &d2))
        return 0;
    *pkt = (uint32_t)(st >> 4) | (uint32_t)st << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24;
    if (port)
        *port = PORT_USB;
    return 1;
}
void plat_midi_send(uint32_t pkt, uint32_t to)
{
    (void)to;
    H->midi_send(pkt >> 8 & 0xFFu, pkt >> 16 & 0xFFu, pkt >> 24 & 0xFFu);
}

/* ---- the store: ATEK303's objects in the module's banks (8 KiB each, 9 for a module): the settings 1, the four
 * preset banks 2..5, OPTIONS 6; the panel calibration is the host's */
static int atm_banks(void) { return HB_HOST_HAS(H, bank_free); }
static uint32_t atm_bank(uint32_t obj)
{
    return obj == OBJ_SETTINGS ? 1u : obj >= OBJ_PRESETS_A && obj <= OBJ_PRESETS_D ? 2u + obj - OBJ_PRESETS_A
         : obj == OBJ_OPTIONS ? 6u : 0u;
}
int plat_store_load(uint32_t obj, void *dst, uint32_t max)
{
    uint32_t b = atm_bank(obj);
    if (!b || !atm_banks())
        return -1;
    return H->bank_load_n(b, dst, max);
}
int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    uint32_t b = atm_bank(obj);
    if (!b || !atm_banks() || len > PLAT_STORE_MAX)
        return -1;
    return H->bank_save_n(b, src, len) ? -1 : 0;
}

/* ---- what the FM-1 has and a module not: the battery's reading (none: the gauge hidden) */
int plat_battery(void) { return -1; }

/* ---- the screen: gfx.c's canvas onto the host's (the colours as RGB565 to fill, the canvas already as the panel
 * takes it to blit) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { H->fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, c); }
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px) { H->blit((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px); }
#include "gfx.c"

/* ---- the screen reader (fm1-base's announce.c, over USB): not in a module */
#define ANN_MAX 240u
#define ANN_NOW 1u
#define ANN_KEEP 2u
static int ann_on(void) { return 0; }
static void ann_say(const char *a, const char *b, uint32_t flags) { (void)a; (void)b; (void)flags; }
static void ann_tick(uint32_t now) { (void)now; }
