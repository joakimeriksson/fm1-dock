/* SPDX-License-Identifier: GPL-3.0-only */
/* The organ engine (tonewheels, drawbars, percussion, vibrato, overdrive, Leslie-lite), shared by the firmware
 * (dinghy.c) and the browser emulator (web/organ_wasm.c). Needs <stdint.h> types, fm1_ticks() and
 * FM1_TICKS_PER_US from whoever includes it, and organ_tab.h before it. Integers only. */

#include "organ_pgm.h"
#include "organ_contrib.h"                    /* setBfree's tapering + leakage, per key and drawbar */

#define NW 91u                                  /* tonewheels */
#define NB 9u                                   /* drawbars */
#define NK 61u                                  /* keys C2..C7 */
#define NOTE0 36u                               /* MIDI note of key 0 (C2) */

/* ---- the controls the audio ISR reads (single bytes, written by the main loop) */
static volatile struct {
    uint8_t db[NB];                             /* drawbars 0..8 */
    uint8_t vtype;                              /* setBfree's vibrato knob: 0 V1, 1 C1, 2 V2, 3 C2, 4 V3, 5 C3 */
    uint8_t vup;                                /* vibrato switched on (for the one manual) */
    uint8_t perc, perc3, fast, soft;            /* percussion: on, 3rd (else 2nd), fast decay, soft */
    uint8_t les;                                /* Leslie: 0 stop, 1 slow, 2 fast */
    uint8_t rev;                                /* reverb mix 0..127 (CC 91) */
    uint8_t od, drv;                            /* overdrive on, and the drive 0..20 (input gain; 12 sounds like setBfree) */
    uint8_t swell;                              /* 0..127 (CC 7 / CC 11) */
    uint8_t pgm;                                /* the program last selected (setBfree numbering), 0 none */
    uint8_t dbl[NB];                            /* the lower manual's drawbars (MIDI channel 2) */
    uint8_t dbp[2];                             /* the pedal's: 16' and 8' (MIDI channel 3) */
    uint8_t vlo;                                /* vibrato / chorus on the lower manual (the upper one is vup) */
    uint8_t keys;                               /* the FM-1's own keys play: 0 upper, 1 lower, 2 pedal */
    uint8_t chr;                                /* setBfree's overdrive.character 0..127 (NRPN 1/0, the web editor); 16 as in 0.4 */
} P = {{8, 8, 8, 0, 0, 0, 0, 0, 0}, 0, 0, 0, 1, 0, 0, 0, 12, 0, 12, 127, 0, {8, 3, 8, 0, 0, 0, 0, 0, 0}, {8, 6}, 0, 0, 16};          /* chr 16: OD_CHARACTER */
/* keys held (bit k): the upper manual (k = 0..60, C2..C7) from the panel and from MIDI channel 1, the lower one (the same keys) from the
 * panel and channel 2, the pedal (k = 0..31, C1..G3) from the panel and channel 3 */
static volatile uint32_t pk_lo, pk_hi, mk_lo, mk_hi, pl_lo, pl_hi, ml_lo, ml_hi, pp_k, mp_k;
static volatile uint32_t load_us, wheels_on;
static volatile uint32_t sum_pk, sum_clip, out_pk;      /* the sum of the wheels before the preamp: its peak since the screen last looked, and the samples above full scale since the start */
static volatile uint32_t load_org, load_od, load_rv, load_le;   /* peaks since the screen last looked: organ, preamp, reverb, Leslie (us) */
static volatile uint8_t fx_bypass;              /* tests: skip the reverb and the Leslie (the tone generator alone, in mono) */    /* the last block: render time, wheels sounding */

/* ---- the sound (runs in the audio ISR: keep it short, no flash, no screen) */
#define PERC_Q10 11264                          /* percussion level, setBfree's percEnvScaling 11 (1024 = a drawbar at 8); soft is in perc_env (0.5012) */
#ifndef ORGAN_LEVEL
#define ORGAN_LEVEL 32768                       /* the tone generator's level against setBfree's (Q15, 32768 = the same); a lower value makes room, the preamp is compensated (organ_od.h) */
#endif
#define PERC_NORMAL_Q15 19828                   /* setBfree's percDrawbarNormalGain 0.60512: with percussion on and not soft, the whole sound is this much quieter */
/* the envelope's decay per 256 frames, Q15: setBfree's plug-in multiplies by 0.9999 (slow) or 0.9995 (fast) every sample (its
 * computePercResets, the "4 s" and "1 s", is not called), at 48 kHz as on the Pi it was measured on: 0.9999 ^ (256 * 48000 / 44100) */
#define DEC_FAST 28506
#define DEC_SLOW 31868
#define VIB_INC 706089u                         /* 7.25 Hz: f * 2^32 / 44100 */

static uint32_t wph[NW];                        /* tonewheel phases: they turn all the time, as on the organ */
static int32_t wcur[NW], wtgt[NW];              /* level of each wheel on the direct bus (setBfree's swell bus), Q10 (1024 = one drawbar at 8) */
static int32_t wcurv[NW], wtgtv[NW];            /* and on the vibrato bus: what goes through the scanner */
static int32_t wpc[NW], wpt[NW];                /* the percussion's share of it before the high-pass (Q10), now and next: its attack is differentiated too */
static int32_t acc[256], accv[256];             /* direct and vibrato bus */
static int32_t kcl = 4096 << 4;                 /* key compression, now: Q16 */
static int32_t vdl[256];                        /* scanner delay line */
static uint32_t vwi, vph;
static int32_t mono[256], lbuf[256], rbuf[256]; /* after the overdrive: mono, then through the reverb, then the Leslie makes the pair */
#include "organ_fx.h"
#include "organ_od.h"                         /* setBfree's preamp */

/* ---- the Leslie's speeds and time constants (NRPN 2/0..7, the web editor): horn slow, horn fast, drum slow, drum fast in 0.1 rpm;
 * horn acceleration, horn deceleration, drum acceleration, drum deceleration in ms (the time constant). setBfree's defaults. */
#define LES_NP 8u
static const uint16_t LES_DEF[LES_NP] = {403, 4234, 360, 3573, 161, 321, 4127, 1371};
static const uint16_t LES_MIN[LES_NP] = {0, 600, 0, 600, 20, 20, 20, 20};
static const uint16_t LES_MAX[LES_NP] = {1500, 9000, 1500, 9000, 10000, 10000, 10000, 10000};
static uint16_t les_p[LES_NP] = {403, 4234, 360, 3573, 161, 321, 4127, 1371};
static void les_set(uint32_t i, uint32_t v)     /* main loop only: clamps, then the speed or the time constant the Leslie uses */
{
    if (i >= LES_NP) return;
    if (v < LES_MIN[i]) v = LES_MIN[i];
    if (v > LES_MAX[i]) v = LES_MAX[i];
    les_p[i] = (uint16_t)v;
    if (i < 4u) {                               /* 0.1 rpm -> phase step per frame: v / 600 Hz * 2^32 / 44100 = v * 162.319 */
        static const uint32_t DEF[4] = {LES_HORN_SLOW, LES_HORN_FAST, LES_DRUM_SLOW, LES_DRUM_FAST};
        ls_tgt[i] = v == LES_DEF[i] ? DEF[i] : (uint32_t)(((uint64_t)v * 10637669u) >> 16);
    } else {                                    /* ms -> 1 - exp(-x), x = 256 / (44100 * t): x - x^2/2 + x^3/6 in Q24, then Q16 */
        static const int32_t DEF[4] = {LES_HORN_ACC, LES_HORN_DEC, LES_DRUM_ACC, LES_DRUM_DEC};
        int32_t x = 97391549 / (int32_t)v, x2 = (int32_t)(((int64_t)x * x) >> 24), x3 = (int32_t)(((int64_t)x2 * x) >> 24);
        ls_k[i - 4u] = v == LES_DEF[i] ? DEF[i - 4u] : (x - x2 / 2 + x3 / 6 + 128) >> 8;
    }
}
static int32_t perc_env = 32767;                /* Q15 */

static const uint8_t VIBMODE[6] = {1, 4, 2, 5, 3, 6};   /* knob position -> mode: 1..3 V1..V3, 4..6 C1..C3 */

/* ---- the tone generator's tuning and key click (NRPN 3/0, 3/1, the web editor), 14 bits: 3/0 setBfree's osc.tuning in 0.1 Hz (4400:
 * A = 440 Hz; rounded to whole Hz, 4300..4500), 3/1 osc.attack.click.level in 0.1 % (500: 0.5, 0..1000). setBfree's values by default. */
#define TON_NP 2u
static const uint16_t TON_DEF[TON_NP] = {4400, 500};
static uint16_t ton_p[TON_NP] = {4400, 500};
static const uint32_t *winc = WHEEL_INC;        /* the wheels' phase steps: the table at 440 Hz, else wheel_inc_t */
static uint32_t wheel_inc_t[NW];
static int32_t atk_kd;                          /* (0.5 - click level) in Q12: the envelope moves by atk_kd * ATK_DIP */
static void ton_set(uint32_t i, uint32_t v)     /* main loop only */
{
    uint32_t w, m;
    if (i >= TON_NP) return;
    if (i == 0u) {
        v = (v + 5u) / 10u * 10u;                /* whole Hz, 430..450 */
        if (v < 4300u) v = 4300u;
        if (v > 4500u) v = 4500u;
        ton_p[0] = (uint16_t)v;
        if (v == 4400u) { winc = WHEEL_INC; return; }
        m = (v << 18) / 4400u;                  /* the ratio, Q18 */
        for (w = 0; w < NW; w++) wheel_inc_t[w] = (uint32_t)(((uint64_t)WHEEL_INC[w] * m) >> 18);
        winc = wheel_inc_t;                     /* the other core reads it: one word, the table is ready before */
    } else {
        if (v > 1000u) v = 1000u;
        ton_p[1] = (uint16_t)v;
        atk_kd = 2048 - (int32_t)((v * 4096u + 500u) / 1000u);
    }
}

/* The tonewheels w0, w0 + ws, ... of one block into the buses ac (direct) and acv (vibrato): the part of the sound that costs the time. Every wheel
 * is its own (phase, levels), so two calls with the even and the odd wheels, one on each core (organ_c1_job), add to exactly the sum of one call
 * with all of them. *act counts the wheels sounding, *vany says one of them reaches the scanner. */
static void wheels_render(uint32_t w0, uint32_t ws, uint32_t n, int32_t *acc, int32_t *accv, uint32_t *actp, uint32_t *vanyp)
{
    uint32_t w, i, act = 0, vany = 0;
    for (i = 0; i < n; i++) acc[i] = accv[i] = 0;
    for (w = w0; w < NW; w += ws) {
        int32_t c = wcur[w], t = wtgt[w], cv = wcurv[w], tv = wtgtv[w];
        uint32_t ph = wph[w], inc = winc[w];
        if (!c && !t && !cv && !tv) { wph[w] = ph + inc * n; wpc[w] = wpt[w]; continue; }   /* silent: costs nothing, the wheel turns on */
        {
            act++;
            if (cv | tv) vany = 1;
            if (c == t && cv == tv) {           /* steady: no envelope */
                if (!cv) {
                    for (i = 0; i < n; i++) {
                        uint32_t idx;
                        int32_t s0, s1, s;
                        ph += inc;
                        idx = ph >> 24;
                        s0 = SINE_TAB[idx];
                        s1 = SINE_TAB[idx + 1u];
                        s = s0 + (((s1 - s0) * (int32_t)((ph >> 8) & 0xFFFFu)) >> 16);
                        acc[i] += (s >> 2) * c;
                    }
                } else if (!c) {
                    for (i = 0; i < n; i++) {
                        uint32_t idx;
                        int32_t s0, s1, s;
                        ph += inc;
                        idx = ph >> 24;
                        s0 = SINE_TAB[idx];
                        s1 = SINE_TAB[idx + 1u];
                        s = s0 + (((s1 - s0) * (int32_t)((ph >> 8) & 0xFFFFu)) >> 16);
                        accv[i] += (s >> 2) * cv;
                    }
                } else {                        /* on both buses: one sine, two products */
                    for (i = 0; i < n; i++) {
                        uint32_t idx;
                        int32_t s0, s1, s;
                        ph += inc;
                        idx = ph >> 24;
                        s0 = SINE_TAB[idx];
                        s1 = SINE_TAB[idx + 1u];
                        s = s0 + (((s1 - s0) * (int32_t)((ph >> 8) & 0xFFFFu)) >> 16);
                        acc[i] += (s >> 2) * c;
                        accv[i] += (s >> 2) * cv;
                    }
                }
            } else {                            /* a level changes: setBfree's key click (an attack envelope; the release is a ramp) */
                const uint16_t *env = ATK_ENV[w & 7u], *dip = ATK_DIP[w & 7u];
                int32_t kd = atk_kd;
                int32_t d = t - c, dv = tv - cv, pd = wpt[w] - wpc[w], sp = 0, ep = 0;
                if (pd) {                       /* setBfree differentiates the percussion bus, envelope and all: d(e s) = e ds + s de, and de is the click */
                    uint32_t i0 = ph >> 24;
                    sp = SINE_TAB[i0] + (((SINE_TAB[i0 + 1u] - SINE_TAB[i0]) * (int32_t)((ph >> 8) & 0xFFFFu)) >> 16);
                }
                for (i = 0; i < n; i++) {
                    uint32_t idx;
                    int32_t s0, s1, s, e, a;
                    ph += inc;
                    idx = ph >> 24;
                    s0 = SINE_TAB[idx];
                    s1 = SINE_TAB[idx + 1u];
                    s = s0 + (((s1 - s0) * (int32_t)((ph >> 8) & 0xFFFFu)) >> 16);
                    e = i >= 128u ? 4096 : t ? (int32_t)env[i] + (kd ? (kd * (int32_t)dip[i] + 2048) >> 12 : 0) : (int32_t)i << 5;
                    a = c + ((d * e) >> 12);
                    acc[i] += (s >> 2) * a;
                    if (cv | tv) {
                        int32_t ev = i >= 128u ? 4096 : tv ? (int32_t)env[i] + (kd ? (kd * (int32_t)dip[i] + 2048) >> 12 : 0) : (int32_t)i << 5;
                        accv[i] += (s >> 2) * (cv + ((dv * ev) >> 12));
                    }
                    if (pd) {
                        acc[i] -= (sp >> 2) * ((pd * (e - ep)) >> 12);
                        sp = s;
                        ep = e;
                    }
                }
            }
        }
        wph[w] = ph;
        wcur[w] = t;
        wcurv[w] = tv;
        wpc[w] = wpt[w];
    }
    *actp = act;
    *vanyp = vany;
}

/* The second core. platform.c defines ORGAN_C1_RUN / ORGAN_C1_WAIT (fm1_cpu1.h); elsewhere (host tests, the browser) the odd wheels run in the same
 * thread, one after the other, with the same result: the sum of the two halves is the whole (organ_two = 0 runs all the wheels in one call). */
static int32_t acc2[256], accv2[256];           /* the odd wheels' buses */
static volatile uint32_t c1_act, c1_vany;
static uint8_t organ_two = 1;
#ifndef ORGAN_C1_RUN
#define ORGAN_C1_RUN(fn, arg) (organ_two ? ((fn)(arg), 1) : 0)
#define ORGAN_C1_WAIT() 0
#endif
static void organ_c1_job(uint32_t n) { wheels_render(1u, 2u, n, acc2, accv2, (uint32_t *)&c1_act, (uint32_t *)&c1_vany); }
static void organ_split(uint32_t n, uint32_t *act, uint32_t *vany)
{
    uint32_t a0, v0, i;
    if (!ORGAN_C1_RUN(organ_c1_job, n)) {
        wheels_render(0u, 1u, n, acc, accv, act, vany);
        return;
    }
    wheels_render(0u, 2u, n, acc, accv, &a0, &v0);
    if (ORGAN_C1_WAIT())                        /* core 1 did not answer in time: it is held now, the odd wheels (one block late) on this one from here on */
        organ_c1_job(n);
    for (i = 0; i < n; i++) {
        acc[i] += acc2[i];
        accv[i] += accv2[i];
    }
    *act = a0 + c1_act;
    *vany = v0 | c1_vany;
}

/* A limiter for the sum of the wheels with the overdrive off. A hard clip makes harsh, aliased overtones (six or seven notes with four drawbars
 * out and the chorus on reach full scale); a static soft clip only moves the same distortion lower down (measured: a sine 5 % over full scale
 * still carries -29 dB of it). So this one turns the gain down instead: the whole block is looked at first, the gain for it is the one that keeps
 * its peak at LIM_T, it falls to that within 32 samples and comes back 1/16 of the way every block (about 90 ms). A gain that moves slowly
 * only changes the loudness; whatever still gets over (a note that starts inside the first 32 samples) is clipped as before. */
#define LIM_T 29491                                /* 0.9 of full scale */
typedef struct { int32_t g; volatile int32_t lo; } lim_t;     /* g: Q15, the gain the last block ended with; lo: the lowest gain since the screen last looked */
/* ---- the output equalizer (the web editor's SysEx 07, or SELECT / ALGORITHM on the pedal screen), after reverb and Leslie, before the
 * limiter: a bass shelf 20..200 Hz, a peak 250..3100 Hz (Q 1), a treble shelf 4..8 kHz, each -9..+9 dB in 0.5 dB steps (gain index 0..36,
 * 18 = 0 dB). The biquads are Robert Bristow-Johnson's (as setBfree's eqcomp.c), computed here without floating point (Q29, sine and cosine
 * by their series): within a few LSB of Q28 of tools/eq_coef.py and the editor's curve. A band at 0 dB is not computed: all flat, the sound
 * is bit for bit without it. */
#define EQ_NP 6u
static const uint16_t EQ_DEF[EQ_NP] = {18, 80, 18, 800, 18, 6000};
static const uint16_t EQ_MIN[EQ_NP] = {0, 20, 0, 250, 0, 4000}, EQ_MAX[EQ_NP] = {36, 200, 36, 3100, 36, 8000};
static uint16_t eq_p[EQ_NP] = {18, 80, 18, 800, 18, 6000};
static int32_t eq_c[3][5];                      /* b0 b1 b2 a1 a2, Q28 (bq_run) */
static int32_t eq_new[3][5];
static volatile uint32_t eq_on, eq_pend;        /* the bands computed (bit k), new coefficients waiting for the next block */
static uint32_t eq_on_new;
static bq_t eq_s[2][3];
#define EQ_ONE (1LL << 29)
static const int32_t EQ_A[37] = {               /* 10^(dB/40), Q29, for the gain index (dB = (i - 18) / 2); 1/A is EQ_A[36 - i] */
    319793678, 329131822, 338742645, 348634109, 358814409, 369291978, 380075499, 391173904, 402596388, 414352415, 426451724, 438904339,
    451720577, 464911056, 478486704, 492458768, 506838824, 521638784, 536870912, 552547826, 568682513, 585288343, 602379071, 619968857,
    638072275, 656704322, 675880434, 695616499, 715928867, 736834368, 758350320, 780494549, 803285401, 826741758, 850883054, 875729287,
    901301044};
static const int32_t EQ_SQ[37] = {              /* sqrt(A), Q29 */
    414352415, 420358539, 426451724, 432633230, 438904339, 445266349, 451720577, 458268361, 464911056, 471650039, 478486704, 485422468,
    492458768, 499597060, 506838824, 514185558, 521638784, 529200047, 536870912, 544652968, 552547826, 560557121, 568682513, 576925685,
    585288343, 593772219, 602379071, 611110681, 619968857, 628955435, 638072275, 647321265, 656704322, 666223387, 675880434, 685677461,
    695616499};
static int64_t eq_mul(int64_t a, int64_t b) { return (a * b + (1LL << 28)) >> 29; }   /* Q29: |a|, |b| < 2^33 and one of them < 2^30 */
static uint64_t eq_udiv(uint64_t n, uint64_t d)   /* n / d by shifting and subtracting: the FM-1's libgcc has no 64-bit division */
{
    uint64_t q = 0, r = 0;
    int b;
    for (b = 63; b >= 0; b--) {
        r = r << 1 | (n >> b & 1u);
        if (r >= d) { r -= d; q |= 1ull << b; }
    }
    return q;
}
static int64_t eq_sdiv(int64_t n, int64_t d)    /* d > 0, toward zero like C */
{
    int64_t q = (int64_t)eq_udiv((uint64_t)(n < 0 ? -n : n), (uint64_t)d);
    return n < 0 ? -q : q;
}
static int32_t eq_div(int64_t n, int64_t d)     /* n / d in Q28 (n and d Q29, d > 0), rounded */
{
    int64_t q = (int64_t)eq_udiv((uint64_t)(n < 0 ? -n : n) << 28, (uint64_t)d);
    uint64_t r = ((uint64_t)(n < 0 ? -n : n) << 28) - (uint64_t)q * (uint64_t)d;
    if (2u * r >= (uint64_t)d) q++;
    return (int32_t)(n < 0 ? -q : q);
}
static void eq_calc(uint32_t kind, uint32_t g, uint32_t f, int32_t c[5])   /* kind 0 low shelf (Q 0.7071), 1 peak (Q 1), 2 high shelf */
{
    int64_t w = (int64_t)((uint64_t)f * 5012923577ull >> 16), x2 = eq_mul(w, w), sn = w, cs = EQ_ONE, t = w, u = EQ_ONE;   /* 2 pi f / 44100 */
    int64_t A = EQ_A[g], al, sa, ap, am, b0, b1, b2, a0, a1, a2;
    uint32_t n;
    for (n = 1; n < 12u; n++) {                 /* sine and cosine: their series, |w| < 1.2 */
        t = -eq_sdiv(eq_mul(t, x2), (int64_t)((2 * n) * (2 * n + 1))); sn += t;
        u = -eq_sdiv(eq_mul(u, x2), (int64_t)((2 * n - 1) * (2 * n))); cs += u;
    }
    if (kind == 1u) {
        al = sn >> 1;                            /* sin / (2 Q), Q 1 */
        b0 = EQ_ONE + eq_mul(al, A); b1 = -2 * cs; b2 = EQ_ONE - eq_mul(al, A);
        a0 = EQ_ONE + eq_mul(al, EQ_A[36u - g]); a1 = -2 * cs; a2 = EQ_ONE - eq_mul(al, EQ_A[36u - g]);
    } else {
        al = eq_mul(sn, 379628703);             /* sin / (2 x 0.7071) */
        sa = 2 * eq_mul(EQ_SQ[g], al); ap = A + EQ_ONE; am = A - EQ_ONE;
        if (kind == 0u) {
            b0 = eq_mul(A, ap - eq_mul(am, cs) + sa); b1 = 2 * eq_mul(A, am - eq_mul(ap, cs)); b2 = eq_mul(A, ap - eq_mul(am, cs) - sa);
            a0 = ap + eq_mul(am, cs) + sa; a1 = -2 * (am + eq_mul(ap, cs)); a2 = ap + eq_mul(am, cs) - sa;
        } else {
            b0 = eq_mul(A, ap + eq_mul(am, cs) + sa); b1 = -2 * eq_mul(A, am + eq_mul(ap, cs)); b2 = eq_mul(A, ap + eq_mul(am, cs) - sa);
            a0 = ap - eq_mul(am, cs) + sa; a1 = 2 * (am - eq_mul(ap, cs)); a2 = ap - eq_mul(am, cs) - sa;
        }
    }
    c[0] = eq_div(b0, a0); c[1] = eq_div(b1, a0); c[2] = eq_div(b2, a0); c[3] = eq_div(a1, a0); c[4] = eq_div(a2, a0);
}
static int eq_set(const uint16_t *p)           /* main loop: 0 when taken; the audio switches at its next block */
{
    uint32_t k, on = 0;
    for (k = 0; k < EQ_NP; k++) if (p[k] < EQ_MIN[k] || p[k] > EQ_MAX[k]) return -1;
    eq_pend = 0;                                /* (the audio interrupts this: it takes nothing until all is written) */
    for (k = 0; k < EQ_NP; k++) eq_p[k] = p[k];
    for (k = 0; k < 3u; k++) {
        if (p[2u * k] != 18u) on |= 1u << k;
        eq_calc(k, p[2u * k], p[2u * k + 1u], eq_new[k]);
    }
    eq_on_new = on;
    eq_pend = 1;
    return 0;
}
static void eq_run(int32_t *l, int32_t *r, uint32_t n)   /* Q15 in and out (bq_run: Q19) */
{
    uint32_t i, k, on;
    if (eq_pend) {
        on = eq_on_new;
        for (k = 0; k < 3u; k++) {
            if ((on >> k & 1u) && !(eq_on >> k & 1u)) eq_s[0][k] = eq_s[1][k] = (bq_t){0, 0, 0, 0};   /* a band comes on: from silence */
            for (i = 0; i < 5u; i++) eq_c[k][i] = eq_new[k][i];
        }
        eq_on = on;
        eq_pend = 0;
    }
    on = eq_on;
    if (!on) return;
    for (i = 0; i < n; i++) {
        int32_t a = l[i] << 4, b = r[i] << 4;
        for (k = 0; k < 3u; k++)
            if (on >> k & 1u) { a = bq_run(&eq_s[0][k], eq_c[k], a); b = bq_run(&eq_s[1][k], eq_c[k], b); }
        l[i] = a >> 4; r[i] = b >> 4;
    }
}

static lim_t lim_sum = {32768, 32768}, lim_out = {32768, 32768};
/* x and y (y may be 0): one block; returns its peak before the limiter */
static uint32_t block_limit(lim_t *s, int32_t *x, int32_t *y, uint32_t n)
{
    int32_t pk = 0, g1, g0 = s->g;
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t a = x[i] < 0 ? -x[i] : x[i];
        if (a > pk) pk = a;
        if (y) { a = y[i] < 0 ? -y[i] : y[i]; if (a > pk) pk = a; }
    }
    g1 = pk > LIM_T ? (int32_t)(((int64_t)LIM_T << 15) / pk) : 32768;
    if (g1 >= g0) g1 = g0 + ((g1 - g0 + 15) >> 4);   /* back up slowly (the gain is 1 again after a few hundred ms) */
    if (g1 == 32768 && g0 == 32768) return (uint32_t)pk;   /* the usual case: nothing to do */
    for (i = 0; i < n; i++) {
        int32_t g = i < 32u ? g0 + (int32_t)(((int64_t)(g1 - g0) * (int32_t)i) >> 5) : g1;
        int32_t v = (int32_t)(((int64_t)x[i] * g) >> 15);
        x[i] = v > 32767 ? 32767 : v < -32767 ? -32767 : v;
        if (y) { v = (int32_t)(((int64_t)y[i] * g) >> 15); y[i] = v > 32767 ? 32767 : v < -32767 ? -32767 : v; }
    }
    s->g = g1;
    if (g1 < s->lo) s->lo = g1;
    return (uint32_t)pk;
}

static void app_audio(int32_t *out, uint32_t n)
{
    /* setBfree scanner.modulation.v1..v3: the delay is 1 + A + A sin(phase) samples, A = 3, 6, 9, at 48 kHz (the Pi it was measured on);
     * here in samples of 44.1 kHz (x 44100 / 48000), Q8 */
    static const int32_t VIBA[3] = {706, 1411, 2117};
    static const int32_t VIBO = 235;            /* the "1 +": one sample of 48 kHz */
    uint32_t lo = pk_lo | mk_lo, hi = pk_hi | mk_hi, llo = pl_lo | ml_lo, lhi = pl_hi | ml_hi, pk = pp_k | mp_k;
    uint32_t k, d, w, e, i, nk = 0, nku = 0, act = 0, vany = 0, t0 = fm1_ticks(), t1, t2, t3, t4;
    int32_t pa = 0, kd;
    uint32_t dm = P.drv, od = P.od, les = P.les, vm, spk = 0, scl = 0, opk = 0;
    int32_t gsw = (int32_t)((1229u * P.swell) / 127u);
    int32_t *tu = P.vup ? wtgtv : wtgt, *tl = P.vlo ? wtgtv : wtgt;   /* the manuals reach the scanner or not (pedals and percussion never do) */
    if (P.perc && !P.soft) gsw = (gsw * PERC_NORMAL_Q15) >> 15;
    gsw = (gsw * ORGAN_LEVEL) >> 15;
    if (n > 256u) n = 256u;

    for (w = 0; w < NW; w++) wtgt[w] = wtgtv[w] = wpt[w] = 0;
    for (k = 0; k < NK; k++) {
        nku += k < 32u ? lo >> k & 1u : hi >> (k - 32u) & 1u;
        nk += (k < 32u ? lo >> k & 1u : hi >> (k - 32u) & 1u) + (k < 32u ? llo >> k & 1u : lhi >> (k - 32u) & 1u);
    }
    for (k = 0; k < 32u; k++) nk += pk >> k & 1u;                     /* setBfree's key compression counts every key down */
    if (!nku) perc_env = P.soft ? 16424 : 32767; /* single trigger: the envelope restarts when every upper key is up */
    else pa = (PERC_Q10 * perc_env) >> 15;     /* percussion level now (setBfree: 11 * 0.5012 when soft, decaying) */
    for (k = 0; k < NK; k++) {
        if (k < 32u ? !(lo >> k & 1u) : !(hi >> (k - 32u) & 1u)) continue;
        for (d = 0; d < NB; d++) {
            int32_t lvl = P.db[d];
            uint32_t e;
            if (!lvl || (d == 8u && P.perc)) continue;   /* percussion takes the 1' drawbar's place */
            for (e = CT_IDX[k * 9u + d]; e < CT_IDX[k * 9u + d + 1u]; e++)   /* the main wheel, tapered, and the leakage */
                tu[CT_W[e]] += (lvl * 128 * (int32_t)CT_G[e] + 2048) >> 12;
        }
        if (P.perc) {                           /* percussion: the 2nd or 3rd harmonic bus (as the 4' or 2 2/3' drawbar's: tapering, leakage), through setBfree's high-pass */
            uint32_t pd = P.perc3 ? 4u : 3u;
            for (e = CT_IDX[k * 9u + pd]; e < CT_IDX[k * 9u + pd + 1u]; e++)
            {
                int32_t pl = (pa * (int32_t)CT_G[e]) >> 12;
                wpt[CT_W[e]] += pl;
                wtgt[CT_W[e]] += (pl * (int32_t)PERC_HP[CT_W[e]]) >> 15;
            }
        }
    }
    for (k = 0; k < NK; k++) {
        if (k < 32u ? !(llo >> k & 1u) : !(lhi >> (k - 32u) & 1u)) continue;
        for (d = 0; d < NB; d++) {              /* the lower manual: the same wiring and tapering, its own drawbars, no percussion */
            int32_t lvl = P.dbl[d];
            uint32_t e;
            if (!lvl) continue;
            for (e = CT_IDX[k * 9u + d]; e < CT_IDX[k * 9u + d + 1u]; e++)
                tl[CT_W[e]] += (lvl * 128 * (int32_t)CT_G[e] + 2048) >> 12;
        }
    }
    for (k = 0; k < 32u; k++) {                 /* the pedal: 16' and 8', straight to the swell bus */
        if (!(pk >> k & 1u)) continue;
        for (d = 0; d < 2u; d++) {
            int32_t lvl = P.dbp[d];
            uint32_t e;
            if (!lvl) continue;
            for (e = CP_IDX[k * 2u + d]; e < CP_IDX[k * 2u + d + 1u]; e++)
                wtgt[CP_W[e]] += (lvl * 128 * (int32_t)CP_G[e] + 2048) >> 12;
        }
    }
    if (nku && P.perc) perc_env = (perc_env * (P.fast ? DEC_FAST : DEC_SLOW)) >> 15;

    organ_split(n, &act, &vany);
    vm = vany ? VIBMODE[P.vtype % 6u] : 0u;
    {
        int32_t kt = (int32_t)KEY_COMP[nk > 61u ? 61u : nk] << 4;       /* key compression glides over the block */
        kd = ((kt - kcl) * (int32_t)(65536u / n)) >> 16;
    }
    for (i = 0; i < n; i++) {
        int32_t x = acc[i] >> 10, xv = accv[i] >> 10, y;
        if (x > 262144) x = 262144;
        if (x < -262144) x = -262144;
        if (xv > 262144) xv = 262144;
        if (xv < -262144) xv = -262144;
        x = (x * gsw) >> 12;
        xv = (xv * gsw) >> 12;
        if (vm) {                               /* scanner vibrato / chorus (setBfree vibrato.c): a delay swinging as a sine, 7.25 times a second */
            uint32_t pos, i0, ix;
            int32_t v0, v1, v, s0, s1, sn, a = VIBA[(vm - 1u) % 3u], dl;
            vph += VIB_INC;
            ix = vph >> 24;
            s0 = SINE_TAB[ix]; s1 = SINE_TAB[ix + 1u];
            sn = s0 + (((s1 - s0) * (int32_t)((vph >> 8) & 0xFFFFu)) >> 16);   /* sin, Q15 */
            dl = VIBO + a + ((a * sn) >> 15);                                   /* the delay in samples, Q8 */
            pos = (vwi << 8) - (uint32_t)dl;
            i0 = (pos >> 8) & 255u;
            v0 = vdl[i0];
            v1 = vdl[(i0 + 1u) & 255u];
            v = v0 + (((v1 - v0) * (int32_t)(pos & 255u)) >> 8);
            vdl[vwi] = xv;
            vwi = (vwi + 1u) & 255u;
            xv = vm >= 4u ? ((xv + v) * 181) >> 8 : v;   /* the chorus mixes with 1/sqrt(2) as setBfree does (not 1/2) */
            x += xv;                            /* setBfree: the swell bus plus the scanner's output (percussion rides on the swell bus) */
        }
        kcl += kd;
        x = (x * (kcl >> 4)) >> 12;
        { int32_t ax = x < 0 ? -x : x; if ((uint32_t)ax > spk) spk = (uint32_t)ax; if (ax > 32767) scl++; }
        y = x > 60000 ? 60000 : x < -60000 ? -60000 : x;   /* the preamp below takes it unclipped */
        mono[i] = y;
    }
    if (spk > sum_pk) sum_pk = spk;
    sum_clip += scl;
    t1 = fm1_ticks();
    if (od) od_run(mono, n, P.chr, dm);          /* setBfree's preamp: the character (NRPN 1/0) and the drive (the input gain) */
    else block_limit(&lim_sum, mono, 0, n);          /* overdrive off: the sum is limited by turning the gain down, not clipped */
    t2 = fm1_ticks();
    if (fx_bypass) {
        for (i = 0; i < n; i++) lbuf[i] = rbuf[i] = mono[i];
        t3 = t2;
    } else {
        reverb_run(mono, n, P.rev);             /* setBfree's order: overdrive, reverb, Leslie */
        t3 = fm1_ticks();
        leslie_run(mono, lbuf, rbuf, n, les);
    }
    eq_run(lbuf, rbuf, n);                      /* the output equalizer (flat: nothing) */
    t4 = fm1_ticks();
    opk = block_limit(&lim_out, lbuf, rbuf, n);      /* the output, after reverb and Leslie (they add gain too): the same limiter, over both channels */
    for (i = 0; i < n; i++) {
        int32_t l = lbuf[i], r = rbuf[i];
        out[2u * i] += l > 32767 ? 32767 : l < -32767 ? -32767 : l;
        out[2u * i + 1u] += r > 32767 ? 32767 : r < -32767 ? -32767 : r;
    }
    if (opk > out_pk) out_pk = opk;
    wheels_on = act;
    {
        uint32_t us = (uint32_t)(fm1_ticks() - t0) / FM1_TICKS_PER_US, a = (uint32_t)(t1 - t0) / FM1_TICKS_PER_US, b = (uint32_t)(t2 - t1) / FM1_TICKS_PER_US,
                 c = (uint32_t)(t3 - t2) / FM1_TICKS_PER_US, f = (uint32_t)(t4 - t3) / FM1_TICKS_PER_US;
        if (us > load_us) load_us = us;         /* the peak since the screen last looked */
        if (a > load_org) load_org = a;
        if (b > load_od) load_od = b;
        if (c > load_rv) load_rv = c;
        if (f > load_le) load_le = f;
    }
}

/* ---- keys (main loop) */
static void key_set(volatile uint32_t *lo, volatile uint32_t *hi, uint32_t k, int on)
{
    if (k >= NK) return;
    if (k < 32u) *lo = on ? *lo | 1u << k : *lo & ~(1u << k);
    else *hi = on ? *hi | 1u << (k - 32u) : *hi & ~(1u << (k - 32u));
}
/* a note on one of the three manuals (0 upper, 1 lower, 2 pedal): from the panel, or from MIDI (fold = bring a note outside the keyboard's
 * range back into it by octaves; the pedal sounds C1..G3, MIDI notes 24 to 55, as in setBfree) */
static void organ_note(uint32_t manual, uint32_t panel, uint32_t note, int on, int fold)
{
    uint32_t n = note;
    if (manual == 2u) {
        volatile uint32_t *m = panel ? &pp_k : &mp_k;
        if (fold) { while (n < 24u) n += 12u; while (n > 55u) n -= 12u; }
        if (n < 24u || n > 55u) return;
        n -= 24u;
        *m = on ? *m | 1u << n : *m & ~(1u << n);
        return;
    }
    if (fold) { while (n < NOTE0) n += 12u; while (n >= NOTE0 + NK) n -= 12u; }
    if (n < NOTE0 || n >= NOTE0 + NK) return;
    n -= NOTE0;
    if (manual == 1u) key_set(panel ? &pl_lo : &ml_lo, panel ? &pl_hi : &ml_hi, n, on);
    else key_set(panel ? &pk_lo : &mk_lo, panel ? &pk_hi : &mk_hi, n, on);
}
static void organ_midi_note(uint32_t ch, uint32_t note, int on)   /* channel 1 upper, 2 lower, 3 pedal; the others play the upper manual, as before */
{
    organ_note(ch == 1u ? 1u : ch == 2u ? 2u : 0u, 0u, note, on, 1);
}

static int quiet(void)                          /* nothing sounds, not even the glide out */
{
    uint32_t w;
    if (pk_lo | pk_hi | mk_lo | mk_hi | pl_lo | pl_hi | ml_lo | ml_hi | pp_k | mp_k) return 0;
    for (w = 0; w < NW; w++)
        if (wcur[w] | wcurv[w]) return 0;
    return 1;
}

/* ---- setBfree's programs (default.pgm) and MIDI controllers (midi.c, midiPrimeControllerMapping) */
static uint32_t rnd_state = 2463534242u;
static uint32_t rnd(void)                       /* xorshift, stirred with the clock */
{
    rnd_state ^= fm1_ticks();
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

/* ---- the user's own programs 112 to 127 (stored by the web editor, SysEx): the whole sound of all three manuals */
#define UP_FIRST 112u
#define UP_N 16u
typedef struct {
    uint8_t used;                               /* 1: the slot holds a program */
    char name[13];                              /* printable ASCII, 0-terminated */
    uint8_t db[NB], dbl[NB], dbp[2], vtype, vup, vlo, perc, perc3, fast, soft, les, od, drv, chr, rev;
    uint8_t rsv[2];                             /* (48 bytes a slot) rsv[0]: UP_DRV_NEW when the drive is in 0.6's scale */
} organ_user_t;
#define UP_DRV_NEW 1u
static organ_user_t UPGM[UP_N];                 /* loaded from the flash at start (dinghy.c), zero: empty */
static organ_user_t *user_pgm(uint32_t n) { return n >= UP_FIRST && n < UP_FIRST + UP_N && UPGM[n - UP_FIRST].used == 1u ? &UPGM[n - UP_FIRST] : 0; }
static const char *pgm_name(uint32_t n)         /* 0: no such program */
{
    if (n >= UP_FIRST && n < UP_FIRST + UP_N) return user_pgm(n) ? user_pgm(n)->name : 0;
    return n < 128u ? PGM[n].name : 0;
}
static void user_store(uint32_t n, const uint8_t *name)   /* the sound now into program n (112..127), name: 12 bytes, 0 ends early */
{
    organ_user_t *u = &UPGM[n - UP_FIRST];
    uint32_t i;
    for (i = 0; i < NB; i++) { u->db[i] = P.db[i]; u->dbl[i] = P.dbl[i]; }
    u->dbp[0] = P.dbp[0]; u->dbp[1] = P.dbp[1];
    u->vtype = P.vtype; u->vup = P.vup; u->vlo = P.vlo; u->perc = P.perc; u->perc3 = P.perc3; u->fast = P.fast; u->soft = P.soft;
    u->les = P.les; u->od = P.od; u->drv = P.drv; u->chr = P.chr; u->rev = P.rev; u->rsv[0] = UP_DRV_NEW; u->rsv[1] = 0;
    for (i = 0; i < 12u && name[i]; i++) u->name[i] = name[i] >= 32u && name[i] < 127u ? (char)name[i] : '?';
    if (i == 0) { u->name[0] = 'U'; u->name[1] = 'S'; u->name[2] = 'E'; u->name[3] = 'R'; i = 4; }
    for (; i < 13u; i++) u->name[i] = 0;
    u->used = 1;
    P.pgm = (uint8_t)n;
}
static int user_ok(const organ_user_t *u)       /* 1: a used slot with everything in range */
{
    uint32_t i, ok = u->used == 1u && u->name[12] == 0 && u->vtype < 6u && u->les < 3u && u->drv <= OD_DRIVE_MAX && u->chr < 128u && u->rev < 128u;
    for (i = 0; ok && i < NB; i++) ok = u->db[i] < 9u && u->dbl[i] < 9u;
    return ok && u->dbp[0] < 9u && u->dbp[1] < 9u;
}
static void user_check(void)                    /* after loading: anything out of range makes the slot empty */
{
    uint32_t k;
    for (k = 0; k < UP_N; k++) {
        organ_user_t *u = &UPGM[k];
        if (u->used == 1u && u->rsv[0] != UP_DRV_NEW) {   /* stored before 0.6: the drive was 0..16, the same sound is 4 steps higher now */
            u->drv = (uint8_t)(u->drv + 4u > OD_DRIVE_MAX ? OD_DRIVE_MAX : u->drv + 4u);
            u->rsv[0] = UP_DRV_NEW;
        }
        if (!user_ok(u)) u->used = 0;
    }
}

static int pgm_registration(uint32_t n)         /* a program that sets the drawbars: what the PRESETS knob steps through */
{
    if (n >= UP_FIRST) return user_pgm(n) != 0;
    return n < 128u && PGM[n].name && (PGM[n].set & (PGM_DB | PGM_RND));
}
static uint32_t pgm_step(uint32_t cur, int32_t dir)
{
    int32_t n = (int32_t)cur;
    for (;;) {
        n += dir;
        if (n < 1 || n > 127) return cur;
        if (pgm_registration((uint32_t)n)) return (uint32_t)n;
    }
}

static void organ_program(uint32_t n)           /* only the properties a program names change, as in setBfree */
{
    const organ_pgm_t *g;
    const organ_user_t *u = user_pgm(n);
    uint32_t i;
    if (u) {                                    /* the user's own: everything it holds */
        for (i = 0; i < NB; i++) { P.db[i] = u->db[i]; P.dbl[i] = u->dbl[i]; }
        P.dbp[0] = u->dbp[0]; P.dbp[1] = u->dbp[1];
        P.vtype = u->vtype; P.vup = u->vup & 1u; P.vlo = u->vlo & 1u; P.perc = u->perc & 1u; P.perc3 = u->perc3 & 1u;
        P.fast = u->fast & 1u; P.soft = u->soft & 1u; P.les = u->les; P.od = u->od & 1u; P.drv = u->drv; P.chr = u->chr; P.rev = u->rev;
        P.pgm = (uint8_t)n;
        return;
    }
    if (n < 1u || n >= UP_FIRST || !PGM[n].name) return;
    g = &PGM[n];
    if (g->set & PGM_RND) for (i = 0; i < NB; i++) P.db[i] = (uint8_t)(rnd() % 9u);
    if (g->set & PGM_DB) for (i = 0; i < NB; i++) P.db[i] = g->db[i];
    if (g->set & PGM_PERC) P.perc = g->perc;
    if (g->set & PGM_PVOL) P.soft = g->pvol;
    if (g->set & PGM_PSPD) P.fast = g->pspd;
    if (g->set & PGM_PHARM) P.perc3 = g->pharm;
    if (g->set & PGM_VIB) P.vtype = g->vib;
    if (g->set & PGM_VUP) P.vup = g->vup;
    if (g->set & PGM_OD) P.od = g->od;
    if (g->set & PGM_ROT) P.les = g->rot;
    P.pgm = (uint8_t)n;
}
static void organ_pc(uint32_t uc)               /* MIDI program change: setBfree's pgm.controller.offset is 1 */
{
    organ_program(uc + 1u);
}

/* MIDI controllers, as setBfree's defaults. The drawbars (70 to 78) and the notes follow the channel: 1 upper, 2 lower, 3 pedal (16' and 8'
 * only); everything else is accepted on any channel. Not used here, no such part: 94 convolution. */
static uint32_t nrpn = 0x3FFFu, nrpn_msb;       /* the NRPN chosen by CC 99 / 98 (MSB << 7 | LSB); 0x3FFF none; the data entry's MSB */
static uint32_t sus_dn;                          /* CC 64 is down (64 and up) */
static void organ_cc_ch(uint32_t ch, uint32_t cc, uint32_t v)   /* ch 0..15: the drawbars follow the channel, 0 upper, 1 lower, 2 pedal (16' = 70, 8' = 72) */
{
    if (v > 127u) return;
    if (cc >= 70u && cc <= 78u) {               /* drawbars: 127 is pushed in, 0 pulled out (setBfree: 127 - v) */
        uint8_t db = (uint8_t)((16u * (127u - v) + 127u) / 254u);
        if (ch == 1u) P.dbl[cc - 70u] = db;
        else if (ch == 2u) { if (cc == 70u) P.dbp[0] = db; else if (cc == 72u) P.dbp[1] = db; }
        else P.db[cc - 70u] = db;
    } else switch (cc) {
    case 7: case 11: P.swell = (uint8_t)v; break;                       /* swell pedal */
    case 91: P.rev = (uint8_t)v; break;                                 /* reverb mix (setBfree: reverb.mix) */
    case 1: P.les = v / 43u == 0u ? 1u : v / 43u == 1u ? 0u : 2u; break;    /* rotary preset: slow, stop, fast */
    case 64:                                                            /* rotary speed toggle (sustain pedal): once as it goes down past 64; */
        if (v > 63u && !sus_dn)                                         /* a pedal with values between (32, 63, 95, 127, ...) toggled at each */
            P.les = P.les == 1u ? 2u : 1u;
        sus_dn = v > 63u;
        break;
    case 65: P.od = v > 63u; break;                                     /* overdrive on */
    case 93: P.drv = (uint8_t)((20u * v + 63u) / 127u); break;          /* overdrive drive 0..20 */
    case 66: case 80: P.perc = v > 63u; break;                          /* percussion on */
    case 81: P.soft = v > 63u; break;                                   /* percussion volume: soft at 64 and up */
    case 82: P.fast = v > 63u; break;                                   /* percussion decay: fast at 64 and up */
    case 83: P.perc3 = v < 64u; break;                                  /* percussion harmonic: 2nd at 64 and up */
    case 92: P.vtype = (uint8_t)(v / 23u > 5u ? 5u : v / 23u); break;   /* vibrato knob: V1 C1 V2 C2 V3 C3 */
    case 95: P.vup = v / 32u >= 2u; P.vlo = (v / 32u & 1u) != 0u; break;   /* vibrato routing: off, lower, upper, both */
    case 31: P.vup = v > 63u; break;                                    /* vibrato upper */
    case 30: P.vlo = v > 63u; break;                                    /* vibrato lower */
    case 99: nrpn = (nrpn & 0x7Fu) | v << 7; break;                     /* NRPN number, MSB */
    case 98: nrpn = (nrpn & 0x3F80u) | v; break;                        /* and LSB */
    case 101: case 100: nrpn = 0x3FFFu; break;                          /* an RPN: not ours, data entry goes nowhere */
    case 6:                                                             /* data entry (MSB) for the NRPN chosen */
        if (nrpn == (1u << 7 | 0u)) P.chr = (uint8_t)v;                 /* 1/0: setBfree's overdrive.character 0..127 */
        nrpn_msb = v;
        break;
    case 38:                                                            /* data entry LSB: the 14-bit ones, MSB << 7 | LSB */
        if (nrpn >= (2u << 7) && nrpn < (2u << 7) + LES_NP) les_set(nrpn - (2u << 7), nrpn_msb << 7 | v);   /* 2/0..7: the Leslie */
        if (nrpn >= (3u << 7) && nrpn < (3u << 7) + TON_NP) ton_set(nrpn - (3u << 7), nrpn_msb << 7 | v);   /* 3/0..1: tuning, key click */
        break;
    case 120: case 123: mk_lo = mk_hi = ml_lo = ml_hi = mp_k = 0; break;   /* all sound / notes off (MIDI's notes; the panel's stay) */
    default: break;
    }
}
static void organ_cc(uint32_t cc, uint32_t v) { organ_cc_ch(0u, cc, v); }
