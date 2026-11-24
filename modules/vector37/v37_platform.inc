/* SPDX-License-Identifier: GPL-3.0-only */
/* What Vector37's own platform (vector37.c, its HAL, usb.c, storage.c) gives its engine, UI and screen, on the
 * fm1-dock host. The panel, the time and MIDI in: modules/lib/hb_fm1.h (fm1_in, the edges, the encoders), Vector37's
 * USB-MIDI packets made from the module's midi(); the LEDs lit and with the glow (hb_host_t.led 1 / 2); the screen:
 * Vector37's own gfx.c and draw.c onto the host's fill and blit; MASTER is the host's (it applies it). The store: the
 * settings in the module's settings record (16 B), the USER bank of sixteen sounds in its first bank (st_load /
 * st_save for storage.c's objects). The wavetables: the data pack V37-WT (hb_module.h "Data packs", take.py), read
 * in place from flash. What a module has no use for: USB, the update, the console, the TRS MIDI OUT (the FM-1's jack
 * is MIDI IN), the boot guard. */
#include <stdint.h>
#define fm1_led_key hbf_led_key                 /* (hb_fm1.h's, lit only: this file's has the glow beside it) */
#include "hb_fm1.h"
#undef fm1_led_key

#define BOARD_SMK37 0
#define MK_VERSION "0.3.2"
#define BRAND_NAME "VECTOR37"
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define HALF_FRAMES 256u
#define FM1_NBTN 14u                            /* buttons: ids 0..13 (hal/fm1_input.h's) */
static void fm1_wdt_feed(void) {}               /* (the host feeds the watchdog) */

/* ---- the LEDs: lit (fm1_led_key), and the glow (panel_fm1.c's FP_LED: 0 dark, 1 the glow, 2 lit) */
static uint64_t v37_lit, v37_glow, v37_sent_lit, v37_sent_glow;
static void v37_led(uint32_t id)
{
    uint64_t b = (uint64_t)1 << id;
    uint32_t v = (v37_lit & b) ? 1u : (v37_glow & b) ? 2u : 0u, was = (v37_sent_lit & b) ? 1u : (v37_sent_glow & b) ? 2u : 0u;
    if (v != was) {
        H->led(id, v);
        v37_sent_lit = (v37_sent_lit & ~b) | (v == 1u ? b : 0);
        v37_sent_glow = (v37_sent_glow & ~b) | (v == 2u ? b : 0);
    }
}
static void fm1_led_key(uint32_t id, int on)
{
    if (id >= 41u) return;
    v37_lit = on ? v37_lit | (uint64_t)1 << id : v37_lit & ~((uint64_t)1 << id);
    v37_led(id);
}
static void v37_fp_led(uint32_t id, uint32_t lvl)
{
    if (id >= 41u) return;
    v37_lit = lvl == 2u ? v37_lit | (uint64_t)1 << id : v37_lit & ~((uint64_t)1 << id);
    v37_glow = lvl == 1u ? v37_glow | (uint64_t)1 << id : v37_glow & ~((uint64_t)1 << id);
    v37_led(id);
}
#define FP_LED(id, lvl) v37_fp_led(id, lvl)
static void v37_leds_resend(void)               /* (back from the host's module list: every LED again) */
{
    uint32_t id;
    v37_sent_lit = ~v37_lit;
    v37_sent_glow = 0;
    for (id = 0; id < 41u; id++)
        v37_led(id);
}

/* ---- MASTER: the host's (it applies the pot after the module); panel_fm1.c reads none */
#define FM1_ADC_MASTER 0
static int32_t fm1_adc_read(uint32_t ch) { (void)ch; return -1; }
static volatile int32_t panel_master_q12 = 4096;

/* ---- the TRS MIDI OUT: the FM-1 has none (as Vector37's FM-1 build) */
static uint8_t trs_out;
static struct { uint8_t dead; } smo;
static void smo_put(uint32_t pkt) { (void)pkt; }
static void smo_on(uint32_t on) { (void)on; }

/* ---- MIDI in: as Vector37's usb.c queues it (USB-MIDI packets: CIN, status, d1, d2), from the module's midi() */
#define MQ 64u
static uint32_t midi_in_q[MQ];
static volatile uint32_t mi_w, mi_r;
static void v37_midi_in(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (st >= 0xF0u || mi_w - mi_r >= MQ)
        return;
    midi_in_q[mi_w % MQ] = (st >> 4) | st << 8 | (d1 & 127u) << 16 | (d2 & 127u) << 24;
    RING_PUBLISH();
    mi_w++;
}

/* ---- the screen: gfx.c's canvas, draw.c's tiles, onto the host's (blit: bytes as the panel takes them) */
static void lcd_sync(void) {}
static void lcd_sync_buf(const void *p, uint32_t n) { (void)p; (void)n; }
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { H->fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, c); }
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    H->blit((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px);
}

/* ---- the store (storage.c's interface): OBJ_SETTINGS in the settings record, the USER bank in the first bank */
enum { OBJ_SETTINGS, OBJ_SCENE0, OBJ_COUNT = OBJ_SCENE0 + 8 };
static uint8_t flash_ok = 1;
static int flash_probe(void) { return 1; }
static void st_owner_check(void) {}
#define ST_PAYLOAD_MAX HB_BANK_MAX
static uint8_t st_buf[ST_PAYLOAD_MAX] __attribute__((aligned(4)));
typedef struct { uint32_t len; } st_hdr_t;      /* (storage.c's record header: its length is all scenes.c reads) */
static int st_current(uint32_t obj, st_hdr_t *h)   /* the object into st_buf: 0, or -1 none (storage.c's) */
{
    int n;
    if (obj != OBJ_SCENE0 || !HB_HOST_HAS(H, bank_load_n) || (n = H->bank_load_n(1, st_buf, sizeof st_buf)) < 0)
        return -1;
    h->len = (uint32_t)n;
    return 0;
}
static int st_load(uint32_t obj, void *dst, uint32_t max)
{
    uint32_t i;
    int n;
    if (obj == OBJ_SETTINGS)
        return max <= 240u && !H->load(dst, max) ? (int)max : -1;
    if (obj != OBJ_SCENE0 || !HB_HOST_HAS(H, bank_load_n))
        return -1;
    n = H->bank_load_n(1, st_buf, sizeof st_buf);
    if (n < 0)
        return -1;
    if ((uint32_t)n > max)
        n = (int)max;
    for (i = 0; i < (uint32_t)n; i++)
        ((uint8_t *)dst)[i] = st_buf[i];
    return n;
}
static int st_save2(uint32_t obj, const void *src, uint32_t len, const void *src2, uint32_t len2)
{
    uint32_t i;
    if (obj == OBJ_SETTINGS && !src2 && len <= 240u) {
        H->save(src, len);
        return 0;
    }
    if (obj != OBJ_SCENE0 || len + len2 > sizeof st_buf || !HB_HOST_HAS(H, bank_save_n))
        return -1;
    for (i = 0; i < len; i++) st_buf[i] = ((const uint8_t *)src)[i];
    for (i = 0; i < len2; i++) st_buf[len + i] = ((const uint8_t *)src2)[i];
    return H->bank_save_n(1, st_buf, len + len2) ? -1 : 0;
}
static int st_save(uint32_t obj, const void *src, uint32_t len) { return st_save2(obj, src, len, 0, 0); }

/* ---- the wavetables: the data pack V37-WT (take.py makes it: "V3WT", bits, banks, frames, levels, frame size, the
 * levels' offsets and lengths; at 8 bits a scale per frame and level, then the samples) */
static const int8_t *v37_wt_s8;
static const int16_t *v37_wt_s16;
static const uint16_t *v37_wt_mul;
static int32_t v37_wt_at(uint32_t frame, uint32_t j)   /* level 0's sample j of frame (bank x WT_NF + k): the screen's */
{
    uint32_t at = frame * WT_FR + WT_LVL_OFF[0] + j;
    if (v37_wt_s16)
        return v37_wt_s16[at];
    if (v37_wt_s8)
        return (v37_wt_s8[at] * (int32_t)v37_wt_mul[frame * WT_NLVL]) >> 7;
    return 0;
}
static int v37_wt_find(void)                    /* 0: the pack is here and fits these tables */
{
    const uint8_t *p;
    uint32_t n = 0, i, nfl, hd;
    v37_wt_s8 = 0, v37_wt_s16 = 0, v37_wt_mul = 0;
    if (!HB_HOST_HAS(H, pack_find) || !(p = H->pack_find("V37-WT", &n)))
        return -1;
    hd = 10u + 4u * WT_NLVL;
    if (n < hd || p[0] != 'V' || p[1] != '3' || p[2] != 'W' || p[3] != 'T' || p[5] != WT_NB || p[6] != WT_NF ||
        p[7] != WT_NLVL || (uint32_t)(p[8] | p[9] << 8) != WT_FR)
        return -2;
    for (i = 0; i < WT_NLVL; i++)
        if ((uint32_t)(p[10 + 2 * i] | p[11 + 2 * i] << 8) != WT_LVL_OFF[i] ||
            (uint32_t)(p[10 + 2 * WT_NLVL + 2 * i] | p[11 + 2 * WT_NLVL + 2 * i] << 8) != WT_LVL_N[i])
            return -2;
    nfl = (uint32_t)WT_NB * WT_NF;
    if (p[4] == 16u && n == hd + 2u * nfl * WT_FR && !(((uint32_t)p + hd) & 1u))
        v37_wt_s16 = (const int16_t *)(p + hd);
    else if (p[4] == 8u && n == hd + 2u * nfl * WT_NLVL + nfl * WT_FR && !(((uint32_t)p + hd) & 1u)) {
        v37_wt_mul = (const uint16_t *)(p + hd);
        v37_wt_s8 = (const int8_t *)(p + hd + 2u * nfl * WT_NLVL);
    } else
        return -2;
    return 0;
}
