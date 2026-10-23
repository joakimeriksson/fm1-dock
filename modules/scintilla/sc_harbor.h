/* SPDX-License-Identifier: GPL-3.0-only */
/* What Dinghy's platform.c gives Scintilla's scintilla.c (built with HB_MODULE), on the harbor host: the
 * panel, the time and MIDI (modules/lib/hb_fm1.h), the screen calls (the host's canvas and fonts, the
 * same as gfx.c's), the settings record, and libc.c's string helpers (not its memcpy / memset: the
 * module has hb_lib.c's). The DIN jack is the host's: its messages come through midi() as USB's do. */
#ifndef SC_HARBOR_H
#define SC_HARBOR_H
#include <stdint.h>
#include "hb_fm1.h"

#define FS 44100u
#define HALF_FRAMES 256u

enum { BTN_OCT_DOWN = 0, BTN_OCT_UP = 1, BTN_FX = 2, BTN_SEL = 3, BTN_ENV = 4, BTN_LFO = 5, BTN_EDIT = 6,
       BTN_GLO = 7, BTN_HOME = 8, BTN_SAVE = 9, BTN_ARP = 10, BTN_SEQ = 11, BTN_PLAY = 12, BTN_REC = 13 };
enum { ENC_SELECT = 0, ENC_ALGORITHM = 1, ENC_KNOB1 = 2, ENC_KNOB2 = 3, ENC_KNOB3 = 4, ENC_KNOB4 = 5, ENC_PRESETS = 6 };
#define KEY_LED(k) (14u + (k))

static void platform_poll(void) {}             /* (the host polls; app_loop() runs from draw()) */

/* ---- the screen (gfx.c / lcd.c's calls; the host draws with the same fonts) */
#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define C_BLACK 0x0000u
#define C_WHITE 0xFFFFu
typedef struct { uint32_t h, big; } felucca_font_t;
static const felucca_font_t FONT_S = {16, 0}, FONT_L = {32, 1};
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { H->fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, c); }
static void cv_begin(uint32_t w, uint32_t h, uint16_t bg) { H->cv_begin((int32_t)w, (int32_t)h, bg); }
static void cv_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) { H->cv_rect(x, y, w, h, c); }
static int32_t text_w(const felucca_font_t *f, const char *s) { return H->text_w(f->big, s); }
static int32_t cv_text(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    H->cv_text(x, y, f->big, s, c);
    return x + text_w(f, s);
}
static void cv_blit(uint32_t x, uint32_t y) { H->cv_blit((int32_t)x, (int32_t)y); }
/* one-shot: text in a box, cleared to black, blitted (align 0 left, 1 centre, 2 right) */
static void draw_text_box(uint32_t x, uint32_t y, uint32_t w, const felucca_font_t *f, const char *s,
                          uint16_t c, int align)
{
    int32_t tw = text_w(f, s), tx = align == 1 ? ((int32_t)w - tw) / 2 : align == 2 ? (int32_t)w - tw : 0;
    cv_begin(w, f->h, C_BLACK);
    cv_text(tx, 0, f, s, c);
    cv_blit(x, y);
}

/* ---- settings */
static int settings_load(void *p, uint32_t n) { return H->load(p, n); }
static int settings_save(const void *p, uint32_t n) { H->save(p, n); return 0; }

/* ---- libc.c's string helpers */
static unsigned str_len(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}
static void str_cpy(char *d, const char *s, unsigned max)
{
    unsigned i = 0;
    while (s[i] && i + 1 < max) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}
/* decimal with an optional fixed point: fmt_fix(buf, -63, 1) -> "-6.3" */
static void fmt_fix(char *b, int32_t v, int decimals)
{
    char t[12];
    int n = 0, neg = v < 0, i;
    uint32_t u = (uint32_t)(neg ? -v : v);
    do {
        t[n++] = (char)('0' + u % 10u);
        u /= 10u;
        if (n == decimals)
            t[n++] = '.';
    } while (u || n <= decimals);
    if (neg)
        t[n++] = '-';
    for (i = 0; i < n; i++)
        b[i] = t[n - 1 - i];
    b[n] = 0;
}
static void fmt_int(char *b, int32_t v) { fmt_fix(b, v, 0); }

#endif
