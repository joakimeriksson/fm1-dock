/* SPDX-License-Identifier: GPL-3.0-only */
/* modules/minipops/mp_engine.c on this computer, for tests/minipops_check.py: the same renders as
 * minipops_ref.mjs makes of the web engine. 16-bit mono raw.
 *   minipops_host voices OUT.raw | rhythm KEY OUT.raw */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mp_engine.h"

static mp_t m;
static int32_t blk[2 * 256];

static void render(FILE *f, uint32_t frames)
{
    while (frames) {
        uint32_t n = frames > 256u ? 256u : frames, i;
        memset(blk, 0, sizeof blk);
        mp_render(&m, blk, n);
        for (i = 0; i < n; i++) {
            int32_t v = blk[2 * i];
            int16_t s = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
            fwrite(&s, 2, 1, f);
        }
        frames -= n;
    }
}

int main(int argc, char **argv)
{
    FILE *f;
    if (argc < 3) return 2;
    mp_init(&m);
    mp_set_level(&m, 100, 50);
    mp_set_extra(&m, 70, 70, 70);
    if (!strcmp(argv[1], "voices")) {
        uint32_t v;
        f = fopen(argv[2], "wb");
        for (v = 0; v < MP_VOICES; v++) {
            mp_render(&m, blk, 0);              /* (the settings taken up before the strike) */
            mp_trigger(&m, v, 65536);
            render(f, MP_SR);
        }
    } else {
        f = fopen(argv[3], "wb");
        mp_set_keys(&m, 1u << atoi(argv[2]), 0);
        mp_set_tempo(&m, 120);
        mp_play(&m, 1);
        render(f, 4 * MP_SR);
    }
    fclose(f);
    return 0;
}
