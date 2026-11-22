/* SPDX-License-Identifier: GPL-3.0-only */
/* The ARP 2600 with its 1601 sequencer as a fm1-dock module (arp_engine.c, arp_presets.h). The whole panel and
 * screen are its own (HOME held 1.5 s alone opens fm1-dock's list).
 *
 *   Keys          play, mono with portamento; while the sequence runs they transpose it (C3 as written)
 *   OCT- / OCT+   the keys' octave; in REC: a REST / a TIE written at the cursor
 *   PRESETS       the patch (ten of Jarre's manner, then USER 1-4); ALGORITHM: the sequence (eight, then USER 1-4)
 *   EDIT          VCO 1, VCO 2, MIX pages; GLO: VCF, VCF MOD; ENV: ADSR 1 (the filter), ADSR 2 (the amplifier);
 *                 LFO; FX: OUTPUT (volume, the host's reverb, the accent); KNOB1..4 turn the page's four
 *   SEQ           the sequencer page: SELECT moves the cursor, KNOB1 the step's note, KNOB2 its gate (REST, SHORT,
 *                 LONG, TIE, END), KNOB3 its accent, KNOB4 the tempo; REC: the keys write steps and move on. SEQ again:
 *                 the PATTERN page, the sequence's links (two bars as written, one a step up, one two up, again):
 *                 SELECT the link, KNOB1 its semitones, KNOB2 its bars, KNOB3 the filter SWEEP over the pattern,
 *                 KNOB4 the number of links (0: the sequence as written)
 *   PLAY          the sequence from its first step, or stops it; SAVE: the patch into a USER slot (on the SEQ
 *                 page: the sequence); ARP: all notes off, the transposition back
 * The patch's REVERB goes to the host's reverb (fx_reverb, when the host has one). The settings record keeps the patch and the
 * sequence as edited; the module's bank the USER slots. */
#include "hb_module.h"
#include "arp_engine.h"
#include "arp_presets.h"

#define N_USER 4
#define SET_MAGIC 0x36325041u                   /* "AP26" */
#define BANK_MAGIC 0x4B425041u                  /* "APBK" */
#define C_ACC HB_RGB(255, 140, 40)              /* the 2600's orange and blue-grey */
#define C_BLUE HB_RGB(90, 120, 170)
#define C_WHITE 0xFFFFu
#define C_GREY HB_RGB(110, 110, 110)
#define C_DIM HB_RGB(50, 50, 56)
#define C_BLACK 0u

static const hb_host_t *H;
static arp_t box;
static struct { uint32_t magic; uint8_t patch, seq, oct, page; arp_patch_t p; arp_seq_t s; } set;
static struct { uint32_t magic; arp_patch_t p[N_USER]; arp_seq_t s[N_USER]; } bank;
static uint32_t rec, cursor, lcur, drawn_page = ~0u, drawn_pos = ~0u, drawn_state = ~0u, t_save, dirty, bank_ok;
static uint32_t drawn_vals[4] = {~0u, ~0u, ~0u, ~0u};

enum { PG_VCO1, PG_VCO2, PG_MIX, PG_VCF, PG_VCF2, PG_ENV1, PG_ENV2, PG_LFO, PG_OUT, PG_SEQ, PG_CHAIN, N_PAGES };
enum { K_NUM, K_WAVE, K_OCT, K_DETUNE, K_ONOFF, K_LFOW, K_TIME, K_PW };
typedef struct { const char *name; uint8_t off, lo, hi, kind; } prm_t;
#define PO(f) ((uint8_t)__builtin_offsetof(arp_patch_t, f))
static const prm_t PRM[PG_SEQ][4] = {
    { {"WAVE", PO(vco1_wave), 0, 2, K_WAVE}, {"OCTAVE", PO(vco1_oct), 0, 4, K_OCT}, {"WIDTH", PO(vco1_pw), 5, 95, K_PW}, {"LEVEL", PO(vco1_lvl), 0, 100, K_NUM} },
    { {"WAVE", PO(vco2_wave), 0, 2, K_WAVE}, {"OCTAVE", PO(vco2_oct), 0, 4, K_OCT}, {"DETUNE", PO(vco2_detune), 0, 100, K_DETUNE}, {"LEVEL", PO(vco2_lvl), 0, 100, K_NUM} },
    { {"NOISE", PO(noise_lvl), 0, 100, K_NUM}, {"RING", PO(ring_lvl), 0, 100, K_NUM}, {"SYNC", PO(sync), 0, 1, K_ONOFF}, {"SUB", PO(sub_lvl), 0, 100, K_NUM} },
    { {"CUTOFF", PO(cutoff), 0, 100, K_NUM}, {"RESON", PO(reso), 0, 100, K_NUM}, {"ENV 1", PO(env_amt), 0, 100, K_NUM}, {"KEYBD", PO(kbd), 0, 100, K_NUM} },
    { {"LFO", PO(lfo_vcf), 0, 100, K_NUM}, {"S&H", PO(sh_vcf), 0, 100, K_NUM}, {"DRIVE", PO(drive), 0, 100, K_NUM}, {"GLIDE", PO(glide), 0, 100, K_NUM} },
    { {"ATTACK", PO(a1), 0, 100, K_TIME}, {"DECAY", PO(d1), 0, 100, K_TIME}, {"SUSTAIN", PO(s1), 0, 100, K_NUM}, {"RELEASE", PO(r1), 0, 100, K_TIME} },
    { {"ATTACK", PO(a2), 0, 100, K_TIME}, {"DECAY", PO(d2), 0, 100, K_TIME}, {"SUSTAIN", PO(s2), 0, 100, K_NUM}, {"RELEASE", PO(r2), 0, 100, K_TIME} },
    { {"WAVE", PO(lfo_wave), 0, 3, K_LFOW}, {"RATE", PO(lfo_rate), 0, 100, K_NUM}, {"PITCH", PO(lfo_pitch), 0, 100, K_NUM}, {"WIDTH", PO(lfo_pw), 0, 100, K_NUM} },
    { {"VOLUME", PO(volume), 0, 100, K_NUM}, {"REVERB", PO(reverb), 0, 100, K_NUM}, {"ACCENT", PO(accent), 0, 100, K_NUM}, {"ENV>VCO2", PO(env_pitch), 0, 100, K_NUM} },
};
static const char *const PAGE_NAME[N_PAGES] = {"VCO 1", "VCO 2", "MIXER", "VCF 4072", "VCF MOD", "ADSR 1  VCF", "ADSR 2  VCA", "LFO", "OUTPUT", "SEQUENCER 1601", "PATTERN"};
static const char *const WAVE_NAME[3] = {"SAW", "PULSE", "TRI"};
static const char *const LFOW_NAME[4] = {"TRI", "SQUARE", "SAW", "S&H"};
static const char *const GATE_NAME[5] = {"REST", "SHORT", "LONG", "TIE", "END"};
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static char *put_u(char *p, uint32_t v)
{
    if (v >= 1000u) *p++ = (char)('0' + v / 1000u % 10u);
    if (v >= 100u) *p++ = (char)('0' + v / 100u % 10u);
    if (v >= 10u) *p++ = (char)('0' + v / 10u % 10u);
    *p++ = (char)('0' + v % 10u);
    *p = 0;
    return p;
}
static char *put_s(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static void note_name(char *d, uint32_t n)      /* C4 = 60 */
{
    char *p = put_s(d, NOTE_NAME[n % 12u]);
    int32_t o = (int32_t)(n / 12u) - 1;
    if (o < 0) { *p++ = '-'; o = -o; }
    put_u(p, (uint32_t)o);
}

/* ---- the sound's hand-off: the reverb to the host, the tempo from its rhythm box */
static void reverb_out(void)
{
#ifdef HB_HAS_FX_REVERB                         /* (hb_module.h says so with the processing section: not in this host yet) */
    static int32_t sent = -1;
    int32_t mix = set.p.reverb;
    if (!HB_HOST_HAS(H, fx_reverb) || mix == sent) return;
    sent = mix;
    H->fx_reverb(mix > 0, 2, 32768, 32768, mix * 655);   /* HALL, size and damping half way */
#endif
}
static uint32_t running(void) { return (uint32_t)box.running; }
static int position(void) { return arp_position(&box); }
static void note_on(uint32_t note, uint32_t vel) { arp_note_on(&box, note, vel); }
static void note_off(uint32_t note) { arp_note_off(&box, note); }
static void all_off(void) { arp_all_off(&box); box.transpose = 0; }

/* ---- the patch and the sequence in hand */
static void apply_patch(void) { arp_set_patch(&box, &set.p); reverb_out(); dirty = 1; t_save = H->ms(); }
static void apply_seq(void) { arp_set_seq(&box, &set.s); dirty = 1; t_save = H->ms(); }
static void load_patch(uint32_t i)
{
    set.patch = (uint8_t)i;
    set.p = i < ARP_N_PATCHES ? ARP_PATCHES[i] : bank.p[(i - ARP_N_PATCHES) % N_USER];
    apply_patch();
    drawn_page = ~0u;
}
static void load_seq(uint32_t i)
{
    set.seq = (uint8_t)i;
    set.s = i < ARP_N_SEQS ? ARP_SEQS[i] : bank.s[(i - ARP_N_SEQS) % N_USER];
    apply_seq();
    drawn_page = ~0u;
}
static void bank_write(void)
{
    bank.magic = BANK_MAGIC;
    bank_ok = H->bank_save(&bank, sizeof bank) == 0;
}

/* ---- the screen */
static void value_text(char *d, const prm_t *q, uint32_t v)
{
    char *p = d;
    switch (q->kind) {
    case K_WAVE: put_s(d, WAVE_NAME[v % 3u]); break;
    case K_LFOW: put_s(d, LFOW_NAME[v % 4u]); break;
    case K_ONOFF: put_s(d, v ? "ON" : "OFF"); break;
    case K_OCT: { int32_t o = (int32_t)v - 2; if (o < 0) { *p++ = '-'; o = -o; } else if (o > 0) *p++ = '+'; put_u(p, (uint32_t)o); break; }
    case K_DETUNE: { int32_t c = (int32_t)v - 50; if (c < 0) { *p++ = '-'; c = -c; } else if (c > 0) *p++ = '+'; p = put_u(p, (uint32_t)c); put_s(p, "c"); break; }
    case K_PW: p = put_u(p, v); put_s(p, "%"); break;
    case K_TIME: {                              /* 1 ms .. 10 s, 2^(v 13.29 / 100) ms: shown near enough */
        uint32_t ms = 1u;
        int32_t k;
        for (k = 0; k < (int32_t)v / 8; k++) ms = ms * 2u + ms / 8u;   /* (2^1.063 per 8 steps) */
        ms += ms * (v % 8u) / 11u;
        if (ms >= 1000u) { p = put_u(p, ms / 1000u); *p++ = '.'; p = put_u(p, ms / 100u % 10u); put_s(p, " s"); }
        else { p = put_u(p, ms); put_s(p, " ms"); }
        break;
    }
    default: put_u(d, v); break;
    }
}
static void draw_head(void)
{
    char t[20], *p;
    H->fill(0, 0, 240, 44, C_BLACK);
    H->text(4, 2, 150, 0, set.p.name, C_ACC, 0);
    p = put_s(t, set.patch < ARP_N_PATCHES ? "P" : "U");
    put_u(p, set.patch < ARP_N_PATCHES ? set.patch + 1u : set.patch - ARP_N_PATCHES + 1u);
    H->text(160, 2, 76, 0, t, C_GREY, 2);
    H->text(4, 22, 232, 0, PAGE_NAME[set.page], C_BLUE, 0);
    H->fill(0, 43, 240, 1, C_DIM);
}
static void draw_params(void)
{
    uint32_t k;
    for (k = 0; k < 4u; k++) {
        const prm_t *q = &PRM[set.page][k];
        uint32_t v = ((const uint8_t *)&set.p)[q->off];
        char d[16];
        int32_t x = 4 + (int32_t)k * 59, w;
        if (v == drawn_vals[k]) continue;
        drawn_vals[k] = v;
        value_text(d, q, v);
        H->fill(x, 60, 56, 120, C_BLACK);
        H->text(x, 60, 56, 0, q->name, C_GREY, 1);
        H->text(x, 84, 56, 0, d, C_WHITE, 1);
        w = (int32_t)((v - q->lo) * 52u / (q->hi - q->lo ? q->hi - q->lo : 1u));
        H->fill(x + 2, 112, 52, 6, C_DIM);
        H->fill(x + 2, 112, w, 6, C_ACC);
    }
}
static void draw_seq(uint32_t full)
{
    uint32_t i, pos = (uint32_t)position(), state = cursor | rec << 5 | running() << 6 | (uint32_t)set.s.tempo << 8;
    char t[24], *p;
    if (full) { H->fill(0, 44, 240, 196, C_BLACK); drawn_pos = ~0u; drawn_state = ~0u; }
    if (state != drawn_state || pos != drawn_pos) {
        for (i = 0; i < ARP_STEPS; i++) {
            const arp_step_t *s = &set.s.step[i];
            int32_t x = 2 + (int32_t)(i % 8u) * 30, y = 52 + (int32_t)(i / 8u) * 46;
            uint16_t bg = i == pos ? C_ACC : s->gate == ARP_REST ? C_DIM : s->gate == ARP_END ? HB_RGB(80, 30, 30) : C_BLUE;
            uint16_t fg = i == pos ? C_ACC : C_WHITE;
            H->fill(x, y, 28, 40, bg);          /* a frame: the host's text paints its own black line */
            H->fill(x + 2, y + 2, 24, 36, C_BLACK);
            if (i == cursor) { H->fill(x, y, 28, 2, C_WHITE); H->fill(x, y + 38, 28, 2, C_WHITE); }
            if (s->gate == ARP_END) { H->text(x, y + 12, 28, 0, "END", fg, 1); continue; }
            if (s->gate == ARP_REST) { H->text(x, y + 12, 28, 0, "-", fg, 1); continue; }
            note_name(t, s->note);
            H->text(x, y + 4, 28, 0, t, fg, 1);
            H->text(x, y + 22, 28, 0, s->gate == ARP_TIE ? "~" : s->gate == ARP_LONG ? "=" : ".", s->accent ? C_ACC : fg, 1);
            if (s->accent) H->fill(x + 11, y + 35, 6, 2, C_ACC);
        }
        drawn_pos = pos;
    }
    if (state != drawn_state) {
        const arp_step_t *s = &set.s.step[cursor];
        H->fill(0, 146, 240, 94, C_BLACK);
        p = put_s(t, "STEP "); p = put_u(p, cursor + 1u); p = put_s(p, "  ");
        if (s->gate == ARP_REST || s->gate == ARP_END) put_s(p, GATE_NAME[s->gate]);
        else { note_name(p, s->note); p += 3; while (*p) p++; *p++ = ' '; p = put_s(p, GATE_NAME[s->gate]); if (s->accent) put_s(p, " >"); }
        H->text(4, 150, 232, 0, t, C_WHITE, 0);
        p = put_s(t, "TEMPO "); put_u(p, set.s.tempo);
        H->text(4, 172, 120, 0, t, C_GREY, 0);
        H->text(124, 172, 112, 0, running() ? "RUNNING" : "STOPPED", running() ? C_ACC : C_GREY, 2);
        p = put_s(t, set.seq < ARP_N_SEQS ? "S" : "U"); p = put_u(p, set.seq < ARP_N_SEQS ? set.seq + 1u : set.seq - ARP_N_SEQS + 1u);
        p = put_s(p, " "); put_s(p, set.s.name);
        H->text(4, 196, 160, 0, t, C_BLUE, 0);
        H->text(164, 196, 72, 0, rec ? "REC" : "", C_ACC, 2);
        H->text(4, 220, 232, 0, rec ? "KEYS WRITE  OCT- REST  OCT+ TIE" : "SELECT STEP  KNOBS NOTE GATE BPM", C_GREY, 1);
        drawn_state = state;
    }
}
static void draw_chain(uint32_t full)
{
    uint32_t k, n = set.s.n_chain, link = (uint32_t)arp_link(&box);
    uint32_t state = lcur | n << 4 | (uint32_t)set.s.sweep << 8 | (link & 15u) << 16 | running() << 20;
    char t[24], *p;
    if (full) { H->fill(0, 44, 240, 196, C_BLACK); drawn_state = ~0u; }
    if (state == drawn_state) return;
    for (k = 0; k < ARP_CHAIN; k++) {
        int32_t x = 4 + (int32_t)(k % 4u) * 58, y = 52 + (int32_t)(k / 4u) * 50, tr = set.s.tr[k];
        uint16_t bg = k == link ? C_ACC : k < n ? C_BLUE : C_DIM, fg = k == link ? C_ACC : k < n ? C_WHITE : C_GREY;
        H->fill(x, y, 54, 44, bg);              /* a frame (see draw_seq) */
        H->fill(x + 2, y + 2, 50, 40, C_BLACK);
        if (k == lcur) { H->fill(x, y, 54, 2, C_WHITE); H->fill(x, y + 42, 54, 2, C_WHITE); }
        p = t; if (tr < 0) { *p++ = '-'; tr = -tr; } else if (tr > 0) *p++ = '+'; put_u(p, (uint32_t)tr);
        H->text(x, y + 6, 54, 0, t, fg, 1);
        p = put_u(t, set.s.bars[k] ? set.s.bars[k] : 1u); put_s(p, set.s.bars[k] == 1u || !set.s.bars[k] ? " BAR" : " BARS");
        H->text(x, y + 26, 54, 0, t, fg, 1);
    }
    H->fill(0, 152, 240, 88, C_BLACK);
    p = put_s(t, "LINK "); p = put_u(p, lcur + 1u); p = put_s(p, "  "); put_s(p, lcur < n ? "IN THE PATTERN" : "UNUSED");
    H->text(4, 156, 232, 0, t, C_WHITE, 0);
    p = put_s(t, "SWEEP "); p = put_u(p, set.s.sweep); p = put_s(p, "%   LINKS "); put_u(p, n);
    H->text(4, 180, 232, 0, t, n ? C_ACC : C_GREY, 0);
    H->text(4, 204, 232, 0, n ? "SEMI  BARS  SWEEP  LINKS" : "NO PATTERN: AS WRITTEN. KNOB4 ADDS", C_GREY, 1);
    H->text(4, 222, 232, 0, "SELECT: LINK   SEQ: THE STEPS", C_GREY, 1);
    drawn_state = state;
}
static void draw(uint32_t full)
{
    uint32_t k;
    hb_key0(H, 53 + 12 * (int32_t)set.oct);   /* (the SMK-37's outer keys go on from this octave) */
    if (full || set.page != drawn_page) {
        H->fill(0, 0, 240, 240, C_BLACK);
        draw_head();
        drawn_page = set.page;
        for (k = 0; k < 4u; k++) drawn_vals[k] = ~0u;
        if (set.page == PG_SEQ) draw_seq(1);
        if (set.page == PG_CHAIN) draw_chain(1);
    }
    if (set.page == PG_SEQ) draw_seq(0);
    else if (set.page == PG_CHAIN) draw_chain(0);
    else {
        draw_params();
        if (running() && (H->ms() & 512u) != drawn_pos) {   /* a small sign that the sequence runs */
            drawn_pos = H->ms() & 512u;
            H->text(4, 200, 232, 0, "SEQ RUNNING", drawn_pos ? C_ACC : C_GREY, 1);
        }
    }
    if (dirty && H->ms() - t_save > 2000u) {
        set.magic = SET_MAGIC;
        H->save(&set, sizeof set);
        dirty = 0;
    }
    H->led(HB_BTN_PLAY, running());
    H->led(HB_BTN_REC, rec);
    H->led(HB_BTN_SEQ, set.page == PG_SEQ || set.page == PG_CHAIN);
}

/* ---- the panel */
static void page_to(uint32_t pg) { set.page = (uint8_t)pg; dirty = 1; t_save = H->ms(); }
static void cycle(uint32_t a, uint32_t b, uint32_t c)   /* a button's pages, in turn (c: 0 for two) */
{
    page_to(set.page == a ? b : set.page == b && c ? c : a);
}
static void key_down(uint32_t k, int32_t vel)
{
    uint32_t note = 53u + k + 12u * set.oct;     /* F3 at oct 0 */
    if (rec && set.page == PG_SEQ && !running()) {
        arp_step_t *s = &set.s.step[cursor];
        s->note = (uint8_t)(note > 127u ? 127u : note);
        if (s->gate == ARP_REST || s->gate == ARP_END || s->gate == ARP_TIE) s->gate = ARP_LONG;
        cursor = (cursor + 1u) % ARP_STEPS;
        apply_seq();
    }
    note_on(note, vel > 0 ? (uint32_t)vel : 100u);
}
static void key_up(uint32_t k) { note_off(53u + k + 12u * set.oct); }
static void rec_step(uint32_t gate)              /* REC: a rest or a tie at the cursor, then on */
{
    set.s.step[cursor].gate = (uint8_t)gate;
    cursor = (cursor + 1u) % ARP_STEPS;
    apply_seq();
}
static void ui(uint32_t ev, int32_t arg)
{
    uint32_t id = HB_EV_ID(ev);
    int32_t v;
    switch (HB_EV_KIND(ev)) {
    case HB_EV_KEY_DOWN: key_down(id, arg); return;
    case HB_EV_KEY_UP: key_up(id); return;
    case HB_EV_BTN_DOWN:
        switch (id) {
        case HB_BTN_EDIT: cycle(PG_VCO1, PG_VCO2, PG_MIX); break;
        case HB_BTN_GLO: cycle(PG_VCF, PG_VCF2, 0); break;
        case HB_BTN_ENV: cycle(PG_ENV1, PG_ENV2, 0); break;
        case HB_BTN_LFO: page_to(PG_LFO); break;
        case HB_BTN_FX: page_to(PG_OUT); break;
        case HB_BTN_SEQ: page_to(set.page == PG_SEQ ? PG_CHAIN : PG_SEQ); break;
        case HB_BTN_PLAY:
            arp_run(&box, !box.running);
            if (running()) rec = 0;
            drawn_state = ~0u;
            break;
        case HB_BTN_REC: rec = !rec; if (rec) page_to(PG_SEQ); drawn_state = ~0u; break;
        case HB_BTN_ARP: all_off(); break;
        case HB_BTN_SAVE: {
            if (set.page == PG_SEQ) {
                uint32_t u = set.seq >= ARP_N_SEQS ? set.seq - ARP_N_SEQS : 0;
                bank.s[u] = set.s;
                set.seq = (uint8_t)(ARP_N_SEQS + u);
            } else {
                uint32_t u = set.patch >= ARP_N_PATCHES ? set.patch - ARP_N_PATCHES : 0;
                bank.p[u] = set.p;
                set.patch = (uint8_t)(ARP_N_PATCHES + u);
            }
            bank_write();
            H->log(bank_ok ? "arp2600: saved" : "arp2600: save failed");
            drawn_page = ~0u;
            break;
        }
        case HB_BTN_OCT_DOWN:
            if (rec && set.page == PG_SEQ) rec_step(ARP_REST);
            else if (set.oct > 0) { set.oct--; dirty = 1; }
            break;
        case HB_BTN_OCT_UP:
            if (rec && set.page == PG_SEQ) rec_step(ARP_TIE);
            else if (set.oct < 4u) { set.oct++; dirty = 1; }
            break;
        default: break;
        }
        return;
    case HB_EV_ENC:
        switch (id) {
        case HB_ENC_PRESETS:
            v = (int32_t)set.patch + arg;
            v = v < 0 ? 0 : v >= ARP_N_PATCHES + N_USER ? ARP_N_PATCHES + N_USER - 1 : v;
            load_patch((uint32_t)v);
            return;
        case HB_ENC_ALGORITHM:
            v = (int32_t)set.seq + arg;
            v = v < 0 ? 0 : v >= ARP_N_SEQS + N_USER ? ARP_N_SEQS + N_USER - 1 : v;
            load_seq((uint32_t)v);
            return;
        case HB_ENC_SELECT:
            if (set.page == PG_SEQ) cursor = (uint32_t)((((int32_t)cursor + arg) % ARP_STEPS) + ARP_STEPS) % ARP_STEPS;
            if (set.page == PG_CHAIN) lcur = (uint32_t)((((int32_t)lcur + arg) % ARP_CHAIN) + ARP_CHAIN) % ARP_CHAIN;
            return;
        default: break;
        }
        if (id < HB_ENC_KNOB1 || id > HB_ENC_KNOB4) return;
        if (set.page == PG_CHAIN) {             /* the link's semitones and bars, the sweep, the links */
            switch (id) {
            case HB_ENC_KNOB1: v = (int32_t)set.s.tr[lcur] + arg; set.s.tr[lcur] = (int8_t)(v < -12 ? -12 : v > 12 ? 12 : v); break;
            case HB_ENC_KNOB2: v = (int32_t)(set.s.bars[lcur] ? set.s.bars[lcur] : 1u) + arg; set.s.bars[lcur] = (uint8_t)(v < 1 ? 1 : v > 8 ? 8 : v); break;
            case HB_ENC_KNOB3: v = (int32_t)set.s.sweep + 5 * arg; set.s.sweep = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v); break;
            default: v = (int32_t)set.s.n_chain + arg; set.s.n_chain = (uint8_t)(v < 0 ? 0 : v > ARP_CHAIN ? ARP_CHAIN : v); break;
            }
            if (lcur >= set.s.n_chain && id != HB_ENC_KNOB3 && id != HB_ENC_KNOB4 && set.s.n_chain < ARP_CHAIN)
                set.s.n_chain = (uint8_t)(lcur + 1u);   /* (a link turned: it is in) */
            apply_seq();
            return;
        }
        if (set.page == PG_SEQ) {
            arp_step_t *s = &set.s.step[cursor];
            switch (id) {
            case HB_ENC_KNOB1: v = (int32_t)s->note + arg; s->note = (uint8_t)(v < 24 ? 24 : v > 96 ? 96 : v); if (s->gate == ARP_REST) s->gate = ARP_LONG; break;
            case HB_ENC_KNOB2: v = (int32_t)s->gate + arg; s->gate = (uint8_t)(v < 0 ? 0 : v > ARP_END ? ARP_END : v); break;
            case HB_ENC_KNOB3: s->accent = arg > 0; break;
            default:
                v = (int32_t)set.s.tempo + arg; set.s.tempo = (uint16_t)(v < 40 ? 40 : v > 240 ? 240 : v);
                break;
            }
            apply_seq();
        } else {
            const prm_t *q = &PRM[set.page][id - HB_ENC_KNOB1];
            uint8_t *f = (uint8_t *)&set.p + q->off;
            int32_t step = q->hi - q->lo > 10 ? (arg > 0 ? arg : arg) : (arg > 0 ? 1 : -1);
            v = (int32_t)*f + step;
            *f = (uint8_t)(v < q->lo ? q->lo : v > q->hi ? q->hi : v);
            apply_patch();
        }
        return;
    default: return;
    }
}
static void midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    switch (st & 0xF0u) {
    case 0x90: if (d2) note_on(d1, d2); else note_off(d1); break;
    case 0x80: note_off(d1); break;
    case 0xC0: if (d1 < ARP_N_PATCHES + N_USER) load_patch(d1); break;
    case 0xB0:
        if (d1 == 74u) { set.p.cutoff = (uint8_t)(d2 * 100u / 127u); apply_patch(); }
        else if (d1 == 71u) { set.p.reso = (uint8_t)(d2 * 100u / 127u); apply_patch(); }
        else if (d1 == 91u) { set.p.reverb = (uint8_t)(d2 * 100u / 127u); apply_patch(); }
        else if (d1 == 123u || d1 == 120u) all_off();
        break;
    default: break;
    }
}
static void note(uint32_t ch, uint32_t key, uint32_t vel) { (void)ch; if (vel) note_on(key, vel); else note_off(key); }
static void cc(uint32_t ch, uint32_t num, uint32_t val) { midi(0xB0u | ch, num, val); }
static void render(int32_t *out, uint32_t n) { arp_render(&box, out, n); }

static int init(const hb_host_t *h)
{
    H = h;
    arp_init(&box);
    if (H->bank_load(&bank, sizeof bank) || bank.magic != BANK_MAGIC) {
        uint32_t i;
        for (i = 0; i < N_USER; i++) { bank.p[i] = ARP_PATCHES[0]; bank.s[i] = ARP_SEQS[0]; }
        bank.magic = BANK_MAGIC;
    }
    if (H->load(&set, sizeof set) || set.magic != SET_MAGIC) {
        set.magic = SET_MAGIC;
        set.oct = 1;
        set.page = PG_SEQ;
        set.patch = 0;
        set.seq = 0;
        set.p = ARP_PATCHES[0];
        set.s = ARP_SEQS[0];
    }
    if (set.page >= N_PAGES) set.page = PG_SEQ;
    if (set.oct > 4u) set.oct = 1;
    arp_set_patch(&box, &set.p);
    arp_set_seq(&box, &set.s);
    reverb_out();
    H->log("arp2600: ARP 2600 and its 1601");
    dirty = 0;
    return 0;
}
static void release(void) { arp_run(&box, 0); arp_all_off(&box); }

HB_DESC = {HB_ABI, "ARP 2600", init, release, note, cc, render, ui, draw, 0, 0, HB_F_SCREEN | HB_F_PANEL, midi};
