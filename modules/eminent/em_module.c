/* SPDX-License-Identifier: GPL-3.0-only */
/* EMINENT: the Eminent 310 string ensemble with the Small Stone phaser (em_engine.c), Jarre's string bed under
 * Oxygène and Equinoxe, as a fm1-dock module. Every key sounds (16 at once); the screen is its own, the keys,
 * octave and HOME the host's, so on seqdock it plays over the dock's bass and drums with the dock's echo and reverb.
 *   PRESETS      the registration: STRINGS 8'+4', CELLO 16'+8', VIOLIN 4', FULL
 *   KNOB1..4     ATTACK, RELEASE, ENSEMBLE (the chorus depth, 0 off), PHASER (the sweep's rate, 0 off)
 *   FX           the phaser's COLOR (feedback); ALGORITHM: the host's reverb; ARP: all notes off
 *   MIDI         notes; PC the registration; CC 73 attack, 72 release, 93 ensemble, 1 the phaser, 91 reverb */
#include "hb_module.h"
#include "em_engine.h"

#define SET_MAGIC 0x31314D45u                   /* "EM11" */
#define C_ACC HB_RGB(235, 205, 130)             /* the 310's cream over walnut */
#define C_WOOD HB_RGB(120, 70, 40)
#define C_WHITE 0xFFFFu
#define C_GREY HB_RGB(110, 110, 110)
#define C_DIM HB_RGB(50, 46, 40)
#define C_BLACK 0u

static const hb_host_t *H;
static em_t box;
static struct { uint32_t magic; em_patch_t p; } set;
static uint32_t dirty, t_save, drawn = ~0u, drawn_held = ~0u;
static const char *const REG_NAME[EM_N_REG] = {"STRINGS 8'+4'", "CELLO 16'+8'", "VIOLIN 4'", "FULL 16'+8'+4'"};
static const char *const KNOB_NAME[4] = {"ATTACK", "RELEASE", "CHORUS", "PHASER"};

static char *put_u(char *p, uint32_t v)
{
    if (v >= 100u) *p++ = (char)('0' + v / 100u % 10u);
    if (v >= 10u) *p++ = (char)('0' + v / 10u % 10u);
    *p++ = (char)('0' + v % 10u);
    *p = 0;
    return p;
}
static char *put_s(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }

static void reverb_out(void)                    /* the patch's REVERB to the host's reverb, where the host has one */
{
#ifdef HB_HAS_FX_REVERB                         /* (hb_module.h says so with the processing section: not in this host yet) */
    static uint32_t sent = ~0u;
    if (!HB_HOST_HAS(H, fx_reverb) || set.p.reverb == sent) return;
    sent = set.p.reverb;
    H->fx_reverb(set.p.reverb > 0, 2, 45875, 32768, set.p.reverb * 655);   /* HALL, a large room */
#endif
}
static void apply(void)
{
    em_set_patch(&box, &set.p);
    reverb_out();
    dirty = 1;
    t_save = H->ms();
}

static void draw(uint32_t full)
{
    const em_patch_t *p = &set.p;
    uint32_t k, held = em_held(&box);
    uint32_t state = p->reg | p->attack << 2 | p->release << 9 | p->ensemble << 16 | (uint32_t)p->phaser << 23 | (uint32_t)p->color << 30 | (uint32_t)p->reverb << 31;
    char t[24], *q;
    if (full || state != drawn) {
        H->fill(0, 0, 240, 240, C_BLACK);
        H->text(6, 4, 160, 0, "EMINENT 310", C_ACC, 0);   /* (the host's text paints its own black line: the wood under it) */
        H->text(6, 24, 228, 0, REG_NAME[p->reg], C_WHITE, 0);
        H->fill(0, 44, 240, 4, C_WOOD);
        for (k = 0; k < 4u; k++) {
            uint32_t v = k == 0 ? p->attack : k == 1 ? p->release : k == 2 ? p->ensemble : p->phaser;
            int32_t x = 4 + (int32_t)k * 59;
            if (k >= 2u && !v) put_s(t, "OFF"); else put_u(t, v);
            H->text(x, 60, 56, 0, KNOB_NAME[k], C_GREY, 1);
            H->text(x, 84, 56, 0, t, k >= 2u && !v ? C_GREY : C_WHITE, 1);
            H->fill(x + 2, 112, 52, 6, C_DIM);
            H->fill(x + 2, 112, (int32_t)(v * 52u / 100u), 6, C_ACC);
        }
        q = put_s(t, "COLOR "); q = put_s(q, p->color ? "ON" : "OFF"); q = put_s(q, "   REVERB "); q = put_u(q, p->reverb); put_s(q, "%");
        H->text(4, 136, 232, 0, t, C_GREY, 0);
        H->text(4, 196, 232, 0, "PRESETS VOICE   FX COLOR", C_GREY, 1);
        H->text(4, 216, 232, 0, "ALGO REVERB   ECHO: OPTIONS", C_DIM, 1);
        drawn = state;
        drawn_held = ~0u;
    }
    if (held != drawn_held) {
        H->fill(0, 160, 240, 28, C_BLACK);
        if (held) { q = put_u(t, held); put_s(q, held == 1u ? " KEY" : " KEYS"); H->text(4, 164, 232, 0, t, C_ACC, 1); }
        drawn_held = held;
    }
    if (dirty && H->ms() - t_save > 2000u) {
        set.magic = SET_MAGIC;
        H->save(&set, sizeof set);
        dirty = 0;
    }
    H->led(HB_BTN_FX, p->color);
}

static void turn(uint8_t *f, int32_t d, int32_t lo, int32_t hi)
{
    int32_t v = (int32_t)*f + d;
    *f = (uint8_t)(v < lo ? lo : v > hi ? hi : v);
    apply();
}
static void ui(uint32_t ev, int32_t arg)
{
    uint32_t id = HB_EV_ID(ev);
    switch (HB_EV_KIND(ev)) {
    case HB_EV_ENC:
        switch (id) {
        case HB_ENC_PRESETS: turn(&set.p.reg, arg, 0, EM_N_REG - 1); break;
        case HB_ENC_ALGORITHM: turn(&set.p.reverb, 5 * arg, 0, 100); break;
        case HB_ENC_KNOB1: turn(&set.p.attack, 2 * arg, 0, 100); break;
        case HB_ENC_KNOB2: turn(&set.p.release, 2 * arg, 0, 100); break;
        case HB_ENC_KNOB3: turn(&set.p.ensemble, 5 * arg, 0, 100); break;
        case HB_ENC_KNOB4: turn(&set.p.phaser, 5 * arg, 0, 100); break;
        default: break;
        }
        return;
    case HB_EV_BTN_DOWN:
        if (id == HB_BTN_FX) { set.p.color = !set.p.color; apply(); }
        else if (id == HB_BTN_ARP) em_all_off(&box);
        else if (id == HB_BTN_SAVE) { set.magic = SET_MAGIC; H->save(&set, sizeof set); dirty = 0; H->log("eminent: saved"); }
        return;
    default: return;
    }
}
static void note(uint32_t ch, uint32_t key, uint32_t vel) { (void)ch; if (vel) em_note_on(&box, key, vel); else em_note_off(&box, key); }
static void cc(uint32_t ch, uint32_t num, uint32_t val)
{
    (void)ch;
    switch (num) {
    case 73: set.p.attack = (uint8_t)(val * 100u / 127u); apply(); break;
    case 72: set.p.release = (uint8_t)(val * 100u / 127u); apply(); break;
    case 93: set.p.ensemble = (uint8_t)(val * 100u / 127u); apply(); break;
    case 1: set.p.phaser = (uint8_t)(val * 100u / 127u); apply(); break;
    case 91: set.p.reverb = (uint8_t)(val * 100u / 127u); apply(); break;
    case 120: case 123: em_all_off(&box); break;
    default: break;
    }
}
static void midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    switch (st & 0xF0u) {
    case 0x90: note(st & 15u, d1, d2); break;
    case 0x80: note(st & 15u, d1, 0); break;
    case 0xB0: cc(st & 15u, d1, d2); break;
    case 0xC0: if (d1 < EM_N_REG) { set.p.reg = (uint8_t)d1; apply(); } break;
    default: break;
    }
}
static void render(int32_t *out, uint32_t n) { em_render(&box, out, n); }

static int init(const hb_host_t *h)
{
    H = h;
    em_init(&box);
    if (H->load(&set, sizeof set) || set.magic != SET_MAGIC) {
        set.magic = SET_MAGIC;
        set.p = box.p;                          /* the engine's defaults: strings, a slow swell, the ensemble, a slow phaser */
    }
    if (set.p.reg >= EM_N_REG) set.p.reg = EM_STRINGS;
    em_set_patch(&box, &set.p);
    reverb_out();
    dirty = 0;
    H->log("eminent: Eminent 310 strings, Small Stone");
    return 0;
}
static void release(void) { em_all_off(&box); }

HB_DESC = {HB_ABI, "EMINENT", init, release, note, cc, render, ui, draw, 0, 0, HB_F_SCREEN, midi};
