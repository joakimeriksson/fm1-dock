/* setBfree's preamp (b_overdrive/overdrive.c) in integers: input gain and sag, an 8-tap interpolation filter, the biased transfer function
 * 1 / (1 + x^2) between a bass tone control before and one after it, global negative feedback, and a 33-tap (17 non-zero) decimation filter. */
#include <stdint.h>
/* OD_T[1025] = 32768 / (1 + x^2), x = 0 .. 16 in 1/64 steps, comes from organ_tab.h */
static int32_t od_xh[8], od_yh[32];
static uint32_t od_xi, od_yi;
static int32_t od_sag, od_z, od_z1, od_gf;   /* sag Q12, the two bass filters Q20, the feedback Q15 */
static int32_t od_t(int32_t x)               /* 1 / (1 + x^2) for x in Q15, result Q15 */
{
    uint32_t a = (uint32_t)(x < 0 ? -x : x), i, f;
    if (a > (1u << 21)) a = 1u << 21;
    i = a >> 9; f = a & 511u;
    int32_t t0, t1;
    if (i >= 1024u) {                        /* beyond 16: about 1 / x^2 */
        uint32_t q = a >> 5;                 /* Q10 */
        return (int32_t)((1u << 30) / ((q * q >> 10) + 1024u)) >> 5;
    }
    t0 = OD_T[i]; t1 = OD_T[i + 1u];
    return t0 + (((t1 - t0) * (int32_t)f) >> 9);
}
static void od_run(int32_t *x, uint32_t n, uint32_t chr)   /* x: Q15 in and out; chr: setBfree's overdrive.character 0 .. 127 */
{
    static const int32_t C[8] = {10, 41, 117, 1154, 3805, 117, 41, 10};            /* Q13 */
    static const int32_t A[8] = {6691, -2076, 1074, -607, 338, -175, 82, -40};      /* Q15, at distance 1, 3, 5 .. 15 */
    static const int32_t GIN = 29224 /* 3.5675 Q13 */, GOUT = 28820 /* 0.8795 Q15 */;
    int32_t fb, fb2;                                                                 /* Q15 */
    uint32_t i, k;
    if (chr < 32u) { fb = 19075; fb2 = 32735 + (int32_t)(((19075 - 32735) * (int32_t)chr) / 31); }
    else if (chr < 64u) { fb = 19075 + (int32_t)(((32735 - 19075) * (int32_t)(chr - 32u)) / 31); fb2 = 19075; }
    else { fb = 32735; fb2 = 19075 + (int32_t)(((32735 - 19075) * (int32_t)(chr - 64u)) / 63); }
    for (i = 0; i < n; i++) {
        int32_t xin, bias, norm, u, v, tm, ac, ex;
        int64_t m;
        xin = (x[i] * GIN) >> 13;                                         /* Q15 */
        od_sag -= (od_sag * 295) >> 15;                                   /* sag = 0.991 sag + |in|, Q12 */
        od_sag += (xin < 0 ? -xin : xin) >> 3;
        if (od_sag > (1 << 22)) od_sag = 1 << 22;
        bias = 17520 - ((od_sag * 308) >> 12);                            /* 0.5347 - 0.0094 sag, Q15 */
        norm = 32768 - od_t(bias);
        od_xi = (od_xi + 7u) & 7u; od_xh[od_xi] = xin;
        ac = 0;
        for (k = 0; k < 8u; k++) ac += C[k] * od_xh[(od_xi + k) & 7u];    /* Q28 */
        u = ac >> 8;                                                      /* Q20 */
        u += (od_gf * 20361) >> 10;                                       /* - gfb * gf, gfb = -0.6214: Q15 * Q15 -> Q20 */
        tm = u - od_z;
        m = (int64_t)od_z * fb;
        od_z = u + (int32_t)(m >> 15);
        u = tm;
        ex = u >> 5;                                                      /* Q15 */
        if (ex < 0) v = od_t(ex - bias) - 32768 + norm;
        else v = 32768 - norm - od_t(ex + bias);
        tm = (v << 5) + (int32_t)(((int64_t)od_z1 * fb2) >> 15);          /* Q20 */
        v = (tm - od_z1) >> 5;
        od_z1 = tm;
        od_gf = v;
        od_yi = (od_yi + 31u) & 31u; od_yh[od_yi] = v;
        ac = 10604 * od_yh[(od_yi + 16u) & 31u];                           /* the centre tap, 16 samples back; the pairs 1, 3 .. 15 either side */
        for (k = 0; k < 8u; k++)
            ac += A[k] * (od_yh[(od_yi + 15u - 2u * k) & 31u] + od_yh[(od_yi + 17u + 2u * k) & 31u]);
        x[i] = (int32_t)(((int64_t)ac * GOUT) >> 30);
    }
}
