/* SPDX-License-Identifier: GPL-3.0-only */
/* What FLAT-20's platform.c (Dinghy's) gives its flat20.c, on the fm1-dock host: copied to flat20/platform.c by
 * take.py, where flat20.c includes it (.inc here: not a module source). The panel, the time and MIDI
 * (modules/lib/hb_fm1.h); the LEDs (lit); the screen: FLAT-20's own gfx.c (its canvas and font) onto the host's fill
 * and blit; the storage on the host's: the settings in the module's settings record, the preset bank in its first
 * bank. What a module has not: the panel link to the Mac app (the SysEx out goes nowhere: the module has no USB MIDI
 * of its own), the battery (no bars shown), the SMK-37 (BOARD_SMK37 0, as FLAT-20 itself). */
#include <stdint.h>
#include "hb_fm1.h"

#define BOARD_SMK37 0
#ifndef APP_NAME
#define APP_NAME "FLAT-20"
#endif
#ifndef APP_VER
#define APP_VER "0.3.2"
#endif

/* the panel by name, as Dinghy's platform.c */
enum { BTN_OCT_DOWN = 0, BTN_OCT_UP = 1, BTN_FX = 2, BTN_SEL = 3, BTN_ENV = 4, BTN_LFO = 5, BTN_EDIT = 6,
       BTN_GLO = 7, BTN_HOME = 8, BTN_SAVE = 9, BTN_ARP = 10, BTN_SEQ = 11, BTN_PLAY = 12, BTN_REC = 13 };
enum { ENC_SELECT = 0, ENC_ALGORITHM = 1, ENC_KNOB1 = 2, ENC_KNOB2 = 3, ENC_KNOB3 = 4, ENC_KNOB4 = 5, ENC_PRESETS = 6 };
#define KEY_LED(k) (14u + (k))

void *memcpy(void *d, const void *s, unsigned n);   /* (modules/lib/hb_lib.c) */
void *memset(void *d, int c, unsigned n);
static int memcmp(const void *a, const void *b, unsigned n)
{
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p < *q ? -1 : 1;
    return 0;
}

static void platform_poll(void) {}              /* (the host polls USB, the keys, MIDI) */
static int32_t batt_raw;                        /* (no battery reading: batt_now keeps its bars) */
static int batt_ext(void) { return 0; }
static int sysex_send(const uint8_t *p, uint32_t n) { (void)p; (void)n; return 0; }   /* (the panel link: nowhere to go) */

/* ---- the screen: gfx.c's canvas onto the host's (blit: bytes as the panel takes them, as gfx.c makes them) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { H->fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, c); }
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    H->blit((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px);
}
#include "gfx.c"

/* ---- the storage: the settings in the host's record, the bank (blob 0) in the module's first bank */
static int settings_load(void *p, uint32_t n) { return H->load(p, n); }
static int settings_save(const void *p, uint32_t n)
{
    H->save(p, n);
    return 0;
}
static int blob_load(uint32_t i, void *p, uint32_t n)
{
    return i == 0 && n <= HB_BANK_MAX && H->bank_load_n(1u, p, n) == (int)n ? 0 : -1;
}
static int blob_save(uint32_t i, const void *p, uint32_t n)
{
    return i == 0 && n <= HB_BANK_MAX ? H->bank_save_n(1u, p, n) : -1;
}
