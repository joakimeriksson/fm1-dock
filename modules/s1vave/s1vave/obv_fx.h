/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Andrew GG — Roland S-1 parity engine */
/* The effects after the output stage: a stereo chorus, a ping-pong delay in note values of the tempo, and a
 * plate reverb (J. Dattorro's figure-of-eight tank, 1997). All lines are int16 in one .pool buffer; the
 * signals are Q15 as the output stage leaves them. Integers only.
 * REV_DIV 1: the reverb at the full rate (the sound comes first). 2: at half the rate, if the chip turns out
 * to need it: a dark hall loses little by it and it halves the reverb's time and memory. */
#ifndef REV_DIV
#define REV_DIV 1u
#endif
#define RVL(x) ((uint32_t)((x) * 44100u / 29761u / REV_DIV))   /* the paper's 29761 Hz lengths at our rate */
#ifndef DLY_MAX                                  /* fm1-dock: settable (modules/s1vave/take.py) */
#define DLY_MAX 32768u                          /* a side of the delay: 743 ms */
#endif
#define CHO_N 1024u
#define REV_PRE_N (8192u / REV_DIV)             /* up to 120 ms of pre-delay */
#define RV_IN1 RVL(142)
#define RV_IN2 RVL(107)
#define RV_IN3 RVL(379)
#define RV_IN4 RVL(277)
#define RV_AP1 RVL(672)
#define RV_D1 RVL(4453)
#define RV_AP2 RVL(1800)
#define RV_D2 RVL(3720)
#define RV_AP3 RVL(908)
#define RV_D3 RVL(4217)
#define RV_AP4 RVL(2656)
#define RV_D4 RVL(3163)
#define REV_N (REV_PRE_N + RV_IN1 + RV_IN2 + RV_IN3 + RV_IN4 + RV_AP1 + 16u + RV_D1 + RV_AP2 + RV_D2 + RV_AP3 + 16u + RV_D3 + RV_AP4 + RV_D4)
#define RV_BW (REV_DIV == 2u ? 24000 : 16700)   /* the input's one-pole: ~5 kHz either way */
#define FX_MEM_N (CHO_N + 2u * DLY_MAX + REV_N)

typedef struct { int16_t *b; uint32_t len, w; } line_t;   /* w: where the next sample goes */
static int16_t fx_mem[FX_MEM_N];
static uint32_t fx_used;

static inline int32_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }
static void line_init(line_t *l, uint32_t len) { l->b = fx_mem + fx_used; l->len = len; l->w = 0; fx_used += len; }
static inline __attribute__((always_inline)) int32_t line_read(const line_t *l, uint32_t back)   /* 1 <= back <= len */
{
    if (back > l->len) back = l->len;
    if (back < 1u) back = 1u;
    uint32_t i = l->w + l->len - back;
    if (i >= l->len) i -= l->len;
    return l->b[i];
}
static inline __attribute__((always_inline)) void line_write(line_t *l, int32_t v)
{
    l->b[l->w] = (int16_t)sat16(v);
    if (++l->w >= l->len) l->w = 0;
}
/* Dattorro's allpass: the line holds the inner node, the output is tapped from it. g Q15, signed */
static inline __attribute__((always_inline)) int32_t ap_step(line_t *l, int32_t in, int32_t g)
{
    int32_t d = line_read(l, l->len), v = in - ((d * g) >> 15);
    line_write(l, v);
    return d + ((v * g) >> 15);
}
/* the same with the delay swaying by e (Q8 samples, |e| <= 8 << 8) around base; the line is base + 16 long */
static inline __attribute__((always_inline)) int32_t ap_mod(line_t *l, int32_t in, int32_t g, uint32_t base, int32_t e)
{
    uint32_t back = (uint32_t)((int32_t)base + (e >> 8));
    int32_t f = e & 255, a = line_read(l, back), b = line_read(l, back + 1u), d = a + (((b - a) * f) >> 8), v;
    v = in - ((d * g) >> 15);
    line_write(l, v);
    return d + ((v * g) >> 15);
}
static inline int32_t tri_q(uint32_t ph)        /* a triangle -2048..2047 (so Q8 = +-8 samples) */
{
    int32_t t = (int32_t)(ph >> 19);            /* 0..8191 */
    return t < 4096 ? t - 2048 : 6143 - t;
}

/* ---- the state */
static line_t cho, dl, dr, rv_pre, rv_in[4], rv_ap1, rv_d1, rv_ap2, rv_d2, rv_ap3, rv_d3, rv_ap4, rv_d4;
static uint32_t cho_ph, cho_inc, rv_ph1, rv_ph2, rv_ph_inc1 = 97391u * REV_DIV, rv_ph_inc2 = 68174u * REV_DIV;
static int32_t cho_depth_k, cho_mix, dly_mix, rev_mix, dly_fb, dly_tone_k, rev_decay, rev_damp_k, rev_bw = 16700, rev_mod_scale = 256;
static int32_t dly_time, dly_cur;               /* Q16 samples: the target and where the read is (it slews like a tape) */
static int32_t dly_lpl, dly_lpr, rv_hp_in, rv_lp_in, rv_lpl, rv_lpr, rv_yl, rv_yr;
static int32_t rv_hold;
static uint32_t rev_pre_n = 1, dly_active, rev_active;
static volatile uint32_t ctl_rev_eco = 0;       /* 1: half-rate reverb tank (~50% lower CPU) */
/* ADV FX (SEL pages): selectable rate reducer / distortion slot. */
static uint32_t rr_acc, rr_inc;                           /* the rate reducer's phase accumulator */
static int32_t rr_hold_l, rr_hold_r, rr_shift;
static int32_t fx_drive, fx_makeup, fx_wet;               /* distortion (Q8), shared mix */
static uint32_t fx_is_dist, fx_amt;

static uint32_t fx_odd;

static void fx_init(void)
{
    line_init(&cho, CHO_N);
    line_init(&dl, DLY_MAX);
    line_init(&dr, DLY_MAX);
    line_init(&rv_pre, REV_PRE_N);
    line_init(&rv_in[0], RV_IN1); line_init(&rv_in[1], RV_IN2); line_init(&rv_in[2], RV_IN3); line_init(&rv_in[3], RV_IN4);
    line_init(&rv_ap1, RV_AP1 + 16u); line_init(&rv_d1, RV_D1); line_init(&rv_ap2, RV_AP2); line_init(&rv_d2, RV_D2);
    line_init(&rv_ap3, RV_AP3 + 16u); line_init(&rv_d3, RV_D3); line_init(&rv_ap4, RV_AP4); line_init(&rv_d4, RV_D4);
    dly_cur = dly_time = 11025 << 16;
}

/* the patch's numbers, once per ISR call. quarter: frames per quarter note (the arp's clock) */
static void fx_block(const patch_t *p, uint32_t quarter)
{
    int32_t d;
    uint32_t samples;
    uint32_t cho_m = p->v[P_CHO_MODE];

    /* Chorus 4 modes */
    switch (cho_m) {
    case CHO_TYPE1:
        cho_mix = 70;
        cho_inc = 50u * 974u;   /* ~0.5 Hz */
        cho_depth_k = 35 * 67;
        break;
    case CHO_TYPE2:
        cho_mix = 85;
        cho_inc = 120u * 974u;  /* ~1.2 Hz */
        cho_depth_k = 50 * 67;
        break;
    case CHO_TYPE3:
        cho_mix = 95;
        cho_inc = 400u * 974u;  /* ~4.0 Hz */
        cho_depth_k = 25 * 67;
        break;
    case CHO_TYPE4:
        cho_mix = 80;
        cho_inc = 30u * 974u;   /* ~0.3 Hz */
        cho_depth_k = 65 * 67;
        break;
    default:
        cho_mix = 0;
        cho_inc = 0;
        cho_depth_k = 0;
        break;
    }

    /* ADV FX taps: the shared reducer / distortion slot */
    fx_is_dist = p->v[P_FX_MODE] != 0;
    {
        int32_t amt = p->v[P_FX_AMT];                        /* shared amount */
        rr_inc = 65536u / (1u + (uint32_t)(amt * 31) / 100u);  /* hold 1..32 samples */
        rr_shift = (uint32_t)amt / 25u;                        /* bit crush 0..4 */
        fx_drive = 256 + amt * 16;                             /* 1x .. 7.3x */
        fx_makeup = (int32_t)(65536u / (uint32_t)fx_drive);   /* level match */
        fx_wet = (p->v[P_FX_MIX] * 256) / 100;                 /* shared dry..wet */
        fx_amt = (amt != 0);
    }

    dly_mix = (p->v[P_DLY_LVL] * 200) >> 7;     /* wet up to 0.78 */
    rev_mix = (p->v[P_REV_LVL] * 160) >> 7;     /* wet up to 0.625 (calibrated to Roland S-1 level) */
    dly_fb = p->v[P_DLY_FB] * 250;              /* up to 0.97 */
    dly_tone_k = 180;                           /* ~7 kHz tone damping */

    /* Roland S-1 Reverb Profiles: Amb, Room, Hall1, Hall2, Plate, Spring, Mod */
    {
        uint32_t rev_m = p->v[P_REV_TYPE];
        int32_t bw, damp, dec_base, dec_range, mod_scale;
        uint32_t inc1, inc2;

        switch (rev_m) {
        case REV_AMB:    /* 0: Ambience - tight small space, heavy damping, fast decay */
            bw = (REV_DIV == 2u ? 20000 : 16000);  /* ~4.5 kHz */
            damp = 65;                             /* controlled high-cut */
            dec_base = 8000; dec_range = 11000;    /* RT60: 0.89s .. 2.30s tight intimate room */
            mod_scale = 140;
            inc1 = 70000u * REV_DIV; inc2 = 50000u * REV_DIV;
            break;
        case REV_ROOM:   /* 1: Room - natural room acoustic */
            bw = (REV_DIV == 2u ? 24000 : 19000);  /* ~6.0 kHz */
            damp = 45;                             /* natural acoustic absorption */
            dec_base = 10000; dec_range = 12500;   /* RT60: 1.05s .. 3.33s natural room */
            mod_scale = 180;
            inc1 = 90000u * REV_DIV; inc2 = 64000u * REV_DIV;
            break;
        case REV_HALL1:  /* 2: Hall 1 - clear concert hall, expansive decay */
        default:
            bw = (REV_DIV == 2u ? 27000 : 22000);  /* ~8.5 kHz */
            damp = 32;                             /* warm subtle damping, clear and spacious */
            dec_base = 14000; dec_range = 14500;   /* RT60: 1.47s .. 8.97s clear concert hall */
            mod_scale = 220;
            inc1 = 97391u * REV_DIV; inc2 = 68174u * REV_DIV;
            break;
        case REV_HALL2:  /* 3: Hall 2 - mild warm concert hall, cavernous */
            bw = (REV_DIV == 2u ? 25000 : 20000);  /* ~7.0 kHz (mild/warmer than Hall 1) */
            damp = 42;                             /* milder, softer highs */
            dec_base = 14000; dec_range = 15000;   /* RT60: 1.47s .. 10.25s cavernous mild hall */
            mod_scale = 240;
            inc1 = 97391u * REV_DIV; inc2 = 68174u * REV_DIV;
            break;
        case REV_PLATE:  /* 4: Plate - dense metallic plate, sparkling highs */
            bw = (REV_DIV == 2u ? 32000 : 28000);  /* ~14 kHz, wide open metallic sparkle */
            damp = 16;                             /* minimal damping, sparkling highs */
            dec_base = 12000; dec_range = 16800;   /* RT60: 1.25s .. 9.70s shimmering plate */
            mod_scale = 180;                       /* subtle flutter */
            inc1 = 120000u * REV_DIV; inc2 = 85000u * REV_DIV;
            break;
        case REV_SPRING: /* 5: Spring - guitar amp spring tank, distinct boomy chirp */
            bw = (REV_DIV == 2u ? 18000 : 15000);  /* ~4.5 kHz, spring body */
            damp = 55;                             /* spring tank damping */
            dec_base = 12000; dec_range = 14000;   /* RT60: 1.25s .. 5.41s bouncy spring tank ring */
            mod_scale = 260;                       /* high flutter bounce */
            inc1 = 280000u * REV_DIV; inc2 = 205000u * REV_DIV; /* fast flutter chirp */
            break;
        case REV_MOD:    /* 6: Modulated - Roland signature lush chorused shimmer */
            bw = (REV_DIV == 2u ? 28000 : 24000);  /* ~10 kHz */
            damp = 24;                             /* luminous clean highs */
            dec_base = 16000; dec_range = 14000;   /* RT60: 1.75s .. 14.19s lush infinite singing tail */
            mod_scale = 360;                       /* rich chorused pitch swirl */
            inc1 = 55000u * REV_DIV; inc2 = 38000u * REV_DIV;   /* slow undulating shimmer */
            break;
        }

        rev_bw = bw;
        rev_mod_scale = mod_scale;
        rv_ph_inc1 = inc1;
        rv_ph_inc2 = inc2;

        d = p->v[P_REV_TIME];
        /* Perceptual curve calibrated to Roland S-1 RT60 times */
        int32_t t_norm = (d * 115 + ((d * d * 25) >> 7));
        rev_decay = dec_base + (int32_t)(((int64_t)dec_range * t_norm) >> 15);
        if (rev_decay > 31000) rev_decay = 31000; /* Safe stability boundary (< 32768) */
        rev_damp_k = 32768 - damp * damp * 2;
        if (REV_DIV == 1u) {                        /* the same corner at the full rate: k' = 1 - sqrt(1 - k) */
            uint32_t x = (uint32_t)(32768 - rev_damp_k) << 15, r = 0, bit = 1u << 15;
            for (; bit; bit >>= 1)
                if ((r + bit) * (r + bit) <= x) r += bit;
            rev_damp_k = 32768 - (int32_t)r;
        }
    }
    rev_pre_n = 1u + (uint32_t)p->v[P_REV_PRE] * (41u / REV_DIV);
    if (rev_pre_n >= REV_PRE_N) rev_pre_n = REV_PRE_N - 1u;
    if (p->v[P_DLY_SYNC]) {
        uint32_t dt = p->v[P_DLY_TIME];
        uint32_t div = (dt * 16u) / 128u;
        if (div >= NDDIV) div = NDDIV - 1u;
        samples = (quarter * DDIV_NUM[div]) / DDIV_DEN[div];
    } else {
        /* Roland S-1 Free Delay Time: 1 ms .. 740 ms piecewise linear curve */
        static const uint8_t ANCHOR_RAW[8] = {0, 11, 26, 55, 106, 179, 245, 255};
        static const uint16_t ANCHOR_MS[8] = {1, 14, 44, 103, 273, 517, 737, 740};
        uint32_t raw = (uint32_t)p->v[P_DLY_TIME] * 255u / 127u;
        uint32_t ms = 740;
        for (uint32_t k = 0; k < 7u; k++) {
            if (raw <= ANCHOR_RAW[k + 1]) {
                uint32_t num = raw - ANCHOR_RAW[k];
                uint32_t den = ANCHOR_RAW[k + 1] - ANCHOR_RAW[k];
                ms = ANCHOR_MS[k] + (num * (ANCHOR_MS[k + 1] - ANCHOR_MS[k])) / den;
                break;
            }
        }
        samples = (ms * 44100u) / 1000u;
    }
    if (samples > DLY_MAX - 4u) samples = DLY_MAX - 4u;
    if (samples < 64u) samples = 64u;
    dly_time = (int32_t)(samples << 16);
    if (dly_mix) dly_active = 12u * FS; else if (dly_active > HALF_FRAMES) dly_active -= HALF_FRAMES; else dly_active = 0;
    if (rev_mix) rev_active = 24u * FS; else if (rev_active > HALF_FRAMES) rev_active -= HALF_FRAMES; else rev_active = 0;
}

static inline __attribute__((always_inline)) int32_t line_read_q16(const line_t *l, int32_t back_q16)
{
    if (back_q16 < 0) back_q16 = 0;
    uint32_t b = (uint32_t)back_q16 >> 16;
    int32_t f = (back_q16 >> 8) & 255, a = line_read(l, b), c = line_read(l, b + 1u);
    return a + (((c - a) * f) >> 8);
}
/* 4-point Hermite for a tap that is always moving (the chorus): no treble loss between the samples */
static inline __attribute__((always_inline)) int32_t line_read_q16_h(const line_t *l, int32_t back_q16)
{
    if (back_q16 < (1 << 16)) back_q16 = 1 << 16;
    uint32_t b = (uint32_t)back_q16 >> 16;
    if (b + 2u >= l->len) b = l->len - 3u;
    int32_t f = (back_q16 >> 8) & 255, p0 = line_read(l, b - 1u), p1 = line_read(l, b), p2 = line_read(l, b + 1u), p3 = line_read(l, b + 2u);
    int32_t c1 = (p2 - p0) >> 1, c2 = p0 - ((5 * p1) >> 1) + 2 * p2 - (p3 >> 1), c3 = ((p3 - p0) >> 1) + ((3 * (p1 - p2)) >> 1);
    return p1 + ((f * (c1 + ((f * (c2 + ((f * c3) >> 8))) >> 8))) >> 8);
}

/* one reverb sample at the half rate: in -> pre-delay -> bandwidth -> 4 diffusers -> the two tank halves */
static inline void rev_tick(int32_t in)
{
    int32_t x, a, b;
    int32_t e1 = (tri_q(rv_ph1) * rev_mod_scale) >> 8;
    int32_t e2 = (tri_q(rv_ph2) * rev_mod_scale) >> 8;
    int32_t dec = rev_decay, dk = rev_damp_k;
    rv_ph1 += rv_ph_inc1;
    rv_ph2 += rv_ph_inc2;
    line_write(&rv_pre, in);
    x = line_read(&rv_pre, rev_pre_n);
    /* Roland S-1 Reverb Low-Cut (~200 Hz): removes sub-bass muddiness from tank */
    rv_hp_in += ((x - rv_hp_in) * 1870) >> 15;
    x -= rv_hp_in;
    rv_lp_in += ((x - rv_lp_in) * rev_bw) >> 15;
    x = ap_step(&rv_in[0], rv_lp_in, 24576);
    x = ap_step(&rv_in[1], x, 24576);
    x = ap_step(&rv_in[2], x, 20480);
    x = ap_step(&rv_in[3], x, 20480);
    /* left half, fed by the right's end */
    a = x + ((line_read(&rv_d4, RV_D4) * dec) >> 15);
    a = ap_mod(&rv_ap1, a, -22938, RV_AP1, e1);
    line_write(&rv_d1, a);
    b = line_read(&rv_d1, RV_D1);
    rv_lpl += ((b - rv_lpl) * dk) >> 15;
    b = ap_step(&rv_ap2, (rv_lpl * dec) >> 15, 16384);
    line_write(&rv_d2, b);
    /* right half, fed by the left's end */
    a = x + ((line_read(&rv_d2, RV_D2) * dec) >> 15);
    a = ap_mod(&rv_ap3, a, -22938, RV_AP3, e2);
    line_write(&rv_d3, a);
    b = line_read(&rv_d3, RV_D3);
    rv_lpr += ((b - rv_lpr) * dk) >> 15;
    b = ap_step(&rv_ap4, (rv_lpr * dec) >> 15, 16384);
    line_write(&rv_d4, b);
    /* the output taps (the paper's), calibrated x 0.65 */
    a = line_read(&rv_d3, RVL(266)) + line_read(&rv_d3, RVL(2974)) - line_read(&rv_ap4, RVL(1913)) + line_read(&rv_d4, RVL(1996))
      - line_read(&rv_d1, RVL(1990)) - line_read(&rv_ap2, RVL(187)) - line_read(&rv_d2, RVL(1066));
    b = line_read(&rv_d1, RVL(353)) + line_read(&rv_d1, RVL(3627)) - line_read(&rv_ap2, RVL(1228)) + line_read(&rv_d2, RVL(2673))
      - line_read(&rv_d3, RVL(2111)) - line_read(&rv_ap4, RVL(335)) - line_read(&rv_d4, RVL(121));
    rv_yl = (a * 21299) >> 15;
    rv_yr = (b * 21299) >> 15;
}

static void fx_process(int32_t *out, uint32_t n)
{
    uint32_t i, do_dly = dly_active != 0, do_rev = rev_active != 0;
    int32_t cm = cho_mix, dm = dly_mix, rm = rev_mix;
    for (i = 0; i < n; i++) {
        int32_t l = out[2u * i], r = out[2u * i + 1u];
        if (cm) {                               /* two taps a quarter turn apart on one line, around 12 ms */
            int32_t bl = (529 << 16) + tri_q(cho_ph) * cho_depth_k, br = (529 << 16) + tri_q(cho_ph + 0x40000000u) * cho_depth_k;
            cho_ph += cho_inc;
            line_write(&cho, (l + r) >> 1);
            l += ((line_read_q16_h(&cho, bl) - l) * cm) >> 8;
            r += ((line_read_q16_h(&cho, br) - r) * cm) >> 8;
        }
        if (do_dly) {                           /* Mono delay with exact timing across stereo/mono */
            int32_t w;
            if (dly_cur < dly_time) {
                int32_t step = (dly_time - dly_cur) >> 5;
                if (step < (1 << 16)) step = 1 << 16;
                dly_cur += step;
                if (dly_cur > dly_time) dly_cur = dly_time;
            } else if (dly_cur > dly_time) {
                int32_t step = (dly_cur - dly_time) >> 5;
                if (step < (1 << 16)) step = 1 << 16;
                dly_cur -= step;
                if (dly_cur < dly_time) dly_cur = dly_time;
            }
            w = line_read_q16(&dl, dly_cur);
            dly_lpl += ((w - dly_lpl) * dly_tone_k) >> 8;
            int32_t din = (l + r) >> 1;
            line_write(&dl, din + ((dly_lpl * dly_fb) >> 15));
            int32_t wet = (w * dm) >> 8;
            l += wet;
            r += wet;
        }
        if (do_rev) {
            if (ctl_rev_eco || REV_DIV == 2u) { /* half rate: a pair in, the output interpolated between ticks */
                int32_t pl = rv_yl, pr = rv_yr, din = l + r;
                if (!(fx_odd ^= 1u)) {
                    rev_tick((rv_hold + din) >> 2);
                    l += (((pl + rv_yl) >> 1) * rm) >> 8;
                    r += (((pr + rv_yr) >> 1) * rm) >> 8;
                } else {
                    l += (rv_yl * rm) >> 8;
                    r += (rv_yr * rm) >> 8;
                }
                rv_hold = din;
            } else {
                rev_tick((l + r) >> 1);
                l += (rv_yl * rm) >> 8;
                r += (rv_yr * rm) >> 8;
            }
        }
        if (!fx_amt) {                      /* bypass: the slot is off */
        } else {                            /* selector picks the engine; amount + mix are shared */
            int32_t l1 = l, r1 = r;
            if (fx_is_dist) {               /* distortion: drive into the softclip below */
                l1 = (softclip((l * fx_drive) >> 8) * fx_makeup) >> 8;
                r1 = (softclip((r * fx_drive) >> 8) * fx_makeup) >> 8;
            } else if (rr_inc < 65536u) {   /* rate reducer: sample & hold + bit crush */
                rr_acc += rr_inc;
                if (rr_acc >= 65536u) { rr_acc -= 65536u; rr_hold_l = l; rr_hold_r = r; }
                l1 = rr_hold_l; r1 = rr_hold_r;
                if (rr_shift) { l1 = (l1 >> rr_shift) << rr_shift; r1 = (r1 >> rr_shift) << rr_shift; }
            }
            l = l + (((l1 - l) * fx_wet) >> 8);   /* the shared dry..wet */
            r = r + (((r1 - r) * fx_wet) >> 8);
        }
        out[2u * i] = softclip(l);
        out[2u * i + 1u] = softclip(r);
    }
}
