/* SPDX-License-Identifier: GPL-3.0-only */
/* VECTOR37's voices on two cores (included into mix.c by take.py, after voice_render): every control block the
 * sounding voices are listed and the second core (hb_host_t c1_run) renders every other one into its own bus, with
 * its own corner buffers, while this core renders the rest; then the two buses are added. A voice's state is its own
 * (vs[v], vf[v], its wavetable slots in wsl), so the halves share nothing they write. Without the second core (not
 * started, still screened, a job that did not end in time) every voice is rendered here, as Vector37 does. */
static int32_t v37_tb0[FT_N][CTL], v37_tb1[FT_N][CTL], v37_b1l[CTL], v37_b1r[CTL];
static uint8_t v37_live[NVOICE], v37_nlive;
static uint8_t c1_have, c1_on, c1_dead;
static uint32_t c1_jobs;

static void v37_half(uint32_t from, int32_t (*tb)[CTL], int32_t *bl, int32_t *br)
{
    uint32_t k;
    for (k = from; k < v37_nlive; k += 2u)
        voice_render(&vs[v37_live[k]], tb, bl, br);
}
static void c1_job(uint32_t arg)                    /* on core 1: the odd ones of the list */
{
    uint32_t i;
    (void)arg;
    for (i = 0; i < CTL; i++)
        v37_b1l[i] = v37_b1r[i] = 0;
    v37_half(1, v37_tb1, v37_b1l, v37_b1r);
}
static void c1_nop(uint32_t arg) { (void)arg; }
static void c1_probe(void)                          /* (from render: is the second core there yet) */
{
    if (!c1_have || c1_dead || c1_on)
        return;
    if (H->c1_run(c1_nop, 0)) {
        if (H->c1_wait() == 0)
            c1_on = 1;
        else
            c1_dead = 1;
    }
}

static void v37_voices(void)
{
    uint32_t v, i, split;
    v37_nlive = 0;
    for (v = 0; v < NVOICE; v++)
        if (vs[v].stage)
            v37_live[v37_nlive++] = (uint8_t)v;
    split = c1_on && v37_nlive >= 2u && H->c1_run(c1_job, 0);
    if (!split) {
        for (i = 0; i < v37_nlive; i++)
            voice_render(&vs[v37_live[i]], v37_tb0, bus_l, bus_r);
        return;
    }
    v37_half(0, v37_tb0, bus_l, bus_r);
    c1_jobs++;
    if (H->c1_wait() != 0) {                        /* it did not end in time: core 1 is held, its half done here */
        c1_dead = 1;
        c1_on = 0;
        c1_job(0);
    }
    for (i = 0; i < CTL; i++)
        bus_l[i] += v37_b1l[i], bus_r[i] += v37_b1r[i];
}
