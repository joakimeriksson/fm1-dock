/* SPDX-License-Identifier: GPL-3.0-only */
/* Dinghy's platform calls (firmware/src/platform.c, hal/fm1_input.h) on the host's table, for a
 * firmware's own app code moved into a module with HB_F_PANEL: the panel as the firmware saw
 * it (fm1_in, the edges, the encoders, the LEDs), the time, MIDI in and out.
 *
 * The module's ui() hands every event to hbf_event(); its midi() to hbf_midi(). The app's loop
 * body then runs from draw() and reads them the way it read the hardware. Include it once,
 * after hb_module.h; the module sets H in init(). */
#ifndef HB_FM1_H
#define HB_FM1_H
#include "hb_module.h"

static const hb_host_t *H;

#define fm1_ms (H->ms())
static inline uint32_t fm1_ticks(void) { return H->ticks(); }
#define FM1_TICKS_PER_US 24u

static struct { uint32_t notes, buttons; } fm1_in;   /* bit k: note key k / button k held */
static uint32_t hbf_pressed, hbf_released, hbf_taps;
static int32_t hbf_enc[7];

static void hbf_event(uint32_t ev, int32_t arg)
{
    uint32_t id = HB_EV_ID(ev);
    switch (HB_EV_KIND(ev)) {
    case HB_EV_KEY_DOWN: if (id < 27u) { fm1_in.notes |= 1u << id; hbf_taps |= 1u << id; } break;
    case HB_EV_KEY_UP: if (id < 27u) fm1_in.notes &= ~(1u << id); break;
    case HB_EV_BTN_DOWN: if (id <= HB_BTN_RIGHT) { fm1_in.buttons |= 1u << id; hbf_pressed |= 1u << id; } break;
    case HB_EV_BTN_UP: if (id <= HB_BTN_RIGHT) { fm1_in.buttons &= ~(1u << id); hbf_released |= 1u << id; } break;
    case HB_EV_ENC: if (id < 7u) hbf_enc[id] += arg; break;
    default: break;
    }
}

/* buttons pressed since the last call (bit = button id); *released the ones let go */
static uint32_t fm1_input_edges(uint32_t *released)
{
    uint32_t p = hbf_pressed;
    if (released) *released = hbf_released;
    hbf_pressed = hbf_released = 0;
    return p;
}
/* note keys pressed since the last call, also those already let go again (a tap) */
static uint32_t fm1_input_note_edges(void)
{
    uint32_t t = hbf_taps;
    hbf_taps = 0;
    return t;
}
static int32_t fm1_enc_take(uint32_t e)
{
    int32_t c;
    if (e >= 7u) return 0;
    c = hbf_enc[e];
    hbf_enc[e] = 0;
    return c;
}
static void fm1_led_key(uint32_t id, int on) { H->led(id, (uint32_t)on); }

/* MIDI in (the module's midi() puts it here) and out */
#define HBF_MQ 64u
static uint32_t hbf_mq[HBF_MQ], hbf_mq_r, hbf_mq_w;
static void hbf_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (hbf_mq_w - hbf_mq_r < HBF_MQ)
        hbf_mq[hbf_mq_w++ % HBF_MQ] = st | d1 << 8 | d2 << 16;
}
static int midi_get(uint8_t *st, uint8_t *d1, uint8_t *d2)
{
    uint32_t m;
    if (hbf_mq_r == hbf_mq_w) return 0;
    m = hbf_mq[hbf_mq_r++ % HBF_MQ];
    *st = (uint8_t)m;
    *d1 = (uint8_t)(m >> 8);
    *d2 = (uint8_t)(m >> 16);
    return 1;
}
static void midi_send(uint32_t st, uint32_t d1, uint32_t d2) { H->midi_send(st, d1, d2); }

#endif
