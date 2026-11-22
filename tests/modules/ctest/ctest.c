/* SPDX-License-Identifier: GPL-3.0-only */
/* CTEST: which build's code runs (tests/cache_hw_test.py, #87). modules/sine's 4-voice sine (Dinghy's), with 64
 * small functions whose sum depends on the build (VAL): init() logs what it should be and what it is. */
#include "hb_module.h"

static const hb_host_t *H;

#define NV 4u                                   /* voices */
static struct {
    volatile uint32_t inc;                      /* phase step per frame: f * 2^32 / 44100 */
    volatile int32_t gate;                      /* target level, Q15: 0 = released */
    int32_t amp;                                /* the level now: glides to gate, no clicks */
    uint32_t ph, age;
    uint8_t note;
} voice[NV];
static volatile int32_t att = 48, rel = 6;      /* level steps per frame */
static uint32_t ages, last_note = 255, drawn_note, drawn_env;

static int32_t sine(uint32_t ph)                /* a full cycle over 2^32 -> -32767..32767 (a refined parabola, ~0.1 %) */
{
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x, y, ay;
    y = (x * (32768 - ax)) >> 13;               /* 4x(1-|x|), Q15 */
    ay = y < 0 ? -y : y;
    y += ((((y * ay) >> 15) - y) * 7373) >> 15; /* + 0.225 (y|y| - y) */
    return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}

static void render(int32_t *out, uint32_t n)
{
    uint32_t v, i;
    int32_t a = att, r = rel;
    for (v = 0; v < NV; v++) {
        uint32_t ph = voice[v].ph, step = voice[v].inc;
        int32_t amp = voice[v].amp, g = voice[v].gate;
        if (!amp && !g)
            continue;                           /* silent: costs nothing */
        for (i = 0; i < n; i++) {
            int32_t s;
            if (amp < g) { amp += a; if (amp > g) amp = g; }
            else if (amp > g) { amp -= r; if (amp < g) amp = g; }
            ph += step;
            s = (sine(ph) * amp) >> 15;
            out[2u * i] += s;                   /* L */
            out[2u * i + 1u] += s;              /* R */
        }
        voice[v].ph = ph;
        voice[v].amp = amp;
    }
}

static const uint32_t INC_C9[12] = {            /* MIDI 120..131 (C9..B9); lower octaves shift right */
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};

static void note(uint32_t ch, uint32_t key, uint32_t vel)
{
    uint32_t v, pick = 0, best = 0xFFFFFFFFu;
    (void)ch;
    if (key > 127u)
        return;
    if (!vel) {
        for (v = 0; v < NV; v++)
            if (voice[v].note == key)
                voice[v].gate = 0;
        return;
    }
    for (v = 0; v < NV; v++) {                  /* the same note, else a free voice, else the oldest */
        uint32_t score = voice[v].note == key && voice[v].gate ? 0u
                       : voice[v].gate ? 0x80000000u + voice[v].age : (uint32_t)voice[v].amp + 1u;
        if (score < best) { best = score; pick = v; }
    }
    voice[pick].note = (uint8_t)key;
    voice[pick].age = ++ages;
    voice[pick].inc = INC_C9[key % 12u] >> (10u - key / 12u);   /* the pitch before the gate */
    voice[pick].gate = (int32_t)(vel * 90u);    /* 127 -> 11430: four voices stay under full scale */
    last_note = key;
}

static void cc(uint32_t ch, uint32_t num, uint32_t val)
{
    uint32_t v;
    (void)ch; (void)val;
    if (num == 123u)                            /* all notes off */
        for (v = 0; v < NV; v++)
            voice[v].gate = 0;
}

static void ui(uint32_t ev, int32_t arg)
{
    if (HB_EV_KIND(ev) != HB_EV_ENC)
        return;
    if (HB_EV_ID(ev) == HB_ENC_KNOB1) {
        int32_t a = att - arg * 4;
        att = a < 4 ? 4 : a > 400 ? 400 : a;
    } else if (HB_EV_ID(ev) == HB_ENC_KNOB2) {
        int32_t r = rel - arg;
        rel = r < 1 ? 1 : r > 100 ? 100 : r;
    }
}

static void draw(uint32_t full)
{
    static const char *const NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char s[8] = "-";
    uint32_t env = (uint32_t)att << 16 | (uint32_t)rel;
    if (full) {
        H->fill(0, HB_TOP, 240, 240 - HB_TOP, 0);
        H->text(0, 52, 240, 0, "a sine to build on", HB_RGB(90, 90, 90), 1);
        drawn_note = drawn_env = ~0u;
    }
    if (last_note != drawn_note) {
        drawn_note = last_note;
        if (last_note < 128u) {
            const char *p = NAME[last_note % 12u];
            uint32_t i = 0;
            while (*p) s[i++] = *p++;
            if (last_note >= 12u) s[i++] = (char)('0' + last_note / 12u - 1u);
            s[i] = 0;
        }
        H->text(0, 100, 240, 1, s, 0xFFFFu, 1);
    }
    if (env != drawn_env) {
        char b[24] = "ATT 000 MS  REL 000 MS";  /* the time to full level / to silence */
        uint32_t a = (uint32_t)(11430 / att) * 1000u / 44100u, r = (uint32_t)(11430 / rel) * 1000u / 44100u;
        b[4] = (char)('0' + a / 100u % 10u); b[5] = (char)('0' + a / 10u % 10u); b[6] = (char)('0' + a % 10u);
        b[16] = (char)('0' + r / 100u % 10u); b[17] = (char)('0' + r / 10u % 10u); b[18] = (char)('0' + r % 10u);
        drawn_env = env;
        H->text(0, 200, 240, 0, b, HB_RGB(180, 140, 255), 1);
    }
}


/* ---- the test (issue 87): 64 small functions whose result says which build's CODE ran (VAL), called through a
 * table at start; after a deploy to the flash another build ran from, a stale instruction cache would run the old */
#define F(n) __attribute__((noinline)) static uint32_t f##n(uint32_t x) { return x + VAL * 1000u + n##u; }
F(0) F(1) F(2) F(3) F(4) F(5) F(6) F(7) 
F(8) F(9) F(10) F(11) F(12) F(13) F(14) F(15) 
F(16) F(17) F(18) F(19) F(20) F(21) F(22) F(23) 
F(24) F(25) F(26) F(27) F(28) F(29) F(30) F(31) 
F(32) F(33) F(34) F(35) F(36) F(37) F(38) F(39) 
F(40) F(41) F(42) F(43) F(44) F(45) F(46) F(47) 
F(48) F(49) F(50) F(51) F(52) F(53) F(54) F(55) 
F(56) F(57) F(58) F(59) F(60) F(61) F(62) F(63) 

static uint32_t (*const T[64])(uint32_t) = {f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58, f59, f60, f61, f62, f63};
static void say_hex(const char *what, uint32_t v)
{
    static char b[40];
    uint32_t i = 0, k;
    while (what[i]) { b[i] = what[i]; i++; }
    for (k = 0; k < 8u; k++) b[i + k] = "0123456789abcdef"[(v >> (28u - 4u * k)) & 15u];
    b[i + 8] = 0;
    H->log(b);
}
static int init(const hb_host_t *h)
{
    uint32_t i, s = 0;
    H = h;
    for (i = 0; i < 64u; i++) s += T[i](i);
    say_hex("ctest: built ", 4032u + 64000u * VAL);
    say_hex("ctest: ran   ", s);
    return 0;
}

static void release(void)
{
    uint32_t v;
    for (v = 0; v < NV; v++)
        voice[v].gate = voice[v].amp = 0;
}

static const hb_param_t PARAMS[] = {{"ATTACK", 4, 400, 48}, {"RELEASE", 1, 100, 6}};

HB_DESC = {HB_ABI, "CTEST", init, release, note, cc, render, ui, draw, PARAMS, 2, 0, 0};
