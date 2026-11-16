/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_COMMANDS_H
#define MD_COMMANDS_H
#include "event.h"

/* Bounded command handoff from the one producer context to the audio context.
 *
 * Contexts (fm1-dock, firmware/dock/hb_module.h and src/dock.c): render() runs in the
 * audio interrupt; init, ui, note, cc, midi, draw and data all run in the main loop.
 * Main is therefore the single producer and audio the single consumer, on one core,
 * and the interrupt can preempt main but never the reverse. Each shared index has one
 * writer and is a naturally aligned 32-bit word. MD_FENCE orders the payload against
 * publication of the index.
 *
 * On the device define MD_SINGLE_CORE: a compiler barrier is enough there, and the SDK
 * lowers C11 atomics to a missing CAS helper (work/evidence/synth-seq-audition.md).
 * Native builds use a full fence so the threaded tests are meaningful on weakly ordered
 * hosts. Calling the producer side from audio, or from a second core, is not supported. */
#ifndef MD_FENCE
#ifdef MD_SINGLE_CORE
#define MD_FENCE() __asm__ volatile("" ::: "memory")
#else
#define MD_FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif
#endif

typedef enum {
    MD_CMD_EVENT = 1,      /* event: note/hit/param/bend/pressure/source release/panic */
    MD_CMD_SUSTAIN,        /* track, event.source, value 0 up / 1 down */
    MD_CMD_START,          /* value: start quarter (bar*4) */
    MD_CMD_STOP,
    MD_CMD_CONTINUE,
    MD_CMD_TEMPO,          /* value: BPM Q8 */
    MD_CMD_MUTE,           /* track, value 0/1 */
    MD_CMD_SOLO,           /* track, value 0/1 */
    MD_CMD_LAUNCH,         /* track, arg pattern, value 1 immediate */
    MD_CMD_RETURN_LEVEL,   /* arg bus, value Q15 */
    MD_CMD_BYPASS,         /* track (0xff master), arg slot, value 0/1 */
    MD_CMD_BANK,           /* arg staged bank index, value generation */
    MD_CMD_SOUND,          /* track, value generation; payload in the session's sound slot */
    MD_CMD_BIND            /* track, value 0 its instrument, 1 its alternate (config.alternate) */
} md_command_kind_t;

typedef struct {
    uint8_t kind, track, arg, reserved;
    uint32_t value;
    md_event_t event;
} md_command_t;

typedef struct {
    md_command_t slot[MD_COMMAND_CAPACITY];
    volatile uint32_t head;        /* written by the producer only */
    volatile uint32_t tail;        /* written by the consumer only */
    /* Producer-owned diagnostics, readable from the producer context. */
    uint32_t rejected;             /* ordinary commands refused (ring at reserve) */
    uint32_t release_refused;      /* release commands refused (ring completely full) */
    uint32_t high_water;           /* most commands ever queued at once */
} md_command_ring_t;

_Static_assert((MD_COMMAND_CAPACITY & (MD_COMMAND_CAPACITY - 1u)) == 0, "ring needs a power of two");
_Static_assert(MD_RELEASE_RESERVE < MD_COMMAND_CAPACITY, "reserve must leave room");

void md_ring_init(md_command_ring_t *);
/* Producer. Ordinary commands may fill the ring up to CAPACITY-RESERVE; release
 * commands (note-off, source release, panic, stop, sustain up) may use the reserve.
 * Returns MD_ERR_CAPACITY without queuing when there is no room. */
int md_ring_push(md_command_ring_t *, const md_command_t *, int release);
/* Consumer. Returns 1 with a command, 0 when empty. */
int md_ring_pop(md_command_ring_t *, md_command_t *);
uint32_t md_ring_used(const md_command_ring_t *);
#endif
