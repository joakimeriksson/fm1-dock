/* SPDX-License-Identifier: GPL-3.0-only */
/* modules/arp2600's engine on this computer, for tests/arp_check.py: 16-bit mono raw renders.
 *   arp_host note PATCH MIDI OUT.raw      two seconds, the key held for one
 *   arp_host seq PATCH SEQ OUT.raw        eight seconds of a sequence
 *   arp_host sweep OUT.raw                noise through the ladder, the cutoff 10..90 in steps, 0.5 s each
 *   arp_host echo PATCH SEQ OUT.raw       as seq, through the host's echo (hb_echo.c: 1/8. of the tempo, 45 %, tone 50, send 35 %): stereo
 *   arp_host play PATCH OUT.raw           arp_play: C3 0.5 s, LEGATO to G3 0.5 s, SLIDE to C4 0.5 s, released 0.5 s,
 *                                         C3 again plain 0.5 s, released 0.5 s, C3 ACCENT 0.5 s, released 0.5 s */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arp_engine.h"
#include "arp_presets.h"
#include "../firmware/src/hb_echo.c"

static arp_t a;
static int32_t blk[2 * 256];

static int32_t ecin[256], wet[2 * 256];
static int echo;
static void render(FILE *f, uint32_t frames)
{
    while (frames) {
        uint32_t n = frames > 256u ? 256u : frames, i;
        memset(blk, 0, sizeof blk);
        arp_render(&a, blk, n);
        if (echo) {                             /* the host's order: the bass's send into the echo, the repeats beside the dry */
            memset(wet, 0, sizeof wet);
            for (i = 0; i < n; i++) ecin[i] = ((blk[2 * i] + blk[2 * i + 1]) >> 1) * 35 / 100;
            ec_run(ecin, wet, n);
            for (i = 0; i < 2u * n; i++) blk[i] += wet[i];
        }
        for (i = 0; i < n; i++) {
            int32_t v = blk[2 * i], w = blk[2 * i + 1];
            int16_t s = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v), r = (int16_t)(w > 32767 ? 32767 : w < -32767 ? -32767 : w);
            fwrite(&s, 2, 1, f);
            if (echo) fwrite(&r, 2, 1, f);
        }
        frames -= n;
    }
}

int main(int argc, char **argv)
{
    FILE *f;
    if (argc < 3) return 2;
    arp_init(&a);
    if (!strcmp(argv[1], "note")) {
        arp_set_patch(&a, &ARP_PATCHES[atoi(argv[2]) % ARP_N_PATCHES]);
        f = fopen(argv[4], "wb");
        arp_note_on(&a, (uint32_t)atoi(argv[3]), 100);
        render(f, ARP_SR);
        arp_note_off(&a, (uint32_t)atoi(argv[3]));
        render(f, ARP_SR);
    } else if (!strcmp(argv[1], "play")) {
        arp_set_patch(&a, &ARP_PATCHES[atoi(argv[2]) % ARP_N_PATCHES]);
        f = fopen(argv[3], "wb");
        arp_play(&a, 48, 0); render(f, ARP_SR / 2);
        arp_play(&a, 55, ARP_PLAY_LEGATO); render(f, ARP_SR / 2);
        arp_play(&a, 60, ARP_PLAY_SLIDE); render(f, ARP_SR / 2);
        arp_release(&a); render(f, ARP_SR / 2);
        arp_play(&a, 48, 0); render(f, ARP_SR / 2);
        arp_release(&a); render(f, ARP_SR / 2);
        arp_play(&a, 48, ARP_PLAY_ACCENT); render(f, ARP_SR / 2);
        arp_release(&a); render(f, ARP_SR / 2);
    } else if (!strcmp(argv[1], "seq") || !strcmp(argv[1], "echo")) {
        arp_set_patch(&a, &ARP_PATCHES[atoi(argv[2]) % ARP_N_PATCHES]);
        arp_set_seq(&a, &ARP_SEQS[atoi(argv[3]) % ARP_N_SEQS]);
        if ((echo = argv[1][0] == 'e') != 0) { ec_clear(); ec_prep(ec_div_ms(3, a.seq.tempo), 45, 50); }
        f = fopen(argv[4], "wb");
        arp_run(&a, 1);
        render(f, 8 * ARP_SR);
    } else {
        arp_patch_t p = ARP_PATCHES[0];
        uint32_t c;
        p.vco1_lvl = p.vco2_lvl = p.sub_lvl = 0;
        p.noise_lvl = 100;
        p.reso = 0;
        p.env_amt = 0;
        p.kbd = 0;
        p.drive = 20;
        p.a2 = 0; p.s2 = 100; p.r2 = 50;
        f = fopen(argv[2], "wb");
        for (c = 10; c <= 90; c += 20) {
            p.cutoff = (uint8_t)c;
            arp_set_patch(&a, &p);
            if (c == 10) arp_note_on(&a, 60, 100);
            render(f, ARP_SR / 2);
        }
    }
    fclose(f);
    return 0;
}
