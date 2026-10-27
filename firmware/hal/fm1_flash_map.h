/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * SMK-37 flash map after the reference port's: Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* The SMK-37's flash map for fm1-dock (BOARD_SMK37; fm1_flash.h includes it): which ranges the RAM flash
 * driver may erase / program, and the ranges nothing may touch. Macros and compile-time checks only.
 *
 *   0x00000-0x03FFF  head (SPL, isd_config)                       NEVER
 *   0x04000-0x6FFFF  the app: the host, its modules, HX (BLE-MIDI) the update loader only (FL_APP)
 *                    at 0x65000
 *   0x70000-0xB7FFF  deployed modules (area 1, 288 KiB)           the plain window starts here
 *   0xB8000-0xBAFFF  the deployed table (two copies), settings
 *   0xBB000-0xBFFFF  update staging + record (ota.c)              (the stock VM's end, as the reference port)
 *   0xC0000-0xC1FFF  BTIF (pairing), USRTRIM (the key / pad / wheel calibration, "trimdat")   NEVER
 *   0xC2000-0xE1FFF  deployed modules (area 2, 128 KiB)           (stock USRFLASH: see FL_STOCKTAG)
 *   0xE2000-0xF3FFF  the modules' banks, 9 x 8 KiB
 *   0xF4000-0xFDFFF  USR (stock resources, kept for the way back)  NEVER
 *   0xFE000-0xFEFFF  unused
 *   0xFF000-0xFFFFF  key_mac                                      NEVER
 * The stock firmware keeps its key / knob mapping in USRFLASH (0xC2000..) and takes it unchecked while its
 * end marker "usrdata" sits at 0xEAFF8: a write there first erases the marker's sector (dock.c), so a return
 * to the stock firmware rebuilds its factory mapping (the reference port's st_stock_release).
 * The stock VM region (0x9D000..0xBFFFF) holds the stock firmware's VM, with the radio's stored calibration
 * somewhere in it: dock.c keeps a copy of that before anything writes the region (ble/ble_vm.h). */
#pragma once
#include <stdint.h>
#include "fm1_cc.h"

/* [off, off + n) inside [lo, hi), without wrapping: off + n can overflow, and
 * the 1 MiB part ignores the high address bits, so a wrapped range lands low. */
#define FL_IN(off, n, lo, hi) ((uint32_t)(off) >= (lo) && (uint32_t)(off) <= (hi) && \
                               (uint32_t)(n) <= (hi) - (uint32_t)(off))
/* the flash chips: the FM-1's, and the SMK-37's (read from a unit over UBOOT, the reference port) */
#define FL_JEDEC_OK(id) ((id) == 0xEB6014u || (id) == 0x856014u)

#define FL_HEAD_HI      0x00004000u                /* protected: [0, FL_HEAD_HI) */
#define FL_CAL_LO       0x000C0000u                /* protected: BTIF + USRTRIM */
#define FL_CAL_HI       0x000C2000u
#define FL_USRRES_LO    0x000F4000u                /* protected: the stock USR resources */
#define FL_USRRES_HI    0x000FE000u
#define FL_KEYMAC_LO    0x000FF000u                /* protected: key_mac and above */
#define FL_APP_LO       0x00004000u                /* the update loader: the app */
#define FL_APP_HI       0x00070000u
#define FL_REC_LO       0x0009D000u                /* the update loader: where it may drop records (the stock VM) */
#define FL_REC_HI       0x000C0000u
#define FL_PLAIN_LO     FL_APP_HI                  /* the plain XIP window: what is not the app */
#define FL_OTA_LO       0x000BB000u                /* update staging (ota.c OTA_AREA) */
#define FL_OTA_HI       0x000C0000u
#define FL_USRFLASH_LO  0x000C2000u                /* the stock firmware's USRFLASH, its marker's sector */
#define FL_USRFLASH_HI  0x000EB000u
#define FL_STOCKTAG     0x000EA000u
/* a window [lo, hi) is allowed: sector aligned, non-empty, clear of every protected range */
#define FL_WIN_SAFE(lo, hi) ((lo) % 0x1000u == 0u && (hi) % 0x1000u == 0u && (lo) < (hi) && \
                             (lo) >= FL_HEAD_HI && (hi) <= FL_KEYMAC_LO && \
                             ((hi) <= FL_CAL_LO || (lo) >= FL_CAL_HI) && \
                             ((hi) <= FL_USRRES_LO || (lo) >= FL_USRRES_HI))
_Static_assert(FL_WIN_SAFE(FL_APP_LO, FL_APP_HI), "SMK: FL_APP overlaps a protected range");
_Static_assert(FL_WIN_SAFE(FL_REC_LO, FL_REC_HI), "SMK: FL_REC overlaps a protected range");
_Static_assert(FL_WIN_SAFE(FL_OTA_LO, FL_OTA_HI), "SMK: FL_OTA overlaps a protected range");
_Static_assert(FL_REC_HI == FL_CAL_LO && FL_OTA_LO >= FL_REC_LO && FL_OTA_HI <= FL_REC_HI,
               "SMK: the update record must lie in the loader's record window, below BTIF");
/* the update loader: the app, and records only in the stock VM below BTIF */
#define FL_LDR_RANGE_OK(off, n) (FL_IN(off, n, FL_APP_LO, FL_APP_HI) || FL_IN(off, n, FL_REC_LO, FL_REC_HI))
/* the second, independent net in the RAM driver: [off, off + n) touches a protected range (also when a
 * caller supplies its own FL_RANGE_OK, as the app and the update loader do) */
#define FL_HITS(off, n, lo, hi) ((uint32_t)(off) < (hi) && \
                                 ((uint32_t)(off) >= (lo) || (uint32_t)(n) > (lo) - (uint32_t)(off)))
#define FL_PROTECTED(off, n) (FL_HITS(off, n, 0u, FL_HEAD_HI) || FL_HITS(off, n, FL_CAL_LO, FL_CAL_HI) || \
                              FL_HITS(off, n, FL_USRRES_LO, FL_USRRES_HI) || (uint32_t)(off) >= FL_KEYMAC_LO || \
                              (uint32_t)(n) > FL_KEYMAC_LO - (uint32_t)(off))
