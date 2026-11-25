/* SPDX-License-Identifier: GPL-3.0-only */
/* PACKUSE: a test module (tests/pack_test.py) that finds the data pack TESTPACK (hb_host_t.pack_find) at init, logs
 * its length and its CRC-32 as read in place from flash, and plays a key only while it has the pack: silence without
 * it. The pack's first byte sets the tone's loudness, so what it plays comes from the pack too. */
#include "hb_module.h"

static const hb_host_t *H;
static const uint8_t *pk;
static uint32_t pk_n, ph, inc;
static volatile uint32_t held;
static int32_t amp;

static void render(int32_t *out, uint32_t n)
{
    uint32_t i;
    if (!held || !pk)
        return;
    for (i = 0; i < n; i++) {                   /* a triangle, amp from the pack */
        int32_t t = (int32_t)(ph >> 16) - 32768;
        int32_t v = ((t < 0 ? -t : t) - 16384) * amp >> 8;
        out[2u * i] += v;
        out[2u * i + 1u] += v;
        ph += inc;
    }
}

static void note(uint32_t ch, uint32_t key, uint32_t vel)
{
    (void)ch;
    inc = 0x2000000u + key * 0x80000u;
    held = vel != 0;
}
static void cc(uint32_t ch, uint32_t num, uint32_t val) { (void)ch; (void)num; (void)val; }
static void ui(uint32_t ev, int32_t arg) { (void)ev; (void)arg; }
static void draw(uint32_t full) { (void)full; }
static void release(void) { held = 0; pk = 0; }

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = ~0u, k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++)
            c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

static int init(const hb_host_t *h)
{
    H = h;
    held = 0;
    pk = 0;
    pk_n = 0;
    if (HB_HOST_HAS(h, pack_find))
        pk = (const uint8_t *)h->pack_find("TESTPACK", &pk_n);
    if (pk) {
        amp = 64 + pk[0];
        H->log_u32("packuse: pack bytes", pk_n);
        H->log_u32("packuse: pack crc", crc32(pk, pk_n));
    } else {
        H->log("packuse: no pack");
    }
    return 0;
}

HB_DESC = {HB_ABI, "PACKUSE", init, release, note, cc, render, ui, draw, 0, 0, 0, 0};
