/* SPDX-License-Identifier: GPL-3.0-only */
/* ORRERY's factory content, the one place a style set is written: the effect patterns (what the effects box plays
 * by itself) and the setups (a scene each: key, chords, voices, levels, echo, hall, effect pattern). Included by
 * orrery.c only. Everything a player sees is a descriptive name: no artist, record or track title.
 *
 * To change a setup, edit its block in SETUPS; to add one, add a block and raise N_SETUPS (the setups' bank takes
 * up to 60 of them; a bank saved with another count is not loaded). To change what the box plays, edit a pattern or add one (and its FXP_ name). Then listen:
 *     uv run --with numpy --with pillow tools/listen.py [SETUP ...] --png
 * prints every part's level and pitch classes and where the energy sits, without Docker or the emulator. A setup
 * should keep one part in front (the strings here), the bass 3-6 dB under it, the box and the wind under that, and
 * every pitched part on the setup's key. On the device the same things are edited on the pages (the effect sequence
 * on FX SEQ) and kept by SAVE; a saved bank is used instead of this table until the layout changes. */
#ifndef ORRERY_SETUPS_H
#define ORRERY_SETUPS_H

/* ---- names for the table (the numbers the engines use) */
enum { nC, nCs, nD, nEb, nE, nF, nFs, nG, nAb, nA, nBb, nB };                 /* chord roots */
enum { BAND = MD_BAND_SCENE_BAND, AMB = MD_BAND_SCENE_AMBIENT };
/* the strings' voice: the Eminent's four registrations, then the fourteen ENSEMBLE pads (ens_engine.c's order) */
enum { V_STRINGS, V_CELLO, V_VIOLIN, V_FULL, V_VINTAGE_STRINGS, V_GLASS_CHOIR, V_DUSK_ORCHESTRA, V_ABYSS_PAD, V_PROFILE_A,
       V_STRING_MACHINE, V_BREATH_CHOIR, V_DEEP_DRONE, V_GLASS_SHIMMER, V_SLOW_SWELL, V_ROUGH_PULSE, V_SOLO_DRIFT,
       V_HUGE_ENSEMBLE, V_RUNNING_PAD };
#define PAD_OWN 255u                                 /* a strings knob: the pad's own value */
/* the 2600's patches (arp_presets.h's order): the bass's, and the lead's as L_2600(P_...) */
enum { P_WARM_BASS, P_PLUCK_SEQ, P_GLASS_PLUCK, P_ARP_LEAD, P_LASER_SYNC, P_SH_SPACE, P_PERCUSSIVE, P_SEQ_BASS, P_SUB_BASS,
       P_TWIN_SAWS };
enum { L_SOFT, L_HARP };                         /* the lead: SOFT LEAD, LASER HARP (the Synthex), a 2600 patch, or */
#define L_2600(patch) (2u + (patch))
/* one of the Synthex's other factory sounds by its number there (sc_presets: 4 Pan flutes, 28 Chimes, 29 Vibes,
 * 40 Cathedral choir, 51 Solo Violin, 75 Filtered brass: a brass that swells in half a second, ...; not 36) */
#define L_SX(n) (12u + ((n) < 36u ? (n) : (n) - 1u))
enum { PH_NONE, PH_HARP, PH_LONG, PH_BELLS, PH_CALLS };   /* AMBIENT's lead phrase (band.h) */
/* the halls (orrery.c's HALLS): the reverb's kind, size and damping as one choice */
enum { H_ROOM, H_PLATE, H_HALL, H_BIG_HALL, H_DARK_HALL, H_CATHEDRAL, H_VAST, H_ENDLESS };
enum { Q_E_MIN_ARP, Q_ROOT_PULSE, Q_OCTAVE_ECHO, Q_C_MIN_PULSE, Q_BREATHING, Q_SLOW_TIES, Q_OCTAVE_JAB, Q_UP_AND_DOWN };
/* the Mini Pops: no rhythm, lever A's ten, lever B's ten */
enum { R_NONE, R_WALTZ, R_SAMBA, R_MAMBO, R_RUMBA, R_TANGO, R_BOSSA, R_ROCK1, R_ROCK3, R_MARCH, R_SWING, R_JAZZ_WALTZ,
       R_MERINGUE, R_CHACHA, R_BEGUINE, R_HABANERA, R_SLOW_ROCK, R_ROCK2, R_ROCK4, R_MARCH68, R_LATIN_SWING };
enum { X_TAMBOURINE = 1, X_GUIRO = 2, X_QUIJADA = 4 };
enum { E_OFF, E_16TH, E_8TH, E_8TH_DOT, E_4TH, E_4TH_DOT, E_HALF };            /* the echo's time, of the tempo */

/* ---- the effect patterns: sixteen events at most. An event is a sound and how it is played:
 *     EV(bar 1..16, eighth 1..8, sound, degree 0..14, chance %)         the sound once, on its step
 *     RUN(bar, eighth, sound, how, degree, chance %)                    a gesture of the sound from that note: H_UP the
 *                                                                       scale upwards, H_DOWN the same downwards, H_ARC
 *                                                                       up and back, H_SPARKLE its notes at random
 * A struck sound's gesture is eleven hits over two octaves in sixteenths (an arc twenty-one, back within the bar); a
 * long sound's is a few, well apart (three whistles, four gulls, two waves: orrery.c's FX_GESTURE).
 * A degree is a step of the setup's pentatonic from the C4 octave (0, 5 and 10 its root; 14 the top), so a pattern
 * is in key in every setup. BAND plays bars 1..8 and folds 9..16 onto them. The sounds ring out as the box's own
 * recipes. Struck: DROP a falling sine droplet, PING a glass bell, BUBBLE a wobbling low bloop, CHIRP a quick rising
 * call, METAL a ring-modulated clang, ZAP a bright shot falling three octaves onto its note. Long (they swell and fall
 * away, and travel across the stereo): WHISTLE a tone gliding two and a half seconds down to its note, GULL a cry,
 * TWITTER a second and a half of an electronic bird, BLEEPS a second of sample-and-hold steps; and of noise, where
 * the degree is the colour, low to high: WHOOSH a resonant sweep upwards, SURF a wave coming in and going out (four
 * seconds), THUNDER a crack falling into a long roll (low degrees: the deepest). Where they sit: a ZAP or a WHISTLE on
 * degrees 5..10, a GULL 9..13, a TWITTER 10..14, BLEEPS 5..10, the noises 0..8. */
enum { FX_NONE, FX_DROP, FX_PING, FX_BUBBLE, FX_CHIRP, FX_METAL, FX_ZAP, FX_WHISTLE, FX_GULL, FX_TWITTER, FX_BLEEPS,
       FX_WHOOSH, FX_SURF, FX_THUNDER, N_FX_SOUNDS };
enum { H_ONCE = MD_BAND_FX_ONCE, H_UP = MD_BAND_FX_UP, H_DOWN = MD_BAND_FX_DOWN, H_ARC = MD_BAND_FX_ARC, H_SPARKLE = MD_BAND_FX_SPARKLE };
#define EV(bar, eighth, sound, degree, chance) {(uint8_t)(((bar) - 1) * 8 + (eighth) - 1), sound, degree, chance, H_ONCE}
#define RUN(bar, eighth, sound, how, degree, chance) {(uint8_t)(((bar) - 1) * 8 + (eighth) - 1), sound, degree, chance, how}
enum { FXP_EMPTY, FXP_DROPLETS, FXP_SPARSE, FXP_RAIN, FXP_BELLS, FXP_BUBBLES, FXP_SIGNALS, FXP_CAVERN, FXP_SHORE, FXP_STORM,
       FXP_BIRDS, FXP_COMPUTER, FXP_LASERS, FXP_SPACE, FXP_SCALES, FXP_CASCADES, FXP_CITY, FXP_FROST, FXP_GONGS, N_FX_PATTERNS };
typedef struct { const char *name; md_band_fx_event_t ev[MD_BAND_FX_EVENTS]; } fx_pattern_t;
static const fx_pattern_t FX_PATTERNS[N_FX_PATTERNS] = {
    [FXP_EMPTY] = {"EMPTY", {{0, 0, 0, 0, 0}}},
    /* high droplets seven times in ten, a rising call before each eight bars end */
    [FXP_DROPLETS] = {"DROPLETS", {
        EV(1, 7, FX_DROP, 11, 70), EV(2, 5, FX_DROP, 13, 70), EV(3, 5, FX_DROP, 10, 70), EV(4, 3, FX_DROP, 12, 70),
        EV(6, 3, FX_DROP, 10, 70), EV(7, 1, FX_DROP, 14, 70), EV(7, 7, FX_DROP, 11, 70), EV(8, 5, FX_CHIRP, 10, 80),
        EV(9, 5, FX_DROP, 12, 70), EV(11, 3, FX_DROP, 9, 70), EV(12, 1, FX_DROP, 11, 70), EV(12, 7, FX_DROP, 13, 70),
        EV(14, 7, FX_DROP, 10, 70), EV(16, 5, FX_CHIRP, 12, 80)}},
    /* a few drops and two bells: room for the player */
    [FXP_SPARSE] = {"SPARSE", {
        EV(2, 6, FX_DROP, 12, 60), EV(4, 1, FX_PING, 9, 80), EV(7, 4, FX_DROP, 10, 60), EV(10, 7, FX_DROP, 13, 60),
        EV(12, 1, FX_PING, 7, 80), EV(15, 3, FX_DROP, 11, 60)}},
    /* all sixteen: drops off the beat, never the same bar twice */
    [FXP_RAIN] = {"RAIN", {
        EV(1, 2, FX_DROP, 12, 75), EV(1, 7, FX_DROP, 9, 60), EV(2, 5, FX_DROP, 14, 70), EV(3, 3, FX_DROP, 10, 65),
        EV(4, 1, FX_DROP, 13, 75), EV(4, 6, FX_DROP, 8, 55), EV(5, 4, FX_DROP, 11, 70), EV(6, 2, FX_DROP, 14, 60),
        EV(7, 1, FX_DROP, 9, 75), EV(7, 6, FX_DROP, 12, 65), EV(9, 3, FX_DROP, 10, 70), EV(10, 1, FX_DROP, 13, 60),
        EV(11, 5, FX_DROP, 8, 75), EV(12, 4, FX_DROP, 11, 65), EV(14, 2, FX_DROP, 14, 70), EV(15, 6, FX_DROP, 9, 60)}},
    /* a slow bell line on the bars' ones and the middles, two high drops answering */
    [FXP_BELLS] = {"BELLS", {
        EV(1, 1, FX_PING, 5, 90), EV(2, 5, FX_PING, 7, 90), EV(3, 1, FX_PING, 8, 90), EV(5, 1, FX_PING, 7, 90),
        EV(6, 5, FX_PING, 9, 90), EV(7, 1, FX_PING, 10, 90), EV(8, 7, FX_DROP, 13, 50), EV(9, 1, FX_PING, 5, 90),
        EV(10, 5, FX_PING, 8, 90), EV(11, 1, FX_PING, 7, 90), EV(13, 1, FX_PING, 9, 90), EV(14, 5, FX_PING, 7, 90),
        EV(15, 1, FX_PING, 5, 90), EV(16, 7, FX_DROP, 12, 50)}},
    /* low bubbles in twos and threes every four bars, a drop between */
    [FXP_BUBBLES] = {"BUBBLES", {
        EV(2, 1, FX_BUBBLE, 2, 75), EV(2, 3, FX_BUBBLE, 4, 75), EV(2, 4, FX_BUBBLE, 3, 75), EV(4, 6, FX_DROP, 12, 60),
        EV(6, 5, FX_BUBBLE, 1, 75), EV(6, 7, FX_BUBBLE, 3, 75), EV(8, 3, FX_DROP, 10, 60), EV(10, 1, FX_BUBBLE, 4, 75),
        EV(10, 2, FX_BUBBLE, 2, 75), EV(10, 4, FX_BUBBLE, 5, 75), EV(12, 7, FX_DROP, 13, 60), EV(14, 5, FX_BUBBLE, 3, 75),
        EV(14, 7, FX_BUBBLE, 1, 75), EV(16, 2, FX_DROP, 11, 60)}},
    /* a call and a bell's answer every four bars, far drops */
    [FXP_SIGNALS] = {"SIGNALS", {
        EV(1, 5, FX_CHIRP, 10, 80), EV(1, 7, FX_PING, 8, 80), EV(3, 4, FX_DROP, 11, 50), EV(5, 5, FX_CHIRP, 12, 80),
        EV(5, 7, FX_PING, 9, 80), EV(7, 2, FX_DROP, 13, 50), EV(9, 5, FX_CHIRP, 10, 80), EV(9, 7, FX_PING, 7, 80),
        EV(11, 6, FX_DROP, 9, 50), EV(13, 5, FX_CHIRP, 13, 80), EV(13, 7, FX_PING, 10, 80), EV(15, 4, FX_DROP, 12, 50)}},
    /* metal on every fourth bar's one, a low bubble between, a bell and a drop far off */
    [FXP_CAVERN] = {"CAVERN", {
        EV(1, 1, FX_METAL, 2, 90), EV(3, 5, FX_BUBBLE, 0, 70), EV(4, 8, FX_DROP, 13, 40), EV(5, 1, FX_METAL, 4, 90),
        EV(7, 3, FX_BUBBLE, 2, 70), EV(8, 1, FX_PING, 6, 60), EV(9, 1, FX_METAL, 1, 90), EV(11, 7, FX_BUBBLE, 1, 70),
        EV(12, 2, FX_DROP, 14, 40), EV(13, 1, FX_METAL, 3, 90), EV(15, 5, FX_BUBBLE, 3, 70), EV(16, 1, FX_PING, 5, 60)}},
    /* the sea: a wave every two or three bars, each a different colour, gulls in twos between them, an arc of glass
     * up and back twice */
    [FXP_SHORE] = {"SHORE", {
        EV(1, 1, FX_SURF, 4, 100), EV(3, 5, FX_SURF, 6, 90), EV(4, 3, FX_GULL, 12, 70), EV(4, 6, FX_GULL, 11, 60),
        EV(6, 1, FX_SURF, 3, 100), EV(8, 3, FX_BUBBLE, 2, 50), EV(9, 1, FX_SURF, 5, 100), EV(10, 7, FX_GULL, 13, 70),
        EV(11, 2, FX_GULL, 12, 50), EV(11, 5, FX_SURF, 7, 90), EV(13, 5, FX_DROP, 12, 50), EV(14, 1, FX_SURF, 4, 100),
        EV(15, 6, FX_GULL, 11, 60), EV(16, 2, FX_GULL, 10, 40), RUN(2, 1, FX_PING, H_ARC, 5, 80), RUN(12, 1, FX_PING, H_ARC, 6, 80)}},
    /* thunder three times in sixteen bars, not always; rain between, a gust */
    [FXP_STORM] = {"STORM", {
        EV(1, 1, FX_THUNDER, 2, 90), EV(1, 6, FX_DROP, 12, 70), EV(2, 3, FX_DROP, 9, 60), EV(3, 2, FX_DROP, 13, 70),
        EV(3, 7, FX_DROP, 10, 60), EV(4, 5, FX_WHOOSH, 3, 60), EV(5, 4, FX_DROP, 11, 70), EV(6, 2, FX_DROP, 14, 60),
        EV(7, 1, FX_THUNDER, 5, 70), EV(7, 6, FX_DROP, 9, 65), EV(9, 3, FX_DROP, 12, 70), EV(10, 5, FX_DROP, 8, 60),
        EV(11, 1, FX_DROP, 13, 65), EV(12, 3, FX_THUNDER, 0, 80), EV(13, 6, FX_DROP, 10, 70), EV(15, 2, FX_DROP, 11, 60)}},
    /* the bell line on the odd bars' ones, birds answering in the even bars */
    [FXP_BIRDS] = {"BIRDS", {
        EV(1, 1, FX_PING, 5, 90), EV(2, 3, FX_TWITTER, 12, 70), EV(3, 1, FX_PING, 8, 90), EV(4, 5, FX_TWITTER, 10, 60),
        EV(5, 1, FX_PING, 7, 90), EV(6, 4, FX_TWITTER, 13, 60), EV(7, 1, FX_PING, 9, 90), EV(8, 6, FX_DROP, 13, 50),
        EV(9, 1, FX_PING, 5, 90), EV(10, 2, FX_TWITTER, 11, 70), EV(11, 1, FX_PING, 7, 90), EV(12, 6, FX_TWITTER, 14, 50),
        EV(13, 1, FX_PING, 8, 90), EV(14, 4, FX_TWITTER, 12, 60), EV(15, 1, FX_PING, 5, 90)}},
    /* a machine room: bleeps every other bar, a sweep, a whistle falling, far calls, a scale of glass up and one down */
    [FXP_COMPUTER] = {"COMPUTER", {
        EV(1, 3, FX_BLEEPS, 7, 80), EV(2, 7, FX_PING, 9, 60), EV(3, 5, FX_WHOOSH, 2, 70), EV(5, 1, FX_BLEEPS, 5, 70),
        EV(6, 3, FX_WHISTLE, 8, 60), EV(7, 6, FX_BLEEPS, 9, 60), EV(8, 5, FX_CHIRP, 10, 60), EV(9, 3, FX_BLEEPS, 6, 80),
        EV(10, 6, FX_PING, 7, 60), EV(11, 5, FX_WHOOSH, 4, 70), EV(13, 1, FX_BLEEPS, 8, 70), EV(14, 4, FX_WHISTLE, 6, 50),
        EV(15, 5, FX_BLEEPS, 5, 60), EV(16, 6, FX_ZAP, 9, 40), RUN(4, 1, FX_PING, H_UP, 5, 80), RUN(12, 1, FX_PING, H_DOWN, 5, 80)}},
    /* shots in twos off the beat, a sweep into the fifth and the ninth bar, nothing that holds a pitch: for a band
     * whose line changes key (few, it loops in eight) */
    [FXP_LASERS] = {"LASERS", {
        EV(2, 7, FX_ZAP, 6, 70), EV(2, 8, FX_ZAP, 6, 50), EV(4, 5, FX_WHOOSH, 3, 60), EV(6, 7, FX_ZAP, 8, 70),
        EV(7, 1, FX_ZAP, 5, 50), EV(8, 3, FX_WHOOSH, 5, 70), EV(10, 3, FX_ZAP, 10, 40), EV(12, 7, FX_CHIRP, 10, 40),
        EV(16, 1, FX_ZAP, 6, 40)}},
    /* slow and far: sweeps and falling whistles in turn, a bell, a shot, a bubble, metal once */
    [FXP_SPACE] = {"SPACE", {
        EV(1, 5, FX_WHOOSH, 2, 80), EV(3, 3, FX_WHISTLE, 9, 70), EV(4, 7, FX_PING, 7, 50), EV(6, 1, FX_WHOOSH, 5, 70),
        EV(7, 5, FX_WHISTLE, 6, 60), EV(8, 6, FX_ZAP, 10, 40), EV(9, 5, FX_WHOOSH, 3, 80), EV(11, 1, FX_WHISTLE, 8, 70),
        EV(12, 5, FX_BUBBLE, 2, 50), EV(13, 7, FX_WHOOSH, 6, 70), EV(15, 3, FX_WHISTLE, 5, 60), EV(16, 5, FX_METAL, 3, 40)}},
    /* scales of glass in every second bar: up, then down, from a different note each time; an arc to end each eight */
    [FXP_SCALES] = {"SCALES", {
        RUN(2, 1, FX_PING, H_UP, 5, 100), RUN(4, 1, FX_PING, H_DOWN, 5, 100), RUN(6, 1, FX_PING, H_UP, 6, 100),
        RUN(8, 1, FX_PING, H_ARC, 5, 100), RUN(10, 1, FX_PING, H_DOWN, 6, 100), RUN(12, 1, FX_PING, H_UP, 7, 100),
        RUN(14, 1, FX_PING, H_DOWN, 5, 100), RUN(16, 1, FX_PING, H_ARC, 5, 100)}},
    /* gestures of other sounds: droplets falling and rising, a sparkle of glass, shots walking down, gulls calling
     * downwards, bubbles rising; single drops between */
    [FXP_CASCADES] = {"CASCADES", {
        RUN(1, 5, FX_DROP, H_DOWN, 6, 80), EV(3, 2, FX_DROP, 12, 50), RUN(3, 5, FX_PING, H_SPARKLE, 8, 70),
        RUN(5, 1, FX_GULL, H_DOWN, 9, 60), EV(6, 6, FX_DROP, 10, 50), RUN(7, 5, FX_BUBBLE, H_UP, 0, 70),
        RUN(9, 5, FX_DROP, H_UP, 5, 80), EV(11, 2, FX_DROP, 13, 50), RUN(11, 5, FX_ZAP, H_DOWN, 5, 50),
        RUN(13, 1, FX_PING, H_ARC, 6, 70), EV(14, 6, FX_DROP, 11, 50), RUN(15, 5, FX_BUBBLE, H_DOWN, 1, 60)}},
    /* a city at night from high up: thin rain, a siren's tone falling far off twice, machines talking once, thunder
     * rolling in the ninth bar, a slow arc of glass before the end */
    [FXP_CITY] = {"CITY", {
        EV(1, 6, FX_DROP, 12, 60), EV(2, 3, FX_DROP, 9, 50), EV(3, 1, FX_WHISTLE, 9, 70), EV(4, 4, FX_DROP, 13, 60),
        EV(5, 7, FX_DROP, 10, 50), EV(6, 5, FX_BLEEPS, 6, 40), EV(7, 2, FX_DROP, 14, 60), EV(8, 6, FX_DROP, 11, 50),
        EV(9, 1, FX_THUNDER, 1, 70), EV(10, 4, FX_DROP, 12, 60), EV(11, 5, FX_WHISTLE, 7, 60), EV(12, 2, FX_DROP, 9, 50),
        EV(13, 6, FX_DROP, 13, 60), RUN(14, 5, FX_PING, H_ARC, 5, 70), EV(16, 3, FX_DROP, 10, 50)}},
    /* ice: glass far up in ones and twos, a gust across, the ice cracking low once in sixteen bars, a sparkle */
    [FXP_FROST] = {"FROST", {
        EV(1, 3, FX_PING, 13, 80), EV(2, 6, FX_PING, 11, 60), EV(3, 7, FX_WHOOSH, 7, 60), EV(4, 5, FX_PING, 14, 70),
        EV(6, 2, FX_PING, 12, 70), EV(6, 4, FX_PING, 10, 50), RUN(7, 5, FX_PING, H_SPARKLE, 10, 60), EV(9, 1, FX_METAL, 1, 50),
        EV(10, 4, FX_PING, 13, 70), EV(11, 7, FX_WHOOSH, 5, 60), EV(12, 6, FX_PING, 11, 60), EV(14, 3, FX_PING, 14, 70),
        EV(15, 1, FX_DROP, 12, 50), EV(16, 5, FX_PING, 10, 60)}},
    /* a gong on the first and the ninth bar's one, a slow bell line between, a rising scale once, far drops */
    [FXP_GONGS] = {"GONGS", {
        EV(1, 1, FX_METAL, 0, 100), EV(2, 5, FX_PING, 7, 80), EV(3, 5, FX_PING, 9, 80), EV(4, 7, FX_DROP, 12, 50),
        EV(5, 1, FX_PING, 10, 80), EV(6, 5, FX_PING, 8, 80), RUN(8, 1, FX_PING, H_UP, 5, 70), EV(9, 1, FX_METAL, 2, 90),
        EV(10, 5, FX_PING, 9, 80), EV(11, 5, FX_PING, 7, 80), EV(12, 7, FX_DROP, 13, 50), EV(13, 1, FX_PING, 8, 80),
        EV(14, 5, FX_PING, 5, 80), EV(16, 3, FX_DROP, 11, 50)}},
};
#undef EV
#undef RUN

/* ---- the setups: each a different place in the manner of the French and Greek electronic records of 1976-82, none
 * copied and none named after one. Seven of the first nine AMBIENT setups had the same eight chords in another key;
 * now each has its own harmony and its own hall: an Aeolian drift, a Dorian sea, a harmonic-minor city with a brass
 * that swells, a major-key hymn on vibes, a Phrygian cave, a descending-bass storm, a modal machine room, open
 * fifths on ice, a pentatonic temple. AMBIENT plays the eight chords two bars each over a held bass, voiced so a
 * tone two chords share stays where it is (band.c: voiced); BAND plays the 1601 sequence with a chord on each of its
 * links, on the link's root: a minor chord where the line stays on one chord a bar, SUS2 or a bare fifth where it
 * walks through others (a third would rub against them). Levels are percent of each part's fader and were set by
 * measurement (tools/listen.py). */
typedef struct {
    const char *name, *about;                    /* about: a line for the banner, 26 characters at most */
    uint8_t scene, tempo, sequence;              /* AMB or BAND; BPM; BAND's 1601 sequence (Q_...) */
    chord_t chord[ARP_CHAIN];                    /* AMBIENT: eight, two bars each; BAND: one a link of the sequence */
    /* the strings: voice, octave 0..4, then attack, release, chorus depth, phaser (a pad: its tone); PAD_OWN on a pad */
    uint8_t strings, octave, attack, release, depth, phaser;
    uint8_t lead;                                /* L_SOFT, L_HARP, L_2600(P_...) or L_SX(n) */
    uint8_t run, phrase;                         /* AMBIENT's background: the box's run up the chords, the lead's phrase (PH_...) */
    uint8_t bass;                                /* the 2600 patch (AMBIENT softens it and holds the roots) */
    int8_t bass_cut;                             /* its cutoff, -50 .. +50 from the patch's */
    uint8_t rhythm, extras, extras_level, balance;   /* R_..., X_... bits, their level, the drums' balance (dark .. bright) */
    uint8_t fx;                                  /* the effect pattern, FXP_... */
    uint8_t strings_level, lead_level, bass_level, drums_level, box_level, wind_level, wind_colour;
    uint8_t hall, reverb, lead_reverb;           /* the hall (H_...); the strings' (and BAND's parts') send to it, the lead's */
    uint8_t echo, echo_feedback, echo_tone, echo_strings, echo_lead;   /* E_..., then percent */
    uint8_t motion;                              /* the slow filter sweeps' depth */
} setup_t;
#define N_SETUPS 17u
static const setup_t SETUPS[N_SETUPS] = {
    {   /* 1: the string ensemble through its slow phaser in C minor (Aeolian: i v VI III, then iv), a far Bossa Nova,
         * droplets and a run up each chord; the default */
        .name = "DRIFT", .about = "PHASED STRINGS, FAR BOSSA", .scene = AMB, .tempo = 75,
        .chord = {{nC, CH_MIN}, {nG, CH_MIN7}, {nAb, CH_MAJ7}, {nEb, CH_MAJ}, {nC, CH_MIN}, {nF, CH_MIN7}, {nAb, CH_MAJ7}, {nG, CH_SUS4}},
        .strings = V_STRINGS, .octave = 1, .attack = 85, .release = 90, .depth = 100, .phaser = 35,
        .lead = L_SOFT, .run = 1, .bass = P_WARM_BASS, .rhythm = R_BOSSA, .extras_level = 70, .balance = 30, .fx = FXP_DROPLETS,
        .strings_level = 86, .lead_level = 100, .bass_level = 16, .drums_level = 39, .box_level = 30, .wind_level = 50, .wind_colour = 45,
        .hall = H_BIG_HALL, .reverb = 60, .lead_reverb = 50, .echo = E_8TH_DOT, .echo_feedback = 62, .echo_tone = 32, .echo_strings = 22, .echo_lead = 45,
        .motion = 50 },
    {   /* 2: the classic band: the Eminent through its slow phaser, the root-and-octave bass, Rock 1, the 2600 lead
         * in a dotted echo */
        .name = "PULSE", .about = "STRINGS, ROCK 1, 2600 LEAD", .scene = BAND, .tempo = 120, .sequence = Q_ROOT_PULSE,
        .chord = {{nE, CH_MIN}, {nA, CH_MIN}, {nE, CH_MIN}, {nG, CH_MIN}},
        .strings = V_STRINGS, .octave = 1, .attack = 75, .release = 80, .depth = 100, .phaser = 25,
        .lead = L_2600(P_ARP_LEAD), .bass = P_SEQ_BASS, .rhythm = R_ROCK1, .extras_level = 70, .balance = 50, .fx = FXP_EMPTY,
        .strings_level = 78, .lead_level = 100, .bass_level = 55, .drums_level = 65, .box_level = 0, .wind_level = 0, .wind_colour = 45,
        .hall = H_HALL, .reverb = 40, .lead_reverb = 50, .echo = E_8TH_DOT, .echo_feedback = 40, .echo_tone = 40, .echo_strings = 10, .echo_lead = 45,
        .motion = 60 },
    {   /* 3: no drums: a plucked E minor arpeggio turning under a huge ensemble an octave up, high wind and a few drops */
        .name = "HELIX", .about = "HUGE ENSEMBLE, NO DRUMS", .scene = BAND, .tempo = 128, .sequence = Q_E_MIN_ARP,
        .chord = {{nE, CH_SUS2}, {nE, CH_SUS2}, {nG, CH_SUS2}, {nA, CH_SUS2}},   /* (open: the line walks E D C B a bar) */
        .strings = V_HUGE_ENSEMBLE, .octave = 2, .attack = PAD_OWN, .release = PAD_OWN, .depth = PAD_OWN, .phaser = PAD_OWN,
        .lead = L_2600(P_LASER_SYNC), .bass = P_PLUCK_SEQ, .bass_cut = 15, .rhythm = R_NONE, .extras_level = 70, .balance = 50, .fx = FXP_SPARSE,
        .strings_level = 77, .lead_level = 100, .bass_level = 42, .drums_level = 0, .box_level = 31, .wind_level = 45, .wind_colour = 75,
        .hall = H_BIG_HALL, .reverb = 50, .lead_reverb = 50, .echo = E_8TH_DOT, .echo_feedback = 50, .echo_tone = 40, .echo_strings = 0, .echo_lead = 40,
        .motion = 80 },
    {   /* 4: bright: violins up high over the octave-answer line in D minor, Rock 3 with the tambourine, a plate */
        .name = "OCTAVES", .about = "BRIGHT VIOLINS, ROCK 3", .scene = BAND, .tempo = 130, .sequence = Q_OCTAVE_ECHO,
        .chord = {{nD, CH_SUS2}, {nC, CH_SUS2}, {nF, CH_SUS2}, {nBb, CH_SUS2}},   /* (the line's second half is a major chord) */
        .strings = V_VIOLIN, .octave = 2, .attack = 55, .release = 60, .depth = 100, .phaser = 15,
        .lead = L_2600(P_TWIN_SAWS), .bass = P_GLASS_PLUCK, .rhythm = R_ROCK3, .extras = X_TAMBOURINE, .extras_level = 60, .balance = 50, .fx = FXP_EMPTY,
        .strings_level = 75, .lead_level = 100, .bass_level = 24, .drums_level = 49, .box_level = 0, .wind_level = 0, .wind_colour = 45,
        .hall = H_PLATE, .reverb = 45, .lead_reverb = 50, .echo = E_8TH_DOT, .echo_feedback = 35, .echo_tone = 45, .echo_strings = 0, .echo_lead = 35,
        .motion = 40 },
    {   /* 5: the fast C minor pulse under low cellos, a quiet Rumba with its tambourine, the sample-and-hold lead */
        .name = "MAGNET", .about = "C MINOR PULSE, RUMBA", .scene = BAND, .tempo = 138, .sequence = Q_C_MIN_PULSE,
        .chord = {{nC, CH_5TH}, {nCs, CH_5TH}, {nD, CH_5TH}, {nC, CH_5TH}},   /* (bare fifths over the pulse's own thirds) */
        .strings = V_CELLO, .octave = 1, .attack = 75, .release = 80, .depth = 100, .phaser = 20,
        .lead = L_2600(P_SH_SPACE), .bass = P_WARM_BASS, .bass_cut = 10, .rhythm = R_RUMBA, .extras = X_TAMBOURINE, .extras_level = 45, .balance = 40, .fx = FXP_EMPTY,
        .strings_level = 64, .lead_level = 100, .bass_level = 27, .drums_level = 36, .box_level = 0, .wind_level = 0, .wind_colour = 45,
        .hall = H_HALL, .reverb = 40, .lead_reverb = 45, .echo = E_8TH, .echo_feedback = 35, .echo_tone = 40, .echo_strings = 0, .echo_lead = 40,
        .motion = 50 },
    {   /* 6: the laser harp's: E minor rising to its relative major and back (i VI III VII), a dusk orchestra; the
         * Synthex plays a slow phrase of its own in a long dotted echo and a vast hall, the keys play along on it */
        .name = "LASER", .about = "LASER HARP PHRASE, VAST", .scene = AMB, .tempo = 70,
        .chord = {{nE, CH_MIN}, {nC, CH_MAJ7}, {nG, CH_MAJ}, {nD, CH_SUS4}, {nE, CH_MIN}, {nC, CH_MAJ7}, {nA, CH_MIN7}, {nB, CH_SUS4}},
        .strings = V_DUSK_ORCHESTRA, .octave = 1, .attack = PAD_OWN, .release = PAD_OWN, .depth = PAD_OWN, .phaser = PAD_OWN,
        .lead = L_HARP, .phrase = PH_HARP, .bass = P_WARM_BASS, .rhythm = R_NONE, .extras_level = 70, .balance = 30, .fx = FXP_SPARSE,
        .strings_level = 80, .lead_level = 100, .bass_level = 16, .drums_level = 0, .box_level = 34, .wind_level = 30, .wind_colour = 25,
        .hall = H_VAST, .reverb = 65, .lead_reverb = 70, .echo = E_4TH_DOT, .echo_feedback = 60, .echo_tone = 35, .echo_strings = 22, .echo_lead = 60,
        .motion = 70 },
    {   /* 7: the sea: a glass choir rocking between A minor and D major (Dorian: the raised sixth is the light on the
         * water), low wind in long swells, waves coming in and gulls, a run up each chord */
        .name = "TIDES", .about = "GLASS CHOIR, THE SEA", .scene = AMB, .tempo = 66,
        .chord = {{nA, CH_MIN}, {nD, CH_MAJ}, {nA, CH_MIN7}, {nD, CH_SUS2}, {nF, CH_MAJ7}, {nC, CH_MAJ}, {nG, CH_SUS2}, {nE, CH_SUS4}},
        .strings = V_GLASS_CHOIR, .octave = 1, .attack = PAD_OWN, .release = PAD_OWN, .depth = PAD_OWN, .phaser = PAD_OWN,
        .lead = L_SOFT, .run = 1, .bass = P_WARM_BASS, .rhythm = R_NONE, .extras_level = 70, .balance = 30, .fx = FXP_SHORE,
        .strings_level = 83, .lead_level = 100, .bass_level = 14, .drums_level = 0, .box_level = 26, .wind_level = 70, .wind_colour = 30,
        .hall = H_BIG_HALL, .reverb = 70, .lead_reverb = 50, .echo = E_8TH_DOT, .echo_feedback = 70, .echo_tone = 32, .echo_strings = 22, .echo_lead = 45,
        .motion = 70 },
    {   /* 8: a city at night, seen from high up: D minor with its major dominant and the flat second (harmonic minor,
         * Neapolitan), an abyss pad, a brass that swells into each long note of its own slow line, thin rain, a siren
         * far off, thunder; the longest hall. The keys play the brass */
        .name = "NEON", .about = "BRASS SWELLS, NIGHT RAIN", .scene = AMB, .tempo = 60,
        .chord = {{nD, CH_MIN}, {nBb, CH_MAJ7}, {nG, CH_MIN7}, {nA, CH_MAJ}, {nD, CH_MIN}, {nEb, CH_MAJ7}, {nG, CH_MIN7}, {nA, CH_SUS4}},
        .strings = V_ABYSS_PAD, .octave = 1, .attack = PAD_OWN, .release = PAD_OWN, .depth = PAD_OWN, .phaser = PAD_OWN,
        .lead = L_SX(75), .phrase = PH_LONG, .bass = P_WARM_BASS, .bass_cut = -6, .rhythm = R_NONE, .extras_level = 70, .balance = 30, .fx = FXP_CITY,
        .strings_level = 70, .lead_level = 85, .bass_level = 17, .drums_level = 0, .box_level = 29, .wind_level = 45, .wind_colour = 20,
        .hall = H_ENDLESS, .reverb = 60, .lead_reverb = 70, .echo = E_4TH_DOT, .echo_feedback = 55, .echo_tone = 30, .echo_strings = 15, .echo_lead = 50,
        .motion = 60 },
    {   /* 9: first light, a hymn: F major (I V vi IV, then iii), warm strings, vibes ringing a few notes of their own
         * off the beat, birds, thin high air */
        .name = "DAWN", .about = "MAJOR KEY, VIBES, BIRDS", .scene = AMB, .tempo = 76,
        .chord = {{nF, CH_MAJ}, {nC, CH_MAJ}, {nD, CH_MIN7}, {nBb, CH_MAJ7}, {nF, CH_MAJ}, {nA, CH_MIN7}, {nBb, CH_MAJ7}, {nC, CH_SUS4}},
        .strings = V_VINTAGE_STRINGS, .octave = 1, .attack = 80, .release = PAD_OWN, .depth = PAD_OWN, .phaser = PAD_OWN,
        .lead = L_SX(29), .phrase = PH_BELLS, .bass = P_SUB_BASS, .rhythm = R_NONE, .extras_level = 70, .balance = 30, .fx = FXP_BIRDS,
        .strings_level = 80, .lead_level = 100, .bass_level = 26, .drums_level = 0, .box_level = 40, .wind_level = 25, .wind_colour = 70,
        .hall = H_BIG_HALL, .reverb = 65, .lead_reverb = 60, .echo = E_4TH, .echo_feedback = 50, .echo_tone = 45, .echo_strings = 10, .echo_lead = 50,
        .motion = 50 },
    {   /* 10: a breath choir in G minor falling by fifths to its relative major (i VI III VII) under steady rain,
         * hardly a bass */
        .name = "RAIN", .about = "BREATH CHOIR, DROPLETS", .scene = AMB, .tempo = 72,
        .chord = {{nG, CH_MIN}, {nEb, CH_MAJ7}, {nBb, CH_MAJ}, {nF, CH_SUS2}, {nG, CH_MIN}, {nC, CH_MIN7}, {nEb, CH_MAJ7}, {nD, CH_SUS4}},
        .strings = V_BREATH_CHOIR, .octave = 1, .attack = PAD_OWN, .release = PAD_OWN, .depth = PAD_OWN, .phaser = PAD_OWN,
        .lead = L_SOFT, .bass = P_WARM_BASS, .rhythm = R_NONE, .extras_level = 70, .balance = 30, .fx = FXP_RAIN,
        .strings_level = 96, .lead_level = 100, .bass_level = 14, .drums_level = 0, .box_level = 27, .wind_level = 55, .wind_colour = 60,
        .hall = H_BIG_HALL, .reverb = 65, .lead_reverb = 55, .echo = E_8TH_DOT, .echo_feedback = 55, .echo_tone = 40, .echo_strings = 15, .echo_lead = 45,
        .motion = 60 },
    {   /* 11: a cave: a deep drone on B with the chord a semitone over it pressing down (Phrygian), metal and low
         * bubbles far off in a long echo, low wind, nine seconds of stone; the slowest */
        .name = "CAVERN", .about = "DEEP DRONE, FAR METAL", .scene = AMB, .tempo = 54,
        .chord = {{nB, CH_5TH}, {nC, CH_MAJ7}, {nB, CH_MIN}, {nA, CH_SUS2}, {nB, CH_5TH}, {nG, CH_MAJ7}, {nE, CH_MIN7}, {nB, CH_SUS4}},
        .strings = V_DEEP_DRONE, .octave = 1, .attack = PAD_OWN, .release = PAD_OWN, .depth = PAD_OWN, .phaser = PAD_OWN,
        .lead = L_SOFT, .bass = P_SUB_BASS, .rhythm = R_NONE, .extras_level = 70, .balance = 30, .fx = FXP_CAVERN,
        .strings_level = 77, .lead_level = 100, .bass_level = 22, .drums_level = 0, .box_level = 32, .wind_level = 60, .wind_colour = 15,
        .hall = H_ENDLESS, .reverb = 65, .lead_reverb = 65, .echo = E_4TH_DOT, .echo_feedback = 66, .echo_tone = 28, .echo_strings = 20, .echo_lead = 55,
        .motion = 60 },
    {   /* 12: the line with rests in it, A minor then down a third: full Eminent through the phaser, a far Bossa
         * Nova, a few drops; the gentlest of the bands */
        .name = "BREATH", .about = "BREATHING BASS, PHASER", .scene = BAND, .tempo = 104, .sequence = Q_BREATHING,
        .chord = {{nA, CH_MIN}, {nFs, CH_MIN}},
        .strings = V_FULL, .octave = 1, .attack = 80, .release = 85, .depth = 100, .phaser = 30,
        .lead = L_SOFT, .bass = P_WARM_BASS, .rhythm = R_BOSSA, .extras_level = 70, .balance = 30, .fx = FXP_SPARSE,
        .strings_level = 54, .lead_level = 100, .bass_level = 22, .drums_level = 36, .box_level = 33, .wind_level = 30, .wind_colour = 50,
        .hall = H_BIG_HALL, .reverb = 55, .lead_reverb = 55, .echo = E_8TH_DOT, .echo_feedback = 45, .echo_tone = 40, .echo_strings = 10, .echo_lead = 45,
        .motion = 60 },
    {   /* 13: weather: a slow swell in F minor over a bass that walks down a step at a time to the dominant, thunder
         * rolling far off, rain, a low wind that gusts, a dark hall */
        .name = "STORM", .about = "THUNDER, RAIN, SLOW SWELL", .scene = AMB, .tempo = 62,
        .chord = {{nF, CH_MIN}, {nEb, CH_MAJ}, {nCs, CH_MAJ7}, {nC, CH_SUS4}, {nF, CH_MIN}, {nAb, CH_MAJ}, {nBb, CH_MIN7}, {nC, CH_MAJ}},
        .strings = V_SLOW_SWELL, .octave = 1, .attack = PAD_OWN, .release = PAD_OWN, .depth = PAD_OWN, .phaser = PAD_OWN,
        .lead = L_SOFT, .bass = P_SUB_BASS, .rhythm = R_NONE, .extras_level = 70, .balance = 30, .fx = FXP_STORM,
        .strings_level = 46, .lead_level = 100, .bass_level = 20, .drums_level = 0, .box_level = 34, .wind_level = 65, .wind_colour = 35,
        .hall = H_DARK_HALL, .reverb = 70, .lead_reverb = 60, .echo = E_4TH_DOT, .echo_feedback = 55, .echo_tone = 30, .echo_strings = 18, .echo_lead = 50,
        .motion = 60 },
    {   /* 14: a machine room in orbit: the string machine two octaves up on chords that never quite land (F sharp
         * minor seventh, suspended seconds), sample-and-hold bleeps, sweeps, a whistle falling, scales of glass; the
         * lead calls three notes and waits six bars for the answer. (GLASS SHIMMER and ROUGH PULSE carry a third of
         * their energy under 50 Hz and RUNNING PAD reads a semitone off: not used in a setup) */
        .name = "ORBIT", .about = "BLEEPS, SWEEPS, FAR CALLS", .scene = AMB, .tempo = 68,
        .chord = {{nFs, CH_MIN7}, {nE, CH_SUS2}, {nD, CH_MAJ7}, {nE, CH_SUS2}, {nFs, CH_MIN7}, {nA, CH_MAJ7}, {nB, CH_SUS2}, {nCs, CH_SUS4}},
        .strings = V_STRING_MACHINE, .octave = 2, .attack = PAD_OWN, .release = PAD_OWN, .depth = PAD_OWN, .phaser = PAD_OWN,
        .lead = L_SOFT, .phrase = PH_CALLS, .bass = P_WARM_BASS, .rhythm = R_NONE, .extras_level = 70, .balance = 30, .fx = FXP_COMPUTER,
        .strings_level = 80, .lead_level = 50, .bass_level = 16, .drums_level = 0, .box_level = 34, .wind_level = 35, .wind_colour = 80,
        .hall = H_VAST, .reverb = 65, .lead_reverb = 60, .echo = E_8TH_DOT, .echo_feedback = 65, .echo_tone = 38, .echo_strings = 18, .echo_lead = 55,
        .motion = 60 },
    {   /* 15: the up-and-down arpeggio in A minor walking down a fourth, a drifting pad on bare fifths, Rock 4 far
         * back, shots and sweeps in the echo; the laser lead */
        .name = "COMET", .about = "ARPEGGIO, SHOTS, ROCK 4", .scene = BAND, .tempo = 134, .sequence = Q_UP_AND_DOWN,
        .chord = {{nA, CH_5TH}, {nG, CH_5TH}, {nF, CH_5TH}, {nE, CH_5TH}},   /* (each bar goes minor, then major a tone down) */
        .strings = V_SOLO_DRIFT, .octave = 1, .attack = PAD_OWN, .release = PAD_OWN, .depth = PAD_OWN, .phaser = PAD_OWN,
        .lead = L_2600(P_LASER_SYNC), .bass = P_PLUCK_SEQ, .bass_cut = 10, .rhythm = R_ROCK4, .extras_level = 60, .balance = 45, .fx = FXP_LASERS,
        .strings_level = 75, .lead_level = 100, .bass_level = 40, .drums_level = 40, .box_level = 30, .wind_level = 20, .wind_colour = 70,
        .hall = H_HALL, .reverb = 45, .lead_reverb = 50, .echo = E_8TH_DOT, .echo_feedback = 45, .echo_tone = 42, .echo_strings = 0, .echo_lead = 40,
        .motion = 60 },
    {   /* 16: ice: a glass choir two octaves up on bare fifths and suspended seconds over A (no third anywhere near
         * the start: neither major nor minor, cold), chimes ringing a few notes of their own, glass far up, a gust,
         * the ice cracking low once in a while, a thin high wind, the longest hall; the slowest but one */
        .name = "ICE", .about = "OPEN FIFTHS, CHIMES, WIND", .scene = AMB, .tempo = 56,
        .chord = {{nA, CH_5TH}, {nF, CH_MAJ7}, {nA, CH_SUS2}, {nG, CH_SUS2}, {nA, CH_5TH}, {nF, CH_MAJ7}, {nD, CH_SUS2}, {nE, CH_5TH}},
        .strings = V_GLASS_CHOIR, .octave = 2, .attack = PAD_OWN, .release = PAD_OWN, .depth = PAD_OWN, .phaser = PAD_OWN,
        .lead = L_SX(28), .phrase = PH_BELLS, .bass = P_WARM_BASS, .rhythm = R_NONE, .extras_level = 70, .balance = 30, .fx = FXP_FROST,
        .strings_level = 60, .lead_level = 80, .bass_level = 15, .drums_level = 0, .box_level = 22, .wind_level = 65, .wind_colour = 85,
        .hall = H_ENDLESS, .reverb = 65, .lead_reverb = 70, .echo = E_4TH_DOT, .echo_feedback = 60, .echo_tone = 45, .echo_strings = 15, .echo_lead = 55,
        .motion = 50 },
    {   /* 17: a temple: D minor's five notes only (fifths, suspensions, the relative major: no leading tone), a pan
         * flute calling three notes and waiting, a gong on the first bar, a slow bell line, a cathedral */
        .name = "TEMPLE", .about = "FIVE NOTES, FLUTE, GONG", .scene = AMB, .tempo = 64,
        .chord = {{nD, CH_5TH}, {nG, CH_SUS2}, {nC, CH_SUS2}, {nD, CH_SUS4}, {nD, CH_5TH}, {nF, CH_MAJ}, {nC, CH_SUS2}, {nA, CH_MIN7}},
        .strings = V_VINTAGE_STRINGS, .octave = 1, .attack = 90, .release = PAD_OWN, .depth = PAD_OWN, .phaser = PAD_OWN,
        .lead = L_SX(4), .phrase = PH_CALLS, .bass = P_WARM_BASS, .rhythm = R_NONE, .extras_level = 70, .balance = 30, .fx = FXP_GONGS,
        .strings_level = 70, .lead_level = 60, .bass_level = 16, .drums_level = 0, .box_level = 36, .wind_level = 30, .wind_colour = 40,
        .hall = H_CATHEDRAL, .reverb = 65, .lead_reverb = 65, .echo = E_4TH, .echo_feedback = 50, .echo_tone = 40, .echo_strings = 12, .echo_lead = 50,
        .motion = 50 },
};
#endif
