/* SPDX-License-Identifier: GPL-3.0-only */
/* What ARTIHIVE's platform.c (Dinghy's) gives its artihive.c, on the fm1-dock host: copied to
 * artihive/platform.c by take.py, where artihive.c includes it (.inc here: not a module source). The panel, the
 * time and MIDI (modules/lib/hb_fm1.h); the LEDs, lit and the glow (hb_host_t.led 1 / 2); the screen: ARTIHIVE's
 * own gfx.c (its canvas and font) onto the host's fill and blit; the storage on the host's: the settings in the
 * module's settings record (240 B), the eight user slots (blob 0) in its bank 1. What a module cannot have: the
 * SMK-37 (BOARD_SMK37 0), LCD_IDLE (the host sends the screen; its MIDI is read once a pass anyway). */
#include <stdint.h>
#define fm1_led_key hbf_led_key                 /* (hb_fm1.h's, lit only: this file's has the glow beside it) */
#include "hb_fm1.h"
#undef fm1_led_key

#ifndef APP_VER
#define APP_VER "2.0.3"
#endif
#define BOARD_SMK37 0
#define FS 44100u
#define HALF_FRAMES 256u

/* the panel by name, as Dinghy's platform.c */
enum { BTN_OCT_DOWN = 0, BTN_OCT_UP = 1, BTN_FX = 2, BTN_SEL = 3, BTN_ENV = 4, BTN_LFO = 5, BTN_EDIT = 6,
       BTN_GLO = 7, BTN_HOME = 8, BTN_SAVE = 9, BTN_ARP = 10, BTN_SEQ = 11, BTN_PLAY = 12, BTN_REC = 13 };
enum { ENC_SELECT = 0, ENC_ALGORITHM = 1, ENC_KNOB1 = 2, ENC_KNOB2 = 3, ENC_KNOB3 = 4, ENC_KNOB4 = 5, ENC_PRESETS = 6 };
#define KEY_LED(k) (14u + (k))

void *memcpy(void *d, const void *s, unsigned n);   /* (modules/lib/hb_lib.c) */
void *memset(void *d, int c, unsigned n);

/* ---- the LEDs: lit, and the glow under it */
static uint64_t ah_lit, ah_glow, ah_sent_lit, ah_sent_glow;
static void ah_led(uint32_t id)
{
    uint64_t b = (uint64_t)1 << id;
    uint32_t v = (ah_lit & b) ? 1u : (ah_glow & b) ? 2u : 0u, was = (ah_sent_lit & b) ? 1u : (ah_sent_glow & b) ? 2u : 0u;
    if (v != was) {
        H->led(id, v);
        ah_sent_lit = (ah_sent_lit & ~b) | (v == 1u ? b : 0);
        ah_sent_glow = (ah_sent_glow & ~b) | (v == 2u ? b : 0);
    }
}
static void fm1_led_key(uint32_t id, int on)
{
    if (id >= 41u) return;
    ah_lit = on ? ah_lit | (uint64_t)1 << id : ah_lit & ~((uint64_t)1 << id);
    ah_led(id);
}
static void fm1_led_key_glow(uint32_t id, int on)
{
    if (id >= 41u) return;
    ah_glow = on ? ah_glow | (uint64_t)1 << id : ah_glow & ~((uint64_t)1 << id);
    ah_led(id);
}
static void ah_leds_resend(void)                /* (back from the host's module list: every LED again) */
{
    uint32_t id;
    ah_sent_lit = ~ah_lit;
    ah_sent_glow = 0;
    for (id = 0; id < 41u; id++)
        ah_led(id);
}

/* ---- the screen: gfx.c's canvas onto the host's (blit: bytes as the panel takes them, as gfx.c makes them) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { H->fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, c); }
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    H->blit((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px);
}
#include "gfx.c"

/* ---- the storage */
static int settings_load(void *p, uint32_t n) { return n <= 240u ? H->load(p, n) : -1; }
static int settings_save(const void *p, uint32_t n)
{
    if (n > 240u) return -1;
    H->save(p, n);
    return 0;
}
/* blob i in bank 1 + i; 0 only when it holds exactly n bytes (artihive.c tries each older layout's size) */
#define NBLOB 1u
static int ah_banks(void) { return HB_HOST_HAS(H, bank_free); }
static int blob_load(uint32_t i, void *p, uint32_t n)
{
    if (i >= NBLOB || !ah_banks() || n > HB_BANK_MAX || H->bank_load_n(1u + i, 0, 0) != (int)n) return -1;
    return H->bank_load_n(1u + i, p, n) == (int)n ? 0 : -1;
}
static int blob_save(uint32_t i, const void *p, uint32_t n)
{
    return i < NBLOB && ah_banks() && n <= HB_BANK_MAX ? H->bank_save_n(1u + i, p, n) : -1;
}

static void platform_poll(void) {}              /* (the host polls USB, the keys, MIDI) */
