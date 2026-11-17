/* SPDX-License-Identifier: GPL-3.0-only */
/* modules/eminent's engine on this computer, for tests/eminent_check.py: 16-bit stereo raw renders.
 *   eminent_host NOTE ENSEMBLE PHASER COLOR ATTACK RELEASE OUT.raw    a key held two seconds, then two seconds more
 *   eminent_host chord ENSEMBLE PHASER OUT.raw                        a D minor chord held six seconds, then two more */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "em_engine.h"

static em_t e;
static int32_t blk[2 * 256];

static void render(FILE *f, uint32_t frames)
{
    while (frames) {
        uint32_t n = frames > 256u ? 256u : frames, i;
        memset(blk, 0, sizeof blk);
        em_render(&e, blk, n);
        for (i = 0; i < 2u * n; i++) {
            int32_t v = blk[i];
            int16_t s = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
            fwrite(&s, 2, 1, f);
        }
        frames -= n;
    }
}

int main(int argc, char **argv)
{
    FILE *f;
    em_init(&e);
    if (argc >= 5 && !strcmp(argv[1], "chord")) {
        e.p.ensemble = (uint8_t)atoi(argv[2]); e.p.phaser = (uint8_t)atoi(argv[3]);
        f = fopen(argv[4], "wb");
        em_note_on(&e, 50, 100); em_note_on(&e, 53, 100); em_note_on(&e, 57, 100); em_note_on(&e, 62, 100);
        render(f, 6 * EM_SR);
        em_all_off(&e);
        render(f, 2 * EM_SR);
    } else if (argc >= 8) {
        e.p.ensemble = (uint8_t)atoi(argv[2]); e.p.phaser = (uint8_t)atoi(argv[3]); e.p.color = (uint8_t)atoi(argv[4]);
        e.p.attack = (uint8_t)atoi(argv[5]); e.p.release = (uint8_t)atoi(argv[6]);
        f = fopen(argv[7], "wb");
        em_note_on(&e, (uint32_t)atoi(argv[1]), 100);
        render(f, 2 * EM_SR);
        em_note_off(&e, (uint32_t)atoi(argv[1]));
        render(f, 2 * EM_SR);
    } else return 2;
    fclose(f);
    return 0;
}
