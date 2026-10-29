/* SPDX-License-Identifier: GPL-3.0-only */
/* fm1-dock's module interface, version 1: what a module exports (hb_module_t, the descriptor at
 * the start of its image) and what the host gives it (hb_host_t, passed to init). Tables only: a
 * module links against nothing of the host's, and the host checks `abi` before any call.
 *
 * A module runs in place from flash and has one fixed RAM region (HB_MOD_RAM), shared by all
 * modules: only one runs at a time. The host copies its .data there, clears its .bss and calls
 * init(); switching away calls release(). render() runs in the audio interrupt; everything else
 * runs in the main loop.
 *
 * Rules (as Dinghy's, docs/RULES.md): integers only, no 64-bit division, no hardware registers,
 * no waiting. Draw only below HB_TOP (the host's line at the top). */
#ifndef HB_MODULE_H
#define HB_MODULE_H
#include <stdint.h>

#define HB_ABI 2u                               /* 2: flags, midi(), blit(), text_w(), key events */

/* memory a module may use (modules/module.ld places .data / .bss here) */
#define HB_MOD_RAM      0x01C30000u             /* .data + .bss, 272 KiB */
#define HB_MOD_RAM_SIZE 0x44000u
#define HB_MOD_RT       0x01C04000u             /* .ram_text, 16 KiB: call it through a pointer (XIP -> RAM is */
#define HB_MOD_RT_SIZE  0x4000u                 /* beyond the 23-bit call range), see HB_FAR */

#define HB_TOP 24                               /* screen rows above this are the host's (not with HB_F_SCREEN) */

/* hb_module_t.flags */
#define HB_F_SCREEN 1u                          /* all of the screen is the module's: the host draws only its picker */
#define HB_F_PANEL  2u                          /* all keys and buttons are the module's, OCT- / OCT+ / HOME too, as
                                                 * events; the host plays no note itself. HOME held 1.5 s alone opens
                                                 * the picker (the module gets the HOME release first) */
#define HB_F_DATA   4u                          /* the descriptor has data(): what the computer sends the module
                                                 * (tools/dock.py's MODULE frames) reaches it */

/* ui() events: kind in the top byte, the id below; arg is the encoder's clicks */
enum {
    HB_EV_BTN_DOWN = 0x01000000u,               /* | button id (BTN_FX ...; not OCT-/OCT+/HOME: the host's) */
    HB_EV_BTN_UP   = 0x02000000u,
    HB_EV_ENC      = 0x03000000u,               /* | encoder id (ENC_SELECT ... ENC_PRESETS), arg = clicks */
    HB_EV_KEY_DOWN = 0x04000000u,               /* | note key k (0 = F3 .. 26 = G5): HB_F_PANEL only; arg = the
                                                 * velocity (1..127) of a key whose force is sensed, 0 if not */
    HB_EV_KEY_UP   = 0x05000000u,
    HB_EV_KEY_PRESSURE = 0x06000000u,           /* | key k held, arg = its pressure 0..127 (aftertouch; sensed keys,
                                                 * HB_F_PANEL only; a module with midi() but no panel gets An) */
};
#define HB_EV_KIND(ev) ((ev) & 0xFF000000u)
#define HB_EV_ID(ev)   ((ev) & 0xFFu)

/* panel ids, as platform.c names them */
enum { HB_BTN_OCT_DOWN = 0, HB_BTN_OCT_UP = 1, HB_BTN_FX = 2, HB_BTN_SEL = 3, HB_BTN_ENV = 4, HB_BTN_LFO = 5,
       HB_BTN_EDIT = 6, HB_BTN_GLO = 7, HB_BTN_HOME = 8, HB_BTN_SAVE = 9, HB_BTN_ARP = 10, HB_BTN_SEQ = 11,
       HB_BTN_PLAY = 12, HB_BTN_REC = 13 };
enum { HB_ENC_SELECT = 0, HB_ENC_ALGORITHM = 1, HB_ENC_KNOB1 = 2, HB_ENC_KNOB2 = 3, HB_ENC_KNOB3 = 4,
       HB_ENC_KNOB4 = 5, HB_ENC_PRESETS = 6 };
#define HB_LED_KEY(k) (14u + (k))               /* note key k's LED (0 = F3 .. 26 = G5) */

/* the descriptor: at the start of the module's image (modules/module.ld) */
#ifndef HB_DESC
#define HB_DESC const hb_module_t hb_module __attribute__((section(".hb_desc"), used))
#endif
/* a call from flash into the module's .ram_text: through a pointer the compiler cannot fold */
static inline void *hb_far(void *p) { void *volatile q = p; return q; }
#define HB_FAR(fn) ((__typeof__(&fn))hb_far((void *)&fn))

#define HB_RGB(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

/* one key's force readings (hb_host_t.key_sense) */
typedef struct hb_key_sense {
    uint8_t state;                              /* 0 open, 1 touched, 2 down, 3 lifting (sense.c's) */
    uint8_t vel;                                /* the last landing's velocity */
    uint16_t v;                                 /* the last reading pulled up (1023 open .. ~186 the diode) */
    uint16_t d;                                 /* the last driven reading (0: none), HB_SENSE_DBASE: no pill */
    uint16_t dland;                             /* the driven reading the velocity came from */
} hb_key_sense_t;
#define HB_SENSE_DBASE 185u

typedef struct hb_host {
    uint32_t abi;                               /* HB_ABI */
    uint32_t size;                              /* sizeof(hb_host_t): later versions only append */
    /* time */
    uint32_t (*ms)(void);                       /* milliseconds since power-on */
    uint32_t (*ticks)(void);                    /* the 24 MHz counter (busy time, not for waiting) */
    /* the log: a ring in RAM that survives a reset (tools/dock.py log) */
    void (*log)(const char *s);
    void (*log_u32)(const char *s, uint32_t v); /* s then v in hex */
    /* the screen, 240 x 240, RGB565; text sizes 0 (16 px) and 1 (32 px) */
    void (*fill)(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c);
    void (*text)(int32_t x, int32_t y, int32_t w, uint32_t big, const char *s, uint16_t fg, uint32_t center);
    /* a canvas (up to 240 x 96): drawn off screen, then put on it in one go */
    void (*cv_begin)(int32_t w, int32_t h, uint16_t bg);
    void (*cv_rect)(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c);
    void (*cv_line)(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c);
    void (*cv_pixel)(int32_t x, int32_t y, uint16_t c);
    void (*cv_text)(int32_t x, int32_t y, uint32_t big, const char *s, uint16_t fg);
    void (*cv_blit)(int32_t x, int32_t y);
    /* LEDs: buttons by id, note keys HB_LED_KEY(k); on: 0 off, 1 lit, 2 the glow (a dim light: what
     * could be pressed; a host before the glow lights it) */
    void (*led)(uint32_t id, uint32_t on);
    uint32_t (*rand)(void);
    /* this module's own settings record (up to 240 bytes), kept in flash when nothing plays */
    int (*load)(void *p, uint32_t n);           /* 0: loaded */
    void (*save)(const void *p, uint32_t n);
    void (*midi_send)(uint32_t st, uint32_t d1, uint32_t d2);
    uint32_t (*master)(void);                   /* the MASTER pot as a gain, Q12 (the host applies it) */
    /* ABI 2 */
    void (*blit)(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *px);   /* pixels as the panel takes
                                                 * them (RGB565, high byte first); done when it returns */
    int32_t (*text_w)(uint32_t big, const char *s);   /* the width text() / cv_text() give s */
    /* this module's own 8 KiB of flash (found by its name), for what does not fit its settings
     * record (user presets): up to HB_BANK_MAX bytes with a CRC, p in the module's RAM; a save
     * erases the bank (the sound stalls ~100 ms) */
    int (*bank_load)(void *p, uint32_t n);      /* 0: loaded (n as saved) */
    int (*bank_save)(const void *p, uint32_t n);
    /* the keys' force (Velo's sensing, by way of CS-1, in the host's key scan; a module checks size first): the
     * keys whose force is sensed now (bit k, 0: none / off: the bottom two rows, 20 keys); one key's
     * readings; and, while this module runs, the sensing's own settings (on, DRIVE 0..4, the
     * velocity curve x 100, the reading of the lightest press that seats) */
    uint32_t (*sense_keys)(void);
    int (*key_sense)(uint32_t k, hb_key_sense_t *out);   /* 0: filled */
    void (*sense_set)(uint32_t on, uint32_t drive, uint32_t curve, uint32_t ceil);
    /* more banks than the first (a module checks size first): bank idx 1..HB_BANK_IDX_MAX, each found by
     * the module's name and idx, up to HB_BANK_MAX bytes with a CRC (FM-1 SID's songs, a few banks each).
     * load: up to n bytes into p (in the module's RAM), returns their length, -1 none (n 0: the length
     * only); save: n bytes from p, n 0 gives the bank back; free: banks a save could still take. The
     * bank area is 9 banks for every module here: a save may find none (-1) */
    int (*bank_load_n)(uint32_t idx, void *p, uint32_t n);
    int (*bank_save_n)(uint32_t idx, const void *p, uint32_t n);
    uint32_t (*bank_free)(void);
    /* the second core (a module checks size first), from render() only: c1_run hands it fn(arg), one job at a
     * time, and returns 1 when it took it, 0 when there is none (not started, still being screened in the
     * first ~0.75 s, failed its checks, or a job is out): then the module does the work itself. c1_wait waits for the job: 0 done; -1 it did not end in
     * time: core 1 is held from then on and the module does the work itself (so a job must stand being
     * run again from the start, from where core 1 left its state). Between the two the module
     * must not touch what the job reads or writes; render() waits before it returns (the host waits for
     * a job left out). fn runs on core 1 from the module's flash or .ram_text, on a 2.8 KiB stack, with
     * the same rules as render(), and no host call but ticks() */
    int (*c1_run)(void (*fn)(uint32_t), uint32_t arg);
    int (*c1_wait)(void);
} hb_host_t;
/* the host has field f of hb_host_t (an older one ends before it) */
#define HB_HOST_HAS(h, f) ((h)->size >= (uint32_t)(__builtin_offsetof(hb_host_t, f) + sizeof (h)->f))
#define HB_BANK_MAX 8176u
#define HB_BANK_IDX_MAX 255u

typedef struct hb_param {
    const char *name;
    int32_t lo, hi, def;
} hb_param_t;

typedef struct hb_module {
    uint32_t abi;                               /* HB_ABI */
    const char *name;
    int  (*init)(const hb_host_t *h);           /* 0: ready; anything else and the host falls back */
    void (*release)(void);                      /* notes are cut, the module is switched away */
    void (*note)(uint32_t ch, uint32_t key, uint32_t vel);   /* vel 0: note off */
    void (*cc)(uint32_t ch, uint32_t num, uint32_t val);
    void (*render)(int32_t *out, uint32_t n);   /* audio interrupt: add n stereo frames (L R, Q15) into out */
    void (*ui)(uint32_t ev, int32_t arg);
    void (*draw)(uint32_t full);                /* main loop, often: draw what changed (full: all of it) */
    const hb_param_t *params;                   /* for a generic page (may be 0) */
    uint32_t n_params;
    uint32_t flags;                             /* HB_F_SCREEN, HB_F_PANEL */
    void (*midi)(uint32_t st, uint32_t d1, uint32_t d2);   /* 0: note() / cc() only; else every channel message */
    /* HB_F_DATA only (a descriptor without the flag ends at midi): a MODULE frame's payload from the
     * computer, up to HB_DATA_MAX bytes, in the main loop; 0: taken, else the module's own error (1..255),
     * which goes back to the computer. Nothing of it is kept by the host */
    int (*data)(const uint8_t *p, uint32_t n);
} hb_module_t;
#define HB_DESC_MIN ((uint32_t)__builtin_offsetof(hb_module_t, data))   /* a descriptor without HB_F_DATA */
#define HB_DATA_MAX 512u
/* A file to a module (tools/dock.py send, the pages' WebMIDI): data() gets 'B' [size u32][arg u32] (a
 * file begins: arg is the module's, FM-1 SID's subtune), then 'D' [at u32][bytes] in order from at 0 (the
 * computer sends a chunk again when no answer came: one already taken is taken again), then 'E' (all of it
 * is there). Into the module's RAM only: the flash is the host's, and nothing of the file outlives the
 * module's switching away. */
enum { HB_FILE_BEGIN = 'B', HB_FILE_DATA = 'D', HB_FILE_END = 'E' };

/* A module in flash (and in a .dock file; .hrbr before 0.5): this header, the relocations, then the image.
 *   image: .text + .rodata (code_size), .data's initial values (data_size), .ram_text (rt_size)
 * The image starts at the first 256-byte boundary after the relocations, linked to run there (`base`); relocs[] are offsets of 32-bit words in the image that
 * hold module addresses. Whoever moves it adds (new base - base) to them and sets base. The
 * descriptor is at image offset desc. crc is CRC-32 over the header up to crc, the relocations
 * and the image. */
#define HB_MAGIC 0x52425248u                    /* "HRBR" */
typedef struct {
    uint32_t magic, abi, flags;
    char name[24];
    uint32_t version;
    uint32_t code_size, data_size, bss_size, rt_size;
    uint32_t worst_cycles;                      /* per 256-frame block, measured (0: unknown) */
    uint32_t desc;                              /* the descriptor's offset in the image */
    uint32_t base;                              /* the image's run address */
    uint32_t n_relocs;
    uint32_t reserved;                          /* 0 */
    uint32_t crc;
} hb_file_t;                                    /* 80 bytes; then n_relocs u32, 0xFF to a 256-byte boundary, the image */

/* the slot table: one 4 KiB sector of these; an entry is written after its module, and an
 * erased entry (all 0xFF) ends the table */
#define HB_SLOT_MAGIC 0x544F4C53u               /* "SLOT" */
#define HB_SLOTS 32u
typedef struct {
    uint32_t magic;
    uint32_t at;                                /* XIP address of the hb_file_t */
    uint32_t size;                              /* header + relocs + image */
    uint32_t crc;                               /* the module's hb_file_t.crc */
} hb_slot_t;

/* Probes (tools/probe.py): a program that runs once, from RAM, to measure or test something on
 * the FM-1 and in fm1-emu alike; never written to flash. Built as a module (hb_file_t.flags
 * HB_F_PROBE, probes/probe.ld), sent with BEGIN / DATA, run with RUN: the host relocates it where
 * it was received (code from HB_MOD_RAM up), puts its .data / .bss at HB_PROBE_RAM, calls run()
 * in the main loop (the module stopped, interrupts on) and replies with what it wrote to out.
 * The result is in NOINIT: a probe that crashes or hangs into the watchdog leaves its output so
 * far and the crash's PC, for RESULT after the reboot. Unlike a module, a probe may do anything
 * (wait, turn interrupts off, read hardware registers); the flash is the host's alone. */
#define HB_F_PROBE      0x100u                  /* hb_file_t.flags */
#define HB_PROBE_MAGIC  0x424F5250u             /* "PROB" */
#define HB_PROBE_FILE   0x30000u                /* a probe's file: 192 KiB */
#define HB_PROBE_RAM    (HB_MOD_RAM + HB_PROBE_FILE)   /* its .data + .bss, 80 KiB */
#define HB_PROBE_RAM_SIZE (HB_MOD_RAM_SIZE - HB_PROBE_FILE)
#define HB_PROBE_OUT    8192u                   /* its output */
#define HB_PROBE_ARG    512u                    /* its argument (RUN's payload) */
typedef struct hb_probe_ctx {
    const hb_host_t *host;                      /* the module service table (ticks, log, ...) */
    const uint8_t *arg;                         /* RUN's argument */
    uint32_t arg_n;
    uint8_t *out;                               /* HB_PROBE_OUT bytes, in NOINIT */
    volatile uint32_t out_n;                    /* what of out is the result (kept as it grows: a crash keeps it) */
    void (*wdt_feed)(void);                     /* the watchdog bites after 8 s */
    uint32_t ticks_per_us;                      /* host->ticks(): TIMER4, 24 */
} hb_probe_ctx_t;
typedef struct hb_probe {
    uint32_t magic;                             /* HB_PROBE_MAGIC */
    const char *name;
    int32_t (*run)(hb_probe_ctx_t *c);          /* its return value goes to the computer */
} hb_probe_t;
#define HB_PROBE const hb_probe_t hb_module __attribute__((section(".hb_desc"), used))

/* The boot slot (tools/hbboot.py): a whole firmware, built from its source to run at flash
 * HB_BOOT_IMG, that fm1-dock starts at power-on in place of itself (HOME held: fm1-dock). It has
 * the FM-1 to itself: both cores, all the RAM, its own USB, its own flash areas above 0x97000.
 * Its file is an hb_file_t (flags HB_F_BOOT, no relocations, base = FM1 XIP of HB_BOOT_IMG,
 * desc = 0: the entry is the image's first byte, as the SPL's) and the image, its app.bin. The
 * host writes it encrypted with the chip key, as an update would, below the plain window. */
#define HB_F_BOOT       0x200u                  /* hb_file_t.flags */
#define HB_F_BOOT_NODATA 0x400u                 /* with HB_F_BOOT: it keeps nothing in flash outside its slot
                                                 * (FM-1 SID), so deployed modules may stay beside it */
#define HB_BOOT_LO      0x14000u                /* flash: the first sector (the header at its end) */
#define HB_BOOT_IMG     0x15000u                /* the image */
#define HB_BOOT_HI      0x93000u                /* the end (the plain window: Felucca's data from 0x97000) */

#endif
