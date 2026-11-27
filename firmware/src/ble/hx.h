/* SPDX-License-Identifier: GPL-3.0-only */
/* HX: fm1-dock's BLE-MIDI, a unit of its own at a fixed place in the image (PLAN.md, "BLE-MIDI").
 * The stack is Optimist's (w0ts/optimist, firmware/src/ble/, GPL-3.0-only): written from the Core and
 * BLE-MIDI specifications, with its driver for the WL82 baseband engine. HX is hx.c around it.
 *
 * The host (dock.c) calls HX only through the header at HX_XIP, and only once the header's CRC is
 * good: a boot firmware may have written over that flash, as over the image's modules. HX may call
 * the host's code (it is always there), never the other way but through the header.
 *
 *   init(host, kept, on, failed)  once at boot, interrupts off: HX's data, the radio's stored trims
 *                                 (stock's VM, else the copy in kept), the two vectors (masked); with
 *                                 on (and the trims, and a boot that did not fail) the radio and
 *                                 advertising. -> HX_S_* of state()
 *   set(on)                       the main loop: BLUETOOTH ON / OFF (the first ON starts the radio)
 *   poll()                        TIMER5, 2 kHz: BLE MIDI in -> host->midi_in
 *   out(pkt)                      the audio ISR (usb.c midi_out_event): a USB-MIDI event packet out
 *   state()                       HX_S_*
 *   kept(rec)                     -> 1 when rec (the settings record) changed and should be saved
 *   stop()                        before flash under HX is written: the link and the interrupts off
 */
#ifndef HX_H
#define HX_H
#include <stdint.h>

#define HX_MAGIC 0x31584248u                    /* "HBX1" */
#define HX_ABI 4u                               /* 4: DEVICES (connecting out, hx_dev.c) */
#define HX_DIAG_N 6u                            /* (ABI 3) diag's words */
/* (fm1-dock) the two BLE interrupts' priority: above the audio (3) and TIMER5 (4). Stock's 2 is
 * below both: the audio interrupt (a module's render, and with two cores the wait for core 1's job) held a
 * CONNECT_IND past its transmit window (1.25 ms) and a link's stop past its wait (fm1_ble_link_stop: preempted, its
 * 3 ms ran out on the clock with the engine still busy, then advertising was programmed over it). Their bodies are
 * short but for those waits (an advertising RX poll up to ~1 ms, a link's stop up to 3 ms) and share only SPSC rings
 * with the audio (out) and TIMER5 (in) */
/* (fm1-dock) the radio's start (hal/fm1_ble_rf.h fm1_rf_cal_mode): 2 everything measured at every start (fm1-base's
 * way, Melodee's measured path: the VCO bank scan, the RF-die banks, the RX filter, TX LO and RX DC trims, the BT TX
 * trims from them; VM 187 not used); 1 stock's second start (the scan and the RF-die banks measured, VM 187 applied as
 * stock applies it); 0 the capture as it is (v0.6). HX_RF_CAL_FULL 1: also TX DC / gain / phase (fm1_rf_cal.h
 * FM1_RF_CAL_FULL). On an FM-1 (2026-10-31) the capture's start advertised but heard nothing on the data channels: its
 * per-channel VCO banks and the RF-die RX DC are the emulator's (hal/fm1_ble_rf.h); measured (2), a Mac connected
 * and played CS-1 on two cores for minutes */
#ifndef HX_RF_CAL
#define HX_RF_CAL 2u
#endif
#ifndef HX_RF_CAL_FULL
#define HX_RF_CAL_FULL 0
#endif
#ifndef HX_IRQ_PRIO
#define HX_IRQ_PRIO 5u
#endif
#define HX_AT 0x19000u                          /* flash: right after the slot table (dock.c HB_TABLE_AT), before the
                                                 * built-ins, on both boards: at most 40 KiB (to 0x23000) */
#define HX_SIZE 0x10000u                        /* (dev: the central role measured) */
#define HX_XIP (0x02000000u + HX_AT - 0x4000u)
#define HX_KEPT 112u                            /* the settings record: [0..7] the address, [8..107] the trims copy */

enum {
    HX_S_OFF = 0,                               /* BLUETOOTH OFF */
    HX_S_NOCAL = 1,                             /* ON, but no stored trims: the radio is never started */
    HX_S_HELD = 2,                              /* ON, but this boot follows a failed one: not started */
    HX_S_ADV = 3,                               /* advertising */
    HX_S_LINK = 4,                              /* a central connected */
    HX_S_READY = 5                              /* ... and listening to the MIDI notifications */
};

/* (ABI 4) DEVICES: a row of the list (hx_dev.c), the status line's tone, the device record the host keeps */
enum { HX_TAG_NONE = 0, HX_TAG_LAST = 1, HX_TAG_CONNECTING = 2, HX_TAG_CONNECTED = 3 };
enum { HX_CS_NONE = 0, HX_CS_INFO = 1, HX_CS_GOOD = 2, HX_CS_BAD = 3 };
typedef struct {
    char name[17];                              /* as advertised (terminated); row 0 "NONE" */
    uint8_t tag, chosen, bars;                  /* HX_TAG_*; NONE or LAST is the choice; signal 0..3 (0: not heard) */
} hx_dev_t;
#define HX_DEVREC 70u                           /* LAST and the choice (ble_store.h BLE_DEV_STORE_SIZE) */
#define HX_DEV_ROWS 10u                         /* NONE, LAST, 8 nearby */

typedef struct {
    int (*flash_read)(uint32_t off, void *dst, uint32_t n);   /* SPI, as stored -> 0 ok */
    int (*midi_in)(uint32_t pkt);               /* TIMER5: into the host's MIDI ring -> 1 taken */
    uint32_t board;                             /* (ABI 4) hb_module.h HB_BOARD_*: on the SMK-37 the host finds the
                                                 * radio's trims in its stock VM and gives HX the copy (kept[8..]) */
} hx_host_t;

typedef struct {
    uint32_t magic, size, crc, abi;             /* crc: CRC-32 (zlib's) of the image from byte 16 to size */
    uint32_t (*init)(const hx_host_t *h, const uint8_t kept[HX_KEPT], uint32_t on, uint32_t failed);
    void (*set)(uint32_t on);
    void (*poll)(void);
    void (*out)(uint32_t pkt);
    uint32_t (*state)(void);
    int (*kept)(uint8_t kept[HX_KEPT]);
    void (*stop)(void);
    void (*diag)(uint32_t d[HX_DIAG_N]);        /* (ABI 2) the stack's counters for the host's log: d[0] CONNECT_INDs
                                                 * seen << 24 | taken << 16 | refused << 8 | why; d[1] link closes
                                                 * << 24 | by << 20 (BDC_*) | reason << 12 | the longest BLE
                                                 * interrupt in 16 us steps (8 bits) | stops that ran out (4 bits);
                                                 * d[2] connection packets received << 16 | ours acknowledged (all
                                                 * links since the radio started); (ABI 3) d[3..5] the radio's
                                                 * start-up calibration (hx.c hx_diag) */
    void (*tick)(void);                         /* (ABI 4) the main loop: scan reports, connecting out */
    void (*scan)(uint32_t open);                /* the DEVICES page open: the radio scans */
    uint32_t (*list)(hx_dev_t *rows, uint32_t max);   /* -> how many rows */
    void (*pick)(uint32_t row);                 /* SEL on a row: NONE, LAST, or connect to a nearby device */
    uint32_t (*cstatus)(char *out, uint32_t room);    /* the status line -> HX_CS_* */
    int (*rec)(uint8_t raw[HX_DEVREC], uint32_t dir); /* dir 1: set at start-up; 0: the changed record -> 1 */
} hx_api_t;
#endif
