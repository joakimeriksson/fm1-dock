/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (C) 2026 Andrew GG — Roland S-1 parity engine */
/* The step sequencer: two tracks that share 256 steps (A from the front, B from the back, so a length change
 * never moves the other track's steps). A step holds a chord of up to four notes, a velocity, a gate (or a tie
 * into the next step), a ratchet (1-4 hits) and a chance. Each track has its own length, note value, swing and
 * transpose. A plays sound A; B plays it too, or its own program (trk[1].pad[0] = program + 1, pad[1] = how much of
 * it passes the effects by, 0 = none), straight into the voices (not through the arpeggiator).
 * The player runs in the audio ISR per control block, as the arpeggiator does; the main loop edits `pat`. */
#define SEQ_STEPS 256u
#define SEQ_CHORD 4u
#define GATE_TIE 127u                           /* a gate of 127: the notes hold into the next step */
#define NPAT NBLOB                              /* patterns, one flash sector each */
#define PAT_MAGIC 0x31535653u                   /* "SVS1" */

typedef struct { uint8_t note[SEQ_CHORD], vel, gate, ratch, chance; } step_t;   /* note 0 = none; ratch 0..3 = 1..4 hits */
#define STEP_OFF 0x80u                          /* in ratch: the step is switched off, its notes kept */
typedef struct { uint16_t len; uint8_t div, swing; int8_t transp; uint8_t mute, pad[2]; } seqtrk_t;   /* swing 50..75 %; pad: B's sound */
#define B_PROG pad[0]                           /* track B: its own program + 1, 0 = it plays sound A */
#define B_DRY pad[1]                            /* track B's own sound: 0 all into the effects .. 127 all dry */
typedef struct {
    uint32_t magic;
    seqtrk_t trk[2];
    step_t st[SEQ_STEPS];
    int16_t draw_table[16];   /* OSC DRAW custom 16-step wavetable */
    uint16_t chop_mask[4];    /* OSC CHOP masks: [0]=Sqr, [1]=Saw, [2]=Sub, [3]=Noise */
    int8_t master_prob;       /* Master probability (-100..+100) */
    uint8_t step_loop_start;  /* Step loop start index */
    uint8_t step_loop_end;    /* Step loop end index */
    uint8_t tempo;            /* Pattern tempo (BPM, 40..240) */
    uint8_t pad[4];
} pattern_t;

static pattern_t pat;                           /* the pattern now: the main loop edits, the ISR plays */
static step_t *seq_step(uint32_t t, uint32_t i) { return &pat.st[t ? SEQ_STEPS - 1u - i : i]; }
static uint32_t seq_b_own(void) { return pat.trk[1].B_PROG != 0u; }
static uint32_t seq_b_dry(void) { return b_own ? (pat.trk[1].B_DRY > 127u ? 127u : pat.trk[1].B_DRY) : 0u; }
static inline const int16_t *seq_draw_table(void) { return pat.draw_table; }
static inline const uint16_t *seq_chop_masks(void) { return pat.chop_mask; }

/* main loop -> ISR */
static volatile uint32_t ctl_seq_run, ctl_seq_restart;
/* the next pattern, as zp12 switches loops: the main loop reads it into pat_q and sets ctl_pat_swap; the ISR swaps it
 * in when track A starts over (the one it leaves goes to pat_old, so its edits can be saved at the stop) */
static pattern_t pat_q, pat_old;
static volatile uint32_t ctl_pat_swap, ctl_pat_keep, ui_pat_swapped;   /* keep: the one left goes to pat_old */
/* ISR -> main loop: where each track is (step, and how far into it, Q8) */
static volatile uint32_t ui_seq_pos[2], ui_seq_frac[2], ui_seq_dur[2], ui_seq_ticks, ui_seq_beat;   /* dur: frames */

/* ---- a fresh pattern */
static void step_clear(step_t *s)
{
    uint32_t i;
    for (i = 0; i < SEQ_CHORD; i++) s->note[i] = 0;
    s->vel = 100;
    s->gate = 96;                               /* 76 % */
    s->ratch = 0;
    s->chance = 100;
}
static void pat_init(pattern_t *p)
{
    uint32_t i, t;
    p->magic = PAT_MAGIC;
    for (i = 0; i < SEQ_STEPS; i++) step_clear(&p->st[i]);
    for (t = 0; t < 2u; t++) {
        p->trk[t].len = t ? 0 : 16;             /* B off until it gets a length */
        p->trk[t].div = D_16;
        p->trk[t].swing = 50;
        p->trk[t].transp = 0;
        p->trk[t].mute = 0;
        p->trk[t].pad[0] = p->trk[t].pad[1] = 0;
    }
    for (i = 0; i < 16u; i++) p->draw_table[i] = (int16_t)(((int32_t)i - 7) * 4096);
    p->chop_mask[0] = 0xFFFFu;
    p->chop_mask[1] = 0xFFFFu;
    p->chop_mask[2] = 0xFFFFu;
    p->chop_mask[3] = 0xFFFFu;
    p->master_prob = 0;
    p->step_loop_start = 0;
    p->step_loop_end = 15;
    p->tempo = 120;
}
static void pat_factory(uint32_t n, pattern_t *p)   /* a pattern never saved: empty, track A 16 sixteenths */
{
    pat_init(p);
    if (n < NFACT) {
        uint32_t i;
        for (i = 0; i < 16u; i++) p->draw_table[i] = FACT_WAVE[n].draw[i];
        for (i = 0; i < 4u; i++) p->chop_mask[i] = FACT_WAVE[n].chop[i];
        p->tempo = FACT_TEMPO[n];
    }
}

/* ---- the player (ISR, per CTL block) */
typedef struct { uint32_t cnt, dur, step, sub, rat_n, rat_i, gate_at, nsnd, play, tie, part; uint8_t snd[SEQ_CHORD]; } seqrun_t;
static seqrun_t sr[2];
static uint32_t seq_was, seq_beat_cnt, seq_beat_n, count_left;

static void seq_release(seqrun_t *r)
{
    uint32_t i;
    for (i = 0; i < r->nsnd; i++) voice_release(r->part, r->snd[i], 1);
    r->nsnd = 0;
    r->part = r == &sr[1] && b_own;             /* (what sounds lets go by the part it began on) */
}
static uint32_t seq_note(const step_t *s, uint32_t i, int32_t transp)
{
    return (uint32_t)clampi((int32_t)s->note[i] + transp, 1, 127);
}
static void seq_sound(seqrun_t *r, const step_t *s, int32_t transp)
{
    uint32_t i;
    for (i = 0; i < SEQ_CHORD && s->note[i] && r->nsnd < SEQ_CHORD; i++) {
        uint32_t n = seq_note(s, i, transp);
        voice_trigger(r->part, n, s->vel ? s->vel : 100u);
        r->snd[r->nsnd++] = (uint8_t)n;
    }
}
static uint32_t seq_has(const step_t *s, uint32_t m, int32_t transp)   /* the step plays note m */
{
    uint32_t i;
    for (i = 0; i < SEQ_CHORD && s->note[i]; i++)
        if (seq_note(s, i, transp) == m) return 1;
    return 0;
}

/* a tie, note by note: what the new step plays again keeps sounding, the rest lets go, the new ones start */
static void seq_tie_into(seqrun_t *r, const step_t *s, int32_t transp, uint32_t play)
{
    uint32_t i, j, keep = 0;
    for (i = j = 0; i < r->nsnd; i++) {
        if (play && seq_has(s, r->snd[i], transp)) { r->snd[j++] = r->snd[i]; keep++; }
        else voice_release(r->part, r->snd[i], 1);
    }
    r->nsnd = j;
    if (!play) return;
    for (i = 0; i < SEQ_CHORD && s->note[i] && r->nsnd < SEQ_CHORD; i++) {
        uint32_t m = seq_note(s, i, transp);
        for (j = 0; j < keep && r->snd[j] != m; j++) ;
        if (j < keep) continue;
        voice_trigger(r->part, m, s->vel ? s->vel : 100u);
        r->snd[r->nsnd++] = (uint8_t)m;
    }
}

static void seq_tick(void)
{
    uint32_t t, q;
    if (!ctl_seq_run) {
        if (seq_was) { seq_release(&sr[0]); seq_release(&sr[1]); seq_was = 0; }
        return;
    }
    if (!seq_was || ctl_seq_restart) {          /* start: step 0 of both tracks in this block */
        ctl_seq_restart = 0;
        for (t = 0; t < 2u; t++) {
            seq_release(&sr[t]);
            sr[t].step = 0xFFFFFFFFu;
            sr[t].cnt = sr[t].dur = 0;
            sr[t].play = sr[t].tie = 0;
        }
        seq_beat_cnt = 0;
        seq_beat_n = 0;
        count_left = ctl_count ? 4u : 0u;
        seq_was = 1;
    }
    q = ctl_ext_quarter ? ctl_ext_quarter : 60u * FS / clampi(ctl_tempo, 40, 240);
    if (seq_beat_cnt == 0u) {                   /* a beat: the PLAY light, the click, the count-in */
        if (ctl_count && !count_left) {         /* counted in: the tracks from the top, now */
            ctl_count = 0;
            seq_beat_n = 0;
            for (t = 0; t < 2u; t++) { sr[t].step = 0xFFFFFFFFu; sr[t].cnt = sr[t].dur = 0; sr[t].play = sr[t].tie = 0; }
        }
        ui_seq_beat++;
        if (ctl_click || ctl_count) click_start(seq_beat_n % 4u == 0u);
        seq_beat_n++;
        if (count_left) count_left--;
        ui_seq_count = ctl_count ? count_left + 1u : 0u;
    }
    seq_beat_cnt += CTL;
    if (seq_beat_cnt >= q) seq_beat_cnt = 0;
    if (ctl_count) return;                      /* counting in: no steps yet */
    for (t = 0; t < 2u; t++) {
        seqrun_t *r = &sr[t];
        const seqtrk_t *k = &pat.trk[t];
        const step_t *s;
        uint32_t len = k->len;
        if (!len) { seq_release(r); r->tie = 0; continue; }
        if (r->cnt >= r->dur) {                 /* the next step */
            uint32_t base = q / DIV_PER_QUARTER[k->div % NDIV], sw;
            r->cnt -= r->dur;
            r->step = r->step + 1u >= len ? 0u : r->step + 1u;
            if (t == 0u && r->step == 0u && ctl_pat_swap) {   /* track A from the top: the next pattern */
                if (ctl_pat_keep) pat_old = pat;
                pat = pat_q;
                if (ctl_bq_swap) { live_b = live_bq; ctl_bq_swap = 0; }   /* B's sound comes along */
                ctl_pat_swap = 0;
                parts_point();
                ui_pat_swapped++;
                len = k->len;
            }
            sw = base * (uint32_t)(clampi(k->swing, 50, 75) - 50) / 50u;
            r->dur = r->step & 1u ? base - sw : base + sw;   /* swing: the first of each pair longer */
            if (r->dur < CTL) r->dur = CTL;
            s = seq_step(t, r->step);
            uint32_t eff_chance = s->chance;
            if (eff_chance < 100u) {
                int32_t c = (int32_t)eff_chance + (int32_t)pat.master_prob;
                eff_chance = (uint32_t)clampi(c, 0, 99);
            }
            r->play = s->note[0] && !(s->ratch & STEP_OFF) && !k->mute && (s->chance >= 100u || xorshift(&rnd_state) % 100u < eff_chance);
            if (r->tie)                         /* a tie: the notes the step plays again hold */
                seq_tie_into(r, s, k->transp, r->play);
            else {
                seq_release(r);
                if (r->play) seq_sound(r, s, k->transp);
            }
            r->rat_n = (s->ratch & 3u) + 1u;
            r->sub = r->dur / r->rat_n;
            r->rat_i = 0;
            r->tie = r->play && s->gate >= GATE_TIE;
            r->gate_at = s->gate >= GATE_TIE ? 0xFFFFFFFFu : (uint32_t)clampi((int32_t)((r->sub * (s->gate + 1u)) >> 7), CTL, (int32_t)r->sub);
            ui_seq_ticks++;
        } else {
            s = seq_step(t, r->step < len ? r->step : 0u);
            if (r->play && r->rat_n > 1u && r->sub) {   /* a ratchet: the next hit */
                uint32_t i = r->cnt / r->sub;
                if (i > r->rat_i && i < r->rat_n) { r->rat_i = i; seq_release(r); seq_sound(r, s, k->transp); }
            }
        }
        if (r->nsnd && r->gate_at != 0xFFFFFFFFu && r->cnt - r->rat_i * r->sub >= r->gate_at) seq_release(r);
        r->cnt += CTL;
        ui_seq_dur[t] = r->dur;
        ui_seq_pos[t] = r->step;
        ui_seq_frac[t] = r->dur ? r->cnt * 256u / r->dur : 0u;
    }
}
