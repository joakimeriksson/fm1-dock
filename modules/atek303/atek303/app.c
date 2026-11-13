/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Javier (Animatek)
 * Built on fm1-base's template (MPL-2.0, Charles Vestal).
 *
 * ATEK303 on the FM-1: the ATEK303 voice and the ATEK303 SEQ's acid pattern generator.
 *
 *   PLAY            the sequencer runs / stops (internal clock, or MIDI clock when it comes in)
 *   SEQ             GENERATE: a new pattern (a new seed). With LOCK on: mutate all three layers. Running:
 *                   at the next bar (16 steps; NEXT until then)
 *   ENV / LFO / ARP mutate TIME / NOTES (octaves) / SLIDES and ACCENTS, on the page of 16 steps on view
 *   EDIT            the step editor (a piano roll with UP, DOWN, GATE, ACC and SLIDE rows). EDIT goes round the
 *                   sequence's three: the editor -> the SEQ page -> the PATT page -> the editor.
 *                     SELECT moves the cursor over the whole pattern (the page on view follows it);
 *                     a key sets the step's note and moves on; K1 its note a semitone, K2 its octave (UP / DOWN),
 *                     K3 ACC / SLIDE (none, ACC, SLIDE, both), K4 its gate (REST, NOTE, TIE); OCT- held + K4 the
 *                     pattern's STEPS; ENV toggles its ACCENT, LFO its SLIDE; REC undoes; HOME leaves.
 *                     A note outside the scale turns the scale CHROMATIC (nothing is lost)
 *   ALGO            the WAVE (SAW, the extra layer's combinations, SQUARE), from any page
 *   HOME            the four sound pages in turn: VCF, VOICE, PATT, SEQ (again: the next one); from the
 *                   editor, the preset list or an effect page, straight to VCF
 *   REC             undo the last mutation or edit
 *   GLO             OPTIONS (colour, MIDI, fine tuning); any button but OCT- closes them (OCT- + HOME opens them too)
 *   OCT- + SEQ      LOCK: GENERATE mutates instead of making a new seed (SEQ lit while on)
 *   keys            stopped: play the voice (mono; legato slides; a hard press is an accent).
 *                   Running: transpose the pattern (the key is the new root, as on a 303)
 *   OCT- / OCT+     the octave
 *   SELECT          the page; K1..K4 its four knobs:
 *                     1 VOICE   CUTOFF, RESO, ENV MOD, DECAY
 *                     2 VOICE   ACCENT, TUNE, WAVE (SAW, the extra layer's combinations, SQUARE), GATE (the
 *                               sequencer's gate length)
 *                     3 PATTERN DENSITY, RANGE, ACCENTS, SLIDES (they regrow the pattern from its seed)
 *                     4 PATTERN STEPS, SCALE, TEMPO, ROOT
 *                     5 DELAY   TIME (synced: 1/16 .. 1/4.), FEEDBACK, TONE, MIX (ping-pong)
 *                     6 CHORUS  TYPE (CHORUS, FLANGER), RATE, DEPTH, MIX
 *                     7 DRIVE   TYPE (OFF, OVERDRIVE, RAT, FUZZ), DRIVE, TONE, LEVEL
 *                     (the pages' order; the sound goes drive -> chorus -> delay -> reverb)
 *                     8 REVERB  DECAY, DAMP, PRE-DELAY, MIX (Dattorro's plate)
 *   FX              jumps to the effect pages (again: the next one)
 *   SAVE            the SAVE screen: a PRESET is everything (pattern, voice, effects, tempo, scale, root).
 *                   It opens on the preset loaded (the list's cursor, or the first empty slot); PRESETS picks
 *                   the slot (of 32), SELECT a new random acid name; SAVE saves, and over a slot in use asks
 *                   OVERWRITE? (NO / YES, NO first; any encoder turns it); any other button cancels
 *   PRESETS         the encoder opens the PRESET LIST (the 32 slots) and moves through it; a saved preset
 *                   loads as the cursor reaches it (running: at the end of the pattern, as on a 303; NEXT
 *                   until then). A fresh FM-1 comes with 8 factory presets in slots 1..8.
 *                   In the list (SEL lit): SEL loads the one selected at once (running too); the white keys are
 *                   the presets of the cursor's half (1..16 or 17..32: the one loaded lit, the saved ones dim) and load
 *                   at once; ENV twice deletes the one selected, SAVE opens the SAVE screen on it, HOME closes it (it closes by itself after 6 s untouched)
 *   MIDI in         stopped: notes play the voice; running: notes transpose. Clock, start, stop and
 *                   continue drive the sequencer. CC 74 cutoff, 71 reso, 75 env mod, 76 decay,
 *                   77 accent, 78 tune, 79 wave
 *
 * Threads: the voice and the sequencer live in the audio interrupt. The main loop posts the keys,
 * the knobs and the buttons to a queue the render drains at the start of each block; MIDI in is
 * read there too. The screen reads the render's state as it is (a step drawn one frame late is
 * harmless). Part of fm1-base's unity build, after the platform: gfx.c and announce.c are in scope. */
#include "fastmath.h"
#include "atek_voice.h"
#include "acid_pattern.h"
#include "fx.h"

/* the Animatek palette, as on the VCV Rack modules (UZZ-VCV-RACK src/ui/CommonWidgets.hpp, AcidEditor.hpp):
 * the screens are slate, the values in the displays' blue, the selection in the logo's blue */
#undef C_BLACK
#define C_BLACK RGB(0x16, 0x18, 0x1C)          /* the panels' slate */
#define AK_AMBER RGB(0xFD, 0x9A, 0x00)          /* the Animatek amber: what waits (NEXT), OVERWRITE */
/* the colour (OPTIONS > COLOR): Animatek's blues, or one of fm1-base's themes (X0X's: GREEN, AMBER, CYAN, RED, MONO)
 * with the acid editor's colours drawn from its hue */
typedef struct {
    const char *name;
    uint16_t line, dim, gray, sec, hi;         /* fm1-base's five steps (gfx.c pal[]) */
    uint16_t logo, tie, accent, slide;         /* the logo's colour (bars, cursor, notes), and the editor's */
} theme_t;
#define FM1_NEUTRALS RGB(34, 34, 40), RGB(74, 74, 82), RGB(150, 150, 158)
static const theme_t THEMES[] = {
    {"ANIMATEK", RGB(0x2C, 0x2C, 0x2C), RGB(0x5A, 0x5A, 0x5A), RGB(0x9A, 0xA2, 0xB5), RGB(0x7F, 0x9C, 0xC4),
     RGB(0x5D, 0xB7, 0xFF), RGB(0x2C, 0x7F, 0xFF), RGB(0x1E, 0x4E, 0x9C), RGB(0xCF, 0xE6, 0xFF), RGB(0x00, 0xC2, 0xFF)},
    {"GREEN", FM1_NEUTRALS, RGB(60, 154, 90), RGB(150, 255, 175), RGB(80, 220, 120), RGB(42, 110, 64),
     RGB(224, 255, 232), RGB(150, 255, 175)},
    {"AMBER", FM1_NEUTRALS, RGB(176, 112, 48), RGB(255, 184, 70), RGB(232, 136, 30), RGB(122, 74, 20),
     RGB(255, 240, 208), RGB(255, 184, 70)},
    {"CYAN", FM1_NEUTRALS, RGB(94, 156, 184), RGB(150, 225, 255), RGB(60, 180, 235), RGB(30, 94, 124),
     RGB(224, 246, 255), RGB(150, 225, 255)},
    {"RED", FM1_NEUTRALS, RGB(176, 90, 80), RGB(255, 132, 112), RGB(232, 76, 60), RGB(122, 42, 32),
     RGB(255, 224, 218), RGB(255, 132, 112)},
    {"MONO", FM1_NEUTRALS, RGB(150, 150, 160), RGB(236, 236, 240), RGB(196, 196, 202), RGB(90, 90, 96),
     RGB(255, 255, 255), RGB(236, 236, 240)},
};
#define NTHEMES (int)(sizeof THEMES / sizeof THEMES[0])
static uint16_t c_logo, c_tie, c_accent, c_slide, c_octave;
#define C_LOGO c_logo                          /* bars, the page selected, the cursor */
#define AK_NOTE c_logo                         /* the acid editor's colours */
#define AK_TIE c_tie
#define AK_ACCENT c_accent
#define AK_SLIDE c_slide
#define AK_OCTAVE c_octave
static void theme_apply(int i)                 /* over fm1-base's palette (gfx.c pal[]: C_LINE, C_DIM, C_GRAY, C_AMB, C_HI) */
{
    const theme_t *t = &THEMES[i >= 0 && i < NTHEMES ? i : 0];
    pal[0] = t->line;                          /* rules, empty bars */
    pal[1] = t->dim;                           /* inactive */
    pal[2] = t->gray;                          /* labels */
    pal[3] = t->sec;                           /* secondary */
    pal[4] = t->hi;                            /* values */
    c_logo = t->logo;
    c_tie = t->tie;
    c_accent = t->accent;
    c_slide = t->slide;
    c_octave = t->sec;
}

#define SET_MAGIC 0x41543634u                  /* "AT64" (64 steps): another magic loads the defaults */
#define ACCENT_VEL 100u
#define SR 44100.0f
#define CHUNK 32u                              /* the sequencer's resolution: 0.7 ms */

/* the sequencer's knobs, beside the voice's (0..1 like them, or a count) */
enum { SQ_DENSITY, SQ_RANGE, SQ_ACCENT, SQ_SLIDE, SQ_GATE, SQ_NKNOBS };

typedef struct {
    uint32_t magic;
    float param[AT_NPARAMS];
    float seqk[SQ_NKNOBS];
    int8_t octave;                             /* -2 .. +2 */
    uint8_t page;
    uint8_t steps;                             /* 1..64 */
    uint8_t scale;
    uint8_t root;                              /* 0..11 */
    uint8_t locked;
    uint16_t tempo;                            /* BPM */
    uint32_t mutation_counter;
    acid_pattern_t pattern;
    float fx[FX_NPARAMS];                      /* the effects: knobs 0..1, types and TIME as indices */
} settings_t;
static settings_t set;
static uint8_t set_dirty;
static volatile uint8_t backup_hold;           /* a backup streams over USB: SAVE waits */

/* ------------------------------------------------------------------ presets --- */
/* 32 presets, 8 to a store object (4 objects; with 64 steps a preset is about 470 bytes, of the 480 an eighth
 * of an object leaves: checked below). A preset is the
 * whole settings record and a name; the page it was saved on is not restored. */
#define NPRESETS 32
#define PRESETS_PER_OBJ 8
typedef struct {
    char name[15];
    uint8_t used;
    settings_t s;
} preset_t;
static preset_t presets[NPRESETS];
typedef char preset_fits[sizeof(preset_t) * PRESETS_PER_OBJ <= PLAT_STORE_MAX ? 1 : -1];
static int8_t cur_preset = -1;                 /* the one loaded (or saved) last, -1 none */

/* the OPTIONS menu (OCT- + HOME): the unit's own choices, apart from the settings so no preset carries them (OBJ_OPTIONS,
 * saved as the menu closes). Each an index into its choices */
#define OPT_MAGIC 0x4F505431u                  /* "OPT1" */
enum { OP_COLOR, OP_MIDI_IN, OP_MIDI_OUT, OP_SEQ_OUT, OP_CLOCK_OUT, OP_FINE, OP_N = OP_FINE + AT_NOPTS };
typedef struct {
    uint32_t magic;
    uint8_t v[OP_N];
} options_t;
static options_t opt;
static void options_defaults(void)
{
    int i;
    opt.magic = OPT_MAGIC;
    opt.v[OP_COLOR] = 0;                       /* ANIMATEK */
    opt.v[OP_MIDI_IN] = 0;                     /* every channel */
    opt.v[OP_MIDI_OUT] = 0;                    /* channel 1 */
    opt.v[OP_SEQ_OUT] = 1;                     /* the sequencer's notes out */
    opt.v[OP_CLOCK_OUT] = 0;
    for (i = 0; i < AT_NOPTS; i++)
        opt.v[OP_FINE + i] = AT_OPT_DEFAULT[i];
}
static int opt_count(int i)                    /* how many choices item i has */
{
    static const uint8_t N[OP_FINE] = {0, 17, 16, 2, 2};
    return i == OP_COLOR ? NTHEMES : i < OP_FINE ? N[i] : AT_OPT_N[i - OP_FINE];
}
static void options_load(void)
{
    int i, ok = plat_store_load(OBJ_OPTIONS, &opt, sizeof opt) == (int)sizeof opt && opt.magic == OPT_MAGIC;
    for (i = 0; ok && i < OP_N; i++)
        ok = opt.v[i] < opt_count(i);
    if (!ok)
        options_defaults();
}
static uint32_t midi_out_ch(void) { return opt.v[OP_MIDI_OUT] & 15u; }
static void midi_note_out(int note, uint32_t vel)   /* a note to USB on the OPTIONS channel (vel 0: off); main loop or render */
{
    uint32_t ch = midi_out_ch();
    if (vel)
        plat_midi_send(0x09u | (0x90u | ch) << 8 | (uint32_t)note << 16 | vel << 24, TO_USB);
    else
        plat_midi_send(0x08u | (0x80u | ch) << 8 | (uint32_t)note << 16, TO_USB);
}
static void midi_rt_out(uint32_t st) { plat_midi_send(0x0Fu | st << 8, TO_USB); }   /* clock, start, stop */

/* random acid names: two words, at most 14 characters */
static const char *const NAME_A[] = {"ACID", "RUBBER", "TOXIC", "HYPER", "SQUELCH", "LASER", "MELTED", "ELECTRIC",
                                     "BUBBLE", "WOBBLY", "SILVER", "CHROME", "PLASTIC", "LIQUID", "NEON", "RAW",
                                     "TWISTED", "FUZZY", "BENT", "WARPED", "SNAKE", "JACKIN", "DIRTY", "COSMIC"};
static const char *const NAME_B[] = {"WORM", "LINE", "BASS", "JACK", "RAVE", "PULSE", "MELT", "DROP", "TRIP", "BLOB",
                                     "SNAKE", "SQUID", "LOOP", "TRAX", "BOX", "WAVE", "ZAP", "FROG", "GOO", "BUZZ",
                                     "SLIDE", "SMILE", "FIZZ", "CRAWL"};
static uint32_t name_rng = 0x303A303Au;
static void acid_name(char *out)
{
    uint32_t a, b, i, n = 0;
    const char *p;
    name_rng ^= name_rng << 13;
    name_rng ^= name_rng >> 17;
    name_rng ^= name_rng << 5;
    a = (name_rng >> 4) % (sizeof NAME_A / sizeof NAME_A[0]);
    b = (name_rng >> 12) % (sizeof NAME_B / sizeof NAME_B[0]);
    if (NAME_A[a][0] == NAME_B[b][0] && NAME_A[a][1] == NAME_B[b][1])
        b = (b + 1u) % (sizeof NAME_B / sizeof NAME_B[0]);   /* no "SNAKE SNAKE" */
    for (p = NAME_A[a]; *p && n < 14u; p++)
        out[n++] = *p;
    if (n < 13u)
        out[n++] = ' ';
    for (p = NAME_B[b]; *p && n < 14u; p++)
        out[n++] = *p;
    for (i = n; i < 15u; i++)
        out[i] = 0;
}

static at_voice_t voice;

/* ---------------------------------------------- main loop -> render queue --- */
enum { EV_NOTE_ON, EV_NOTE_OFF, EV_PARAM, EV_PANIC, EV_PLAY, EV_GENERATE, EV_MUTATE, EV_UNDO, EV_REGROW,
       EV_ROOT, EV_FX, EV_EDIT, EV_LOAD, EV_APPLY, EV_OPTION };
/* the editor's operations (EV_EDIT: a = step, b = op | value << 8) */
enum { ED_NOTE, ED_GATE, ED_ACC, ED_SLIDE, ED_OCT, ED_SEMI, ED_ARTIC };
#define QN 64u
static uint32_t q[QN];
static volatile uint32_t q_w, q_r;
static void post(uint32_t kind, uint32_t a, uint32_t b)
{
    uint32_t w = q_w;
    if (w - q_r >= QN)
        return;
    q[w % QN] = kind | (a & 0xFFu) << 8 | (b & 0xFFFFu) << 16;
    q_w = w + 1u;
}

/* ------------------------------------------------- the held notes (render) --- */
#define NHELD 16
static int8_t held[NHELD];
static uint8_t nheld;
static volatile int32_t ui_note = -1;
static volatile uint8_t ui_accent, ui_slide;

static void voice_follow(int accent)
{
    if (nheld) {
        int note = held[nheld - 1];
        ui_slide = (uint8_t)voice.gate;
        at_gate(&voice, 1, note, accent, 0);
        ui_note = note;
        ui_accent = (uint8_t)accent;
    } else {
        at_gate(&voice, 0, voice.held_note < 0 ? 36 : voice.held_note, 0, 0);
        ui_note = -1;
        ui_accent = ui_slide = 0;
    }
}
static void held_on(int note, uint32_t vel)
{
    uint32_t i, j;
    for (i = j = 0; i < nheld; i++)
        if (held[i] != note)
            held[j++] = held[i];
    nheld = (uint8_t)j;
    if (nheld == NHELD) {
        for (i = 1; i < NHELD; i++)
            held[i - 1] = held[i];
        nheld--;
    }
    held[nheld++] = (int8_t)note;
    voice_follow(vel >= ACCENT_VEL);
}
static void held_off(int note)
{
    uint32_t i, j, top = nheld && held[nheld - 1] == note;
    for (i = j = 0; i < nheld; i++)
        if (held[i] != note)
            held[j++] = held[i];
    nheld = (uint8_t)j;
    if (top || !nheld)
        voice_follow(0);
}

/* ----------------------------------------------------- the sequencer (render) --- */
/* Atek303Seq::process, step by step: the clock, then gate, tie and slide from the grid. */
static struct {
    acid_grid_t grid;
    acid_pattern_t undo;
    uint32_t undo_counter;
    uint8_t has_undo;
    volatile uint8_t running;
    volatile uint8_t step;
    uint8_t started;                           /* the first step is under way */
    uint8_t ext;                               /* following MIDI clock */
    uint8_t ticks;                             /* MIDI clock ticks into this step (6 a step) */
    uint32_t ext_last;                         /* render samples at the last step from MIDI clock */
    uint32_t now;                              /* samples since boot (render) */
    float step_time, period;                   /* seconds */
    uint8_t was_gate;
    uint8_t out_note;                          /* the sequencer's note sounding on MIDI out, + 1 (0: none) */
    uint8_t out_ticks;                         /* MIDI clock out: the ticks sent into this step (6 a step) */
} sq;
static volatile uint32_t midi_count;

static acid_params_t gen_params(void)
{
    acid_params_t p;
    acid_params_default(&p);
    p.steps = ACID_MAX_STEPS;
    p.scale = set.scale;
    p.density = fm_maxf(0.05f, set.seqk[SQ_DENSITY]);
    p.accent = set.seqk[SQ_ACCENT];
    p.slide = set.seqk[SQ_SLIDE];
    p.range = set.seqk[SQ_RANGE];
    p.tie = 0.4f;
    return p;
}
static volatile uint32_t grid_version;
#ifdef PLAT_HOST
static acid_grid_t sq_before;                  /* the tests: the grid before the last change */
#endif
static void seq_render_grid(void)
{
#ifdef PLAT_HOST
    sq_before = sq.grid;
#endif
    acid_render(&set.pattern, &sq.grid);
    grid_version++;
}
static uint32_t seed_entropy;
static void seq_generate(int new_seed)
{
    acid_params_t p = gen_params();
    uint32_t seed = set.pattern.seed;
    if (new_seed) {
        seed_entropy = seed_entropy * 1664525u + 1013904223u + sq.now;
        seed = seed_entropy ? seed_entropy : 1u;
    }
    acid_generate(&set.pattern, &p, seed);
    set.mutation_counter = 0;
    sq.has_undo = 0;
    seq_render_grid();
}
static int seq_mutate(int layer, int lo)          /* on the page of 16 from lo, as the SEQ module's pages */
{
    acid_pattern_t before = set.pattern;
    uint32_t last = set.mutation_counter;
    if (acid_mutate_burst_range(&set.pattern, layer, set.mutation_counter + 1u, layer == ACID_MUT_ARTICULATION ? 3 : 2,
                                set.scale, &last, lo, lo + 16)) {
        sq.undo = before;
        sq.undo_counter = set.mutation_counter;
        set.mutation_counter = last;
        sq.has_undo = 1;
        seq_render_grid();
        return 1;
    }
    return 0;
}
static int seq_mutate_all(void)
{
    acid_pattern_t before = set.pattern;
    uint32_t before_counter = set.mutation_counter, cursor = before_counter;
    int layer;
    for (layer = ACID_MUT_TIME; layer <= ACID_MUT_ARTICULATION; layer++) {
        uint32_t last = cursor;
        if (!acid_mutate_burst(&set.pattern, layer, cursor + 1u, layer == ACID_MUT_ARTICULATION ? 3 : 2, set.scale,
                               &last)) {
            set.pattern = before;
            return 0;
        }
        cursor = last;
    }
    sq.undo = before;
    sq.undo_counter = before_counter;
    set.mutation_counter = cursor;
    sq.has_undo = 1;
    seq_render_grid();
    return 1;
}
static int seq_undo(void)
{
    if (!sq.has_undo)
        return 0;
    set.pattern = sq.undo;
    set.mutation_counter = sq.undo_counter;
    sq.has_undo = 0;
    seq_render_grid();
    return 1;
}
/* a preset chosen while the sequencer runs waits for the end of the pattern, as on a 303 (render) */
static volatile int8_t pend_preset = -1;
static volatile uint8_t pend_gen;              /* SEQ while running: the new pattern waits for the next bar */
static volatile uint8_t ui_event, ui_event_ok; /* the outcome of a button, for the screen (the render sets it) */
static void seq_generate_now(void)
{
    ui_event_ok = (uint8_t)(set.locked ? seq_mutate_all() : (seq_generate(1), 1));
    ui_event = (uint8_t)(EV_GENERATE + 1);
}
static volatile int8_t loaded_preset = -1;     /* the last one the render loaded, and a count the screen follows */
static volatile uint8_t loaded_n;
static void preset_apply(int a)                /* a preset, whole, but the page */
{
    uint8_t page = set.page;
    int k;
    if (a < 0 || a >= NPRESETS || !presets[a].used)
        return;
    set = presets[a].s;
    set.page = page;
    for (k = 0; k < AT_NPARAMS; k++)
        at_set(&voice, k, set.param[k]);
    for (k = 0; k < FX_NPARAMS; k++)
        fx_set(k, set.fx[k]);
    acid_sanitize_ex(&set.pattern, set.scale, 1);
    sq.has_undo = 0;
    seq_render_grid();
    loaded_preset = (int8_t)a;
    loaded_n++;
}
static void seq_play(int on)
{
    if (on && !sq.running) {
        nheld = 0;
        sq.step = 0;
        sq.step_time = 0.0f;
        sq.started = 1;
        sq.ticks = 0;
        sq.out_ticks = 0;
        if (opt.v[OP_CLOCK_OUT] && !sq.ext)
            midi_rt_out(0xFAu);                /* start: the clock follows from the first step */
    }
    if (!on && sq.running && opt.v[OP_CLOCK_OUT] && !sq.ext)
        midi_rt_out(0xFCu);
    sq.running = (uint8_t)on;
    if (!on) {
        if (sq.out_note) {                     /* the note out ends */
            midi_note_out(sq.out_note - 1, 0);
            sq.out_note = 0;
        }
        if (pend_gen) {                        /* stopped: what waited, now */
            pend_gen = 0;
            seq_generate_now();
        }
        if (pend_preset >= 0) {                /* stopped: the one waiting loads now */
            int a = pend_preset;
            pend_preset = -1;
            preset_apply(a);
        }
        sq.started = 0;
        at_gate(&voice, 0, voice.held_note < 0 ? 36 : voice.held_note, 0, 0);
        ui_note = -1;
        ui_accent = ui_slide = 0;
        sq.was_gate = 0;
    }
}
static int seq_len(void) { return set.steps < 1 ? 1 : set.steps > ACID_MAX_STEPS ? ACID_MAX_STEPS : set.steps; }
static void seq_next_step(void)
{
    int len = seq_len();
    sq.step = (uint8_t)((sq.step + 1) % len);
    sq.step_time = 0.0f;
    sq.out_ticks = 0;
    if (sq.step % 16 == 0 && pend_gen) {       /* a bar ended: the new pattern from here */
        pend_gen = 0;
        seq_generate_now();
    }
    if (sq.step == 0 && pend_preset >= 0) {    /* the pattern ended: the one waiting starts on its first step */
        int a = pend_preset;
        pend_preset = -1;
        preset_apply(a);
    }
}
/* the gate, tie and slide of the step under way (Atek303Seq::process), into the voice */
static void seq_apply(void)
{
    int len = seq_len(), step = sq.step >= len ? 0 : sq.step, next = (step + 1) % len, si = set.scale;
    const acid_grid_t *g = &sq.grid;
    int active = sq.started && g->gate[step], next_active = sq.started && g->gate[next];
    int tie_in = active && g->tie[step], tie_out = active && next_active && g->tie[next];
    int step_semi = acid_semi_of(g->deg[step], g->oct[step], si);
    /* a slide into another note glides; into the same note it is legato (held, no new attack), as on a 303;
     * into a rest or a tie it does nothing. (Generated patterns never slide to the same note.) */
    int slide = active && g->slide[step] && next_active && !tie_in && !g->tie[next];
    float period = sq.period;
    float gap = fm_maxf(0.0015f, 0.03f * period);
    float normal_hold = fm_minf(set.seqk[SQ_GATE] * period, period - gap);
    float tied_hold = period - gap;
    int gate = active && (tie_out || sq.step_time < (tie_in ? tied_hold : normal_hold));
    int note = 36 + 12 * (set.octave + 1) + set.root + step_semi;
    int accent = active && g->accent[step];
    if (note < 0)
        note = 0;
    if (note > 127)
        note = 127;
    at_gate(&voice, gate, note, accent, slide);
    if ((uint32_t)(gate ? note + 1 : 0) != sq.out_note) {   /* MIDI out: a new note before the last one's end (legato) */
        uint8_t last = sq.out_note;
        sq.out_note = 0;
        if (gate && opt.v[OP_SEQ_OUT]) {
            midi_note_out(note, accent ? 127u : 80u);
            sq.out_note = (uint8_t)(note + 1);
        }
        if (last)
            midi_note_out(last - 1, 0);
    }
    if (gate) {
        ui_note = note;
        ui_accent = (uint8_t)accent;
        ui_slide = (uint8_t)slide;
    } else if (sq.was_gate)
        ui_note = -1;
    sq.was_gate = (uint8_t)gate;
}
static void seq_chunk(uint32_t n)              /* the internal clock, n samples on */
{
    float dt = (float)n / SR;
    if (!sq.running)
        return;
    if (!sq.ext) {
        sq.period = 15.0f / (float)(set.tempo < 30u ? 30u : set.tempo);   /* a sixteenth */
        if (sq.step_time >= sq.period)
            seq_next_step();
    }
    seq_apply();
    if (!sq.ext && opt.v[OP_CLOCK_OUT])         /* MIDI clock out, 24 a quarter: tick k of the step at k/6 of it */
        while (sq.out_ticks < 6u && sq.step_time >= (float)sq.out_ticks * sq.period * (1.0f / 6.0f)) {
            midi_rt_out(0xF8u);
            sq.out_ticks++;
        }
    sq.step_time += dt;
}
static void seq_clock(uint32_t st)             /* render: MIDI realtime messages */
{
    if (st == 0xFAu) {                         /* start */
        sq.ext = 1;
        sq.running = 0;
        seq_play(1);
        sq.ticks = 255;                        /* the clock after a start is the first step's own: the next
                                                * step comes on the 7th, as a DAW counts (it wraps to 0 here) */
        sq.ext_last = sq.now;
    } else if (st == 0xFBu) {                  /* continue */
        sq.ext = 1;
        sq.running = 1;
        sq.started = 1;
    } else if (st == 0xFCu) {                  /* stop */
        seq_play(0);
    } else if (st == 0xF8u) {
        sq.ext = 1;
        if (!sq.running)
            return;
        if (++sq.ticks >= 6u) {
            uint32_t span = sq.now - sq.ext_last;
            sq.ticks = 0;
            if (span > 0u)
                sq.period = fm_clampf((float)span / SR, 0.002f, 4.0f);
            sq.ext_last = sq.now;
            seq_next_step();
        }
    }
}

/* the editor (render): change one step of the rendered grid and bring it back into the pattern */
static int degree_of(int semi, int *d, int *o)      /* semitones from the root -> degree, octave; 0: not in the scale */
{
    const acid_scale_t *sc = &ACID_SCALES[set.scale];
    int pc = ((semi % 12) + 12) % 12, i;
    for (i = 0; i < sc->n; i++)
        if (sc->s[i] == pc) {
            *d = i;
            *o = (semi - pc) / 12;
            return 1;
        }
    return 0;
}
static void grid_to_chromatic(acid_grid_t *g)       /* every step's degree as a semitone: CROM keeps every pitch */
{
    int s2;
    for (s2 = 0; s2 < ACID_MAX_STEPS; s2++) {
        int semi = acid_semi_of(g->deg[s2], g->oct[s2], set.scale), pc = ((semi % 12) + 12) % 12;
        g->deg[s2] = (int8_t)pc;
        g->oct[s2] = (int8_t)((semi - pc) / 12);
    }
    set.scale = ACID_SCALES_LEN - 1;
}
static void seq_edit(int step, int op, int val)
{
    acid_grid_t g = sq.grid;
    int d = 0, o = 0, semi;
    if (step < 0 || step >= ACID_MAX_STEPS)
        return;
    sq.undo = set.pattern;
    sq.undo_counter = set.mutation_counter;
    sq.has_undo = 1;
    if (op == ED_NOTE || op == ED_SEMI) {
        semi = op == ED_NOTE ? val - 64 : acid_semi_of(g.deg[step], g.oct[step], set.scale) + (val ? 1 : -1);
        if (semi < -24)
            semi = -24;
        if (semi > 35)
            semi = 35;
        if (!degree_of(semi, &d, &o)) {
            grid_to_chromatic(&g);
            degree_of(semi, &d, &o);
        }
        g.deg[step] = (int8_t)d;
        g.oct[step] = (int8_t)(o < -2 ? -2 : o > 2 ? 2 : o);
        if (op == ED_NOTE) {
            g.gate[step] = 1;
            g.tie[step] = 0;
        }
    } else if (op == ED_GATE) {                    /* REST -> NOTE -> TIE -> REST (a tie needs a note before) */
        int state = !g.gate[step] ? 0 : g.tie[step] ? 2 : 1, prev = step > 0 && g.gate[step - 1];
        state = (state + (val ? 1 : 2)) % 3;
        if (state == 2 && !prev)
            state = val ? 0 : 1;
        g.gate[step] = (uint8_t)(state != 0);
        g.tie[step] = (uint8_t)(state == 2);
        if (state == 2) {
            g.deg[step] = g.deg[step - 1];
            g.oct[step] = g.oct[step - 1];
        }
    } else if (op == ED_ACC) {
        if (g.gate[step] && !g.tie[step])
            g.accent[step] = (uint8_t)!g.accent[step];
    } else if (op == ED_SLIDE) {
        if (g.gate[step] && !g.tie[step])
            g.slide[step] = (uint8_t)!g.slide[step];
    } else if (op == ED_ARTIC) {                   /* K3: none -> ACC -> SLIDE -> ACC + SLIDE (no wrap) */
        int a = (g.accent[step] ? 1 : 0) | (g.slide[step] ? 2 : 0);
        if (!g.gate[step] || g.tie[step])
            return;
        a += val ? 1 : -1;
        if (a < 0 || a > 3)
            return;
        g.accent[step] = (uint8_t)(a & 1);
        g.slide[step] = (uint8_t)(a >> 1);
    } else if (op == ED_OCT) {
        o = g.oct[step] + (val ? 1 : -1);
        g.oct[step] = (int8_t)(o < -2 ? -2 : o > 2 ? 2 : o);
    }
    acid_import(&set.pattern, &g, set.pattern.time_length, set.pattern.seed, 3);
    acid_sanitize_ex(&set.pattern, set.scale, 1);   /* the player's slides stay, valid or not */
    seq_render_grid();
}

static void midi_in(void)                      /* render: the packets that came in */
{
    static const uint8_t CC_PARAM[7][2] = {{74, AT_CUTOFF}, {71, AT_RESONANCE}, {75, AT_ENVMOD}, {76, AT_DECAY},
                                           {77, AT_ACCENT}, {78, AT_TUNING}, {79, AT_WAVE}};
    uint32_t pkt, i;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24;
        if (st >= 0xF8u) {
            if (st != 0xFEu)
                seq_clock(st);
            continue;
        }
        if (opt.v[OP_MIDI_IN] && st < 0xF0u && (st & 15u) != opt.v[OP_MIDI_IN] - 1u)
            continue;                          /* OPTIONS: another channel */
        midi_count++;
        if ((st & 0xF0u) == 0x90u && d2) {
            if (sq.running)
                set.root = (uint8_t)(d1 % 12u);
            else
                held_on((int)d1, d2);
        } else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) {
            if (!sq.running)
                held_off((int)d1);
        } else if ((st & 0xF0u) == 0xB0u) {
            if (d1 == 123u && !sq.running) {
                nheld = 0;
                voice_follow(0);
            }
            for (i = 0; i < 7u; i++)
                if (d1 == CC_PARAM[i][0]) {
                    at_set(&voice, CC_PARAM[i][1], (float)d2 / 127.0f);
                    set.param[CC_PARAM[i][1]] = (float)d2 / 127.0f;
                }
        }
    }
}


static float mono[APP_BLOCK], fxl[APP_BLOCK], fxr[APP_BLOCK];

/* the output stage, before MASTER: +6 dB, a limiter (both sides together: 1 ms attack, 80 ms release, to 0.8) and a
 * soft clip from 0.8 for what the attack lets through, so the peaks are rounded to just under full scale instead of
 * cut. A 303 is one voice with resonant peaks, 12-18 dB over its average: without it the level is low and the loud
 * presets clip hard */
#define LIM_MAKEUP 2.0f
#define LIM_THR 0.8f
#define LIM_ATT 0.0224f                        /* 1 - exp(-1 / (1 ms at 44.1 kHz)) */
#define LIM_REL 0.99972f                       /* exp(-1 / (80 ms at 44.1 kHz)) */
static float lim_env;
static float soft_clip(float x)                /* linear to LIM_THR, then a tanh knee up to 1.0 */
{
    float a = fm_fabsf(x);
    if (a <= LIM_THR)
        return x;
    a = LIM_THR + (1.0f - LIM_THR) * fm_tanhf((a - LIM_THR) * (1.0f / (1.0f - LIM_THR)));
    return x < 0.0f ? -a : a;
}
static void limit(float *l, float *r)
{
    float p = fm_maxf(fm_fabsf(*l *= LIM_MAKEUP), fm_fabsf(*r *= LIM_MAKEUP)), g;
    if (p > lim_env)
        lim_env += (p - lim_env) * LIM_ATT;
    else
        lim_env = p + (lim_env - p) * LIM_REL;
    g = lim_env > LIM_THR ? LIM_THR / lim_env : 1.0f;
    *l = soft_clip(*l * g);
    *r = soft_clip(*r * g);
}

/* The render in three steps, so that a build with a second core (fm1-dock's module) can run the effects of one block
 * there while this core makes the voice of the next: app_render_events (what the main loop posted, MIDI in: all that
 * changes the voice's, the sequencer's and the effects' settings), app_render_fx (the effects, the limiter and the
 * gain: a mono block into the output; it touches nothing the other two do) and app_render_voice (the sequencer and
 * the voice: a mono block). app_render, the platform's, is the three in turn. */
static void app_render_events(void)
{
    while (q_r != q_w) {
        uint32_t e = q[q_r % QN], kind = e & 0xFFu, a = (e >> 8) & 0xFFu, b = e >> 16;
        if (kind == EV_NOTE_ON)
            held_on((int)a, b);
        else if (kind == EV_NOTE_OFF)
            held_off((int)a);
        else if (kind == EV_PARAM)
            at_set(&voice, (int)a, (float)b / 1000.0f);
        else if (kind == EV_PANIC) {
            nheld = 0;
            if (!sq.running) {
                at_panic(&voice);
                ui_note = -1;
            }
        } else if (kind == EV_PLAY) {
            sq.ext = 0;
            seq_play(!sq.running);
        } else if (kind == EV_GENERATE) {
            if (sq.running && sq.started)
                pend_gen = 1;                  /* on at the next bar (16 steps) */
            else
                seq_generate_now();
        } else if (kind == EV_MUTATE) {
            ui_event_ok = (uint8_t)seq_mutate((int)a, (int)b);
            ui_event = (uint8_t)(EV_MUTATE + 1);
        } else if (kind == EV_UNDO) {
            ui_event_ok = (uint8_t)seq_undo();
            ui_event = (uint8_t)(EV_UNDO + 1);
        } else if (kind == EV_REGROW)
            seq_generate(0);
        else if (kind == EV_FX)
            fx_set((int)a, (float)b / 1000.0f);
        else if (kind == EV_EDIT)
            seq_edit((int)a, (int)(b & 0xFFu), (int)(b >> 8));
        else if (kind == EV_APPLY) {           /* a restore: the settings as they are now, into the sound */
            int k;
            for (k = 0; k < AT_NPARAMS; k++)
                at_set(&voice, k, set.param[k]);
            for (k = 0; k < FX_NPARAMS; k++)
                fx_set(k, set.fx[k]);
            acid_sanitize(&set.pattern, set.scale);
            sq.has_undo = 0;
            seq_render_grid();
        } else if (kind == EV_OPTION)            /* OPTIONS: a fine-tuning choice into the voice */
            at_option(&voice, (int)a, (int)b);
        else if (kind == EV_LOAD && a < NPRESETS && presets[a].used) {
            if (b)                             /* SEL or a key in the list: at once, running or not */
                pend_preset = -1;
            if (sq.running && sq.started && !b)
                pend_preset = (int8_t)a;       /* on at the end of the pattern */
            else
                preset_apply((int)a);
        }
        q_r++;
    }
    midi_in();
    fx_tempo(sq.running && sq.ext ? sq.period : 15.0f / (float)(set.tempo < 30u ? 30u : set.tempo));
}
static void app_render_voice(float *out, uint32_t n)
{
    uint32_t off;
    for (off = 0; off < n; off += CHUNK) {     /* the sequencer at 0.7 ms, the voice between */
        uint32_t c = n - off < CHUNK ? n - off : CHUNK;
        seq_chunk(c);
        at_render(&voice, out + off, c);
        sq.now += c;
    }
}
static void app_render_fx(const float *in, int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i;
    float gain = (float)master_q12 / 4096.0f * 8300000.0f;
    static float gain_now;
    fx_process(in, fxl, fxr, n);
    for (i = 0; i < n; i++) {
        gain_now += (gain - gain_now) * 0.004f;
        limit(&fxl[i], &fxr[i]);
        lr[2 * i] = (int32_t)fm_clampf(fxl[i] * gain_now, -8388000.0f, 8388000.0f);
        lr[2 * i + 1] = (int32_t)fm_clampf(fxr[i] * gain_now, -8388000.0f, 8388000.0f);
    }
}
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    if (n > APP_BLOCK)
        n = APP_BLOCK;
    app_render_events();
    app_render_voice(mono, n);
    app_render_fx(mono, lr, n, master_q12);
}

void app_panic(void)
{
    nheld = 0;
    sq.running = 0;
    sq.started = 0;
    at_panic(&voice);
    ui_note = -1;
}

/* --------------------------------------------------------------- settings --- */
static void settings_defaults(void)
{
    int i;
    static at_voice_t d;
    acid_params_t p;
    at_init(&d, SR);
    set.magic = SET_MAGIC;
    for (i = 0; i < AT_NPARAMS; i++)
        set.param[i] = d.param[i];
    set.param[AT_CUTOFF] = 0.08f;              /* Javier's start (2026-11-07): 369 Hz, reso 80 %, env mod 4 %, */
    set.param[AT_RESONANCE] = 0.80f;           /* decay 707 ms; density 100 %, range 80 %, accents 51 %, slides 44 % */
    set.param[AT_ENVMOD] = 0.34f;
    set.param[AT_DECAY] = 0.50f;
    acid_params_default(&p);
    set.seqk[SQ_DENSITY] = 1.0f;
    set.seqk[SQ_RANGE] = 0.80f;
    set.seqk[SQ_ACCENT] = 0.51f;
    set.seqk[SQ_SLIDE] = 0.44f;
    set.seqk[SQ_GATE] = 0.85f;
    set.octave = -1;
    set.page = 0;
    set.steps = 16;
    set.scale = 0;
    set.root = 0;
    set.locked = 0;
    set.tempo = 125;
    set.mutation_counter = 0;
    p.scale = 0;
    p.density = set.seqk[SQ_DENSITY];          /* the first pattern grows from those knobs */
    p.range = set.seqk[SQ_RANGE];
    p.accent = set.seqk[SQ_ACCENT];
    p.slide = set.seqk[SQ_SLIDE];
    acid_generate(&set.pattern, &p, 0x41544B33u);
    set.fx[FX_DRIVE_TYPE] = 0.0f;              /* the effects: every one off (its mix at 0) */
    set.fx[FX_DRIVE] = 0.5f;
    set.fx[FX_TONE] = 0.6f;
    set.fx[FX_LEVEL] = 0.6f;
    set.fx[FX_MOD_TYPE] = 0.0f;
    set.fx[FX_MOD_RATE] = 0.3f;
    set.fx[FX_MOD_DEPTH] = 0.5f;
    set.fx[FX_MOD_MIX] = 0.0f;
    set.fx[FX_DLY_TIME] = 3.0f;                /* 1/8 dotted, the acid classic */
    set.fx[FX_DLY_FEEDBACK] = 0.45f;
    set.fx[FX_DLY_TONE] = 0.5f;
    set.fx[FX_DLY_MIX] = 0.0f;
    set.fx[FX_REV_DECAY] = 0.6f;
    set.fx[FX_REV_DAMP] = 0.4f;
    set.fx[FX_REV_PRE] = 0.2f;
    set.fx[FX_REV_MIX] = 0.0f;   /* "ATK3": the same first pattern on every FM-1 */
}

#if __has_include("factory_presets.h") && !(defined(PLAT_HOST) && !defined(PLAT_WEB))
#include "factory_presets.h"                   /* FACTORY_BANKS: Javier's presets, from a backup (the FM-1 and the web
                                                * emulator; the host simulator keeps the table below: its tests count on it) */
#endif
/* the factory presets: what an empty bank starts with (slots 1..8). The pattern grows from the seed with the
 * preset's own pattern knobs, so it is the same on every FM-1; delete one and it stays deleted */
typedef struct {
    const char *name;
    float v[AT_NPARAMS];                       /* TUNE, CUTOFF, RESO, ENV MOD, DECAY, ACCENT, WAVE (0 saw, 1 square) */
    float sk[SQ_NKNOBS];                       /* DENSITY, RANGE, ACCENTS, SLIDES, GATE */
    uint8_t steps, scale, root;
    uint16_t tempo;
    uint32_t seed;
    float fx[FX_NPARAMS];                      /* drive (type, drive, tone, level), chorus (type, rate, depth, mix),
                                                  delay (time, feedback, tone, mix), reverb (decay, damp, pre, mix) */
} factory_t;
static const factory_t FACTORY[] = {
    {"ACID CLASSIC", {.5f, .30f, .78f, .62f, .38f, .70f, 0}, {.65f, .45f, .60f, .50f, .85f}, 16, 0, 0, 125, 0x303A0001u,
     {0, .5f, .6f, .6f, 0, .3f, .5f, 0, 3, .40f, .5f, .18f, .6f, .4f, .2f, .08f}},
    {"SQUARE SQUELCH", {.5f, .24f, .86f, .70f, .30f, .75f, 1}, {.70f, .50f, .65f, .55f, .80f}, 16, 0, 2, 130, 0x303A0002u,
     {1, .45f, .55f, .6f, 0, .3f, .5f, 0, 2, .35f, .5f, .12f, .6f, .4f, .2f, 0}},
    {"RUBBER BASS", {.5f, .18f, .45f, .40f, .55f, .55f, 0}, {.80f, .25f, .40f, .35f, .90f}, 16, 1, 9, 120, 0x303A0003u,
     {0, .5f, .6f, .6f, 0, .3f, .5f, 0, 3, .45f, .5f, 0, .6f, .4f, .2f, .05f}},
    {"PHRYGIAN RAT", {.5f, .28f, .82f, .66f, .34f, .72f, 0}, {.65f, .50f, .60f, .45f, .80f}, 16, 2, 4, 135, 0x303A0004u,
     {2, .55f, .5f, .55f, 0, .3f, .5f, 0, 3, .30f, .45f, .10f, .6f, .4f, .2f, 0}},
    {"DUB ACID", {.5f, .32f, .72f, .55f, .50f, .60f, 0}, {.45f, .40f, .50f, .40f, .75f}, 16, 4, 7, 118, 0x303A0005u,
     {0, .5f, .6f, .6f, 0, .25f, .5f, .15f, 5, .62f, .35f, .32f, .75f, .5f, .3f, .18f}},
    {"HYPER ACID", {.5f, .35f, .90f, .80f, .28f, .85f, 0}, {.90f, .60f, .70f, .65f, .70f}, 16, 0, 0, 145, 0x303A0006u,
     {3, .40f, .6f, .5f, 0, .3f, .5f, 0, 0, .30f, .6f, .10f, .6f, .4f, .2f, 0}},
    {"FLANGE WORM", {.5f, .30f, .80f, .60f, .40f, .65f, 1}, {.60f, .45f, .55f, .60f, .85f}, 16, 5, 5, 128, 0x303A0007u,
     {0, .5f, .6f, .6f, 1, .20f, .6f, .45f, 3, .35f, .5f, .10f, .6f, .4f, .2f, 0}},
    {"SPACE MELT", {.5f, .26f, .70f, .50f, .70f, .50f, 1}, {.40f, .55f, .40f, .60f, .90f}, 16, 3, 11, 100, 0x303A0008u,
     {0, .5f, .6f, .6f, 0, .25f, .6f, .35f, 4, .55f, .4f, .30f, .85f, .35f, .4f, .35f}},
};
#define NFACTORY ((int)(sizeof FACTORY / sizeof FACTORY[0]))
static void factory_presets(void)              /* into slots 0.. of an empty bank A, through the settings record */
{
    settings_t keep = set;
    int i, k;
    for (i = 0; i < NFACTORY; i++) {
        const factory_t *f = &FACTORY[i];
        acid_params_t p;
        settings_defaults();
        for (k = 0; k < AT_NPARAMS; k++)
            set.param[k] = f->v[k];
        for (k = 0; k < SQ_NKNOBS; k++)
            set.seqk[k] = f->sk[k];
        for (k = 0; k < FX_NPARAMS; k++)
            set.fx[k] = f->fx[k];
        set.steps = f->steps;
        set.scale = f->scale;
        set.root = f->root;
        set.tempo = f->tempo;
        p = gen_params();
        acid_generate(&set.pattern, &p, f->seed);
        for (k = 0; k < 15 && f->name[k]; k++)
            presets[i].name[k] = f->name[k];
        for (; k < 15; k++)
            presets[i].name[k] = 0;
        presets[i].s = set;
        presets[i].used = 1;
    }
    set = keep;
}

static void bytes_copy(void *d, const void *s, uint32_t n)
{
    uint8_t *a = (uint8_t *)d;
    const uint8_t *b = (const uint8_t *)s;
    while (n--)
        *a++ = *b++;
}
static int settings_ok(const settings_t *t)
{
    int i, ok = t->magic == SET_MAGIC && t->octave >= -2 && t->octave <= 2 && t->page < 8u && t->steps >= 1u
                && t->steps <= ACID_MAX_STEPS && t->scale < ACID_SCALES_LEN && t->root < 12u && t->tempo >= 30u
                && t->tempo <= 300u;
    for (i = 0; ok && i < AT_NPARAMS; i++)
        ok = t->param[i] >= 0.0f && t->param[i] <= 1.0f;
    for (i = 0; ok && i < SQ_NKNOBS; i++)
        ok = t->seqk[i] >= 0.0f && t->seqk[i] <= 1.0f;
    for (i = 0; ok && i < FX_NPARAMS; i++)
        ok = t->fx[i] >= 0.0f && t->fx[i] <= 6.0f;
    return ok;
}
/* the settings and the presets from the store. What is missing (a fresh FM-1) comes from the factory: Javier's own
 * (factory_presets.h, made from a backup by tools/factory_from_backup.py) or, without it, the table above */
static void store_load(void)
{
    int i, b;
    int ok = plat_store_load(OBJ_SETTINGS, &set, sizeof set) == (int)sizeof set && settings_ok(&set);
    for (b = 0; b < NPRESETS / PRESETS_PER_OBJ; b++) {
        preset_t *bank = &presets[b * PRESETS_PER_OBJ];
        if (plat_store_load((uint32_t)(OBJ_PRESETS_A + b), bank, sizeof(preset_t) * PRESETS_PER_OBJ)
            != (int)(sizeof(preset_t) * PRESETS_PER_OBJ)) {
            int k;
            for (k = 0; k < PRESETS_PER_OBJ; k++)
                bank[k].used = 0;
#ifdef FACTORY_BANKS
            if (FACTORY_BANK_LEN[b] == sizeof(preset_t) * PRESETS_PER_OBJ)
                bytes_copy(bank, FACTORY_BANK[b], sizeof(preset_t) * PRESETS_PER_OBJ);
#else
            if (b == 0)
                factory_presets();
#endif
        }
    }
    for (i = 0; i < NPRESETS; i++)              /* a preset from another layout is not loaded */
        if (presets[i].used && (presets[i].used != 1u || !settings_ok(&presets[i].s)))
            presets[i].used = 0;
    if (!ok) {
        settings_defaults();
#ifdef FACTORY_BANKS
        if (presets[0].used) {                 /* a fresh FM-1 starts on preset 01 (its name in the header) */
            set = presets[0].s;
            set.page = 0;
            cur_preset = 0;
        } else if (sizeof FACTORY_SETTINGS == sizeof set) {   /* else the state Javier's FM-1 was in */
            settings_t t;
            bytes_copy(&t, FACTORY_SETTINGS, sizeof t);
            if (settings_ok(&t))
                set = t;
        }
#endif
    }
}

void app_init(void)
{
    int i;
    at_init(&voice, SR);
    options_load();
    for (i = 0; i < AT_NOPTS; i++)
        at_option(&voice, i, opt.v[OP_FINE + i]);
    store_load();
    acid_sanitize(&set.pattern, set.scale);
    for (i = 0; i < AT_NPARAMS; i++)
        at_set(&voice, i, set.param[i]);
    fx_init(SR);
    for (i = 0; i < FX_NPARAMS; i++)
        fx_set(i, set.fx[i]);
    seq_render_grid();
    sq.period = 15.0f / (float)set.tempo;
    name_rng ^= set.pattern.seed;
    seed_entropy = set.pattern.seed;
}

/* --------------------------------------------------------------------- UI --- */
/* a knob of a page: a voice parameter (0..), or a sequencer control */
enum { K_NONE = -1, K_SQ = 100, K_STEPS = 200, K_SCALE, K_TEMPO, K_ROOT, K_FX = 300 };
#define NPAGES 8
static const char *const PAGE_TITLE[NPAGES] = {"VOICE", "VOICE", "PATTERN", "PATTERN", "DELAY", "CHORUS", "DRIVE", "REVERB"};
static const char *const PAGE_NAME[NPAGES][4] = {{"CUTOFF", "RESO", "ENV MOD", "DECAY"},
                                            {"ACCENT", "TUNE", "WAVE", "GATE"},
                                            {"DENSITY", "RANGE", "ACCENTS", "SLIDES"},
                                            {"STEPS", "SCALE", "TEMPO", "ROOT"},
                                            {"TIME", "FEEDBACK", "TONE", "MIX"},
                                            {"TYPE", "RATE", "DEPTH", "MIX"},
                                            {"TYPE", "DRIVE", "TONE", "LEVEL"},
                                            {"DECAY", "DAMP", "PRE-DELAY", "MIX"}};
static const int PAGE_KNOB[NPAGES][4] = {{AT_CUTOFF, AT_RESONANCE, AT_ENVMOD, AT_DECAY},
                                    {AT_ACCENT, AT_TUNING, AT_WAVE, K_SQ + SQ_GATE},
                                    {K_SQ + SQ_DENSITY, K_SQ + SQ_RANGE, K_SQ + SQ_ACCENT, K_SQ + SQ_SLIDE},
                                    {K_STEPS, K_SCALE, K_TEMPO, K_ROOT},
                                    {K_FX + FX_DLY_TIME, K_FX + FX_DLY_FEEDBACK, K_FX + FX_DLY_TONE, K_FX + FX_DLY_MIX},
                                    {K_FX + FX_MOD_TYPE, K_FX + FX_MOD_RATE, K_FX + FX_MOD_DEPTH, K_FX + FX_MOD_MIX},
                                    {K_FX + FX_DRIVE_TYPE, K_FX + FX_DRIVE, K_FX + FX_TONE, K_FX + FX_LEVEL},
                                    {K_FX + FX_REV_DECAY, K_FX + FX_REV_DAMP, K_FX + FX_REV_PRE, K_FX + FX_REV_MIX}};

static struct {
    char msg[2][24];
    uint32_t msg_until;
    uint32_t keys, btn;
    uint32_t regrow_at;                        /* a pattern knob turned: regrow when it rests */
    uint8_t edit, cursor;                      /* the step editor, and the step it points at */
    uint8_t save;                              /* the SAVE screen: 0 none, 1 slot and name, 2 OVERWRITE? */
    int8_t save_slot;                          /* where it goes */
    uint8_t save_yes;                          /* the confirmation's choice (NO by default) */
    uint8_t save_named;                        /* SELECT gave it a new name: the slot's name no longer follows */
    char save_name[15];
    uint8_t list;                              /* the preset list is up */
    int8_t list_cur;                           /* its cursor: a slot, 0..31 */
    uint32_t list_until;                       /* it closes by itself then */
    uint32_t del_t;                            /* ENV pressed once in the list: a second press deletes */
    uint8_t redraw;
    uint8_t loaded_seen;
    uint8_t oct_hold, oct_used;                /* OCT- held, and used in a combination meanwhile (+ HOME, + K4) */
    uint8_t opts, opt_cur, opt_dirty;          /* the OPTIONS menu: up, its item, changed (saved as it closes) */
} ui;

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}
static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}
static void pct_text(float x, char *b)
{
    int_text((int32_t)(100.0f * x + 0.5f), b);
    text_cat(b, " %");
}

static float knob_value(int k)                 /* 0..1, for the bar */
{
    if (k >= 0 && k < AT_NPARAMS)
        return set.param[k];
    if (k >= K_SQ && k < K_SQ + SQ_NKNOBS)
        return set.seqk[k - K_SQ];
    if (k == K_STEPS)
        return (float)set.steps / (float)ACID_MAX_STEPS;
    if (k == K_SCALE)
        return (float)set.scale / (float)(ACID_SCALES_LEN - 1);
    if (k == K_TEMPO)
        return (float)(set.tempo - 30) / 270.0f;
    if (k == K_ROOT)
        return (float)set.root / 11.0f;
    if (k == K_FX + FX_DRIVE_TYPE)
        return set.fx[FX_DRIVE_TYPE] / (float)(FX_DRIVE_TYPES - 1);
    if (k == K_FX + FX_MOD_TYPE)
        return set.fx[FX_MOD_TYPE];
    if (k == K_FX + FX_DLY_TIME)
        return set.fx[FX_DLY_TIME] / (float)(FX_DLY_TIMES - 1);
    if (k >= K_FX && k < K_FX + FX_NPARAMS)
        return set.fx[k - K_FX];
    return 0.0f;
}
static void knob_text(int k, char *b)
{
    char t[12];
    float x = knob_value(k);
    b[0] = 0;
    if (k == AT_CUTOFF) {
        int_text((int32_t)(314.0f * fm_powf(2394.0f / 314.0f, x) + 0.5f), t);
        text_cat(b, t);
        text_cat(b, " HZ");
    } else if (k == AT_DECAY) {
        int_text((int32_t)(200.0f * fm_powf(12.5f, x) + 0.5f), t);
        text_cat(b, t);
        text_cat(b, " MS");
    } else if (k == AT_TUNING) {
        int_text((int32_t)(400.0f + 80.0f * x + 0.5f), t);
        text_cat(b, t);
        text_cat(b, " HZ");
    } else if (k == AT_WAVE)                   /* the oscillator list: SAW, the combinations, SQUARE */
        text_cat(b, AT_OSC_NAME[at_osc_index(x)]);
    else if (k == AT_ENVMOD)
        pct_text(x * x * x, b);
    else if (k == K_STEPS)
        int_text(set.steps, b);
    else if (k == K_SCALE)
        text_cat(b, ACID_SCALES[set.scale].short_name);
    else if (k == K_TEMPO) {
        int_text(set.tempo, t);
        text_cat(b, t);
        text_cat(b, sq.ext ? " EXT" : " BPM");
    } else if (k == K_ROOT)
        text_cat(b, ACID_NOTE_NAMES[set.root]);
    else if (k == K_FX + FX_DRIVE_TYPE)
        text_cat(b, FX_DRIVE_NAME[(int)set.fx[FX_DRIVE_TYPE]]);
    else if (k == K_FX + FX_MOD_TYPE)
        text_cat(b, FX_MOD_NAME[(int)set.fx[FX_MOD_TYPE]]);
    else if (k == K_FX + FX_DLY_TIME)
        text_cat(b, FX_DLY_NAME[(int)set.fx[FX_DLY_TIME]]);
    else if (k == K_FX + FX_MOD_RATE) {        /* tenths of a hertz: "0.3 HZ", "6.4 HZ" */
        int32_t d = (int32_t)(0.5f * fm_powf(2.0f, 7.0f * x) + 0.5f);
        int_text(d / 10, t);
        text_cat(b, t);
        text_cat(b, ".");
        int_text(d % 10, t);
        text_cat(b, t);
        text_cat(b, " HZ");
    } else if (k == K_FX + FX_REV_PRE) {
        int_text((int32_t)(50.0f * x + 0.5f), t);
        text_cat(b, t);
        text_cat(b, " MS");
    } else
        pct_text(x, b);
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
    ui.redraw = 1;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void knob_turn(int k, int32_t det)
{
    int32_t v;
    float step = det > 1 || det < -1 ? 0.02f : 0.01f;
    if (k >= 0 && k < AT_NPARAMS) {
        float x = set.param[k];
        if (k == AT_WAVE) {                    /* one entry of the list a detent */
            int i = at_osc_index(x) + (det > 0 ? 1 : -1);
            if (i < 0 || i >= AT_NOSC)
                return;
            x = (float)i / (float)(AT_NOSC - 1);
        } else
            x = fm_clampf(x + (float)det * step, 0.0f, 1.0f);
        if (x == set.param[k])
            return;
        set.param[k] = x;
        post(EV_PARAM, (uint32_t)k, (uint32_t)(x * 1000.0f + 0.5f));
    } else if (k >= K_SQ && k < K_SQ + SQ_NKNOBS) {
        float *x = &set.seqk[k - K_SQ];
        float y = fm_clampf(*x + (float)det * step, k == K_SQ + SQ_GATE ? 0.05f : 0.0f, 1.0f);
        if (y == *x)
            return;
        *x = y;
        if (k != K_SQ + SQ_GATE)
            ui.regrow_at = plat_ms() + 120u;   /* regrow from the same seed once the knob rests */
    } else if (k == K_STEPS) {
        v = set.steps + det;
        set.steps = (uint8_t)(v < 1 ? 1 : v > ACID_MAX_STEPS ? ACID_MAX_STEPS : v);
    } else if (k == K_SCALE) {
        v = set.scale + (det > 0 ? 1 : -1);
        set.scale = (uint8_t)(v < 0 ? 0 : v >= ACID_SCALES_LEN ? ACID_SCALES_LEN - 1 : v);
    } else if (k == K_TEMPO) {
        v = set.tempo + det;
        set.tempo = (uint16_t)(v < 30 ? 30 : v > 300 ? 300 : v);
    } else if (k == K_ROOT) {
        v = (set.root + det % 12 + 12) % 12;
        set.root = (uint8_t)v;
    } else if (k == K_FX + FX_DRIVE_TYPE || k == K_FX + FX_MOD_TYPE || k == K_FX + FX_DLY_TIME) {
        int p = k - K_FX;
        int top = p == FX_DRIVE_TYPE ? FX_DRIVE_TYPES - 1 : p == FX_MOD_TYPE ? FX_MOD_TYPES - 1 : FX_DLY_TIMES - 1;
        v = (int32_t)set.fx[p] + (det > 0 ? 1 : -1);
        v = v < 0 ? 0 : v > top ? top : v;
        if ((float)v == set.fx[p])
            return;
        set.fx[p] = (float)v;
        post(EV_FX, (uint32_t)p, (uint32_t)(v * 1000));
    } else if (k >= K_FX && k < K_FX + FX_NPARAMS) {
        int p = k - K_FX;
        float y = fm_clampf(set.fx[p] + (float)det * step, 0.0f, 1.0f);
        if (y == set.fx[p])
            return;
        set.fx[p] = y;
        post(EV_FX, (uint32_t)p, (uint32_t)(y * 1000.0f + 0.5f));
    }
    set_dirty = 1;
    ui.redraw = 1;
}

static void octave_step(int32_t d)
{
    int32_t o = set.octave + d;
    char t[4];
    if (o < -2 || o > 2)
        return;
    post(EV_PANIC, 0, 0);
    set.octave = (int8_t)o;
    set_dirty = 1;
    t[0] = o < 0 ? '-' : '+';
    t[1] = (char)('0' + (o < 0 ? -o : o));
    t[2] = 0;
    app_say("OCTAVE", t);
}

static void preset_say(int i, const char *what)
{
    char t[24];
    t[0] = (char)('0' + (i + 1) / 10);
    t[1] = (char)('0' + (i + 1) % 10);
    t[2] = ' ';
    t[3] = 0;
    text_cat(t, presets[i].name);
    app_say(what, t);
}
/* the preset, whole, into a slot under a name */
static void preset_save(int slot, const char *name)
{
    int i, bank, ok;
    for (i = 0; i < 15; i++)
        presets[slot].name[i] = name[i];
    presets[slot].s = set;
    presets[slot].used = 1;
    bank = slot / PRESETS_PER_OBJ;
    ok = plat_store_save((uint32_t)(OBJ_PRESETS_A + bank), &presets[bank * PRESETS_PER_OBJ],
                         sizeof(preset_t) * PRESETS_PER_OBJ) == 0;
    ok = ok && plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0;   /* and the state at power-on */
    if (!ok) {
        app_say("SAVE", "FAILED");
        return;
    }
    cur_preset = (int8_t)slot;
    set_dirty = 0;
    preset_say(slot, "SAVED");
}
static void save_new_name(void)
{
    name_rng ^= plat_ms() * 2654435761u;       /* when it was asked for: a different name each time */
    if (!name_rng)
        name_rng = 1u;
    acid_name(ui.save_name);
}
static void save_slot_to(int slot)             /* the slot's own name follows, unless SELECT gave a new one */
{
    int i;
    ui.save_slot = (int8_t)slot;
    if (ui.save_named)
        return;
    if (presets[slot].used)
        for (i = 0; i < 15; i++)
            ui.save_name[i] = presets[slot].name[i];
    else
        save_new_name();
}
/* SAVE: the screen opens on the preset loaded (or the list's cursor, or the first empty slot) */
static void save_open(void)
{
    int i, slot = ui.list ? ui.list_cur : cur_preset;
    if (slot < 0) {
        for (i = 0; i < NPRESETS && presets[i].used; i++)
            ;
        slot = i < NPRESETS ? i : 0;
    }
    ui.save = 1;
    ui.save_yes = 0;
    ui.save_named = 0;
    ui.list = 0;
    ui.edit = 0;
    save_slot_to(slot);
}
/* the PRESETS encoder: the first turn opens the list on the preset loaded; then each detent moves the cursor
 * over the 32 slots, and a saved preset under it loads at once */
static void preset_step(int32_t d)
{
    if (!ui.list) {
        ui.list = 1;
        ui.list_cur = (int8_t)(cur_preset >= 0 ? cur_preset : 0);
    } else {
        int c = ((ui.list_cur + d) % NPRESETS + NPRESETS) % NPRESETS;
        ui.list_cur = (int8_t)c;
        if (presets[c].used)
            post(EV_LOAD, (uint32_t)c, 0);     /* running: it waits for the end of the pattern (NEXT) */
    }
    ui.del_t = 0;
    ui.list_until = plat_ms() + 6000u;
    ui.redraw = 1;
}
/* in the list, SEL or a white key: the preset loads at once (running too, without waiting for the pattern's end) */
static const uint8_t WHITE_KEY[16] = PLAT_WHITE_KEYS;
static int list_half(void) { return ui.list_cur >= 16 ? 16 : 0; }   /* the white keys: presets 1..16 or 17..32 */
static void preset_load_now(int c)
{
    ui.list_cur = (int8_t)c;
    ui.list_until = plat_ms() + 6000u;
    ui.del_t = 0;
    if (presets[c].used)
        post(EV_LOAD, (uint32_t)c, 1);
    else
        app_say("EMPTY", "");
    ui.redraw = 1;
}
static void preset_delete(int slot)
{
    int bank = slot / PRESETS_PER_OBJ;
    uint32_t i;
    presets[slot].used = 0;
    for (i = 0; i < sizeof presets[slot].name; i++)
        presets[slot].name[i] = 0;
    if (plat_store_save((uint32_t)(OBJ_PRESETS_A + bank), &presets[bank * PRESETS_PER_OBJ],
                        sizeof(preset_t) * PRESETS_PER_OBJ) != 0) {
        app_say("DELETE", "FAILED");
        return;
    }
    if (cur_preset == slot)
        cur_preset = -1;
    app_say("DELETED", "");
}

static int32_t view_base(void);
/* the editor: SELECT moves the cursor over the whole pattern (wrapping at its end); the page on view follows it */
static void edit_move(int32_t d)
{
    int32_t len = seq_len(), c = ((int32_t)ui.cursor + d) % len;
    ui.cursor = (uint8_t)(c < 0 ? c + len : c);
}
/* ------------------------------------------------------------ OPTIONS --- */
static const char *const OPT_LABEL[OP_N] = {"COLOR", "MIDI IN CH", "MIDI OUT CH", "SEQ NOTES OUT", "CLOCK OUT",
    "PULSE WIDTH", "SQUARE DROOP", "SAW RESET", "SAW DROOP", "FILTER DRIVE", "SLIDE TIME", "AUTO LEGATO",
    "OTA SATURATION", "OUT SATURATION", "ANALOG DRIFT", "UNIT"};
static const char *const OPT_SECTION[OP_N] = {"DISPLAY", "MIDI", 0, 0, 0, "FINE TUNING", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static void opt_value_text(int i, char *t)
{
    static const char *const FINE[AT_NOPTS][5] = {
        {"44 %", "47 %", "50 %", "53 %", "56 %"}, {"NONE", "8 HZ", "15 HZ", "30 HZ", "60 HZ"},
        {"1 US", "3 US", "8 US", "20 US"}, {"0.7 HZ", "3 HZ", "8 HZ", "15 HZ"}, {"0 DB", "+6 DB", "+12 DB"},
        {"60 MS", "120 MS", "220 MS"}, {"OFF", "ON"}, {"NONE", "SOFT", "HARD"}, {"NONE", "SOFT", "HARD"},
        {"NONE", "SUBTLE", "MARKED", "HEAVY"}, {"1", "2", "3", "4"}};
    int v = opt.v[i];
    t[0] = 0;
    if (i == OP_COLOR)
        text_cat(t, THEMES[v].name);
    else if (i == OP_MIDI_IN && v == 0)
        text_cat(t, "ALL");
    else if (i == OP_MIDI_IN || i == OP_MIDI_OUT)
        int_text(i == OP_MIDI_IN ? v : v + 1, t);
    else if (i < OP_FINE)
        text_cat(t, v ? "ON" : "OFF");
    else
        text_cat(t, FINE[i - OP_FINE][v]);
}
static void opt_change(int i, int32_t e)       /* a step through item i's choices (no wrap) */
{
    int32_t v = opt.v[i] + (e > 0 ? 1 : -1), n = opt_count(i);
    if (v < 0 || v >= n)
        return;
    opt.v[i] = (uint8_t)v;
    ui.opt_dirty = 1;
    if (i == OP_COLOR) {
        theme_apply(v);
        lcd_fill(0, 0, 240, 240, C_BLACK);
    } else if (i >= OP_FINE)
        post(EV_OPTION, (uint32_t)(i - OP_FINE), (uint32_t)v);
}
static void opt_close(void)
{
    ui.opts = 0;
    if (ui.opt_dirty && !backup_hold) {
        ui.opt_dirty = 0;
        if (plat_store_save(OBJ_OPTIONS, &opt, sizeof opt) == 0)
            app_say("OPTIONS", "SAVED");
        else
            app_say("OPTIONS", "NOT SAVED");
    }
    ui.redraw = 1;
}
static void home_pages(void)                   /* HOME: the four sound pages in turn (VCF, VOICE, PATT, SEQ); from the
                                                  editor, the list or an effect page, straight to VCF */
{
    set.page = (uint8_t)(ui.edit || ui.list || set.page >= 3u ? 0u : set.page + 1u);
    ui.edit = 0;
    ui.list = 0;
    set_dirty = 1;
    ui.redraw = 1;
}

static void input(void)
{
    uint32_t keys, btn = plat_buttons(), k;
    int32_t e;
    if (ui.loaded_seen != loaded_n) {          /* a preset went in (now, or at the end of the pattern) */
        ui.loaded_seen = loaded_n;
        cur_preset = loaded_preset;
        set_dirty = 0;
        ui.redraw = 1;
    }
    touch_poll();
    keys = touch_keys();
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k;
        int32_t note = 53 + (int32_t)k + 12 * set.octave;
        if (ui.list && (keys & bit) && !(ui.keys & bit)) {   /* the list: a white key is a preset (1..16 or 17..32) */
            int w;
            for (w = 0; w < 16; w++)
                if (WHITE_KEY[w] == k)
                    preset_load_now(list_half() + w);
            continue;                          /* (its release: a note off for nothing, harmless) */
        }
        if ((keys & bit) && !(ui.keys & bit)) {
            uint32_t v = touch_vel(k);
            if (ui.edit) {                     /* the editor: the key is the step's note; on to the next step */
                int32_t base = 36 + 12 * (set.octave + 1) + set.root;
                post(EV_EDIT, ui.cursor, (uint32_t)ED_NOTE | (uint32_t)(note - base + 64) << 8);
                if (!sq.running)
                    post(EV_NOTE_ON, (uint32_t)note, v);   /* and hear it */
                edit_move(1);
                set_dirty = 1;
                ui.redraw = 1;
            } else if (sq.running) {           /* transpose: the key is the new root */
                set.root = (uint8_t)(note % 12);
                set_dirty = 1;
                app_say("ROOT", ACID_NOTE_NAMES[set.root]);
            } else
                post(EV_NOTE_ON, (uint32_t)note, v);
            midi_note_out(note, v ? v : 1u);
        } else if (!(keys & bit) && (ui.keys & bit)) {
            if (!sq.running)
                post(EV_NOTE_OFF, (uint32_t)note, 0);   /* (in the editor too: the note heard ends) */
            midi_note_out(note, 0);
        }
    }
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        if (k == B_SAVE && backup_hold) {      /* a backup is streaming: nothing is written meanwhile */
            app_say("SAVE", "BACKUP RUNNING");
            continue;
        }
        if (ui.save) {                         /* the SAVE screen: SAVE goes on, any other button cancels */
            if (k == B_SAVE && (ui.save == 1 && presets[ui.save_slot].used)) {
                ui.save = 2;                   /* a slot in use: OVERWRITE? NO / YES */
                ui.save_yes = 0;
            } else if (k == B_SAVE && (ui.save == 1 || ui.save_yes)) {
                ui.save = 0;
                preset_save(ui.save_slot, ui.save_name);
            } else if (k == B_SAVE)
                ui.save = 1;                   /* NO: back to choosing */
            else {
                ui.save = 0;
                app_say("SAVE", "CANCELLED");
            }
            ui.redraw = 1;
            continue;
        }
        if (ui.opts) {                         /* OPTIONS: HOME (or any button but OCT-) closes it, saved */
            if (k == B_OCTDN) {                /* (OCT- + HOME again: closes; its release changes nothing) */
                ui.oct_hold = 1;
                ui.oct_used = 1;
            } else
                opt_close();
            continue;
        }
        if (ui.list && k == B_SEL) {           /* SEL: the one under the cursor, at once */
            preset_load_now(ui.list_cur);
            continue;
        }
        if (ui.list && k == B_ENV) {           /* delete: twice, within two seconds */
            int c = ui.list_cur;
            ui.list_until = plat_ms() + 6000u;
            if (!presets[c].used)
                app_say("EMPTY", "");
            else if (ui.del_t && plat_ms() - ui.del_t < 2000u) {
                ui.del_t = 0;
                preset_delete(c);
            } else {
                ui.del_t = plat_ms() | 1u;
                app_say("ENV AGAIN", "TO DELETE");
            }
            ui.redraw = 1;
        } else if (k == B_OCTDN) {
            ui.oct_hold = 1;                   /* on release, unless held for a combination meanwhile: + HOME
                                                  OPTIONS, + K4 in the editor STEPS */
            ui.oct_used = 0;
        }
        else if (k == B_OCTUP)
            octave_step(1);
        else if (k == B_PLAY)
            post(EV_PLAY, 0, 0);
        else if (k == B_FX) {                  /* the effect pages: in, then the next one */
            set.page = (uint8_t)(set.page < 4u || set.page == NPAGES - 1u ? 4u : set.page + 1u);
            set_dirty = 1;
            ui.redraw = 1;
        }
        else if (k == B_SEQ && ui.oct_hold) {  /* OCT- + SEQ: LOCK (GENERATE mutates instead of a new seed) */
            ui.oct_used = 1;
            set.locked = (uint8_t)!set.locked;
            set_dirty = 1;
            app_say("LOCK", set.locked ? "ON: MUTATE" : "OFF: NEW SEED");
        } else if (k == B_SEQ) {
            post(EV_GENERATE, 0, 0);
            set_dirty = 1;
        } else if (k == B_EDIT) {              /* the sequence's three: the editor -> SEQ -> PATT -> the editor */
            if (ui.edit) {
                ui.edit = 0;
                set.page = 3u;
                set_dirty = 1;
            } else if (set.page == 3u && !ui.list) {
                set.page = 2u;
                set_dirty = 1;
            } else {
                ui.edit = 1;
                ui.list = 0;
                if (ui.cursor >= seq_len())
                    ui.cursor = 0;
            }
            ui.redraw = 1;
        } else if (ui.edit && (k == B_ENV || k == B_LFO)) {
            post(EV_EDIT, ui.cursor, k == B_ENV ? ED_ACC : ED_SLIDE);
            set_dirty = 1;
        } else if (k == B_ENV || k == B_LFO || k == B_ARP) {
            post(EV_MUTATE, k == B_ENV ? ACID_MUT_TIME : k == B_LFO ? ACID_MUT_PITCH : ACID_MUT_ARTICULATION,
                 (uint32_t)view_base());       /* the page on view */
            set_dirty = 1;
        } else if (k == B_GLO || (k == B_HOME && ui.oct_hold)) {   /* GLOBAL (or OCT- + HOME): OPTIONS */
            ui.oct_used = 1;
            ui.opts = 1;
            ui.edit = ui.list = 0;
            ui.redraw = 1;
        } else if (k == B_HOME)
            home_pages();
        else if (k == B_REC)
            post(EV_UNDO, 0, 0);
        else if (k == B_SAVE) {
            save_open();
            ui.redraw = 1;
        }
    }
    if (ui.oct_hold && !(btn >> B_OCTDN & 1u)) {   /* OCT- let go: the octave, unless a combination used it */
        ui.oct_hold = 0;
        if (!ui.oct_used)
            octave_step(-1);
    }
    for (k = 0; k < NE; k++) {
        if ((e = plat_enc((int)k)) == 0)
            continue;
        if (ui.save) {                         /* PRESETS: the slot; SELECT: a new name; in OVERWRITE? any: NO / YES */
            if (ui.save == 2)
                ui.save_yes = (uint8_t)(e > 0);
            else if (k == EN_PRESET)
                save_slot_to(((ui.save_slot + e) % NPRESETS + NPRESETS) % NPRESETS);
            else if (k == EN_SELECT || k == EN_K1) {
                ui.save_named = 1;
                save_new_name();
            }
            ui.redraw = 1;
            continue;
        }
        if (ui.opts) {                         /* OPTIONS: SELECT the item, any other its value */
            if (k == EN_SELECT) {
                int32_t c = ui.opt_cur + (e > 0 ? 1 : -1);
                if (c >= 0 && c < OP_N)
                    ui.opt_cur = (uint8_t)c;
            } else
                opt_change(ui.opt_cur, e);
            ui.redraw = 1;
            continue;
        }
        if (k == EN_PRESET) {                  /* through the saved presets, loading each */
            preset_step(e);
            continue;
        }
        if (ui.edit && k == EN_K4 && ui.oct_hold) {   /* OCT- held + K4: the pattern's length */
            int32_t v = set.steps + e;
            char t[8];
            set.steps = (uint8_t)(v < 1 ? 1 : v > ACID_MAX_STEPS ? ACID_MAX_STEPS : v);
            if (ui.cursor >= set.steps)
                ui.cursor = (uint8_t)(set.steps - 1);
            ui.oct_used = 1;
            set_dirty = 1;
            int_text(set.steps, t);
            app_say("STEPS", t);
        } else if (ui.edit && k == EN_SELECT) {
            edit_move(e);
            ui.redraw = 1;
        } else if (ui.edit && k >= EN_K1 && k <= EN_K4) {   /* K1 a semitone, K2 octave, K3 ACC / SLIDE, K4 gate */
            static const uint8_t OP[4] = {ED_SEMI, ED_OCT, ED_ARTIC, ED_GATE};
            post(EV_EDIT, ui.cursor, (uint32_t)OP[k - EN_K1] | (e > 0 ? 1u : 0u) << 8);
            set_dirty = 1;
        } else if (k == EN_ALGO) {             /* ALGO: the wave, wherever you are (the VOICE page's WAVE) */
            char t[24];
            knob_turn(AT_WAVE, e > 0 ? 1 : -1);
            t[0] = 0;
            text_cat(t, AT_OSC_NAME[at_osc_index(set.param[AT_WAVE])]);
            app_say("WAVE", t);
        } else if (k == EN_SELECT) {
            set.page = (uint8_t)((set.page + (e > 0 ? 1u : NPAGES - 1u)) % NPAGES);
            set_dirty = 1;
            ui.redraw = 1;
        } else if (k >= EN_K1 && k <= EN_K4) {
            int kn = PAGE_KNOB[set.page][k - EN_K1];
            if (kn != K_NONE)
                knob_turn(kn, e);
        }
    }
    if (ui.regrow_at && plat_ms() >= ui.regrow_at) {
        ui.regrow_at = 0;
        post(EV_REGROW, 0, 0);
    }
    if (ui_event) {                            /* what a pattern button did */
        static const char *const NAME[] = {"", "", "", "", "", "GENERATE", "MUTATE", "UNDO"};
        uint8_t ev = ui_event - 1u;
        ui_event = 0;
        if (ev < 8u)
            app_say(NAME[ev], ui_event_ok ? "OK" : "NOTHING");
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    {
        /* lit: SAVE with changes, PLAY, SEQ with LOCK on, EDIT in the editor, GLOBAL in OPTIONS, SEL in the list.
         * In the list the white keys are its presets: the one loaded lit, the saved ones at the mid level */
        uint32_t b = btn | (set_dirty ? 1u << B_SAVE : 0u) | (sq.running ? 1u << B_PLAY : 0u)
                     | (set.locked ? 1u << B_SEQ : 0u) | (ui.edit ? 1u << B_EDIT : 0u)
                     | (ui.opts ? 1u << B_GLO : 0u) | (ui.list ? 1u << B_SEL : 0u),
                 kl = keys, km = 0;
        if (ui.list) {
            int w, half = list_half();
            for (w = 0; w < 16; w++) {
                uint32_t bit = 1u << WHITE_KEY[w];
                if (half + w == cur_preset)
                    kl |= bit;
                else if (presets[half + w].used)
                    km |= bit;
            }
        }
        plat_glow(1);
        plat_leds(b, kl);
        plat_lights_mid(0, km);
    }
}

void app_input_only(void) { input(); }

static void note_name(int32_t note, char *b)
{
    char o[12];
    b[0] = 0;
    text_cat(b, ACID_NOTE_NAMES[note % 12]);
    int_text(note / 12 - 1, o);
    text_cat(b, o);
}

/* the pattern strip: a cell a step, as the SEQ module's LEDs (green note, blue tie, amber slide,
 * now in the acid editor's colours of the VCV modules: blue note, dark blue tie, pale accent, cyan slide), the step
 * under way framed white */
/* the 16 steps on view (a page of the 64, as the ATEK303 SEQ's 1:4 .. 4:4): running, the one that plays; the
 * editor's, the cursor's; stopped, the first */
static int32_t view_base(void)
{
    int32_t s = ui.edit ? ui.cursor : sq.running ? sq.step : 0;
    return s / 16 * 16;
}
static void draw_pages(int32_t x, int32_t y)   /* the four pages: the one on view lit, the ones beyond the steps dark */
{
    int32_t k, len = seq_len(), on = view_base() / 16;
    for (k = 0; k < 4; k++)
        cv_rect(x + k * 15, y, 12, 7, k == on ? C_LOGO : k * 16 < len ? C_GRAY : C_LINE);
}
static void draw_pattern(int32_t y)
{
    int32_t n, len = seq_len(), cur = sq.step, base = view_base();
    const acid_grid_t *g = &sq.grid;
    for (n = 0; n < 16; n++) {
        int32_t x = 1 + n * 15, i = base + n;
        uint16_t c = C_LINE;
        if (i < len && g->gate[i]) {
            if (g->tie[i])
                c = AK_TIE;
            else if (g->accent[i])
                c = AK_ACCENT;
            else if (g->slide[i])
                c = AK_SLIDE;
            else
                c = AK_NOTE;
        } else if (i >= len)
            c = C_BLACK;
        if (sq.running && i == cur)
            cv_rect(x - 1, y - 1, 15, 22, C_WHITE);
        cv_rect(x, y, 13, 20, c);
    }
}

/* the page navigator at the foot: five tabs at a time around the page, the page boxed, arrows where there are more */
static const char *const PAGE_TAB[NPAGES] = {"VCF", "VOICE", "PATT", "SEQ", "DELAY", "CHOR", "DRIVE", "REVRB"};
static void draw_nav(int32_t y)
{
    int32_t first = (int32_t)set.page - 2, k;
    const int32_t TABS = 5, W = 44, X0 = 10, H = 20;
    if (first > NPAGES - TABS)
        first = NPAGES - TABS;
    if (first < 0)
        first = 0;
    cv_rect(0, y, 240, 1, C_LOGO);               /* the frame */
    cv_rect(0, y + H + 1, 240, 1, C_LOGO);
    cv_rect(0, y, 1, H + 2, C_LOGO);
    cv_rect(239, y, 1, H + 2, C_LOGO);
    for (k = 0; k < TABS; k++) {
        int32_t i = first + k, x = X0 + k * W, tw = text_w(&FONT_XS, PAGE_TAB[i]);
        if (i == set.page) {
            cv_rect(x, y + 3, W - 4, H - 4, C_LOGO);
            cv_text(x + (W - 4 - tw) / 2, y + 4, &FONT_XS, PAGE_TAB[i], C_WHITE);
        } else
            cv_text(x + (W - 4 - tw) / 2, y + 4, &FONT_XS, PAGE_TAB[i], C_HI);
    }
    for (k = 0; k < 4; k++) {                    /* the arrows: small triangles */
        if (first > 0)
            cv_rect(6 - k, y + 7 + k, 1, 8 - 2 * k, C_LOGO);
        if (first + TABS < NPAGES)
            cv_rect(233 + k, y + 7 + k, 1, 8 - 2 * k, C_LOGO);
    }
}

/* the step editor: a piano roll of one octave (the pitch class, root included), the octave in the UP and
 * DOWN rows below, then GATE, ACC and SLIDE, as on a 303's own pattern sheet */
#define ED_X 30                                /* the grid's left edge; 16 columns of 13 */
#define ED_W 13
static int32_t step_pc(const acid_grid_t *g, int st)
{
    int semi = acid_semi_of(g->deg[st], g->oct[st], set.scale) + set.root;
    return ((semi % 12) + 12) % 12;
}
static int32_t step_oct(const acid_grid_t *g, int st)
{
    int semi = acid_semi_of(g->deg[st], g->oct[st], set.scale) + set.root;
    return (semi - (((semi % 12) + 12) % 12)) / 12;
}
static void draw_editor(void)
{
    static const uint8_t BLACK[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    static const char *const ROW[5] = {"UP", "DOWN", "GATE", "ACC", "SLIDE"};
    const acid_grid_t *g = &sq.grid;
    int32_t n, i, r, len = seq_len(), cur = ui.cursor, base = view_base(), play = sq.running ? sq.step - base : -1;
    const uint16_t NOTE = AK_NOTE, ACC = AK_ACCENT, TIE = AK_TIE;
    const uint16_t SLIDE = AK_SLIDE, ROWA = RGB(0x16, 0x18, 0x1C), ROWB = RGB(0x22, 0x25, 0x2B);
    /* the roll: 12 rows of 8 px, B at the top */
    cv_begin(240, 100, C_BLACK);
    for (r = 0; r < 12; r++) {
        int32_t y = 2 + (11 - r) * 8;
        cv_rect(2, y, 26, 7, BLACK[r] ? RGB(20, 20, 24) : RGB(200, 200, 206));
        if (r == 0)
            cv_text(17, y - 3, &FONT_T, "C", C_BLACK);
        cv_rect(ED_X, y, 16 * ED_W, 7, BLACK[r] ? ROWA : ROWB);
    }
    for (i = 4; i < 16; i += 4)
        cv_rect(ED_X + i * ED_W - 1, 2, 1, 96, C_DIM);
    for (n = 0; n < 16; n++) {
        int32_t x = ED_X + n * ED_W, y;
        i = base + n;
        if (i >= len) {
            cv_rect(x, 2, ED_W, 96, C_BLACK);
            continue;
        }
        if (i == cur) {                        /* the cursor's column, lit */
            for (r = 0; r < 12; r++)
                cv_rect(x, 2 + (11 - r) * 8, ED_W, 7, BLACK[r] ? RGB(44, 50, 72) : RGB(58, 66, 92));
            cv_rect(x, 0, ED_W, 1, C_WHITE);
            cv_rect(x, 99, ED_W, 1, C_WHITE);
        }
        if (!g->gate[i])
            continue;
        y = 2 + (11 - step_pc(g, i)) * 8;
        cv_rect(x + 1, y + 1, ED_W - 2, 5, g->tie[i] ? TIE : g->accent[i] ? ACC : NOTE);
        if (g->slide[i] && i + 1 < len && g->gate[i + 1] && n < 15) {   /* the slide: a line to the next note */
            int32_t y2 = 2 + (11 - step_pc(g, i + 1)) * 8;
            cv_line(x + ED_W - 2, y + 3, x + ED_W + ED_W / 2, y2 + 3, SLIDE);
        }
    }
    if (play >= 0 && play < 16)
        cv_rect(ED_X + play * ED_W, 1, ED_W, 1, C_HI);
    cv_blit(0, 32);
    /* the five rows, and what the controls do */
    cv_begin(240, 108, C_BLACK);
    for (r = 0; r < 5; r++) {
        int32_t y = 4 + r * 16;
        cv_text(2, y + 1, &FONT_T, ROW[r], C_GRAY);
        for (n = 0; n < 16; n++) {
            int32_t x = ED_X + n * ED_W, i = base + n, oc = step_oct(g, i);
            uint16_t c = RGB(28, 30, 40);
            int on = i < len && g->gate[i];
            if (i >= len)
                c = C_BLACK;
            else if (r == 0 && on && oc > 0)
                c = oc > 1 ? ACC : AK_OCTAVE;
            else if (r == 1 && on && oc < 0)
                c = oc < -1 ? ACC : AK_OCTAVE;
            else if (r == 2 && on)
                c = g->tie[i] ? TIE : NOTE;
            else if (r == 3 && on && g->accent[i])
                c = ACC;
            else if (r == 4 && on && g->slide[i]) {   /* dim: set, but into a rest or a tie it does nothing */
                int nx = (i + 1) % len;
                c = g->gate[nx] && !g->tie[nx] ? SLIDE : RGB(0, 70, 100);
            }
            cv_rect(x + 1, y, ED_W - 2, 13, c);
            if (i == cur)
                cv_rect(x, y + 14, ED_W, 1, C_WHITE);
        }
    }
    cv_text(2, 86, &FONT_T, "SEL STEP  K1 NOTE  K2 OCT  K3 ACC/SLD  K4 GATE", C_DIM);
    cv_text(2, 97, &FONT_T, "KEY NOTE ENV ACC LFO SLD REC UNDO OCT-+K4 LEN", C_DIM);
    cv_blit(0, 132);
}

/* the preset list: 8 rows of the 32 slots around the cursor; the one loaded in colour, the cursor lit */
static void draw_list(void)
{
    int32_t top = ui.list_cur - 3, r, band;
    char t[24];
    if (top < 0)
        top = 0;
    if (top > NPRESETS - 8)
        top = NPRESETS - 8;
    for (band = 0; band < 2; band++) {
        cv_begin(240, band ? 124 : 84, C_BLACK);   /* the whole area: 32 .. 240 */
        for (r = band ? 4 : 0; r < (band ? 8 : 4); r++) {
            int32_t i = top + r, y = (r - (band ? 4 : 0)) * 20 + (band ? 0 : 4);
            int cur = i == ui.list_cur;
            if (cur)
                cv_rect(4, y, 232, 19, RGB(0x22, 0x25, 0x2B));
            t[0] = (char)('0' + (i + 1) / 10);
            t[1] = (char)('0' + (i + 1) % 10);
            t[2] = 0;
            cv_text(10, y + 2, &FONT_B, t, cur ? C_WHITE : C_DIM);
            if (presets[i].used) {
                cv_text(40, y + 2, &FONT_B, presets[i].name, i == cur_preset ? C_HI : cur ? C_WHITE : C_GRAY);
                if (i == pend_preset)          /* chosen, on at the end of the pattern */
                    cv_text(196, y + 3, &FONT_XS, "NEXT", AK_AMBER);
            } else
                cv_text(40, y + 3, &FONT_XS, "- EMPTY -", C_DIM);
        }
        if (band) {
            cv_text(4, 84, &FONT_T, "PRESETS MOVE  SEL / WHITE KEYS LOAD NOW", C_DIM);
            cv_text(4, 94, &FONT_T, "ENV x2 DELETE  SAVE HERE  HOME CLOSE", C_DIM);
        }
        cv_blit(0, band ? 116 : 32);
    }
}

/* OPTIONS: the items, 8 on view, the section's name over the first of each */
static void draw_options(void)
{
    int32_t top = ui.opt_cur - 3, r, band;
    char t[24];
    if (top < 0)
        top = 0;
    if (top > OP_N - 8)
        top = OP_N - 8;
    for (band = 0; band < 2; band++) {
        cv_begin(240, band ? 124 : 84, C_BLACK);
        for (r = band ? 4 : 0; r < (band ? 8 : 4); r++) {
            int32_t i = top + r, y = (r - (band ? 4 : 0)) * 20 + (band ? 0 : 4);
            int cur = i == ui.opt_cur;
            if (cur)
                cv_rect(4, y, 232, 19, RGB(0x22, 0x25, 0x2B));
            if (OPT_SECTION[i])
                cv_rect(4, y, 232, 1, C_LINE);
            cv_text(10, y + 4, &FONT_XS, OPT_LABEL[i], cur ? C_WHITE : C_GRAY);
            opt_value_text(i, t);
            cv_text(230 - text_w(&FONT_XS, t), y + 4, &FONT_XS, t, cur ? C_HI : C_AMB);
        }
        if (band) {
            const char *sec = 0;
            int32_t i;
            for (i = ui.opt_cur; i >= 0 && !sec; i--)
                sec = OPT_SECTION[i];
            cv_text(4, 84, &FONT_T, sec, C_LOGO);
            cv_text(4, 96, &FONT_T, "SEL ITEM  K1-K4 VALUE  HOME CLOSE", C_DIM);
        }
        cv_blit(0, band ? 116 : 32);
    }
}

/* the SAVE screen (as on the FMini-1): slot, OVERWRITE or EMPTY, the name; then OVERWRITE? NO / YES */
static void draw_save(void)
{
    char t[24];
    int32_t band, x;
    int used = presets[ui.save_slot].used;
    for (band = 0; band < 2; band++) {
        cv_begin(240, band ? 124 : 84, C_BLACK);
        cv_rect(4, 0, 2, band ? 120 : 84, C_LOGO);   /* a frame down the sides */
        cv_rect(234, 0, 2, band ? 120 : 84, C_LOGO);
        if (band)
            cv_rect(4, 118, 232, 2, C_LOGO);
        else
            cv_rect(4, 0, 232, 2, C_LOGO);
        if (ui.save == 1 && !band) {
            t[0] = (char)('0' + (ui.save_slot + 1) / 100);
            t[1] = (char)('0' + (ui.save_slot + 1) / 10 % 10);
            t[2] = (char)('0' + (ui.save_slot + 1) % 10);
            t[3] = 0;
            cv_text(120 - text_w(&FONT_B, "SAVE PRESET") / 2, 8, &FONT_B, "SAVE PRESET", C_WHITE);
            x = cv_text(18, 40, &FONT_B, t, C_WHITE);
            cv_text(x + 12, 44, &FONT_XS, used ? "OVERWRITE" : "EMPTY", used ? AK_AMBER : C_GRAY);
        } else if (ui.save == 1) {
            cv_text(120 - text_w(&FONT_B, ui.save_name) / 2, 0, &FONT_B, ui.save_name, C_HI);
            cv_text(16, 40, &FONT_T, "PRESETS  SLOT", C_GRAY);
            cv_text(130, 40, &FONT_T, "SELECT  NAME", C_GRAY);
            cv_text(16, 60, &FONT_T, "SAVE = NEXT", C_GRAY);
            cv_text(130, 60, &FONT_T, "OTHER = CANCEL", C_GRAY);
        } else if (!band) {
            t[0] = (char)('0' + (ui.save_slot + 1) / 100);
            t[1] = (char)('0' + (ui.save_slot + 1) / 10 % 10);
            t[2] = (char)('0' + (ui.save_slot + 1) % 10);
            t[3] = ' ';
            t[4] = 0;
            text_cat(t, presets[ui.save_slot].name);
            cv_text(120 - text_w(&FONT_B, "OVERWRITE?") / 2, 12, &FONT_B, "OVERWRITE?", C_WHITE);
            cv_text(120 - text_w(&FONT_XS, t) / 2, 50, &FONT_XS, t, C_GRAY);
        } else {
            int32_t i;
            for (i = 0; i < 2; i++) {          /* NO / YES: the chosen one filled */
                int on = i == ui.save_yes;
                int32_t bx = 22 + i * 104;
                const char *w = i ? "YES" : "NO";
                cv_rect(bx, 0, 92, 44, on ? C_LOGO : C_LINE);
                if (!on)
                    cv_rect(bx + 2, 2, 88, 40, C_BLACK);
                cv_text(bx + 46 - text_w(&FONT_B, w) / 2, 12, &FONT_B, w, C_WHITE);
            }
            cv_text(16, 60, &FONT_T, "TURN  NO / YES", C_GRAY);
            cv_text(130, 60, &FONT_T, "SAVE = OK", C_GRAY);
        }
        cv_blit(0, band ? 116 : 32);
    }
}

/* the battery, top right of the header: 3 bars (amber when empty), blue when USB powers the FM-1, none when not
 * read yet (or the web emulator's). Returns where the header's right-aligned text ends, left of it */
static int32_t draw_battery(int b)
{
    int32_t i;
    uint16_t c = b == 4 ? C_LOGO : b == 0 ? AK_AMBER : C_HI;
    if (b < 0)
        return 232;
    cv_rect(213, 10, 20, 12, c);
    cv_rect(214, 11, 18, 10, C_BLACK);
    cv_rect(233, 13, 2, 6, c);
    for (i = 0; i < (b == 4 ? 3 : b); i++)
        cv_rect(216 + 5 * i, 13, 4, 6, c);       /* 2 px of black around them, as above and below */
    return 206;
}

static void draw(void)
{
    char t[24];
    int32_t note = ui_note, i, right;
    uint32_t now = plat_ms();
    cv_begin(240, 32, C_BLACK);
    right = draw_battery(plat_battery());
    if (now < ui.msg_until && !ui.save) {
        int32_t x = cv_text(10, 6, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], C_HI);
    } else {
        int32_t x = cv_text(10, 6, &FONT_B, ui.opts ? "OPTIONS" : ui.save ? "SAVE" : ui.list ? "PRESETS" : ui.edit ? "EDIT" : cur_preset >= 0 ? presets[cur_preset].name : APP_NAME, C_WHITE);
        if (ui.edit) {                         /* the step under the cursor: number, note, accent, slide */
            const acid_grid_t *gg = &sq.grid;
            int st = ui.cursor;
            t[0] = 0;
            text_cat(t, "STEP ");
            {
                char nn[4];
                int_text(st + 1, nn);
                text_cat(t, nn);
                if (seq_len() > 16) {          /* the page, as the SEQ module's 1:4 .. 4:4 */
                    text_cat(t, "  ");
                    int_text(st / 16 + 1, nn);
                    text_cat(t, nn);
                    text_cat(t, ":");
                    int_text((seq_len() + 15) / 16, nn);
                    text_cat(t, nn);
                }
            }
            x = cv_text(x + 8, 8, &FONT_XS, t, C_GRAY);
            t[0] = 0;
            if (gg->gate[st]) {
                note_name(36 + 12 * (set.octave + 1) + set.root + acid_semi_of(gg->deg[st], gg->oct[st], set.scale), t);
                if (gg->tie[st])
                    text_cat(t, " TIE");
                if (gg->accent[st])
                    text_cat(t, " ACC");
                if (gg->slide[st])
                    text_cat(t, " SLD");
            } else
                text_cat(t, "REST");
            cv_text(right - text_w(&FONT_XS, t), 8, &FONT_XS, t, C_HI);
            note = -1;
        } else
            cv_text(x + 8, 8, &FONT_XS, pend_preset >= 0 || pend_gen ? "NEXT" : sq.running ? "PLAY" : "", pend_preset >= 0 || pend_gen ? AK_AMBER : C_HI);
        if (note >= 0) {
            note_name(note, t);
            cv_text(right - 2 - text_w(&FONT_B, t), 6, &FONT_B, t, C_HI);
        }
    }
    cv_blit(0, 0);
    if (ui.opts) {
        draw_options();
        return;
    }
    if (ui.save) {
        draw_save();
        return;
    }
    if (ui.list) {
        draw_list();
        return;
    }
    if (ui.edit) {
        draw_editor();
        return;
    }
    cv_begin(240, 138, C_BLACK);               /* 32 .. 170: no gap under the header */
    for (i = 0; i < 4; i++) {
        int kn = PAGE_KNOB[set.page][i];
        int32_t x = (i & 1) * 120, y = (i >> 1) * 68 + 2, w;
        if (kn == K_NONE)
            continue;
        cv_text(x + 8, y + 2, &FONT_XS, PAGE_NAME[set.page][i], C_GRAY);
        knob_text(kn, t);
        cv_text(x + 8, y + 18, &FONT_B, t, C_HI);
        cv_rect(x + 8, y + 46, 104, 6, C_LINE);
        w = (int32_t)(104.0f * knob_value(kn) + 0.5f);
        cv_rect(x + 8, y + 46, w, 6, C_LOGO);
    }
    cv_blit(0, 32);
    cv_begin(240, 70, C_BLACK);
    t[0] = 0;
    text_cat(t, set.locked ? "LOCK  " : "");
    text_cat(t, "OCT ");
    {
        char o[4];
        o[0] = set.octave < 0 ? '-' : '+';
        o[1] = (char)('0' + (set.octave < 0 ? -set.octave : set.octave));
        o[2] = 0;
        text_cat(t, o);
    }
    cv_text(232 - text_w(&FONT_T, t), 0, &FONT_T, t, C_GRAY);
    if (seq_len() > 16)
        draw_pages(4, 3);
    draw_pattern(14);
    draw_nav(46);
    cv_blit(0, 170);
}

/* the first screen: the acid smiley in Animatek blue (art/splash-smiley.png, tools/make_splash.py), and the name */
#include "splash_img.h"
void app_splash(void)
{
    const uint16_t BG = RGB(33, 33, 33);
    uint32_t run = 0, left = 0, idx = 0, y0, rows;
    int32_t w;
    lcd_fill(0, 0, 240, 240, BG);
    for (y0 = 0; y0 < SPLASH_H; y0 += rows) {   /* in bands: the canvas holds 240 x 144 */
        uint32_t y, x;
        rows = SPLASH_H - y0 < 98u ? SPLASH_H - y0 : 98u;
        cv_begin(SPLASH_W, rows, BG);
        for (y = 0; y < rows; y++)
            for (x = 0; x < SPLASH_W; x++) {
                if (!left && run < SPLASH_RUNS) {
                    idx = SPLASH_RLE[2u * run];
                    left = SPLASH_RLE[2u * run + 1u];
                    run++;
                }
                if (left)
                    left--;
                cv_pset((int32_t)x, (int32_t)y, SPLASH_PAL[idx & 15u]);
            }
        cv_blit((240u - SPLASH_W) / 2u, 6u + y0);
    }
    cv_begin(240, 30, BG);
    w = text_w(&FONT_B, APP_NAME);
    cv_text((240 - w) / 2, 6, &FONT_B, APP_NAME, C_WHITE);
    cv_blit(0, 206);
}

void app_ui_init(void)
{
    theme_apply(opt.v[OP_COLOR]);
    touch_on(1);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.redraw = 1;
}

void app_frame(void)
{
    static int32_t last_note = -2;
    static uint8_t last_acc, last_slide, last_step = 255, last_run;
    static uint32_t last_seed, last_count, last_grid;
    static int last_batt = -2;
    uint32_t now = plat_ms();
    input();
    ann_tick(now);
    if (plat_battery() != last_batt) {         /* the battery's bars (header) */
        last_batt = plat_battery();
        ui.redraw = 1;
    }
    if (ui_note != last_note || ui_accent != last_acc || ui_slide != last_slide || sq.step != last_step
        || sq.running != last_run || set.pattern.seed != last_seed || set.mutation_counter != last_count || grid_version != last_grid) {
        last_grid = grid_version;
        last_note = ui_note;
        last_acc = ui_accent;
        last_slide = ui_slide;
        last_step = sq.step;
        last_run = sq.running;
        last_seed = set.pattern.seed;
        last_count = set.mutation_counter;
        ui.redraw = 1;
    }
    if (ui.list && now >= ui.list_until) {
        ui.list = 0;
        ui.redraw = 1;
    }
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (ui.redraw) {
        ui.redraw = 0;
        draw();
    }
}

/* backup and restore over USB (fm1-base: tools/fm1_backup.py, --magic AT64): the settings and the four preset banks.
 * Saving happens only on SAVE, so there is nothing to flush; while a backup streams, SAVE waits */
int app_backup_playing(void) { return sq.running; }
void app_backup_flush(void) {}
void app_backup_hold(int on) { backup_hold = (uint8_t)on; }
void app_backup_restored(void)
{
    store_load();
    cur_preset = -1;
    pend_preset = -1;
    pend_gen = 0;
    set_dirty = 0;
    post(EV_APPLY, 0, 0);
    ui.redraw = 1;
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "note"))
        *got = ui_note;
    else if (!strcmp(what, "octave"))
        *got = set.octave;
    else if (!strcmp(what, "page"))
        *got = set.page;
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "accent"))
        *got = ui_accent;
    else if (!strcmp(what, "slide"))
        *got = ui_slide;
    else if (!strcmp(what, "wave"))           /* the WAVE list's entry, 0 (SAW) .. AT_NOSC - 1 (SQUARE) */
        *got = at_osc_index(set.param[AT_WAVE]);
    else if (!strcmp(what, "cutoff"))
        *got = (int)(set.param[AT_CUTOFF] * 100.0f + 0.5f);
    else if (!strcmp(what, "running"))
        *got = sq.running;
    else if (!strcmp(what, "step"))
        *got = sq.step;
    else if (!strcmp(what, "seed"))
        *got = (int)(set.pattern.seed & 0x7FFFFFFFu);
    else if (!strcmp(what, "mutations"))
        *got = (int)set.mutation_counter;
    else if (!strcmp(what, "root"))
        *got = set.root;
    else if (!strcmp(what, "tempo"))
        *got = set.tempo;
    else if (!strcmp(what, "drive_type"))
        *got = (int)set.fx[FX_DRIVE_TYPE];
    else if (!strcmp(what, "dly_mix"))
        *got = (int)(set.fx[FX_DLY_MIX] * 100.0f + 0.5f);
    else if (!strcmp(what, "edit"))
        *got = ui.edit;
    else if (!strcmp(what, "cursor"))
        *got = ui.cursor;
    else if (!strncmp(what, "gate", 4) && what[4])   /* gateN: step N rest 0, note 1, tie 2 */
        *got = sq.grid.gate[atoi(what + 4) & 63] ? (sq.grid.tie[atoi(what + 4) & 63] ? 2 : 1) : 0;
    else if (!strncmp(what, "acc", 3) && what[3])
        *got = sq.grid.accent[atoi(what + 3) & 63];
    else if (!strncmp(what, "sld", 3) && what[3])
        *got = sq.grid.slide[atoi(what + 3) & 63];
    else if (!strncmp(what, "semi", 4) && what[4])   /* semiN: step N, semitones from the root */
        *got = acid_semi_of(sq.grid.deg[atoi(what + 4) & 63], sq.grid.oct[atoi(what + 4) & 63], set.scale);
    else if (!strcmp(what, "scale"))
        *got = set.scale;
    else if (!strcmp(what, "list"))
        *got = ui.list;
    else if (!strcmp(what, "listcur"))
        *got = ui.list_cur;
    else if (!strcmp(what, "preset"))
        *got = cur_preset;
    else if (!strcmp(what, "save"))
        *got = ui.save;
    else if (!strcmp(what, "opts"))
        *got = ui.opts;
    else if (!strcmp(what, "optcur"))
        *got = ui.opt_cur;
    else if (!strcmp(what, "optval"))          /* the value of the item under the cursor */
        *got = opt.v[ui.opt_cur];
    else if (!strcmp(what, "color"))
        *got = opt.v[OP_COLOR];
    else if (!strcmp(what, "drift"))
        *got = voice.drift;
    else if (!strcmp(what, "saveslot"))
        *got = ui.save_slot;
    else if (!strcmp(what, "steps"))
        *got = set.steps;
    else if (!strcmp(what, "stepgt15"))
        *got = sq.step > 15;
    else if (!strcmp(what, "samepage1")) {    /* the steps outside the first page as before the last change */
        int k, same = 1;
        for (k = 16; k < ACID_MAX_STEPS; k++)
            same = same && sq.grid.gate[k] == sq_before.gate[k] && sq.grid.deg[k] == sq_before.deg[k]
                   && sq.grid.oct[k] == sq_before.oct[k] && sq.grid.accent[k] == sq_before.accent[k]
                   && sq.grid.slide[k] == sq_before.slide[k] && sq.grid.tie[k] == sq_before.tie[k];
        *got = same;
    } else if (!strcmp(what, "pendgen"))
        *got = pend_gen;
    else if (!strcmp(what, "pending"))
        *got = pend_preset;
    else if (!strcmp(what, "npresets")) {
        int k, n = 0;
        for (k = 0; k < NPRESETS; k++)
            n += presets[k].used;
        *got = n;
    } else if (!strcmp(what, "locked"))
        *got = set.locked;
    else if (!strcmp(what, "notes"))
        *got = acid_semi_of(0, 0, 0) + set.pattern.pitch_length;
    else
        return 0;
    return 1;
}
#endif
