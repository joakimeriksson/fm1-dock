/* fm1-dock: ARTIHIVE's firmware/src/artihive.c, split by modules/artihive/take.py: do not edit */
/* SPDX-License-Identifier: GPL-3.0-only */
/* ARTIHIVE - an independent experimental eight-voice drone synthesizer for the M-VAVE FM-1.
 * Integer DSP for the JieLi AC791N: four interacting pairs, dual LFO, cross-feedback delay
 * and output distortion.
 *
 * Built on Dinghy (zvenson), whose platform is Felucca (Leo Kuroshita / Huegelton Instruments)
 * as SLOOP (isod89) carries it. platform.c and everything under it are theirs, unchanged.
 *
 *   the 8 lower white keys   the eight sensors: touch a voice, it sounds at its own TUNE
 *   the 8 upper white keys   latch the same eight voices (drones)
 *   KNOB 1..4                the four parameters of the page on screen
 *   SELECT / ALGORITHM       the voice (1..8) / the pair (1..4) being edited
 *   PRESETS / SAVE held      preset / write it to a user slot
 *   HOME EDIT ENV LFO FX GLO the six pages
 *   OCT- OCT+                transpose; PLAY every drone; REC latch mode; ARP mutate
 *
 * The chip's rules (docs/RULES.md): integers only, no float, no 64-bit division, no constant that
 * looks like a ROM address. Every register access stays in hal/.
 */
static void app_idle(void);
#define LCD_IDLE app_idle
#define APP_NAME "ARTIHIVE"
#define CV_MAX (240u * 88u)   /* covers the largest themed UI canvas with transfer headroom */
#include "platform.c"
#include "artihive_dsp.h"

#define NV 8u
#define NP 4u
#define NFACT 16u
#define NUSER 8u

/* ---------------------------------------------------------------- the patch */
typedef struct {
    char name[10];
    uint16_t tune[NV];      /* Q8 semitones: note * 256 + fine */
    uint8_t sharp[NP];      /* 0..127 triangle -> square, per pair */
    uint8_t mod[NP];        /* 0..127 modulation depth, per pair */
    uint8_t src[NP];        /* 0 off, 1 FM from another pair, 2 LFO / total feedback */
    uint8_t fast[NP];       /* 0 slow envelope, 1 fast */
    uint8_t rel[NP];        /* 0..127 release within fast / slow (panel value, NOT the Q24 coefficient) */
    uint8_t hold[2];        /* 0..127 drone floor, per group of four voices */
    int8_t pitch[2];        /* -24..24 semitones, per group */
    uint8_t lfo_a, lfo_b;   /* 0..127 rate of the two LFO operators */
    uint8_t lfo_mode;       /* 0 OR (sum), 1 AND (product) */
    uint8_t lfo_link;       /* 0..127 A frequency-modulates B */
    uint8_t vib;            /* 0..127 vibrato depth */
    uint8_t dly_time[2];    /* 0..127 */
    uint8_t dly_mod[2];     /* 0..127 depth of the time modulation */
    uint8_t dly_src;        /* 0 LFO, 1 the delay's own output (SELF) */
    uint8_t dly_wave;       /* 0 triangle sum, 1 the AND square */
    uint8_t dly_fb, dly_mix;
    uint8_t drive, drive_mix;
    uint8_t total_fb;       /* 0 the LFO feeds the voices, 1 the output does */
    uint8_t structure;      /* 0 two closed loops, 1 one loop through all four pairs */
    uint8_t drift;          /* 0..127 slow independent pitch drift per voice */
    uint8_t press;          /* 1..127 how hard the panel keys touch */
    uint8_t atk;            /* 0..127 attack time */
    uint8_t space_width;    /* 0..127 stereo rotation depth */
    uint8_t space_src;      /* 0 A, 1 B, 2 combined OUT */
    uint8_t space_dir;      /* 0 clockwise, 1 counter-clockwise */
    uint8_t space_mode;     /* 0 ALL, 1 PAIR, 2 ALT */
    uint8_t arp_rate;       /* low byte of exact integer BPM */
    uint8_t arp_steps;      /* 1..8 voices */
    uint8_t arp_gate;       /* 0..127 -> 1..100% of a step */
    uint8_t arp_order;      /* eight traversal modes plus USER */
    uint8_t arp_pattern[8]; /* USER mode: one bit per voice, up to eight voices per step */
    uint8_t comp_thresh, comp_ratio, comp_attack, comp_release;
    uint8_t arp_rate_hi;    /* 1.8.2: high byte, giving 10..400 BPM */
    uint8_t env_fast[NV], env_rel[NV], env_hold[NP], env_atk[NV];
    uint8_t env_layout;     /* 1: per-voice FAST/REL/ATK and per-pair HOLD */
} patch_t;

/* The user slots are stored as raw patch_t bytes. 1.1 accidentally widened rel[] to
 * uint32_t and moved the blob from 560 to 672 bytes, which silently invalidated every
 * saved slot. Keep the on-flash layout pinned so this cannot happen again. */
typedef char patch_layout_check[(sizeof(patch_t) == 120u) ? 1 : -1];
#define PATCH_V13_SIZE 70u
#define PATCH_V15_SIZE 74u
#define PATCH_V16_SIZE 78u
#define PATCH_V17_SIZE 86u
#define PATCH_V181_SIZE 90u
#define PATCH_V182_SIZE 92u
static uint32_t patch_bpm(const patch_t *q){return (uint32_t)q->arp_rate|((uint32_t)q->arp_rate_hi<<8);}
static void patch_set_bpm(patch_t *q,uint32_t bpm){q->arp_rate=(uint8_t)bpm;q->arp_rate_hi=(uint8_t)(bpm>>8);}

#define SET_MAGIC_V13 0x4C563801u
#define SET_MAGIC_V15 0x41523134u
#define SET_MAGIC_V16 0x41523136u

/* 1.4 appends SPACE controls. The first 70 bytes stay byte-for-byte identical to 1.3.3,
 * so old settings and all eight user slots can be widened without changing their sound. */
static void patch_from_old(patch_t *dst, const uint8_t *src, uint32_t n)
{
    uint32_t i;
    uint8_t *d = (uint8_t *)dst;
    for (i = 0; i < sizeof *dst; i++) d[i] = 0;
    for (i = 0; i < n; i++) d[i] = src[i];
    dst->arp_rate = 120u; dst->arp_steps = 8u; dst->arp_gate = 64u; dst->arp_order = 0u;
    for (i = 0; i < 8u; i++) dst->arp_pattern[i] = (uint8_t)(1u << i);
}
static void env_defaults(patch_t *q){uint32_t v,k;if(q->env_layout==1u)return;for(v=0;v<NV;v++){k=v>>1;q->env_fast[v]=q->fast[k];q->env_rel[v]=q->rel[k];q->env_atk[v]=q->atk;}for(k=0;k<NP;k++)q->env_hold[k]=q->hold[k>>1];q->env_layout=1u;}
static void patch_from_v182(patch_t *dst,const uint8_t *src){patch_from_old(dst,src,PATCH_V182_SIZE);env_defaults(dst);}
static void patch_from_v181(patch_t *dst,const uint8_t *src){patch_from_old(dst,src,PATCH_V181_SIZE);}
static void patch_from_v17(patch_t *dst,const uint8_t *src){patch_from_old(dst,src,PATCH_V17_SIZE);dst->comp_thresh=80u;dst->comp_attack=32u;dst->comp_release=72u;}
static void patch_from_v13(patch_t *dst, const uint8_t *src) { patch_from_old(dst, src, PATCH_V13_SIZE); }
static void patch_from_v15(patch_t *dst, const uint8_t *src) { patch_from_old(dst, src, PATCH_V15_SIZE); }
static void comp_defaults(patch_t *q) { if (!(q->comp_thresh|q->comp_ratio|q->comp_attack|q->comp_release)) { q->comp_thresh=80u; q->comp_attack=32u; q->comp_release=72u; } }
static void patch_from_v16(patch_t *dst, const uint8_t *src)
{
    uint32_t i; uint8_t *d=(uint8_t *)dst;
    for(i=0;i<sizeof *dst;i++)d[i]=0;
    for(i=0;i<PATCH_V16_SIZE;i++)d[i]=src[i];
    for(i=0;i<8u;i++)dst->arp_pattern[i]=(uint8_t)(1u<<i);
}

static const patch_t FACTORY[NFACT] = {
    {"ORGAN",     {12288,14080,15360,16384,17152,18432,19456,20224},
     {18,18,18,18}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {64,64,64,64}, {0,0}, {0,0},
     40,25,0,0,8, {75,86}, {0,0}, 0,0, 30,40, 6,0, 0,0, 10,100,40},
    {"DRONE MASS",{8448,8538,11520,11660,13312,13382,14592,16384},
     {70,84,60,96}, {70,58,80,64}, {1,1,1,1}, {0,0,0,0}, {90,90,90,90}, {96,88}, {0,0},
     22,17,0,30,14, {89,100}, {40,52}, 0,0, 96,64, 40,50, 1,1, 26,100,70},
    {"FM BELLS",  {15360,20224,16384,21248,17152,22016,18432,23296},
     {30,44,20,52}, {96,110,88,104}, {1,1,1,1}, {1,1,1,1}, {40,52,36,48}, {0,0}, {0,0},
     50,31,1,20,0, {60,74}, {24,18}, 1,1, 78,70, 20,30, 0,0, 8,110,20},
    {"SLOW TIDE", {9216,11008,12288,14080,15360,15872,17152,18944},
     {40,52,64,44}, {44,60,38,56}, {2,1,2,1}, {0,0,0,0}, {110,120,100,116}, {70,64}, {0,0},
     12,9,0,44,26, {103,111}, {60,72}, 0,0, 104,80, 24,40, 0,1, 40,90,110},
    {"ACID SWARM",{10240,10496,13312,13568,16384,16640,19456,19712},
     {110,96,120,104}, {112,124,100,118}, {1,1,1,1}, {1,0,1,0}, {20,70,26,64}, {40,30}, {0,0},
     70,96,1,60,10, {51,63}, {70,90}, 1,1, 110,96, 70,80, 1,1, 60,120,10},
    {"GHOST PAIR",{13312,13352,16384,16444,18176,18206,21248,21328},
     {56,24,72,36}, {88,40,96,52}, {1,2,1,2}, {0,0,0,0}, {100,80,110,90}, {54,40}, {0,0},
     16,23,1,52,30, {85,95}, {52,64}, 1,0, 88,74, 16,34, 0,1, 44,90,90},
    {"SUB MASS",  {7168,7228,9216,9306,10240,11008,12288,14080},
     {84,60,40,70}, {60,84,50,72}, {1,1,2,1}, {0,0,0,0}, {80,100,90,86}, {110,70}, {-12,0},
     10,14,0,20,6, {95,106}, {30,44}, 0,0, 90,56, 54,60, 1,0, 18,100,80},
    {"SHIMMER",   {18432,20224,21504,22528,23296,24576,25600,26368},
     {20,34,16,44}, {50,64,40,70}, {2,1,2,1}, {0,0,0,0}, {120,110,124,116}, {40,30}, {0,0},
     30,47,1,36,40, {68,82}, {44,56}, 0,0, 84,90, 10,20, 0,1, 50,80,100},
    {"MACHINE",   {11520,11520,14592,14592,17664,17664,20736,20736},
     {127,127,127,127}, {120,120,120,120}, {1,1,1,1}, {1,1,1,1}, {10,10,10,10}, {80,80}, {0,0},
     90,60,1,0,0, {40,47}, {90,110}, 1,1, 120,110, 90,90, 1,1, 30,127,4},
    {"SEA BED",   {7936,9728,11008,12800,14080,15872,17152,18944},
     {30,44,26,52}, {30,46,24,50}, {2,2,2,2}, {0,0,0,0}, {127,127,127,127}, {86,80}, {0,0},
     7,5,0,10,20, {113,119}, {80,96}, 0,0, 110,86, 30,44, 0,0, 70,80,127},
    {"CHOIR",     {14080,15104,15872,17152,18176,18944,20224,21248},
     {44,50,38,56}, {26,34,20,40}, {2,2,2,2}, {0,0,0,0}, {116,124,110,120}, {60,54}, {0,0},
     24,19,0,14,60, {78,90}, {20,30}, 0,0, 70,60, 14,24, 0,1, 34,90,116},
    {"RUST",      {9728,9984,12544,12800,15616,15872,18688,18944},
     {100,116,90,124}, {100,116,94,120}, {1,1,1,1}, {1,1,0,1}, {30,20,60,26}, {60,50}, {0,0},
     56,83,1,90,16, {56,50}, {100,80}, 1,1, 116,100, 100,86, 1,1, 90,110,16},
    {"PENDULUM",  {12288,15360,14080,17152,15872,18944,17664,20736},
     {36,36,36,36}, {70,70,70,70}, {2,2,2,2}, {1,1,1,1}, {50,50,50,50}, {30,30}, {0,0},
     48,72,1,0,24, {72,88}, {50,50}, 0,1, 80,60, 20,20, 0,1, 20,100,60},
    {"VOID",      {6144,7936,9216,11008,12288,14080,15360,17152},
     {60,70,80,90}, {60,70,80,90}, {1,1,1,1}, {0,0,0,0}, {127,127,127,127}, {100,96}, {0,0},
     5,3,0,8,10, {122,127}, {90,110}, 1,0, 120,90, 44,50, 1,1, 100,70,127},
    {"GLASS",     {19456,21248,22528,24320,25600,27392,28672,30464},
     {10,24,14,30}, {84,96,70,110}, {1,1,1,1}, {1,1,1,1}, {70,60,80,66}, {0,0}, {0,0},
     40,60,1,24,10, {62,76}, {30,40}, 1,1, 74,80, 16,26, 0,0, 12,100,50},
    {"INIT",      {12288,14080,15360,17152,18432,20224,21504,23296},
     {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, {64,64,64,64}, {0,0}, {0,0},
     40,40,0,0,0, {80,92}, {0,0}, 0,0, 40,30, 0,0, 0,0, 0,100,40},
};

static patch_t p;                       /* the live patch, the main loop's */
static patch_t user[NUSER];             /* user slots, kept in blob 0 */
static patch_t undo_patch;              /* one-level mutation undo: 74 bytes */
static uint8_t undo_valid;

/* ---------------------------------------------------------------- the audio side */
/* The ISR reads only these (and its own state). The main loop writes them; a byte or a 32-bit
 * store is atomic here, so no lock is needed. Pitch is written before the gate that opens it. */
static volatile uint32_t a_inc[NV];
static volatile int32_t a_gate[NV];
static volatile uint8_t a_sharp[NP], a_mod[NP], a_src[NP];
static volatile uint32_t a_rel[NV];
static volatile int32_t a_hold[NP];
static volatile uint32_t a_atk[NV];
static volatile uint8_t a_structure, a_total_fb, a_lfo_mode, a_dly_src, a_dly_wave, a_dly_freeze, a_poly;
static volatile uint32_t a_lfo_inc[2];
static volatile int32_t a_link, a_drift, a_vib;
static volatile int32_t a_space_width;
static volatile uint8_t a_space_src, a_space_dir, a_space_mode;
static volatile int32_t a_dmod[2];
static volatile uint32_t a_dly_base[2];
static const uint16_t COMP_THRESH[128] = {
    130, 136, 142, 149, 155, 162, 169, 177, 185, 193, 202, 211, 220, 230, 240, 251, 262, 273, 285, 298, 311, 325, 340, 355, 371, 387, 404, 422, 441, 461, 481, 503, 525, 548, 573, 598, 625, 653, 682, 712, 744, 777, 811, 847, 885, 924, 965, 1008, 1053, 1100, 1149, 1200, 1253, 1309, 1367, 1428, 1492, 1558, 1627, 1700, 1775, 1854, 1937, 2023, 2113, 2207, 2305, 2408, 2515, 2626, 2743, 2865, 2993, 3126, 3265, 3410, 3562, 3720, 3886, 4058, 4239, 4427, 4624, 4830, 5045, 5269, 5503, 5748, 6004, 6271, 6550, 6841, 7145, 7463, 7795, 8142, 8504, 8882, 9277, 9690, 10120, 10571, 11041, 11532, 12045, 12580, 13140, 13724, 14334, 14972, 15638, 16333, 17060, 17818, 18611, 19439, 20303, 21206, 22149, 23134, 24163, 25238, 26360, 27533, 28757, 30036, 31372, 32767
};

static volatile int32_t a_fb, a_wet, a_drive_gain, a_drive_mix;
static volatile int32_t a_comp_thresh, a_comp_ratio;
static volatile uint32_t a_comp_attack, a_comp_release;
static volatile uint32_t a_mute = 1;            /* no startup burst while state and delay settle */
static volatile uint32_t a_started;             /* HOLD is silent until the first PLAY */
static volatile int32_t mon_out, mon_lfo, mon_space[NP];
static volatile int32_t mon_comp_in, mon_comp_out, mon_comp_gr;
static volatile uint32_t mon_cpu;             /* smoothed audio ISR load, Q8 percent */
static volatile int16_t scope_wave[128];       /* recent mono output for the VOICE oscilloscope */
static volatile uint32_t scope_wr;

static uint32_t v_ph[NV], v_prevph[NV], v_drift[NV], v_vibph[NV];
static int32_t v_env[NV], v_lp[NV], v_int[NV], v_driftval[NV];
static int32_t v_dc[NV];          /* per-voice DC block: the triangle integrator must not feed FM a bias */   /* v_env is Q31: long tails need sub-count steps */
static uint32_t lfo_ph[2];
static int32_t lfo_tri, lfo_and, fb_prev, pair_prev[NP];
static int32_t comp_peak, comp_gain = 32767;
static uint32_t rs32 = 0x13579BDFu;

/* 1.1: two 2.177 s lines in packed 12-bit PCM. 288000 B total; a 16-bit
 * pair of the same length would need 384000 B and would not fit the pool. */
#define DLEN DFRAMES
static uint8_t dbuf[2][DPACKED] __attribute__((aligned(4)));
static uint32_t dwr;
static int32_t dlp[2], d_dc[2], m_dc;
static int32_t out_dc[2];        /* the last word on DC: one filter a channel */

static int32_t tri(uint32_t ph)         /* a full cycle over 2^32 -> -32768..32764 */
{
    uint32_t q = ph >> 17;
    int32_t t = (ph & 0x80000000u) ? (int32_t)(32767u - q) : (int32_t)q;
    return (t << 2) - 32766;
}

static int32_t shape(int32_t x, int32_t g)   /* triangle -> square: gain into a cubic knee, g Q12 */
{
    int32_t u = (x * g) >> 12, c;
    if (u > 32767) u = 32767;
    else if (u < -32767) u = -32767;
    c = (((u * u) >> 15) * u) >> 15;
    return (u * 3 - c) >> 1;
}

/* soft knee, unity gain for small signals: x - x^3/(3*L^2) with L = 49152, so it
 * lands on full scale exactly. A limiter that amplifies would make the delay loop
 * self-oscillate far too early, so the gain at zero stays 1. */
static int32_t sat(int32_t x)
{
    int32_t a = x < 0 ? -x : x, t, u, c;
    if (a >= 49152) return x < 0 ? -32767 : 32767;
    t = ((x >> 4) * (x >> 4)) >> 8;             /* x^2 >> 16 */
    u = (t * (x >> 4)) >> 12;                   /* x^3 >> 32 */
    c = (u * 19416) >> 15;                      /* / (3 * L^2) */
    return x - c;
}

/* the FM structure switch: which pair modulates this one */
static uint32_t fm_from(uint32_t pair, uint32_t structure)
{
    static const uint8_t TWO[NP] = {1, 0, 3, 2};        /* 12<->34 and 56<->78 */
    static const uint8_t ONE[NP] = {3, 0, 1, 2};        /* 78>12>34>56>78 */
    return structure ? ONE[pair] : TWO[pair];
}

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i, v, k, fmp[NP], src[NP];
    int32_t morph[NP], depth[NP], hold[NV], space_pan[NP], peak = 0;
    uint32_t inc[NV];
#ifndef DINGHY_HOST
    uint32_t t0 = fm1_ticks();
#endif
    uint32_t rel[NV];                            /* Q24 decay coefficients: 8 bits would truncate the long tails */
    { /* Stereo-linked compressor: one division per block, never per sample. */
        int32_t target = 32767, pk = comp_peak, th = a_comp_thresh;
        if (a_comp_ratio && pk > th) {
            int32_t reduced = th + ((pk - th) * (127 - a_comp_ratio)) / 127;
            target = (int32_t)(((int64_t)reduced * 32767) / pk);
        }
        if (target < comp_gain) comp_gain += (int32_t)(((int64_t)(target - comp_gain) * a_comp_attack) >> 15);
        else comp_gain += (int32_t)(((int64_t)(target - comp_gain) * a_comp_release) >> 15);
        if (comp_gain < 512) comp_gain = 512;
        if (comp_gain > 32767) comp_gain = 32767;
        mon_comp_in = pk; mon_comp_out = (pk * comp_gain) >> 15;
        comp_peak = 0; mon_comp_gr = 32767 - comp_gain;
    }

    /* ---- once a block: rates, drift, vibrato, the LFO */
    for (k = 0; k < NP; k++) {
        morph[k] = ((int32_t)a_sharp[k] * 32768 + 63) / 127;   /* 0..32768, Q15 */
        depth[k] = (int32_t)a_mod[k] << 8;
        src[k] = a_src[k];
        fmp[k] = fm_from(k, a_structure);
    }
    for(v=0;v<NV;v++)rel[v]=a_rel[v];
    {
        uint32_t ia = a_lfo_inc[0], ib = a_lfo_inc[1];
        int32_t ta, tb, sa, sb, lfo;
        lfo_ph[0] += ia * n;
        ta = tri(lfo_ph[0]);
        sa = (lfo_ph[0] & 0x80000000u) ? 32767 : -32767;
        ib += (uint32_t)(((int64_t)ib * ((ta * a_link) >> 15)) >> 17);   /* LINK: A bends B */
        lfo_ph[1] += ib * n;
        tb = tri(lfo_ph[1]);
        sb = (lfo_ph[1] & 0x80000000u) ? 32767 : -32767;
        lfo_tri = (ta + tb) >> 1;
        lfo_and = (sa > 0 && sb > 0) ? 32767 : -32767;
        lfo = a_lfo_mode ? ((sa * sb) >> 15) : ((sa + sb) >> 1);
        mon_lfo = lfo;
        for (v = 0; v < NV; v++) {
            int32_t vib;
            uint32_t base = a_inc[v];
            v_drift[v] += 590000u + v * 71000u;
            v_driftval[v] += ((tri(v_drift[v]) >> 5) - v_driftval[v]) >> 3;
            vib = (v_driftval[v] * a_drift) >> 12;
            if (a_vib) {
                v_vibph[v] += (700000u + v * 90000u) * n;
                vib += ((tri(v_vibph[v]) >> 8) * a_vib) >> 9;
            }
            base = (uint32_t)(((int64_t)base * (65536 + vib)) >> 16);
            if (src[v >> 1] == 2u && !a_total_fb) {         /* the LFO as this pair's CV */
                int32_t d = (lfo * depth[v >> 1]) >> 15;
                base = (uint32_t)(((int64_t)base * (65536 + (d >> 1))) >> 16);
            }
            inc[v] = base;
            hold[v] = (a_started && !a_poly) ? a_hold[v >> 1] << 16 : 0; /* boot is silent; PLAY starts the preset */
        }
        /* One coefficient per pair and audio block is enough at LFO rates. PAIR uses
         * quarter-cycle offsets; ALT sends alternating pairs in opposite directions. */
        for (k = 0; k < NP; k++) {
            uint32_t off = a_space_mode == 1u ? k * 0x40000000u : 0u;
            int32_t r;
            if (a_space_src == 0u) r = tri(lfo_ph[0] + off);
            else if (a_space_src == 1u) r = tri(lfo_ph[1] + off);
            else {
                int32_t xa = ((lfo_ph[0] + off) & 0x80000000u) ? 32767 : -32767;
                int32_t xb = ((lfo_ph[1] + off) & 0x80000000u) ? 32767 : -32767;
                r = a_lfo_mode ? ((xa * xb) >> 15) : ((xa + xb) >> 1);
            }
            if (a_space_mode == 2u && (k & 1u)) r = -r;
            if (a_space_dir) r = -r;
            space_pan[k] = (r * a_space_width) >> 15;
            mon_space[k] = space_pan[k];
        }
    }

    for (i = 0; i < n; i++) {
        int32_t mix = 0, rot_side = 0, s;
        for (k = 0; k < NP; k++) {
            int32_t ps = 0;
            uint32_t fm = 0;
            if (src[k] == 1u)
                fm = ((uint32_t)(int32_t)(((int64_t)pair_prev[fmp[k]] * depth[k]) >> 15)) << 16;
            else if (src[k] == 2u && a_total_fb)
                fm = ((uint32_t)(int32_t)(((int64_t)fb_prev * depth[k]) >> 15)) << 14;
            for (v = 0; v < 2u; v++) {
                uint32_t id = k * 2u + v;
                int32_t e = v_env[id], g = a_gate[id] << 16, lvl, y;
                if (e < g) {
                    e += (int32_t)a_atk[id] << 16;
                    if (e > g) e = g;
                } else if (e > g) {
                    int32_t d = e - g;
                    int32_t step = (int32_t)(((int64_t)d * rel[id] + 8388608) >> 24);
                    if (step < 1) step = 1;
                    e -= step;
                    if (e < g || e - g <= (32 << 16)) e = g;  /* -60 dB: silence, no infinite tail */
                }
                v_env[id] = e;
                lvl = (e > hold[id] ? e : hold[id]) >> 16;
                v_ph[id] += inc[id];
                {
                    uint32_t ph = v_ph[id] + fm;
                    uint32_t step = ph - v_prevph[id];      /* actual FM/PM rate, not just TUNE */
                    v_prevph[id] = ph;
                    if (!lvl) continue;
                    y = bl_voice(ph, step, (uint32_t)morph[k], &v_int[id]);
                    y = (dc_block(y, &v_dc[id]) * 21845) >> 15;   /* 2/3: Gibbs headroom inside the Q15 bus */
                }
                v_lp[id] += (y - v_lp[id] + 1) >> 1;
                ps += (v_lp[id] * lvl) >> 15;
            }
            pair_prev[k] = ps >> 1;
            mix += ps;
            rot_side += (ps * space_pan[k]) >> 15;
        }
        mix >>= 3;                                  /* eight voices onto one bus, with headroom */

        /* ---- the modulation delay: two lines, cross feedback, time played by a signal */
        {
            int32_t y0, y1, dry = mix, wet, dl;
            int32_t t;
            int32_t m0 = a_dly_src ? dlp[0] : (a_dly_wave ? lfo_and : lfo_tri);
            int32_t m1 = a_dly_src ? dlp[1] : (a_dly_wave ? lfo_and : lfo_tri);
            {
                int32_t dm = ((m0 * a_dmod[0]) >> 15) * 256;
                t = (int32_t)a_dly_base[0] + dm;
            }
            if (t < (1 << 8)) t = 1 << 8;
            if (t > ((int32_t)(DLEN - 4u) << 8)) t = (int32_t)(DLEN - 4u) << 8;
            y0 = d12_tap(dbuf[0], dwr, t);
            {
                int32_t dm = ((m1 * a_dmod[1]) >> 15) * 256;
                t = (int32_t)a_dly_base[1] + dm;
            }
            if (t < (1 << 8)) t = 1 << 8;
            if (t > ((int32_t)(DLEN - 4u) << 8)) t = (int32_t)(DLEN - 4u) << 8;
            y1 = d12_tap(dbuf[1], dwr, t);
            dl = sat(dry + ((a_fb * y1) >> 15));
            dlp[0] += (dl - dlp[0] + 2) >> 2;           /* the feedback loop is deliberately dark and lo-fi */
            dl = sat(dry + ((a_fb * y0) >> 15));
            dlp[1] += (dl - dlp[1] + 2) >> 2;
            /* FREEZE keeps the read head moving around the existing 2.177 s rings but
             * stops both writes. The captured material therefore loops without allocating
             * another buffer or changing the delay geometry. */
            if (!a_dly_freeze) {
                d12_write(dbuf[0], dwr, dc_block(dlp[0], &d_dc[0]));
                d12_write(dbuf[1], dwr, dc_block(dlp[1], &d_dc[1]));
            }
            if (++dwr == DLEN) dwr = 0;
            wet = (y0 + y1) >> 1;
            mix = dry + (((wet - dry) * a_wet) >> 15);

            /* ---- distortion, last: it also drives the total feedback */
            {
                int32_t p32 = mix > 32767 ? 32767 : mix < -32767 ? -32767 : mix;
                int32_t d = shape(p32, a_drive_gain);
                mix = p32 + (((d - p32) * a_drive_mix) >> 15);
                fb_prev = mix;
            }
            {
                int32_t side;
                mix = sat(dc_block(fb_prev, &m_dc));
                if (a_mute) mix = 0;
                int32_t ch, lr[2];
                side = ((y0 - y1) * a_wet) >> 17;   /* the two lines lean left and right */
                side += rot_side >> 3;               /* pair rotation, same eight-voice headroom as mix */
                if (side > 12000) side = 12000;
                if (side < -12000) side = -12000;
                lr[0] = mix + side;
                lr[1] = mix - side;
                { int32_t al=lr[0]<0?-lr[0]:lr[0], ar=lr[1]<0?-lr[1]:lr[1], pk=al>ar?al:ar;
                  if(pk>32767)pk=32767;
                  if(pk>comp_peak)comp_peak=pk;
                  if(a_comp_ratio){lr[0]=(int32_t)(((int64_t)lr[0]*comp_gain)>>15);lr[1]=(int32_t)(((int64_t)lr[1]*comp_gain)>>15);} }
                for (ch = 0; ch < 2; ch++) {        /* after the width, so no rounding leaves a DC offset */
                    if (a_mute) out_dc[ch] = 0;
                    else out[2u * i + (uint32_t)ch] += sat(dc_block(lr[ch], &out_dc[ch]));
                }
                if ((i & 3u) == 0u) { scope_wave[scope_wr & 127u] = (int16_t)mix; scope_wr++; }
                s = mix < 0 ? -mix : mix;
                if (s > peak) peak = s;
            }
        }
    }
    mon_out = peak;
#ifndef DINGHY_HOST
    {
        uint32_t us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
        uint32_t pct = (uint32_t)(((uint64_t)us * 44100u * 100u + (uint64_t)n * 500000u) /
                                  ((uint64_t)n * 1000000u));
        int32_t target;
        if (pct > 100u) pct = 100u;
        target = (int32_t)(pct << 8);
        mon_cpu = (uint32_t)((int32_t)mon_cpu + (target - (int32_t)mon_cpu) / 8);
    }
#endif
}

/* ---------------------------------------------------------------- pitch */
static const uint32_t INC_C9[12] = {    /* MIDI 120..131; lower octaves shift right */
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};

static uint32_t inc_of(int32_t q8)
{
    uint32_t note, fine, base;
    if (q8 < 0) q8 = 0;
    if (q8 > 32767) q8 = 32767;
    note = (uint32_t)q8 >> 8;
    fine = (uint32_t)q8 & 255u;
    base = INC_C9[note % 12u] >> (10u - note / 12u);
    return (uint32_t)(((uint64_t)base * (65536u + fine * 15u)) >> 16);
}

/* ---------------------------------------------------------------- the panel */
static const uint8_t WKEY[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
enum { PG_VOICE, PG_FM, PG_ENV, PG_LFO, PG_DLY, PG_ARP, PG_OUT, NPAGE };
static const char *const PGNAME[NPAGE] = {"DRONE", "FM ROUTE", "ENVELOPE", "HYPER LFO", "MOD DEL", "ARPEGGIO", "OUTPUT"};

static struct {
    uint8_t page, voice, pair, preset, uslot, ch, latch, pal, fxsub, lfosub, outsub, polysub;
} ui;

static uint32_t touch_mask, latch_mask, idle_ok, dirty, t_change, msg_ms;
static uint32_t arp_mask, arp_next_ms, arp_off_ms, arp_fraction;
static uint8_t arp_running, arp_step, arp_direction, arp_display_step, arp_event_count;
static uint8_t arp_learn_step, arp_learn_mask, arp_learn_active;
static uint8_t vvel[NV];
static int8_t poly_key_voice[27], poly_voice_key[NV];
static uint32_t poly_age[NV], poly_clock;
static uint8_t poly_root, poly_octave=3u, poly_detune;
static int8_t ui_transpose;
static char msg[24];

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static uint8_t clamp8(int32_t v) { return (uint8_t)clampi(v, 0, 127); }
static void gates(void);
static void poly_set_pitch(uint32_t v,uint32_t key){int32_t note=12*((int32_t)poly_octave+1)+(int32_t)poly_root+(int32_t)key;int32_t spread=((int32_t)v*2-7)*(int32_t)poly_detune/8;a_inc[v]=inc_of(note*256+spread);}
static void poly_reset(void){uint32_t i;touch_mask=latch_mask=arp_mask=0;for(i=0;i<27u;i++)poly_key_voice[i]=-1;for(i=0;i<NV;i++){poly_voice_key[i]=-1;vvel[i]=0;}gates();}
static void poly_retune(void){uint32_t v;for(v=0;v<NV;v++)if(poly_voice_key[v]>=0)poly_set_pitch(v,(uint32_t)poly_voice_key[v]);}
/* Held-note editing targets the newest still-held voice; no held note means ALL. */
static int32_t poly_env_target(void){uint32_t v,best=0,age=0;if(!touch_mask)return -1;for(v=0;v<NV;v++)if((touch_mask&(1u<<v))&&poly_age[v]>=age){age=poly_age[v];best=v;}return (int32_t)best;}
static uint32_t poly_alloc(uint32_t key){uint32_t v,best=0;for(v=0;v<NV;v++)if(poly_voice_key[v]<0)return v;for(v=1;v<NV;v++)if(poly_age[v]<poly_age[best])best=v;if(poly_voice_key[best]>=0)poly_key_voice[(uint32_t)poly_voice_key[best]]=-1;touch_mask&=~(1u<<best);(void)key;return best;}

/* push the patch to the ISR */
static void to_audio(void)
{
    uint32_t v, k;
    if (!a_poly) for (v = 0; v < NV; v++) {
        int32_t q8 = (int32_t)p.tune[v] + 256 * ((int32_t)p.pitch[v >> 2] + (int32_t)ui_transpose);
        a_inc[v] = inc_of(q8);
    }
    for (k = 0; k < NP; k++) {
        a_sharp[k] = p.sharp[k];
        a_mod[k] = p.mod[k];
        a_src[k] = p.src[k];
        /* 1.1: true exponential release, Q24 coefficient from a generated table.
         * FAST 54..2.4 ms, SLOW 8000..93 ms to -60 dB; REL stays inverted. */
    }
    for (k=0;k<NV;k++){a_rel[k]=p.env_fast[k]?REL_FAST[p.env_rel[k]]:REL_SLOW[p.env_rel[k]];a_atk[k]=1u+((127u-p.env_atk[k])*(127u-p.env_atk[k]))/48u;}
    for (k=0;k<NP;k++){uint32_t h=p.env_hold[k];a_hold[k]=(int32_t)((h*h*26000u)/(127u*127u));}
    for (k = 0; k < 2u; k++) {
        /* 0.023 .. 2039.4 ms quadratic base; signed modulation clamps safely at one sample */
        a_dly_base[k] = delay_frames(p.dly_time[k]) << 8;
        a_dmod[k] = (int32_t)p.dly_mod[k] * 32;
    }
    a_structure = p.structure;
    a_total_fb = p.total_fb;
    a_lfo_mode = p.lfo_mode;
    a_dly_src = p.dly_src;
    a_dly_wave = p.dly_wave;
    /* 0.02 Hz .. 24 Hz, a musical (exponential-ish) taper */
    a_lfo_inc[0] = 8000u + (uint32_t)p.lfo_a * (uint32_t)p.lfo_a * 130u;
    a_lfo_inc[1] = 8000u + (uint32_t)p.lfo_b * (uint32_t)p.lfo_b * 130u;
    a_link = (int32_t)p.lfo_link << 8;
    a_drift = (int32_t)p.drift * 6;
    a_vib = (int32_t)p.vib * 3;
    a_space_width = (int32_t)p.space_width * 96; /* capped side contribution, Q15-ish */
    a_space_src = p.space_src;
    a_space_dir = p.space_dir;
    a_space_mode = p.space_mode;
    a_fb = (int32_t)p.dly_fb * 300;                 /* past the middle it self-oscillates */
    a_wet = (int32_t)p.dly_mix * 258;
    a_drive_gain = 4096 + (int32_t)p.drive * 190;
    a_drive_mix = (int32_t)p.drive_mix * 258;
    a_comp_thresh = COMP_THRESH[p.comp_thresh];
    a_comp_ratio = p.comp_ratio;
    { uint32_t ms=1u+(127u-p.comp_attack)*99u/127u; a_comp_attack=(32768u*1451u)/(ms*1000u+1451u); }
    { uint32_t ms=20u+(127u-p.comp_release)*1980u/127u; a_comp_release=(32768u*1451u)/(ms*1000u+1451u); }
}

static void gates(void)
{
    uint32_t v;
    for (v = 0; v < NV; v++) {
        uint32_t on = ((touch_mask | latch_mask | arp_mask) >> v) & 1u;
        uint32_t lvl = vvel[v] ? vvel[v] : p.press;
        a_gate[v] = on ? (int32_t)(lvl * 252) : 0;
    }
}

static void load_preset(uint32_t i)
{
    if (i < NFACT) p = FACTORY[i];
    else if (i < NFACT + NUSER) p = user[i - NFACT];
    else return;
    comp_defaults(&p); env_defaults(&p);
    if (!p.arp_steps) { uint32_t j; p.arp_rate = 120u; p.arp_steps = 8u; p.arp_gate = 64u; p.arp_order = 0u; for(j=0;j<8u;j++)p.arp_pattern[j]=(uint8_t)(1u<<j); }
    ui.preset = (uint8_t)i;
    undo_valid = 0;
    latch_mask = 0;
    arp_running = 0; arp_mask = 0; arp_learn_mask = 0; arp_learn_active = 0; arp_learn_step = 0;
    a_dly_freeze = 0;
    to_audio();
    gates();
}

static void say(const char *s)
{
    uint32_t i = 0;
    while (*s && i < sizeof msg - 1u) msg[i++] = *s++;
    msg[i] = 0;
    msg_ms = fm1_ms;
}

/* ---------------------------------------------------------------- MIDI */
static void midi_in(void)
{
    uint8_t st, d1, d2;
    int src;
    while ((src = midi_get(&st, &d1, &d2)) != 0) {
        uint32_t ty = st & 0xF0u;
        (void)src;
        if (st >= 0xF0u) continue;
        if (ui.ch && (st & 15u) != ui.ch - 1u) continue;
        if (ty == 0x90u && d2) {
            uint32_t v = d1 & 7u;                   /* the voice keeps its own TUNE */
            latch_mask |= 1u << v;
            vvel[v] = d2;
            gates();
        } else if (ty == 0x80u || ty == 0x90u) {
            uint32_t v = d1 & 7u;
            latch_mask &= ~(1u << v);
            vvel[v] = 0;
            gates();
        } else if (ty == 0xB0u) {
            uint32_t c = d1, x = d2;
            if (c >= 20u && c <= 23u) { p.mod[c - 20u] = clamp8(x); dirty = 1; }
            else if (c >= 24u && c <= 27u) { p.sharp[c - 24u] = clamp8(x); dirty = 1; }
            else if (c == 28u || c == 29u) { uint32_t g=c-28u; p.env_hold[g*2u]=clamp8(x); p.env_hold[g*2u+1u]=clamp8(x); dirty = 1; }
            else if (c == 30u) { p.lfo_a = clamp8(x); dirty = 1; }
            else if (c == 31u) { p.lfo_b = clamp8(x); dirty = 1; }
            else if (c == 32u) { p.dly_fb = clamp8(x); dirty = 1; }
            else if (c == 33u) { p.dly_mix = clamp8(x); dirty = 1; }
            else if (c == 34u) { p.dly_time[0] = clamp8(x); dirty = 1; }
            else if (c == 35u) { p.dly_time[1] = clamp8(x); dirty = 1; }
            else if (c == 36u) { p.drive = clamp8(x); dirty = 1; }
            else if (c == 37u) { p.drive_mix = clamp8(x); dirty = 1; }
            else if (c == 38u) { p.drift = clamp8(x); dirty = 1; }
            else if (c == 39u) { p.vib = clamp8(x); dirty = 1; }
            else if (c == 1u) { uint32_t k; for (k = 0; k < NP; k++) { p.mod[k] = clamp8(x); } dirty = 1; }
            else if (c == 64u) { latch_mask = x >= 64u ? 255u : 0u; gates(); }
            else if (c == 120u || c == 123u) { latch_mask = 0; gates(); }
            t_change = fm1_ms;
            if (dirty) to_audio();
        } else if (ty == 0xC0u) {
            load_preset(d1 < NFACT + NUSER ? d1 : 0);
        }
    }
}

static void app_idle(void)
{
    if (idle_ok) midi_in();
}

/* ---------------------------------------------------------------- the screen */
/* On the Deep Blue panel the graphics are its white silkscreen. C_ACC is the printing,
 * C_HOT is a plate under a finger, C_OFF is a control that is not being edited. */
#define C_ACC C_HI
#define C_OFF C_GRAY
#define C_HOT C_HI                              /* the strongest printing: white on a dark panel,
                                                 * near-black on a light one. Plain white would
                                                 * vanish on the Porcelain, Rose and Toxic Green panels. */

static void num_text(char *s, int32_t v, uint32_t pad)   /* right-aligned number, no libc printf */
{
    char t[12];
    uint32_t i = 0, j = 0;
    uint32_t u = v < 0 ? (uint32_t)(-v) : (uint32_t)v;
    if (v < 0) t[i++] = '-';
    if (!u) t[i++] = '0';
    else { char d[10]; uint32_t n = 0; while (u && n < 10u) { d[n++] = (char)('0' + u % 10u); u /= 10u; } while (n) t[i++] = d[--n]; }
    while (j + i < pad) s[j++] = ' ';
    { uint32_t k; for (k = 0; k < i; k++) s[j++] = t[k]; }
    s[j] = 0;
}

/* a duration for the panel: "8.00s", "848ms", "2ms" — REL and ATK read as time, not 0..127 */
static void dur_text(char *s, uint32_t ms)
{
    char t[8];
    uint32_t n = 0, i;
    if (ms >= 1000u) {
        uint32_t whole = ms / 1000u, frac = (ms % 1000u) / 10u;
        if (whole >= 10u) t[n++] = (char)('0' + whole / 10u);
        t[n++] = (char)('0' + whole % 10u);
        t[n++] = '.';
        t[n++] = (char)('0' + frac / 10u);
        t[n++] = (char)('0' + frac % 10u);
        t[n++] = 's';
    } else {
        if (ms >= 100u) t[n++] = (char)('0' + ms / 100u);
        if (ms >= 10u) t[n++] = (char)('0' + (ms / 10u) % 10u);
        t[n++] = (char)('0' + ms % 10u);
        t[n++] = 'm'; t[n++] = 's';
    }
    for (i = 0; i < n; i++) s[i] = t[i];
    s[n] = 0;
}

/* HOLD as the share of full scale it never lets the voice fall below */
static uint32_t hold_floor(uint32_t h) { return (h * h * 26000u) / (127u * 127u); }

static void pct_text(char *s, uint32_t hold)
{
    uint32_t pc = hold_floor(hold) * 100u / 32768u, i = 0;
    if (pc >= 10u) s[i++] = (char)('0' + pc / 10u);
    s[i++] = (char)('0' + pc % 10u);
    s[i++] = '%';
    s[i] = 0;
}

static void linear_pct_text(char *s, uint32_t v)
{
    uint32_t pc = (v * 100u + 63u) / 127u, i = 0;
    if (pc >= 100u) s[i++] = '1';
    if (pc >= 10u) s[i++] = (char)('0' + (pc / 10u) % 10u);
    s[i++] = (char)('0' + pc % 10u); s[i++] = '%'; s[i] = 0;
}

static void note_name(char *s, uint32_t q8)
{
    static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    uint32_t note = q8 >> 8, i = 0;
    const char *q = N[note % 12u];
    while (*q) s[i++] = *q++;
    if (note >= 12u) s[i++] = (char)('0' + note / 12u - 1u);
    s[i] = 0;
}

/* Eight compact single-contact sensors. There is no pad behind the metal. On VOICE,
 * the freed upper strip is a real output oscilloscope. Envelope colour grows inside each
 * octagon from its centre towards all eight sides. */
static void draw_voices(void)
{
    uint32_t v;
    cv_begin(240, 68, C_BG);
    cv_oxide(0, 172);
    for (v = 0; v < NV; v++) {
        int32_t x = 3 + (int32_t)v * 30, dy;
        int32_t h = env_bar(v_env[v], a_poly ? 0 : a_hold[v >> 1], 46);
        uint32_t on = ((touch_mask | latch_mask) >> v) & 1u;
        uint32_t sel = v == ui.voice;
        char t[8];
        if (h > 46) h = 46;
        if (h < 0) h = 0;
        /* Metal octagon, directly on the panel: 23 px rim and 19 px body. */
        for (dy = -11; dy <= 11; dy++) {
            int32_t ad = dy < 0 ? -dy : dy, hw = ad <= 4 ? 11 : 15 - ad;
            cv_rect(x + 12 - hw, 35 + dy, hw * 2 + 1, 1, C_METAL_RIM);
        }
        for (dy = -9; dy <= 9; dy++) {
            int32_t ad = dy < 0 ? -dy : dy, hw = ad <= 3 ? 9 : 12 - ad;
            cv_rect(x + 12 - hw, 35 + dy, hw * 2 + 1, 1, on ? C_METAL_HI : C_METAL);
        }
        /* Envelope octagon: radius 1..8, centred and symmetric. */
        if (h) {
            int32_t r = 1 + h * 7 / 46;
            for (dy = -r; dy <= r; dy++) {
                int32_t ad = dy < 0 ? -dy : dy, shoulder = (r * 2 + 2) / 5;
                int32_t hw = ad <= shoulder ? r : r + shoulder - ad;
                if (hw >= 0) cv_rect(x + 12 - hw, 35 + dy, hw * 2 + 1, 1, C_GRAPH_HI);
            }
        }
        cv_disc(x + 9, 32, 1, C_METAL_GLINT);
        if (latch_mask >> v & 1u) cv_disc(x + 21, 20, 2, C_HI);
        if(!a_poly){
            char n[2]; n[0]=(char)('1'+v);n[1]=0;cv_text(x+(24-text_w(&FONT_S,n))/2,0,&FONT_S,n,sel?C_HI:C_GRAY);
            note_name(t,p.tune[v]);cv_text(x+(24-text_w(&FONT_S,t))/2,52,&FONT_S,t,sel?C_HI:C_GRAY);
        }else if(poly_voice_key[v]>=0){
            int32_t note=12*((int32_t)poly_octave+1)+(int32_t)poly_root+poly_voice_key[v];
            note_name(t,(uint16_t)(note*256));cv_text(x+(24-text_w(&FONT_S,t))/2,52,&FONT_S,t,C_HI);
        }
    }
    cv_blit(0, 172);
}

/* one row of four: the page's parameters, drawn as the panel's own knobs */
/* knob pointer directions, 270 deg from lower-left to lower-right, Q10 (no float on the chip) */
static const int16_t KNOB_DIR[33][2] = {
    {-724,724}, {-822,610}, {-903,483}, {-964,345}, {-1004,200}, {-1023,50},
    {-1019,-100}, {-993,-249}, {-946,-392}, {-878,-526}, {-792,-650}, {-688,-759},
    {-569,-851}, {-438,-926}, {-297,-980}, {-150,-1013}, {0,-1024}, {150,-1013},
    {297,-980}, {438,-926}, {569,-851}, {688,-759}, {792,-650}, {878,-526},
    {946,-392}, {993,-249}, {1019,-100}, {1023,50}, {1004,200}, {964,345},
    {903,483}, {822,610}, {724,724}
};

/* the value 0..127 a knob stands at, or -1 when the parameter is a switch, not a knob */
static int32_t knob_pos[4];

static void draw_knob(int32_t cx, int32_t cy, int32_t pos, uint32_t hot)
{
    int32_t r = 9, i;
    /* the chrome collar, the black cap, then the pointer silk-screened on it */
    cv_disc(cx, cy, r, hot ? C_CHROME : C_DIM);
    cv_disc(cx, cy, r - 2, C_KNOB);
    if (pos >= 0) {
        int32_t dx = KNOB_DIR[pos][0], dy = KNOB_DIR[pos][1];
        cv_line(cx + (dx * 4) / 1024, cy + (dy * 4) / 1024,
                cx + (dx * (r - 3)) / 1024, cy + (dy * (r - 3)) / 1024, C_WHITE);
    } else {
        cv_disc(cx, cy, 2, C_CHROME);          /* a switch: no position to point at */
    }
    /* the skirt marks, as printed around each knob on the panel */
    for (i = 0; i <= 32; i += 8) {
        int32_t dx = KNOB_DIR[i][0], dy = KNOB_DIR[i][1];
        cv_pset(cx + (dx * (r + 2)) / 1024, cy + (dy * (r + 2)) / 1024, C_LINE);
    }
}

static void draw_knobs(const char *const *lab, const char *const *val)
{
    uint32_t k;
    cv_begin(240, 56, C_BG);
    cv_oxide(0, 114);
    for (k = 0; k < 4u; k++) {
        int32_t x = 8 + (int32_t)k * 56;   /* 4 cells of 56 fill x=8..231 exactly */
        int32_t pos = knob_pos[k] < 0 ? -1 : (knob_pos[k] > 127 ? 32 : (int32_t)(knob_pos[k] * 32 / 127));
        cv_text(x + (56 - text_w(&FONT_S, lab[k])) / 2, 0, &FONT_S, lab[k], C_GRAY);
        draw_knob(x + 28, 27, pos, 1);
        cv_text(x + (56 - text_w(&FONT_S, val[k])) / 2, 40, &FONT_S, val[k], C_HI);
    }
    cv_blit(0, 114);
}

static const char *SRCNAME[3] = {"OFF", "FM", "LFO"};

static void draw_page(void)
{
    const char *lab[4];
    char vals[4][8];
    const char *val[4];
    uint32_t k;
    uint32_t v = ui.voice, pr = ui.pair;
    for (k = 0; k < 4u; k++) val[k] = vals[k];
    switch (ui.page) {
    case PG_VOICE:
        if(!ui.polysub){lab[0]="TUNE";lab[1]="FINE";lab[2]="SHARP";lab[3]="MOD";note_name(vals[0],p.tune[v]);num_text(vals[1],(int32_t)(p.tune[v]&255u)*100/256,0);num_text(vals[2],p.sharp[pr],0);num_text(vals[3],p.mod[pr],0);knob_pos[0]=(int32_t)(p.tune[v]>>8);knob_pos[1]=(int32_t)(p.tune[v]&255u)*127/256;knob_pos[2]=p.sharp[pr];knob_pos[3]=p.mod[pr];}
        else {static const char*const NN[12]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};const char*q=NN[poly_root];uint32_t j=0;lab[0]="ROOT";lab[1]="SHARP";lab[2]="DETUNE";lab[3]="MOD";while(*q)vals[0][j++]=*q++;vals[0][j]=0;num_text(vals[1],p.sharp[0],0);num_text(vals[2],poly_detune,0);num_text(vals[3],p.mod[0],0);knob_pos[0]=poly_root*11;knob_pos[1]=p.sharp[0];knob_pos[2]=poly_detune;knob_pos[3]=p.mod[0];}
        break;
    case PG_FM:
        lab[0] = "SOURCE"; lab[1] = "MOD"; lab[2] = "STRUCT"; lab[3] = "TOT FB";
        { uint32_t i = 0; const char *q = SRCNAME[p.src[pr] < 3u ? p.src[pr] : 0]; while (*q) vals[0][i++] = *q++; vals[0][i] = 0; }
        num_text(vals[1], p.mod[pr], 0);
        { const char *q = p.structure ? "RING" : "2 LOOP"; uint32_t i = 0; while (*q) vals[2][i++] = *q++; vals[2][i] = 0; }
        { const char *q = p.total_fb ? "ON" : "OFF"; uint32_t i = 0; while (*q) vals[3][i++] = *q++;
        vals[3][i] = 0; }
        knob_pos[0] = p.src[pr] < 3u ? (int32_t)p.src[pr] * 63 : 0; knob_pos[1] = p.mod[pr];
        knob_pos[2] = p.structure ? 127 : 0; knob_pos[3] = p.total_fb ? 127 : 0;
        break;
    case PG_ENV:
        if(a_poly){int32_t tv=poly_env_target();if(tv>=0)v=(uint32_t)tv;pr=v>>1;}
        lab[0] = "FAST"; lab[1] = "REL"; lab[2] = "HOLD"; lab[3] = "ATK";
        { const char *q = p.env_fast[v] ? "FAST" : "SLOW"; uint32_t i = 0; while (*q) vals[0][i++] = *q++; vals[0][i] = 0; }
        dur_text(vals[1], p.env_fast[v] ? REL_FAST_MS[p.env_rel[v]] : REL_SLOW_MS[p.env_rel[v]]);
        if(a_poly){vals[2][0]='O';vals[2][1]='F';vals[2][2]='F';vals[2][3]=0;}else pct_text(vals[2], p.env_hold[pr]);
        dur_text(vals[3], ATK_MS[p.env_atk[v]]);
        knob_pos[0] = p.env_fast[v] ? 127 : 0; knob_pos[1] = p.env_rel[v];
        knob_pos[2] = a_poly ? -1 : p.env_hold[pr]; knob_pos[3] = p.env_atk[v];
        break;
    case PG_LFO:
        if (!ui.lfosub) {
            lab[0] = "FREQ A"; lab[1] = "FREQ B"; lab[2] = "MODE"; lab[3] = "LINK";
            num_text(vals[0], p.lfo_a, 0); num_text(vals[1], p.lfo_b, 0);
            { const char *q = p.lfo_mode ? "AND" : "OR"; uint32_t i = 0; while (*q) vals[2][i++] = *q++; vals[2][i] = 0; }
            num_text(vals[3], p.lfo_link, 0);
            knob_pos[0] = p.lfo_a; knob_pos[1] = p.lfo_b;
            knob_pos[2] = p.lfo_mode ? 127 : 0; knob_pos[3] = p.lfo_link;
        } else {
            static const char *const SRC[3] = {"A", "B", "OUT"};
            static const char *const MODE[3] = {"ALL", "PAIR", "ALT"};
            const char *q; uint32_t i;
            lab[0] = "WIDTH"; lab[1] = "SOURCE"; lab[2] = "DIR"; lab[3] = "MODE";
            num_text(vals[0], p.space_width, 0);
            q = SRC[p.space_src < 3u ? p.space_src : 0u]; i = 0; while (*q) vals[1][i++] = *q++; vals[1][i] = 0;
            q = p.space_dir ? "CCW" : "CW"; i = 0; while (*q) vals[2][i++] = *q++; vals[2][i] = 0;
            q = MODE[p.space_mode < 3u ? p.space_mode : 0u]; i = 0; while (*q) vals[3][i++] = *q++; vals[3][i] = 0;
            knob_pos[0] = p.space_width; knob_pos[1] = (int32_t)p.space_src * 63;
            knob_pos[2] = p.space_dir ? 127 : 0; knob_pos[3] = (int32_t)p.space_mode * 63;
        }
        break;
    case PG_DLY:
        if (!ui.fxsub) {
            lab[0] = "TIME 1"; lab[1] = "TIME 2"; lab[2] = "FB"; lab[3] = "MIX";
            num_text(vals[0], p.dly_time[0], 0);
            num_text(vals[1], p.dly_time[1], 0);
            num_text(vals[2], p.dly_fb, 0);
            num_text(vals[3], p.dly_mix, 0);
            knob_pos[0] = p.dly_time[0]; knob_pos[1] = p.dly_time[1];
            knob_pos[2] = p.dly_fb; knob_pos[3] = p.dly_mix;
        } else {
            lab[0] = "MOD 1"; lab[1] = "MOD 2"; lab[2] = "SOURCE"; lab[3] = "WAVE";
            num_text(vals[0], p.dly_mod[0], 0);
            num_text(vals[1], p.dly_mod[1], 0);
            { const char *q = p.dly_src ? "SELF" : "LFO"; uint32_t i = 0; while (*q) vals[2][i++] = *q++; vals[2][i] = 0; }
            { const char *q = p.dly_wave ? "SQR" : "TRI"; uint32_t i = 0; while (*q) vals[3][i++] = *q++; vals[3][i] = 0; }
            knob_pos[0] = p.dly_mod[0]; knob_pos[1] = p.dly_mod[1];
            knob_pos[2] = p.dly_src ? 127 : 0; knob_pos[3] = p.dly_wave ? 127 : 0;
        }
        break;
    case PG_ARP: {
        static const char *const ORDER[9] = {"UP", "DOWN", "PEND", "RND", "ALT UP", "ALT DN", "OUT-IN", "IN-OUT", "USER"};
        const char *q = ORDER[p.arp_order < 9u ? p.arp_order : 0u]; uint32_t i = 0;
        lab[0] = "BPM"; lab[1] = "STEPS"; lab[2] = "GATE"; lab[3] = "ORDER";
        num_text(vals[0], (int32_t)patch_bpm(&p), 0); while (vals[0][i]) i++; vals[0][i++]=' '; vals[0][i++]='B'; vals[0][i++]='P'; vals[0][i++]='M'; vals[0][i]=0; i=0;
        num_text(vals[1], p.arp_steps, 0); linear_pct_text(vals[2], p.arp_gate);
        while (*q) vals[3][i++] = *q++;
        vals[3][i] = 0;
        knob_pos[0] = ((int32_t)patch_bpm(&p) - 10) * 127 / 390; knob_pos[1] = (p.arp_steps - 1) * 18;
        knob_pos[2] = p.arp_gate; knob_pos[3] = (int32_t)p.arp_order * 16;
        break;
    }
    default:
        if (!ui.outsub) { lab[0]="DRIVE";lab[1]="D MIX";lab[2]="DRIFT";lab[3]="PRESS";
          num_text(vals[0],p.drive,0);num_text(vals[1],p.drive_mix,0);num_text(vals[2],p.drift,0);num_text(vals[3],p.press,0);
          knob_pos[0]=p.drive;knob_pos[1]=p.drive_mix;knob_pos[2]=p.drift;knob_pos[3]=p.press;
        } else { lab[0]="THRESH";lab[1]="RATIO";lab[2]="ATTACK";lab[3]="RELEASE";
          {int32_t db=-48+(int32_t)p.comp_thresh*48/127;num_text(vals[0],db,1);}
          if(!p.comp_ratio){vals[1][0]='1';vals[1][1]=':';vals[1][2]='1';vals[1][3]=0;}else{uint32_t r=1u+(uint32_t)p.comp_ratio*19u/127u;num_text(vals[1],r,0);{uint32_t z=0;while(vals[1][z])z++;vals[1][z++]=':';vals[1][z++]='1';vals[1][z]=0;}}
          num_text(vals[2],1u+(uint32_t)(127u-p.comp_attack)*99u/127u,0);num_text(vals[3],20u+(uint32_t)(127u-p.comp_release)*1980u/127u,0);
          knob_pos[0]=p.comp_thresh;knob_pos[1]=p.comp_ratio;knob_pos[2]=p.comp_attack;knob_pos[3]=p.comp_release;
        } break;
    }
    draw_knobs(lab, val);
}

/* the middle of the screen: what the page is about */
static void draw_body(void)
{
    uint32_t k;
    cv_begin(240, 70, C_BG);
    cv_oxide(0, 42);
    switch (ui.page) {
    case PG_FM:                                 /* the four pairs and where each one's modulation comes from */
        for (k = 0; k < NP; k++) {
            int32_t x = 8 + (int32_t)k * 58;
            char s[8];
            uint16_t c = k == ui.pair ? C_HOT : C_ACC;
            cv_rect(x, 4, 50, 32, C_BG);
            cv_rect(x, 4, 50, 2, c);
            cv_rect(x, 34, 50, 2, c);
            s[0] = (char)('1' + k * 2u); s[1] = (char)('1' + k * 2u + 1u); s[2] = 0;
            cv_text(x + 4, 11, &FONT_S, s, c);
            cv_text(x + 24, 11, &FONT_S, SRCNAME[p.src[k] < 3u ? p.src[k] : 0], C_HI);
            if (p.src[k] == 1u) {
                uint32_t f = fm_from(k, p.structure);
                int32_t fx = 8 + (int32_t)f * 58 + 25;
                cv_line(x + 25, 43, fx, 43, C_ACC);
                cv_line(fx, 43, fx, 37, C_ACC);
            }
            num_text(s, p.mod[k], 0);
            cv_text(x + 4, 48, &FONT_S, s, C_HI);
        }
        break;
    case PG_LFO: {                              /* the hyper LFO, drawn as it runs */
        int32_t x;
        if (ui.lfosub) {
            static int32_t prev[NP];
            static const char *const mode[3]={"ALL","PAIR","ALT"};
            const char *m=mode[p.space_mode<3u?p.space_mode:0u];
            int32_t mw=text_w(&FONT_S,m);
            cv_text(8,2,&FONT_S,"L",C_GRAPH_HI);cv_text(225,2,&FONT_S,"R",C_GRAPH_HI);
            cv_text(120-mw/2,2,&FONT_S,m,C_GRAPH_HI);
            for(k=0;k<NP;k++){
                int32_t y=18+(int32_t)k*15,x=120+((mon_space[k]*107)>>15),d=mon_space[k]-prev[k];
                if(x<13)x=13;
                if(x>227)x=227;
                cv_line(8,y,231,y,C_GRAPH_DIM);cv_line(8,y-3,8,y+3,C_GRAPH_HI);cv_line(231,y-3,231,y+3,C_GRAPH_HI);
                cv_line(x-4,y,x+4,y,C_GRAPH_HI);cv_disc(x-3,y-1,2,C_GRAPH_HI);cv_disc(x+3,y+1,2,C_GRAPH_HI);
                if(d>128&&x<220){cv_line(x+7,y,x+11,y,C_GRAPH_HI);cv_line(x+9,y-2,x+11,y,C_GRAPH_HI);cv_line(x+9,y+2,x+11,y,C_GRAPH_HI);}
                else if(d<-128&&x>39){cv_line(x-11,y,x-7,y,C_GRAPH_HI);cv_line(x-11,y,x-9,y-2,C_GRAPH_HI);cv_line(x-11,y,x-9,y+2,C_GRAPH_HI);}
                prev[k]=mon_space[k];
            }
            break;
        }
        /* a window of about a second (224 px / 44100 Hz), so the rate is visible. A alone is dim,
         * the combined modulator - what actually reaches the voices - is bright: MODE and LINK show here. */
        uint32_t sa_step = a_lfo_inc[0] * 184u, sb_step = a_lfo_inc[1] * 184u;
        cv_line(8, 38, 231, 38, C_LINE);
        for (x = 8; x <= 231; x++) {
            uint32_t pa = lfo_ph[0] + (uint32_t)(x - 8) * sa_step;
            uint32_t pb = lfo_ph[1] + (uint32_t)(x - 8) * sb_step;
            int32_t va = (pa & 0x80000000u) ? 32767 : -32767;
            int32_t vb = (pb & 0x80000000u) ? 32767 : -32767;
            int32_t both = a_lfo_mode ? ((va * vb) >> 15) : ((va + vb) >> 1);
            /* Keep both traces one pixel inside the 70-row body. The old signed >> 10
             * mapped the negative full-scale endpoint to y=70, clipping the lower triangle
             * tip and placing the low square level outside the canvas. */
            cv_pset(x, 38 - (int32_t)(((int64_t)tri(pa) * 30) >> 15), C_GRAPH_DIM);
            cv_pset(x, 38 - (int32_t)(((int64_t)both * 30) >> 15), C_GRAPH_HI);
        }
        {
            int32_t y = 38 - (int32_t)(((int64_t)mon_lfo * 30) >> 15);
            cv_rect(8, y - 1, 3, 3, C_GRAPH_HI);
        }
        break;
    }
    case PG_DLY: {                              /* two lines, their length and how they feed each other */
        for (k = 0; k < 2u; k++) {
            int32_t y = 6 + (int32_t)k * 25;
            uint32_t frames = delay_frames(p.dly_time[k]);
            int32_t w = 4 + (int32_t)(frames * 180u / DELAY_MAX_FRAMES);
            char s[8];
            cv_rect(8, y, 192, 14, C_LINE);
            cv_rect(8, y, w, 14, C_ACC);
            if (frames < 45u) { s[0] = '<'; s[1] = '1'; s[2] = 0; }
            else num_text(s, (int32_t)(frames * 1000u / 44100u), 0);
            /* right-aligned to the same edge as every other figure: 232 is one past 231 */
            cv_text(232 - text_w(&FONT_S, s), y - 2, &FONT_S, s, C_OFF);
        }
        { const char *m = p.dly_src ? "SELF MODULATION" : "LFO MODULATION";
          cv_text(text_pen_x(8, &FONT_S, m), 53, &FONT_S, m, C_HI); }
        break;
    }
    case PG_ENV: {
        for (k = 0; k < NP; k++) {
            int32_t x = 8 + (int32_t)k * 58;
            uint32_t id=k*2u+(ui.voice&1u),ms=p.env_fast[id]?REL_FAST_MS[p.env_rel[id]]:REL_SLOW_MS[p.env_rel[id]];
            uint32_t m = ms;
            int32_t lg = 0, rl, fy;
            uint16_t c = k == ui.pair ? C_GRAPH_HI : C_GRAPH_DIM;
            /* the slope follows the real time, so it gets SHORTER as REL rises — the old
             * drawing did the opposite and taught the panel backwards. Log scale keeps
             * both 2 ms and 8 s visible. */
            while (m > 2u && lg < 12) { m >>= 1; lg++; }
            rl = 3 + lg * 37 / 12;
            fy = 62 - (int32_t)(hold_floor(p.env_hold[k]) * 50u / 32768u);
            cv_line(x, 62, x + 7, 8, c);                    /* attack */
            cv_line(x + 7, 8, x + 7 + rl, fy, c);           /* release, down to the floor */
            if (fy < 62) cv_line(x + 7 + rl, fy, x + 49, fy, C_GRAPH_HI);   /* HOLD keeps it here */
            cv_line(x, 65, x + 49, 65, C_LINE);
            if (p.env_fast[id]) cv_text(x + 41, 0, &FONT_S, "F", C_HI);
        }
        break;
    }
    case PG_ARP: {
        const char *state = arp_running ? "RUN" : (p.arp_order == 8u ? "LEARN" : "STOP");
        for (k = 0; k < NV; k++) {
            int32_t x = 15 + (int32_t)k * 30; uint32_t active = arp_running && k == arp_display_step;
            uint32_t cursor = !arp_running && p.arp_order == 8u && k == arp_learn_step;
            uint16_t c = k < p.arp_steps ? (active || cursor ? C_GRAPH_HI : C_GRAPH_DIM) : C_LINE;
            char n[6] = {0}; uint32_t i=0,j,mask=p.arp_pattern[k];
            if (k) cv_line(x - 30, 34, x, 34, C_LINE);
            cv_disc(x,34,active?5:(cursor?4:3),c);
            if (p.arp_order == 8u) {
                for(j=0;j<8u&&i<3u;j++) if(mask&(1u<<j)){if(i)n[i++]='+';n[i++]=(char)('1'+j);}
                if(!i)n[i++]='-';
                n[i]=0; cv_text(x-text_w(&FONT_S,n)/2,46,&FONT_S,n,k<p.arp_steps?C_HI:C_GRAY);
            }
        }
        cv_text(120 - text_w(&FONT_S, state) / 2, 4, &FONT_S, state, arp_running ? C_GRAPH_HI : C_GRAPH_DIM);
        break;
    }
    case PG_OUT: {
        if (!ui.outsub) {                              /* SIGNAL WEB: the eight voices and their couplings */
        static const int16_t px[8] = {120,176,205,176,120,64,35,64};
        /* Lower half reaches the last safe body rows: node 5 lands between D MIX and DRIFT,
         * while its radius still ends at y=47 and cannot enter the knob strip at screen y=92. */
        static const int16_t py[8] = {17,23,38,56,64,56,38,23};
        int32_t w = (mon_out * 224) >> 15;
        if (w > 224) w = 224;
        /* y=2..7 only; the web starts at y=9, so the level meter can never touch it. */
        cv_rect(8, 2, 224, 9, C_LINE);
        cv_rect(8, 2, w, 9, w > 202 ? C_HOT : C_ACC);
        /* Every outgoing modulation link keeps the colour of its source pair, so the
         * crossings remain readable without labels: 12 amber, 34 cyan, 56 magenta, 78 green. */
        for (k = 0; k < 8u; k++)
            cv_line(px[k], py[k], px[(k + 2u) & 7u], py[(k + 2u) & 7u], pair_colour(k >> 1));
        /* The short bond between both voices of a pair uses the same colour. */
        for (k = 0; k < 8u; k += 2u)
            cv_line(px[k], py[k], px[k + 1u], py[k + 1u], pair_colour(k >> 1));
        /* TOTAL FB closes the signal network's outer ring. */
        if (p.total_fb)
            for (k = 0; k < 8u; k++)
                cv_line(px[k], py[k], px[(k + 1u) & 7u], py[(k + 1u) & 7u], C_ACC);
        /* Keep the network's coloured links continuous. FB / LFO has no plate or cleared
         * island: only its glyph pixels sit over the web, centred exactly on x=120. */
        {
            const char *mode = p.total_fb ? "FB" : "LFO";
            int32_t mw = text_w(&FONT_S, mode);
            cv_text(120 - mw / 2, 31, &FONT_S, mode, p.total_fb ? C_HOT : C_ACC);
        }
        /* A voice grows by one pixel when its envelope is alive; touch/latch makes it hot. */
        for (k = 0; k < 8u; k++) {
            int32_t e = env_bar(v_env[k], a_hold[k >> 2], 4), r = 2 + (e > 1);
            uint16_t c = pair_colour(k >> 1);
            cv_disc(px[k], py[k], r, c);
            if ((touch_mask | latch_mask) >> k & 1u) cv_disc(px[k], py[k], 1, C_HOT);
        }
        } else {
            static const char *const L[3]={"IN","OUT","GR"}; int32_t val[3]={mon_comp_in,mon_comp_out,mon_comp_gr}; int32_t y;
            for(k=0;k<3u;k++){int32_t w=(val[k]*184)>>15;if(w<0)w=0;if(w>184)w=184;y=10+(int32_t)k*22;cv_text(8,y-3,&FONT_S,L[k],C_HI);cv_rect(43,y,188,9,C_LINE);if(w)cv_rect(45,y+2,w,5,k==2u?C_HOT:C_GRAPH_HI);}
            {int32_t tx=45+(int32_t)p.comp_thresh*184/127;cv_line(tx,6,tx,21,C_ACC);}
        }
        break;
    }
    default: {                                  /* VOICE: large real scope, compact SHARP forms below */
        uint32_t wr = scope_wr;
        int32_t py = 25;
        cv_line(8, 25, 231, 25, C_LINE);
        for (k = 0; k < 112u; k++) {
            int32_t y = 25 - ((int32_t)scope_wave[(wr - 112u + k) & 127u] * 42 >> 15);
            /* Scope peaks may escape their nominal field, but y=53..69 is reserved for SHARP. */
            if (y < 0) y = 0;
            if (y > 52) y = 52;
            if (k) cv_line(8 + (int32_t)(k - 1u) * 2, py, 8 + (int32_t)k * 2, y, C_HI);
            py = y;
        }
        for (k = 0; k < NP; k++) {
            int32_t x = 8 + (int32_t)k * 58, j;
            uint16_t c = k == ui.pair ? C_GRAPH_HI : C_GRAPH_DIM;
            for (j = 0; j < 50; j++) {
                int32_t ph = (uint32_t)j << 26;
                int32_t y = 60 - (shape(tri(ph), 2731 + (int32_t)p.sharp[k] * 170) >> 13);
                cv_pset(x + j, y, c);
            }
        }
        break;
    }
    }
    cv_blit(0, 42);
}

/* The instrument's mark: eight voices around the ring that couples them, drawn as the panel
 * would print it. 15 x 15, integer only: small enough that the wordmark fits under it and
 * still above the status line. */
static const int8_t LOGO_PT[8][2] = {
    { 0, -6}, { 4, -4}, { 6, 0}, { 4, 4}, { 0, 6}, {-4, 4}, {-6, 0}, {-4, -4}
};
static void draw_logo_raw(int32_t cx,int32_t cy,uint16_t ink,uint16_t faint)
{
    uint32_t k;for(k=0;k<8u;k+=2){uint32_t n=(k+2u)&7u;cv_line(cx+LOGO_PT[k][0],cy+LOGO_PT[k][1],cx+LOGO_PT[n][0],cy+LOGO_PT[n][1],faint);}for(k=1;k<8u;k+=2){uint32_t n=(k+2u)&7u;cv_line(cx+LOGO_PT[k][0],cy+LOGO_PT[k][1],cx+LOGO_PT[n][0],cy+LOGO_PT[n][1],faint);}for(k=0;k<8u;k++)cv_disc(cx+LOGO_PT[k][0],cy+LOGO_PT[k][1],1,ink);cv_disc(cx,cy,1,faint);
}
static void draw_logo(int32_t cx,int32_t cy,uint16_t ink,uint16_t faint)
{
    if(pal_index>=7u){uint16_t edge=pal_light?C_WHITE:C_BLACK;draw_logo_raw(cx-1,cy,edge,edge);draw_logo_raw(cx+1,cy,edge,edge);draw_logo_raw(cx,cy-1,edge,edge);draw_logo_raw(cx,cy+1,edge,edge);ink=faint=pal_light?C_BLACK:C_WHITE;}draw_logo_raw(cx,cy,ink,faint);
}

/* Compact ARTIHIVE wordmark for the 44 px header block. Four-pixel glyphs keep all eight
 * letters centred without changing the existing small eight-node mark above it. */
static const uint8_t WM_GLYPH[8][5] = {
    {0x06, 0x09, 0x0F, 0x09, 0x09}, /* A */
    {0x0E, 0x09, 0x0E, 0x0A, 0x09}, /* R */
    {0x0F, 0x06, 0x06, 0x06, 0x06}, /* T */
    {0x0F, 0x06, 0x06, 0x06, 0x0F}, /* I */
    {0x09, 0x09, 0x0F, 0x09, 0x09}, /* H */
    {0x0F, 0x06, 0x06, 0x06, 0x0F}, /* I */
    {0x09, 0x09, 0x09, 0x06, 0x06}, /* V */
    {0x0F, 0x08, 0x0E, 0x08, 0x0F}, /* E */
};
#define WM_W 39
static void draw_wordmark_raw(int32_t cx,int32_t y,uint16_t ink)
{
    int32_t x=cx-WM_W/2,g,r,b;for(g=0;g<8;g++)for(r=0;r<5;r++){uint8_t row=WM_GLYPH[g][r];for(b=0;b<4;b++)if(row&(1u<<(3-b)))cv_pset(x+g*5+b,y+r,ink);}
}
static void draw_wordmark(int32_t cx,int32_t y,uint16_t ink)
{
    if(pal_index>=7u){uint16_t edge=pal_light?C_WHITE:C_BLACK;draw_wordmark_raw(cx-1,y,edge);draw_wordmark_raw(cx+1,y,edge);draw_wordmark_raw(cx,y-1,edge);draw_wordmark_raw(cx,y+1,edge);ink=pal_light?C_BLACK:C_WHITE;}
    draw_wordmark_raw(cx,y,ink);
}

static void draw_boot_logo_raw(int32_t cx,int32_t cy,int32_t scale,uint16_t ink)
{
    uint32_t k;for(k=0;k<8u;k+=2){uint32_t n=(k+2u)&7u;cv_line(cx+LOGO_PT[k][0]*scale,cy+LOGO_PT[k][1]*scale,cx+LOGO_PT[n][0]*scale,cy+LOGO_PT[n][1]*scale,ink);}for(k=1;k<8u;k+=2){uint32_t n=(k+2u)&7u;cv_line(cx+LOGO_PT[k][0]*scale,cy+LOGO_PT[k][1]*scale,cx+LOGO_PT[n][0]*scale,cy+LOGO_PT[n][1]*scale,ink);}for(k=0;k<8u;k++){uint32_t n=(k+1u)&7u;cv_line(cx+LOGO_PT[k][0]*scale,cy+LOGO_PT[k][1]*scale,cx+LOGO_PT[n][0]*scale,cy+LOGO_PT[n][1]*scale,ink);cv_line(cx+LOGO_PT[k][0]*scale,cy+LOGO_PT[k][1]*scale,cx,cy,ink);}for(k=0;k<8u;k++)cv_disc(cx+LOGO_PT[k][0]*scale,cy+LOGO_PT[k][1]*scale,4,ink);cv_disc(cx,cy,4,ink);
}

static void draw_boot(void)
{
    const int32_t scale = 5, cx = 120, cy = 88;
    char ver[16] = "V";
    uint32_t vi = 1;
    draw_panel_bg();
    while (APP_VER[vi - 1u] && vi < sizeof ver - 1u) {
        ver[vi] = APP_VER[vi - 1u];
        vi++;
    }
    ver[vi] = 0;
    /* Firmware identity stays visible throughout loading, clear of the centred logo. */
    draw_text_box((uint32_t)text_pen_x(8, &FONT_S, ver), 8, 72, &FONT_S, ver, C_AMB, 0);

    /* Original mark, plus one new outer contour connecting all eight points in sequence. */
    cv_begin(240, 88, C_BG);
    cv_oxide(0, 44);
    /* Texture themes keyline the complete loading mark just like the page-header mark. */
    if(pal_index>=7u){uint16_t edge=pal_light?C_WHITE:C_BLACK,ink=pal_light?C_BLACK:C_WHITE;draw_boot_logo_raw(cx-1,cy-44,scale,edge);draw_boot_logo_raw(cx+1,cy-44,scale,edge);draw_boot_logo_raw(cx,cy-45,scale,edge);draw_boot_logo_raw(cx,cy-43,scale,edge);draw_boot_logo_raw(cx,cy-44,scale,ink);}else draw_boot_logo_raw(cx,cy-44,scale,C_HI);
    cv_blit(0, 44);

    draw_text_box(0, 137, 240, &FONT_L, APP_NAME, C_HI, 1);
    draw_text_box(0, 176, 240, &FONT_S, "SETTINGS", C_ACC, 1);
    draw_text_box(152, 218, 80, &FONT_S, "byKHAV", C_AMB, 2);
    lcd_sync();
}

static void boot_progress(const char *stage, uint32_t pct)
{
    uint32_t w = pct > 100u ? 100u : pct;
    draw_text_box(0, 176, 240, &FONT_S, stage, C_ACC, 1);
    lcd_fill(24, 202, 192, 10, C_DIM);
    lcd_fill(26, 204, 188, 6, C_BG);
    if (w) lcd_fill(26, 204, 188u * w / 100u, 6, C_HI);
    lcd_sync();
}

/* Validate every stored control before audio sees it. A damaged settings record is already
 * rejected by CRC; these bounds also make malformed/old user-slot bytes harmless. */
static uint32_t patch_sanitize(patch_t *q)
{
    uint32_t i, changed = 0;
#define LIM(field, hi) do { if ((q->field) > (hi)) { (q->field) = (hi); changed++; } } while (0)
    q->name[sizeof q->name - 1u] = 0;
    for (i = 0; i < NV; i++) {
        uint16_t x = q->tune[i];
        if (x < 2048u) { q->tune[i] = 2048u; changed++; }
        else if (x > 32512u) { q->tune[i] = 32512u; changed++; }
    }
    for (i = 0; i < NP; i++) {
        if (q->sharp[i] > 127u) { q->sharp[i] = 127u; changed++; }
        if (q->mod[i] > 127u) { q->mod[i] = 127u; changed++; }
        if (q->src[i] > 2u) { q->src[i] = 0; changed++; }
        if (q->fast[i] > 1u) { q->fast[i] = 0; changed++; }
        if (q->rel[i] > 127u) { q->rel[i] = 127u; changed++; }
    }
    for (i = 0; i < 2u; i++) {
        if (q->hold[i] > 127u) { q->hold[i] = 127u; changed++; }
        if (q->pitch[i] < -24) { q->pitch[i] = -24; changed++; }
        else if (q->pitch[i] > 24) { q->pitch[i] = 24; changed++; }
        if (q->dly_time[i] > 127u) { q->dly_time[i] = 127u; changed++; }
        if (q->dly_mod[i] > 127u) { q->dly_mod[i] = 127u; changed++; }
    }
    LIM(lfo_a,127u); LIM(lfo_b,127u); LIM(lfo_mode,1u); LIM(lfo_link,127u);
    LIM(vib,127u); LIM(dly_src,1u); LIM(dly_wave,1u); LIM(dly_fb,127u); LIM(dly_mix,127u);
    LIM(drive,127u); LIM(drive_mix,127u); LIM(total_fb,1u); LIM(structure,1u);
    LIM(drift,127u); LIM(atk,127u); LIM(space_width,127u); LIM(space_src,2u);
    for(i=0;i<NV;i++){if(q->env_fast[i]>1u){q->env_fast[i]=0;changed++;}if(q->env_rel[i]>127u){q->env_rel[i]=127;changed++;}if(q->env_atk[i]>127u){q->env_atk[i]=127;changed++;}}
    for(i=0;i<NP;i++)if(q->env_hold[i]>127u){q->env_hold[i]=127;changed++;}
    LIM(space_dir,1u); LIM(space_mode,2u); LIM(comp_thresh,127u); LIM(comp_ratio,127u); LIM(comp_attack,127u); LIM(comp_release,127u);
    if (!q->arp_steps) { uint32_t j; q->arp_rate = 120u; q->arp_steps = 8u; q->arp_gate = 64u; q->arp_order = 0u; for(j=0;j<8u;j++)q->arp_pattern[j]=(uint8_t)(1u<<j); changed++; }
    else { uint32_t j,bpm=patch_bpm(q); if (q->arp_steps > 8u) { q->arp_steps = 8u; changed++; } if (bpm < 10u) { patch_set_bpm(q,10u); changed++; } else if (bpm > 400u) { patch_set_bpm(q,400u); changed++; } LIM(arp_gate,127u); LIM(arp_order,8u); for(j=0;j<8u;j++)if(!q->arp_pattern[j]){q->arp_pattern[j]=(uint8_t)(1u<<j);changed++;} }
    if (!q->press) { q->press = 1u; changed++; }
    else LIM(press,127u);
#undef LIM
    return changed;
}

static uint32_t dsp_tables_ok(void)
{
    uint32_t i;
    if (REL_SLOW_MS[0] != 8000u || REL_FAST_MS[127] != 2u || ATK_MS[0] != 2u) return 0;
    for (i = 1; i < 128u; i++)
        if (REL_SLOW_MS[i] > REL_SLOW_MS[i - 1u] || REL_FAST_MS[i] > REL_FAST_MS[i - 1u] ||
            ATK_MS[i] < ATK_MS[i - 1u]) return 0;
    return 1;
}

static void draw_header(void)
{
    char s[28];
    uint32_t i = 0;
    const char *q = (ui.page==PG_VOICE&&ui.polysub)?"POLY":((ui.page==PG_OUT&&ui.outsub)?"OUT COMP":((ui.page == PG_LFO && ui.lfosub) ? "LFO SPACE" : PGNAME[ui.page]));
    while (*q) s[i++] = *q++;
    s[i] = 0;
    /* One margin for the whole screen: labels live in x=8..231. The page name box stops at
     * x=96 so it can never reach the centred mark block (x=98..141); the longest name is
     * HYPER LFO, 9 * 8 = 72 px. The pen is placed by text_pen_x so the first inked column
     * lands on 8 for every page: M has no left side bearing, most other letters have one
     * pixel, and without the correction MOD DEL sat a pixel left of the preset name. */
    draw_text_box((uint32_t)text_pen_x(8, &FONT_S, s), 4, 88, &FONT_S, s, C_ACC, 0);
    num_text(s, ui.voice + 1u, 0);
    { char t[12] = "V"; uint32_t n = 1, j; for (j = 0; s[j]; j++) t[n++] = s[j]; t[n] = 0;
      draw_text_box(148, 4, 40, &FONT_S, t, C_HOT, 2); }
    { char t[12] = "P"; uint32_t n = 1, j; num_text(s, ui.pair + 1u, 0); for (j = 0; s[j]; j++) t[n++] = s[j]; t[n] = 0;
      draw_text_box(192, 4, 40, &FONT_S, t, C_OFF, 2); }
    /* The mark and the wordmark, both centred on the screen axis x=120: a 44 px canvas at
     * x=98 puts its middle at 98 + 22 = 120. Drawn last, because draw_text_box clears its
     * whole rectangle and would otherwise clip the block. Rows 0..23 only: the status line
     * starts at 24, so the name never meets it. */
    cv_begin(44, 24, C_BG);
    cv_oxide(98, 0);
    draw_logo(22, 8, C_HI, C_GRAY);
    draw_wordmark(22, 17, pal_index >= 7u ? (pal_light ? C_BLACK : C_WHITE) : C_GRAY);
    cv_blit(98, 0);
    lcd_sync();
}

static void draw_status(void)
{
    char s[32];
    uint32_t i = 0;
    const char *q;
    uint32_t live = msg[0] && fm1_ms - msg_ms < 1600u;
    if (live) q = msg;
    else if (ui.page == PG_OUT) {
        uint32_t pc = (mon_cpu + 128u) >> 8, n = 0;
        s[n++] = 'C'; s[n++] = 'P'; s[n++] = 'U'; s[n++] = ' ';
        if (pc >= 100u) s[n++] = '1';
        if (pc >= 10u) s[n++] = (char)('0' + (pc / 10u) % 10u);
        s[n++] = (char)('0' + pc % 10u); s[n++] = '%'; s[n] = 0;
        q = s;
    }
    else if (a_dly_freeze) q = "FREEZE";
    else if (ui.latch) q = "LATCH: KEYS HOLD";
    else q = p.name;
    if (q != s) {
        while (*q && i < sizeof s - 1u) s[i++] = *q++;
        s[i] = 0;
    }
    draw_text_box((uint32_t)text_pen_x(8, &FONT_S, s), 24, 160, &FONT_S, s, live ? C_HI : C_AMB, 0);
    if (!live) {
        char t[16];
        if (ui.page == PG_OUT) {
            uint32_t n = 1, j;
            t[0] = 'V';
            for (j = 0; APP_VER[j] && n < sizeof t - 1u; j++) t[n++] = APP_VER[j];
            t[n] = 0;
        } else num_text(t, ui.preset + 1u, 0);
        draw_text_box(172, 24, 60, &FONT_S, t, C_GRAY, 2);
    } else draw_text_box(172, 24, 60, &FONT_S, "", C_GRAY, 2);
}

/* ---------------------------------------------------------------- parameter editing */
static void knob(uint32_t k, int32_t e)
{
    uint32_t v = ui.voice, pr = ui.pair;
    if (!e) return;
    undo_valid = 0;                       /* UNDO is only valid before any manual edit */
    switch (ui.page) {
    case PG_VOICE:
        if(!ui.polysub){if(k==0){int32_t t=(int32_t)p.tune[v]+e*256;p.tune[v]=(uint16_t)clampi(t,2048,32512);}else if(k==1){int32_t t=(int32_t)p.tune[v]+e*8;p.tune[v]=(uint16_t)clampi(t,2048,32512);}else if(k==2)p.sharp[pr]=clamp8((int32_t)p.sharp[pr]+e);else p.mod[pr]=clamp8((int32_t)p.mod[pr]+e);}
        else {uint32_t i;if(k==0)poly_root=(uint8_t)clampi((int32_t)poly_root+e,0,11);else if(k==1){uint8_t x=clamp8((int32_t)p.sharp[0]+e);for(i=0;i<NP;i++)p.sharp[i]=x;}else if(k==2)poly_detune=clamp8((int32_t)poly_detune+e);else{uint8_t x=clamp8((int32_t)p.mod[0]+e);for(i=0;i<NP;i++)p.mod[i]=x;}poly_retune();}
        break;
    case PG_FM:
        if (k == 0) p.src[pr] = (uint8_t)clampi((int32_t)p.src[pr] + e, 0, 2);
        else if (k == 1) p.mod[pr] = clamp8((int32_t)p.mod[pr] + e);
        else if (k == 2) p.structure = p.structure ? 0u : 1u;
        else p.total_fb = p.total_fb ? 0u : 1u;
        break;
    case PG_ENV:
        if(a_poly){int32_t tv=poly_env_target();uint32_t i;if(k==2){say("HOLD OFF");break;}if(tv>=0){v=(uint32_t)tv;if(k==0)p.env_fast[v]^=1u;else if(k==1)p.env_rel[v]=clamp8((int32_t)p.env_rel[v]+e);else p.env_atk[v]=clamp8((int32_t)p.env_atk[v]+e);}else if(k==0){uint8_t x=p.env_fast[v]?0u:1u;for(i=0;i<NV;i++)p.env_fast[i]=x;}else if(k==1){uint8_t x=clamp8((int32_t)p.env_rel[v]+e);for(i=0;i<NV;i++)p.env_rel[i]=x;}else{uint8_t x=clamp8((int32_t)p.env_atk[v]+e);for(i=0;i<NV;i++)p.env_atk[i]=x;}}
        else if (k == 0) p.env_fast[v] ^= 1u;
        else if (k == 1) {p.env_rel[v] = clamp8((int32_t)p.env_rel[v] + e);if (p.env_hold[pr]) say("HOLD MASKS REL");}
        else if (k == 2) p.env_hold[pr] = clamp8((int32_t)p.env_hold[pr] + e);
        else p.env_atk[v] = clamp8((int32_t)p.env_atk[v] + e);
        break;
    case PG_LFO:
        if (!ui.lfosub) {
            if (k == 0) p.lfo_a = clamp8((int32_t)p.lfo_a + e);
            else if (k == 1) p.lfo_b = clamp8((int32_t)p.lfo_b + e);
            else if (k == 2) p.lfo_mode = p.lfo_mode ? 0u : 1u;
            else p.lfo_link = clamp8((int32_t)p.lfo_link + e);
        } else {
            if (k == 0) p.space_width = clamp8((int32_t)p.space_width + e);
            else if (k == 1) p.space_src = (uint8_t)clampi((int32_t)p.space_src + (e > 0 ? 1 : -1),0,2);
            else if (k == 2) p.space_dir ^= 1u;
            else p.space_mode = (uint8_t)clampi((int32_t)p.space_mode + (e > 0 ? 1 : -1),0,2);
        }
        break;
    case PG_DLY:
        if (!ui.fxsub) {
            if (k == 0) p.dly_time[0] = clamp8((int32_t)p.dly_time[0] + e);
            else if (k == 1) p.dly_time[1] = clamp8((int32_t)p.dly_time[1] + e);
            else if (k == 2) p.dly_fb = clamp8((int32_t)p.dly_fb + e);
            else p.dly_mix = clamp8((int32_t)p.dly_mix + e);
        } else {
            if (k == 0) p.dly_mod[0] = clamp8((int32_t)p.dly_mod[0] + e);
            else if (k == 1) p.dly_mod[1] = clamp8((int32_t)p.dly_mod[1] + e);
            else if (k == 2) p.dly_src = p.dly_src ? 0u : 1u;
            else p.dly_wave = p.dly_wave ? 0u : 1u;
        }
        break;
    case PG_ARP:
        if (k == 0) { int32_t a=e<0?-e:e,m=a>=8?10:(a>=4?5:1); patch_set_bpm(&p,(uint32_t)clampi((int32_t)patch_bpm(&p)+e*m,10,400)); }
        else if (k == 1) p.arp_steps = (uint8_t)clampi((int32_t)p.arp_steps + e, 1, 8);
        else if (k == 2) p.arp_gate = clamp8((int32_t)p.arp_gate + e);
        else { p.arp_order = (uint8_t)clampi((int32_t)p.arp_order + e, 0, 8); if (p.arp_order == 8u) { arp_learn_step=0; arp_learn_mask=0; arp_learn_active=0; } }
        break;
    default:
        if(!ui.outsub){if(k==0)p.drive=clamp8((int32_t)p.drive+e);else if(k==1)p.drive_mix=clamp8((int32_t)p.drive_mix+e);else if(k==2)p.drift=clamp8((int32_t)p.drift+e);else p.press=(uint8_t)clampi((int32_t)p.press+e,1,127);}
        else {if(k==0)p.comp_thresh=clamp8((int32_t)p.comp_thresh+e);else if(k==1)p.comp_ratio=clamp8((int32_t)p.comp_ratio+e);else if(k==2)p.comp_attack=clamp8((int32_t)p.comp_attack+e);else p.comp_release=clamp8((int32_t)p.comp_release+e);} break;
    }
    dirty = 1;
    t_change = fm1_ms;
    to_audio();
    gates();
}

static int32_t mutate_step(uint32_t span)
{
    int32_t d;
    rs32 = rs32 * 1664525u + 1013904223u;
    d = (int32_t)(rs32 % (span * 2u + 1u)) - (int32_t)span;
    return d ? d : ((rs32 >> 31) ? 1 : -1);
}

/* Change exactly one visible control and return its panel label. */
static const char *mutate_control(uint32_t k)
{
    uint32_t i;
    switch (ui.page) {
    case PG_VOICE:
        if(k==0){for(i=0;i<NV;i++)p.tune[i]=(uint16_t)clampi((int32_t)p.tune[i]+mutate_step(2)*256,2048,32512);return "ALL TUNE";}
        if(k==1){for(i=0;i<NV;i++)p.tune[i]=(uint16_t)clampi((int32_t)p.tune[i]+mutate_step(12)*8,2048,32512);return "ALL FINE";}
        if(k==2){for(i=0;i<NP;i++)p.sharp[i]=clamp8((int32_t)p.sharp[i]+mutate_step(8));return "ALL SHARP";}
        for(i=0;i<NP;i++) p.mod[i]=clamp8((int32_t)p.mod[i]+mutate_step(8));
        return "ALL MOD";
    case PG_FM:
        if(k==0){for(i=0;i<NP;i++)p.src[i]=(uint8_t)(((int32_t)p.src[i]+(mutate_step(1)>0?1:2))%3);return "ALL SOURCE";}
        if(k==1){for(i=0;i<NP;i++)p.mod[i]=clamp8((int32_t)p.mod[i]+mutate_step(8));return "ALL MOD";}
        if(k==2){p.structure^=1u;return "STRUCT";}rs32=rs32*1664525u+1013904223u;if(!(rs32&3u))p.total_fb^=1u;return "TOT FB";
    case PG_ENV:
        if(k==0){for(i=0;i<NV;i++)p.env_fast[i]^=1u;return "ALL FAST";}
        if(k==1){for(i=0;i<NV;i++)p.env_rel[i]=clamp8((int32_t)p.env_rel[i]+mutate_step(6));return "ALL REL";}
        if(k==2){for(i=0;i<NP;i++)p.env_hold[i]=clamp8((int32_t)p.env_hold[i]+mutate_step(6));return "ALL HOLD";}
        for(i=0;i<NV;i++) p.env_atk[i]=clamp8((int32_t)p.env_atk[i]+mutate_step(6));
        return "ALL ATK";
    case PG_LFO:
        if(!ui.lfosub){if(k==0){p.lfo_a=clamp8((int32_t)p.lfo_a+mutate_step(6));return "FREQ A";}if(k==1){p.lfo_b=clamp8((int32_t)p.lfo_b+mutate_step(6));return "FREQ B";}if(k==2){p.lfo_mode^=1u;return "MODE";}p.lfo_link=clamp8((int32_t)p.lfo_link+mutate_step(8));return "LINK";}
        if(k==0){p.space_width=clamp8((int32_t)p.space_width+mutate_step(10));return "WIDTH";}if(k==1){p.space_src=(uint8_t)(((int32_t)p.space_src+(mutate_step(1)>0?1:2))%3);return "SOURCE";}if(k==2){p.space_dir^=1u;return "DIR";}p.space_mode=(uint8_t)(((int32_t)p.space_mode+(mutate_step(1)>0?1:2))%3);return "MODE";
    case PG_DLY:
        if(!ui.fxsub){if(k==0){p.dly_time[0]=clamp8((int32_t)p.dly_time[0]+mutate_step(8));return "TIME 1";}if(k==1){p.dly_time[1]=clamp8((int32_t)p.dly_time[1]+mutate_step(8));return "TIME 2";}if(k==2){p.dly_fb=clamp8((int32_t)p.dly_fb+mutate_step(5));return "FB";}p.dly_mix=clamp8((int32_t)p.dly_mix+mutate_step(8));return "MIX";}
        if(k==0){p.dly_mod[0]=clamp8((int32_t)p.dly_mod[0]+mutate_step(8));return "MOD 1";}if(k==1){p.dly_mod[1]=clamp8((int32_t)p.dly_mod[1]+mutate_step(8));return "MOD 2";}if(k==2){p.dly_src^=1u;return "SOURCE";}p.dly_wave^=1u;return "WAVE";
    case PG_ARP:
        if(k==0){patch_set_bpm(&p,(uint32_t)clampi((int32_t)patch_bpm(&p)+mutate_step(12),10,400));return "BPM";}if(k==1){p.arp_steps=(uint8_t)clampi((int32_t)p.arp_steps+mutate_step(1),1,8);return "STEPS";}if(k==2){p.arp_gate=clamp8((int32_t)p.arp_gate+mutate_step(8));return "GATE";}p.arp_order=(uint8_t)(((int32_t)p.arp_order+(mutate_step(1)>0?1:8))%9);return "ORDER";
    default:
        if(ui.outsub){if(k==0){p.comp_thresh=clamp8((int32_t)p.comp_thresh+mutate_step(7));return "THRESH";}if(k==1){p.comp_ratio=clamp8((int32_t)p.comp_ratio+mutate_step(8));return "RATIO";}if(k==2){p.comp_attack=clamp8((int32_t)p.comp_attack+mutate_step(7));return "ATTACK";}p.comp_release=clamp8((int32_t)p.comp_release+mutate_step(7));return "RELEASE";}
        if(k==0){p.drive=clamp8((int32_t)p.drive+mutate_step(7));return "DRIVE";}if(k==1){p.drive_mix=clamp8((int32_t)p.drive_mix+mutate_step(8));return "D MIX";}if(k==2){p.drift=clamp8((int32_t)p.drift+mutate_step(6));return "DRIFT";}p.press=(uint8_t)clampi((int32_t)p.press+mutate_step(6),1,127);return "PRESS";
    }
}
static void mutation_begin(void)
{
    undo_patch = p;
    undo_valid = 1;
}

static void mutate_short(void)
{
    const char *lab;
    char m[24] = "MUTATE ";
    uint32_t k, n = 7, i;
    rs32 = rs32 * 1664525u + 1013904223u;
    k = (rs32 >> 16) & 3u;
    mutation_begin();
    lab = mutate_control(k);
    for (i = 0; lab[i] && n < sizeof m - 1u; i++) m[n++] = lab[i];
    m[n] = 0;
    dirty = 1; t_change = fm1_ms; to_audio(); say(m);
}

static void mutate_page(void)
{
    uint32_t k;
    mutation_begin();
    for (k = 0; k < 4u; k++) (void)mutate_control(k);
    dirty = 1; t_change = fm1_ms; to_audio(); say("PAGE MUTATED");
}

static void mutate_undo(void)
{
    if (!undo_valid) { say("NOTHING TO UNDO"); return; }
    p = undo_patch;
    undo_valid = 0;
    dirty = 1; t_change = fm1_ms; to_audio(); gates(); say("MUTATE UNDONE");
}

/* ---------------------------------------------------------------- settings */
typedef struct {
    uint32_t magic;
    uint8_t cur[PATCH_V13_SIZE];
    uint8_t page, voice, pair, preset, uslot, ch, latch, pal, fxsub;
    int8_t transpose;
} settings_v13_t;
typedef char settings_v13_size_check[(sizeof(settings_v13_t) == 84u) ? 1 : -1];

typedef struct {
    uint32_t magic;
    uint8_t cur[PATCH_V15_SIZE];
    uint8_t page, voice, pair, preset, uslot, ch, latch, pal, fxsub, lfosub;
    int8_t transpose;
} settings_v15_t;
typedef char settings_v15_size_check[(sizeof(settings_v15_t) == 92u) ? 1 : -1];

typedef struct {
    uint32_t magic;
    uint8_t cur[PATCH_V16_SIZE];
    uint8_t page, voice, pair, preset, uslot, ch, latch, pal, fxsub, lfosub;
    int8_t transpose;
} settings_v16_t;
typedef char settings_v16_size_check[(sizeof(settings_v16_t) == 96u) ? 1 : -1];

typedef struct {
    uint32_t magic; uint8_t cur[PATCH_V17_SIZE];
    uint8_t page, voice, pair, preset, uslot, ch, latch, pal, fxsub, lfosub;
    int8_t transpose;
} settings_v17_t;

typedef struct {uint32_t magic;uint8_t cur[PATCH_V182_SIZE];uint8_t page,voice,pair,preset,uslot,ch,latch,pal,fxsub,lfosub;int8_t transpose;} settings_v182_t;
typedef struct {
    uint32_t magic; uint8_t cur[PATCH_V181_SIZE];
    uint8_t page, voice, pair, preset, uslot, ch, latch, pal, fxsub, lfosub;
    int8_t transpose;
} settings_v181_t;

typedef struct {
    uint32_t magic;
    patch_t cur;
    uint8_t page, voice, pair, preset, uslot, ch, latch, pal, fxsub, lfosub;
    int8_t transpose;
} settings_t;
#define SET_MAGIC 0x41523138u

static void settings_write(void)
{
    settings_t s;
    uint32_t i;
    uint8_t *d = (uint8_t *)&s;
    for (i = 0; i < sizeof s; i++) d[i] = 0;
    s.magic = SET_MAGIC;
    s.cur = p;
    s.page = ui.page; s.voice = ui.voice; s.pair = ui.pair; s.preset = ui.preset;
    s.uslot = ui.uslot; s.ch = ui.ch; s.latch = ui.latch; s.pal = ui.pal; s.fxsub = ui.fxsub; s.lfosub = ui.lfosub;
    s.transpose = ui_transpose;
    settings_save(&s, sizeof s);
}

static void settings_read(void)
{
    settings_t s;
    settings_v182_t v182;
    settings_v181_t v181;
    settings_v17_t v17;
    settings_v16_t v16;
    settings_v15_t v15;
    settings_v13_t old;
    uint32_t migrated = 0;
    if (!settings_load(&s, sizeof s) && s.magic == SET_MAGIC) {
        p = s.cur;
        ui.page = s.page < NPAGE ? s.page : PG_VOICE;
        ui.voice = s.voice < NV ? s.voice : 0;
        ui.pair = s.pair < NP ? s.pair : 0;
        ui.preset = s.preset < NFACT + NUSER ? s.preset : 0;
        ui.uslot = s.uslot < NUSER ? s.uslot : 0;
        ui.ch = s.ch <= 16u ? s.ch : 0;
        ui.latch = s.latch ? 1u : 0u;
        ui.pal = (uint8_t)(s.pal % NPALETTES);
        ui.fxsub = s.fxsub ? 1u : 0u; ui.lfosub = s.lfosub ? 1u : 0u;
        ui_transpose = (int8_t)clampi(s.transpose, -24, 24);
    } else if (!settings_load(&v182,sizeof v182) && v182.magic==SET_MAGIC) {
        patch_from_v182(&p,v182.cur);ui.page=v182.page<NPAGE?v182.page:PG_VOICE;ui.voice=v182.voice<NV?v182.voice:0;ui.pair=v182.pair<NP?v182.pair:0;ui.preset=v182.preset<NFACT+NUSER?v182.preset:0;ui.uslot=v182.uslot<NUSER?v182.uslot:0;ui.ch=v182.ch<=16u?v182.ch:0;ui.latch=v182.latch?1u:0u;ui.pal=(uint8_t)(v182.pal%NPALETTES);ui.fxsub=v182.fxsub?1u:0u;ui.lfosub=v182.lfosub?1u:0u;ui_transpose=(int8_t)clampi(v182.transpose,-24,24);migrated=1;
    } else if (!settings_load(&v181,sizeof v181) && v181.magic==SET_MAGIC) {
        patch_from_v181(&p,v181.cur);ui.page=v181.page<NPAGE?v181.page:PG_VOICE;ui.voice=v181.voice<NV?v181.voice:0;ui.pair=v181.pair<NP?v181.pair:0;ui.preset=v181.preset<NFACT+NUSER?v181.preset:0;ui.uslot=v181.uslot<NUSER?v181.uslot:0;ui.ch=v181.ch<=16u?v181.ch:0;ui.latch=v181.latch?1u:0u;ui.pal=(uint8_t)(v181.pal%NPALETTES);ui.fxsub=v181.fxsub?1u:0u;ui.lfosub=v181.lfosub?1u:0u;ui_transpose=(int8_t)clampi(v181.transpose,-24,24);migrated=1;
    } else if (!settings_load(&v17,sizeof v17) && v17.magic==SET_MAGIC) {
        patch_from_v17(&p,v17.cur); ui.page=v17.page<NPAGE?v17.page:PG_VOICE;ui.voice=v17.voice<NV?v17.voice:0;ui.pair=v17.pair<NP?v17.pair:0;ui.preset=v17.preset<NFACT+NUSER?v17.preset:0;ui.uslot=v17.uslot<NUSER?v17.uslot:0;ui.ch=v17.ch<=16u?v17.ch:0;ui.latch=v17.latch?1u:0u;ui.pal=(uint8_t)(v17.pal%NPALETTES);ui.fxsub=v17.fxsub?1u:0u;ui.lfosub=v17.lfosub?1u:0u;ui_transpose=(int8_t)clampi(v17.transpose,-24,24);migrated=1;
    } else if (!settings_load(&v16, sizeof v16) && v16.magic == SET_MAGIC_V16) {
        patch_from_v16(&p, v16.cur);
        ui.page=v16.page<NPAGE?v16.page:PG_VOICE; ui.voice=v16.voice<NV?v16.voice:0; ui.pair=v16.pair<NP?v16.pair:0;
        ui.preset=v16.preset<NFACT+NUSER?v16.preset:0; ui.uslot=v16.uslot<NUSER?v16.uslot:0; ui.ch=v16.ch<=16u?v16.ch:0;
        ui.latch=v16.latch?1u:0u; ui.pal=(uint8_t)(v16.pal%NPALETTES); ui.fxsub=v16.fxsub?1u:0u; ui.lfosub=v16.lfosub?1u:0u;
        ui_transpose=(int8_t)clampi(v16.transpose,-24,24); migrated=1;
    } else if (!settings_load(&v15, sizeof v15) && v15.magic == SET_MAGIC_V15) {
        patch_from_v15(&p, v15.cur);
        ui.page = v15.page == 5u ? PG_OUT : (v15.page < 5u ? v15.page : PG_VOICE);
        ui.voice = v15.voice < NV ? v15.voice : 0; ui.pair = v15.pair < NP ? v15.pair : 0;
        ui.preset = v15.preset < NFACT + NUSER ? v15.preset : 0; ui.uslot = v15.uslot < NUSER ? v15.uslot : 0;
        ui.ch = v15.ch <= 16u ? v15.ch : 0; ui.latch = v15.latch ? 1u : 0u; ui.pal = (uint8_t)(v15.pal % NPALETTES);
        ui.fxsub = v15.fxsub ? 1u : 0u; ui.lfosub = v15.lfosub ? 1u : 0u;
        ui_transpose = (int8_t)clampi(v15.transpose, -24, 24); migrated = 1;
    } else if (!settings_load(&old, sizeof old) && old.magic == SET_MAGIC_V13) {
        patch_from_v13(&p, old.cur);
        ui.page = old.page == 5u ? PG_OUT : (old.page < 5u ? old.page : PG_VOICE);
        ui.voice = old.voice < NV ? old.voice : 0;
        ui.pair = old.pair < NP ? old.pair : 0;
        ui.preset = old.preset < NFACT + NUSER ? old.preset : 0;
        ui.uslot = old.uslot < NUSER ? old.uslot : 0;
        ui.ch = old.ch <= 16u ? old.ch : 0;
        ui.latch = old.latch ? 1u : 0u;
        ui.pal = (uint8_t)(old.pal % NPALETTES);
        ui.fxsub = old.fxsub ? 1u : 0u;
        ui.lfosub = 0;
        ui_transpose = (int8_t)clampi(old.transpose, -24, 24);
        migrated = 1;
    } else {
        p = FACTORY[1];                         /* DRONE MASS: it sounds as it is switched on */
        ui.preset = 1;
        ui.pal = 7;
    }
    comp_defaults(&p); env_defaults(&p);
    if (migrated) { dirty = 1; t_change = fm1_ms; }
}

static uint32_t arp_period(void) { return 60000u / patch_bpm(&p); }
static uint8_t arp_voice(uint32_t pos,uint32_t steps,uint32_t order)
{
    uint32_t half;if(steps<=1u)return 0;if(order==1u)return(uint8_t)(steps-1u-pos);if(order==2u)return(uint8_t)(pos<steps?pos:2u*steps-2u-pos);if(order==4u){half=(steps+1u)/2u;return(uint8_t)(pos<half?pos*2u:(pos-half)*2u+1u);}if(order==5u)return arp_voice(steps-1u-pos,steps,4u);if(order==6u)return(uint8_t)((pos&1u)?steps-1u-pos/2u:pos/2u);if(order==7u)return arp_voice(steps-1u-pos,steps,6u);return(uint8_t)pos;
}
static void arp_stop(void){arp_running=0;arp_mask=0;gates();}
static void arp_start(void){arp_running=1;arp_step=0;arp_direction=0;arp_display_step=0;arp_event_count=0;arp_mask=0;arp_fraction=0;arp_learn_mask=0;arp_learn_active=0;arp_next_ms=fm1_ms;}
static void arp_tick(void)
{
    uint32_t now=fm1_ms,period,gate_ms,steps=p.arp_steps,cycle;if(!arp_running||!steps)return;if(arp_mask&&(int32_t)(now-arp_off_ms)>=0){arp_mask=0;gates();}if((int32_t)(now-arp_next_ms)<0)return;period=arp_period();cycle=(p.arp_order==2u&&steps>1u)?2u*steps-2u:steps;if(p.arp_order==3u){rs32=rs32*1664525u+1013904223u;arp_step=(uint8_t)(rs32%steps);}else arp_step=arp_voice(arp_direction,steps,p.arp_order);arp_display_step=(uint8_t)(arp_event_count%steps);arp_event_count++;arp_mask=p.arp_order==8u?p.arp_pattern[arp_direction]:(1u<<arp_step);gate_ms=period*(1u+(uint32_t)p.arp_gate*99u/127u)/100u;if(!gate_ms)gate_ms=1u;arp_off_ms=now+gate_ms; arp_next_ms+=period; arp_fraction+=60000u%patch_bpm(&p); if(arp_fraction>=patch_bpm(&p)){arp_fraction-=patch_bpm(&p);arp_next_ms++;} gates();if(p.arp_order!=3u)arp_direction=(uint8_t)((arp_direction+1u)%cycle);
}
static void arp_learn_note(uint32_t voice)
{
    if (voice < NV) { arp_learn_mask |= (uint8_t)(1u << voice); arp_learn_active = 1; }
}
static void arp_learn_commit(void)
{
    if (!arp_learn_active || !arp_learn_mask) return;
    p.arp_pattern[arp_learn_step] = arp_learn_mask;
    arp_learn_step = (uint8_t)((arp_learn_step + 1u) % p.arp_steps);
    arp_learn_mask = 0; arp_learn_active = 0; dirty = 1; t_change = fm1_ms;
    say("ARP STEP SET");
}

/* ---------------------------------------------------------------- main */
/* fm1-dock: app_main's loop-wide variables, for app_init and app_loop (modules/artihive/take.py) */
static uint32_t prev_keys = 0, t_draw = 0, save_hold = 0, mutate_hold = 0, theme_hold = 0;
static uint8_t save_suppress = 0;
static uint32_t k;
static void app_init(void)                      /* fm1-dock: app_main up to its loop */
{
    settings_read();
    if (blob_load(0, user, sizeof user)) {
        uint8_t old_user[NUSER * PATCH_V182_SIZE];
        if (!blob_load(0, old_user, NUSER * PATCH_V182_SIZE)) {for(k=0;k<NUSER;k++)patch_from_v182(&user[k],old_user+k*PATCH_V182_SIZE);
        } else if (!blob_load(0, old_user, NUSER * PATCH_V181_SIZE)) {
            for (k = 0; k < NUSER; k++) patch_from_v181(&user[k], old_user + k * PATCH_V181_SIZE);
        } else if (!blob_load(0, old_user, NUSER * PATCH_V17_SIZE)) {
            for (k = 0; k < NUSER; k++) patch_from_v17(&user[k], old_user + k * PATCH_V17_SIZE);
        } else if (!blob_load(0, old_user, NUSER * PATCH_V16_SIZE)) {
            for (k = 0; k < NUSER; k++) patch_from_v16(&user[k], old_user + k * PATCH_V16_SIZE);
        } else if (!blob_load(0, old_user, NUSER * PATCH_V15_SIZE)) {
            for (k = 0; k < NUSER; k++) patch_from_v15(&user[k], old_user + k * PATCH_V15_SIZE);
        } else if (!blob_load(0, old_user, NUSER * PATCH_V13_SIZE)) {
            for (k = 0; k < NUSER; k++) patch_from_v13(&user[k], old_user + k * PATCH_V13_SIZE);
        } else {                                  /* no user slots yet: fill from the factory */
            for (k = 0; k < NUSER; k++) {
                user[k] = FACTORY[NFACT - 1u];
                user[k].name[0] = 'U'; user[k].name[1] = (char)('1' + k); user[k].name[2] = 0;
                env_defaults(&user[k]); user[k].arp_rate = 120u; user[k].arp_steps = 8u; user[k].arp_gate = 64u; { uint32_t j; for(j=0;j<8u;j++)user[k].arp_pattern[j]=(uint8_t)(1u<<j); }
            }
        }
    }
    palette_set(ui.pal);
    draw_boot();
#ifndef BOOT_HOLD_MS
#define BOOT_HOLD_MS 3600u
#endif
    {
        uint32_t boot_ms = fm1_ms, last_pct = 999u, fixed = 0, tables_ok;
        boot_progress("SETTINGS", 5u);
        fixed += patch_sanitize(&p);
        boot_progress("PRESETS", 20u);
        for (k = 0; k < NUSER; k++) fixed += patch_sanitize(&user[k]);
        boot_progress("DSP TABLES", 35u);
        tables_ok = dsp_tables_ok();
        boot_progress("AUDIO MEMORY", 50u);
        to_audio();
        gates();
        while (0) {                             /* fm1-dock: no boot hold (a module's init returns) */
            uint32_t elapsed = fm1_ms - boot_ms;
            uint32_t pct = 50u + elapsed * 48u / BOOT_HOLD_MS;
            platform_poll();
            if (pct != last_pct && !(pct & 3u)) {
                boot_progress(pct < 72u ? "PARAMETERS" : pct < 92u ? "AUDIO" : "READY", pct);
                last_pct = pct;
            }
        }
        boot_progress(tables_ok ? (fixed ? "FIXED / READY" : "READY") : "TABLE ERROR", 100u);
    }
    draw_panel_bg();
    draw_header();
    draw_page();
    draw_body();
    draw_voices();
    draw_status();
}

static void app_loop(void)                      /* fm1-dock: a pass of app_main's loop, from the host's draw() */
{
    do {
        uint32_t keys, down, up, rel, b, redraw = 0, sensor_held = 0, learning;
        int32_t e;
        platform_poll();
        arp_tick();

        keys = fm1_in.notes;
        down = keys & ~prev_keys;
        up = prev_keys & ~keys;
        prev_keys = keys;
        learning = ui.page == PG_ARP && p.arp_order == 8u && !arp_running;
        if(a_poly){
            for(k=0;k<27u;k++){
                if(down&(1u<<k)){uint32_t v=poly_alloc(k);poly_key_voice[k]=(int8_t)v;poly_voice_key[v]=(int8_t)k;poly_age[v]=++poly_clock;vvel[v]=127u;touch_mask|=1u<<v;poly_set_pitch(v,k);ui.voice=(uint8_t)v;ui.pair=(uint8_t)(v>>1);midi_send(0x90u|(ui.ch?ui.ch-1u:0u),(uint8_t)(12u*(poly_octave+1u)+poly_root+k),127u);redraw=1;}
                if(up&(1u<<k)){int32_t v=poly_key_voice[k];if(v>=0){touch_mask&=~(1u<<(uint32_t)v);vvel[(uint32_t)v]=0;poly_voice_key[(uint32_t)v]=-1;poly_key_voice[k]=-1;midi_send(0x80u|(ui.ch?ui.ch-1u:0u),(uint8_t)(12u*(poly_octave+1u)+poly_root+k),0);}}
            }
        }else{
        for (k = 0; k < 8u; k++) {              /* the lower eight white keys: the sensors */
            uint32_t key = WKEY[k];
            if (keys >> key & 1u) sensor_held |= 1u << k;
            if (down >> key & 1u) {
                if (learning) arp_learn_note(k);
                else if (ui.latch) {
                    latch_mask ^= 1u << k;
                    if (latch_mask >> k & 1u) vvel[k] = p.press;
                } else {
                    touch_mask |= 1u << k;
                    vvel[k] = p.press;
                }
                ui.voice = (uint8_t)k;
                ui.pair = (uint8_t)(k >> 1);
                redraw = 1;
                midi_send(0x90u | (ui.ch ? ui.ch - 1u : 0u), 60u + k, p.press);
            }
            if ((up >> key & 1u) && !ui.latch && !learning) {
                touch_mask &= ~(1u << k);
                midi_send(0x80u | (ui.ch ? ui.ch - 1u : 0u), 60u + k, 0);
            }
        }
        if (learning && arp_learn_active && !sensor_held) { arp_learn_commit(); redraw = 1; }
        for (k = 0; k < 8u; k++) {              /* the upper eight: latch that voice as a drone */
            uint32_t key = WKEY[8u + k];
            if (down >> key & 1u) {
                latch_mask ^= 1u << k;
                if (latch_mask >> k & 1u) vvel[k] = p.press;
                redraw = 1;
            }
        }
        }
        gates();

        b = fm1_input_edges(&rel);
        if (b & (1u << BTN_HOME)) { if(ui.page==PG_VOICE){ui.polysub^=1u;a_poly=ui.polysub;if(a_poly){arp_stop();a_started=1;a_mute=0;ui.latch=0;poly_reset();}else{poly_reset();to_audio();}}else ui.page=PG_VOICE;redraw=1; }
        if (b & (1u << BTN_EDIT)) { ui.page = PG_FM; redraw = 1; }
        if (b & (1u << BTN_ENV))  { ui.page = PG_ENV; redraw = 1; }
        if (b & (1u << BTN_LFO))  { if (ui.page == PG_LFO) ui.lfosub ^= 1u; else { ui.page = PG_LFO; ui.lfosub = 0; } redraw = 1; }
        if (b & (1u << BTN_FX))   { if (ui.page == PG_DLY) ui.fxsub ^= 1u; else ui.page = PG_DLY; redraw = 1; }
        if (b & (1u << BTN_GLO))  { if(ui.page==PG_OUT)ui.outsub^=1u;else{ui.page=PG_OUT;ui.outsub=0;} redraw=1; }
        if (b & (1u << BTN_SEL))  {
            a_dly_freeze ^= 1u;
            say(a_dly_freeze ? "FREEZE ON" : "FREEZE OFF");
            redraw = 1;
        }
        if ((b & (1u << BTN_REC)) && !a_poly)  { ui.latch ^= 1u; if (!ui.latch) touch_mask = 0; redraw = 1; say(ui.latch ? "LATCH ON" : "LATCH OFF"); }
        if ((b & (1u << BTN_PLAY)) && !a_poly) {
            if (ui.page == PG_ARP) {
                if (!a_started) { a_started = 1; a_mute = 0; }
                if (arp_running) arp_stop();
                else arp_start();
            } else {
                if (!a_started) { a_started = 1; a_mute = 0; latch_mask = 255u; }
                else latch_mask = latch_mask ? 0u : 255u;
                gates(); say(latch_mask ? "ALL DRONES ON" : "ALL DRONES OFF");
            }
            redraw = 1;
        }
        if (b & (1u << BTN_SEQ)) {
            if (fm1_in.buttons & (1u << BTN_SAVE)) {
                mutate_undo(); save_suppress = 1; mutate_hold = 0; redraw = 1;
            } else mutate_hold = fm1_ms;
        }
        if (rel & (1u << BTN_SEQ)) {
            if (mutate_hold) {
                if (fm1_ms - mutate_hold > 700u) mutate_page(); else mutate_short();
                mutate_hold = 0; redraw = 1;
            }
        }
        if (b & (1u << BTN_ARP)) theme_hold = fm1_ms;
        if (rel & (1u << BTN_ARP)) {
            if (theme_hold && fm1_ms - theme_hold > 700u) {
                ui.pal = (uint8_t)((ui.pal + 1u) % NPALETTES); palette_set(ui.pal); draw_panel_bg(); dirty = 1; t_change = fm1_ms;
                say("COLOUR NEXT");
            } else ui.page = PG_ARP;
            theme_hold = 0; redraw = 1;
        }
        if (b & (1u << BTN_OCT_DOWN)) { if(a_poly){poly_octave=(uint8_t)clampi((int32_t)poly_octave-1,1,7);poly_retune();say("OCTAVE DOWN");}else{ui_transpose=(int8_t)clampi(ui_transpose-1,-24,24);to_audio();say("TRANSPOSE DOWN");}redraw=1; }
        if (b & (1u << BTN_OCT_UP))   { if(a_poly){poly_octave=(uint8_t)clampi((int32_t)poly_octave+1,1,7);poly_retune();say("OCTAVE UP");}else{ui_transpose=(int8_t)clampi(ui_transpose+1,-24,24);to_audio();say("TRANSPOSE UP");}redraw=1; }
        if (b & (1u << BTN_SAVE)) save_hold = fm1_ms;
        if (rel & (1u << BTN_SAVE)) {
            if (save_suppress) { save_suppress = 0; save_hold = 0; }
            else if (save_hold && fm1_ms - save_hold > 700u) {
                uint32_t i;
                user[ui.uslot] = p;
                for (i = 0; i < 6u; i++) user[ui.uslot].name[i] = 0;
                user[ui.uslot].name[0] = 'U';
                user[ui.uslot].name[1] = (char)('1' + ui.uslot);
                a_mute = 1;
                blob_save(0, user, sizeof user);   /* an erase stalls the sound: do it muted */
                a_mute = 0;
                ui.preset = (uint8_t)(NFACT + ui.uslot);
                say("SAVED TO USER");
            } else {
                ui.uslot = (uint8_t)((ui.uslot + 1u) % NUSER);
                say("USER SLOT NEXT");
            }
            save_hold = 0;
            redraw = 1;
        }

        if ((e = fm1_enc_take(ENC_SELECT)) != 0) {
            ui.voice = (uint8_t)(((int32_t)ui.voice + e) & 7);
            ui.pair = (uint8_t)(ui.voice >> 1);
            redraw = 1;
        }
        if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) {
            ui.pair = (uint8_t)(((int32_t)ui.pair + e) & 3);
            redraw = 1;
        }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {
            int32_t n = (int32_t)ui.preset + e;
            while (n < 0) n += (int32_t)(NFACT + NUSER);
            load_preset((uint32_t)n % (NFACT + NUSER));
            for (k = 0; k < NV; k++) vvel[k] = 0;
            redraw = 1;
            dirty = 1;
            t_change = fm1_ms;
        }
        for (k = 0; k < 4u; k++) {
            e = fm1_enc_take(ENC_KNOB1 + k);
            if (e) { knob(k, e); redraw = 1; }
        }

        midi_in();

        /* LEDs: chromatic POLY uses all 27 physical note keys. */
        if(a_poly)for(k=0;k<27u;k++)fm1_led_key(KEY_LED(k),poly_key_voice[k]>=0);
        if(!a_poly) for (k = 0; k < 8u; k++) {
            uint32_t on = ((touch_mask | latch_mask) >> k) & 1u;
            fm1_led_key(KEY_LED(WKEY[k]), on);
            fm1_led_key(KEY_LED(WKEY[8u + k]), (latch_mask >> k) & 1u);
            if (!on) fm1_led_key_glow(KEY_LED(WKEY[k]), k == ui.voice);
        }
        fm1_led_key(BTN_PLAY, latch_mask != 0);
        fm1_led_key(BTN_REC, ui.latch);
        fm1_led_key(BTN_SEL, a_dly_freeze);
        fm1_led_key(BTN_HOME, ui.page == PG_VOICE);
        fm1_led_key(BTN_EDIT, ui.page == PG_FM);
        fm1_led_key(BTN_ENV, ui.page == PG_ENV);
        fm1_led_key(BTN_LFO, ui.page == PG_LFO);
        fm1_led_key(BTN_FX, ui.page == PG_DLY);
        fm1_led_key(BTN_ARP, ui.page == PG_ARP || arp_running);
        fm1_led_key(BTN_GLO, ui.page == PG_OUT);

        idle_ok = 1;
        if (redraw) { draw_header(); draw_page(); }
        if (fm1_ms - t_draw > 60u) {            /* the moving parts, 16 times a second */
            t_draw = fm1_ms;
            draw_voices();
            draw_body();
            draw_status();
        }
        idle_ok = 0;

        if (dirty && fm1_ms - t_change > 2500u) {
            uint32_t quiet = 1;
            for (k = 0; k < NV; k++) if (a_gate[k] || v_env[k] > (200 << 16)) quiet = 0;
            if (quiet || fm1_ms - t_change > 30000u) {
                a_mute = 1;
                settings_write();
                a_mute = 0;
                dirty = 0;
            }
        }
    } while (0);
}
