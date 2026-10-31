/* SPDX-License-Identifier: GPL-3.0-only */
/* What GATOR's platform (firmware/sys: Felucca by way of ORBIT; the app's side of it is fw.h) gives its app.c, on
 * the fm1-dock host: the panel as fw_input_read made it, a frame at a time (modules/lib/hb_fm1.h's events, in GATOR's
 * button and knob order), the LEDs (lit, what changed), MIDI in (the module's midi()), the screen (GATOR's gfx.c onto
 * the host's fill and blit), the store (the bank of presets in the module's first flash bank) and the audio load (the
 * render timed here, as audio.c times it). MASTER is the host's: the app's volume stays at full. What a module has
 * not: the boot logo, the battery reading, USB-MIDI out (GATOR sends none), the update loader. */
#include <stdint.h>
#define fm1_led_key hbf_led_key                 /* (hb_fm1.h's, every call to the host: this file's sends what changed) */
#include "hb_fm1.h"
#undef fm1_led_key
#undef fm1_ms                                   /* (fw.h's own: the frame's time, set before app_frame) */
#include "fw.h"

void *memcpy(void *d, const void *s, unsigned n);   /* (modules/lib/hb_lib.c; libc.c leaves them out with FW_HOST) */
void *memset(void *d, int c, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);
#include "libc.c"

#define FW_LOGO 0
#define FW_FRAME_MS 15u                         /* main.c's frame */

/* the main loop's few moments with the audio interrupt's state (the CPU's own flag, no register) */
static void fm1_irq_off(void) { __asm__ volatile("cli" ::: "memory"); }
static void fm1_irq_on(void) { __asm__ volatile("csync\n\tsti" ::: "memory"); }

/* ---- the panel: GATOR's FW_BTN_* and FW_KNOB_* from the host's ids (input.c's matrix) */
static const uint8_t GATOR_BTN[FW_BTN_COUNT] = {HB_BTN_FX, HB_BTN_SEL, HB_BTN_ENV, HB_BTN_LFO, HB_BTN_EDIT, HB_BTN_GLO,
                                                HB_BTN_HOME, HB_BTN_SAVE, HB_BTN_ARP, HB_BTN_SEQ, HB_BTN_PLAY,
                                                HB_BTN_REC, HB_BTN_OCT_DOWN, HB_BTN_OCT_UP};
static const uint8_t GATOR_ENC[FW_KNOB_COUNT] = {HB_ENC_SELECT, HB_ENC_ALGORITHM, HB_ENC_PRESETS, HB_ENC_KNOB1,
                                                 HB_ENC_KNOB2, HB_ENC_KNOB3, HB_ENC_KNOB4};
static uint32_t gator_labels(uint32_t ids)
{
    uint32_t out = 0, b;
    for (b = 0; b < FW_BTN_COUNT; b++)
        if (ids & 1u << GATOR_BTN[b])
            out |= 1u << b;
    return out;
}
static uint32_t gator_prev_keys;
static void fw_input_read(fw_input_t *in)       /* (input.c's, on the host's events) */
{
    uint32_t rel = 0, k;
    const uint32_t pressed = fm1_input_note_edges();
    const uint32_t keys = fm1_in.notes & ((1u << FW_KEYS) - 1u);
    const uint32_t btn_pressed = fm1_input_edges(&rel);
    memset(in, 0, sizeof *in);
    in->ms = fm1_ms;
    in->held = gator_labels(fm1_in.buttons);
    in->pressed = gator_labels(btn_pressed);
    in->released = gator_labels(rel);
    in->keys = keys;
    in->keys_pressed = pressed;
    in->keys_released = (gator_prev_keys | pressed) & ~keys;
    gator_prev_keys = keys;
    for (k = 0; k < FW_KNOB_COUNT; k++)
        in->knob[k] = fm1_enc_take(GATOR_ENC[k]);
    in->master = 1024u;                         /* (the volume at full, 1024^2 >> 12: the host applies MASTER) */
}

/* ---- the LEDs: what changed goes to the host; all of them again when the module's screen is back */
static uint64_t gator_lit, gator_sent = ~(uint64_t)0;
static void fm1_led_key(uint32_t id, int on)
{
    const uint64_t b = (uint64_t)1 << id;
    if (id >= 41u)
        return;
    gator_lit = on ? gator_lit | b : gator_lit & ~b;
    if ((gator_lit ^ gator_sent) & b) {
        H->led(id, on ? 1u : 0u);
        gator_sent ^= b;
    }
}
static void gator_leds_resend(void)
{
    uint32_t id;
    for (id = 0; id < 41u; id++)
        H->led(id, (uint32_t)(gator_lit >> id & 1u));
    gator_sent = gator_lit;
}
static void fw_led_button(uint32_t label, int on) { fm1_led_key(GATOR_BTN[label], on); }
static void fw_leds_keys(uint32_t mask)
{
    uint32_t k;
    for (k = 0; k < FW_KEYS; k++)
        fm1_led_key(HB_LED_KEY(k), (mask >> k) & 1u);
}

/* ---- MIDI in: what the module's midi() queued (hb_fm1.h), as USB-MIDI's cable 0 */
static int fw_midi_read(fw_midi_t *m)
{
    uint8_t st, d1, d2;
    if (!midi_get(&st, &d1, &d2))
        return 0;
    m->cable = 0;
    m->status = st;
    m->d1 = d1;
    m->d2 = d2;
    return 1;
}

/* ---- the store: GATOR's slot 0 (its bank of 16 presets and the global settings) in the module's first bank */
static int fw_store_load(uint32_t slot, void *dst, uint32_t max)
{
    if (slot || max > FW_STORE_MAX || !HB_HOST_HAS(H, bank_load))
        return -1;
    return H->bank_load(dst, max) == 0 ? (int)max : -1;   /* (only as saved: max is its size) */
}
static int fw_store_save(uint32_t slot, const void *src, uint32_t len)
{
    if (slot || len > FW_STORE_MAX || !HB_HOST_HAS(H, bank_save))
        return -1;
    return H->bank_save(src, len) ? -1 : 0;
}

/* ---- the audio load: audio.c's measure, the render's own time against its block (256 frames at 44.1 kHz) */
static fw_stats_t fw_stats;

/* ---- the screen: gfx.c's canvas onto the host's (blit: the bytes as the panel takes them, as gfx.c makes them) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) { H->fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, c); }
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    H->blit((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px);
}
#include "gfx.c"
