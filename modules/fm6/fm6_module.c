/* SPDX-License-Identifier: GPL-3.0-only */
/* FM6: Melodee's six-operator FM engine (Kerem Kilic / Ellic Studio, github.com/keremimo/melodee), a DX7-style
 * synth that plays DX7 voices the way Dexed plays them, as a fm1-dock module. The engine is Melodee's own files
 * (melodee/eng_fm6.c and fm6_core.c, taken by take.py): the 32 algorithms, feedback, the log-domain envelopes with
 * the DX7's attack and static times, level and rate key scaling, velocity, the LFO, pitch envelope, portamento and
 * the controllers; three renderers (MODERN: MSFA in fp32, MARK I: the DX7's log-sine and exponent tables, OPL); its
 * operator kernels in RAM (.ram_text); its 155-byte single-voice patches. Under it, this file is what Melodee's
 * track, voice, allocator and second core give the engine (voice.c, engines.c, dsp.c, audio_worker.h in Melodee):
 *   - one part (track_t) with Melodee's voices (voice_t), a control block of 32 frames (CTL; the engine runs
 *     Dexed's 64-sample blocks over two of them), vmod_t as voice.c fills it for an engine with its own envelopes;
 *   - the voices: POLY by the engine's own allocator (Dexed's chooseNote), MONO and LEGATO by Melodee's mono stack
 *     with the engine's mono_key / legato (Dexed's hand-over), the sustain pedal;
 *   - the second core: Melodee's audio worker on the host's c1_run / c1_wait. The engine pairs its voices (one on
 *     core 1 while the next renders on core 0): NV_2 voices while core 1 takes jobs, NV_1 while it does not.
 * The keys, MIDI and edits come in from the main loop and are played at the start of the next render, in order.
 *
 * Voices: Melodee's: INIT VOICE, F1..F8 (Felucca's own, tools/gen_fm6_patches.py) and F9..F24 (Melodee's own,
 * eng_fm6_rom.h); no Yamaha voice. Send DX7 SysEx (a 32-voice bank, VMEM, or single voices, VCED) with `tools/dock.py send FILE.syx`: it is
 * kept in the module's flash banks after Melodee's voices, and the first voice sent is played.
 *
 *   PRESETS   the voice            ALGORITHM the voice's algorithm (1..32)    SELECT  the page
 *   KNOB1..4  the page's controls  MIDI      notes, velocity, CC 1 wheel, 2 breath, 4 foot, 5 portamento time,
 *                                            64 sustain, 65 portamento, 74 brightness, 71 feedback, 73 mod
 *                                            envelope, 72 detune spread, 0 bank (x 128) + program change, bend,
 *                                            channel / key pressure */
#include "hb_module.h"

void *memcpy(void *d, const void *s, unsigned n);   /* (modules/lib/hb_lib.c) */
void *memset(void *d, int c, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);

static const hb_host_t *H;

/* ---- what Melodee's core.h, dsp.c and voice.c give the engine */
#define MELODEE_DUAL_CORE 1
#define MELODEE_FLASH 1                         /* (eng_fm6.c's SysEx scratch: the small one, unused here) */
#define MELODEE_AUDIO_PROFILE 0
#define NPART 1u
#define NTRK 1u
#define NVOICE 16u                              /* Dexed's 16 */
#define NV_2 16u                                /* voices with the second core */
#define NV_1 10u                                /* on core 0 alone */
#define NPOLY 8
#define VOICE_FS 24000                          /* (dsp.c: one voice's level) */
#define RES_OK() 1
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define TABLE_RAM
#include "fm6_tables.h"                         /* (take.py: Melodee's gen_tables.py fm6_tables, FS, CTL) */

enum { V_POLY, V_MONO, V_LEGATO, V_UNISON };   /* P_VOICE */
enum { F_INT, F_PCT, F_BIPCT };
enum { P_VOICE, P_PRIO, P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7, P_COUNT };
enum { G_TUNE };
typedef struct {
    const char *label;
    uint8_t fmt;
    int16_t min, max, def;
    const char *const *names;
    const char *unit;
} param_desc_t;
typedef struct {                                /* (Melodee's voice_t: what the engine reads of it) */
    uint8_t note, gate, active, mvel, vel, stage;
    int32_t env_out;                            /* the last block's amplitude, Q15 */
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
    uint8_t mw, foot, breath, at, porta;        /* CC 1, 4, 2, pressure, CC 65 */
    int16_t bend_raw;                           /* -8192..8191 */
    uint8_t nmono, mono_stack[NVOICE], mono_note;
    uint8_t eng_req, preset;                    /* (eng_fm6.c's fm6_track_loaded, unused here) */
} track_t;
typedef struct {
    const char *name;
    int8_t e[8];
    uint8_t env[4];
    int8_t fenv;
    uint8_t mono;
} preset_t;
typedef struct engine {
    const char *name;
    const char *page_title[2];
    param_desc_t edit[8];
    const preset_t *presets;
    uint32_t npresets;
    void (*note_on)(struct track *t, voice_t *v);
    void (*render)(struct track *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m);
    uint8_t knob[4];
    uint32_t poly;
    void (*block)(struct track *t);
    uint8_t ownenv;
    int (*done)(struct track *t, voice_t *v);
    uint32_t (*alloc)(struct track *t, uint32_t note);
    void (*legato)(struct track *t, voice_t *v);
    void (*mono_key)(struct track *t, uint32_t note);
    void (*post)(struct track *t, int32_t *out, uint32_t n, uint32_t nr);
} engine_t;

static voice_t vo[NVOICE];
static track_t trk[NTRK] = {{.v = vo}};
static struct { int16_t g[1]; } song;           /* (TUNE: 0 cents) */
static uint8_t midi_verb[NTRK];                 /* (Melodee's MIDI release override: none) */
static inline int32_t tuning_log(void) { return 0; }   /* (A4 = 440 Hz) */

static inline __attribute__((always_inline)) int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}
/* dsp.c's amplitude ramp over a block (CTL long) */
static inline __attribute__((always_inline)) int32_t amp_at(const vmod_t *m, uint32_t i)
{
    int32_t x = (m->amp1 - m->amp0) * (int32_t)i;
    return m->amp0 + ((x + ((x >> 31) & (CTL - 1))) >> 5);
}
/* dsp.c's cut of a float to a whole sample with dither d; here without dither (a row of zeros: rounded) */
static const float dith_zero[CTL];
static inline __attribute__((always_inline)) const float *dith_row(uint32_t k) { (void)k; return dith_zero; }
static inline __attribute__((always_inline)) int32_t dith_cutf_d(float x, float d)
{
    return x != 0.0f ? (int32_t)(x + d + 12582912.0f) - 12582912 : 0;
}
/* Melodee's batched renderer (audio_batch.h) and its profiler are not used here */
#define audio_batch_active 0
#define AUDIO_BATCH_BYTES 256u
#define AP_FM6 0u
#define AP_FM6_CONTROL 0u
#define AP_START(name) ((void)0)
#define AP_END(id, name) ((void)0)
static inline void audio_batch_push(void (*k)(const void *, int32_t *), const void *d, uint32_t sz, int32_t *out,
                                    uint32_t n, uint32_t ap)
{
    (void)k; (void)d; (void)sz; (void)out; (void)n; (void)ap;
}
static inline void audio_batch_push_cost(void (*k)(const void *, int32_t *), const void *d, uint32_t sz,
                                         int32_t *out, uint32_t n, uint32_t ap, uint32_t cost)
{
    (void)k; (void)d; (void)sz; (void)out; (void)n; (void)ap; (void)cost;
}

/* ---- Melodee's audio worker (audio_worker.h) on the host's second core (c1_run / c1_wait), from the render only,
 * as in PROPHET. audio_worker_online: core 1 took the last job offered (the engine then pairs its voices and plays
 * NV_2). Until it does (the host is screening it, or it failed), a probe each render asks again; a job it does not
 * finish in time (c1_wait -1: the host holds core 1 from then on) is rendered on core 0, from where core 1 left it. */
typedef void (*audio_worker_fn)(void *);
static int audio_worker_online;
static uint8_t c1_have, c1_dead, aw_out;
static audio_worker_fn aw_fn;
static void *aw_ctx;
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
#include "eng_fm6.c"
#define ENG ENG_FM6

static fm6_part_t fm6_arena;                    /* (engines.c's fm6_part: the part's state in its arena) */
static fm6_part_t *fm6_part(uint32_t part) { (void)part; return &fm6_arena; }
static uint32_t trk_nvoice(const track_t *t) { (void)t; return audio_worker_online ? NV_2 : NV_1; }

/* ---- the voices (the audio interrupt's) */
static uint32_t vage;
static uint8_t key_held[128], key_sus[128], sustain;
static volatile uint32_t voices_live;           /* for the screen */

static void voice_start(voice_t *v, uint32_t note, uint32_t vel)   /* (voice.c's, for an engine with its own envelopes) */
{
    if (!v->active)
        v->env_out = 0;
    v->note = (uint8_t)note;
    v->gate = v->active = 1;
    v->stage = 1;
    v->age = ++vage;
    v->mvel = v->vel = (uint8_t)vel;
    ENG.note_on(&trk[0], v);
}

/* MONO / LEGATO: voice.c's mono stack, the highest key (as Dexed's mono); MONO strikes every note anew */
static void mono_remove(track_t *t, uint32_t note)
{
    uint32_t j, k = 0;
    for (j = 0; j < t->nmono; j++)
        if (t->mono_stack[j] != note)
            t->mono_stack[k++] = t->mono_stack[j];
    t->nmono = (uint8_t)k;
}
static uint32_t mono_pick(const track_t *t)
{
    uint32_t i, n = 0;
    for (i = 0; i < t->nmono; i++)
        if (t->mono_stack[i] > n) n = t->mono_stack[i];
    return n;
}
static void mono_play(track_t *t, uint32_t note, uint32_t vel, int retrig)
{
    voice_t *v = &t->v[0];
    if (retrig || !v->active || !v->gate) {
        voice_start(v, note, vel);
    } else {                                    /* legato: the new pitch, the same envelopes */
        v->note = (uint8_t)note;
        v->mvel = (uint8_t)vel;
        ENG.legato(t, v);
    }
    t->mono_note = (uint8_t)note;
}

static void gates_off(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++) {
        vo[i].gate = 0;
        vo[i].stage = vo[i].active ? 3 : 0;
    }
    trk[0].nmono = trk[0].mono_note = 0;
    memset(key_sus, 0, sizeof key_sus);
}

static void note_release(uint32_t key)
{
    track_t *t = &trk[0];
    uint32_t i;
    mono_remove(t, key);
    if (t->p[P_VOICE] != V_POLY) {
        if (t->mono_note == key) {
            uint32_t next = mono_pick(t);
            if (next) {                         /* back to a held key, legato */
                mono_play(t, next, t->v[0].vel, 0);
            } else {
                t->v[0].gate = 0;
                t->v[0].stage = 3;
                t->mono_note = 0;
            }
        }
        return;
    }
    for (i = 0; i < NVOICE; i++)
        if (vo[i].gate && vo[i].note == key) {
            vo[i].gate = 0;
            vo[i].stage = 3;
        }
}
static void key_on(uint32_t key, uint32_t vel)
{
    track_t *t = &trk[0];
    key_held[key] = 1;
    key_sus[key] = 0;
    if (t->p[P_VOICE] == V_POLY) {
        voice_start(&vo[ENG.alloc(t, key) % trk_nvoice(t)], key, vel);
        return;
    }
    {
        uint32_t sounding = t->mono_note && t->v[0].active && t->v[0].gate, want;
        mono_remove(t, key);
        if (t->nmono >= NVOICE)
            mono_remove(t, t->mono_stack[0]);
        t->mono_stack[t->nmono++] = (uint8_t)key;
        ENG.mono_key(t, key);
        want = mono_pick(t);
        if (sounding && want == t->mono_note)
            return;                             /* a higher key sounds */
        mono_play(t, want, vel, t->p[P_VOICE] == V_MONO || !sounding);
    }
}
static void key_off(uint32_t key)
{
    key_held[key] = 0;
    if (sustain) { key_sus[key] = 1; return; }
    note_release(key);
}

/* ---- from the main loop to the audio interrupt: events, played in order at the next render */
enum { EV_ON, EV_OFF, EV_CC, EV_BEND, EV_AT, EV_EDIT, EV_MACRO, EV_FN, EV_MODE, EV_PATCH, EV_PANIC };
typedef struct { uint8_t type, a, b, c; } ev_t;
#define EVQ 128u
static ev_t evq[EVQ];
static volatile uint32_t ev_w, ev_r;
static uint8_t stage[FP_SIZE + 1u];             /* EV_PATCH's voice (the main loop writes it only while none is pending) */
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

static void ev_play(const ev_t *e)
{
    track_t *t = &trk[0];
    uint32_t k;
    switch (e->type) {
    case EV_ON: key_on(e->a & 127u, e->b ? e->b : 1u); break;
    case EV_OFF: key_off(e->a & 127u); break;
    case EV_CC:
        if (e->a == 1u) t->mw = e->b;
        else if (e->a == 2u) t->breath = e->b;
        else if (e->a == 4u) t->foot = e->b;
        else if (e->a == 65u) t->porta = e->b >= 64u;
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
    case EV_EDIT: {                             /* a byte of the voice: an edit the sounding notes follow */
        uint8_t v[FP_SIZE + 1u];
        memcpy(v, fm6_patch[0], FP_SIZE);
        v[e->a < FP_SIZE ? e->a : 0] = e->b;
        fm6_put_patch(0, v, 0);
        break;
    }
    case EV_MACRO: t->p[P_E0 + (e->a & 7u)] = (int16_t)((int32_t)e->b - 128); break;
    case EV_FN: fm6_fn_set(0, e->a, e->b); break;
    case EV_MODE:
        if (t->p[P_VOICE] != e->a) {
            gates_off();
            t->p[P_VOICE] = e->a;
        }
        break;
    case EV_PATCH:                              /* a new voice: the notes stop, as on a DX7 program change */
        fm6_put_patch(0, stage, 1);
        stage_pending = 0;
        gates_off();
        break;
    case EV_PANIC:
        gates_off();
        t->mw = t->at = t->foot = t->breath = t->porta = 0;
        t->bend_raw = 0;
        sustain = 0;
        break;
    }
}

/* ---- the sound: control blocks of CTL frames, as Melodee's voice.c renders a part */
static int32_t mono_buf[CTL];

static void part_block(int32_t *out, uint32_t n)
{
    track_t *t = &trk[0];
    uint32_t i, nr = 0, live = 0;
    for (i = 0; i < n; i++)
        mono_buf[i] = 0;
    ENG.block(t);
    for (i = 0; i < NVOICE; i++)
        live |= vo[i].active;
    if (!live) {
        ENG.post(t, mono_buf, n, 0);
        return;
    }
    for (i = 0; i < NVOICE; i++) {
        voice_t *v = &vo[i];
        vmod_t m;
        int32_t amp = 32767;
        if (!v->active)
            continue;
        if (ENG.done(t, v) || (i >= NV_1 && !audio_worker_online)) {   /* its envelopes ended (the engine's state
                                                 * goes on), or a voice past NV_1 without core 1: one more block, faded */
            v->active = v->gate = 0;
            v->stage = 0;
            amp = 0;
        }
        m.amp0 = v->env_out;
        m.amp1 = amp;
        v->env_out = amp;
        m.envq15 = 0;
        m.pitch16 = (int32_t)v->note * 16;
        m.inc = 0;
        m.fine = 0;
        m.cutoff = 0;
        m.shape = 64 << 8;
        m.plog = 0;
        ENG.render(t, v, mono_buf, n, &m);
        nr++;
    }
    ENG.post(t, mono_buf, n, nr);
    /* a voice of Melodee's peaks near +-2000..10000 (fm6_output: VOICE_FS), as it is; a soft knee above -2.5 dBFS for
     * chords */
    for (i = 0; i < n; i++) {
        int32_t s = mono_buf[i], a = s < 0 ? -s : s;
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

/* ---- the voices: Melodee's (INIT VOICE, F1..F24), then those the player sent (imported) */
#define NOWN (1u + FM6_NFAC)                    /* INIT VOICE first */
#define IMP_MAX 128u
#define IMP_MAGIC 0x4D493646u                   /* "F6IM": the imported voices in the module's flash banks 1.. */
static struct { uint32_t magic, count; uint8_t pk[IMP_MAX][FM6_PACKED]; } imp;

static uint32_t npatches(void) { return NOWN + imp.count; }
static void patch_get(uint32_t k, uint8_t *v)   /* voice k as the 155-byte layout */
{
    uint8_t pk[FM6_PACKED];
    if (k >= NOWN) {
        fm6_unpack(imp.pk[k - NOWN], v);
        return;
    }
    if (!k) {
        fm6_unpack(FM6_INIT, v);
        return;
    }
    fm6_factory(k - 1u, pk);
    fm6_unpack(pk, v);
}

/* ---- the main loop's side: the voice as edited (ui_v, what the audio interrupt gets as events) */
static uint8_t ui_v[FP_SIZE + 1u];
static int8_t ui_e[7];                          /* Melodee's EDIT macros: ALG FB MLVL MRAT MEG VMOD DTUN */
static uint8_t ui_mode, ui_fn_engine = FM6_MARK1, ui_fn_bend = 3;
static uint32_t cur, page, want_stage, save_at;
static struct { uint32_t magic; uint16_t patch; uint8_t page, mode, engine, bend, pad[2]; } rec;
#define REC_MAGIC 0x31364D46u
static uint8_t bank_cc;

static void stage_patch(void)
{
    if (stage_pending) { want_stage = 1; return; }
    memcpy(stage, ui_v, sizeof stage);
    stage_pending = 1;
    ev_put(EV_PATCH, 0, 0);
}
static void set_mode(uint32_t m)
{
    ui_mode = (uint8_t)m;
    ev_put(EV_MODE, m, 0);
}
static void patch_select(uint32_t k)
{
    if (k >= npatches())
        return;
    cur = k;
    patch_get(k, ui_v);
    stage_patch();
    if (k && k < NOWN)                          /* (Melodee's presets: the bass and lead voices are MONO) */
        set_mode(FM6_PRESETS[k - 1u].mono ? V_LEGATO : V_POLY);
    save_at = H->ms() + 2000u;
}
static void edit(uint32_t i, int32_t v)
{
    v = clamp(v, 0, (int32_t)fm6_max(i));
    if (ui_v[i] == (uint8_t)v)
        return;
    ui_v[i] = (uint8_t)v;
    ev_put(EV_EDIT, i, (uint32_t)v);
}
static void macro(uint32_t k, int32_t v)
{
    static const int8_t LO[7] = {0, -7, -64, -16, -64, -7, 0}, HI[7] = {32, 7, 63, 16, 63, 7, 127};
    v = clamp(v, LO[k], HI[k]);
    if (ui_e[k] == v)
        return;
    ui_e[k] = (int8_t)v;
    ev_put(EV_MACRO, k, (uint32_t)(v + 128));
}
static void fn(uint32_t k, uint32_t v) { ev_put(EV_FN, k, v); }

static void cc_in(uint32_t num, uint32_t val)
{
    switch (num) {
    case 0: bank_cc = (uint8_t)val; break;
    case 5: fn(FN_PTIME, val); break;
    case 74: macro(2, (int32_t)val - 64); break;         /* (fm1-dock's KNOBS CC: KNOB1..4) brightness */
    case 71: edit(FP_FB, (int32_t)val * 8 / 128); break;  /* feedback */
    case 73: macro(4, (int32_t)val - 64); break;         /* the modulators' envelope times */
    case 72: macro(6, (int32_t)val); break;              /* the carriers' detune spread */
    case 1: case 2: case 4: case 64: case 65: case 120: case 123: ev_put(EV_CC, num, val); break;
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

/* ---- the pages: MAIN, each operator's tone and envelope, the LFO, the voice settings */
enum { K_BYTE, K_ALG, K_COARSE, K_DET, K_MACRO, K_BIP, K_WAVE, K_MODE, K_ENGINE, K_BEND, K_TRNSP };
typedef struct { uint8_t id, kind; const char *label; } ctl_t;
typedef struct { const char *title; uint8_t op; ctl_t c[4]; } page_t;   /* op: 1..6 (its bytes), 0 the voice's */
#define OPAGES(n) \
    {"OP" #n, n, {{FP_OL, K_BYTE, "LEVEL"}, {FP_FC, K_COARSE, "RATIO"}, {FP_FF, K_BYTE, "FINE"}, \
                  {FP_DET, K_DET, "DETUNE"}}}, \
    {"OP" #n " ENV", n, {{FP_R1, K_BYTE, "ATTACK RATE"}, {FP_R1 + 1, K_BYTE, "DECAY RATE"}, \
                         {FP_L1 + 2, K_BYTE, "SUSTAIN LVL"}, {FP_R1 + 3, K_BYTE, "RELEASE RATE"}}}
static const page_t PAGES[] = {
    {"MAIN", 0, {{FP_ALG, K_ALG, "ALGORITHM"}, {FP_FB, K_BYTE, "FEEDBACK"}, {2, K_BIP, "BRIGHTNESS"},
                 {4, K_BIP, "MOD ENV"}}},
    OPAGES(1), OPAGES(2), OPAGES(3), OPAGES(4), OPAGES(5), OPAGES(6),
    {"LFO", 0, {{FP_LFS, K_BYTE, "SPEED"}, {FP_LFD, K_BYTE, "DELAY"}, {FP_LPMD, K_BYTE, "PITCH DEPTH"},
                {FP_LFW, K_WAVE, "WAVE"}}},
    {"VOICE", 0, {{0, K_MODE, "VOICES"}, {0, K_ENGINE, "ENGINE"}, {0, K_BEND, "BEND RANGE"},
                  {FP_TRNSP, K_TRNSP, "TRANSPOSE"}}},
};
#define NPAGES NELEM(PAGES)

static uint32_t at_of(const page_t *pg, const ctl_t *x) { return pg->op ? (6u - pg->op) * FP_OP + x->id : x->id; }

static void knob(uint32_t k, int32_t c)
{
    const page_t *pg = &PAGES[page];
    const ctl_t *x = &pg->c[k];
    uint32_t at = at_of(pg, x);
    int32_t step = c > 1 || c < -1 ? 2 * c : c;
    switch (x->kind) {
    case K_BYTE: edit(at, ui_v[at] + (fm6_max(at) > 20u ? step : c)); break;
    case K_ALG: case K_COARSE: case K_DET: case K_WAVE: case K_TRNSP: edit(at, ui_v[at] + c); break;
    case K_BIP: macro(x->id, ui_e[x->id] + step); break;
    case K_MODE: set_mode((uint32_t)clamp(ui_mode + c, V_POLY, V_LEGATO)); break;
    case K_ENGINE:
        ui_fn_engine = (uint8_t)clamp(ui_fn_engine + c, FM6_MODERN, FM6_OPL);
        fn(FN_ENGINE, ui_fn_engine);
        break;
    case K_BEND:
        ui_fn_bend = (uint8_t)clamp(ui_fn_bend + c, 0, 12);
        fn(FN_PBUP, ui_fn_bend);
        fn(FN_PBDN, ui_fn_bend);
        break;
    default: break;
    }
    save_at = H->ms() + 2000u;
}

static void ui(uint32_t ev, int32_t arg)
{
    uint32_t id = HB_EV_ID(ev);
    if (HB_EV_KIND(ev) != HB_EV_ENC)
        return;
    if (id == HB_ENC_PRESETS) {
        int32_t k = (int32_t)cur + arg, n = (int32_t)npatches();
        patch_select((uint32_t)(k < 0 ? 0 : k >= n ? n - 1 : k));
    } else if (id == HB_ENC_ALGORITHM) {
        edit(FP_ALG, ui_v[FP_ALG] + arg);
    } else if (id == HB_ENC_SELECT) {
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
static uint32_t d_cur = ~0u, d_page = ~0u, d_voices = ~0u, d_c1 = ~0u, d_alg = ~0u, d_ms;
static int32_t d_val[4];
static char d_name[12];

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
static void fix2(char *s, int32_t x100)         /* x100 / 100 with two decimals */
{
    char t[8];
    num(s, x100 / 100, 0);
    cat(s, ".");
    t[0] = (char)('0' + x100 / 10 % 10);
    t[1] = (char)('0' + x100 % 10);
    t[2] = 0;
    cat(s, t);
}

static int32_t ctl_value(const page_t *pg, const ctl_t *x)
{
    uint32_t at = at_of(pg, x);
    switch (x->kind) {
    case K_BIP: return ui_e[x->id];
    case K_MODE: return ui_mode;
    case K_ENGINE: return ui_fn_engine;
    case K_BEND: return ui_fn_bend;
    case K_COARSE: return ui_v[at] | ui_v[at + 1u] << 8 | ui_v[at - 1u] << 16;   /* (with FINE and the mode) */
    default: return ui_v[at];
    }
}

static void value_text(const page_t *pg, const ctl_t *x, char *s)
{
    static const char *const WAVE[] = {"TRIANGLE", "SAW DOWN", "SAW UP", "SQUARE", "SINE", "S&H"};
    static const char *const MODE[] = {"POLY", "MONO", "LEGATO"}, *const ENGN[] = {"MODERN", "MARK I", "OPL"};
    uint32_t at = at_of(pg, x);
    int32_t v = ui_v[at];
    s[0] = 0;
    switch (x->kind) {
    case K_BYTE: case K_ALG: num(s, v + (x->kind == K_ALG), 0); break;
    case K_COARSE:
        if (ui_v[at - 1u]) {                    /* FP_MODE: fixed, 10 ^ (COARSE % 4 + FINE / 100) Hz */
            float hz = fm_exp2f(((float)(v & 3) + (float)ui_v[at + 1u] * 0.01f) * 3.32192809f);
            num(s, (int32_t)(hz + 0.5f), 0);
            cat(s, " HZ");
        } else {                                /* coarse x (1 + fine / 100), coarse 0 = 0.5 */
            int32_t c = v ? v * 100 : 50;
            fix2(s, c + c * ui_v[at + 1u] / 100);
        }
        break;
    case K_DET: num(s, v - 7, 1); break;
    case K_BIP: num(s, ui_e[x->id], 1); break;
    case K_WAVE: cat(s, WAVE[v % 6]); break;
    case K_MODE: cat(s, MODE[ui_mode % 3u]); break;
    case K_ENGINE: cat(s, ENGN[ui_fn_engine % 3u]); break;
    case K_BEND: num(s, ui_fn_bend, 0); cat(s, " ST"); break;
    case K_TRNSP: num(s, v - 24, 1); cat(s, " ST"); break;
    default: break;
    }
}

static void draw(uint32_t full)
{
    char s[40], name[12];
    uint32_t k, now = H->ms(), c1 = audio_worker_online ? 2u : c1_dead ? 0u : 1u;
    if (want_stage && !stage_pending) {
        want_stage = 0;
        stage_patch();
    }
    if (save_at && (int32_t)(now - save_at) >= 0 && !voices_live) {   /* (when nothing sounds) */
        save_at = 0;
        rec.magic = REC_MAGIC;
        rec.patch = (uint16_t)cur;
        rec.page = (uint8_t)page;
        rec.mode = ui_mode;
        rec.engine = ui_fn_engine;
        rec.bend = ui_fn_bend;
        H->save(&rec, sizeof rec);
    }
    if (full) {
        H->fill(0, HB_TOP, 240, 240 - HB_TOP, 0);
        d_cur = d_page = d_voices = d_c1 = d_alg = ~0u;
        d_name[0] = 1;
    }
    fm6_name(name, ui_v);
    if (cur != d_cur || memcmp(name, d_name, sizeof d_name)) {
        char t[8];
        d_cur = cur;
        memcpy(d_name, name, sizeof d_name);
        s[0] = 0;                               /* "FM6 3 OF 24" (Melodee's), "BANK 12 OF 32" (sent) */
        cat(s, !cur ? "FM6  INIT" : cur < NOWN ? "FM6 " : "BANK ");
        if (cur) {
            num(t, (int32_t)(cur < NOWN ? cur : cur - NOWN + 1u), 0);
            cat(s, t);
            cat(s, " OF ");
            num(t, (int32_t)(cur < NOWN ? NOWN - 1u : imp.count), 0);
            cat(s, t);
        }
        H->fill(0, 28, 240, 16, 0);
        H->text(0, 28, 240, 0, s, C_GREY, 1);
        H->fill(0, 46, 240, 34, 0);
        H->text(0, 46, 240, 1, name[0] ? name : "-", C_AMBER, 1);
        d_page = ~0u;                           /* (the values may all have changed) */
    }
    if (page != d_page || (PAGES[page].op && ui_v[FP_ALG] != d_alg)) {
        const page_t *pg = &PAGES[page];
        d_page = page;
        d_alg = ui_v[FP_ALG];
        s[0] = 0;
        cat(s, "< ");
        cat(s, pg->title);
        if (pg->op)                             /* the operator's place in this algorithm */
            cat(s, (fm6_carriers(ui_v[FP_ALG]) >> (6u - pg->op)) & 1u ? " CARRIER" : " MOD");
        cat(s, " >");
        H->fill(0, 86, 240, 18, 0);
        H->text(0, 86, 240, 0, s, C_PAGE, 1);
        for (k = 0; k < 4u; k++)
            d_val[k] = -1000;
    }
    for (k = 0; k < 4u; k++) {
        const page_t *pg = &PAGES[page];
        const ctl_t *x = &pg->c[k];
        int32_t v = ctl_value(pg, x), y = 110 + 26 * (int32_t)k;
        if (v == d_val[k])
            continue;
        d_val[k] = v;
        H->fill(0, y, 240, 24, 0);
        s[0] = 'K'; s[1] = (char)('1' + k); s[2] = ' '; s[3] = 0;
        cat(s, x->label);
        H->text(8, y, 150, 0, s, 0xFFFFu, 0);
        value_text(pg, x, s);
        H->text(150, y, 82, 0, s, C_AMBER, 0);
        if (x->kind == K_BYTE || x->kind == K_BIP) {   /* a bar under it */
            uint32_t at = at_of(pg, x);
            int32_t w = x->kind == K_BIP ? (ui_e[x->id] + 64) * 224 / 127 : ui_v[at] * 224 / (int32_t)fm6_max(at);
            H->fill(8, y + 19, w, 2, C_AMBER);
            H->fill(8 + w, y + 19, 224 - w, 2, C_DIM);
        }
    }
    if ((voices_live != d_voices || c1 != d_c1) && (full || now - d_ms >= 100u)) {
        d_ms = now;
        d_voices = voices_live;
        d_c1 = c1;
        s[0] = 0;
        cat(s, "VOICES ");
        num(s + 7, (int32_t)voices_live, 0);
        cat(s, c1 == 2u ? " OF 16, 2 CORES" : " OF 10, 1 CORE");
        H->fill(0, 218, 240, 16, 0);
        H->text(0, 218, 240, 0, s, C_GREY, 1);
    }
}

/* ---- DX7 SysEx from the computer (tools/dock.py send FILE.syx): 32-voice banks (VMEM) and single voices (VCED),
 * into the module's flash banks 1.. (kept, after Melodee's voices). A file with a bank replaces what was sent
 * before; single voices alone are added to it. The first voice of the file is played. */
static uint8_t fbuf[4u * 4104u + 4u * 163u];
static uint32_t fsize;
static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int imp_save(void)
{
    const uint8_t *b = (const uint8_t *)&imp;
    uint32_t n = 8u + imp.count * FM6_PACKED, idx = 1;
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
        if (got >= 8u && imp.count <= IMP_MAX && got >= 8u + imp.count * FM6_PACKED)
            break;
    }
    if (imp.magic != IMP_MAGIC || imp.count > IMP_MAX || got < 8u + imp.count * FM6_PACKED)
        imp.count = 0;
}
static uint8_t sx_ok(const uint8_t *p, uint32_t n)      /* the DX7 checksum: data + it = 0 mod 128 */
{
    uint32_t s = 0, i;
    for (i = 0; i < n; i++) s += p[i];
    return (uint8_t)(-s & 0x7Fu);
}
static int data(const uint8_t *p, uint32_t n)
{
    uint32_t i, j, k, first = imp.count, bank = 0, add = 0;
    if (!n)
        return 1;
    if (p[0] == HB_FILE_BEGIN) {
        if (n < 9u || rd32(p + 1) > sizeof fbuf) return 3;   /* (four banks, or four of them and voices) */
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
    for (i = 0; i + 6u < fsize; i++)            /* a bank anywhere in it: it replaces the voices sent before */
        if (fbuf[i] == 0xF0u && fbuf[i + 1u] == 0x43u && !(fbuf[i + 2u] & 0xF0u) && fbuf[i + 3u] == 9u)
            bank = 1;
    if (bank)
        imp.count = first = 0;
    for (i = 0; i < fsize && imp.count < IMP_MAX; i++) {   /* each F0 43 .. F7 frame that is a voice or a bank */
        const uint8_t *b = fbuf + i;
        if (b[0] != 0xF0u)
            continue;
        for (j = i + 1u; j < fsize && fbuf[j] != 0xF7u && fbuf[j] != 0xF0u; j++)
            ;
        if (j < fsize && fbuf[j] == 0xF7u && b[1] == 0x43u && !(b[2] & 0xF0u)) {
            uint32_t len = j - i + 1u;
            if (len == 163u && b[3] == 0u && b[4] == 1u && b[5] == 0x1Bu && sx_ok(b + 6, 155) == b[161]) {
                uint8_t v[FP_SIZE + 1u];        /* VCED: one voice, as the engine keeps it */
                memcpy(v, b + 6, FP_SIZE);
                fm6_sanitize(v);
                memset(imp.pk[imp.count], 0, FM6_PACKED);
                fm6_pack(v, imp.pk[imp.count++]);
                add++;
            } else if (len == 4104u && b[3] == 9u && b[4] == 0x20u && b[5] == 0u && sx_ok(b + 6, 4096) == b[4102]) {
                for (k = 0; k < 32u && imp.count < IMP_MAX; k++) {   /* VMEM: 32 packed voices */
                    uint8_t v[FP_SIZE + 1u];
                    fm6_unpack(b + 6 + k * FM6_PACKED, v);   /* (unpacked and packed again: in range, unused bits 0) */
                    memset(imp.pk[imp.count], 0, FM6_PACKED);
                    fm6_pack(v, imp.pk[imp.count++]);
                    add++;
                }
            }
        }
        i = j - 1u;
    }
    if (!add)
        return 2;                               /* no DX7 voice in it */
    imp.magic = IMP_MAGIC;
    H->log_u32("fm6: voices in", add);
    if (imp_save())
        H->log("fm6: the voices are not kept (no flash bank free)");
    patch_select(NOWN + first);
    return 0;
}

static int init(const hb_host_t *h)
{
    H = h;
    c1_have = HB_HOST_HAS(H, c1_wait);
    dsp_tables_init();                          /* (the sine, MARK I and OPL tables in RAM) */
    fm6_init();
    imp_load();
    if (!H->load(&rec, sizeof rec) && rec.magic == REC_MAGIC) {
        cur = rec.patch < npatches() ? rec.patch : 0;
        page = rec.page < NPAGES ? rec.page : 0;
        ui_mode = rec.mode <= V_LEGATO ? rec.mode : V_POLY;
        ui_fn_engine = rec.engine <= FM6_OPL ? rec.engine : FM6_MARK1;
        ui_fn_bend = rec.bend <= 12u ? rec.bend : 3u;
    } else {
        cur = 1;                                /* (F1, TINE EP) */
        ui_mode = FM6_PRESETS[0].mono ? V_LEGATO : V_POLY;
    }
    patch_get(cur, ui_v);
    fm6_put_patch(0, ui_v, 1);                  /* (the audio interrupt does not run the module yet) */
    fm6_fn_set(0, FN_ENGINE, ui_fn_engine);
    fm6_fn_set(0, FN_PBUP, ui_fn_bend);
    fm6_fn_set(0, FN_PBDN, ui_fn_bend);
    trk[0].p[P_VOICE] = ui_mode;
    trk[0].p[P_PRIO] = 2;
    H->log_u32("fm6: Melodee 165d177, voices", npatches());
    return 0;
}

static void release(void)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        vo[i].gate = vo[i].active = 0;
    trk[0].nmono = trk[0].mono_note = 0;
    ev_r = ev_w;
    stage_pending = 0;
}

HB_DESC = {HB_ABI, "FM6", init, release, note, cc, render, ui, draw, 0, 0, HB_F_DATA, midi, data};
