/* SPDX-License-Identifier: GPL-3.0-only */
/* What MONK-1's platform (its app/plat.h, plat_fm1.c, sense.c) gives MONK-1, on the fm1-dock host: the panel, the time
 * and MIDI (modules/lib/hb_fm1.h, the panel in MONK-1's own order: B_* and EN_*), the lights (lit, and the glow of the
 * unlit buttons), the keys' force (the host's sensing, Velo's as MONK-1 has it: its landings come as KEY_DOWN events
 * with their velocity, its readings through key_sense), the screen (MONK-1's gfx.c onto the host's fill and blit) and
 * the store (the project in the module's first bank). What a module has not: the screen reader over USB (its ann_*
 * calls do nothing), the top row's velocity through the PA10 probe (those keys play at 100, as MONK-1's do when the
 * probe reads nothing), the sensing's WAIT 2 (the host's velocity is read at WAIT 1). As
 * modules/fomni/fomni_platform.inc, and the sensing as modules/cs1/cs1_platform.h. */
#include <stdint.h>
#include "hb_fm1.h"
#include "app.h"                                /* (plat.h, monk.h, art.h: the panel, the store objects, the voice) */

void *memcpy(void *d, const void *s, unsigned n);   /* (modules/lib/hb_lib.c) */
void *memset(void *d, int c, unsigned n);
static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }   /* (main.c's) */

/* ---- the panel: MONK-1's B_FX .. B_OCTUP and EN_SELECT .. EN_K4, from the host's ids */
static const uint8_t MK_BTN[NB] = {HB_BTN_FX, HB_BTN_SEL, HB_BTN_ENV, HB_BTN_LFO, HB_BTN_EDIT, HB_BTN_GLO, HB_BTN_HOME,
                                   HB_BTN_SAVE, HB_BTN_ARP, HB_BTN_SEQ, HB_BTN_PLAY, HB_BTN_REC, HB_BTN_OCT_DOWN,
                                   HB_BTN_OCT_UP};
static const uint8_t MK_ENC[NE] = {HB_ENC_SELECT, HB_ENC_ALGORITHM, HB_ENC_PRESETS, HB_ENC_KNOB1, HB_ENC_KNOB2,
                                   HB_ENC_KNOB3, HB_ENC_KNOB4};

uint32_t plat_ms(void) { return H->ms(); }
/* held, as MONK-1 reads the panel; a press that came and went since the last read (the host hands the module events,
 * MONK-1 polls) counts as held for that one read, so its release is seen at the next */
uint32_t plat_buttons(void)
{
    uint32_t b = 0, i, held = fm1_in.buttons | fm1_input_edges(0);
    for (i = 0; i < NB; i++)
        if (held & 1u << MK_BTN[i])
            b |= 1u << i;
    return b;
}
uint32_t plat_keys(void) { return (fm1_in.notes | fm1_input_note_edges()) & ((1u << NKEYS) - 1u); }
int32_t plat_enc(int role) { return role >= 0 && role < NE ? fm1_enc_take(MK_ENC[role]) : 0; }

/* ---- the lights: what changed goes to the host; every button not lit glows (MONK-1's dim light) */
static uint32_t mk_lit, mk_sent = ~0u;
static void mk_lights(int all)
{
    uint32_t i;
    for (i = 0; i < NB; i++)
        if (all || ((mk_lit ^ mk_sent) >> i & 1u))
            H->led(MK_BTN[i], (mk_lit >> i & 1u) ? 1u : 2u);
    mk_sent = mk_lit;
}
void plat_leds(uint32_t buttons, uint32_t keys)  /* (no key lights: MONK-1 lights none) */
{
    (void)keys;
    mk_lit = buttons & ((1u << NB) - 1u);
    mk_lights(0);
}

/* ---- MIDI: USB-MIDI packets (byte 0 the code index, then status, data 1, data 2) */
int plat_midi_in(uint32_t *pkt)
{
    uint8_t st, d1, d2;
    if (!midi_get(&st, &d1, &d2))
        return 0;
    *pkt = (uint32_t)(st >> 4) | (uint32_t)st << 8 | (uint32_t)d1 << 16 | (uint32_t)d2 << 24;
    return 1;
}
void plat_midi3(uint32_t st, uint32_t d1, uint32_t d2) { H->midi_send(st, d1, d2); }

/* ---- the store: the project (OBJ_PROJ, 840 B) in the module's first bank (8 KiB); the panel calibration is the host's */
static int mk_banks(void) { return HB_HOST_HAS(H, bank_free); }
int plat_store_load(uint32_t obj, void *dst, uint32_t max)
{
    if (obj != OBJ_PROJ || !mk_banks())
        return -1;
    return H->bank_load_n(1u, dst, max);
}
int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    if (obj != OBJ_PROJ || !mk_banks() || len > PLAT_STORE_MAX)
        return -1;
    return H->bank_save_n(1u, src, len) ? -1 : 0;
}

/* ---- the keys' force: the host's sensing in sense.c's terms (the states, the driven reading ui.c's pressure works
 * from, the landings), synced before each of MONK-1's input reads (monk_module.c) */
#define SN_NKEYS 27
#define SN_TOP0 20u                             /* the first top key (no converter) */
#define SN_DBASE HB_SENSE_DBASE
enum { SN_OPEN, SN_TOUCH, SN_DOWN, SN_LIFT };
enum { EV_NONE, EV_ON, EV_OFF, EV_GRAZE };
typedef struct { uint8_t type, key, vel, dt_ms; } sense_ev_t;
static struct { uint8_t st, dn; uint16_t d; } sn_key[SN_NKEYS];
static struct {                                 /* (what project.c's sense_setup sets: drive, curve and ceiling go to the host) */
    uint16_t touch, press, release;
    uint8_t wait;
    uint16_t v_lo, v_hi;
    uint8_t vmax;
    uint16_t at_range;
    uint8_t drive;
    uint16_t d_curve, d_ceil, d_at_range, t_lo, t_hi;
    uint8_t learn;
} sn_cfg;
static uint32_t mk_sensed;                      /* the keys whose force is sensed now */
static sense_ev_t mk_ev[16];
static uint8_t mk_ev_r, mk_ev_w;
static uint32_t mk_sdown;                       /* the sensed keys that landed (their lift is sense.c's too) */
static int mk_has_sense(void) { return HB_HOST_HAS(H, sense_set); }   /* (an older host: no sensing, all keys at 100) */
static void sense_cal_reset(void) {}            /* (the calibration is off in MONK-1) */
void plat_analog_config(uint32_t div, uint32_t delay, uint32_t immediate, uint32_t drive, uint32_t probe)
{
    (void)div; (void)delay; (void)immediate; (void)probe;
    if (mk_has_sense())
        H->sense_set(1, drive, sn_cfg.d_curve, sn_cfg.d_ceil);
}
static int sense_is_analog(uint32_t k) { return k < SN_NKEYS && (mk_sensed >> k & 1u); }
static uint8_t sense_probe_velocity(uint32_t k) { (void)k; return 0; }
static int sense_take(sense_ev_t *e)
{
    if (mk_ev_r == mk_ev_w)
        return 0;
    *e = mk_ev[mk_ev_r++ % 16u];
    return 1;
}
static void mk_sense_ev(uint32_t type, uint32_t key, uint32_t vel)   /* from ui(): a sensed key landed / lifted */
{
    if ((uint8_t)(mk_ev_w - mk_ev_r) < 16u) {
        sense_ev_t *e = &mk_ev[mk_ev_w++ % 16u];
        e->type = (uint8_t)type;
        e->key = (uint8_t)key;
        e->vel = (uint8_t)vel;
        e->dt_ms = 0;
    }
}
static void mk_sense_sync(void)
{
    uint32_t k;
    hb_key_sense_t r;
    mk_sensed = mk_has_sense() ? H->sense_keys() : 0;
    for (k = 0; k < SN_NKEYS; k++) {
        if (mk_sensed >> k & 1u && !H->key_sense(k, &r)) {
            sn_key[k].st = r.state;
            sn_key[k].d = r.d;
            sn_key[k].dn = r.d != 0;            /* (a driven reading this press: the host's d is the last one) */
        } else {
            sn_key[k].st = SN_OPEN;
            sn_key[k].dn = 0;
        }
    }
}

/* ---- the screen: gfx.c's canvas onto the host's (the colours as RGB565 to fill, the canvas already as the panel
 * takes it to blit) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { H->fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, c); }
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px) { H->blit((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px); }
#include "gfx.c"

/* ---- the screen reader (MONK-1's announce.c, over USB): not in a module */
#define ANN_MAX 240u
#define ANN_NOW 1u
#define ANN_KEEP 2u
static int ann_on(void) { return 0; }
static void ann_say(const char *a, const char *b, uint32_t flags) { (void)a; (void)b; (void)flags; }
static void ann_later(const char *s, uint32_t flags, uint32_t quiet_ms, uint32_t now) { (void)s; (void)flags; (void)quiet_ms; (void)now; }
static int ann_where(void) { return 0; }
static void ann_tick(uint32_t now) { (void)now; }
