/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_BAND_H
#define MD_BAND_H
#include "module/seq.h"

/* ORRERY' four parts as sequencer patterns (M3-04). The engines' own sequencers stay off:
 * the ARP 1601 sequences and the Mini Pops rhythms are converted here into editable patterns
 * for the one session clock, so the parts are synchronised by construction.
 *
 *   track 0 BASS    pattern k = the ARP sequence transposed for link k of its chain
 *   track 1 LEAD    empty (the keys and MIDI play it live); AMBIENT with phrase: a slow phrase on the chords' tones
 *                   (patterns 0/1 the two eight-bar halves, linked as the chords)
 *   track 2 CHORDS  pattern k = the chord of link k, held for the link's bars
 *   track 3 DRUMS   pattern 0 = the rhythm keys down, merged as the machine's diode matrix does
 *   track 4 FX      the effects box (electronic FX): the settings' effect sequence, up to sixteen events (a kit
 *                   sound locked on each one's step: MD_FX_KIT) in the home key's pentatonic; AMBIENT plays it over
 *                   its sixteen bars (patterns 0/1) under an optional run up each chord (the box's base sound),
 *                   BAND as one eight-bar pattern (not linked). The wind is its own layer, set by the module
 *
 * Moving from link to link is a next-bar pattern launch of tracks 0 and 2 (md_band_link_at says
 * which link a bar belongs to). Portable: no host, engine state or allocation. */

enum { MD_BAND_BASS, MD_BAND_LEAD, MD_BAND_CHORDS, MD_BAND_DRUMS, MD_BAND_FX };
enum { MD_BAND_CHORD_NONE, MD_BAND_CHORD_MIN, MD_BAND_CHORD_MAJ, MD_BAND_CHORD_MIN7, MD_BAND_CHORD_MAJ7,
       MD_BAND_CHORD_SUS2, MD_BAND_CHORD_SUS4, MD_BAND_CHORD_5TH, MD_BAND_CHORD_KINDS };
#define MD_BAND_LINKS 8u

#define MD_BAND_PHRASES 4u
typedef struct { uint8_t root, kind; } md_band_chord_t;   /* root 0..11, kind MD_BAND_CHORD_* */
/* The effect sequence: what the effects box plays by itself. An event is one hit of a kit sound.
 *   at      when, in eighths from the start: 0..127 over AMBIENT's sixteen bars (bar b, eighth e: 8 b + e);
 *           BAND's loop is eight bars: at % 64
 *   sound   0 none (the slot is free), 1 DROP, 2 PING, 3 BUBBLE, 4 CHIRP, 5 METAL, 6 ZAP, 7 WHISTLE, 8 GULL, 9 TWITTER,
 *           10 BLEEPS, 11 WHOOSH, 12 SURF, 13 THUNDER (the box's recipes, electronic_fx.h; for the three of noise the
 *           note is the colour)
 *   degree  0..14: a step of the home key's pentatonic, three octaves up from the C4 octave (md_band_fx_key),
 *           so a sequence stays in key whatever the chords are set to
 *   chance  how often it sounds, percent
 *   how     how it is played: ONCE, a hit on its step (in the pattern, sample-exact); or a gesture of several hits of
 *           the sound from its note through the scale: UP, DOWN, ARC (up and back), SPARKLE (at random). A gesture
 *           is not in the pattern: the module plays it from its main loop when the sequence comes to it (orrery.c:
 *           fx_gestures), so md_band_build leaves the event out
 * Two events on one eighth: the first in the list plays. An event on a step of AMBIENT's run takes the step. */
enum { MD_BAND_FX_ONCE, MD_BAND_FX_UP, MD_BAND_FX_DOWN, MD_BAND_FX_ARC, MD_BAND_FX_SPARKLE, MD_BAND_FX_HOWS };
#define MD_BAND_FX_EVENTS 16u
#define MD_BAND_FX_DEGREES 15u
#define MD_BAND_FX_SPAN 128u
typedef struct { uint8_t at, sound, degree, chance, how; } md_band_fx_event_t;
enum { MD_BAND_SCENE_BAND, MD_BAND_SCENE_AMBIENT };
typedef struct {
    uint8_t bass_seq;                  /* ARP_SEQS index */
    uint8_t chord_octave;              /* 0..4: chord roots from C2 + 12 x octave */
    md_band_chord_t chord[MD_BAND_LINKS];
    uint16_t rhythm_keys;              /* Mini Pops keys down, bit k = key k (0..9) */
    uint8_t rhythm_section;            /* lever: 0 A, 1 B */
    uint8_t extras;                    /* bit 0 tambourine, 1 guiro, 2 quijada (the extra rhythms on) */
    uint8_t scene;                     /* MD_BAND_SCENE_*: AMBIENT plays chord[0..7], two bars each
                                        * (sixteen bars as two eight-bar links), a NONE a rest */
    uint8_t amb_arp;                   /* AMBIENT effects track: 1 a background run up the chord each second bar */
    uint8_t phrase;                    /* AMBIENT: the lead plays a phrase of its own on the chords (track 1): 0 none,
                                        * 1 HARP (long notes that rise, answer, climb, settle), 2 LONG TONES (one or two
                                        * a chord, an octave lower: for a sound that swells), 3 BELLS (a few short high
                                        * notes off the beat, left to ring), 4 CALLS (three notes and six bars of quiet) */
    int8_t phrase_shift;               /* the phrase's keys, semitones (a sound that speaks octaves over its key: -12) */
    md_band_fx_event_t fx[MD_BAND_FX_EVENTS];   /* the effect sequence (both scenes) */
} md_band_settings_t;

typedef struct {
    uint8_t links;                     /* 1..4 patterns in the chain */
    uint8_t bars[MD_PATTERNS_PER_TRACK];
    uint8_t total_bars;
} md_band_chain_t;

typedef struct {
    uint16_t drum_hits;                /* source hits placed (after the diode-matrix merge) */
    uint16_t drum_moved;               /* hits whose time had to move to the v1 grid */
    uint16_t drum_merged;              /* hits that landed on a step already holding that voice */
    uint16_t drum_error_permille;      /* largest move, in thousandths of a quarter note */
    uint8_t links_dropped;             /* chain links beyond four patterns, not played */
    uint8_t ties_as_slides;            /* ARP TIE steps to a new pitch, played as SLIDE */
} md_band_report_t;

/* Fill bank and chain from settings; the report states what the v1 grid could not keep. */
int md_band_build(const md_band_settings_t *, md_seq_bank_t *, md_band_chain_t *, md_band_report_t *);
/* The link (pattern) that bar `bar` (from the start) plays. */
uint32_t md_band_link_at(const md_band_chain_t *, uint32_t bar);
/* Main loop, while running: the link to queue now as a next-bar launch of BASS and CHORDS, or -1.
 * `bar` is the bar playing, `pattern` the bass pattern playing, `queued`/`requested` its pending
 * launch (all from the session snapshot). Queuing any time in the bar before a link change lands the
 * change exactly on the bar line; a whole bar of main-loop slack. A pending launch is never replaced. */
int md_band_next_link(const md_band_chain_t *, uint32_t bar, uint8_t pattern, uint8_t queued, uint8_t requested);
/* The key an effect event's degree plays: the pentatonic of the home chord (the first chord that is not NONE; major
 * on MAJ and MAJ7, else minor; C minor without one), from its root in the C4 octave. */
uint32_t md_band_fx_key(const md_band_settings_t *, uint32_t degree);
/* The velocity an event of that sound (1..13) plays at: the recipes differ in strength and the long ones sit further
 * back than the struck ones, the sequence evens them. */
uint32_t md_band_fx_velocity(uint32_t sound);
#endif
