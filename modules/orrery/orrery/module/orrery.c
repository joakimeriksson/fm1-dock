/* SPDX-License-Identifier: GPL-3.0-only */
/* ORRERY: the seventies' synths of a Jarre record in one fm1-dock module, so they play together: the Eminent 310
 * strings as the bed, an ARP 2600 as the lead on the keys, a second 2600 as the bass with its 1601's sequences and
 * patterns, the Korg Mini Pops 7 as the drums, a tape echo with a send from the strings and the lead, a reverb, and a
 * chord a link of the bass pattern, so the strings move with the bass.
 *
 * Since M3-04 the module runs on the portable meloDock session (core/session.h): one clock, one sequencer and the
 * fixed mixer drive the instruments through their adapters. The 1601 sequences and the Mini Pops rhythms are
 * converted into patterns (module/band.c); the engines' own sequencers stay off. The echo and reverb are the
 * module's (effects/), and the dock's global room is asked to stay off. The panel is as before:
 *
 *   EDIT          STRINGS page:  KNOB1..4 attack, release, chorus, phaser; ALGORITHM the registration; FX the colour
 *   GLO           LEAD page:     ALGORITHM the sound (SOFT LEAD, LASER HARP, the 2600's ten, the Synthex's other 79); KNOB1..4 cutoff,
 *                                resonance, glide, level; SELECT the run: OFF, PENTA / SCALE / CHORD, FAST (70 ms) or
 *                                1/16: a key (or a MIDI note) plays a rise through the chord's scale, two octaves up
 *   ENV           BASS page:     ALGORITHM the patch, SELECT the sequence (eight, with their patterns); KNOB1..4
 *                                cutoff, (sweep: not in this version), tempo, level
 *   LFO           DRUMS page:    the white keys from F3 are the rhythm keys (one at a time: combining them is over the
 *                                audio budget), the black ones
 *                                F#3 G#3 A#3 the tambourine, guiro, quijada; OCT- / OCT+ the lever; ALGORITHM one
 *                                rhythm; KNOB1..4 tempo, level, balance, the extras' level
 *   SEQ           CHORDS page:   SELECT the link, KNOB1 its root, KNOB2 its kind (-, MIN, MAJ, MIN7, MAJ7, SUS2,
 *                                SUS4, 5TH), KNOB3 the strings' octave, KNOB4 the strings' level. REC: LATCH, the
 *                                keys set the chord instead of playing the lead (the link under the cursor)
 *   FX            MIX page, FX again ECHO page: KNOB1..4 the four levels; ALGORITHM the reverb; on ECHO: ALGORITHM
 *                                the time (OFF, 1/16 .. 1/2 of the tempo), KNOB1..4 feedback, tone, the strings'
 *                                send, the lead's send; SELECT the hall (HALLS: ROOM .. ENDLESS)
 *   keys          play the page's part: the strings on STRINGS / CHORDS, the bass on BASS, else the lead
 *   SEL           solo the page's part (again: everyone); the header says SOLO
 *   FX held       KNOB1 sweeps the master low-pass cutoff, KNOB2 its resonance (FX alone still changes page)
 *   PRESETS       on every page: the setup (fifteen, round; module/setups.h is where they are written), its name
 *                 over the page for two seconds; each setup
 *                 is a scene: AMBIENT (the default: slow strings, soft bass, soft drums, the effects box, a lead with
 *                 a background run, droplets and rising chirps, long echo, big hall: six parts) or BAND;
 *                 SEL on PARTS rounds the background parts: none, the box's run, then each of the lead's phrases
 *                 (HARP, LONG TONES, BELLS, CALLS) without the run and with it
 *   FX            cycles MIX (four levels, reverb, scene), PARTS (the wind's level and colour, the effects box, the lead's echo
 *                 send; ALGORITHM its reverb; SELECT the gestures' speed; the keys play the box: see fx_key), FX SEQ
 *                 and ECHO
 *   FX SEQ        the effect sequence: the sixteen events the effects box plays by itself, on a line of the setup's
 *                 sixteen bars (BAND: eight). SELECT the event; KNOB1..4 when (bar.eighth), its sound (none, or one
 *                 of the box's thirteen, DROP .. THUNDER: setups.h says what each is), its note (a step of the
 *                 setup's pentatonic: always in key; the colour of a sound of noise), its chance; a key sets the
 *                 note; ARP how the event is played, round: ONCE, or a gesture of the sound from its note, UP the
 *                 scale, DOWN, ARC (up and back), SPARKLE (a taller mark on the line); ALGORITHM a whole pattern
 *                 (setups.h's FX_PATTERNS, EMPTY .. CASCADES, and YOURS: the one edited here, kept while the others
 *                 are tried); SEL
 *                 solos the box. Each edit is heard at once and is kept with the setup
 *   AMBIENT       opens on the LEAD page: the keys play a soft lead over the bed (strings, bass, soft drums,
 *                 and the effects box: the background run, droplets, chirps and the wind on one channel)
 *                 or BAND (the 1601 bass sequences, the Mini Pops, the chord links)
 *   PLAY          the scene: everything starts together on one clock, and stops
 *   ARP           all notes off; SAVE: the settings now (else two seconds after a change)
 *   SMK-37        pads (channel 10, notes 36..51): mutes, everyone back, scene, PLAY, the box's first five hits, a wind gust,
 *                 bells (see pad()); faders CC 70..73: strings, bass, drums, box levels; mod wheel CC 1: the master
 *                 low-pass; pedal CC 64: the lead's sustain
 *   keys          the lead (OCT- / OCT+ its octave); MIDI notes the lead too, PC its sound (0 SOFT LEAD, 1 LASER HARP, 2..11 the 2600's, then the Synthex's), CC 74 its cutoff
 *
 * Contexts: render() is the audio interrupt and only calls md_session_render. Everything else is the main loop,
 * which talks to the session through its bounded command ring, staged edits and published snapshot. */
#include "hb_module.h"
#include "core/session.h"
#include "module/band.h"
#include "instruments/arp.h"
#include "instruments/eminent.h"
#include "instruments/minipops.h"
#include "instruments/electronic_fx.h"
#include "instruments/synthex.h"
#include "instruments/ensemble.h"
#include "effects/effects.h"
#include "arp_presets.h"   /* fm1-dock: the engine of this tree (take.py) */
#include "mp_engine.h"   /* fm1-dock: the engine of this tree (take.py) */

#define SET_MAGIC 0x31305252u                   /* "RR01": ORRERY's record (the hall and the phrase's style in it) */
#define SETUPS_MAGIC 0x31425252u                /* "RRB1": the setups' bank (bank 1): all of them, with their sequences
                                                 * (its size is checked too: a bank of another count is the factory's) */
#define C_ACC HB_RGB(255, 150, 60)              /* orange, as the decade's panels */
#define C_CREAM HB_RGB(235, 205, 130)
#define C_BLUE HB_RGB(90, 120, 170)
#define C_WHITE 0xFFFFu
#define C_GREY HB_RGB(110, 110, 110)
#define C_DIM HB_RGB(50, 46, 40)
#define C_BLACK 0u
#define T_BASS MD_BAND_BASS
#define T_LEAD MD_BAND_LEAD
#define T_STRINGS MD_BAND_CHORDS
#define T_DRUMS MD_BAND_DRUMS
#define T_FX MD_BAND_FX

enum { PG_STRINGS, PG_LEAD, PG_BASS, PG_DRUMS, PG_CHORDS, PG_MIX, PG_ECHO, PG_PARTS, PG_FXSEQ, N_PAGES };
enum { CH_NONE, CH_MIN, CH_MAJ, CH_MIN7, CH_MAJ7, CH_SUS2, CH_SUS4, CH_5TH, N_KINDS };
static const char *const PAGE_NAME[N_PAGES] = {"STRINGS", "LEAD", "BASS", "DRUMS", "CHORDS", "MIX", "ECHO", "PARTS", "FX SEQ"};
static const char *const PART_NAME[5] = {"BASS", "LEAD", "STRINGS", "DRUMS", "FX BOX"};   /* by track */
static const char *const KIND_NAME[N_KINDS] = {"-", "MIN", "MAJ", "MIN7", "MAJ7", "SUS2", "SUS4", "5TH"};
static const uint8_t KIND_IV[N_KINDS][4] = {{0, 0, 0, 0}, {0, 3, 7, 12}, {0, 4, 7, 12}, {0, 3, 7, 10}, {0, 4, 7, 11}, {0, 2, 7, 12}, {0, 5, 7, 12}, {0, 7, 12, 19}};
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
#define N_RUNS 7u
#define RUN_MAX 24u
/* AMBIENT's lead phrases (band.c: lead_phrase), and the halls: the reverb's kind, size and damping as one choice */
static const char *const PHRASE_NAME[MD_BAND_PHRASES + 1u] = {"NONE", "HARP", "LONG TONES", "BELLS", "CALLS"};
#define N_HALLS 8u
#define HALL_BAND 2u
#define HALL_AMBIENT 3u
static const struct { const char *name; uint8_t kind, size, damping; } HALLS[N_HALLS] = {
    {"ROOM", MD_REVERB_ROOM, 60, 45}, {"PLATE", MD_REVERB_PLATE, 80, 30}, {"HALL", MD_REVERB_HALL, 70, 50},
    {"BIG HALL", MD_REVERB_HALL, 90, 35}, {"DARK HALL", MD_REVERB_HALL, 100, 65}, {"CATHEDRAL", MD_REVERB_CATHEDRAL, 45, 40},
    {"VAST", MD_REVERB_CATHEDRAL, 70, 45}, {"ENDLESS", MD_REVERB_CATHEDRAL, 100, 35}};
static const char *const RUN_NAME[N_RUNS] = {"OFF", "PENTA FAST", "PENTA 1/16", "SCALE FAST", "SCALE 1/16", "CHORD FAST", "CHORD 1/16"};
static const char *const EC_DIV_NAME[7] = {"OFF", "1/16", "1/8", "1/8.", "1/4", "1/4.", "1/2"};
static const char *const REG_NAME[EM_N_REG] = {"STRINGS", "CELLO", "VIOLIN", "FULL"};
/* the strings' voices (str_reg): the Eminent's four registrations, then Caravel's fourteen ENSEMBLE pads (its alternate) */
#define STR_VOICES (EM_N_REG + ENS_N_PRESETS)
#define STR_ENS (set.str_reg >= EM_N_REG)
#define STR_PAD ((set.str_reg - EM_N_REG) % ENS_N_PRESETS)
static const int8_t WHITE[27] = { 0, -1, 1, -1, 2, -1, 3, 4, -1, 5, -1, 6, 7, -1, 8, -1, 9,
                                  -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

typedef struct { uint8_t root, kind; } chord_t;
#include "module/setups.h"                      /* the factory setups and effect patterns: where a style set is written */
typedef struct {                                /* the settings: the module's record (7S01's layout, then the sequence) */
    uint32_t magic;
    uint8_t page, oct, latch, cursor;
    uint8_t lead_patch, bass_patch, bass_seq;
    uint8_t str_reg, str_att, str_rel, str_ens, str_ph, str_col, str_oct;
    int8_t lead_cut, bass_cut;
    uint8_t lead_res, lead_glide, bass_sweep;   /* (255: the patch's / the sequence's own) */
    uint16_t tempo, rh_keys;
    uint8_t rh_section, rh_xmask, rh_extra, rh_balance;
    uint8_t lvl[4];                             /* strings, lead, bass, drums: percent */
    uint8_t reverb, ec_div, ec_fb, ec_tone, ec_str, ec_lead;
    chord_t chord[ARP_CHAIN];
    uint8_t scene;                              /* MD_BAND_SCENE_AMBIENT (the default) or _BAND */
    uint8_t amb_arp;                            /* AMBIENT: the lead's background run on */
    uint8_t lvl_wind, lvl_fx, lead_rev;         /* PARTS page: the wind's level, the effects box's, the lead's reverb */
    uint8_t wind_col;                           /* PARTS page: the wind's colour, low to high */
    uint8_t lead_soft;                          /* the lead: 0 the 2600's lead_patch, 1 SOFT LEAD, 2 the Synthex's lead_sx */
    uint8_t setup;                              /* which of the setups this is */        
    uint8_t hall;                               /* ECHO + SELECT: the reverb's hall (HALLS) */
    uint8_t motion;                             /* MIX + SELECT: the slow filter sweeps' depth, percent */
    uint8_t lead_sx;                            /* the Synthex patch (0..79) when lead_soft is 2 */
    uint8_t run;                                /* LEAD + SELECT: a key plays a rising run (0 off; in the tail padding) */
    int8_t fx_speed;                            /* PARTS + SELECT: the gestures' speed, -4 slow .. +4 fast (0 as made) */
    uint8_t phrase;                             /* AMBIENT: the lead's own phrase, 0 none or its style (PARTS + SEL) */
    md_band_fx_event_t fx[MD_BAND_FX_EVENTS];   /* FX SEQ: the effect sequence, what the effects box plays by itself */
} set_t;
_Static_assert(sizeof(set_t) <= 240u, "the settings record");
_Static_assert(sizeof(set_t) == 152u, "a change of layout needs new magics: saved records and banks are read as they are");
_Static_assert(4u + N_SETUPS * sizeof(set_t) <= 8176u, "the setups' bank");

static const hb_host_t *H;
static set_t set;
static uint32_t dirty, t_save, drawn = ~0u, drawn_page = ~0u, drawn_live = ~0u, room_forced;

/* ---- the session and what it plays: caller-owned instances, all in the module's RAM */
static md_session_t S;
static md_arp_state_t bass_state, lead_state;
static md_eminent_state_t strings_state;
static md_minipops_state_t drums_state;
static md_instrument_t inst[MD_TRACKS];
static md_fx_echo_t echo_state;
static md_fx_reverb_t reverb_state;
static md_fx_filter_t master_state, warm_state;
static md_effect_t echo_fx, reverb_fx, master_fx, warm_fx;
static md_electronic_fx_state_t fx_state;       /* the effects box (track 4): run, droplets, chirps, the wind */
static md_synthex_state_t sx_state;             /* the Elka Synthex: the lead track's alternate (the laser harp) */
static md_instrument_t sx;
static md_ensemble_state_t ens_state;           /* Caravel's ENSEMBLE: the strings track's alternate (its fourteen pads) */
static md_instrument_t ens;
static int32_t wind_sent = -1;                  /* the wind level the box last took */
static uint8_t wind_gust, gust_key;             /* a pad or PARTS's F# held: the wind at full (not saved) */
static uint8_t muted;                           /* the pads' mutes, a bit a track (not saved) */
/* the setups: complete snapshots (scene, chords, tempo, rhythm, sounds, levels, echo, the box), the one playing
 * is set; switching keeps each one's edits in RAM, SAVE writes them all to bank 1 */
static struct { uint32_t magic; set_t s[N_SETUPS]; } store;
static uint8_t restart;                         /* a setup changed while playing: start again once it is sent */
static uint8_t fx_cur;                          /* FX SEQ: the event under the cursor (not saved) */
static md_band_fx_event_t fx_own[MD_BAND_FX_EVENTS];   /* the player's own sequence while a pattern is tried (not saved) */
static uint8_t fx_own_kept;                     /* (of the setup that plays: another setup forgets it) */
static int32_t motion_bass = -1, motion_str = -1;   /* the sweeps' values last sent (-1: none) */
static uint32_t motion_t0;                      /* the main loop's ms when the band started */
#define SETUP_NAME (SETUPS[set.setup % N_SETUPS].name)
#define AMBIENT (set.scene == MD_BAND_SCENE_AMBIENT)

static md_seq_bank_t first_bank;
static md_band_chain_t chain;
static md_band_report_t report;
static md_session_snapshot_t snap;              /* the last consistent snapshot */
static uint32_t running, cur_link = 0xffu;

/* What the session still has to be told: a group is resent until all of its commands fit the ring. */
enum { G_FX = 1u << 12, G_SCENE = 1u << 10, G_BANK = 1u << 0, G_LEAD_SOUND = 1u << 1, G_LEAD = 1u << 2, G_BASS_SOUND = 1u << 3, G_BASS = 1u << 4,
       G_STRINGS = 1u << 5, G_DRUMS = 1u << 6, G_MIX = 1u << 7, G_ECHO = 1u << 8, G_TEMPO = 1u << 9 };
static uint32_t todo;

/* live notes: the token each key or MIDI note started, so its release finds it after octave changes */
static md_note_token_t key_token[27], midi_token[16][128], chord_token[4];
static uint8_t key_track[27];                   /* the part each held key started, for its key-up */
static uint8_t solo_track = 0xffu;              /* SEL: the part heard alone, 0xff none (not saved) */
static int32_t sweep_cut = 990, sweep_res;      /* FX held + KNOB1/2: the master filter (not saved) */
static uint32_t fx_held, fx_used;
static uint8_t n_chord_on;

static char *put_u(char *p, uint32_t v)
{
    if (v >= 100u) *p++ = (char)('0' + v / 100u % 10u);
    if (v >= 10u) *p++ = (char)('0' + v / 10u % 10u);
    *p++ = (char)('0' + v % 10u);
    *p = 0;
    return p;
}
static char *put_s(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *put_i(char *p, int32_t v) { if (v < 0) { *p++ = '-'; v = -v; } else if (v > 0) *p++ = '+'; return put_u(p, (uint32_t)v); }
static uint8_t clamp8(int32_t v, int32_t lo, int32_t hi) { return (uint8_t)(v < lo ? lo : v > hi ? hi : v); }
static int32_t q15(uint32_t pct) { return pct >= 100u ? 32767 : (int32_t)(pct * 32767u / 100u); }

/* ---- the parts as set: each a group of session commands */
static void touch(void) { dirty = 1; t_save = H->ms(); }
static int param(uint8_t track, uint16_t target, int32_t value) { return md_session_param(&S, track, target, value); }
static int command(uint8_t kind, uint8_t track, uint8_t arg, uint32_t value)
{
    md_command_t c = {.kind = kind, .track = track, .arg = arg, .value = value};
    return md_session_submit(&S, &c);
}
static void band_settings(md_band_settings_t *b)
{
    uint32_t k;
    b->bass_seq = set.bass_seq;
    b->chord_octave = set.str_oct;
    for (k = 0; k < MD_BAND_LINKS; k++) { b->chord[k].root = set.chord[k].root; b->chord[k].kind = set.chord[k].kind; }
    b->rhythm_keys = set.rh_keys;
    b->rhythm_section = set.rh_section;
    b->extras = set.rh_xmask;
    b->scene = set.scene;
    b->amb_arp = set.amb_arp;
    b->phrase = set.phrase;
    b->phrase_shift = set.lead_soft == 2u && set.lead_sx == 28u ? -12 : 0;   /* the Synthex's Chimes speak three octaves up */
    for (k = 0; k < MD_BAND_FX_EVENTS; k++) b->fx[k] = set.fx[k];
}
static int send_bank(void)
{
    md_band_settings_t b;
    md_band_chain_t c;
    md_seq_bank_t *staging = md_session_edit_bank(&S);
    if (!staging) return 1;                     /* the last swap is not acknowledged yet */
    band_settings(&b);
    if (md_band_build(&b, staging, &c, &report)) return 1;
    if (md_session_publish_bank(&S)) return 1;
    chain = c;
    return 0;
}
static int send_sound(uint8_t track, uint32_t patch)
{
    const md_preset_t *p = &MD_ARP2600.presets[patch % MD_ARP2600.preset_count];
    if (md_session_pending(&S) & 2u) return 1;  /* one sound in flight */
    return md_session_stage_sound(&S, track, MD_ARP_SETTINGS_VERSION, p->settings, p->settings_bytes) != MD_OK;
}
static int send_list(uint8_t track, const int32_t (*list)[2], uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) if (param(track, (uint16_t)list[i][0], list[i][1])) return 1;
    return 0;
}
/* The lead's sounds: SOFT LEAD (the 2600's ARP LEAD made soft), LASER HARP (the Synthex's own "Ring mod."), the
 * 2600's ten as they are, then the Synthex's other 79 factory patches. A Synthex sound binds the lead track to the
 * Synthex (its alternate instrument); the others bind it back to the 2600. */
#define SOFT_BASE 3u                             /* ARP LEAD under SOFT LEAD */
#define LEAD_BASE (set.lead_soft == 1u ? SOFT_BASE : set.lead_patch % ARP_N_PATCHES)
#define SX_FIRST (2u + ARP_N_PATCHES)            /* the Synthex patches after the 2600's */
#define N_LEAD_SOUNDS (SX_FIRST + MD_SYNTHEX_PATCHES - 1u)
/* the Synthex's own level: its patches are loud, and not equally: the struck and the soft ones (a held G4 measured,
 * tools scratch sx.c) come up to where the brass and the laser harp sit at 40 */
static int32_t sx_level(uint32_t patch)
{
    switch (patch) {
    case 28: return 55;                          /* Chimes */
    case 29: return 85;                          /* Vibes */
    case 60: case 73: case 74: case 44: return 80;   /* Plink, Marimba, Harp, Echo: plucked, they fall away */
    default: return 40;
    }
}
static uint32_t sx_ring;                         /* "Ring mod.": the laser harp */
static uint32_t lead_pos(void)
{
    if (set.lead_soft == 1u) return 0;
    if (set.lead_soft == 2u)
        return set.lead_sx == sx_ring ? 1u : SX_FIRST + (set.lead_sx < sx_ring ? set.lead_sx : set.lead_sx - 1u);
    return 2u + set.lead_patch % ARP_N_PATCHES;
}
static void lead_set(uint32_t p)                /* 0 SOFT LEAD, 1 LASER HARP, 2..11 the 2600's, then the Synthex's */
{
    if (p >= N_LEAD_SOUNDS) p = N_LEAD_SOUNDS - 1u;
    if (p == 0) { set.lead_soft = 1; set.lead_res = 15; set.lead_glide = 20; set.lead_cut = 5; }
    else if (p == 1) { set.lead_soft = 2; set.lead_sx = (uint8_t)sx_ring; }
    else if (p < SX_FIRST) { set.lead_soft = 0; set.lead_patch = (uint8_t)(p - 2u); set.lead_res = set.lead_glide = 255; }
    else { set.lead_soft = 2; p -= SX_FIRST; set.lead_sx = (uint8_t)(p < sx_ring ? p : p + 1u); }
}
static void lead_select(int32_t pos)
{
    lead_set((uint32_t)(pos < 0 ? 0 : pos));
    todo |= G_SCENE | G_LEAD_SOUND;              /* the binding first */
}
static void lead_name(char *out)                 /* the sound's name, upper case (the Synthex's are mixed) */
{
    const char *n = set.lead_soft == 1u ? "SOFT LEAD" : set.lead_soft == 0u ? ARP_PATCHES[set.lead_patch % ARP_N_PATCHES].name :
                    set.lead_sx == sx_ring ? "LASER HARP" : md_synthex_name(set.lead_sx);
    uint32_t i;
    for (i = 0; n[i] && i < 15u; i++) out[i] = (char)(n[i] >= 'a' && n[i] <= 'z' ? n[i] - 32 : n[i]);
    out[i] = 0;
}
static int send_lead(void)
{
    /* AMBIENT: the soft lead you play over the bed: two triangle oscillators a few cents apart, a gentle swell
     * and long release, a slow vibrato, a little glide (set below), a mellow filter */
    static const int32_t SOFT[][2] = {
        {MD_ARP_OSC1_WAVE, 2}, {MD_ARP_OSC1_LEVEL, 90}, {MD_ARP_OSC2_WAVE, 2}, {MD_ARP_OSC2_LEVEL, 60},
        {MD_ARP_OSC2_DETUNE, 56}, {MD_ARP_NOISE_LEVEL, 0}, {MD_ARP_SUB_LEVEL, 0}, {MD_ARP_RING_LEVEL, 0},
        {MD_ARP_LFO_WAVE, 0}, {MD_ARP_LFO_RATE, 30}, {MD_ARP_LFO_PITCH, 2}, {MD_ARP_LFO_FILTER, 0},
        {MD_ARP_SAMPLE_HOLD_FILTER, 0}, {MD_ARP_AMP_ATTACK, 55}, {MD_ARP_AMP_DECAY, 60}, {MD_ARP_AMP_SUSTAIN, 85},
        {MD_ARP_AMP_RELEASE, 75}, {MD_ARP_FILTER_ENV, 10}, {MD_ARP_DRIVE, 0}, {MD_ARP_KEYBOARD_TRACK, 60}};
    /* the 2600's sustained patches are 4 to 9 dB over SOFT LEAD at the same fader (ARP LEAD the loudest: it clipped
     * the band by itself): each patch's level puts a held note where SOFT LEAD's is, so the LEAD fader means the
     * same whatever the sound (measured with tools/render_setups.c, a key held four seconds; the plucks as they are) */
    static const uint8_t LEAD_TRIM[ARP_N_PATCHES] = {62, 100, 100, 41, 68, 72, 100, 100, 100, 69};
    const arp_patch_t *q = &ARP_PATCHES[LEAD_BASE];
    if (set.lead_soft == 2u)                     /* the Synthex: its patch and level (the 2600's knobs rest) */
        return param(T_LEAD, MD_SYNTHEX_PATCH, set.lead_sx) || param(T_LEAD, MD_SYNTHEX_LEVEL, sx_level(set.lead_sx));
    if (set.lead_soft == 1u && send_list(T_LEAD, SOFT, sizeof SOFT / sizeof SOFT[0])) return 1;
    return param(T_LEAD, MD_ARP_CUTOFF, clamp8((int32_t)q->cutoff + set.lead_cut, 0, 100)) ||
           param(T_LEAD, MD_ARP_RESONANCE, set.lead_res == 255u ? q->reso : set.lead_res) ||
           param(T_LEAD, MD_ARP_GLIDE, set.lead_glide == 255u ? q->glide : set.lead_glide) ||
           param(T_LEAD, MD_ARP_LEVEL, set.lead_soft == 1u ? 100 : LEAD_TRIM[LEAD_BASE]);
}
static int send_bass(void)
{
    motion_bass = -1;
    const arp_patch_t *q = &ARP_PATCHES[set.bass_patch % ARP_N_PATCHES];
    /* AMBIENT's bass: the preset softened: low cutoff, a little glide, a gentle attack and long release */
    static const int32_t SOFT[][2] = {{MD_ARP_RESONANCE, 20}, {MD_ARP_FILTER_ENV, 20}, {MD_ARP_AMP_ATTACK, 35},
        {MD_ARP_AMP_SUSTAIN, 90}, {MD_ARP_AMP_RELEASE, 60}, {MD_ARP_GLIDE, 15}, {MD_ARP_LEVEL, 100}};
    if (AMBIENT && (send_list(T_BASS, SOFT, sizeof SOFT / sizeof SOFT[0]) ||
                    param(T_BASS, MD_ARP_CUTOFF, clamp8(34 + set.bass_cut, 0, 100)))) return 1;
    if (AMBIENT) return 0;
    return param(T_BASS, MD_ARP_CUTOFF, clamp8((int32_t)q->cutoff + set.bass_cut, 0, 100)) ||
           param(T_BASS, MD_ARP_LEVEL, 100);
}
/* the effects box's base sound (the run): a glassy ping with a little drop into each note; droplets and chirps lock
 * their own recipe on their steps */
static const int32_t BOX[][2] = {{MD_FX_RECIPE, MD_FX_PING}, {MD_FX_DECAY_MS, 800}, {MD_FX_SWEEP_SEMITONES, 7},
    {MD_FX_SWEEP_MS, 40}, {MD_FX_TONE, 48}, {MD_FX_RESONANCE, 60}, {MD_FX_MOD_DEPTH, 0}, {MD_FX_LEVEL, 95},
    {MD_FX_SPREAD, 70}};                         /* (each sound a place of its own, the long ones travelling) */
static int send_fx(void)                         /* the effects box's base sound and the wind's colour */
{
    wind_sent = -1;
    return send_list(T_FX, BOX, sizeof BOX / sizeof BOX[0]) || param(T_FX, MD_FX_WIND_COLOUR, set.wind_col);
}
static uint8_t pct(uint32_t v127) { return (uint8_t)((v127 * 100u + 63u) / 127u); }
static int32_t of_pct(uint32_t v) { return (int32_t)((v * 127u + 50u) / 100u); }
static void str_voice(uint32_t r)                /* a voice from the list; an ENSEMBLE pad brings its own knobs */
{
    set.str_reg = (uint8_t)(r % STR_VOICES);
    if (STR_ENS) {
        const ens_preset_t *q = &ENS_PRESETS[STR_PAD];
        set.str_att = pct(q->env[ENS_ATK]); set.str_rel = pct(q->env[ENS_REL]);
        set.str_ens = pct(q->e[ENS_DEPTH]); set.str_ph = pct(q->e[ENS_TONE]);
    }
}
static int send_strings(void)
{
    /* the pads differ by 9.5 dB as Caravel made them (ensemble_test prints each: STRING MACHINE the loudest, the two
     * choirs the quietest): each pad's level brings a held chord to the same RMS, so the STRINGS fader means the same
     * whatever the voice (percent, 200 twice) */
    static const uint8_t PAD_TRIM[ENS_N_PRESETS] = {83, 200, 114, 78, 78, 67, 194, 79, 136, 166, 100, 96, 77, 119};
    if (STR_ENS)                                 /* the pad, then the knobs over it: attack, release, ensemble, tone */
        return param(T_STRINGS, MD_ENSEMBLE_PRESET, (int32_t)STR_PAD) || param(T_STRINGS, MD_ENSEMBLE_ATTACK, of_pct(set.str_att)) ||
               param(T_STRINGS, MD_ENSEMBLE_RELEASE, of_pct(set.str_rel)) || param(T_STRINGS, MD_ENSEMBLE_ENS, of_pct(set.str_ens)) ||
               param(T_STRINGS, MD_ENSEMBLE_TONE, of_pct(set.str_ph)) || param(T_STRINGS, MD_ENSEMBLE_LEVEL, PAD_TRIM[STR_PAD]);
    return param(T_STRINGS, MD_EMINENT_REGISTRATION, set.str_reg) || param(T_STRINGS, MD_EMINENT_ATTACK, set.str_att) ||
           param(T_STRINGS, MD_EMINENT_RELEASE, set.str_rel) || param(T_STRINGS, MD_EMINENT_ENSEMBLE, set.str_ens) ||
           param(T_STRINGS, MD_EMINENT_PHASER, set.str_ph) || param(T_STRINGS, MD_EMINENT_COLOR, set.str_col != 0) ||
           param(T_STRINGS, MD_EMINENT_LEVEL, 100);  /* (65 while the phaser's feedback only made it louder: em_engine.c) */
}
static int send_drums(void)
{
    /* (its level is the mix's; with that at nothing the machine strikes nothing: no work for no sound) */
    return param(T_DRUMS, MD_MINIPOPS_LEVEL, set.lvl[3] ? 100 : 0) || param(T_DRUMS, MD_MINIPOPS_BALANCE, set.rh_balance) ||
           param(T_DRUMS, MD_MINIPOPS_QUIJADA, set.rh_extra) || param(T_DRUMS, MD_MINIPOPS_GUIRO, set.rh_extra) ||
           param(T_DRUMS, MD_MINIPOPS_TAMBOURINE, set.rh_extra);
}
static int send_mix(void)
{
    static const uint8_t TRACK_OF[4] = {T_STRINGS, T_LEAD, T_BASS, T_DRUMS};   /* set.lvl's order */
    uint32_t k;
    /* AMBIENT: the reverb send per part: strings at the MIX page's amount, the lead at the PARTS page's,
     * the bass and drums light, the effects box deep */
    static const uint8_t AMBIENT_REVERB[4] = {0, 0, 30, 40};   /* strings, lead (set), bass, drums */
    for (k = 0; k < 4u; k++) {
        uint32_t rev = k == 1 ? set.lead_rev : !AMBIENT || k == 0 ? set.reverb : AMBIENT_REVERB[k];
        if (param(TRACK_OF[k], MD_TARGET_MIXER | MD_MIX_LEVEL, q15(set.lvl[k]))) return 1;
        if (param(TRACK_OF[k], MD_TARGET_MIXER | MD_MIX_REVERB_SEND, q15(rev))) return 1;
    }
    return param(T_DRUMS, MD_MINIPOPS_LEVEL, set.lvl[3] ? 100 : 0) ||
           param(T_FX, MD_TARGET_MIXER | MD_MIX_LEVEL, q15(set.lvl_fx)) ||
           param(T_FX, MD_TARGET_MIXER | MD_MIX_REVERB_SEND, q15(65));
}
static int send_echo(void)
{
    uint32_t on = set.ec_div && set.ec_div < 7u;
    return param(T_STRINGS, MD_TARGET_MIXER | MD_MIX_ECHO_SEND, on ? q15(set.ec_str) : 0) ||
           param(T_LEAD, MD_TARGET_MIXER | MD_MIX_ECHO_SEND, on ? q15(set.ec_lead) : 0) ||
           param(T_FX, MD_TARGET_MIXER | MD_MIX_ECHO_SEND, on ? q15(78) : 0) ||     /* the effects box, deep */
           (on && param(T_STRINGS, MD_TARGET_ECHO | MD_ECHO_SYNC, set.ec_div)) ||
           param(T_STRINGS, MD_TARGET_ECHO | MD_ECHO_FEEDBACK, set.ec_fb) ||
           param(T_STRINGS, MD_TARGET_ECHO | MD_ECHO_TONE, set.ec_tone);
}
static int send_scene(void)
{
    /* the strings' warming filter, the reverb's character */
    return command(MD_CMD_BIND, T_LEAD, 0, set.lead_soft == 2u ? 1u : 0u) ||   /* the Synthex or the 2600 */
           command(MD_CMD_BIND, T_STRINGS, 0, STR_ENS ? 1u : 0u) ||           /* an ENSEMBLE pad or the Eminent */
           command(MD_CMD_BYPASS, T_STRINGS, 0, AMBIENT || set.motion ? 0u : 1u) ||
           param(T_STRINGS, MD_TARGET_REVERB | MD_REVERB_ALGORITHM, HALLS[set.hall % N_HALLS].kind) ||
           param(T_STRINGS, MD_TARGET_REVERB | MD_REVERB_SIZE, HALLS[set.hall % N_HALLS].size) ||
           param(T_STRINGS, MD_TARGET_REVERB | MD_REVERB_DAMPING, HALLS[set.hall % N_HALLS].damping);
}
static void pump(void)                          /* main loop: send what changed, in order */
{
    if ((todo & G_SCENE) && !send_scene()) todo &= ~G_SCENE;
    if (todo & G_SCENE) return;                  /* the binding first: later parameters are for its instrument */
    if ((todo & G_TEMPO) && !command(MD_CMD_TEMPO, 0, 0, (uint32_t)set.tempo * 256u)) todo &= ~G_TEMPO;
    if ((todo & G_BANK) && !send_bank()) todo &= ~G_BANK;
    if ((todo & G_LEAD_SOUND) && (set.lead_soft == 2u || !send_sound(T_LEAD, LEAD_BASE)))
        todo = (todo & ~G_LEAD_SOUND) | G_LEAD;     /* (the Synthex takes its patch as a parameter) */
    if ((todo & G_BASS_SOUND) && !(todo & G_LEAD_SOUND) && !send_sound(T_BASS, set.bass_patch)) todo = (todo & ~G_BASS_SOUND) | G_BASS;
    if ((todo & G_LEAD) && !(todo & G_LEAD_SOUND) && !send_lead()) todo &= ~G_LEAD;
    if ((todo & G_BASS) && !(todo & G_BASS_SOUND) && !send_bass()) todo &= ~G_BASS;
    if ((todo & G_STRINGS) && !send_strings()) todo &= ~G_STRINGS;
    if ((todo & G_DRUMS) && !send_drums()) todo &= ~G_DRUMS;
    if ((todo & G_FX) && !send_fx()) todo &= ~G_FX;
    if ((todo & G_MIX) && !send_mix()) todo &= ~G_MIX;
    if ((todo & G_ECHO) && !send_echo()) todo &= ~G_ECHO;
}
static void band(uint32_t on);
static uint32_t playing_chord(void);
/* a scene's starting point; the knobs edit from there */
static void scene_defaults(void)
{
    set.str_reg = EM_STRINGS; set.str_ens = 100; set.str_col = 1; set.str_oct = 1;
    set.lead_patch = 3; set.lead_cut = 0; set.lead_res = set.lead_glide = 255; set.bass_patch = 7; set.bass_cut = 0;
    set.rh_keys = 0; set.rh_balance = 50; set.rh_extra = 70; set.ec_div = 3;
    set.hall = AMBIENT ? HALL_AMBIENT : HALL_BAND;
    if (AMBIENT) {
        /* Oxygène-like: 75 BPM, slow strings, a soft bass (or the wind), gliding lead with its background run,
         * droplets and rising chirps, a long echo and a big hall */
        set.tempo = 75; set.str_att = 85; set.str_rel = 90; set.str_ph = 8;
        set.lead_soft = 1; set.lead_glide = 20; set.lead_res = 15; set.lead_cut = 5; set.bass_patch = 0; set.amb_arp = 1;
        set.page = PG_LEAD;                      /* the keys play the soft lead over the bed */
        set.rh_keys = 1u << 5; set.rh_section = 0; set.rh_balance = 30;   /* Bossa Nova, soft and dark */
        /* the bed under the player: strings, bass, soft drums, wind, droplets and the run; the lead in front, wet */
        set.lvl[0] = 45; set.lvl[1] = 100; set.lvl[2] = 40; set.lvl[3] = 30;
        set.lvl_wind = 50; set.wind_col = 45; set.lvl_fx = 60; set.lead_rev = 50;
        set.reverb = 60; set.ec_fb = 62; set.ec_tone = 32; set.ec_str = 22; set.ec_lead = 45;
    } else {
        set.tempo = ARP_SEQS[set.bass_seq % ARP_N_SEQS].tempo; set.str_att = 75; set.str_rel = 80; set.str_ph = 15;
        set.lvl[0] = 60; set.lvl[1] = 100; set.lvl[2] = 55; set.lvl[3] = 50;
        set.reverb = 50; set.ec_fb = 45; set.ec_tone = 40; set.ec_str = 30; set.ec_lead = 35;
        set.lvl_wind = 0; set.wind_col = 45; set.lvl_fx = 0; set.lead_rev = set.reverb; set.lead_soft = 0;
    }
}
/* the settings a fresh record starts from, before a scene's defaults */
static void base_defaults(void)
{
    uint32_t k;
    for (k = 0; k < sizeof set; k++) ((uint8_t *)&set)[k] = 0;
    set.magic = SET_MAGIC;
    set.oct = 3; set.page = PG_STRINGS;          /* the keys from F4 (OCT- three times: F1) */
    set.lead_patch = 3; set.bass_patch = 7; set.bass_seq = 1;
    set.str_reg = EM_STRINGS; set.str_att = 75; set.str_rel = 80; set.str_ens = 100; set.str_ph = 15; set.str_col = 1; set.str_oct = 1;
    set.lead_res = set.lead_glide = set.bass_sweep = 255;
    set.rh_balance = 50; set.rh_extra = 70;
    set.reverb = 50; set.ec_div = 3; set.ec_fb = 45; set.ec_tone = 40; set.ec_str = 30; set.ec_lead = 35;
}
/* A factory setup from module/setups.h's table (the scene's defaults under it for what the table does not name). */
static void factory(uint32_t i)
{
    const setup_t *f = &SETUPS[i % N_SETUPS];
    uint32_t k;
    base_defaults();
    set.scene = f->scene;
    set.bass_seq = (uint8_t)(f->sequence % ARP_N_SEQS);
    scene_defaults();
    set.setup = (uint8_t)(i % N_SETUPS);
    set.tempo = f->tempo;
    for (k = 0; k < ARP_CHAIN; k++) set.chord[k] = f->chord[k];
    str_voice(f->strings);                       /* (a pad brings its own knobs: the table's over them) */
    set.str_oct = f->octave;
    if (f->attack != PAD_OWN) set.str_att = f->attack;
    if (f->release != PAD_OWN) set.str_rel = f->release;
    if (f->depth != PAD_OWN) set.str_ens = f->depth;
    if (f->phaser != PAD_OWN) set.str_ph = f->phaser;
    lead_set(f->lead);
    set.amb_arp = f->run != 0; set.phrase = f->phrase <= MD_BAND_PHRASES ? f->phrase : 0;
    set.hall = (uint8_t)(f->hall % N_HALLS);
    set.bass_patch = f->bass; set.bass_cut = f->bass_cut;
    set.rh_keys = f->rhythm ? (uint16_t)(1u << ((f->rhythm - 1u) % 10u)) : 0u;
    set.rh_section = f->rhythm > 10u; set.rh_xmask = f->extras; set.rh_extra = f->extras_level; set.rh_balance = f->balance;
    for (k = 0; k < MD_BAND_FX_EVENTS; k++) set.fx[k] = FX_PATTERNS[f->fx % N_FX_PATTERNS].ev[k];
    set.lvl[0] = f->strings_level; set.lvl[1] = f->lead_level; set.lvl[2] = f->bass_level; set.lvl[3] = f->drums_level;
    set.lvl_fx = f->box_level; set.lvl_wind = f->wind_level; set.wind_col = f->wind_colour;
    set.reverb = f->reverb; set.lead_rev = f->lead_reverb;
    set.ec_div = f->echo; set.ec_fb = f->echo_feedback; set.ec_tone = f->echo_tone; set.ec_str = f->echo_strings; set.ec_lead = f->echo_lead;
    set.motion = f->motion;
}
static void sanitize(void)                       /* a record or a bank from flash: in range before it is used */
{
    if (set.scene > MD_BAND_SCENE_AMBIENT) set.scene = MD_BAND_SCENE_AMBIENT;
    if (set.amb_arp > 1u) set.amb_arp = 1;
    if (set.lead_soft > 2u) set.lead_soft = 1;
    if (set.lead_sx >= MD_SYNTHEX_PATCHES) set.lead_sx = (uint8_t)sx_ring;
    if (set.hall >= N_HALLS) set.hall = AMBIENT ? HALL_AMBIENT : HALL_BAND;
    for (uint32_t k = 0; k < MD_BAND_FX_EVENTS; k++) {
        md_band_fx_event_t *e = &set.fx[k];
        if (e->sound >= N_FX_SOUNDS || e->at >= MD_BAND_FX_SPAN || e->degree >= MD_BAND_FX_DEGREES || e->chance > 100u ||
            e->how >= MD_BAND_FX_HOWS)
            *e = (md_band_fx_event_t){0, 0, 0, 0, 0};
    }
    if (set.run >= N_RUNS) set.run = 0;
    if (set.fx_speed < -4 || set.fx_speed > 4) set.fx_speed = 0;
    if (set.phrase > MD_BAND_PHRASES) set.phrase = 0;
    if (set.motion > 100u) set.motion = 50;
    if (set.lvl_wind > 100u) set.lvl_wind = 60;
    if (set.wind_col > 100u) set.wind_col = 45;
    if (set.lvl_fx > 100u) set.lvl_fx = 75;
    if (set.lead_rev > 100u) set.lead_rev = 60;
    if (set.setup >= N_SETUPS) set.setup = 0;
    if (set.page >= N_PAGES) set.page = PG_CHORDS;
    if (set.oct > 6u) set.oct = 3;
    if (set.tempo < 40u || set.tempo > 240u) set.tempo = 120;
    if (set.cursor >= ARP_CHAIN) set.cursor = 0;
    if (set.str_reg >= STR_VOICES) set.str_reg = 0;
    if (set.str_oct > 4u) set.str_oct = 1;
    if (set.ec_div > 6u) set.ec_div = 0;
    if (set.lead_patch >= ARP_N_PATCHES) set.lead_patch = 3;
    if (set.bass_patch >= ARP_N_PATCHES) set.bass_patch = 7;
    if (set.bass_seq >= ARP_N_SEQS) set.bass_seq = 1;
    for (uint32_t k = 0; k < ARP_CHAIN; k++) { set.chord[k].root %= 12u; if (set.chord[k].kind >= N_KINDS) set.chord[k].kind = CH_NONE; }
    set.rh_keys &= 0x3ffu;
    if (set.rh_keys & (set.rh_keys - 1u)) set.rh_keys &= (uint16_t)-set.rh_keys;   /* saved combinations: the lowest key */
}
static uint32_t banner_t, banner_on;            /* a setup's banner: its name over the page for two seconds */
static uint8_t banner_shown = 0xffu;
static void setup_to(uint32_t i)                 /* another setup: everything sent again; a running band restarts */
{
    uint8_t page = set.page, oct = set.oct;
    i %= N_SETUPS;
    if (i == set.setup) return;
    store.s[set.setup % N_SETUPS] = set;         /* its edits stay, until a power-off unless saved */
    if (running) { band(0); restart = 1; }
    set = store.s[i];
    set.setup = (uint8_t)i;
    sanitize();
    set.page = page; set.oct = oct; set.latch = 0;
    fx_own_kept = 0;
    banner_t = H->ms(); banner_on = 1;
    todo |= G_SCENE | G_BANK | G_TEMPO | G_LEAD_SOUND | G_BASS_SOUND | G_STRINGS | G_DRUMS | G_FX | G_MIX | G_ECHO;
    touch();
}
static void scene_to(uint32_t scene)             /* (the stress builds: a scene's defaults) */
{
    if (scene == set.scene) return;
    if (running) band(0);
    set.scene = (uint8_t)scene;
    scene_defaults();
    todo |= G_SCENE | G_BANK | G_TEMPO | G_LEAD_SOUND | G_BASS_SOUND | G_STRINGS | G_DRUMS | G_FX | G_MIX | G_ECHO;
    touch();
}
static void save_all(void)                       /* SAVE: this setup's record and all of them in bank 1 */
{
    set.magic = SET_MAGIC;
    H->save(&set, sizeof set);
    store.s[set.setup % N_SETUPS] = set;
    store.magic = SETUPS_MAGIC;
    if (HB_HOST_HAS(H, bank_save_n) && H->bank_save_n(1, &store, sizeof store) < 0) H->log("orrery: no bank for the setups");
    dirty = 0;
    H->log("orrery: saved");
}
/* the wind plays while AMBIENT runs: the box swells it in and out (about a second) and sweeps it itself */
static void wind_sweep(void)
{
    int32_t want = wind_gust ? 100 : running ? set.lvl_wind : 0;   /* (BAND setups: 0 unless they have wind) */
    if (want != wind_sent && !param(T_FX, MD_FX_WIND, want)) wind_sent = want;
}
/* Motion, as the records ride it by hand: while the band runs, the sequence's (or AMBIENT's bass's) cutoff opens
 * and closes once every eight bars, as far as the sequence's own sweep asks, and the strings' warming filter
 * breathes once every sixteen; the depth is set.motion. Main loop only, the rate from the tempo; a value is sent
 * only when it changes, and the bases go back when it stops. */
static int32_t wave(uint32_t t, uint32_t period)  /* a smooth wave, -1000..1000 (a parabola a half) */
{
    int32_t x = (int32_t)((t % period) * 2000u / period) - 1000;
    int32_t h = x < 0 ? -x : x;
    int32_t y = 4 * h * (1000 - h) / 1000;
    return x < 0 ? -y : y;
}
static void motion(void)
{
    const arp_patch_t *q = &ARP_PATCHES[set.bass_patch % ARP_N_PATCHES];
    int32_t base = AMBIENT ? 34 + set.bass_cut : (int32_t)q->cutoff + set.bass_cut, bass = base, str = 745;
    static uint32_t was;
    if (running && !was) motion_t0 = H->ms();
    was = running;
    if (running && set.motion) {
        uint32_t quarters_m = (uint32_t)((uint64_t)(H->ms() - motion_t0) * set.tempo / 60u);   /* quarters x 1000 */
        int32_t depth = AMBIENT ? 18 : (int32_t)ARP_SEQS[set.bass_seq % ARP_N_SEQS].sweep * 9 / 10;
        int32_t w = wave(quarters_m + 24000u, 32000u);   /* starting closed: a quarter of the way past the top */
        depth = depth * set.motion / 100;
        bass = base - depth / 3 + depth * (w + 1000) / 2000;
        str = 745 + 230 * (int32_t)set.motion / 100 * wave(quarters_m + 48000u, 64000u) / 1000;
    }
    bass = clamp8(bass, 0, 100);
    if (str > 990) str = 990;
    if (bass != motion_bass && !param(T_BASS, MD_ARP_CUTOFF, bass)) motion_bass = bass;
    if (str != motion_str && !param(T_STRINGS, MD_TARGET_INSERT_1 | MD_FILTER_CUTOFF, str)) motion_str = str;
}
static void reverb_room(void)                   /* outside render: the dock's room off, the module's reverb on */
{
#ifdef HB_HAS_FX_REVERB                         /* a host header with the processing section (fm1-dock 0.8 has no room) */
    if (HB_HOST_HAS(H, fx_reverb)) { H->fx_reverb(0, 0, 0, 0, 0); room_forced = 0; }
    else room_forced = 1;                       /* an old host: whatever room it has, we cannot turn it off */
#endif
}
static void band(uint32_t on)                   /* bass, chords and drums together: one clock, one start */
{
    if (on) {
        /* stopped launches select the first link at once; START plays all four tracks from bar 1 */
        if (command(MD_CMD_LAUNCH, T_BASS, 0, 0) || command(MD_CMD_LAUNCH, T_STRINGS, 0, 0) ||
            command(MD_CMD_LAUNCH, T_FX, 0, 0) || command(MD_CMD_LAUNCH, T_LEAD, 0, 0)) return;
        if (!command(MD_CMD_START, 0, 0, 0)) running = 1;
    } else if (!command(MD_CMD_STOP, 0, 0, 0)) running = 0;
}
static void schedule_links(void)                /* main loop: the next link as a next-bar launch, a bar early */
{
    static const uint8_t TRACKS[4] = {T_BASS, T_STRINGS, T_FX, T_LEAD};
    uint32_t k, bar = snap.position.quarter / 4u;
    if (!snap.running) return;
    /* each track from its own queued state, so a launch the ring refused for one is retried alone */
    for (k = 0; k < 4u; k++) {
        uint8_t t = TRACKS[k];
        if ((t == T_FX || t == T_LEAD) && !AMBIENT) continue;   /* BAND's box is one pattern, its lead empty */
        int next = md_band_next_link(&chain, bar, snap.pattern[t], snap.queued[t], snap.requested[t]);
        if (next >= 0) (void)command(MD_CMD_LAUNCH, t, (uint8_t)next, 0);
    }
}

/* ---- the runs: a key plays a quick sweep of tones through the chord's scale, one note a main-loop pass on H->ms()
 * (not the sequencer's grid; resynced when late, never a clump). Two at once: the lead's (LEAD + SELECT: a rise from
 * the key, two octaves; its top holds while the key does) and the effects box's (PARTS: the white keys, each a
 * gesture of the box's hits: up, down, up and back, a sparkle; to its end, whatever the key does). */
enum { SH_UP, SH_DOWN, SH_ARC, SH_SPARKLE };
#define GESTURE_DECAY_MS 500u                    /* a sequence's gesture: its struck sounds ring half a second at most
                                                 * (quick notes; four voices of the box, not all eight) */
typedef struct {
    uint8_t notes[RUN_MAX], n, i, vel, held, track, recipe;   /* T_LEAD: notes; T_FX: hits of the recipe */
    uint8_t sync;                               /* T_FX: its step is of the tempo (PARTS' speed leaves it alone) */
    md_source_id_t source;
    md_note_token_t token, *owner;              /* the lead's note sounding; the key's slot that started it */
    uint32_t next, step;
} run_t;
static run_t runs[3];                           /* 0 the lead's, 1 the effects box's, 2 the background scales */
static const uint16_t FX_PACE[9] = {400, 280, 200, 140, 100, 75, 55, 40, 30};   /* a step's percent, slow .. fast */
static uint32_t run_rng = 0x2545f491u;

/* the effects box's hit with the recipe's own macros (its factory preset: the drop's rise, the bubble's wobble, the
 * metal's ring) at a pitch; then the box's base sound and the pads' 72 again (one FIFO, in order: the hit snapshots
 * them, the background run's notes keep the base sound) */
static void fx_hit_decay(uint32_t recipe, uint32_t pitch, uint32_t vel, uint32_t decay_ms);
static void fx_hit(uint32_t recipe, uint32_t pitch, uint32_t vel) { fx_hit_decay(recipe, pitch, vel, 0); }
/* (decay_ms: a struck recipe's decay is that at most; 0, and the long recipes: their own) */
static void fx_hit_decay(uint32_t recipe, uint32_t pitch, uint32_t vel, uint32_t decay_ms)
{
    static const uint16_t ID[6] = {MD_FX_DECAY_MS, MD_FX_SWEEP_SEMITONES, MD_FX_SWEEP_MS, MD_FX_TONE, MD_FX_RESONANCE,
                                   MD_FX_MOD_DEPTH};
    md_command_t c = {.kind = MD_CMD_EVENT};
    int16_t own[6];                              /* decay ms, sweep st, sweep ms, tone, resonance, mod depth */
    uint32_t j;
    recipe %= MD_FX_RECIPES;
    md_electronic_fx_macros(recipe, own);
    if (decay_ms && recipe < MD_FX_WHISTLE && own[0] > (int16_t)decay_ms) own[0] = (int16_t)decay_ms;
    for (j = 0; j < 6u; j++) (void)param(T_FX, ID[j], own[j]);
    (void)param(T_FX, MD_FX_BASE_PITCH, (int32_t)(pitch > 127u ? 127u : pitch));
    c.event = (md_event_t){.kind = MD_EVENT_HIT, .track = T_FX, .source = MD_SOURCE_PANEL};
    c.event.data.hit.voice = (uint16_t)recipe;
    c.event.data.hit.velocity = (uint8_t)(vel > 0u && vel < 128u ? vel : 100u);
    (void)md_session_submit(&S, &c);
    (void)send_list(T_FX, BOX, sizeof BOX / sizeof BOX[0]);
    (void)param(T_FX, MD_FX_BASE_PITCH, 72);
}
/* the scale's pitch classes (bits from the root): the playing chord's, else the latched cursor's, else minor on the
 * key; style 0 pentatonic, 1 the seven-note scale, 2 the chord's tones; major on MAJ / MAJ7 */
static uint32_t run_scale(uint32_t note, uint32_t style, uint32_t *root)
{
    static const uint8_t SCALE[4][7] = {{0, 3, 5, 7, 10}, {0, 2, 4, 7, 9}, {0, 2, 3, 5, 7, 8, 10}, {0, 2, 4, 5, 7, 9, 11}};
    static const uint8_t SCALE_N[4] = {5, 5, 7, 7};
    uint32_t link = running ? playing_chord() : set.latch ? set.cursor : 0xffu, kind = CH_MIN, pcs = 0, k;
    *root = note % 12u;
    if (link < ARP_CHAIN && set.chord[link].kind != CH_NONE && set.chord[link].kind < N_KINDS) {
        *root = set.chord[link].root % 12u;
        kind = set.chord[link].kind;
    }
    if (style == 2u) {
        for (k = 0; k < 4u; k++) pcs |= 1u << (KIND_IV[kind][k] % 12u);
    } else {
        uint32_t i = style * 2u + (kind == CH_MAJ || kind == CH_MAJ7);
        for (k = 0; k < SCALE_N[i]; k++) pcs |= 1u << SCALE[i][k];
    }
    return pcs;
}
/* the run's notes: the scale's from the key up two octaves (up), the same down, up and back, or a sparkle of them */
static void run_shape(run_t *r, const uint8_t *up, uint32_t n, uint32_t shape)   /* up: the notes rising, n of them */
{
    uint32_t k;
    r->n = 0;
    for (k = 0; k < n; k++) {
        if (shape == SH_DOWN) r->notes[r->n++] = up[n - 1u - k];
        else if (shape != SH_SPARKLE) r->notes[r->n++] = up[k];
    }
    if (shape == SH_ARC)
        for (k = 1; k < n && r->n < RUN_MAX; k++) r->notes[r->n++] = up[n - 1u - k];
    if (shape == SH_SPARKLE && n)
        for (k = 0; k < (n > 6u ? 14u : n + 1u); k++) {
            uint32_t j;
            do { run_rng = run_rng * 1664525u + 1013904223u; j = (run_rng >> 16) % n; } while (k && up[j] == r->notes[k - 1u] && n > 1u);
            r->notes[r->n++] = up[j];
        }
}
static void run_fill(run_t *r, uint32_t note, uint32_t pcs, uint32_t root, uint32_t shape)
{
    uint8_t up[RUN_MAX];
    uint32_t n = 0, m;
    for (m = note; m <= note + 24u && m <= 127u && n < RUN_MAX; m++)
        if (pcs >> ((m + 12u - root) % 12u) & 1u) up[n++] = (uint8_t)m;
    run_shape(r, up, n, shape);
}
/* the lead's notes must each be heard: the 2600's glide off and the soft lead's slow swell short while a run plays,
 * the sound's own again after (the Synthex has neither) */
static void run_crisp(uint32_t on)
{
    if (set.lead_soft == 2u) return;
    (void)param(T_LEAD, MD_ARP_GLIDE, on ? 0 : set.lead_glide == 255u ? ARP_PATCHES[LEAD_BASE].glide : set.lead_glide);
    if (set.lead_soft == 1u) (void)param(T_LEAD, MD_ARP_AMP_ATTACK, on ? 8 : 55);
}
static void run_stop(run_t *r)
{
    if (r->track == T_LEAD) {
        if (r->token) (void)md_session_note_off(&S, T_LEAD, r->source, r->token);
        if (r->n) run_crisp(0);
    }
    r->token = 0;
    r->n = 0;
    r->owner = 0;
}
static void run_start(uint8_t note, uint8_t vel, md_source_id_t source, md_note_token_t *slot)   /* the lead's */
{
    run_t *r = &runs[0];
    uint32_t root, pcs = run_scale(note, (set.run - 1u) / 2u, &root);
    run_stop(r);
    r->track = T_LEAD;
    run_crisp(1);
    run_fill(r, note, pcs, root, SH_UP);
    r->i = 0;
    r->vel = vel ? vel : 100;
    r->held = 1;
    r->source = source;
    r->owner = slot;
    r->step = set.run & 1u ? 70u : 15000u / set.tempo;   /* FAST about 14 notes a second, else sixteenths */
    r->next = H->ms();
    *slot = 0;
}
static void run_key_up(md_note_token_t *slot)  /* the key that started the lead's let go: the top ends a step later */
{
    run_t *r = &runs[0];
    if (r->owner != slot) return;
    r->held = 0;
    r->owner = 0;
    if (r->i >= r->n) r->next = H->ms() + r->step;
}
static void run_tick1(run_t *r, uint32_t now)
{
    if (!r->n || (int32_t)(now - r->next) < 0) return;
    if (r->track == T_FX) {
        if (r->i >= r->n) { r->n = 0; return; }
        {   /* a sparkle's hits at random strengths; a fall fades a little to its foot */
            uint32_t v = r->vel;
            if (r->held == SH_SPARKLE) { run_rng = run_rng * 1664525u + 1013904223u; v = v * (55u + (run_rng >> 24) % 46u) / 100u; }
            fx_hit_decay(r->recipe, r->notes[r->i], v, r->sync ? GESTURE_DECAY_MS : 0u);
        }
        r->i++;
        {   /* the speed as it is now: SELECT turns a gesture that is playing too */
            uint32_t step = r->sync ? r->step : r->step * FX_PACE[(set.fx_speed + 4) % 9] / 100u;
            r->next = now - r->next > step ? now + step : r->next + step;
        }
        return;
    }
    if (r->token) (void)md_session_note_off(&S, T_LEAD, r->source, r->token);
    r->token = 0;
    if (r->i >= r->n) {                          /* the top, its key gone: done */
        if (!r->held) run_stop(r);
        return;
    }
    (void)md_session_note_on(&S, T_LEAD, r->source, r->notes[r->i], r->vel, 0, &r->token);
    r->i++;
    r->next = now - r->next > r->step ? now + r->step : r->next + r->step;
    if (r->i >= r->n && r->held) r->next = now + 0x7fffffffu;   /* the top: held until the key goes */
}
static void run_tick(void)                      /* main loop */
{
    uint32_t now = H->ms();
    run_tick1(&runs[0], now);
    run_tick1(&runs[1], now);
    run_tick1(&runs[2], now);
}

/* ---- the live chord: LATCH while stopped sounds the cursor's chord on the strings */
static void chord_stop(void)
{
    uint32_t i;
    for (i = 0; i < n_chord_on; i++) (void)md_session_note_off(&S, T_STRINGS, MD_SOURCE_PANEL, chord_token[i]);
    n_chord_on = 0;
}
static void chord_play(uint32_t link)
{
    const chord_t *c = &set.chord[link % ARP_CHAIN];
    uint32_t i;
    chord_stop();
    if (c->kind == CH_NONE || c->kind >= N_KINDS) return;
    for (i = 0; i < 4u; i++) {
        uint32_t n = 36u + 12u * set.str_oct + c->root + KIND_IV[c->kind][i];
        if (n > 108u) n = 108u;
        if (!md_session_note_on(&S, T_STRINGS, MD_SOURCE_PANEL, (uint8_t)n, 100, 0, &chord_token[n_chord_on])) n_chord_on++;
    }
}
static void chords_follow(void)                 /* main loop */
{
    static uint32_t was_running;
    if (was_running && !running) cur_link = 0xffu;   /* stopped: the latched chord sounds again */
    was_running = running;
    uint32_t want = running ? 0xfeu : set.latch ? set.cursor : 0xffu;   /* running: the patterns play the chords */
    if (running) cur_link = snap.pattern[T_BASS];
    if (want == 0xfeu) { if (n_chord_on) chord_stop(); return; }
    if (want == cur_link) return;
    cur_link = want;
    if (want == 0xffu) chord_stop();
    else chord_play(want);
}
static void notes_off(void)                     /* ARP: every live note from the panel and MIDI */
{
    uint32_t c;
    md_command_t r = {.kind = MD_CMD_EVENT};
    r.event = (md_event_t){.kind = MD_EVENT_SOURCE_RELEASE, .track = MD_ALL_TRACKS, .source = MD_SOURCE_PANEL};
    (void)md_session_submit(&S, &r);
    for (c = 0; c < 16u; c++) {
        r.event.source = (md_source_id_t)(MD_SOURCE_MIDI_BASE + c);
        (void)md_session_submit(&S, &r);
    }
    for (c = 0; c < 27u; c++) key_token[c] = 0;
    for (c = 0; c < 16u * 128u; c++) midi_token[c / 128u][c % 128u] = 0;
    n_chord_on = 0;
    cur_link = 0xffu;
    runs[0].token = 0;                          /* (released with the panel's and MIDI's notes) */
    run_stop(&runs[0]);
    run_stop(&runs[1]);
    run_stop(&runs[2]);
}

/* ---- FX SEQ: the effect sequence, the sixteen events the effects box plays by itself (band.h has what an event is) */
static const char *const FX_HOW_NAME[MD_BAND_FX_HOWS] = {"ONCE", "UP", "DOWN", "ARC", "SPARKLE"};
static const char *const FX_SOUND_NAME[N_FX_SOUNDS] = {"--", "DROP", "PING", "BUBBLE", "CHIRP", "METAL", "ZAP", "WHISTLE", "GULL",
                                                       "TWITTER", "BLEEPS", "WHOOSH", "SURF", "THUNDER"};
_Static_assert(N_FX_SOUNDS == MD_FX_RECIPES + 1u, "a sound for every recipe of the box");
#define FX_SPAN (AMBIENT ? MD_BAND_FX_SPAN : 64u)   /* its length in eighths: sixteen bars, BAND's loop eight */
static uint32_t fx_event_key(uint32_t degree)
{
    md_band_settings_t b;
    band_settings(&b);
    return md_band_fx_key(&b, degree);
}
static int fx_pattern_now(void)                 /* the factory pattern the sequence is, -1: the player's own */
{
    uint32_t p, k;
    for (p = 0; p < N_FX_PATTERNS; p++) {
        for (k = 0; k < MD_BAND_FX_EVENTS; k++) {
            const md_band_fx_event_t *a = &set.fx[k], *b = &FX_PATTERNS[p].ev[k];
            if (a->sound != b->sound || (a->sound && (a->at != b->at || a->degree != b->degree || a->chance != b->chance || a->how != b->how))) break;
        }
        if (k == MD_BAND_FX_EVENTS) return (int)p;
    }
    return -1;
}
static void fx_gesture(const md_band_fx_event_t *e);
static void fx_audition(void)                   /* the event under the cursor, as the sequence will play it */
{
    const md_band_fx_event_t *e = &set.fx[fx_cur % MD_BAND_FX_EVENTS];
    if (e->sound && e->how) fx_gesture(e);
    else if (e->sound) fx_hit(e->sound - 1u, fx_event_key(e->degree), md_band_fx_velocity(e->sound));
}
static void fx_pattern_to(int32_t d)            /* ALGORITHM: the patterns, then YOURS (the sequence as it was edited) */
{
    int now = fx_pattern_now();
    int32_t v, top = (int32_t)N_FX_PATTERNS - 1;
    uint32_t k;
    if (now < 0) { for (k = 0; k < MD_BAND_FX_EVENTS; k++) fx_own[k] = set.fx[k]; fx_own_kept = 1; }
    if (fx_own_kept) top++;
    v = (now < 0 ? top : now) + d;
    v = v < 0 ? 0 : v > top ? top : v;
    for (k = 0; k < MD_BAND_FX_EVENTS; k++) set.fx[k] = v < (int32_t)N_FX_PATTERNS ? FX_PATTERNS[v].ev[k] : fx_own[k];
    todo |= G_BANK;
}
static uint32_t fx_bar(void)                    /* the bar of the sequence that plays, 0xff stopped */
{
    return running ? (snap.position.quarter / 4u) % (FX_SPAN / 8u) : 0xffu;
}

/* ---- the screen */
static void draw_knobs(const char *const name[4], const char *const val[4], const uint8_t bar[4], const uint8_t lit[4])
{
    uint32_t k;
    for (k = 0; k < 4u; k++) {
        int32_t x = 4 + (int32_t)k * 59;
        H->fill(x, 56, 56, 72, C_BLACK);
        H->text(x, 56, 56, 0, name[k], C_GREY, 1);
        H->text(x, 78, 56, 0, val[k], lit[k] ? C_WHITE : C_GREY, 1);
        H->fill(x + 2, 104, 52, 6, C_DIM);
        H->fill(x + 2, 104, (int32_t)(bar[k] > 100u ? 100u : bar[k]) * 52 / 100, 6, lit[k] ? C_ACC : C_DIM);
    }
}
static void draw_head(void)
{
    char t[20], *p;
    H->fill(0, 0, 240, 48, C_BLACK);
    p = put_u(t, set.setup + 1u); p = put_s(p, " "); put_s(p, SETUP_NAME);
    H->text(4, 2, 150, 0, t, C_ACC, 0);
    p = put_u(t, set.tempo); put_s(p, running ? " RUN" : " BPM");
    H->text(150, 2, 86, 0, t, running ? C_ACC : C_GREY, 2);
    H->text(4, 24, 120, 0, PAGE_NAME[set.page], C_CREAM, 0);
    if (room_forced) H->text(124, 24, 112, 0, "GLOBAL ROOM", C_GREY, 1);   /* the host's room we cannot turn off */
    else if (solo_track < MD_TRACKS) {
        p = put_s(t, "SOLO "); put_s(p, PART_NAME[solo_track]);
        H->text(124, 24, 112, 0, t, C_ACC, 1);
    } else if (sweep_cut < 990 || sweep_res > 0) {
        p = put_s(t, "SWEEP "); put_u(p, (uint32_t)sweep_cut / 10u);
        H->text(124, 24, 112, 0, t, C_ACC, 1);
    } else H->text(124, 24, 112, 0, "PRESETS: SETUP", C_GREY, 1);
    H->fill(0, 46, 240, 2, C_DIM);
}
static void draw_body(uint32_t full)
{
    static const char *const K_STR[4] = {"ATTACK", "RELEASE", "CHORUS", "PHASER"}, *const K_PAD[4] = {"ATTACK", "RELEASE", "DEPTH", "TONE"}, *const K_LEAD[4] = {"CUTOFF", "RESO", "GLIDE", "LEVEL"},
        *const K_BASS[4] = {"CUTOFF", "SWEEP", "TEMPO", "LEVEL"}, *const K_DRUM[4] = {"TEMPO", "LEVEL", "BALANCE", "EXTRAS"},
        *const K_CHORD[4] = {"ROOT", "KIND", "OCTAVE", "STRINGS"}, *const K_MIX[4] = {"STRINGS", "LEAD", "BASS", "DRUMS"},
        *const K_ECHO[4] = {"FEEDBK", "TONE", "STRINGS", "LEAD"};
    char v[4][12], line[40], *p;
    const char *vals[4] = {v[0], v[1], v[2], v[3]};
    uint8_t bar[4] = {0, 0, 0, 0}, lit[4] = {1, 1, 1, 1};
    uint32_t k;
    (void)full;
    H->fill(0, 128, 240, 112, C_BLACK);
    switch (set.page) {
    case PG_STRINGS:
        put_u(v[0], set.str_att); put_u(v[1], set.str_rel);
        if (set.str_ens) put_u(v[2], set.str_ens); else put_s(v[2], "OFF");
        if (set.str_ph) put_u(v[3], set.str_ph); else put_s(v[3], "OFF");
        bar[0] = set.str_att; bar[1] = set.str_rel; bar[2] = set.str_ens; bar[3] = set.str_ph; lit[2] = set.str_ens != 0; lit[3] = set.str_ph != 0;
        if (STR_ENS) {                           /* an ENSEMBLE pad: the fourth knob is its tone */
            put_u(v[3], set.str_ph); lit[3] = 1;
            draw_knobs(K_PAD, vals, bar, lit);
            H->text(4, 136, 232, 0, ENS_PRESETS[STR_PAD].name, C_WHITE, 0);
            H->text(4, 200, 232, 0, "ALGO VOICE   ENSEMBLE PAD", C_GREY, 1);
            break;
        }
        draw_knobs(K_STR, vals, bar, lit);
        p = put_s(line, REG_NAME[set.str_reg % EM_N_REG]); p = put_s(p, "   COLOR "); put_s(p, set.str_col ? "ON" : "OFF");
        H->text(4, 136, 232, 0, line, C_WHITE, 0);
        H->text(4, 200, 232, 0, "ALGO VOICE   FX COLOR", C_GREY, 1);
        break;
    case PG_LEAD: {
        const arp_patch_t *q = &ARP_PATCHES[LEAD_BASE];
        put_i(v[0], set.lead_cut); put_u(v[1], set.lead_res == 255u ? q->reso : set.lead_res);
        put_u(v[2], set.lead_glide == 255u ? q->glide : set.lead_glide); put_u(v[3], set.lvl[1]);
        bar[0] = (uint8_t)(50 + set.lead_cut); bar[1] = set.lead_res == 255u ? q->reso : set.lead_res; bar[2] = set.lead_glide == 255u ? q->glide : set.lead_glide; bar[3] = set.lvl[1];
        draw_knobs(K_LEAD, vals, bar, lit);
        p = put_u(line, lead_pos()); p = put_s(p, "/"); p = put_u(p, N_LEAD_SOUNDS - 1u); p = put_s(p, "  ");
        lead_name(p);
        H->text(4, 136, 232, 0, line, C_WHITE, 0);
        if (set.run) { p = put_s(line, "RUN "); p = put_s(p, RUN_NAME[set.run % N_RUNS]); put_s(p, "  (SELECT)"); }
        else put_s(line, "KEYS PLAY LEAD  SELECT RUN");
        H->text(4, 158, 232, 0, line, C_ACC, 1);
        p = put_s(line, "KEYS FROM F"); p = put_u(p, set.oct + 1u); put_s(p, "  (OCT- / OCT+)");
        H->text(4, 178, 232, 0, line, C_CREAM, 1);
        H->text(4, 200, 232, 0, "ALGO THE SOUND", C_GREY, 1);
        break;
    }
    case PG_BASS: {
        const arp_patch_t *q = &ARP_PATCHES[set.bass_patch % ARP_N_PATCHES];
        const arp_seq_t *s = &ARP_SEQS[set.bass_seq % ARP_N_SEQS];
        put_i(v[0], set.bass_cut); put_s(v[1], "--"); put_u(v[2], set.tempo); put_u(v[3], set.lvl[2]);
        bar[0] = (uint8_t)(50 + set.bass_cut); bar[2] = (uint8_t)((set.tempo - 40u) * 100u / 200u); bar[3] = set.lvl[2];
        lit[1] = 0;                             /* the sweep is not in this version: shown off, not pretended */
        draw_knobs(K_BASS, vals, bar, lit);
        p = put_s(line, "P"); p = put_u(p, set.bass_patch + 1u); p = put_s(p, " "); put_s(p, q->name);
        H->text(4, 136, 232, 0, line, C_WHITE, 0);
        p = put_s(line, "S"); p = put_u(p, set.bass_seq + 1u); p = put_s(p, " "); p = put_s(p, s->name);
        if (s->n_chain) { p = put_s(p, "  "); p = put_u(p, s->n_chain); put_s(p, " LINKS"); }
        H->text(4, 158, 232, 0, line, C_BLUE, 0);
        H->text(4, 200, 232, 0, AMBIENT ? "ALGO PATCH   KEYS PLAY IT" : "ALGO PATCH  SELECT SEQ  PLAY", C_GREY, 1);
        break;
    }
    case PG_DRUMS: {
        uint32_t down = 0, one = 10;
        put_u(v[0], set.tempo); put_u(v[1], set.lvl[3]); put_u(v[2], set.rh_balance); put_u(v[3], set.rh_extra);
        bar[0] = (uint8_t)((set.tempo - 40u) * 100u / 200u); bar[1] = set.lvl[3]; bar[2] = set.rh_balance; bar[3] = set.rh_extra;
        draw_knobs(K_DRUM, vals, bar, lit);
        for (k = 0; k < 10u; k++) if (set.rh_keys >> k & 1u) { down++; one = k; }
        if (!down) put_s(line, "NO KEY");
        else if (down > 1u) { p = put_u(line, down); put_s(p, " KEYS"); }
        else { const char *nm = mp_rhythm(set.rh_section, one)->name; p = line; while (*nm) { char c = *nm++; *p++ = c >= 'a' && c <= 'z' ? (char)(c - 32) : c; } *p = 0; }
        p = line; while (*p) p++;
        p = put_s(p, "   LEVER "); put_s(p, set.rh_section ? "B" : "A");
        H->text(4, 136, 232, 0, line, C_WHITE, 0);
        p = put_s(line, "TAMB "); p = put_s(p, set.rh_xmask & 1u ? "ON " : "OFF"); p = put_s(p, "  GUIRO "); p = put_s(p, set.rh_xmask & 2u ? "ON " : "OFF");
        p = put_s(p, "  QUIJ "); put_s(p, set.rh_xmask & 4u ? "ON" : "OFF");
        H->text(4, 158, 232, 0, line, C_GREY, 0);
        if (report.drum_moved || (set.rh_xmask & 2u)) {   /* what the v1 grid cannot keep: say so */
            p = line;
            if (report.drum_moved) { p = put_u(p, report.drum_moved); p = put_s(p, " HITS ON 32NDS "); }
            if (set.rh_xmask & 2u) put_s(p, "GUIRO 1/4 S");   /* one scrape length, not the machine's three */
            H->text(4, 178, 232, 0, line, C_GREY, 1);
        }
        H->text(4, 189, 232, 0, "ONE RHYTHM AT A TIME (CPU)", C_GREY, 1);
        H->text(4, 200, 232, 0, "KEYS RHYTHMS  OCT LEVER  PLAY", C_GREY, 1);
        break;
    }
    case PG_CHORDS: {
        const chord_t *c = &set.chord[set.cursor];
        put_s(v[0], NOTE_NAME[c->root % 12u]); put_s(v[1], KIND_NAME[c->kind % N_KINDS]); put_u(v[2], set.str_oct); put_u(v[3], set.lvl[0]);
        bar[0] = (uint8_t)(c->root * 100u / 11u); bar[1] = (uint8_t)(c->kind * 100u / (N_KINDS - 1u)); bar[2] = (uint8_t)(set.str_oct * 25u); bar[3] = set.lvl[0];
        lit[0] = lit[1] = c->kind != CH_NONE;
        draw_knobs(K_CHORD, vals, bar, lit);
        for (k = 0; k < ARP_CHAIN; k++) {       /* the eight links: root and kind, the cursor and the one playing */
            int32_t x = 4 + (int32_t)k * 29;
            const chord_t *d = &set.chord[k];
            uint16_t col = running && k == playing_chord() ? C_ACC : AMBIENT || k < chain.links ? C_WHITE : C_GREY;
            H->fill(x, 132, 27, 36, k == set.cursor ? C_BLUE : C_DIM);
            H->fill(x + 2, 134, 23, 32, C_BLACK);
            H->text(x, 134, 27, 0, d->kind == CH_NONE ? "-" : NOTE_NAME[d->root % 12u], col, 1);
            H->text(x, 150, 27, 0, d->kind == CH_NONE ? "" : KIND_NAME[d->kind % N_KINDS], col, 1);
        }
        H->text(4, 176, 232, 0, set.latch ? "LATCH: THE KEYS SET THE CHORD" : "FOLLOWING THE BASS PATTERN", set.latch ? C_ACC : C_GREY, 1);
        H->text(4, 200, 232, 0, AMBIENT ? "SELECT CHORD (2 BARS EACH)" : "SELECT LINK   REC LATCH", C_GREY, 1);
        break;
    }
    case PG_PARTS: {
        static const char *const K_PARTS[4] = {"WIND", "COLOUR", "FX BOX", "L.ECHO"};
        put_u(v[0], set.lvl_wind); put_u(v[1], set.wind_col); put_u(v[2], set.lvl_fx); put_u(v[3], set.ec_lead);
        bar[0] = set.lvl_wind; bar[1] = set.wind_col; bar[2] = set.lvl_fx; bar[3] = set.ec_lead;
        draw_knobs(K_PARTS, vals, bar, lit);
        p = put_s(line, "FX SPEED "); p = put_i(p, set.fx_speed); put_s(p, "  (SEL)");
        H->text(4, 136, 232, 0, line, C_WHITE, 0);
        H->text(4, 156, 232, 0, "RISE FALL ARC SPARKLE", C_CREAM, 1);
        H->text(4, 167, 232, 0, "DROPS UP/DN METAL BUBBLE", C_CREAM, 1);
        H->text(4, 178, 232, 0, "F# GUST  BLACK ONE-SHOTS", C_CREAM, 1);
        p = put_s(line, "SEL BG: "); p = put_s(p, set.amb_arp ? set.phrase ? "RUN + " : "FX RUN" : "");
        put_s(p, set.phrase || !set.amb_arp ? PHRASE_NAME[set.phrase % (MD_BAND_PHRASES + 1u)] : "");
        H->text(4, 189, 232, 0, line, AMBIENT ? C_GREY : C_DIM, 1);
        H->text(4, 200, 232, 0, "FX AGAIN: FX SEQUENCE", C_GREY, 1);
        break;
    }
    case PG_FXSEQ: {
        static const char *const K_SEQ[4] = {"WHEN", "SOUND", "NOTE", "CHANCE"};
        static const uint16_t FX_COL[N_FX_SOUNDS] = {C_DIM, C_BLUE, C_WHITE, C_CREAM, C_ACC, C_GREY,
            HB_RGB(235, 70, 60), HB_RGB(150, 200, 255), HB_RGB(255, 240, 200), HB_RGB(120, 200, 110), HB_RGB(200, 120, 220),
            HB_RGB(110, 170, 170), HB_RGB(60, 150, 210), HB_RGB(160, 135, 90)};
        const md_band_fx_event_t *e = &set.fx[fx_cur % MD_BAND_FX_EVENTS];
        uint32_t span = FX_SPAN, at = e->at % span, key = fx_event_key(e->degree), now_bar = fx_bar();
        int now = fx_pattern_now();
        p = put_u(v[0], at / 8u + 1u); p = put_s(p, "."); put_u(p, at % 8u + 1u);
        put_s(v[1], FX_SOUND_NAME[e->sound % N_FX_SOUNDS]);
        p = put_s(v[2], NOTE_NAME[key % 12u]); put_u(p, key / 12u - 1u);
        put_u(v[3], e->chance);
        bar[0] = (uint8_t)(at * 100u / (span - 1u)); bar[1] = (uint8_t)(e->sound * 100u / (N_FX_SOUNDS - 1u));
        bar[2] = (uint8_t)(e->degree * 100u / (MD_BAND_FX_DEGREES - 1u)); bar[3] = e->chance;
        lit[0] = lit[2] = lit[3] = e->sound != 0;
        draw_knobs(K_SEQ, vals, bar, lit);
        /* the line: the bars (a longer tick every fourth), the bar playing, the cursor, then each event at its time,
         * higher for a higher note, in its sound's colour */
        H->fill(4, 166, 232, 1, C_GREY);
        for (k = 0; k <= span / 8u; k++)
            H->fill(4 + (int32_t)(k * 8u * 231u / span), k % 4u ? 163 : 158, 1, k % 4u ? 3 : 8, C_GREY);
        if (now_bar != 0xffu) H->fill(4 + (int32_t)(now_bar * 8u * 231u / span), 168, (int32_t)(8u * 231u / span), 3, C_ACC);
        H->fill(4 + (int32_t)(at * 231u / span), 130, 1, 36, e->sound ? C_ACC : C_GREY);
        for (k = 0; k < MD_BAND_FX_EVENTS; k++) {
            const md_band_fx_event_t *d = &set.fx[k];
            int32_t x = 4 + (int32_t)((d->at % span) * 231u / span), y = 160 - 2 * (int32_t)d->degree;
            if (!d->sound) continue;
            if (k == fx_cur) H->fill(x - 3, y - 2, 7, 8, C_ACC);
            H->fill(x - 1, d->how ? y - 4 : y, 3, d->how ? 8 : 4, FX_COL[d->sound % N_FX_SOUNDS]);   /* (a gesture: taller) */
        }
        p = put_s(line, "EVENT "); p = put_u(p, fx_cur + 1u); p = put_s(p, "/"); p = put_u(p, MD_BAND_FX_EVENTS);
        if (e->sound) { p = put_s(p, "  ARP: "); put_s(p, FX_HOW_NAME[e->how % MD_BAND_FX_HOWS]); }
        else put_s(p, "  FREE (SELECT)");
        H->text(4, 174, 232, 0, line, C_WHITE, 0);
        p = put_s(line, "ALGO PATTERN: "); put_s(p, now < 0 ? "YOURS" : FX_PATTERNS[now].name);
        H->text(4, 187, 232, 0, line, C_CREAM, 0);
        H->text(4, 200, 232, 0, set.lvl_fx ? "KEYS SET THE NOTE  SEL SOLO" : "FX BOX LEVEL IS 0 (PARTS)", C_GREY, 1);
        break;
    }
    case PG_MIX:
        for (k = 0; k < 4u; k++) { put_u(v[k], set.lvl[k]); bar[k] = set.lvl[k]; lit[k] = set.lvl[k] != 0; }
        draw_knobs(K_MIX, vals, bar, lit);
        p = put_s(line, "REVERB "); p = put_u(p, set.reverb); p = put_s(p, "%  MOTION "); p = put_u(p, set.motion);
        put_s(p, "%");
        H->text(4, 136, 232, 0, line, C_WHITE, 0);
        p = put_s(line, "SETUP "); p = put_u(p, set.setup + 1u); p = put_s(p, " "); p = put_s(p, SETUP_NAME);
        put_s(p, AMBIENT ? "  AMB" : "  BAND");
        H->text(4, 158, 232, 0, line, C_CREAM, 0);
        H->text(4, 180, 232, 0, "ALGO REVERB  SELECT MOTION", C_GREY, 1);
        if (snap.meters.clipped) H->text(4, 191, 232, 0, "CLIPPING: LOWER THE LEVELS", C_ACC, 1);
        H->text(4, 200, 232, 0, "FX AGAIN: FX BOX, LEAD", C_GREY, 1);
        break;
    default:
        put_u(v[0], set.ec_fb); put_u(v[1], set.ec_tone); put_u(v[2], set.ec_str); put_u(v[3], set.ec_lead);
        bar[0] = set.ec_fb; bar[1] = set.ec_tone; bar[2] = set.ec_str; bar[3] = set.ec_lead; lit[2] = set.ec_str != 0; lit[3] = set.ec_lead != 0;
        draw_knobs(K_ECHO, vals, bar, lit);
        p = put_s(line, "TIME "); p = put_s(p, EC_DIV_NAME[set.ec_div % 7u]); put_s(p, "   (ALGORITHM)");
        H->text(4, 136, 232, 0, line, set.ec_div ? C_WHITE : C_GREY, 0);
        p = put_s(line, "HALL "); p = put_s(p, HALLS[set.hall % N_HALLS].name); put_s(p, "   (SELECT)");
        H->text(4, 158, 232, 0, line, C_CREAM, 0);
        H->text(4, 180, 232, 0, "SENDS: BASS AND DRUMS DRY", C_GREY, 1);
        H->text(4, 200, 232, 0, "FX AGAIN: THE MIX", C_GREY, 1);
        break;
    }
}
static uint32_t playing_chord(void)              /* the chord sounding: AMBIENT's eight two-bar chords, BAND's link */
{
    if (!running) return 0xffu;
    return AMBIENT ? (snap.position.quarter / 8u) % 8u : cur_link;
}
/* The effect sequence's gestures: an event whose HOW is not ONCE is played here, when the sequence comes to its
 * eighth (the pattern holds only the single hits): several hits of its sound from its note, through the setup's
 * pentatonic in AMBIENT (in key whatever the chord, as the single hits) and through the pentatonic of the link's own
 * chord in BAND (its links change key). How many and how fast is the sound's: a struck one runs two octaves in
 * sixteenths (an ARC twenty-four to the bar, to be back within it), a long one is a few, well apart. One gesture at a
 * time: the next takes over. */
static const struct { uint8_t n, every, sixteenths; } FX_GESTURE[MD_FX_RECIPES] = {   /* hits, scale steps apart, time apart */
    {11, 1, 1}, {11, 1, 1}, {11, 1, 1}, {11, 1, 1}, {11, 1, 1},   /* DROP PING BUBBLE CHIRP METAL */
    {6, 2, 2}, {3, 2, 8}, {4, 1, 4}, {3, 2, 8}, {3, 2, 6},        /* ZAP WHISTLE GULL TWITTER BLEEPS */
    {3, 2, 8}, {2, 3, 16}, {2, 2, 12}};                          /* WHOOSH SURF THUNDER */
static void fx_gesture(const md_band_fx_event_t *e)
{
    run_t *r = &runs[2];
    uint32_t recipe = (e->sound - 1u) % MD_FX_RECIPES, n = 0, k, shape, link = playing_chord();
    const uint8_t every = FX_GESTURE[recipe].every, want = FX_GESTURE[recipe].n;
    uint8_t up[RUN_MAX];
    if (AMBIENT || link >= ARP_CHAIN || set.chord[link].kind == CH_NONE || set.chord[link].kind >= N_KINDS) {
        for (k = 0; k < want; k++) {             /* the scale goes on over the sequence's three octaves */
            uint32_t d = e->degree + k * every, key = fx_event_key(d % MD_BAND_FX_DEGREES) + 36u * (d / MD_BAND_FX_DEGREES);
            if (key > 120u) break;
            up[n++] = (uint8_t)key;
        }
    } else {
        uint32_t root, note = fx_event_key(e->degree), pcs = run_scale(note, 0, &root), m, seen = 0;
        for (m = note; m <= 120u && n < want; m++)
            if (pcs >> ((m + 12u - root) % 12u) & 1u) { if (!(seen++ % every)) up[n++] = (uint8_t)m; }
    }
    shape = e->how == MD_BAND_FX_DOWN ? SH_DOWN : e->how == MD_BAND_FX_ARC ? SH_ARC : e->how == MD_BAND_FX_SPARKLE ? SH_SPARKLE : SH_UP;
    run_shape(r, up, n, shape);
    r->track = T_FX;
    r->recipe = (uint8_t)recipe;
    r->held = (uint8_t)shape;
    r->sync = 1;
    r->i = 0;
    r->vel = (uint8_t)md_band_fx_velocity(e->sound);
    r->step = FX_GESTURE[recipe].sixteenths == 1u && shape == SH_ARC ? 10000u / set.tempo
                                                                    : 15000u * FX_GESTURE[recipe].sixteenths / set.tempo;
    r->next = H->ms();
}
static void fx_gestures(void)                    /* main loop: the gesture on the eighth the sequence has come to */
{
    static uint32_t last = 0xffffffffu;
    uint32_t span = FX_SPAN, now, k;
    if (!running) {
        if (last != 0xffffffffu) runs[2].n = 0;  /* stopped: the gesture with it (one tried on FX SEQ plays on) */
        last = 0xffffffffu;
        return;
    }
    now = (snap.position.quarter % (span / 2u)) * 2u + snap.position.unit / 4800u;
    if (now == last) return;
    last = now;
    for (k = 0; k < MD_BAND_FX_EVENTS; k++) {
        const md_band_fx_event_t *e = &set.fx[k];
        if (!e->sound || !e->how || !e->chance || e->at % span != now) continue;
        run_rng = run_rng * 1664525u + 1013904223u;
        if ((run_rng >> 16) % 100u < e->chance) fx_gesture(e);
        break;                                   /* (the first in the list has the eighth) */
    }
}
static uint32_t state_hash(void)
{
    const uint8_t *b = (const uint8_t *)&set;
    uint32_t i, h = 2166136261u;
    for (i = 0; i < sizeof set; i++) h = (h ^ b[i]) * 16777619u;
    return (h ^ (cur_link + 1u) << 24 ^ playing_chord() << 16 ^ running << 23 ^ (snap.meters.clipped != 0) << 22 ^ room_forced << 21)
           + solo_track * 7919u + (uint32_t)sweep_cut * 31u + (uint32_t)sweep_res * 131u
           + (set.page == PG_FXSEQ ? fx_cur * 977u + fx_bar() * 613u : 0u);
}
static void draw_banner(void)                   /* the setup just chosen, over the page */
{
    char t[24], *p;
    H->fill(8, 60, 224, 150, C_ACC);
    H->fill(11, 63, 218, 144, C_BLACK);
    p = put_s(t, "SETUP "); p = put_u(p, set.setup + 1u); p = put_s(p, " OF "); put_u(p, N_SETUPS);
    H->text(16, 72, 208, 0, t, C_GREY, 1);
    H->text(16, 96, 208, 0, SETUP_NAME, C_ACC, 0);
    H->text(16, 130, 208, 0, SETUPS[set.setup % N_SETUPS].about, C_CREAM, 1);
    H->text(16, 150, 208, 0, AMBIENT ? "AMBIENT: PLAY TO START" : "BAND: PLAY TO START", C_WHITE, 1);
    H->text(16, 180, 208, 0, "PRESETS: ANOTHER SETUP", C_GREY, 1);
}
static void draw(uint32_t full)
{
    uint32_t h;
    if (!md_session_snapshot(&S, &snap)) running = snap.running;
#ifdef MD_STRESS_HOOK
    MD_STRESS_HOOK(2, 0);                        /* tests/stress only: timed events of a long run */
#endif
    (void)md_session_pending(&S);               /* takes up acknowledged edits */
    schedule_links();
    pump();
    if (restart && !todo && !running) { band(1); restart = 0; }
    if (!todo) motion();
    wind_sweep();
    chords_follow();
    fx_gestures();
    run_tick();
    h = state_hash();
    if (banner_on && H->ms() - banner_t > 2000u) { banner_on = 0; banner_shown = 0xffu; drawn_page = ~0u; }
    if (full || set.page != drawn_page) { H->fill(0, 0, 240, 240, C_BLACK); drawn_page = set.page; drawn = ~0u; banner_shown = 0xffu; }
    if (h != drawn) { draw_head(); if (!banner_on) draw_body(full); drawn = h; }
    if (banner_on && banner_shown != set.setup) { draw_banner(); banner_shown = set.setup; }
    if (set.page == PG_DRUMS) {
        uint32_t k;
        for (k = 0; k < 17u; k++)
            H->led(HB_LED_KEY(k), WHITE[k] >= 0 ? (set.rh_keys >> WHITE[k] & 1u) :
                   k == 1u ? (set.rh_xmask & 1u) : k == 3u ? (set.rh_xmask >> 1 & 1u) : k == 5u ? (set.rh_xmask >> 2 & 1u) : 0u);
        drawn_live = 1;
    } else if (drawn_live) {
        uint32_t k;
        for (k = 0; k < 17u; k++) H->led(HB_LED_KEY(k), 0);
        drawn_live = 0;
    }
    H->led(HB_BTN_PLAY, running);
    H->led(HB_BTN_REC, set.latch);
    H->led(HB_BTN_SEL, solo_track != 0xffu);
    H->led(HB_BTN_FX, set.page == PG_MIX || set.page == PG_ECHO || set.page == PG_PARTS || set.page == PG_FXSEQ);
    if (dirty && H->ms() - t_save > 2000u) {
        set.magic = SET_MAGIC;
        H->save(&set, sizeof set);
        dirty = 0;
    }
}

/* ---- the panel */
static void page_to(uint32_t pg) { set.page = (uint8_t)pg; touch(); }

static void lead_on(uint8_t note, uint8_t vel, md_source_id_t source, md_note_token_t *slot)
{
    if (set.run) { if (*slot) (void)md_session_note_off(&S, T_LEAD, source, *slot); run_start(note, vel, source, slot); return; }
    if (*slot) (void)md_session_note_off(&S, T_LEAD, source, *slot);   /* a repeated key-down: the old one first */
    *slot = 0;
    (void)md_session_note_on(&S, T_LEAD, source, note, vel ? vel : 100, 0, slot);
}
/* the part a page's keys play and SEL solos: strings, bass and lead pages their own part, DRUMS the drums */
static uint8_t page_part(void)
{
    switch (set.page) {
    case PG_STRINGS: case PG_CHORDS: return T_STRINGS;
    case PG_BASS: return T_BASS;
    case PG_DRUMS: return T_DRUMS;
    case PG_LEAD: return T_LEAD;
    case PG_FXSEQ: return T_FX;
    default: return 0xffu;
    }
}
static void solo_toggle(void)
{
    uint8_t part = page_part();
    if (part == 0xffu) return;
    if (solo_track != 0xffu) (void)command(MD_CMD_SOLO, solo_track, 0, 0);
    if (solo_track == part) { solo_track = 0xffu; return; }   /* the same part again: everyone back */
    if (!command(MD_CMD_SOLO, part, 0, 1)) solo_track = part;
}
static void sweep_send(void)                     /* the master filter: in only while it does something */
{
    int on = sweep_cut < 990 || sweep_res > 0;
    (void)param(T_STRINGS, MD_TARGET_MASTER_FILTER | MD_FILTER_CUTOFF, sweep_cut);
    (void)param(T_STRINGS, MD_TARGET_MASTER_FILTER | MD_FILTER_RESONANCE, sweep_res);
    (void)command(MD_CMD_BYPASS, MD_BYPASS_MASTER, 0, on ? 0u : 1u);
}
static void lead_off(md_source_id_t source, md_note_token_t *slot)
{
    run_key_up(slot);
    if (*slot) (void)md_session_note_off(&S, T_LEAD, source, *slot);   /* a refused off becomes a source flush */
    *slot = 0;
}
/* PARTS: the keys play the effects box. The white keys from the lowest F, round again an octave up: gestures through
 * the chord's pentatonic, two octaves from the key: RISE, FALL, ARC (up and back) and SPARKLE on glass pings, DROPS
 * UP and DROPS DOWN, METAL FALL, BUBBLE RISE. The first F# the wind gust while held; the other black keys one hit each,
 * DROP PING BUBBLE CHIRP METAL ZAP WHISTLE GULL TWITTER BLEEPS WHOOSH SURF THUNDER round, at the key's pitch (the three
 * of noise: its colour). The long ones play at twice the strength the sequence gives them: by hand they are in front. */
static const struct { const char *name; uint8_t recipe, shape, step; } FX_RUN[8] = {
    {"RISE", MD_FX_PING, SH_UP, 55}, {"FALL", MD_FX_PING, SH_DOWN, 55}, {"ARC", MD_FX_PING, SH_ARC, 50},
    {"SPARKLE", MD_FX_PING, SH_SPARKLE, 75}, {"DROPS UP", MD_FX_DROP, SH_UP, 60}, {"DROPS DOWN", MD_FX_DROP, SH_DOWN, 60},
    {"METAL FALL", MD_FX_METAL, SH_DOWN, 85}, {"BUBBLE RISE", MD_FX_BUBBLE, SH_UP, 70}};
static void fx_key(uint32_t k, uint32_t note, int32_t vel)
{
    static const uint16_t BLACK = 0x54au;        /* the pitch classes C# D# F# G# A# (bits) */
    uint32_t j, count = 0, v = vel > 0 && vel < 128 ? (uint32_t)vel : 100u;
    int black = BLACK >> ((5u + k) % 12u) & 1u;  /* (key 0 is an F) */
    if (k == 1u) { gust_key = 1; wind_gust = 1; return; }
    for (j = 0; j < k; j++) count += (uint32_t)((BLACK >> ((5u + j) % 12u) & 1u) == (uint32_t)black);
    if (black) {                                 /* (the gust is the first black) */
        uint32_t recipe = (count - 1u) % MD_FX_RECIPES;
        if (recipe > MD_FX_METAL) { v = v * 2u * md_band_fx_velocity(recipe + 1u) / 100u; v = v > 127u ? 127u : v ? v : 1u; }
        fx_hit(recipe, note, v);
        return;
    }
    {
        run_t *r = &runs[1];
        uint32_t root, pcs = run_scale(note, 0, &root), e = count % 8u;
        run_stop(r);
        r->track = T_FX;
        r->recipe = FX_RUN[e].recipe;
        r->held = FX_RUN[e].shape;               /* (an effects run holds nothing: the field keeps its shape) */
        run_fill(r, note, pcs, root, FX_RUN[e].shape);
        r->i = 0;
        r->vel = (uint8_t)v;
        r->step = FX_RUN[e].step;
        r->next = H->ms();
    }
}
static void key_down(uint32_t k, int32_t vel)
{
    uint32_t note = 29u + k + 12u * set.oct;     /* octave 0: the lowest key F1, 6: F7 */
    if (k >= 27u) return;
    if (set.page == PG_DRUMS) {                 /* the rhythm keys */
        /* one rhythm at a time: combined keys exceed the audio budget (work/evidence/audio-budget.md);
         * a key selects its rhythm, the lit key again clears it */
        if (WHITE[k] >= 0) set.rh_keys = set.rh_keys == (1u << WHITE[k]) ? 0u : (uint16_t)(1u << WHITE[k]);
        else if (k == 1u) set.rh_xmask ^= 1u;
        else if (k == 3u) set.rh_xmask ^= 2u;
        else if (k == 5u) set.rh_xmask ^= 4u;
        else return;
        todo |= G_BANK; touch();
        return;
    }
    if (set.page == PG_PARTS) { fx_key(k, note, vel); return; }
    if (set.page == PG_FXSEQ) {                 /* the key's note, to the nearest step of the scale, for the event under
                                                 * the cursor (a free slot becomes a droplet); heard at once */
        md_band_fx_event_t *e = &set.fx[fx_cur % MD_BAND_FX_EVENTS];
        uint32_t d, best = 0, far = 999u;
        for (d = 0; d < MD_BAND_FX_DEGREES; d++) {
            int32_t gap = (int32_t)fx_event_key(d) - (int32_t)note;
            if (gap < 0) gap = -gap;
            if ((uint32_t)gap < far) { far = (uint32_t)gap; best = d; }
        }
        e->degree = (uint8_t)best;
        if (!e->sound) { e->sound = FX_DROP; e->chance = 100; }
        fx_audition();
        todo |= G_BANK; touch();
        return;
    }
    if (set.latch) {                            /* the chord under the cursor: this key's note, a minor chord if none */
        chord_t *c = &set.chord[set.cursor];
        c->root = (uint8_t)(note % 12u);
        if (c->kind == CH_NONE) c->kind = CH_MIN;
        cur_link = 0xffu;                       /* (played again now when stopped) */
        todo |= G_BANK; touch();
        return;
    }
    {   /* the keys play the page's part (strings, bass or lead); other pages the lead */
        uint8_t part = page_part();
        if (part == 0xffu || part == T_DRUMS || part == T_FX) part = T_LEAD;
        if (key_token[k]) (void)md_session_note_off(&S, key_track[k], MD_SOURCE_PANEL, key_token[k]);
        key_token[k] = 0;
        key_track[k] = part;
        if (part == T_LEAD && set.run) { run_start((uint8_t)(note > 127u ? 127u : note), (uint8_t)(vel > 0 && vel < 128 ? vel : 100), MD_SOURCE_PANEL, &key_token[k]); return; }
        (void)md_session_note_on(&S, part, MD_SOURCE_PANEL, (uint8_t)(note > 127u ? 127u : note),
                                 (uint8_t)(vel > 0 && vel < 128 ? vel : 100), 0, &key_token[k]);
    }
}
static void key_up(uint32_t k)
{
    /* the token and part from key-down, whatever the page now; a refused off becomes a source flush */
    if (k == 1u && gust_key) { gust_key = 0; wind_gust = 0; }
    if (k < 27u) run_key_up(&key_token[k]);
    if (k < 27u && key_token[k]) (void)md_session_note_off(&S, key_track[k], MD_SOURCE_PANEL, key_token[k]);
    if (k < 27u) key_token[k] = 0;
}
static void knob(uint32_t k, int32_t d)         /* KNOB1..4 on the page */
{
    if (fx_held && k < 2u) {                     /* FX held: KNOB1 sweeps the master cutoff, KNOB2 its resonance */
        if (k == 0) { sweep_cut += 15 * d; sweep_cut = sweep_cut < 0 ? 0 : sweep_cut > 990 ? 990 : sweep_cut; }
        else { sweep_res += 4 * d; sweep_res = sweep_res < 0 ? 0 : sweep_res > 100 ? 100 : sweep_res; }
        fx_used = 1;
        sweep_send();
        return;
    }
    switch (set.page) {
    case PG_STRINGS:
        if (k == 0) set.str_att = clamp8(set.str_att + 2 * d, 0, 100);
        else if (k == 1) set.str_rel = clamp8(set.str_rel + 2 * d, 0, 100);
        else if (k == 2) set.str_ens = clamp8(set.str_ens + 5 * d, 0, 100);
        else set.str_ph = clamp8(set.str_ph + 5 * d, 0, 100);
        todo |= G_STRINGS;
        break;
    case PG_LEAD: {
        const arp_patch_t *q = &ARP_PATCHES[LEAD_BASE];
        if (k == 0) set.lead_cut = (int8_t)((int32_t)clamp8(set.lead_cut + 50 + 2 * d, 0, 100) - 50);
        else if (k == 1) set.lead_res = clamp8((set.lead_res == 255u ? q->reso : set.lead_res) + 2 * d, 0, 100);
        else if (k == 2) set.lead_glide = clamp8((set.lead_glide == 255u ? q->glide : set.lead_glide) + 2 * d, 0, 100);
        else { set.lvl[1] = clamp8(set.lvl[1] + 5 * d, 0, 100); todo |= G_MIX; break; }
        todo |= G_LEAD;
        break;
    }
    case PG_BASS:
        if (k == 0) { set.bass_cut = (int8_t)((int32_t)clamp8(set.bass_cut + 50 + 2 * d, 0, 100) - 50); todo |= G_BASS; }
        else if (k == 1) return;                /* the sweep: not in this version */
        else if (k == 2) { int32_t v = (int32_t)set.tempo + 2 * d; set.tempo = (uint16_t)(v < 40 ? 40 : v > 240 ? 240 : v); todo |= G_TEMPO; }
        else { set.lvl[2] = clamp8(set.lvl[2] + 5 * d, 0, 100); todo |= G_MIX; }
        break;
    case PG_DRUMS:
        if (k == 0) { int32_t v = (int32_t)set.tempo + 2 * d; set.tempo = (uint16_t)(v < 40 ? 40 : v > 240 ? 240 : v); todo |= G_TEMPO; break; }
        else if (k == 1) { set.lvl[3] = clamp8(set.lvl[3] + 5 * d, 0, 100); todo |= G_MIX; break; }
        else if (k == 2) set.rh_balance = clamp8(set.rh_balance + 5 * d, 0, 100);
        else set.rh_extra = clamp8(set.rh_extra + 10 * d, 0, 100);
        todo |= G_DRUMS;
        break;
    case PG_CHORDS: {
        chord_t *c = &set.chord[set.cursor];
        if (k == 0) { c->root = (uint8_t)(((int32_t)c->root + d) % 12 + 12) % 12; if (c->kind == CH_NONE) c->kind = CH_MIN; }
        else if (k == 1) c->kind = clamp8((int32_t)c->kind + d, 0, N_KINDS - 1);
        else if (k == 2) set.str_oct = clamp8(set.str_oct + d, 0, 4);
        else { set.lvl[0] = clamp8(set.lvl[0] + 5 * d, 0, 100); todo |= G_MIX; break; }
        if (cur_link == set.cursor) cur_link = 0xffu;   /* (the chord sounding changed: again) */
        todo |= G_BANK;
        break;
    }
    case PG_MIX:
        set.lvl[k] = clamp8(set.lvl[k] + 5 * d, 0, 100);
        todo |= G_MIX;
        break;
    case PG_PARTS:
        if (k == 0) { set.lvl_wind = clamp8(set.lvl_wind + 5 * d, 0, 100); break; }   /* wind_sweep sends it */
        else if (k == 1) { set.wind_col = clamp8(set.wind_col + 5 * d, 0, 100); todo |= G_FX; break; }
        else if (k == 2) set.lvl_fx = clamp8(set.lvl_fx + 5 * d, 0, 100);
        else { set.ec_lead = clamp8(set.ec_lead + 5 * d, 0, 100); todo |= G_ECHO; break; }
        todo |= G_MIX;
        break;
    case PG_FXSEQ: {
        md_band_fx_event_t *e = &set.fx[fx_cur % MD_BAND_FX_EVENTS];
        if (k == 0) e->at = clamp8((int32_t)(e->at % FX_SPAN) + d, 0, (int32_t)FX_SPAN - 1);
        else if (k == 1) {
            uint8_t was = e->sound;
            e->sound = clamp8((int32_t)e->sound + d, 0, N_FX_SOUNDS - 1);
            if (!was && e->sound) { if (!e->chance) e->chance = 100; if (!e->degree) e->degree = 10; }   /* a free slot: a note to hear */
        }
        else if (k == 2) e->degree = clamp8((int32_t)e->degree + d, 0, MD_BAND_FX_DEGREES - 1);
        else e->chance = clamp8((int32_t)e->chance + 5 * d, 0, 100);
        if (k == 1 || k == 2) fx_audition();
        todo |= G_BANK;
        break;
    }
    default:
        if (k == 0) set.ec_fb = clamp8(set.ec_fb + 5 * d, 0, 100);
        else if (k == 1) set.ec_tone = clamp8(set.ec_tone + 5 * d, 0, 100);
        else if (k == 2) set.ec_str = clamp8(set.ec_str + 5 * d, 0, 100);
        else set.ec_lead = clamp8(set.ec_lead + 5 * d, 0, 100);
        todo |= G_ECHO;
        break;
    }
    touch();
}
static void algo(int32_t d)                     /* ALGORITHM on the page */
{
    switch (set.page) {
    case PG_STRINGS: str_voice(clamp8((int32_t)set.str_reg + d, 0, STR_VOICES - 1)); todo |= G_SCENE | G_STRINGS; break;
    case PG_LEAD: lead_select((int32_t)lead_pos() + d); break;
    case PG_BASS: set.bass_patch = clamp8((int32_t)set.bass_patch + d, 0, ARP_N_PATCHES - 1); todo |= G_BASS_SOUND; break;
    case PG_DRUMS: {                            /* one rhythm of the twenty */
        uint32_t k;
        int32_t v;
        for (k = 0; k < 10u && !(set.rh_keys >> k & 1u); k++)
            ;
        v = (int32_t)(k < 10u ? k + 10u * set.rh_section : 6u) + d;
        v = v < 0 ? 0 : v > 19 ? 19 : v;
        set.rh_keys = (uint16_t)(1u << (v % 10));
        set.rh_section = (uint8_t)(v / 10);
        todo |= G_BANK;
        break;
    }
    case PG_CHORDS: set.cursor = (uint8_t)((((int32_t)set.cursor + d) % ARP_CHAIN) + ARP_CHAIN) % ARP_CHAIN; break;
    case PG_MIX: set.reverb = clamp8(set.reverb + 5 * d, 0, 100); todo |= G_MIX; break;
    case PG_PARTS: set.lead_rev = clamp8(set.lead_rev + 5 * d, 0, 100); todo |= G_MIX; break;
    case PG_FXSEQ: fx_pattern_to(d); break;
    default: set.ec_div = clamp8((int32_t)set.ec_div + d, 0, 6); todo |= G_ECHO; break;
    }
    touch();
}
static void ui(uint32_t ev, int32_t arg)
{
    uint32_t id = HB_EV_ID(ev);
    switch (HB_EV_KIND(ev)) {
    case HB_EV_KEY_DOWN: key_down(id, arg); return;
    case HB_EV_KEY_UP: key_up(id); return;
    case HB_EV_BTN_DOWN:
        switch (id) {
        case HB_BTN_EDIT: page_to(PG_STRINGS); break;
        case HB_BTN_GLO: page_to(PG_LEAD); break;
        case HB_BTN_ENV: page_to(PG_BASS); break;
        case HB_BTN_LFO: page_to(PG_DRUMS); break;
        case HB_BTN_SEQ: page_to(PG_CHORDS); break;
        case HB_BTN_FX: fx_held = 1; fx_used = 0; break;   /* the page changes on release, if no knob was turned */
        case HB_BTN_SEL:
            if (set.page == PG_PARTS) {          /* the background parts round: none, the box's run, the lead's phrase, both */
                uint32_t bg = ((uint32_t)set.amb_arp | (uint32_t)set.phrase << 1) + 1u;   /* (then each phrase, without and with) */
                if (running) band(0);
                set.amb_arp = (uint8_t)(bg & 1u); set.phrase = (uint8_t)((bg >> 1) % (MD_BAND_PHRASES + 1u));
                todo |= G_BANK;
                touch();
            } else solo_toggle();
            break;
        case HB_BTN_PLAY: band(!running); break;
        case HB_BTN_REC: set.latch = !set.latch; if (set.latch) page_to(PG_CHORDS); touch(); break;
        case HB_BTN_ARP:                         /* FX SEQ: how the event is played, round, and heard; else: all notes off */
            if (set.page == PG_FXSEQ && set.fx[fx_cur % MD_BAND_FX_EVENTS].sound) {
                md_band_fx_event_t *e = &set.fx[fx_cur % MD_BAND_FX_EVENTS];
                e->how = (uint8_t)((e->how + 1u) % MD_BAND_FX_HOWS);
                todo |= G_BANK;                  /* (ONCE is in the pattern, the others are not) */
                fx_audition();
                touch();
            } else notes_off();
            break;
        case HB_BTN_SAVE: save_all(); break;
        case HB_BTN_OCT_DOWN:
            if (set.page == PG_DRUMS) { set.rh_section = 0; todo |= G_BANK; }
            else if (set.oct > 0) set.oct--;
            touch();
            break;
        case HB_BTN_OCT_UP:
            if (set.page == PG_DRUMS) { set.rh_section = 1; todo |= G_BANK; }
            else if (set.oct < 6u) set.oct++;
            touch();
            break;
        default: break;
        }
        return;
    case HB_EV_BTN_UP:
        if (id == HB_BTN_FX) {
            if (fx_held && !fx_used)             /* round: MIX, PARTS, FX SEQ, ECHO */
                page_to(set.page == PG_MIX ? PG_PARTS : set.page == PG_PARTS ? PG_FXSEQ : set.page == PG_FXSEQ ? PG_ECHO : PG_MIX);
            fx_held = 0;
        }
        return;
    case HB_EV_ENC:
        switch (id) {
        case HB_ENC_ALGORITHM: algo(arg); return;
        case HB_ENC_PRESETS:                     /* on every page: the setup, as far as it was turned, round them all */
            if (arg) setup_to((uint32_t)((((int32_t)set.setup + arg) % (int32_t)N_SETUPS + (int32_t)N_SETUPS) % (int32_t)N_SETUPS));
            return;
        case HB_ENC_SELECT:
            if (set.page == PG_CHORDS) algo(arg);
            else if (set.page == PG_LEAD) { set.run = clamp8((int32_t)set.run + arg, 0, N_RUNS - 1); touch(); }
            else if (set.page == PG_BASS && !AMBIENT && arg) {   /* the 1601 sequence (no sequences in AMBIENT) */
                set.bass_seq = clamp8((int32_t)set.bass_seq + arg, 0, ARP_N_SEQS - 1); set.bass_sweep = 255;
                set.tempo = ARP_SEQS[set.bass_seq].tempo; todo |= G_BANK | G_TEMPO; touch();
            }
            else if (set.page == PG_PARTS) { set.fx_speed = (int8_t)((int32_t)clamp8(set.fx_speed + 4 + arg, 0, 8) - 4); touch(); }
            else if (set.page == PG_FXSEQ && arg) {   /* the event under the cursor, heard as it is chosen */
                fx_cur = clamp8((int32_t)fx_cur + arg, 0, MD_BAND_FX_EVENTS - 1);
                fx_audition();
            }
            else if (set.page == PG_ECHO && arg) { set.hall = clamp8((int32_t)set.hall + arg, 0, N_HALLS - 1); todo |= G_SCENE; touch(); }
            else if (set.page == PG_MIX) {
                uint8_t was_on = set.motion != 0;
                set.motion = clamp8(set.motion + 5 * arg, 0, 100);
                if (was_on != (set.motion != 0)) todo |= G_SCENE;   /* the strings' filter in or out in BAND */
                touch();
            }
            return;
        default:
            if (id >= HB_ENC_KNOB1 && id <= HB_ENC_KNOB4) knob(id - HB_ENC_KNOB1, arg);
            return;
        }
    default: return;
    }
}
/* An SMK-37 (or any controller): its 16 pads (notes 36..51, channel 10) play the band.
 *   bottom row 36..40  mute / unmute bass, lead, strings, drums, the effects box; 41 everyone back (mutes, solo)
 *              42      the scene (AMBIENT / BAND); 43 PLAY / STOP
 *   top row    44..48  the effects box's hits: drop, ping, bubble, chirp, metal (with the pad's velocity)
 *              49      a gust: the wind at full while held; 50, 51 two glass bells (notes 84, 91 on the box) */
static void pad(uint32_t k, uint32_t vel, md_source_id_t source)
{
    static const uint8_t BELL[2] = {84, 91};
    if (k == 13u) { wind_gust = vel != 0; return; }
    if (k >= 14u) {
        md_note_token_t *slot = &midi_token[9][36u + k];
        if (*slot) (void)md_session_note_off(&S, T_FX, source, *slot);
        *slot = 0;
        if (vel) (void)md_session_note_on(&S, T_FX, source, BELL[k - 14u], (uint8_t)vel, 0, slot);
        return;
    }
    if (!vel) return;                            /* the rest act on the hit */
    if (k < 5u) {
        uint8_t on = (uint8_t)!(muted >> k & 1u);
        if (!command(MD_CMD_MUTE, (uint8_t)k, 0, on)) muted = (uint8_t)(on ? muted | 1u << k : muted & ~(1u << k));
    } else if (k == 5u) {
        uint32_t t;
        for (t = 0; t < MD_TRACKS; t++) if ((muted >> t & 1u) && !command(MD_CMD_MUTE, (uint8_t)t, 0, 0)) muted &= (uint8_t)~(1u << t);
        if (solo_track != 0xffu && !command(MD_CMD_SOLO, solo_track, 0, 0)) solo_track = 0xffu;
    } else if (k == 6u) setup_to(set.setup + 1u);   /* the next setup */
    else if (k == 7u) band(!running);
    else {
        md_command_t c = {.kind = MD_CMD_EVENT};
        c.event = (md_event_t){.kind = MD_EVENT_HIT, .track = T_FX, .source = source};
        c.event.data.hit.voice = (uint16_t)(k - 8u);
        c.event.data.hit.velocity = (uint8_t)vel;
        (void)md_session_submit(&S, &c);
    }
    touch();
}
static void midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint32_t ch = st & 15u;
    md_source_id_t source = (md_source_id_t)(MD_SOURCE_MIDI_BASE + ch);
    d1 &= 127u;
    switch (st & 0xF0u) {
    case 0x90:
        if (ch == 9u && d1 >= 36u && d1 <= 51u) { pad(d1 - 36u, d2, source); break; }
        if (d2) { lead_on((uint8_t)d1, (uint8_t)d2, source, &midi_token[ch][d1]); break; }
        lead_off(source, &midi_token[ch][d1]);   /* velocity 0 is off */
        break;
    case 0x80:
        if (ch == 9u && d1 >= 36u && d1 <= 51u) { pad(d1 - 36u, 0, source); break; }
        lead_off(source, &midi_token[ch][d1]);
        break;
    case 0xC0: if (d1 < N_LEAD_SOUNDS) { lead_select((int32_t)d1); touch(); } break;   /* 0 SOFT, 1 HARP, 2..11 */
    case 0xB0:
        if (d1 == 74u) { set.lead_cut = (int8_t)((int32_t)d2 * 100 / 127 - 50); todo |= G_LEAD; touch(); }
        else if (d1 == 7u) { set.lvl[1] = (uint8_t)(d2 * 100u / 127u); todo |= G_MIX; touch(); }
        else if (d1 == 123u || d1 == 120u) notes_off();
        else if (d1 >= 70u && d1 <= 73u) {       /* the SMK-37's faders: strings, bass, drums, the effects box */
            uint8_t v = (uint8_t)(d2 * 100u / 127u);
            if (d1 == 70u) set.lvl[0] = v; else if (d1 == 71u) set.lvl[2] = v; else if (d1 == 72u) set.lvl[3] = v;
            else set.lvl_fx = v;
            todo |= G_MIX; touch();
        }
        else if (d1 == 1u) {                     /* the mod wheel closes the master low-pass */
            sweep_cut = 990 - (int32_t)d2 * 6;     /* full travel: about 230, dark but not gone */
            sweep_send(); touch();
        }
        else if (d1 == 64u) {                    /* the sustain pedal holds the lead */
            md_command_t c = {.kind = MD_CMD_SUSTAIN, .track = T_LEAD, .value = d2 >= 64u};
            c.event.source = source;
            (void)md_session_submit(&S, &c);
        }
        break;
    default: break;
    }
}
static void note(uint32_t ch, uint32_t key, uint32_t vel) { midi((vel ? 0x90u : 0x80u) | (ch & 15u), key, vel); }
static void cc(uint32_t ch, uint32_t num, uint32_t val) { midi(0xB0u | (ch & 15u), num, val); }

/* ---- the sound: the session renders and mixes everything */
static void render(int32_t *out, uint32_t n)
{
    while (n) {
        uint32_t span = n > MD_MAX_RENDER_FRAMES ? MD_MAX_RENDER_FRAMES : n;
        (void)md_session_render(&S, out, span);
        out += 2u * span;
        n -= span;
    }
}

static int init(const hb_host_t *h)
{
    md_session_config_t c = {0};
    md_band_settings_t b;
    uint32_t k;
    H = h;
    sx_ring = (uint32_t)md_synthex_find("Ring mod.");   /* (the factory bank has it: synthex_test) */
    for (k = 0; k < N_SETUPS; k++) { factory(k); store.s[k] = set; }   /* the factory's, unless bank 1 has them */
    if (HB_HOST_HAS(H, bank_load_n)) {
        static set_t loaded[N_SETUPS];
        uint32_t magic = 0;
        if (H->bank_load_n(1, &store, sizeof store) == (int)sizeof store && store.magic == SETUPS_MAGIC) magic = 1;
        if (!magic) {                            /* none, or another layout: the factory's again */
            for (k = 0; k < N_SETUPS; k++) { factory(k); loaded[k] = set; }
            for (k = 0; k < N_SETUPS; k++) store.s[k] = loaded[k];
        }
    }
    for (k = 0; k < N_SETUPS; k++) { set = store.s[k]; set.setup = (uint8_t)k; sanitize(); store.s[k] = set; }
    if (H->load(&set, sizeof set) || set.magic != SET_MAGIC) {
        set = store.s[0];                        /* DRIFT, on the LEAD page: the keys play over it */
        set.page = PG_LEAD;
    }
    sanitize();
    store.s[set.setup] = set;                    /* the record is the newest of its setup */
#ifdef MD_STRESS_HOOK
    MD_STRESS_HOOK(3, 0);                        /* tests/stress only: the case's scene, before anything is built */
#endif
    /* the instruments and effects, caller-owned; then the session around them */
    if (md_instrument_init(&inst[T_BASS], &MD_ARP2600, &bass_state, sizeof bass_state, MD_SAMPLE_RATE) ||
        md_instrument_init(&inst[T_LEAD], &MD_ARP2600, &lead_state, sizeof lead_state, MD_SAMPLE_RATE) ||
        md_instrument_init(&inst[T_STRINGS], &MD_EMINENT, &strings_state, sizeof strings_state, MD_SAMPLE_RATE) ||
        md_instrument_init(&inst[T_DRUMS], &MD_MINIPOPS, &drums_state, sizeof drums_state, MD_SAMPLE_RATE) ||
        md_effect_init(&echo_fx, &MD_FX_ECHO, &echo_state, sizeof echo_state, MD_SAMPLE_RATE) ||
        md_effect_init(&reverb_fx, &MD_FX_REVERB, &reverb_state, sizeof reverb_state, MD_SAMPLE_RATE) ||
        md_effect_init(&master_fx, &MD_FX_FILTER, &master_state, sizeof master_state, MD_SAMPLE_RATE) ||
        md_effect_init(&warm_fx, &MD_FX_FILTER, &warm_state, sizeof warm_state, MD_SAMPLE_RATE) ||
        md_instrument_init(&inst[T_FX], &MD_ELECTRONIC_FX, &fx_state, sizeof fx_state, MD_SAMPLE_RATE) ||
        md_instrument_init(&sx, &MD_SYNTHEX, &sx_state, sizeof sx_state, MD_SAMPLE_RATE) ||
        md_instrument_init(&ens, &MD_ENSEMBLE, &ens_state, sizeof ens_state, MD_SAMPLE_RATE)) {
        H->log("orrery: an instrument or effect failed to start");
        return 1;
    }
    /* the presets before the session, which reads them as its parameter bases */
    if (md_sound_apply(&inst[T_BASS], MD_ARP_SETTINGS_VERSION, MD_ARP2600.presets[set.bass_patch % ARP_N_PATCHES].settings, MD_ARP_SETTINGS_BYTES) ||
        md_sound_apply(&inst[T_LEAD], MD_ARP_SETTINGS_VERSION, MD_ARP2600.presets[LEAD_BASE].settings, MD_ARP_SETTINGS_BYTES) ||
        md_effect_param(&reverb_fx, MD_REVERB_ALGORITHM, MD_REVERB_HALL) || md_effect_param(&reverb_fx, MD_REVERB_SIZE, 70) ||
        md_effect_param(&reverb_fx, MD_REVERB_DAMPING, 50) ||
        /* the strings' warming filter: a gentle low-pass near 3.5 kHz (AMBIENT; bypassed in BAND) */
        md_effect_param(&warm_fx, MD_FILTER_CUTOFF, 745) || md_effect_param(&warm_fx, MD_FILTER_RESONANCE, 10)) return 1;
    band_settings(&b);
    if (md_band_build(&b, &first_bank, &chain, &report)) return 1;
    for (k = 0; k < MD_TRACKS; k++) c.instrument[k] = &inst[k];
    c.bus[MD_MIX_BUS_ECHO] = &echo_fx;
    c.bus[MD_MIX_BUS_REVERB] = &reverb_fx;
    c.master = &master_fx;                       /* the master sweep: bypassed until a page asks for it */
    c.insert[T_STRINGS][0] = &warm_fx;
    c.alternate[T_LEAD] = &sx;
    c.alternate[T_STRINGS] = &ens;
    /* five parts are over one core's budget: some render on the second core
     * while this one renders the rest; the session does it here when the core is not available */
#ifdef ORRERY_ONE_CORE                        /* (a timing build: every part's own cost, in fm1-emu too) */
    if (0) {
#else
    if (HB_HOST_HAS(H, c1_wait)) {
#endif
        c.parallel.run = H->c1_run;
        c.parallel.wait = H->c1_wait;
        c.parallel_track = T_STRINGS;
        /* the parts shared between the cores by what each costs, block by block (the session, with the host's
         * counter): a setup's dear part is not always the same one (the Synthex as the lead, a wave with gulls
         * from the effects box), and fm1-emu cannot tell (its second core costs next to nothing). They start
         * with the strings and the box on core 1. Any part may go to either core: there is one Eminent, one
         * ENSEMBLE and one Synthex, and the 2600 keeps nothing outside its state. */
#ifndef ORRERY_CORE1
#define ORRERY_CORE1 ((1u << T_STRINGS) | (1u << T_FX))
#endif
        c.parallel_mask = (uint8_t)(ORRERY_CORE1);
#ifndef ORRERY_FIXED_CORES
        if (HB_HOST_HAS(H, ticks)) { c.parallel.ticks = H->ticks; c.parallel_balance = 1; }
#endif
    } else c.parallel_track = 0xffu;
    c.bank = &first_bank;
    c.bpm_q8 = (uint16_t)(set.tempo * 256u);
#ifdef MD_STRESS_HOOK
    MD_STRESS_HOOK(0, &c);                       /* tests/stress only: change bindings before the session */
#endif
    if (md_session_init(&S, &c)) { H->log("orrery: the session failed to start"); return 1; }
    reverb_room();
    for (k = 0; k < 27u; k++) key_token[k] = 0;
    for (k = 0; k < 16u * 128u; k++) midi_token[k / 128u][k % 128u] = 0;
    n_chord_on = 0;
    running = 0;
    cur_link = 0xffu;
    dirty = 0;
    wind_sent = -1;
    todo = G_SCENE | G_LEAD | G_BASS | G_STRINGS | G_DRUMS | G_FX | G_MIX | G_ECHO;
    pump();                                      /* the ring is empty: all of it fits before the first block */
#ifdef MD_STRESS_HOOK
    MD_STRESS_HOOK(1, &c);                       /* tests/stress only: the stress case's bank, notes and start */
#endif
    H->log("orrery: strings, lead, bass, drums on one session");
    return 0;
}
static void release(void)
{
    md_command_t p = {.kind = MD_CMD_EVENT};
    p.event = (md_event_t){.kind = MD_EVENT_PANIC, .track = MD_ALL_TRACKS, .source = MD_SOURCE_NONE};
    (void)md_session_submit(&S, &p);
}

HB_DESC = {HB_ABI, "ORRERY", init, release, note, cc, render, ui, draw, 0, 0, HB_F_SCREEN | HB_F_PANEL, midi};
