/* SPDX-License-Identifier: GPL-3.0-only */
/* FLAT-20 SysEx panel link protocol (USB MIDI).
 * The FM-1 broadcasts its state to the macOS companion app (mac/) on change and at 1 Hz,
 * and accepts incoming CABLES messages to update the patch panel.
 *
 *   F0 7D 46 32 30 VER TYPE ... F7          (7D: non-commercial SysEx ID; "F20"; VER: LINK_VERSION)
 *
 *   TYPE 1, STATE   NPARAM NSRC NDST  p[NPARAM]  (jack jack) x NCABLE  preset flags octave+3  name[12]
 *                   jack IDs: 0 = unconnected, 1..NSRC-1 = outputs (SRC_*), NSRC..NJACK-1 = inputs (NSRC-1 + DST_*).
 *                   One zero end = dummy plug; both zero = empty slot.
 *                   flags: bit 0 = HOLD active, bit 1 = patch modified, bit 2 = firmware accepts CABLES.
 *   TYPE 2, LIVE    gate note wheel_hi wheel_lo button lamps
 *                   gate: key active (or held); note: 0..127 active note (127 with gate=0 means none);
 *                   wheel: (wheel + 127) as 14-bit value (-127..127 = -5..+5 V);
 *                   button: momentary switch; lamps: bit 0 = MG, bit 1 = EG2 trig, bit 2 = ESP trig, bit 3 = ESP peak.
 *   TYPE 3, CABLES  (host -> FM-1) NSRC NDST  (jack jack) x NCABLE
 *                   Full patch panel state. Accepted only if all jack IDs are valid, no jack is used twice,
 *                   and no cable connects a jack to itself. Firmware responds with a STATE message.
 *
 * All payload bytes are 7-bit clean (knobs 0..127, ASCII name). NPARAM/NSRC/NDST let the host
 * detect schema mismatches. Bump LINK_VERSION on wire format changes. */
#ifndef MS20_LINK_H
#define MS20_LINK_H
#include <stdint.h>
#include "ms20.h"

#define LINK_VERSION 2u                             /* v2: cables connect any two jacks (MS_JACK_*) */
#define LINK_STATE 1u
#define LINK_LIVE 2u
#define LINK_CABLES 3u
#define LINK_CABLES_LEN (7u + 2u + 2u * MS_NCABLE + 1u)
#define LINK_TAKES_CABLES 4u    /* (STATE's flag) */
#define LINK_STATE_LEN (7u + 3u + MS_NPARAM + 2u * MS_NCABLE + 3u + 12u + 1u)
#define LINK_LIVE_LEN (7u + 6u + 1u)
#define LINK_MAX LINK_STATE_LEN
/* If a host echoes SysEx back to the FM-1, it hits ota.c's frame decoder, which expects
 * the first 7-bit-unpacked byte to be 0. Keeping LINK_ID's low 7 bits nonzero (0x7D) prevents
 * echoed link packets from being misinterpreted as OTA frames. */
#define LINK_ID 0x7Du
_Static_assert((LINK_ID & 0x7Fu) != 0u, "the first byte must not decode as an update frame");
_Static_assert(MS_NPARAM < 128u && MS_NJACK < 128u && MS_NSRC < 128u && MS_NDST < 128u, "counts are sent as 7-bit bytes");

typedef struct {
    uint8_t preset, flags, octave;                   /* octave: -3..3 as 0..6 */
} link_meta_t;

typedef struct {
    uint8_t gate, note, button, lamps;
    int32_t wheel;                                   /* -127..127 */
} link_live_t;

static uint32_t link_head(uint8_t *o, uint32_t type)
{
    o[0] = 0xF0u; o[1] = LINK_ID; o[2] = 0x46u; o[3] = 0x32u; o[4] = 0x30u; o[5] = (uint8_t)LINK_VERSION; o[6] = (uint8_t)type;
    return 7u;
}

/* Encode STATE message into o (LINK_STATE_LEN bytes); returns byte count. */
static uint32_t link_state(uint8_t *o, const ms20_patch_t *p, const link_meta_t *m)
{
    uint32_t n = link_head(o, LINK_STATE), i;
    o[n++] = (uint8_t)MS_NPARAM;
    o[n++] = (uint8_t)MS_NSRC;
    o[n++] = (uint8_t)MS_NDST;
    for (i = 0; i < MS_NPARAM; i++)
        o[n++] = p->p[i] & 0x7Fu;
    for (i = 0; i < MS_NCABLE; i++) {
        uint32_t a = p->cable[i][0], b = p->cable[i][1];
        o[n++] = (uint8_t)(a < MS_NJACK && b < MS_NJACK ? a : 0u);
        o[n++] = (uint8_t)(a < MS_NJACK && b < MS_NJACK ? b : 0u);
    }
    o[n++] = m->preset & 0x7Fu;
    o[n++] = m->flags & 0x7Fu;
    o[n++] = m->octave & 0x7Fu;
    for (i = 0; i < 12u; i++) {
        uint8_t c = i < sizeof p->name ? (uint8_t)p->name[i] : 0u;
        o[n++] = c < 0x20u || c > 0x7Eu ? 0u : c;
    }
    for (i = 0; i < 12u; i++)                        /* zero-pad after first NUL */
        if (!o[n - 12u + i]) {
            for (; i < 12u; i++)
                o[n - 12u + i] = 0;
            break;
        }
    o[n++] = 0xF7u;
    return n;
}

/* Validate and decode a CABLES payload (excluding F0/F7) into c. Returns 0 on success, -1 on error (c unchanged). */
static int link_cables_take(const uint8_t *p, uint32_t n, uint8_t c[MS_NCABLE][2])
{
    uint8_t t[MS_NCABLE][2], used[MS_NJACK];
    uint32_t i;
    if (n != LINK_CABLES_LEN - 2u || p[0] != LINK_ID || p[1] != 0x46u || p[2] != 0x32u || p[3] != 0x30u ||
        p[4] != LINK_VERSION || p[5] != LINK_CABLES || p[6] != MS_NSRC || p[7] != MS_NDST)
        return -1;
    for (i = 0; i < MS_NJACK; i++)
        used[i] = 0;
    for (i = 0; i < MS_NCABLE; i++) {
        uint8_t a = p[8u + 2u * i], b = p[9u + 2u * i];
        if (a >= MS_NJACK || b >= MS_NJACK || (a && a == b))
            return -1;                               /* invalid jack ID or self-loop */
        if ((a && used[a]++) || (b && used[b]++))
            return -1;                               /* duplicate plug in same jack */
        t[i][0] = a;
        t[i][1] = b;
    }
    for (i = 0; i < MS_NCABLE; i++) {
        c[i][0] = t[i][0];
        c[i][1] = t[i][1];
    }
    return 0;
}

/* Encode LIVE message into o (LINK_LIVE_LEN bytes); returns byte count. */
static uint32_t link_live(uint8_t *o, const link_live_t *l)
{
    uint32_t n = link_head(o, LINK_LIVE), w = (uint32_t)(l->wheel < -127 ? 0 : l->wheel > 127 ? 254 : l->wheel + 127);
    o[n++] = l->gate ? 1u : 0u;
    o[n++] = l->note & 0x7Fu;
    o[n++] = (uint8_t)(w >> 7);
    o[n++] = (uint8_t)(w & 0x7Fu);
    o[n++] = l->button ? 1u : 0u;
    o[n++] = l->lamps & 0x0Fu;
    o[n++] = 0xF7u;
    return n;
}
#endif
