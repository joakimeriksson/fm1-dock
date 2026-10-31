/* SPDX-License-Identifier: GPL-3.0-only */
/* What DUB-1's platform (its app/plat.h, plat_fm1.c, main_fm1.c) gives DUB-1, on the fm1-dock host: the panel, the time
 * and MIDI (modules/lib/hb_fm1.h, the panel in DUB-1's own order: B_* and EN_*), the lights (lit, and the glow of the
 * unlit buttons), the screen (DUB-1's gfx.c onto the host's fill and blit), the store (the project in the module's
 * first bank) and the audio load (the render timed here, by stage as main_fm1.c times it). What a module has not: the
 * screen reader over USB (its ann_* calls do nothing), the battery's reading, the clock's registers, PLAY's red LED.
 * As modules/fomni/fomni_platform.inc, whose platform DUB-1's grew from (by way of AMB-1). */
#include <stdint.h>
#include "hb_fm1.h"
#include "app.h"                                /* (plat.h: the panel, the store objects) */

void *memcpy(void *d, const void *s, unsigned n);   /* (modules/lib/hb_lib.c) */
void *memset(void *d, int c, unsigned n);

/* ---- the panel: DUB-1's B_FX .. B_OCTUP and EN_SELECT .. EN_K4, from the host's ids */
static const uint8_t DUB_BTN[NB] = {HB_BTN_FX, HB_BTN_SEL, HB_BTN_ENV, HB_BTN_LFO, HB_BTN_EDIT, HB_BTN_GLO, HB_BTN_HOME,
                                    HB_BTN_SAVE, HB_BTN_ARP, HB_BTN_SEQ, HB_BTN_PLAY, HB_BTN_REC, HB_BTN_OCT_DOWN,
                                    HB_BTN_OCT_UP};
static const uint8_t DUB_ENC[NE] = {HB_ENC_SELECT, HB_ENC_ALGORITHM, HB_ENC_PRESETS, HB_ENC_KNOB1, HB_ENC_KNOB2,
                                    HB_ENC_KNOB3, HB_ENC_KNOB4};

uint32_t plat_ms(void) { return H->ms(); }
/* held, as DUB-1 reads the panel; a press that came and went since the last read (the host hands the module events,
 * DUB-1 polls) counts as held for that one read, so its release is seen at the next */
uint32_t plat_buttons(void)
{
    uint32_t b = 0, i, held = fm1_in.buttons | fm1_input_edges(0);
    for (i = 0; i < NB; i++)
        if (held & 1u << DUB_BTN[i])
            b |= 1u << i;
    return b;
}
uint32_t plat_keys(void) { return (fm1_in.notes | fm1_input_note_edges()) & ((1u << NKEYS) - 1u); }
int32_t plat_enc(int role) { return role >= 0 && role < NE ? fm1_enc_take(DUB_ENC[role]) : 0; }
uint32_t plat_master(void) { return 4096u; }   /* (the host applies MASTER) */

/* ---- the lights: what changed goes to the host (lit 1, the glow 2) */
static uint32_t dub_lit_b, dub_lit_k, dub_glow = 1, dub_sent_b = ~0u, dub_sent_k = ~0u, dub_sent_glow = ~0u;
static void dub_lights(int all)
{
    uint32_t i;
    for (i = 0; i < NB; i++) {
        uint32_t m = 1u << i, v = (dub_lit_b & m) ? 1u : dub_glow ? 2u : 0u;
        uint32_t was = (dub_sent_b & m) ? 1u : dub_sent_glow ? 2u : 0u;
        if (all || v != was)
            H->led(DUB_BTN[i], v);
    }
    for (i = 0; i < NKEYS; i++) {
        uint32_t m = 1u << i;
        if (all || ((dub_lit_k ^ dub_sent_k) & m))
            H->led(HB_LED_KEY(i), (dub_lit_k & m) ? 1u : 0u);
    }
    dub_sent_b = dub_lit_b;
    dub_sent_k = dub_lit_k;
    dub_sent_glow = dub_glow;
}
void plat_leds(uint32_t buttons, uint32_t keys)
{
    dub_lit_b = buttons & ((1u << NB) - 1u);
    dub_lit_k = keys;
    dub_lights(0);
}
void plat_glow(int on) { dub_glow = on != 0; }   /* (before plat_leds, which sends it) */
void plat_play_red(int red) { (void)red; }       /* (PLAY's red LED: the host lights a button one way) */

/* ---- MIDI: USB-MIDI packets (byte 0 the code index, then status, data 1, data 2) */
int plat_midi_in(uint32_t *pkt)
{
    uint8_t st, d1, d2;
    if (!midi_get(&st, &d1, &d2))
        return 0;
    *pkt = (uint32_t)(st >> 4) | (uint32_t)st << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24;
    return 1;
}
void plat_midi_out(uint32_t pkt) { H->midi_send(pkt >> 8 & 0xFFu, pkt >> 16 & 0xFFu, pkt >> 24 & 0xFFu); }
void en_midi_out(uint32_t st, uint32_t d1, uint32_t d2) { H->midi_send(st, d1, d2); }   /* (plat_fm1.c's: the engine's) */

/* ---- the store: the project (OBJ_PROJ) in the module's first bank (8 KiB); the panel calibration is the host's */
static int dub_banks(void) { return HB_HOST_HAS(H, bank_free); }
int plat_store_load(uint32_t obj, void *dst, uint32_t max)
{
    if (obj != OBJ_PROJ || !dub_banks())
        return -1;
    return H->bank_load_n(1u, dst, max);
}
int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    if (obj != OBJ_PROJ || !dub_banks() || len > PLAT_STORE_MAX)
        return -1;
    return H->bank_save_n(1u, src, len) ? -1 : 0;
}

/* ---- the audio load: the render's own time against its block's (256 frames at 44.1 kHz: 5805 us), smoothed, the
 * heaviest block of each ~2 s and its stages (main_fm1.c's render_block) */
static volatile uint32_t dub_cpu, dub_peak, dub_xruns, dub_pk[PLAT_NPROF], dub_mean[PLAT_NPROF];
uint32_t plat_cpu_pct(void) { return dub_cpu; }
uint32_t plat_cpu_peak(void) { return dub_peak; }
uint32_t plat_xruns(void) { return dub_xruns; }
uint32_t en_prof_ticks(void) { return H->ticks(); }   /* (amb.c's stages: the host's 24 MHz counter) */
void plat_prof(uint32_t peak[PLAT_NPROF], uint32_t mean[PLAT_NPROF])
{
    int i;
    for (i = 0; i < PLAT_NPROF; i++)
        peak[i] = dub_pk[i] / 24u, mean[i] = dub_mean[i] / 24u;
}
_Static_assert(PLAT_NPROF == EN_PROF_N, "plat.h's stages are amb.h's");
static void dub_load(uint32_t ticks)
{
    static uint32_t q8, win_max, win_n, win_pk[PLAT_NPROF], win_sum[PLAT_NPROF];
    uint32_t pct = ticks * 100u / (5805u * 24u), k;
    q8 = (q8 * 15u + ticks * 256u / (5805u * 24u)) / 16u;
    dub_cpu = (q8 * 100u) >> 8;
    en_load = (uint8_t)(pct > 255u ? 255u : pct);   /* (the engine sheds over 75: en_render) */
    if (pct >= 100u)
        dub_xruns++;
    if (pct >= win_max) {
        win_max = pct;
        for (k = 0; k < PLAT_NPROF; k++)
            win_pk[k] = en_prof[k];
    }
    for (k = 0; k < PLAT_NPROF; k++)
        win_sum[k] += en_prof[k];
    if (++win_n >= 344u) {
        dub_peak = win_max;
        for (k = 0; k < PLAT_NPROF; k++) {
            dub_pk[k] = win_pk[k];
            dub_mean[k] = win_sum[k] / 344u;
            win_sum[k] = 0;
        }
        win_max = 0;
        win_n = 0;
    }
}

/* ---- what the FM-1 has and a module not: the battery's reading and the USB's power (none: the gauge hidden), the
 * CPU's clock and its registers (0: not measured) */
int32_t plat_battery(void) { return -1; }
int plat_usb_power(void) { return 0; }
uint32_t plat_clock_mhz(void) { return 0; }
void plat_clock_regs(uint32_t r[3]) { r[0] = r[1] = r[2] = 0; }

/* ---- the screen: gfx.c's canvas onto the host's (the colours as RGB565 to fill, the canvas already as the panel
 * takes it to blit) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { H->fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, c); }
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px) { H->blit((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px); }
#include "gfx.c"

/* ---- the screen reader (announce.c, over USB): not in a module */
#define ANN_MAX 240u
#define ANN_NOW 1u
#define ANN_KEEP 2u
static int ann_on(void) { return 0; }
static void ann_say(const char *a, const char *b, uint32_t flags) { (void)a; (void)b; (void)flags; }
static void ann_later(const char *s, uint32_t flags, uint32_t quiet_ms, uint32_t now) { (void)s; (void)flags; (void)quiet_ms; (void)now; }
static int ann_where(void) { return 0; }
static void ann_tick(uint32_t now) { (void)now; }
