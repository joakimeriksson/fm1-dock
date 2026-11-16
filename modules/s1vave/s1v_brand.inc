/* SPDX-License-Identifier: GPL-3.0-only */
/* S1VAVE in fm1-dock: the header's name and the boot splash in plain text, in place of S1Vave's logo_s1vave.h and
 * its splash (Andrew GG's logo and brand: S1Vave's TRADEMARK.md asks a repackaged firmware to leave them out).
 * The splash names the author and says that this is fm1-dock's port (modules/s1vave/take.py). */
#define LOGO_W 48                               /* "S1VAVE": six of FONT_S's 8-pixel characters */
#define LOGO_H 16

static void draw_logo_small(int32_t x0, int32_t y0)
{
    cv_text(x0, y0 - 2, &FONT_S, "S1VAVE", C_CREAM);
}

static void ui_splash(void)                     /* about a second: the module starts from the host's list */
{
    static const char *const L[] = {"S1VAVE", "8-VOICE SYNTH", "", "BY ANDREW GG", "FM1-DOCK PORT, UNOFFICIAL"};
    uint32_t t0 = fm1_ms, i;
    lcd_fill(0, 0, 240, 240, C_PANEL);
    cv_begin(240, 120, C_PANEL);
    for (i = 0; i < 5u; i++)
        cv_text((240 - text_w(&FONT_S, L[i])) / 2, 14 + 20 * (int32_t)i, &FONT_S, L[i], i ? C_DIMT : C_CREAM);
    cv_blit(0, 60);
    while (fm1_ms - t0 < 1000u) platform_poll();
    lcd_fill(0, 0, 240, 240, C_PANEL);
}
