/* SPDX-License-Identifier: GPL-3.0-only */
/* fm1-dock: one firmware for the FM-1, installed once; the synths are modules in flash slots
 * (firmware/dock/hb_module.h). This file is the host, on Dinghy's platform (platform.c):
 *
 *   slots      the slot table (flash 0x18000) lists the modules; each is checked (header, ABI,
 *              CRC, sizes) before anything of it runs. The built-in sine is always there.
 *   switching  HOME opens the picker: SELECT / PRESETS move, HOME / SEL / PLAY load. A module is
 *              muted at a block boundary, released, its .data copied, .bss cleared, init(). Under the
 *              picker the module still runs (its draw(): many play their keys there), its drawing hidden.
 *   safety     OCT+ held at power-on starts the sine only. A module that crashes (or hangs into
 *              the watchdog) twice is skipped at boot until it is picked again. A module's crash
 *              does not count towards Dinghy's boot guard (the host is fine).
 *   debug      a log ring, the crash record and the render stats in NOINIT (they survive the
 *              reset): tools/dock.py log reads them from a RAM dump.
 *   playing    the keys (OCT- / OCT+ the octave), USB-MIDI and the DIN MIDI jack go to the module;
 *              the other buttons and the encoders go to its ui().
 *   settings   one flash sector: the host's (the module, the octave, the options) and a record per module;
 *              each module's bank (8 KiB, by its name) for its user presets.
 *   options    past the last module the list turns to OPTIONS: CC7 VOLUME (MIDI CC 7 sets the level of
 *              whatever module plays, on top of MASTER; OFF by default, CC 7 goes to the module as before),
 *              MASTER CC7 (the MASTER knob sends CC 7), MIDI CH (ALL, or the one channel the module hears,
 *              as channel 1, and the keys send on), LOCAL (OFF: the keys only send MIDI), EQ (its page: the
 *              master EQ, eq.c), AFTERTOUCH (the keys' pressure to the module as it comes, or as one value:
 *              channel pressure, the mod wheel or CC 74; at_route), MIDI THRU (the DIN jack's messages on to
 *              USB and BLE-MIDI) and BLUETOOTH
 *              (BLE-MIDI). All off by default.
 *   deploys    a module sent over SysEx (tools/dock.py deploy) goes into flash of its own, never
 *              the host's: received whole and checked first, the table written last (see below).
 *   keys       the bottom two rows' force is sensed (Velo's sense.c, by way of CS-1, in the key
 *              scan): they play with the velocity they land at, report their pressure while held.
 *   overload   a module that renders slower than real time for 64 blocks in a row (0.4 s or more)
 *              is stopped (the sine plays): the main loop, so USB and the updater, keep running.
 *   probes     a program sent as a deploy is, but run once from RAM (RUN) to measure or test, its
 *              result kept through a crash (RESULT): tools/probe.py. Nothing goes into flash.
 *   room       the image (the host, HX, the built-ins) ends where its JLFS says; the flash from there to the
 *              end of what an update writes is add-on room too (the image's room, IR): written encrypted
 *              with the chip key, read through the SFC as the built-ins are. An update leaves it alone
 *              (the update loader writes the package's own length) and refuses one that would cover an
 *              add-on there (ota.c ota_keep_check, ldr_core.c). */
#ifndef APP_NAME                                /* (tools/build.py --app-name: a solo app's) */
#define APP_NAME "FM1-DOCK"
#endif
#define CV_MAX (240u * 96u)
#define APP_FAULT_HOOK hb_fault
#define APP_EARLY_INIT hb_early_init
#include "fm1_cc.h"                             /* BOARD_SMK37 */
#include <stdint.h>
/* what the host may erase / program besides its settings starts at the image's end (ir_init: its room) or, until that
 * is known, at the plain window; never the image (the host, HX, the built-ins: only an update writes them) */
#if BOARD_SMK37
static volatile uint32_t hb_fl_lo = 0x70000u;   /* (HB_AREA_HI, below) */
#else
static volatile uint32_t hb_fl_lo = 0x93000u;
#endif
#define APP_FL_LO hb_fl_lo
#if BOARD_SMK37
/* the SMK-37 (hal/fm1_flash_map.h): the deployed modules, their table and the banks from the app's end up, around
 * the protected calibration (0xC0000..0xC1FFF) and below the stock resources (0xF4000) */
#define APP_FL_HI 0xF4000u
#define SETTINGS_AT 0xBA000u
#define APP_FL_KEEP_LO 0xC0000u                 /* BTIF and the factory calibration: never written (FL_PROTECTED too) */
#define APP_FL_KEEP_HI 0xC2000u
#else
#define APP_FL_HI 0xFC000u                      /* the image's room, the deployed modules, their table, the banks */
#define SETTINGS_AT 0x95000u                    /* the host's own sector (0xFC000 is Felucca's) */
#define APP_FL_KEEP_LO 0xE7000u                 /* stock's VM with the radio's factory trims (0xE8000) and BTIF: */
#define APP_FL_KEEP_HI 0xEA000u                 /* never written, whatever else the area may take */
#endif
#define APP_SYSEX hb_sysex
#define APP_MIDI_OUT ble_out                    /* BLE-MIDI (src/ble/hx.h): out from the audio ISR, in from TIMER5 */
#define APP_T5_POLL t5_poll                     /* TIMER5: the DIN jack's UART flushed, BLE-MIDI in */
#define APP_MIDI_RT rt_in                       /* USB-MIDI's clock and transport: rt_in, not the main loop's ring */
#include <stdint.h>
static void rt_in(uint32_t st);
static void ble_out(uint32_t pkt);
static void t5_poll(void);
#if !BOARD_SMK37                                /* (the SMK-37's keys have two contacts: their velocity is the scan's) */
static void hb_analog(uint32_t col, uint32_t rowbit, uint32_t v, uint32_t driven);
static int hb_analog_drive(uint32_t col, uint32_t rowbit);
#define FM1_INPUT_ANALOG(col, rowbit, v, driven) hb_analog(col, rowbit, v, driven)
#define FM1_INPUT_DRIVE(col, rowbit) hb_analog_drive(col, rowbit)
#endif
#define OTA_KEEP_ADDONS 1                       /* ota.c: an update over add-ons in the image's room is refused */
#include "platform.c"
#include "fm1_uart.h"
#include "hb_module.h"
#include "sense.c"
#include "eq.c"

/* ---- clock and transport, on time. They used to reach the module from the main loop, which goes round only
 * between screen frames: late and in bursts, so a module stepping on MIDI clock lost steps (ATEK303's 16ths from a
 * Digitakt on the DIN jack at 120 BPM came every ~177 ms instead of 125 on an FM-1). Now USB's (usb.c, APP_MIDI_RT), BLE's and the DIN jack's go into rt_q from
 * TIMER5, and the audio interrupt hands them to the module just before its render (HB_F_MIDI_RT), else on to
 * rt_late for the main loop, as before. TIMER5 skips its part while a render runs (in_audio), so the two never
 * meet; each queue has one writer and one reader */
#define RTQ 64u
static volatile uint8_t rt_q[RTQ], rt_late[RTQ];
static volatile uint32_t rt_w, rt_r, rl_w, rl_r;
static void rt_in(uint32_t st)                  /* TIMER5 */
{
    if (rt_w - rt_r < RTQ) {
        rt_q[rt_w % RTQ] = (uint8_t)st;
        RING_PUBLISH();
        rt_w++;
    }
}
static int rt_late_get(uint8_t *st_, uint8_t *d1, uint8_t *d2)   /* main loop */
{
    if (rl_r == rl_w)
        return 0;
    *st_ = rt_late[rl_r % RTQ];
    RING_PUBLISH();
    rl_r++;
    *d1 = *d2 = 0;
    return 1;
}

#define C_ACC RGB(180, 140, 255)                /* lilac, as the logo */
#define C_OFF RGB(90, 90, 90)
#define C_BAD RGB(255, 90, 70)
#define KEY0 53u                                /* key 0 = F3 */

/* the image (tools/build.py pack_slots), the same on both boards: the host from flash 0x4120, the slot table of the
 * built-ins at 0x18000, HX (BLE-MIDI, src/ble/hx.h) at 0x19000, the built-ins from the next 4 KiB after it; app.bin ends
 * with the last of them and the update writes no further (ir_init) */
#define HB_TABLE_AT 0x18000u                    /* flash offset of the image's slot table (XIP 0x02014000) */
#define HB_AREA_LO  0x19000u                    /* the image's modules (tools/build.py: after HX, from 0x19000 up) */
#if BOARD_SMK37
#define HB_AREA_HI  FL_APP_HI                   /* (the end of what an update may write: 0x70000) */
#define DT_A        0xB8000u                    /* the deployed table, two copies (deploys, below) */
#define DT_B        0xB9000u
#define DP_LO       0x70000u                    /* the deployed modules, in the plain window: two areas around */
#define DP_HI       0xB8000u                    /* the table, the update's staging and the calibration */
#define DP2_LO      0xC2000u
#define DP2_HI      0xE2000u
#define BANK_LO     0xE2000u                    /* the modules' banks, 8 KiB each, 9 as on the FM-1 */
#define BANK_HI     0xF4000u
#define DP_TOP      DP2_HI                      /* the deployed modules' end (the PC limit) */
#define DP_N        3u
static uint32_t DP_AREA[DP_N][2] = {{DP_LO, DP_HI}, {DP2_LO, DP2_HI}, {0, 0}};
#else
#define HB_AREA_HI  0x93000u                    /* (the end of what an update may write) */
#define DT_A        0x93000u                    /* the deployed table, two copies (deploys, below) */
#define DT_B        0x94000u
#define DP_LO       0x96000u                    /* the deployed modules, in the plain window */
#define DP_HI       0xE0000u
#define BANK_LO     0xEA000u                    /* the modules' banks, 8 KiB each (Felucca's USR area) */
#define BANK_HI     0xFC000u
#define DP_TOP      DP_HI
#define DP_N        2u
static uint32_t DP_AREA[DP_N][2] = {{DP_LO, DP_HI}, {0, 0}};
#endif
#define IR          (DP_N - 1u)                 /* the last area: the image's room [ir_lo, HB_AREA_HI), encrypted; ir_init */
static uint32_t ir_end;                         /* the image's end as its JLFS says (4 KiB up), 0: not known this boot */
static uint32_t ir_key;                         /* the chip key the room is written with */
static uint32_t dp_area(uint32_t off)           /* the deploy area flash offset off is in, DP_N: none */
{
    uint32_t a;
    for (a = 0; a < DP_N; a++)
        if (off >= DP_AREA[a][0] && off < DP_AREA[a][1])
            return a;
    return DP_N;
}
#define FL_OF(xip)  ((uint32_t)(xip) - FM1_XIP(0u))             /* XIP address -> flash offset */
#define BLOCK_TICKS (HALF_FRAMES * 1000000u / FS * FM1_TICKS_PER_US)   /* one block's time: 139 296 */

/* ---- what survives a reset: the log, the last crash, which module ran, its strikes */
#define NV_MAGIC 0x564E4248u                    /* "HBNV" */
#define LOG_SIZE 2048u
typedef struct {
    uint32_t magic;
    uint32_t log_w;                             /* bytes written: the ring position is log_w % LOG_SIZE */
    char log[LOG_SIZE];
    struct { uint32_t magic, vec, pc, rets, module, ms; } crash;
    uint32_t active;                            /* 1 + the module running, 0: none */
    struct { uint32_t module, last, worst, blocks, overruns; } stats;   /* render ticks per block */
    uint8_t strikes[HB_SLOTS + 1];
    struct { uint32_t magic, state, last, worst; } c1;   /* the second core (cpu1.c): a job's ticks, C1_MAGIC */
} hb_nv_t;
hb_nv_t hb_nv __attribute__((section(".noinit")));
#define CRASH_MAGIC 0x48535243u                 /* "CRSH" */
#define C1_MAGIC 0x31555043u                    /* "CPU1" */
/* the last probe's run (probes, below): it outlives the probe's crash or hang */
#define PR_MAGIC 0x52504248u                    /* "HBPR" */
enum { PR_NONE, PR_RUNNING, PR_DONE, PR_CRASHED, PR_HUNG };
typedef struct {
    uint32_t magic, id, state, ticks, pc;       /* id: the computer's for this run; ticks: run()'s, TIMER4 */
    int32_t rc;
    char name[24];
    hb_probe_ctx_t c;                           /* (c.out_n kept here: a crash leaves it) */
    uint8_t out[HB_PROBE_OUT];
} hb_pr_t;
hb_pr_t hb_probe __attribute__((section(".noinit")));

static void nv_init(void)
{
    if (hb_nv.magic == NV_MAGIC)
        return;
    memset(&hb_nv, 0, sizeof hb_nv);
    hb_nv.magic = NV_MAGIC;
}

static void log_put(const char *s)
{
    while (*s)
        hb_nv.log[hb_nv.log_w++ % LOG_SIZE] = *s++;
}
static void hb_log_u32(const char *s, uint32_t v)   /* "<ms> s v\n"; v in hex unless ~0 */
{
    char b[12];
    uint32_t i, t = fm1_ms, n = 0;
    do { b[n++] = (char)('0' + t % 10u); t /= 10u; } while (t);
    for (i = 0; i < n / 2u; i++) { char c = b[i]; b[i] = b[n - 1u - i]; b[n - 1u - i] = c; }
    b[n++] = ' ';
    b[n] = 0;
    log_put(b);
    log_put(s);
    if (v != ~0u) {
        for (i = 0; i < 8u; i++) b[1u + i] = "0123456789abcdef"[(v >> (28u - 4u * i)) & 15u];
        b[0] = ' ';
        b[9] = 0;
        log_put(b);
    }
    log_put("\n");
}
static void hb_log(const char *s) { hb_log_u32(s, ~0u); }

/* ---- CRC-32 (zlib's), in pieces */
static uint32_t crc_upd(uint32_t c, const uint8_t *p, uint32_t n)
{
    uint32_t k;
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++)
            c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

/* ---- the modules: 0 is the built-in sine, then the slot table's */
extern const hb_module_t hb_builtin_sine;       /* modules/sine/sine.c, linked into the host */
enum { M_OK, M_BAD_CRC, M_BAD_HEAD, M_TOO_BIG, M_STRUCK, M_INIT_FAILED, M_SLOW };
static const char *const M_STATE[] = {"", "BAD CRC", "BAD HEADER", "TOO BIG", "CRASHED", "INIT FAILED", "TOO SLOW"};
enum { K_BUILTIN, K_IMAGE, K_DEPLOYED, K_BOOT };   /* (K_BOOT: the boot slot's, removed; the number kept) */
typedef struct {
    const hb_file_t *file;                      /* 0: built in */
    const hb_module_t *desc;
    uint32_t key;                               /* crc of the name: its settings record, its bank */
    uint8_t state, kind;
    char name[24];
} hb_entry_t;
static hb_entry_t mods[HB_SLOTS + 1];
static uint32_t n_mods, cur;
static const hb_module_t *volatile hb_active;  /* what the audio ISR renders (0: silence) */
static volatile uint32_t hb_blocks, hb_over, hb_slow;   /* blocks; overruns in a row; stopped for them */
/* Code written where code ran: the chip keeps what it fetched from flash (its instruction cache), and the flash
 * driver's flushinv drops the data cache's lines only: after a write there the old code and the new run mixed,
 * until a reset (seen on an FM-1, #87: tests/cache_hw_test.py; iflush, by line or by word, changed nothing:
 * probes/iflush.c). So the host notes the flash sectors a module's code has run from since power-on (ran_map),
 * and a deploy that writes into one of them does not start what it wrote: the unit restarts, and starts it then. */
static uint8_t ran_map[32];                     /* a bit a 4 KiB sector of the 1 MiB */
static uint32_t restart_on, restart_at;         /* a restart asked for, and when (fm1_ms) */
static void ran_mark(uint32_t off, uint32_t n)
{
    uint32_t s;
    for (s = off >> 12; s <= (off + n - 1u) >> 12 && s < 256u; s++)
        ran_map[s >> 3] |= (uint8_t)(1u << (s & 7u));
}
static int ran_any(uint32_t off, uint32_t n)
{
    uint32_t s;
    for (s = off >> 12; s <= (off + n - 1u) >> 12 && s < 256u; s++)
        if (ran_map[s >> 3] >> (s & 7u) & 1u)
            return 1;
    return 0;
}

static uint32_t name_key(const char *s)
{
    uint32_t n = 0;
    while (n < 23u && s[n]) n++;
    return crc_upd(0, (const uint8_t *)s, n);
}

static uint32_t check_module(const hb_slot_t *sl, uint32_t lo, uint32_t hi)   /* in flash [lo, hi) */
{
    const hb_file_t *f = (const hb_file_t *)sl->at;
    uint32_t io, img, c, head = (uint32_t)((const uint8_t *)&f->crc - (const uint8_t *)f);
    if (sl->at < FM1_XIP(lo) || sl->at >= FM1_XIP(hi) || sl->size < sizeof *f || sl->size > FM1_XIP(hi) - sl->at)
        return M_BAD_HEAD;
    if (f->magic != HB_MAGIC || f->abi != HB_ABI || f->crc != sl->crc || f->n_relocs > 65536u || (f->flags & (HB_F_PROBE | HB_F_BOOT)))
        return M_BAD_HEAD;
    io = (sizeof *f + 4u * f->n_relocs + 255u) & ~255u;
    img = f->code_size + f->data_size + f->rt_size;
    if (f->base != sl->at + io || f->code_size > sl->size || img > sl->size - io ||
        f->desc + HB_DESC_MIN > f->code_size || (f->desc & 3u))
        return M_BAD_HEAD;
    if (f->data_size + f->bss_size > HB_MOD_RAM_SIZE || f->rt_size > HB_MOD_RT_SIZE)
        return M_TOO_BIG;
    c = crc_upd(0, (const uint8_t *)f, head);
    c = crc_upd(c, (const uint8_t *)(f + 1), 4u * f->n_relocs);
    c = crc_upd(c, (const uint8_t *)f->base, img);
    if (c != f->crc)
        return M_BAD_CRC;
    if (((const hb_module_t *)(f->base + f->desc))->abi != HB_ABI)
        return M_BAD_HEAD;
    if ((((const hb_module_t *)(f->base + f->desc))->flags & HB_F_DATA) && f->desc + sizeof(hb_module_t) > f->code_size)
        return M_BAD_HEAD;
    return M_OK;
}

/* one slot into the list; a deployed module that is fine stands in for the image's of the same name */
static void add_slot(const hb_slot_t *sl, uint32_t kind, uint32_t lo, uint32_t hi)
{
    hb_entry_t e;
    const hb_file_t *f = (const hb_file_t *)sl->at;
    uint32_t i, at = n_mods;
    e.state = (uint8_t)check_module(sl, lo, hi);
    e.kind = (uint8_t)kind;
    e.file = f;
    e.desc = e.state == M_OK ? (const hb_module_t *)(f->base + f->desc) : 0;
    str_cpy(e.name, e.state != M_BAD_HEAD ? f->name : "SLOT ?", sizeof e.name);
    e.key = name_key(e.name);
    if (kind == K_DEPLOYED && e.state == M_OK)
        for (i = 1; i < n_mods; i++)
            if (mods[i].kind == K_IMAGE && mods[i].key == e.key) { at = i; break; }
    if (at == n_mods) {
        if (n_mods > HB_SLOTS)
            return;
        n_mods++;
    }
    mods[at] = e;
    if (e.state == M_OK && hb_nv.strikes[at] >= 2u)
        mods[at].state = M_STRUCK;
    hb_log_u32(e.state == M_OK ? "slot ok" : "slot refused", kind << 16 | at << 8 | e.state);
    hb_log(e.name);
}

static void dt_load(void);
static uint32_t dt_write(const hb_slot_t *e, uint32_t n);
static struct { uint32_t magic, seq, n, crc; hb_slot_t s[HB_SLOTS]; } dt;   /* the deployed table in force */

static uint32_t crc_fast(uint32_t c, const uint8_t *p, uint32_t n)   /* crc_upd's, by a table: 500 KB at boot */
{
    static uint32_t t[256];
    uint32_t i, k, v;
    if (!t[1])
        for (i = 0; i < 256u; i++) {
            for (v = i, k = 0; k < 8u; k++) v = v & 1u ? (v >> 1) ^ 0xEDB88320u : v >> 1;
            t[i] = v;
        }
    c = ~c;
    while (n--) c = t[(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return ~c;
}
static void scan_slots(void)
{
    const hb_slot_t *t = (const hb_slot_t *)FM1_XIP(HB_TABLE_AT);
    uint32_t i;
    mods[0].file = 0;
    mods[0].desc = &hb_builtin_sine;
    mods[0].state = M_OK;
    mods[0].kind = K_BUILTIN;
    str_cpy(mods[0].name, hb_builtin_sine.name, sizeof mods[0].name);
    mods[0].key = name_key("(built in)");       /* its own settings, apart from a slot of the same name */
    n_mods = 1;
    dt_load();
    {   /* entries the table should no longer have: a boot firmware's (the boot slot, removed), and an add-on in the
         * image's room that the image now covers (an install by another loader wrote there: it is gone) */
        static hb_slot_t e[HB_SLOTS];
        uint32_t m, gone = 0;
        for (i = m = 0; i < dt.n; i++) {
            uint32_t at = FL_OF(dt.s[i].at);
            if (dt.s[i].magic != HB_SLOT_MAGIC || (ir_end && at >= 0x4000u && at < ir_end))
                gone++;
            else
                e[m++] = dt.s[i];
        }
        if (gone) {
            hb_log_u32("table: entries forgotten (a boot firmware, or covered by the image)", gone);
            hb_log_u32("table: written", dt_write(e, m));
            dt_load();
        }
    }
    for (i = 0; i < HB_SLOTS && t[i].magic == HB_SLOT_MAGIC; i++)
        add_slot(&t[i], K_IMAGE, HB_AREA_LO, ir_end ? ir_end : HB_AREA_HI);
    for (i = 0; i < dt.n; i++) {
        uint32_t a = dp_area(FL_OF(dt.s[i].at));
        add_slot(&dt.s[i], K_DEPLOYED, a < DP_N ? DP_AREA[a][0] : 0, a < DP_N ? DP_AREA[a][1] : 0);   /* (none: refused) */
    }
}

/* ---- settings: the host's record and one per module, in one sector; flushed when nothing plays */
#define ST_MAGIC 0x53425248u                    /* "HRBS" */
#define ST_RECS 8u
typedef struct { uint32_t key, len; uint8_t d[240]; } st_rec_t;
typedef struct {
    uint32_t magic, len, crc;                   /* crc over what follows the header */
    uint32_t sel_key;                           /* the module to start */
    int32_t oct;
    uint32_t spare[3];                          /* [0]: (was the boot slot's count, unused); [1]: OPT_*;
                                                 * [2]: the EQ (EQ_ON, the bands' dB: eq_db) */
    st_rec_t rec[ST_RECS];
} st_t;
#define OPT_CC7 1u                              /* CC7 VOLUME: the host takes MIDI CC 7 as the level (cc7_set) */
#define OPT_MASTER_CC7 2u                       /* MASTER CC7: the MASTER knob sends CC 7 (master_cc7) */
#define OPT_THRU 4u                             /* MIDI THRU: the DIN jack's channel messages on to USB and BLE */
#define OPT_LOCAL_OFF 8u                        /* LOCAL OFF: the keys send MIDI only, the module gets none of them */
#define OPT_KNOBS 16u                           /* KNOBS CC: KNOB1..4 send CCs (knobs_cc), not the module's knobs */
#define OPT_CH_SHIFT 8u                         /* MIDI CH in bits 8..12: 0 ALL, else the channel 1..16 */
#define OPT_CH_MASK (31u << OPT_CH_SHIFT)
#define OPT_AT_SHIFT 13u                        /* AFTERTOUCH in bits 13..14: AS IS, CHANNEL, MOD WHL, FILTER (at_route) */
#define OPT_AT_MASK (3u << OPT_AT_SHIFT)
#define OPT_PADS_SHIFT 15u                      /* (SMK-37) PADS in bits 15..18: NOTES, OFF, CHORDS in C .. B (2..13) */
#define OPT_PADS_MASK (15u << OPT_PADS_SHIFT)
#define PADS_N 14u
static st_t st;
static uint32_t st_dirty, st_changed;

static void store_load(void)
{
    uint32_t h[3];
    memset(&st, 0, sizeof st);
    if (flash_ok && !st_read(SETTINGS_AT, h, sizeof h) && h[0] == ST_MAGIC && h[1] == sizeof st &&
        !st_read(SETTINGS_AT, &st, sizeof st) &&
        crc_upd(0, (const uint8_t *)&st + 12, sizeof st - 12u) == st.crc)
        return;
    memset(&st, 0, sizeof st);
}
static void store_flush(void)
{
    uint32_t took;
    st.magic = ST_MAGIC;
    st.len = sizeof st;
    st.crc = crc_upd(0, (const uint8_t *)&st + 12, sizeof st - 12u);
    st_dirty = 0;
    if (!flash_ok || fl_erase4k(SETTINGS_AT, &took))
        return;
    if (!fl_write(SETTINGS_AT + 12u, (const uint8_t *)&st + 12, sizeof st - 12u))
        fl_write(SETTINGS_AT, (const uint8_t *)&st, 12u);   /* the header last: a cut save stays invalid */
    hb_log("settings saved");
}
static void store_touch(void) { st_dirty = 1; st_changed = fm1_ms; }

/* CC7 VOLUME: CC 7's value v as a level on top of MASTER, (v / 127)^2 (the MIDI spec's -40 log(127 / v) dB; the
 * MASTER knob's curve is a square too). Not kept: every boot starts at 127, as the knob alone. */
static uint32_t cc7 = 127u;
static void cc7_set(uint32_t v)
{
    cc7 = v;
    app_vol_q12 = (int32_t)((v * v * 4096u + 8064u) / 16129u);   /* 127: 4096 */
}

/* EQ: st.spare[2] bit 0 on, then a byte a band (LOW, MID, HIGH), its gain in dB as an int8; 0: off, flat */
#define EQ_ON 1u
static int32_t eq_db(uint32_t b)
{
    int32_t v = (int8_t)(st.spare[2] >> (8u + 8u * b));
    return v < -EQ_DB_MAX ? -EQ_DB_MAX : v > EQ_DB_MAX ? EQ_DB_MAX : v;
}
static void eq_apply(void)                      /* the settings to the EQ */
{
    int8_t db[EQ_BANDS];
    uint32_t b;
    for (b = 0; b < EQ_BANDS; b++)
        db[b] = (int8_t)eq_db(b);
    eq_set(st.spare[2] & EQ_ON, db);
}
static void eq_put(uint32_t b, int32_t v)       /* band b's gain, kept */
{
    v = v < -EQ_DB_MAX ? -EQ_DB_MAX : v > EQ_DB_MAX ? EQ_DB_MAX : v;
    st.spare[2] = (st.spare[2] & ~(0xFFu << (8u + 8u * b))) | ((uint32_t)v & 0xFFu) << (8u + 8u * b);
}

/* MIDI CH: 0 ALL, else the channel 1..16 */
static uint32_t opt_ch(void)
{
    uint32_t c = (st.spare[1] & OPT_CH_MASK) >> OPT_CH_SHIFT;
    return c > 16u ? 0 : c;
}

/* AFTERTOUCH: 0 AS IS (poly and channel pressure to the module as they come), else the pressure as one value, the
 * hardest-pressed key's (a poly message's) or the channel's, sent on only when it changes: 1 as channel pressure,
 * 2 as CC 1 (the mod wheel), 3 as CC 74 (the filter, brilliance on most modules). For the modules that do nothing
 * with pressure: what they have for the wheel or the cutoff follows the keys. The MIDI the keys send out stays poly. */
static uint32_t opt_at(void) { return (st.spare[1] & OPT_AT_MASK) >> OPT_AT_SHIFT; }
#if BOARD_SMK37
static uint32_t opt_pads(void) { uint32_t v = (st.spare[1] & OPT_PADS_MASK) >> OPT_PADS_SHIFT; return v < PADS_N ? v : 0u; }
#endif
static uint8_t at_poly[128], at_last;           /* each note's pressure; the value last sent */
static void at_send(const hb_module_t *m, uint32_t ch, uint32_t v)
{
    uint32_t mode = opt_at();
    if (!m || !mode || v == at_last)
        return;
    at_last = (uint8_t)v;
    if (mode == 1u) {
        if (m->midi) m->midi(0xD0u | ch, v, 0);
    } else if (m->midi) {
        m->midi(0xB0u | ch, mode == 2u ? 1u : 74u, v);
    } else if (m->cc) {
        m->cc(ch, mode == 2u ? 1u : 74u, v);
    }
}
static void at_max(const hb_module_t *m, uint32_t ch)
{
    uint32_t i, v = 0;
    for (i = 0; i < 128u; i++)
        if (at_poly[i] > v) v = at_poly[i];
    at_send(m, ch, v);
}
static void at_route(const hb_module_t *m, uint32_t s, uint32_t d1, uint32_t d2)   /* (AFTERTOUCH not AS IS) */
{
    if ((s & 0xF0u) == 0xD0u) {
        at_send(m, s & 15u, d1 & 127u);
    } else {
        at_poly[d1 & 127u] = (uint8_t)(d2 & 127u);
        at_max(m, s & 15u);
    }
}
static void at_note_off(const hb_module_t *m, uint32_t ch, uint32_t key)   /* a note let go: its pressure with it */
{
    if (!opt_at() || !at_poly[key & 127u])
        return;
    at_poly[key & 127u] = 0;
    at_max(m, ch);
}
static void at_reset(void)
{
    memset(at_poly, 0, sizeof at_poly);
    at_last = 0;
}

/* MASTER CC7: the MASTER knob as CC 7 on the keys' channel when it moves, not for where it is at power-on.
 * A value is 8 of the pot's 1024 steps; it changes 4 past its edges, so the knob's noise sends nothing. */
static uint32_t mcc7 = 128u;                    /* the value sent last; 128: none, the knob's place is taken next */
static void master_cc7(uint32_t ch)
{
    uint32_t k = master_k10;
    if (k > 1023u || fm1_ms < 500u)             /* (the readings settle first) */
        return;
    if (mcc7 > 127u) {
        mcc7 = k >> 3;
    } else if (k + 4u < mcc7 * 8u || k > mcc7 * 8u + 11u) {
        mcc7 = k >> 3;
        midi_send(0xB0u | ch, 7u, mcc7);
    }
}

/* KNOBS CC: KNOB1..4 as CC 74, 71, 73, 72 (cutoff, resonance, attack, release: MIDI's sound controllers) on the
 * keys' channel, a value each the host keeps (64 at power-on); a detent one step, turned fast four. The module
 * hears them as MIDI in (LOCAL ON; LOCAL OFF: what comes back from the computer). */
static const uint8_t KNOB_CC[4] = {74u, 71u, 73u, 72u};
static uint8_t knob_v[4] = {64u, 64u, 64u, 64u};
static uint32_t knob_ms[4];
static void knobs_cc(const hb_module_t *m, uint32_t ch, uint32_t local)
{
    uint32_t e;
    for (e = 0; e < 4u; e++) {
        int32_t c = fm1_enc_take(ENC_KNOB1 + e), v;
        if (!c)
            continue;
        v = (int32_t)knob_v[e] + (fm1_ms - knob_ms[e] < 40u ? 4 * c : c);
        knob_ms[e] = fm1_ms;
        v = v < 0 ? 0 : v > 127 ? 127 : v;
        if ((uint32_t)v == knob_v[e])
            continue;
        knob_v[e] = (uint8_t)v;
        midi_send(0xB0u | ch, KNOB_CC[e], (uint32_t)v);
        if (local && m && m->midi) m->midi(0xB0u, KNOB_CC[e], (uint32_t)v);
        else if (local && m && m->cc) m->cc(0, KNOB_CC[e], (uint32_t)v);
    }
}

#define BLE_KEY 0x454C4248u                     /* "HBLE": BLE-MIDI's record (ble_rec), never a module's key */
static int rec_load(uint32_t k, void *p, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < ST_RECS; i++)
        if (st.rec[i].key == k && st.rec[i].len == n && n <= sizeof st.rec[i].d) {
            memcpy(p, st.rec[i].d, n);
            return 0;
        }
    return -1;
}
static void rec_save(uint32_t k, const void *p, uint32_t n)
{
    uint32_t i, pick = ST_RECS;
    if (n > sizeof st.rec[0].d)
        return;
    for (i = 0; i < ST_RECS; i++)
        if (st.rec[i].key == k) { pick = i; break; }
    if (pick == ST_RECS)                        /* a free record, else the first that is not a module's here */
        for (i = 0; i < ST_RECS && pick == ST_RECS; i++) {
            uint32_t m, used = st.rec[i].key == BLE_KEY;
            for (m = 0; m < n_mods; m++)
                used |= st.rec[i].key == mods[m].key;
            if (!st.rec[i].key || !used) pick = i;
        }
    if (pick == ST_RECS)
        return;
    if (st.rec[pick].key == k && st.rec[pick].len == n && !memcmp(st.rec[pick].d, p, n))
        return;
    st.rec[pick].key = k;
    st.rec[pick].len = n;
    memcpy(st.rec[pick].d, p, n);
    store_touch();
}
static int mod_load(void *p, uint32_t n) { return rec_load(mods[cur].key, p, n); }
static void mod_save(const void *p, uint32_t n) { rec_save(mods[cur].key, p, n); }

/* ---- BLE-MIDI: HX (src/ble/hx.h, Optimist's stack) at its place in the image, called only through its
 * header and only once that header's CRC is good (a boot firmware may have written over it). OFF by
 * default; ON in the module list's BLUETOOTH line. BLE in goes into the USB-MIDI ring (TIMER5, as USB's
 * own), so a module gets it as it gets USB; what the host sends out goes to BLE too. */
#include "ble/hx.h"
#define HX_S_MISSING 0xFFu                      /* (no HX) */
static const hx_api_t *hx;
static volatile uint8_t hx_live;                /* its TIMER5 and audio ISR parts may run */
static uint8_t hx_built;                        /* the image has HX (the list's BLUETOOTH line); tools/build.py --no-ble: not */
static uint8_t ble_rec[4 + HX_KEPT];            /* the settings record: [0] ON, then HX's (its address, the trims copy) */
static int ble_flash_read(uint32_t off, void *dst, uint32_t n) { return flash_ok ? st_read(off, dst, n) : -1; }
static int ble_midi_in(uint32_t pkt)            /* TIMER5: the USB-MIDI ring's other producer, in the same ISR */
{
    uint32_t st = (pkt >> 8) & 0xFFu;
    if ((pkt & 15u) == 0xFu && (st == 0xF8u || st == 0xFAu || st == 0xFBu || st == 0xFCu)) {
        rt_in(st);                              /* clock, transport: on time (rt_q) */
        return 1;
    }
    if (mi_w - mi_r >= MQ)
        return 0;
    midi_in_q[mi_w % MQ] = pkt;
    RING_PUBLISH();
    mi_w++;
    return 1;
}
static const hx_host_t hx_host = {ble_flash_read, ble_midi_in};
static void ble_out(uint32_t pkt) { if (hx_live) hx->out(pkt); }
static void ble_poll(void) { if (hx_live) hx->poll(); }
static uint8_t ble_held;                        /* ON saved, but this boot does not start it (safe boot, a failed boot) */
static uint8_t eq_held;                         /* the EQ saved ON, but this boot does not run it (the same two cases) */
static uint32_t ble_state(void)
{
    if (!hx)
        return HX_S_MISSING;
    if (!hx_live)                               /* (nothing of HX has run yet) */
        return ble_rec[0] ? HX_S_HELD : HX_S_OFF;
    return hx->state();
}
static void ble_start(uint32_t failed)          /* HX's first code: only once BLUETOOTH is ON */
{
    fm1_irq_off();                              /* (the radio's start-up: before any BLE interrupt) */
    fm1_guard_unlock_top();                     /* HX attaches its two vectors: the write limit over the vector
                                                 * table (hal/fm1_guard.h) faulted it on an FM-1 (2026-10-25) */
    hx->init(&hx_host, ble_rec + 4, 1, failed);
    fm1_guard_lock_top();
    hx_live = 1;
    fm1_irq_on();
}

#if BOARD_SMK37
/* the SMK-37: the stock VM's region (ble/ble_vm.h) is where its deployed modules, their table, the settings and an
 * update's staging go, so the radio's trims are copied out of it at boot, before anything here writes it: HX takes
 * the copy when the VM is gone. ble_vm.c's reader, the host's own (HX's is HX's) */
#undef BLE_API
#define BLE_API static
#include "ble/ble_vm.c"
static int ble_vm_rd(void *ctx, uint32_t off, uint8_t *dst, uint32_t n) { (void)ctx; return ble_flash_read(off, dst, n); }
static void ble_capture(void)                   /* after the settings, before any flash write */
{
    struct ble_vm_info info;
    struct ble_rf_trims t;
    uint8_t *copy = ble_rec + 4 + 8;            /* (HX_KEPT: [0..7] the address, [8..107] the trims copy) */
    if (rec_load(BLE_KEY, ble_rec, sizeof ble_rec))
        memset(ble_rec, 0, sizeof ble_rec);
    if (!ble_vm_scan(ble_vm_rd, 0, &info, &t, 0, 0)) {
        hb_log_u32(ble_rf_copy_ok(copy) ? "BLE: no stock VM, the trims copy kept" : "BLE: no radio trims (stock's VM)",
                   info.area);
        return;
    }
    if (ble_rf_copy_ok(copy) && ble_eq(copy + 1, (const uint8_t *)&t, BLE_RF_TRIMS_SIZE))
        return;
    ble_rf_copy_make(copy, &t, info.area);
    rec_save(BLE_KEY, ble_rec, sizeof ble_rec);
    store_flush();                              /* (now: a deploy may write over the VM next) */
    hb_log_u32("BLE: the radio's trims kept, from", info.area);
}
#endif

static void ble_boot(uint32_t safe)             /* after the settings, the module table and the boot guard */
{
    const hx_api_t *h = (const hx_api_t *)HX_XIP;
    if (h->magic != HX_MAGIC || h->abi != HX_ABI || h->size < sizeof *h || h->size > HX_SIZE ||
        crc_fast(0, (const uint8_t *)h + 16, h->size - 16u) != h->crc) {
        hb_log(h->magic != HX_MAGIC ? "BLE: not in this build" : "BLE: HX damaged (written over?)");
        hx_built = h->magic == HX_MAGIC;        /* (damaged: the line says MISSING; none: no line) */
        return;
    }
    hx_built = 1;
    if (rec_load(BLE_KEY, ble_rec, sizeof ble_rec))
        memset(ble_rec, 0, sizeof ble_rec);
    hx = h;
    ble_held = (uint8_t)(ble_rec[0] && (bootguard.failed || safe));
    if (ble_rec[0] && !ble_held)
        ble_start(0);                           /* ON saved: HX starts now; OFF: none of its code runs */
    hb_log_u32("BLE", ble_state());
}
static void ble_set(uint32_t on)                /* the main loop */
{
    if (!hx)
        return;
    if (!hx_live && on)
        ble_start(0);                           /* (the first ON: HX's start, the radio's with it) */
    else if (hx_live)
        hx->set(on);
    hb_log_u32("BLE set", ble_state());
    ble_rec[0] = (uint8_t)on;
    rec_save(BLE_KEY, ble_rec, sizeof ble_rec);
}
static void ble_main(void)                      /* the main loop: what HX wants kept */
{
    if (hx_live && hx->kept(ble_rec + 4))
        rec_save(BLE_KEY, ble_rec, sizeof ble_rec);
}
/* the link's changes in the log, the first 16 of a boot: the state and the CONNECT_INDs (seen <<
 * 24 | taken << 16 | refused << 8 | why), and after a link the close (count << 24 | by << 20 | reason << 12 | the
 * longest BLE interrupt / 16 us << 4 | stops that ran out) and the packets received, ours acknowledged: hx.h's diag */
static void ble_watch(void)
{
    static const char *const st[6] = { "BLE off; cind", "BLE nocal; cind", "BLE held; cind", "BLE adv; cind",
                                       "BLE link; cind", "BLE ready; cind" };
    static uint32_t said = HX_S_OFF, n, cal_said;
    uint32_t s, d[HX_DIAG_N];
    if (!hx_live || n >= 16u)
        return;
    s = hx->state();
    if (s == said || s > HX_S_READY)
        return;
    n++;
    hx->diag(d);
    if (!cal_said && s >= HX_S_ADV) {           /* the radio's calibration, once: hx.c hx_diag */
        cal_said = 1;
        hb_log_u32("BLE rf cal: mode fail mid bank121 ms", d[3]);
        hb_log_u32("BLE rf cal: trim ms, BT TX 2FC10", d[4]);
        hb_log_u32("BLE rf cal: limit 0 23 46 top", d[5]);
    }
    hb_log_u32(st[s], d[0]);
    if (said == HX_S_LINK || said == HX_S_READY) {
        hb_log_u32("BLE closed; n by why isr16 stopsout", d[1]);
        hb_log_u32("BLE rx good, tx acked", d[2]);
    }
    said = s;
}
static void ble_stop(void)                      /* before the image area is written (a boot deploy): no more HX */
{
    fm1_irq_off();                              /* (no ISR between the test of hx_live and its call) */
    if (hx_live) {
        hx_live = 0;
        hx->stop();
    }
    hx = 0;
    fm1_irq_on();
}

/* ---- the keys' force: Velo's sensing (charlesvestal/fm1-velocity; sense.c as CS-1 carries it) on the
 * key scan's analog rows (hal/fm1_input.h).
 * The host's settings are CS-1's defaults at DRIVE 2; a module may set its own while it runs. */
#define SENSE_DRIVE 2u
static uint32_t sense_mask;                     /* the keys on the analog rows (PA7 / PA8): 0..19 */
#if BOARD_SMK37
/* the SMK-37: every key lands with a velocity (its two contacts, the scan's); sense.c's queue carries them, so a
 * module sees them as the FM-1's sensed keys (KEY_DOWN with the velocity), with no pressure */
static void sense_cfg_set(uint32_t on, uint32_t drive, uint32_t curve, uint32_t ceil) { (void)on; (void)drive; (void)curve; (void)ceil; }
static void sense_defaults(void) {}
static uint32_t h_sense_keys(void) { return sense_mask; }
static void smk_sense_keys(uint32_t down, uint32_t up)   /* the main loop: the keys pressed / let go since */
{
    uint32_t k;
    for (k = 0; k < 27u; k++) {
        if (up >> k & 1u) {
            sn_key[k].st = SN_OPEN;
            sn_push(EV_OFF, (uint8_t)k, 0, 0);
        }
        if (down >> k & 1u) {
            sn_key[k].st = SN_DOWN;
            sn_key[k].v = 0;                    /* (no pressure: sense_pressure reads 0, key_sense no driven reading) */
            sn_key[k].d = sn_key[k].dn = 0;
            sn_key[k].vel = (uint8_t)smk_key_vel(k);
            sn_push(EV_ON, (uint8_t)k, sn_key[k].vel, 0);
        }
    }
}
#else
static void hb_analog(uint32_t col, uint32_t rowbit, uint32_t v, uint32_t driven)   /* (the scan's ISR) */
{
    int id = FM1_KEYMAP[rowbit][col];
    if (id >= 14)
        sense_sample((uint32_t)id - 14u, v, fm1_ticks(), (int)driven);
}
static int hb_analog_drive(uint32_t col, uint32_t rowbit)
{
    int id = FM1_KEYMAP[rowbit][col];
    return id >= 14 && sense_want_drive((uint32_t)id - 14u);
}
static void sense_cfg_set(uint32_t on, uint32_t drive, uint32_t curve, uint32_t ceil)   /* main loop */
{
    drive = drive > 4u ? 4u : drive;
    sn_cfg.d_curve = (uint16_t)curve;
    sn_cfg.d_ceil = (uint16_t)ceil;
    if (drive != sn_cfg.drive) {
        sn_cfg.drive = (uint8_t)drive;
        sense_cal_reset();                      /* (the driven floor depends on it) */
    }
    fm1_an.drive = (uint8_t)drive;
    fm1_an.con = on ? (uint16_t)FM1_ADC_CON(7, 2) : 0u;   /* (CS-1's: divider 7, delay 2, as measured) */
}
static void sense_defaults(void) { sense_cfg_set(1, SENSE_DRIVE, 70, 1000); }
static uint32_t h_sense_keys(void) { return fm1_an.con ? sense_mask : 0; }
#endif
static int h_key_sense(uint32_t k, hb_key_sense_t *o)
{
    if (k >= 27u || !(h_sense_keys() >> k & 1u))
        return -1;
    o->state = sn_key[k].st;
    o->vel = sn_key[k].vel;
    o->v = sn_key[k].v;
    o->d = sn_key[k].d;
    o->dland = sn_key[k].dland;
    return 0;
}

/* ---- the service table */
static uint8_t scr_hidden;                      /* 1: the picker on screen: the module's drawing does nothing; 2: the
                                                 * host's note (save_failed) is: its drawing only, the LEDs are its own */
static uint32_t h_ms(void) { return fm1_ms; }
static uint32_t h_ticks(void) { return fm1_ticks(); }
static void h_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    if (scr_hidden)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 240) w = 240 - x;
    if (y + h > 240) h = 240 - y;
    if (w > 0 && h > 0)
        lcd_fill((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, c);
}
static void h_text(int32_t x, int32_t y, int32_t w, uint32_t big, const char *s, uint16_t fg, uint32_t center)
{
    const felucca_font_t *f = big ? &FONT_L : &FONT_S;
    if (scr_hidden || x < 0 || y < 0 || w <= 0 || x + w > 240 || y + (int32_t)f->h > 240)
        return;
    draw_text_box((uint32_t)x, (uint32_t)y, (uint32_t)w, f, s, fg, center ? 1 : 0);
}
static void h_cv_begin(int32_t w, int32_t h, uint16_t bg)
{
    if (w < 1) w = 1;
    if (w > 240) w = 240;
    if (h < 1) h = 1;
    if ((uint32_t)(w * h) > CV_MAX) h = (int32_t)(CV_MAX / (uint32_t)w);
    cv_begin((uint32_t)w, (uint32_t)h, bg);
}
static void h_cv_pixel(int32_t x, int32_t y, uint16_t c) { cv_pset(x, y, c); }
static void h_cv_text(int32_t x, int32_t y, uint32_t big, const char *s, uint16_t fg) { cv_text(x, y, big ? &FONT_L : &FONT_S, s, fg); }
static void h_cv_blit(int32_t x, int32_t y)
{
    if (!scr_hidden && x >= 0 && y >= 0 && x + (int32_t)cv_w <= 240 && y + (int32_t)cv_h <= 240)
        cv_blit((uint32_t)x, (uint32_t)y);
}
#if BOARD_SMK37
static void h_led(uint32_t id, uint32_t on)   /* the buttons' LEDs (the SMK's keys have none); the glow: off */
{
    if (scr_hidden)
        return;
    if (id < 14u)
        fm1_led_key(id, on == 1u);
    else if (id >= HB_LED_XBTN(HB_BTN_KNOB_BANK) && id <= HB_LED_XBTN(HB_BTN_RIGHT))
        fm1_led[(id & 0x7Fu) - HB_BTN_KNOB_BANK] = (uint8_t)(on == 1u);   /* (SMK LED i = button i: 0 KNOB BANK) */
}
#else
static void h_led(uint32_t id, uint32_t on)   /* on: 0 off, 1 lit, 2 the glow (hal/fm1_input.h's fm1_led_dim) */
{
    uint32_t p, r;
    if (id >= 41u || scr_hidden == 1u)          /* (under the picker: its LEDs, the module's again after) */
        return;
    fm1_led_key(id, on == 1u);
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id) {
                if (on == 2u)
                    fm1_led_dim[p] |= (uint8_t)(1u << r);
                else
                    fm1_led_dim[p] &= (uint8_t)~(1u << r);
            }
}
#endif
static uint32_t h_rand(void) { return rng(); }
static volatile uint8_t in_render;              /* the module's render runs (the audio interrupt) */
#if BOARD_SMK37
static void mod_ch_learn(uint32_t st_, uint32_t d1, uint32_t d2);
#endif
static void h_midi_send(uint32_t st_, uint32_t d1, uint32_t d2)
{
#if BOARD_SMK37
    mod_ch_learn(st_, d1, d2);
#endif
    midi_send(st_, d1, d2);
}
static uint32_t h_master(void) { return (uint32_t)master_q12; }
static void h_blit(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *px)
{
    int32_t r, k;
    if (scr_hidden || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > 240 || y + h > 240)
        return;
    for (r = 0; r < h; r += k) {                /* in strips of at most 11 520 bytes a DMA, as FM-1 SID does */
        k = h - r < 5760 / w ? h - r : 5760 / w;
        lcd_blit((uint32_t)x, (uint32_t)(y + r), (uint32_t)w, (uint32_t)k, px + r * w);
        lcd_sync();
    }
}
static int32_t h_text_w(uint32_t big, const char *s) { return text_w(big ? &FONT_L : &FONT_S, s); }
/* a module's bank: 8 KiB of the bank area, found by the module's name: [magic][key][length][crc] then
 * the bytes. A save takes the module's own bank, else a free one, else one no module here owns. */
#define BANK_MAGIC 0x324B4248u                  /* "HBK2" */
#define BANK_SIZE  0x2000u
static uint32_t bank_find(uint32_t key, uint32_t h[4])   /* its flash offset (h: its header), 0: none */
{
    uint32_t at;
    for (at = BANK_LO; at + BANK_SIZE <= BANK_HI; at += BANK_SIZE)
        if (!st_read(at, h, 16u) && h[0] == BANK_MAGIC && h[1] == key)
            return at;
    return 0;
}
static int in_mod_ram(const void *p, uint32_t n)   /* (the flash driver reads its source with the flash off) */
{
    uint32_t a = (uint32_t)p;
    return a >= HB_MOD_RAM && n <= HB_MOD_RAM_SIZE && a - HB_MOD_RAM <= HB_MOD_RAM_SIZE - n;
}
/* a module's banks beyond its first (index 1 up: hb_host_t.bank_load_n): keys of their own, derived
 * from the module's; a bank is owned when its key is a module's here or one derived from it */
#define BANK_IDX_TAG 0x5B000000u
static uint32_t bank_key(uint32_t idx) { return idx ? (mods[cur].key ^ (BANK_IDX_TAG | (idx & 0xFFFFu))) : mods[cur].key; }
static uint32_t bank_owner(uint32_t k, uint32_t *idx)   /* the module here a bank's key is of (*idx: which of its banks);
                                                          * n_mods: no module's here */
{
    uint32_t m;
    for (m = 0; m < n_mods; m++)
        if (k == mods[m].key || ((k ^ mods[m].key) & 0xFFFF0000u) == BANK_IDX_TAG) {
            *idx = k ^ mods[m].key ? (k ^ mods[m].key) & 0xFFFFu : 0;
            break;
        }
    return m;
}
static int bank_owned(uint32_t k)
{
    uint32_t i;
    return bank_owner(k, &i) < n_mods;
}
static int h_bank_load(void *p, uint32_t n)
{
    uint32_t h[4], at;
    if (!flash_ok || n > HB_BANK_MAX || !in_mod_ram(p, n) || !(at = bank_find(mods[cur].key, h)) || h[2] != n ||
        st_read(at + 16u, p, n))
        return -1;
    if (crc_upd(0, (const uint8_t *)p, n) != h[3]) {
        memset(p, 0, n);
        return -1;
    }
    hb_log_u32("bank loaded", cur);
    return 0;
}
/* A save that did not reach the flash is said by the host, on the screen and in the log (the main loop draws
 * save_note): a module's own code may show SAVED whatever the host answers, and an autosave shows nothing. Said
 * once, and again after a button: an autosave that tries every pass does not repeat it. */
static const char *save_note;                   /* its second line (0: no note) */
static uint32_t save_note_at;
static uint8_t save_note_on, save_note_armed = 1;
static const char SAVE_NO_ROOM[] = "NO ROOM FOR PRESETS", SAVE_FLASH[] = "FLASH ERROR";
static void save_failed(const char *why, uint32_t v)
{
    if (!save_note_armed)
        return;
    save_note_armed = 0;
    hb_log_u32(why == SAVE_NO_ROOM ? "bank not saved: no bank free" : "bank not saved: flash", v);
    save_note = why;
}
static int bank_check(uint32_t at, const uint32_t h[4])   /* the bank in flash is the one sent (its header, its CRC): 0 */
{
    static uint8_t b[256];
    uint32_t g[4], c = 0, i, k;
    if (st_read(at, g, 16u) || memcmp(g, h, 16u))
        return -5;
    for (i = 0; i < h[2]; i += k) {
        k = h[2] - i > sizeof b ? (uint32_t)sizeof b : h[2] - i;
        if (st_read(at + 16u + i, b, k))
            return -5;
        c = crc_upd(c, b, k);
    }
    return c == h[3] ? 0 : -5;
}
static int bank_put(uint32_t key, const void *p, uint32_t n)   /* (p in RAM, n up to HB_BANK_MAX: the caller's to see) */
{
    uint32_t h[4] = {BANK_MAGIC, key, n, 0}, g[4], at, b, spare = 0, took;
    int rc;
    if (!flash_ok) {
        save_failed(SAVE_FLASH, flash_id);
        return -1;
    }
    at = bank_find(h[1], g);
    for (b = BANK_LO; !at && b + BANK_SIZE <= BANK_HI; b += BANK_SIZE) {
        if (st_read(b, g, 16u))
            continue;
        if (g[0] != BANK_MAGIC) {
            at = b;
            break;
        }
        if (!bank_owned(g[1]) && !spare)
            spare = b;
    }
    if (!at && !(at = spare)) {
        save_failed(SAVE_NO_ROOM, key);
        return -1;
    }
    h[3] = crc_upd(0, (const uint8_t *)p, n);
    if ((rc = fl_erase4k(at, &took)) == 0 && (rc = fl_erase4k(at + 0x1000u, &took)) == 0 &&
        (rc = fl_write(at + 16u, (const uint8_t *)p, n)) == 0 &&
        (rc = fl_write(at, (const uint8_t *)h, sizeof h)) == 0)   /* the header last */
        rc = bank_check(at, h);                 /* (a chip that took the commands and kept nothing: read back) */
    if (rc) {
        save_failed(SAVE_FLASH, (uint32_t)rc);
        return rc;
    }
    hb_log_u32("bank saved", key);
    return 0;
}
static int bank_save_key(uint32_t key, const void *p, uint32_t n)   /* a module's save: from its own RAM */
{
    if (flash_ok && (n > HB_BANK_MAX || !in_mod_ram(p, n)))
        return -1;
    return bank_put(key, p, n);
}
static int h_bank_save(const void *p, uint32_t n) { return bank_save_key(bank_key(0), p, n); }
/* bank idx (1..HB_BANK_IDX_MAX): up to n of its bytes into p, their length (all of them, the CRC
 * right) or -1; n 0: only the length */
static int h_bank_load_n(uint32_t idx, void *p, uint32_t n)
{
    uint32_t h[4], at;
    if (!flash_ok || !idx || idx > HB_BANK_IDX_MAX || !(at = bank_find(bank_key(idx), h)) || h[2] > HB_BANK_MAX)
        return -1;
    if (!n)
        return (int)h[2];
    if (n < h[2] || !in_mod_ram(p, h[2]) || st_read(at + 16u, p, h[2]) || crc_upd(0, (const uint8_t *)p, h[2]) != h[3])
        return -1;
    return (int)h[2];
}
/* bank idx saved (n bytes from p), or given back (n 0: its header erased, free for anyone) */
static int h_bank_save_n(uint32_t idx, const void *p, uint32_t n)
{
    uint32_t h[4], at, took;
    if (!idx || idx > HB_BANK_IDX_MAX)
        return -1;
    if (n)
        return bank_save_key(bank_key(idx), p, n);
    if (!flash_ok || !(at = bank_find(bank_key(idx), h)))
        return 0;
    return fl_erase4k(at, &took);
}
static uint32_t h_bank_free(void)              /* banks a save could still take: unused, or no module's here */
{
    uint32_t g[4], b, k = 0;
    if (!flash_ok)
        return 0;
    for (b = BANK_LO; b + BANK_SIZE <= BANK_HI; b += BANK_SIZE)
        if (!st_read(b, g, 16u) && (g[0] != BANK_MAGIC || !bank_owned(g[1])))
            k++;
    return k;
}

/* the second core (cpu1.c), for the module's render only: a job is never out while the main loop runs
 * (flash writes, a module switch) */
static uint8_t cpu1_said;                       /* the second core's state as last logged */
static int h_c1_run(void (*fn)(uint32_t), uint32_t arg) { return in_render && cpu1_run(fn, arg); }
static int h_c1_wait(void) { return in_render ? cpu1_wait() : 0; }
static uint32_t mod_key0;                       /* the note a panel module's key 0 plays (key_base), 0: the host's */
static void h_key_base(uint32_t n) { mod_key0 = n < 128u ? n : 0u; }
#if BOARD_SMK37
/* A panel module sends its keys' notes itself, on a channel of its own (MIDI CH is the host's keys'): the outer keys
 * go out on that one too. It is the channel of the note the module sends for a middle key it was just given, which it
 * must be exactly (mod_key0: told, hb_key0); channel 1 until then. Not from the audio interrupt: a sequencer's. */
static uint32_t given_k, given_ms;              /* the key given + 1 (0: none), and when */
static uint8_t mod_ch;
static void key_given(uint32_t k) { given_k = k + 1u; given_ms = fm1_ms; }
static void mod_ch_learn(uint32_t st_, uint32_t d1, uint32_t d2)
{
    if ((st_ & 0xF0u) == 0x90u && d2 && given_k && mod_key0 && !in_render && fm1_ms - given_ms < 100u &&
        d1 == mod_key0 + given_k - 1u) {
        mod_ch = (uint8_t)(st_ & 15u);
        given_k = 0;
    }
}
#endif

static const hb_host_t HOST = {
    HB_ABI, sizeof(hb_host_t), h_ms, h_ticks, hb_log, hb_log_u32,
    h_fill, h_text, h_cv_begin, cv_rect, cv_line, h_cv_pixel, h_cv_text, h_cv_blit,
    h_led, h_rand, mod_load, mod_save, h_midi_send, h_master, h_blit, h_text_w, h_bank_load, h_bank_save,
    h_sense_keys, h_key_sense, sense_cfg_set, h_bank_load_n, h_bank_save_n, h_bank_free, h_c1_run, h_c1_wait,
    BOARD_SMK37 ? HB_BOARD_SMK37 : HB_BOARD_FM1, h_key_base,
};

/* ---- the sound: the module renders (and the EQ filters), the host measures */
static void app_audio(int32_t *out, uint32_t n)
{
    const hb_module_t *m = hb_active;
    while (rt_r != rt_w) {                      /* clock and transport since the last block (rt_q) */
        uint32_t st = rt_q[rt_r % RTQ];
        RING_PUBLISH();
        rt_r++;
        if (m && m->midi && (m->flags & HB_F_MIDI_RT))
            m->midi(st, 0, 0);
        else if (rl_w - rl_r < RTQ) {
            rt_late[rl_w % RTQ] = (uint8_t)st;
            RING_PUBLISH();
            rl_w++;
        }
    }
    cpu1_screen_begin();                        /* the second core's screening, beside the render */
    if (m) {
        uint32_t t0 = fm1_ticks(), dt;
        in_render = 1;
        m->render(out, n);
        in_render = 0;
        if (cpu1_busy())
            (void)cpu1_wait();                  /* a job the module left out: over before the main loop runs */
        eq_audio(out, n);                       /* the master EQ (eq.c): its time counts as the module's */
        dt = fm1_ticks() - t0;
        hb_nv.c1.magic = C1_MAGIC;
        hb_nv.c1.state = cpu1_state;
        hb_nv.c1.last = cpu1_job_ticks;
        hb_nv.c1.worst = cpu1_job_worst;
        hb_nv.stats.last = dt;
        if (dt > hb_nv.stats.worst) hb_nv.stats.worst = dt;
        if (dt <= BLOCK_TICKS)
            hb_over = 0;
        else if (hb_nv.stats.overruns++, ++hb_over >= 64u) {
            hb_active = 0;                      /* slower than real time 64 blocks in a row: stopped, the main loop runs again */
            hb_slow = 1;
        }
    }
    cpu1_screen_end();                          /* (not the module's time) */
    hb_nv.stats.blocks++;
    hb_blocks++;
}

/* ---- switching */
static void stop_module(void)
{
    const hb_module_t *m = hb_active;
    uint32_t b = hb_blocks, t0 = fm1_ms;
    hb_active = 0;                              /* the ISR renders silence from the next block */
    while (hb_blocks == b && fm1_ms - t0 < 50u) /* the block that may be running ends */
        fm1_wdt_feed();
    if (m && m->release)
        m->release();
    hb_nv.active = 0;
}

static int start_module(uint32_t i)
{
    hb_entry_t *e = &mods[i];
    const hb_file_t *f = e->file;
    stop_module();
    eq_clear = 1;                               /* (the EQ from silence: nothing of the last module rings on) */
#if BOARD_SMK37
    memset(fm1_led, 0, HB_BTN_RIGHT + 1u - HB_BTN_KNOB_BANK);   /* (the SMK's own buttons: only XBTN lights them) */
#endif
    if (e->state != M_OK || e->kind == K_BOOT)
        return -1;
    if (f) {
        const uint8_t *img = (const uint8_t *)f->base;
        ran_mark(FL_OF((uint32_t)f), (uint32_t)(img - (const uint8_t *)f) + f->code_size + f->data_size + f->rt_size);
        memcpy((void *)HB_MOD_RAM, img + f->code_size, f->data_size);
        memset((uint8_t *)HB_MOD_RAM + f->data_size, 0, f->bss_size);
        memcpy((void *)HB_MOD_RT, img + f->code_size + f->data_size, f->rt_size);
    }
    cur = i;
    hb_over = 0;
    sense_defaults();                           /* (what the module before set is gone) */
    mod_key0 = 0;
#if BOARD_SMK37
    mod_ch = 0;
    given_k = 0;
#endif
    hb_nv.active = i + 1u;                      /* a crash in init counts as the module's */
    hb_nv.stats.module = i;
    hb_nv.stats.worst = hb_nv.stats.overruns = 0;
    if (e->desc->init(&HOST)) {
        hb_log_u32("init failed", i);
        hb_nv.active = 0;
        e->state = M_INIT_FAILED;
        return -1;                              /* (the caller starts the sine) */
    }
    hb_log_u32("started", i);
    hb_log(e->name);
    hb_active = e->desc;
    return 0;
}

/* ---- faults: the record, a strike for the module, and the boot guard left alone */
static void hb_fault(const fm1_crash_t *c)
{
    nv_init();
    hb_nv.crash.magic = CRASH_MAGIC;
    hb_nv.crash.vec = c->vec;
    hb_nv.crash.pc = c->pc;
    hb_nv.crash.rets = c->rets;
    hb_nv.crash.module = hb_nv.active;
    hb_nv.crash.ms = fm1_ms;
    hb_log_u32("FAULT pc", c->pc);
    {
        static const char *const by[4] = { "DBG (no core's bit), EMU", "DBG core 0's, EMU", "DBG core 1's, EMU",
                                           "DBG both cores', EMU" };
        hb_log_u32(by[fm1_dbg_cores(c->dbg)], c->emu);   /* which core: DBG_MSG's bits (fm1_irq.h) */
        hb_log_u32("DBG", c->dbg);
    }
    if (cpu1_crashed())
        hb_log("with 2 cores: 1 core until power-off");
    if (hb_probe.magic == PR_MAGIC && hb_probe.state == PR_RUNNING) {
        hb_probe.state = PR_CRASHED;            /* a probe's: the host is fine */
        hb_probe.pc = c->pc;
        hb_log("in the probe");
        bootguard_clear(&bootguard);
    }
    if (hb_nv.active && hb_nv.active <= HB_SLOTS + 1u) {
        if (hb_nv.strikes[hb_nv.active - 1u] < 255u) hb_nv.strikes[hb_nv.active - 1u]++;
        hb_log_u32("in module", hb_nv.active - 1u);
        if (hb_nv.active > 1u)
            bootguard_clear(&bootguard);        /* a slot module's crash: the host itself boots (the sine's
                                                 * counts: it is the host's fallback) */
        hb_nv.active = 0;
    }
}

/* ---- MIDI from the DIN jack: UART1 DMAs into a ring filled with 0xFF (a byte MIDI never sends
 * here: 0xFF is a reset), read by content (hal/fm1_uart.h) */
#define DIN_RING 256u
static volatile uint8_t din_ring[DIN_RING] __attribute__((aligned(16)));
static uint32_t din_r;

/* the DIN jack's ring, read in TIMER5 (din_pull): its clock and transport to rt_in, the rest into din_fifo for
 * din_get in the main loop (a SysEx frame of a deploy waits there while the main loop is long) */
#define DIN_FIFO 1024u
static uint8_t din_fifo[DIN_FIFO];
static volatile uint32_t din_fw, din_fr;
static void __attribute__((unused)) din_pull(void)   /* (unused on the SMK-37: no MIDI in) */
{
    for (;;) {
        uint32_t b = din_ring[din_r];
        if (b == 0xFFu)
            return;
        din_ring[din_r] = 0xFFu;
        din_r = (din_r + 1u) % DIN_RING;
        if (b >= 0xF8u) {                       /* real time, anywhere (between a message's bytes too) */
            if (b == 0xF8u || b == 0xFAu || b == 0xFBu || b == 0xFCu)
                rt_in(b);
            continue;                           /* active sensing, reset: ignored */
        }
        if (din_fw - din_fr < DIN_FIFO) {
            din_fifo[din_fw % DIN_FIFO] = (uint8_t)b;
            RING_PUBLISH();
            din_fw++;
        }
    }
}
static void hb_early_init(void)
{
    uint32_t i;
    for (i = 0; i < DIN_RING; i++)
        din_ring[i] = 0xFFu;
#if !BOARD_SMK37                                /* (the SMK-37 has no MIDI in: PH8 is a row of its key matrix) */
    fm1_uart1_midi_init(din_ring, DIN_RING);
#endif
}

/* TIMER5's 2 kHz part. The DIN jack's UART latched (RDC) and its pendings cleared every poll, as Dinghy's
 * midi_uart.c does (fm1_uart1_rx_take): without it, notes from the jack were reported lagging badly on an
 * FM-1 where Dinghy's OberVave played them at once (fm1-emu cannot tell: its UART DMA writes each byte
 * as it lands). The ring is still read by content, in the main loop (din_get). */
static void t5_poll(void)
{
#if !BOARD_SMK37
    fm1_uart1_rx_take();
    din_pull();
#endif
    ble_poll();
}

static struct { uint8_t st, n, d[2]; } din;      /* the parser: running status, data bytes so far */
#define DF_MAX 640u                             /* a SysEx frame (the 7-bit bytes between F0 and F7), as usb.c's */
#define DF_USB_Q 4u                             /* USB frames waiting: a sender that does not wait for each answer, or
                                                 * one whose frame comes while the main loop is long (a flash write) */
static uint8_t df_usb[DF_USB_Q][DF_MAX], df_din[DF_MAX];
static uint32_t df_usb_len[DF_USB_Q], df_usb_r, df_usb_w;
#define df_usb_n (df_usb_w - df_usb_r)          /* USB frames waiting */
static uint32_t df_din_n, din_sx;               /* a DIN frame waiting: its length; din_sx: 1 + bytes so far */
static int din_get(uint8_t *st_, uint8_t *d1, uint8_t *d2)
{
    for (;;) {
        uint32_t b;
        if (df_din_n)
            return 0;                           /* a SysEx frame waits for the main loop (deploys) */
        if (din_fr == din_fw)
            return 0;
        b = din_fifo[din_fr % DIN_FIFO];
        RING_PUBLISH();
        din_fr++;
        if (b >= 0xF8u)
            continue;                           /* real time: din_pull took it (rt_in) */
        if (b == 0xF7u && din_sx > 1u)
            df_din_n = din_sx - 1u;
        if (din_sx && b < 0x80u) {
            if (din_sx <= DF_MAX)
                df_din[din_sx++ - 1u] = (uint8_t)b;
            else
                din_sx = 0;                     /* too long: not ours */
            continue;
        }
        if (b & 0x80u) {
            din_sx = b == 0xF0u;                /* SysEx: kept for the deploys */
            din.st = b < 0xF0u ? (uint8_t)b : 0;    /* system common / SysEx: end running status */
            din.n = 0;
            continue;
        }
        if (!din.st)
            continue;
        din.d[din.n++] = (uint8_t)b;
        if (din.n == ((din.st & 0xE0u) == 0xC0u ? 1u : 2u)) {   /* Cn / Dn take one data byte */
            *st_ = din.st;
            *d1 = din.d[0];
            *d2 = din.n > 1u ? din.d[1] : 0;
            din.n = 0;
            return 1;
        }
    }
}

/* ---- the screen: the host's line at the top, the picker */
static void draw_top(int32_t oct)
{
    char s[12] = "OCT +0", c[12] = "1CPU  0%";   /* (2CPU: the second core runs, or is being screened) */
    uint32_t pct = hb_nv.stats.last * 100u / BLOCK_TICKS;
    if (pct > 99u) pct = 99u;
    s[4] = oct < 0 ? '-' : '+';
    s[5] = (char)('0' + (oct < 0 ? -oct : oct));
    if (cpu1_state == CPU1_OK || cpu1_state == CPU1_SCREENING)
        c[0] = '2';
    c[5] = pct >= 10u ? (char)('0' + pct / 10u) : ' ';
    c[6] = (char)('0' + pct % 10u);
    cv_begin(240, 20, C_BLACK);
    cv_text(4, 2, &FONT_S, mods[cur].name, C_ACC);
    cv_text(236 - text_w(&FONT_S, s) - 12 - text_w(&FONT_S, c), 2, &FONT_S, c, C_OFF);
    cv_text(236 - text_w(&FONT_S, s), 2, &FONT_S, s, C_OFF);
    if (ble_state() == HX_S_LINK || ble_state() == HX_S_READY)   /* a central connected */
        cv_text(236 - text_w(&FONT_S, s) - 12 - text_w(&FONT_S, c) - 12 - text_w(&FONT_S, "BT"), 2, &FONT_S, "BT", C_ACC);
    cv_rect(0, 19, 240, 1, C_OFF);
    cv_blit(0, 0);
}

/* the list, then OPTIONS (past the last module): the host's, ABOUT, then BLUETOOTH (the last line, as it was) */
enum { O_CC7, O_MASTER_CC7, O_CH, O_LOCAL, O_EQ, O_KNOBS, O_AT, O_THRU };
#define N_OPTS_HOST 9u
#define O_PADS O_THRU                           /* (the SMK-37 has no DIN jack, so no MIDI THRU: PADS in its place) */
#define O_ABOUT (N_OPTS_HOST - 1u)              /* the host's last: its page (draw_about) */
#define N_OPTS (N_OPTS_HOST + hx_built)
#define OPT_H 16u                               /* (ten lines above the footer) */
#define OPT_Y(i) (50u + (i) * OPT_H)
static const char *const BLE_ST[] = {"OFF", "NO CAL", "HELD", "ON", "LINKED", "READY"};
static char *put_u(char *p, uint32_t v)         /* v in decimal; the end */
{
    if (v >= 10u) p = put_u(p, v / 10u);
    *p++ = (char)('0' + v % 10u);
    *p = 0;
    return p;
}
static void draw_opt_line(uint32_t o, uint32_t y, uint32_t on_it)
{
    static const char *const NAME[] = {"CC7 VOLUME", "MASTER CC7", "MIDI CH", "LOCAL", "EQ", "KNOBS CC", "AFTERTOUCH",
#if BOARD_SMK37
                                       "PADS"};
#else
                                       "MIDI THRU"};
#endif
    char v[12] = "OFF";
    uint32_t lit = 0, opt = st.spare[1];
    uint16_t col;
    cv_begin(240, OPT_H, on_it ? RGB(40, 30, 60) : C_BLACK);
    if (o >= N_OPTS_HOST) {                     /* BLUETOOTH and HX's state */
        uint32_t b = ble_state();
        cv_text(12, 2, &FONT_S, "BLUETOOTH", on_it ? C_ACC : C_WHITE);
        cv_text(150, 2, &FONT_S, b == HX_S_MISSING ? "MISSING" : BLE_ST[b],
                b == HX_S_MISSING || b == HX_S_NOCAL || b == HX_S_HELD ? C_BAD : b >= HX_S_LINK ? C_ACC : C_OFF);
        cv_blit(0, y);
        return;
    }
    if (o == O_ABOUT) v[0] = '>', v[1] = 0;     /* (SEL: its page) */
    else switch (o) {                           /* (lit: not as it is by default) */
    case O_CC7:                                 /* OFF, ON, ON 64 (a level under 127: what it is now) */
        if ((lit = opt & OPT_CC7) != 0) {
            v[0] = 'O', v[1] = 'N', v[2] = 0;
            if (cc7 < 127u) { v[2] = ' '; put_u(v + 3, cc7); }
        }
        break;
    case O_MASTER_CC7: if ((lit = opt & OPT_MASTER_CC7) != 0) v[0] = 'O', v[1] = 'N', v[2] = 0; break;
    case O_CH: if ((lit = opt_ch()) != 0) put_u(v, opt_ch()); else v[0] = 'A', v[1] = 'L', v[2] = 'L'; break;
    case O_LOCAL: if (!(lit = opt & OPT_LOCAL_OFF)) v[0] = 'O', v[1] = 'N', v[2] = 0; break;
    case O_EQ:                                  /* OFF >, ON >, FAILED (SEL: its page) */
        if (eq_failed) { memcpy(v, "FAILED", 7); break; }
        if (eq_held) { memcpy(v, "HELD", 5); break; }
        if ((lit = st.spare[2] & EQ_ON) != 0) v[0] = 'O', v[1] = 'N', v[2] = 0;
        memcpy(v + (lit ? 2 : 3), " >", 3);
        break;
    case O_KNOBS: if ((lit = opt & OPT_KNOBS) != 0) v[0] = 'O', v[1] = 'N', v[2] = 0; break;
    case O_AT: { static const char *const AT[] = {"AS IS", "CHANNEL", "MOD WHL", "FILTER"};
        const char *t = AT[lit = opt_at()];
        uint32_t i = 0;
        do v[i] = t[i]; while (t[i++]);
        break; }
#if BOARD_SMK37
    case O_PADS: {                              /* NOTES, OFF, CHORDS and their key */
        static const char KEYS[12][3] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        lit = opt_pads();
        if (!lit) memcpy(v, "NOTES", 6);
        else if (lit > 1u) { memcpy(v, "CHORDS ", 7); memcpy(v + 7, KEYS[lit - 2u], 3); }
        break; }
#else
    case O_THRU: if ((lit = opt & OPT_THRU) != 0) v[0] = 'O', v[1] = 'N', v[2] = 0; break;
#endif
    default: break;
    }
    col = o == O_EQ && eq_failed ? C_BAD : lit ? C_ACC : C_OFF;
    cv_text(12, 2, &FONT_S, o == O_ABOUT ? "ABOUT" : NAME[o], on_it ? C_ACC : C_WHITE);
    cv_text(150, 2, &FONT_S, v, col);
    cv_blit(0, y);
}

/* the EQ's page (OPTIONS, EQ): its ON line, then the bands. SELECT / PRESETS pick a line, ALGORITHM changes it
 * (KNOB1..3: a band each, on the FM-1), SEL: ON / OFF, a band back to 0 dB; HOME: OPTIONS again */
static const char *const EQ_NAME[EQ_BANDS] = {"LOW   150 Hz", "MID     1 kHz", "HIGH   6 kHz"};
static void draw_eq_page(uint32_t row)
{
    uint32_t r;
    lcd_fill(0, HB_TOP, 240, 240 - HB_TOP, C_BLACK);
    draw_text_box(0, 30, 240, &FONT_S, "EQ", C_OFF, 1);
    for (r = 0; r < 1u + EQ_BANDS; r++) {
        char d[12], *p = d;
        const char *v = d;
        uint32_t lit;
        cv_begin(240, 28, r == row ? RGB(40, 30, 60) : C_BLACK);
        if (r == 0) {
            lit = (st.spare[2] & EQ_ON) && !eq_failed && !eq_held;
            v = eq_failed ? "FAILED" : eq_held ? "HELD" : lit ? "ON" : "OFF";
            cv_text(12, 6, &FONT_S, "EQ", r == row ? C_ACC : C_WHITE);
        } else {
            int32_t g = eq_db(r - 1u);
            lit = g != 0;
            if (g) *p++ = g > 0 ? '+' : '-';
            p = put_u(p, (uint32_t)(g < 0 ? -g : g));
            memcpy(p, " dB", 4);
            cv_text(12, 6, &FONT_S, EQ_NAME[r - 1u], r == row ? C_ACC : C_WHITE);
        }
        cv_text(166, 6, &FONT_S, v, r == 0 && eq_failed ? C_BAD : lit ? C_ACC : C_OFF);
        cv_blit(0, 56u + r * 30u);
    }
#if BOARD_SMK37
    draw_text_box(0, 212, 240, &FONT_S, row ? "SCALE: 0 dB    PATCH: BACK" : "SCALE: ON/OFF   PATCH: BACK", C_OFF, 1);
#else
    draw_text_box(0, 212, 240, &FONT_S, row ? "SEL: 0 dB    HOME: BACK" : "SEL: ON/OFF   HOME: BACK", C_OFF, 1);
#endif
}
static uint32_t eq_page_input(uint32_t *row, const int32_t *en, uint32_t b)   /* 1: back to OPTIONS */
{
    int32_t mv = en[ENC_SELECT] + en[ENC_PRESETS], t = en[ENC_ALGORITHM] + en[ENC_KNOB4];
    uint32_t k, was = st.spare[2], moved = 0;
    if (b & (1u << BTN_HOME))
        return 1;
    if (mv) {
        int32_t r = (int32_t)*row + mv;
        *row = r < 0 ? 0 : r > (int32_t)EQ_BANDS ? EQ_BANDS : (uint32_t)r;
        moved = 1;
    }
    for (k = 0; k < EQ_BANDS; k++)              /* KNOB1..3: a band each */
        if (en[ENC_KNOB1 + k]) {
            eq_put(k, eq_db(k) + en[ENC_KNOB1 + k]);
            *row = k + 1u;
            moved = 1;
        }
    if (t) {
        if (*row)
            eq_put(*row - 1u, eq_db(*row - 1u) + t);
        else
            st.spare[2] ^= EQ_ON;
    }
    if (b & (1u << BTN_SEL | 1u << BTN_PLAY)) {
        if (*row)
            eq_put(*row - 1u, 0);
        else
            st.spare[2] ^= EQ_ON;
    }
    if (st.spare[2] != was) {
        if ((st.spare[2] ^ was) & ~EQ_ON)
            st.spare[2] |= EQ_ON;               /* (a band turned: the EQ on, to hear it) */
        eq_held = 0;                            /* (held this boot: the user's own change runs it) */
        eq_apply();
        store_touch();
        moved = 1;
    }
    if (moved)
        draw_eq_page(*row);
    return 0;
}

static void draw_picker(uint32_t sel)
{
    uint32_t i, first = sel > 5u ? sel - 5u : 0;
    lcd_fill(0, HB_TOP, 240, 240 - HB_TOP, C_BLACK);
    if (sel >= n_mods) {                        /* OPTIONS */
        draw_text_box(0, 30, 240, &FONT_S, "OPTIONS", C_OFF, 1);
        for (i = 0; i < N_OPTS; i++)
            draw_opt_line(i, OPT_Y(i), sel == n_mods + i);
#if BOARD_SMK37
        draw_text_box(0, 212, 240, &FONT_S, "SCALE: CHANGE    PATCH: BACK", C_OFF, 1);
#else
        draw_text_box(0, 212, 240, &FONT_S, "SEL: CHANGE    HOME: BACK", C_OFF, 1);
#endif
        return;
    }
    draw_text_box(0, 30, 240, &FONT_S, "MODULES", C_OFF, 1);
    if (first + 6u > n_mods)                    /* (room after the last module: where OPTIONS is) */
        draw_text_box(0, 62 + (n_mods - first) * 30u, 240, &FONT_S, "OPTIONS >", C_OFF, 1);
    for (i = first; i < n_mods && i < first + 6u; i++) {
        hb_entry_t *e = &mods[i];
        char sz[12] = "BUILT IN";
        uint16_t col = e->state != M_OK ? C_BAD : i == sel ? C_ACC : C_WHITE;
        if (e->file) {
            uint32_t kb = (e->file->code_size + e->file->data_size + 1023u) / 1024u, n = 0, t = kb;
            char d[6];
            do { d[n++] = (char)('0' + t % 10u); t /= 10u; } while (t);
            for (t = 0; t < n; t++) sz[t] = d[n - 1u - t];
            sz[n] = ' '; sz[n + 1u] = 'K'; sz[n + 2u] = 'B'; sz[n + 3u] = 0;
        }
        cv_begin(240, 28, i == sel ? RGB(40, 30, 60) : C_BLACK);
        cv_text(12, 6, &FONT_S, e->name, col);
        cv_text(150, 6, &FONT_S, e->state != M_OK ? M_STATE[e->state] : sz, e->state != M_OK ? C_BAD : C_OFF);
        cv_blit(0, 56 + (i - first) * 30u);
    }
}

/* the list lights SEL (the button that loads a module, or changes an option); the LEDs as they were after it */
#if BOARD_SMK37
static void sel_led(uint32_t on)
{
    static uint8_t lit, led[SMK_NLED];
    if (on == lit)
        return;
    if (on) {
        memcpy(led, fm1_led, sizeof led);
        h_led(BTN_SEL, 1);
    } else {
        memcpy(fm1_led, led, sizeof led);
    }
    lit = (uint8_t)on;
}
#else
static void sel_led(uint32_t on)
{
    static uint8_t lit, led[FM1_NCOL], dim[FM1_NCOL];
    if (on == lit)
        return;
    if (on) {
        memcpy(led, fm1_led, sizeof led);
        memcpy(dim, fm1_led_dim, sizeof dim);
        h_led(BTN_SEL, 1);
    } else {
        memcpy(fm1_led, led, sizeof led);
        memcpy(fm1_led_dim, dim, sizeof dim);
    }
    lit = (uint8_t)on;
}
#endif

static uint32_t find_key(uint32_t k)
{
    uint32_t i;
    for (i = 0; i < n_mods; i++)
        if (mods[i].key == k)
            return i;
    return 0;
}

/* ---- deploys: a module file (tools/hbmod.py's .dock) sent over SysEx, USB-MIDI or the DIN jack, into
 * flash of its own. What it may write starts at the image's end (APP_FL_LO), never the host or the image's
 * modules below it, which only an update writes (the FM-1's; the SMK-37's are above):
 *   END-0x93000        the image's room: deployed modules, encrypted with the chip key (room, ir_init)
 *   0x93000, 0x94000   the deployed table, two copies: the valid copy with the higher sequence number
 *                      counts. A new table goes into the other copy, its header last.
 *   0x96000-0xE0000    the deployed modules, each 4 KiB aligned (Dinghy's plain XIP window)
 * The module is received whole into the module RAM (the module that ran is stopped) and checked
 * there (the file's CRC, its header, its relocations); only then is flash touched: free space
 * erased, the module relocated to run there and written, read back through XIP and checked as at
 * boot, then the new table. A cut at any moment leaves the old table or the new one, and the host.
 *
 * Frames: F0 7D 48 42 cmd seq pack7(payload, CRC-32 of it) F7 (7D: non-commercial; 48 42 "HB";
 * pack7 as ota.c's). Over USB each one is answered (cmd | 0x40, the same seq, a status first);
 * over DIN nothing comes back, the sender paces itself (tools/hbdeploy.py). */
enum { D_HELLO = 0x01, D_LIST = 0x02, D_INFO = 0x03, D_BEGIN = 0x10, D_DATA = 0x11, D_COMMIT = 0x12, D_ABORT = 0x13,
       D_DELETE = 0x14, D_LOG = 0x20, D_SAFE = 0x21, D_SELECT = 0x22, D_MODULE = 0x23, D_BANKS = 0x24, D_BANK_READ = 0x25,
       D_BANK_WRITE = 0x26, D_BANK_ERASE = 0x27, D_RUN = 0x30, D_RESULT = 0x31 };
enum { D_OK, D_BAD_CRC, D_BAD_HEAD, D_TOO_BIG, D_NO_SPACE, D_FLASH, D_VERIFY, D_MISSING, D_IDLE, D_NOT_FOUND,
       D_BAD_MSG, D_REFUSED };
static const char *const D_TXT[] = {"DONE", "BAD CRC", "BAD HEADER", "TOO BIG", "NO SPACE", "FLASH ERROR",
                                    "VERIFY FAILED", "DATA MISSING", "NOT STARTED", "NOT FOUND", "BAD MESSAGE",
                                    "REFUSED"};
#define DT_MAGIC 0x54444248u                    /* "HBDT" */
#define RX_BUF ((uint8_t *)HB_MOD_RAM)
#define RX_MAX HB_MOD_RAM_SIZE                  /* (less than DP_HI - DP_LO) */
static uint32_t dt_at;                          /* the table's copy in force: DT_A / DT_B, 0: none */
static struct {
    uint32_t on, size, crc, got, t_last, t_draw, prev_key;
    char name[24];
    uint8_t have[RX_MAX / 256u / 8u];           /* the 256-byte chunks received */
} rx;

static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t dt_crc(uint32_t seq, uint32_t n, const hb_slot_t *e)
{
    uint32_t h[2] = {seq, n};
    return crc_upd(crc_upd(0, (const uint8_t *)h, 8u), (const uint8_t *)e, 16u * n);
}
static void dt_load(void)
{
    static struct { uint32_t magic, seq, n, crc; hb_slot_t s[HB_SLOTS]; } t;
    uint32_t i, at[2] = {DT_A, DT_B};
    memset(&dt, 0, sizeof dt);
    dt_at = 0;
    for (i = 0; i < 2u; i++) {
        if (!flash_ok || st_read(at[i], &t, 16u) || t.magic != DT_MAGIC || t.n > HB_SLOTS ||
            st_read(at[i] + 16u, t.s, 16u * t.n) || dt_crc(t.seq, t.n, t.s) != t.crc)
            continue;
        if (!dt_at || (int32_t)(t.seq - dt.seq) > 0) {
            memcpy(&dt, &t, sizeof dt);
            dt_at = at[i];
        }
    }
}
static uint32_t dt_write(const hb_slot_t *e, uint32_t n)   /* into the other copy, the header last */
{
    static struct { uint32_t magic, seq, n, crc; hb_slot_t s[HB_SLOTS]; } t;
    uint32_t to = dt_at == DT_A ? DT_B : DT_A, took;
    t.magic = DT_MAGIC;
    t.seq = dt.seq + 1u;
    t.n = n;
    memcpy(t.s, e, 16u * n);
    t.crc = dt_crc(t.seq, n, t.s);
    if (fl_erase4k(to, &took) || (n && fl_write(to + 16u, (const uint8_t *)t.s, 16u * n)) ||
        fl_write(to, (const uint8_t *)&t, 16u))
        return D_FLASH;
    dt_load();
    hb_log_u32("deploy: table", t.seq);
    return dt_at == to && dt.seq == t.seq ? D_OK : D_VERIFY;
}
static uint32_t dt_find(uint32_t key)          /* the table's entry of that name, HB_SLOTS: none */
{
    uint32_t i;
    for (i = 0; i < dt.n; i++) {
        const hb_slot_t *e = &dt.s[i];
        if (dp_area(FL_OF(e->at)) < DP_N && name_key(((const hb_file_t *)e->at)->name) == key)
            return i;
    }
    return HB_SLOTS;
}
static uint32_t dp_room(uint32_t at)            /* the free bytes from at (4 KiB aligned) to the next module */
{
    uint32_t i, end = DP_AREA[dp_area(at) % DP_N][1];
    for (i = 0; i < dt.n; i++) {
        uint32_t lo = FL_OF(dt.s[i].at), hi = lo + ((dt.s[i].size + 0xFFFu) & ~0xFFFu);
        if (at >= lo && at < hi)
            return 0;
        if (lo >= at && lo < end)
            end = lo;
    }
    return end - at;
}
/* where a module of need bytes goes, 0: nowhere; *largest, *total: the free room. The areas in the plain window first,
 * each first fit from its start (no update ever writes there); the image's room last and from its top down, so that
 * what is there stays as far as it can from the image's end (a package with more built-ins covers less) */
static uint32_t dp_place(uint32_t need, uint32_t *largest, uint32_t *total)
{
    uint32_t i, a, at = 0, at_a = DP_N, r;
    *largest = *total = 0;
    for (a = 0; a < DP_N; a++)
        for (i = 0; i <= dt.n; i++) {           /* the gaps start at an area's start or where a module ends */
            uint32_t s0 = i < dt.n ? FL_OF(dt.s[i].at) + ((dt.s[i].size + 0xFFFu) & ~0xFFFu) : DP_AREA[a][0];
            if (DP_AREA[a][0] >= DP_AREA[a][1] || dp_area(s0) != a || !(r = dp_room(s0)))
                continue;
            if (r > *largest) *largest = r;
            *total += r;
            if (r < need || a > at_a)
                continue;
            if (a == IR) {                      /* (the gap's top) */
                if (at_a != IR || s0 + r - need > at) { at = s0 + r - need; at_a = a; }
            } else if (a < at_a || s0 < at) {
                at = s0;
                at_a = a;
            }
        }
    return at;
}
static uint32_t dp_size(void)                   /* every area's bytes: the room for add-ons in all */
{
    uint32_t a, n = 0;
    for (a = 0; a < DP_N; a++)
        n += DP_AREA[a][1] > DP_AREA[a][0] ? DP_AREA[a][1] - DP_AREA[a][0] : 0u;
    return n;
}

/* OPTIONS, ABOUT and INFO (0x03): what runs here, from what is in RAM (the flash's JEDEC id was read at boot;
 * nothing is read or written for them). info_fill gathers it for both: the build, the board, the chip, the flash, the
 * RAM, the add-ons' room, the radio's address. A record, format 1, grown only at its end (docs/index.html) */
#ifdef APP_BUILD
#define AB_VER APP_VER " (" APP_BUILD ")"       /* a release: X.Y and its source */
#else
#define APP_BUILD APP_VER                       /* any other build: its source (tools/build.py git_build) */
#define AB_VER APP_VER
#endif
#if BOARD_SMK37
#define AB_BOARD SMK_MODEL
#define AB_KEYS "K2: SCROLL    PATCH: BACK"
#else
#define AB_BOARD "FM-1"
#define AB_KEYS "SELECT: SCROLL    HOME: BACK"
#endif
enum { IN_FMT = 1, IN_BOARD, IN_CPU1, IN_RST, IN_BT, IN_ADDR, IN_FLASH = 12, IN_FREE = 16, IN_BIG = 20, IN_RAM = 24,
       IN_USED = 28, IN_MHZ = 32, IN_SOC = 36, IN_VER = 44, IN_BUILD = 60, IN_MODEL = 72, IN_ROOM = 88, IN_END = 92,
       IN_LEN = 96 };                           /* (IN_ROOM: every add-on area's bytes; IN_END: the image's end, 0 unknown) */
typedef char in_fits[sizeof APP_VER <= 16u && sizeof APP_BUILD <= 12u && sizeof AB_BOARD <= 16u ? 1 : -1];
static char *put_s(char *p, const char *s) { while ((*p = *s++) != 0) p++; return p; }
static void info_fill(uint8_t *b)               /* [0] the status, then the record (IN_*) */
{
    uint32_t big, all;
    const hb_file_t *f = mods[cur].file;
    memset(b, 0, IN_LEN);
    dp_place(~0u, &big, &all);
    b[IN_FMT] = 1;
    b[IN_BOARD] = BOARD_SMK37;
    b[IN_CPU1] = cpu1_state;                    /* (cpu1.c CPU1_*: 3 OK and 5 screening are two cores) */
    b[IN_RST] = fm1_boot.p3_rst;
    if (ble_rec[4 + 6] == 0xA5u) {              /* HX's random static address, once the radio made it (hx.c ADDR_KEPT) */
        b[IN_BT] = 1;
        memcpy(b + IN_ADDR, ble_rec + 4, 6);
    }
    wr32(b + IN_FLASH, flash_id);
    wr32(b + IN_FREE, all);
    wr32(b + IN_BIG, big);
    wr32(b + IN_RAM, HB_MOD_RAM_SIZE);
    wr32(b + IN_USED, f ? f->data_size + f->bss_size : 0);
    wr32(b + IN_MHZ, 360u);                     /* (as the boot loader sets it: fm1_sys.h) */
    put_s((char *)b + IN_SOC, "AC791N");
    put_s((char *)b + IN_VER, APP_VER);
    put_s((char *)b + IN_BUILD, APP_BUILD);
    put_s((char *)b + IN_MODEL, AB_BOARD);
    wr32(b + IN_ROOM, dp_size());
    wr32(b + IN_END, ir_end);
}

/* ABOUT's page: the version, then info_fill's, then each module (name, vN, KiB; built in: the sine). SELECT /
 * PRESETS scroll, HOME: OPTIONS again */
#define AB_FIXED 7u                             /* the lines above the modules */
#define AB_ROWS 10u                             /* on the screen at once, 18 px each */
static char *put_x(char *p, uint32_t v, uint32_t n) { while (n--) *p++ = "0123456789ABCDEF"[v >> 4u * n & 15u]; *p = 0; return p; }
static char *put_kib(char *p, const char *s, uint32_t v) { return put_u(put_s(p, s), v >> 10); }
static void draw_about(uint32_t top)
{
    uint8_t in[IN_LEN];
    uint32_t i, k, id;
    info_fill(in);
    id = rd32(in + IN_FLASH);
    lcd_fill(0, HB_TOP, 240, 240 - HB_TOP, C_BLACK);
    for (i = 0; i < AB_ROWS && top + i < AB_FIXED + n_mods; i++) {
        char s[40], *p = s;
        switch (top + i) {
        case 0: put_s(p, "fm1-dock " AB_VER); break;
        case 1:
            p = put_s(p, AB_BOARD "  1CPU  reset ");
            if (in[IN_CPU1] == CPU1_OK || in[IN_CPU1] == CPU1_SCREENING)   /* (as the top line has it) */
                p[-12] = '2';
            put_x(p, in[IN_RST], 2);
            break;
        case 2: put_s(p, "AC791N (WL82) 360 MHz"); break;
        case 3:                                 /* the id's last byte: log2 of the size (0x14: 1 MiB) */
            p = put_x(put_s(p, "flash "), id, 6);
            if ((id & 0xFFu) - 16u < 11u)
                put_s(put_kib(p, ", ", 1u << (id & 0xFFu)), " KiB");
            break;
        case 4: put_s(put_kib(put_s(put_kib(p, "module RAM ", HB_MOD_RAM_SIZE), " KiB, "), "", rd32(in + IN_USED) + 1023u), " used"); break;
        case 5: put_s(put_kib(put_s(put_kib(p, "add-ons ", rd32(in + IN_FREE)), " KiB free, "), "", rd32(in + IN_BIG)), " max"); break;
        case 6:
            p = put_s(p, "BT ");
            if (!in[IN_BT]) { put_s(p, "-"); break; }
            for (k = 6; k--; ) {
                p = put_x(p, in[IN_ADDR + k], 2);
                if (k) *p++ = ':';
            }
            break;
        default: {
            const hb_entry_t *m = &mods[top + i - AB_FIXED];
            memset(s, ' ', 22);
            for (k = 0; k < 13u && m->name[k]; k++)
                s[k] = m->name[k];
            if (!m->file) { put_s(s + 15, "built in"); break; }
            p = put_u(s + 15, m->file->version);
            s[14] = 'v';
            *p = ' ';
            put_s(put_kib(s + 20, "", m->file->code_size + m->file->data_size + 1023u), " KiB");
        }
        }
        draw_text_box(4, 28u + i * 18u, 232, &FONT_S, s, top + i ? C_OFF : C_ACC, 0);
    }
    draw_text_box(0, 212, 240, &FONT_S, AB_KEYS, C_OFF, 1);
}
static uint32_t about_input(uint32_t *top, const int32_t *en, uint32_t b)   /* 1: back to OPTIONS */
{
    int32_t t = (int32_t)*top + en[ENC_SELECT] + en[ENC_PRESETS], end = (int32_t)(AB_FIXED + n_mods - AB_ROWS);
    if (b & (1u << BTN_HOME))
        return 1;
    if (t > end) t = end;
    if (t < 0) t = 0;
    if ((uint32_t)t != *top)
        draw_about(*top = (uint32_t)t);
    return 0;
}

static void rx_draw(const char *what, uint16_t col)
{
    uint32_t w = rx.size ? rx.got * 256u * 200u / rx.size : 0;
    if (w > 200u) w = 200u;
    cv_begin(240, 96, C_BLACK);
    cv_text(120 - text_w(&FONT_S, what) / 2, 4, &FONT_S, what, col);
    cv_text(120 - text_w(&FONT_S, rx.name) / 2, 32, &FONT_S, rx.name, C_ACC);
    cv_rect(20, 68, 200, 8, C_OFF);
    cv_rect(20, 68, (int32_t)w, 8, C_ACC);
    cv_blit(0, 80);
}

/* the received file into flash; *key: its name's */
static uint32_t d_commit(uint32_t *key)
{
    static hb_slot_t e[HB_SLOTS];
    hb_file_t *f = (hb_file_t *)RX_BUF;
    const uint32_t *rel = (const uint32_t *)(RX_BUF + sizeof *f);
    uint8_t *img;
    uint32_t io, isz, total, need, at, i, n, old, rc, big, all, d, took, a;
    hb_slot_t sl;
    if (rx.got != (rx.size + 255u) / 256u)
        return D_MISSING;
    if (crc_upd(0, RX_BUF, rx.size) != rx.crc)
        return D_BAD_CRC;
    if (f->magic != HB_MAGIC || f->abi != HB_ABI || f->n_relocs > 65536u || f->code_size > RX_MAX ||
        f->data_size > RX_MAX || f->rt_size > RX_MAX || (f->flags & (HB_F_PROBE | HB_F_BOOT)))
        return D_BAD_HEAD;                      /* (a probe is run, never written: RUN) */
    io = (sizeof *f + 4u * f->n_relocs + 255u) & ~255u;
    isz = f->code_size + f->data_size + f->rt_size;
    total = io + isz;
    if (total > rx.size || isz < 4u || f->desc + HB_DESC_MIN > f->code_size || (f->desc & 3u))
        return D_BAD_HEAD;
    if (f->data_size + f->bss_size > HB_MOD_RAM_SIZE || f->rt_size > HB_MOD_RT_SIZE)
        return D_TOO_BIG;
    img = RX_BUF + io;
    if (crc_upd(crc_upd(crc_upd(0, RX_BUF, (uint32_t)((uint8_t *)&f->crc - RX_BUF)), (const uint8_t *)rel,
                        4u * f->n_relocs), img, isz) != f->crc)
        return D_BAD_CRC;
    for (i = 0; i < f->n_relocs; i++)
        if (rel[i] > isz - 4u)
            return D_BAD_HEAD;
    *key = name_key(f->name);
    old = dt_find(*key);
    if (old == HB_SLOTS && dt.n >= HB_SLOTS)
        return D_NO_SPACE;
    need = (total + 0xFFFu) & ~0xFFFu;
    at = dp_place(need, &big, &all);
    if (!at && old < dt.n) {                    /* no room beside the old copy: the table without it first */
        for (i = n = 0; i < dt.n; i++)
            if (i != old) e[n++] = dt.s[i];
        if ((rc = dt_write(e, n)) != D_OK)
            return rc;
        hb_log("deploy: the old copy dropped first");
        old = HB_SLOTS;
        at = dp_place(need, &big, &all);
    }
    if (!at)
        return D_NO_SPACE;
    rx_draw("WRITING", C_WHITE);
    d = FM1_XIP(at) + io - f->base;             /* relocated to run there (as hbmod.move) */
    for (i = 0; i < f->n_relocs; i++)
        wr32(img + rel[i], rd32(img + rel[i]) + d);
    f->base = FM1_XIP(at) + io;
    f->crc = crc_upd(crc_upd(crc_upd(0, RX_BUF, (uint32_t)((uint8_t *)&f->crc - RX_BUF)), (const uint8_t *)rel,
                             4u * f->n_relocs), img, isz);
    for (i = 0; i < need; i += 0x1000u) {
        if (fl_erase4k(at + i, &took))
            return D_FLASH;
        fm1_wdt_feed();
    }
    a = dp_area(at);
    for (i = 0; i < total; i += 0x1000u) {     /* (the image's room: encrypted as an update writes, its own inverse) */
        uint32_t m = total - i < 0x1000u ? total - i : 0x1000u;
        if (a == IR) ota_sfc(RX_BUF + i, m, at + i - 0x4000u, ir_key);
        rc = (uint32_t)fl_write(at + i, RX_BUF + i, m);
        if (a == IR) ota_sfc(RX_BUF + i, m, at + i - 0x4000u, ir_key);
        if (rc)
            return D_FLASH;
        fm1_wdt_feed();
    }
    sl.magic = HB_SLOT_MAGIC;
    sl.at = FM1_XIP(at);
    sl.size = total;
    sl.crc = f->crc;
    if (fl_verify_xip(at, RX_BUF, total) || check_module(&sl, DP_AREA[a][0], DP_AREA[a][1]) != M_OK)
        return D_VERIFY;                        /* (read back through XIP: decrypted by the SFC in the room) */
    for (i = n = 0; i < dt.n; i++)              /* the new one in the old one's place, else at the end */
        e[n++] = i == old ? sl : dt.s[i];
    if (old == HB_SLOTS)
        e[n++] = sl;
    hb_log_u32("deploy: written at", sl.at);
    if (ran_any(at, need))
        restart_on = 1;                         /* (code ran from there since power-on: not started before a restart) */
    return dt_write(e, n);
}

/* after the table changed: the list again, and a module running (the one asked for, else the sine) */
static void d_restart(uint32_t key)
{
    uint32_t k;
    stop_module();
    memset(hb_nv.strikes, 0, sizeof hb_nv.strikes);   /* (by place in the list, which may have moved) */
    scan_slots();
    k = find_key(key);
    if (start_module(k))
        start_module(0);
    if (mods[cur].key != st.sel_key) {
        st.sel_key = mods[cur].key;
        store_touch();
    }
}
static void rx_end(uint32_t status)
{
    rx.on = 0;
    if (status != D_OK) {
        uint32_t t0 = fm1_ms;
        hb_log_u32("deploy failed", status);
        rx_draw(D_TXT[status < 11u ? status : 10u], C_BAD);
        while (fm1_ms - t0 < 1500u)
            fm1_wdt_feed();
    }
}

/* ---- the image's room (room, above): its end from its own JLFS (as ota.c reads a package's), the chip key that writes
 * it, its area. Read once at boot, before the table is scanned; anything that does not add up: no room this boot (the
 * add-ons there are listed as refused, not forgotten) */
static int ota_chip_key(uint32_t *key)          /* (ota.c's hook) as the loader's ldr_chip_key: the isd_config blob */
{
    uint8_t e[32], blob[34];
    uint32_t off, i, sum, k;
    for (off = 32; off < 0x400u; off += 32u) {
        if (st_read(off, e, 32))
            return -1;
        ota_jl_enc(e, 32);
        if (ota_crc16(e + 2, 30, 0) != ota_rd16(e))
            return -2;
        if (memcmp(e + 16, "isd_config.ini", 15)) {
            if (ota_rd16(e + 14))
                return -3;
            continue;
        }
        if (st_read(rd32(e + 4), blob, 34) || ota_crc16(blob, 32, 0) != ota_rd16(blob + 32))
            return -4;
        for (i = 0, sum = 0; i < 16u; i++)
            sum += blob[i];
        sum &= 0xFFu;
        sum = sum >= 0xE0u ? 0xAAu : sum <= 0x10u ? 0x55u : sum;
        for (i = 0, k = 0; i < 16u; i++)
            if ((uint32_t)(blob[16 + i] ^ blob[15 - i]) < sum)
                k |= 1u << i;
        *key = k;
        return 0;
    }
    return -3;
}
static int ir_rd(uint32_t off, uint8_t *p, uint32_t n) { return st_read(0x4000u + off, p, n); }
static void ir_init(void)
{
    const hb_slot_t *t = (const hb_slot_t *)FM1_XIP(HB_TABLE_AT);
    uint32_t end, i;
    ir_end = 0;
    hb_fl_lo = HB_AREA_HI;
    DP_AREA[IR][0] = DP_AREA[IR][1] = 0;
    if (!flash_ok || ota_chip_key(&ir_key) || ota_area_end(ir_rd, ir_key, &end)) {
        hb_log("room: the image's end not known: no room in it this boot");
        return;
    }
    for (i = 0; i < HB_SLOTS && t[i].magic == HB_SLOT_MAGIC; i++)   /* (the built-ins are inside it) */
        if (FL_OF(t[i].at) + t[i].size > end)
            end = 0;
    if (end < HB_TABLE_AT + 0x1000u || end > HB_AREA_HI) {
        hb_log_u32("room: the image's end does not add up", end);
        return;
    }
    ir_end = end;
    hb_fl_lo = end;                             /* (the flash driver's net: nothing below the room) */
    DP_AREA[IR][0] = end;
    DP_AREA[IR][1] = HB_AREA_HI;
    hb_log_u32("room: the image ends at", end);
}

/* ---- probes (hb_module.h, tools/probe.py): received as a deploy is, then RUN instead of COMMIT:
 * relocated where it lies in RX_BUF, its .data / .bss at HB_PROBE_RAM, run once in the main loop.
 * Nothing is written to flash. The result (NOINIT) outlives a crash or the watchdog: RESULT. */
static void h_wdt_feed(void) { fm1_wdt_feed(); }

/* The PC limits (hal/fm1_guard.h; Dinghy's startup arms them to the host's own code and .ram_text):
 * no FM-1 runs an instruction fetched outside both windows (the CPU exception, DBG_MSG bit 12: a
 * probe from RAM faulted on its first instructions on a real FM-1, 2026-10-24; fm1-emu has the
 * guard since). So they cover what fm1-dock runs besides itself: window 0 the XIP up to the deployed
 * modules' end (DP_HI), window 1 RAM code from .ram_text through the modules' (HB_MOD_RT) to ram_hi:
 * PC_RAM_HI, or while a probe runs its image in RX_BUF. */
#define PC_RAM_HI (HB_MOD_RT + HB_MOD_RT_SIZE - 1u)
static void hb_pc_limits(uint32_t ram_hi)
{
    fm1_guard_pc(0, FM1_XIP(DP_TOP) - 1u, (uint32_t)(uintptr_t)_rt_start, ram_hi);
}

static uint32_t pr_run(uint32_t id, const uint8_t *arg, uint32_t argn)
{
    static uint8_t a[HB_PROBE_ARG];
    hb_file_t *f = (hb_file_t *)RX_BUF;
    const uint32_t *rel = (const uint32_t *)(RX_BUF + sizeof *f);
    const hb_probe_t *p;
    uint8_t *img;
    uint32_t io, isz, i, d, t0;
    if (rx.got != (rx.size + 255u) / 256u)
        return D_MISSING;
    if (crc_upd(0, RX_BUF, rx.size) != rx.crc)
        return D_BAD_CRC;
    if (f->magic != HB_MAGIC || f->abi != HB_ABI || !(f->flags & HB_F_PROBE) || f->n_relocs > 65536u ||
        f->code_size > HB_PROBE_FILE || f->data_size > HB_PROBE_FILE || f->rt_size > HB_PROBE_FILE || argn > HB_PROBE_ARG)
        return D_BAD_HEAD;
    io = (sizeof *f + 4u * f->n_relocs + 255u) & ~255u;
    isz = f->code_size + f->data_size + f->rt_size;
    if (io + isz > rx.size || isz < 4u || f->desc + sizeof(hb_probe_t) > f->code_size || (f->desc & 3u))
        return D_BAD_HEAD;
    if (io + isz > HB_PROBE_FILE || f->data_size + f->bss_size > HB_PROBE_RAM_SIZE || f->rt_size > HB_MOD_RT_SIZE)
        return D_TOO_BIG;
    img = RX_BUF + io;
    if (crc_upd(crc_upd(crc_upd(0, RX_BUF, (uint32_t)((uint8_t *)&f->crc - RX_BUF)), (const uint8_t *)rel,
                        4u * f->n_relocs), img, isz) != f->crc)
        return D_BAD_CRC;
    for (i = 0; i < f->n_relocs; i++)
        if (rel[i] > isz - 4u)
            return D_BAD_HEAD;
    memcpy(a, arg, argn);
    d = (uint32_t)img - f->base;                /* to run where it lies */
    for (i = 0; i < f->n_relocs; i++)
        wr32(img + rel[i], rd32(img + rel[i]) + d);
    memcpy((void *)HB_PROBE_RAM, img + f->code_size, f->data_size);
    memset((uint8_t *)HB_PROBE_RAM + f->data_size, 0, f->bss_size);
    memcpy((void *)HB_MOD_RT, img + f->code_size + f->data_size, f->rt_size);
    fl_csync();                                 /* (the code was just written: no stale fetch) */
    p = (const hb_probe_t *)(img + f->desc);
    if (p->magic != HB_PROBE_MAGIC)
        return D_BAD_HEAD;
    memset(&hb_probe, 0, sizeof hb_probe - sizeof hb_probe.out);
    hb_probe.magic = PR_MAGIC;
    hb_probe.id = id;
    str_cpy(hb_probe.name, f->name, sizeof hb_probe.name);
    hb_probe.c.host = &HOST;
    hb_probe.c.arg = a;
    hb_probe.c.arg_n = argn;
    hb_probe.c.out = hb_probe.out;
    hb_probe.c.wdt_feed = h_wdt_feed;
    hb_probe.c.ticks_per_us = FM1_TICKS_PER_US;
    hb_log_u32("probe: run", id);
    hb_log(hb_probe.name);
    hb_probe.state = PR_RUNNING;
    fm1_wdt_feed();
    hb_pc_limits(HB_MOD_RAM + HB_MOD_RAM_SIZE - 1u);   /* (a probe may run code anywhere in its RAM: EXEC runs
                                                 * the instructions it was sent from its .bss) */
    t0 = fm1_ticks();
    hb_probe.rc = p->run(&hb_probe.c);
    hb_probe.ticks = fm1_ticks() - t0;
    hb_pc_limits(PC_RAM_HI);
    hb_probe.state = PR_DONE;
    hb_log_u32("probe: done", (uint32_t)hb_probe.rc);
    return D_OK;
}
static uint32_t pr_out_n(void) { return hb_probe.c.out_n < HB_PROBE_OUT ? hb_probe.c.out_n : HB_PROBE_OUT; }
static uint32_t pr_head(uint8_t *r)             /* the result's summary, r[1..27]: state, id, rc, ticks, out_n, pc */
{
    r[1] = (uint8_t)(hb_probe.magic == PR_MAGIC ? hb_probe.state : PR_NONE);
    wr32(r + 4, hb_probe.id); wr32(r + 8, (uint32_t)hb_probe.rc); wr32(r + 12, hb_probe.ticks);
    wr32(r + 16, pr_out_n()); wr32(r + 20, hb_probe.pc);
    return 24;
}

static int hb_sysex(const uint8_t *p, uint32_t n)   /* platform_poll: a USB frame, kept for the main loop */
{
    if (n < 5u || n > DF_MAX || p[0] != 0x7Du || p[1] != 0x48u || p[2] != 0x42u || df_usb_n >= DF_USB_Q)
        return 0;
    memcpy(df_usb[df_usb_w % DF_USB_Q], p, n);
    df_usb_len[df_usb_w % DF_USB_Q] = n;
    df_usb_w++;
    return 1;
}

static uint8_t d_msg[DF_MAX], d_out[DF_MAX];
static struct { uint32_t on, key, len, crc, have; } bw;   /* a bank on its way in (D_BANK_WRITE): the 256-byte chunks had */
static uint8_t bw_buf[HB_BANK_MAX];             /* (in RAM whole before the flash is touched: all of it, or nothing) */
static void d_reply(uint32_t cmd, uint32_t seq, const uint8_t *p, uint32_t n)
{
    static uint8_t b[264];
    uint32_t w;
    if (!usb.config || n > 260u)
        return;
    memcpy(b, p, n);
    wr32(b + n, crc_upd(0, p, n));
    d_out[0] = 0xF0; d_out[1] = 0x7D; d_out[2] = 0x48; d_out[3] = 0x42;
    d_out[4] = (uint8_t)(cmd | 0x40u);
    d_out[5] = (uint8_t)(seq & 0x7Fu);
    w = 6u + ota_pack7(b, n + 4u, d_out + 6);
    d_out[w++] = 0xF7;
    ota_wire_send(d_out, w);
}

/* the frame waiting (USB first); 1: the module or the screen changed */
static uint32_t deploy_frame(void)
{
    uint32_t usb_src = df_usb_n != 0, n = usb_src ? df_usb_len[df_usb_r % DF_USB_Q] : df_din_n, len, cmd, seq, i, k, ch = 0;
    const uint8_t *p = usb_src ? df_usb[df_usb_r % DF_USB_Q] : df_din;
    uint8_t r[68];
    uint32_t rn = 1;
    char name[24];
    memset(r, 0, sizeof r);
    cmd = p[3];
    seq = p[4];
    len = ota_unpack7(p + 5, n - 5u, d_msg, sizeof d_msg);
    if (len < 4u || len > sizeof d_msg || crc_upd(0, d_msg, len - 4u) != rd32(d_msg + len - 4u)) {
        r[0] = D_BAD_MSG;
        cmd = 0x3Fu;
    } else {
        len -= 4u;
    }
    if (usb_src) df_usb_r++; else df_din_n = 0;   /* (unpacked into d_msg: its slot is free) */
    if (cmd == D_BEGIN || cmd == D_DELETE || cmd == D_SELECT) {
        for (i = 0; i < 23u; i++) name[i] = i + (cmd == D_BEGIN ? 8u : 0u) < len ? (char)d_msg[i + (cmd == D_BEGIN ? 8u : 0u)] : 0;
        name[23] = 0;
    }
    switch (cmd) {
    case D_HELLO: {
        uint32_t big, all;
        dp_place(~0u, &big, &all);
        r[1] = 1; r[2] = HB_ABI; r[3] = (uint8_t)n_mods; r[4] = (uint8_t)cur; r[5] = (uint8_t)rx.on;
        wr32(r + 8, big); wr32(r + 12, all); wr32(r + 16, RX_MAX);
        wr32(r + 20, hb_nv.crash.magic == CRASH_MAGIC ? hb_nv.crash.pc : 0); wr32(r + 24, hb_nv.crash.module);
        wr32(r + 28, hb_nv.stats.worst); wr32(r + 32, hb_nv.stats.overruns); wr32(r + 36, hb_nv.log_w);
        wr32(r + 40, BLOCK_TICKS);
        r[44] = cpu1_state; r[45] = (uint8_t)(cpu1_timeouts > 255u ? 255u : cpu1_timeouts);   /* the second core */
        wr32(r + 48, cpu1_job_ticks); wr32(r + 52, cpu1_job_worst); wr32(r + 56, cpu1_stack_free());
        wr32(r + 60, dp_size()); wr32(r + 64, ir_end);   /* every area's bytes (free + largest's whole); the image's end */
        rn = 68;
        break;
    }
    case D_INFO: {                              /* the unit and the build: ABOUT's facts (info_fill) */
        uint8_t b[IN_LEN];
        info_fill(b);
        if (usb_src) d_reply(cmd, seq, b, IN_LEN);
        return 0;
    }
    case D_LIST:                                /* one reply per module */
        for (i = 0; i < n_mods; i++) {
            memset(r, 0, sizeof r);
            r[1] = (uint8_t)i; r[2] = mods[i].state; r[3] = mods[i].kind;
            if (mods[i].file) {
                wr32(r + 4, mods[i].file->version);
                wr32(r + 8, mods[i].file->code_size + mods[i].file->data_size);
                wr32(r + 12, (uint32_t)mods[i].file);
            }
            memcpy(r + 16, mods[i].name, 24);
            d_reply(cmd, seq, r, 40);
        }
        r[0] = D_OK; r[1] = 0xFFu;              /* the end */
        rn = 2;
        break;
    case D_BEGIN: {
        uint32_t size = len >= 8u ? rd32(d_msg) : 0;
        if (len < 32u) { r[0] = D_BAD_MSG; break; }
        if (size < sizeof(hb_file_t) + 4u || size > RX_MAX) { r[0] = D_TOO_BIG; break; }
        k = rx.on ? rx.prev_key : mods[cur].key;   /* (a BEGIN again: the same module to go back to) */
        stop_module();
        memset(&rx, 0, sizeof rx);
        rx.prev_key = k;
        rx.on = 1;
        rx.size = size;
        rx.crc = rd32(d_msg + 4);
        rx.t_last = fm1_ms;
        str_cpy(rx.name, name, sizeof rx.name);
        hb_log_u32("deploy: receiving", size);
        hb_log(rx.name);
        lcd_fill(0, 0, 240, 240, C_BLACK);
        rx_draw("RECEIVING", C_WHITE);
        ch = 1;
        break;
    }
    case D_DATA: {
        uint32_t off = len >= 4u ? rd32(d_msg) : ~0u, want;
        wr32(r + 4, off);
        rn = 8;
        if (!rx.on) { r[0] = D_IDLE; break; }
        want = off < rx.size ? (rx.size - off < 256u ? rx.size - off : 256u) : 0;
        if ((off & 255u) || !want || len - 4u != want) { r[0] = D_BAD_MSG; break; }
        if (rx.size > RX_MAX) { r[0] = D_TOO_BIG; break; }
        memcpy(RX_BUF + off, d_msg + 4, want);
        if (!(rx.have[off >> 11] >> (off >> 8 & 7u) & 1u)) {
            rx.have[off >> 11] |= (uint8_t)(1u << (off >> 8 & 7u));
            rx.got++;
        }
        rx.t_last = fm1_ms;
        break;
    }
    case D_COMMIT: {
        uint32_t key = 0, st_ = !rx.on ? D_IDLE : d_commit(&key);
        r[0] = (uint8_t)st_;
        if (rx.on) {
            uint32_t back = st_ == D_OK ? key : rx.prev_key;
            if (st_ == D_OK) hb_log("deploy: done");
            rx_end(st_);
            if (st_ == D_OK && restart_on) {    /* written where code ran: started after a restart, not now (ran_map) */
                hb_log("deploy: code ran from that flash: restarting");
                st.sel_key = key;
                store_flush();
                rx_draw("RESTARTING", C_WHITE);
                restart_at = fm1_ms;
                memset(hb_nv.strikes, 0, sizeof hb_nv.strikes);   /* (by place in the list, which may have moved: as d_restart) */
                r[2] = 1;                       /* (the computer waits for the unit to come back) */
                rn = 8;
                break;                          /* (the screen stays as it is until then) */
            }
            restart_on = 0;                     /* (a commit that failed after its write: nothing new to start) */
            d_restart(back);
            wr32(r + 4, mods[cur].file ? (uint32_t)mods[cur].file : 0);
            r[1] = (uint8_t)cur;
            rn = 8;
            ch = 1;
        }
        break;
    }
    case D_ABORT:
        if (rx.on) {
            hb_log("deploy: aborted");
            rx.on = 0;
            d_restart(rx.prev_key);
            ch = 1;
        }
        break;
    case D_DELETE:
        if (rx.on) { r[0] = D_IDLE; break; }
        k = dt_find(name_key(name));
        if (k == HB_SLOTS) { r[0] = D_NOT_FOUND; break; }
        {
            static hb_slot_t e[HB_SLOTS];
            uint32_t m, keep = mods[cur].key;
            for (i = m = 0; i < dt.n; i++)
                if (i != k) e[m++] = dt.s[i];
            stop_module();
            r[0] = (uint8_t)dt_write(e, m);
            hb_log("deploy: deleted");
            hb_log(name);
            d_restart(keep);
            ch = 1;
        }
        break;
    case D_LOG: {                               /* the log ring from a byte count on: [log_w][from] bytes */
        static uint8_t b[200];
        uint32_t from = len >= 4u ? rd32(d_msg) : 0, w = hb_nv.log_w, m = 0;
        if (w - from > LOG_SIZE) from = w > LOG_SIZE ? w - LOG_SIZE : 0;
        if (from > w) from = w;
        b[0] = D_OK;
        wr32(b + 4, w);
        wr32(b + 8, from);
        while (from + m < w && m < 180u) {
            b[12 + m] = (uint8_t)hb_nv.log[(from + m) % LOG_SIZE];
            m++;
        }
        if (usb_src) d_reply(cmd, seq, b, 12u + m);
        return 0;
    }
    case D_RUN: {                               /* [id u32][argument]: the probe received, run */
        if (!rx.on) { r[0] = D_IDLE; break; }
        r[0] = (uint8_t)pr_run(len >= 4u ? rd32(d_msg) : 0, d_msg + 4, len >= 4u ? len - 4u : 0);
        rx.on = 0;
        if (r[0] != D_OK) hb_log_u32("probe failed", r[0]);
        rn = pr_head(r);
        if (start_module(find_key(rx.prev_key))) start_module(0);
        ch = 1;
        break;
    }
    case D_RESULT: {                            /* [from u32]: the summary, then out from there (180 B) */
        static uint8_t b[208];
        uint32_t from = len >= 4u ? rd32(d_msg) : 0, m = 0, on = pr_out_n();
        memset(b, 0, 28);
        b[0] = D_OK;
        pr_head(b);
        wr32(b + 24, from);
        while (from + m < on && m < 180u) {
            b[28 + m] = hb_probe.out[from + m];
            m++;
        }
        if (usb_src) d_reply(cmd, seq, b, 28u + m);
        return 0;
    }
    case D_SAFE:                                /* the sine, from the computer */
    case D_SELECT:
        if (rx.on) { r[0] = D_IDLE; break; }
        k = cmd == D_SAFE ? 0 : find_key(name_key(name));
        if (cmd == D_SELECT && !k) {
            r[0] = D_NOT_FOUND;
            break;
        }
        if (mods[k].state == M_STRUCK || mods[k].state == M_SLOW) {
            hb_nv.strikes[k] = 0;
            mods[k].state = M_OK;
        }
        if (start_module(k)) start_module(0);
        if (cmd == D_SELECT) { st.sel_key = mods[cur].key; store_touch(); }
        r[1] = (uint8_t)cur;
        rn = 2;
        ch = 1;
        break;
    case D_MODULE: {                            /* [payload]: to the module playing (its data(): a tune for FM-1 SID) */
        const hb_module_t *m = hb_active;
        int rc;
        if (rx.on || !m || !(m->flags & HB_F_DATA) || !m->data) { r[0] = D_NOT_FOUND; break; }
        if (len > HB_DATA_MAX) { r[0] = D_TOO_BIG; break; }
        rc = m->data(d_msg, len);
        r[0] = rc ? D_REFUSED : D_OK;
        r[1] = (uint8_t)rc;
        r[2] = (uint8_t)cur;
        rn = 3;
        break;
    }
    /* The banks from the computer (the page's Presets, tools/dock.py banks / bank-get / bank-put): a backup, a
     * restore, a preset file from another unit. The host carries the bytes and does not read them: every module's
     * bank is its own format. A bank is the module's whose name its key is made of, so a file lands on the same
     * synth on any unit. */
    case D_BANKS: {                             /* one reply per bank in use, then the end: [n banks][free] */
        uint32_t h[4], at, idx = 0;
        for (at = BANK_LO; flash_ok && at + BANK_SIZE <= BANK_HI; at += BANK_SIZE) {
            if (st_read(at, h, 16u) || h[0] != BANK_MAGIC || h[2] > HB_BANK_MAX)
                continue;
            memset(r, 0, sizeof r);
            k = bank_owner(h[1], &idx);
            r[1] = (uint8_t)((at - BANK_LO) / BANK_SIZE);
            r[2] = (uint8_t)(k < n_mods ? k : 0xFFu);   /* (0xFF: of no module here; its idx not known) */
            wr32(r + 4, h[1]); wr32(r + 8, h[2]); wr32(r + 12, h[3]); wr32(r + 16, k < n_mods ? idx : 0);
            if (k < n_mods) memcpy(r + 20, mods[k].name, 24);
            d_reply(cmd, seq, r, 44);
        }
        memset(r, 0, sizeof r);
        r[1] = 0xFFu; r[2] = (uint8_t)((BANK_HI - BANK_LO) / BANK_SIZE); r[3] = (uint8_t)h_bank_free();
        rn = 4;
        break;
    }
    case D_BANK_READ: {                         /* [slot u8][0 0 0][from u32]: [from u32] and up to 240 of its bytes */
        static uint8_t b[248];
        uint32_t h[4], at = BANK_LO + (len >= 8u ? d_msg[0] : 0xFFu) * BANK_SIZE, from = len >= 8u ? rd32(d_msg + 4) : 0;
        if (len < 8u) { r[0] = D_BAD_MSG; break; }
        if (!flash_ok || at + BANK_SIZE > BANK_HI || st_read(at, h, 16u) || h[0] != BANK_MAGIC || h[2] > HB_BANK_MAX ||
            from >= h[2]) { r[0] = D_NOT_FOUND; break; }
        k = h[2] - from < 240u ? h[2] - from : 240u;
        if (st_read(at + 16u + from, b + 8, k)) { r[0] = D_FLASH; break; }
        b[0] = D_OK; b[1] = d_msg[0]; b[2] = b[3] = 0;
        wr32(b + 4, from);
        if (usb_src) d_reply(cmd, seq, b, 8u + k);
        return 0;
    }
    case D_BANK_WRITE: {                        /* B [0][idx u16][n u32][crc u32][name 24], D [0 0 0][at u32][bytes], E */
        uint32_t op = len ? d_msg[0] : 0, idx = 0;
        r[1] = (uint8_t)op;
        rn = 2;
        if (op == 'B') {
            bw.on = 0;
            if (rx.on) { r[0] = D_IDLE; break; }
            if (len < 36u || (d_msg[2] | (uint32_t)d_msg[3] << 8) > HB_BANK_IDX_MAX) { r[0] = D_BAD_MSG; break; }
            bw.len = rd32(d_msg + 4);
            if (!bw.len || bw.len > HB_BANK_MAX) { r[0] = D_TOO_BIG; break; }
            for (i = 0; i < 23u; i++) name[i] = (char)d_msg[12u + i];
            name[23] = 0;
            idx = d_msg[2] | (uint32_t)d_msg[3] << 8;
            bw.key = name_key(name) ^ (idx ? BANK_IDX_TAG | idx : 0);
            if (!find_key(name_key(name))) { r[0] = D_NOT_FOUND; break; }   /* (a bank of no module here would be anyone's to take) */
            bw.crc = rd32(d_msg + 8);
            bw.have = 0;
            bw.on = 1;
        } else if (op == 'D') {
            uint32_t at = len >= 8u ? rd32(d_msg + 4) : ~0u, want;
            wr32(r + 4, at);
            rn = 8;
            if (!bw.on) { r[0] = D_IDLE; break; }
            want = at < bw.len ? (bw.len - at < 256u ? bw.len - at : 256u) : 0;
            if ((at & 255u) || !want || len - 8u != want) { r[0] = D_BAD_MSG; break; }
            memcpy(bw_buf + at, d_msg + 8, want);
            bw.have |= 1u << (at >> 8);
        } else if (op == 'E') {
            uint32_t all = (bw.len + 255u) >> 8, own;
            int rc;
            if (!bw.on || rx.on) { r[0] = D_IDLE; break; }
            bw.on = 0;
            if (bw.have != (all >= 32u ? ~0u : (1u << all) - 1u)) { r[0] = D_MISSING; break; }
            if (crc_upd(0, bw_buf, bw.len) != bw.crc) { r[0] = D_BAD_CRC; break; }
            own = cur && bank_owner(bw.key, &idx) == cur;
            if (own) stop_module();             /* (before: its release may save; after: it loads the new bank) */
            rc = bank_put(bw.key, bw_buf, bw.len);
            r[0] = (uint8_t)(!rc ? D_OK : rc == -1 && flash_ok ? D_NO_SPACE : D_FLASH);
            if (own) {
                if (start_module(cur)) start_module(0);
                ch = 1;
            }
        } else {
            r[0] = D_BAD_MSG;
        }
        break;
    }
    case D_BANK_ERASE: {                        /* [slot u8][0 0 0][key u32]: that bank given back (the key: it is still that one) */
        uint32_t h[4], at = BANK_LO + (len >= 8u ? d_msg[0] : 0xFFu) * BANK_SIZE, idx = 0, own, took;
        if (len < 8u) { r[0] = D_BAD_MSG; break; }
        if (rx.on) { r[0] = D_IDLE; break; }
        if (!flash_ok || at + BANK_SIZE > BANK_HI || st_read(at, h, 16u) || h[0] != BANK_MAGIC || h[1] != rd32(d_msg + 4)) {
            r[0] = D_NOT_FOUND;
            break;
        }
        own = cur && bank_owner(h[1], &idx) == cur;
        if (own) stop_module();
        if (fl_erase4k(at, &took)) r[0] = D_FLASH;
        hb_log_u32("bank erased", h[1]);
        if (own) {
            if (start_module(cur)) start_module(0);
            ch = 1;
        }
        break;
    }
    default:
        if (!r[0]) r[0] = D_BAD_MSG;
        break;
    }
    if (usb_src && cmd == D_COMMIT) {           /* (the flash writes kept USB waiting: if it has to come back, first) */
        uint32_t t0 = fm1_ms;
        while (!*(volatile uint8_t *)&usb.config && fm1_ms - t0 < 1000u)
            fm1_wdt_feed();
    }
    if (usb_src)
        d_reply(cmd, seq, r, rn);
    return ch;
}

static uint32_t deploy_tick(void)               /* the main loop's: a frame, the progress, the time-out */
{
    uint32_t ch = 0;
    if (df_usb_n || df_din_n)
        ch = deploy_frame();
    if (rx.on && fm1_ms - rx.t_last > 15000u) {
        hb_log("deploy: timed out");
        rx_end(D_MISSING);
        d_restart(rx.prev_key);
        ch = 1;
    } else if (rx.on && fm1_ms - rx.t_draw > 200u) {
        rx.t_draw = fm1_ms;
        rx_draw("RECEIVING", C_WHITE);
    }
    return ch;
}

/* OCT- held as fm1-dock starts (its first 600 ms, the keys scanned by the timer): the USB rescue,
 * where the platform's 50 ms look at power-on missed it. Only the updater from here on. */
static void rescue_late(void)
{
    hb_log("OCT- held at power-on: the USB rescue");
    fm1_audio_stop();
    fm1_irq_off();
    usb_detach();
    fm1_delay_ms(30);
    recovery_main();
}

static void play_note(uint32_t ch, uint32_t key, uint32_t vel)
{
    const hb_module_t *m = hb_active;
    if (m) m->note(ch, key, vel);
    if (!vel) at_note_off(m, ch, key);
}

#if BOARD_SMK37
/* ---- the SMK-37's extras, as MIDI to the module and out over USB (as the keys' notes): the 10 outer keys extend
 * the keyboard (the notes below F3 and above G5, with the octave), the 16 pads play notes 36..51 on channel 10
 * (the bottom row 36..43, the top row 44..51, left to right) with their velocity, the faders send CC 70..73, the
 * mod wheel CC 1 and the pitch wheel the bend, on channel 1. The wheels calibrate themselves as the reference
 * port's do (its smk_wheel.h, not taught): the pitch wheel's first reading is its centre, which follows a slow
 * drift while it rests, and its ends widen to what it reaches; the mod wheel's bottom and top likewise. */
#define SMK_PAD_NOTE(p) ((p) < 8u ? 44u + (p) : 36u + (p) - 8u)
static uint8_t smk_outer[SMK_NKEY], smk_outer_ch[SMK_NKEY], smk_fad[4], smk_mod, smk_live;
static int32_t smk_bend;
static struct { int32_t pc, plo, phi; uint32_t drift_ms; } smk_pw;
static struct { int32_t lo, hi; } smk_mw;

static uint8_t smk_local = 1, smk_ch;           /* LOCAL, and the channel the keys send on, this pass */
static void smk_send_ch(const hb_module_t *m, uint32_t st_, uint32_t d1, uint32_t d2, uint32_t ch)
{                                               /* to the module on channel 1 (a pad's: 10), out on ch */
    if (!smk_local) {
        /* LOCAL OFF: MIDI only, as the middle keys (#53, #61, #88) */
    } else if (m && m->midi) {
        m->midi(st_, d1, d2);
    } else if ((st_ & 0xF0u) == 0x90u || (st_ & 0xF0u) == 0x80u) {
        play_note(st_ & 15u, d1, (st_ & 0xF0u) == 0x90u ? d2 : 0u);
    } else if ((st_ & 0xF0u) == 0xB0u && m && m->cc) {
        m->cc(st_ & 15u, d1, d2);
    }
    midi_send((st_ & 15u) == 9u ? st_ : st_ | ch, d1, d2);   /* (the pads keep channel 10) */
}
static void smk_send(const hb_module_t *m, uint32_t st_, uint32_t d1, uint32_t d2) { smk_send_ch(m, st_, d1, d2, smk_ch); }
static void smk_outer_up(const hb_module_t *m, uint32_t code)   /* what the key started, where it started it */
{
    if (smk_outer[code])
        smk_send_ch(m, 0x80u, smk_outer[code], 0, smk_outer_ch[code]);
    smk_outer[code] = 0;
}
static void smk_let_go(const hb_module_t *m)    /* before LOCAL or MIDI CH changes: what is held ends as it began */
{
    uint32_t i;
    for (i = 0; i < SMK_NKEY; i++)
        smk_outer_up(m, i);
}
static int32_t smk_pitch(int32_t p, uint32_t now)   /* the pitch wheel's reading -> the bend, -8192..8191 */
{
    int32_t d, b, up, dn;
    if (!smk_pw.pc) {
        smk_pw.pc = p ? p : 1;
        smk_pw.plo = p - 80;
        smk_pw.phi = p + 80;
        smk_pw.drift_ms = now;
    }
    if (p < smk_pw.plo) smk_pw.plo = p;
    if (p > smk_pw.phi) smk_pw.phi = p;
    d = p - smk_pw.pc;
    if (d > -12 && d < 12) {                    /* resting: no bend, the centre follows a slow drift */
        if (now - smk_pw.drift_ms >= 250u) {
            smk_pw.pc += d > 0 ? 1 : d < 0 ? -1 : 0;
            smk_pw.drift_ms = now;
        }
        return 0;
    }
    smk_pw.drift_ms = now;
    up = smk_pw.phi - smk_pw.pc - 12;
    dn = smk_pw.pc - smk_pw.plo - 12;
    b = d > 0 ? (d - 12) * 8191 / (up > 1 ? up : 1) : (d + 12) * 8192 / (dn > 1 ? dn : 1);
    return b < -8192 ? -8192 : b > 8191 ? 8191 : b;
}
static uint32_t smk_modval(int32_t v)          /* the mod wheel's reading (up reads lower) -> 0..127 */
{
    int32_t span, r;
    if (!smk_mw.hi) {
        smk_mw.lo = v < 485 ? v : 485;
        smk_mw.hi = v > 615 ? v : 615;
    }
    if (v < smk_mw.lo) smk_mw.lo = v;
    if (v > smk_mw.hi) smk_mw.hi = v;
    span = smk_mw.hi - smk_mw.lo - 12;
    r = smk_mw.hi - v <= 12 ? 0 : (smk_mw.hi - v - 12) * 127 / (span > 1 ? span : 1);
    return r < 0 ? 0u : r > 127 ? 127u : (uint32_t)r;
}
/* PADS (OPTIONS): NOTES, as above; OFF; or CHORDS in a major key: the bottom row its seven triads and the first an
 * octave up, the top row the same with their sevenths, on the keys' channel and in their octave (a C major's first is
 * C3 E3 G3 where the keyboard's F3 is its key 0). What a pad struck is kept, so it lets go of that whatever has been
 * changed since; a note two held pads share sounds until both are up. */
static struct { uint8_t n, st, note[4]; } smk_pad[16];
static uint8_t smk_pad_cnt[128], smk_pad_ch[128], smk_pads_shown = 255;   /* how many pads hold a note; its last note-on's channel */
static void smk_pad_idle(uint32_t p)            /* a pad at rest: dark, or in CHORDS dim (a key's first chords brighter) */
{
    uint32_t c = opt_pads() > 1u, first = !(p & 7u) || (p & 7u) == 7u;
    padled_set(p, 0, c ? (p < 8u ? 14u : 4u) * (first ? 2u : 1u) : 0u, c ? 14u * (first ? 2u : 1u) : 0u);
}
static void smk_pad_up(const hb_module_t *m, uint32_t p)
{
    uint32_t i;
    for (i = 0; i < smk_pad[p].n; i++) {
        uint32_t d = smk_pad[p].note[i];
        if (smk_pad[p].st == 0x99u)
            smk_send(m, 0x89u, d, 0);
        else if (smk_pad_cnt[d] && --smk_pad_cnt[d] == 0)
            smk_send_ch(m, 0x80u, d, 0, smk_pad_ch[d]);
    }
    smk_pad[p].n = 0;
    smk_pad_idle(p);
}
static void smk_pad_hit(const hb_module_t *m, uint32_t p, uint32_t vel, uint32_t base)
{
    static const uint8_t SCALE[7] = {0, 2, 4, 5, 7, 9, 11};
    uint32_t mode = opt_pads(), i, deg = p & 7u, n = p < 8u ? 4u : 3u;
    smk_pad_up(m, p);                           /* (struck again without a let-go) */
    if (mode == 1u)
        return;
    padled_set(p, (uint8_t)(180u * vel / 127u), (uint8_t)(140u * vel / 127u), (uint8_t)(255u * vel / 127u));
    if (!mode) {
        smk_pad[p].st = 0x99u;
        smk_pad[p].note[smk_pad[p].n++] = (uint8_t)SMK_PAD_NOTE(p);
        smk_send(m, 0x99u, SMK_PAD_NOTE(p), vel);
        return;
    }
    smk_pad[p].st = 0x90u;
    for (i = 0; i < n; i++) {                   /* the chord's thirds up the scale */
        uint32_t s7 = deg + 2u * i, d = 48u + (mode - 2u) + base - KEY0 + SCALE[s7 % 7u] + 12u * (s7 / 7u);
        if (d > 127u)
            continue;
        smk_pad[p].note[smk_pad[p].n++] = (uint8_t)d;
        if (smk_pad_cnt[d]++)                   /* (another pad's too: ended first, struck again; two note-ons and one
                                                 * note-off would leave a synth that counts them a voice: FM-106) */
            smk_send_ch(m, 0x80u, d, 0, smk_pad_ch[d]);
        smk_pad_ch[d] = smk_ch;
        smk_send(m, 0x90u, d, vel);
    }
}
static void smk_pads_let_go(const hb_module_t *m)   /* as smk_let_go: before LOCAL, MIDI CH or PADS changes */
{
    uint32_t i;
    for (i = 0; i < 16u; i++)
        smk_pad_up(m, i);
}
static void smk_extras(const hb_module_t *m, uint32_t base)
{
    int32_t x, b;
    uint32_t i, v;
    while ((x = smk_ext_take()) >= 0) {
        uint32_t code = (uint32_t)x >> 8, vel = (uint32_t)x & 0x7Fu;
        if (code >= SMK_EXT_PAD) {              /* a pad */
            uint32_t p = code - SMK_EXT_PAD;
            if (vel) smk_pad_hit(m, p, vel, base);
            else smk_pad_up(m, p);
        } else if (code < SMK_NKEY) {           /* an outer key: the note it started stops */
            if (vel) {
                uint32_t note = base + code - SMK_KEY_F3;
                if (note > 127u)                /* (past MIDI's notes, either way: no note, not another one) */
                    continue;
                smk_outer_up(m, code);          /* (struck again without a let-go) */
                smk_outer[code] = (uint8_t)note;
                smk_outer_ch[code] = smk_ch;
                smk_send(m, 0x90u, note, vel);
            } else {
                smk_outer_up(m, code);
            }
        }
    }
    if (smk_pads_shown != opt_pads()) {         /* (at power-on, and when PADS changes) */
        smk_pads_shown = (uint8_t)opt_pads();
        for (i = 0; i < 16u; i++)
            if (!smk_pad[i].n) smk_pad_idle(i);
    }
    padled_flush();
    if (!fm1_in.wheel[0] || fm1_ms < 400u)      /* (the readings settle first) */
        return;
    if (!smk_live) {                            /* the faders as they are at power-on: nothing sent for them */
        smk_live = 1;
        for (i = 0; i < 4u; i++) smk_fad[i] = fm1_in.fader[i];
        smk_mod = (uint8_t)smk_modval(fm1_in.wheel[1]);
    }
    for (i = 0; i < 4u; i++)
        if ((v = fm1_in.fader[i]) != smk_fad[i]) {
            smk_fad[i] = (uint8_t)v;
            smk_send(m, 0xB0u, 70u + i, v);
        }
    if ((v = smk_modval(fm1_in.wheel[1])) != smk_mod) {
        smk_mod = (uint8_t)v;
        smk_send(m, 0xB0u, 1u, v);
    }
    if ((b = smk_pitch(fm1_in.wheel[0], fm1_ms)) != smk_bend) {
        smk_bend = b;
        smk_send(m, 0xE0u, (uint32_t)(b + 8192) & 0x7Fu, (uint32_t)(b + 8192) >> 7);
    }
}
#endif

static void app_main(void)
{
    uint32_t prev = 0, k, full = 1, picker = 0, sel = 0, t_top = 0, held_any = 0, t_notes = 0, safe = 0;
    uint32_t home_t0 = 0, home_alone = 0, home_eaten = 0;   /* HOME held alone 1.5 s: the picker (HB_F_PANEL) */
    uint32_t ble_drawn = HX_S_MISSING + 1u, cc7_drawn = 127u;   /* (the OPTIONS lines as drawn) */
    uint32_t eqp = 0, eq_row = 0, eq_said = 0;  /* a page open (1 the EQ's, 2 ABOUT), its line; the EQ's failure logged */
    uint32_t sheld = 0, t_press = 0;            /* the sensed keys held (bit k); the pressure's last round */
    int32_t oct_drawn = 99;
    uint8_t held[27] = {0}, press_sent[27] = {0};
    sense_ev_t se;

    hb_pc_limits(PC_RAM_HI);                    /* (the deployed modules' code may run: see there) */
    nv_init();
    if (hb_nv.active && (fm1_boot.p3_rst & 4u)) {   /* the watchdog, with a module running */
        if (hb_nv.active <= HB_SLOTS + 1u && hb_nv.strikes[hb_nv.active - 1u] < 255u)
            hb_nv.strikes[hb_nv.active - 1u]++;
        hb_log_u32("watchdog in module", hb_nv.active - 1u);
        if (hb_nv.active > 1u && bootguard.failed)
            bootguard.failed--;                 /* a slot module hung, not the host's boot: as hb_fault */
    }
    if (hb_probe.magic == PR_MAGIC && hb_probe.state == PR_RUNNING) {   /* reset in a probe: the watchdog or its own */
        hb_probe.state = PR_HUNG;
        hb_log_u32("reset in the probe", fm1_boot.p3_rst);
        if (bootguard.failed)
            bootguard.failed--;
    }
    hb_nv.active = 0;
    hb_log_u32("fm1-dock " APP_VER " boot, reset", fm1_boot.p3_rst);
    hb_log_u32("supplies VDC14 SYSVDD: found, now", fm1_supplies);   /* (fm1_power.h: 7 / 15 M-VAVE's own) */
    if (fm1_power_err)                          /* (P33 as the boot loader left them and after: fm1_power_was / _now) */
        hb_log_u32("supplies not as stock", fm1_power_err);
    cpu1_said = cpu1_state == CPU1_NOSTART || cpu1_state == CPU1_BARRED || cpu1_state == CPU1_SUPPLY
                || (cpu1_state == CPU1_FAILED && cpu1_late == 1) ? cpu1_state : CPU1_SCREENING;
    hb_log(cpu1_said == CPU1_SCREENING ? "cpu1: started" : cpu1_said == CPU1_NOSTART   /* (the screening may */
           ? "cpu1: did not start, 1 core" : cpu1_said == CPU1_SUPPLY                  /* have ended already) */
           ? "cpu1: supplies not as stock, 1 core" : cpu1_said != CPU1_BARRED
           ? "cpu1: failed its self-test, 1 core" : cpu1_bar_why == 1
           ? "cpu1: the watchdog with 2 cores: 1 core until power-off"
           : "cpu1: off since a crash with 2 cores, until power-off");
    if (hb_nv.crash.magic == CRASH_MAGIC)
        hb_log_u32("last crash pc", hb_nv.crash.pc);

    store_load();
    if (st.oct < -2 || st.oct > 2) st.oct = 0;
#if BOARD_SMK37
    ble_capture();                              /* (the stock VM's region: before anything writes it) */
    sense_mask = (1u << 27) - 1u;               /* (every key: its velocity is the scan's) */
    {
        static uint8_t trim[SMK_TRIM_SIZE];     /* the unit's own key and pad curves (read only) */
        if (flash_ok && !st_read(SMK_TRIM_FLASH, trim, sizeof trim))
            hb_log(smk_trim_load(trim) ? "SMK-37: the factory key curves" : "SMK-37: no factory key curves, the defaults");
    }
#else
    for (k = 0; k < FM1_NCOL; k++) {            /* the keys on the analog rows */
        if (FM1_KEYMAP[3][k] >= 14) sense_mask |= 1u << (FM1_KEYMAP[3][k] - 14);
        if (FM1_KEYMAP[4][k] >= 14) sense_mask |= 1u << (FM1_KEYMAP[4][k] - 14);
    }
#endif
    sense_init(sense_mask);
    sense_defaults();
    fl_plain_window_init();                     /* flash 0x93000 up reads as stored: the deployed modules */
    ir_init();                                  /* (below it: the image's room, read through the SFC) */
    scan_slots();

    fm1_delay_ms(60);                           /* the key scan sees what is held at power-on */
    safe = (fm1_in.buttons >> BTN_OCT_UP) & 1u;
    ble_boot(safe);
    eq_held = (uint8_t)((st.spare[2] & EQ_ON) && (bootguard.failed || safe));   /* (as the radio: a way back from an EQ that misbehaves on the chip) */
    if (eq_held)
        hb_log("EQ saved ON, held this boot");
    else
        eq_apply();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    if (safe)
        hb_log("safe boot (OCT+): the sine only, the radio not started");
    k = safe ? 0 : find_key(st.sel_key);
#ifdef HB_SOLO
    if (!safe && !st.sel_key && n_mods > 1u && mods[1].kind == K_IMAGE)
        k = 1;                                  /* a solo app's new install (nothing picked yet): its module */
#endif
    if (k && mods[k].state != M_OK) {
        hb_log_u32("skipped at boot", k);
        k = 0;
    }
    if (start_module(k))
        start_module(0);

    for (;;) {
        uint32_t keys, down, up, rel, b, e, base = (uint32_t)(KEY0 + 12 * st.oct), panel, screen, sensed;
        uint32_t local, kpanel, och, from;
        uint8_t s, d1, d2;
        const hb_module_t *m;
        platform_poll();
        if (cpu1_said != cpu1_state) {          /* the second core's changes, each once */
            if (cpu1_said == CPU1_SCREENING)
                hb_log_u32(cpu1_state == CPU1_OK ? "cpu1: 2 cores, screened in us" : cpu1_state == CPU1_MISREAD
                           ? "cpu1: misread RAM, 1 core; screened us" : "cpu1: late in its screening, 1 core; us",
                           cpu1_screen_ticks / 24u);
            else
                hb_log_u32(cpu1_state == CPU1_MISREAD ? "cpu1: misread around a job, 1 core; timeouts"
                           : "cpu1: a job ran late, 1 core; timeouts", cpu1_timeouts);
            cpu1_said = cpu1_state;
        }
        if (eq_failed && !eq_said) {
            eq_said = 1;
            hb_log("EQ: a run did not finish, the EQ is off");
        }
        if (fm1_ms < 600u && (fm1_in.buttons >> BTN_OCT_DOWN & 1u))
            rescue_late();                      /* (OCT- held from power-on) */
        m = hb_active;
        panel = m && (m->flags & HB_F_PANEL) && m->ui;
        screen = m && (m->flags & HB_F_SCREEN);
        local = !(st.spare[1] & OPT_LOCAL_OFF);
        kpanel = panel && local;                /* the module plays its keys (LOCAL OFF: the host sends them) */
        och = opt_ch() ? opt_ch() - 1u : 0;     /* the channel the keys send on (MIDI CH; ALL: 1) */
        if (panel && !local && mod_key0)
            base = mod_key0;                    /* (LOCAL OFF: the OCT buttons are the module's, so the keys send in its octave) */

        keys = fm1_in.notes;                    /* bit k: key k (0 = F3 .. 26 = G5) */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
#if BOARD_SMK37
        smk_sense_keys(down, up);               /* (as the FM-1's sensed keys, with the velocity) */
        smk_local = (uint8_t)local;
        smk_ch = (uint8_t)(kpanel ? mod_ch : och);   /* (as the middle keys: the module's own, or MIDI CH) */
        smk_extras(m, kpanel && mod_key0 ? mod_key0 : base);   /* (the outer keys go on from the middle ones') */
#endif
        sensed = h_sense_keys();
        for (k = 0; k < 27u; k++) {
            if (sensed >> k & 1u) {             /* a sensed key plays from its landing (below) */
                if (!kpanel)
                    fm1_led_key(KEY_LED(k), keys >> k & 1u);
                continue;
            }
            if (kpanel) {                       /* the module plays its keys */
#if BOARD_SMK37
                if (down >> k & 1u) key_given(k);
#endif
                if (down >> k & 1u) m->ui(HB_EV_KEY_DOWN | k, 0);
                if (up >> k & 1u) m->ui(HB_EV_KEY_UP | k, 0);
                continue;
            }
            if (down >> k & 1u) {
                held[k] = (uint8_t)(base + k);
                if (local) play_note(0, held[k], 100);
                midi_send(0x90u | och, held[k], 100);
            }
            if (up >> k & 1u) {                 /* a key stops the note it started */
                if (local) play_note(0, held[k], 0);
                midi_send(0x80u | och, held[k], 0);
            }
            fm1_led_key(KEY_LED(k), keys >> k & 1u);
        }
        while (sense_take(&se)) {               /* the sensed keys: landed (with a velocity), lifted */
            k = se.key;
            if (k >= 27u || !(sensed >> k & 1u))
                continue;
            if (se.type == EV_ON && !(sheld >> k & 1u)) {
                uint32_t v = se.vel ? se.vel : 1u;
                sheld |= 1u << k;
                press_sent[k] = 0;
                if (kpanel) {
#if BOARD_SMK37
                    key_given(k);
#endif
                    m->ui(HB_EV_KEY_DOWN | k, (int32_t)v);
                } else {
                    held[k] = (uint8_t)(base + k);
                    if (local) play_note(0, held[k], v);
                    midi_send(0x90u | och, held[k], v);
                }
            } else if (se.type == EV_OFF && sheld >> k & 1u) {
                sheld &= ~(1u << k);
                if (kpanel) {
                    m->ui(HB_EV_KEY_UP | k, 0);
                    at_note_off(m, 0, k);       /* (AFTERTOUCH: its pressure goes with it) */
                } else {
                    if (local) play_note(0, held[k], 0);
                    midi_send(0x80u | och, held[k], 0);
                }
            }
        }
        for (k = 0; k < 27u; k++)               /* sensing switched off with keys held: let them go */
            if (sheld >> k & ~sensed >> k & 1u) {
                sheld &= ~(1u << k);
                if (kpanel) {
                    m->ui(HB_EV_KEY_UP | k, 0);
                    at_note_off(m, 0, k);       /* (AFTERTOUCH: its pressure goes with it) */
                } else {
                    if (local) play_note(0, held[k], 0);
                    midi_send(0x80u | och, held[k], 0);
                }
            }
        if (sheld && fm1_ms - t_press >= 10u) {  /* their pressure, ~100 times a second */
            t_press = fm1_ms;
            for (k = 0; k < 27u; k++) {
                uint32_t pr;
                if (!(sheld >> k & 1u) || (pr = sense_pressure(k)) == press_sent[k])
                    continue;
                press_sent[k] = (uint8_t)pr;
                if (kpanel && opt_at()) {       /* AFTERTOUCH turns it: by the key's number (a panel module */
                    at_route(m, 0xA0u, k, pr);  /* picks its note), the hardest-pressed key counts */
                } else if (kpanel) {
                    m->ui(HB_EV_KEY_PRESSURE | k, (int32_t)pr);
                } else {
                    if (local && m && opt_at()) at_route(m, 0xA0u, held[k], pr);
                    else if (local && m && m->midi) m->midi(0xA0, held[k], pr);
                    midi_send(0xA0u | och, held[k], pr);
                }
            }
        }
        held_any = keys != 0;
        if (down)
            home_alone = 0;
        if (st.spare[1] & OPT_MASTER_CC7)
            master_cc7(och);
        else
            mcc7 = 128u;                        /* (switched on: from where the knob is then) */

        while ((from = rt_late_get(&s, &d1, &d2) ? 3u : midi_get(&s, &d1, &d2) ? 1u : din_get(&s, &d1, &d2) ? 2u : 0) != 0) {   /* clock for a module without HB_F_MIDI_RT, USB / BLE-MIDI, the DIN jack */
            uint32_t ch;
            if (from == 2u && (st.spare[1] & OPT_THRU))
                midi_send(s, d1, d2);           /* MIDI THRU: on to USB and BLE as it came */
            if (opt_ch() && s < 0xF0u) {        /* MIDI CH: that channel only, to the module as channel 1 */
                if ((s & 15u) != opt_ch() - 1u)
                    continue;
                s &= 0xF0u;
            }
            ch = s & 15u;
            if ((s & 0xE0u) == 0x80u) t_notes = fm1_ms;
            if ((s & 0xF0u) == 0xB0u && d1 == 7u && (st.spare[1] & OPT_CC7)) { cc7_set(d2); continue; }   /* CC7 VOLUME: the host's */
            if (opt_at() && ((s & 0xF0u) == 0xA0u || (s & 0xF0u) == 0xD0u)) { at_route(m, s, d1, d2); continue; }
            if ((s & 0xF0u) == 0x80u || ((s & 0xF0u) == 0x90u && !d2)) {
                if (m && m->midi) { m->midi(s, d1, d2); at_note_off(m, ch, d1); continue; }
            }
            if (m && m->midi) { m->midi(s, d1, d2); continue; }
            switch (s & 0xF0u) {
            case 0x90: play_note(ch, d1, d2); break;
            case 0x80: play_note(ch, d1, 0); break;
            case 0xB0: if (m && m->cc) m->cc(ch, d1, d2); break;
            default: break;
            }
        }

        if (restart_on) {                       /* (no frame, button or note meanwhile: the list is of before) */
            fm1_wdt_feed();
            if (fm1_ms - restart_at > (so_w == so_r ? 400u : 2000u)) {   /* (the COMMIT's answer is out) */
                fm1_audio_stop();
                bootguard_clear(&bootguard);    /* (asked for: not a start that failed, however soon after one) */
                usb_detach();                   /* (the computer sees it go, as before an update) */
                fm1_delay_ms(30);
                fm1_reboot();
            }
            continue;
        }
        if (deploy_tick()) {                    /* a deploy: the module may have changed */
            picker = eqp = 0;
            full = 1;
            if (!rx.on)
                lcd_fill(0, 0, 240, 240, C_BLACK);
            continue;
        }
        if (hb_slow) {
            hb_slow = 0;
            hb_log_u32("too slow: stopped", cur);
            mods[cur].state = M_SLOW;
            start_module(0);
            picker = eqp = 0;
            full = 1;
            lcd_fill(0, 0, 240, 240, C_BLACK);
        }

        b = fm1_input_edges(&rel);              /* buttons pressed / released since the last call */
        if (b)
            save_note_armed = 1;                /* (a failed save after it is said again) */
        if (rx.on) {                            /* receiving: the module RAM is the deploy's */
            b = rel = 0;
            for (e = 0; e < 7u; e++)
                fm1_enc_take(e);
        }
        if (!picker && (st.spare[1] & OPT_KNOBS))
            knobs_cc(m, och, local);            /* (before the module's: they are not its knobs then) */
        if (!panel || picker) {
            if (b & (1u << BTN_OCT_DOWN) && st.oct > -2) { st.oct--; store_touch(); }
            if (b & (1u << BTN_OCT_UP) && st.oct < 2) { st.oct++; store_touch(); }
        }
        if (picker) {
            int32_t en[7], mv, turn = 0;
            for (e = 0; e < 7u; e++)
                en[e] = fm1_enc_take(e);
            mv = en[ENC_SELECT] + en[ENC_PRESETS];
            for (e = 1; e < 6u; e++)
                turn += en[e];                  /* (the module's knobs: MIDI CH's value, else nothing meanwhile) */
            if (eqp) {                          /* the page takes them all */
                if (eqp == 2u ? about_input(&eq_row, en, b) : eq_page_input(&eq_row, en, b)) {
                    eqp = 0;
                    draw_picker(sel);
                }
                b = 0;
                mv = turn = 0;
            }
            if (mv) {
                int32_t ns = (int32_t)sel + mv;
                sel = ns < 0 ? 0 : ns > (int32_t)(n_mods + N_OPTS - 1u) ? n_mods + N_OPTS - 1u : (uint32_t)ns;   /* (then OPTIONS) */
                draw_picker(sel);
            }
            if (ble_state() != ble_drawn || cc7 != cc7_drawn) {
                ble_drawn = ble_state();
                cc7_drawn = cc7;
                if (sel >= n_mods && !eqp)
                    draw_picker(sel);
            }
            if (sel == n_mods + O_EQ && (b & (1u << BTN_SEL | 1u << BTN_PLAY))) {
                eqp = 1;                        /* the EQ's page */
                eq_row = 0;
                draw_eq_page(0);
            } else if (sel == n_mods + O_ABOUT && (b & (1u << BTN_SEL | 1u << BTN_PLAY))) {
                eqp = 2;                        /* ABOUT */
                eq_row = 0;
                draw_about(0);
            } else if (sel >= n_mods && ((b & (1u << BTN_SEL | 1u << BTN_PLAY)) || (turn && (sel == n_mods + O_CH || (BOARD_SMK37 && sel == n_mods + O_PADS))))) {
                uint32_t o = sel - n_mods;      /* an option changed; OPTIONS stays */
                if (o == O_LOCAL || o == O_CH)  /* the keys held: let go as they were played */
                    for (k = 0; k < 27u; k++)
                        if ((prev | sheld) >> k & 1u) {
                            if (kpanel) {
                                m->ui(HB_EV_KEY_UP | k, 0);
                            } else {
                                if (local) play_note(0, held[k], 0);
                                midi_send(0x80u | och, held[k], 0);
                            }
                        }
                if (o == O_LOCAL || o == O_CH)
                    sheld = 0;
#if BOARD_SMK37
                if (o == O_LOCAL || o == O_CH || o == O_PADS) {
                    smk_let_go(m);              /* (the outer keys and the pads too) */
                    smk_pads_let_go(m);
                }
#endif
                if (o == O_CC7) {
                    st.spare[1] ^= OPT_CC7;     /* CC7 VOLUME ON / OFF (OFF: the level as the knob alone again) */
                    cc7_set(127u);
                } else if (o == O_MASTER_CC7) {
                    st.spare[1] ^= OPT_MASTER_CC7;
                } else if (o == O_CH) {         /* SEL: the next channel; a knob: either way (ALL, 1 .. 16) */
                    int32_t c = (int32_t)opt_ch() + (turn ? turn : 1);
                    c = ((c % 17) + 17) % 17;
                    st.spare[1] = (st.spare[1] & ~OPT_CH_MASK) | (uint32_t)c << OPT_CH_SHIFT;
                } else if (o == O_LOCAL) {
                    st.spare[1] ^= OPT_LOCAL_OFF;
                } else if (o == O_KNOBS) {
                    st.spare[1] ^= OPT_KNOBS;
                } else if (o == O_AT) {         /* SEL: the next; a knob: either way */
                    int32_t a = (int32_t)opt_at() + (turn ? turn : 1);
                    a = ((a % 4) + 4) % 4;
                    at_send(m, 0, 0);           /* (the old way lets go first: the wheel or the filter back) */
                    at_reset();
                    st.spare[1] = (st.spare[1] & ~OPT_AT_MASK) | (uint32_t)a << OPT_AT_SHIFT;
#if BOARD_SMK37
                } else if (o == O_PADS) {       /* SEL: the next; a knob: either way */
                    int32_t c = (int32_t)opt_pads() + (turn ? turn : 1);
                    c = ((c % (int32_t)PADS_N) + (int32_t)PADS_N) % (int32_t)PADS_N;
                    st.spare[1] = (st.spare[1] & ~OPT_PADS_MASK) | (uint32_t)c << OPT_PADS_SHIFT;
#else
                } else if (o == O_THRU) {
                    st.spare[1] ^= OPT_THRU;
#endif
                }
                if (o < N_OPTS_HOST)
                    store_touch();
                else
                    ble_set(ble_state() == HX_S_OFF);   /* BLUETOOTH ON / OFF */
                draw_picker(sel);
            } else if (b & (1u << BTN_HOME | 1u << BTN_SEL | 1u << BTN_PLAY)) {
                if (sel >= n_mods)
                    sel = cur;                  /* (HOME on OPTIONS: back to the module) */
                picker = 0;
                full = 1;
                lcd_fill(0, 0, 240, 240, C_BLACK);
                if (sel != cur || mods[sel].state == M_STRUCK || mods[sel].state == M_SLOW) {
                    if (mods[sel].state == M_STRUCK || mods[sel].state == M_SLOW) {   /* picked again: another chance */
                        hb_nv.strikes[sel] = 0;
                        mods[sel].state = M_OK;
                    }
                    if (start_module(sel))
                        start_module(0);
                    st.sel_key = mods[cur].key;
                    store_touch();
                }
            }
        } else if (panel) {                     /* every button and encoder is the module's */
            if (b >> BTN_HOME & 1u) { home_t0 = fm1_ms; home_alone = 1; home_eaten = 0; }
            if ((b & ~(1u << BTN_HOME)) || (keys && b >> BTN_HOME & 1u))
                home_alone = 0;
            for (k = 0; k < (m->flags & HB_F_XBTN ? HB_BTN_RIGHT + 1u : 14u); k++) {   /* (XBTN: the SMK's too) */
                if (b >> k & 1u) m->ui(HB_EV_BTN_DOWN | k, 0);
                if (rel >> k & 1u && !(k == BTN_HOME && home_eaten)) m->ui(HB_EV_BTN_UP | k, 0);
            }
            for (e = 0; e < 7u; e++) {
                int32_t c = fm1_enc_take(e);
                if (c) { m->ui(HB_EV_ENC | e, c); home_alone = 0; }
            }
            if (home_alone && (fm1_in.buttons >> BTN_HOME & 1u) && fm1_ms - home_t0 >= 1500u) {
                m->ui(HB_EV_BTN_UP | BTN_HOME, 0);   /* HOME held alone: the picker; the module saw it let go */
                home_alone = 0;
                home_eaten = 1;
                picker = 1;
                sel = cur;
                lcd_fill(0, 0, 240, HB_TOP, C_BLACK);
                draw_picker(sel);
                full = 1;
            }
        } else {
            if (b & (1u << BTN_HOME)) {
                picker = 1;
                sel = cur;
                draw_picker(sel);
            } else if (m && m->ui) {
                for (k = 2; k < (m->flags & HB_F_XBTN ? HB_BTN_RIGHT + 1u : 14u); k++) {   /* not OCT-/+, HOME */
                    if (k == BTN_HOME) continue;
                    if (b >> k & 1u) m->ui(HB_EV_BTN_DOWN | k, 0);
                    if (rel >> k & 1u) m->ui(HB_EV_BTN_UP | k, 0);
                }
                for (e = 0; e < 7u; e++) {
                    int32_t c = fm1_enc_take(e);
                    if (c) m->ui(HB_EV_ENC | e, c);
                }
            }
        }

        sel_led(picker);
        if ((!screen || picker) && (st.oct != oct_drawn || fm1_ms - t_top > 500u || full)) {
            oct_drawn = st.oct;
            t_top = fm1_ms;
            draw_top(st.oct);
        }
        m = hb_active;                          /* under the picker: the module's loop still (its keys), not seen */
        if (save_note && !picker && !rx.on) {   /* a save that failed: said for 2.5 s over the module's screen */
            if (!save_note_on) {
                save_note_on = 1;
                save_note_at = fm1_ms;
                lcd_fill(0, 0, 240, 240, C_BLACK);   /* (all of it: the module may have drawn its own SAVED) */
                cv_begin(240, 56, C_BLACK);
                cv_rect(0, 0, 240, 2, C_BAD);
                cv_rect(0, 54, 240, 2, C_BAD);
                cv_text(120 - text_w(&FONT_S, "NOT SAVED") / 2, 8, &FONT_S, "NOT SAVED", C_BAD);
                cv_text(120 - text_w(&FONT_S, save_note) / 2, 30, &FONT_S, save_note, C_WHITE);
                cv_blit(0, 92);
            } else if (fm1_ms - save_note_at > 2500u) {
                save_note = 0;
                save_note_on = 0;
                lcd_fill(0, 0, 240, 240, C_BLACK);
                full = 1;
            }
        }
        scr_hidden = (uint8_t)(picker ? 1u : save_note_on ? 2u : 0u);
        if (m && m->draw) m->draw(picker ? 0 : full);
        scr_hidden = (uint8_t)(save_note_on ? 2u : 0u);   /* (under the note: what its ui() and midi() draw too) */
        if (!picker)
            full = 0;                           /* (the picker gone: full, all of it again) */

        ble_main();
        ble_watch();
        if (st_dirty && !held_any && fm1_ms - st_changed > 2000u && fm1_ms - t_notes > 2000u)
            store_flush();                      /* (interrupts are off while the flash is busy: no module code runs) */
    }
}
