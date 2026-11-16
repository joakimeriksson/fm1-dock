/* SPDX-License-Identifier: GPL-3.0-only */
/* An ARP 2600 (1971) as a monophonic integer engine for the FM-1, with the 1601's sixteen steps beside it.
 * The 2600's normalled patch, fixed: VCO 1 and VCO 2 (saw, pulse with its width, triangle; VCO 2 detuned, or
 * synced to VCO 1), a sub-octave square under VCO 2, the noise generator and the ring modulator (VCO 1 x VCO 2)
 * into the mixer; the 4072 ladder filter, 24 dB, with its resonance up to oscillation, driven by ADSR 1, the
 * keyboard, the LFO and the sample-and-hold; the VCA by ADSR 2; the LFO (triangle, square, saw, S&H) to pitch and
 * pulse width; portamento. The reverb is the host's (hb_host_t.fx_reverb): the patch sends its amount.
 *
 * Integers only: signals Q20 (1.0 = 1 << 20), phases 32-bit, coefficients Q30, pitch in MIDI notes Q16. Only
 * 32-bit divisions, none 64-bit. The sequencer runs in the render, sample-accurate: a step is a sixteenth of the
 * tempo, a gate REST, SHORT (a quarter of the step), LONG (three quarters), TIE (into the next step, no new
 * attack) or END (the sequence starts over here). */
#ifndef ARP_ENGINE_H
#define ARP_ENGINE_H
#include <stdint.h>

#define ARP_SR 44100
#define ARP_STEPS 16
#define ARP_CHAIN 8                             /* a pattern's links */
enum { ARP_SAW, ARP_PULSE, ARP_TRI };           /* a VCO's wave */
enum { ARP_LFO_TRI, ARP_LFO_SQ, ARP_LFO_SAW, ARP_LFO_SH };
enum { ARP_REST, ARP_SHORT, ARP_LONG, ARP_TIE, ARP_END };   /* a step's gate */

typedef struct {                                /* every value 0..100 unless said; a page of four a row */
    uint8_t vco1_wave, vco1_oct, vco1_pw, vco1_lvl;     /* wave ARP_*, oct 0..4 (= -2 .. +2 octaves), pw 5..95 */
    uint8_t vco2_wave, vco2_oct, vco2_detune, vco2_lvl; /* detune 0..100 (= -50 .. +50 cents) */
    uint8_t noise_lvl, ring_lvl, sync, sub_lvl;         /* sync 0 / 1: VCO 2 to VCO 1 */
    uint8_t cutoff, reso, env_amt, kbd;                 /* cutoff 0..100 (= 20 .. 130 in notes), kbd 0..100 (= 0 .. 100 %) */
    uint8_t lfo_vcf, sh_vcf, drive, glide;              /* drive: into the ladder; glide 0..100 (= 0 .. 2 s) */
    uint8_t a1, d1, s1, r1;                             /* ADSR 1, the filter: times 0..100 (= 1 ms .. 10 s) */
    uint8_t a2, d2, s2, r2;                             /* ADSR 2, the amplifier */
    uint8_t lfo_wave, lfo_rate, lfo_pitch, lfo_pw;      /* rate 0..100 (= 0.05 .. 40 Hz), pitch 0..100 (= 0 .. 12 semitones) */
    uint8_t volume, reverb, accent, env_pitch;          /* reverb: the host's mix; accent: a step's extra cutoff and level;
                                                         * env_pitch: ADSR 1 to VCO 2's pitch, 0..100 (= 0 .. 48 semitones) */
    char name[12];
} arp_patch_t;

typedef struct { uint8_t note, gate, accent; } arp_step_t;   /* note: MIDI */
typedef struct {
    arp_step_t step[ARP_STEPS];
    uint16_t tempo;                             /* bpm 40..240 */
    char name[12];
    int8_t tr[ARP_CHAIN];                       /* the pattern: the sequence tr[k] semitones up for bars[k] bars, k = 0 .. n_chain - 1, */
    uint8_t bars[ARP_CHAIN];                    /* then from the first again (two bars as written, one up a step, one up two: four links) */
    uint8_t n_chain;                            /* 0: the sequence as written, over and over */
    uint8_t sweep;                              /* 0..100 (= 0 .. 48 notes): the cutoff up and down once over the pattern (four bars without one) */
} arp_seq_t;

typedef struct {
    int32_t level;                              /* Q30 */
    uint8_t stage;                              /* 0 off, 1 attack, 2 decay, 3 sustain, 4 release */
    int32_t ka, kd, kr, sus;                    /* Q30 coefficients; the sustain level Q30 */
} arp_env_t;

typedef struct {
    arp_patch_t patch;
    arp_seq_t seq;
    /* the voice */
    uint32_t ph1, ph2, ph3, phl;                /* VCO 1, VCO 2, the sub (half VCO 2), the LFO */
    uint32_t inc1, inc2;
    int32_t note_q16, target_q16;               /* the pitch played, and the one glided to (MIDI notes, Q16) */
    int32_t glide_k;                            /* Q30 */
    int32_t s1, s2, s3, s4;                     /* the ladder's stages, Q20 */
    int32_t g, k_res;                           /* the ladder: its coefficient Q30 this block, its feedback Q16 */
    int32_t lfo_out, sh_out;                    /* Q20 */
    uint32_t lfo_inc, seed;
    arp_env_t env1, env2;
    int32_t accent_q16;                         /* the note's accent, 0 or the patch's */
    int32_t slide_k;                            /* Q30: a slide's glide, the patch's or ~60 ms without one */
    uint8_t sliding;                            /* the pitch is gliding by a slide (arp_play) */
    int32_t gate;                               /* a key or step held */
    uint8_t held[16], n_held;                   /* the keys down, last on top */
    int32_t dc_x, dc_y;
    /* the sequencer */
    uint8_t running, pos;
    uint32_t sps_q8;                            /* samples a step, Q8 */
    int32_t counter_q8, gate_left;              /* samples to the next step; samples the step's gate still lasts */
    int8_t transpose;                           /* semitones, from the keys while it runs */
    int8_t chain_tr;                            /* the pattern's link playing: its semitones */
    uint8_t bar, link;                          /* the bar within the link, the link */
    uint32_t chain_step, chain_total;           /* steps into the pattern; steps in it */
    int32_t sweep_q16;                          /* the sweep's cutoff this block, notes Q16 */
    volatile uint32_t steps;                    /* steps played (the position: steps % ARP_STEPS, with END) */
    volatile uint32_t req_seq, seq_done;        /* the settings: taken up at the start of a block */
    uint8_t req_running;
    uint32_t req_delay;                         /* samples into the block the start waits (arp_run_at) */
} arp_t;

void arp_init(arp_t *a);
void arp_set_patch(arp_t *a, const arp_patch_t *p);   /* also after editing a->patch in place: recomputes */
void arp_set_seq(arp_t *a, const arp_seq_t *s);       /* a sequence (its tempo too) */
void arp_set_tempo(arp_t *a, uint32_t bpm);
void arp_run(arp_t *a, uint32_t on);                  /* the sequencer: from its first step */
void arp_run_at(arp_t *a, uint32_t delay);           /* the sequencer from its first step, delay samples into the next block */
/* the keys (any context with one writer): while the sequencer runs a key transposes it */
void arp_note_on(arp_t *a, uint32_t note, uint32_t vel);
void arp_note_off(arp_t *a, uint32_t note);
void arp_all_off(arp_t *a);
/* the explicit performance path, for a host that owns its own note priority (one voice, no key stack: do not
 * mix with arp_note_on/off on one engine). ACCENT: the patch's accent (extra cutoff and level), independent of
 * velocity. LEGATO: with the gate up, the new pitch without a new attack. SLIDE: legato and the pitch glides to
 * it, at the patch's glide or about 60 ms when the patch has none. Without the gate up both are a plain attack.
 * The sequencer must be stopped. */
enum { ARP_PLAY_ACCENT = 1, ARP_PLAY_LEGATO = 2, ARP_PLAY_SLIDE = 4 };
void arp_play(arp_t *a, uint32_t note, uint32_t flags);
void arp_release(arp_t *a);
/* the audio interrupt: add n stereo frames (L R, Q15) into out */
void arp_render(arp_t *a, int32_t *out, uint32_t n);
/* the step playing now (0..15), or -1 stopped */
int arp_position(const arp_t *a);
int arp_link(const arp_t *a);                          /* the pattern's link playing, -1 stopped or none */

#endif
