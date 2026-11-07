/* SPDX-License-Identifier: GPL-3.0-only */
/* NIAGARA: Evan Yerburgh's Niagara Drift (github.com/evanyerburgh/niagara-drift, 86f47d2, v0.2.1; its files in
 * niagara/, see take.py), a drone synth after the SOMA Lyra-8, the Moog Subharmonicon and the Stylophone CPM DS-2,
 * as a fm1-dock module: two virtual-analog drones that breathe and drift, scenes on the lower white keys (their
 * pressure from the host's sensing), gestures on the top four, an LFO, a delay and a reverb. Its app on fm1-dock's
 * host (niagara_platform.inc: its platform calls); its engine renders in the host's audio interrupt, as on its own
 * firmware (one core), with the FPU as Niagara Drift builds it (module.cfg). HOME held 1.5 s alone opens fm1-dock's
 * module list. */
#include "niagara_platform.inc"
#include "project.c"
#include "ui.c"

#define ND_BLOCK 256u
static int32_t nd_buf[2u * ND_BLOCK];
static void niagara_render(int32_t *out, uint32_t n)
{
    uint32_t t0 = H->ticks(), i;
    if (n != ND_BLOCK)
        return;
    dr_render(nd_buf, n, ND_GAIN);          /* (the host applies MASTER) */
    for (i = 0; i < 2u * n; i++) {          /* its 24 bits into the host's Q15, added */
        int32_t v = out[i] + (nd_buf[i] >> 8);
        out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
    nd_load = (H->ticks() - t0) * 100u / (5805u * 24u);
}

static void niagara_ui(uint32_t ev, int32_t arg)
{
    nd_key_event(ev, arg);
    hbf_event(ev, arg);
}
static void niagara_midi(uint32_t st, uint32_t d1, uint32_t d2) { hbf_midi(st, d1, d2); }
static void niagara_note(uint32_t ch, uint32_t key, uint32_t vel) { hbf_midi(0x90u | (ch & 15u), key, vel); }
static void niagara_cc(uint32_t ch, uint32_t num, uint32_t val) { hbf_midi(0xB0u | (ch & 15u), num, val); }

static uint32_t nd_last_frame;
static uint8_t nd_fresh;                    /* init ran, draw has not yet */
static void niagara_draw(uint32_t full)
{
    nd_sensed = HB_HOST_HAS(H, sense_keys) ? H->sense_keys() : 0;
    if (full && nd_fresh) {                 /* (just started: its start animation, as init left it) */
        nd_fresh = 0;
        nd_all = 1;
    } else if (full) {                      /* (back from the module list: the whole screen and the lights again; the
                                             * drones play on, no start animation) */
        uint8_t playing = ui.playing;
        ui_init();
        ui.playing = playing;
        ui.splash_t0 = plat_ms() - SPLASH_MS;
        nd_all = 1;
    }
    if (H->ms() - nd_last_frame >= 16u) {   /* ~60 frames a second at most, input in between (its main loop) */
        nd_last_frame = H->ms();
        ui_frame();
    } else {
        ui_input_only();
    }
}

static int niagara_init(const hb_host_t *h)
{
    H = h;
    nd_sensed = HB_HOST_HAS(H, sense_keys) ? H->sense_keys() : 0;
    dr_init();
    dr_seed(H->rand());
    project_load();                         /* defaults when nothing (or another format) is saved */
    project_apply();
    ui_init();
    nd_fresh = 1;
    H->log("niagara: Niagara Drift (evanyerburgh/niagara-drift 86f47d2)");
    return 0;
}

static void niagara_release(void)
{
    dr_play(0);
    dr_panic();
}

HB_DESC = {HB_ABI, "NIAGARA", niagara_init, niagara_release, niagara_note, niagara_cc, niagara_render, niagara_ui,
           niagara_draw, 0, 0, HB_F_SCREEN | HB_F_PANEL, niagara_midi};
