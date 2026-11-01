/* SPDX-License-Identifier: GPL-3.0-only */
/* Scintilla: the Elka Synthex engine of synthex-web (VoiceCore and its instrument, ported
 * to integers in ../../../engine, verified against it there) on Dinghy's platform (platform.c: the
 * updater, the USB rescue, USB-MIDI, USB audio, the panel, the screen, the settings sector).
 *
 *   keys            play; two layers of 8 voices, as the patch says (single, split, double). The bottom two
 *                   rows are force sensed (Velo's sense.c): velocity, and pressing harder is aftertouch
 *                   (below), each when SETTINGS turns it on (both are off at first); the top row plays at 100
 *   PRESETS         the 80 factory sounds (PRESETS 10..49, MEMORIES 10..49)
 *   KNOB1..4        the four parameters on the screen; a change applies to both layers
 *   HOME            the page of the most played: cutoff, resonance, envelope, release
 *   EDIT ENV LFO    pages (press again for the next one in the group):
 *   GLO FX SEL        OSC 1, OSC 2, MIX / FILTER, FILTER ENV, AMP ENV / LFO 1, LFO 2 /
 *                     GLIDE, SETTINGS / OUTPUT, REVERB / JOYSTICK (SEL again centres the stick)
 *   SETTINGS        TOUCH (aftertouch) on, its DEPTH, its FLOOR (a key's pressure under it is none),
 *                   VELO (velocity) on (off: the keys play at 100); kept in flash
 *   SELECT          the joystick up and down: pitch bend (and the filter, by the BEND sliders)
 *   ALGORITHM       the joystick left and right: LFO 2 into the pitch (left) or the filter (right)
 *   OCT- OCT+       the octave
 *   MIDI in         USB and the DIN jack, any channel: notes with velocity, pitch bend (2 semitones),
 *                   CC1 = the stick pushed left (vibrato, as on the Synthex), CC7 the volume, CC74 the cutoff,
 *                   CC91 the reverb's mix and CC93 the chorus's (0 turns it off), program change,
 *                   CC120/123, channel and poly pressure (aftertouch)
 *   aftertouch      the hardest press (a key's, or MIDI's) pushes the stick further left: vibrato, as CC1;
 *                   it springs back as the press eases. MIDI out: the keys' notes, and their poly pressure
 *                   while TOUCH is on
 *
 * The preset, the octave, the page and the settings are kept in flash; edits to a sound are not (yet).
 * Rules of the road: integers only, no float, no 64-bit division (docs/RULES.md). */
#define APP_NAME "SCINTILLA"
#ifdef HB_MODULE                                /* a module of fm1-harbor (its modules/scintilla): its platform calls */
#include "sc_harbor.h"
#else
#define APP_EARLY_INIT din_init                 /* the DIN jack's UART, before platform.c starts the key scan */
static void din_init(void);
#ifndef DINGHY_HOST
#define APP_T5_POLL din_poll                    /* TIMER5, 2 kHz: the DIN jack's UART latched */
static void din_poll(void);
#endif
#ifndef DINGHY_HOST                             /* the key scan's analog rows: the keys' force (below) */
#include <stdint.h>
static void key_analog(uint32_t col, uint32_t rowbit, uint32_t v, uint32_t driven);
static int key_analog_drive(uint32_t col, uint32_t rowbit);
#define FM1_INPUT_ANALOG(col, rowbit, v, driven) key_analog(col, rowbit, v, driven)
#define FM1_INPUT_DRIVE(col, rowbit) key_analog_drive(col, rowbit)
#endif
#include "platform.c"
#ifndef DINGHY_HOST
#include "sense.c"
#endif
#endif
#include "sc_synth.h"
#include "sc_presets.h"
#ifndef SC_RENDER                               /* the platform may render otherwise (fm1-dock's module: the voices on two cores) */
#define SC_RENDER(s, out, n) sc_render(s, out, n)
#endif

/* ---- MIDI from the DIN jack: UART1 DMAs into a ring filled with 0xFF (a byte MIDI never sends here:
 * it is a reset), read by content (hal/fm1_uart.h; as fm1-harbor). Channel messages only. */
#if !defined(DINGHY_HOST) && !defined(HB_MODULE) /* (in harbor the DIN jack is the host's: midi()) */
#include "fm1_uart.h"
#define DIN_RING 256u
static volatile uint8_t din_ring[DIN_RING] __attribute__((aligned(16)));
static uint32_t din_r;
static void din_init(void)
{
    uint32_t i;
    for (i = 0; i < DIN_RING; i++)
        din_ring[i] = 0xFFu;
    fm1_uart1_midi_init(din_ring, DIN_RING);
}
/* The UART latched (RDC) and its pendings cleared every TIMER5 poll, as Dinghy 0.4's midi_uart.c does
 * (fm1_uart1_rx_take): fm1-dock, which did not, was reported lagging badly on notes from the jack where
 * Dinghy's OberVave played them at once (fm1-emu cannot tell: its UART DMA writes each byte as it lands).
 * The ring is still read by content, in the main loop (din_get). */
static void din_poll(void)
{
    fm1_uart1_rx_take();
}
static int din_get(uint8_t *st, uint8_t *d1, uint8_t *d2)
{
    static struct { uint8_t st, n, d[2]; } din;     /* running status, the data bytes so far */
    for (;;) {
        uint32_t b = din_ring[din_r];
        if (b == 0xFFu)
            return 0;
        din_ring[din_r] = 0xFFu;
        din_r = (din_r + 1u) % DIN_RING;
        if (b >= 0xF8u)
            continue;                           /* real time */
        if (b & 0x80u) {                        /* a status; system common and SysEx end running status */
            din.st = b < 0xF0u ? (uint8_t)b : 0;
            din.n = 0;
            continue;
        }
        if (!din.st)
            continue;                           /* SysEx data, or data with no status */
        din.d[din.n++] = (uint8_t)b;
        if (din.n == ((din.st & 0xE0u) == 0xC0u ? 1u : 2u)) {   /* Cn / Dn take one data byte */
            *st = din.st;
            *d1 = din.d[0];
            *d2 = din.n > 1u ? din.d[1] : 0;
            din.n = 0;
            return 1;
        }
    }
}
#else
static void din_init(void) {}
static int din_get(uint8_t *st, uint8_t *d1, uint8_t *d2) { (void)st; (void)d1; (void)d2; return 0; }
#endif

/* ---- the sound (the audio ISR) */
#if defined(DINGHY_HOST) || defined(HB_MODULE)  /* (a Mach-O section needs "segment,section", as gfx.c's) */
static sc_synth_t syn;
#else
static sc_synth_t syn __attribute__((section(".pool")));   /* 80 KB with the reverb's lines: not in .bss's 96 */
#endif
static sc_patch_t staged;                       /* a preset on its way in: the main loop writes it, then load_req */
static volatile uint32_t load_req, ready;       /* main -> ISR / ISR -> main */
static sc_perf_t perf;                          /* the joystick section: not in the patch, kept across sounds */
static volatile uint32_t cpu_pub;               /* the slowest render of the last second, in fm1_ticks */
static volatile int32_t cc7_q12 = 4096;         /* MIDI CC 7, the level after the synth: (v / 127)^2, Q12 */
#define BLOCK_TICKS (HALF_FRAMES * 1000000u / FS * FM1_TICKS_PER_US)   /* 5.8 ms of ticks: 100 % */

static void app_audio(int32_t *out, uint32_t n)
{
    static uint32_t worst, blocks;
    uint32_t t0, dt;
    if (load_req) {                             /* a whole new sound, between two blocks, with the stick as it is */
        sc_init(&syn, &staged);
        syn.perf = perf;
        sc_reprep(&syn);
        load_req = 0;
        ready = 1;
    }
    if (!ready)
        return;
    t0 = fm1_ticks();
    SC_RENDER(&syn, out, n);
    {
        static int32_t was = 4096;              /* the last block's CC 7 level; a change is spread over the block */
        int32_t g1 = cc7_q12;
        if (g1 != 4096 || was != 4096) {        /* (at 127 the sound is the engine's, bit for bit) */
            int32_t gq = was << 8, dg = ((g1 - was) << 8) / (int32_t)n, g;   /* gq: Q20 */
            uint32_t i;
            for (i = 0; i < n; i++) {
                gq += dg;
                g = i + 1u < n ? gq >> 8 : g1;
                out[2u * i] = (out[2u * i] * g) >> 12;
                out[2u * i + 1u] = (out[2u * i + 1u] * g) >> 12;
            }
            was = g1;
        }
    }
    dt = fm1_ticks() - t0;
    if (dt > worst) worst = dt;
    if (++blocks >= FS / HALF_FRAMES) { cpu_pub = worst; worst = 0; blocks = 0; }
}

/* ---- the parameters: a page is four, one per knob */
enum { W_LAYER, W_PATCH, W_PERF, W_SET };       /* where the field is: each layer's, the patch's, the joystick's,
                                                 * the settings' (set) */
enum { K_LIN, K_EXP, K_ENUM, K_CHORUS, K_REVERB, K_INT };   /* Q16 by steps, Q16 by ratios (times, rates), int8
                                                 * choices; CHORUS and REVERB: an int8 on and its mode, OFF first;
                                                 * INT: an int8 by ones */
enum { U_NUM, U_SEC, U_HZ, U_CENT, U_ST, U_PCT };
#define NO_OFF 0xFFFFu
typedef struct {
    const char *name;
    uint8_t where, kind, unit;
    uint16_t off, off2;                         /* off2: a second field that follows (LFO 1 to both oscillators) */
    int32_t lo, hi, step;                       /* K_LIN/K_EXP: Q16; K_ENUM: the choices */
    const char *const *labels;
} prm_t;

static const char *const L_WAVE[] = {"TRI", "SAW", "SQR", "SIN", "NOISE"};
static const char *const L_OCT[] = {"16'", "8'", "4'", "2'", "1'"};
static const char *const L_OFFON[] = {"OFF", "ON"};
static const char *const L_FLT[] = {"LP24", "LP12", "BP12", "BP6", "HP12"};
static const char *const L_LFO[] = {"TRI", "SQR", "S&H", "RND", "RAMP", "RAMP-", "SQR+"};
static const char *const L_GLIDE[] = {"OFF", "GLIDE", "PORTA"};
static const char *const L_KEYS[] = {"POLY", "MONO", "UNISON"};
static const char *const L_CHO[] = {"OFF", "1", "2", "3"};
static const char *const L_REV[] = {"OFF", "PLATE", "ROOM", "HALL", "GALACTIC"};

typedef struct {                                /* what survives a power-off (0.1's was the first three) */
    uint8_t preset, page;
    int8_t oct;
    int8_t at_on, at_depth, at_floor, vel_on;   /* SETTINGS: aftertouch, its depth (%) and floor; velocity */
} set_t;
static set_t set;

#define LO(f) ((uint16_t)__builtin_offsetof(sc_layer_t, f))
#define PO(f) ((uint16_t)__builtin_offsetof(sc_patch_t, f))
#define FO(f) ((uint16_t)__builtin_offsetof(sc_perf_t, f))
#define SO(f) ((uint16_t)__builtin_offsetof(set_t, f))
#define Q(x) SC_Q16(x)
#define LIN(n, f, lo, hi, st, u) { n, W_LAYER, K_LIN, u, LO(f), NO_OFF, Q(lo), Q(hi), Q(st), 0 }
#define EXP(n, f, lo, hi, u) { n, W_LAYER, K_EXP, u, LO(f), NO_OFF, Q(lo), Q(hi), Q(lo), 0 }
#define ENUM(n, f, lo, hi, l) { n, W_LAYER, K_ENUM, U_NUM, LO(f), NO_OFF, lo, hi, 1, l }
#define ADSR(e) EXP("ATTACK", e.a, 0.001, 15, U_SEC), EXP("DECAY", e.d, 0.001, 15, U_SEC), \
                LIN("SUSTAIN", e.s, 0, 1, 0.01, U_NUM), EXP("RELEASE", e.r, 0.001, 15, U_SEC)

/* The ranges take in the Synthex's own (synthex-web's tools/synthex-rom/gen_factory.py maps the ROM
 * presets' knobs to the engine): ENV +-3.08, cutoff -0.26..1.48 (the knob with the key tracking
 * folded in; the engine clamps the note to 0..132), OSC2 detune +-1250 cents (the transposes folded
 * in), envelope stages to 13.2 s, LFO 0.07..27.7 Hz, glide -32..31 st, 5 ms..1 s. */
static const prm_t PAGE[][4] = {
    { LIN("CUTOFF", cutoff, -0.3, 1.5, 0.01, U_NUM), LIN("RESO", reso, 0, 1, 0.01, U_NUM),
      LIN("ENV", env_amt, -3.1, 3.1, 0.01, U_NUM), EXP("RELEASE", env_a.r, 0.001, 15, U_SEC) },
    { ENUM("WAVE", o1_wave, 0, 4, L_WAVE), ENUM("OCTAVE", o1_oct, -2, 2, L_OCT),
      LIN("PW", o1_pwm, 0.05, 0.95, 0.01, U_NUM), LIN("LEVEL", mix_o1, 0, 1, 0.01, U_NUM) },
    { ENUM("WAVE", o2_wave, 0, 4, L_WAVE), ENUM("OCTAVE", o2_oct, -2, 2, L_OCT),
      LIN("DETUNE", o2_detune, -1250, 1250, 1, U_CENT), LIN("LEVEL", mix_o2, 0, 1, 0.01, U_NUM) },
    { LIN("NOISE", mix_noise, 0, 1, 0.01, U_NUM), ENUM("SYNC", o2_sync, 0, 1, L_OFFON),
      ENUM("RING", ring, 0, 1, L_OFFON), LIN("X-MOD", cross, 0, 1, 0.01, U_NUM) },
    { ENUM("MODE", flt_mode, 0, 4, L_FLT), LIN("CUTOFF", cutoff, -0.3, 1.5, 0.01, U_NUM),
      LIN("ENV", env_amt, -3.1, 3.1, 0.01, U_NUM), LIN("KEYS", key_track, 0, 1, 0.01, U_NUM) },
    { ADSR(env_f) },
    { ADSR(env_a) },
    { EXP("RATE", lfo[0].rate, 0.05, 30, U_HZ), ENUM("SHAPE", lfo[0].shape, 0, 6, L_LFO),
      { "PITCH", W_LAYER, K_LIN, U_NUM, LO(mm[SC_MM_L1_P1]), LO(mm[SC_MM_L1_P2]), 0, Q(1), Q(0.01), 0 },
      LIN("CUTOFF", mm[SC_MM_L1_CUT], 0, 1, 0.01, U_NUM) },
    { EXP("RATE", lfo[1].rate, 0.05, 30, U_HZ), ENUM("SHAPE", lfo[1].shape, 0, 6, L_LFO),
      { "PITCH", W_PERF, K_LIN, U_NUM, FO(lfo2_osc), NO_OFF, 0, Q(1), Q(0.01), 0 },
      { "CUTOFF", W_PERF, K_LIN, U_NUM, FO(lfo2_filt), NO_OFF, 0, Q(1), Q(0.01), 0 } },
    { ENUM("MODE", glide_mode, 0, 2, L_GLIDE), LIN("AMOUNT", glide_amt, -32, 31, 1, U_ST),
      EXP("SPEED", glide_time, 0.005, 10, U_SEC), ENUM("KEYS", key_assign, 0, 2, L_KEYS) },
    { { "TOUCH", W_SET, K_ENUM, U_NUM, SO(at_on), NO_OFF, 0, 1, 1, L_OFFON },
      { "DEPTH", W_SET, K_INT, U_PCT, SO(at_depth), NO_OFF, 0, 100, 1, 0 },
      { "FLOOR", W_SET, K_INT, U_NUM, SO(at_floor), NO_OFF, 0, 40, 1, 0 },
      { "VELO", W_SET, K_ENUM, U_NUM, SO(vel_on), NO_OFF, 0, 1, 1, L_OFFON } },
    { { "CHORUS", W_PATCH, K_CHORUS, U_NUM, PO(fx.chorus.on), NO_OFF, 0, 3, 1, L_CHO },
      { "VOLUME", W_PATCH, K_LIN, U_NUM, PO(volume), NO_OFF, 0, Q(1), Q(0.01), 0 },
      { "UNISON", W_PATCH, K_LIN, U_NUM, PO(unison_detune), NO_OFF, 0, Q(1), Q(0.01), 0 },
      { "TUNE", W_PATCH, K_LIN, U_ST, PO(tune), NO_OFF, Q(-12), Q(12), Q(1), 0 } },
    { { "REVERB", W_PATCH, K_REVERB, U_NUM, PO(fx.reverb.on), NO_OFF, 0, 4, 1, L_REV },
      { "SIZE", W_PATCH, K_LIN, U_NUM, PO(fx.reverb.size), NO_OFF, 0, Q(1), Q(0.01), 0 },
      { "DAMP", W_PATCH, K_LIN, U_NUM, PO(fx.reverb.damping), NO_OFF, 0, Q(1), Q(0.01), 0 },
      { "MIX", W_PATCH, K_LIN, U_NUM, PO(fx.reverb.mix), NO_OFF, 0, Q(1), Q(0.01), 0 } },
    { { "STICK X", W_PERF, K_LIN, U_NUM, FO(joy_x), NO_OFF, Q(-1), Q(1), Q(0.02), 0 },
      { "STICK Y", W_PERF, K_LIN, U_NUM, FO(joy_y), NO_OFF, Q(-1), Q(1), Q(0.02), 0 },
      { "BEND OSC", W_PERF, K_LIN, U_NUM, FO(bend_osc), NO_OFF, 0, Q(1), Q(0.01), 0 },
      { "BEND FLT", W_PERF, K_LIN, U_NUM, FO(bend_filt), NO_OFF, 0, Q(1), Q(0.01), 0 } },
};
#define N_PAGES (sizeof PAGE / sizeof PAGE[0])
static const char *const PAGE_NAME[N_PAGES] = {
    "HOME", "OSC 1", "OSC 2", "MIX", "FILTER", "FILTER ENV", "AMP ENV", "LFO 1", "LFO 2", "GLIDE + KEYS",
    "SETTINGS", "OUTPUT", "REVERB", "JOYSTICK"};
/* the buttons that pick pages: each opens its first page, and steps through its group when pressed again */
static const struct { uint8_t btn, first, n; } GROUP[] = {
    {BTN_HOME, 0, 1}, {BTN_EDIT, 1, 3}, {BTN_ENV, 4, 3}, {BTN_LFO, 7, 2}, {BTN_GLO, 9, 2}, {BTN_FX, 11, 2},
    {BTN_SEL, 13, 1}};
#define N_GROUPS (sizeof GROUP / sizeof GROUP[0])
#define PG_STICK 13u

static int32_t *field32(int layer, const prm_t *p, uint16_t off)
{
    uint8_t *b = p->where == W_LAYER ? (uint8_t *)&syn.patch.layer[layer]
               : p->where == W_PATCH ? (uint8_t *)&syn.patch : p->where == W_SET ? (uint8_t *)&set
               : (uint8_t *)&syn.perf;
    return (int32_t *)(b + off);
}
static int8_t *field8(int layer, const prm_t *p) { return (int8_t *)field32(layer, p, p->off); }

static int32_t prm_get(const prm_t *p)          /* the value shown: the upper layer's */
{
    if (p->kind == K_ENUM || p->kind == K_INT) return *field8(0, p);
    if (p->kind == K_CHORUS) { int8_t *c = field8(0, p); return c[0] ? c[1] : 0; }
    if (p->kind == K_REVERB) { int8_t *c = field8(0, p); return c[0] ? c[1] + 1 : 0; }
    return *field32(0, p, p->off);
}

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* turn a parameter by d detents (acc: steps a detent, by the knob's speed); each layer moves from its own value */
static void prm_turn(const prm_t *p, int32_t d, int32_t acc)
{
    int layers = p->where == W_LAYER && syn.patch.voice_mode != SC_VM_SINGLE ? 2 : 1, k;
    for (k = 0; k < layers; k++) {
        if (p->kind == K_ENUM || p->kind == K_INT) {
            int8_t *f = field8(k, p);
            *f = (int8_t)clamp32(*f + d * (p->kind == K_INT ? acc : 1), p->lo, p->hi);
        } else if (p->kind == K_CHORUS) {
            int8_t *c = field8(k, p);
            int32_t v = clamp32((c[0] ? c[1] : 0) + d, 0, 3);
            c[1] = (int8_t)(v ? v : c[1]);
            c[0] = (int8_t)(v != 0);
        } else if (p->kind == K_REVERB) {
            int8_t *c = field8(k, p);
            int32_t v = clamp32((c[0] ? c[1] + 1 : 0) + d, 0, 4);
            c[1] = (int8_t)(v ? v - 1 : c[1]);
            c[0] = (int8_t)(v != 0);
        } else {
            int32_t *f = field32(k, p, p->off), v = *f, i;
            if (p->kind == K_LIN)
                v = clamp32(v + d * p->step * acc, p->lo, p->hi);
            else
                for (i = 0; i < (d < 0 ? -d : d); i++) {      /* ~6 % a detent: a time or a rate by ratios */
                    int32_t s = v >> 4 > p->step ? v >> 4 : p->step;
                    v = clamp32(d > 0 ? v + s : v - s, p->lo, p->hi);
                }
            *f = v;
            if (p->off2 != NO_OFF) *field32(k, p, p->off2) = v;
        }
    }
}

static void fmt_dec(char *b, int32_t v, int dec)   /* fmt_fix with the 0 before the point: "0.50", not ".50" */
{
    fmt_fix(b, v, dec);
    if (b[0] == '.') {
        uint32_t n = str_len(b) + 1u;
        while (n--) b[n + 1u] = b[n];
        b[0] = '0';
    }
}

/* the value as text: "0.44", "-0.30", "250ms", "1.25s", "5.0Hz", "+700c", "+12st" */
static void prm_text(char *b, const prm_t *p, int32_t v)
{
    int32_t a = v < 0 ? -v : v, i = 0;
    if (p->kind == K_ENUM || p->kind == K_CHORUS || p->kind == K_REVERB) {
        int32_t j = v - (p->kind == K_ENUM ? p->lo : 0);
        str_cpy(b, p->labels[j], 12);
        return;
    }
    if (p->kind == K_INT) {
        fmt_int(b, v);
        if (p->unit == U_PCT) str_cpy(b + str_len(b), "%", 4);
        return;
    }
    if (v < 0) b[i++] = '-';
    else if (p->unit == U_CENT || p->unit == U_ST || (p->lo < 0 && v > 0)) b[i++] = '+';
    switch (p->unit) {
    case U_SEC:
        if (a < Q(0.1)) { fmt_int(b + i, (a * 1000 + 32768) >> 16); str_cpy(b + str_len(b), "ms", 4); }
        else if (a < Q(10)) { fmt_dec(b + i, (int32_t)(((int64_t)a * 100 + 32768) >> 16), 2); str_cpy(b + str_len(b), "s", 4); }
        else { fmt_dec(b + i, (a * 10 + 32768) >> 16, 1); str_cpy(b + str_len(b), "s", 4); }
        break;
    case U_HZ:
        fmt_dec(b + i, (int32_t)(((int64_t)a * (a < Q(10) ? 100 : 10) + 32768) >> 16), a < Q(10) ? 2 : 1);
        str_cpy(b + str_len(b), "Hz", 4);
        break;
    case U_CENT: fmt_int(b + i, (a + 32768) >> 16); str_cpy(b + str_len(b), "c", 4); break;
    case U_ST: fmt_int(b + i, (a + 32768) >> 16); str_cpy(b + str_len(b), "st", 4); break;
    default: fmt_dec(b + i, (int32_t)(((int64_t)a * 100 + 32768) >> 16), 2); break;
    }
}

static int32_t log2_q4(int32_t v)               /* log2 in 1/16ths, enough for a bar */
{
    uint32_t u = (uint32_t)(v > 0 ? v : 1);
    int32_t e = 31 - __builtin_clz(u);          /* one instruction (and fm1-emu runs it from 2026-10-15) */
    return e * 16 + (int32_t)((u << (31 - e) >> 27) & 15u);
}

static int32_t prm_bar(const prm_t *p, int32_t v, int32_t w)   /* 0..w: where the value sits in its range */
{
    int32_t n, d;
    if (p->kind == K_EXP) { n = log2_q4(v) - log2_q4(p->lo); d = log2_q4(p->hi) - log2_q4(p->lo); }
    else if (p->kind == K_LIN) { n = (v - p->lo) >> 8; d = (p->hi - p->lo) >> 8; }
    else { n = v - (p->kind == K_ENUM ? p->lo : 0); d = p->hi - (p->kind == K_ENUM ? p->lo : 0); }
    return d > 0 ? clamp32(n * w / d, 0, w) : 0;
}

/* ---- notes (main loop) */
static uint8_t sounding[128];                   /* notes on, from the keys and from MIDI */
static int32_t note_ms;                         /* when a note last started or stopped */

static void note_on(uint32_t note, uint32_t vel)
{
    if (note > 127u || !vel) return;
    sc_post(&syn, SC_EV_NOTE_ON, (uint8_t)note, (int32_t)(vel * 516u));   /* 516 = 65536 / 127 */
    if (sounding[note] < 255u) sounding[note]++;
    note_ms = (int32_t)fm1_ms;
}
static void note_off(uint32_t note)
{
    if (note > 127u) return;
    sc_post(&syn, SC_EV_NOTE_OFF, (uint8_t)note, 0);
    sounding[note] = 0;
    note_ms = (int32_t)fm1_ms;
}
static int quiet(void)                          /* nothing sounds, not even a release */
{
    int k, i;
    for (k = 0; k < 2; k++)
        for (i = 0; i < SC_VOICES; i++)
            if (!sc_voice_idle(&syn.layer[k].v[i])) return 0;
    return 1;
}

/* a preset in, whole: the audio interrupt takes it at its next block (nothing waits for it, so it also
 * works before the audio runs, as in a harbor module's init). Notes and edits posted before then are lost
 * with the old sound. */
static void load(uint32_t i)
{
    uint32_t n;
    load_req = 0;                               /* (the interrupt leaves staged alone while it is written) */
    __asm__ volatile("" ::: "memory");
    staged = SC_PRESETS[i % SC_N_PRESETS];
    __asm__ volatile("" ::: "memory");
    load_req = 1;
    for (n = 0; n < 128u; n++) sounding[n] = 0;
}

/* ---- the screen */
#define C_ACC RGB(255, 150, 60)                 /* the Synthex's orange */
#define C_KEY RGB(210, 210, 210)
#define C_GREY RGB(110, 110, 110)
#define C_BAR RGB(60, 60, 60)
#define KEY0 53u                                /* key 0 = F3 */
#define Y_HEAD 0
#define Y_NAME 26
#define Y_PAGE 66
#define Y_PRM 88
#define Y_STICK 150
#define Y_KEYS 174
#define Y_OCT 222
static int black(uint32_t note) { return (0x54Au >> (note % 12u)) & 1u; }

static void draw_head(uint32_t preset, int edited, uint32_t cpu)
{
    char s[24], t[12];
    uint32_t a = SC_PRESET_ADDR[preset];
    cv_begin(240, 20, C_BLACK);
    str_cpy(s, a >= 100u ? "MEM " : "PRE ", 8);
    fmt_int(s + 4, (int32_t)(a % 100u));
    if (edited) str_cpy(s + str_len(s), " *", 4);
    cv_text(4, 2, &FONT_S, s, C_ACC);
    str_cpy(s, "CPU ", 8);
    fmt_int(t, (int32_t)cpu);
    str_cpy(s + 4, t, 8);
    str_cpy(s + str_len(s), "%", 4);
    cv_text(236 - text_w(&FONT_S, s), 2, &FONT_S, s, cpu >= 70u ? RGB(255, 60, 60) : C_GREY);
    cv_blit(0, Y_HEAD);
}

static void draw_name(uint32_t preset)
{
    char s[32];
    uint32_t i;
    for (i = 0; i < 31u && SC_PRESETS[preset].name[i]; i++) {   /* FONT_L has capitals only */
        char c = SC_PRESETS[preset].name[i];
        s[i] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
    }
    s[i] = 0;
    lcd_fill(0, Y_NAME, 240, 34, C_BLACK);
    if (text_w(&FONT_L, s) <= 236)
        draw_text_box(0, Y_NAME, 240, &FONT_L, s, C_WHITE, 1);
    else
        draw_text_box(0, Y_NAME + 8, 240, &FONT_S, SC_PRESETS[preset].name, C_WHITE, 1);
}

static void draw_page(uint32_t page)
{
    draw_text_box(0, Y_PAGE, 240, &FONT_S, PAGE_NAME[page], C_ACC, 1);
}

static void draw_prm(uint32_t col, const prm_t *p, int32_t v)   /* one knob's column, 60 x 56 */
{
    char s[16];
    int32_t w;
    cv_begin(60, 56, C_BLACK);
    cv_text(30 - text_w(&FONT_S, p->name) / 2, 0, &FONT_S, p->name, C_GREY);
    prm_text(s, p, v);
    cv_text(30 - text_w(&FONT_S, s) / 2, 20, &FONT_S, s, C_WHITE);
    cv_rect(6, 44, 48, 6, C_BAR);
    if (p->kind == K_LIN && p->lo < 0) {        /* bipolar: from the middle */
        w = prm_bar(p, v, 48) - 24;
        cv_rect(w < 0 ? 30 + w : 30, 44, w < 0 ? -w : w > 0 ? w : 1, 6, C_ACC);
    } else {
        w = prm_bar(p, v, 48);
        cv_rect(6, 44, w ? w : 1, 6, C_ACC);
    }
    cv_blit((int32_t)col * 60, Y_PRM);
}

static void draw_stick(void)
{
    char s[40], t[12];
    str_cpy(s, "STICK X ", 12);
    prm_text(t, &PAGE[PG_STICK][0], syn.perf.joy_x);   /* (where it is: the aftertouch moves it) */
    str_cpy(s + str_len(s), t, 12);
    str_cpy(s + str_len(s), "   Y ", 8);
    prm_text(t, &PAGE[PG_STICK][1], perf.joy_y);
    str_cpy(s + str_len(s), t, 12);
    draw_text_box(0, Y_STICK, 240, &FONT_S, s, C_GREY, 1);
}

static void draw_keys(uint32_t lit)             /* bit k: key k sounds */
{
    uint32_t k, w = 0;
    cv_begin(240, 44, C_BLACK);
    for (k = 0; k < 27u; k++)                   /* 16 white keys of 14 px from x = 8 */
        if (!black(KEY0 + k)) {
            cv_rect(8 + (int32_t)w * 14, 0, 13, 44, lit >> k & 1u ? C_ACC : C_KEY);
            w++;
        }
    for (k = 0, w = 0; k < 27u; k++)
        if (black(KEY0 + k))
            cv_rect(8 + (int32_t)w * 14 - 5, 0, 9, 27, lit >> k & 1u ? C_ACC : C_BLACK);
        else
            w++;
    cv_blit(0, Y_KEYS);
}

static void draw_oct(int32_t oct)
{
    char s[12] = "OCTAVE +0";
    s[7] = oct < 0 ? '-' : '+';
    s[8] = (char)('0' + (oct < 0 ? -oct : oct));
    draw_text_box(0, Y_OCT, 240, &FONT_S, s, C_GREY, 1);
}

/* ---- the main loop: app_init() once, then app_loop() over and over (Dinghy's app_main, or a harbor
 * module's init() and draw()) */
static uint32_t prev, dirty, t_change, edited, patch_dirty;
static uint32_t lit_drawn, page_drawn, preset_drawn, edited_drawn, cpu_drawn;
static int32_t oct_drawn, shown[4], stick_drawn[2];
static uint32_t knob_ms[4], knob_acc[4];
static uint8_t held[27];                        /* the note each key started */

/* ---- the keys' force and aftertouch. key_vel / key_press: a key's velocity (its last landing) and its
 * pressure now, 0..127. Here the key scan senses the bottom two rows' force (Velo's sense.c, as fm1-harbor's
 * host does, with its settings: DRIVE 2, curve 0.7): such a key plays from its landing, not from its
 * contact, and the top row at 100. In a harbor module the host senses and the module's ui() fills them.
 * The hardest press (a key's, MIDI's channel pressure, or its poly pressure on a note that sounds) pushes
 * the stick left beyond where it is set (vibrato: LFO 2 into the pitch, as CC1 and the Synthex's stick),
 * and it springs back as the press eases. VoiceCore has no aftertouch (nor had the Synthex): the engine only
 * sees the stick move, so its output is VoiceCore's for that stick. Both are off until SETTINGS turns them
 * on (set.at_on, set.vel_on): vibrato does not suit every sound. */
static uint8_t key_vel[27], key_press[27], press_sent[27];
static uint8_t at_chan, at_poly[128], at_any_poly;
static uint32_t t_press;
#if !defined(DINGHY_HOST) && !defined(HB_MODULE)
static uint32_t sense_mask, sense_held;         /* bit k: key k is on an analog row / has landed */
static void key_analog(uint32_t col, uint32_t rowbit, uint32_t v, uint32_t driven)   /* (the scan's ISR) */
{
    int id = FM1_KEYMAP[rowbit][col];
    if (id >= 14)
        sense_sample((uint32_t)id - 14u, v, fm1_ticks(), (int)driven);
}
static int key_analog_drive(uint32_t col, uint32_t rowbit)
{
    int id = FM1_KEYMAP[rowbit][col];
    return id >= 14 && sense_want_drive((uint32_t)id - 14u);
}
static void keys_init(void)
{
    uint32_t k;
    for (k = 0; k < FM1_NCOL; k++) {
        if (FM1_KEYMAP[3][k] >= 14) sense_mask |= 1u << (FM1_KEYMAP[3][k] - 14);
        if (FM1_KEYMAP[4][k] >= 14) sense_mask |= 1u << (FM1_KEYMAP[4][k] - 14);
    }
    sense_init(sense_mask);
    sn_cfg.drive = 2;                           /* fm1-harbor's sense_defaults(): DRIVE 2, curve 0.7, ceiling 1000 */
    sn_cfg.d_curve = 70;
    sn_cfg.d_ceil = 1000;
    sense_cal_reset();
    fm1_an.drive = 2;
    fm1_an.con = (uint16_t)FM1_ADC_CON(7, 2);   /* (CS-1's: divider 7, delay 2, as measured) */
}
static uint32_t keys_now(void)                  /* bit k: key k is down (a sensed one from its landing) */
{
    sense_ev_t e;
    while (sense_take(&e)) {
        if (e.key >= 27u || !(sense_mask >> e.key & 1u))
            continue;
        if (e.type == EV_ON) {
            sense_held |= 1u << e.key;
            key_vel[e.key] = e.vel ? e.vel : 1u;
        } else if (e.type == EV_OFF) {
            sense_held &= ~(1u << e.key);
        }
    }
    return (fm1_in.notes & ~sense_mask) | sense_held;
}
static void keys_pressure(uint32_t keys)
{
    uint32_t k;
    for (k = 0; k < 27u; k++)
        if (sense_mask >> k & 1u)
            key_press[k] = keys >> k & 1u ? sense_pressure(k) : 0;
}
#else
static void keys_init(void) {}
static uint32_t keys_now(void) { return fm1_in.notes; }
static void keys_pressure(uint32_t keys) { (void)keys; }   /* (a module's ui() sets key_press) */
#endif

/* 100 times a second: a move of the stick costs the audio interrupt both layers' prep (SC_EV_PERF), and
 * a held key's reading has a count or two of noise, so pressure under 8 is none and one within 2 of the
 * one applied stays put (but 0 and 127 are reached) */
static uint8_t at_applied;
static uint32_t perf_dirty;
static void aftertouch(uint32_t keys)
{
    uint32_t k, p = 0, f;
    int32_t x;
    if (fm1_ms - t_press < 10u)
        return;
    t_press = fm1_ms;
    keys_pressure(keys);
    for (k = 0; k < 27u; k++)                   /* the keys' pressure out, as poly pressure */
        if (set.at_on && keys >> k & 1u && key_press[k] != press_sent[k]) {
            press_sent[k] = key_press[k];
            midi_send(0xA0, held[k], key_press[k]);
        }
    for (k = 0; k < 27u; k++)
        if (keys >> k & 1u && key_press[k] > p) p = key_press[k];
    f = (uint32_t)set.at_floor;                 /* a resting finger (~3 counts) is none: any vibrato costs the */
    p = p < f ? 0u : (p - f) * 127u / (127u - f);   /* voices their per-sample pitch path (Ring mod. x8: 53 -> 63 %) */
    if (at_chan > p) p = at_chan;               /* MIDI's pressure as it comes */
    if (at_any_poly)
        for (k = 0; k < 128u; k++)
            if (at_poly[k] > p && sounding[k]) p = at_poly[k];
    if (!set.at_on) p = 0;                      /* (SETTINGS: TOUCH off) */
    if (p + 2u >= at_applied && p <= at_applied + 2u && p != 0u && p != 127u)
        p = at_applied;
    at_applied = (uint8_t)p;
    x = -(int32_t)(p * 516u * (uint32_t)set.at_depth / 100u);   /* 0..127 -> 0..-DEPTH, Q16, as CC1 */
    if (x > perf.joy_x) x = perf.joy_x;         /* (never less left than the stick is set) */
    if (x != syn.perf.joy_x) {
        syn.perf.joy_x = x;
        perf_dirty = 1;
    }
}

static void redraw_all(void)                    /* the whole screen again, from black */
{
    lit_drawn = page_drawn = preset_drawn = edited_drawn = cpu_drawn = ~0u;
    oct_drawn = 99;
    stick_drawn[0] = stick_drawn[1] = -1;
    lcd_fill(0, 0, 240, 240, C_BLACK);
}

static void app_init(void)
{
    uint32_t k;
    set.at_depth = 100;
    set.at_floor = 8;
    if (settings_load(&set, sizeof set) && !settings_load(&set, 3))   /* 0.1's: no SETTINGS page yet */
        if (set.page >= 10u) set.page++;
    if (set.preset >= SC_N_PRESETS) set.preset = 0;
    if (set.page >= N_PAGES) set.page = 0;
    if (set.oct < -2 || set.oct > 2) set.oct = 0;
    if (set.at_on != 1) set.at_on = 0;
    if (set.vel_on != 1) set.vel_on = 0;
    if (set.at_depth < 0 || set.at_depth > 100) set.at_depth = 100;
    if (set.at_floor < 0 || set.at_floor > 40) set.at_floor = 8;
    sc_perf_default(&perf);
    for (k = 0; k < 27u; k++) key_vel[k] = 100;
    keys_init();
    load(set.preset);
    redraw_all();
}

static void app_loop(void)
{
    uint32_t keys, down, up, rel, b, lit = 0, base = (uint32_t)(KEY0 + 12 * set.oct), cpu, k;
    int32_t d;
    uint8_t st, d1, d2;
    platform_poll();

    keys = keys_now();                      /* bit k: key k (0 = F3 .. 26 = G5) */
    down = keys & ~prev;
    up = prev & ~keys;
    prev = keys;
    for (k = 0; k < 27u; k++) {             /* a key stops the note it started (the octave may change meanwhile) */
        if (down >> k & 1u) {
            held[k] = (uint8_t)(base + k);
            key_press[k] = press_sent[k] = 0;
            note_on(held[k], set.vel_on ? key_vel[k] : 100u);
            midi_send(0x90, held[k], set.vel_on ? key_vel[k] : 100u);
        }
        if (up >> k & 1u) { note_off(held[k]); midi_send(0x80, held[k], 0); key_press[k] = 0; }
    }

    b = fm1_input_edges(&rel);
    if (b & (1u << BTN_OCT_DOWN) && set.oct > -2) { set.oct--; dirty = 1; t_change = fm1_ms; }
    if (b & (1u << BTN_OCT_UP) && set.oct < 2) { set.oct++; dirty = 1; t_change = fm1_ms; }
    for (k = 0; k < N_GROUPS; k++)
        if (b & (1u << GROUP[k].btn)) {
            uint32_t in = set.page >= GROUP[k].first && set.page < GROUP[k].first + GROUP[k].n;
            if (in && GROUP[k].first == PG_STICK) {          /* SEL on the joystick page: centre it */
                perf.joy_x = perf.joy_y = 0;
                syn.perf.joy_x = syn.perf.joy_y = 0;
                perf_dirty = 1;
            }
            set.page = (uint8_t)(in ? GROUP[k].first + (set.page - GROUP[k].first + 1u) % GROUP[k].n : GROUP[k].first);
            dirty = 1; t_change = fm1_ms;
        }

    if ((d = fm1_enc_take(ENC_PRESETS)) != 0) {
        set.preset = (uint8_t)(((int32_t)set.preset + d % SC_N_PRESETS + SC_N_PRESETS) % SC_N_PRESETS);
        load(set.preset);
        edited = 0;
        dirty = 1; t_change = fm1_ms;
    }
    for (k = 0; k < 4u; k++)
        if ((d = fm1_enc_take(ENC_KNOB1 + k)) != 0) {
            const prm_t *p = &PAGE[set.page][k];
            knob_acc[k] = fm1_ms - knob_ms[k] < 60u ? (knob_acc[k] < 15u ? knob_acc[k] + 1u : 15u) : 1u;
            knob_ms[k] = fm1_ms;
            prm_turn(p, d, (int32_t)(1u + knob_acc[k] / 4u));   /* turned fast: up to 4 steps a detent */
            if (p->where == W_SET) { dirty = 1; t_change = fm1_ms; continue; }   /* (the sound is not touched) */
            if (p->where == W_PERF) *(int32_t *)((uint8_t *)&perf + p->off) = *field32(0, p, p->off);
            else edited = 1;
            patch_dirty = 1;
        }
    for (k = 0; k < 2u; k++)                /* SELECT: the stick up / down; ALGORITHM: left / right */
        if ((d = fm1_enc_take(k ? ENC_ALGORITHM : ENC_SELECT)) != 0) {
            const prm_t *p = &PAGE[PG_STICK][k ? 0 : 1];
            prm_turn(p, d, 2);
            *(int32_t *)((uint8_t *)&perf + p->off) = *field32(0, p, p->off);
            perf_dirty = 1;
        }

    while (midi_get(&st, &d1, &d2) || din_get(&st, &d1, &d2)) {   /* USB-MIDI, then the DIN jack */
        uint32_t ty = st & 0xF0u;
        if (ty == 0x90u && d2) { at_poly[d1 & 127u] = 0; note_on(d1, d2); }
        else if (ty == 0x80u || ty == 0x90u) { at_poly[d1 & 127u] = 0; note_off(d1); }
        else if (ty == 0xD0u) at_chan = d1;                 /* channel pressure */
        else if (ty == 0xA0u) { at_poly[d1 & 127u] = d2; at_any_poly = 1; }   /* poly pressure */
        else if (ty == 0xE0u) sc_post(&syn, SC_EV_BEND, 0, ((int32_t)(d1 | d2 << 7) - 8192) * 16);   /* +-2 st, Q16 */
        else if (ty == 0xC0u) { set.preset = (uint8_t)(d1 % SC_N_PRESETS); load(set.preset); edited = 0; dirty = 1; t_change = fm1_ms; }
        else if (ty == 0xB0u && d1 == 1u) {                  /* the mod wheel: the stick pushed left */
            perf.joy_x = syn.perf.joy_x = -(int32_t)(d2 * 516u);
            perf_dirty = 1;
        } else if (ty == 0xB0u && d1 == 74u) {
            int L;
            for (L = 0; L < 2; L++) syn.patch.layer[L].cutoff = (int32_t)(d2 * 516u);
            edited = 1;
            patch_dirty = 1;
        } else if (ty == 0xB0u && (d1 == 91u || d1 == 93u)) {   /* the reverb's mix, the chorus's */
            if (d1 == 91u) { syn.patch.fx.reverb.mix = (int32_t)(d2 * 516u); syn.patch.fx.reverb.on = (int8_t)(d2 != 0); }
            else { syn.patch.fx.chorus.mix = (int32_t)(d2 * 516u); syn.patch.fx.chorus.on = (int8_t)(d2 != 0); }
            edited = 1;
            patch_dirty = 1;
        } else if (ty == 0xB0u && (d1 == 120u || d1 == 123u)) {
            sc_post(&syn, SC_EV_ALL_OFF, 0, 0);
            for (k = 0; k < 128u; k++) sounding[k] = at_poly[k] = 0;
            at_chan = 0;
        } else if (ty == 0xB0u && d1 == 7u) {                /* the volume: as the MIDI spec has it, 127 = as it was */
            cc7_q12 = (int32_t)((d2 * d2 * 4096u + 8064u) / 16129u);
        }
    }
    aftertouch(keys);
    if (patch_dirty) { sc_post(&syn, SC_EV_PATCH, 0, 0); patch_dirty = perf_dirty = 0; }
    else if (perf_dirty) { sc_post(&syn, SC_EV_PERF, 0, 0); perf_dirty = 0; }

    base = (uint32_t)(KEY0 + 12 * set.oct);
    for (k = 0; k < 27u; k++)               /* what sounds, on the keys of this octave */
        if (sounding[base + k]) lit |= 1u << k;
    for (k = 0; k < 27u; k++)
        fm1_led_key(KEY_LED(k), lit >> k & 1u);
    fm1_led_key(BTN_OCT_DOWN, set.oct < 0);
    fm1_led_key(BTN_OCT_UP, set.oct > 0);
    for (k = 0; k < N_GROUPS; k++)
        fm1_led_key(GROUP[k].btn, set.page >= GROUP[k].first && set.page < GROUP[k].first + GROUP[k].n);

    /* redraw only what changed */
    cpu = cpu_pub * 100u / BLOCK_TICKS;
    if (set.preset != preset_drawn || edited != edited_drawn || cpu != cpu_drawn) {
        draw_head(set.preset, (int)edited, cpu);
        edited_drawn = edited;
        cpu_drawn = cpu;
    }
    if (set.preset != preset_drawn) { draw_name(set.preset); preset_drawn = set.preset; page_drawn = ~0u; }
    if (set.page != page_drawn) { draw_page(set.page); page_drawn = set.page; for (k = 0; k < 4u; k++) shown[k] = 0x7FFFFFFF; }
    for (k = 0; k < 4u; k++) {
        int32_t v = prm_get(&PAGE[set.page][k]);
        if (v != shown[k]) { shown[k] = v; draw_prm(k, &PAGE[set.page][k], v); }
    }
    if (syn.perf.joy_x != stick_drawn[0] || perf.joy_y != stick_drawn[1]) {
        stick_drawn[0] = syn.perf.joy_x; stick_drawn[1] = perf.joy_y;
        draw_stick();
    }
    if (lit != lit_drawn) { lit_drawn = lit; draw_keys(lit); }
    if (set.oct != oct_drawn) { oct_drawn = set.oct; draw_oct(set.oct); }

    if (dirty && fm1_ms - t_change > 2000u && (int32_t)fm1_ms - note_ms > 2000 && quiet()) {
        settings_save(&set, sizeof set);    /* 2 s after the change, when silent (an erase stalls the sound) */
        dirty = 0;
    }
}

static void app_main(void)
{
    app_init();
    for (;;)
        app_loop();
}
