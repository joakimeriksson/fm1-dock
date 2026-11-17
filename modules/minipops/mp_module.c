/* The Mini Pops as a module: the Korg Mini Pops 7 (mp_engine.c), worked as the machine is. The white keys from F3
 * are its ten rhythm keys, down until pressed again, and several down at once combine their rhythms; the black
 * keys F#3, G#3 and A#3 switch the tambourine, guiro and quijada; OCT- / OCT+ move the A / B lever; PLAY starts and
 * stops. ALGORITHM picks one rhythm (the keys down replaced), KNOB1 the tempo, KNOB2 the volume, KNOB3 the
 * balance, KNOB4 the additional rhythms' level. The top ten keys (A#4 up) strike the sounds by hand, as do MIDI
 * notes (C2 the bass drum, then up the fourteen). The settings are the module's record. */
#include "hb_module.h"
#include "mp_engine.h"

#define SET_MAGIC 0x50504D33u                   /* "3MPP" */
static const hb_host_t *H;
static mp_t box;
typedef struct { uint16_t keys, tempo; uint8_t section, volume, balance, level, xmask, on; } mp_set_t;   /* the ten keys (bit k), bpm, the lever, percents, the extras (bit 0 tambourine, 1 guiro, 2 quijada), running */
static struct { uint32_t magic; mp_set_t r; } set;
static uint32_t t_save, dirty, drawn = ~0u, drawn_tempo, drawn_level;
static const int8_t WHITE[27] = { 0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9,   /* key k -> rhythm key */
                                  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
#define C_ACC HB_RGB(180, 140, 255)
#define C_OFF HB_RGB(90, 90, 90)
#define C_WHITE 0xFFFFu
#define R (set.r)

static void leds(void)
{
    uint32_t k;
    for (k = 0; k < 17u; k++)                   /* the keys' LEDs: the keys down */
        H->led(HB_LED_KEY(k), WHITE[k] >= 0 ? (R.keys >> WHITE[k] & 1u) :
               k == 1u ? (R.xmask & 1u) : k == 3u ? (R.xmask >> 1 & 1u) : k == 5u ? (R.xmask >> 2 & 1u) : 0u);
    H->led(HB_BTN_PLAY, R.on);
}
static void apply(void)                         /* the settings to the engine */
{
    mp_set_keys(&box, R.keys, R.section);
    mp_set_tempo(&box, R.tempo);
    mp_set_level(&box, R.volume, R.balance);
    mp_set_extra(&box, R.xmask & 4u ? R.level : 0, R.xmask & 2u ? R.level : 0, R.xmask & 1u ? R.level : 0);
    mp_play(&box, R.on);
    dirty = 1;
    t_save = H->ms();
    leds();
}
static void render(int32_t *out, uint32_t n) { mp_render(&box, out, n); }
static void strike(uint32_t voice, uint32_t vel)
{
    mp_trigger(&box, voice % MP_VOICES, (int32_t)(vel > 100u ? 65536u : vel * 655u));
}
static void note(uint32_t ch, uint32_t key, uint32_t vel) { (void)ch; if (vel) strike(key + 14u * 8u - 36u, vel); }
static void cc(uint32_t ch, uint32_t num, uint32_t val) { (void)ch; (void)num; (void)val; }
static void midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    if ((st & 0xF0u) == 0x90u) note(st & 15u, d1, d2);
}
static void ui(uint32_t ev, int32_t arg)
{
    uint32_t id = HB_EV_ID(ev), k;
    int32_t v;
    switch (HB_EV_KIND(ev)) {
    case HB_EV_KEY_DOWN:
        if (id < 17u && WHITE[id] >= 0) R.keys ^= (uint16_t)(1u << WHITE[id]);
        else if (id == 1u) R.xmask ^= 1u;
        else if (id == 3u) R.xmask ^= 2u;
        else if (id == 5u) R.xmask ^= 4u;
        else if (id >= 17u && id < 27u) { strike(id - 17u, arg > 0 ? (uint32_t)arg : 100u); return; }
        else return;
        break;
    case HB_EV_BTN_DOWN:
        if (id == HB_BTN_PLAY) R.on = !R.on;
        else if (id == HB_BTN_OCT_DOWN) R.section = 0;
        else if (id == HB_BTN_OCT_UP) R.section = 1;
        else return;
        break;
    case HB_EV_ENC:
        switch (id) {
        case HB_ENC_ALGORITHM:
            for (k = 0; k < 10u && !(R.keys >> k & 1u); k++) ;
            v = (int32_t)(k < 10u ? k + 10u * R.section : 6u) + arg;
            v = v < 0 ? 0 : v > 19 ? 19 : v;
            R.keys = (uint16_t)(1u << (v % 10));
            R.section = (uint8_t)(v / 10);
            break;
        case HB_ENC_KNOB1: v = (int32_t)R.tempo + 2 * arg; R.tempo = (uint16_t)(v < 40 ? 40 : v > 240 ? 240 : v); break;
        case HB_ENC_KNOB2: v = (int32_t)R.volume + 5 * arg; R.volume = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v); break;
        case HB_ENC_KNOB3: v = (int32_t)R.balance + 5 * arg; R.balance = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v); break;
        case HB_ENC_KNOB4: v = (int32_t)R.level + 10 * arg; R.level = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v); break;
        default: return;
        }
        break;
    default:
        return;
    }
    apply();
}
static void num(char *t, uint32_t v, const char *tail)
{
    uint32_t n = 0;
    if (v >= 100u) t[n++] = (char)('0' + v / 100u);
    if (v >= 10u) t[n++] = (char)('0' + v / 10u % 10u);
    t[n++] = (char)('0' + v % 10u);
    while (*tail) t[n++] = *tail++;
    t[n] = 0;
}
static void draw(uint32_t full)
{
    uint32_t state, k, down = 0;
    char t[12];
    state = R.keys | (uint32_t)R.section << 10 | (uint32_t)R.xmask << 11 | (uint32_t)R.on << 14;
    if (full) {
        H->fill(0, HB_TOP, 240, 240 - HB_TOP, 0);
        drawn = ~0u;
        drawn_tempo = 0;
    }
    if (state != drawn) {                       /* the ten keys as the machine's: lit when down, the lever's row marked */
        const char *name = "NO KEY";
        for (k = 0; k < 10u; k++) {
            uint32_t x = 8 + 23 * k;
            if (R.keys >> k & 1u) { down++; name = mp_rhythm(R.section, k)->name; }
            H->fill((int32_t)x, 150, 20, 44, R.keys >> k & 1u ? C_ACC : HB_RGB(60, 60, 64));
        }
        H->text(0, 128, 240, 0, R.section ? "A   B <" : "> A   B", C_OFF, 1);
        H->fill(0, 40, 240, 36, 0);
        H->text(0, 40, 240, 1, down > 1u ? (down == 2u ? "2 KEYS" : down == 3u ? "3 KEYS" : "KEYS") : name,
                R.on ? C_ACC : C_WHITE, 1);
        H->text(0, 200, 80, 0, R.xmask & 1u ? "TAMB" : "tamb", R.xmask & 1u ? C_WHITE : C_OFF, 1);
        H->text(80, 200, 80, 0, R.xmask & 2u ? "GUIRO" : "guiro", R.xmask & 2u ? C_WHITE : C_OFF, 1);
        H->text(160, 200, 80, 0, R.xmask & 4u ? "QUIJADA" : "quijada", R.xmask & 4u ? C_WHITE : C_OFF, 1);
        H->text(0, 220, 240, 0, R.on ? "PLAY: STOP" : "PLAY: START", C_OFF, 1);
        drawn = state;
    }
    if (R.tempo != drawn_tempo || R.level != drawn_level) {
        H->fill(0, 84, 240, 36, 0);
        num(t, R.tempo, " BPM");
        H->text(0, 90, 120, 0, t, C_WHITE, 1);
        num(t, R.level, "% EXTRA");
        H->text(120, 90, 120, 0, t, C_WHITE, 1);
        drawn_tempo = R.tempo;
        drawn_level = R.level;
    }
    if (dirty && H->ms() - t_save > 2000u) {
        dirty = 0;
        H->save(&set, sizeof set);
    }
}
static int init(const hb_host_t *h)
{
    H = h;
    if (H->load(&set, sizeof set) || set.magic != SET_MAGIC) {
        set.magic = SET_MAGIC;
        R.keys = 1u << 6;                       /* Rock 1 */
        R.tempo = 120;
        R.section = 0;
        R.volume = 80;
        R.balance = 50;
        R.level = 70;
        R.xmask = 0;
        R.on = 0;
    }
    mp_init(&box);
    R.on = 0;
    apply();
    dirty = 0;
    H->log("minipops: Korg Mini Pops 7");
    return 0;
}
static void release(void) { R.on = 0; mp_play(&box, 0); }

HB_DESC = {HB_ABI, "MINIPOPS", init, release, note, cc, render, ui, draw, 0, 0, HB_F_PANEL, midi};
