/* SPDX-License-Identifier: GPL-3.0-only */
/* The ARP 2600's patches and the 1601's sequences, in the manner of Jarre's records of 1976-86: the 2600 was his
 * bass and sequence voice from Oxygène on. The patches are drawn as the 2600's patch book draws them (two saws a
 * few cents apart into the ladder, a pulse against a saw, sync for the laser), with the bass patches kept soft: a
 * 3 to 5 ms attack, the sub under the saws, the drive at the soft clip's knee, the accent a lift and not a jab; the
 * sequence patches a pluck that lasts a step, some with a slow LFO on the filter, the sweep Jarre rode by hand. The
 * sequences are written here in that style, not copied from the records: minor keys, octave leaps, sixteenths with
 * rests and accents. */
#ifndef ARP_PRESETS_H
#define ARP_PRESETS_H
#include "arp_engine.h"

#define ARP_N_PATCHES 10
#define ARP_N_SEQS 8

/* vco1: wave oct pw lvl | vco2: wave oct detune lvl | noise ring sync sub | cutoff reso env kbd | lfo_vcf sh_vcf drive glide |
 * a1 d1 s1 r1 | a2 d2 s2 r2 | lfo: wave rate pitch pw | volume reverb accent env_pitch | name */
static const arp_patch_t ARP_PATCHES[ARP_N_PATCHES] = {
    { ARP_SAW, 2, 50, 80,  ARP_SAW, 2, 56, 70,  0, 0, 0, 50,   38, 12, 40, 50,   0, 0, 25, 0,
      8, 50, 25, 25,  12, 60, 70, 25,  ARP_LFO_TRI, 30, 0, 0,  80, 15, 25, 0,  "WARM BASS" },
    { ARP_PULSE, 2, 40, 80,  ARP_SAW, 2, 53, 60,  0, 0, 0, 20,   36, 42, 50, 30,   0, 0, 25, 0,
      6, 52, 10, 30,  8, 60, 25, 40,  ARP_LFO_TRI, 30, 0, 20,  80, 30, 35, 0,  "PLUCK SEQ" },
    { ARP_PULSE, 2, 50, 70,  ARP_PULSE, 2, 50, 60,  0, 0, 0, 40,   42, 28, 30, 40,   18, 0, 25, 0,
      6, 48, 0, 25,  8, 56, 10, 30,  ARP_LFO_TRI, 10, 0, 0,  80, 25, 35, 0,  "GLASS PLUCK" },
    { ARP_SAW, 2, 50, 80,  ARP_SAW, 2, 58, 80,  0, 0, 0, 0,   60, 25, 30, 60,   0, 0, 35, 35,
      8, 50, 60, 30,  10, 30, 90, 30,  ARP_LFO_TRI, 45, 3, 0,  75, 35, 30, 0,  "ARP LEAD" },
    { ARP_SAW, 2, 50, 0,  ARP_SAW, 3, 50, 100,  0, 0, 1, 0,   70, 10, 0, 50,   0, 0, 35, 0,
      0, 55, 0, 40,  6, 60, 60, 40,  ARP_LFO_TRI, 30, 0, 0,  75, 40, 30, 60,  "LASER SYNC" },
    { ARP_PULSE, 2, 50, 70,  ARP_TRI, 2, 50, 60,  0, 0, 0, 0,   40, 55, 10, 30,   0, 60, 25, 0,
      20, 40, 80, 50,  20, 40, 80, 50,  ARP_LFO_SH, 55, 0, 0,  75, 50, 0, 0,  "S&H SPACE" },
    { ARP_PULSE, 3, 50, 50,  ARP_SAW, 2, 70, 50,  60, 50, 0, 0,   55, 40, 70, 30,   0, 0, 45, 0,
      0, 25, 0, 15,  4, 25, 0, 10,  ARP_LFO_TRI, 30, 0, 0,  80, 20, 60, 0,  "PERCUSSIVE" },
    { ARP_SAW, 2, 50, 75,  ARP_PULSE, 2, 54, 55,  0, 0, 0, 10,   48, 45, 55, 40,   12, 0, 30, 0,
      6, 52, 10, 30,  8, 55, 15, 38,  ARP_LFO_TRI, 8, 0, 0,  80, 35, 30, 0,  "SEQ BASS" },
    { ARP_TRI, 1, 50, 80,  ARP_SAW, 1, 50, 15,  0, 0, 0, 25,   28, 8, 22, 50,   0, 0, 20, 0,
      8, 45, 40, 25,  14, 55, 60, 25,  ARP_LFO_TRI, 30, 0, 0,  85, 10, 25, 0,  "SUB BASS" },
    { ARP_SAW, 2, 50, 80,  ARP_SAW, 2, 45, 80,  0, 0, 0, 0,   50, 25, 38, 40,   14, 0, 30, 0,
      6, 48, 20, 30,  10, 44, 50, 35,  ARP_LFO_TRI, 12, 0, 30,  80, 45, 35, 0,  "TWIN SAWS" },
};

#define S ARP_SHORT
#define L ARP_LONG
#define T ARP_TIE
#define R ARP_REST
#define N(n, g, a) { n, g, a }
/* after the steps: the tempo, the name, then the pattern (semitones a link, bars a link, links) and the sweep: the
 * bass moving under the phrase, two bars as written, then up, then up again, as Jarre's lines go */
static const arp_seq_t ARP_SEQS[ARP_N_SEQS] = {
    /* an E minor arpeggio in sixteenths, the roots accented: the Oxygène II manner */
    { { N(52,S,1), N(55,S,0), N(59,S,0), N(64,S,0), N(50,S,1), N(54,S,0), N(57,S,0), N(62,S,0),
        N(48,S,1), N(52,S,0), N(55,S,0), N(60,S,0), N(47,S,1), N(50,S,0), N(54,S,0), N(59,S,0) }, 128, "E MIN ARP", {0, 0, 3, 5}, {1, 1, 1, 1}, 4, 40 },
    /* a bass under a chord a bar: root, root, octave, root, down the sequence E D C B: the Oxygène IV manner */
    { { N(40,S,1), N(40,S,0), N(52,S,0), N(40,L,0), N(38,S,1), N(38,S,0), N(50,S,0), N(38,L,0),
        N(36,S,1), N(36,S,0), N(48,S,0), N(36,L,0), N(35,S,1), N(35,S,0), N(47,S,0), N(35,L,0) }, 120, "ROOT PULSE", {0, 5, 0, 3}, {2, 1, 1, 1}, 4, 50 },
    /* D minor, every note answered an octave up: the Equinoxe V manner */
    { { N(38,S,1), N(50,S,0), N(41,S,0), N(53,S,0), N(45,S,1), N(57,S,0), N(50,S,0), N(62,S,0),
        N(36,S,1), N(48,S,0), N(40,S,0), N(52,S,0), N(43,S,1), N(55,S,0), N(48,S,0), N(60,S,0) }, 130, "OCTAVE ECHO", {0, -2, 3, -4}, {1, 1, 1, 1}, 4, 45 },
    /* a C minor pulse on the root with the fifth and the third thrown in: Magnetic Fields II */
    { { N(36,S,1), N(36,S,0), N(43,S,0), N(36,S,0), N(39,S,0), N(36,S,0), N(43,S,0), N(36,S,0),
        N(34,S,1), N(34,S,0), N(41,S,0), N(34,S,0), N(38,S,0), N(34,S,0), N(41,S,0), N(34,S,0) }, 138, "C MIN PULSE", {0, 1, 2, 0}, {2, 1, 1, 2}, 4, 60 },
    /* A minor with rests, the line breathing: Equinoxe IV */
    { { N(45,L,1), N(45,R,0), N(45,L,0), N(52,L,0), N(45,R,0), N(45,L,0), N(43,L,0), N(43,R,0),
        N(41,L,1), N(41,R,0), N(41,L,0), N(48,L,0), N(41,R,0), N(41,L,0), N(40,L,0), N(40,R,0) }, 112, "BREATHING", {0, -3}, {2, 2}, 2, 30 },
    /* slow eighths, each note tied over two steps: the Chants Magnétiques manner */
    { { N(40,L,1), N(40,T,0), N(47,L,0), N(47,T,0), N(52,L,0), N(52,T,0), N(50,L,0), N(50,T,0),
        N(48,L,1), N(48,T,0), N(43,L,0), N(43,T,0), N(47,L,0), N(47,T,0), N(40,L,0), N(40,T,0) }, 96, "SLOW TIES", {0, 2}, {2, 2}, 2, 40 },
    /* G minor, a rhythm with holes and an octave jab: the Zoolook manner */
    { { N(43,S,1), N(43,S,0), N(43,R,0), N(43,S,0), N(46,S,0), N(46,R,0), N(43,S,0), N(55,S,1),
        N(41,S,1), N(41,S,0), N(41,R,0), N(41,S,0), N(44,S,0), N(44,R,0), N(41,S,0), N(53,S,1) }, 124, "OCTAVE JAB", {0, 1, 2, 0}, {2, 1, 1, 1}, 4, 35 },
    /* an A minor arpeggio up and down, then G: the Rendez-vous manner */
    { { N(45,S,1), N(48,S,0), N(52,S,0), N(57,S,0), N(60,S,0), N(57,S,0), N(52,S,0), N(48,S,0),
        N(43,S,1), N(47,S,0), N(50,S,0), N(55,S,0), N(59,S,0), N(55,S,0), N(50,S,0), N(47,S,0) }, 134, "UP AND DOWN", {0, -2, -4, -5}, {2, 1, 1, 2}, 4, 45 },
};
#undef S
#undef L
#undef T
#undef R
#undef N

#endif
