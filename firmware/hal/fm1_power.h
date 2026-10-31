/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio): Melodee's hal/fm1_power.h (github.com/keremimo/melodee, fm1_power_init),
 * the stock V15 supply start-up
 * Copyright (C) 2026 w0ts: Optimist's hal/fm1_supply.h (github.com/w0ts/optimist, fm1_supply_stock, fm1_lvr_stock),
 * after it
 * Copyright (C) 2026 Joakim Eriksson: brought into fm1-dock, the registers kept for the log */
/* The supplies as M-VAVE's own firmware (V15) sets them as it starts (app_core 0x02004F90..0x0200509A, the AC79 SDK's
 * p33.h names), in Optimist's form of Melodee's port: levels raised a step at a time, never lowered, stock's waits
 * (a 1,000-pass loop) 100 us here. Optimist: before it, every BLE + two-core boot crashed within 2-60 s on an FM-1;
 * after it, 3 x 180 s ran clean. fm1_supplies_stock (fm1_sys.h) raised only the two levels.
 *
 *   CON0 |= 0x04, |= 0x08, &= ~0x04   the 1.4 V rail from the LDO onto the DC-DC (LDO13_EN on, DCDC13_EN on, LDO13 off)
 *   CON1 = 0x20                       MAIN_VDD_LOAD_EN
 *   CON5 = 0x74                       VDDIO_HD_SEL, VDDIOW 3, VDDIOM 4 (3.2 V; stock's last value, written once)
 *   CON6 [4:3] = 1                    VD14_HD_SEL (its level kept)
 *   CON7 = 0xA5, CON8 = 0x96          the BT DC-DC's bias, duty, oscillator, V2I, current sense, comparator, zero
 *   CON9 |= 0x10                      SYSVDD_VOL_HD_SEL
 *   VDC14 (CON6 [2:0]) up to 7        1.60 V
 *   CON0 |= 0x80                      RC_250k_EN
 *   0x16 [5:0] = 0                    P3_ANA_KEEP
 *   CON2 |= 0x40                      VDDOK_DIS
 *   0x17 &= ~0x20                     P3_VLD_KEEP bit 5
 *   SYSVDD (CON9 [3:0]) up to 15      1.38 V, last
 *   0x11 = 0x78, |= 1                 the low-voltage detector at 2.6 V, on (Optimist's fm1_lvr_stock)
 *
 * fm1_power_stock -> 0 when the read-back is right (DC-DC on and LDO off, VDDIOM 4, VDC14 7, SYSVDD 15, the detector
 * on, no P33 timeout), else FM1_PWR_* bits. A transfer that times out stops the sequence (no field written from a
 * failed read). At power-on, before .bss is cleared (platform.c fm1_cstart), interrupts off, core 1 not started. */
#pragma once
#include <stdint.h>
#include "fm1_sys.h"
#include "fm1_time.h"

#define FM1_PWR_TIMEOUT 1u
#define FM1_PWR_MODE 2u
#define FM1_PWR_LEVEL 4u
#define FM1_PWR_LVD 8u

/* the registers it touches, as found and as left (dock.c's log): CON0 1 2 5, CON6 7 8 9, 0x16 0x17 0x11, a byte each,
 * the first in the top byte */
static const uint8_t fm1_power_reg[12] = { 0x00, 0x01, 0x02, 0x05, 0x06, 0x07, 0x08, 0x09, 0x16, 0x17, 0x11, 0xFF };
static uint32_t fm1_power_was[3] __attribute__((section(".noinit")));
static uint32_t fm1_power_now[3] __attribute__((section(".noinit")));
static uint32_t fm1_power_err __attribute__((section(".noinit")));

static void fm1_power_regs(uint32_t *w)
{
    uint32_t i;
    for (i = 0; i < 12u; i++) {
        if (!(i & 3u))
            w[i >> 2] = 0;
        if (fm1_power_reg[i] != 0xFFu)
            w[i >> 2] |= (uint32_t)fm1_p33_read(fm1_power_reg[i]) << (24u - 8u * (i & 3u));
    }
}

static void fm1_p33_rmw(uint32_t reg, uint8_t clr, uint8_t set)
{
    fm1_p33_write(reg, (uint8_t)((fm1_p33_read(reg) & ~clr) | set));
}

static uint32_t fm1_power_stock(void)
{
    static const uint8_t step[][4] = {          /* reg, clear, set, then 100 us */
        {0x00, 0x00, 0x04, 1}, {0x00, 0x00, 0x08, 1}, {0x00, 0x04, 0x00, 1},
        {0x01, 0xFF, 0x20, 0}, {0x05, 0xFF, 0x74, 0}, {0x06, 0x18, 0x08, 0},
        {0x07, 0xFF, 0xA5, 0}, {0x08, 0xFF, 0x96, 0}, {0x09, 0x00, 0x10, 0},
    };
    static const uint8_t late[][3] = { {0x00, 0x00, 0x80}, {0x16, 0x3F, 0x00}, {0x02, 0x00, 0x40}, {0x17, 0x20, 0x00} };
    uint32_t i, t0 = fm1_p33_timeouts, err = 0;
    for (i = 0; i < sizeof step / sizeof step[0]; i++) {
        fm1_p33_rmw(step[i][0], step[i][1], step[i][2]);
        if (step[i][3])
            fm1_delay_us(100);
        if (fm1_p33_timeouts != t0)
            return FM1_PWR_TIMEOUT;
    }
    fm1_supply_raise(FM1_P3_ANA_CON6, 7u, 7u);
    for (i = 0; i < sizeof late / sizeof late[0] && fm1_p33_timeouts == t0; i++)
        fm1_p33_rmw(late[i][0], late[i][1], late[i][2]);
    if (fm1_p33_timeouts != t0)
        return FM1_PWR_TIMEOUT;
    fm1_supply_raise(FM1_P3_ANA_CON9, 15u, 15u);
    fm1_delay_us(1000);
    fm1_p33_write(0x11, 0x78);                  /* the detector: 2.6 V, its flag cleared; then on */
    fm1_p33_rmw(0x11, 0x00, 0x01);
    if ((fm1_p33_read(0x00) & 0x0Cu) != 0x08u)
        err |= FM1_PWR_MODE;
    if ((fm1_p33_read(0x05) & 7u) != 4u || fm1_vdc14() != 7u || fm1_sysvdd() != 15u)
        err |= FM1_PWR_LEVEL;
    if ((fm1_p33_read(0x11) & 0x39u) != 0x39u)
        err |= FM1_PWR_LVD;
    if (fm1_p33_timeouts != t0)
        err |= FM1_PWR_TIMEOUT;
    return err;
}

/* fm1_cstart's: the registers before and after, the two levels in fm1_supplies (as fm1_supplies_stock) */
static void fm1_power_init(void)
{
    fm1_power_regs(fm1_power_was);
    fm1_supplies = fm1_vdc14() << 24 | fm1_sysvdd() << 16;
    fm1_power_err = fm1_power_stock();
    fm1_supplies |= fm1_vdc14() << 8 | fm1_sysvdd();
    fm1_power_regs(fm1_power_now);
}
