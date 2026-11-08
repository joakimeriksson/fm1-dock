/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Javier (Animatek) */
/* ATEK303 on fm1-base: the app's half of the contract with the platform (fm1-base's template, adapted). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "ATEK303"
#define APP_SPLASH_RGB RGB(44, 127, 255)       /* Animatek blue */
#define APP_BLOCK 256u                         /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_TOUCH                              /* velocity from the keys: a hard press is an accent */
#define APP_SPLASH                             /* our own first screen: the acid smiley (app_splash) */
void app_splash(void);

/* the store: the app's settings, the panel calibration (the platform's), and their flash offsets */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_PRESETS_A, OBJ_PRESETS_B, OBJ_PRESETS_C, OBJ_PRESETS_D, OBJ_OPTIONS, OBJ_NOBJ };   /* presets: 8 in each */
/* in the main store (0x97000..0xE0000); OBJ_OPTIONS (the OPTIONS menu: colour, MIDI, fine tuning) is the unit's own,
 * apart from the settings so that no preset carries it, and not in the backup */
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xD8000u, 0xDA000u, 0xD4000u, 0xD6000u, 0xDE000u}
/* backup and restore over USB (fm1.toml backup = true; tools/fm1_backup.py --magic AT64): the settings and the presets.
 * A backup is also how Javier's presets become the factory ones (tools/factory_from_backup.py) */
#define APP_BACKUP_OBJS {OBJ_SETTINGS, OBJ_PRESETS_A, OBJ_PRESETS_B, OBJ_PRESETS_C, OBJ_PRESETS_D}
#define APP_BACKUP_MAGIC 0x34365441u           /* "AT64" */
#define APP_BACKUP_VERSION 1u
#define APP_BACKUP_WHAT "replaces all presets"
int app_backup_playing(void);
void app_backup_flush(void);
void app_backup_hold(int on);
void app_backup_restored(void);

void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
