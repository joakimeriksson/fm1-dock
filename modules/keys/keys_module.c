/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Joakim Eriksson */
/* KEYS: a clavinet, an electric piano and a piano from physical models (keys_dsp.h; the string's excitation, the SVF
 * and the generator after Mutable Instruments' Plaits and stmlib, keys_mi.h, MIT), as a fm1-dock module.
 *   - three families, a few presets each (PRESETS); ALGORITHM jumps from family to family;
 *   - KNOB1..4: TONE, DECAY, PICKUP (CLAV, E.PIANO) or HARDNESS (PIANO), VOLUME: a preset's starting values; the
 *     first three turned for the next notes (a struck string keeps what it was struck with), VOLUME at once; MIDI
 *     CC 74, 71, 73, 72 the same (fm1-dock's KNOBS CC), 64 the sustain pedal, program change the preset;
 *   - the velocity: the hammer (its contact time, so the brightness) and the level, in all three;
 *   - the voices: up to NVOICE with the second core (half of them on it), NV_1 on core 0 alone; a key struck again
 *     strikes its string (or tine) again, as it still rings;
 *   - the piano's soundboard: three wide resonances after its voices, on the part.
 * The keys, MIDI and edits come in from the main loop and are played at the start of the next render, in order. */
#include "hb_module.h"
#include "keys_dsp.h"

void *memcpy(void *d, const void *s, unsigned n);   /* (modules/lib/hb_lib.c) */
void *memset(void *d, int c, unsigned n);

static const hb_host_t *H;

#define NVOICE 24u                              /* with the second core */
#define NV_1 16u                                /* on core 0 alone */
#define BLK 256u                                /* the host's block */
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

/* ---- the presets: knobs' starting values (0..127) and the family's own (keys_dsp.h's x[]) */
typedef struct {
    const char *name;
    uint8_t fam, k[4];
    float x[4], gain;
} preset_t;
/* CLAV x: second pickup (+1 in phase, -1 out of phase), resonance, envelope depth (octaves), band-pass;
 * E.PIANO x: bell, hammer hardness, bark, the tone bar's beat (Hz); PIANO x: detune (cents), stiffness */
static const preset_t PRESETS[] = {
    {"CLAV BRIGHT", FAM_CLAV, {92, 64, 30, 96}, {-0.5f, 0.05f, 1.0f, 0.0f}, 1.4f},
    {"CLAV MELLOW", FAM_CLAV, {54, 64, 92, 96}, {0.0f, 0.10f, 0.6f, 0.0f}, 1.75f},
    {"CLAV MUTED", FAM_CLAV, {60, 8, 46, 100}, {1.0f, 0.15f, 0.8f, 0.0f}, 1.15f},
    {"CLAV WAH", FAM_CLAV, {64, 50, 40, 96}, {-1.0f, 0.60f, 2.5f, 1.0f}, 1.3f},
    {"EP SOFT", FAM_EP, {40, 80, 30, 96}, {0.3f, 0.2f, 0.2f, 0.25f}, 1.5f},
    {"EP BARK", FAM_EP, {70, 70, 100, 96}, {0.6f, 0.6f, 0.9f, 0.35f}, 1.3f},
    {"EP BELL", FAM_EP, {90, 90, 44, 96}, {1.0f, 0.9f, 0.3f, 0.5f}, 1.3f},
    {"GRAND", FAM_PIANO, {70, 76, 60, 96}, {0.6f, 0.5f, 0.0f, 0.0f}, 0.85f},
    {"UPRIGHT", FAM_PIANO, {48, 52, 40, 96}, {1.5f, 0.7f, 0.0f, 0.0f}, 1.1f},
    {"HONKY", FAM_PIANO, {84, 60, 90, 96}, {9.0f, 0.6f, 0.0f, 0.0f}, 0.95f},
};
#define NPRESET NELEM(PRESETS)
static const char *const FAM_NAME[NFAM] = {"CLAV", "E.PIANO", "PIANO"};
static const char *const KNOB_NAME[NFAM][4] = {
    {"TONE", "DECAY", "PICKUP", "VOLUME"},
    {"TONE", "DECAY", "PICKUP", "VOLUME"},
    {"TONE", "DECAY", "HARDNESS", "VOLUME"},
};

/* what the next notes are played with: the main loop writes it, the audio interrupt reads it at a note-on (a float
 * torn between two edits is still one of them) */
static volatile uint32_t play_fam;
static kd_par_t play_par;
static volatile float play_gain = 1.0f;         /* the preset's level */
static volatile float out_vol = 1.0f;           /* VOLUME, on the part (as Q15 x 12000) */
static float out_now;                           /* where its ramp is */

/* ---- the voices (the audio interrupt's) */
typedef struct {
    uint8_t fam, note, gate, sus, active;
    uint32_t age;
    float pk, g;
    union {
        kd_clav_t c;
        kd_ep_t e;
        kd_piano_t p;
    } u;
} voice_t;
static voice_t vo[NVOICE];
static float dline[NVOICE][2][KD_LEN];          /* the strings' delay lines: CLAV takes one, PIANO two */
static uint32_t vage;
static uint8_t sustain;
static volatile uint32_t voices_live;

static uint32_t c1_have, c1_on, c1_dead;
static uint32_t vcap(void) { return c1_on ? NVOICE : NV_1; }

static void voice_strike(voice_t *v, uint32_t note, uint32_t vel)
{
    float vn = (float)vel * (1.0f / 127.0f);
    float f0 = 440.0f * kd_exp2(((float)note - 69.0f) * (1.0f / 12.0f));
    int fresh = !v->active || v->fam != play_fam || v->note != note;
    kd_par_t p = play_par;
    float (*d)[KD_LEN] = dline[v - vo];
    v->fam = (uint8_t)play_fam;
    v->note = (uint8_t)note;
    v->gate = 1;
    v->sus = 0;
    v->age = ++vage;
    v->g = play_gain;
    v->pk = 1.0f;
    switch (v->fam) {                           /* (the union's other members are over a string's pointers) */
    case FAM_CLAV:
        v->u.c.s.d = d[0];
        kd_clav_on(&v->u.c, f0, vn, (float)note, &p, fresh);
        break;
    case FAM_EP: kd_ep_on(&v->u.e, f0, vn, &p, fresh); break;
    default:
        v->u.p.s[0].d = d[0];
        v->u.p.s[1].d = d[1];
        kd_piano_on(&v->u.p, f0, vn, (float)note, &p, fresh);
        break;
    }
    v->active = 1;
}

/* the voice for a key: its own if it still rings (struck again), else a free one, else the quietest let go, else
 * the oldest */
static voice_t *voice_for(uint32_t note)
{
    uint32_t i, cap = vcap();
    voice_t *best = 0;
    for (i = 0; i < cap; i++)
        if (vo[i].active && vo[i].note == note && vo[i].fam == play_fam)
            return &vo[i];
    for (i = 0; i < cap; i++)
        if (!vo[i].active)
            return &vo[i];
    for (i = 0; i < cap; i++)
        if (!vo[i].gate && (!best || vo[i].pk < best->pk))
            best = &vo[i];
    if (best)
        return best;
    for (i = 0; i < cap; i++)
        if (!best || vo[i].age < best->age)
            best = &vo[i];
    return best;
}

static void key_on(uint32_t note, uint32_t vel)
{
    voice_t *v = voice_for(note);
    if (v->active && v->note != note)
        v->active = 0;                          /* (stolen: struck anew) */
    voice_strike(v, note, vel);
}
static void key_off(uint32_t note)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (vo[i].active && vo[i].gate && vo[i].note == note) {
            if (sustain) vo[i].sus = 1;
            else vo[i].gate = 0;
        }
}
static void all_off(int cut)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++) {
        vo[i].gate = vo[i].sus = 0;
        if (cut) vo[i].active = 0;
    }
    sustain = 0;
}

/* ---- from the main loop to the audio interrupt: events, played in order at the next render */
enum { EV_ON, EV_OFF, EV_SUS, EV_ALLOFF, EV_CUT };
typedef struct { uint8_t type, a, b, c; } ev_t;
#define EVQ 64u
static ev_t evq[EVQ];
static volatile uint32_t ev_w, ev_r;

static void ev_put(uint32_t type, uint32_t a, uint32_t b)
{
    uint32_t w = ev_w;
    if (w - ev_r >= EVQ)
        return;
    evq[w % EVQ].type = (uint8_t)type;
    evq[w % EVQ].a = (uint8_t)a;
    evq[w % EVQ].b = (uint8_t)b;
    __asm__ volatile("" ::: "memory");
    ev_w = w + 1u;
}

static void ev_play(const ev_t *e)
{
    uint32_t i;
    switch (e->type) {
    case EV_ON: key_on(e->a & 127u, e->b ? e->b : 1u); break;
    case EV_OFF: key_off(e->a & 127u); break;
    case EV_SUS:
        sustain = e->a >= 64u;
        if (!sustain)
            for (i = 0; i < NVOICE; i++)
                if (vo[i].sus) vo[i].sus = vo[i].gate = 0;
        break;
    case EV_ALLOFF: all_off(0); break;
    case EV_CUT: all_off(1); break;
    }
}

/* ---- the sound: the voices into two buses (dry, and the piano's through its soundboard), half of them on core 1 */
static float bus0[2][BLK], bus1[2][BLK];
static uint8_t list0[NVOICE], list1[NVOICE];
static uint32_t n0, n1;

static void voices_render(const uint8_t *list, uint32_t cnt, float (*bus)[BLK], uint32_t n)
{
    uint32_t i, k;
    for (i = 0; i < n; i++)
        bus[0][i] = bus[1][i] = 0.0f;
    for (k = 0; k < cnt; k++) {
        voice_t *v = &vo[list[k]];
        switch (v->fam) {
        case FAM_CLAV: v->pk = kd_clav_run(&v->u.c, v->gate, v->g, bus[0], n); break;
        case FAM_EP: v->pk = kd_ep_run(&v->u.e, v->gate, v->g, bus[0], n); break;
        default: v->pk = kd_piano_run(&v->u.p, v->gate, v->g, bus[1], n); break;
        }
    }
}
static void c1_job(uint32_t n) { voices_render(list1, n1, bus1, n); }   /* on core 1 */
static void c1_nop(uint32_t arg) { (void)arg; }
static void c1_probe(void)
{
    if (!c1_have || c1_dead || c1_on)
        return;
    if (H->c1_run(c1_nop, 0)) {
        if (H->c1_wait() == 0) c1_on = 1;
        else c1_dead = 1;
    }
}

/* the soundboard: three wide resonances (a body, its first plate modes) beside the strings, and the part's DC */
static km_svf_t sb[3];
static const float SB_F[3] = {95.0f, 240.0f, 720.0f}, SB_Q[3] = {1.6f, 1.8f, 1.4f}, SB_G[3] = {0.9f, 0.7f, 0.45f};
static km_dc_t dc;
static float sb_lvl;                            /* the piano bus's recent level: the soundboard runs while it rings */

static void part_block(int32_t *out, uint32_t n)
{
    uint32_t i, k, split, cnt = 0;
    uint8_t act[NVOICE];
    for (i = 0; i < NVOICE; i++) {
        if (!vo[i].active)
            continue;
        if (i >= vcap() || vo[i].pk < 2e-5f) {   /* died away, or a voice past NV_1 without core 1 */
            vo[i].active = 0;
            continue;
        }
        act[cnt++] = (uint8_t)i;
    }
    n0 = n1 = 0;
    for (k = 0; k < cnt; k++) {                 /* every other one to core 1 */
        if (c1_on && (k & 1u)) list1[n1++] = act[k];
        else list0[n0++] = act[k];
    }
    split = n1 && H->c1_run(c1_job, n);
    if (n1 && !split) {
        c1_on = 0;                              /* (asked again next render) */
        for (k = 0; k < n1; k++)
            list0[n0++] = list1[k];
        n1 = 0;
    }
    voices_render(list0, n0, bus0, n);
    if (split) {
        if (H->c1_wait() != 0) {                /* it did not end in time: core 1 is held, its half done here */
            c1_dead = 1;
            c1_on = 0;
            voices_render(list1, n1, bus1, n);
        }
        for (i = 0; i < n; i++) {
            bus0[0][i] += bus1[0][i];
            bus0[1][i] += bus1[1][i];
        }
    }
    {
        float pk = 0.0f;
        for (i = 0; i < n; i++) {
            float x = bus0[1][i];
            pk = x > pk ? x : -x > pk ? -x : pk;
        }
        if (pk > 1e-5f || sb_lvl > 1e-5f) {
            for (i = 0; i < n; i++) {
                float x = bus0[1][i], y = x, lp, bp;
                for (k = 0; k < 3u; k++) {
                    km_svf_lp_bp(&sb[k], x, &lp, &bp);
                    y += SB_G[k] * bp;
                }
                bus0[0][i] += y;
            }
        }
        sb_lvl = pk;
    }
    {
        float to = out_vol * 12000.0f, step = (to - out_now) / (float)n;   /* (VOLUME turned: a ramp, no steps) */
        for (i = 0; i < n; i++) {
            out_now += step;
            bus0[0][i] = km_dc(&dc, bus0[0][i]) * out_now;
        }
        out_now = to;
    }
    for (i = 0; i < n; i++) {                   /* Q15, a soft knee above -2.5 dBFS (chords) */
        float x = bus0[0][i];
        int32_t s = (int32_t)x, a = s < 0 ? -s : s;
        if (a > 24576) {
            a = 24576 + ((a - 24576) >> 2);
            if (a > 32767) a = 32767;
            s = s < 0 ? -a : a;
        }
        out[2u * i] += s;
        out[2u * i + 1u] += s;
    }
}

static void render(int32_t *out, uint32_t n)
{
    uint32_t r = ev_r, i, live = 0;
    while (r != ev_w) {
        ev_play(&evq[r % EVQ]);
        r++;
    }
    ev_r = r;
    c1_probe();
    for (i = 0; i < n; i += BLK)
        part_block(out + 2u * i, n - i < BLK ? n - i : BLK);
    for (i = 0; i < NVOICE; i++)
        live += vo[i].active;
    voices_live = live;
}

/* ---- the main loop's side: the preset, the knobs */
static uint32_t cur, save_at;
static uint8_t knob[4];
static struct { uint32_t magic; uint8_t preset, k[4], pad[3]; } rec;
#define REC_MAGIC 0x3159454Bu                   /* "KEY1" */

static void par_update(void)
{
    const preset_t *p = &PRESETS[cur];
    kd_par_t q;
    float v = (float)knob[3] * (1.0f / 96.0f);
    q.tone = (float)knob[0] * (1.0f / 127.0f);
    q.decay = (float)knob[1] * (1.0f / 127.0f);
    q.pick = (float)knob[2] * (1.0f / 127.0f);
    q.vol = v * v;
    memcpy(q.x, p->x, sizeof q.x);
    memcpy(&play_par, &q, sizeof q);
    play_gain = p->gain;
    out_vol = q.vol;
    play_fam = p->fam;
}
static void preset_select(uint32_t k)
{
    if (k >= NPRESET)
        return;
    cur = k;
    memcpy(knob, PRESETS[k].k, 4);
    par_update();
    save_at = H->ms() + 2000u;
}
static void knob_turn(uint32_t k, int32_t c)
{
    int32_t v = (int32_t)knob[k] + (c > 1 || c < -1 ? 3 * c : 2 * c);
    knob[k] = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
    par_update();
    save_at = H->ms() + 2000u;
}
static void knob_set(uint32_t k, uint32_t val)
{
    knob[k] = (uint8_t)(val > 127u ? 127u : val);
    par_update();
    save_at = H->ms() + 2000u;
}

static void cc_in(uint32_t num, uint32_t val)
{
    switch (num) {
    case 74: knob_set(0, val); break;           /* (fm1-dock's KNOBS CC: KNOB1..4) */
    case 71: knob_set(1, val); break;
    case 73: knob_set(2, val); break;
    case 72: knob_set(3, val); break;
    case 64: ev_put(EV_SUS, val, 0); break;
    case 120: ev_put(EV_CUT, 0, 0); break;
    case 123: ev_put(EV_ALLOFF, 0, 0); break;
    default: break;
    }
}
static void note(uint32_t ch, uint32_t key, uint32_t vel)
{
    (void)ch;
    ev_put(vel ? EV_ON : EV_OFF, key, vel);
}
static void cc(uint32_t ch, uint32_t num, uint32_t val) { (void)ch; cc_in(num, val); }
static void midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    switch (st & 0xF0u) {
    case 0x90: ev_put(d2 ? EV_ON : EV_OFF, d1, d2); break;
    case 0x80: ev_put(EV_OFF, d1, 0); break;
    case 0xB0: cc_in(d1, d2); break;
    case 0xC0: preset_select(d1 % NPRESET); break;
    default: break;
    }
}

static void ui(uint32_t ev, int32_t arg)
{
    uint32_t id = HB_EV_ID(ev);
    if (HB_EV_KIND(ev) != HB_EV_ENC)
        return;
    if (id == HB_ENC_PRESETS || id == HB_ENC_SELECT) {
        int32_t k = (int32_t)cur + arg;
        preset_select((uint32_t)(k < 0 ? 0 : k >= (int32_t)NPRESET ? (int32_t)NPRESET - 1 : k));
    } else if (id == HB_ENC_ALGORITHM) {        /* the next (previous) family's first preset */
        int32_t f = (int32_t)PRESETS[cur].fam + (arg > 0 ? 1 : -1);
        uint32_t k;
        if (f < 0 || f >= NFAM)
            return;
        for (k = 0; k < NPRESET; k++)
            if (PRESETS[k].fam == (uint32_t)f) {
                preset_select(k);
                break;
            }
    } else if (id >= HB_ENC_KNOB1 && id <= HB_ENC_KNOB4) {
        knob_turn(id - HB_ENC_KNOB1, arg);
    }
}

/* ---- the screen */
#define C_AMBER HB_RGB(255, 170, 40)
#define C_GREY  HB_RGB(130, 130, 130)
#define C_DIM   HB_RGB(60, 60, 60)
#define C_FAM   HB_RGB(120, 200, 255)
static uint32_t d_cur = ~0u, d_voices = ~0u, d_c1 = ~0u, d_ms;
static int32_t d_val[4];

static void num(char *s, int32_t v)
{
    char b[8];
    uint32_t i = 0, u = (uint32_t)(v < 0 ? -v : v);
    if (v < 0) *s++ = '-';
    do { b[i++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (i) *s++ = b[--i];
    *s = 0;
}
static void cat(char *d, const char *s) { while (*d) d++; while (*s) *d++ = *s++; *d = 0; }

static void draw(uint32_t full)
{
    char s[40], t[8];
    uint32_t k, now = H->ms(), c1 = c1_on ? 2u : c1_dead ? 0u : 1u, fam = PRESETS[cur].fam;
    if (save_at && (int32_t)(now - save_at) >= 0 && !voices_live) {   /* (when nothing sounds) */
        save_at = 0;
        rec.magic = REC_MAGIC;
        rec.preset = (uint8_t)cur;
        memcpy(rec.k, knob, 4);
        H->save(&rec, sizeof rec);
    }
    if (full) {
        H->fill(0, HB_TOP, 240, 240 - HB_TOP, 0);
        d_cur = d_voices = d_c1 = ~0u;
    }
    if (cur != d_cur) {
        uint32_t i, nf = 0, at = 0, x;
        d_cur = cur;
        for (i = 0; i < NPRESET; i++)           /* "CLAV 2 OF 4" */
            if (PRESETS[i].fam == fam) {
                nf++;
                if (i <= cur) at++;
            }
        s[0] = 0;
        cat(s, "KEYS  ");
        cat(s, FAM_NAME[fam]);
        cat(s, " ");
        num(t, (int32_t)at);
        cat(s, t);
        cat(s, " OF ");
        num(t, (int32_t)nf);
        cat(s, t);
        H->fill(0, 28, 240, 16, 0);
        H->text(0, 28, 240, 0, s, C_GREY, 1);
        H->fill(0, 46, 240, 34, 0);
        H->text(0, 46, 240, 1, PRESETS[cur].name, C_AMBER, 1);
        H->fill(0, 86, 240, 18, 0);             /* the three families, this one lit */
        for (i = 0, x = 8; i < NFAM; i++) {
            H->text((int32_t)x, 86, 80, 0, FAM_NAME[i], i == fam ? C_FAM : C_DIM, 0);
            x += (uint32_t)H->text_w(0, FAM_NAME[i]) + 22u;
        }
        for (k = 0; k < 4u; k++)
            d_val[k] = -1;
    }
    for (k = 0; k < 4u; k++) {
        int32_t v = knob[k], y = 110 + 26 * (int32_t)k, w;
        if (v == d_val[k])
            continue;
        d_val[k] = v;
        H->fill(0, y, 240, 24, 0);
        s[0] = 'K'; s[1] = (char)('1' + k); s[2] = ' '; s[3] = 0;
        cat(s, KNOB_NAME[fam][k]);
        H->text(8, y, 150, 0, s, 0xFFFFu, 0);
        num(s, v);
        H->text(150, y, 82, 0, s, C_AMBER, 0);
        w = v * 224 / 127;
        H->fill(8, y + 19, w, 2, C_AMBER);
        H->fill(8 + w, y + 19, 224 - w, 2, C_DIM);
    }
    if ((voices_live != d_voices || c1 != d_c1) && (full || now - d_ms >= 100u)) {
        d_ms = now;
        d_voices = voices_live;
        d_c1 = c1;
        s[0] = 0;
        cat(s, "VOICES ");
        num(t, (int32_t)voices_live);
        cat(s, t);
        cat(s, c1 == 2u ? " OF 24, 2 CORES" : " OF 16, 1 CORE");
        H->fill(0, 218, 240, 16, 0);
        H->text(0, 218, 240, 0, s, C_GREY, 1);
    }
}

static int init(const hb_host_t *h)
{
    uint32_t i;
    H = h;
    c1_have = HB_HOST_HAS(H, c1_wait);
    for (i = 0; i < 3u; i++)
        km_svf_set(&sb[i], SB_F[i] / KD_SR, SB_Q[i]);
    km_dc_init(&dc, 1.0f - 20.0f / KD_SR);
    if (!H->load(&rec, sizeof rec) && rec.magic == REC_MAGIC && rec.preset < NPRESET) {
        cur = rec.preset;
        memcpy(knob, rec.k, 4);
        for (i = 0; i < 4u; i++)
            if (knob[i] > 127u) knob[i] = 127u;
    } else {
        cur = 0;
        memcpy(knob, PRESETS[0].k, 4);
    }
    par_update();
    H->log_u32("keys: presets", NPRESET);
    return 0;
}

static void release(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        vo[i].gate = vo[i].active = 0;
    ev_r = ev_w;
}

HB_DESC = {HB_ABI, "KEYS", init, release, note, cc, render, ui, draw, 0, 0, 0, midi};
