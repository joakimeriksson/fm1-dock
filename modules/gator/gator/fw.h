/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef FW_H
#define FW_H
#include <stdint.h>
#include "fw_rate.h"

#define FW_BLOCK 256u
#define FW_KEYS 27u
#define FW_KEY_BASE_NOTE 53u
#define FW_STORE_SLOTS 2u
#define FW_STORE_MAX 3840u

#ifdef FW_HOST
#define FW_POOL
#else
#define FW_POOL __attribute__((section(".pool")))
#endif

#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define FW_NELEM(a) (sizeof(a) / sizeof((a)[0]))
#define FW_BIT(n) (1u << (n))

enum {
    FW_BTN_FX, FW_BTN_SCL, FW_BTN_ENV, FW_BTN_LFO, FW_BTN_EDIT, FW_BTN_GLO, FW_BTN_HOME,
    FW_BTN_SAVE, FW_BTN_ARP, FW_BTN_SEQ, FW_BTN_PLAY, FW_BTN_REC, FW_BTN_OCT_DOWN, FW_BTN_OCT_UP,
    FW_BTN_COUNT
};

enum {
    FW_KNOB_SELECT, FW_KNOB_ALGO, FW_KNOB_PRESET, FW_KNOB_1, FW_KNOB_2, FW_KNOB_3, FW_KNOB_4,
    FW_KNOB_COUNT
};

typedef struct {
    uint32_t ms;
    uint32_t held, pressed, released;
    uint32_t keys, keys_pressed, keys_released;
    int32_t knob[FW_KNOB_COUNT];
    uint32_t master;
    uint32_t battery;
} fw_input_t;

typedef struct {
    uint8_t cable, status, d1, d2;
} fw_midi_t;

typedef struct {
    uint32_t cpu_q8;
    uint32_t audio_max_us;
    uint32_t audio_late;
    uint32_t usb_up;
} fw_stats_t;

static volatile uint32_t fm1_ms;

static void app_init(void);
static void app_frame(const fw_input_t *in);
static void app_audio(int32_t *lr, uint32_t frames);

#endif
