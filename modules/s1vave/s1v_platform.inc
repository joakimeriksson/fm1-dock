/* SPDX-License-Identifier: GPL-3.0-only */
/* What S1Vave's platform.c (Dinghy's, by way of OberVave) gives its s1vave.c, on the fm1-dock host: copied to
 * s1vave/platform.c by take.py, where s1vave.c includes it (.inc here: not a module source). As OberVave's
 * (modules/obervave/obv_platform.inc), but S1Vave keeps its eight pattern slots as blobs. The panel, the time and MIDI
 * (modules/lib/hb_fm1.h); the LEDs, lit and the glow (hb_host_t.led 1 / 2); the screen: S1Vave's
 * own gfx.c (its canvas and font) onto the host's fill and blit; the storage on the host's: the
 * settings in the module's settings record (240 B), the user programs and the pattern slots in its
 * banks (bank_load_n / bank_save_n, a few KB each; the bank area is shared by every module, so a
 * slot that finds no free bank is not saved, and S1Vave says so). What a module cannot have: the
 * SMK-37's own layer (BOARD_SMK37 0: on an SMK the host gives it the FM-1's panel), USB audio, its
 * SysEx (live patch push) and its updater. */
#include <stdint.h>
#define fm1_led_key hbf_led_key                 /* (hb_fm1.h's, lit only: this file's has the glow beside it) */
#include "hb_fm1.h"
#undef fm1_led_key

#ifndef APP_NAME
#define APP_NAME "S1VAVE"
#endif
#ifndef APP_VER
#define APP_VER "2.0.1"
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
static int memcmp(const void *a, const void *b, unsigned n)
{
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p < *q ? -1 : 1;
    return 0;
}

/* the main loop's few moments with the audio interrupt's state (the CPU's own flag, no register) */
static void fm1_irq_off(void) { __asm__ volatile("cli" ::: "memory"); }
static void fm1_irq_on(void) { __asm__ volatile("csync\n\tsti" ::: "memory"); }

/* ---- the LEDs: lit, and the glow under it */
static uint64_t obv_lit, obv_glow, obv_sent_lit, obv_sent_glow;
static void obv_led(uint32_t id)
{
    uint64_t b = (uint64_t)1 << id;
    uint32_t v = (obv_lit & b) ? 1u : (obv_glow & b) ? 2u : 0u, was = (obv_sent_lit & b) ? 1u : (obv_sent_glow & b) ? 2u : 0u;
    if (v != was) {
        H->led(id, v);
        obv_sent_lit = (obv_sent_lit & ~b) | (v == 1u ? b : 0);
        obv_sent_glow = (obv_sent_glow & ~b) | (v == 2u ? b : 0);
    }
}
static void fm1_led_key(uint32_t id, int on)
{
    if (id >= 41u) return;
    obv_lit = on ? obv_lit | (uint64_t)1 << id : obv_lit & ~((uint64_t)1 << id);
    obv_led(id);
}
static void fm1_led_key_glow(uint32_t id, int on)
{
    if (id >= 41u) return;
    obv_glow = on ? obv_glow | (uint64_t)1 << id : obv_glow & ~((uint64_t)1 << id);
    obv_led(id);
}
static void obv_leds_resend(void)               /* (back from the host's module list: every LED again) */
{
    uint32_t id;
    obv_sent_lit = ~obv_lit;
    obv_sent_glow = 0;
    for (id = 0; id < 41u; id++)
        obv_led(id);
}

/* ---- the screen: gfx.c's canvas onto the host's (blit: bytes as the panel takes them, as gfx.c makes them) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { H->fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, c); }
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    H->blit((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px);
}
#define CV_MAX (240u * 120u)
#include "gfx.c"

/* ---- the storage */
#define SETTINGS_MAX (4096u - 12u)              /* (Dinghy's: a sector; S1Vave's patterns are up to one) */
/* the settings: the host's settings record (240 B), or the module's first bank if they outgrow it */
static int settings_load(void *p, uint32_t n) { return n <= 240u ? H->load(p, n) : H->bank_load(p, n); }
static int settings_save(const void *p, uint32_t n)
{
    if (n > 240u) return H->bank_save(p, n);
    H->save(p, n);
    return 0;
}
/* a piece of n bytes in banks from idx on (HB_BANK_MAX each); the host's banks need an ABI-2 host
 * that has them (hb_host_t.size) */
static int obv_banks(void) { return H->size >= (uint32_t)((const char *)&H->bank_free - (const char *)H) + sizeof H->bank_free; }
static int obv_put(uint32_t idx, const void *p, uint32_t n)
{
    const uint8_t *b = p;
    if (!obv_banks()) return -1;
    while (n) {
        uint32_t k = n < HB_BANK_MAX ? n : HB_BANK_MAX;
        if (H->bank_save_n(idx++, b, k)) return -1;
        b += k; n -= k;
    }
    return 0;
}
static int obv_get(uint32_t idx, void *p, uint32_t n)
{
    uint8_t *b = p;
    if (!obv_banks()) return -1;
    while (n) {
        uint32_t k = n < HB_BANK_MAX ? n : HB_BANK_MAX;
        if (H->bank_load_n(idx++, b, k) != (int)k) return -1;
        b += k; n -= k;
    }
    return 0;
}
#define OBV_BANKS(n) (((n) + HB_BANK_MAX - 1u) / HB_BANK_MAX)
/* the user programs: banks 1, 2; the eight pattern slots (blobs, up to a sector each as on Dinghy) banks 3..10 */
#define NPSECT 2u
static int psect_load(uint32_t i, void *p, uint32_t n) { return i < NPSECT && n <= HB_BANK_MAX ? obv_get(1u + i, p, n) : -1; }
static int psect_save(uint32_t i, const void *p, uint32_t n) { return i < NPSECT && n <= HB_BANK_MAX ? obv_put(1u + i, p, n) : -1; }
#define NBLOB 8u
static int blob_load(uint32_t i, void *p, uint32_t n) { return i < NBLOB && n <= SETTINGS_MAX ? obv_get(3u + i, p, n) : -1; }
static int blob_save(uint32_t i, const void *p, uint32_t n) { return i < NBLOB && n <= SETTINGS_MAX ? obv_put(3u + i, p, n) : -1; }

/* its editor's SysEx (S1Vave's VST and web editor: patch and pattern dumps) is not a module's: nothing reaches
 * app_sysex, and what it would send back is dropped */
static int ota_send_msg(uint32_t cmd, const uint8_t *body, uint32_t n) { (void)cmd; (void)body; (void)n; return -1; }

static int32_t platform_battery_bars(void) { return 4; }   /* (no reading for a module: the header has no icon,
                                                 * take.py; a constant keeps its redraw check quiet) */
static void platform_poll(void) {}              /* (the host polls USB, the keys, MIDI) */
