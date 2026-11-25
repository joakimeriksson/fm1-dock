/* fm1-dock: B3 ORGAN's firmware/src/dinghy.c, split by modules/b3/take.py: do not edit */
/* SPDX-License-Identifier: GPL-3.0-only */
/* B3 ORGAN for the M-VAVE FM-1: a fixed-point tonewheel organ on Dinghy's platform.
 *
 * The model follows setBfree (pantherb/setBfree, GPL-2.0-or-later, so it combines with Dinghy's GPL-3.0):
 *   - 91 tonewheels with the 60 Hz gear ratios of setBfree's tonegen.c (tables from tools/gen_organ.py)
 *   - 9 drawbars with setBfree's footage-to-wheel mapping and its linear level (setting / 8)
 *   - percussion on the 2nd or 3rd harmonic: single trigger, fast/slow decay (-60 dB in 1 s / 4 s), soft/normal,
 *     the 1' drawbar muted while it is on
 *   - a vibrato/chorus scanner at 7.25 Hz (V1..V3, C1..C3), an overdrive and a "Leslie-lite"
 * It is NOT setBfree itself: no float, no malloc, no whirl convolution, three manuals (upper, lower, pedal).
 *
 *   keys        play the organ (key 0 = F3, OCT-/OCT+ shifts) on the manual REC chose    MASTER   volume (platform.c)
 *   SELECT      choose a drawbar    ALGORITHM  set it 0..8         PRESETS  setBfree's registrations (default.pgm)
 *               (pedal screen: after its two drawbars the equalizer's bass, middle, treble, -9..+9 dB in 0.5 dB steps)
 *   KNOB1       screen: upper, lower, pedal, test values    KNOB2   vibrato V1 C1 V2 C2 V3 C3    KNOB3   overdrive drive 0..20    KNOB4   reverb
 *   FX          vibrato on/off (upper on screen 1, lower on screen 2)    LFO    Leslie stop/slow/fast    SEL    overdrive on/off
 *   ENV         percussion on/off   ARP  2nd/3rd   SEQ  fast/slow   PLAY  soft/normal   (screen 1)     REC   keys: upper/lower/pedal
 *   EDIT, SAVE  free
 *   USB-MIDI    and the TRS MIDI IN jack: channel 1 plays the upper manual, 2 the lower one, 3 the pedal (drawbars 70..78 follow the
 *               channel; the pedal has 16' = 70 and 8' = 72), program change (setBfree's default.pgm), and setBfree's controllers
 *               (CC 1, 7, 11, 30, 31, 64..66, 70..78, 80..83, 91..93, 95)
 *
 * Rules of the road (the chip has no FPU and no libgcc): integers only, no float, no 64-bit division. */
#define APP_NAME "B3 ORGAN"                     /* the rescue and the crash screen say this */
#include "platform.c"
#include "organ_tab.h"

#ifdef DINGHY_HOST
static uint32_t fm1_ticks(void) { return 0; }
#define FM1_TICKS_PER_US 24u
#endif

#define ORGAN_POOL                              /* fm1-dock: the delay lines in .bss, which the host clears */
#include "organ_core.h"
#include "organ_logo.h"                        /* the logo (tools/gen_logo.py) */

/* ---- the screen */
#define C_ACC RGB(255, 190, 90)                 /* tungsten */
#define C_OFF RGB(110, 110, 110)
#define C_BAR RGB(230, 230, 230)
static const char *const FOOT[NB] = {"16", "513", "8", "4", "223", "2", "135", "113", "1"};
static const uint16_t BARC[NB] = {RGB(190, 120, 60), RGB(190, 120, 60), RGB(235, 235, 235), RGB(235, 235, 235),
                                  RGB(90, 90, 100), RGB(235, 235, 235), RGB(90, 90, 100), RGB(90, 90, 100),
                                  RGB(235, 235, 235)};
#define C_TRK RGB(34, 34, 34)
#define C_PCH RGB(44, 30, 18)                   /* the peach boxes of the sketch: vibrato and percussion */

static char *sp(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *su(char *p, uint32_t v)
{
    char t[10];
    uint32_t n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

#define C_RED RGB(230, 70, 60)
#define C_GRN RGB(120, 220, 140)
static void draw_battery(uint32_t st)           /* st: bars 0 .. 4, bit 3 = a USB host has it (a bolt instead) */
{
    static const char *const BOLT[7] = {"...XX.", "..XX..", ".XX...", "XXXXXX", "...XX.", "..XX..", ".XX..."};
    const uint32_t x = 212, y = 222, bars = st & 7u;
    uint32_t r, c, k;
    uint16_t line = bars ? C_BAR : C_RED;
    lcd_fill(x, y, 28, 16, C_BLACK);
    if (st & 8u) {
        for (r = 0; r < 7u; r++)
            for (c = 0; c < 6u; c++)
                if (BOLT[r][c] == 'X') lcd_fill(x + 8u + c * 2u, y + 1u + r * 2u, 2, 2, C_GRN);
        return;
    }
    lcd_fill(x + 1u, y + 2u, 24, 1, line);      /* the cell: outline, the nub, then a bar for each step */
    lcd_fill(x + 1u, y + 13u, 24, 1, line);
    lcd_fill(x + 1u, y + 2u, 1, 12, line);
    lcd_fill(x + 24u, y + 2u, 1, 12, line);
    lcd_fill(x + 25u, y + 5u, 2, 6, line);
    for (k = 0; k < bars; k++)
        lcd_fill(x + 3u + k * 5u, y + 4u, 4, 8, bars <= 1u ? C_RED : C_ACC);
}

/* ---- three screens: 1 upper, 2 lower, 3 pedal. Each is built from bands (a canvas of 240 x h, drawn and blitted on its own, and only when
 * what it shows has changed): the header, the three manual blocks, four rows of switches, the program. The block of the manual a screen is
 * for shows its drawbars as bars; the others show their values. */
#define TRACK 44                                /* a drawbar: the track, the cap that hangs down as far as the value says */
#define CAP 12
#define STEP 4
#define BLK_BARS 62                             /* the track, its footage row */
#define Y_CTRL 142                              /* the switches start here, 18 apart, on every screen */
static const char *const MAN[3] = {"UPPER", "LOWER", "PEDAL"};
static const uint8_t PEDID[2] = {0, 2};         /* the pedal's drawbars sit in the columns of the 16' and the 8' */
static uint32_t screen, sel[4];                 /* screen 0..2 are the manuals, 3 the test values */
#ifdef DINGHY_HOST
#define CORES() 2u
#else
#define CORES() (fm1_c1_on ? 2u : 1u)                  /* the second core runs (fm1_cpu1.h), or it was not there / was given up */
#endif
static uint32_t load_peak, load_now, load_wheels, ld_org, ld_od, ld_rv, ld_le, ld_pk, ld_g, ld_go, ld_q;
static uint32_t midi_ch, midi_kind, midi_cnt, midi_t, midi_d1;   /* the last message that came in: channel 1..16, kind 0 none 1 on 2 off 3 CC 4 PC 5 other */

static uint32_t hmix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t colx(uint32_t i) { return 3u + 26u * i; }
static void cv_box(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    cv_rect(x, y, w, 1, c); cv_rect(x, y + h - 1, w, 1, c); cv_rect(x, y, 1, h, c); cv_rect(x + w - 1, y, 1, h, c);
}
static int32_t chip(int32_t x, int32_t y, const char *t, int on, int edit)     /* a switch: boxed, tungsten when it is on and this screen can set it */
{
    int32_t w = text_w(&FONT_S, t) + 6;
    uint16_t c = !edit ? C_OFF : on ? C_ACC : C_BAR;
    cv_rect(x + 1, y + 1, w - 2, 15, C_BLACK);
    cv_box(x, y, w, 17, edit ? (on ? C_ACC : C_OFF) : C_TRK);
    cv_text(x + 3, y + 1, &FONT_S, t, c);
    return x + w;
}
static int32_t oval(int32_t x, int32_t y, const char *t)                       /* a knob's value, in a pill */
{
    int32_t w = text_w(&FONT_S, t) + 10, i;
    cv_rect(x + 4, y, w - 8, 1, C_ACC); cv_rect(x + 4, y + 16, w - 8, 1, C_ACC);
    cv_rect(x, y + 4, 1, 9, C_ACC); cv_rect(x + w - 1, y + 4, 1, 9, C_ACC);
    for (i = 0; i < 4; i++) {
        cv_pset(x + 3 - i, y + 1 + i, C_ACC); cv_pset(x + w - 4 + i, y + 1 + i, C_ACC);
        cv_pset(x + 3 - i, y + 15 - i, C_ACC); cv_pset(x + w - 4 + i, y + 15 - i, C_ACC);
    }
    cv_text(x + 5, y + 1, &FONT_S, t, C_BAR);
    return x + w;
}
static void draw_bars(int32_t y0, const uint8_t *v, const uint8_t *ids, uint32_t cnt, uint32_t selected)
{
    uint32_t k;
    for (k = 0; k < cnt; k++) {
        uint32_t i = ids ? ids[k] : k;
        int32_t x = (int32_t)colx(i), cy = y0 + (int32_t)v[k] * STEP;
        char d[2] = {(char)('0' + v[k]), 0}, *f = (char *)FOOT[i];
        cv_rect(x + 4, y0, 16, TRACK, C_TRK);
        if (v[k]) cv_rect(x + 9, y0, 6, cy - y0, RGB(70, 70, 70));
        cv_rect(x + 1, cy, 22, CAP, BARC[i]);
        if (k == selected) cv_box(x, cy - 1, 24, CAP + 2, C_ACC);
        cv_text(x + 8, cy - 2, &FONT_S, d, BARC[i] > RGB(150, 150, 150) ? C_BLACK : C_BAR);
        cv_text(x + 13 - text_w(&FONT_S, f) / 2, y0 + TRACK + 2, &FONT_S, f, k == selected ? C_ACC : C_BAR);
    }
}
static void draw_digits(int32_t y0, const uint8_t *v, const uint8_t *ids, uint32_t cnt)
{
    uint32_t k;
    for (k = 0; k < cnt; k++) {
        char d[2] = {(char)('0' + v[k]), 0};
        cv_text((int32_t)colx(ids ? ids[k] : k) + 8, y0, &FONT_S, d, C_OFF);
    }
}
/* the pedal screen's right side: the equalizer's three gains as sliders (up is more, the middle 0 dB), set like the drawbars */
#define EQ_X0 120
#define EQ_DX 42
#define EQ_CAP 6                                /* 38 px of way for 37 steps */
static const char *const EQN[3] = {"BASS", "MID", "TREB"};
static void eq_label(char *t, uint32_t g)       /* gain index -> "+4.5", "-9", "0" */
{
    uint32_t a = g >= 18u ? g - 18u : 18u - g;
    if (a) *t++ = g > 18u ? '+' : '-';
    if (a / 2u >= 10u) *t++ = '1';
    *t++ = (char)('0' + a / 2u % 10u);
    if (a & 1u) { *t++ = '.'; *t++ = '5'; }
    *t = 0;
}
static void draw_eq(int32_t y0, uint32_t selected)   /* selected: 0..2, or none; the cap is half a drawbar's (no digit on it): more way */
{
    uint32_t k;
    for (k = 0; k < 3u; k++) {
        int32_t x = EQ_X0 + EQ_DX * (int32_t)k, g = (int32_t)eq_p[2u * k], cy = y0 + (36 - g) * (TRACK - EQ_CAP) / 36, mid = y0 + (TRACK - EQ_CAP) / 2 + EQ_CAP / 2;
        char t[6];
        cv_rect(x + 4, y0, 16, TRACK, C_TRK);
        cv_rect(x - 2, mid, 28, 1, C_OFF);                       /* 0 dB: wider than the cap, so it shows beside it */
        if (cy + EQ_CAP / 2 < mid) cv_rect(x + 9, cy + EQ_CAP / 2, 6, mid - cy - EQ_CAP / 2, RGB(70, 70, 70));
        else if (cy + EQ_CAP / 2 > mid) cv_rect(x + 9, mid + 1, 6, cy + EQ_CAP / 2 - mid, RGB(70, 70, 70));
        cv_rect(x + 1, cy, 22, EQ_CAP, g == 18 ? RGB(120, 120, 120) : C_BAR);
        if (k == selected) cv_box(x, cy - 1, 24, EQ_CAP + 2, C_ACC);
        eq_label(t, (uint32_t)g);
        cv_text(x + 12 - text_w(&FONT_S, t) / 2, y0 + TRACK + 2, &FONT_S, t, k == selected ? C_ACC : C_BAR);
    }
}
static uint32_t block_h(uint32_t scr, uint32_t m) { return (m ? 16u : 0u) + (m == scr ? BLK_BARS : 16u); }
static uint32_t block_y(uint32_t scr, uint32_t m) { uint32_t y = 16, j; for (j = 0; j < m; j++) y += block_h(scr, j); return y; }

static void band_header(uint32_t scr)
{
    char t[24];
    cv_begin(240, 16, C_BLACK);
    if (scr == 3u) cv_text(4, 0, &FONT_S, "TEST", C_ACC);
    else cv_text(4, 0, &FONT_S, scr == 0 ? "UPPER" : "UPPER INFO", scr == 0 ? C_ACC : C_OFF);
    sp(sp(t, "B3 ORGAN v"), APP_VER)[0] = 0;
    cv_text(238 - text_w(&FONT_S, t), 0, &FONT_S, t, C_OFF);
    cv_blit(0, 0);
}
static void band_block(uint32_t scr, uint32_t m)
{
    uint32_t act = m == scr, y = block_y(scr, m);
    const uint8_t *v = m == 0 ? (const uint8_t *)P.db : m == 1 ? (const uint8_t *)P.dbl : (const uint8_t *)P.dbp;
    uint32_t cnt = m == 2 ? 2u : NB;
    if (m) {                                    /* the manual's name: its own strip (the canvas holds 64 rows on the FM-1: */
        cv_begin(240, 16, C_BLACK);             /* name and bars together, 78, lost the footage row) */
        cv_text(4, 0, &FONT_S, MAN[m], act ? C_ACC : C_OFF);
        if (!act) cv_text(4 + 8 * 6, 0, &FONT_S, "INFO", C_OFF);
        if (act && m == 2u) {                   /* the equalizer's names over its sliders */
            uint32_t k;
            for (k = 0; k < 3u; k++)
                cv_text(EQ_X0 + EQ_DX * (int32_t)k + 12 - text_w(&FONT_S, EQN[k]) / 2, 0, &FONT_S, EQN[k], sel[2] == 2u + k ? C_ACC : C_OFF);
        }
        cv_blit(0, y);
        y += 16u;
    }
    cv_begin(240, act ? BLK_BARS : 16u, C_BLACK);
    if (act) {
        draw_bars(1, v, m == 2 ? PEDID : 0, cnt, sel[m]);
        if (m == 2u) draw_eq(1, sel[2] >= 2u ? sel[2] - 2u : 9u);
    } else draw_digits(0, v, m == 2 ? PEDID : 0, cnt);   /* (0.6 drew these over the active manual's bars too) */
    cv_blit(0, y);
}
static const char *const VIB[6] = {"V1", "C1", "V2", "C2", "V3", "C3"};
static const char *const LES[3] = {"STOP", "SLOW", "FAST"};
static const char *const KEYN[3] = {"UPPER", "LOWER", "PEDAL"};
/* ---- screen 4: the test values, one row each */
#define TEST_ROWS 10
static uint32_t test_vals(uint32_t r, uint32_t *a, uint32_t *b)    /* what row r shows; the return value says whether it is a warning */
{
    *a = *b = 0;
    switch (r) {
    case 0: *a = load_now; *b = load_peak; return 0;
    case 1: *a = ld_org; *b = load_wheels; return 0;
    case 2: *a = ld_od; *b = ld_rv; return 0;
    case 3: *a = ld_le; *b = CORES(); return CORES() != 2u;
    case 4: *a = ld_pk; return ld_pk > 100u;
    case 5: *a = ld_g; return ld_g < 100u;
    case 6: *a = ld_q; return ld_q > 100u;
    case 7: *a = ld_go; return ld_go < 100u;
    case 8: *a = midi_ch * 16u + midi_kind; *b = midi_cnt; return 0;
#if FELUCCA_UART
    case 9: *a = um.drops + um.resync; *b = um.bytes; return um.drops + um.resync != 0u;
#endif
    }
    return 0;
}
static void band_test(uint32_t r)
{
    char t[40], *q;
    uint32_t a, b, warn = test_vals(r, &a, &b);
    uint16_t c = warn ? C_ACC : C_BAR;
    cv_begin(240, 18, C_BLACK);
    switch (r) {
    case 0: q = su(sp(su(sp(t, "LOAD "), a), " us   PEAK "), b); sp(q, " us")[0] = 0; break;
    case 1: q = su(sp(su(sp(t, "ORGAN "), a), " us   WHEELS "), b); *q = 0; break;
    case 2: q = su(sp(su(sp(t, "PREAMP "), a), " us   REVERB "), b); sp(q, " us")[0] = 0; break;
    case 3: q = su(sp(su(sp(t, "LESLIE "), a), " us   CORES "), b); *q = 0; break;
    case 4: q = su(sp(t, "SUM OF THE WHEELS "), a > 999u ? 999u : a); sp(q, " %")[0] = 0; break;
    case 5: q = su(sp(t, "SUM LIMITER GAIN "), a); sp(q, " %")[0] = 0; break;
    case 6: q = su(sp(t, "OUTPUT PEAK "), a > 999u ? 999u : a); sp(q, " %")[0] = 0; break;
    case 7: q = su(sp(t, "OUTPUT LIMITER GAIN "), a); sp(q, " %")[0] = 0; break;
    case 8: {
        static const char *const KN[6] = {"--", "NOTE ON", "NOTE OFF", "CC", "PC", "OTHER"};
        q = a & 15u ? sp(su(sp(t, "MIDI CH "), a >> 4), "  ") : sp(t, "MIDI -  ");
        q = sp(q, KN[(a & 15u) < 6u ? a & 15u : 5u]);
        if ((a & 15u) == 3u || (a & 15u) == 4u) q = su(sp(q, " "), midi_d1);
        q = su(sp(q, "   COUNT "), b % 10000u); *q = 0;
        break; }
    default: q = su(sp(su(sp(t, "MIDI ERRORS "), a > 99u ? 99u : a), "  BYTES "), b); *q = 0; break;
    }
    cv_text(4, 2, &FONT_S, t, c);
    cv_blit(0, 24u + 20u * r);
}
static void band_ctrl(uint32_t scr, uint32_t r)
{
    int32_t x;
    char t[24], *q;
    cv_begin(240, 18, C_BLACK);
    if (r == 0) {                               /* vibrato / chorus: the type (KNOB 2) and where it is on (FX) */
        cv_rect(0, 0, 240, 17, C_PCH);
        x = cv_text(4, 1, &FONT_S, "V/C", C_ACC) + 4;
        x = oval(x, 0, VIB[P.vtype % 6u]) + 6;
        x = chip(x, 0, P.vup ? "UP ON" : "UP OFF", P.vup, scr == 0) + 4;
        chip(x, 0, P.vlo ? "LO ON" : "LO OFF", P.vlo, scr == 1);
    } else if (r == 1) {                        /* percussion belongs to the upper manual: set on screen 1, shown on the others */
        int pe = scr == 0;
        cv_rect(0, 0, 240, 17, C_PCH);
        x = cv_text(4, 1, &FONT_S, "PERC", pe ? C_ACC : C_OFF) + 4;
        x = chip(x, 0, P.perc ? "ON" : "OFF", P.perc, pe) + 3;
        x = chip(x, 0, P.perc3 ? "3RD" : "2ND", P.perc, pe) + 3;
        x = chip(x, 0, P.fast ? "FAST" : "SLOW", P.perc, pe) + 3;
        chip(x, 0, P.soft ? "SOFT" : "NORM", P.perc, pe);
    } else if (r == 2) {
        q = sp(sp(t, "LES "), LES[P.les % 3u]); *q = 0;
        x = chip(1, 0, t, 1, 1) + 4;
        q = sp(t, P.od ? "OD ON" : "OD OFF"); *q = 0;
        x = chip(x, 0, t, P.od, 1) + 4;
        q = sp(sp(t, "KEYS "), KEYN[P.keys % 3u]); *q = 0;
        chip(x, 0, t, 1, 1);
    } else {
        x = cv_text(4, 1, &FONT_S, "DRIVE", C_BAR) + 3;
        q = su(t, P.drv); *q = 0;
        x = oval(x, 0, t) + 8;
        x = cv_text(x, 1, &FONT_S, "REVERB", C_BAR) + 3;
        q = su(t, P.rev); *q = 0;
        oval(x, 0, t);
    }
    cv_blit(0, Y_CTRL + 18u * r);
}
/* ---- the logo: at the start, and again after LOGO_IDLE_MS without a key, a knob or a button (a screen saver) */
#define LOGO_IDLE_MS 60000u
#include "../logo_bars.h"                       /* fm1-dock: the drawbars where the organ's are (modules/b3/gen_logo_bars.py) */
static const uint8_t *logo_db(void) { return P.keys % 3u == 1u ? (const uint8_t *)P.dbl : (const uint8_t *)P.db; }
static uint32_t logo_key(void)                  /* the drawbars the logo shows, never 0 (logo_drawn's "draw it") */
{
    uint32_t i, k = 0;
    const uint8_t *v = logo_db();
    for (i = 0; i < 9u; i++) k = k * 9u + (v[i] > 8u ? 8u : v[i]);
    return k + 1u;
}
static void logo_spr(uint32_t s, int32_t px, int32_t py, uint32_t r0)   /* a bar's piece into the strip from row r0 */
{
    const lb_spr_t *q = &LB_SPR[s];
    const uint8_t *a = LB_PX + q->at;
    int32_t sx, sy;
    for (sy = 0; sy < (int32_t)q->h; sy++) {
        int32_t y = py + sy;
        if (y < (int32_t)r0 || y >= (int32_t)(r0 + cv_h)) continue;
        for (sx = 0; sx < (int32_t)q->w; sx++) {
            uint32_t c = a[sy * (int32_t)q->w + sx];
            int32_t x = px + sx;
            if (c == LB_T || x < 0 || x >= 240 || 100 * x + 188 * y < (int32_t)LB_K) continue;   /* (behind the band) */
            cv_px[(uint32_t)(y - (int32_t)r0) * 240u + (uint32_t)x] = swap16(LOGO_PAL[c]);
        }
    }
}
static void draw_logo(void)                     /* in strips as high as the canvas holds (CV_MAX: 64 rows on the FM-1, more on the PC) */
{
    uint32_t r0, i = 0, left = 0, c = 0;
    for (r0 = 0; r0 < LOGO_H; r0 += cv_h) {
        uint32_t p = 0, n, h = LOGO_H - r0;
        if (h > CV_MAX / 240u) h = CV_MAX / 240u;
        cv_begin(240, h, C_BLACK);
        n = 240u * cv_h;                        /* (never more than the canvas: cv_begin clamps cv_h) */
        while (p < n) {
            if (!left) {
                if (i >= sizeof LB_BG) break;      /* fm1-dock: the logo without its bars */
                c = LB_BG[i] >> 4; left = (LB_BG[i] & 15u) + 1u; i++;
            }
            for (; left && p < n; left--) cv_px[p++] = swap16(LOGO_PAL[c]);
        }
        {                                       /* fm1-dock: the bars where the organ's drawbars are */
            const uint8_t *v = logo_db();
            uint32_t b;
            (void)LOGO_RLE;
            for (b = 0; b < 9u; b++) {
                uint32_t n = v[b] > 8u ? 8u : v[b];
                logo_spr(0, LB_OFF[b][n][0], LB_OFF[b][n][1], r0);
                logo_spr(1u + b, LB_OFF[b][n][2], LB_OFF[b][n][3], r0);
            }
        }
        if (r0 == 0) cv_text(6, 4, &FONT_S, "v" APP_VER, RGB(150, 128, 104));
        cv_blit(0, r0);
    }
}

static void band_preset(void)
{
    char s[40], *p = s;
    if (P.pgm && pgm_name(P.pgm)) { p = su(p, P.pgm); p = sp(p, " "); p = sp(p, pgm_name(P.pgm)); }
    *p = 0;
    while (p > s && text_w(&FONT_S, s) > 206) *--p = 0;     /* the corner is the battery's */
    draw_text_box(0, 222, 208, &FONT_S, s, C_ACC, 0);
}

static uint32_t state_key(int32_t oct, uint32_t *status)   /* what is worth saving; *status: what the screen shows */
{
    uint32_t i, dbk = 0, k = P.vtype | P.vup << 3 | P.les << 4 | P.perc << 6 | P.perc3 << 7 | P.fast << 8 | P.soft << 9 |
                             P.od << 10 | P.pgm << 15 | (uint32_t)P.rev << 22 | P.vlo << 29 | P.keys << 30;
    for (i = 0; i < NB; i++) dbk = dbk * 9u + P.db[i];
    for (i = 0; i < NB; i++) dbk = dbk * 9u + P.dbl[i];
    dbk = dbk * 9u + P.dbp[0]; dbk = dbk * 9u + P.dbp[1];
    dbk = dbk * 21u + P.drv;                    /* the drive 0..20 does not fit the bits above */
    dbk = dbk * 128u + P.chr;
    for (i = 0; i < LES_NP; i++) dbk = dbk * 31u + les_p[i];
    for (i = 0; i < TON_NP; i++) dbk = dbk * 31u + ton_p[i];
    for (i = 0; i < EQ_NP; i++) dbk = dbk * 31u + eq_p[i];
    *status = k;
    return (k ^ dbk * 2654435761u) + (uint32_t)(oct + 2);
}

/* ---- the web editor's SysEx (docs/editor): F0 7D 42 33 CMD ... F7 ("B3"). README_ORGAN.md has the list. */
static uint32_t up_pend, up_dirty, up_gen;     /* slots waiting for the flash (bit k: program 112 + k), anything to save, a count for the screen */
static void ed_send(uint32_t cmd, const uint8_t *d, uint32_t n)
{
    uint8_t m[104];
    uint32_t i, w = 0;
    m[w++] = 0xF0; m[w++] = 0x7D; m[w++] = 0x42; m[w++] = 0x33; m[w++] = (uint8_t)cmd;
    for (i = 0; i < n && w < sizeof m - 1u; i++) m[w++] = d[i] & 0x7Fu;
    m[w++] = 0xF7;
    sysex_send(m, w);
}
static void ed_state(void)                      /* 41: the sound now, then the version */
{
    uint8_t d[96];
    uint32_t i, w = 0;
    const char *v = APP_VER;
    for (i = 0; i < NB; i++) d[w++] = P.db[i];
    for (i = 0; i < NB; i++) d[w++] = P.dbl[i];
    d[w++] = P.dbp[0]; d[w++] = P.dbp[1];
    d[w++] = P.vtype; d[w++] = P.vup; d[w++] = P.vlo; d[w++] = P.perc; d[w++] = P.perc3; d[w++] = P.fast; d[w++] = P.soft;
    d[w++] = P.les; d[w++] = P.od; d[w++] = P.drv; d[w++] = P.chr; d[w++] = P.rev; d[w++] = P.swell; d[w++] = P.pgm; d[w++] = P.keys;
    for (i = 0; i < LES_NP; i++) { d[w++] = (uint8_t)(les_p[i] >> 7); d[w++] = (uint8_t)(les_p[i] & 127u); }   /* the Leslie's, 14 bits: MSB, LSB */
    for (i = 0; i < TON_NP; i++) { d[w++] = (uint8_t)(ton_p[i] >> 7); d[w++] = (uint8_t)(ton_p[i] & 127u); }   /* tuning, key click */
    for (i = 0; i < EQ_NP; i++) { d[w++] = (uint8_t)(eq_p[i] >> 7); d[w++] = (uint8_t)(eq_p[i] & 127u); }       /* the equalizer */
    while (*v && w < sizeof d) d[w++] = (uint8_t)*v++;
    ed_send(0x41, d, w);
}
static void ed_slot(uint32_t n)                 /* 42: one of the user's programs: number, used, name (12 bytes) */
{
    uint8_t d[14];
    const organ_user_t *u = user_pgm(n);
    uint32_t i;
    d[0] = (uint8_t)n; d[1] = u != 0;
    for (i = 0; i < 12u; i++) d[2 + i] = u ? (uint8_t)u->name[i] : 0;
    ed_send(0x42, d, 14);
}
#define UP_BYTES 44u                            /* a user program on the wire: name 12, db 9, dbl 9, dbp 2, then 12 switches and values */
static void up_pack(const organ_user_t *u, uint8_t *d)
{
    uint32_t i, w = 0;
    for (i = 0; i < 12u; i++) d[w++] = (uint8_t)u->name[i] & 0x7Fu;
    for (i = 0; i < NB; i++) d[w++] = u->db[i];
    for (i = 0; i < NB; i++) d[w++] = u->dbl[i];
    d[w++] = u->dbp[0]; d[w++] = u->dbp[1];
    d[w++] = u->vtype; d[w++] = u->vup; d[w++] = u->vlo; d[w++] = u->perc; d[w++] = u->perc3; d[w++] = u->fast; d[w++] = u->soft;
    d[w++] = u->les; d[w++] = u->od; d[w++] = u->drv; d[w++] = u->chr; d[w++] = u->rev;
}
static void up_unpack(organ_user_t *u, const uint8_t *d)   /* the name made printable; user_ok checks the rest */
{
    uint32_t i, r = 0, end = 0;
    u->used = 1;
    for (i = 0; i < 12u; i++, r++) {
        if (!d[r]) end = 1;
        u->name[i] = end ? 0 : d[r] >= 32u && d[r] < 127u ? (char)d[r] : '?';
    }
    u->name[12] = 0;
    for (i = 0; i < NB; i++) u->db[i] = d[r++];
    for (i = 0; i < NB; i++) u->dbl[i] = d[r++];
    u->dbp[0] = d[r++]; u->dbp[1] = d[r++];
    u->vtype = d[r++]; u->vup = d[r++] & 1u; u->vlo = d[r++] & 1u; u->perc = d[r++] & 1u; u->perc3 = d[r++] & 1u; u->fast = d[r++] & 1u;
    u->soft = d[r++] & 1u; u->les = d[r++]; u->od = d[r++] & 1u; u->drv = d[r++]; u->chr = d[r++]; u->rev = d[r++];
    u->rsv[0] = UP_DRV_NEW; u->rsv[1] = 0;      /* the editor sends the drive in 0.6's scale (it converts older backups) */
    if (!u->name[0]) { u->name[0] = 'U'; u->name[1] = 'S'; u->name[2] = 'E'; u->name[3] = 'R'; }
}
static void ed_sysex(const uint8_t *p, uint32_t n)   /* p: the bytes between F0 and F7 */
{
    uint32_t k, slot;
    if (n < 4u || p[0] != 0x7Du || p[1] != 0x42 || p[2] != 0x33) return;
    slot = n > 4u ? p[4] : 0;
    switch (p[3]) {
    case 0x01: ed_state(); break;                                       /* 01: send the state */
    case 0x02: for (k = UP_FIRST; k < UP_FIRST + UP_N; k++) ed_slot(k); break;   /* 02: send the user's programs */
    case 0x05:                                                          /* 05 slot: send all of it (a backup): 45 slot used + UP_BYTES */
        if (slot >= UP_FIRST && slot < UP_FIRST + UP_N) {
            uint8_t d[2 + UP_BYTES];
            d[0] = (uint8_t)slot; d[1] = user_pgm(slot) != 0;
            up_pack(&UPGM[slot - UP_FIRST], d + 2);
            if (!d[1]) for (k = 2; k < sizeof d; k++) d[k] = 0;
            ed_send(0x45, d, sizeof d);
        }
        break;
    case 0x07:                                                          /* 07: the equalizer: 6 values (14 bits); the FM-1 computes the filters */
        if (n >= 4u + 12u) {                                            /* (the first editor sent coefficients after them: ignored) */
            uint16_t ep[EQ_NP];
            uint8_t d[1];
            const uint8_t *q = p + 4;
            for (k = 0; k < EQ_NP; k++, q += 2) ep[k] = (uint16_t)(q[0] << 7 | q[1]);
            d[0] = eq_set(ep) ? 1 : 0;
            ed_send(0x47, d, 1);                                        /* 47 status: 0 taken, 1 out of range */
        }
        break;
    case 0x06:                                                          /* 06 slot + UP_BYTES: write it (a restore); answer as 03 */
        if (slot < UP_FIRST || slot >= UP_FIRST + UP_N || n < 5u + UP_BYTES) { uint8_t d[2] = {(uint8_t)slot, 1}; ed_send(0x43, d, 2); break; }
        {
            organ_user_t u;
            up_unpack(&u, p + 5);
            if (!user_ok(&u)) { uint8_t d[2] = {(uint8_t)slot, 3}; ed_send(0x43, d, 2); break; }   /* 3: out of range */
            UPGM[slot - UP_FIRST] = u;
        }
        up_pend |= 1u << (slot - UP_FIRST); up_dirty = 1; up_gen++;
        break;
    case 0x03:                                                          /* 03 slot name[12]: store the sound now in it */
    case 0x04:                                                          /* 04 slot: delete it */
        if (slot < UP_FIRST || slot >= UP_FIRST + UP_N) { uint8_t d[2] = {(uint8_t)slot, 1}; ed_send(0x43, d, 2); break; }
        if (p[3] == 0x03u) {
            uint8_t name[12] = {0};
            for (k = 0; k < 12u && 5u + k < n; k++) name[k] = p[5 + k];
            user_store(slot, name);
        } else {
            UPGM[slot - UP_FIRST].used = 0;
            if (P.pgm == slot) P.pgm = 0;
        }
        up_pend |= 1u << (slot - UP_FIRST); up_dirty = 1; up_gen++;   /* saved when nothing sounds (an erase stops the sound) */
        break;
    default: break;
    }
}

static uint8_t clampu(int32_t v, int32_t hi) { return (uint8_t)(v < 0 ? 0 : v > hi ? hi : v); }

/* fm1-dock: app_main's loop-wide variables, for app_init and app_loop (modules/b3/take.py) */
static struct { uint8_t magic, db[NB], vtype, vup, perc, perc3, fast, soft, les, od, drv, pgm, rev; int8_t oct;
             uint8_t dbl[NB], dbp[2], vlo, keys, chr, lesp[2 * LES_NP], tonp[2 * TON_NP],
             eqp[2 * EQ_NP], eqc[60]; } set;   /* eqc: unused (the first equalizer builds kept the editor's coefficients; the size stays) */   /* les: the Leslie's les_p, low byte first */
static uint32_t prev = 0, k, i, t_change = 0, dirty = 0, t_load = 0, seed = 12345u, saved_key = 0, last_key = 0;
static uint32_t hash[22], drawn_screen = 99, drawn_batt = ~0u, logo_on = 1, logo_drawn = 0, t_input = 0;
static uint8_t held[27], heldm[27];
static int32_t oct = 0;
static int set_old;
static void app_init(void)                      /* fm1-dock: app_main up to its loop */
{
#ifdef DINGHY_HOST
    if (getenv("ORGAN_ONE")) organ_two = 0;          /* tests: all the wheels in one call, to compare with the two halves */
#endif
    for (i = 0; i < NW; i++) { seed = seed * 1664525u + 1013904223u; wph[i] = seed; }   /* the wheels start anywhere */
    for (i = 0; i < 27u; i++) { held[i] = 255; heldm[i] = 0; }
    sel[0] = sel[1] = 2; sel[2] = 0;
    set.chr = OD_CHARACTER;
    if (data_load(UPGM, sizeof UPGM) == 0) user_check();
    else for (i = 0; i < UP_N; i++) UPGM[i].used = 0;
    for (i = 0; i < LES_NP; i++) { set.lesp[2 * i] = (uint8_t)LES_DEF[i]; set.lesp[2 * i + 1] = (uint8_t)(LES_DEF[i] >> 8); }
    for (i = 0; i < TON_NP; i++) { set.tonp[2 * i] = (uint8_t)TON_DEF[i]; set.tonp[2 * i + 1] = (uint8_t)(TON_DEF[i] >> 8); }
    for (i = 0; i < 2u * EQ_NP; i++) set.eqp[i] = 0;   /* (no equalizer saved: flat) */
#define SET_EQ (2u * EQ_NP + 60u)
    {
        int now = settings_load(&set, sizeof set) == 0;   /* 0.6's; anything older has the drive 0..16: the same sound is 4 steps higher now */
        set_old = !now;
    }
    if ((!set_old || settings_load(&set, sizeof set - SET_EQ) == 0 ||                                      /* 0.5.1: no equalizer */
         settings_load(&set, sizeof set - SET_EQ - 2u * TON_NP) == 0 ||                                      /* without the tuning, click */
         settings_load(&set, sizeof set - SET_EQ - 2u * TON_NP - 2u * LES_NP) == 0 ||                        /* without the Leslie's */
         settings_load(&set, sizeof set - SET_EQ - 2u * TON_NP - 2u * LES_NP - 1u) == 0) && set.chr < 128u && set.magic == 0xB6 && set.oct >= -2 && set.oct <= 2 && set.vtype < 6u &&
        set.les < 3u && set.drv <= OD_DRIVE_MAX && set.pgm < 128u && set.rev < 128u && set.keys < 3u) {
        for (i = 0; i < NB; i++) { P.db[i] = set.db[i] > 8u ? 8u : set.db[i]; P.dbl[i] = set.dbl[i] > 8u ? 8u : set.dbl[i]; }
        P.dbp[0] = set.dbp[0] > 8u ? 8u : set.dbp[0]; P.dbp[1] = set.dbp[1] > 8u ? 8u : set.dbp[1];
        P.vtype = set.vtype; P.vup = set.vup & 1u; P.vlo = set.vlo & 1u; P.perc = set.perc & 1u; P.perc3 = set.perc3 & 1u;
        P.fast = set.fast & 1u; P.soft = set.soft & 1u; P.les = set.les; P.od = set.od & 1u; P.drv = set_old ? (set.drv + 4u > OD_DRIVE_MAX ? OD_DRIVE_MAX : set.drv + 4u) : set.drv;
        P.pgm = set.pgm; P.rev = set.rev; P.keys = set.keys; P.chr = set.chr; oct = set.oct;
        for (i = 0; i < LES_NP; i++) les_set(i, set.lesp[2 * i] | (uint32_t)set.lesp[2 * i + 1] << 8);
        for (i = 0; i < TON_NP; i++) ton_set(i, set.tonp[2 * i] | (uint32_t)set.tonp[2 * i + 1] << 8);
        if (set.eqp[0] | set.eqp[1]) {          /* an equalizer saved (gain index 0 with frequency 0 is never one) */
            uint16_t ep[EQ_NP];
            for (i = 0; i < EQ_NP; i++) ep[i] = (uint16_t)(set.eqp[2 * i] | set.eqp[2 * i + 1] << 8);
            eq_set(ep);
        }
        if (P.pgm >= UP_FIRST && !user_pgm(P.pgm)) P.pgm = 0;
    } else
        organ_program(5u);                      /* Standard B, 88 8000 000 */
    last_key = saved_key = state_key(oct, &i);

}

static void app_loop(void)                      /* fm1-dock: a pass of app_main's loop, from the host's draw() */
{
    do {
        uint32_t keys, down, up, rel, b;
        int32_t e, t_enc;
        uint8_t st, d1, d2;
        platform_poll();

        keys = fm1_in.notes;
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {             /* a key stops the note it started, on the manual it started it, whatever the octave is now */
            if (down >> k & 1u) {
                uint32_t note = (uint32_t)(53 + 12 * oct + (int32_t)k);
                held[k] = (uint8_t)note; heldm[k] = P.keys;
                organ_note(P.keys, 1u, note, 1, 0);
            }
            if (up >> k & 1u && held[k] != 255) { organ_note(heldm[k], 1u, held[k], 0, 0); held[k] = 255; }
        }

        b = fm1_input_edges(&rel);
        if (keys || b) t_input = fm1_ms;
        if (logo_on) {                          /* the logo: the first knob or button only brings the controls back; a key plays */
            uint32_t any = keys != 0 || b != 0;
            for (k = 0; k < 7u; k++) if (fm1_enc_take(k) != 0) any = 1;
            if (any) { logo_on = 0; logo_drawn = 0; screen = 0; drawn_screen = 99; t_input = fm1_ms; }   /* (fm1-dock: the press that wakes it does its job too) */
        } else if (fm1_ms - t_input > LOGO_IDLE_MS) { logo_on = 1; logo_drawn = 0; }
        if (b & (1u << BTN_OCT_DOWN) && oct > -2) { oct--; dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && oct < 2) { oct++; dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_FX)) { if (screen == 0) P.vup ^= 1u; else if (screen == 1) P.vlo ^= 1u; }   /* vibrato / chorus on the manual of the screen */
        if (b & (1u << BTN_LFO)) P.les = (uint8_t)((P.les + 1u) % 3u);   /* Leslie: stop, slow, fast */
        if (b & (1u << BTN_SEL)) P.od ^= 1u;                    /* overdrive on / off */
        if (b & (1u << BTN_REC)) P.keys = (uint8_t)((P.keys + 1u) % 3u);   /* the FM-1's own keys: upper, lower, pedal */
        if (screen == 0) {                      /* percussion is the upper manual's */
            if (b & (1u << BTN_ENV)) P.perc ^= 1u;
            if (b & (1u << BTN_ARP)) P.perc3 ^= 1u;
            if (b & (1u << BTN_SEQ)) P.fast ^= 1u;
            if (b & (1u << BTN_PLAY)) P.soft ^= 1u;
        }

#define enc_in(id) (t_enc = fm1_enc_take(id), t_enc ? (t_input = fm1_ms, t_enc) : 0)
        if ((e = enc_in(ENC_KNOB1)) != 0) screen = clampu((int32_t)screen + e, 3);
        if ((e = enc_in(ENC_SELECT)) != 0) if (screen < 3u) sel[screen] = clampu((int32_t)sel[screen] + e, screen == 2 ? 4 : 8);
        if ((e = enc_in(ENC_ALGORITHM)) != 0 && screen == 2u && sel[2] >= 2u) {   /* the equalizer: 0.5 dB a step */
            uint16_t ep[EQ_NP];
            for (k = 0; k < EQ_NP; k++) ep[k] = eq_p[k];
            ep[2u * (sel[2] - 2u)] = (uint16_t)clampu((int32_t)ep[2u * (sel[2] - 2u)] + e, 36);
            eq_set(ep);
        } else if (e != 0 && screen < 3u) {
            volatile uint8_t *db = screen == 0 ? P.db : screen == 1 ? P.dbl : P.dbp;
            db[sel[screen]] = clampu((int32_t)db[sel[screen]] + e, 8);
        }
        if ((e = enc_in(ENC_PRESETS)) != 0) {             /* setBfree's registrations, one after the other (the upper manual's) */
            uint32_t n = P.pgm ? P.pgm : 5u;
            for (; e > 0; e--) n = pgm_step(n, 1);
            for (; e < 0; e++) n = pgm_step(n, -1);
            organ_program(n);
        }
        if ((e = enc_in(ENC_KNOB2)) != 0) P.vtype = clampu((int32_t)P.vtype + e, 5);
        if ((e = enc_in(ENC_KNOB3)) != 0) P.drv = clampu((int32_t)P.drv + e, OD_DRIVE_MAX);
        if ((e = enc_in(ENC_KNOB4)) != 0) {               /* reverb mix, like CC 91: a doubling a step, fine at the bottom */
            static const uint8_t REVS[9] = {0, 1, 2, 4, 8, 16, 32, 64, 127};
            int32_t j = 0;
            while (j < 8 && REVS[j + 1] <= P.rev) j++;                      /* the step at or below the value (CC 91 may sit between) */
            if (REVS[j] < P.rev && e < 0) e++;                              /* between two steps: down lands on the one below, up on the next */
            j += e;
            P.rev = REVS[clampu(j, 8)];
        }

        if (midi_in_overflow) {                 /* a message was lost (queue full): its note off may have been it, so release the MIDI notes */
            midi_in_overflow = 0;
            organ_cc_ch(0, 123, 0);
        }
        while (midi_get(&st, &d1, &d2)) {       /* USB-MIDI in: channel 1 upper, 2 lower, 3 pedal */
            uint32_t cmd = st & 0xF0u, ch = st & 0x0Fu;
            if (cmd >= 0x80u && cmd < 0xF0u) {
                midi_ch = ch + 1u; midi_cnt++; midi_t = fm1_ms; midi_d1 = d1;
                midi_kind = cmd == 0x90u && d2 ? 1u : cmd == 0x80u || cmd == 0x90u ? 2u : cmd == 0xB0u ? 3u : cmd == 0xC0u ? 4u : 5u;
            }
            if (cmd == 0x90u && d2) organ_midi_note(ch, d1, 1);
            else if (cmd == 0x80u || cmd == 0x90u) organ_midi_note(ch, d1, 0);
            else if (cmd == 0xB0u) organ_cc_ch(ch, d1, d2);     /* setBfree's controller numbers (organ_core.h) */
            else if (cmd == 0xC0u) organ_pc(d1);                /* program change: setBfree's default.pgm */
        }
        {
            const uint8_t *sx;
            uint32_t sxn;
            if (sysex_get(&sx, &sxn)) { ed_sysex(sx, sxn); sysex_done(); }
        }

        for (k = 0; k < 27u; k++)
            fm1_led_key(KEY_LED(k), held[k] != 255);
        fm1_led_key(BTN_OCT_DOWN, oct < 0);
        fm1_led_key(BTN_OCT_UP, oct > 0);
        fm1_led_key(BTN_FX, screen == 0 ? P.vup != 0 : screen == 1 ? P.vlo != 0 : 0);
        fm1_led_key(BTN_LFO, P.les != 0);
        fm1_led_key(BTN_ENV, P.perc != 0);
        fm1_led_key(BTN_SEL, P.od != 0);
        fm1_led_key(BTN_ARP, P.perc3 != 0);
        fm1_led_key(BTN_SEQ, P.fast != 0);
        fm1_led_key(BTN_PLAY, P.soft != 0);
        fm1_led_key(BTN_REC, P.keys != 0);

        if (fm1_ms - t_load > 500u) {           /* how close to the 5800 us a block has: the number to watch */
            t_load = fm1_ms;
            load_now = load_us; load_wheels = wheels_on; ld_org = load_org; ld_od = load_od; ld_rv = load_rv; ld_le = load_le;
            ld_pk = (sum_pk * 100u + 16384u) >> 15; sum_pk = 0; ld_q = (out_pk * 100u + 16384u) >> 15; out_pk = 0; ld_g = ((uint32_t)lim_sum.lo * 100u + 16384u) >> 15; lim_sum.lo = 32768; ld_go = ((uint32_t)lim_out.lo * 100u + 16384u) >> 15; lim_out.lo = 32768;
            if (load_now > load_peak) load_peak = load_now;
            load_us = load_org = load_od = load_rv = load_le = 0;
        }

        if (logo_on) {
            if (logo_drawn != logo_key()) { logo_drawn = logo_key(); draw_logo(); }   /* (fm1-dock: again when a drawbar moves) */
        } else if (screen != drawn_screen) {    /* a new screen: clear, and draw every band */
            drawn_screen = screen;
            lcd_fill(0, 0, 240, 240, C_BLACK);
            for (i = 0; i < 22u; i++) hash[i] = 0xFFFFFFFFu;
            drawn_batt = ~0u;
        }
        if (!logo_on) {
            uint32_t h, m, r;
            h = hmix(0, screen);
            if (h != hash[0]) { hash[0] = h; band_header(screen); }
            if (screen == 3u) {                 /* the test values: a row is drawn again when its numbers change */
                for (r = 0; r < TEST_ROWS; r++) {
                    uint32_t va, vb;
                    test_vals(r, &va, &vb);
                    h = hmix(hmix(hmix(9, r), va), vb);
                    if (h != hash[9u + r]) { hash[9u + r] = h; band_test(r); }
                }
            } else {
                for (m = 0; m < 3u; m++) {
                    const volatile uint8_t *v = m == 0 ? P.db : m == 1 ? P.dbl : P.dbp;
                    uint32_t cnt = m == 2u ? 2u : NB;
                    h = hmix(hmix(1, screen), m);
                    for (i = 0; i < cnt; i++) h = hmix(h, v[i]);
                    h = hmix(h, sel[m]);
                    if (m == 2u) for (i = 0; i < 3u; i++) h = hmix(h, eq_p[2u * i]);
                    if (h != hash[1 + m]) { hash[1 + m] = h; band_block(screen, m); }
                }
                for (r = 0; r < 4u; r++) {
                    h = hmix(2, screen);
                    if (r == 0) h = hmix(hmix(hmix(h, P.vtype), P.vup), P.vlo);
                    else if (r == 1) h = hmix(hmix(hmix(hmix(h, P.perc), P.perc3), P.fast), P.soft);
                    else if (r == 2) h = hmix(hmix(hmix(h, P.les), P.od), P.keys);
                    else h = hmix(hmix(h, P.drv), P.rev);
                    h = hmix(h, r);
                    if (h != hash[4 + r]) { hash[4 + r] = h; band_ctrl(screen, r); }
                }
                h = hmix(hmix(3, P.pgm), up_gen);
                if (h != hash[8]) { hash[8] = h; band_preset(); }
            }
        }
        {
            uint32_t sk, key = state_key(oct, &sk);
            (void)draw_battery; (void)drawn_batt;   /* fm1-dock: no battery reading in a module */
            if (key != last_key) { last_key = key; dirty = key != saved_key; t_change = fm1_ms; }
        }

        if (dirty && quiet() && fm1_ms - t_change > 2000u) {   /* an erase stalls the sound: save when silent */
            set.magic = 0xB6;
            for (i = 0; i < NB; i++) { set.db[i] = P.db[i]; set.dbl[i] = P.dbl[i]; }
            set.dbp[0] = P.dbp[0]; set.dbp[1] = P.dbp[1]; set.vlo = P.vlo; set.keys = P.keys;
            set.vtype = P.vtype; set.vup = P.vup; set.perc = P.perc; set.perc3 = P.perc3; set.fast = P.fast;
            set.soft = P.soft; set.les = P.les; set.od = P.od; set.drv = P.drv; set.pgm = P.pgm; set.rev = P.rev; set.oct = (int8_t)oct;
            set.chr = P.chr;
            for (i = 0; i < LES_NP; i++) { set.lesp[2 * i] = (uint8_t)les_p[i]; set.lesp[2 * i + 1] = (uint8_t)(les_p[i] >> 8); }
            for (i = 0; i < TON_NP; i++) { set.tonp[2 * i] = (uint8_t)ton_p[i]; set.tonp[2 * i + 1] = (uint8_t)(ton_p[i] >> 8); }
            for (i = 0; i < EQ_NP; i++) { set.eqp[2 * i] = (uint8_t)eq_p[i]; set.eqp[2 * i + 1] = (uint8_t)(eq_p[i] >> 8); }
            settings_save(&set, sizeof set);
            saved_key = last_key;
            dirty = 0;
        }
        if (up_dirty && quiet()) {                 /* the user's programs: when the editor stored or deleted one, and nothing sounds */
            int rc = data_save(UPGM, sizeof UPGM);
            up_dirty = 0;
            for (k = 0; k < UP_N; k++)
                if (up_pend >> k & 1u) { uint8_t d[2] = {(uint8_t)(UP_FIRST + k), rc ? 2u : 0u}; ed_send(0x43, d, 2); ed_slot(UP_FIRST + k); }
            up_pend = 0;
        }
    } while (0);
}
