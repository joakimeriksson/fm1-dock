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
 * It is NOT setBfree itself: no float, no malloc, no whirl convolution, no reverb, one manual only.
 *
 *   keys        play the organ (key 0 = F3, OCT-/OCT+ shifts)      MASTER   volume (platform.c)
 *   SELECT      choose a drawbar    ALGORITHM  set it 0..8         PRESETS  setBfree's registrations (default.pgm)
 *   KNOB1       overdrive amount    KNOB2      vibrato V1 C1 V2 C2 V3 C3    KNOB3    Leslie stop/slow/fast
 *   FX          vibrato on/off      LFO        Leslie cycle        EDIT     overdrive on/off
 *   ENV         percussion on/off   SEL        2nd/3rd             ARP      fast/slow     SEQ  soft/normal
 *   USB-MIDI    and the TRS MIDI IN jack: notes (folded into C2..C7), program change (setBfree's default.pgm),
 *               and setBfree's default controllers (CC 1, 7, 11, 31, 64..66, 70..78, 80..83, 92, 93, 95)
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

/* ---- the screen */
#define C_ACC RGB(255, 190, 90)                 /* tungsten */
#define C_OFF RGB(110, 110, 110)
#define C_BAR RGB(230, 230, 230)
static const char *const FOOTAGE[NB] = {"16", "5 1/3", "8", "4", "2 2/3", "2", "1 3/5", "1 1/3", "1"};
static const uint16_t BARC[NB] = {RGB(190, 120, 60), RGB(190, 120, 60), RGB(235, 235, 235), RGB(235, 235, 235),
                                  RGB(90, 90, 100), RGB(235, 235, 235), RGB(90, 90, 100), RGB(90, 90, 100),
                                  RGB(235, 235, 235)};

static char *sp(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *su(char *p, uint32_t v)
{
    char t[10];
    uint32_t n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) *p++ = t[--n];
    return p;
}

static void draw_row(uint32_t i, uint32_t sel)
{
    uint32_t j;
    cv_begin(240, 16, C_BLACK);
    cv_text(i == sel ? 6 : 10, 0, &FONT_S, i == sel ? ">" : " ", C_ACC);
    cv_text(26, 0, &FONT_S, FOOTAGE[i], i == sel ? C_ACC : C_BAR);
    for (j = 0; j < 8u; j++)
        cv_rect(76 + (int32_t)j * 18, 2, 16, 12, j < P.db[i] ? BARC[i] : RGB(34, 34, 34));
    {
        char s[2] = {(char)('0' + P.db[i]), 0};
        cv_text(226, 0, &FONT_S, s, i == sel ? C_ACC : C_BAR);
    }
    cv_blit(0, 38u + 16u * i);
}

static void draw_status(void)
{
    static const char *const VIB[6] = {"V1", "C1", "V2", "C2", "V3", "C3"};
    static const char *const LES[3] = {"STOP", "SLOW", "FAST"};
    char s[40], *p = s;
    p = sp(p, "VIB "); p = sp(p, VIB[P.vtype % 6u]); p = sp(p, P.vup ? " ON" : " OFF");
    p = sp(p, "  LESLIE "); p = sp(p, LES[P.les % 3u]); *p = 0;
    draw_text_box(0, 184, 240, &FONT_S, s, C_BAR, 1);
    p = s;
    if (P.perc) { p = sp(p, "PERC "); p = sp(p, P.perc3 ? "3RD " : "2ND "); p = sp(p, P.fast ? "FAST " : "SLOW "); p = sp(p, P.soft ? "SOFT" : "NORM"); }
    else p = sp(p, "PERC OFF");
    p = sp(p, "  OD "); if (P.od) p = su(p, P.drv); else p = sp(p, "OFF");
    p = sp(p, "  REV "); p = su(p, P.rev); *p = 0;
    draw_text_box(0, 202, 240, &FONT_S, s, C_BAR, 1);
    p = s;
    if (P.pgm && PGM[P.pgm].name) { p = su(p, P.pgm); p = sp(p, " "); p = sp(p, PGM[P.pgm].name); }
    *p = 0;
    while (p > s && text_w(&FONT_S, s) > 206) *--p = 0;     /* the corner is the battery's */
    draw_text_box(0, 220, 208, &FONT_S, s, C_ACC, 1);
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

static void draw_load(uint32_t us, uint32_t w)
{
    char s[24], *p = sp(s, "W ");
    p = su(p, w); p = sp(p, "  "); p = su(p, us); p = sp(p, " us"); *p = 0;
    draw_text_box(120, 4, 120, &FONT_S, s, C_OFF, 2);
}

static uint32_t state_key(int32_t oct, uint32_t *status)   /* what is worth saving; *status: what the lower screen shows */
{
    uint32_t i, dbk = 0, k = P.vtype | P.vup << 3 | P.les << 4 | P.perc << 6 | P.perc3 << 7 | P.fast << 8 | P.soft << 9 |
                             P.od << 10 | P.drv << 11 | P.pgm << 15 | (uint32_t)P.rev << 22;
    for (i = 0; i < NB; i++) dbk = dbk * 9u + P.db[i];
    *status = k;
    return (k ^ dbk * 2654435761u) + (uint32_t)(oct + 2);
}

/* fm1-dock: app_main's loop-wide variables, for app_init and app_loop (modules/b3/take.py) */
static struct { uint8_t magic, db[NB], vtype, vup, perc, perc3, fast, soft, les, od, drv, pgm, rev; int8_t oct; } set;
static uint32_t prev = 0, k, i, sel = 2, t_change = 0, dirty = 0, t_load = 0, seed = 12345u, saved_key = 0, last_key = 0;
static uint32_t drawn_db[NB], drawn_sel = 99, status_key = ~0u;
static uint8_t held[27];
static int32_t oct = 0;
static void app_init(void)                      /* fm1-dock: app_main up to its loop */
{

    for (i = 0; i < NW; i++) { seed = seed * 1664525u + 1013904223u; wph[i] = seed; }   /* the wheels start anywhere */
    for (i = 0; i < 27u; i++) held[i] = 255;
    for (i = 0; i < NB; i++) drawn_db[i] = 99;
    if (settings_load(&set, sizeof set) == 0 && set.magic == 0xB5 && set.oct >= -2 && set.oct <= 2 && set.vtype < 6u &&
        set.les < 3u && set.drv < 9u && set.pgm < 128u && set.rev < 128u) {
        for (i = 0; i < NB; i++) P.db[i] = set.db[i] > 8u ? 8u : set.db[i];
        P.vtype = set.vtype; P.vup = set.vup & 1u; P.perc = set.perc & 1u; P.perc3 = set.perc3 & 1u;
        P.fast = set.fast & 1u; P.soft = set.soft & 1u; P.les = set.les; P.od = set.od & 1u; P.drv = set.drv;
        P.pgm = set.pgm; P.rev = set.rev; oct = set.oct;
    } else
        organ_program(5u);                      /* Standard B, 88 8000 000 */
    last_key = saved_key = state_key(oct, &i);

    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 4, 120, &FONT_S, "B3 ORGAN v" APP_VER, C_ACC, 0);
}

static void app_loop(void)                      /* fm1-dock: a pass of app_main's loop, from the host's draw() */
{
    do {
        uint32_t keys, down, up, rel, b;
        int32_t e;
        uint8_t st, d1, d2;
        platform_poll();

        keys = fm1_in.notes;
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {             /* a key stops the note it started, whatever the octave is now */
            if (down >> k & 1u) {
                int32_t key = (int32_t)(53 + 12 * oct + (int32_t)k) - (int32_t)NOTE0;
                if (key >= 0 && key < (int32_t)NK) { held[k] = (uint8_t)key; key_set(&pk_lo, &pk_hi, (uint32_t)key, 1); }
            }
            if (up >> k & 1u && held[k] != 255) { key_set(&pk_lo, &pk_hi, held[k], 0); held[k] = 255; }
        }

        b = fm1_input_edges(&rel);
        if (b & (1u << BTN_OCT_DOWN) && oct > -2) { oct--; dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && oct < 2) { oct++; dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_FX)) P.vup ^= 1u;                    /* vibrato on / off */
        if (b & (1u << BTN_LFO)) P.les = (uint8_t)((P.les + 1u) % 3u);   /* Leslie: stop, slow, fast */
        if (b & (1u << BTN_ENV)) P.perc ^= 1u;
        if (b & (1u << BTN_SEL)) P.perc3 ^= 1u;
        if (b & (1u << BTN_ARP)) P.fast ^= 1u;
        if (b & (1u << BTN_SEQ)) P.soft ^= 1u;
        if (b & (1u << BTN_EDIT)) P.od ^= 1u;                   /* overdrive on / off */

        if ((e = fm1_enc_take(ENC_SELECT)) != 0) sel = (uint32_t)((int32_t)sel + e < 0 ? 0 : (int32_t)sel + e > 8 ? 8 : (int32_t)sel + e);
        if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) {
            int32_t v = (int32_t)P.db[sel] + e;
            P.db[sel] = (uint8_t)(v < 0 ? 0 : v > 8 ? 8 : v);
        }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {             /* setBfree's registrations, one after the other */
            uint32_t n = P.pgm ? P.pgm : 5u;
            for (; e > 0; e--) n = pgm_step(n, 1);
            for (; e < 0; e++) n = pgm_step(n, -1);
            organ_program(n);
        }
        if ((e = fm1_enc_take(ENC_KNOB1)) != 0) { int32_t v = (int32_t)P.drv + e; P.drv = (uint8_t)(v < 0 ? 0 : v > 8 ? 8 : v); }
        if ((e = fm1_enc_take(ENC_KNOB2)) != 0) { int32_t v = (int32_t)P.vtype + e; P.vtype = (uint8_t)(v < 0 ? 0 : v > 5 ? 5 : v); }
        if ((e = fm1_enc_take(ENC_KNOB3)) != 0) { int32_t v = (int32_t)P.les + e; P.les = (uint8_t)(v < 0 ? 0 : v > 2 ? 2 : v); }
        if ((e = fm1_enc_take(ENC_KNOB4)) != 0) { int32_t v = (int32_t)P.rev + e * 4; P.rev = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v); }   /* reverb mix, like CC 91 */

        while (midi_get(&st, &d1, &d2)) {       /* USB-MIDI in, any channel */
            uint32_t cmd = st & 0xF0u;
            uint32_t n = d1;                    /* a keyboard may play outside C2..C7: fold it in by octaves */
            while (n < NOTE0) n += 12u;
            while (n >= NOTE0 + NK) n -= 12u;
            if (cmd == 0x90u && d2) key_set(&mk_lo, &mk_hi, n - NOTE0, 1);
            else if (cmd == 0x80u || cmd == 0x90u) key_set(&mk_lo, &mk_hi, n - NOTE0, 0);
            else if (cmd == 0xB0u) organ_cc(d1, d2);            /* setBfree's controller numbers (organ_core.h) */
            else if (cmd == 0xC0u) organ_pc(d1);                /* program change: setBfree's default.pgm */
        }

        for (k = 0; k < 27u; k++)
            fm1_led_key(KEY_LED(k), held[k] != 255);
        fm1_led_key(BTN_OCT_DOWN, oct < 0);
        fm1_led_key(BTN_OCT_UP, oct > 0);
        fm1_led_key(BTN_FX, P.vup != 0);
        fm1_led_key(BTN_LFO, P.les != 0);
        fm1_led_key(BTN_ENV, P.perc != 0);
        fm1_led_key(BTN_EDIT, P.od != 0);

        for (i = 0; i < NB; i++)                /* draw only what changed */
            if (P.db[i] != drawn_db[i] || (i == sel) != (i == drawn_sel)) {
                drawn_db[i] = P.db[i];
                draw_row(i, sel);
            }
        drawn_sel = sel;
        {
            uint32_t sk, key = state_key(oct, &sk);
            if (key != last_key) { last_key = key; dirty = key != saved_key; t_change = fm1_ms; }
            if (sk != status_key) { status_key = sk; draw_status(); }   /* anything a knob, a button or MIDI changed */
        }
        (void)draw_battery;                     /* fm1-dock: no battery reading in a module */
        if (fm1_ms - t_load > 500u) {           /* how close to the 5800 us a block has: the number to watch */
            t_load = fm1_ms;
            draw_load(load_us, wheels_on);
            load_us = 0;
        }

        if (dirty && quiet() && fm1_ms - t_change > 2000u) {   /* an erase stalls the sound: save when silent */
            set.magic = 0xB5;
            for (i = 0; i < NB; i++) set.db[i] = P.db[i];
            set.vtype = P.vtype; set.vup = P.vup; set.perc = P.perc; set.perc3 = P.perc3; set.fast = P.fast;
            set.soft = P.soft; set.les = P.les; set.od = P.od; set.drv = P.drv; set.pgm = P.pgm; set.rev = P.rev; set.oct = (int8_t)oct;
            settings_save(&set, sizeof set);
            saved_key = last_key;
            dirty = 0;
        }
    } while (0);
}
