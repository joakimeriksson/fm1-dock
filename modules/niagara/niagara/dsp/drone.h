/* SPDX-License-Identifier: GPL-3.0-only */
/* The drone engine, after the Stylophone CPM DS-2 (its manual). Two drones, always on (in the
 * code a drone is a "group": GP_*, DR_GP), each:
 *
 *   MAIN     an oscillator whose X-WAVE blends sine -> triangle -> saw -> square (after the DS-2's
 *            X-WAVE, with a sine before it), the square's pulse Width
 *   PARTNER  a second oscillator riding along at an interval from the main one (unison, fifth,
 *            octave...) with Detune and a slow independent drift; on or off (GP_PARTNER)
 *   SUB      a square one or two octaves under the main oscillator; OSC MIX crossfades main <-> sub
 *   NOISE    a white or pink noise (its Colour), at its own level beside the
 *            oscillators (as a synth's mixer)
 *
 * mixed into the drone's own filter (12 dB/oct state-variable, LP / BP / HP, resonance up to
 * self-oscillation, soft-saturating drive). A drone's pitch is a parameter in cents, free between notes
 * (27 Hz .. 1.6 kHz, the DS-2's oscillator range), and slides or glides when it changes. There are no envelopes:
 * dr_play() fades the whole drone in or out, dr_voice() one drone.
 *
 * The PULSE chops both drones in time, on its own clock: rate, depth, shape (swell .. gate), swing.
 * Then the EFFECTS, as the DS-2's: a dark, PT2399-style DELAY (LFO on its time, repeats up to runaway)
 * and a stereo REVERB of four types, each with a dry/wet mix (drone_fx.c).
 *
 * Between the two, as the DS-2's switches and its best-known patch: HARD SYNC (drone II's oscillator
 * restarts with drone I's), PITCH LINK (drone II follows drone I's pitch, at an offset) and CROSS-MOD
 * (drone I's sound moves drone II's cutoff, at audio rate: the DS-2's DRONE I OUT -> CUTOFF II cable).
 *
 * The ULTRA LFO (after the DS-2's): one Rate, 0.02 .. 70 Hz, two taps (Wave I, Wave II), each Off or one
 * of seven shapes, locked to the same clock. Each drone routes a tap to its oscillator (pitch or X-WAVE)
 * and a tap to its filter (cutoff or resonance), each with a bipolar depth (0: none, negative: inverted).
 *
 * Threads, as in FoMni's engine: dr_render() runs in the audio interrupt; everything else posts commands that
 * the render drains at the start of a block. The state marked (UI) is written by the render and only
 * read elsewhere. Float only, no libm: fastmath.h. */
#pragma once
#include <stdint.h>

#define DR_SR 44100.0f
#define DR_NGROUPS 2                           /* the drones */

/* a drone's parameters; drone g's copy of GP_x is parameter DR_GP(g, GP_x) */
enum {
    GP_PITCH,       /* cents over MIDI note 0: 2068 (27 Hz) .. 9135 (1.6 kHz); 4500 is A2, 5700 A3 */
    GP_WAVE,        /* X-WAVE, 0..100: 0 sine, 33 triangle, 67 saw, 100 square, blended in between */
    GP_WIDTH,       /* the square's pulse width: 0 even (50 %) .. 100 narrow (5 %) */
    GP_INTERVAL,    /* the partner's interval: an index into DR_IV_SEMI (each tempered, then just, pure) */
    GP_DETUNE,      /* the partner's detune, 0..100 = 0..30 cents */
    GP_MIX,         /* OSC MIX, 0..100: 0 the oscillators alone .. 100 the sub alone (a crossfade) */
    GP_SUBOCT,      /* the sub: 1 an octave below, 2 two, 3 a third of the pitch, 4 a fifth (subharmonics) */
    GP_CUTOFF,      /* 0..1000 = 20 Hz .. 18 kHz, exponential (a knob's detent: 10; with SEL held: 1) */
    GP_RESO,        /* 0..1000; self-oscillates near the top (a detent: 10; with SEL held: 1) */
    GP_MODE,        /* LP, BP, HP */
    GP_DRIVE,       /* 0..100: into the filter, soft saturation */
    GP_LEVEL,       /* the drone's level */
    GP_PARTNER,     /* the partner oscillator on (1) / off (0): a switch */
    GP_OLFO,        /* the oscillator's LFO tap: DR_LFO_OFF, _I, _II, _BOTH (a switch) */
    GP_OLFO_DEPTH,  /* -100..100: up to +-24 semitones (pitch) or the whole X-WAVE range; 0 none */
    GP_OLFO_DEST,   /* what the oscillator's LFO moves: DR_TO_PITCH, DR_TO_XWAVE (a switch) */
    GP_FLFO,        /* the filter's LFO tap: DR_LFO_OFF, _I, _II, _BOTH (a switch) */
    GP_FLFO_DEPTH,  /* -100..100: up to +-5 octaves of cutoff, or the whole resonance range; 0 none */
    GP_FLFO_DEST,   /* what the filter's LFO moves: DR_TO_CUTOFF, DR_TO_RESO (a switch) */
    GP_NOISE,       /* NOISE, 0..100: its level, added to the oscillators (they stay as they are); 100 is level
                     * with a saw, 50 about -7 dB, 25 -12 */
    GP_NCOLOR,      /* the noise's COLOUR, 0..100: white .. pink (-3 dB/oct), a blend (v0.2; it was 0 / 1) */
    GP_N
};
#define DR_GP(g, p) ((g) * GP_N + (p))

/* global parameters, after the drones' */
enum {
    P_XMOD = DR_NGROUPS * GP_N,     /* drone I -> drone II's cutoff on (1) / off (0): a switch */
    P_XMOD_AMT,                     /* 0..100: how far drone I's sound moves drone II's cutoff */
    P_SYNC,                         /* drone II's oscillator hard-synced to drone I's (1) / free (0): a switch */
    P_PLINK,                        /* drone II's pitch linked to drone I's (1) / its own (0): a switch */
    P_PLINK_OFF,                    /* linked: drone II's pitch = drone I's + this, in cents (-2400..2400) */
    P_LFO_RATE,                     /* 0..100 = 0.02 (a cycle in 50 s, the DS-2's slowest) .. 70 Hz, exponential */
    P_LFO_W1,                       /* Wave I: 0 off, 1..7 (DR_LFO_W1_NAME) (a switch) */
    P_LFO_W2,                       /* Wave II: 0 off, 1..7 (DR_LFO_W2_NAME) (a switch) */
    P_GLIDE,                        /* 0..100 = 0 .. 10 s to reach a new pitch (quadratic); 0 by default: a pitch
                                     * knob answers at once */
    P_FADE,                         /* 0..100 = 0.05 .. 60 s, the PLAY fade and the drone fades */
    P_DRIFT,                        /* 0..100: DRIFT, the drones never quite still: pitch (+-8 cents), and slow
                                     * random wanders of each drone's cutoff, X-WAVE, level and detune */
    P_WIDTH,                        /* stereo: 0 mono .. 100 drone I left, drone II right */
    P_TUNE,                         /* cents, -50..50 */
    P_PULSE,                        /* the PULSE on (1) / off (0): both drones chopped in time (a switch) */
    P_PULSE_RATE,                   /* 0..100 = 0.03 .. 16 pulses a second, exponential (a 33 s breath .. a buzz) */
    P_PULSE_DEPTH,                  /* 0..100: how far the level drops between pulses */
    P_PULSE_SHAPE,                  /* 0..100: a smooth swell .. a hard gate */
    P_PULSE_SWING,                  /* 0..50: every other pulse longer (50: 3 to 1) */
    P_DLY_TIME,                     /* the DELAY: 0..100 = 70 .. 450 ms (exponential) */
    P_DLY_REPEATS,                  /* 0..100: its feedback; past ~85 it runs away (held by a soft clip) */
    P_DLY_MIX,                      /* 0..100: dry .. wet */
    P_REV_MIX,                      /* the REVERB: 0..100, dry .. wet */
    P_REV_TYPE,                     /* Room, Hall, Cathedral, Chorus hall (DR_REV_NAME) */
    P_DLY_LFO,                      /* the delay time's LFO tap: DR_LFO_I, _II, _BOTH (a switch) */
    P_DLY_LFO_DEPTH,                /* -100..100: up to +-25 % of the time; 0 none */
    P_LFO_LOCK,                     /* the LFO restarts at each Pulse bar (two beats) while the pulse runs (1) /
                                     * runs free (0) (a switch) */
    P_LFO_RMOD,                     /* -100..100: the LFO's own wave moving its rate, up to +-3 octaves (linear); 0 none */
    P_LFO_RMOD_SRC,                 /* which wave moves it: DR_LFO_I, _II, _BOTH */
    P_MORPH,                        /* 0..100 = 0.02 .. 30 s: how long a switch (a button or key) takes to move
                                     * the sound to its new setting: OCT, filter mode, pulse, partner, sub, the
                                     * links, the LFO routes */
    P_SCENE_T,                      /* 0..100 = 0.1 s .. 5 min: a scene's morph (the white keys) */
    P_II_OUT,                       /* drone II faded out on its own (1; SEL + PLAY), a sound's (P12 Clean: one
                                     * oscillator); was Tuning until 30, 0 since */
    P_SPEAKER,                      /* 0..100: a low cut for a small speaker, 0 off (headphones) .. 240 Hz */
    P_PLOCK,                        /* the pitches locked (1): the pitch knobs do nothing (a switch) */
    P_MOVE,                         /* MOVEMENT, 0..100 = every LFO depth x 0 .. x 2 (50: as set) */
    P_FEEDBACK,                     /* 0..100: the finished sound (after the effects) fed back into both drones'
                                     * filters through their drive: thicker, then howling, held by the clip */
    P_CHAR,                         /* CHARACTER, 0..100: clean .. vintage oscillators (softer edges, a bent
                                     * saw, even harmonics, pitch jitter, the drones a little apart, noise) */
    /* THE MACROS (v0.2): offsets on top of the sound (drone.c MACRO_MV), the sound's own values untouched. These
     * four were the set macros (Bloom, Weight, Tide, Tear) until 2026-10-18 */
    P_WANDER,                       /* WANDER, 0..100: drone II plays a slow random melody, jumping between the
                                     * notes of a scale on drone I (P_WSCALE), up to an octave away; how often:
                                     * 25-30 s apart at 1 .. 0.5-2 s at 100 (GLO's Glide glides it); 0 still (Pitch) */
    P_AGE,                          /* AGE, 0..100: the sound worn, like an old tape: darker, a slow wow, hiss,
                                     * more character; it wears in (and out) slowly (Tone 2) */
    P_SHIFT_MOVE,                   /* Shift's move in cents, 0..1400 = a fifth down .. none (700) .. a fifth up (a
                                     * detent 10, with SEL held 1) */
    P_WSCALE,                       /* Wander's scale (DR_WSCALE_NAME), 0 minor pentatonic (Moves) */
    P_GD_0, P_GD_1, P_GD_2, P_GD_3,      /* unused since 2026-10-18 (each key's depth: now DR_GEST_DEPTH) */
    P_GR_0, P_GR_1, P_GR_2, P_GR_3,      /* unused since 2026-10-18 (each key's time: now DR_GEST_TIME) */
    P_GL_0, P_GL_1, P_GL_2, P_GL_3,      /* ... latched at, 0..100 (0: not latched) */
    /* THE EFFECTS (v0.2, ChoralRoot's design) */
    P_DLY_TYPE,                     /* the delay's type: Tape, Wide, Ping-pong, Clean (DR_DLY_NAME) */
    P_REV_LEN,                      /* the reverb's LENGTH: 0..99 = 0.3 .. 40 s (its decay time), 100 Inf (a freeze) */
    P_REV_DAMP,                     /* 0..100: how dark its tail gets, open .. ~600 Hz (out at Inf) */
    P_PRESS_SENS,                   /* the keys' pressure sensitivity, 0..100 (50: as tuned; a room setting) */
    DR_NPARAMS
};

enum { DR_LP, DR_BP, DR_HP };
enum { DR_SINE, DR_TRI, DR_SAW, DR_SQUARE };          /* the X-WAVE's four waves, in its order */
enum { DR_LFO_OFF, DR_LFO_I, DR_LFO_II, DR_LFO_BOTH };   /* a route's wave; BOTH: I + II, summed */
enum { DR_TO_PITCH, DR_TO_XWAVE };
enum { DR_TO_CUTOFF, DR_TO_RESO };
#define DR_NGEST 4                             /* the gesture keys, D5-G5 */
enum { DR_G_WEIGHT, DR_G_SHIFT, DR_G_CLEAN, DR_G_WASH, DR_NGTYPE };   /* what each key plays, D5-G5 (fixed) */
#define DR_SHIFT_MID 700                       /* Shift's move: none (cents + 700) */
extern const char *const DR_GEST_NAME[DR_NGTYPE];
extern const uint8_t DR_GEST_DEPTH[DR_NGTYPE], DR_GEST_TIME[DR_NGTYPE];   /* each one's full depth and its time */
#define DR_NWSCALE 6                           /* Wander's scales */
extern const char *const DR_WSCALE_NAME[DR_NWSCALE];
void dr_gesture(int g, int amt);               /* gesture key g's amount now, 0..1000 (the UI swells it) */
float dr_wander(void);                         /* Wander's move of drone II now, semitones (the screen) */
int dr_heard(int p);                           /* a value as heard: the sound's own plus the macros (tests) */
float dr_mac_pitch(int g);                     /* drone g's pitch move by the macros now, semitones (tests) */
#define DR_LFO_NSHAPES 9                                /* each tap: off and eight shapes */
#define DR_LFO_RANDOM 8                                 /* the eighth: random (I: steps, II: a glide) */
extern const char *const DR_LFO_W1_NAME[DR_LFO_NSHAPES];
extern const char *const DR_LFO_W2_NAME[DR_LFO_NSHAPES];
#define DR_PITCH_LO 2068                       /* 27 Hz, in cents over MIDI note 0 */
#define DR_PITCH_HI 9135                       /* 1.6 kHz */
#define DR_NREV 4
extern const char *const DR_REV_NAME[DR_NREV];          /* "Room", "Hall", "Cathedral", "Chorus hall" */
#define DR_NDLY 4
extern const char *const DR_DLY_NAME[DR_NDLY];          /* "Tape", "Wide", "Ping-pong", "Clean" */
#define DR_NINTERVALS 17
extern const float DR_IV_SEMI[DR_NINTERVALS];           /* semitones: -Oct, -5th, -5th just .. +Oct+5 just */
#define DR_IV_UNISON 5

typedef struct {
    const char *name;
    int16_t lo, hi, def;
} dr_param_t;
const dr_param_t *dr_param_info(int i);
void dr_param_text(int i, int v, char *buf);   /* the value as the screen shows it (12 bytes) */

/* commands (main loop -> render) */
void dr_init(void);
void dr_set(int param, int value);
void dr_play(int on);                          /* fade the whole drone in (1) / out (0) over P_FADE */
void dr_voice(int group, int on);              /* fade one drone in / out */
/* the drone's next pitch change slides there in a straight line over ms (1..30000), instead of the Glide:
 * a pitch knob sends each detent's change with the time until the next detent is expected, so a turn at
 * any speed is one continuous slide */
void dr_slide(int group, int ms);
/* the next change of parameter p glides there over ms (1..30000), as a knob's detent does; otherwise a
 * change glides over 25 ms */
void dr_pslide(int param, int ms);
void dr_panic(void);                           /* silence now, no fade */
void dr_duck(int on);                          /* the output dips out (20 ms) or back (30 ms): around a flash write */
void dr_lfo_reset(void);                       /* the LFO back to the start of its cycle (the DS-2's RESET) */
void dr_seed(uint32_t s);                      /* mix s into the random generators (LFO, Drift): each run its own */

/* the render (audio ISR): n stereo frames, 24-bit in int32; gain Q12 (the MASTER pot) */
void dr_render(int32_t *out_lr, uint32_t n, uint32_t gain_q12);

/* state for the screen (UI) */
extern volatile float dr_level[DR_NGROUPS];    /* each drone's output level, 0..~1 (peak, decaying) */
extern volatile float dr_fade;                 /* the PLAY fade, 0..1 */
extern volatile uint8_t dr_phones;             /* headphones in: the Speaker low cut stays out (the UI sets it) */
extern volatile float dr_duck_level;           /* the dip now, 1 (none) .. 0 (out) */
extern volatile float dr_lfo[2];               /* the LFO taps now, -1..1 (the DS-2's rate lights) */
extern volatile float dr_pulse;                /* the pulse's gain now, 0..1 (1: open, or off) */
extern volatile float dr_lfo_ph;               /* the LFO's phase now, 0..1 (cycles) */
extern volatile float dr_dly_ms;              /* the delay's time now, with its LFO, ms (the host's probe) */
extern volatile float dr_cut[DR_NGROUPS];      /* each drone's cutoff now, gliding, with its LFO (knob units 0..100) */
extern volatile float dr_fb;                   /* the FEEDBACK loop's level now (its gain applied), 0..~2 */
extern volatile float dr_pulse_ph;             /* the pulse's phase now, 0..2 (its two beats) */
float dr_lfo_wave(int tap, int shape, float ph);   /* a tap's shape (1..8; 0 off: flat) at ph, -1..1; for
                                                    * random, ph 0..1 the last cycle, 1..2 this one */
float dr_lfo_rmod_k(float rm, int src, int w1, int w2);  /* rate mod's speed scale (the screen's picture) */
float dr_pulse_wave(float ph);                 /* the pulse's gain at ph (0..2) as set now, 0..1 */
/* each drone's sound, for the screen: DR_SCOPE_N points spanning about two cycles of its oscillator,
 * starting where a cycle starts (so the picture stands still), as it sounds (after its filter and level);
 * renewed about 30 times a second */
#define DR_SCOPE_N 104
extern volatile float dr_scope[DR_NGROUPS][DR_SCOPE_N];
extern volatile float dr_pitch[DR_NGROUPS];    /* each drone's pitch now (MIDI note, fractional while sliding) */
