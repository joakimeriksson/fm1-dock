/* SPDX-License-Identifier: GPL-3.0-only */
/* FMINI-1: FMini-1's FMINI engine (Ruppert, github.com/ruppert/fmini-1, 2.2.0) as a fm1-dock module: a monosynth in
 * the style of the classic three-oscillator Moogs (three polyBLEP oscillators and noise, the mixer's soft overload, the
 * Minimoog-style 24 dB ladder with its own contour) and its 200 factory patches, the engine's own code (take.py). Under
 * it the slice of FMini-1's host the engine reads (fmini_platform.inc); around it an interface of fm1-dock's own on
 * the FM-1's panel (FMini-1's screens, sequencer, arpeggiator and effects are not in it):
 *
 *   PRESETS   the patch: F001..F200 (the factory's), then U001.. (yours)
 *   SELECT    the page (ALGORITHM too): FILTER, F.CONTOUR, LOUDNESS, OSC 1..3, NOISE & OUT, MOD, LFO, PLAY, EXTRA
 *   KNOB1..4  the page's four controls
 *   EDIT ENV LFO FX ARP GLO   straight to FILTER, LOUDNESS, MOD, NOISE & OUT, OSC 1, PLAY
 *   SAVE      into a user slot: PRESETS picks it, SAVE again keeps it (any other button: not saved)
 *   MIDI      notes (velocity: loudness), FMini-1's CC map (74 CUTOFF, 71 EMPHASIS, 44 CONTOUR, 73 / 75 / 50 the
 *             loudness contour, 7 volume, 5 glide, ...), 1 the mod wheel (vibrato), 64 sustain, bank (0) and
 *             program change, the pitch bend (the patch's range)
 *
 * The voices: MONO and LEGATO (FMini-1's MONO patches: glide and one contour between held keys) on one voice, POLY on
 * up to 12; with the host's second core the voices are rendered in two halves, one on each core. */
#include "hb_module.h"

void *memcpy(void *d, const void *s, unsigned n);   /* (modules/lib/hb_lib.c) */
void *memset(void *d, int c, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);

static const hb_host_t *H;

#include "fmini_platform.inc"

/* ---- a sound: what a preset holds */
typedef struct {
    uint8_t g[FG_SIZE];                         /* the engine's patch (eng_fmini.c FG_*) */
    int8_t e[7];                                /* CUTOFF EMPHASIS CONTOUR F.ATK F.DEC F.SUS MOD (P_E0..P_E6) */
    uint8_t env[4];                             /* the loudness contour: ATK DEC SUS REL */
    uint8_t frel;                               /* the filter contour's RELEASE: bit 7 = its own, else the DECAY's */
    uint8_t mode;                               /* V_POLY / V_MONO / V_LEGATO */
    uint8_t level;                              /* P_LEVEL (LEVEL_Q12) */
    uint8_t lfo[3];                             /* RATE WAVE FADE */
    int8_t mod[3];                              /* LFO > PITCH, LFO > AMP, CONTOUR > PITCH */
    int8_t fenv;                                /* ENV > FILTER */
    int8_t oct;                                 /* the keys' octave */
    uint8_t bend;                               /* the bend range: +-2, +-12, +2 -12, +2 -24 */
    char name[13];
} snd_t;

static void factory(uint32_t k, snd_t *s)       /* FMini-1's factory patch k as its loads make it (fa_factory_extras) */
{
    const preset_t *p = &FMINI_PRESETS[k % FMINI_NFACTORY];
    const fmini_x_t *x = &FMINI_X[k % FMINI_NFACTORY];
    uint32_t i;
    memset(s, 0, sizeof *s);
    memcpy(s->g, FMINI_FACTORY[k % FMINI_NFACTORY], FG_SIZE);
    for (i = 0; i < 7u; i++)
        s->e[i] = p->e[i];
    for (i = 0; i < 4u; i++)
        s->env[i] = p->env[i];
    s->env[3] = s->env[1];                      /* a factory patch: the RELEASEs are its DECAYs (no PSET block) */
    s->fenv = p->fenv;
    s->mode = p->mono ? V_LEGATO : V_POLY;
    s->level = x->level;
    for (i = 0; i < 3u; i++) {
        s->lfo[i] = x->lfo[i];
        s->mod[i] = x->mod[i];
    }
    s->oct = (int8_t)clamp(x->oct, -2, 2);
    for (i = 0; i < 12u && p->name[i]; i++)
        s->name[i] = p->name[i];
}

/* ---- user slots: in the module's flash bank */
#define NUSER ((HB_BANK_MAX - 8u) / sizeof(snd_t))
#define UB_MAGIC 0x31494D46u                    /* "FMI1" */
static struct { uint32_t magic, count; snd_t s[NUSER]; } ub;
static uint32_t npatches(void) { return FMINI_NFACTORY + ub.count; }
static void patch_get(uint32_t k, snd_t *s)
{
    if (k < FMINI_NFACTORY) factory(k, s);
    else if (k - FMINI_NFACTORY < ub.count) *s = ub.s[k - FMINI_NFACTORY];
    else factory(0, s);
}

/* ---- from the main loop to the audio interrupt: events, played in order at the next render */
enum { EV_ON, EV_OFF, EV_CC, EV_BEND, EV_PANIC };
typedef struct { uint8_t type, a, b, c; } ev_t;
#define EVQ 128u
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
    RING_PUBLISH();
    ev_w = w + 1u;
}

static uint8_t key_held[128], key_sus[128], sustain;
static volatile uint8_t bend_mode;
static int16_t bend_raw;
static void bend_update(void)
{
    static const uint8_t UP[4] = {2, 12, 2, 2}, DN[4] = {2, 12, 12, 24};
    int32_t r = (int32_t)(bend_raw < 0 ? DN : UP)[bend_mode & 3u] * 256;
    bend_target = (int32_t)bend_raw * r / (bend_raw < 0 ? 8192 : 8191);
}
static void ev_play(const ev_t *e)
{
    track_t *t = &trk[0];
    uint32_t k;
    switch (e->type) {
    case EV_ON:
        key_held[e->a & 127u] = 1;
        key_sus[e->a & 127u] = 0;
        trk_note_on(t, e->a & 127u, e->b ? e->b : 1u);
        break;
    case EV_OFF:
        key_held[e->a & 127u] = 0;
        if (sustain) key_sus[e->a & 127u] = 1;
        else trk_note_off(t, e->a & 127u);
        break;
    case EV_CC:
        if (e->a == 1u) {
            t->mw = e->b;
        } else if (e->a == 64u) {
            sustain = e->b >= 64u;
            if (!sustain)
                for (k = 0; k < 128u; k++)
                    if (key_sus[k]) { key_sus[k] = 0; if (!key_held[k]) trk_note_off(t, k); }
        } else if (e->a == 120u || e->a == 123u) {
            trk_all_off(t);
            memset(key_sus, 0, sizeof key_sus);
        }
        break;
    case EV_BEND: bend_raw = (int16_t)((int32_t)(e->a | e->b << 7) - 8192); bend_update(); break;
    case EV_PANIC:
        trk_all_off(t);
        memset(key_sus, 0, sizeof key_sus);
        memset(key_held, 0, sizeof key_held);
        sustain = 0;
        break;
    }
}

/* ---- the sound: control blocks of CTL frames; the voices in two halves when the second core takes jobs */
static int32_t buf0[CTL], buf1[CTL];
static vmod_t vm[NVOICE];
static uint8_t vgo[NVOICE];
static uint32_t c1_n;
static uint8_t c1_have, c1_on, c1_dead;
static volatile uint32_t voices_live;
static void voices_render(int32_t *b, uint32_t from, uint32_t step, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        b[i] = 0;
    for (i = from; i < NVOICE; i += step)
        if (vgo[i])
            ENG_FMINI.render(&trk[0], &trk[0].v[i], b, n, &vm[i]);
}
static void c1_job(uint32_t n) { voices_render(buf1, 1, 2, n); }   /* on core 1: the odd voices */
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

static void part_block(int32_t *out, uint32_t n)
{
    track_t *t = &trk[0];
    uint32_t i, live = 0, split;
    int32_t gain;
    part_head(t);
    for (i = 0; i < NVOICE; i++) {
        vgo[i] = 0;
        if (t->v[i].active) {
            vgo[i] = (uint8_t)voice_prep(t, &t->v[i], &vm[i]);
            live += vgo[i];
        }
    }
    split = c1_on && live >= 2u && H->c1_run(c1_job, n);
    if (c1_on && live >= 2u && !split)
        c1_on = 0;                              /* (asked again next render) */
    voices_render(buf0, 0, split ? 2u : 1u, n);
    if (split) {
        c1_n++;
        if (H->c1_wait() != 0) {                /* it did not end in time: core 1 is held, its half done here */
            c1_dead = 1;
            c1_on = 0;
            voices_render(buf1, 1, 2, n);
        }
        for (i = 0; i < n; i++)
            buf0[i] += buf1[i];
    }
    /* the patch's LEVEL (unity at 112), as FMini-1 mixes a part (fx.c mix_part); then a soft knee above -2.5 dBFS where
     * FMini-1 has its master's limiter (loud chords) */
    gain = (int32_t)LEVEL_Q12[t->p[P_LEVEL] & 127];
    for (i = 0; i < n; i++) {
        int32_t s = (int32_t)(((int64_t)buf0[i] * gain) >> 12), a = s < 0 ? -s : s;
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
    poly_cap = c1_on ? NVOICE : 8u;              /* (one core: 8 voices, as many as it renders in time) */
    for (i = 0; i < n; i += CTL)
        part_block(out + 2u * i, n - i < CTL ? n - i : CTL);
    for (i = 0; i < NVOICE; i++)
        live += trk[0].v[i].active;
    voices_live = live;
}

/* ---- the main loop's side: the sound as edited, put into the part as it changes */
static snd_t snd;
static uint32_t cur, page, save_at, edited;
static int8_t key_oct[128];                     /* the octave a key was played at (its note-off follows it) */
static uint8_t bank_cc;

static void apply_glide(void)
{
    trk[0].p[P_GLIDE] = (int16_t)((snd.g[FG_SW2] & FT_GLIDE) ? snd.g[FG_GLID] : 0);
}
static void sound_apply(void)                   /* the whole sound into the part (a patch load) */
{
    track_t *t = &trk[0];
    uint32_t i, was = (uint32_t)t->p[P_VOICE];
    for (i = 0; i < 7u; i++)
        t->p[P_E0 + i] = snd.e[i];
    for (i = 0; i < 4u; i++)
        t->p[P_ATK + i] = snd.env[i];
    t->frel = snd.frel;
    t->p[P_ED_FLT] = snd.fenv;
    t->p[P_LEVEL] = snd.level;
    t->p[P_LRATE] = snd.lfo[0];
    t->p[P_LWAVE] = snd.lfo[1];
    t->p[P_LFADE] = snd.lfo[2];
    t->p[P_LD_PIT] = snd.mod[0];
    t->p[P_LD_AMP] = snd.mod[1];
    t->p[P_ED_PIT] = snd.mod[2];
    t->p[P_VOICE] = snd.mode;
    bend_mode = snd.bend & 3u;
    apply_glide();
    fmini_set_patch(0, snd.g);
    if (was != snd.mode)
        ev_put(EV_PANIC, 0, 0);                 /* another voice mode: what sounds is let go */
}

static void patch_select(uint32_t k)
{
    if (k >= npatches())
        return;
    cur = k;
    patch_get(k, &snd);
    sound_apply();
    edited = 0;
    save_at = H->ms() + 2000u;
}

/* ---- the controls */
enum { K_E, K_ENV, K_FREL, K_G, K_SW, K_LVL, K_DET, K_RANGE, K_WAVE, K_NOISE, K_MODTO, K_LFO, K_LWAVE, K_MOD,
       K_FENV, K_MODE, K_GLIDE, K_VOL, K_OCT, K_BEND, K_PRIO, K_NONE };
typedef struct { const char *label; uint8_t kind, a, b; } ctl_t;
typedef struct { const char *title; ctl_t c[4]; } page_t;
static const page_t PAGES[] = {
    {"FILTER", {{"CUTOFF", K_E, 0, 0}, {"EMPHASIS", K_E, 1, 0}, {"CONTOUR", K_E, 2, 0}, {"KBD CTRL", K_G, FG_KBD, 0}}},
    {"FILTER CONTOUR", {{"ATTACK", K_E, 3, 0}, {"DECAY", K_E, 4, 0}, {"SUSTAIN", K_E, 5, 0}, {"RELEASE", K_FREL, 0, 0}}},
    {"LOUDNESS", {{"ATTACK", K_ENV, 0, 0}, {"DECAY", K_ENV, 1, 0}, {"SUSTAIN", K_ENV, 2, 0}, {"RELEASE", K_ENV, 3, 0}}},
    {"OSC 1", {{"RANGE", K_RANGE, FG_R1, 0}, {"WAVE", K_WAVE, FG_W1, 0}, {"LEVEL", K_LVL, FG_L1, FS_O1},
               {"TUNE", K_G, FG_TUNE, 0}}},
    {"OSC 2", {{"RANGE", K_RANGE, FG_R2, 0}, {"WAVE", K_WAVE, FG_W2, 0}, {"DETUNE", K_DET, FG_DC2L, FG_F2},
               {"LEVEL", K_LVL, FG_L2, FS_O2}}},
    {"OSC 3", {{"RANGE", K_RANGE, FG_R3, 0}, {"WAVE", K_WAVE, FG_W3, 0}, {"DETUNE", K_DET, FG_DC3L, FG_F3},
               {"LEVEL", K_LVL, FG_L3, FS_O3}}},
    {"NOISE & OUT", {{"NOISE", K_LVL, FG_LN, FS_NZ}, {"COLOUR", K_NOISE, 0, 0}, {"LP / HP", K_SW, FG_SW2, FT_HP},
                     {"VOLUME", K_VOL, 0, 0}}},
    {"MODULATION", {{"MOD", K_E, 6, 0}, {"MOD MIX", K_G, FG_MMIX, 0}, {"MOD TO", K_MODTO, 0, 0}, {"LFO RATE", K_LFO, 0, 0}}},
    {"LFO", {{"WAVE", K_LWAVE, 1, 0}, {"FADE IN", K_LFO, 2, 0}, {"LFO > PITCH", K_MOD, 0, 0}, {"LFO > AMP", K_MOD, 1, 0}}},
    {"PLAY", {{"MODE", K_MODE, 0, 0}, {"GLIDE", K_GLIDE, 0, 0}, {"PRIORITY", K_PRIO, 0, 0}, {"BEND", K_BEND, 0, 0}}},
    {"EXTRA", {{"OCTAVE", K_OCT, 0, 0}, {"LFO SQUARE", K_SW, FG_SW2, FT_SQR}, {"CNTR > PITCH", K_MOD, 2, 0},
               {"ENV > FILTER", K_FENV, 0, 0}}},
};
#define NPAGES NELEM(PAGES)
enum { PG_FILTER, PG_FCONT, PG_LOUD, PG_OSC1, PG_OSC2, PG_OSC3, PG_NOISE, PG_MOD, PG_LFO, PG_PLAY, PG_EXTRA };

static int32_t det_get(uint32_t lo) { return fmini_det(snd.g, lo); }
static int32_t cget(const ctl_t *c)
{
    switch (c->kind) {
    case K_E: return snd.e[c->a];
    case K_ENV: return snd.env[c->a];
    case K_FREL: return FMINI_FREL(&trk[0]);
    case K_G: case K_RANGE: case K_WAVE: return snd.g[c->a];
    case K_SW: return (snd.g[c->a] & c->b) != 0;
    case K_LVL: return snd.g[FG_SW1] & c->b ? snd.g[c->a] : -1;
    case K_DET: return det_get(c->a);
    case K_NOISE: return (snd.g[FG_SW1] & FS_PINK) != 0;
    case K_MODTO: return ((snd.g[FG_SW1] & FS_MOSC) ? 1 : 0) | ((snd.g[FG_SW1] & FS_MFLT) ? 2 : 0);
    case K_LFO: return snd.lfo[c->a];
    case K_LWAVE: return snd.lfo[1];
    case K_MOD: return snd.mod[c->a];
    case K_FENV: return snd.fenv;
    case K_MODE: return snd.mode;
    case K_GLIDE: return snd.g[FG_SW2] & FT_GLIDE ? snd.g[FG_GLID] : -1;
    case K_VOL: return snd.level;
    case K_OCT: return snd.oct;
    case K_BEND: return snd.bend;
    case K_PRIO: return glo_note_prio;
    default: return 0;
    }
}
static void lims(const ctl_t *c, int32_t *lo, int32_t *hi)
{
    *lo = 0;
    *hi = 127;
    switch (c->kind) {
    case K_SW: case K_NOISE: *hi = 1; break;
    case K_LVL: case K_GLIDE: *lo = -1; break;
    case K_DET: *lo = -700; *hi = 700; break;
    case K_RANGE: *hi = 5; break;
    case K_WAVE: *hi = 6; break;
    case K_MODTO: *hi = 3; break;
    case K_LWAVE: *hi = 4; break;
    case K_MOD: case K_FENV: *lo = -64; *hi = 63; break;
    case K_MODE: case K_PRIO: *hi = 2; break;
    case K_OCT: *lo = -2; *hi = 2; break;
    case K_BEND: *hi = 3; break;
    case K_G: if (c->a == FG_GLID) *hi = 127; break;
    default: break;
    }
}
static void cset(const ctl_t *c, int32_t v)
{
    track_t *t = &trk[0];
    int32_t lo, hi;
    uint32_t patch = 0;
    lims(c, &lo, &hi);
    v = clamp(v, lo, hi);
    if (v == cget(c))
        return;
    switch (c->kind) {
    case K_E: snd.e[c->a] = (int8_t)v; t->p[P_E0 + c->a] = (int16_t)v; break;
    case K_ENV: snd.env[c->a] = (uint8_t)v; t->p[P_ATK + c->a] = (int16_t)v; break;
    case K_FREL: snd.frel = (uint8_t)(0x80u | (uint32_t)v); t->frel = snd.frel; break;
    case K_G: case K_RANGE: case K_WAVE: snd.g[c->a] = (uint8_t)v; patch = 1; break;
    case K_SW: snd.g[c->a] = (uint8_t)(v ? snd.g[c->a] | c->b : snd.g[c->a] & ~c->b); patch = 1; break;
    case K_LVL:
        if (v < 0) snd.g[FG_SW1] &= (uint8_t)~c->b;
        else { snd.g[FG_SW1] |= c->b; snd.g[c->a] = (uint8_t)v; }
        patch = 1;
        break;
    case K_DET:
        fmini_det_put(snd.g, c->a, v);
        snd.g[c->b] = (uint8_t)clamp(64 + (v >= 0 ? (v + 5) / 11 : -((-v + 5) / 11)), 0, 127);   /* (the coarse byte: 11 cents a step) */
        patch = 1;
        break;
    case K_NOISE: snd.g[FG_SW1] = (uint8_t)(v ? snd.g[FG_SW1] | FS_PINK : snd.g[FG_SW1] & ~FS_PINK); patch = 1; break;
    case K_MODTO:
        snd.g[FG_SW1] = (uint8_t)((snd.g[FG_SW1] & ~(FS_MOSC | FS_MFLT)) | (v & 1 ? FS_MOSC : 0) | (v & 2 ? FS_MFLT : 0));
        patch = 1;
        break;
    case K_LFO: snd.lfo[c->a] = (uint8_t)v; t->p[P_LRATE + c->a] = (int16_t)v; break;
    case K_LWAVE: snd.lfo[1] = (uint8_t)v; t->p[P_LWAVE] = (int16_t)v; break;
    case K_MOD:
        snd.mod[c->a] = (int8_t)v;
        t->p[c->a == 0 ? P_LD_PIT : c->a == 1 ? P_LD_AMP : P_ED_PIT] = (int16_t)v;
        break;
    case K_FENV: snd.fenv = (int8_t)v; t->p[P_ED_FLT] = (int16_t)v; break;
    case K_MODE: snd.mode = (uint8_t)v; t->p[P_VOICE] = (int16_t)v; ev_put(EV_PANIC, 0, 0); break;
    case K_GLIDE:
        if (v < 0) snd.g[FG_SW2] &= (uint8_t)~FT_GLIDE;
        else { snd.g[FG_SW2] |= FT_GLIDE; snd.g[FG_GLID] = (uint8_t)v; }
        apply_glide();
        patch = 1;
        break;
    case K_VOL: snd.level = (uint8_t)v; t->p[P_LEVEL] = (int16_t)v; break;
    case K_OCT: snd.oct = (int8_t)v; break;
    case K_BEND: snd.bend = (uint8_t)v; bend_mode = (uint8_t)v; break;
    case K_PRIO: glo_note_prio = (uint8_t)v; break;
    default: return;
    }
    if (patch)
        fmini_set_patch(0, snd.g);
    if (c->kind != K_PRIO)
        edited = 1;
    save_at = H->ms() + 2000u;
}

/* MIDI CC: FMini-1's map out of the box (fmini_app.c FA_CC0), 0..127 over each control's range */
typedef struct { uint8_t cc, page, k; } ccmap_t;
static const ccmap_t CCMAP[] = {
    {20, PG_OSC1, 3}, {21, PG_MOD, 1}, {22, PG_MOD, 3}, {27, PG_OSC1, 0}, {28, PG_OSC2, 0}, {29, PG_OSC3, 0},
    {30, PG_OSC2, 2}, {31, PG_OSC1, 1}, {33, PG_OSC2, 1}, {34, PG_OSC3, 1}, {62, PG_OSC3, 2}, {35, PG_OSC1, 2},
    {36, PG_OSC2, 3}, {37, PG_OSC3, 3}, {39, PG_NOISE, 0}, {63, PG_NOISE, 1}, {74, PG_FILTER, 0}, {71, PG_FILTER, 1},
    {44, PG_FILTER, 2}, {45, PG_FILTER, 3}, {72, PG_NOISE, 2}, {46, PG_FCONT, 0}, {47, PG_FCONT, 1}, {48, PG_FCONT, 2},
    {49, PG_FCONT, 3}, {73, PG_LOUD, 0}, {75, PG_LOUD, 1}, {50, PG_LOUD, 2}, {7, PG_NOISE, 3}, {26, PG_EXTRA, 1},
};
static void cc_abs(const ctl_t *c, uint32_t val)
{
    int32_t lo, hi;
    lims(c, &lo, &hi);
    if (c->kind == K_SW || c->kind == K_NOISE)
        cset(c, val >= 64u);
    else if (c->kind == K_LVL)
        cset(c, (int32_t)val);                  /* (a level: its switch on) */
    else
        cset(c, lo + (int32_t)val * (hi - lo + 1) / 128);
}
static void cc_in(uint32_t num, uint32_t val)
{
    uint32_t i;
    switch (num) {
    case 0: bank_cc = (uint8_t)val; return;
    case 5: {                                   /* GLIDE time; 0: off */
        static const ctl_t GL = {"GLIDE", K_GLIDE, 0, 0};
        cset(&GL, val ? (int32_t)val : -1);
        return;
    }
    case 23: case 24: {                         /* MOD OSC, MOD FLT: switches */
        static const ctl_t MT = {"MOD TO", K_MODTO, 0, 0};
        int32_t v = cget(&MT), bit = num == 23u ? 1 : 2;
        cset(&MT, val >= 64u ? v | bit : v & ~bit);
        return;
    }
    case 25: {                                  /* the GLIDE switch */
        static const ctl_t GL = {"GLIDE", K_GLIDE, 0, 0};
        cset(&GL, val >= 64u ? (snd.g[FG_GLID] ? snd.g[FG_GLID] : 40) : -1);
        return;
    }
    case 40: case 41: case 42: case 43: {       /* the mixer's switches */
        static const uint8_t BIT[4] = {FS_O1, FS_O2, FS_O3, FS_NZ};
        snd.g[FG_SW1] = (uint8_t)(val >= 64u ? snd.g[FG_SW1] | BIT[num - 40u] : snd.g[FG_SW1] & ~BIT[num - 40u]);
        fmini_set_patch(0, snd.g);
        edited = 1;
        return;
    }
    case 76: {                                  /* FILT. MOD */
        static const ctl_t MT = {"MOD TO", K_MODTO, 0, 0};
        int32_t v = cget(&MT);
        cset(&MT, val >= 64u ? v | 2 : v & ~2);
        return;
    }
    case 1: case 64: case 120: case 123: ev_put(EV_CC, num, val); return;
    default: break;
    }
    for (i = 0; i < NELEM(CCMAP); i++)
        if (CCMAP[i].cc == num) {
            cc_abs(&PAGES[CCMAP[i].page].c[CCMAP[i].k], val);
            return;
        }
}

static void note(uint32_t ch, uint32_t key, uint32_t vel)   /* the FM-1's keys: the patch's octave */
{
    int32_t n;
    (void)ch;
    key &= 127u;
    if (vel)
        key_oct[key] = snd.oct;
    n = (int32_t)key + key_oct[key] * 12;
    if (n < 0 || n > 127)
        return;
    ev_put(vel ? EV_ON : EV_OFF, (uint32_t)n, vel);
}
static void cc(uint32_t ch, uint32_t num, uint32_t val) { (void)ch; cc_in(num, val); }
static void midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    switch (st & 0xF0u) {
    case 0x90: ev_put(d2 ? EV_ON : EV_OFF, d1, d2); break;
    case 0x80: ev_put(EV_OFF, d1, 0); break;
    case 0xB0: cc_in(d1, d2); break;
    case 0xC0: patch_select(bank_cc * 128u + d1); break;
    case 0xE0: ev_put(EV_BEND, d1, d2); break;
    default: break;
    }
}

/* ---- the panel */
static uint32_t saving, save_to;                /* SAVE: the user slot it would go to */
static void knob(uint32_t k, int32_t c)
{
    const ctl_t *x = &PAGES[page].c[k];
    int32_t step = c > 1 || c < -1 ? 2 * c : c;
    if (x->kind == K_DET)
        step = c > 1 || c < -1 ? 10 * c : c;
    cset(x, cget(x) + step);
}
static void ui(uint32_t ev, int32_t arg)
{
    uint32_t id = HB_EV_ID(ev);
    if (HB_EV_KIND(ev) == HB_EV_BTN_DOWN) {
        static const struct { uint8_t btn, page; } JUMP[] = {
            {HB_BTN_EDIT, PG_FILTER}, {HB_BTN_ENV, PG_LOUD}, {HB_BTN_LFO, PG_MOD}, {HB_BTN_FX, PG_NOISE},
            {HB_BTN_ARP, PG_OSC1}, {HB_BTN_GLO, PG_PLAY},
        };
        uint32_t i;
        if (id == HB_BTN_SAVE) {
            if (!saving) {
                saving = 1;
                save_to = cur >= FMINI_NFACTORY ? cur - FMINI_NFACTORY : ub.count;
                if (save_to >= NUSER) save_to = NUSER - 1u;
                return;
            }
            saving = 0;
            if (save_to > ub.count) save_to = ub.count;
            ub.s[save_to] = snd;
            if (save_to == ub.count) ub.count++;
            ub.magic = UB_MAGIC;
            if (H->bank_save(&ub, sizeof ub))   /* (the whole bank: bank_load takes it back by its length) */
                H->log("fmini: the user slots are not kept (the bank)");
            cur = FMINI_NFACTORY + save_to;
            edited = 0;
            save_at = H->ms() + 2000u;
            return;
        }
        saving = 0;
        for (i = 0; i < NELEM(JUMP); i++)
            if (JUMP[i].btn == id) page = JUMP[i].page;
        return;
    }
    if (HB_EV_KIND(ev) != HB_EV_ENC)
        return;
    if (id == HB_ENC_PRESETS) {
        if (saving) {
            int32_t s = (int32_t)save_to + arg, mx = (int32_t)(ub.count < NUSER ? ub.count : NUSER - 1u);
            save_to = (uint32_t)clamp(s, 0, mx);
            return;
        }
        {
            int32_t k = (int32_t)cur + arg, n = (int32_t)npatches();
            patch_select((uint32_t)(((k % n) + n) % n));
        }
    } else if (id == HB_ENC_SELECT || id == HB_ENC_ALGORITHM) {
        page = (uint32_t)(((int32_t)page + arg) % (int32_t)NPAGES + (int32_t)NPAGES) % NPAGES;
        save_at = H->ms() + 2000u;
    } else if (id >= HB_ENC_KNOB1 && id <= HB_ENC_KNOB4) {
        knob(id - HB_ENC_KNOB1, arg);
    }
}

/* ---- the screen (FMini-1's colours: orange values on black) */
#define C_OR    HB_RGB(0xE0, 0x75, 0x2B)
#define C_CREAM HB_RGB(0xF1, 0xE9, 0xD2)
#define C_GREY  HB_RGB(130, 130, 130)
#define C_DIM   HB_RGB(60, 60, 60)
static uint32_t d_cur = ~0u, d_page = ~0u, d_voices = ~0u, d_c1 = ~0u, d_ms, d_ed = ~0u, d_sv = ~0u;
static int32_t d_val[4];

static void num(char *s, int32_t v, int sign)
{
    char b[8];
    uint32_t i = 0, u = (uint32_t)(v < 0 ? -v : v);
    while (*s) s++;
    if (v < 0) *s++ = '-';
    else if (sign && v > 0) *s++ = '+';
    do { b[i++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (i) *s++ = b[--i];
    *s = 0;
}
static void cat(char *d, const char *s) { while (*d) d++; while (*s) *d++ = *s++; *d = 0; }
static void num3(char *s, uint32_t v) { while (*s) s++; s[0] = (char)('0' + v / 100u % 10u); s[1] = (char)('0' + v / 10u % 10u); s[2] = (char)('0' + v % 10u); s[3] = 0; }
static void ms_text(char *s, uint32_t v)        /* a time knob: TIME_MS_X10 */
{
    uint32_t x = TIME_MS_X10[v & 127u];
    if (x < 10000u) { num(s, (int32_t)((x + 5u) / 10u), 0); cat(s, " MS"); }
    else { num(s, (int32_t)(x / 10000u), 0); cat(s, "."); num(s, (int32_t)(x / 1000u % 10u), 0); cat(s, " S"); }
}
static void dial(char *s, int32_t v, int32_t lo, int32_t span)   /* a dial -5 .. +5 or 0 .. 10, one decimal */
{
    int32_t x = lo * 10 + (v * span * 10 + 63) / 127;
    if (x < 0) { cat(s, "-"); x = -x; }
    num(s, x / 10, 0);
    cat(s, ".");
    num(s, x % 10, 0);
}
static void value_text(const ctl_t *c, char *s)
{
    static const char *const RANGE[6] = {"LO", "32'", "16'", "8'", "4'", "2'"};
    static const char *const WAVE[7] = {"TRI", "SHARK", "SAW", "SQR", "PLS W", "PLS N", "SIN"};
    static const char *const MODTO[4] = {"OFF", "OSC", "FILTER", "BOTH"};
    static const char *const LW[5] = {"SINE", "TRI", "SAW", "SQUARE", "S&H"};
    static const char *const MODE[3] = {"POLY", "MONO", "LEGATO"};
    static const char *const PRIO[3] = {"LAST", "LOW", "HIGH"};
    static const char *const BEND[4] = {"+/-2", "+/-12", "+2/-12", "+2/-24"};
    int32_t v = cget(c);
    s[0] = 0;
    switch (c->kind) {
    case K_E:
        if (c->a == 0) dial(s, v, -5, 10);
        else if (c->a == 1) dial(s, v, 0, 10);
        else if (c->a == 3 || c->a == 4) ms_text(s, (uint32_t)v);
        else num(s, v, 0);
        break;
    case K_ENV: if (c->a == 2) num(s, v, 0); else ms_text(s, (uint32_t)v); break;
    case K_FREL: ms_text(s, (uint32_t)v); break;
    case K_G:
        if (c->a == FG_TUNE) { num(s, (v - 64) * 100 / 64, 1); cat(s, " CT"); }
        else num(s, v, 0);
        break;
    case K_SW: cat(s, c->b == FT_HP ? (v ? "HP" : "LP") : v ? "ON" : "OFF"); break;
    case K_LVL: if (v < 0) cat(s, "OFF"); else num(s, v, 0); break;
    case K_DET: num(s, v, 1); cat(s, " CT"); break;
    case K_RANGE: cat(s, RANGE[v % 6]); break;
    case K_WAVE: cat(s, WAVE[v % 7]); break;
    case K_NOISE: cat(s, v ? "PINK" : "WHITE"); break;
    case K_MODTO: cat(s, MODTO[v & 3]); break;
    case K_LFO:
        if (c->a == 0) {                        /* RATE in Hz, two decimals */
            uint32_t h = LFO_HZ_X100[v & 127];
            num(s, (int32_t)(h / 100u), 0);
            cat(s, h % 100u < 10u ? ".0" : ".");
            num(s, (int32_t)(h % 100u), 0);
            cat(s, " HZ");
        } else {
            ms_text(s, (uint32_t)v);
        }
        break;
    case K_LWAVE: cat(s, LW[v % 5]); break;
    case K_MOD: case K_FENV: num(s, v, 1); break;
    case K_MODE: cat(s, MODE[v % 3]); break;
    case K_GLIDE: if (v < 0) cat(s, "OFF"); else num(s, v, 0); break;
    case K_VOL: num(s, v, 0); break;
    case K_OCT: num(s, v, 1); break;
    case K_BEND: cat(s, BEND[v & 3]); break;
    case K_PRIO: cat(s, PRIO[v % 3]); break;
    default: break;
    }
}
static int bar(const ctl_t *c, int32_t *w)      /* a bar under the value: its share of the range */
{
    int32_t lo, hi, v = cget(c);
    if (c->kind == K_SW || c->kind == K_NOISE || c->kind == K_RANGE || c->kind == K_WAVE || c->kind == K_MODTO ||
        c->kind == K_LWAVE || c->kind == K_MODE || c->kind == K_PRIO || c->kind == K_BEND || c->kind == K_OCT)
        return 0;
    lims(c, &lo, &hi);
    if (v < 0 && (c->kind == K_LVL || c->kind == K_GLIDE)) v = 0, lo = 0;
    *w = (v - lo) * 224 / (hi - lo ? hi - lo : 1);
    return 1;
}

static struct { uint32_t magic; uint16_t cur; uint8_t page, prio; uint8_t edited, pad[3]; snd_t snd; } rec;
#define REC_MAGIC 0x32494D46u                   /* "FMI2" */

static void draw(uint32_t full)
{
    char s[40];
    uint32_t k, now = H->ms(), c1 = c1_on ? 2u : 1u;
    if (save_at && (int32_t)(now - save_at) >= 0 && !voices_live) {   /* (when nothing sounds) */
        save_at = 0;
        rec.magic = REC_MAGIC;
        rec.cur = (uint16_t)cur;
        rec.page = (uint8_t)page;
        rec.prio = glo_note_prio;
        rec.edited = (uint8_t)edited;
        rec.snd = snd;
        H->save(&rec, sizeof rec);
    }
    if (full) {
        H->fill(0, HB_TOP, 240, 240 - HB_TOP, 0);
        d_cur = d_page = d_voices = d_c1 = d_ed = d_sv = ~0u;
    }
    if (cur != d_cur || edited != d_ed || saving != d_sv || (saving && save_to != d_sv >> 8)) {
        d_cur = cur;
        d_ed = edited;
        d_sv = saving | save_to << 8;
        s[0] = 0;
        if (saving) {
            cat(s, "SAVE TO U");
            num3(s, save_to + 1u);
            cat(s, "?  SAVE: YES");
        } else {
            cat(s, cur < FMINI_NFACTORY ? "F" : "U");      /* "F004 OF 200", "U002 OF 3" (yours) */
            num3(s, cur < FMINI_NFACTORY ? cur + 1u : cur - FMINI_NFACTORY + 1u);
            cat(s, " OF ");
            num(s, (int32_t)(cur < FMINI_NFACTORY ? FMINI_NFACTORY : ub.count), 0);
            if (edited) cat(s, "  EDITED");
        }
        H->fill(0, 26, 240, 18, 0);
        H->text(0, 28, 240, 0, s, saving ? C_CREAM : C_GREY, 1);
        H->fill(0, 46, 240, 34, 0);
        H->text(0, H->text_w(1, snd.name) <= 236 ? 46 : 54, 240, (uint32_t)(H->text_w(1, snd.name) <= 236),
                snd.name[0] ? snd.name : "-", C_OR, 1);
        d_page = ~0u;
    }
    if (page != d_page) {
        d_page = page;
        s[0] = 0;
        cat(s, "< ");
        cat(s, PAGES[page].title);
        cat(s, " >");
        H->fill(0, 84, 240, 20, 0);
        H->text(0, 86, 240, 0, s, C_CREAM, 1);
        for (k = 0; k < 4u; k++)
            d_val[k] = 0x7FFFFFFF;
    }
    for (k = 0; k < 4u; k++) {
        const ctl_t *x = &PAGES[page].c[k];
        int32_t v = cget(x), y = 110 + 26 * (int32_t)k, w;
        if (v == d_val[k])
            continue;
        d_val[k] = v;
        H->fill(0, y, 240, 24, 0);
        s[0] = 'K'; s[1] = (char)('1' + k); s[2] = ' '; s[3] = 0;
        cat(s, x->label);
        H->text(8, y, 140, 0, s, 0xFFFFu, 0);
        value_text(x, s);
        H->text(148, y, 88, 0, s, C_OR, 0);
        if (bar(x, &w)) {
            H->fill(8, y + 19, w, 2, C_OR);
            H->fill(8 + w, y + 19, 224 - w, 2, C_DIM);
        }
    }
    if ((voices_live != d_voices || c1 != d_c1) && (full || now - d_ms >= 100u)) {
        d_ms = now;
        d_voices = voices_live;
        d_c1 = c1;
        s[0] = 0;
        if (trk[0].p[P_VOICE] != V_POLY) {
            cat(s, trk[0].p[P_VOICE] == V_MONO ? "MONO" : "LEGATO");
            cat(s, voices_live ? "  PLAYING" : "");
        } else {
            cat(s, "POLY ");
            num(s, (int32_t)voices_live, 0);
            cat(s, c1 == 2u ? " OF 12" : " OF 8");
            cat(s, c1 == 2u ? ", 2 CORES" : ", 1 CORE");
        }
        H->fill(0, 216, 240, 20, 0);
        H->text(0, 218, 240, 0, s, C_GREY, 1);
    }
}

static int init(const hb_host_t *h)
{
    H = h;
    c1_have = HB_HOST_HAS(H, c1_wait);
    fmini_init();
    trk[0].eng_req = ENGI_FMINI;
    if (H->bank_load(&ub, sizeof ub) != 0 || ub.magic != UB_MAGIC || ub.count > NUSER)
        ub.count = 0;
    if (!H->load(&rec, sizeof rec) && rec.magic == REC_MAGIC) {
        cur = rec.cur < npatches() ? rec.cur : 0;
        page = rec.page < NPAGES ? rec.page : 0;
        glo_note_prio = rec.prio < 3u ? rec.prio : 1u;
        patch_select(cur);
        if (rec.edited) {                       /* the sound as it was left */
            snd = rec.snd;
            fmini_sanitize(snd.g);
            sound_apply();
            edited = 1;
        }
    } else {
        patch_select(0);
    }
    save_at = 0;
    H->log_u32("fmini: FMini-1 2.2.0, patches", npatches());
    return 0;
}

static void release(void)
{
    trk_all_off(&trk[0]);
    ev_r = ev_w;
    saving = 0;
}

HB_DESC = {HB_ABI, "FMINI-1", init, release, note, cc, render, ui, draw, 0, 0, 0, midi};
