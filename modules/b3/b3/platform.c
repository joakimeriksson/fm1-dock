/* SPDX-License-Identifier: GPL-3.0-only */
/* What B3 ORGAN's platform.c (Dinghy's) gives its dinghy.c, on the fm1-dock host: copied to b3/platform.c by
 * take.py, where b3.c includes it (.inc here: not a module source). The panel, the time and MIDI
 * (modules/lib/hb_fm1.h); the LEDs, sent only when they change; the screen: B3's own gfx.c (its canvas and font)
 * onto the host's fill and blit; its settings (a few bytes) in the module's settings record. */
#include <stdint.h>
#define fm1_led_key hbf_led_key                 /* (hb_fm1.h's: this file's sends only what changed) */
#include "hb_fm1.h"
#undef fm1_led_key

#ifndef APP_NAME
#define APP_NAME "B3 ORGAN"
#endif
#ifndef APP_VER
#define APP_VER "0.2"
#endif
#define FS 44100u
#define HALF_FRAMES 256u

/* the panel by name, as Dinghy's platform.c */
enum { BTN_OCT_DOWN = 0, BTN_OCT_UP = 1, BTN_FX = 2, BTN_SEL = 3, BTN_ENV = 4, BTN_LFO = 5, BTN_EDIT = 6,
       BTN_GLO = 7, BTN_HOME = 8, BTN_SAVE = 9, BTN_ARP = 10, BTN_SEQ = 11, BTN_PLAY = 12, BTN_REC = 13 };
enum { ENC_SELECT = 0, ENC_ALGORITHM = 1, ENC_KNOB1 = 2, ENC_KNOB2 = 3, ENC_KNOB3 = 4, ENC_KNOB4 = 5, ENC_PRESETS = 6 };
#define KEY_LED(k) (14u + (k))

void *memcpy(void *d, const void *s, unsigned n);   /* (modules/lib/hb_lib.c) */
void *memset(void *d, int c, unsigned n);

/* ---- the LEDs: B3 sets every one on every pass; the host hears only the changes */
static uint64_t b3_lit, b3_sent;
static void fm1_led_key(uint32_t id, int on)
{
    uint64_t b = (uint64_t)1 << id;
    if (id >= 41u) return;
    b3_lit = on ? b3_lit | b : b3_lit & ~b;
    if ((b3_lit ^ b3_sent) & b) {
        H->led(id, on ? 1u : 0u);
        b3_sent ^= b;
    }
}
static void b3_leds_resend(void)                /* (back from the host's module list: every LED again) */
{
    uint32_t id;
    for (id = 0; id < 41u; id++)
        H->led(id, (uint32_t)(b3_lit >> id & 1u));
    b3_sent = b3_lit;
}

/* ---- the screen: gfx.c's canvas onto the host's (blit: bytes as the panel takes them, as gfx.c makes them) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { H->fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, c); }
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    H->blit((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px);
}
#define CV_MAX (240u * 64u)                     /* (Dinghy's: the biggest text box drawn at once) */
#include "gfx.c"

/* ---- the settings: the host's settings record (240 B; B3 keeps 22) */
static int settings_load(void *p, uint32_t n) { return n <= 240u ? H->load(p, n) : -1; }
static int settings_save(const void *p, uint32_t n)
{
    if (n > 240u) return -1;
    H->save(p, n);
    return 0;
}

static void platform_poll(void) {}              /* (the host polls USB, the keys, MIDI) */
