/* SPDX-License-Identifier: GPL-3.0-only */
/* The update on a computer: firmware/src/ota.c and firmware/loader/ldr_core.c as the FM-1 runs them, over a flash
 * chip in a file (fm1-live --flash's: 1 MiB as stored) and a package served as the installers serve it (its logical
 * image). tests/install_room_test.py drives it.
 *
 *   cc -O2 -Wall -Ifirmware/hal -Ifirmware/src -o build/ldr_host tests/ldr_host.c
 *   build/ldr_host install FLASH.bin IMAGE.bin [CUT]   the loader's pass(es) (ldr_session); CUT: the power goes half way
 *                                                      through the CUT-th erase / program (exit 99, the flash as left)
 *   build/ldr_host stage FLASH.bin IMAGE.bin           the running firmware's step 1 (ota_stage: the checks, then the
 *                                                      loader staged), its own hooks over the same flash
 * Prints "rc N ops N refused N" (refused: the device read OTA_REFUSED_AT); writes FLASH.bin back. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BOARD_SMK37
#define BOARD_SMK37 0
#endif
#define OTA_KEEP_ADDONS 1
#define FELUCCA_ID "ota-FM-1_900"

static uint8_t flash[0x100000];
static uint8_t *image;
static long image_n;
static unsigned long ops, cut;
static int refused;
static jmp_buf power;

/* ---- the host's side of the M-UPGRADE reads: every request answered at once */
static uint8_t q_frame[2048];
static uint32_t q_n, q_have;
static uint32_t pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        for (nb += 8u; nb >= 7u; nb -= 7u, acc >>= 7) out[o++] = (uint8_t)(acc & 0x7Fu);
    }
    if (nb) out[o++] = (uint8_t)(acc & 0x7Fu);
    return o;
}
static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    uint8_t m[64], body[600];
    uint32_t acc = 0, nb = 0, d = 0, i, addr, len, s = 0;
    for (i = 1; i + 1 < n; i++) {                       /* (unpack7) */
        acc |= (uint32_t)(p[i] & 0x7Fu) << nb;
        nb += 7u;
        if (nb >= 8u) { if (d < sizeof m) m[d] = (uint8_t)acc; d++; acc >>= 8; nb -= 8u; }
    }
    if (d < 15u || m[2] != 0x30)
        return 0;                                       /* (the identity, ...: nothing to answer) */
    addr = m[7] | m[8] << 8 | m[9] << 16 | (uint32_t)m[10] << 24;
    len = m[11] | m[12] << 8;
    if (len > 512u) return 0;
    body[0] = 0; body[1] = 0x59; body[2] = 0x30;
    body[3] = (uint8_t)(len + 8u); body[4] = (uint8_t)((len + 8u) >> 8); body[5] = 0; body[6] = 0;
    memcpy(body + 7, &addr, 4);
    body[11] = (uint8_t)len; body[12] = (uint8_t)(len >> 8); body[13] = 0;
    if (addr == 0xE0000000u || addr == 0xF0000000u)
        memcpy(body + 14, "success\0", 8);
    else if (addr == 0xF2000000u) {
        refused++;
        memset(body + 14, 0, len);
    } else if (addr + len <= (uint32_t)image_n)
        memcpy(body + 14, image + addr, len);
    else {
        fprintf(stderr, "read outside the package: %#x+%u\n", addr, len);
        exit(3);
    }
    for (i = 6; i < 14u + len; i++) s += body[i];
    body[14 + len] = (uint8_t)~s;
    q_n = pack7(body, 15u + len, q_frame);
    q_have = 1;
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n)
{
    if (!q_have) return 0;
    *p = q_frame;
    *n = q_n;
    return 1;
}
static void ota_frame_done(void) { q_have = 0; }
static uint32_t ms;
static uint32_t ota_now_ms(void) { return ms++; }
static void ota_idle(void) {}

/* ---- the flash chip, with a power cut */
static void op(uint32_t off, uint32_t n, const uint8_t *src)    /* src 0: an erase */
{
    ops++;
    if (cut && ops == cut) {                            /* half of it done, then nothing */
        if (src) memcpy(flash + off, src, n / 2u);
        else memset(flash + off, 0xFF, n / 2u);
        longjmp(power, 1);
    }
    if (src) { uint32_t i; for (i = 0; i < n; i++) flash[off + i] &= src[i]; }
    else memset(flash + off, 0xFF, n);
}
static int rd(uint32_t off, void *p, uint32_t n)
{
    if (off + n > sizeof flash) return -1;
    memcpy(p, flash + off, n);
    return 0;
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return rd(off, p, n); }
#define OTA_AREA_HOST 0xE0000u
static int ota_erase(uint32_t off) { if (off < OTA_AREA_HOST || off >= OTA_AREA_HOST + 0x5000u) return -1; op(off, 0x1000u, 0); return 0; }
static int ota_prog(uint32_t off, const void *p, uint32_t n) { if (off < OTA_AREA_HOST || off + n > OTA_AREA_HOST + 0x5000u) return -1; op(off, n, p); return 0; }
static int last_step, last_code;
static void ota_show(uint32_t step, int32_t code) { last_step = (int)step; last_code = code; }
static void ota_commit(const uint8_t *parm) { (void)parm; }
#include "../firmware/src/ota.c"

static int ldr_fread(uint32_t off, void *p, uint32_t n) { return rd(off, p, n); }
static int ldr_erase(uint32_t off)
{
    if (off < 0x4000u || off >= 0xFC000u || (off & 0xFFFu)) return -1;   /* (loader.c FL_RANGE_OK) */
    op(off, 0x1000u, 0);
    return 0;
}
static int ldr_prog(uint32_t off, const void *p, uint32_t n)
{
    if (off < 0x4000u || off + n > 0xFC000u) return -1;
    op(off, n, p);
    return 0;
}
static void ldr_record_clear(void) {}
static int ldr_flash_known(void) { return 1; }
static void ldr_progress(uint32_t done, uint32_t total) { (void)done; (void)total; }
#include "../firmware/loader/ldr_core.c"
static int ota_chip_key(uint32_t *key) { return ldr_chip_key(key); }

static long load(const char *path, uint8_t *dst, long max)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) { perror(path); exit(2); }
    n = (long)fread(dst, 1, (size_t)max, f);
    fclose(f);
    return n;
}

int main(int argc, char **argv)
{
    int rc = 0, stage;
    FILE *f;
    if (argc < 4) { fprintf(stderr, "ldr_host install|stage FLASH IMAGE [CUT]\n"); return 2; }
    stage = !strcmp(argv[1], "stage");
    memset(flash, 0xFF, sizeof flash);
    load(argv[2], flash, sizeof flash);
    image = malloc(4u << 20);
    image_n = load(argv[3], image, 4L << 20);
    cut = argc > 4 ? strtoul(argv[4], 0, 0) : 0;
    if (setjmp(power))
        rc = 99;
    else
        rc = stage ? ota_stage() : ldr_session();
    f = fopen(argv[2], "wb");
    fwrite(flash, 1, sizeof flash, f);
    fclose(f);
    printf("rc %d ops %lu refused %d\n", rc, ops, refused);
    return 0;
}
