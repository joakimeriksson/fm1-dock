/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Andrew GG — Roland S-1 parity engine */
/* The screen: an own design (no copied trade dress). A blue-black panel with light blue legends, as the classic
 * American polys had them, a red LED program number drawn as 7-segment digits, "S1VAVE" with three blue bands
 * through the lower half of the letters, four anti-aliased round knobs with a blue value arc, a scope strip, eight
 * round voice LEDs. 240 x 240:
 *   0..43    header: program number, name, tags, the logo
 *   46..143  the page: title between rules, four knobs (label above, value below)
 *   146..195 the scope
 *   200..236 the voice LEDs */
#define C_PANEL RGB(12, 14, 15)                 /* Deep obsidian matte black */
#define C_CREAM RGB(242, 246, 250)              /* Ice crisp white */
#define C_AMBER RGB(0, 230, 163)                /* Roland S-1 Neon Emerald Teal (#00e5a3) */
#define C_ORANGE RGB(255, 122, 0)               /* Roland S-1 Bright Tangerine Orange (#ff7a00) */
#define C_RED RGB(255, 36, 36)                  /* High-brightness 7-segment LED red */
#define C_RED_DIM RGB(42, 8, 8)                 /* 7-segment LED unlit */
#define C_DIMT RGB(125, 140, 146)               /* Precision grey-cyan */
#define C_RULE RGB(32, 38, 42)                  /* Modern panel border line */
#define C_KNOB RGB(22, 25, 27)                  /* Dark tactile knob cap */
#define C_KNOB_HI RGB(54, 62, 66)               /* Knob bevel rim */
#define C_SCOPE_BG RGB(8, 10, 11)               /* Pitch-black scope chamber */
#define C_SCOPE_MID RGB(24, 30, 32)             /* Subtle center zero line */
#define C_SCOPE_GRID RGB(16, 22, 24)            /* Precision graticule grid */
#define C_MUTED RGB(75, 90, 96)                 /* Muted footer text */
#define C_CYAN_LOGO RGB(14, 186, 251)           /* Exact Andrew GG electric cyan (#0ebafb) */
/* Page & parameter category colors for hierarchy & modulation contrast */
#define C_CAT_OSC  C_AMBER                      /* Emerald teal: Oscillator & Perform */
#define C_CAT_FLT  C_ORANGE                     /* Tangerine orange: Filter & Filter Char */
#define C_CAT_MOD  RGB(32, 188, 255)            /* Electric sky-blue: LFO, ENV & Modulation */
#define C_CAT_PWM  RGB(255, 212, 36)            /* Amber gold: PWM depth & source */
#define C_CAT_FX   RGB(96, 152, 255)            /* Cobalt blue: Chorus, Delay, Reverb, Stereo/FX */
#define C_CAT_ARP  RGB(184, 108, 255)           /* Vivid violet: Arpeggiator */
#define C_CAT_GLO  RGB(255, 192, 64)            /* Gold amber: Voice, Global & System */
static const uint16_t STRIPE[3] = {RGB(0, 140, 95), RGB(0, 230, 163), RGB(40, 255, 185)};

/* a * (1 - t) + b * t for RGB565, t = 0..256 */
static uint16_t blend565(uint16_t a, uint16_t b, uint32_t t)
{
    uint32_t r = ((a >> 11) * (256u - t) + (b >> 11) * t) >> 8;
    uint32_t g = (((a >> 5) & 63u) * (256u - t) + ((b >> 5) & 63u) * t) >> 8;
    uint32_t bl = ((a & 31u) * (256u - t) + (b & 31u) * t) >> 8;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

static inline uint16_t cv_get(int32_t x, int32_t y)
{
    y += cv_oy;
    if ((uint32_t)x < cv_w && (uint32_t)y < cv_h)
        return swap16(cv_px[(uint32_t)y * cv_w + (uint32_t)x]);
    return C_PANEL;
}

/* an anti-aliased disc: coverage from the distance to the centre (pixel centres at +0.5, so in doubled units),
 * blended over what is there. r in pixels, ring: 0 = full disc, else only r - ring .. r */
static void cv_disc(int32_t cx, int32_t cy, int32_t r, int32_t ring, uint16_t c)
{
    int32_t x, y, ro2 = (2 * r + 1) * (2 * r + 1), ri2 = (2 * r - 1) * (2 * r - 1);
    int32_t in2 = ring ? (2 * (r - ring) + 1) * (2 * (r - ring) + 1) : -1, in1 = ring ? (2 * (r - ring) - 1) * (2 * (r - ring) - 1) : -1;
    for (y = cy - r - 1; y <= cy + r + 1; y++)
        for (x = cx - r - 1; x <= cx + r + 1; x++) {
            int32_t dx = 2 * (x - cx), dy = 2 * (y - cy), d2 = dx * dx + dy * dy, t;
            if (d2 >= ro2 || (ring && d2 < in1))
                continue;
            t = 256;
            if (d2 > ri2)                       /* the outer edge: 1 px of coverage */
                t = 256 * (ro2 - d2) / (ro2 - ri2);
            if (ring && d2 < in2)               /* the inner edge */
                t = 256 * (d2 - in1) / (in2 - in1);
            cv_pset(x, y, t >= 256 ? c : blend565(cv_get(x, y), c, (uint32_t)t));
        }
}

/* a line of width 2, for the arcs and pointers */
static void cv_line2(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    cv_line(x0, y0, x1, y1, c);
    cv_line(x0 + 1, y0, x1 + 1, y1, c);
    cv_line(x0, y0 + 1, x1, y1 + 1, c);
}

/* the knob's angle: value 0 at 225 deg (lower left), max at -45 deg (lower right), 270 deg of travel */
static void knob_xy(int32_t cx, int32_t cy, int32_t rad, int32_t deg, int32_t *x, int32_t *y)
{
    int32_t i;
    while (deg < 0) deg += 360;
    while (deg >= 360) deg -= 360;
    i = deg / 5;
    *x = cx + ((rad * COS_Q8[i]) >> 8);
    *y = cy - ((rad * COS_Q8[(i + 54u) % 72u]) >> 8);   /* sin = cos(deg - 90): 90 deg = 18 steps back = 54 on */
}

/* AIRA Studio: segmented LED collar around knob (19 discrete backlit LED tick segments across 270 degrees) */
static void cv_led_collar(int32_t cx, int32_t cy, int32_t r_in, int32_t r_out, int32_t cur_ang, int32_t bip, uint16_t lit_c)
{
    const int32_t NSEG = 19;
    const int32_t TOTAL_DEG = 270;
    const int32_t START_DEG = 225;
    int32_t s;
    for (s = 0; s < NSEG; s++) {
        int32_t deg = START_DEG - (s * TOTAL_DEG) / (NSEG - 1);
        int32_t x0, y0, x1, y1;
        int32_t is_lit;
        uint16_t seg_c;
        if (bip) {
            int32_t mid_deg = 90;
            if (cur_ang <= mid_deg) {
                is_lit = (deg <= mid_deg && deg >= cur_ang);
            } else {
                is_lit = (deg >= mid_deg && deg <= cur_ang);
            }
        } else {
            is_lit = (deg >= cur_ang);
        }
        uint16_t off_c = blend565(RGB(22, 26, 30), lit_c, 52);
        seg_c = is_lit ? lit_c : off_c;
        knob_xy(cx, cy, r_in, deg, &x0, &y0);
        knob_xy(cx, cy, r_out, deg, &x1, &y1);
        cv_line2(x0, y0, x1, y1, seg_c);
    }
}

/* AIRA Studio Knob: LED collar with cardinal detents, tactile bevel rim, and illuminated indicator pip */
static void cv_knob(int32_t cx, int32_t cy, const char *label, const char *value, int32_t v, int32_t max, int32_t bip, uint16_t arc_c)
{
    int32_t ang = 225 - (max ? 270 * v / max : 0), px, py, qx, qy, r = 14, i;
    static const int16_t TICKS[3] = {225, 90, -45};
    if (!arc_c) arc_c = (label[0] == 'C' && label[1] == 'U') || (label[0] == 'R' && label[1] == 'E' && label[2] == 'S') ? C_ORANGE : C_AMBER;
    uint16_t bezel = blend565(C_KNOB_HI, arc_c, 96);
    uint16_t rim_hi = blend565(bezel, C_CREAM, 40);
    uint16_t lbl_c = blend565(arc_c, C_CREAM, 65);
    cv_rect(cx - 12, cy - 49, 24, 2, arc_c);
    cv_text(cx - text_w(&FONT_S, label) / 2, cy - 44, &FONT_S, label, lbl_c);
    for (i = 0; i < 3; i++) {
        knob_xy(cx, cy, r + 9, TICKS[i], &px, &py);
        cv_rect(px, py, 2, 2, C_MUTED);
    }
    cv_led_collar(cx, cy, r + 3, r + 7, ang, bip, arc_c);
    cv_disc(cx, cy, r, 0, C_KNOB);
    cv_disc(cx, cy, r, 2, bezel);
    cv_disc(cx, cy, r - 2, 1, rim_hi);
    cv_disc(cx, cy, 4, 0, C_SCOPE_BG);
    knob_xy(cx, cy, 4, ang, &px, &py);
    knob_xy(cx, cy, r - 1, ang, &qx, &qy);
    cv_line2(px, py, qx, qy, arc_c);
    cv_text(cx - text_w(&FONT_S, value) / 2, cy + 24, &FONT_S, value, C_CREAM);
}

/* 7-segment digit, w 16 x h 28, thickness 4: segments a (top) b c d (bottom) e f g (middle) */
static void cv_seg7(int32_t x, int32_t y, uint32_t digit, uint16_t on, uint16_t off)
{
    static const uint8_t SEG[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    uint32_t m = SEG[digit % 10u];
    const int32_t t = 4, w = 16, h = 28;
    cv_rect(x + 2, y, w - 4, t, m & 1u ? on : off);                       /* a */
    cv_rect(x + w - t, y + 2, t, h / 2 - 3, m & 2u ? on : off);           /* b */
    cv_rect(x + w - t, y + h / 2 + 1, t, h / 2 - 3, m & 4u ? on : off);   /* c */
    cv_rect(x + 2, y + h - t, w - 4, t, m & 8u ? on : off);               /* d */
    cv_rect(x, y + h / 2 + 1, t, h / 2 - 3, m & 16u ? on : off);          /* e */
    cv_rect(x, y + 2, t, h / 2 - 3, m & 32u ? on : off);                  /* f */
    cv_rect(x + 2, y + h / 2 - 2, w - 4, t, m & 64u ? on : off);          /* g */
}

/* the stripes: three bands of `band` px from row y0. In the letters (text_only, the logo) the text takes the
 * band's colour by its brightness; across the screen (the splash) the band runs up to x_to and the letters
 * stand in it as dark cut-outs */
static void cv_stripes(int32_t y0, int32_t band, int32_t gap, int32_t x_to, int32_t text_only)
{
    int32_t b, y, x;
    for (b = 0; b < 3; b++)
        for (y = y0 + b * (band + gap); y < y0 + b * (band + gap) + band; y++)
            for (x = 0; x < (int32_t)cv_w; x++) {
                uint16_t c = cv_get(x, y);
                uint32_t g = (c >> 5) & 63u, is_text = c != C_PANEL && c != C_BLACK && g > 14u;   /* brighter than the panel */
                if (text_only) {
                    if (is_text) cv_pset(x, y, blend565(C_PANEL, STRIPE[b], g * 256u / 63u));
                } else if (x < x_to)
                    cv_pset(x, y, is_text ? blend565(STRIPE[b], C_PANEL, g * 256u / 63u) : STRIPE[b]);
            }
}

#include "s1v_brand.inc"                      /* fm1-dock: the name in text (take.py) */

/* ---- the parts of the screen (the app decides when) */
#define C_TRK_B RGB(255, 150, 40)               /* track B's orange */
/* b: the panel is on track B's own sound (its program number and name in B's orange, a B before the name) */
static void draw_battery_icon(int32_t x0, int32_t y0, int32_t bars)
{
    cv_rect(x0, y0, 14, 1, C_DIMT);
    cv_rect(x0, y0 + 7, 14, 1, C_DIMT);
    cv_rect(x0, y0, 1, 8, C_DIMT);
    cv_rect(x0 + 13, y0, 1, 8, C_DIMT);
    cv_rect(x0 + 14, y0 + 2, 2, 4, C_DIMT);
    cv_rect(x0 + 1, y0 + 1, 12, 6, C_PANEL);
    uint16_t c = (bars >= 3) ? C_AMBER : (bars == 2) ? C_ORANGE : C_RED;
    if (bars <= 0) {
        if ((fm1_ms / 500u) & 1u) cv_rect(x0 + 2, y0 + 2, 2, 4, C_RED);
        return;
    }
    for (int32_t b = 0; b < bars && b < 4; b++) {
        cv_rect(x0 + 2 + b * 3, y0 + 2, 2, 4, c);
    }
}

static void ui_header(uint32_t prog, const uint8_t *name, uint32_t edited, const char *tags, uint32_t b)
{
    char n[PATCH_NAME + 2];
    uint32_t i;
    cv_begin(240, 44, C_PANEL);
    cv_seg7(6, 6, (prog + 1u) / 10u, b ? C_TRK_B : C_RED, C_RED_DIM);
    cv_seg7(26, 6, (prog + 1u) % 10u, b ? C_TRK_B : C_RED, C_RED_DIM);
    for (i = 0; i < PATCH_NAME; i++) n[i] = (char)name[i];
    n[PATCH_NAME] = edited ? '*' : 0;
    n[PATCH_NAME + 1] = 0;
    if (b) cv_text(54, 4, &FONT_S, "B", C_TRK_B);
    cv_text(b ? 66 : 54, 4, &FONT_S, n, b ? C_TRK_B : C_CREAM);
    cv_text(54, 22, &FONT_S, tags, C_DIMT);
    draw_logo_small(240 - LOGO_W - 6, 6);
    cv_rect(0, 42, 240, 1, C_RULE);
    cv_blit(0, 0);
}

typedef struct { const char *label; char value[12]; int32_t v, max, bip, present; uint16_t col; } knob_view_t;
static void ui_page(const char *title, const char *cat_tag, uint16_t cat_col, const knob_view_t *k, const char *note, const char *pg_pos)
{
    int32_t tw = text_w(&FONT_S, title), pw = pg_pos ? text_w(&FONT_S, pg_pos) : 0, nw = note ? text_w(&FONT_S, note) : 0, i;
    int32_t tag_w = (cat_tag && cat_tag[0]) ? (text_w(&FONT_S, cat_tag) + 6) : 0;
    int32_t left_x = tag_w ? (3 + tag_w + 5) : 6;
    int32_t bx = pg_pos ? (237 - (pw + 8)) : 237;
    int32_t right_x = pg_pos ? (bx - 5) : 234;
    int32_t tx = note ? left_x : (240 - tw) / 2;
    uint16_t bg_fill;
    if (!cat_col) cat_col = C_AMBER;
    bg_fill = blend565(C_SCOPE_BG, cat_col, 24);
    cv_begin(240, 98, C_PANEL);
    /* Category-colored outline banner across the page header */
    cv_rect(2, 0, 236, 17, cat_col);
    cv_rect(3, 1, 234, 15, bg_fill);
    if (tag_w) {
        cv_rect(3, 1, tag_w, 15, cat_col);
        cv_text(6, 1, &FONT_S, cat_tag, C_PANEL);
    }
    if (pg_pos) {
        cv_rect(bx - 1, 1, 1, 15, cat_col);
        cv_rect(bx, 1, pw + 7, 15, C_SCOPE_BG);
        cv_text(bx + 4, 1, &FONT_S, pg_pos, cat_col);
    }
    if (!note) {
        if (tx + tw > right_x) tx = right_x - tw;
        if (tx < left_x) tx = left_x;
    }
    cv_text(tx, 1, &FONT_S, title, cat_col);
    if (note) {
        int32_t nx = right_x - nw;
        if (nx < tx + tw + 4) nx = tx + tw + 4;
        cv_text(nx, 1, &FONT_S, note, pg_pos ? C_ORANGE : C_RED);
    }
    for (i = 0; i < 4; i++)
        if (k[i].present) {
            cv_knob(30 + 60 * i, 62, k[i].label, k[i].value, k[i].v, k[i].max, k[i].bip, k[i].col ? k[i].col : cat_col);
            app_idle();                         /* (a knob is ~1 ms: the keys and MIDI in between) */
        }
    cv_blit(0, 46);
}

/* AIRA Studio Oscilloscope: phosphor dual-trace wave with subtle oscilloscope graticule grid */
static void ui_scope(const int16_t *s, uint32_t n)
{
    uint32_t i;
    int32_t gx, gy;
    cv_begin(240, 50, C_SCOPE_BG);
    /* Graticule grid */
    for (gx = 20; gx < 240; gx += 40) cv_line(gx, 0, gx, 49, C_SCOPE_GRID);
    for (gy = 10; gy < 50; gy += 15) cv_line(0, gy, 239, gy, C_SCOPE_GRID);
    cv_rect(0, 25, 240, 1, C_SCOPE_MID);
    /* Dual-trace phosphor effect */
    for (i = 1; i < n; i++) {
        int32_t y0 = 25 - clampi(s[i - 1u] >> 9, -22, 22);
        int32_t y1 = 25 - clampi(s[i] >> 9, -22, 22);
        /* Ghost secondary trace (slight lag/depth) */
        int32_t gy0 = 25 - clampi((s[i - 1u] * 3) >> 11, -20, 20);
        int32_t gy1 = 25 - clampi((s[i] * 3) >> 11, -20, 20);
        cv_line((int32_t)i - 1, gy0, (int32_t)i, gy1, RGB(0, 90, 60));
        /* Primary bright trace */
        cv_line((int32_t)i - 1, y0, (int32_t)i, y1, C_AMBER);
    }
    cv_blit(0, 146);
}

static void ui_draw_table(const int16_t *draw, uint32_t cur_step)
{
    uint32_t i;
    cv_begin(240, 50, C_SCOPE_BG);
    cv_rect(0, 25, 240, 1, C_SCOPE_MID);
    cv_text(4, 2, &FONT_S, "OSC DRAW: 16-STEP WAVE", C_CREAM);
    for (i = 0; i < 16u; i++) {
        int32_t x = 8 + (int32_t)i * 14;
        int32_t val = (int32_t)draw[i];
        int32_t h = val * 20 / 32768;
        uint16_t c = (i == cur_step) ? C_ORANGE : C_AMBER;
        if (h >= 0) {
            cv_rect(x, 25 - h, 12, h + 1, c);
        } else {
            cv_rect(x, 25, 12, -h + 1, c);
        }
        if (i == cur_step) {
            cv_rect(x, h >= 0 ? 25 - h : 25 - h - 1, 12, 2, C_CREAM);
        }
    }
    cv_blit(0, 146);
}

static void ui_chop_mask(uint16_t mask, uint32_t target)
{
    static const char *const TGT_NAME[4] = {"CHOP TARGET: SQUARE", "CHOP TARGET: SAW", "CHOP TARGET: SUB", "CHOP TARGET: NOISE"};
    uint32_t i;
    cv_begin(240, 50, C_SCOPE_BG);
    cv_rect(0, 25, 240, 1, C_SCOPE_MID);
    cv_text(4, 2, &FONT_S, TGT_NAME[target & 3u], C_CREAM);
    for (i = 0; i < 16u; i++) {
        int32_t x = 8 + (int32_t)i * 14;
        uint32_t on = (mask >> i) & 1u;
        cv_rect(x, 18, 12, 26, on ? C_ORANGE : C_KNOB);
        if (on) {
            cv_rect(x + 2, 20, 8, 22, RGB(255, 150, 40));
        }
    }
    cv_blit(0, 146);
}

/* AIRA Studio 8-Voice Polyphonic Matrix (Voices 1..8): clean sleek meter capsules */
static void ui_leds(const volatile int32_t *level, uint32_t n)
{
    uint32_t v;
    (void)n;
    cv_begin(240, 36, C_PANEL);
    for (v = 0; v < 8u; v++) {
        int32_t slot_x = 8 + (int32_t)v * 28;
        int32_t track_x = slot_x + 7;
        int32_t l = (int32_t)((uint32_t)level[v] >> 7);   /* 0..255 */
        int32_t bar_h = (l * 16) / 255;
        char vdigit[2];
        vdigit[0] = (char)('1' + v); vdigit[1] = 0;
        /* Number 1..8 centered above slot */
        cv_text(slot_x + 9, 2, &FONT_S, vdigit, l > 16 ? C_CREAM : C_DIMT);
        /* Vertical precision meter slot (18px tall, 14px wide) */
        cv_rect(track_x - 1, 17, 16, 18, C_RULE);
        cv_rect(track_x, 18, 14, 16, RGB(18, 22, 24));
        if (bar_h > 0) {
            uint16_t meter_c = (v < 4) ? C_AMBER : C_ORANGE;
            cv_rect(track_x + 2, 18 + (16 - bar_h), 10, bar_h, meter_c);
            if (bar_h >= 14) cv_rect(track_x + 2, 18, 10, 2, C_CREAM);
        }
    }
    cv_blit(0, 200);
}

/* the sequencer's strip (instead of the scope): a line of text, then the 16 steps of track t from w0 (the page the
 * white keys play). A step: its chord as bars by pitch, dim when it plays by chance, grey when switched off, ticks
 * for a ratchet, a bridge to the next for a tie. The playhead's step is blue, the selected one framed in red. */
static void ui_steps(uint32_t t, uint32_t w0, int32_t cursor, int32_t playhead, uint32_t rec, const char *left, const char *right)
{
    uint32_t len = pat.trk[t].len, i, k;
    int32_t lo = 127, hi = 0;
    cv_begin(240, 50, C_SCOPE_BG);
    cv_text(4, 0, &FONT_S, left, C_CREAM);
    if (right) cv_text(236 - text_w(&FONT_S, right), 0, &FONT_S, right, rec ? C_RED : C_AMBER);
    if (!len) {
        cv_text(4, 24, &FONT_S, "TRACK OFF: GIVE IT A LENGTH", C_DIMT);
        cv_blit(0, 146);
        return;
    }
    for (i = 0; i < 16u && w0 + i < len; i++) {   /* the pitch range shown: the window's notes, at least an octave */
        const step_t *st = seq_step(t, w0 + i);
        for (k = 0; k < SEQ_CHORD && st->note[k]; k++) {
            if (st->note[k] < lo) lo = st->note[k];
            if (st->note[k] > hi) hi = st->note[k];
        }
    }
    if (hi < lo) { lo = 48; hi = 72; }
    if (hi - lo < 12) { lo -= (12 - (hi - lo)) / 2; hi = lo + 12; }
    for (i = 0; i < 16u; i++) {
        uint32_t n = w0 + i;
        int32_t x = (int32_t)i * 15, y = 19, h = 29;
        const step_t *st;
        uint16_t bg;
        if (n >= len) break;
        st = seq_step(t, n);
        bg = playhead >= 0 && n == (uint32_t)playhead ? STRIPE[0] : (n & 3u) ? C_KNOB : C_RULE;
        cv_rect(x + 1, y, 13, h, bg);
        if (n % 4u == 0u) cv_rect(x + 1, y, 13, 1, C_KNOB_HI);   /* the beats */
        if (!st->note[0]) cv_rect(x + 6, y + h / 2, 3, 2, C_DIMT);
        else {
            uint16_t c = st->ratch & STEP_OFF ? C_KNOB_HI : st->chance < 100u ? C_DIMT : C_CREAM;
            for (k = 0; k < SEQ_CHORD && st->note[k]; k++) {
                int32_t yy = y + h - 5 - (int32_t)(st->note[k] - lo) * (h - 9) / (hi - lo);
                cv_rect(x + 3, yy, 9, 2, c);
            }
            for (k = 0; k < (st->ratch & 3u); k++) cv_rect(x + 3 + (int32_t)k * 3, y + h - 3, 2, 2, C_AMBER);
            if (st->gate >= GATE_TIE && i < 15u) cv_rect(x + 12, y + h / 2 - 1, 5, 3, C_AMBER);
        }
        if (cursor >= 0 && n == (uint32_t)cursor) {   /* the selected step: a red frame */
            cv_rect(x, y - 1, 15, 2, C_RED); cv_rect(x, y + h - 1, 15, 2, C_RED);
            cv_rect(x, y - 1, 2, h + 1, C_RED); cv_rect(x + 13, y - 1, 2, h + 1, C_RED);
        }
    }
    cv_blit(0, 146);
}

/* the step view's hints (instead of the voice LEDs): what the keys do now, two lines */
static void ui_hints(const char *l1, const char *l2, uint16_t c1)
{
    cv_begin(240, 36, C_PANEL);
    cv_text((240 - text_w(&FONT_S, l1)) / 2, 1, &FONT_S, l1, c1);
    cv_text((240 - text_w(&FONT_S, l2)) / 2, 19, &FONT_S, l2, C_DIMT);
    cv_blit(0, 200);
}

/* ---- the full-screen sequencer (SEQ tapped), as SLOOP's STEP page but on the whole screen:
 *   0..23     the top line: the track, the view, the pattern, the tempo, PLAY / STOP / REC
 *   24..135   the 16 steps around the cursor as a piano roll (a bar per note: its width the gate, its brightness
 *             the velocity), the cursor framed in red, the playhead blue, step numbers under every beat
 *   136..187  a line (what the step holds, or a message), then up to 64 steps as small cells (the roll's 16 lit)
 *   188..239  the four knobs as label, value and a bar */
/* the tracks' colours, everywhere in the sequencer (the user: "not always clear if you are in A or B"): A the light
 * blue accent, B orange (C_TRK_B, above); ui_trk is the track drawn */
static uint32_t ui_trk;
#define C_TRK (ui_trk ? C_TRK_B : C_AMBER)

static void ui_seq_top(uint32_t t, const char *view, const char *mid, const char *right, uint16_t rc, uint32_t b_off, const char *pg_pos)
{
    uint32_t i;
    int32_t vw = text_w(&FONT_S, view), mw = text_w(&FONT_S, mid), rw = text_w(&FONT_S, right);
    int32_t pw = pg_pos ? text_w(&FONT_S, pg_pos) : 0;
    int32_t rx = 236 - rw;
    int32_t bx = rx - pw - 12;
    cv_begin(240, 24, C_PANEL);
    for (i = 0; i < 2u; i++) {                  /* both tracks: the one shown filled, the other a frame */
        char tl[2] = {(char)('A' + i), 0};
        int32_t x = 3 + (int32_t)i * 20;
        uint16_t c = i ? C_TRK_B : C_AMBER;
        if (i == t) { cv_rect(x, 3, 17, 17, c); cv_text(x + 9 - text_w(&FONT_S, tl) / 2, 3, &FONT_S, tl, C_PANEL); }
        else {
            uint16_t d = i && b_off ? C_KNOB_HI : blend565(C_PANEL, c, 150);
            cv_rect(x, 3, 17, 1, d); cv_rect(x, 19, 17, 1, d); cv_rect(x, 3, 1, 17, d); cv_rect(x + 16, 3, 1, 17, d);
            cv_text(x + 9 - text_w(&FONT_S, tl) / 2, 3, &FONT_S, tl, d);
        }
    }
    cv_text(46, 3, &FONT_S, view, C_TRK);
    cv_text(50 + vw, 3, &FONT_S, mid, C_CREAM);
    if (pg_pos && bx > 54 + vw + mw) {
        cv_rect(bx, 3, pw + 8, 15, C_SCOPE_BG);
        cv_rect(bx, 3, pw + 8, 1, C_RULE);
        cv_rect(bx, 17, pw + 8, 1, C_RULE);
        cv_text(bx + 4, 3, &FONT_S, pg_pos, C_AMBER);
    }
    cv_text(rx, 3, &FONT_S, right, rc);
    cv_rect(0, 23, 240, 1, C_RULE);
    cv_blit(0, 0);
}

static uint16_t seq_note_c(const step_t *st)    /* a note's colour: its velocity, dim by chance, grey when off */
{
    if (st->ratch & STEP_OFF) return C_KNOB_HI;
    return blend565(st->chance < 100u ? C_RULE : blend565(C_PANEL, C_TRK, 110), st->chance < 100u ? blend565(C_PANEL, C_TRK, 150) : C_TRK, (uint32_t)st->vel * 2u);
}

static void ui_seq_roll(uint32_t t, uint32_t w0, int32_t cursor, int32_t playhead)
{
    static const int32_t TOP = 4, AH = 88;      /* the notes' area */
    uint32_t len = pat.trk[t].len, i, k;
    int32_t lo = 127, hi = 0;
    cv_begin(240, 112, C_SCOPE_BG);
    cv_rect(0, 0, 240, 1, C_TRK);               /* (the track's colour round the roll) */
    for (i = 0; i < 16u && w0 + i < len; i++) {   /* the pitch range: the window's notes, at least an octave */
        const step_t *st = seq_step(t, w0 + i);
        for (k = 0; k < SEQ_CHORD && st->note[k]; k++) {
            if (st->note[k] < lo) lo = st->note[k];
            if (st->note[k] > hi) hi = st->note[k];
        }
    }
    if (hi < lo) { lo = 54; hi = 66; }
    if (hi - lo < 12) { lo -= (12 - (hi - lo)) / 2; hi = lo + 12; }
    for (i = 0; i < 16u; i++) {
        uint32_t n = w0 + i;
        int32_t x = (int32_t)i * 15;
        const step_t *st;
        if (n >= len) { cv_rect(x + 1, TOP, 13, AH, C_PANEL); continue; }
        st = seq_step(t, n);
        cv_rect(x + 1, TOP, 13, AH, playhead >= 0 && n == (uint32_t)playhead ? STRIPE[0]
                                    : cursor >= 0 && n == (uint32_t)cursor ? C_RULE : n % 4u ? C_KNOB : RGB(44, 52, 68));
        if (n % 4u == 0u) {                     /* the beats: a tick and the step's number */
            char s[4];
            uint32_t v = n + 1u, j = 0;
            if (v >= 100u) s[j++] = (char)('0' + v / 100u);
            if (v >= 10u) s[j++] = (char)('0' + v / 10u % 10u);
            s[j++] = (char)('0' + v % 10u);
            s[j] = 0;
            cv_rect(x + 1, TOP, 13, 1, C_KNOB_HI);
            cv_text(x + 2, TOP + AH + 3, &FONT_S, s, cursor >= 0 && n == (uint32_t)cursor ? C_CREAM : C_DIMT);
        }
        if (!st->note[0]) cv_rect(x + 5, TOP + AH / 2, 5, 2, C_KNOB_HI);
        else {
            uint16_t c = seq_note_c(st);
            int32_t w = st->gate >= GATE_TIE ? 15 : clampi(st->gate * 11 / 120, 3, 11);
            for (k = 0; k < SEQ_CHORD && st->note[k]; k++) {
                int32_t y = TOP + 2 + (hi - st->note[k]) * (AH - 8) / (hi - lo);
                cv_rect(x + 2, y, i == 15u && w > 11 ? 12 : w, 4, c);   /* a tie runs on into the next step */
            }
            for (k = 0; k < (st->ratch & 3u); k++) cv_rect(x + 3 + (int32_t)k * 3, TOP + AH - 4, 2, 2, C_AMBER);
        }
        if (cursor >= 0 && n == (uint32_t)cursor) {   /* the cursor: a red frame */
            cv_rect(x, TOP - 2, 15, 2, C_RED); cv_rect(x, TOP + AH, 15, 2, C_RED);
            cv_rect(x, TOP - 2, 2, AH + 4, C_RED); cv_rect(x + 13, TOP - 2, 2, AH + 4, C_RED);
        }
    }
    cv_blit(0, 24);
}

static void ui_seq_info(uint32_t t, const char *line, uint16_t lc, const char *right, uint32_t w0, int32_t cursor, int32_t playhead)
{
    uint32_t len = pat.trk[t].len, win = w0 / 64u * 64u, i;
    cv_begin(240, 52, C_PANEL);
    cv_text(4, 0, &FONT_S, line, lc);
    if (right) cv_text(236 - text_w(&FONT_S, right), 0, &FONT_S, right, C_DIMT);
    for (i = 0; i < 64u; i++) {                 /* the overview: four rows of 16 */
        uint32_t n = win + i;
        int32_t x = (int32_t)(i % 16u) * 15 + 1, y = 20 + (int32_t)(i / 16u) * 8;
        const step_t *st;
        uint16_t c;
        if (n >= len) break;
        st = seq_step(t, n);
        if (cursor >= 0 && n == (uint32_t)cursor) c = C_RED;
        else if (playhead >= 0 && n == (uint32_t)playhead) c = STRIPE[2];
        else if (st->note[0]) c = seq_note_c(st);
        else c = n / 16u == w0 / 16u ? C_KNOB_HI : C_KNOB;
        cv_rect(x, y, 13, 6, c);
    }
    cv_blit(0, 136);
}

static void ui_seq_knobs(const knob_view_t *k)
{
    int32_t i;
    cv_begin(240, 52, C_PANEL);
    cv_rect(0, 0, 240, 1, C_RULE);
    for (i = 0; i < 4; i++) {
        int32_t cx = 30 + 60 * i, f;
        if (!k[i].present) continue;
        cv_text(cx - text_w(&FONT_S, k[i].label) / 2, 3, &FONT_S, k[i].label, C_DIMT);
        cv_text(cx - text_w(&FONT_S, k[i].value) / 2, 19, &FONT_S, k[i].value, C_CREAM);
        cv_rect(cx - 22, 42, 44, 3, C_KNOB);
        if (k[i].max > 0) {
            f = 44 * clampi(k[i].v, 0, k[i].max) / k[i].max;
            if (k[i].bip) { if (f >= 22) cv_rect(cx, 42, f - 22, 3, C_TRK); else cv_rect(cx - 22 + f, 42, 22 - f, 3, C_TRK); }
            else cv_rect(cx - 22, 42, f, 3, C_TRK);
        }
    }
    cv_blit(0, 188);
}

/* TRACKS (as SLOOP's): A and B one under the other, 72 px each from y 24, the chosen one lit in its colour. A row:
 * the letter in a tile, the sound (number and name), a line under it, the 16 steps of the page that plays (stopped:
 * the cursor's), the playhead a bar under its step. ui_trk is the row drawn */
static void ui_seq_track_row(uint32_t t, uint32_t sel, const char *title, uint16_t tc, const char *sub, const char *status,
                             uint32_t w0, int32_t playhead)
{
    uint32_t len = pat.trk[t].len, i;
    uint16_t c = t ? C_TRK_B : C_AMBER, dim = blend565(C_PANEL, c, 110);
    char tl[2] = {(char)('A' + t), 0};
    cv_begin(240, 72, sel ? C_KNOB : C_PANEL);
    if (sel) cv_rect(0, 0, 3, 72, c);
    if (sel) { cv_rect(8, 6, 28, 28, c); cv_text(22 - text_w(&FONT_S, tl) / 2, 12, &FONT_S, tl, C_PANEL); }
    else {
        cv_rect(8, 6, 28, 1, dim); cv_rect(8, 33, 28, 1, dim); cv_rect(8, 6, 1, 28, dim); cv_rect(35, 6, 1, 28, dim);
        cv_text(22 - text_w(&FONT_S, tl) / 2, 12, &FONT_S, tl, dim);
    }
    cv_text(44, 4, &FONT_S, title, sel ? tc : blend565(C_PANEL, tc, 170));
    cv_text(44, 21, &FONT_S, sub, C_DIMT);
    if (status[0]) cv_text(236 - text_w(&FONT_S, status), 4, &FONT_S, status, C_RED);
    for (i = 0; i < 16u; i++) {
        uint32_t n = w0 + i;
        int32_t x = 4 + (int32_t)i * 15 - (i > 7u ? 1 : 0);
        uint16_t cc;
        if (n >= len) { cv_rect(x, 42, 13, 16, sel ? C_PANEL : C_KNOB); continue; }
        if (seq_step(t, n)->note[0] && !(seq_step(t, n)->ratch & STEP_OFF)) cc = sel ? c : dim;
        else cc = n % 4u ? (sel ? C_PANEL : C_KNOB) : C_KNOB_HI;
        cv_rect(x, 42, 13, 16, cc);
        if (playhead >= 0 && n == (uint32_t)playhead) cv_rect(x, 61, 13, 3, C_CREAM);
    }
    cv_rect(0, 71, 240, 1, C_RULE);
    cv_blit(0, 24 + (int32_t)t * 72);
}

static void ui_seq_tline(const char *l, uint16_t c)   /* TRACKS: the line under the two rows */
{
    cv_begin(240, 20, C_PANEL);
    cv_text((240 - text_w(&FONT_S, l)) / 2, 2, &FONT_S, l, c);
    cv_blit(0, 168);
}

/* PATTERNS: the eight slots as two columns of four (selected: framed in red; the one playing: a blue bar) */
static void ui_seq_slots(const char *const *txt, uint32_t used, uint32_t sel, uint32_t now, uint32_t now_edited)
{
    uint32_t i;
    cv_begin(240, 112, C_PANEL);
    for (i = 0; i < 8u; i++) {
        int32_t x = (int32_t)(i % 2u) * 120 + 2, y = (int32_t)(i / 2u) * 28 + 2;
        char num[3] = {(char)('1' + i), i == now && now_edited ? '*' : 0, 0};
        cv_rect(x, y, 116, 24, i == sel ? C_RULE : C_KNOB);
        if (i == now) cv_rect(x, y, 4, 24, STRIPE[1]);
        cv_text(x + 10, y + 4, &FONT_S, num, i == now ? C_AMBER : C_CREAM);
        cv_text(x + 34, y + 4, &FONT_S, txt[i], used >> i & 1u ? C_CREAM : C_DIMT);
        if (i == sel) {
            cv_rect(x - 2, y - 2, 120, 2, C_RED); cv_rect(x - 2, y + 24, 120, 2, C_RED);
            cv_rect(x - 2, y - 2, 2, 28, C_RED); cv_rect(x + 116, y - 2, 2, 28, C_RED);
        }
    }
    cv_blit(0, 24);
}

static void ui_seq_lines(const char *l1, uint16_t c1, const char *l2, const char *l3)
{
    cv_begin(240, 52, C_PANEL);
    cv_text(4, 0, &FONT_S, l1, c1);
    cv_text(4, 18, &FONT_S, l2, C_CREAM);
    cv_text(4, 34, &FONT_S, l3, C_DIMT);
    cv_blit(0, 136);
}

