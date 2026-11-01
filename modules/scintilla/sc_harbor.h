/* SPDX-License-Identifier: GPL-3.0-only */
/* What Dinghy's platform.c (gfx.c, libc.c) gives Scintilla's scintilla.c, on the fm1-dock host: the
 * panel, the time and MIDI (modules/lib/hb_fm1.h), the screen calls (the host's canvas and font,
 * as CS-1's cs1_platform.h), the settings record, the block, the render on both cores (SC_RENDER).
 * scintilla.c includes it, built with -DHB_MODULE (by this name: it was fm1-harbor's). platform_poll:
 * nothing (the host's main loop polls; the audio runs in its interrupt). The DIN jack is the host's:
 * its MIDI comes through midi(). */
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
static void platform_poll(void) {}

/* ---- the screen (gfx.c's calls; the host draws with the same fonts) */
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
static void draw_text_box(uint32_t x, uint32_t y, uint32_t w, const felucca_font_t *f, const char *s,
                          uint16_t c, int align)   /* gfx.c's, without its lcd_sync */
{
    int32_t tw = text_w(f, s), tx = 0;
    cv_begin(w, f->h, C_BLACK);
    if (align == 1)
        tx = ((int32_t)w - tw) / 2;
    else if (align == 2)
        tx = (int32_t)w - tw;
    cv_text(tx, 0, f, s, c);
    cv_blit(x, y);
}

/* ---- the render: the voices on both cores. Core 1 (the host's c1_run / c1_wait, when it lends it)
 * renders half of the sounding voices (sc_render_split: every other one, the odd one out too: core 0
 * also runs the choruses, the reverb and the limiter) on the engine's second lane into a bus of its
 * own, while core 0 renders the rest; sc_render_end sums the two. The same samples as sc_render
 * (integer sums). Without core 1 (none, not yet screened, held after a fault) core 0 renders them all;
 * if core 1 does not finish in time (it is held from then on) core 0 renders its voices too, from
 * where core 1 left them, this once. Needs -DSC_LANES=2 (module.cfg). */
#include "sc_synth.h"
#define SC_RENDER(s, out, n) sc_render_hb(s, out, n)
static int32_t hb_bus1[2][256];
static struct { sc_synth_t *s; uint32_t mask, n; } hb_job;
static void hb_c1_job(uint32_t arg)            /* on core 1 */
{
    (void)arg;
    sc_render_voices(hb_job.s, hb_bus1, hb_job.mask, 1, hb_job.n);
}
static void sc_render_hb(sc_synth_t *s, int32_t *out, uint32_t n)
{
    uint32_t m0, m1;
    if (n > 256u)
        n = 256u;
    sc_render_begin(s);
    m1 = sc_render_split(s, &m0);               /* (core 1 takes the first: with an odd number, one more) */
    hb_job.s = s;
    hb_job.mask = m1;
    hb_job.n = n;
    if (!m1 || !HB_HOST_HAS(H, c1_wait) || !H->c1_run(hb_c1_job, 0)) {
        sc_render_voices(s, s->bus, m0 | m1, 0, n);
        sc_render_end(s, 0, out, n);
        return;
    }
    sc_render_voices(s, s->bus, m0, 0, n);
    if (H->c1_wait() != 0)
        hb_c1_job(0);
    sc_render_end(s, (const int32_t (*)[256])hb_bus1, out, n);
}

/* ---- settings */
static int settings_load(void *p, uint32_t n) { return H->load(p, n); }
static int settings_save(const void *p, uint32_t n) { H->save(p, n); return 0; }

/* ---- libc.c's */
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
static void fmt_fix(char *b, int32_t v, int decimals)   /* fmt_fix(buf, -63, 1) -> "-6.3" */
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
