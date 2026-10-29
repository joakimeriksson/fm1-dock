/* SPDX-License-Identifier: GPL-3.0-only */
/* What FM-106's platform.c (Dinghy's, with FM-106's dual-core worker) gives its fm106.c, on the fm1-dock
 * host: copied to fm106/platform.c by take.py, where fm106.c includes it (.inc here: not a module source).
 * The panel, the time and MIDI (modules/lib/hb_fm1.h); the LEDs, lit and the dim backlight of the buttons
 * (hb_host_t.led 1 / 2); the screen: FM-106's own gfx.c (Dinghy's canvas and font) onto the host's fill and
 * blit; the storage on the host's: the settings in the module's settings record (240 B) or its first bank,
 * the user presets in banks 1.. (bank_load_n / bank_save_n). The second core: FM-106's audio worker
 * (audio_worker.h) on the host's c1_run / c1_wait, the same one job in each render (voices 3..5 on core 1);
 * where the host has none to lend (not started, being screened, a unit that misreads) the engine renders
 * those voices on core 0 itself, as FM-106 does when its worker is not online.
 * USB audio is the host's (it records what the speaker plays, fed from its timer during the render), so
 * FELUCCA_UAC 0 only drops the engine's own uac_isr_service calls. What a module cannot have: SysEx in and
 * out (the Juno-106 librarian: the host keeps SysEx for its deploys), the battery reading (its indicator
 * stays off), its own MASTER law (the host's knob scales every module). */
#include <stdint.h>
#define fm1_led_key hbf_led_key                 /* (hb_fm1.h's, lit only: this file's has the backlight beside it) */
#include "hb_fm1.h"
#undef fm1_led_key

#ifndef APP_NAME
#define APP_NAME "FM-106"
#endif
#ifndef APP_VER
#define APP_VER "db59d58"
#endif
#define FS 44100u
#define HALF_FRAMES 256u
#define FELUCCA_UAC 0
#define DINGHY_DUAL_CORE 1

/* the panel by name, as Dinghy's platform.c */
enum { BTN_OCT_DOWN = 0, BTN_OCT_UP = 1, BTN_FX = 2, BTN_SEL = 3, BTN_ENV = 4, BTN_LFO = 5, BTN_EDIT = 6,
       BTN_GLO = 7, BTN_HOME = 8, BTN_SAVE = 9, BTN_ARP = 10, BTN_SEQ = 11, BTN_PLAY = 12, BTN_REC = 13 };
enum { ENC_SELECT = 0, ENC_ALGORITHM = 1, ENC_KNOB1 = 2, ENC_KNOB2 = 3, ENC_KNOB3 = 4, ENC_KNOB4 = 5, ENC_PRESETS = 6 };
#define KEY_LED(k) (14u + (k))

void *memcpy(void *d, const void *s, unsigned n);   /* (modules/lib/hb_lib.c) */
void *memset(void *d, int c, unsigned n);
static void str_cpy(char *d, const char *s, unsigned max)   /* (Dinghy's libc.c) */
{
    unsigned i = 0;
    while (s[i] && i + 1 < max) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}

/* the main loop's few moments with the audio interrupt's state (the CPU's own flag, no register) */
static void fm1_irq_off(void) { __asm__ volatile("cli" ::: "memory"); }
static void fm1_irq_on(void) { __asm__ volatile("csync\n\tsti" ::: "memory"); }
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
static uint32_t fm106_now_ms(void) { return fm1_ms; }

/* what fm106.c reads of the platform's state: USB audio's counters (none here), the audio ISR's late halves */
static struct { uint32_t overruns, underruns; uint8_t alt, starts, stalls, rate_sets, flowing, missed; } uac;
static volatile uint32_t audio_lates;
enum { FM1_ADC_BATT = 3, FM1_ADC_MASTER = 4 };
static int32_t fm1_adc_read(uint32_t ch) { (void)ch; return -1; }   /* (no reading: the battery stays off) */

/* ---- the LEDs: lit, and the buttons' dim backlight (fm1_led_bg as the key scan's map: row bit per column) */
#define FM1_NCOL 11u
static const int8_t FM1_KEYMAP[6][FM1_NCOL] = {   /* (hal/fm1_input.h) */
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    { 5, 11,  4, 10,  3,  9,  2,  8, -1, -1, -1},
    {34, 35, 36, 37, 38, 40, 39, 13,  7,  6, 12},
    {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},
    { 0,  1, 15, 14, 17, 16, 19, 18, 20, 21, 22},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
};
static uint8_t fm1_led_bg[FM1_NCOL];
static volatile uint16_t fm1_led_bg_ns;
static uint64_t fm106_lit, fm106_glow, fm106_sent_lit, fm106_sent_glow;
static void fm106_led(uint32_t id)
{
    uint64_t b = (uint64_t)1 << id;
    uint32_t v = (fm106_lit & b) ? 1u : (fm106_glow & b) ? 2u : 0u;
    uint32_t was = (fm106_sent_lit & b) ? 1u : (fm106_sent_glow & b) ? 2u : 0u;
    if (v != was) {
        H->led(id, v);
        fm106_sent_lit = (fm106_sent_lit & ~b) | (v == 1u ? b : 0);
        fm106_sent_glow = (fm106_sent_glow & ~b) | (v == 2u ? b : 0);
    }
}
static void fm1_led_key(uint32_t id, int on)
{
    if (id >= 41u) return;
    fm106_lit = on ? fm106_lit | (uint64_t)1 << id : fm106_lit & ~((uint64_t)1 << id);
    fm106_led(id);
}
static void fm106_leds_resend(void)             /* after fm106_app_init (its backlight map), and back from the host's list */
{
    uint32_t col, row, id;
    fm106_glow = 0;
    if (fm1_led_bg_ns)
        for (col = 0; col < FM1_NCOL; col++)
            for (row = 0; row < 6u; row++)
                if ((fm1_led_bg[col] >> row & 1u) && FM1_KEYMAP[row][col] >= 0)
                    fm106_glow |= (uint64_t)1 << FM1_KEYMAP[row][col];
    fm106_sent_lit = ~fm106_lit;
    fm106_sent_glow = 0;
    for (id = 0; id < 41u; id++)
        fm106_led(id);
}

/* ---- the screen: gfx.c's canvas onto the host's (blit: bytes as the panel takes them, as gfx.c makes them) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { H->fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, c); }
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    H->blit((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px);
}
#define CV_MAX (240u * 96u)                     /* (FM-106's: the biggest box drawn at once) */
#include "gfx.c"

/* ---- the storage */
static int fm106_put(uint32_t idx, const void *p, uint32_t n)   /* n bytes in banks from idx on (HB_BANK_MAX each) */
{
    const uint8_t *b = p;
    if (!HB_HOST_HAS(H, bank_free)) return -1;
    while (n) {
        uint32_t k = n < HB_BANK_MAX ? n : HB_BANK_MAX;
        if (H->bank_save_n(idx++, b, k)) return -1;
        b += k; n -= k;
    }
    return 0;
}
/* the first n bytes of what fm106_put saved from idx on: a bank is loaded whole (the host gives one only
 * whole, into the module's RAM), so through this buffer: FM-106 reads its presets' header first, onto
 * its stack */
static uint8_t fm106_bank_buf[HB_BANK_MAX];
static int fm106_get(uint32_t idx, void *p, uint32_t n)
{
    uint8_t *b = p;
    if (!HB_HOST_HAS(H, bank_free)) return -1;
    while (n) {
        uint32_t k = n < HB_BANK_MAX ? n : HB_BANK_MAX;
        if (H->bank_load_n(idx++, fm106_bank_buf, HB_BANK_MAX) < (int)k) return -1;
        memcpy(b, fm106_bank_buf, k);
        b += k; n -= k;
    }
    return 0;
}
/* the settings: the host's settings record (240 B), or bank 0 if they outgrow it */
static int settings_load(void *p, uint32_t n) { return n <= 240u ? H->load(p, n) : fm106_get(0, p, n); }
static int settings_save(const void *p, uint32_t n)
{
    if (n > 240u) return fm106_put(0, p, n);
    H->save(p, n);
    return 0;
}
/* the user presets (FM-106: 16 KiB of its own flash): banks 1, 2 (and 3) */
#define FM106_PRESETS_FLASH_SIZE 0x4000u
static int presets_flash_read(void *dst, uint32_t n) { return n <= FM106_PRESETS_FLASH_SIZE ? fm106_get(1, dst, n) : -1; }
static int presets_flash_save(const void *p, uint32_t n) { return n <= FM106_PRESETS_FLASH_SIZE ? fm106_put(1, p, n) : -1; }
static int fm106_sysex_send(const uint8_t *p, uint32_t n) { (void)p; (void)n; return -1; }   /* (no SysEx out) */

static void platform_poll(void) {}              /* (the host polls USB, the keys, MIDI, the MASTER knob) */

/* ---- the second core: FM-106's audio_worker.h on the host's (hb_host_t.c1_run / c1_wait), from the render only */
typedef void (*audio_worker_fn)(void *);
static struct { uint32_t jobs, max_job_ticks, max_wait_ticks, timeouts, last_job_ticks; } audio_worker;
static int audio_worker_online;                 /* the host lends one (fm106_module.c init) */
static audio_worker_fn aw_fn;
static void *aw_ctx;
static uint8_t aw_out;                          /* a job out: the render joins it */
static void aw_job(uint32_t arg)                /* on core 1 */
{
    uint32_t t0 = H->ticks(), dt;
    (void)arg;
    aw_fn(aw_ctx);
    dt = H->ticks() - t0;
    audio_worker.jobs++;
    audio_worker.last_job_ticks = dt;
    if (dt > audio_worker.max_job_ticks) audio_worker.max_job_ticks = dt;
}
static int audio_worker_submit(audio_worker_fn fn, void *context)
{
    if (!audio_worker_online || aw_out)
        return 0;
    aw_fn = fn;
    aw_ctx = context;
    aw_out = (uint8_t)H->c1_run(aw_job, 0);
    if (!aw_out)
        audio_worker.last_job_ticks = 0;        /* (none lent this block: core 0 renders it all) */
    return aw_out;
}
static void audio_worker_join(void)             /* (from the main loop too, with the interrupts off: nothing is out then) */
{
    if (!aw_out)
        return;
    aw_out = 0;
    if (H->c1_wait() != 0) {                    /* core 1 did not finish (it is held from now on): core 0 renders the
                                                 * voices, from where core 1 left them, this once */
        audio_worker.timeouts++;
        aw_job(0);
    }
}
