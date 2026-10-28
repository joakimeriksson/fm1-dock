/* SPDX-License-Identifier: GPL-3.0-only */
/* What CS-1's platform.c (on Dinghy's) gives its cs1.c, on the fm1-dock host: the panel, the time
 * and MIDI (modules/lib/hb_fm1.h), the screen calls (the host's canvas and font, the same as
 * gfx.c's), the settings record and the user presets' bank, the block, and the keys' sensing:
 * CS-1's own sense.c (Velo's, charlesvestal/fm1-velocity) runs in the host's key scan
 * (hb_host_t.sense_keys ...); its landings come as KEY_DOWN events with their velocity
 * (cs1_module.c makes sense_take()'s events of them), and the readings CS-1's pressure
 * (key_lean) works from are fetched before each loop (cs1_sense_sync).
 * The KEYS pages' SENSE, DRIVE, CURVE and LIGHT go to the host's sensing while CS-1 runs. What a
 * module cannot have is stubbed and says so:
 *   - the sensing's capture of the last press, its per-key calibration storage, the top row's probe;
 *   - (the second core, audio_worker, is the host's: hb_host_t.c1_run / c1_wait, when it has one);
 *   - the updater drawing over the screen (platform_screen_take), the TRS ring's drop count. */
#ifndef CS1_PLATFORM_H
#define CS1_PLATFORM_H
#include <stdint.h>
#include "hb_fm1.h"

#ifndef APP_VER
#define APP_VER "0.3.2"                         /* CS-1's VERSION */
#endif
#define FS 44100u
#define HALF_FRAMES 256u
#define FM1_POOL                                /* (the module's RAM: all of it .bss) */

enum { BTN_OCT_DOWN = 0, BTN_OCT_UP = 1, BTN_FX = 2, BTN_SEL = 3, BTN_ENV = 4, BTN_LFO = 5, BTN_EDIT = 6,
       BTN_GLO = 7, BTN_HOME = 8, BTN_SAVE = 9, BTN_ARP = 10, BTN_SEQ = 11, BTN_PLAY = 12, BTN_REC = 13 };
enum { ENC_SELECT = 0, ENC_ALGORITHM = 1, ENC_KNOB1 = 2, ENC_KNOB2 = 3, ENC_KNOB3 = 4, ENC_KNOB4 = 5, ENC_PRESETS = 6 };
#define KEY_LED(k) (14u + (k))

/* ---- the screen (gfx.c / lcd.c's calls; the host draws with the same fonts) */
#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define C_BLACK 0x0000u
#define C_WHITE 0xFFFFu
typedef struct { uint32_t h, big; } felucca_font_t;
static const felucca_font_t FONT_S = {16, 0}, FONT_L = {32, 1};
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { H->fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, c); }
static void cv_begin(uint32_t w, uint32_t h, uint16_t bg) { H->cv_begin((int32_t)w, (int32_t)h, bg); }
static void cv_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) { H->cv_rect(x, y, w, h, c); }
static void cv_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c) { H->cv_line(x0, y0, x1, y1, c); }
static int32_t text_w(const felucca_font_t *f, const char *s) { return H->text_w(f->big, s); }
static int32_t cv_text(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    H->cv_text(x, y, f->big, s, c);
    return x + text_w(f, s);
}
static void cv_blit(uint32_t x, uint32_t y) { H->cv_blit((int32_t)x, (int32_t)y); }
static int platform_screen_take(void) { return 0; }

/* ---- settings and the user presets */
static int settings_load(void *p, uint32_t n) { return H->load(p, n); }
static int settings_save(const void *p, uint32_t n) { H->save(p, n); return 0; }
static int bank_load(void *p, uint32_t n) { return H->bank_load(p, n); }
static int bank_save(const void *p, uint32_t n) { return H->bank_save(p, n); }
static uint32_t midi_in_drops(void) { return 0; }

/* ---- the second core: the host's (hb_host_t.c1_run / c1_wait), when it has one and it passed its checks.
 * CS-1 hands it the odd voices and the reverb (cs1_voice.h voices_block); without it, all on core 0 */
typedef void (*audio_worker_fn)(void *);
static struct { uint32_t last_job_ticks; } audio_worker;
static audio_worker_fn aw_fn;
static void *aw_ctx;
static void aw_job(uint32_t arg)                /* on core 1 (or here, after a timeout) */
{
    uint32_t t0 = H->ticks();
    (void)arg;
    aw_fn(aw_ctx);
    audio_worker.last_job_ticks = H->ticks() - t0;
}
static int audio_worker_submit(audio_worker_fn fn, void *ctx)
{
    if (!HB_HOST_HAS(H, c1_wait))
        return 0;
    aw_fn = fn;
    aw_ctx = ctx;
    return H->c1_run(aw_job, 0);
}
static void audio_worker_join(void)
{
    if (H->c1_wait() != 0)
        aw_job(0);                              /* core 1 did not finish in time (it is held now): here, once */
}

/* ---- the keys' sensing: the host's (see the top) */
enum { EV_ON = 1, EV_OFF = 2 };
enum { SN_DOWN = 2 };                           /* (sense.c's states: hb_key_sense_t.state) */
#define SN_DBASE HB_SENSE_DBASE
typedef struct { uint8_t type, key, vel, dt_ms; } sense_ev_t;
static struct { uint8_t st; uint16_t d, dland; } sn_key[27];
static struct { uint8_t drive; uint16_t d_curve, d_ceil; } sn_cfg;   /* (cs1.c sets them, then keys_sense_set) */
static sense_ev_t cs1_ev[16];
static uint8_t cs1_ev_r, cs1_ev_w;
static int cs1_has_sense(void) { return HB_HOST_HAS(H, sense_set); }   /* (an older host: no sensing) */
static uint32_t cs1_sensed(void) { return cs1_has_sense() ? H->sense_keys() : 0; }
static void sense_init(uint32_t mask) { (void)mask; }
static uint32_t keys_analog_mask(void) { return cs1_sensed(); }
static void keys_sense_set(uint32_t on, uint32_t drive)
{
    if (cs1_has_sense())
        H->sense_set(on, drive, sn_cfg.d_curve, sn_cfg.d_ceil);
}
static int keys_sense_live(void) { return cs1_sensed() != 0; }
static int sense_is_analog(uint32_t k) { return k < 27u && (cs1_sensed() >> k & 1u); }
static int sense_take(sense_ev_t *e)
{
    if (cs1_ev_r == cs1_ev_w)
        return 0;
    *e = cs1_ev[cs1_ev_r++ % 16u];
    return 1;
}
static void cs1_sense_ev(uint32_t type, uint32_t key, uint32_t vel)   /* from ui(): a sensed key landed / lifted */
{
    if ((uint8_t)(cs1_ev_w - cs1_ev_r) < 16u) {
        sense_ev_t *e = &cs1_ev[cs1_ev_w++ % 16u];
        e->type = (uint8_t)type;
        e->key = (uint8_t)key;
        e->vel = (uint8_t)vel;
        e->dt_ms = 0;
    }
}
static void cs1_sense_sync(void)                /* the readings cs1.c's pressure works from, before its loop */
{
    uint32_t k, m = cs1_sensed();
    hb_key_sense_t r;
    for (k = 0; k < 27u; k++) {
        if (m >> k & 1u && !H->key_sense(k, &r)) {
            sn_key[k].st = r.state;
            sn_key[k].d = r.d;
            sn_key[k].dland = r.dland;
        } else {
            sn_key[k].st = 0;
        }
    }
}

#endif
