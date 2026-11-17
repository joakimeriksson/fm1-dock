/* SPDX-License-Identifier: GPL-3.0-only */
/* ENSEMBLE: fourteen string, choir and dark pads in a stereo three-tap ensemble (ens_engine.c: ORRERY's pads,
 * ported from Caravel's ENSEMBLE), as a fm1-dock module of its own. Six keys sound at once; the screen is its
 * own, the keys, the octave and HOME the host's.
 *   PRESETS      the pad, VINTAGE STRINGS .. RUNNING PAD: its own controls, envelope and width
 *   KNOB1..4     MORPH (strings .. choir .. dark), TONE, ENSEMBLE (the taps' depth), RATE (their speed)
 *   FX           four more knobs: ATTACK, RELEASE, REGISTER (16' .. 8' .. 4'), BREATH (the choir's noise)
 *   ARP          all notes off; SAVE keeps the pad as it is turned (else two seconds after a change)
 *   MIDI         notes; PC the pad; CC 1 morph, 74 tone, 93 ensemble, 73 attack, 72 release */
#include "hb_module.h"
#include "ens_engine.h"

#define SET_MAGIC 0x31534E45u                   /* "ENS1" */
#define C_ACC HB_RGB(150, 190, 255)             /* a pale blue: strings in a hall */
#define C_LINE HB_RGB(50, 70, 120)
#define C_WHITE 0xFFFFu
#define C_GREY HB_RGB(110, 110, 110)
#define C_DIM HB_RGB(44, 46, 54)
#define C_BLACK 0u

static const hb_host_t *H;
static ens_t box;
static struct { uint32_t magic; uint8_t preset, e[8], env[4]; } set;
static uint32_t page, dirty, t_save, drawn = ~0u;
/* a page's four knobs: which of the engine's controls (0..7), or its envelope (8 + ENS_ATK ..) */
static const uint8_t KNOB[2][4] = {{ENS_MORPH, ENS_TONE, ENS_DEPTH, ENS_RATE}, {8 + ENS_ATK, 8 + ENS_REL, ENS_REG, ENS_TEXT}};
static const char *const KNOB_NAME[2][4] = {{"MORPH", "TONE", "ENS", "RATE"}, {"ATTACK", "RELEASE", "REG", "BREATH"}};

static char *put_u(char *p, uint32_t v)
{
    if (v >= 100u) *p++ = (char)('0' + v / 100u % 10u);
    if (v >= 10u) *p++ = (char)('0' + v / 10u % 10u);
    *p++ = (char)('0' + v % 10u);
    *p = 0;
    return p;
}
static char *put_s(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }

static uint8_t *field(uint32_t k) { return k < 8u ? &set.e[k] : &set.env[k - 8u]; }
static void to_engine(void)                     /* (bytes the render reads as they are: no more to hand over) */
{
    uint32_t k;
    for (k = 0; k < 8u; k++) box.e[k] = set.e[k];
    for (k = 0; k < 4u; k++) box.env[k] = set.env[k];
}
static void touch(void) { to_engine(); dirty = 1; t_save = H->ms(); }
static void load(uint32_t preset)               /* a pad as it comes: its controls, its envelope, its width */
{
    uint32_t k;
    set.preset = (uint8_t)(preset % ENS_N_PRESETS);
    ens_load(&box, set.preset);
    for (k = 0; k < 8u; k++) set.e[k] = box.e[k];
    for (k = 0; k < 4u; k++) set.env[k] = box.env[k];
    dirty = 1;
    t_save = H->ms();
}

static void draw(uint32_t full)
{
    uint32_t k, sum = set.preset | page << 8;
    char t[24], *q;
    for (k = 0; k < 4u; k++) sum = sum * 131u + *field(KNOB[page][k]);
    if (full || sum != drawn) {
        H->fill(0, 0, 240, 240, C_BLACK);
        H->text(6, 4, 150, 0, "ENSEMBLE", C_ACC, 0);
        q = put_u(t, set.preset + 1u); q = put_s(q, "/"); put_u(q, ENS_N_PRESETS);
        H->text(170, 4, 64, 0, t, C_GREY, 0);
        H->text(6, 24, 228, 0, ENS_PRESETS[set.preset].name, C_WHITE, 0);
        H->fill(0, 44, 240, 4, C_LINE);
        for (k = 0; k < 4u; k++) {
            uint32_t v = *field(KNOB[page][k]);
            int32_t x = 4 + (int32_t)k * 59;
            put_u(t, v);
            H->text(x, 60, 56, 0, KNOB_NAME[page][k], C_GREY, 1);
            H->text(x, 84, 56, 0, t, C_WHITE, 1);
            H->fill(x + 2, 112, 52, 6, C_DIM);
            H->fill(x + 2, 112, (int32_t)(v * 52u / 127u), 6, C_ACC);
        }
        H->text(4, 140, 232, 0, page ? "ENVELOPE AND VOICE" : "SOUND", C_ACC, 1);
        H->text(4, 196, 232, 0, "PRESETS PAD   FX MORE KNOBS", C_GREY, 1);
        H->text(4, 216, 232, 0, "ARP ALL OFF   SAVE KEEPS", C_DIM, 1);
        drawn = sum;
    }
    if (dirty && H->ms() - t_save > 2000u) {
        set.magic = SET_MAGIC;
        H->save(&set, sizeof set);
        dirty = 0;
    }
    H->led(HB_BTN_FX, page);
}

static void turn(uint32_t k, int32_t d)
{
    uint8_t *f = field(k);
    int32_t v = (int32_t)*f + d;
    *f = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
    touch();
}
static void ui(uint32_t ev, int32_t arg)
{
    uint32_t id = HB_EV_ID(ev);
    switch (HB_EV_KIND(ev)) {
    case HB_EV_ENC:
        if (id == HB_ENC_PRESETS) {
            int32_t p = (int32_t)set.preset + arg;
            load((uint32_t)(p < 0 ? 0 : p >= (int32_t)ENS_N_PRESETS ? (int32_t)ENS_N_PRESETS - 1 : p));
        } else if (id >= HB_ENC_KNOB1 && id <= HB_ENC_KNOB4) turn(KNOB[page][id - HB_ENC_KNOB1], 3 * arg);
        return;
    case HB_EV_BTN_DOWN:
        if (id == HB_BTN_FX) page ^= 1u;
        else if (id == HB_BTN_ARP) ens_all_off(&box);
        else if (id == HB_BTN_SAVE) { set.magic = SET_MAGIC; H->save(&set, sizeof set); dirty = 0; H->log("ensemble: saved"); }
        return;
    default: return;
    }
}
static void note(uint32_t ch, uint32_t key, uint32_t vel)
{
    (void)ch;
    if (key > 127u) return;
    if (vel) ens_note_on(&box, (uint8_t)key, (uint8_t)(vel > 127u ? 127u : vel));
    else ens_note_off(&box, (uint8_t)key);
}
static void cc(uint32_t ch, uint32_t num, uint32_t val)
{
    (void)ch;
    val &= 127u;
    switch (num) {
    case 1: set.e[ENS_MORPH] = (uint8_t)val; touch(); break;
    case 74: set.e[ENS_TONE] = (uint8_t)val; touch(); break;
    case 93: set.e[ENS_DEPTH] = (uint8_t)val; touch(); break;
    case 73: set.env[ENS_ATK] = (uint8_t)val; touch(); break;
    case 72: set.env[ENS_REL] = (uint8_t)val; touch(); break;
    case 120: case 123: ens_all_off(&box); break;
    default: break;
    }
}
static void midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    switch (st & 0xF0u) {
    case 0x90: note(st & 15u, d1, d2); break;
    case 0x80: note(st & 15u, d1, 0); break;
    case 0xB0: cc(st & 15u, d1, d2); break;
    case 0xC0: if (d1 < ENS_N_PRESETS) load(d1); break;
    default: break;
    }
}
static void render(int32_t *out, uint32_t n) { ens_render(&box, out, n); }

static int init(const hb_host_t *h)
{
    uint32_t k, ok;
    H = h;
    ens_init(&box);
    ok = !H->load(&set, sizeof set) && set.magic == SET_MAGIC && set.preset < ENS_N_PRESETS;
    for (k = 0; ok && k < 8u; k++) ok = set.e[k] <= 127u;
    for (k = 0; ok && k < 4u; k++) ok = set.env[k] <= 127u;
    if (ok) {
        ens_load(&box, set.preset);             /* (the pad's width and filter envelope are its own) */
        to_engine();
    } else {
        set.magic = SET_MAGIC;
        load(0);
    }
    page = dirty = 0;
    drawn = ~0u;
    H->log("ensemble: string, choir and dark pads");
    return 0;
}
static void release(void) { ens_all_off(&box); }

HB_DESC = {HB_ABI, "ENSEMBLE", init, release, note, cc, render, ui, draw, 0, 0, HB_F_SCREEN, midi};
