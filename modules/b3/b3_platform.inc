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
#define APP_VER "0.6.1"
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
/* ---- B3's settings (0.6's are bigger than the module's 240-byte record) and the user's programs (data_load / data_save, a
 * second sector in Dinghy): both in the module's 8 KiB flash bank, kept here and written whole. Older, smaller settings
 * (up to 240 bytes) still come from the settings record, so an organ set up before keeps its sound. */
static struct { uint32_t data_n, set_n; uint8_t data[1024], set[512]; } b3_bank;
static int b3_bank_state = -1;                  /* -1 not read yet, 0 nothing saved, 1 loaded */
static void b3_bank_get(void)
{
    if (b3_bank_state < 0)
        b3_bank_state = H->bank_load(&b3_bank, sizeof b3_bank) == 0 ? 1 : 0;
}
static int b3_bank_put(void)
{
    b3_bank_state = 1;
    return H->bank_save(&b3_bank, sizeof b3_bank);
}
static int settings_load(void *p, uint32_t n)
{
    if (n <= 240u && H->load(p, n) == 0)
        return 0;
    b3_bank_get();
    if (b3_bank_state != 1 || n > sizeof b3_bank.set || b3_bank.set_n != n)
        return -1;
    memcpy(p, b3_bank.set, n);
    return 0;
}
static int settings_save(const void *p, uint32_t n)
{
    if (n <= 240u) {
        H->save(p, n);
        return 0;
    }
    if (n > sizeof b3_bank.set)
        return -1;
    b3_bank_get();
    memcpy(b3_bank.set, p, n);
    b3_bank.set_n = n;
    return b3_bank_put();
}
static int data_load(void *p, uint32_t n)
{
    b3_bank_get();
    if (b3_bank_state != 1 || n > sizeof b3_bank.data || b3_bank.data_n != n)
        return -1;
    memcpy(p, b3_bank.data, n);
    return 0;
}
static int data_save(const void *p, uint32_t n)
{
    if (n > sizeof b3_bank.data)
        return -1;
    b3_bank_get();
    memcpy(b3_bank.data, p, n);
    b3_bank.data_n = n;
    return b3_bank_put();
}
/* a module runs on one core (the host's), and the host counts MIDI that didn't fit */
static const uint8_t fm1_c1_on = 0;
static volatile uint32_t midi_in_overflow;
/* B3's web editor (F0 7D 42 33 ..): a module can't send SysEx yet, so its answers go nowhere (B3 on its own has it) */
static int sysex_send(const uint8_t *p, uint32_t n) { (void)p; (void)n; return 0; }
static int sysex_get(const uint8_t **p, uint32_t *n) { return 0; }   /* (nothing from the editor in the dock) */
static void sysex_done(void) {}

static void platform_poll(void) {}              /* (the host polls USB, the keys, MIDI) */
