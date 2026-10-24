/* SPDX-License-Identifier: GPL-3.0-only */
/* The whole instrument: two layers (upper, lower) of voices with synthex-web's voice allocation
 * (synth.ts, voice.ts), each layer through its own chorus, then a reverb, the master volume and a limiter.
 *
 * Threading: sc_render() runs in the audio interrupt; everything else may run in the main loop.
 * The two meet only through the event queue (sc_post: one writer, one reader) and through patch
 * fields written whole (32-bit stores) followed by SC_EV_PATCH. */
#ifndef SC_SYNTH_H
#define SC_SYNTH_H
#include <stdint.h>
#include "sc_patch.h"
#include "sc_voice.h"

#ifndef SC_VOICES
#define SC_VOICES 8                             /* per layer (patch.master.polyphony, capped) */
#endif
#define SC_CHORUS_LEN 1024                      /* samples: 23 ms, the chorus reaches ~20 */

enum { SC_EV_NOTE_ON = 1, SC_EV_NOTE_OFF, SC_EV_ALL_OFF, SC_EV_BEND, SC_EV_PATCH,
       SC_EV_PERF };                            /* PERF: only the stick moved (the layers again, not the reverb) */
typedef struct { uint8_t type, note; int32_t val; } sc_event_t;
#define SC_QUEUE 64

typedef struct {
    int32_t buf[SC_CHORUS_LEN];                 /* Q20: a loud chord's voices sum past 1.0 (Brass I: ~1.8) */
    uint32_t w, ph;
} sc_chorus_t;

/* The reverb: eight delay lines in a Hadamard feedback loop, standing in for the app's convolution
 * with a noise impulse (fx/reverb.ts); sc_synth.c says what is matched. 64 KB. */
#define SC_REV_LINES 8
#define SC_REV_LEN 2048                         /* each line's ring (power of two); the longest is 1949 */
typedef struct {
    int32_t buf[SC_REV_LEN][SC_REV_LINES];      /* Q20, interleaved: slot j of line k is buf[j][k] */
    uint32_t w;
    int32_t lpl, lpr;                           /* the output lowpass, Q20 */
    int32_t g[SC_REV_LINES];                    /* each line's gain per pass, with the matrix's 1/sqrt(8): Q30 */
    int32_t a, wet, dry;                        /* Q16 */
    int8_t on;
} sc_reverb_t;

#define SC_LIM_LOOK 256                         /* the limiter's look-ahead: 5.8 ms (Chrome's is 6) */
typedef struct {
    int32_t env;                                /* the detector, linear Q15 */
    int32_t gain;                               /* the gain applied at the last sub-block's end, Q16 */
    int32_t dl[SC_LIM_LOOK], dr[SC_LIM_LOOK];   /* the audio, delayed while the detector looks ahead */
    uint32_t w;
} sc_limiter_t;

typedef struct {
    sc_voice_t v[SC_VOICES];
    int16_t note[SC_VOICES];                    /* VoicePool: -1 until first used */
    uint32_t started[SC_VOICES], released[SC_VOICES];   /* 0: never released, ~0: held */
    uint8_t held[32];
    uint32_t nheld;
    sc_prep_t prep;
    sc_chorus_t chorus;
} sc_layer_state_t;

typedef struct {
    sc_patch_t patch;                           /* the sound being played; edit, then post SC_EV_PATCH */
    sc_perf_t perf;                             /* the joystick section; edit, then post SC_EV_PERF (or PATCH) */
    sc_layer_state_t layer[2];
    uint32_t clock;                             /* orders voice starts and releases */
    sc_event_t q[SC_QUEUE];
    volatile uint32_t q_w, q_r;
    int32_t bus[2][256];                        /* scratch: each layer's voices, mono Q20 */
    sc_limiter_t lim;
    sc_reverb_t rev;                            /* after both layers' choruses: one for the two */
} sc_synth_t;

void sc_init(sc_synth_t *s, const sc_patch_t *patch);
void sc_perf_default(sc_perf_t *p);
/* from any context with one writer: queue an event for the next sc_render */
int  sc_post(sc_synth_t *s, uint8_t type, uint8_t note, int32_t val);
/* the audio interrupt: add n (<= 256) stereo frames (L R, Q15) into out */
void sc_render(sc_synth_t *s, int32_t *out, uint32_t n);
/* the same events, applied at once (only from the context that calls sc_render) */
void sc_note_on(sc_synth_t *s, int note, int32_t vel_q16);
void sc_note_off(sc_synth_t *s, int note);
void sc_all_off(sc_synth_t *s);
void sc_pitch_bend(sc_synth_t *s, int32_t semis_q16);
void sc_reprep(sc_synth_t *s);
/* the reverb alone (sc_render runs it; these are for tests): set it from the patch's, then L R (Q20)
 * in place, n <= 256 */
void sc_reverb_prep(sc_reverb_t *r, const sc_reverb_p_t *p);
void sc_reverb_run(sc_reverb_t *r, int32_t *L, int32_t *R, uint32_t n);

#endif
