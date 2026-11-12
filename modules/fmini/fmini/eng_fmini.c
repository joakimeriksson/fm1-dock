/* SPDX-License-Identifier: GPL-3.0-only
 * FMINI (FMini-1): a monosynth in the style of the classic three-oscillator Moog instruments.
 *
 * Signal path (per voice): three oscillators (TRI, SHARK, SAW, SQR, wide and narrow PULSE, band-limited by polyBLEP,
 * ranges LO 32' 16' 8' 4' 2') and a noise source (white or pink) -> the mixer (a level each, the sum saturates softly:
 * the "overload" of the original) -> a 24 dB/oct transistor-ladder low-pass modelled on the Minimoog's (see below)
 * with its own contour (ATTACK DECAY SUSTAIN; the release takes the DECAY time) and keyboard tracking at 1 V/oct. The
 * loudness contour is the host's
 * ADSR (P_ATK .. P_REL). Modulation: the track's LFO (or noise, MOD MIX) to the pitch (MOD OSC) and the filter
 * (MOD FLT), its depth E7 MOD (the matrix's MODW source can drive it); the mod wheel (CC1) is a vibrato on the pitch, +-1 semitone.
 *
 * The sound is the track's patch (fmini_patch, FG_SIZE bytes, stored in projects and user presets in the slot of the
 * FM6 patch: the track plays one engine at a time, project.c, up_fm6.c); the eight EDIT values are the ones to play
 * with (E0 CUT, E1 EMPH, E2 CNTR the contour's amount, E3 F.ATK, E4 F.DEC, E5 F.SUS, E6 MOD, E7 SLOT 1..100 / OWN).
 * The patch holds the rest (ranges, waves, tuning, mixer, switches, keyboard tracking, TUNE). Edited with the FMINI
 * pages (ui_fmini.c). State per voice: s[0..3] the ladder, s[4] the feedback's lag pole (all Q8), s[7] the contour (stage << 24 |
 * level, Q23); the noise is the part's (fmini_nbuf: one source, made once a block). */
#define ENGI_FMINI 14u          /* engines.c ENGINES[] (append-only) */
#if FELUCCA_FMINI_ONLY
#define FMINI_POLY 12             /* FMini-1: POLY mode plays 12 voices */
#else
#define FMINI_POLY 4
#endif
#define FG_SIZE 32u
#define FMINI_NFACTORY 200u               /* 1.14.1: the factory patches, 000..199 (eng_fmini_presets.h); the slots are the same count */
#define FMINI_NSLOT 200u                  /* the slots (0..199 factory: fmini_bank.c) */
#define FMINI_OWN FMINI_NSLOT
#define FMINI_PACKED 128u          /* the slot of an FM6 patch in a project / a user preset (eng_fm6.c) */

enum {                           /* the patch's bytes */
    FG_R1, FG_R2, FG_R3,         /* range: 0 LO, 1 32', 2 16', 3 8', 4 4', 5 2' */
    FG_W1, FG_W2, FG_W3,         /* wave: 0 TRI, 1 SHARK, 2 SAW, 3 SQR, 4 PLS W, 5 PLS N, 6 SIN */
    FG_F2, FG_F3,                /* OSC 2 / 3 frequency, 64 = 0, +-7 semitones */
    FG_L1, FG_L2, FG_L3, FG_LN,  /* mixer levels 0..127 (a squared curve) */
    FG_SW1,                      /* FS_ bits */
    FG_SW2,                      /* FT_ bits */
    FG_TUNE,                     /* 64 = A 440, +-100 cents */
    FG_MMIX,                     /* modulation source: 0 LFO .. 127 noise */
    FG_KBD,                      /* keyboard tracking of the cutoff, 0..127 = 0..1 */
    FG_GLID,                     /* glide time kept while the GLIDE switch is off (P_GLIDE holds it while on) */
    FG_DC2L, FG_DC2H,            /* DET 2 in cents: a signed 16-bit (low, high byte), -700 .. +700 (1.14.12) */
    FG_DC3L, FG_DC3H,            /* DET 3 in cents, the same */
    FG_BPML, FG_BPMH,            /* TEMPO in BPM, 7 bits each (lo, hi): 40 .. 240; 0 = none (the song's tempo stays) (1.14.25) */
    FG_NUM
};
enum { FS_O1 = 1, FS_O2 = 2, FS_O3 = 4, FS_NZ = 8, FS_PINK = 16, FS_MOSC = 32, FS_MFLT = 64 };   /* FG_SW1 */
enum { FT_GLIDE = 1, FT_SQR = 2, FT_HP = 4 };                                                      /* FG_SW2 (FT_HP: the high-pass output, 1.x) */

/* DET 2 / 3, in cents: the signed 16-bit at bytes lo, lo + 1 (FG_DC2L, FG_DC3L). The coarse byte FG_F2 / FG_F3 (64 = 0,
 * 11 cents a step) is what a CC writes and is kept near the cents for the editor; the sound uses the cents (1.14.12). */
static int32_t fmini_det(const uint8_t *g, uint32_t lo)
{
    int32_t m = g[lo] | (int32_t)(g[lo + 1] & 7) << 7;
    return g[lo + 1] & 8 ? -m : m;
}
/* the cents c (|c| <= 1023) as the two 7-bit bytes at lo: the magnitude low 7 bits, then bits 7..9 with the sign at bit 3
 * (7 bits each: the patch travels as 7-bit sysex; all zero = 0 cents) */
static void fmini_det_put(uint8_t *g, uint32_t lo, int32_t c)
{
    int32_t m = c < 0 ? -c : c;
    g[lo] = (uint8_t)(m & 127);
    g[lo + 1] = (uint8_t)((m >> 7 & 7) | (c < 0 ? 8 : 0));
}

static uint8_t fmini_patch[NTRK][FG_SIZE];        /* the tracks' patches (main loop writes, then fmini_pgen) */
static volatile uint8_t fmini_pgen[NTRK];         /* +1 after each write of fmini_patch[t] */
static uint16_t fmini_slot[NTRK];                 /* the SLOT value last loaded (main loop); 0xFFFF = none (1.14.0: 256 = OWN) */
#define FMINI_LOAD_MS 50u                 /* a SLOT that keeps turning loads when it stops for this long (1.14.0) */
static uint16_t fmini_want[NTRK];         /* the slot the knob points to, not loaded yet (0xFFFF: none) */
static uint32_t fmini_want_ms[NTRK];      /* when fmini_want last changed */
static uint8_t fmini_own[NTRK][FG_SIZE];          /* the own patch while SLOT shows a factory one (fmini_poll) */
static uint8_t fmini_own_ok;                      /* bit tr: fmini_own[tr] holds it */
static struct {                                  /* the patch as the audio ISR sees it (taken at a block's start) */
    uint8_t p[FG_SIZE];
    uint8_t gen, ok;
} fmini_eff[NTRK];
static int32_t fmini_nst[NTRK] = {0x2F6E2B1, 0x1A2B3C5, 0x3C4D5E7, 0x0F1E2D3};   /* a part's noise generator */
static int32_t fmini_pk[NTRK][3];                 /* its pink filter */
static int32_t fmini_nbuf[NTRK][CTL];             /* the part's noise for this block, +-32768: one source, all its voices */
static int32_t fmini_sm[NTRK][3];                 /* the smoothed CUTOFF, EMPH, CNTR knobs (Q16): the encoder's steps become ramps */
static uint8_t fmini_sm_ok;                       /* bit tr: fmini_sm[tr] follows the knobs (clear = snap to them) */
static int32_t fmini_mn[NTRK];                    /* the modulation noise of this block, +-16384 */

/* the highest value of each byte */
static uint32_t fmini_max(uint32_t i)
{
    static const uint8_t MAXV[FG_NUM] = {5, 5, 5, 6, 6, 6, 127, 127, 127, 127, 127, 127, 127, 7, 127,   /* FG_SW2: FT_GLIDE, FT_SQR, FT_HP */ 127, 127, 127, 127, 15, 127, 15, 127, 127};
    return i < FG_NUM ? MAXV[i] : 0u;
}

static void fmini_sanitize(uint8_t *v)
{
    uint32_t i;
    for (i = 0; i < FG_SIZE; i++)
        v[i] = i < FG_NUM ? (uint8_t)(v[i] > fmini_max(i) ? fmini_max(i) : v[i]) : 0;
}

/* --------------------------------------------------------- factory patches --- */
#define FGP(r1, r2, r3, w1, w2, w3, f2, f3, l1, l2, l3, ln, s1, s2, tn, mm, kb, gl, d2l, d2h, d3l, d3h, bl, bh) \
    {r1, r2, r3, w1, w2, w3, f2, f3, l1, l2, l3, ln, s1, s2, tn, mm, kb, gl, d2l, d2h, d3l, d3h, bl, bh}
#define ALLO (FS_O1 | FS_O2 | FS_O3)
/* what a factory patch sets beyond its preset_t (a load: fmini_app.c fa_factory_extras): the arpeggiator (ON, the MODE the ARP
 * button turns on, RATE, OCT, GATE, SWG, PROB), the LFO (RATE, WAVE, FADE, SYNC), the common modulation (LFO -> pitch, LFO ->
 * amplitude, the amplitude contour -> pitch: P_LD_PIT P_LD_AMP P_ED_PIT, voice.c), the patch's LEVEL (P_LEVEL: every patch as loud as the next), the twelve FX settings (G_RTYPE G_RSIZE G_RDAMP
 * G_DTIME G_DRREV G_DFDBK G_ENGGO G_CDEPTH G_CRATE G_INFO G_DRLVL G_MIDI: the reverb TYPE DECAY TONE, the delay TYPE TIME FEEDBK,
 * the chorus TYPE DEPTH RATE, the drive TYPE TONE OUTPUT) and the keys' octave */
typedef struct {
    uint8_t arp[7], lfo[4], fx[12], level;
    int8_t mod[3], oct;
} fmini_x_t;
#include "eng_fmini_presets.h"          /* FMINI_FACTORY (the 200 patches) and FMINI_PRESETS: tools/gen_fmini_presets.py */
#include "eng_fmini_filter.h"           /* the ladder's laws: FMINI_CUT FMINI_K FMINI_TRIM FMINI_G FMINI_GX (tools/fmini_filter.py) */

/* --------------------------------------------------------------- the patch --- */
/* track tr's patch = v (FG_SIZE bytes, sanitized). Main loop: the ISR takes it at its next block */
static void fmini_set_patch(uint32_t tr, const uint8_t *v)
{
    uint8_t s[FG_SIZE];
    tr %= NTRK;
    memcpy(s, v, FG_SIZE);
    fmini_sanitize(s);
    memcpy(fmini_patch[tr], s, FG_SIZE);
    RING_PUBLISH();
    fmini_pgen[tr]++;
    if (tr == song.sel % NTRK && (s[FG_BPML] | s[FG_BPMH] << 7))
        song.g[G_BPM] = (int16_t)clamp(s[FG_BPML] | s[FG_BPMH] << 7, 40, 240);
}
/* the tempo of patch g (BPM; 0 = none) */
static uint32_t fmini_bpm(const uint8_t *g) { return g[FG_BPML] | (uint32_t)g[FG_BPMH] << 7; }

static void fmini_slot_patch(uint32_t s, uint8_t *dst);   /* fmini_bank.c: the patch of a slot's record */

/* SLOT value s < FMINI_NSLOT: that slot's patch into track tr */
static void fmini_load_slot(uint32_t tr, uint32_t s)
{
    uint8_t p[FG_SIZE];
    s %= FMINI_NSLOT;
    fmini_slot_patch(s, p);
    fmini_set_patch(tr, p);
    fmini_slot[tr % NTRK] = (uint16_t)s;
}

/* track tr's patch was just put in as its own (a project, a user preset, the editor): SLOT shows the factory patch it
 * equals unchanged (F n), else OWN; nothing reloads over it (fmini_poll) */
static void fmini_adopt(uint32_t tr)
{
    track_t *t = &trk[tr % NTRK];
    int32_t s = t->p[P_E7];
    uint32_t k, f = s >= 0 && s < (int32_t)FMINI_NFACTORY ? (uint32_t)s : 0u;
    tr %= NTRK;
    fmini_own_ok &= (uint8_t)~(1u << tr);
    if (t->eng_req == ENGI_FMINI && s >= (int32_t)FMINI_NFACTORY && s < (int32_t)FMINI_NSLOT) {
        uint8_t p[FG_SIZE];                       /* a slot past the factory's (none now: 200 of 200): a saved one; it stays that slot if its patch is the same */
        fmini_slot_patch((uint32_t)s, p);
        if (memcmp(p, fmini_patch[tr], FG_SIZE))
            s = (int32_t)FMINI_OWN;
        t->p[P_E7] = (int16_t)s;
    } else if (t->eng_req == ENGI_FMINI) {
        for (k = 0; k < FMINI_NFACTORY; k++)
            if (!memcmp(FMINI_FACTORY[(f + k) % FMINI_NFACTORY], fmini_patch[tr], FG_SIZE))
                break;
        s = k < FMINI_NFACTORY ? (int32_t)((f + k) % FMINI_NFACTORY) : (int32_t)FMINI_OWN;
        t->p[P_E7] = (int16_t)s;
    }
    fmini_slot[tr] = (uint16_t)s;
}

/* a factory sound load put a SLOT value in (a preset, an engine change): its patch (OWN: the patch stays) */
static void fmini_track_loaded(const track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk);
    if (tr < NTRK && t->eng_req == ENGI_FMINI) {
        fmini_own_ok &= (uint8_t)~(1u << tr);
        if (t->p[P_E7] >= 0 && t->p[P_E7] < (int32_t)FMINI_NSLOT)
            fmini_load_slot(tr, (uint32_t)t->p[P_E7]);
        else
            fmini_adopt(tr);
    }
}

static void fb_names_init(void);                  /* fmini_bank.c: the RAM presets (before the first apply_preset_to) */
static void fmini_init(void)                      /* power-on: every track F1 */
{
    uint32_t tr;
    fb_names_init();
    for (tr = 0; tr < NTRK; tr++) {
        fmini_set_patch(tr, FMINI_FACTORY[0]);
        fmini_slot[tr] = 0xFFFFu;
        fmini_want[tr] = 0xFFFFu;
    }
}

/* main loop: SLOT turned (a knob, the editor, MIDI, motion) -> that factory patch; from OWN the own patch is kept
 * aside (fmini_own), and turning back to OWN brings it back */
static void fmini_poll(void)
{
    uint32_t tr, bit;
    for (tr = 0; tr < NTRK; tr++) {
        int32_t s = trk[tr].p[P_E7];
        if (trk[tr].eng_req != ENGI_FMINI || s == fmini_slot[tr]) {
            fmini_want[tr] = 0xFFFFu;
            continue;
        }
        bit = 1u << tr;
        if (s >= 0 && s < (int32_t)FMINI_NSLOT) {  /* a slot: loads once the knob has been still for FMINI_LOAD_MS */
            if (fmini_want[tr] != (uint16_t)s) {
                fmini_want[tr] = (uint16_t)s;
                fmini_want_ms[tr] = fm1_ms;
                continue;
            }
            if (fm1_ms - fmini_want_ms[tr] < FMINI_LOAD_MS)
                continue;
        }
        if (s < 0 || s >= (int32_t)FMINI_NSLOT) {                                   /* OWN */
            if (fmini_own_ok & bit)
                fmini_set_patch(tr, fmini_own[tr]);
            fmini_own_ok &= (uint8_t)~bit;
            fmini_slot[tr] = FMINI_OWN;
            if (s != (int32_t)FMINI_OWN)
                trk[tr].p[P_E7] = FMINI_OWN;
            continue;
        }
        if (fmini_slot[tr] == FMINI_OWN && !(fmini_own_ok & bit)) {
            memcpy(fmini_own[tr], fmini_patch[tr], FG_SIZE);
            fmini_own_ok |= (uint8_t)bit;
        }
        fmini_load_slot(tr, (uint32_t)s);
    }
}

/* once a block and part, before its voices: the ISR's copy of the patch, the noise (one source for all the part's
 * voices, white or pink: Kellet's filter) and the modulation noise */
static void fmini_block(track_t *t)
{
    uint32_t tr = (uint32_t)(t - trk), i;
    const uint8_t *g;
    if (tr >= NTRK)
        return;
    if (!fmini_eff[tr].ok || fmini_eff[tr].gen != fmini_pgen[tr]) {
        fmini_eff[tr].gen = fmini_pgen[tr];
        memcpy(fmini_eff[tr].p, fmini_patch[tr], FG_SIZE);
        fmini_eff[tr].ok = 1;
    }
    for (i = 0; i < NVOICE && !t->v[i].active; i++)
        ;
    {   /* the knobs' steps (one encoder click = 1/127) glide: a one-pole per block, time constant 200 ms (275 blocks of
         * 0.73 ms: 1/275 = 238/65536 of the distance a block), exact landing; silent = snap */
        static const uint8_t KN[3] = {P_E0, P_E1, P_E2};
        uint32_t j;
        for (j = 0; j < 3; j++) {
            int32_t want = (int32_t)clamp(t->p[KN[j]], 0, 127) << 16, d = want - fmini_sm[tr][j], st;
            if (i == NVOICE || !(fmini_sm_ok & (1u << tr))) {
                fmini_sm[tr][j] = want;
                continue;
            }
            st = d >= 0 ? (int32_t)(((int64_t)d * 238) >> 16) : -(int32_t)(((int64_t)-d * 238) >> 16);
            if (!st)
                st = d > 0 ? 1 : d < 0 ? -1 : 0;
            fmini_sm[tr][j] += st;
        }
        if (i == NVOICE)
            fmini_sm_ok &= (uint8_t)~(1u << tr);
        else
            fmini_sm_ok |= (uint8_t)(1u << tr);
    }
    if (i == NVOICE)
        return;                                              /* nothing sounds: no noise to make */
    g = fmini_eff[tr].p;
    fmini_mn[tr] = (int32_t)(noise32(&fmini_nst[tr]) >> 17) - 16384;
    if (g[FG_SW1] & FS_NZ) {
        int32_t b0 = fmini_pk[tr][0], b1 = fmini_pk[tr][1], b2 = fmini_pk[tr][2];
        for (i = 0; i < CTL; i++) {
            int32_t w = (int32_t)(noise32(&fmini_nst[tr]) >> 16) - 32768;   /* white, +-32768 */
            if (g[FG_SW1] & FS_PINK) {
                b0 += ((w * 3245) >> 15) - ((b0 * 77) >> 15);              /* 0.0990 in, 0.99765 kept */
                b1 += ((w * 9716) >> 15) - ((b1 * 1212) >> 15);            /* 0.2965 in, 0.963 kept */
                b2 += ((w * 34495) >> 15) - ((b2 * 14090) >> 15);          /* 1.0527 in, 0.57 kept */
                w = (b0 + b1 + b2 + ((w * 6056) >> 15)) >> 1;
            }
            fmini_nbuf[tr][i] = w;
        }
        fmini_pk[tr][0] = b0;
        fmini_pk[tr][1] = b1;
        fmini_pk[tr][2] = b2;
    }
}

/* ------------------------------------------------------------------ voice --- */
static inline int32_t fmini_shark(uint32_t ph)            /* a triangle that rises for 3/4 of the cycle */
{
    uint32_t x = ph >> 16;
    return x < 49152u ? (int32_t)((x * 43691u) >> 15) - 32768 : 32767 - (int32_t)((x - 49152u) * 4u);
}

/* one oscillator of the mix: mix[i] += its sample at level g (Q15); a loop per wave */
static void fmini_wave(uint32_t w, uint32_t *php, uint32_t inc, int32_t g, int32_t *mix, uint32_t n)
{
    uint32_t ph = *php, i;
    switch (w) {
    case 0:
        for (i = 0; i < n; i++) {
            mix[i] += mulq15(osc_tri(ph), g);
            ph += inc;
        }
        break;
    case 1:
        for (i = 0; i < n; i++) {
            mix[i] += mulq15(fmini_shark(ph), g);
            ph += inc;
        }
        break;
    case 2:
        for (i = 0; i < n; i++) {
            mix[i] += mulq15(osc_saw(ph, inc), g);
            ph += inc;
        }
        break;
    case 3:
        for (i = 0; i < n; i++) {
            mix[i] += mulq15(osc_pulse(ph, inc, 0x80000000u), g);
            ph += inc;
        }
        break;
    case 4:
        for (i = 0; i < n; i++) {
            mix[i] += mulq15(osc_pulse(ph, inc, 0x55555555u), g);
            ph += inc;
        }
        break;
    case 6:                                             /* SIN: the sine table (dsp.c), no harmonics to fold back */
        for (i = 0; i < n; i++) {
            mix[i] += mulq15(osc_sine(ph), g);
            ph += inc;
        }
        break;
    default:
        for (i = 0; i < n; i++) {
            mix[i] += mulq15(osc_pulse(ph, inc, 0x1C71C71Cu), g);
            ph += inc;
        }
        break;
    }
    *php = ph;
}

/* range of an oscillator in 1/16 semitones (LO: very low) */
static int32_t fmini_range16(uint32_t r)
{
    static const int16_t R[6] = {-48 * 16, -24 * 16, -12 * 16, 0, 12 * 16, 24 * 16};
    return R[r % 6u];
}

static void fmini_note_on(track_t *t, voice_t *v)
{
    if (!v->env && !v->env_out) {                          /* from silence: the ladder and the contour from 0 */
        v->s[0] = v->s[1] = v->s[2] = v->s[3] = 0;
        v->s[4] = 0;
        v->s[7] = 0;
        if (t->p[P_E1] >= 96)                                         /* it will sing: a nudge to start from (the hardware has its own noise) */
            v->s[0] = (noise32(&fmini_nst[(uint32_t)(t - trk) % NTRK]) & 0x80000000u) ? 700 * 256 : -700 * 256;
        v->s[5] = (int32_t)(noise32(&fmini_nst[(uint32_t)(t - trk) % NTRK]) >> 20) - 2048;   /* the VCOs start a little apart: drift, cents * 256 */
        v->s[6] = (int32_t)(noise32(&fmini_nst[(uint32_t)(t - trk) % NTRK]) >> 20) - 2048;
    }
    v->s[7] = (int32_t)(1u << 24) | (v->s[7] & 0xFFFFFF);  /* the contour attacks from where it is */
}

/* the contour, once a block: stage 1 attack, 2 decay to the sustain, 3 release (the DECAY time); level Q23 */
static int32_t fmini_contour(const int16_t *p, voice_t *v, int32_t kr)
{
    uint32_t st = (uint32_t)v->s[7] >> 24;
    int32_t e = v->s[7] & 0xFFFFFF, sus = (int32_t)p[P_E5] << 16, k = (int32_t)ENV_EXP[p[P_E4] & 127];
    if (!v->gate && st && st != 3u)
        st = 3;
    switch (st) {
    case 1:
        {   /* the capacitor charges toward a level above full: a concave rise that ends in a corner, as in the Minimoog */
            int32_t step = (int32_t)(ENV_LIN[p[P_E3] & 127] >> 1), d = (int32_t)(((int64_t)((5 << 21) - e) * step * 824) >> 32);
            e += d < 1 ? 1 : d;
        }
        if (e >= (1 << 23)) {
            e = 1 << 23;
            st = 2;
        }
        break;
    case 2:
        e += mulq16(sus - e, (uint32_t)k);                  /* the DECAY time is the drop from the peak to the SUSTAIN level, whatever it is */
        break;
    case 3:
        e -= mulq16(e, (uint32_t)kr);                       /* the RELEASE: its own time (E4 when the preset has none) */
        break;
    default:
        break;
    }
    v->s[7] = (int32_t)(st << 24) | (e & 0xFFFFFF);
    return e >> 8;                                          /* Q15 */
}

/* a * k / 65536 rounded to the nearest (mulq16 floors: four stages of it leave a bias that a quiet, low ladder cannot sing against) */
static inline int32_t fmini_mulr(int32_t a, uint32_t k)
{
    return (int32_t)(((a >> 16) * (int32_t)k) + (int32_t)(((uint32_t)(a & 0xFFFF) * k + 32768u) >> 16));
}

/* The filter, after the Minimoog's (tools/fmini_filter.py has the laws and the reasoning, the CA-72 project's calibration the
 * measurements): four equal one-pole stages, each a differential pair (tanh in front), the last stage's output fed back to
 * the first through a lag pole (the emphasis network), solved without a delay in the loop (zero-delay feedback: each stage
 * is a trapezoidal integrator, G = g / (1 + g), and the feedback is found from the stages' states before the sample is
 * processed). The lag pole puts the loop's 180-degree point at 0.686 of the stage pole: the filter sings at 1.578 times
 * its -3 dB corner, and it sings when the feedback k reaches 2.92. The knobs and the pitch work in 1/16 semitones of the
 * singing pitch (MIDI x 16): CUTOFF is the dial -5 .. +5 (FMINI_CUT), the keyboard adds 1 V/oct from F (MIDI 41) at KBD 127,
 * the contour up to 5 octaves; EMPHASIS is the dial 0 .. 10 (FMINI_K) with the hardware's passband loss (FMINI_TRIM). With
 * the mixer silent and EMPHASIS past 8 the ladder sings a sine at the singing pitch, 1 V/oct over the keyboard.
 * Q formats: signals +-32768 (tanh(1)); the states Q8 (a quiet ladder at a low cutoff moves less than a LSB a sample); G Q16; the prediction's coefficients c Q14; k Q12. */
static void fmini_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    const uint8_t *g = fmini_eff[(uint32_t)(t - trk) % NTRK].p;
    int32_t mix[CTL];
    int32_t env15, mod, lfo, vib, lc, p16, fine, k12, G16, gx16, c0, c1, c2, c3, inv14, kinv12, trim12;
    int32_t l1 = 0, l2 = 0, l3 = 0, ln = 0, dr1, dr2, dr3;
    uint32_t hp = (uint32_t)(g[FG_SW2] & FT_HP) ? 1u : 0u;      /* the output: LP (the last stage) or HP ((1 - LP)^4 of the same stages) */
    uint32_t tr = (uint32_t)(t - trk) % NTRK, i, sw = g[FG_SW1];
    uint32_t inc1, inc2, inc3;
    int32_t s0 = v->s[0], s1 = v->s[1], s2 = v->s[2], s3 = v->s[3], sx = v->s[4];
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2];
    if (n > CTL)
        n = CTL;
    env15 = fmini_contour(p, v, (int32_t)ENV_EXP[FMINI_FREL(t)]);

    /* modulation: the LFO (a square when FT_SQR) or noise held for the block, MOD deep */
    lfo = mulq15(t->lfo_val, t->lfo_fade);
    if (g[FG_SW2] & FT_SQR)
        lfo = lfo >= 0 ? 32767 : -32767;
    mod = mulq15(lfo, 32767 - (int32_t)g[FG_MMIX] * 258) + mulq15(fmini_mn[tr] << 1, (int32_t)g[FG_MMIX] * 258);
    mod = mulq15(mod, (int32_t)p[P_E6] * 258);                          /* the patch's MOD: Q15 of its depth */
    /* the mod wheel (CC1, t->mw) is the vibrato: the LFO on the pitch, +-1 semitone at full wheel, whatever MOD OSC is */
    vib = (mulq15(lfo, (int32_t)t->mw * 258) * 16) >> 15;

    /* oscillator pitches */
    p16 = m->pitch16 + vib + (sw & FS_MOSC ? (mod * 16) >> 14 : 0);    /* full depth: +-2 semitones */
    fine = m->fine + ((int32_t)g[FG_TUNE] - 64) * 237 / 64 + glo_tune_c * 237 / 100;   /* + the GLO TUNING (A 410..470) */
    {   /* the VCOs wander a few cents (temperature, the integrators): a slow random walk per oscillator, about +-2 cents */
        int32_t d1 = v->s[5], d2 = v->s[6];
        d1 += ((int32_t)(noise32(&fmini_nst[tr]) >> 20) - 2048 >> 6) - (d1 >> 11);
        d2 += ((int32_t)(noise32(&fmini_nst[tr]) >> 20) - 2048 >> 6) - (d2 >> 11);
        v->s[5] = d1;
        v->s[6] = d2;
        dr1 = d1 >> 8;
        dr2 = d2 >> 8;
        dr3 = (d1 - d2) >> 9;
    }
    inc1 = cents_inc(clamp(p16 + fmini_range16(g[FG_R1]), 0, 2047), dr1, fine);
    inc2 = cents_inc(clamp(p16 + fmini_range16(g[FG_R2]), 0, 2047), fmini_det(g, FG_DC2L) + dr2, fine);
    inc3 = cents_inc(clamp(p16 + fmini_range16(g[FG_R3]), 0, 2047), fmini_det(g, FG_DC3L) + dr3, fine);

    /* mixer levels: a squared curve */
    if (sw & FS_O1) l1 = (int32_t)(g[FG_L1] * g[FG_L1] * 2) + 0;
    if (sw & FS_O2) l2 = (int32_t)(g[FG_L2] * g[FG_L2] * 2);
    if (sw & FS_O3) l3 = (int32_t)(g[FG_L3] * g[FG_L3] * 2);
    if (sw & FS_NZ) ln = (int32_t)(g[FG_LN] * g[FG_LN] * 2);
    for (i = 0; i < n; i++)
        mix[i] = 0;
    if (l1)
        fmini_wave(g[FG_W1], &ph0, inc1, l1, mix, n);
    else
        ph0 += inc1 * n;
    if (l2)
        fmini_wave(g[FG_W2], &ph1, inc2, l2, mix, n);
    else
        ph1 += inc2 * n;
    if (l3)
        fmini_wave(g[FG_W3], &ph2, inc3, l3, mix, n);
    else
        ph2 += inc3 * n;
    if (ln)
        for (i = 0; i < n; i++)
            mix[i] += mulq15(fmini_nbuf[tr][i], ln);

    /* the singing pitch (MIDI x 16): the CUTOFF knob's dial, the contour, the keyboard, the modulation, the host's (LFO / matrix / ENV routings) */
    {
        int32_t kn = fmini_sm[tr][0], i0 = kn >> 16, fr = (kn >> 8) & 255;
        lc = FMINI_CUT[i0];
        if (i0 < 127)
            lc += ((FMINI_CUT[i0 + 1] - lc) * fr) >> 8;
    }
    lc += (m->cutoff * 55) >> 10;                                                                           /* the host's steps (1/14 octave) in 1/16 semitones */
    lc += (int32_t)(((int64_t)env15 * fmini_sm[tr][2] / (127 * 65536)) * 960) >> 15;                          /* the contour: up to 5 octaves */
    lc += ((v->pitch_cur - FMINI_NOTE0 * 16) * (int32_t)g[FG_KBD] * 516) >> 16;                              /* the keyboard: 1 V/oct at 127, 0 V on F */
    if (sw & FS_MFLT)
        lc += (mod * 547) >> 15;                                                                            /* +-2.85 octaves at full depth */
    lc = clamp(lc, FMINI_GM0 * 16, FMINI_FOSC_MAX16);
    {   /* the stage pole G and the lag pole's from the tables (one entry a semitone, linear between), the feedback solved ahead */
        uint32_t ix = (uint32_t)((lc >> 4) - FMINI_GM0), fr = (uint32_t)lc & 15u, gq, gxq, g2, g3, g4, om;
        gq = FMINI_G[ix] + (uint32_t)(((int32_t)(FMINI_G[ix + 1] - FMINI_G[ix]) * (int32_t)fr) >> 4);
        gxq = FMINI_GX[ix] + (uint32_t)(((int32_t)(FMINI_GX[ix + 1] - FMINI_GX[ix]) * (int32_t)fr) >> 4);
        G16 = (int32_t)((gq + 128u) >> 8);
        gx16 = (int32_t)((gxq + 128u) >> 8);
        om = 65536u - (uint32_t)G16;
        g2 = ((uint32_t)G16 * (uint32_t)G16) >> 16;
        g3 = (g2 * (uint32_t)G16) >> 16;
        g4 = (g3 * (uint32_t)G16) >> 16;
        c3 = (int32_t)(om >> 2);                                   /* y4 = G^4 u + c0 s0 + c1 s1 + c2 s2 + c3 s3 */
        c2 = (int32_t)(((uint32_t)G16 * om) >> 18);
        c1 = (int32_t)((g2 * om) >> 18);
        c0 = (int32_t)((g3 * om) >> 18);
        {   /* EMPHASIS: the feedback gain k and the passband's loss, from the knob (Q16) */
            int32_t kn = fmini_sm[tr][1], i0 = kn >> 16, fk = (kn >> 8) & 255, den;
            k12 = FMINI_K[i0];
            trim12 = FMINI_TRIM[i0];
            if (i0 < 127) {
                k12 += ((FMINI_K[i0 + 1] - k12) * fk) >> 8;
                trim12 += ((FMINI_TRIM[i0 + 1] - trim12) * fk) >> 8;
            }
            den = 65536 + ((k12 * (int32_t)((((uint32_t)gxq >> 8) * g4) >> 16)) >> 12);        /* 1 + k Gx G^4: the loop around the whole sample */
            inv14 = (int32_t)((1u << 30) / (uint32_t)den);
            kinv12 = (k12 * inv14) >> 14;
        }
    }

    for (i = 0; i < n; i++) {
        int32_t x = softclip(mix[i]), S, fbc, u, xin, v1, v2, v3, v4, y1, y2, y3, y4, w, o;   /* the mixer overloads softly */
        S = (c0 * ((s0 + 128) >> 8) + c1 * ((s1 + 128) >> 8) + c2 * ((s2 + 128) >> 8) + c3 * ((s3 + 128) >> 8)) >> 14;   /* what the stages already hold */
        fbc = sx + fmini_mulr((S << 8) - sx, (uint32_t)gx16);              /* through the lag pole (Q8) */
        u = ((x * inv14) >> 14) - ((kinv12 * ((fbc + 128) >> 8)) >> 12);  /* the input less the feedback, the loop solved */
        xin = softclip(u) << 8;                                              /* the first stage's input (Q8) */
        v1 = fmini_mulr(xin - s0, (uint32_t)G16);                          /* each stage is a differential pair: tanh in front of it */
        y1 = v1 + s0;
        s0 = y1 + v1;
        v2 = fmini_mulr((softclip((y1 + 128) >> 8) << 8) - s1, (uint32_t)G16);
        y2 = v2 + s1;
        s1 = y2 + v2;
        v3 = fmini_mulr((softclip((y2 + 128) >> 8) << 8) - s2, (uint32_t)G16);
        y3 = v3 + s2;
        s2 = y3 + v3;
        v4 = fmini_mulr((softclip((y3 + 128) >> 8) << 8) - s3, (uint32_t)G16);
        y4 = v4 + s3;
        s3 = y4 + v4;
        w = fmini_mulr(y4 - sx, (uint32_t)gx16);                           /* the lag pole follows the output */
        sx += w + w;
        /* HP: the stages are one-pole lows H; (1 - H)^4 = 1 - 4H + 6H^2 - 4H^3 + H^4 on the stages' outputs (linear, the states as they are) */
        o = hp ? xin - 4 * y1 + 6 * y2 - 4 * y3 + y4 : y4;
        out[i] += voice_amp((((o + 128) >> 8) * trim12) >> 12, m, i) << 1;
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = ph2;
    v->s[0] = s0;
    v->s[1] = s1;
    v->s[2] = s2;
    v->s[3] = s3;
    v->s[4] = sx;
}

/* --------------------------------------------------------------- the engine --- */
/* every slot's preset (name, knob bytes, env, fx, mono) as the generic code reads them: fmini_bank.c fills it from the
 * records at boot (saved or factory) and when a slot is saved. The engine's presets are these 200, and nothing else. */
/* fm1-dock: the factory table itself (take.py; FMini-1 keeps a RAM copy its preset bank fills) */

static engine_t ENG_FMINI = {
    .name = "FMINI",
    .page_title = {"FILTER", "PATCH"},
    .edit = {
        {"CUT", F_MCUT, 0, 127, 64, 0, 0},
        {"EMPH", F_MEMP, 0, 127, 40, 0, 0},
        {"CNTR", F_PCT, 0, 127, 64, 0, 0},
        {"F.ATK", F_TIME, 0, 127, 0, 0, 0},
        {"F.DEC", F_TIME, 0, 127, 64, 0, 0},
        {"F.SUS", F_PCT, 0, 127, 40, 0, 0},
        {"MOD", F_PCT, 0, 127, 0, 0, 0},
        {"SLOT", F_INT, 0, FMINI_OWN, 0, 0, 0},
    },
    .presets = FMINI_PRESETS,
    .npresets = FMINI_NSLOT,
    .note_on = fmini_note_on,
    .render = fmini_render,
    .knob = {P_E0, P_E1, P_E2, P_E4},
    .poly = FMINI_POLY,
    .keep = 0x13,
    .block = fmini_block,
};
