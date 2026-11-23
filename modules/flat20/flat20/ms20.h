/* SPDX-License-Identifier: GPL-3.0-only */
/* Korg MS-20 synth engine (1978 Rev 1 board set: KLM-127 with dual KORG35 filters, KLM-128, KLM-129).
 * Modeled from Korg's service manual schematics; see docs/MS20-CIRCUIT.md for part numbers and sources.
 *
 * Key architectural notes:
 *   - Pitch is Hz/V: 1 V at lowest C, 8 V at highest. VCO current is linear with CV via the SCALE resistors.
 *     Only modulation (FM knobs, MASTER TUNE, VCO 2 PITCH) is exponential (18 mV/oct).
 *   - Triggers are switch triggers (S-trig): +5 V idle, 0 V active (threshold ~2.2 V).
 *   - Dual KORG35 filters: Sallen-Key pair (3300 pF / 1000 pF) with transistor-resistors into a 58x amp
 *     with antiparallel feedback diodes. Diodes sit in the forward signal path, clipping both signal and
 *     resonance. High-pass injects at the 1000 pF ground end (6 dB/oct). Input signal voltage modulates
 *     transistor conductance, producing asymmetric resonance.
 *   - VCA is a single series transistor into an I-to-V amp with an 8.8 kHz roll-off. Closed below ~0.4 V EG2;
 *     INITIAL GAIN is diode-ORed with EG2 (max, not sum).
 *   - EG1 normalled to VCOs is -5..0 V (OUT jack is 0..+5 V) and opens the optocoupler modulation VCA.
 *     "Sample & Hold" is a track-and-hold (tracks while clock is high, holds while low).
 *
 * Runs internally at 88.2 kHz (2x oversampled) and decimates to 44.1 kHz via half-band FIR.
 * Plain C, single-precision float, no libm, no allocation. Shared between the FM-1 audio ISR and host tests
 * (tests/ms20_test.c). Float is used only here, never in the main loop. No Korg code. */
#ifndef MS20_H
#define MS20_H
#include <stdint.h>

#define MS_OUT_FS 44100.0f                          /* what ms20_render gives */
#define MS_FS 88200.0f                              /* what the voice runs at */
#define MS_NCABLE 8u
#define MS_VFS 2.5f                                 /* the volts at SIGNAL OUT that are +-1 in ms20_render's output */

/* ---- the patch: every value 0..127 (a knob's 0 .. 10; a four-way switch's position in the upper two bits) */
enum {
    P_V1WAVE, P_V1PW, P_V1SCALE, P_PORTA,           /* VCO 1: TRI SAW PULSE NOISE; pulse width; 32' 16' 8' 4'; portamento */
    P_V2WAVE, P_V2PITCH, P_V2SCALE, P_TUNE,         /* VCO 2: SAW SQR PULSE RING; pitch; 16' 8' 4' 2'; master tune */
    P_MIX1, P_MIX2, P_FM_MG, P_FM_EG,               /* the VCO mixer; frequency modulation by MG / T.EXT and EG1 / EXT */
    P_HP_CUT, P_HP_PEAK, P_HP_MG, P_HP_EG,          /* high-pass: cutoff, peak, cutoff modulation by MG / T.EXT and EG2 / EXT */
    P_LP_CUT, P_LP_PEAK, P_LP_MG, P_LP_EG,          /* low-pass: the same */
    P_MG_WAVE, P_MG_RATE, P_E1_DELAY, P_E1_ATK,     /* modulation generator; EG1 */
    P_E1_REL, P_E2_HOLD, P_E2_ATK, P_E2_DEC,
    P_E2_SUS, P_E2_REL, P_VOLUME, P_ESP_CV,         /* P_ESP_CV: the external signal processor's CV ADJUST */
    P_ESP_LEVEL, P_ESP_LOW, P_ESP_HIGH, P_ESP_THR,  /* its SIGNAL LEVEL, LOW CUT, HIGH CUT, THRESHOLD */
    MS_NPARAM
};

/* Patch panel outputs (in MS-20 volts). VCO 1 and VCO 2 don't exist on real hardware and are placed last. */
enum {
    SRC_NONE, SRC_KBD_CV, SRC_KBD_TRIG, SRC_MG_TRI, SRC_MG_RECT, SRC_EG1, SRC_EG1_REV, SRC_EG2_REV, SRC_SH, SRC_MVCA,
    SRC_WHITE, SRC_PINK, SRC_WHEEL, SRC_BUTTON, SRC_SIGNAL, SRC_PHONES, SRC_ESP_AMP, SRC_ESP_BPF, SRC_ESP_FV,
    SRC_ESP_ENV, SRC_ESP_TRIG, SRC_VCO1, SRC_VCO2, MS_NSRC
};
/* Patch panel inputs. Plugging into an input breaks its internal normal regardless of the other end. */
enum {
    DST_NONE, DST_TOTAL, DST_FREQ, DST_VCO_CV, DST_VCO2_CV, DST_EXT_IN, DST_HP_CUT, DST_LP_CUT, DST_GAIN, DST_TRIG,
    DST_EG1_TRIG, DST_SH_IN, DST_SH_CLOCK, DST_MVCA_IN, DST_MVCA_CTL, DST_ESP_IN, MS_NDST
};

/* Unified jack numbering (cables can connect any two jacks): 0 = unconnected, 1..NSRC-1 = outputs, then inputs. */
#define MS_JACK_OUT(s) ((uint8_t)(s))
#define MS_JACK_IN(d) ((uint8_t)(MS_NSRC - 1 + (d)))
#define MS_NJACK (MS_NSRC + MS_NDST - 1)

typedef struct {
    uint8_t p[MS_NPARAM];
    uint8_t cable[MS_NCABLE][2];                    /* jack pairs (MS_JACK_*); {0,0} = empty, one 0 = dummy plug */
    char name[12];
} ms20_patch_t;                                     /* 64 bytes */

typedef struct {                                    /* what the player does (the main loop writes, the engine reads) */
    volatile int32_t note;                          /* MIDI note * 256 (with the bend); 48 = the keyboard's lowest C, 1 V */
    volatile uint32_t gate;                         /* a key is down */
    volatile int32_t wheel;                         /* -127..127: -5 V .. +5 V */
    volatile uint32_t button;                       /* the momentary switch (closes to ground) */
} ms20_ctl_t;

typedef struct { float s1, s2, x, va, vb; } ms20_k35_t;

#define MS_SCOPE_N 1024u
enum { MS_SCOPE_MIX = MS_NSRC, MS_SCOPE_HP, MS_SCOPE_LP };
typedef struct {
    uint32_t ph1, ph2, rng;
    float cv, sv[MS_NSRC];                          /* the keyboard's voltage after PORTAMENTO; the outputs' volts, last sample */
    float jv[MS_NDST];                              /* each plugged input's volts: what its cable's jacks settle at, last sample */
    float e1, e2, e1_t, e2_t;                       /* EG1 0..1; EG2 in volts; EG1's delay and EG2's hold (samples left) */
    uint8_t e1_st, e2_st, gate1, gate2, key2, mg_up, esp_gate, esp_pos, ok, lamp;
    float mg, sh, pk, wheel, mvca;
    ms20_k35_t hp, lp;
    float hp_dc, lp_dc, vca_i, vca_c, vca_u, out_lp, out_dc;
    float fm_x, fm_y, hp_t, lp_t;                   /* kept between samples: the FM's exponential, the filters' tan(pi f / fs) */
    float sm[MS_NPARAM];                            /* the knobs, smoothed (0..1) */
    float esp_c, esp_h1, esp_h2, esp_l[4], esp_env, esp_f1, esp_f2, esp_t, esp_pk;   /* esp_pk: the PEAK lamp's samples left */
    float dec[64];                                  /* the decimator's history (two copies of 32) */
    uint32_t dec_i;
    /* the screen's scope: two signals, every scope_div-th sample at 88.2 kHz, in millivolts. scope_src[] (the main
     * loop sets them): 0 = off, a jack's output (SRC_*), or MS_SCOPE_MIX / _HP / _LP, the filters' inputs and outputs.
     * scope_trig (1 = VCO 1, 2 = VCO 2): where in the record that oscillator last began a cycle (scope_wrap[], the
     * newest at scope_wn - 1) and its phase step (scope_inc: a cycle is 2^32 / scope_inc samples), so the screen can
     * hold the picture still, as a scope's external trigger would */
    int16_t scope[2][MS_SCOPE_N];
    volatile uint32_t scope_w, scope_wn;
    uint32_t scope_wrap[8], scope_inc;
    uint16_t scope_div, scope_n;
    uint8_t scope_src[2], scope_trig;
} ms20_t;

/* ---- arithmetic without libm */
static inline float ms_clamp(float x, float a, float b) { return x < a ? a : x > b ? b : x; }

static inline float ms_exp2(float x)                /* 2^x, relative error below 1e-5 */
{
    union { float f; uint32_t u; } s;
    int32_t i;
    float f;
    x = ms_clamp(x, -60.f, 60.f);
    i = (int32_t)x;
    if ((float)i > x)
        i--;
    f = x - (float)i;
    s.u = (uint32_t)(i + 127) << 23;
    return s.f * (1.f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f
                 + f * (0.00133336f + f * (0.00015404f + f * 0.00001525f)))))));
}

static inline float ms_tan(float x)                 /* 0 <= x < 1.4: Pade (5, 4), error below 1e-4 */
{
    float x2 = x * x;
    return x * (945.f - 105.f * x2 + x2 * x2) / (945.f - 420.f * x2 + 15.f * x2 * x2);
}

/* ---- Potentiometer tapers
 * "B" pots are linear. "A" pots (times, modulation intensities, VCO levels, VOLUME) use a standard
 * 15% midpoint audio taper (giving 0.17 s EG2 decay at 1.6, matching MS-20 mini measurements). */
static inline float ms_taper(float v) { return (ms_exp2(v * 5.0048f) - 1.f) * (1.f / 31.11f); }
/* an RC's step a sample: the pot's `r_max` ohms at 10, `r_min` in series, a capacitor of `c` farads */
static inline float ms_rc(float v, float r_max, float r_min, float c) { return 1.f / (1.f + (ms_taper(v) * r_max + r_min) * c * MS_FS); }
/* an intensity pot (100 k, A) whose wiper feeds a summing resistor of 100 k * `load`: what reaches the sum, of what a
 * wire in the pot's place would give. The load bends the taper further. */
static inline float ms_amount(float v, float load) { float a = ms_taper(v); return a / (1.f + a * (1.f - a) / load); }
/* a level pot (100 k, A) fed from a source of `rs` (in units of 100 k), its wiper into the mixer's 100 k */
static inline float ms_level(float v, float rs) { float a = ms_taper(v), p = a / (a + 1.f); return p / (rs + 1.f - a + p); }
/* PEAK (VR19 / VR22, 10 k B, with 2.2 k from its top to its wiper) into R106 5.6 k + R107 220: the part of the
 * filter's output that reaches the bottom of the 3300 p capacitor. 1 / 26.5 at 10. */
static inline float ms20_peak(float v)
{
    float top = (1.f - v) * 10e3f, bot = v * 10e3f;
    top = top * 2.2e3f / (top + 2.2e3f);
    bot = bot * 5.82e3f / (bot + 5.82e3f);
    return bot / (top + bot + 1e-3f) * (220.f / 5820.f);
}

/* ---- the KORG35 filter.
 *
 *            R1            R2
 *   in o---/\/\/---+A----/\/\/---+B------[ buffer ]---[ amplifier: 1 + 270 k / 4.7 k, two diodes across the 270 k ]--+-- y
 *                  |             |                                                                                   |
 *                 === C1 3300 p === C2 1000 p                                                                        |
 *                  |             |                                                                                   |
 *                  F             o ground (low-pass)  /  the input (high-pass, `in` then grounded)                   |
 *                  +------------------------------------------- beta * y, by PEAK -----------------------------------+
 *
 * R1 and R2 are the chip's two transistors in reverse saturation: their resistance is set by the cutoff current, and
 * by the few millivolts of signal across them (g ~ exp(-v / VT)), which is why the KORG35 rings more on one half of
 * the wave than the other (Stinchcombe 2006, section 4).
 * Solved before the sample (trapezoidal, zero delay) for the diodes' voltage x = vd / nVT:
 *   vB = Rg (vd / Rf + 2 Is sinh x),   y = vB + vd,   and the network gives  (a - beta) vB - beta vd = c.
 * P1 = T / (2 R1 C1), P2 = T / (2 R2 C1); the self-oscillation is at tan(pi f / fs) = sqrt(P1 * 3.3 P2). */
#ifndef MS20_K35_R
#define MS20_K35_R 1.5f                             /* R2 / R1. Not on the diagram (the chip's inside): docs/MS20-CIRCUIT.md */
#define MS20_K35_P 0.6742f                          /* sqrt(MS20_K35_R / 3.3): P1 = tan(pi f / fs) * this */
#endif
#ifndef MS20_K35_ASYM
#define MS20_K35_ASYM 1.0f                          /* how much of v / VT moves the transistors' conductance */
#endif
#ifndef MS20_K35_ITER
#define MS20_K35_ITER 2
#endif
#ifndef MS20_CUT_SPAN
#define MS20_CUT_SPAN 5.266f                        /* CUTOFF's octaves either side of 500 Hz (see the cutoffs, below) */
#endif
#define MS20_CUT_OCTV (MS20_CUT_SPAN * 0.31333f)    /* a volt through 100 k against CUTOFF's 15 V through 470 k */
#define K35_NVT 0.04529f                            /* 1S1555 as a 1N4148: n 1.752, at 300 K */
#define K35_A 7.8837e-4f                            /* Rg / Rf * nVT:  4.7 k / 270 k */
#define K35_B 1.1844e-5f                            /* Is * Rg:        2.52 nA * 4.7 k */
#define K35_GAIN 58.447f                            /* 1 + 270 k / 4.7 k */

static inline float ms20_k35(ms20_k35_t *f, float in, float P1, float P2, float beta, int hp)
{
    float z1 = (hp ? f->va : in + f->va) * (-0.5f * MS20_K35_ASYM / 0.02585f);
    float z2 = (f->va + f->vb) * (-0.5f * MS20_K35_ASYM / 0.02585f);
    float p1, p2, q2, iq, k, a, c, al, ga, x = f->x, vb, y, va, w;
    int i;
    z1 = ms_clamp(z1, -1.f, 1.f);
    z2 = ms_clamp(z2, -1.f, 1.f);
    p1 = P1 * (1.f + z1 + 0.5f * z1 * z1);
    p2 = P2 * (1.f + z2 + 0.5f * z2 * z2);
    q2 = 3.3f * p2;                                 /* T / (2 R2 C2) */
    iq = 1.f / q2;
    k = 1.f + p1 + p2;
    a = k * iq + 1.f + p1;
    c = hp ? f->s1 + (f->s2 + in) * k * iq : f->s1 + p1 * in + f->s2 * k * iq;
    al = a - beta;
    ga = beta * K35_NVT;
    for (i = 0; i < MS20_K35_ITER; i++) {           /* Newton, from the last sample's answer */
        float e = ms_exp2(x * 1.442695f), ie = 1.f / e, d;
        float F = al * (K35_A * x + K35_B * (e - ie)) - ga * x - c;
        float D = al * (K35_A + K35_B * (e + ie)) - ga;
        d = F / (D > 1e-9f ? D : 1e-9f);
        x -= ms_clamp(d, -4.f, 4.f);
    }
    x = ms_clamp(x, -22.f, 22.f);
    vb = (c + ga * x) / al;                         /* (the network's equation holds exactly, whatever x's last digit) */
    y = vb + K35_NVT * x;
    va = hp ? (vb * (1.f + q2) - f->s2 - in) * iq : (vb * (1.f + q2) - f->s2) * iq;
    w = va - beta * y;
    f->s1 = w + w - f->s1;
    f->s2 = (hp ? vb - in : vb) * 2.f - f->s2;
    f->va = va;
    f->vb = vb;
    f->x = x;
    return y;
}

/* ---- oscillators: a 32-bit phase, polyBLEP at the steps */
static inline float ms_blep(uint32_t ph, uint32_t inc)   /* the correction around a step at phase 0 */
{
    float t, dt = (float)(inc >> 8);
    if (ph < inc) {
        t = (float)(ph >> 8) / dt;
        return t + t - t * t - 1.f;
    }
    if (ph > 0u - inc) {
        t = ((float)(ph >> 8) - 16777216.f) / dt;
        return t * t + t + t + 1.f;
    }
    return 0.f;
}
static inline float ms_saw(uint32_t ph, uint32_t inc)    /* -1..1, rising */
{
    return (float)(int32_t)(ph - 0x80000000u) * (1.f / 2147483648.f) - ms_blep(ph, inc);
}
static inline float ms_pulse(uint32_t ph, uint32_t inc, uint32_t w)   /* high for the first w of the cycle */
{
    return (ph < w ? 1.f : -1.f) + ms_blep(ph, inc) - ms_blep(ph - w, inc);
}
static inline float ms_noise(uint32_t *s)           /* white, -1..1 */
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return (float)(int32_t)*s * (1.f / 2147483648.f);
}

/* VCO 1 triangle: full-wave rectified sawtooth with diodes D2/D3 in series, flattening one peak around 0 V.
 * Table maps |saw| (0..3.84 V, 64 steps) to voltage after diode drop into 100 k. */
static const float MS20_TRI[66] = {
    0.0000f, 0.0007f, 0.0031f, 0.0104f, 0.0273f, 0.0555f, 0.0924f, 0.1353f, 0.1819f, 0.2310f, 0.2820f, 0.3343f, 0.3876f,
    0.4417f, 0.4964f, 0.5516f, 0.6073f, 0.6633f, 0.7196f, 0.7762f, 0.8330f, 0.8900f, 0.9472f, 1.0045f, 1.0620f, 1.1196f,
    1.1773f, 1.2352f, 1.2931f, 1.3511f, 1.4092f, 1.4674f, 1.5256f, 1.5839f, 1.6423f, 1.7007f, 1.7591f, 1.8177f, 1.8762f,
    1.9348f, 1.9935f, 2.0522f, 2.1109f, 2.1696f, 2.2284f, 2.2873f, 2.3461f, 2.4050f, 2.4639f, 2.5228f, 2.5818f, 2.6407f,
    2.6997f, 2.7588f, 2.8178f, 2.8769f, 2.9359f, 2.9950f, 3.0542f, 3.1133f, 3.1724f, 3.2316f, 3.2908f, 3.3500f, 3.4092f,
    3.4092f};
/* The VCA's transistor (Q19, 2SC945): log2 of the volts across its base resistor (R124, 1 M) against the volts on that
 * resistor's far end, 0 .. 5.12 V in steps of 0.04: u + VT ln(1 + u / (Is R)) = V. The knee is the base's 0.4 .. 0.5 V. */
static const float MS20_VCA[130] = {
    -40.000f, -23.688f, -21.177f, -18.892f, -16.649f, -14.417f, -12.193f, -10.003f, -7.943f, -6.228f, -4.998f, -4.154f,
    -3.550f, -3.093f, -2.731f, -2.432f, -2.179f, -1.961f, -1.769f, -1.598f, -1.444f, -1.304f, -1.175f, -1.057f, -0.947f,
    -0.845f, -0.749f, -0.658f, -0.573f, -0.493f, -0.417f, -0.344f, -0.275f, -0.208f, -0.145f, -0.084f, -0.026f, 0.030f,
    0.084f, 0.137f, 0.187f, 0.236f, 0.283f, 0.329f, 0.373f, 0.416f, 0.458f, 0.499f, 0.538f, 0.577f, 0.614f, 0.651f,
    0.687f, 0.721f, 0.755f, 0.789f, 0.821f, 0.853f, 0.884f, 0.915f, 0.945f, 0.974f, 1.003f, 1.031f, 1.059f, 1.086f,
    1.112f, 1.138f, 1.164f, 1.189f, 1.214f, 1.239f, 1.263f, 1.286f, 1.309f, 1.332f, 1.355f, 1.377f, 1.399f, 1.420f,
    1.441f, 1.462f, 1.483f, 1.503f, 1.523f, 1.543f, 1.563f, 1.582f, 1.601f, 1.620f, 1.638f, 1.656f, 1.674f, 1.692f,
    1.710f, 1.727f, 1.744f, 1.761f, 1.778f, 1.795f, 1.811f, 1.827f, 1.843f, 1.859f, 1.875f, 1.891f, 1.906f, 1.921f,
    1.936f, 1.951f, 1.966f, 1.980f, 1.995f, 2.009f, 2.023f, 2.037f, 2.051f, 2.065f, 2.079f, 2.092f, 2.106f, 2.119f,
    2.132f, 2.145f, 2.158f, 2.171f, 2.184f, 2.196f, 2.209f, 2.209f};

static void ms20_init(ms20_t *s)
{
    uint32_t i;
    uint8_t *b = (uint8_t *)s;
    for (i = 0; i < sizeof *s; i++)
        b[i] = 0;
    s->rng = 0x2F6E2B1u;
    s->cv = 0.f;                                    /* C6, the keyboard's hold capacitor, discharged: no key yet */
    s->mg_up = 1;
    s->fm_y = 1.f;
    s->hp_t = s->lp_t = 0.01f;
}

/* ---- Patch panel network solver
 * Each output is a Thevenin source behind its output resistor; each plugged input sees its internal bias
 * voltage and impedance (normal broken). Connected jack groups resolve to sum(V / R) / sum(1 / R),
 * handling dummy plugs, the momentary switch, output loading, and arbitrary jack pairs uniformly.
 * Component values from the service manual (see docs/MS20-CIRCUIT.md). */
static const float MS_ROUT[MS_NSRC] = {             /* ohms */
    [SRC_KBD_CV] = 100.f,                           /* R46 */
    [SRC_KBD_TRIG] = 880.f,                         /* R82 1.3 k, R81 2.7 k */
    [SRC_MG_TRI] = 1000.f,                          /* R144 */
    [SRC_MG_RECT] = 1660.f,                         /* R145 4.3 k, R146 2.7 k */
    [SRC_EG1] = 667.f,                              /* R19 1 k, R18 2 k */
    [SRC_EG1_REV] = 1350.f,                         /* R164, R165 2.7 k */
    [SRC_EG2_REV] = 1000.f,                         /* R170 */
    [SRC_SH] = 1000.f,                              /* R7 */
    [SRC_MVCA] = 470.f,                             /* R23 */
    [SRC_WHITE] = 1000.f, [SRC_PINK] = 1000.f,      /* R37, R30 */
    [SRC_WHEEL] = 2500.f,                           /* the wheel's 10 k pot, at its middle */
    [SRC_BUTTON] = 0.f,                             /* (a contact: see ms20_nets) */
    [SRC_SIGNAL] = 2000.f,                          /* R119 1 k into VOLUME, 10 k A */
    [SRC_PHONES] = 220.f,                           /* R43, R46 */
    [SRC_ESP_AMP] = 2400.f,                         /* R47 22 k, R46 2.7 k */
    [SRC_ESP_BPF] = 2130.f,                         /* R39 10 k, R40 2.7 k */
    [SRC_ESP_FV] = 1500.f,                          /* R36 470, CV ADJUST 10 k A, R38 1 k */
    [SRC_ESP_ENV] = 1350.f,                         /* R11, R35 2.7 k */
    [SRC_ESP_TRIG] = 1660.f,                        /* R9 4.3 k, R34 2.7 k */
    [SRC_VCO1] = 1000.f, [SRC_VCO2] = 1000.f,       /* (not on an MS-20: taken as 1 k) */
};
static const float MS_TERM[MS_NDST][2] = {          /* volts, ohms */
    [DST_TOTAL] = {0.f, 100e3f},                    /* R72 */
    [DST_FREQ] = {0.f, 100e3f},                     /* EG1 / EXT, 100 k A */
    [DST_VCO_CV] = {0.27f, 393e3f},                 /* the SCALE ladder (~400 k) to IC3's virtual ground; R207 22 M to +15 V */
    [DST_VCO2_CV] = {0.27f, 393e3f},                /* (VCO 2's ladder, the same; R205 / R206) */
    [DST_EXT_IN] = {0.f, 50e3f},                    /* R90 100 k, R75 100 k to IC8's virtual ground */
    [DST_HP_CUT] = {0.f, 100e3f}, [DST_LP_CUT] = {0.f, 100e3f},   /* EG2 / EXT, 100 k A */
    [DST_GAIN] = {0.f, 1e6f},                       /* R122 */
    [DST_TRIG] = {15.f, 220e3f},                    /* R193, up */
    [DST_EG1_TRIG] = {15.f, 220e3f},                /* R158, up */
    [DST_SH_IN] = {0.f, 100e3f},                    /* R10 */
    [DST_SH_CLOCK] = {4.69f, 68.75e3f},             /* R38 220 k up, R13 100 k down */
    [DST_MVCA_IN] = {0.f, 100e3f},                  /* R17 to IC3's virtual ground */
    [DST_MVCA_CTL] = {0.f, 110e3f},                 /* R22 10 k, R25 100 k */
    [DST_ESP_IN] = {0.f, 1e6f},                     /* R51 2.2 k, SIGNAL LEVEL 1 M */
};

/* the jacks joined, as the render uses them: for each set with an input on it, its constant part and its outputs' shares */
typedef struct {
    uint8_t plug[MS_NDST];                          /* a plug is in this input */
    uint8_t net[MS_NDST];                           /* which set it is in */
    uint8_t nnet, nout[MS_NCABLE * 2], out[MS_NCABLE * 2][MS_NCABLE * 2];
    float c[MS_NCABLE * 2], w[MS_NCABLE * 2][MS_NCABLE * 2];
} ms20_nets_t;

static void ms20_nets(ms20_nets_t *nt, const ms20_patch_t *pt, uint32_t btn)
{
    uint8_t root[MS_NJACK], used[MS_NJACK], id[MS_NJACK];
    uint32_t i, j;
    for (i = 0; i < MS_NJACK; i++) { root[i] = (uint8_t)i; used[i] = 0; id[i] = 255; }
    for (i = 0; i < MS_NDST; i++) nt->plug[i] = 0;
    nt->nnet = 0;
    for (i = 0; i < MS_NCABLE; i++) {               /* union the cables' ends */
        uint32_t a = pt->cable[i][0], b = pt->cable[i][1];
        if (a >= MS_NJACK || b >= MS_NJACK) continue;
        if (a) used[a] = 1;
        if (b) used[b] = 1;
        if (a && b) {
            while (root[a] != a) a = root[a];
            while (root[b] != b) b = root[b];
            if (a != b) root[b] = (uint8_t)a;
        }
    }
    for (i = 1; i < MS_NJACK; i++) {                /* each set: its conductances and the voltages behind them */
        uint32_t r = i, k;
        float g;
        if (!used[i]) continue;
        while (root[r] != r) r = root[r];
        if (id[r] == 255) {
            if (nt->nnet >= MS_NCABLE * 2) continue;
            id[r] = nt->nnet++;
            nt->nout[id[r]] = 0;
            nt->c[id[r]] = 0.f;
            nt->w[id[r]][0] = 0.f;                  /* (w[k][0] holds the set's total conductance until the end) */
        }
        k = id[r];
        if (i < MS_NSRC) {                          /* an output */
            if (i == SRC_BUTTON) {                  /* the momentary switch: a contact to ground, or nothing */
                if (btn) nt->w[k][0] += 1.f;           /* (1 ohm to 0 V: c gains nothing) */
            } else if (nt->nout[k] < MS_NCABLE * 2 - 1u) {
                g = 1.f / MS_ROUT[i];
                nt->out[k][1u + nt->nout[k]] = (uint8_t)i;
                nt->w[k][1u + nt->nout[k]] = g;
                nt->nout[k]++;
                nt->w[k][0] += g;
            }
        } else {                                    /* an input: its pull-up or pull-down */
            uint32_t d = i - (MS_NSRC - 1u);
            g = 1.f / MS_TERM[d][1];
            nt->c[k] += MS_TERM[d][0] * g;
            nt->w[k][0] += g;
            nt->plug[d] = 1;
            nt->net[d] = (uint8_t)k;
        }
    }
    for (i = 0; i < nt->nnet; i++) {                /* divide through by the set's conductance */
        float gs = nt->w[i][0], inv = gs > 0.f ? 1.f / gs : 0.f;
        nt->c[i] *= inv;
        for (j = 1; j <= nt->nout[i]; j++) nt->w[i][j] *= inv;
    }
}

/* an input's voltage: what its cable's jacks settle at, or `normal` (what the jack's switch connects) with no plug in it */
#define MS_IN(d, normal) (nets.plug[d] ? s->jv[d] : (normal))
/* a trigger input: on while its voltage is under 2.2 V (D31 / R187 100 k / R186 33 k into Q22's base) */
#define MS_TRIG(d, normal) (nets.plug[d] ? s->jv[d] < 2.2f : (normal))

/* n mono samples at 44.1 kHz into out: SIGNAL OUT's volts / MS_VFS */
/* What a VCO does with no control voltage. R203 220 k / R201 1 k off +15 V into R202 10 M (VCO 2: R205, R204, R206)
 * put 6.79 nA into the integrator whatever the keyboard says. The adjustment procedure (7-2) trims each VCO at 8' with
 * two screws: the high one sets the gain (VR1, VR6: the exponential pair's bias), the low one is the TL081's offset null
 * (VR2, VR5), which puts the summing node at d = 6.79 nA / (1 / R8' + 1 / 10 M): the trickle cancels at 8' exactly. At
 * the other footages it does not: in volts on the CV, R x 6.79 nA - d (1 + R / 10 M), R the SCALE resistance
 * (800 k / 2^n; n8: the position that is 8'). VCO 1 at 32': +4.0 mV, its lowest C 7 cents sharp (the procedure allows
 * 10 off 8'); with no key yet (0 V) it creeps at 0.13 Hz there and stands at 8'. */
static inline float ms20_voff(uint32_t n, uint32_t n8)
{
    const float i0 = 15.f * 1e3f / 221e3f / 10e6f, r = 800e3f / (float)(1u << n), r8 = 800e3f / (float)(1u << n8);
    const float d = i0 / (1.f / r8 + 1e-7f);
    return r * i0 - d * (1.f + r * 1e-7f);
}

static void ms20_render(ms20_t *s, const ms20_patch_t *pt, const ms20_ctl_t *ctl, float *out, uint32_t n)
{
    /* the half-band decimator: 23 taps (Kaiser 7), -0.3 dB at 16 kHz, -50 dB from 30 kHz (what folds below 14 kHz) */
    static const float HB[6] = {0.3098484f, -0.0830214f, 0.0314702f, -0.0105175f, 0.0024218f, -0.0001716f};
    ms20_nets_t nets;
    float *sm = s->sm;
    float kbd, porta, r_e1a, r_e1r, r_e2a, r_e2d, r_e2r, e1_delay, e2_hold, sus, mg_su, mg_sd, be_hp, be_lp, pw_v, vol;
    float c1, c2, e2f, hp_base, lp_base, a_fm_mg, a_fm_eg, a_hp_mg, a_hp_eg, a_lp_mg, a_lp_eg, lv1, lv2, rs1, rs2, sc = 0.f, sc2 = 0.f, voff1, voff2;
    float esp_g, esp_klo, esp_khi, esp_kfv, esp_thr, esp_adj, wheel_v = (float)ctl->wheel * (5.f / 127.f);
    uint32_t i, w1 = pt->p[P_V1WAVE] >> 5, w2 = pt->p[P_V2WAVE] >> 5, pw, esp_on, kgate = ctl->gate, btn = ctl->button;
    uint32_t ph1 = s->ph1, ph2 = s->ph2;

    ms20_nets(&nets, pt, btn);
    kbd = (float)n * (1.f / 900.f);                 /* the knobs glide to their places in some 20 ms (a detent is a step otherwise) */
    if (kbd > 1.f)
        kbd = 1.f;
    for (i = 0; i < MS_NPARAM; i++) {
        float want = (float)pt->p[i] * (1.f / 127.f);
        sm[i] = s->ok ? sm[i] + (want - sm[i]) * kbd : want;
    }

    /* The keyboard: a ladder of 1 % resistors, a semitone a step, so its voltage is proportional to the frequency.
     * 1 V at the lowest C (C3 at 8'), 8 V at the highest (the specifications; the adjustment procedure halves it an
     * octave down). MIDI note 48 is that lowest C. */
    kbd = ms_exp2(((float)ctl->note * (1.f / 256.f) - 48.f) * (1.f / 12.f));
    s->ok = 1;
    porta = ms_rc(sm[P_PORTA], 2e6f, 680.f, 0.22e-6f);   /* VR10 2 M A + R30 680 into C6 0.22 uF: up to 0.44 s */

    /* EG1: C38 3.3 uF, ATTACK (VR28) and RELEASE (VR29) 2 M A through the 4007's switches (some 400 ohms on) */
    r_e1a = ms_rc(sm[P_E1_ATK], 2e6f, 400.f, 3.3e-6f);
    r_e1r = ms_rc(sm[P_E1_REL], 2e6f, 1067.f, 3.3e-6f);  /* (from R159 1 k / R160 2 k: 667 ohms) */
    /* EG2: C40 6.8 uF. ATTACK (VR33 2 M A) from R180 470 / R179 680 (8.87 V, 278 ohms) until 5.07 V (R181 43 k,
     * R176 150 k to -15 V, Q21); DECAY VR32 1 M A + R178 1 k; RELEASE VR30 1 M A */
    r_e2a = ms_rc(sm[P_E2_ATK], 2e6f, 678.f, 6.8e-6f);
    r_e2d = ms_rc(sm[P_E2_DEC], 1e6f, 1400.f, 6.8e-6f);
    r_e2r = ms_rc(sm[P_E2_REL], 1e6f, 400.f, 6.8e-6f);
    /* DELAY: VR27 2 M A, C39 6.8 uF, to a CMOS gate's threshold (half the supply): 0.69 RC, 9.4 s at 10.
     * HOLD: VR34 2 M A, C42 10 uF, the same way: 13.9 s at 10 (the specifications say 10 and 20) */
    e1_delay = ms_taper(sm[P_E1_DELAY]) * (2e6f * 6.8e-6f * 0.6931f * MS_FS);
    e2_hold = ms_taper(sm[P_E2_HOLD]) * (2e6f * 10e-6f * 0.6931f * MS_FS);
    /* SUSTAIN: VR31 10 k B with R168 2.7 k across it, under R169 4.3 k from +15 V: 0 .. 4.96 V. It is also what EG2
     * is measured against on its way to the filters (below). */
    sus = sm[P_E2_SUS] * 4.96f;

    /* The modulation generator: an integrator (C22 0.1 uF) and a Schmitt trigger (R149 20 k / R150 100 k: it turns at
     * +-2.7 V, 5.4 V peak to peak). FREQUENCY (VR25, 10 k B, between R147 2.2 k and R148 48) takes a part of the
     * trigger's +-13.5 V; WAVE FORM (VR26, 1 M B, wiper fed through R154 8.2 k) is the integrator's resistor, one
     * half for the way up and the other for the way down: the sum is constant, so the rate does not move with the
     * shape. 0.1 Hz .. 20 Hz. */
    {
        float p = sm[P_MG_RATE], w = sm[P_MG_WAVE];
        float lo = 48.f + p * 10e3f, hi = 2.2e3f + (1.f - p) * 10e3f;
        float vth = 13.5f * lo / 12248.f, rth = lo * hi / 12248.f + 8.2e3f;
        mg_su = vth / ((w * 1e6f + rth) * 0.1e-6f * MS_FS);          /* volts a sample, rising */
        mg_sd = vth / (((1.f - w) * 1e6f + rth) * 0.1e-6f * MS_FS);
    }

    be_hp = ms20_peak(sm[P_HP_PEAK]);
    be_lp = ms20_peak(sm[P_LP_PEAK]);
    /* PW: the comparator's threshold, from 0 V to the top of VR9 (1 M B under R10 300 k from +15 V, R11 100 k across):
     * 3.49 V of the sawtooth's 3.75. Half the cycle down to a sliver of 3.5 % */
    pw_v = 0.5f + sm[P_V1PW] * (3.49f / 7.5f);
    pw = (uint32_t)(pw_v * 4294967040.f);
    vol = ms_taper(sm[P_VOLUME]) * (10.f / 11.f);   /* R119 1 k into VOLUME (10 k A) */

    /* A VCO's frequency is its control voltage through the SCALE resistors (100 k x 8, 0.1 %: 800 k .. 100 k) as a
     * current into C2 6200 p (VCO 1) or C11 3000 p (VCO 2): 130.81 Hz a volt at 8' on VCO 1. c1, c2: phase steps a volt.
     * MASTER TUNE: +-15 V through R36 10 M into the FM amplifier (R39 110 k), R40 5.11 k / R38 100: +-3.17 mV on the
     * pair's base, +-0.177 octave. PITCH: +-15 V through R55 75 k onto R47 100: +-20 mV, +-1.115 octave. */
    c1 = ms_exp2(22.6029f + ((float)(pt->p[P_V1SCALE] >> 5) - 2.f) + (sm[P_TUNE] - 0.5039f) * 0.3536f);
    c2 = ms_exp2(22.6029f + ((float)(pt->p[P_V2SCALE] >> 5) - 1.f) + (sm[P_TUNE] - 0.5039f) * 0.3536f
               + (sm[P_V2PITCH] - 0.5039f) * 2.230f);
    voff1 = ms20_voff((pt->p[P_V1SCALE] >> 5) & 3u, 2u);
    voff2 = ms20_voff((pt->p[P_V2SCALE] >> 5) & 3u, 1u);
    /* FM: 1 V on a knob at 10 is 1.1 * 100 / 5210 = 21.1 mV on the base: 1.178 octaves (17.92 mV an octave at 300 K) */
    a_fm_mg = ms_amount(sm[P_FM_MG], 1.f) * 1.178f;
    a_fm_eg = ms_amount(sm[P_FM_EG], 1.f) * 1.178f;
    /* The cutoffs. CUTOFF's +-15 V through 470 k, the two intensity pots through 100 k each, summed on 10 k (R95),
     * times 22.3 (R94 100 k, R97 4.7 k) into the KORG35's control pin. The chip turns that into octaves; how many a
     * volt depends on the chip (0.91 here: a vintage MS-20's low-pass sings from 13 Hz at 0, and the adjustment
     * procedure puts 5 at 500 Hz). So CUTOFF spans +-5.27 octaves around 500 Hz, and 1 V on EG2 / EXT at 10 is
     * 1.65 octaves (Korg's sheet: "2 oct / V"). CUTOFF's wiper is loaded by its 470 k: a little bow in the scale. */
    hp_base = MS20_CUT_SPAN * (2.f * sm[P_HP_CUT] - 1.f) / (1.f + 0.2128f * sm[P_HP_CUT] * (1.f - sm[P_HP_CUT]));
    lp_base = MS20_CUT_SPAN * (2.f * sm[P_LP_CUT] - 1.f) / (1.f + 0.2128f * sm[P_LP_CUT] * (1.f - sm[P_LP_CUT]));
    a_hp_mg = ms_amount(sm[P_HP_MG], 1.081f) * MS20_CUT_OCTV;
    a_hp_eg = ms_amount(sm[P_HP_EG], 1.081f) * MS20_CUT_OCTV;
    a_lp_mg = ms_amount(sm[P_LP_MG], 1.081f) * MS20_CUT_OCTV;
    a_lp_eg = ms_amount(sm[P_LP_EG], 1.081f) * MS20_CUT_OCTV;
    /* The mixer: each VCO's level pot (100 k A) is fed from its waveform's source resistance and loaded by the
     * mixer's 100 k. The sawtooths come through 100 k (R4), the others from dividers of a few k */
    rs1 = w1 == 0 ? 0.0909f : w1 == 1 ? 1.f : w1 == 2 ? 0.0637f : 0.01f;
    rs2 = w2 == 0 ? 1.f : w2 == 3 ? 0.041f : 0.0637f;
    lv1 = ms_level(sm[P_MIX1], rs1);
    lv2 = ms_level(sm[P_MIX2], rs2);

    /* The external signal processor (KLM-129). SIGNAL LEVEL: VR7 1 M B with R53 1 M across it, over R51 2.2 k, then
     * 22 times: far more than a line needs (it is for a microphone). LOW CUT 47 Hz .. 2.4 kHz (2 poles, 0.033 uF),
     * HIGH CUT 104 Hz .. 5.3 kHz (4 poles, 0.015 uF), both 2 k + 100 k "C" (reverse audio) pots; the frequency-to-
     * voltage converter's smoothing (0.056 uF, 10 k + the same pot) follows LOW CUT. */
    esp_on = nets.plug[DST_ESP_IN];
    esp_g = sm[P_ESP_LEVEL] / (1.f + sm[P_ESP_LEVEL]) * 454.5f * 22.f;
    {
        float rl = 2e3f + 100e3f * ms_taper(1.f - sm[P_ESP_LOW]), rh = 2e3f + 100e3f * ms_taper(1.f - sm[P_ESP_HIGH]);
        esp_klo = 1.f / (1.f + rl * 0.033e-6f * MS_FS);
        esp_khi = 1.f / (1.f + rh * 0.015e-6f * MS_FS);
        esp_kfv = 1.f / (1.f + (rl + 8e3f) * 0.056e-6f * MS_FS);
    }
    esp_thr = 1.07f + sm[P_ESP_THR] * 13.04f;       /* VR3 10 k between R20 680 and R1 820: 1.07 .. 14.1 V at the comparator */
    esp_adj = (1.f + 10.f * ms_taper(sm[P_ESP_CV])) * (1.f / 11.f);   /* CV ADJUST: VR4 10 k A over R38 1 k */

    for (i = 0; i < 2u * n; i++) {
        float mg_rect, v_tb, v_freq, cv, cv2, fm, o1, o2, sq1, x, y, g, e1v, white, pink, f, u, vx, v;
        uint32_t inc1, inc2, g1, g2;

        /* the modulation generator */
        if (s->mg_up) {
            s->mg += mg_su;
            if (s->mg >= 2.7f) { s->mg = 2.7f - (s->mg - 2.7f) * (mg_sd / mg_su); s->mg_up = 0; }
        } else {
            s->mg -= mg_sd;
            if (s->mg <= -2.7f) { s->mg = -2.7f + (-2.7f - s->mg) * (mg_su / mg_sd); s->mg_up = 1; }
        }
        mg_rect = s->mg_up ? 0.f : 5.2f;            /* the trigger's output through R145 4.3 k / R146 2.7 k, D14: high while the triangle falls */

        /* The noise generator: a transistor's junction, amplified to 5 V peak to peak. "Pink" is that through R35 6.8 k
         * into an integrator leaking through R28 220 k (C2 0.022 uF): 6 dB an octave from 33 Hz on, not 3. */
        white = (ms_noise(&s->rng) + ms_noise(&s->rng)) * 1.25f;
        s->pk += (white * 32.35f - s->pk) * 0.002343f;
        pink = ms_clamp(-s->pk, -13.5f, 13.5f);

        /* the triggers. TRIG IN is wired to the keyboard's, EG1 TRIG IN to TRIG IN's jack */
        g2 = MS_TRIG(DST_TRIG, kgate);
        g1 = MS_TRIG(DST_EG1_TRIG, g2);
        if (g1 && !s->gate1) { s->e1_st = 1; s->e1_t = e1_delay; }
        if (!g1 && s->gate1) s->e1_st = 3;
        s->gate1 = (uint8_t)g1;
        /* HOLD keeps EG2's trigger on for a while after it has gone (a monostable after the input; EG1 does not see it) */
        if (g2) s->e2_t = e2_hold;
        else if (s->e2_t > 0.f) { s->e2_t -= 1.f; g2 = s->key2; }
        s->key2 = (uint8_t)g2;
        if (g2 && !s->gate2) s->e2_st = 1;
        if (!g2 && s->gate2) s->e2_st = 3;
        s->gate2 = (uint8_t)g2;

        /* EG1: C38 rests at +10 V; the trigger (after DELAY) lets it down to 0 V through ATTACK, its end lets it back
         * up through RELEASE. Both are plain RCs: the top is approached, never reached. e1: 0 at rest, 1 at the top */
        if (s->e1_st == 1) {
            if (s->e1_t > 0.f) s->e1_t -= 1.f;
            else s->e1 += (1.f - s->e1) * r_e1a;
        } else if (s->e1_st == 3) {
            s->e1 -= s->e1 * r_e1r;
            if (s->e1 < 1e-6f) { s->e1 = 0.f; s->e1_st = 0; }
        }
        /* EG2: up toward 8.87 V until 5.07 V, then down to SUSTAIN; after the trigger, to 0 V */
        if (s->e2_st == 1) {
            s->e2 += (8.87f - s->e2) * r_e2a;
            if (s->e2 >= 5.07f) { s->e2 = 5.07f; s->e2_st = 2; }
        } else if (s->e2_st == 2) {
            s->e2 += (sus - s->e2) * r_e2d;
        } else if (s->e2_st == 3) {
            s->e2 -= s->e2 * r_e2r;
            if (s->e2 < 1e-6f) { s->e2 = 0.f; s->e2_st = 0; }
        }
        e1v = s->e1 * 5.f - 5.f;                    /* EG1 as the VCOs get it (R1 1 k / R2 2 k): -5 V at rest, 0 V at the top */
        /* EG2 reaches the filters' EG2 / EXT knobs and REV OUT through a difference amplifier (IC20: R177, R173, R171,
         * R172, 100 k each) whose other input is SUSTAIN's wiper: the filters get EG2 minus the sustain level. With
         * SUSTAIN up, CUTOFF is where the filter sits while a key is held; before and after, it is below. */
        e2f = s->e2 - sus;

        /* The keyboard's voltage reaches C6 through PORTAMENTO and a FET that conducts only while a key is down:
         * a glide stops where it is when the key is let go, and the pitch is held there. */
        if (kgate)
            s->cv += (kbd - s->cv) * porta;

        /* the panel's voltages */
        s->sv[SRC_KBD_CV] = s->cv;
        s->sv[SRC_KBD_TRIG] = kgate ? 0.f : 5.2f;
        s->sv[SRC_MG_TRI] = s->mg;
        s->sv[SRC_MG_RECT] = mg_rect;
        s->sv[SRC_EG1] = e1v + 5.f;                 /* EG1 OUT (R19 1 k, R18 2 k to +15 V) and the panel's legend: 0 V, up to +5 V */
        s->sv[SRC_EG1_REV] = -e1v;                  /* +5 V at rest, down to 0 V */
        s->sv[SRC_EG2_REV] = -e2f;                  /* SUSTAIN at 0: 0 V, down to -5 V */
        s->sv[SRC_WHITE] = white;
        s->sv[SRC_PINK] = pink;
        s->wheel += (wheel_v - s->wheel) * 0.00075f;   /* its steps (an encoder's detents, a MIDI CC) slewed: 15 ms */
        s->sv[SRC_WHEEL] = s->wheel;
        s->sv[SRC_BUTTON] = 0.f;                    /* (a contact to ground: ms20_nets) */
        /* every set of joined jacks settles: the plugged inputs' voltages for this sample (the outputs set later in the
         * sample, S&H to VCO 2, count with last sample's: a cable is a sample long) */
        {
            uint32_t k, q;
            float nv[MS_NCABLE * 2];
            for (k = 0; k < nets.nnet; k++) {
                float v2 = nets.c[k];
                for (q = 1; q <= nets.nout[k]; q++) v2 += nets.w[k][q] * s->sv[nets.out[k][q]];
                nv[k] = v2;
            }
            for (k = 0; k < MS_NDST; k++) if (nets.plug[k]) s->jv[k] = nv[nets.net[k]];
        }

        /* "Sample and hold": a FET (F1) between a buffer and C1 0.1 uF, on while CLOCK IN is above 1.36 V (R6 10 k /
         * R9 1 k), which it is with nothing plugged in (R38 220 k to +15 V): it follows the input (through R4 470 and
         * the FET, 2 kHz) while the clock is high and holds while it is low. */
        if (!nets.plug[DST_SH_CLOCK] || s->jv[DST_SH_CLOCK] > 1.36f)
            s->sh += (MS_IN(DST_SH_IN, 0.f) - s->sh) * 0.137f;
        s->sv[SRC_SH] = s->sh;

        /* The modulation VCA: -0.75, then a photocell as the next amplifier's input resistor, lit by an LED whose
         * current is 0.94 mA a volt of CONTROL from 0.18 V up (TR1, R26 1 k, R27 33 k). CONTROL is wired to EG1 (0 V at
         * rest, +5 V at the top). Trimmed (VR2) to pass a signal as it is at 5 V. The cell's law (conductance as the
         * current to the 0.75) and its slowness (3 ms on, 40 ms off) are a CdS cell's, not from the diagram. */
        if (nets.plug[DST_MVCA_IN]) {
            float c = MS_IN(DST_MVCA_CTL, s->e1 * 5.f), want = 0.f, d;
            c = ms_clamp(c * 0.2072f - 0.0363f, 0.f, 1.2f);       /* the LED's current, of what 5 V gives */
            if (c > 0.f) {
                float r = c;                         /* c ^ 0.75 = sqrt(c) * sqrt(sqrt(c)): two Newton steps each */
                float q = 0.5f * (1.f + r);
                q = 0.5f * (q + r / q); q = 0.5f * (q + r / q); q = 0.5f * (q + r / q);
                r = 0.5f * (1.f + q);
                r = 0.5f * (r + q / r); r = 0.5f * (r + q / r); r = 0.5f * (r + q / r);
                want = q * r;
            }
            d = want - s->mvca;
            s->mvca += d * (d > 0.f ? 0.003773f : 0.0002834f);
            s->sv[SRC_MVCA] = ms_clamp(s->jv[DST_MVCA_IN] * s->mvca, -13.5f, 13.5f);
        }

        /* the external signal processor */
        if (esp_on) {
            float a = s->jv[DST_ESP_IN] * esp_g, b, r, k;
            if (a > 13.5f || a < -13.5f) { a = ms_clamp(a, -13.5f, 13.5f); s->esp_pk = 0.05f * MS_FS; }   /* PEAK: the amplifier clips (lit 50 ms) */
            else if (s->esp_pk > 0.f) s->esp_pk -= 1.f;
            s->esp_c += (a - s->esp_c) * 0.00227f;  /* C15 / C16 between the two amplifiers: 32 Hz */
            a -= s->esp_c;
            s->sv[SRC_ESP_AMP] = a * 0.1093f;       /* R47 22 k / R46 2.7 k */
            b = a * 0.595f;                         /* R55 680 / R56 1 k */
            s->esp_h1 += (b - s->esp_h1) * esp_klo; b -= s->esp_h1;      /* LOW CUT: two equal poles */
            s->esp_h2 += (b - s->esp_h2) * esp_klo; b -= s->esp_h2;
            s->esp_l[0] += (b - s->esp_l[0]) * esp_khi;                 /* HIGH CUT: four */
            s->esp_l[1] += (s->esp_l[0] - s->esp_l[1]) * esp_khi;
            s->esp_l[2] += (s->esp_l[1] - s->esp_l[2]) * esp_khi;
            s->esp_l[3] += (s->esp_l[2] - s->esp_l[3]) * esp_khi;
            b = s->esp_l[3];
            s->sv[SRC_ESP_BPF] = b * 0.2126f;       /* R39 10 k / R40 2.7 k */
            /* the envelope follower, off the amplifier: D4, R48 1 k into C1 1 uF, R3 22 k (and R2 100 k) down */
            r = a - 0.5f;
            if (r > s->esp_env) s->esp_env += (r - s->esp_env) * 0.0118f;
            s->esp_env -= s->esp_env * 0.00063f;
            s->sv[SRC_ESP_ENV] = s->esp_env * 0.5f; /* R11 / R35, 2.7 k each */
            if (s->esp_env > esp_thr + 0.45f) s->esp_gate = 1;            /* (R19 100 k / R21 3.3 k: the comparator's hysteresis) */
            else if (s->esp_env < esp_thr - 0.45f) s->esp_gate = 0;
            s->sv[SRC_ESP_TRIG] = s->esp_gate ? 0.f : 5.2f;
            /* F-V: a pulse of a fixed width for every cycle of the band-passed signal (two comparators of an LM339,
             * TR2, C2 12000 p), smoothed: a voltage proportional to the frequency, as the VCOs want it. The pulse's
             * width (taken as 1.5 ms: 22.5 mV a hertz at CV ADJUST 10) is not on the diagram. */
            k = 0.f;
            s->esp_t += 1.f;
            if (b > 0.068f && !s->esp_pos) {
                s->esp_pos = 1;
                if (s->esp_t >= 1.5e-3f * MS_FS) { s->esp_t = 0.f; k = 15.f * 1.5e-3f * MS_FS; }   /* (a cycle inside the last pulse is lost) */
            } else if (b < -0.068f) {
                s->esp_pos = 0;
            }
            s->esp_f1 += (k - s->esp_f1) * esp_kfv;
            s->esp_f2 += (s->esp_f1 - s->esp_f2) * esp_kfv;
            s->sv[SRC_ESP_FV] = ms_clamp(s->esp_f2, 0.f, 15.f) * esp_adj;
        }

        /* The VCOs. TOTAL (wired to the MG's triangle) goes through a buffer with 10 k / 100 k in front of it (0.909)
         * to the three MG / T.EXT knobs; FREQ (wired to EG1) to EG1 / EXT. VCO 1 + 2 CV IN is wired to the keyboard,
         * VCO 2 CV IN to VCO 1 + 2 CV IN: a plug there gives VCO 2 a voltage of its own. Below its offset (ms20_voff:
         * about 0 V) a VCO stops. */
        v_tb = MS_IN(DST_TOTAL, s->mg) * 0.909f;
        v_freq = MS_IN(DST_FREQ, e1v);
        cv = MS_IN(DST_VCO_CV, s->cv);
        cv2 = MS_IN(DST_VCO2_CV, cv);
        f = ms_clamp(v_tb * a_fm_mg + v_freq * a_fm_eg, -12.f, 6.f);
        if (f != s->fm_x) { s->fm_x = f; s->fm_y = ms_exp2(f); }     /* (with both FM knobs at 0 it never changes) */
        fm = s->fm_y;
        f = (cv + voff1) * c1 * fm;
        inc1 = (uint32_t)ms_clamp(f, 256.f, 2.0e9f);   /* (256: standing still, and the steps' corrections stay finite) */
        f = (cv2 + voff2) * c2 * fm;
        inc2 = (uint32_t)ms_clamp(f, 256.f, 2.0e9f);
        ph1 += inc1;
        ph2 += inc2;
        if (s->scope_trig && (s->scope_trig == 1u ? ph1 < inc1 : ph2 < inc2)) {   /* the scope's trigger */
            s->scope_wrap[s->scope_wn & 7u] = s->scope_w;
            s->scope_wn++;
            s->scope_inc = s->scope_trig == 1u ? inc1 : inc2;
        }
        /* VCO 1. The sawtooth is 7.5 V peak to peak (the block diagram's check point 10). The pulse is a comparator
         * on it (+-13.5 V through R2 100 k / R1 6.8 k: +-0.86 V); it also feeds the ring modulator whatever WAVE FORM says */
        sq1 = ms_pulse(ph1, inc1, pw);
        if (w1 == 0) {                              /* the triangle: the rectifier, then R6 10 k / R7 100 k to +15 V */
            float a = (float)(int32_t)(ph1 - 0x80000000u) * (3.75f / 2147483648.f), q;
            uint32_t k;
            a = a < 0.f ? -a : a;
            q = a * (64.f / 3.84f);
            k = (uint32_t)q;
            q -= (float)k;
            o1 = 1.364f - 0.909f * (MS20_TRI[k] + (MS20_TRI[k + 1u] - MS20_TRI[k]) * q);
        } else if (w1 == 1) {
            o1 = ms_saw(ph1, inc1) * 3.75f;
        } else if (w1 == 2) {
            o1 = sq1 * 0.86f;
        } else {
            o1 = white;
        }
        /* VCO 2: sawtooth; square and a pulse of 14 % from one comparator (its threshold 0 V, or 2.7 V from R70 100 k /
         * R69 22 k); RING: a 4011 as an exclusive-or of VCO 1's pulse and VCO 2's square, 0 / 15 V through R65 47 k
         * onto R67 4.7 k with R68 100 k to -15 V: +0.69 / -0.62 V */
        if (w2 == 0) o2 = ms_saw(ph2, inc2) * 3.75f;
        else if (w2 == 1) o2 = ms_pulse(ph2, inc2, 0x80000000u) * 0.86f;
        else if (w2 == 2) o2 = ms_pulse(ph2, inc2, 0xDC560000u) * 0.86f;
        else o2 = 0.0385f - 0.6535f * sq1 * ms_pulse(ph2, inc2, 0x80000000u);
        s->sv[SRC_VCO1] = o1;
        s->sv[SRC_VCO2] = o2;

        /* The mixer (IC8, inverting, 100 k all round; EXT SIGNAL IN is its third input), then R100 20 k / R104 47 down
         * to millivolts into the high-pass; its output through R108 2.2 k / R109 47 into the low-pass. The 33 uF after
         * each amplifier (C14, C32) takes the DC out. A little noise, as there is: it is what a filter starts singing from. */
        x = ms_clamp(-(o1 * lv1 + o2 * lv2 + MS_IN(DST_EXT_IN, 0.f)), -13.5f, 13.5f) + white * 2e-4f;
        /* (the cutoffs are worked out for every other sample, the high-pass's on one and the low-pass's on the next:
         * 44.1 kHz is fast enough for an oscillator patched into CUTOFF FREQ, and it is most of what a sample costs) */
        if (i & 1u) {
            f = hp_base + v_tb * a_hp_mg + MS_IN(DST_HP_CUT, e2f) * a_hp_eg;
            g = 500.f * ms_exp2(ms_clamp(f, -8.f, 8.f));
            g = g / (1.f + g * (1.f / 75e3f));      /* (the chip runs out of current at the top: 15.3 kHz at 10) */
            s->hp_t = ms_tan(ms_clamp(g * (3.14159265f / MS_FS), 0.0002f, 1.2f)) * MS20_K35_P;
        } else {
            f = lp_base + v_tb * a_lp_mg + MS_IN(DST_LP_CUT, e2f) * a_lp_eg;
            g = 500.f * ms_exp2(ms_clamp(f, -8.f, 8.f));
            g = g / (1.f + g * (1.f / 75e3f));
            s->lp_t = ms_tan(ms_clamp(g * (3.14159265f / MS_FS), 0.0002f, 1.2f)) * MS20_K35_P;
        }
        y = ms20_k35(&s->hp, x * (47.f / 20047.f), s->hp_t, s->hp_t * (1.f / MS20_K35_R), be_hp, 1);
        s->hp_dc += (y - s->hp_dc) * 0.00019f;
        y -= s->hp_dc;
        if (s->scope_src[0] == MS_SCOPE_HP) sc = y;
        if (s->scope_src[1] == MS_SCOPE_HP) sc2 = y;
        y = ms20_k35(&s->lp, y * (47.f / 2247.f), s->lp_t, s->lp_t * (1.f / MS20_K35_R), be_lp, 0);
        s->lp_dc += (y - s->lp_dc) * 0.0001f;
        y -= s->lp_dc;
        if (s->scope_src[0] && ++s->scope_n >= s->scope_div) {   /* the screen's scope */
            uint32_t a = s->scope_src[0], b = s->scope_src[1], w = s->scope_w & (MS_SCOPE_N - 1u);
            sc = a == MS_SCOPE_LP ? y : a == MS_SCOPE_MIX ? x : a < MS_NSRC ? s->sv[a] : sc;
            sc2 = b == MS_SCOPE_LP ? y : b == MS_SCOPE_MIX ? x : b && b < MS_NSRC ? s->sv[b] : sc2;
            s->scope[0][w] = (int16_t)ms_clamp(sc * 1000.f, -32000.f, 32000.f);
            s->scope[1][w] = (int16_t)ms_clamp(sc2 * 1000.f, -32000.f, 32000.f);
            s->scope_w++;
            s->scope_n = 0;
        }

        /* The VCA: R123 6.2 k / R121 220 put 3.4 % of the low-pass's output on Q19, a transistor with no collector
         * supply: its base current (EG2 through R125 100 k and R124 1 M, or INITIAL GAIN through D22 if that is more)
         * makes it a conductance, I = bR Ib (1 - exp(-v / VT)): a straight line only for the smallest signals. Then a
         * current-to-voltage amplifier, R118 180 k with C34 100 p across it (8.8 kHz), C33 1 uF into R119 1 k and VOLUME. */
        vx = s->e2 * 0.8333f + 0.04f;
        if (nets.plug[DST_GAIN]) { v = s->jv[DST_GAIN] - 0.5f; if (v > vx) vx = v; }
        if (i & 1u) {
            uint32_t k;
            vx = ms_clamp(vx * 25.f, 0.f, 127.99f);
            k = (uint32_t)vx;
            s->vca_u = ms_exp2(MS20_VCA[k] + (MS20_VCA[k + 1u] - MS20_VCA[k]) * (vx - (float)k));
        }
        u = s->vca_u;
        v = ms_clamp(y * (220.f / 6420.f) - 212.7f * s->vca_i - s->vca_c, -0.06f, 0.2f);
        s->vca_i = u * 5e-6f * (1.f - ms_exp2(v * (-1.442695f / 0.02585f)));     /* bR = 5 */
        s->vca_c += s->vca_i * 1.134f;              /* C35 10 uF between the transistor and the amplifier, charged by that current */
        y = -180e3f * s->vca_i;
        s->out_lp += (y - s->out_lp) * 0.4674f;
        y = s->out_lp;
        s->out_dc += (y - s->out_dc) * 0.001032f;   /* 14.5 Hz */
        y = ms_clamp(y - s->out_dc, -13.5f, 13.5f) * vol;
        s->sv[SRC_SIGNAL] = y;
        s->sv[SRC_PHONES] = ms_clamp(y * -5.74f, -13.5f, 13.5f);    /* the headphone amplifier: R41 27 k / R42 4.7 k, inverting */

        /* two samples in, one out: the half-band filter over the last 23 (kept twice, so they lie in a row) */
        {
            uint32_t k = s->dec_i;
            s->dec[k] = s->dec[k + 32u] = y * (1.f / MS_VFS);
            s->dec_i = k = (k + 1u) & 31u;
            if (i & 1u) {
                const float *d = s->dec + k + 9u;   /* d[0] .. d[22]: the oldest .. the newest */
                out[i >> 1] = 0.5f * d[11] + HB[0] * (d[10] + d[12]) + HB[1] * (d[8] + d[14]) + HB[2] * (d[6] + d[16])
                            + HB[3] * (d[4] + d[18]) + HB[4] * (d[2] + d[20]) + HB[5] * (d[0] + d[22]);
            }
        }
    }
    /* the panel's lamps (ms20_link.h): the MG's, the trigger's, the ESP's TRIG and PEAK */
    s->lamp = (uint8_t)((s->mg_up ? 0u : 1u) | (s->gate2 ? 2u : 0u) | (esp_on && s->esp_gate ? 4u : 0u) | (esp_on && s->esp_pk > 0.f ? 8u : 0u));
    if (!(s->out_lp - s->out_lp == 0.f)) {         /* not a number, whatever made it: the signal path starts again */
        ms20_k35_t z = {0.f, 0.f, 0.f, 0.f, 0.f};
        s->hp = s->lp = z;
        s->hp_dc = s->lp_dc = s->vca_i = s->vca_c = s->out_lp = s->out_dc = s->pk = s->sh = s->mvca = 0.f;
        s->esp_c = s->esp_h1 = s->esp_h2 = s->esp_env = s->esp_f1 = s->esp_f2 = s->esp_t = 0.f;
        for (i = 0; i < 4u; i++) s->esp_l[i] = 0.f;
        for (i = 0; i < MS_NSRC; i++) s->sv[i] = 0.f;
        for (i = 0; i < 64u; i++) s->dec[i] = 0.f;
    }
    s->ph1 = ph1;
    s->ph2 = ph2;
}
#endif
