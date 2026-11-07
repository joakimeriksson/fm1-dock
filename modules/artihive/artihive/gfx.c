/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Small-canvas renderer (no full framebuffer). Draw text/lines into
 * an off-screen strip, then blit it in one DMA transfer. Pixels are stored
 * byte-swapped (the panel takes RGB565 big-endian). */
typedef struct {               /* proportional, see tools/gen_font.py */
    uint8_t h;
    uint8_t pad;               /* bitmap starts this many pixels left of the pen */
    uint8_t first, last;
    const uint8_t *adv;        /* advance per glyph */
    const uint8_t *bw;         /* bitmap width per glyph (starts FONT_PAD left of the pen) */
    const uint16_t *off;       /* byte offset of each glyph */
    const uint8_t *data;
} felucca_font_t;
#include "felucca_font.h"

#ifndef CV_MAX
#define CV_MAX (240u * 124u)      /* the graph strip is 240 x 124 */
#endif
static uint16_t cv_px[CV_MAX];
static uint32_t cv_w, cv_h;
static int32_t cv_oy;            /* y offset for graph drawing */

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define C_BLACK 0x0000u
#define C_WHITE 0xFFFFu              /* accent only: what is being touched / where we are */
#define C_KNOB  RGB(16, 18, 22)      /* the black knob caps, on every colourway */
#define C_CHROME RGB(198, 206, 214)  /* the knob collars */
/* The sensor contacts are metal on every colourway: the same grey, never the panel's hue. */
#define C_METAL      RGB(168, 176, 184)
#define C_METAL_HI   RGB(226, 232, 238)
#define C_METAL_RIM  RGB(58, 63, 70)
#define C_METAL_GLINT RGB(244, 248, 252)

/* Seven original ARTIHIVE colourways. Each panel is a painted field with its own print:
 * dark panels use light printing, light panels use dark printing, and sensor pads follow it.
 *   bg    the painted panel
 *   c[5]  the printing, faint rule -> full strength (so on a light panel it runs light -> dark)
 *   pad   the sensor pad's printed body
 *   light 1 when the panel is light: the app darkens what it highlights instead of brightening */
typedef struct {
    const char *name;
    uint16_t bg;
    uint16_t c[5];
    uint16_t pad;
    uint16_t edge;             /* the pad's border, in this colourway's own hue */
    uint8_t light;
} palette_t;
static const palette_t PALETTES[] = {
    /* Deep Blue */
    {"DEEP BLUE", RGB(26, 139, 215),
     {RGB(12, 88, 143), RGB(136, 190, 228), RGB(190, 222, 245), RGB(226, 241, 252), RGB(255, 255, 255)},
     RGB(235, 245, 252), RGB(9, 74, 122), 0},
    /* Porcelain */
    {"PORCELAIN", RGB(240, 241, 245),
     {RGB(196, 199, 205), RGB(150, 155, 163), RGB(104, 110, 119), RGB(58, 62, 70), RGB(16, 18, 22)},
     RGB(28, 30, 34), RGB(108, 116, 128), 1},
    /* Rose */
    {"ROSE", RGB(240, 168, 190),
     {RGB(198, 126, 150), RGB(158, 88, 112), RGB(112, 56, 76), RGB(62, 30, 42), RGB(20, 14, 18)},
     RGB(28, 22, 26), RGB(178, 62, 100), 1},
    /* Toxic Green #BBD4B7 */
    {"TOXIC GREEN", RGB(187, 212, 183),
     {RGB(146, 172, 143), RGB(108, 133, 106), RGB(74, 94, 73), RGB(42, 54, 42), RGB(16, 20, 16)},
     RGB(24, 28, 24), RGB(74, 112, 70), 1},
    /* Ember */
    {"EMBER", RGB(232, 118, 32),
     {RGB(184, 86, 18), RGB(146, 62, 10), RGB(102, 40, 4), RGB(58, 22, 2), RGB(20, 12, 8)},
     RGB(26, 18, 12), RGB(168, 66, 8), 1},
    /* Carbon: warm light printing */
    {"CARBON", RGB(20, 21, 24),
     {RGB(62, 60, 52), RGB(112, 108, 92), RGB(164, 158, 132), RGB(212, 206, 176), RGB(240, 234, 208)},
     RGB(232, 226, 200), RGB(150, 142, 112), 0},
    /* Oxide: dark brown with warm light printing */
    {"OXIDE", RGB(56, 36, 29),
     {RGB(96, 66, 50), RGB(140, 104, 78), RGB(184, 150, 116), RGB(220, 196, 162), RGB(244, 232, 208)},
     RGB(236, 222, 196), RGB(166, 88, 44), 0},
    /* Blush Vein: dark wine-red panel with independent flowing pink veins */
    {"BLUSH VEIN", RGB(130, 42, 70),
     {RGB(94, 31, 53), RGB(177, 91, 119), RGB(218, 143, 165), RGB(244, 196, 209), RGB(255, 238, 243)},
     RGB(245, 218, 227), RGB(184, 74, 112), 0},
    /* Four pastel fields, each with an independent vein map. */
    {"LAVENDER", RGB(205, 183, 226),
     {RGB(176, 151, 201), RGB(139, 112, 169), RGB(101, 76, 130), RGB(68, 47, 94), RGB(37, 25, 55)},
     RGB(49, 34, 68), RGB(146, 105, 181), 1},
    {"LIME VEIL", RGB(201, 226, 160),
     {RGB(169, 197, 127), RGB(128, 159, 89), RGB(91, 121, 58), RGB(57, 82, 34), RGB(29, 46, 17)},
     RGB(38, 56, 23), RGB(132, 175, 80), 1},
    {"VANILLA", RGB(239, 224, 184),
     {RGB(207, 188, 145), RGB(168, 145, 103), RGB(126, 104, 67), RGB(82, 64, 38), RGB(45, 34, 20)},
     RGB(55, 42, 25), RGB(188, 154, 102), 1},
    {"PEARL GREY", RGB(208, 215, 220),
     {RGB(178, 188, 196), RGB(139, 153, 164), RGB(100, 116, 129), RGB(65, 79, 91), RGB(34, 43, 51)},
     RGB(42, 51, 59), RGB(137, 157, 173), 1},
};
#define NPALETTES (sizeof(PALETTES) / sizeof(PALETTES[0]))
static uint16_t pal[5];
static uint16_t pal_bg, pal_pad, pal_edge;
static uint8_t pal_light, pal_oxide, pal_index;
#define C_BG pal_bg                /* the painted panel */
#define C_PAD pal_pad              /* the sensor pad's printed body */
#define C_EDGE pal_edge            /* the pad's border, in the colourway's own hue */
#define C_LIGHT pal_light          /* 1: a light panel, highlights go dark instead of bright */
#define C_LINE pal[0]                /* 1 rules, separators */
#define C_DIM pal[1]                 /* 2 inactive, empty steps, units */
#define C_GRAY pal[2]                /* 3 labels */
#define C_AMB pal[3]                 /* 4 secondary text */
#define C_HI pal[4]                  /* 5 values, curves */
/* Graph ink is deliberately neutral: black on light panels and on the signature
 * Blush Vein theme, light on the remaining dark panels. This keeps information independent
 * from decorative panel colour while preserving two readable intensity levels. */
#define C_GRAPH_HI  (pal_index == 7u || pal_light ? RGB(0, 0, 0) : C_HI)
#define C_GRAPH_DIM (pal_index == 7u ? RGB(52, 20, 31) : (pal_light ? RGB(72, 72, 72) : C_DIM))

static uint16_t pair_colour(uint32_t pair)
{
    static const uint16_t dark[4] = {
        RGB(255, 170, 40), RGB(64, 205, 255), RGB(238, 90, 190), RGB(105, 225, 105)
    };
    static const uint16_t light[4] = {
        RGB(154, 74, 0), RGB(0, 92, 170), RGB(145, 28, 105), RGB(18, 118, 45)
    };
    return (pal_light ? light : dark)[pair & 3u];
}

static void palette_set(uint32_t i)
{
    uint32_t k;
    for (k = 0; k < 5u; k++)
        pal[k] = PALETTES[i % NPALETTES].c[k];
    pal_bg = PALETTES[i % NPALETTES].bg;
    pal_pad = PALETTES[i % NPALETTES].pad;
    pal_edge = PALETTES[i % NPALETTES].edge;
    pal_light = PALETTES[i % NPALETTES].light;
    pal_index = (uint8_t)(i % NPALETTES);
    { uint32_t n = i % NPALETTES;
      pal_oxide = (uint8_t)(n == 7u ? 1u : n == 6u ? 2u : n >= 8u ? (n - 5u) : 0u); }
}

static inline uint16_t swap16(uint32_t c) { return (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8)); }

static void cv_begin(uint32_t w, uint32_t h, uint16_t bg)
{
    uint32_t i, n;
    if (w * h > CV_MAX)
        h = CV_MAX / w;
    lcd_sync();                     /* the last blit may still read cv_px */
    cv_w = w;
    cv_h = h;
    n = w * h;
    for (i = 0; i < n; i++)
        cv_px[i] = swap16(bg);
}

/* Strong OXIDE patina, generated from screen coordinates: no bitmap or extra flash asset.
 * Only the panel background receives it; controls and silkscreen are drawn afterwards. */
static void cv_oxide(uint32_t ox, uint32_t oy)   /* fm1-dock: the patina maps left out (take.py) */
{
    (void)ox;
    (void)oy;
}

static void cv_blit(uint32_t x, uint32_t y) { lcd_blit(x, y, cv_w, cv_h, cv_px); }

/* Paint the panel as one continuous 240 x 240 field. CV_MAX holds 88 rows, so the transfer
 * is split internally, but every chunk samples OXIDE_TEX with absolute screen coordinates. */
static void draw_panel_bg(void)
{
    uint32_t y;
    for (y = 0; y < 240u; y += 88u) {
        uint32_t h = 240u - y;
        if (h > 88u) h = 88u;
        cv_begin(240, h, C_BG);
        cv_oxide(0, y);
        cv_blit(0, y);
    }
    lcd_sync();
}

/* canvas rows r0 .. cv_h-1 only, to screen row y + r0 */
static void cv_blit_from(uint32_t x, uint32_t y, uint32_t r0)
{
    if (r0 < cv_h)
        lcd_blit(x, y + r0, cv_w, cv_h - r0, cv_px + r0 * cv_w);
}

static inline void cv_pset(int32_t x, int32_t y, uint16_t c)
{
    y += cv_oy;
    if ((uint32_t)x < cv_w && (uint32_t)y < cv_h)
        cv_px[(uint32_t)y * cv_w + (uint32_t)x] = swap16(c);
}

static void cv_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t x1 = x + w, y1 = y + h + cv_oy, i;
    uint16_t sc = swap16(c);
    y += cv_oy;                             /* clipped once, then filled row by row */
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > (int32_t)cv_w)
        x1 = (int32_t)cv_w;
    if (y1 > (int32_t)cv_h)
        y1 = (int32_t)cv_h;
    for (; y < y1; y++)
        for (i = x; i < x1; i++)
            cv_px[(uint32_t)y * cv_w + (uint32_t)i] = sc;
}

static void cv_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy, guard = 2000;
    while (guard--) {                       /* bounded: a line is never longer than 480 px */
        int32_t e2 = 2 * err;               /* both tests use the same error value */
        cv_pset(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* glyph index of a character: lower case folds to upper case when the font
 * has none, anything missing (and C1 controls) draws as '?' */
static uint32_t glyph(const felucca_font_t *f, uint32_t ch)
{
    if (ch >= 'a' && ch <= 'z' && f->last < 'a')
        ch -= 32u;
    if (ch < f->first || ch > f->last || (ch >= 127u && ch < 160u))
        ch = '?';
    return ch - f->first;
}

/* text, alpha-blended onto black with colour c; returns the end x */
/* a filled disc: rows, each a half-width from the circle equation, no sqrt and no float */
static void cv_disc(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t dy;
    for (dy = -r; dy <= r; dy++) {
        int32_t dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
        cv_rect(cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}

/* the panel's silkscreen box: a thin outline with the corners taken off */
static void cv_box(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    cv_rect(x + 2, y, w - 4, 1, c);
    cv_rect(x + 2, y + h - 1, w - 4, 1, c);
    cv_rect(x, y + 2, 1, h - 4, c);
    cv_rect(x + w - 1, y + 2, 1, h - 4, c);
    cv_pset(x + 1, y + 1, c);
    cv_pset(x + w - 2, y + 1, c);
    cv_pset(x + 1, y + h - 2, c);
    cv_pset(x + w - 2, y + h - 2, c);
}

static int32_t cv_text_raw(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c, uint32_t solid)
{
    uint16_t ramp[16];
    uint32_t r=c>>11,g=(c>>5)&63u,b=c&31u,a;
    for(a=0;a<16u;a++)ramp[a]=solid&&a?c:(uint16_t)(((r*a/15u)<<11)|((g*a/15u)<<5)|(b*a/15u));
    for(;*s;s++){
        uint32_t gi=glyph(f,(uint8_t)*s),gx,gy,w=f->bw[gi],bpr=(w+1u)/2u;const uint8_t *gd=f->data+f->off[gi];
        for(gy=0;gy<f->h;gy++)for(gx=0;gx<w;gx++){
            uint32_t v=gd[gy*bpr+gx/2u];v=(gx&1u)?(v&15u):(v>>4);
            if(v)cv_pset(x-f->pad+(int32_t)gx,y+(int32_t)gy,ramp[v]);
        }
        x+=f->adv[gi];
    }
    return x;
}

static int32_t cv_text(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    if(pal_index>=7u){
        uint16_t ink=pal_light?C_BLACK:C_WHITE,edge=pal_light?C_WHITE:C_BLACK;
        /* Four-sided opposite-colour keyline keeps every glyph separate from both the light
         * and dark branches of a vein. Draw the full outline first, then the solid letter. */
        cv_text_raw(x-1,y,f,s,edge,1u);cv_text_raw(x+1,y,f,s,edge,1u);
        cv_text_raw(x,y-1,f,s,edge,1u);cv_text_raw(x,y+1,f,s,edge,1u);
        return cv_text_raw(x,y,f,s,ink,1u);
    }
    return cv_text_raw(x,y,f,s,c,0u);
}

/* Where the first inked column of a string falls, relative to the pen. The glyphs carry a
 * different left side bearing (M has none, most letters one pixel), so two boxes placed at
 * the same x do not look aligned. Callers that want the ink on a column use this to place
 * the pen. Returns 0 for an empty string or a space. */
static int32_t text_bearing(const felucca_font_t *f, const char *s)
{
    uint32_t gi, gx, gy, w, bpr;
    const uint8_t *gd;
    while (*s == ' ') s++;
    if (!*s) return 0;
    gi = glyph(f, (uint8_t)*s);
    w = f->bw[gi];
    bpr = (w + 1u) / 2u;
    gd = f->data + f->off[gi];
    for (gx = 0; gx < w; gx++)
        for (gy = 0; gy < f->h; gy++) {
            uint32_t v = gd[gy * bpr + gx / 2u];
            v = (gx & 1u) ? (v & 15u) : (v >> 4);
            if (v) return (int32_t)gx - (int32_t)f->pad;
        }
    return 0;
}

/* text whose first inked column lands exactly on x, whatever the first letter is */
static int32_t text_pen_x(int32_t x, const felucca_font_t *f, const char *s)
{
    int32_t p = x - text_bearing(f, s);
    return p < 0 ? 0 : p;
}

static int32_t text_w(const felucca_font_t *f, const char *s)
{
    int32_t w = 0;
    for (; *s; s++)
        w += f->adv[glyph(f, (uint8_t)*s)];
    return w;
}

/* one-shot: text in a box, cleared to black, blitted */
static void draw_text_box(uint32_t x, uint32_t y, uint32_t w, const felucca_font_t *f, const char *s,
                          uint16_t c, int align)
{
    int32_t tw = text_w(f, s), tx = 0;
    cv_begin(w, f->h, pal_bg);
    cv_oxide(x, y);
    if (align == 1)
        tx = ((int32_t)w - tw) / 2;
    else if (align == 2)
        tx = (int32_t)w - tw;
    cv_text(tx, 0, f, s, c);
    cv_blit(x, y);
    lcd_sync();                     /* one-shots (boot, crash, UBOOT, update) finish here */
}
