/* SPDX-License-Identifier: GPL-3.0-only */
/* PROPHET: Melodee's Prophet-5 Rev 4 engine (Kerem Kilic / Ellic Studio, github.com/keremimo/melodee, a playable
 * prototype whose fidelity is not hardware-verified) as a fm1-dock module. The engine is Melodee's own file
 * (melodee/eng_prophet_test.c, taken by take.py): its fp32 voices with band-limited oscillators, Poly-Mod, the two
 * filter characters (SSI, Curtis) from a coefficient table, both ADSRs, the wheel LFO, Vintage, glide and unison; its
 * sample kernels in RAM (.ram_text), its native 133-byte patches. Under it, this file is what Melodee's track,
 * voice, allocator and second core give the engine (voice.c, engines.c, audio_worker.h in Melodee):
 *   - one part (track_t) with Melodee's voices (voice_t), a control block of 32 frames (CTL), vmod_t as voice.c
 *     fills it for an engine with its own envelopes (ownenv), the pitch bend smoothed as there;
 *   - the allocator: POLY (the same key reuses its voice, else the free voice nearest in pitch, else the oldest
 *     released, else the oldest held but the lowest), UNISON as Melodee's mono_play (the patch's count and detune,
 *     LOW or LAST priority, legato or retriggered by the patch), the sustain pedal;
 *   - the second core: Melodee's audio worker on the host's c1_run / c1_wait. The engine pairs its voices (one on
 *     core 1 while the next renders on core 0) and plays 8 voices while core 1 takes jobs, 5 (the Prophet's own)
 *     while it does not (being screened, failed, or a host without one).
 * The keys and MIDI come in from the main loop and are played at the start of the next render, in order.
 *
 * Patches: INIT (Melodee's) and eight made for this module. Sequential's factory programs, which Melodee carries
 * under Sequential's rights (no licence granted), are not in it: send your own copy of a Prophet-5/10 bank (native
 * single-program dumps, as Sequential's v1.03 .syx) with `tools/dock.py send FILE.syx`; it is kept in the module's
 * flash banks and its programs follow the module's own.
 *
 *   PRESETS   the patch            SELECT    the page (MAIN, OSC A ... BEND: Melodee's tone pages)
 *   KNOB1..4  the page's controls  MIDI      notes, velocity, CC 1 wheel, 64 sustain, 74 cutoff, 71 resonance,
 *                                            73 attack, 72 release, 0 bank (x 128) + program change, bend,
 *                                            channel / key pressure */
#include "hb_module.h"

void *memcpy(void *d, const void *s, unsigned n);   /* (modules/lib/hb_lib.c) */
void *memset(void *d, int c, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);

static const hb_host_t *H;

/* ---- what Melodee's core.h, dsp.c and voice.c give the engine */
#define MELODEE_DUAL_CORE 1
#define P5_OVERSAMPLE 1
#define NPART 1u
#define NTRK 1u
#define NVOICE 8u                               /* = the engine's P5_POLY with the second core */
#define NPOLY 8                                 /* (Melodee's: unison's detune spread) */
#define VOICE_FS 24000                          /* (dsp.c: one voice's level) */
#define RES_OK() 1
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))
#include "p5_tables.h"                          /* (take.py: FS, CTL, the pitch table, ENV_LIN, P5_*) */

enum { V_POLY, V_MONO, V_LEGATO, V_UNISON };   /* P_VOICE */
enum { F_INT, F_SEMI };
enum { P_VOICE, P_PRIO, P_ALLOC, P_DETUNE, P_RESERVED34, P_RESERVED36,
       P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7, P_COUNT };
typedef struct {
    const char *label;
    uint8_t fmt;
    int16_t min, max, def;
    const char *const *names;
    const char *unit;
} param_desc_t;
typedef struct {                                /* (Melodee's voice_t: what the engine reads of it) */
    uint8_t note, gate, active, mvel;
    int32_t pitch16, pitch_cur;                 /* 1/16 semitone (the engine glides itself) */
    int32_t fine;                               /* unison detune, 1/4096 */
    int32_t env_out;                            /* the last block's amplitude, Q15 */
    uint32_t ph[3];
    uint32_t age;
} voice_t;
typedef struct {
    uint32_t inc;
    int32_t pitch16;
    int32_t amp0, amp1;
    int32_t cutoff;
    int32_t shape;
    int32_t envq15;
    int32_t fine;
    int32_t plog;
} vmod_t;
typedef struct track {
    int16_t p[P_COUNT];
    voice_t *v;
    uint8_t mw, at;                             /* CC 1, pressure */
    int16_t bend_raw;                           /* -8192..8191 */
} track_t;
typedef struct { uint8_t env[4]; } preset_t;
typedef struct engine {
    const char *name;
    const char *page_title[2];
    param_desc_t edit[8];
    uint8_t npresets, poly, ownenv;
    uint8_t knob[4];
    void (*preset_make)(uint32_t k, preset_t *p, char *name);
    void (*note_on)(struct track *t, voice_t *v);
    void (*render)(struct track *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m);
    int (*done)(struct track *t, voice_t *v);
    void (*block)(struct track *t);
    uint32_t (*units)(const struct track *t);
    uint32_t (*cap)(const struct track *t);
    void (*legato)(struct track *t, voice_t *v);
    void (*post)(struct track *t, int32_t *out, uint32_t n, uint32_t nr);
} engine_t;

static voice_t vo[NVOICE];
static track_t trk[NTRK] = {{.v = vo}};
static uint8_t voice_was;                       /* what the voice a note starts did before: 0 free, 1 released, 2 held */
static uint8_t midi_verb[NTRK];                 /* (Melodee's MIDI release override: none) */

static inline __attribute__((always_inline)) int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}
static inline __attribute__((always_inline)) uint32_t noise32(int32_t *st)   /* xorshift32 */
{
    uint32_t s = (uint32_t)*st;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *st = (int32_t)s;
    return s;
}
#define tuned_pitch_inc pitch_inc               /* (A4 = 440 Hz) */
static inline uint32_t cents_inc(int32_t pitch16, int32_t ct, int32_t fine)
{
    int32_t d16 = ct * 16 / 100, rem = ct * 16 - d16 * 100;
    uint32_t inc = tuned_pitch_inc((uint32_t)clamp(pitch16 + d16, 0, 2047));
    return inc + (uint32_t)((int32_t)(inc >> 12) * (rem * 2367 / 16000 + fine));
}
static void str_cpy(char *d, const char *s, uint32_t max)
{
    uint32_t i = 0;
    while (s[i] && i + 1u < max) { d[i] = s[i]; i++; }
    d[i] = 0;
}
/* Melodee's batched renderer (audio_batch.h) is not used here */
#define audio_batch_active 0
#define AUDIO_BATCH_BYTES 256u
#define AP_PROPHET 0u
static inline void audio_batch_push(void (*k)(const void *, int32_t *), const void *d, uint32_t sz, int32_t *out,
                                    uint32_t n, uint32_t ap)
{
    (void)k; (void)d; (void)sz; (void)out; (void)n; (void)ap;
}

/* ---- Melodee's audio worker (audio_worker.h) on the host's second core (c1_run / c1_wait), from the render only.
 * audio_worker_online: core 1 took the last job offered (the engine then plays 8 voices and pairs them). Until it
 * does (the host is screening it, or it failed), a probe each render asks again; a job it does not finish in time
 * (c1_wait -1: the host holds core 1 from then on) is rendered on core 0, from where core 1 left it. */
typedef void (*audio_worker_fn)(void *);
static int audio_worker_online;
static uint8_t c1_have, c1_dead, aw_out;
static audio_worker_fn aw_fn;
static void *aw_ctx;
static uint32_t c1_jobs;
static void aw_job(uint32_t arg) { (void)arg; aw_fn(aw_ctx); }   /* on core 1 */
static void aw_nop(uint32_t arg) { (void)arg; }
static int audio_worker_submit(audio_worker_fn fn, void *ctx)
{
    if (!audio_worker_online || aw_out)
        return 0;
    aw_fn = fn;
    aw_ctx = ctx;
    if (!H->c1_run(aw_job, 0)) {
        audio_worker_online = 0;                /* (asked again next render) */
        return 0;
    }
    aw_out = 1;
    c1_jobs++;
    return 1;
}
static int audio_worker_join(void)
{
    if (!aw_out)
        return 1;
    aw_out = 0;
    if (H->c1_wait() != 0) {
        c1_dead = 1;
        audio_worker_online = 0;
        aw_fn(aw_ctx);
    }
    return 1;
}
static void c1_probe(void)
{
    if (!c1_have || c1_dead || audio_worker_online)
        return;
    if (H->c1_run(aw_nop, 0)) {
        if (H->c1_wait() == 0) audio_worker_online = 1;
        else c1_dead = 1;
    }
}


/* ---- Melodee's engine (take.py) */
static void p5_update_bend(track_t *t);
#include "eng_prophet_test.c"
#define ENG ENG_P5_TEST

static p5_part_t p5_state;                      /* (engines.c's p5_part: the part's state in its arena) */
static p5_part_t *p5_part(uint32_t part) { (void)part; return &p5_state; }

static int32_t bend_q8, bend_target;            /* the bend, Q8 semitones: smoothed as voice.c does */
static void p5_update_bend(track_t *t)          /* (voice.c's: the patch's own range, 1..12 semitones) */
{
    int32_t range = (clamp(p5_patch_of(t)->raw[P5_BEND], 0, 11) + 1) * 256;
    bend_target = t->bend_raw * range / (t->bend_raw < 0 ? 8192 : 8191);
}

/* ---- the voices (the audio interrupt's) */
static uint32_t vage;
static uint8_t key_held[128], key_sus[128], sustain;
static uint8_t mstack[16], nmono, mono_cur, mono_vel;
static volatile uint32_t voices_live;           /* for the screen */

static int unison(void) { return trk[0].p[P_VOICE] == V_UNISON; }

static void voice_start(voice_t *v, uint32_t note, uint32_t vel)   /* (voice.c's, for an engine with its own envelopes) */
{
    track_t *t = &trk[0];
    int sounding = v->active;
    voice_was = (uint8_t)(!v->active ? 0u : v->gate ? 2u : 1u);
    v->note = (uint8_t)note;
    v->gate = v->active = 1;
    v->age = ++vage;
    v->pitch16 = v->pitch_cur = (int32_t)note * 16;
    v->mvel = (uint8_t)vel;
    if (!sounding)
        v->env_out = 0;
    ENG.note_on(t, v);
}

static void poly_on(uint32_t note, uint32_t vel)   /* (voice.c voice_alloc, with P_ALLOC: the nearest free voice) */
{
    track_t *t = &trk[0];
    uint32_t i, np = ENG.cap(t), best = np, low = np;
    int32_t bd = 0x7FFFFFFF;
    if (np > NVOICE) np = NVOICE;
    for (i = 0; i < np; i++)
        if (vo[i].active && vo[i].note == note) {
            voice_start(&vo[i], note, vel);
            return;
        }
    for (i = 0; i < np; i++) {
        int32_t d = vo[i].pitch_cur - (int32_t)note * 16;
        if (vo[i].active)
            continue;
        d = d < 0 ? -d : d;
        if (!vo[i].pitch_cur)
            d = 0x7FFFFFF0;                     /* never played: last choice */
        if (d < bd) { bd = d; best = i; }
    }
    if (best == np) {
        for (i = 0; i < np; i++)                /* the lowest held note is kept */
            if (vo[i].gate && (low == np || vo[i].note < vo[low].note))
                low = i;
        for (i = 0; i < np; i++)                /* the oldest released */
            if (!vo[i].gate && (best == np || vo[i].age < vo[best].age))
                best = i;
        if (best == np)
            for (i = 0; i < np; i++)            /* the oldest held, not the bass */
                if (i != low && (best == np || vo[i].age < vo[best].age))
                    best = i;
    }
    voice_start(&vo[best == np ? 0 : best], note, vel);
}

/* UNISON: the patch's count of voices on one note, spread by its detune (voice.c mono_play) */
static void mono_play(uint32_t note, uint32_t vel, int retrig)
{
    track_t *t = &trk[0];
    uint32_t nv = ENG.cap(t), i;
    static uint32_t seed = 0x1234567u;
    if (nv > NVOICE) nv = NVOICE;
    for (i = 0; i < nv; i++) {
        voice_t *v = &vo[i];
        int32_t k = 2 * (int32_t)i - (int32_t)(nv - 1u);
        if (nv > 1u && nv != NPOLY)
            k = k * (NPOLY - 1) / (int32_t)(nv - 1u);
        v->fine = nv > 1u ? k * t->p[P_DETUNE] * 56 / 889 : 0;
        if (retrig || !v->active || !v->gate) {
            if (i && !v->active) {              /* free-running oscillators: in phase they would stack */
                seed = seed * 1664525u + 1013904223u;
                v->ph[0] = seed;
                v->ph[1] = seed * 747796405u;
            }
            voice_start(v, note, vel);
        } else {                                /* legato: the new pitch, the same envelopes (or retriggered) */
            v->note = (uint8_t)note;
            v->pitch16 = v->pitch_cur = (int32_t)note * 16;
            v->mvel = (uint8_t)vel;
            ENG.legato(t, v);
        }
    }
    mono_cur = (uint8_t)note;
}
static uint32_t mono_pick(void)                 /* LOW (P_PRIO 1) or LAST */
{
    uint32_t i, n = mstack[nmono - 1u];
    if (trk[0].p[P_PRIO] == 1)
        for (i = 0; i < nmono; i++)
            if (mstack[i] < n) n = mstack[i];
    return n;
}
static void mono_drop(uint32_t note)
{
    uint32_t i, j;
    for (i = j = 0; i < nmono; i++)
        if (mstack[i] != note) mstack[j++] = mstack[i];
    nmono = (uint8_t)j;
}

static void gates_off(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        vo[i].gate = 0;
    nmono = 0;
    memset(key_sus, 0, sizeof key_sus);
}

static void note_release(uint32_t key)
{
    uint32_t i;
    if (unison()) {
        uint32_t was = nmono;
        mono_drop(key);
        if (!nmono) {
            if (was) for (i = 0; i < NVOICE; i++) vo[i].gate = 0;
        } else if (mono_pick() != mono_cur) {
            mono_play(mono_pick(), mono_vel, 0);
        }
        return;
    }
    for (i = 0; i < NVOICE; i++)
        if (vo[i].gate && vo[i].note == key)
            vo[i].gate = 0;
}
static void key_on(uint32_t key, uint32_t vel)
{
    key_held[key] = 1;
    key_sus[key] = 0;
    if (unison()) {
        int first = !nmono;
        mono_drop(key);
        if (nmono < sizeof mstack) mstack[nmono++] = (uint8_t)key;
        mono_vel = (uint8_t)vel;
        if (first || mono_pick() != mono_cur)
            mono_play(mono_pick(), vel, first);
        return;
    }
    poly_on(key, vel);
}
static void key_off(uint32_t key)
{
    key_held[key] = 0;
    if (sustain) { key_sus[key] = 1; return; }
    note_release(key);
}

/* ---- from the main loop to the audio interrupt: events, played in order at the next render */
enum { EV_ON, EV_OFF, EV_CC, EV_BEND, EV_AT, EV_EDIT, EV_PATCH, EV_PANIC };
typedef struct { uint8_t type, a, b, c; } ev_t;
#define EVQ 128u
static ev_t evq[EVQ];
static volatile uint32_t ev_w, ev_r;
static p5_patch_t stage;                        /* EV_PATCH's patch (the main loop writes it only while none is pending) */
static volatile uint8_t stage_pending;

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

static int voice_param(uint32_t id) { return id == P5_UNISON || id == P5_UNISON_COUNT || id == P5_UNISON_DETUNE || id == P5_RETRIGGER; }

static void ev_play(const ev_t *e)
{
    track_t *t = &trk[0];
    uint32_t k;
    switch (e->type) {
    case EV_ON: key_on(e->a & 127u, e->b ? e->b : 1u); break;
    case EV_OFF: key_off(e->a & 127u); break;
    case EV_CC:
        if (e->a == 1u) t->mw = e->b;
        else if (e->a == 64u) {
            sustain = e->b >= 64u;
            if (!sustain)
                for (k = 0; k < 128u; k++)
                    if (key_sus[k]) { key_sus[k] = 0; if (!key_held[k]) note_release(k); }
        } else if (e->a == 120u || e->a == 123u) {
            gates_off();
        }
        break;
    case EV_BEND: t->bend_raw = (int16_t)((int32_t)(e->a | e->b << 7) - 8192); break;
    case EV_AT: t->at = e->a; break;
    case EV_EDIT: {
        int was = unison();
        p5_patch[0].raw[e->a] = e->b;
        if (voice_param(e->a)) {
            p5_track_accept(t);
            if (was != unison()) gates_off();
        }
        break;
    }
    case EV_PATCH:
        p5_patch[0] = stage;
        p5_ready[0] = 1;
        stage_pending = 0;
        p5_track_accept(t);
        gates_off();
        break;
    case EV_PANIC: gates_off(); t->mw = t->at = 0; t->bend_raw = 0; sustain = 0; break;
    }
}

/* ---- the sound: control blocks of CTL frames, as Melodee's voice.c renders a part */
static int32_t mono_buf[CTL];

static void part_block(int32_t *out, uint32_t n)
{
    track_t *t = &trk[0];
    uint32_t i, nr = 0, live = 0;
    int32_t d = bend_target - bend_q8, bend16, bend_fine, sh;
    bend_q8 += d > 0 ? (d < 4 ? d : (d + 3) / 4) : (d > -4 ? d : (d - 3) / 4);
    for (i = 0; i < n; i++)
        mono_buf[i] = 0;
    ENG.block(t);
    for (i = 0; i < NVOICE; i++)
        live |= vo[i].active;
    if (!live) {
        ENG.post(t, mono_buf, n, 0);
        return;
    }
    bend16 = bend_q8 >= 0 ? bend_q8 / 16 : -((-bend_q8 + 15) / 16);
    bend_fine = (bend_q8 - bend16 * 16) * 2367 / 2560;
    for (i = 0; i < NVOICE; i++) {
        voice_t *v = &vo[i];
        vmod_t m;
        int32_t amp = 32767;
        if (!v->active)
            continue;
        if (ENG.done(t, v) || (i >= 5u && !audio_worker_online)) {   /* its envelopes ended, or a voice past the
                                                 * Prophet's five without core 1 (it was lost): one more block, faded */
            v->active = v->gate = 0;
            amp = 0;
        }
        m.amp0 = v->env_out;
        m.amp1 = amp;
        v->env_out = amp;
        m.envq15 = 0;
        m.pitch16 = clamp(v->pitch_cur + bend16, 0, 2047);
        m.inc = pitch_inc((uint32_t)m.pitch16);
        m.fine = v->fine + bend_fine;
        if (m.fine)
            m.inc += (uint32_t)((int32_t)(m.inc >> 12) * m.fine);
        m.cutoff = 0;
        m.shape = 64 << 8;
        m.plog = 0;
        ENG.render(t, v, mono_buf, n, &m);
        nr++;
    }
    ENG.post(t, mono_buf, n, nr);
    /* one voice peaks near +-48000 (Melodee's 2 x VOICE_FS): 3/8 of it, a unison stack half that, and a soft
     * knee above -2.5 dBFS */
    sh = unison() ? 4 : 3;
    for (i = 0; i < n; i++) {
        int32_t s = (mono_buf[i] * 3) >> sh, a = s < 0 ? -s : s;
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
    for (i = 0; i < n; i += CTL)
        part_block(out + 2u * i, n - i < CTL ? n - i : CTL);
    for (i = 0; i < NVOICE; i++)
        live += vo[i].active;
    voices_live = live;
}

/* ---- the patches: INIT and the module's own, then a bank the player sent (imported) */
typedef struct { const char *name; const uint8_t *kv; } own_t;   /* kv: id, value ... 255 */
static const uint8_t KV_SYNC[] = {
    P5_FREQ_A, 62, P5_SYNC, 1, P5_POLY_ENV, 70, P5_POLY_FREQ, 1, P5_CUTOFF, 80, P5_RESONANCE, 30, P5_KEY_FILTER, 2,
    P5_ENV_FILTER, 40, P5_DECAY_FILTER, 85, P5_SUSTAIN_FILTER, 20, P5_RELEASE_FILTER, 60, P5_DECAY_AMP, 90,
    P5_SUSTAIN_AMP, 110, P5_RELEASE_AMP, 50, P5_VEL_AMP, 1, P5_VEL_FILTER, 1, P5_LFO_TRI, 1, P5_LFO_RATE, 85,
    P5_WHEEL_FREQ_A, 1, P5_WHEEL_FREQ_B, 1, 255};
static const uint8_t KV_BRASS[] = {
    P5_SAW_B, 1, P5_FINE_B, 8, P5_LEVEL_A, 110, P5_LEVEL_B, 110, P5_CUTOFF, 40, P5_RESONANCE, 10, P5_KEY_FILTER, 1,
    P5_ENV_FILTER, 70, P5_ATTACK_FILTER, 55, P5_DECAY_FILTER, 85, P5_SUSTAIN_FILTER, 70, P5_RELEASE_FILTER, 60,
    P5_ATTACK_AMP, 35, P5_DECAY_AMP, 90, P5_SUSTAIN_AMP, 115, P5_RELEASE_AMP, 55, P5_VEL_FILTER, 1, P5_VEL_AMP, 1,
    P5_VINTAGE, 42, P5_LFO_TRI, 1, P5_LFO_RATE, 82, P5_WHEEL_FREQ_A, 1, P5_WHEEL_FREQ_B, 1, 255};
static const uint8_t KV_STRINGS[] = {
    P5_SAW_A, 0, P5_PULSE_A, 1, P5_PW_A, 40, P5_SAW_B, 1, P5_FREQ_B, 48, P5_FINE_B, 10, P5_LEVEL_A, 120, P5_LEVEL_B, 80,
    P5_CUTOFF, 72, P5_KEY_FILTER, 1, P5_FILTER_REV, 0, P5_ENV_FILTER, 20, P5_ATTACK_FILTER, 90, P5_DECAY_FILTER, 100,
    P5_SUSTAIN_FILTER, 90, P5_RELEASE_FILTER, 95, P5_ATTACK_AMP, 88, P5_DECAY_AMP, 100, P5_SUSTAIN_AMP, 120,
    P5_RELEASE_AMP, 98, P5_LFO_TRI, 1, P5_LFO_RATE, 72, P5_LFO_INITIAL, 50, P5_WHEEL_PW_A, 1, P5_VEL_AMP, 1,
    P5_VINTAGE, 42, 255};
static const uint8_t KV_BELL[] = {
    P5_SAW_A, 0, P5_PULSE_A, 1, P5_TRI_B, 1, P5_FREQ_B, 62, P5_LEVEL_B, 0, P5_POLY_B, 80, P5_POLY_FREQ, 1,
    P5_CUTOFF, 95, P5_RESONANCE, 15, P5_KEY_FILTER, 2, P5_ENV_FILTER, 30, P5_DECAY_FILTER, 95, P5_SUSTAIN_FILTER, 0,
    P5_RELEASE_FILTER, 95, P5_DECAY_AMP, 105, P5_SUSTAIN_AMP, 0, P5_RELEASE_AMP, 100, P5_VEL_AMP, 1, P5_VEL_FILTER, 1,
    255};
static const uint8_t KV_BASS[] = {
    P5_FREQ_A, 0, P5_FREQ_B, 0, P5_PULSE_B, 1, P5_PW_B, 30, P5_LEVEL_B, 100, P5_CUTOFF, 25, P5_RESONANCE, 70,
    P5_KEY_FILTER, 1, P5_FILTER_REV, 0, P5_ENV_FILTER, 85, P5_DECAY_FILTER, 70, P5_SUSTAIN_FILTER, 5,
    P5_RELEASE_FILTER, 40, P5_DECAY_AMP, 95, P5_SUSTAIN_AMP, 100, P5_RELEASE_AMP, 30, P5_VEL_FILTER, 1, P5_VEL_AMP, 1,
    255};
static const uint8_t KV_UNISON[] = {
    P5_UNISON, 1, P5_UNISON_COUNT, 5, P5_UNISON_DETUNE, 3, P5_RETRIGGER, 2, P5_SAW_B, 1, P5_FINE_B, 4,
    P5_LEVEL_A, 115, P5_LEVEL_B, 115, P5_GLIDE, 85, P5_CUTOFF, 82, P5_RESONANCE, 35, P5_KEY_FILTER, 2,
    P5_FILTER_REV, 0, P5_ENV_FILTER, 45, P5_ATTACK_FILTER, 10, P5_DECAY_FILTER, 85, P5_SUSTAIN_FILTER, 50,
    P5_RELEASE_FILTER, 60, P5_DECAY_AMP, 90, P5_SUSTAIN_AMP, 120, P5_RELEASE_AMP, 55, P5_LFO_TRI, 1, P5_LFO_RATE, 88,
    P5_WHEEL_FREQ_A, 1, P5_WHEEL_FREQ_B, 1, P5_VEL_AMP, 1, P5_VINTAGE, 60, 255};
static const uint8_t KV_PWM[] = {
    P5_SAW_A, 0, P5_PULSE_A, 1, P5_PW_A, 55, P5_TRI_B, 1, P5_FREQ_B, 48, P5_LEVEL_A, 120, P5_LEVEL_B, 60,
    P5_CUTOFF, 60, P5_RESONANCE, 5, P5_KEY_FILTER, 2, P5_ENV_FILTER, 50, P5_DECAY_FILTER, 92, P5_SUSTAIN_FILTER, 20,
    P5_RELEASE_FILTER, 75, P5_DECAY_AMP, 100, P5_SUSTAIN_AMP, 50, P5_RELEASE_AMP, 75, P5_LFO_TRI, 1, P5_LFO_RATE, 78,
    P5_LFO_INITIAL, 45, P5_WHEEL_PW_A, 1, P5_VEL_AMP, 1, P5_VEL_FILTER, 1, 255};
static const uint8_t KV_SWEEP[] = {
    P5_SAW_B, 1, P5_FREQ_B, 38, P5_LEVEL_A, 120, P5_LEVEL_B, 90, P5_CUTOFF, 40, P5_RESONANCE, 105, P5_KEY_FILTER, 1,
    P5_FILTER_REV, 0, P5_LFO_TRI, 1, P5_LFO_RATE, 45, P5_LFO_INITIAL, 80, P5_WHEEL_FILTER, 1, P5_ENV_FILTER, 20,
    P5_ATTACK_FILTER, 60, P5_DECAY_FILTER, 100, P5_SUSTAIN_FILTER, 80, P5_RELEASE_FILTER, 90, P5_ATTACK_AMP, 60,
    P5_DECAY_AMP, 100, P5_SUSTAIN_AMP, 120, P5_RELEASE_AMP, 95, P5_VEL_AMP, 1, 255};
static const own_t OWN[] = {
    {"SYNC LEAD", KV_SYNC}, {"BRASS", KV_BRASS}, {"STRINGS", KV_STRINGS}, {"POLY-MOD BELL", KV_BELL},
    {"FUNK BASS", KV_BASS}, {"UNISON LEAD", KV_UNISON}, {"PWM KEYS", KV_PWM}, {"RESO SWEEP", KV_SWEEP},
};
#define NOWN (1u + NELEM(OWN))                  /* with INIT */

#define IMP_MAX 200u
#define IMP_MAGIC 0x4D493550u                   /* "P5IM": the imported bank in the module's flash banks 1.. */
static struct { uint32_t magic, count; p5_patch_t p[IMP_MAX]; } imp;

static uint32_t npatches(void) { return NOWN + imp.count; }
static void patch_get(uint32_t k, p5_patch_t *p)
{
    if (k >= NOWN) {
        *p = imp.p[k - NOWN];
        return;
    }
    p5_patch_init(p);                           /* Melodee's INIT, then the module's own changes */
    if (k) {
        const uint8_t *kv = OWN[k - 1u].kv;
        const char *s = OWN[k - 1u].name;
        uint32_t i;
        for (; kv[0] != 255u; kv += 2)
            p->raw[kv[0]] = kv[1];
        memset(p->raw + P5_NAME, ' ', P5_NAME_LEN);
        for (i = 0; s[i] && i < P5_NAME_LEN; i++)
            p->raw[P5_NAME + i] = (uint8_t)s[i];
    }
}

/* ---- the main loop's side: the patch as edited (ui_patch, what the audio interrupt gets as events) */
static p5_patch_t ui_patch;
static uint32_t cur, page, want_stage, save_at;
static struct { uint32_t magic; uint16_t patch; uint8_t page, pad; } rec;
#define REC_MAGIC 0x31503550u
static uint8_t bank_cc;

static void stage_patch(void)
{
    if (stage_pending) { want_stage = 1; return; }
    stage = ui_patch;
    stage_pending = 1;
    ev_put(EV_PATCH, 0, 0);
}
static void patch_select(uint32_t k)
{
    if (k >= npatches())
        return;
    cur = k;
    patch_get(k, &ui_patch);
    stage_patch();
    save_at = H->ms() + 2000u;
}
static void edit(uint32_t id, int32_t v)
{
    v = clamp(v, 0, 127);
    if (ui_patch.raw[id] == (uint8_t)v)
        return;
    ui_patch.raw[id] = (uint8_t)v;
    ev_put(EV_EDIT, id, (uint32_t)v);
}

static void cc_in(uint32_t num, uint32_t val)
{
    switch (num) {
    case 0: bank_cc = (uint8_t)val; break;
    case 74: edit(P5_CUTOFF, (int32_t)val); break;          /* (fm1-dock's KNOBS CC: KNOB1..4) */
    case 71: edit(P5_RESONANCE, (int32_t)val); break;
    case 73: edit(P5_ATTACK_AMP, (int32_t)val); edit(P5_ATTACK_FILTER, (int32_t)val); break;
    case 72: edit(P5_RELEASE_AMP, (int32_t)val); edit(P5_RELEASE_FILTER, (int32_t)val); break;
    case 1: case 64: case 120: case 123: ev_put(EV_CC, num, val); break;
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
    case 0xC0: patch_select(bank_cc * 128u + d1); break;
    case 0xE0: ev_put(EV_BEND, d1, d2); break;
    case 0xD0: ev_put(EV_AT, d1, 0); break;
    case 0xA0: ev_put(EV_AT, d2, 0); break;     /* (a key's pressure, the host's sensed keys: as the channel's) */
    default: break;
    }
}

/* ---- the pages: Melodee's tone pages (docs/PROPHET5_PROTOTYPE.md), MAIN first */
enum { K_KNOB, K_SW, K_TUNE, K_TRACK, K_MODEL, K_VOICES, K_DETUNE, K_PRIO, K_BEND, K_NONE };
typedef struct { uint8_t id, kind; const char *label; } ctl_t;
typedef struct { const char *title; ctl_t c[4]; } page_t;
static const page_t PAGES[] = {
    {"MAIN", {{P5_CUTOFF, K_KNOB, "CUTOFF"}, {P5_RESONANCE, K_KNOB, "RESONANCE"}, {P5_ENV_FILTER, K_KNOB, "ENV AMOUNT"},
              {P5_RELEASE_AMP, K_KNOB, "AMP RELEASE"}}},
    {"OSC A", {{P5_FREQ_A, K_TUNE, "FREQ A"}, {P5_SAW_A, K_SW, "SAW A"}, {P5_PULSE_A, K_SW, "PULSE A"},
               {P5_PW_A, K_KNOB, "WIDTH A"}}},
    {"OSC B", {{P5_FREQ_B, K_TUNE, "FREQ B"}, {P5_FINE_B, K_KNOB, "FINE B"}, {P5_PW_B, K_KNOB, "WIDTH B"},
               {P5_KEY_B, K_SW, "KEY B"}}},
    {"B WAVES", {{P5_SAW_B, K_SW, "SAW B"}, {P5_TRI_B, K_SW, "TRI B"}, {P5_PULSE_B, K_SW, "PULSE B"},
                 {P5_LOW_B, K_SW, "LOW B"}}},
    {"MIXER", {{P5_LEVEL_A, K_KNOB, "OSC A"}, {P5_LEVEL_B, K_KNOB, "OSC B"}, {P5_NOISE, K_KNOB, "NOISE"},
               {P5_SYNC, K_SW, "SYNC"}}},
    {"FILTER", {{P5_CUTOFF, K_KNOB, "CUTOFF"}, {P5_RESONANCE, K_KNOB, "RESONANCE"}, {P5_KEY_FILTER, K_TRACK, "KEYBOARD"},
                {P5_FILTER_REV, K_MODEL, "MODEL"}}},
    {"FILTER ENV", {{P5_ATTACK_FILTER, K_KNOB, "ATTACK"}, {P5_DECAY_FILTER, K_KNOB, "DECAY"},
                    {P5_SUSTAIN_FILTER, K_KNOB, "SUSTAIN"}, {P5_RELEASE_FILTER, K_KNOB, "RELEASE"}}},
    {"AMP ENV", {{P5_ATTACK_AMP, K_KNOB, "ATTACK"}, {P5_DECAY_AMP, K_KNOB, "DECAY"}, {P5_SUSTAIN_AMP, K_KNOB, "SUSTAIN"},
                 {P5_RELEASE_AMP, K_KNOB, "RELEASE"}}},
    {"ENV MOD", {{P5_ENV_FILTER, K_KNOB, "ENV AMOUNT"}, {P5_VEL_FILTER, K_SW, "VEL FILTER"}, {P5_VEL_AMP, K_SW, "VEL AMP"},
                 {P5_RELEASE_ON, K_SW, "RELEASE"}}},
    {"POLY-MOD", {{P5_POLY_ENV, K_KNOB, "FILTER ENV"}, {P5_POLY_B, K_KNOB, "OSC B"}, {P5_POLY_FREQ, K_SW, "> FREQ A"},
                  {P5_POLY_PW, K_SW, "> PW A"}}},
    {"MOD FILTER", {{P5_POLY_FILTER, K_SW, "PM > FILTER"}, {P5_VINTAGE, K_KNOB, "VINTAGE"},
                    {P5_PRESS_FILTER, K_SW, "AT > FILTER"}, {P5_PRESS_LFO, K_SW, "AT > LFO"}}},
    {"LFO", {{P5_LFO_RATE, K_KNOB, "RATE"}, {P5_LFO_SAW, K_SW, "SAW"}, {P5_LFO_TRI, K_SW, "TRIANGLE"},
             {P5_LFO_PULSE, K_SW, "SQUARE"}}},
    {"WHEEL", {{P5_LFO_INITIAL, K_KNOB, "INITIAL"}, {P5_WHEEL_MIX, K_KNOB, "NOISE MIX"}, {P5_WHEEL_FREQ_A, K_SW, "> FREQ A"},
               {P5_WHEEL_FREQ_B, K_SW, "> FREQ B"}}},
    {"WHEEL 2", {{P5_WHEEL_PW_A, K_SW, "> PW A"}, {P5_WHEEL_PW_B, K_SW, "> PW B"}, {P5_WHEEL_FILTER, K_SW, "> FILTER"},
                 {P5_GLIDE, K_KNOB, "GLIDE"}}},
    {"UNISON", {{P5_UNISON, K_SW, "UNISON"}, {P5_UNISON_COUNT, K_VOICES, "VOICES"}, {P5_UNISON_DETUNE, K_DETUNE, "DETUNE"},
                {P5_RETRIGGER, K_PRIO, "PRIORITY"}}},
    {"BEND", {{P5_BEND, K_BEND, "BEND RANGE"}, {0, K_NONE, ""}, {0, K_NONE, ""}, {0, K_NONE, ""}}},
};
#define NPAGES NELEM(PAGES)

static int keyed(uint32_t id) { return id == P5_FREQ_A || (id == P5_FREQ_B && ui_patch.raw[P5_KEY_B]); }

static void knob(uint32_t k, int32_t c)
{
    const ctl_t *x = &PAGES[page].c[k];
    int32_t v = ui_patch.raw[x->id];
    switch (x->kind) {
    case K_KNOB: edit(x->id, v + (c > 1 || c < -1 ? 2 * c : c)); break;
    case K_SW: edit(x->id, c > 0); break;
    case K_TUNE:
        if (!keyed(x->id)) { edit(x->id, v + c); break; }
        edit(x->id, (clamp(clamp(v, 0, 96) / 2 - 12 + c, -12, 36) + 12) * 2);   /* (semitone detents, as Melodee's panel) */
        break;
    case K_TRACK: edit(x->id, clamp(v + c, 0, 2)); break;
    case K_MODEL: edit(x->id, c > 0); break;
    case K_VOICES: edit(x->id, clamp((v ? v : 5) + c, 1, 5)); break;
    case K_DETUNE: edit(x->id, clamp(v + c, 0, 7)); break;
    case K_PRIO: edit(x->id, clamp(v + c, 0, 3)); break;
    case K_BEND: edit(x->id, clamp(v + c, 0, 11)); break;
    default: break;
    }
}

static void ui(uint32_t ev, int32_t arg)
{
    uint32_t id = HB_EV_ID(ev);
    if (HB_EV_KIND(ev) != HB_EV_ENC)
        return;
    if (id == HB_ENC_PRESETS) {
        int32_t k = (int32_t)cur + arg, n = (int32_t)npatches();
        patch_select((uint32_t)(k < 0 ? 0 : k >= n ? n - 1 : k));
    } else if (id == HB_ENC_SELECT || id == HB_ENC_ALGORITHM) {
        page = (uint32_t)(((int32_t)page + arg) % (int32_t)NPAGES + (int32_t)NPAGES) % NPAGES;
        save_at = H->ms() + 2000u;
    } else if (id >= HB_ENC_KNOB1 && id <= HB_ENC_KNOB4) {
        knob(id - HB_ENC_KNOB1, arg);
    }
}

/* ---- the screen */
#define C_AMBER HB_RGB(255, 170, 40)
#define C_GREY  HB_RGB(130, 130, 130)
#define C_DIM   HB_RGB(60, 60, 60)
#define C_PAGE  HB_RGB(120, 200, 255)
static uint32_t d_cur = ~0u, d_page = ~0u, d_voices = ~0u, d_c1 = ~0u, d_ms;
static int32_t d_val[4];
static char d_name[24];

static void num(char *s, int32_t v, int sign)
{
    char b[8];
    uint32_t i = 0, u = (uint32_t)(v < 0 ? -v : v);
    if (v < 0) *s++ = '-';
    else if (sign && v > 0) *s++ = '+';
    do { b[i++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (i) *s++ = b[--i];
    *s = 0;
}
static void cat(char *d, const char *s) { while (*d) d++; while (*s) *d++ = *s++; *d = 0; }

static void value_text(const ctl_t *x, char *s)
{
    static const char *const TRACK[] = {"OFF", "HALF", "FULL"}, *const PRIO[] = {"LOW", "LOW RETRIG", "LAST", "LAST RETRIG"};
    int32_t v = ui_patch.raw[x->id];
    s[0] = 0;
    switch (x->kind) {
    case K_KNOB: num(s, v, 0); break;
    case K_SW: cat(s, v ? "ON" : "OFF"); break;
    case K_TUNE:
        if (keyed(x->id)) { num(s, clamp(v, 0, 96) / 2 - 12, 1); cat(s, " ST"); }
        else num(s, v, 0);
        break;
    case K_TRACK: cat(s, TRACK[clamp(v, 0, 2)]); break;
    case K_MODEL: cat(s, v ? "CURTIS" : "SSI"); break;
    case K_VOICES: num(s, v ? clamp(v, 1, 5) : 5, 0); break;
    case K_DETUNE: num(s, v, 0); break;
    case K_PRIO: cat(s, PRIO[v & 3]); break;
    case K_BEND: num(s, clamp(v, 0, 11) + 1, 1); cat(s, " ST"); break;
    default: break;
    }
}

static void draw(uint32_t full)
{
    char s[40], name[24];
    uint32_t k, now = H->ms(), c1 = audio_worker_online ? 2u : c1_dead ? 0u : 1u;
    int big;
    if (want_stage && !stage_pending) {
        want_stage = 0;
        stage_patch();
    }
    if (save_at && (int32_t)(now - save_at) >= 0 && !voices_live) {   /* (when nothing sounds) */
        save_at = 0;
        rec.magic = REC_MAGIC;
        rec.patch = (uint16_t)cur;
        rec.page = (uint8_t)page;
        H->save(&rec, sizeof rec);
    }
    if (full) {
        H->fill(0, HB_TOP, 240, 240 - HB_TOP, 0);
        d_cur = d_page = d_voices = d_c1 = ~0u;
        d_name[0] = 1;
    }
    p5_patch_name(name, &ui_patch);
    if (cur != d_cur || memcmp(name, d_name, sizeof d_name)) {
        d_cur = cur;
        memcpy(d_name, name, sizeof d_name);
        {                                       /* "PROPHET 3 OF 8" (the module's own), "BANK 12 OF 200" (sent) */
            char t[8];
            s[0] = 0;
            cat(s, !cur ? "PROPHET  INIT" : cur < NOWN ? "PROPHET " : "BANK ");
            if (cur) {
                num(t, (int32_t)(cur < NOWN ? cur : cur - NOWN + 1u), 0);
                cat(s, t);
                cat(s, " OF ");
                num(t, (int32_t)(cur < NOWN ? NOWN - 1u : imp.count), 0);
                cat(s, t);
            }
        }
        H->text(0, 28, 240, 0, s, C_GREY, 1);
        big = H->text_w(1, name) <= 236;
        for (k = 0; name[k]; k++)
            if (name[k] >= 'a' && name[k] <= 'z') big = 0;
        H->fill(0, 46, 240, 34, 0);
        H->text(0, big ? 46 : 54, 240, (uint32_t)big, name[0] ? name : "-", C_AMBER, 1);
        d_page = ~0u;                           /* (the values may all have changed) */
    }
    if (page != d_page) {
        d_page = page;
        s[0] = 0;
        cat(s, "< ");
        cat(s, PAGES[page].title);
        cat(s, " >");
        H->text(0, 86, 240, 0, s, C_PAGE, 1);
        for (k = 0; k < 4u; k++)
            d_val[k] = -1;
    }
    for (k = 0; k < 4u; k++) {
        const ctl_t *x = &PAGES[page].c[k];
        int32_t v = x->kind == K_NONE ? -2 : ui_patch.raw[x->id] | (x->kind == K_TUNE && keyed(x->id) ? 0x100 : 0);
        int32_t y = 110 + 26 * (int32_t)k;
        if (v == d_val[k])
            continue;
        d_val[k] = v;
        if (x->kind == K_NONE) {
            H->fill(0, y, 240, 24, 0);
            continue;
        }
        s[0] = 'K'; s[1] = (char)('1' + k); s[2] = ' '; s[3] = 0;
        cat(s, x->label);
        H->text(8, y, 150, 0, s, 0xFFFFu, 0);
        value_text(x, s);
        H->text(158, y, 74, 0, s, C_AMBER, 0);
        if (x->kind == K_KNOB) {                /* a bar under it */
            int32_t w = (ui_patch.raw[x->id] & 127) * 224 / 127;
            H->fill(8, y + 19, w, 2, C_AMBER);
            H->fill(8 + w, y + 19, 224 - w, 2, C_DIM);
        } else {
            H->fill(8, y + 19, 224, 2, 0);
        }
    }
    if ((voices_live != d_voices || c1 != d_c1) && (full || now - d_ms >= 100u)) {
        d_ms = now;
        d_voices = voices_live;
        d_c1 = c1;
        s[0] = 0;
        cat(s, "VOICES ");
        num(s + 7, (int32_t)voices_live, 0);
        cat(s, c1 == 2u ? " OF 8, 2 CORES" : " OF 5, 1 CORE");
        H->text(0, 218, 240, 0, s, C_GREY, 1);
    }
}

/* ---- a bank from the computer (tools/dock.py send FILE.syx): Prophet-5/10 native program dumps, into the
 * module's flash banks 1.. (kept, after the module's own patches) */
static uint8_t fbuf[40960];
static uint32_t fsize;
static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int imp_save(void)
{
    const uint8_t *b = (const uint8_t *)&imp;
    uint32_t n = 8u + imp.count * (uint32_t)sizeof(p5_patch_t), idx = 1;
    if (!HB_HOST_HAS(H, bank_free))
        return -1;
    while (n) {
        uint32_t k = n < HB_BANK_MAX ? n : HB_BANK_MAX;
        if (H->bank_save_n(idx++, b, k))
            return -1;
        b += k;
        n -= k;
    }
    return 0;
}
static void imp_load(void)
{
    uint8_t *b = (uint8_t *)&imp;
    uint32_t got = 0, idx = 1;
    imp.count = 0;
    if (!HB_HOST_HAS(H, bank_free))
        return;
    while (got < sizeof imp) {
        uint32_t k = sizeof imp - got < HB_BANK_MAX ? sizeof imp - got : HB_BANK_MAX;
        int r = H->bank_load_n(idx++, b + got, k);
        if (r <= 0)
            break;
        got += (uint32_t)r;
        if (got >= 8u && got >= 8u + imp.count * (uint32_t)sizeof(p5_patch_t) && imp.count <= IMP_MAX)
            break;
    }
    if (imp.magic != IMP_MAGIC || imp.count > IMP_MAX || got < 8u + imp.count * (uint32_t)sizeof(p5_patch_t))
        imp.count = 0;
}
static int data(const uint8_t *p, uint32_t n)
{
    uint32_t i, j, cnt = 0;
    if (!n)
        return 1;
    if (p[0] == HB_FILE_BEGIN) {
        if (n < 9u || rd32(p + 1) > sizeof fbuf) return 3;   /* (a Prophet bank is 200 x 159 bytes) */
        fsize = rd32(p + 1);
        return 0;
    }
    if (p[0] == HB_FILE_DATA) {
        uint32_t at = n >= 5u ? rd32(p + 1) : ~0u;
        if (at > fsize || n - 5u > fsize - at) return 4;
        memcpy(fbuf + at, p + 5, n - 5u);
        return 0;
    }
    if (p[0] != HB_FILE_END)
        return 1;
    for (i = 0; i < fsize && cnt < IMP_MAX; i++) {     /* each F0 .. F7 frame that is a native program */
        if (fbuf[i] != 0xF0u)
            continue;
        for (j = i + 1u; j < fsize && fbuf[j] != 0xF7u && fbuf[j] != 0xF0u; j++)
            ;
        if (j < fsize && fbuf[j] == 0xF7u && p5_patch_decode(&imp.p[cnt], fbuf + i, j - i + 1u))
            cnt++;
        i = j - 1u;
    }
    if (!cnt)
        return 2;                               /* no Prophet-5/10 program in it */
    imp.magic = IMP_MAGIC;
    imp.count = cnt;
    H->log_u32("prophet: programs in", cnt);
    if (imp_save())
        H->log("prophet: the bank is not kept (no flash bank free)");
    patch_select(NOWN);
    return 0;
}

static int init(const hb_host_t *h)
{
    H = h;
    c1_have = HB_HOST_HAS(H, c1_wait);
    imp_load();
    if (!H->load(&rec, sizeof rec) && rec.magic == REC_MAGIC) {
        cur = rec.patch < npatches() ? rec.patch : 0;
        page = rec.page < NPAGES ? rec.page : 0;
    }
    patch_get(cur, &ui_patch);
    p5_patch[0] = ui_patch;                     /* (the audio interrupt does not run the module yet) */
    p5_ready[0] = 1;
    p5_track_accept(&trk[0]);
    H->log_u32("prophet: Melodee e9b07b4, programs", npatches());
    return 0;
}

static void release(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        vo[i].gate = vo[i].active = 0;
    nmono = 0;
    ev_r = ev_w;
    stage_pending = 0;
}

HB_DESC = {HB_ABI, "PROPHET", init, release, note, cc, render, ui, draw, 0, 0, HB_F_DATA, midi, data};
