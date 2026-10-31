/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Andre Rezemini (the SLOOP dualcore branch, github.com/andrerezemini/sloop-lab): after its
 * cpu1.c, cut down for fm1-dock (no menu switch, no render split of its own)
 * Copyright (C) 2026 Joakim Eriksson: the screening, the checks around a job, the parked job */
/* The second core of the AC7911: started at power-on (platform.c fm1_cstart), proved with a self-test job,
 * screened for misreads in the first audio blocks, then lent to the module that runs (hb_host_t.c1_run /
 * c1_wait, dock.c), one job at a time, from its render only.
 * The HAL is X0X's (hal/fm1_cpu1.h): core 0 hands a job over and waits; the waiting loop lives in RAM
 * (.c1_text), so an idle core 1 never fetches from the flash, and a job is always over when the audio
 * interrupt returns: flash writes (which turn the flash off with interrupts off) never find it working.
 *
 * Some FM-1s have a core 1 that reads RAM wrong now and then (bit 21 set where core 0 reads 0: Jangada,
 * fm1-x0x#10; fm1-emu --flaky-core1 makes one). Such a misread lands in the module's state as often as
 * anywhere, and a few check words around a job see few of them (fm1-emu, 1e-6 a load: CS-1's sound changed,
 * no check word caught it), so core 1 is screened first: it reads a zeroed kilobyte ~4M times, a slice in
 * each of the first 128 audio blocks (~0.75 s, beside the render: the boot does not wait for it; the module
 * has one core until then), and a unit that misreads once keeps one core. Around each job it reads the
 * kilobyte again: once a word comes back otherwise, it is held and the module runs on one core from the next
 * block. These checks are statistical: a misread elsewhere goes unseen, and a block's results are kept.
 * Its idle loop (fm1_c1_main) runs a job when the job word differs from its done word, so a misread of
 * either one runs the job again: between jobs the job is a no-op in RAM (cpu1_park), so such a run
 * touches nothing and fetches nothing from the flash (a flash write, a module switch).
 *
 * On some units a core goes to a wrong address while core 1 runs: code addresses read with bits 23 / 24 set
 * (0x038405DC for 0x020405DC; fm1-x0x#21, Melodee's 0x03801494), or an unmapped one (DBG bit 4 / 19), at M-VAVE's
 * own supply levels (fm1_supplies_stock), where no check of core 1's sees it. On one FM-1 (2026-10-30) it came
 * with the BLE radio running and core 1 working (screening or a job), within ~0.3 s; with the radio off, two cores
 * ran clean. So the radio holds core 1 (cpu1_radio), and a crash or the watchdog while core 1 runs keeps it off
 * until power-off (cpu1_bar, as Melodee and X0X do): the next boots run on one core instead of crashing again
 * into the boot guard's USB rescue. */
#include "fm1_cpu1.h"

enum { CPU1_OFF, CPU1_NOSTART, CPU1_FAILED, CPU1_OK, CPU1_MISREAD, CPU1_SCREENING, CPU1_BARRED, CPU1_BLE };   /* not
                                                 * started / did not report in / self-test or screening wrong or
                                                 * late / running / held after a misread / being screened / not
                                                 * started: a crash with two cores since power-on / held: the radio
                                                 * runs (cpu1_radio) */
static volatile uint8_t cpu1_state = CPU1_OFF;   /* (the main loop reads it: dock.c) */
#define CPU1_BAR_MAGIC 0x52414231u               /* "1BAR" */
static uint32_t cpu1_bar __attribute__((section(".noinit")));   /* CPU1_BAR_MAGIC: one core until power-off */
static uint8_t cpu1_bar_why;                     /* CPU1_BARRED: 1 the watchdog while core 1 ran (this boot's reset),
                                                 * 2 a crash or that, since power-on */
static uint32_t cpu1_timeouts;                   /* jobs core 1 did not finish in time */
static volatile uint32_t cpu1_test_out;
#define CPU1_ZERO_WORDS 256u
#define CPU1_SCREEN_PASSES 128u                  /* a screening slice: 32K loads, ~0.5 ms of a 5.8 ms block */
#define CPU1_SCREEN_JOBS 128u
static volatile uint32_t cpu1_zero[CPU1_ZERO_WORDS];  /* never written: core 1 reads them in its screening
                                                 * and around each job */
static volatile uint32_t cpu1_misreads;          /* (core 1 counts) */
static void (*volatile cpu1_fn)(uint32_t);       /* the module's job, and its argument */
static volatile uint32_t cpu1_arg;
static uint32_t cpu1_job_ticks, cpu1_job_worst; /* core 1's time in the last job, and the longest */
static uint32_t cpu1_screen_ticks;               /* core 1's time in the screening */
static uint32_t cpu1_screened;                   /* slices done */
static uint8_t cpu1_pending;                     /* a job handed over and not yet waited for (core 0's) */
static uint8_t cpu1_screen_out;                  /* a slice handed over (core 0's) */
static uint8_t cpu1_late;                        /* CPU1_FAILED: 1 the self-test, 2 the screening, 3 a job */

static void __attribute__((section(".c1_text"), used)) cpu1_idle(uint32_t arg)   /* the job between jobs */
{
    (void)arg;
}

static void cpu1_park(void)                      /* after a job: core 1's job word points at the no-op */
{
    fm1_c1_fn = cpu1_idle;
}

static void cpu1_stop(uint8_t state)             /* core 1 held (or never started): one core from here on */
{
    fm1_c1_ran = 0;
    cpu1_state = state;
}

/* the radio is about to start (dock.c ble_start, the main loop's: no job is out between audio blocks): core 1 held
 * until the next power-on (it cannot be started again). On this unit, with the radio running and core 1 working,
 * one core or the other went to an unmapped address within ~0.3 s (core 1 idle while the radio started did not
 * help); with the radio off two cores ran clean, with one core the radio did */
static void cpu1_radio(void)
{
    if (cpu1_state != CPU1_OK && cpu1_state != CPU1_SCREENING)
        return;
    fm1_cpu1_hold();
    cpu1_stop(CPU1_BLE);
}

static int cpu1_crashed(void)                    /* core 0's fault (core 1 already held): 1 when core 1 ran: one
                                                  * core until power-off */
{
    if (cpu1_state != CPU1_OK && cpu1_state != CPU1_SCREENING)
        return 0;
    cpu1_bar = CPU1_BAR_MAGIC;
    return 1;
}

static uint32_t cpu1_lcg(uint32_t x)             /* 64 steps of an LCG: the self-test's work */
{
    uint32_t i;
    for (i = 0; i < 64u; i++)
        x = x * 1664525u + 1013904223u;
    return x;
}

static void cpu1_selftest(uint32_t arg)          /* runs on core 1, from the flash, as the jobs will */
{
    cpu1_test_out = cpu1_lcg(arg);
}

static inline __attribute__((always_inline)) uint32_t cpu1_zeros(void)   /* on core 1: 0 when every check word read
                                                 * as stored (inline: in the RAM code below) */
{
    uint32_t i, x = 0;
    for (i = 0; i < CPU1_ZERO_WORDS; i += 8u)     /* 8 a turn: the loop's own work a fraction of the loads */
        x |= cpu1_zero[i] | cpu1_zero[i + 1u] | cpu1_zero[i + 2u] | cpu1_zero[i + 3u]
             | cpu1_zero[i + 4u] | cpu1_zero[i + 5u] | cpu1_zero[i + 6u] | cpu1_zero[i + 7u];
    return x;
}

/* the screening and the job's wrapper run from RAM (.c1_text), as its idle loop: core 1 fetches from the flash only
 * for the module's own job (the crashes above fell in the screening's time at first, with the radio on) */
static void __attribute__((section(".c1_text"), used)) cpu1_screen(uint32_t arg)   /* on core 1: the check words */
{
    uint32_t i, x = 0, t0 = fm1_c1_ticks();      /* (inline: no call into the flash) */
    for (i = 0; i < arg; i++)
        x |= cpu1_zeros();
    cpu1_test_out = x;
    cpu1_job_ticks = fm1_c1_ticks() - t0;
}

static void __attribute__((section(".c1_text"), used)) cpu1_job(uint32_t arg)   /* on core 1: the module's job
                                                 * between two checks */
{
    uint32_t t0 = fm1_c1_ticks(), x = cpu1_zeros();
    cpu1_fn(arg);
    x |= cpu1_zeros();
    if (x)
        cpu1_misreads++;
    cpu1_job_ticks = fm1_c1_ticks() - t0;
}

/* at power-on, before the audio and the timers (X0X: started later, core 1 never reached its entry), with
 * the reserved top of RAM not locked yet (fm1_guard_lock_top comes after): its entry word can be written.
 * The PC limits are opened for the start (it boots through the chip's ROM) and armed again after. */
static void cpu1_boot(void)
{
    uint32_t p3 = fm1_boot.p3_rst;
    cpu1_bar_why = 2;
    if ((p3 & 1u) && !(p3 & (4u | 0x40u))) {    /* power-on: core 1 gets its chance again (as the boot guard) */
        cpu1_bar = 0;
    } else if ((p3 & 4u) && fm1_c1_ran == FM1_C1_RAN) {
        cpu1_bar = CPU1_BAR_MAGIC;               /* the watchdog while core 1 ran (a reboot or an update holds it */
        cpu1_bar_why = 1;                        /* and clears fm1_c1_ran first: fm1_sys.h) */
    }
    if (cpu1_bar == CPU1_BAR_MAGIC) {
        cpu1_stop(CPU1_BARRED);
        return;
    }
    fm1_guard_pc_open();
    if (fm1_cpu1_start() != 0) {
        fm1_guard_enable(FM1_GUARD_PC);
        cpu1_stop(CPU1_NOSTART);
        return;
    }
    fm1_guard_enable(FM1_GUARD_PC);
    cpu1_test_out = 0;
    if (!fm1_cpu1_run(cpu1_selftest, 20261028u) || fm1_cpu1_wait() != 0 || cpu1_test_out != cpu1_lcg(20261028u)) {
        fm1_cpu1_hold();                         /* answered, but wrong or late: one core from here on */
        cpu1_late = 1;
        cpu1_stop(CPU1_FAILED);
        return;
    }
    cpu1_park();
    fm1_c1_ran = FM1_C1_RAN;
    cpu1_state = CPU1_SCREENING;
}

/* the screening, from the audio interrupt (dock.c app_audio): a slice handed over before the render... */
static void cpu1_screen_begin(void)
{
    if (cpu1_state != CPU1_SCREENING)
        return;
    cpu1_test_out = 1;
    cpu1_screen_out = (uint8_t)fm1_cpu1_run(cpu1_screen, CPU1_SCREEN_PASSES);
}

/* ...and waited for after it */
static void cpu1_screen_end(void)
{
    if (!cpu1_screen_out)
        return;
    cpu1_screen_out = 0;
    if (fm1_cpu1_wait() != 0) {
        cpu1_timeouts++;
        cpu1_late = 2;
        cpu1_stop(CPU1_FAILED);
        return;
    }
    cpu1_park();
    cpu1_screen_ticks += cpu1_job_ticks;
    cpu1_job_ticks = 0;                          /* (a module's job, for the stats) */
    if (cpu1_test_out) {
        fm1_cpu1_hold();                         /* a word read wrong: this unit keeps one core */
        cpu1_stop(CPU1_MISREAD);
    } else if (++cpu1_screened == CPU1_SCREEN_JOBS) {
        cpu1_state = CPU1_OK;
    }
}

/* the module's calls (dock.c h_c1_run / h_c1_wait): 1 = core 1 took the job */
static int cpu1_run(void (*fn)(uint32_t), uint32_t arg)
{
    if (cpu1_state != CPU1_OK || !fn || cpu1_pending)   /* (one job at a time) */
        return 0;
    cpu1_fn = fn;
    cpu1_arg = arg;
    cpu1_pending = (uint8_t)fm1_cpu1_run(cpu1_job, arg);
    return cpu1_pending;
}

/* 0 = the job is done; -1 = core 1 did not finish it in time and is held from now on: the caller runs it */
static int cpu1_wait(void)
{
    if (!cpu1_pending)                           /* (none out: nothing to wait for) */
        return 0;
    cpu1_pending = 0;
    if (fm1_cpu1_wait() != 0) {
        cpu1_timeouts++;
        cpu1_late = 3;
        cpu1_stop(CPU1_FAILED);
        return -1;
    }
    cpu1_park();
    if (cpu1_job_ticks > cpu1_job_worst)
        cpu1_job_worst = cpu1_job_ticks;
    if (cpu1_misreads && cpu1_state == CPU1_OK) {
        fm1_cpu1_hold();                         /* this block is done; the next ones on one core */
        cpu1_stop(CPU1_MISREAD);
    }
    return 0;
}

static int cpu1_busy(void)                       /* a job handed over and not yet waited for */
{
    return cpu1_pending;
}

/* bytes of core 1's user stack never written: fm1_cpu1_start filled it with a mark; the jobs eat it from the top */
static uint32_t cpu1_stack_free(void)
{
    uint32_t i;
    for (i = 0; i < FM1_C1_STACK_WORDS && _c1_ustack[i] == FM1_C1_MARK; i++)
        ;
    return i * 4u;
}
