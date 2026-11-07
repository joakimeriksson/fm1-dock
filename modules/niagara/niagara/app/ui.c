/* SPDX-License-Identifier: GPL-3.0-only */
/* Niagara Drift UI: the control map (Figma "Niagara Drift control map: draft 1"). Two drones, after the CPM DS-2.
 * Adapted from FoMni's ui.c: its drawing helpers, knob acceleration and three-band screen. Part of the
 * unity build (after gfx.c and project.c); the host simulator includes it too.
 *
 * Always live:
 *   KNOB 1-2       drone I's two values on the page shown, KNOB 3-4 drone II's; the Play page (HOME):
 *                  Pitch and Cutoff. A pitch is continuous, 27 Hz .. 1.6 kHz, a slow full turn an octave;
 *                  hold SEL for a cent a detent. With PITCH LINK on, KNOB 3 moves drone II's offset.
 *   SELECT         LFO rate      ALGORITHM   MOVEMENT: every LFO depth together, 0 .. 200 %
 *   PRESETS        GESTURE TIME: every gesture's speed, x0.1 .. x10 (v0.2)
 *   PLAY           the drone fades in / out      MASTER   volume      SAVE   saves
 * One button away (tap: the knobs stay on its page, pressed again its second page; hold: they switch
 * only while it's held, and the page you were on comes back when it's let go):
 *   HOME Play, Play 2 (Level, Reso) · ENV Tone (Noise, Colour: white .. pink) · EDIT Osc (X-Wave, PW), Source
 *   (Osc mix, Drive) · LFO LFO (depths), Mod (Rate mod, Mod by, X-mod amount, Delay LFO depth) · SEQ Partner ·
 *   GLO Setup, Setup 2 (Switch time, Scene time, Tune, Tuning), Setup 3 (Speaker,
 *   Pressure) (global). The top bar shows a dot for each of a
 *   button's pages.
 *   OCT- / OCT+    the LFO's Wave I / Wave II: a tap is the next shape, round and round; SEL + OCT the one
 *                  before; lit while its wave is up
 * White keys: 16 scenes. Tap: recall (every value morphs there over the Scene time; a knob turned stops
 *   it); hold (a visit): there while held, back to where it was when let go (a knob turned: it stays);
 *   hold SAVE and tap: store. Lit: the slot holds a scene; blinking slowly: the sound is that scene (stored or
 *   recalled, and no knob, switch or LFO control touched since).
 * Black keys, lit when on (or on the second choice), following the page:
 *   F#3 G#3 A#3   C#4 D#4   F#4 G#4 A#4   C#5 D#5   F#5. Normally, by meaning: drone I (Partner, Sub
 *   (round: -1, -2 oct, 1/3, 1/5), Filter (round: LP, BP, HP)) | global (Pitch lock, LFO reset) | drone II
 *   (the same) | links (Pitch link, Hard sync) | Cross-mod. On the LFO page: Osc wave, Osc LFO -> X-Wave, Filter LFO -> Reso | Filter wave,
 *   LFO reset | Osc wave, Osc LFO -> X-Wave, Filter LFO -> Reso | Filter wave, Delay wave | Cross-mod. A
 *   wave key goes round I, II, I+II (lit for II, winking for both). LFO reset (either layer): a tap restarts
 *   the LFO; held half a second, it locks the LFO to the Pulse's bars (restarting each bar; lit while
 *   locked), held again, free.
 * ARP            tap: the SWELL on / off (lit, in time); hold: its page (Rate, Depth, Shape, Swing); two
 *                quick taps: its page, to stay
 * REC            the MOTION LOOP: tap to record what the knobs do, tap again to loop it from the start,
 *                tap again to stop (everything stays where it is); up to 5 minutes (lit: blinking recording,
 *                steady looping)
 *   FX             Delay: type, time, repeats, mix; again: Reverb: type, length (to Inf), damp, mix
 *                  (the delay LFO's tap: the LFO page's key in drone II's filter group)
 *   ENV            Tone 1: each drone's noise and colour; again: Tone 2: character, feedback
 * Saving: SAVE, and by itself a few seconds after a change while stopped (a flash write stops the audio
 * for a moment). MIDI in: notes on channel 1 tune drone I, on channel 2 drone II. */

/* ------------------------------------------------------------- pages --- */
/* the pages (2026-10-10, from the review: what's played together sits together, no half-empty pages) */
enum { V_PLAY, V_PLAY2, V_TONE, V_TONE2, V_OSC, V_SOURCE, V_LFO, V_LFO2, V_PARTNER, V_MOVES, V_SETUP, V_SETUP2,
       V_PULSE, V_FX, V_FX2, NVIEWS };
static const char *const VIEW_NAME[NVIEWS] = {"Play 1", "Play 2", "Tone 1", "Life", "Osc", "Sub · Drive", "LFO depths",
                                              "Mods", "Partner", "Pitch moves", "Setup 1", "Setup 2", "Swell", "Delay", "Reverb"};
/* what each knob moves on each page: a drone's parameter (g 0 or 1, GP_*), a global one (g G, P_*), or
 * nothing (g NONE) */
#define G 2
#define NONE 3
typedef struct {
    uint8_t g, p;
} knob_t;
static const knob_t VIEW_KNOB[NVIEWS][4] = {
    {{0, GP_PITCH}, {0, GP_CUTOFF}, {1, GP_PITCH}, {1, GP_CUTOFF}},
    {{0, GP_LEVEL}, {0, GP_RESO}, {1, GP_LEVEL}, {1, GP_RESO}},
    {{0, GP_NOISE}, {0, GP_NCOLOR}, {1, GP_NOISE}, {1, GP_NCOLOR}},
    {{G, P_CHAR}, {G, P_FEEDBACK}, {G, P_AGE}, {G, P_DRIFT}},
    {{0, GP_WAVE}, {0, GP_WIDTH}, {1, GP_WAVE}, {1, GP_WIDTH}},
    {{0, GP_MIX}, {0, GP_DRIVE}, {1, GP_MIX}, {1, GP_DRIVE}},
    {{0, GP_OLFO_DEPTH}, {0, GP_FLFO_DEPTH}, {1, GP_OLFO_DEPTH}, {1, GP_FLFO_DEPTH}},
    {{G, P_LFO_RMOD}, {G, P_LFO_RMOD_SRC}, {G, P_XMOD_AMT}, {G, P_DLY_LFO_DEPTH}},
    {{0, GP_INTERVAL}, {0, GP_DETUNE}, {1, GP_INTERVAL}, {1, GP_DETUNE}},
    {{G, P_SHIFT_MOVE}, {G, P_GLIDE}, {G, P_WANDER}, {G, P_WSCALE}},   /* Shift's move, Glide; Wander, its scale */
    /* GLO: the room's settings only, none in a sound (Evan, 2026-10-18): the times, then the unit and the room */
    {{G, P_FADE}, {G, P_MORPH}, {G, P_SCENE_T}, {NONE, 0}},
    {{G, P_TUNE}, {G, P_WIDTH}, {G, P_PRESS_SENS}, {NONE, 0}},   /* the Speaker cut is automatic (2026-10-20) */
    {{G, P_PULSE_RATE}, {G, P_PULSE_DEPTH}, {G, P_PULSE_SHAPE}, {G, P_PULSE_SWING}},
    {{G, P_DLY_TYPE}, {G, P_DLY_TIME}, {G, P_DLY_REPEATS}, {G, P_DLY_MIX}},
    {{G, P_REV_TYPE}, {G, P_REV_LEN}, {G, P_REV_DAMP}, {G, P_REV_MIX}},
};
/* the page buttons: each shows its first page, pressed again the next */
static const struct {
    uint8_t btn, first, n;
} PAGE_BTN[] = {{B_HOME, V_PLAY, 2}, {B_ENV, V_TONE, 2}, {B_EDIT, V_OSC, 2}, {B_LFO, V_LFO, 2},
                {B_SEQ, V_PARTNER, 2}, {B_GLO, V_SETUP, 2}, {B_FX, V_FX, 2},
                {B_ARP, V_PULSE, 1}};                                    /* ARP: a tap toggles */
#define NPAGE_BTN (sizeof PAGE_BTN / sizeof PAGE_BTN[0])
#define HOLD_MS 400u                   /* a page button held this long (or a knob turned) is a peek */

/* ------------------------------------------------------------ switches --- */
/* the black keys (key numbers, F3 = 0) and what each does: normally, and on the LFO page */
static const uint8_t BLACK_K[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};
/* the white keys: the first NSCENES (F3 .. C5, which sense pressure) the scenes of the bank; the top four
 * (D5 .. G5, no pressure) SEL + key: the bank, alone: the gestures (Weight, Shift, Clean, Wash) */
#define NWHITE 16
static const uint8_t WHITE_K[NWHITE] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static const char BANK_L[NBANKS] = {'A', 'B', 'C', 'P'};
static const char *const GESTURE[DR_NGTYPE] = {"WEIGHT", "SHIFT", "CLEAN", "WASH"};   /* D5-G5, fixed */
enum { S_NONE, S_PARTNER, S_SUBOCT, S_HP, S_OTAP, S_ODEST, S_FTAP, S_FDEST, S_PLINK, S_SYNC, S_XMOD, S_LFORESET, S_DTAP,
       S_PLOCK };
typedef struct {
    uint8_t s, g;
} sw_t;
/* normally, by meaning: drone I (partner, sub, filter) | the global two (pitch lock, LFO reset) | drone II |
 * the links (pitch link, hard sync) | cross-mod */
static const sw_t SW_NORMAL[11] = {{S_PARTNER, 0}, {S_SUBOCT, 0}, {S_HP, 0}, {S_PLOCK, 0}, {S_LFORESET, 0},
                                   {S_PARTNER, 1}, {S_SUBOCT, 1}, {S_HP, 1}, {S_PLINK, 1}, {S_SYNC, 1},
                                   {S_XMOD, 1}};
static const sw_t SW_LFO[11] = {{S_OTAP, 0}, {S_ODEST, 0}, {S_FDEST, 0}, {S_FTAP, 0}, {S_LFORESET, 0},
                                {S_OTAP, 1}, {S_ODEST, 1}, {S_FDEST, 1}, {S_FTAP, 1}, {S_DTAP, 1},
                                {S_XMOD, 1}};

/* the screen's colours: dark, glowing (Evan, 2026-10-09; Figma "Screen concepts", concept D), in the site's palette
 * (2026-10-18, web/emu/index.html :root): a plum black, aubergine panels, a pale pink ink; amber drone I, teal
 * drone II, lilac the LFO, everything else in the ink */
#define K_BG RGB(20, 0, 15)                  /* the site's black */
#define K_PANEL RGB(38, 10, 34)              /* the pictures' panels */
#define K_CARD RGB(58, 18, 53)               /* a lit value, a banner: the site's card */
#define K_GRID RGB(70, 30, 64)               /* bars' tracks, lines */
#define K_FAINT RGB(104, 66, 96)
#define K_DIM RGB(160, 128, 148)
#define K_TEXT RGB(251, 233, 242)            /* ink */
#define K_TEXT2 RGB(211, 179, 196)           /* values at rest: the site's muted */
#define K_LFO RGB(220, 188, 255)             /* lilac */
#define K_PALE RGB(255, 214, 240)            /* the site's pale pink: page names, accents */
#define K_FX K_PALE
#define K_PULSE K_PALE
#define K_SETUP K_PALE
#define K_GOLD K_PALE                        /* messages without a drone */
static const uint16_t K_DRONE[DR_NGROUPS] = {RGB(255, 176, 74), RGB(69, 227, 211)};   /* amber, teal */
static const uint16_t K_DRONE_DIM[DR_NGROUPS] = {RGB(96, 52, 36), RGB(36, 72, 76)};   /* their glow, on plum */

static struct {
    uint8_t view;
    uint8_t playing;                   /* PLAY on: the drone faded in, or fading in */
    uint32_t btn, keys, btn_used;
    uint32_t enc_t[NE];
    int8_t hold_btn;                   /* the page button held (B_*), or -1 */
    uint8_t hold_from, hold_cycle, hold_turned;
    uint32_t hold_t;
    uint8_t dirty;                     /* the project changed since the last save */
    uint32_t act_t;
    uint32_t chg_t;                    /* the last change to the project, by a hand or not (a scene gliding, the loop) */
    uint32_t saved_t;                  /* the last autosave tried */
    int8_t save_q;                     /* a save waiting for the dip (playing): -2 none, -1 the sound, N scene N */
    uint32_t save_t;                   /* ... asked then */
    uint32_t arp_t;                    /* the last ARP tap (two quick taps: stay on the Swell page) */
    uint8_t arp_pend;                  /* ... its on / off still to come (if no second tap) */
    uint8_t page_last[8];              /* each page button's page last shown (0: its first): opened again there */
    uint32_t sel_t;                    /* when SEL went down (held 0.4 s: the black keys' map) */
    int8_t km_key;                     /* the black key touched while SEL is held (named, not switched), or -1 */
    uint8_t jack;                      /* the headphone jack as last read (1: in), and since when */
    uint32_t jack_t;
    uint32_t rst_t;                    /* when the LFO reset key went down (a hold locks the LFO to the pulse) */
    uint8_t rst_done;                  /* that hold has acted */
    char msg[2][24];
    uint32_t msg_until;
    uint16_t msg_col;                  /* the banner's colour */
    char glab[2][16];                  /* SELECT / ALGORITHM / PRESETS turned: their name and value (top bar) */
    uint32_t glab_until;
    uint32_t touch_t[4];               /* each knob: lit until then (each its own timer: two hands) */
    int8_t morph;                      /* the scene being morphed to (VISIT_BACK: back from a visit), or -1 */
    int8_t visit;                      /* the white key held (a visit, if it's held long), or -1 */
    uint32_t visit_t;                  /* ... since */
    int16_t visit_back[DR_NPARAMS];    /* ... and the sound before it: where letting go morphs back to */
    int8_t scene_at;                   /* the scene the sound is (stored or recalled, no hand on it since), or -1:
                                        * its white key blinks (Evan, on the FM-1, 2026-10-14) */
    int8_t visit_from;                 /* ... before a visit: where letting go comes back to */
    uint8_t loop;                      /* the motion loop: L_IDLE, L_REC, L_PLAY */
    uint32_t loop_t0, loop_len;        /* recording: its start; playing: this pass's start, the length */
    uint16_t loop_n, loop_i;           /* events recorded, the next to play */
    int16_t loop_from[DR_NPARAMS];     /* every value when the recording began */
    uint32_t morph_t0, morph_ms;
    int16_t morph_from[DR_NPARAMS];
    int16_t morph_to[DR_NPARAMS];      /* the scene being morphed to, as it was when its key was pressed */
    int8_t press;                      /* the scene key held in a pressure crossfade (v0.2), or -1 */
    uint8_t press_done;                /* ... pressed through: the scene is taken */
    uint8_t press_held;                /* ... SEL pressed during it: held where it is until the key lets go */
    uint32_t press_t0, press_hi_t;     /* ... since; above the commit level since (0: not) */
    uint32_t press_ft;                 /* ... the last frame's time (its glide toward the pressure) */
    float press_x;                     /* ... how far into the scene, 0..1 */
    uint32_t flash_t;                  /* a scene taken: its key flashes from then */
    uint8_t g_held;                    /* the gesture keys held: bit g (Weight, Shift, Clean, Wash) */
    uint8_t g_rel;                     /* ... pressed to release a latch: the let-go does nothing more */
    uint32_t g_t0[DR_NGEST];           /* ... since */
    float g_out[DR_NGEST];             /* ... let go after a hold: how far in it was, and over how long (s, at Gesture
                                        * time x1) it goes: as long as it was held (Evan, 2026-10-18); 0: the swell */
    float g_out_s[DR_NGEST];
    float g_amt[DR_NGEST];             /* ... how far in each is, 0..1 */
    int16_t g_sent[DR_NGEST];          /* ... as the engine has it, 0..1000 */
    uint32_t g_last;                   /* the last swell step's time */
    uint64_t hands[(DR_NPARAMS + 63) / 64];   /* the values a hand has moved since a scene switch began: the
                                               * switch lets go of them (Evan, 2026-10-17: Movement fought it) */
    int16_t press_to[DR_NPARAMS];      /* ... the scene */
    uint32_t frame;
    uint32_t sig[3];                   /* what each band shows: redrawn when it changes */
    uint32_t splash_t0;                /* the start's animation: since */
    uint8_t splash_done;
} ui;

/* a message: a banner across the top of the picture for a moment (the knobs keep the bottom) */
static void say_c(const char *a, const char *b, uint16_t col);
static void say(const char *a, const char *b) { say_c(a, b, 0); }
static void say_c(const char *a, const char *b, uint16_t col)
{
    ui.msg_col = col;
    uint32_t i;
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
}
void ui_say(const char *a, const char *b) { say(a, b); }

static void mark_dirty(void) { ui.dirty = 1; ui.chg_t = plat_ms(); }
int ui_dirty(void) { return ui.dirty != 0; }

static void itoa_u(uint32_t v, char *b)
{
    char t[12];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n)
        *b++ = t[--n];
    *b = 0;
}

static char *scopy(char *d, const char *s)          /* copies, returns the end */
{
    while ((*d = *s++))
        d++;
    return d;
}

/* ------------------------------------------------------------ values --- */
static void param_set(int p, int v)
{
    const dr_param_t *i = dr_param_info(p);
    v = v < i->lo ? i->lo : v > i->hi ? i->hi : v;
    if (v == proj.par[p])
        return;
    proj.par[p] = (int16_t)v;
    dr_set(p, v);
    mark_dirty();
}

static void loop_note(int p, int v, uint32_t slide);  /* the motion loop notes knob moves (below) */

/* the parameter knob i moves on the page shown, or -1 (on the Play page, KNOB 3 is the link offset while
 * PITCH LINK is on) */
static int knob_param(int i)
{
    knob_t k = VIEW_KNOB[ui.view][i];
    if (k.g == NONE)
        return -1;
    if (k.g == G)
        return k.p;
    if (k.g == 1 && k.p == GP_PITCH && proj.par[P_PLINK])
        return P_PLINK_OFF;
    return DR_GP(k.g, k.p);
}

/* knob acceleration by speed (FoMni's): a slow turn is one step a detent, a quick one up to 6 */
static uint8_t accel_off;              /* the host simulator's "spin": exact steps */
static int32_t accel(int role, int32_t s, int range)
{
    uint32_t now = plat_ms(), dt = now - ui.enc_t[role], a = (uint32_t)(s < 0 ? -s : s), m;
    ui.enc_t[role] = now;
    if (range <= 24 || !a || accel_off)
        return s;
    if (a > 1)
        dt /= a;
    m = dt < 15u ? 6u : dt < 30u ? 4u : dt < 60u ? 2u : 1u;
    return s * (int32_t)m;
}

/* a pitch knob (or the link offset). A detent is 60 cents (a slow full turn of 20 detents: an octave),
 * more when turned quickly, on a smooth curve (up to 2x at 70 detents a second: Evan, on the FM-1, 4x went
 * too far); with SEL held, 1 cent.
 * Each change slides over the time since the last detent (15..600 ms): the next one is due about then,
 * so a turn at any speed, even a slow uneven drag, is one continuous slide, as an analog knob's; a
 * first detent after a pause slides over 300 ms. (At 250 ms, detents a third of a second apart left
 * 60-cent stairs.) */
#define PITCH_CENTS 60
static void pitch_turn(int g, int p, int role, int32_t s)
{
    uint32_t now = plat_ms(), dt = now - ui.enc_t[role], a = (uint32_t)(s < 0 ? -s : s), slide;
    int cents;
    if (proj.par[P_PLOCK]) {                           /* locked against a bumped knob */
        say_c("PITCH LOCKED", "KEY 4", K_GOLD);
        return;
    }
    ui.enc_t[role] = now;
    if (a > 1)
        dt /= a;                                       /* several detents since the last look */
    /* the glide: the time since the last detent (when the next is due, so a steady turn never stops),
     * 15 .. 600 ms; after a pause (a first detent), 300 ms */
    slide = accel_off ? 15u : dt < 15u ? 15u : dt > 600u ? 300u : dt;
    if (ui.btn >> B_SEL & 1u) {                        /* fine: SEL held */
        ui.btn_used |= 1u << B_SEL;
        cents = (int)s;
    } else {
        float v = 1000.0f / (float)(dt ? dt : 1u), x = fm_clampf((v - 10.0f) * (1.0f / 60.0f), 0.0f, 1.0f);
        cents = (int)s * (int)((float)PITCH_CENTS * (accel_off ? 1.0f : 1.0f + x * x) + 0.5f);
    }
    dr_slide(g, (int)slide);
    param_set(p, proj.par[p] + cents);
    loop_note(p, proj.par[p], slide);
}

/* a continuous knob (cutoff, resonance, X-Wave, a depth, a level...: a range over 24 steps), as a pitch
 * knob: a detent is a step, more when turned quickly (up to 4x, on the same smooth curve), and each glides
 * over the time since the last detent (15..600 ms; 300 after a pause), so a slow turn is a sweep */
static void cont_turn(int p, int role, int32_t s)
{
    uint32_t now = plat_ms(), dt = now - ui.enc_t[role], a = (uint32_t)(s < 0 ? -s : s), slide;
    int steps;
    ui.enc_t[role] = now;
    if (a > 1)
        dt /= a;
    slide = accel_off ? 15u : dt < 15u ? 15u : dt > 600u ? 300u : dt;
    {
        float v = 1000.0f / (float)(dt ? dt : 1u), x = fm_clampf((v - 10.0f) * (1.0f / 60.0f), 0.0f, 1.0f);
        steps = (int)s * (int)(accel_off ? 1.0f : 1.0f + 3.0f * x * x + 0.5f);
    }
    {   /* cutoff and resonance run 0..1000, Shift's move in cents: a detent is 10, with SEL held 1 (fine, as a
         * pitch's cent) */
        int gp = p < P_XMOD ? p % GP_N : -1;
        if (gp == GP_CUTOFF || gp == GP_RESO || p == P_SHIFT_MOVE) {
            if (ui.btn >> B_SEL & 1u) {
                ui.btn_used |= 1u << B_SEL;
                steps = (int)s;
            } else {
                steps *= 10;
            }
        }
    }
    dr_pslide(p, (int)slide);
    param_set(p, proj.par[p] + steps);
    loop_note(p, proj.par[p], slide);
}

/* an LFO depth turned that can't be heard: the Movement at 0 %, or the wave(s) it follows off. A hint then,
 * only then (Evan, 2026-10-17: nothing on the screen said why the depths did nothing) */
static void depth_hint(int p)
{
    int tap = -1, w1 = proj.par[P_LFO_W1] != 0, w2 = proj.par[P_LFO_W2] != 0, g;
    for (g = 0; g < DR_NGROUPS; g++) {
        if (p == DR_GP(g, GP_OLFO_DEPTH))
            tap = proj.par[DR_GP(g, GP_OLFO)];
        else if (p == DR_GP(g, GP_FLFO_DEPTH))
            tap = proj.par[DR_GP(g, GP_FLFO)];
    }
    if (p == P_DLY_LFO_DEPTH)
        tap = proj.par[P_DLY_LFO];
    if (tap < 0 || !proj.par[p])
        return;
    if (proj.par[P_MOVE] == 0)
        say_c("MOVEMENT 0%", "ALGORITHM", K_LFO);
    else if ((tap == DR_LFO_I && !w1) || (tap == DR_LFO_II && !w2) || (tap == DR_LFO_BOTH && !w1 && !w2))
        say_c(tap == DR_LFO_II ? "WAVE II OFF" : "WAVE I OFF", tap == DR_LFO_II ? "tap OCT+" : "tap OCT-", K_LFO);
}

static void knob_turn(int i, int32_t e)
{
    int p = knob_param(i);
    knob_t k = VIEW_KNOB[ui.view][i];
    if (p < 0)
        return;
    if (proj.par[P_II_OUT] && k.g == 1)        /* drone II out: say how it comes back, on any page (Evan, 2026-10-18) */
        say_c("II OFF", "SEL + PLAY", K_DRONE[1]);
    if (k.p == GP_PITCH && k.g < G)
        pitch_turn(k.g, p, EN_K1 + i, e);
    else if (dr_param_info(p)->hi - dr_param_info(p)->lo > 24)
        cont_turn(p, EN_K1 + i, e);
    else {
        param_set(p, proj.par[p] + accel(EN_K1 + i, e, dr_param_info(p)->hi - dr_param_info(p)->lo));
        loop_note(p, proj.par[p], 0);
    }
    if (ui.view == V_LFO || ui.view == V_LFO2)
        depth_hint(p);
}

/* ------------------------------------------------------------ switches --- */
static const sw_t *sw_layer(void) { return ui.view == V_LFO || ui.view == V_LFO2 ? SW_LFO : SW_NORMAL; }

/* a route's wave as the key shows it: I 0 (dark), II 1 (lit), I+II 2 (lit, winking) */
static int tap_lit(int v) { return v == DR_LFO_BOTH ? 2 : v == DR_LFO_II; }

static int sw_get(sw_t w)                              /* 1: lit (on, or the second choice); 2: a third */
{
    const int16_t *p = &proj.par[DR_GP(w.g, 0)];
    switch (w.s) {
    case S_PARTNER: return p[GP_PARTNER] != 0;
    case S_SUBOCT: return p[GP_SUBOCT] >= 3 ? 2 : p[GP_SUBOCT] == 2;   /* -2 oct lit; a subharmonic winks */
    case S_PLOCK: return proj.par[P_PLOCK] != 0;
    case S_HP: return p[GP_MODE] == DR_HP ? 1 : p[GP_MODE] == DR_BP ? 2 : 0;   /* HP lit, BP winking */
    case S_OTAP: return tap_lit(p[GP_OLFO]);
    case S_ODEST: return p[GP_OLFO_DEST] == DR_TO_XWAVE;
    case S_FDEST: return p[GP_FLFO_DEST] == DR_TO_RESO;
    case S_FTAP: return tap_lit(p[GP_FLFO]);
    case S_PLINK: return proj.par[P_PLINK] != 0;
    case S_SYNC: return proj.par[P_SYNC] != 0;
    case S_XMOD: return proj.par[P_XMOD] != 0;
    case S_DTAP: return tap_lit(proj.par[P_DLY_LFO]);
    default: return 0;
    }
}

/* PITCH LINK on: drone II keeps its pitch, as drone I's + the offset; off: it keeps it as its own */
static void link_set(int on)
{
    int p1 = proj.par[DR_GP(0, GP_PITCH)], p2 = proj.par[DR_GP(1, GP_PITCH)];
    if (on) {
        param_set(P_PLINK_OFF, p2 - p1);
    } else {
        int now = p1 + proj.par[P_PLINK_OFF];
        param_set(DR_GP(1, GP_PITCH), now);
    }
    param_set(P_PLINK, on);
}

static void sw_toggle(sw_t w)
{
    static const char *const DR[2] = {"I ", "II "};
    static const char *const WAVES[4] = {"OFF", "WAVE I", "WAVE II", "WAVES I+II"};
    char a[24];
    const char *what = 0, *st = 0;
    int on = !sw_get(w), base = DR_GP(w.g, 0), tp = -1, nx;
    /* a route's key goes round: Wave I, Wave II, both (I+II), Wave I */
    if (w.s == S_OTAP || w.s == S_FTAP || w.s == S_DTAP)
        tp = w.s == S_OTAP ? base + GP_OLFO : w.s == S_FTAP ? base + GP_FLFO : P_DLY_LFO;
    nx = tp < 0 ? 0 : proj.par[tp] == DR_LFO_I ? DR_LFO_II : proj.par[tp] == DR_LFO_II ? DR_LFO_BOTH : DR_LFO_I;
    char *q = scopy(a, DR[w.g]);
    switch (w.s) {
    case S_PARTNER: param_set(base + GP_PARTNER, on); what = "PARTNER"; st = on ? "ON" : "OFF"; break;
    case S_SUBOCT: {                           /* round: -1 oct, -2 oct, 1/3, 1/5 */
        static const char *const SUB[5] = {"", "-1 OCT", "-2 OCT", "1/3", "1/5"};
        int v = proj.par[base + GP_SUBOCT] % 4 + 1;
        param_set(base + GP_SUBOCT, v);
        what = "SUB";
        st = SUB[v];
        break;
    }
    case S_PLOCK: param_set(P_PLOCK, on); q = a; what = "PITCH LOCK"; st = on ? "ON" : "OFF"; break;
    case S_HP: {                               /* round: LP, BP, HP */
        static const char *const MODE[3] = {"LP", "BP", "HP"};
        int m = (proj.par[base + GP_MODE] + 1) % 3;
        param_set(base + GP_MODE, m);
        what = "FILTER";
        st = MODE[m];
        break;
    }
    case S_OTAP: param_set(tp, nx); what = "OSC LFO"; st = WAVES[nx]; break;
    case S_ODEST: param_set(base + GP_OLFO_DEST, on); what = "OSC LFO TO"; st = on ? "X-WAVE" : "PITCH"; break;
    case S_FDEST: param_set(base + GP_FLFO_DEST, on); what = "FILTER LFO TO"; st = on ? "RESONANCE" : "CUTOFF"; break;
    case S_FTAP: param_set(tp, nx); what = "FILTER LFO"; st = WAVES[nx]; break;
    case S_PLINK: link_set(on); q = a; what = "PITCH LINK"; st = on ? "ON" : "OFF"; break;
    case S_SYNC: param_set(P_SYNC, on); q = a; what = "HARD SYNC"; st = on ? "ON" : "OFF"; break;
    case S_XMOD: param_set(P_XMOD, on); q = a; what = "I > II CUTOFF"; st = on ? "ON" : "OFF"; break;
    case S_LFORESET: dr_lfo_reset(); q = a; what = "LFO"; st = "RESET"; break;
    case S_DTAP: param_set(tp, nx); q = a; what = "DELAY LFO"; st = WAVES[nx]; break;
    default: return;
    }
    scopy(q, what);
    say_c(a, st, w.s == S_LFORESET || w.s == S_DTAP ? RGB(183, 145, 255) : w.g ? RGB(69, 227, 211) : RGB(255, 176, 74));
}

/* ------------------------------------------------------------ buttons --- */
static int on_pages(int i) { return ui.view >= PAGE_BTN[i].first && ui.view < PAGE_BTN[i].first + PAGE_BTN[i].n; }
static void set_view(int v)
{
    uint32_t i;
    ui.view = (uint8_t)v;
    for (i = 0; i < 4; i++)
        ui.touch_t[i] = 0;
    for (i = 0; i < NPAGE_BTN; i++)                    /* remembered: the button opens it next time */
        if (on_pages((int)i))
            ui.page_last[i] = (uint8_t)(v - PAGE_BTN[i].first);
}

static int page_btn(int b)                             /* the index in PAGE_BTN, or -1 */
{
    uint32_t i;
    for (i = 0; i < NPAGE_BTN; i++)
        if (PAGE_BTN[i].btn == b)
            return (int)i;
    return -1;
}

/* a page button pressed: the page of its last shown at once (the review: Reverb, Tone 2, Moves couldn't be peeked;
 * HOME always Play 1, home), unless already on its pages (then the next on a tap) */
static void page_down(int b)
{
    int i = page_btn(b);
    ui.hold_btn = (int8_t)b;
    ui.hold_t = plat_ms();
    ui.hold_turned = 0;
    ui.hold_from = ui.view;
    ui.hold_cycle = (uint8_t)on_pages(i);
    if (!ui.hold_cycle)
        set_view(PAGE_BTN[i].first + (b == B_HOME ? 0 : ui.page_last[i]));
}
#define ARP_TAPS_MS 350u               /* two ARP taps this close: the Swell's page */
static void arp_run(void)                              /* each frame: a lone ARP tap, once it's alone */
{
    if (ui.arp_pend && plat_ms() - ui.arp_t >= ARP_TAPS_MS) {
        ui.arp_pend = 0;
        param_set(P_PULSE, !proj.par[P_PULSE]);
        say("SWELL", proj.par[P_PULSE] ? "ON" : "OFF");
    }
}
/* let go: after a hold (or a knob turned while held), back where it was; after a tap, stay (or step on) */
static void page_up(int b)
{
    int i = page_btn(b), peek;
    if (ui.hold_btn != b)
        return;
    peek = ui.hold_turned || plat_ms() - ui.hold_t >= HOLD_MS;
    if (b == B_ARP) {                                  /* ARP: a tap is the pulse on / off; its page only */
        if (!peek) {                                   /* a tap: the swell on / off, once no second tap */
            uint32_t now = plat_ms();                  /* follows; two quick taps: its page, to stay, the */
            if (ui.arp_pend && now - ui.arp_t < ARP_TAPS_MS) {   /* Swell untouched (the review: it dipped) */
                ui.arp_pend = 0;
                set_view(V_PULSE);
                ui.hold_btn = -1;
                say("SWELL", "PAGE");
                return;
            }
            ui.arp_t = now;
            ui.arp_pend = 1;                           /* arp_run switches it */
        }
        if (!ui.hold_cycle)
            set_view(ui.hold_from);
        ui.hold_btn = -1;
        return;
    }
    if (peek) {
        if (!ui.hold_cycle)
            set_view(ui.hold_from);
    } else if (ui.hold_cycle) {
        set_view(PAGE_BTN[i].first + (ui.view - PAGE_BTN[i].first + 1) % PAGE_BTN[i].n);
    }
    ui.hold_btn = -1;
}

static float morph_ms(void) { return 20.0f * fm_exp2f((float)proj.par[P_MORPH] * 0.105507f); }

/* OCT- / OCT+: the LFO's two waves (v0.2): a tap is the next shape, round and round (Off, the seven, random);
 * SEL + OCT the one before. The engine glides a new shape in over the Switch time. Shown in the top bar */
static void hands_on(int p) { ui.hands[p / 64] |= (uint64_t)1 << (p % 64); }
static int hands_has(int p) { return (int)(ui.hands[p / 64] >> (p % 64) & 1u); }
static void hands_clear(void) { memset(ui.hands, 0, sizeof ui.hands); }

static void wave_step(int w, int dir)
{
    char t[12];
    const int p = w ? P_LFO_W2 : P_LFO_W1, n = DR_LFO_NSHAPES;
    int v = (proj.par[p] + dir + n) % n;
    hands_on(p);                               /* a scene on its way leaves it alone */
    param_set(p, v);
    ui.scene_at = -1;
    loop_note(p, v, 0);
    if (ui.view == V_LFO)                      /* LFO depths draws both waves, named: no need to say it (Evan) */
        return;
    dr_param_text(p, v, t);
    scopy(ui.glab[0], w ? "WAVE II" : "WAVE I");
    scopy(ui.glab[1], t);
    ui.glab_until = plat_ms() + 1500u;
}

/* -------------------------------------------------------- motion loop --- */
enum { L_IDLE, L_REC, L_PLAY };
#define LOOP_MAX 2048 /* fm1-dock (take.py): 4096 */                  /* events */
#define LOOP_MS 300000u                /* the longest loop: 5 minutes */
typedef struct {
    uint16_t t;                        /* ms into the loop / 5 (5 minutes fit) */
    uint8_t p, slide;                  /* the parameter; its glide, ms / 2 */
    int16_t v;
} loop_ev_t;
static loop_ev_t loop_ev[LOOP_MAX] OM_POOL;
static uint64_t loop_used[(DR_NPARAMS + 63) / 64];     /* the parameters the loop moves */

/* a knob moved a value: noted while recording. Detents of the same knob within LOOP_JOIN of an event (a quick
 * turn: a detent every ~15 ms) join it, its slide reaching on to this one, so the 4096 events last (a busy
 * two-handed recording filled them in 1-2 minutes; the loop then held still) */
#define LOOP_JOIN 20u                  /* 100 ms, in the events' 5 ms units */
static void loop_note(int p, int v, uint32_t slide)
{
    loop_ev_t *e;
    uint16_t t = (uint16_t)((plat_ms() - ui.loop_t0) / 5u);
    int i;
    if (ui.loop != L_REC || ui.loop_n >= LOOP_MAX)
        return;
    for (i = ui.loop_n - 1; i >= 0 && i >= ui.loop_n - 8; i--) {
        e = &loop_ev[i];
        if (t - e->t >= LOOP_JOIN)
            break;
        if (e->p == p) {
            uint32_t sl = (uint32_t)(t - e->t) * 5u + slide;
            e->v = (int16_t)v;
            e->slide = (uint8_t)(sl > 510u ? 255u : sl / 2u);
            return;
        }
    }
    e = &loop_ev[ui.loop_n++];
    e->t = t;
    e->p = (uint8_t)p;
    e->v = (int16_t)v;
    e->slide = (uint8_t)(slide > 510u ? 255u : slide / 2u);
    loop_used[p / 64] |= 1ull << (p % 64);
}

static int loop_moves(int p) { return (int)(loop_used[p / 64] >> (p % 64) & 1u); }

static void loop_apply(int p, int v, uint32_t slide)
{
    if (p == DR_GP(0, GP_PITCH) || p == DR_GP(1, GP_PITCH))
        dr_slide(p / GP_N, (int)slide);
    else if (p == P_PLINK_OFF)
        dr_slide(1, (int)slide);
    else if (slide)
        dr_pslide(p, (int)slide);
    param_set(p, v);
}

static void loop_rewind(void)                  /* the start of a pass: what the loop moves, back to how it */
{                                              /* began, gliding over the Switch time (no seam) */
    int p;
    uint32_t back = (uint32_t)morph_ms();
    for (p = 0; p < DR_NPARAMS; p++)
        if (loop_moves(p))
            loop_apply(p, ui.loop_from[p], back < 15u ? 15u : back);
    ui.loop_i = 0;
}

static void loop_button(void)                  /* REC: record, loop, stop */
{
    uint32_t now = plat_ms();
    int p;
    switch (ui.loop) {
    case L_IDLE:
        for (p = 0; p < DR_NPARAMS; p++)
            ui.loop_from[p] = proj.par[p];
        for (p = 0; p < (int)(sizeof loop_used / sizeof loop_used[0]); p++)
            loop_used[p] = 0;
        ui.loop_n = 0;
        ui.loop_t0 = now;
        ui.loop = L_REC;
        say("MOTION LOOP", "RECORDING");
        break;
    case L_REC:
        if (!ui.loop_n) {
            ui.loop = L_IDLE;
            say("MOTION LOOP", "NOTHING MOVED");
            break;
        }
        ui.loop_len = now - ui.loop_t0;
        ui.loop = L_PLAY;
        ui.loop_t0 = now;
        loop_rewind();
        say("MOTION LOOP", "PLAYING");
        break;
    default:
        ui.loop = L_IDLE;                      /* stopped: everything stays where it is */
        say("MOTION LOOP", "STOPPED");
        break;
    }
}

static void loop_run(void)                     /* every millisecond: recording's limit, playing's events */
{
    uint32_t now = plat_ms();
    if (ui.loop == L_REC && (now - ui.loop_t0 >= LOOP_MS || ui.loop_n >= LOOP_MAX)) {
        int full = ui.loop_n >= LOOP_MAX;
        loop_button();                         /* 5 minutes, or its memory full: it starts looping by itself */
        if (full)
            say("MOTION LOOP", "FULL: PLAYING");
    }
    if (ui.loop != L_PLAY)
        return;
    if (now - ui.loop_t0 >= ui.loop_len) {     /* the end of a pass: around again */
        ui.loop_t0 += ui.loop_len;
        loop_rewind();
    }
    while (ui.loop_i < ui.loop_n && (uint32_t)loop_ev[ui.loop_i].t * 5u <= now - ui.loop_t0) {
        const loop_ev_t *e = &loop_ev[ui.loop_i++];
        loop_apply(e->p, e->v, (uint32_t)e->slide * 2u);
    }
}

/* ------------------------------------------------------------- scenes --- */
static int scene_step(int p)                           /* a value that steps (a switch, a list): it flips */
{
    const dr_param_t *in = dr_param_info(p);
    return in->hi - in->lo <= 9;
}

/* settings of the room, not the sound: a scene doesn't carry them (the speaker and the stereo spread, which
 * depend on speaker or headphones; the tuning to the band; the pitch lock; how long things take) */
static int scene_room(int p)
{
    return p == P_FADE || p == P_TUNE || p == P_SPEAKER || p == P_WIDTH || p == P_PLOCK || p == P_MORPH ||
           p == P_SCENE_T || p == P_PRESS_SENS;
}

/* a scene's name: its bank's letter and its number ("B5"; the Presets "P2") */
static void scene_name(char *t, int bank, int n)
{
    t[0] = BANK_L[bank];
    itoa_u((uint32_t)n + 1u, t + 1);
}

/* into the flash now: n = -1 the sound (SAVE), else scene n (just stored) */
static void save_now(int n)
{
    char t[12] = "SCENE ";
    int ok = project_save() == 0;
    if (ok)
        ui.dirty = 0;
    if (n < 0) {
        say(ok ? "SAVED" : "NOT SAVED", ok ? 0 : "FLASH ERROR");
    } else {
        scene_name(t + 6, proj.bank, n);
        say(t, ok ? "STORED" : "NOT STORED");
    }
}

/* a save: while the drone sounds, the output dips out first (the write stops the audio for a few ms, measured
 * on the FM-1: under one 5.8 ms block), then comes back; Evan, 2026-10-14: no more waiting for PLAY to stop */
static void save_ask(int n)
{
    if (!ui.playing && dr_fade <= 0.0f) {
        save_now(n);
        return;
    }
    if (ui.save_q != -2)                               /* one waiting already: it covers this one too */
        n = ui.save_q >= 0 ? ui.save_q : n;
    ui.save_q = (int8_t)n;
    ui.save_t = plat_ms();
    dr_duck(1);
}

static void save_run(void)                             /* each look at the input: the dip out, the write */
{
    if (ui.save_q == -2 || (dr_duck_level > 0.0f && plat_ms() - ui.save_t < 100u))
        return;
    save_now(ui.save_q);
    ui.save_q = -2;
    dr_duck(0);
}

static void scene_store(int n)                         /* SAVE held + a white key: and into the flash */
{
    if (proj.bank == BANK_PRESETS) {                   /* the Presets stay as they are (Evan, 2026-10-19) */
        say_c("PRESETS ARE FIXED", "SEL+D5-G5: BANK", K_GOLD);
        return;
    }
    scene_put(proj.bank, n);
    ui.scene_at = (int8_t)n;
    ui.dirty = 1;
    save_ask(n);
}

static float fade_s(void) { return 0.05f * fm_exp2f((float)proj.par[P_FADE] * 0.102288f); }
static float scene_s(void) { return 0.1f * fm_exp2f((float)proj.par[P_SCENE_T] * 0.115507f); }

static void scene_recall(int n)                        /* a white key: morph there over the Scene time */
{
    char t[12] = "SCENE ";
    const int16_t *sc = scene_get(proj.bank, n);
    int k;
    scene_name(t + 6, proj.bank, n);
    if (!sc) {
        say(t, "EMPTY: SAVE+KEY");
        return;
    }
    for (k = 0; k < DR_NPARAMS; k++) {
        ui.morph_from[k] = proj.par[k];
        ui.morph_to[k] = sc[k];
    }
    hands_clear();                             /* a new switch: it moves everything again */
    ui.morph = (int8_t)n;
    ui.scene_at = (int8_t)n;
    ui.morph_t0 = plat_ms();
    ui.morph_ms = (uint32_t)(scene_s() * 1000.0f);
    say(t, proj.bank == BANK_PRESETS ? preset_name(n) : 0);   /* "SCENE P3" / "EERIE" */
}

/* a visit let go: morph back to the sound before it, over the Scene time */
#define VISIT_BACK NSCENES
static void scene_back(void)
{
    int k;
    for (k = 0; k < DR_NPARAMS; k++)
        ui.morph_from[k] = proj.par[k];
    ui.morph = VISIT_BACK;
    ui.scene_at = ui.visit_from;
    ui.morph_t0 = plat_ms();
    ui.morph_ms = (uint32_t)(scene_s() * 1000.0f);
    say("BACK", 0);
}

/* the sound moved x of the way from one set of values to another: smooth values glide, steps flip half way;
 * the room's settings stay, and with Pitch lock on the pitches */

static void sound_blend(const int16_t *from, const int16_t *to, float x)
{
    int k;
    for (k = 0; k < DR_NPARAMS; k++) {
        int v;
        if (scene_room(k) || hands_has(k))             /* the room's settings aren't a scene's; nor what a
                                                        * hand has moved since the switch began */
            continue;
        if (proj.par[P_PLOCK] && (k == DR_GP(0, GP_PITCH) || k == DR_GP(1, GP_PITCH) || k == P_PLINK_OFF ||
                                  k == P_WANDER || k == P_GL_0 + DR_G_SHIFT))
            continue;                                  /* pitch lock: the drones keep their pitch (2026-10-17) */
        if (scene_step(k))
            v = x >= 0.5f ? to[k] : from[k];
        else
            v = from[k] + (int)((float)(to[k] - from[k]) * x + (to[k] > from[k] ? 0.5f : -0.5f));
        if (v == proj.par[k])
            continue;
        if (k == DR_GP(0, GP_PITCH) || k == DR_GP(1, GP_PITCH))
            dr_slide(k / GP_N, 20);                    /* a frame's step of pitch, slid */
        else if (k == P_PLINK_OFF)
            dr_slide(1, 20);
        param_set(k, v);
    }
}

/* the morph, each frame: over its time, from where it started */
static void scene_morph(void)
{
    float x;
    if (ui.morph < 0)
        return;
    x = ui.morph_ms ? (float)(plat_ms() - ui.morph_t0) / (float)ui.morph_ms : 1.0f;
    if (x > 1.0f)
        x = 1.0f;
    sound_blend(ui.morph_from, ui.morph == VISIT_BACK ? ui.visit_back : ui.morph_to, x);
    if (x >= 1.0f)
        ui.morph = -1;
}

/* THE PRESSURE CROSSFADE (v0.2, option D): a scene key held is a fader into its scene. Under PR_DEAD nothing
 * moves; PR_FULL is all the way; held at PR_COMMIT for PR_COMMIT_MS the scene is taken (the key flashes);
 * a tap (let go under PR_TAP_MS, however hard) goes there to stay. The pressure is where the sound heads, not
 * where it is: it glides there no faster than the whole way in a Scene time, so a hard press glides in as a tap
 * does and a slow one follows the finger (Evan, 2026-10-19: a quick press to 100 was a jump). Let go: back to
 * where it began at that speed; SEL pressed on the way holds the blend (until the key lets go: lifting the
 * finger, or SEL first, doesn't move it), and it stays. Pressing it again on the way back picks up from there. */
#define PR_DEAD 0.08f
#define PR_FULL 0.90f
#define PR_COMMIT 0.90f
#define PR_COMMIT_MS 300u
#define PR_TAP_MS 500u                 /* a scene key let go sooner: a tap, there to stay, as a gesture key (Evan,
                                        * 2026-10-18: 250 was too quick, a relaxed tap became a visit and went BACK) */
#define PR_WAIT_MS 250u                /* ... held this long, the pressure starts to move the sound (a crossfade) */
static void press_start(int n, const int16_t *sc)
{
    int k;
    float x0 = 0.0f;
    if (!(ui.morph == VISIT_BACK && ui.visit == -2 - n)) {  /* not already gliding back from this key: */
        for (k = 0; k < DR_NPARAMS; k++)                   /* where letting go comes back to */
            ui.visit_back[k] = proj.par[k];
        hands_clear();
    } else if (ui.morph_ms) {                              /* on the way back: from where it is now */
        float p = fm_clampf((float)(plat_ms() - ui.morph_t0) / (float)ui.morph_ms, 0.0f, 1.0f);
        x0 = ui.press_x * (1.0f - p);
    }
    ui.visit_from = ui.scene_at;
    for (k = 0; k < DR_NPARAMS; k++)
        ui.press_to[k] = sc[k];
    ui.morph = -1;
    ui.press = (int8_t)n;
    ui.press_done = 0;
    ui.press_held = 0;
    ui.press_ft = plat_ms();
    ui.press_t0 = plat_ms();
    ui.press_hi_t = 0;
    ui.press_x = x0;
}

/* a key's pressure at the player's sensitivity (Setup 2, KNOB 3): a curve, light .. firm, 50 as tuned; a full press
 * is always all the way (QA 2026-10-20: as a gain, at 0 a full press went only 38 % in) */
static float press_gain(void) { return fm_exp2f(((float)proj.par[P_PRESS_SENS] - 50.0f) * 0.02f); }
static float press_curve(float x) { return x > 0.0f ? fm_exp2f(fm_log2f(x) / press_gain()) : 0.0f; }   /* 0..1 */
static int press_scaled(int p)
{
    return p < 0 ? p : (int)fm_clampf(127.0f * press_curve((float)p * (1.0f / 127.0f)) + 0.5f, 0.0f, 127.0f);
}

static void press_run(void)                            /* each frame, while a scene key is held */
{
    char t[16];
    int p;
    float x;
    uint32_t now = plat_ms();
    if (ui.press < 0)
        return;
    if (!ui.press_done && now - ui.press_t0 >= PR_WAIT_MS && (ui.press_held || (ui.btn >> B_SEL & 1u))) {
        /* SEL down during a crossfade: the blend holds where it is, so lifting the finger (its pressure falls
         * on the way up, before the key lets go) can't take it back, SEL let go first or not; let go to keep it */
        ui.press_held = 1;
        ui.press_ft = now;
        ui.btn_used |= 1u << B_SEL;
        ui.press_hi_t = 0;
        scene_name(t, proj.bank, ui.press);
        {
            char *q = t;
            while (*q)
                q++;
            *q++ = ' ';
            *q++ = ' ';
            itoa_u((uint32_t)(ui.press_x * 100.0f + 0.5f), q);
            while (*q)
                q++;
            scopy(q, "%");
        }
        say_c(t, "HELD", K_GOLD);
        return;
    }
    p = press_scaled(plat_pressure(WHITE_K[ui.press]));
    x = fm_clampf(((float)p / 127.0f - PR_DEAD) / (PR_FULL - PR_DEAD), 0.0f, 1.0f);
    if ((float)p / 127.0f >= PR_COMMIT) {
        if (!ui.press_hi_t)
            ui.press_hi_t = now;
        if (!ui.press_done && now - ui.press_hi_t >= PR_COMMIT_MS && now - ui.press_t0 >= PR_WAIT_MS) {
            ui.press_done = 1;
            ui.flash_t = now;
        }
    } else {
        ui.press_hi_t = 0;
    }
    if (ui.press_done)
        x = 1.0f;
    if (now - ui.press_t0 < PR_WAIT_MS && !ui.press_done) {
        ui.press_ft = now;
        return;                                        /* a tap so far: nothing moves yet */
    }
    {                                                  /* toward the pressure, at most the whole way a Scene time */
        float step = (float)(now - ui.press_ft) / (scene_s() * 1000.0f);
        ui.press_ft = now;
        ui.press_x += fm_clampf(x - ui.press_x, -step, step);
    }
    sound_blend(ui.visit_back, ui.press_to, ui.press_x);
    t[0] = 0;
    scene_name(t, proj.bank, ui.press);
    if (ui.press_done)
        say_c(t, "TAKEN", K_GOLD);
    else {                                             /* "B5  40%" */
        char *q = t;
        while (*q)
            q++;
        *q++ = ' ';
        *q++ = ' ';
        itoa_u((uint32_t)(ui.press_x * 100.0f + 0.5f), q);
        while (*q)
            q++;
        scopy(q, "%");
        say_c(t, "SEL: KEEP", K_GOLD);
    }
}

static void press_end(void)                            /* the key let go */
{
    int n = ui.press, k;
    uint32_t now = plat_ms();
    char t[16] = "SCENE ";
    ui.press = -1;
    if (n < 0)
        return;
    if (!ui.press_done && !ui.press_held && now - ui.press_t0 < PR_TAP_MS && !(ui.btn >> B_SEL & 1u)) {
        /* a tap (however far a crossfade had begun): there, over the Scene time */
        scene_recall(n);
        return;
    }
    scene_name(t + 6, proj.bank, n);
    for (k = 0; k < DR_NPARAMS; k++)
        ui.morph_from[k] = proj.par[k];
    ui.morph_t0 = now;                                 /* the rest of the way, at the same speed (a Scene time
                                                        * the whole way) */
    ui.morph_ms = (uint32_t)(scene_s() * 1000.0f * (ui.press_done ? 1.0f - ui.press_x : ui.press_x));
    if (ui.press_done) {                               /* taken: the rest of the way, and it's the scene now */
        for (k = 0; k < DR_NPARAMS; k++)
            ui.morph_to[k] = ui.press_to[k];
        ui.morph = (int8_t)n;
        ui.scene_at = (int8_t)n;
        say_c(t, "TAKEN", K_GOLD);
    } else if (ui.press_held || (ui.btn >> B_SEL & 1u)) {   /* SEL: the blend stays, a sound of its own */
        ui.btn_used |= 1u << B_SEL;
        ui.scene_at = -1;
        say_c("KEPT", 0, K_GOLD);
    } else {                                           /* back to where it began */
        ui.morph = VISIT_BACK;
        ui.visit = (int8_t)(-2 - n);                   /* (pressed again on the way: picked up from there) */
        ui.scene_at = ui.visit_from;
        say("BACK", 0);
    }
}

static void button(int b)                              /* the other buttons, on release */
{
    switch (b) {
    case B_SAVE:
        save_ask(-1);
        break;
    case B_REC:
        loop_button();
        break;
    default: break;                                    /* SEL: a modifier (fine pitch) */
    }
}

/* MIDI in (USB and the TRS jack): a note on channel 1 tunes drone I to it, on channel 2 drone II */
static void midi_in(void)
{
    uint32_t pkt;
    while (plat_midi_in(&pkt)) {
        uint32_t st = (pkt >> 8) & 0xFFu, n = (pkt >> 16) & 0x7Fu, v = pkt >> 24;
        if ((st == 0x90u || st == 0x91u) && v) {
            int g = (int)(st & 1u), c = (int)n * 100;
            if (g == 1 && proj.par[P_PLINK])
                param_set(P_PLINK_OFF, c - proj.par[DR_GP(0, GP_PITCH)]);
            else
                param_set(DR_GP(g, GP_PITCH), c);
        }
    }
}

/* THE GESTURES (v0.2): Weight, Shift, Clean, Wash on the top four white keys (always these: Evan, 2026-10-18, you
 * learn where they are), which sense no pressure, so
 * time stands in: held, a gesture comes in (over its own time x Gesture time), its parts in order; let go, it goes
 * over as long as it was held (x Gesture time, turned live: Evan, 2026-10-18). A tap (under GEST_TAP_MS: a press
 * needn't be quick) latches it on, all the way;
 * tapping a latched one lets it go (over its own time); SEL held as you let go keeps it where it is. No depth or
 * time of its own to set (Evan, 2026-10-18: how you play it sets them); Shift's move is on Moves. Latched
 * levels are values (P_GL_*): scenes keep them. */
#define GEST_TAP_MS PR_TAP_MS          /* a gesture key let go sooner: a tap, as a scene key */
static void gesture_down(int g)
{
    if (proj.par[P_GL_0 + g]) {                     /* latched: let go of it, whatever the press */
        param_set(P_GL_0 + g, 0);
        hands_on(P_GL_0 + g);
        ui.g_rel |= (uint8_t)(1u << g);
        say_c(GESTURE[g], "OFF", K_LFO);
    }
    ui.g_held |= (uint8_t)(1u << g);
    ui.g_t0[g] = plat_ms();
    ui.g_out_s[g] = 0.0f;
}
static void gesture_up(int g)
{
    uint32_t held = plat_ms() - ui.g_t0[g];
    if (!(ui.g_held >> g & 1u))
        return;                                        /* not a gesture's press (SEL + the key: a bank) */
    ui.g_held &= (uint8_t)~(1u << g);
    if (ui.g_rel >> g & 1u) {
        ui.g_rel &= (uint8_t)~(1u << g);
        return;
    }
    if (held >= GEST_TAP_MS && !(ui.btn >> B_SEL & 1u)) {   /* let go after a hold: out over as long as it was held */
        ui.g_out[g] = ui.g_amt[g];
        ui.g_out_s[g] = fm_minf((float)held * 0.001f, 60.0f);
    }
    if (held < GEST_TAP_MS) {                          /* a tap: latched, at full */
        param_set(P_GL_0 + g, 100);
        hands_on(P_GL_0 + g);
        say_c(GESTURE[g], "LATCHED", K_LFO);
    } else if (ui.btn >> B_SEL & 1u) {                 /* SEL held: latched where it is */
        int v = (int)(ui.g_amt[g] * 100.0f + 0.999f);   /* rounded up: a hair less would read as letting go
                                                          * (QA 2026-10-20: Shift's chord swapped, Weight jumped) */
        ui.btn_used |= 1u << B_SEL;
        param_set(P_GL_0 + g, v < 1 ? 1 : v);
        hands_on(P_GL_0 + g);
        say_c(GESTURE[g], "KEPT", K_LFO);
    }
}
static char *dec1(float v, char *b);                   /* below */
static float mtime_k(void)                             /* GESTURE TIME: the gestures' times x 0.1 .. x 10 */
{
    return fm_exp2f(((float)proj.mtime - 50.0f) * 0.0664386f);
}
static void mtime_turn(int32_t e)                      /* PRESETS: Gesture time */
{
    int v = (int)proj.mtime + (int)e;
    float k;
    char *q;
    proj.mtime = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    mark_dirty();
    k = mtime_k();
    scopy(ui.glab[0], "GESTURE TIME");
    q = ui.glab[1];
    *q++ = 'x';
    if (k < 9.95f)
        dec1(k, q);
    else
        itoa_u((uint32_t)(k + 0.5f), q);
    ui.glab_until = plat_ms() + 1500u;
    ui.act_t = plat_ms();
}
static void gesture_run(void)                          /* each frame: each gesture toward where it's going */
{
    uint32_t now = plat_ms(), dt = now - ui.g_last;
    int g;
    ui.g_last = now;
    if (dt > 100u)
        dt = 100u;
    for (g = 0; g < DR_NGEST; g++) {
        float to = (ui.g_held >> g & 1u) && !(ui.g_rel >> g & 1u) ? 1.0f : (float)proj.par[P_GL_0 + g] * 0.01f;
        float *a = &ui.g_amt[g], k = mtime_k();
        float step = *a > to && ui.g_out_s[g] > 0.0f
                       ? (float)dt * 0.001f * ui.g_out[g] / (ui.g_out_s[g] * k)   /* let go: as long as it was held */
                       : (float)dt * 0.001f / (fm_exp2f((float)DR_GEST_TIME[g] * 0.0490689f) * k);
        int16_t q;
        *a = *a < to ? fm_minf(*a + step, to) : fm_maxf(*a - step, to);
        if (*a <= to)
            ui.g_out_s[g] = 0.0f;                      /* out (or on its way in again): its own time from here */
        q = (int16_t)(*a * 1000.0f + 0.5f);
        if (q != ui.g_sent[g]) {
            ui.g_sent[g] = q;
            dr_gesture(g, q);
        }
        if ((ui.g_held >> g & 1u) && !(ui.g_rel >> g & 1u) && now - ui.g_t0[g] >= GEST_TAP_MS) {
            char t[8], *q = t;
            itoa_u((uint32_t)(*a * 100.0f + 0.5f), t);
            while (*q)
                q++;
            scopy(q, "%");
            say_c(GESTURE[g], t, K_LFO);
        }
    }
}

static void global_turn(int role, int p, const char *name, int32_t e)
{
    char t[12];
    const dr_param_t *in = dr_param_info(p);
    ui.scene_at = -1;                          /* a hand on the sound */
    hands_on(p);                               /* a scene on its way leaves it alone (the morph goes on) */
    if (in->hi - in->lo > 24)
        cont_turn(p, role, e);                 /* the LFO rate glides as a knob on the page does */
    else {
        param_set(p, proj.par[p] + accel(role, e, in->hi - in->lo));
        loop_note(p, proj.par[p], 0);
    }
    dr_param_text(p, proj.par[p], t);
    {   /* not on the page: a label in the top bar */
        uint32_t i;
        for (i = 0; i < 15u && name[i]; i++)
            ui.glab[0][i] = name[i];
        ui.glab[0][i] = 0;
        for (i = 0; i < 15u && t[i]; i++)
            ui.glab[1][i] = t[i];
        ui.glab[1][i] = 0;
        ui.glab_until = plat_ms() + 1500u;
    }
    ui.act_t = plat_ms();
}

static void input(void)
{
    uint32_t btn = plat_buttons(), keys = plat_keys(), ch, i;
    int32_t e;
    save_run();
    ch = btn ^ ui.btn;
    ui.btn = btn;
    if ((ch & btn) >> B_SEL & 1u)
        ui.sel_t = plat_ms();
    if (!(btn >> B_SEL & 1u))
        ui.km_key = -1;
    {   /* THE HEADPHONE JACK: the Speaker low cut only while the speaker plays (Evan, 2026-10-20); a plug settled
         * for 150 ms (its contacts bounce) */
        int ph = plat_headphones();
        if (ph != ui.jack) {
            ui.jack = (uint8_t)ph;
            ui.jack_t = plat_ms();
        } else if (ph != dr_phones && plat_ms() - ui.jack_t >= 150u) {
            dr_phones = (uint8_t)ph;                   /* (no message: you hear which it is, Evan) */
        }
    }
    for (i = 0; i < NB; i++) {
        uint32_t m = 1u << i;
        if (!(ch & m))
            continue;
        if (page_btn((int)i) >= 0) {                   /* page buttons: on the press and the release */
            if (btn & m)
                page_down((int)i);
            else
                page_up((int)i);
        } else if (btn & m) {
            ui.btn_used &= ~m;
            if (i == B_OCTDN || i == B_OCTUP) {        /* the LFO waves, on the press; SEL + OCT: back one */
                int back = (int)(btn >> B_SEL & 1u);
                ui.btn_used |= m;
                if (back)
                    ui.btn_used |= 1u << B_SEL;
                wave_step(i == B_OCTUP, back ? -1 : 1);
            }
            if (i == B_PLAY && (btn >> B_SEL & 1u)) {  /* SEL + PLAY: drone II alone, out / in */
                ui.btn_used |= 1u << B_SEL;
                param_set(P_II_OUT, !proj.par[P_II_OUT]);   /* a sound's: scenes keep it */
                say_c("DRONE II", proj.par[P_II_OUT] ? "OUT" : "IN", K_DRONE[1]);
            } else if (i == B_PLAY) {                  /* PLAY on the press */
                static uint8_t seeded;
                if (!seeded) {                         /* the random ones from when the hand first pressed
                                                        * PLAY: a new wander each time it's switched on */
                    seeded = 1;
                    dr_seed(plat_ms());
                }
                ui.playing = (uint8_t)!ui.playing;
                dr_play(ui.playing);
            }
        } else if (!(ui.btn_used & m) && i != B_PLAY) {
            button((int)i);
        }
    }
    ch = keys ^ ui.keys;                               /* the black keys: switches, on the press */
    ui.keys = keys;
    for (i = 0; i < 11u; i++) {
        sw_t w = sw_layer()[i];
        if (ui.btn >> B_SEL & 1u) {                   /* SEL held: a key touched names itself on the map, and
                                                        * stays as it is (Evan, 2026-10-19) */
            if ((ch & keys) >> BLACK_K[i] & 1u) {
                ui.km_key = (int8_t)i;
                ui.btn_used |= 1u << B_SEL;
                ui.rst_done = 1;                       /* LFO reset: no lock from this hold either */
            }
            continue;
        }
        if ((ch & keys) >> BLACK_K[i] & 1u) {
            sw_toggle(w);                              /* LFO reset: at once, on the press */
            if (w.s != S_LFORESET)
                ui.scene_at = -1;
            if (w.s == S_LFORESET) {
                ui.rst_t = plat_ms();
                ui.rst_done = 0;
            }
        } else if (w.s == S_LFORESET && (keys >> BLACK_K[i] & 1u) && !ui.rst_done &&
                   plat_ms() - ui.rst_t >= HOLD_MS + 100u) {
            int on = !proj.par[P_LFO_LOCK];            /* held: the LFO locked to the pulse's bars, or free */
            param_set(P_LFO_LOCK, on);
            ui.scene_at = -1;
            if (on && !proj.par[P_PULSE])              /* (shorter: "LOCKED (SWELL OFF)" ran off the screen) */
                say_c("LFO LOCKED", "SWELL OFF", K_LFO);
            else
                say_c("LFO TO SWELL", on ? "LOCKED" : "FREE", K_LFO);
            ui.rst_done = 1;
        }
    }
    {   /* SAVE held a moment, no key yet: say what it does (the review: holding it gave no hint) */
        static uint32_t save_dn;
        if (btn >> B_SAVE & 1u) {
            if (!save_dn)
                save_dn = plat_ms() | 1u;
            if (plat_ms() - save_dn >= HOLD_MS && !(ui.btn_used >> B_SAVE & 1u))
                say_c(proj.bank == BANK_PRESETS ? "PRESETS ARE FIXED" : "KEY: STORE", "LET GO: SAVE", K_GOLD);
        } else {
            save_dn = 0;
        }
    }
    for (i = 0; i < NSCENES; i++) {                    /* the white keys: scenes (SAVE held: store) */
        if ((ch & keys) >> WHITE_K[i] & 1u) {
            if (btn >> B_SAVE & 1u) {
                ui.btn_used |= 1u << B_SAVE;           /* SAVE let go: not a plain save */
                scene_store((int)i);
            } else if (plat_pressure(WHITE_K[i]) >= 0 && scene_get(proj.bank, (int)i)) {
                press_start((int)i, scene_get(proj.bank, (int)i));   /* the pressure crossfade (v0.2) */
            } else {                                   /* no pressure here (the emulator): 0.1's timed visit */
                int k;
                for (k = 0; k < DR_NPARAMS; k++)       /* where a visit comes back to */
                    ui.visit_back[k] = proj.par[k];
                ui.visit_from = ui.scene_at;
                scene_recall((int)i);
                ui.visit = ui.morph == (int8_t)i ? (int8_t)i : -1;
                ui.visit_t = plat_ms();
            }
        } else if ((ch & ~keys) >> WHITE_K[i] & 1u && ui.press == (int8_t)i) {
            press_end();
        } else if ((ch & ~keys) >> WHITE_K[i] & 1u && ui.visit == (int8_t)i) {
            ui.visit = -1;                             /* let go: a tap stays; a hold was a visit: */
            if (plat_ms() - ui.visit_t >= HOLD_MS)     /* back to the sound before it, as it came */
                scene_back();
        }
    }
    for (i = 0; i < 4u; i++) {                         /* the top four white keys */
        if (!((ch & keys) >> WHITE_K[NSCENES + i] & 1u))
            continue;
        if (btn >> B_SEL & 1u) {                       /* SEL + a top key: the bank (not while a scene key is
                                                        * held: a visit's way back stays put) */
            uint32_t j, held = 0;
            static const char *const BANK_N[NBANKS] = {"A", "B", "C", "PRESETS"};
            ui.btn_used |= 1u << B_SEL;
            for (j = 0; j < NSCENES; j++)
                held |= keys >> WHITE_K[j] & 1u;
            if (held)
                continue;
            if (proj.bank != i) {
                proj.bank = (uint8_t)i;
                ui.scene_at = -1;
                mark_dirty();
            }
            say_c("BANK", BANK_N[i], K_GOLD);
        } else {
            gesture_down((int)i);                      /* the gestures (v0.2) */
        }
    }
    for (i = 0; i < 4u; i++)                         /* ... and let go */
        if ((ch & ~keys) >> WHITE_K[NSCENES + i] & 1u)
            gesture_up((int)i);
    if (btn || keys || ch)
        ui.act_t = plat_ms();
    loop_run();
    midi_in();
    if ((e = plat_enc(EN_SELECT)) != 0)              /* SELECT: the LFO rate */
        global_turn(EN_SELECT, P_LFO_RATE, "LFO RATE", e);
    if ((e = plat_enc(EN_ALGO)) != 0)                /* ALGORITHM: the Movement, every LFO depth 0 .. 200 % */
        global_turn(EN_ALGO, P_MOVE, "MOVEMENT", e);
    if ((e = plat_enc(EN_PRESET)) != 0)              /* PRESETS: Gesture time, every gesture's speed */
        mtime_turn(e);
    for (i = 0; i < 4; i++)
        if ((e = plat_enc(EN_K1 + (int)i)) != 0) {
            knob_turn((int)i, e);
            ui.scene_at = -1;                          /* the sound is no longer the scene's */
            ui.morph = -1;                             /* a hand on a knob: the morph stops where it is */
            ui.visit = -1;                             /* ... and a visit held becomes a stay (no way back) */
            ui.press = -1;                             /* ... a crossfade too, where it is */
            if (ui.hold_btn >= 0)
                ui.hold_turned = 1;                    /* a peek: let go of the button, back to the page */
            ui.touch_t[i] = plat_ms() + 1200u;
            ui.act_t = plat_ms();
        }
}

/* ------------------------------------------------------------- screen --- */
/* Concept D (Figma "Screen concepts"): a thin top bar (the page, badges, a label for SELECT / ALGORITHM /
 * PRESETS, the LFO's lights); the picture in the top two-thirds, drone I on the left over KNOB 1-2, drone
 * II on the right over KNOB 3-4; the page's four values in the bottom third, each lighting while its
 * knob moves (its own timer: two hands, two lit). A switch shows as a banner across the top of the
 * picture. Nothing stays on screen that isn't the page's or moving. */
#define BAR_H 16
#define PIC_Y 16
#define PIC_H 139
#define VAL_Y 155
#define VAL_H 85
#define SCOPE_EVERY 3u                       /* the picture: every 3rd frame (~20 a second; the LCD is slow) */

static uint32_t hash(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

/* a rectangle with rounded corners (radius r, up to 6) */
static void cv_round(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)
{
    static const uint8_t IN[7][6] = {{0}, {1}, {1, 0}, {2, 1, 0}, {2, 1, 0, 0}, {3, 2, 1, 0, 0}, {4, 2, 1, 1, 0, 0}};
    int32_t j;
    if (r > 6)
        r = 6;
    if (r > h / 2)
        r = h / 2;
    cv_rect(x, y + r, w, h - 2 * r, c);
    for (j = 0; j < r; j++) {
        int32_t d = IN[r][j];
        cv_rect(x + d, y + j, w - 2 * d, 1, c);
        cv_rect(x + d, y + h - 1 - j, w - 2 * d, 1, c);
    }
}
static void cv_frame(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)   /* a 1 px outline */
{
    cv_rect(x + 2, y, w - 4, 1, c);
    cv_rect(x + 2, y + h - 1, w - 4, 1, c);
    cv_rect(x, y + 2, 1, h - 4, c);
    cv_rect(x + w - 1, y + 2, 1, h - 4, c);
    cv_rect(x + 1, y + 1, 1, 1, c);
    cv_rect(x + w - 2, y + 1, 1, 1, c);
    cv_rect(x + 1, y + h - 2, 1, 1, c);
    cv_rect(x + w - 2, y + h - 2, 1, 1, c);
}

static void text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

/* centred in w pixels: a word too wide for them has its letters a pixel closer */
static void text_fit(int32_t cx, int32_t y, int32_t w, const felucca_font_t *f, const char *s, uint16_t c)
{
    if (text_w(f, s) > w)
        cv_track = -1;
    text_c(cx, y, f, s, c);
    cv_track = 0;
}

/* the largest face a value fits in w pixels in (the large one only while lit: it has no lower case) */
static const felucca_font_t *fit_face(const char *s, int32_t w, int large)
{
    static const felucca_font_t *const F[3] = {&FONT_B, &FONT_S, &FONT_XS};
    const char *q = s;
    int i, lower = 0;
    for (; *q; q++)
        lower |= *q >= 'a' && *q <= 'z';
    if (large && !lower && text_w(&FONT_M, s) <= w)
        return &FONT_M;
    for (i = 0; i < 2; i++)
        if (text_w(F[i], s) <= w)
            return F[i];
    return F[2];
}

/* a knob's name on its card; the few too long for one, shorter there (the manual has the full ones) */
static const char *card_name(int p)
{
    const char *n = p == P_PLINK_OFF ? "Link ofs" : dr_param_info(p)->name;
    if (p == P_SHIFT_MOVE)
        return "Shift";
    if (p == P_SCENE_T)
        return "Scenes";                       /* Scene time */
    if (n[0] == 'X' && n[1] == '-' && n[2] == 'm')
        return "X-mod";                        /* X-mod amt */
    switch (p) {                               /* too long at the cards' size (FONT_S, 2026-10-20) */
    case P_CHAR: return "Char.";
    case P_FEEDBACK: return "Feedbk";
    case P_LFO_RMOD: return "R. mod";
    case P_MORPH: return "Switch";             /* Switch time */
    case P_GLIDE: return "Glide";              /* Pitch glide */
    case P_PRESS_SENS: return "Press.";
    default: break;
    }
    if (p < P_XMOD && p % GP_N == GP_OLFO_DEPTH)
        return "Osc";                          /* LFO depths: the oscillator's, the filter's */
    if (p < P_XMOD && p % GP_N == GP_FLFO_DEPTH)
        return "Filter";
    return n;
}

static int fade_q(void) { return (int)(dr_fade * 20.0f + 0.5f); }   /* the fade in 20 steps */

static int lit(int i) { return plat_ms() < ui.touch_t[i]; }          /* knob i moving (or just moved) */

static uint16_t blend(uint16_t a, uint16_t b, int q, int n)          /* a -> b, q of n */
{
    int r = ((a >> 11) * (n - q) + (b >> 11) * q) / n, g = (((a >> 5) & 63) * (n - q) + ((b >> 5) & 63) * q) / n,
        bl = ((a & 31) * (n - q) + (b & 31) * q) / n;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

/* the page's colour (its name; global values' accents) */
static uint16_t page_col(void)
{
    switch (ui.view) {
    case V_LFO: case V_LFO2: return K_LFO;
    case V_FX: case V_FX2: case V_TONE2: return K_FX;
    case V_PULSE: return K_PULSE;
    case V_SETUP: case V_SETUP2: return K_SETUP;
    default: return K_DIM;
    }
}

/* the top bar: the page's name, the loop's and pulse's badges, PLAY's hint while stopped (or the label of
 * SELECT / ALGORITHM / PRESETS while one turns), the LFO's two lights */
static void draw_bar(void)
{
    uint32_t rec_s = ui.loop == L_REC ? (plat_ms() - ui.loop_t0) / 1000u : 0u;   /* REC: how long so far */
    uint32_t h = hash(hash(hash(2166136261u ^ rec_s, ui.view | ui.playing << 5 | ui.loop << 7 |
                                                  (uint32_t)proj.par[P_PULSE] << 9 |
                                                  (uint32_t)proj.par[P_II_OUT] << 12),
                           (uint32_t)fade_q()),
                      0);
    int32_t x, i, rx = 234;
    int visiting = ui.visit >= 0 && plat_ms() - ui.visit_t >= HOLD_MS;          /* a scene key held: a visit */
    h = hash(h, (uint32_t)(visiting ? ui.visit + 1 : 0));
    if (h == ui.sig[0])
        return;
    ui.sig[0] = h;
    cv_begin(240, BAR_H, K_BG);
    {   /* the page's name, in capitals */
        char t[20];
        const char *n = VIEW_NAME[ui.view];
        for (i = 0; n[i] && i < 19; i++)
            t[i] = (char)(n[i] >= 'a' && n[i] <= 'z' ? n[i] - 32 : n[i]);
        t[i] = 0;
        x = cv_text(6, 0, &FONT_B, t, page_col() == K_DIM ? K_TEXT2 : page_col());
    }
    for (i = 0; i < (int32_t)NPAGE_BTN; i++)   /* a button's pages: which of them, "2/3" ("tap again") */
        if (on_pages((int)i) && PAGE_BTN[i].n > 1) {
            char t[6];
            t[0] = (char)('1' + ui.view - PAGE_BTN[i].first);
            t[1] = '/';
            t[2] = (char)('0' + PAGE_BTN[i].n);
            t[3] = 0;
            x = cv_text(x + 6, 2, &FONT_XS, t, K_DIM);
        }
    x += 10;
    if (ui.loop != L_IDLE) {                  /* REC and its time ("REC 1:05"; it stops at 5:00), or LOOP */
        char t[12] = "REC ", *q = t + 4;
        if (ui.loop == L_REC) {
            itoa_u(rec_s / 60u, q);
            while (*q)
                q++;
            *q++ = ':';
            *q++ = (char)('0' + rec_s % 60u / 10u);
            *q++ = (char)('0' + rec_s % 10u);
            *q = 0;
        }
        x = cv_text(x, 2, &FONT_XS, ui.loop == L_REC ? t : "LOOP", K_DRONE[0]) + 6;
    }
    if (proj.par[P_PULSE] && ui.view != V_PULSE)  /* on its own page, its name says it */
        x = cv_text(x, 2, &FONT_XS, "SWELL", K_PULSE) + 6;
    if (visiting) {                            /* "VISIT B3": let go and it goes back */
        char v[12] = "VISIT ";
        scene_name(v + 6, proj.bank, ui.visit);
        x = cv_text(x, 2, &FONT_XS, v, K_GOLD) + 6;
    }
    if (proj.par[P_II_OUT])                    /* drone II faded out on its own (SEL + PLAY) */
        x = cv_text(x, 2, &FONT_XS, "II OUT", K_DRONE[1]) + 6;
    /* (SELECT / ALGORITHM / PRESETS turning: big in the values row, draw_values) */
    {                                          /* stopped: say what PLAY does (the panel has no label); fading. Only
                                                * where the badges on the left leave room (II OUT covered it), else
                                                * a short form, else nothing */
        const char *st = !ui.playing ? (fade_q() > 0 ? "FADING OUT" : "PLAY to start") : fade_q() < 20 ? "FADING IN" : 0;
        const char *sh = !ui.playing ? (fade_q() > 0 ? "FADING" : "PLAY") : "FADING";
        if (st && rx - text_w(&FONT_XS, st) < x)
            st = sh;
        if (st && rx - text_w(&FONT_XS, st) >= x)
            cv_text(rx - text_w(&FONT_XS, st), 2, &FONT_XS, st, K_DIM);
    }
    cv_blit(0, 0);
}

/* a drone's waveform in its panel: its scope, glowing (a dim line around a bright one); its height is
 * its level (it fades with PLAY and its level) */
/* the picture as drawn: each new capture blends into it (~0.1 s at ~20 pictures a second), so a detuned partner's
 * ramp slides across instead of jumping from picture to picture (it looked like two waves at once) */
static float sc_disp[DR_NGROUPS][DR_SCOPE_N];
static void draw_scope(int g, int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    int32_t i, mid = y0 + h / 2, py = mid, half = h / 2 - 4;
    const float k = (float)half / 0.6f;       /* a drone at full level (with sub and drive) peaks near 0.6 */
    cv_round(x0, y0, w, h, 4, K_PANEL);
    for (i = 0; i < DR_SCOPE_N && i < w - 8; i++) {
        int32_t yy, x = x0 + 4 + i;
        sc_disp[g][i] += (dr_scope[g][i] - sc_disp[g][i]) * 0.45f;
        yy = mid - (int32_t)fm_clampf(sc_disp[g][i] * k, (float)-half, (float)half);
        if (i) {
            cv_line(x - 1, py - 1, x, yy - 1, K_DRONE_DIM[g]);
            cv_line(x - 1, py + 1, x, yy + 1, K_DRONE_DIM[g]);
            cv_line(x - 1, py, x, yy, K_DRONE[g]);
        }
        py = yy;
    }
    cv_text(x0 + 5, y0 + 3, &FONT_S, g ? "II" : "I", K_DRONE[g]);
}

/* ---------------------------------------------------- the page pictures --- */
/* each page draws what its knobs do, in the picture: a drone's page in two panels (drone I | drone II),
 * as on Play; a page of both drones' settings across one panel. Drawn ~20 times a second like the
 * waveforms, so whatever moves (a meter, a beat, the cross-mod) moves as it sounds. */
#define PAN_W 112
#define PAN_H (PIC_H - 10)
static int32_t pan_x(int g) { return g ? 122 : 6; }
static int gpar(int g, int p) { return proj.par[DR_GP(g, p)]; }
static uint16_t dim_of(uint16_t c) { return blend(K_PANEL, c, 1, 3); }

/* a glowing polyline through n points (x0 + i * dx, y[i]): all the glow first, then the line, so the
 * glow never covers the line */
static void plot(int32_t x0, int32_t dx, const int16_t *y, int n, uint16_t c)
{
    uint16_t d = dim_of(c);
    int i;
    for (i = 1; i < n; i++) {
        cv_line(x0 + (i - 1) * dx, y[i - 1] - 1, x0 + i * dx, y[i] - 1, d);
        cv_line(x0 + (i - 1) * dx, y[i - 1] + 1, x0 + i * dx, y[i] + 1, d);
    }
    for (i = 1; i < n; i++)
        cv_line(x0 + (i - 1) * dx, y[i - 1], x0 + i * dx, y[i], c);
}

/* a number with one decimal: "0.6", "12.5" */
static char *dec1(float v, char *b)
{
    int t = (int)(v * 10.0f + 0.5f);
    itoa_u((uint32_t)(t / 10), b);
    while (*b)
        b++;
    *b++ = '.';
    *b++ = (char)('0' + t % 10);
    *b = 0;
    return b;
}

/* the X-WAVE's four waves at phase ph (0..1), as the engine's: a falling saw, a square of pulse width pw */
static float wave_at(int w, float pw, float ph)
{
    switch (w) {
    case DR_SINE: return fm_sinf(6.2831853f * ph);
    case DR_TRI: return ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f;
    case DR_SAW: return 1.0f - 2.0f * ph;
    default: return ph < pw ? 1.0f : -1.0f;
    }
}
/* the X-WAVE at x (0..100): a wave, or two blended (within 2 of a wave: that wave, as the engine) */
static float xwave_at(int x, float pw, float ph)
{
    int s = x * 3, wi = (s + 2) / 100;
    float wf = (float)(s - 100 * wi) * 0.01f, a;
    if (wi >= 3 || wf <= 0.02f)
        return wave_at(wi > 3 ? 3 : wi, pw, ph);
    a = wave_at(wi, pw, ph);
    return a + (wave_at(wi + 1, pw, ph) - a) * wf;
}
static float pw_of(int g) { return 0.5f - 0.45f * (float)gpar(g, GP_WIDTH) * 0.01f; }

/* a wave across a box: cycles of the X-WAVE x, starting at phase ph0; amp of the half-height */
static void draw_xwave(int g, int x, int32_t x0, int32_t yc, int32_t w, int32_t half, float cycles, float ph0,
                       uint16_t c)
{
    int16_t y[PAN_W];
    int i, n = (int)(w / 2) + 1;
    float pw = pw_of(g);
    if (n > PAN_W)
        n = PAN_W;
    for (i = 0; i < n; i++) {
        float ph = ph0 + cycles * (float)i / (float)(n - 1);
        ph -= (float)(int)ph;
        y[i] = (int16_t)(yc - (int32_t)(xwave_at(x, pw, ph) * (float)half));
    }
    plot(x0, 2, y, n, c);
}

/* a drone's filter, as the engine's (12 dB state-variable): its response in dB at f, on the cutoff knob's
 * own scale (0..100 = 20 Hz .. 18 kHz); cut, the cutoff there (moved, for the cross-mod and the LFO) */
static float filt_db(int g, float cut, float f)
{
    int mode = gpar(g, GP_MODE);
    float r = (float)gpar(g, GP_RESO) * 0.001f, k = fm_maxf(2.0f - 2.1f * r, 0.06f), rin = 1.0f - 0.6f * r * r;
    float rr = fm_exp2f((f - cut) * 0.0981378f), r2 = rr * rr, d = (1.0f - r2) * (1.0f - r2) + k * k * r2, m2;
    if (mode == DR_LP) {                       /* as the engine: rin of the filter + (1 - rin) of a one-pole an
                                                * octave under the cutoff (the bass makeup), summed as complex */
        float re = rin * (1.0f - r2) / d, im = -rin * k * rr / d, q = 1.0f + 4.0f * r2;
        re += (1.0f - rin) / q;
        im -= (1.0f - rin) * 2.0f * rr / q;
        m2 = re * re + im * im;
    } else {
        m2 = (mode == DR_BP ? r2 : r2 * r2) * rin * rin / d;
    }
    return 3.0103f * fm_log2f(fm_maxf(m2, 1e-9f));
}

/* ... its curve across 20 Hz .. 18 kHz, +18 dB at the top, -36 at the bottom */
static void draw_filter(int g, float cut, int32_t x0, int32_t y0, int32_t w, int32_t h, uint16_t c)
{
    int16_t y[PAN_W];
    int i, n = (int)(w / 2) + 1;
    int32_t z = y0 + h * 18 / 54, cx;
    if (n > PAN_W)
        n = PAN_W;
    cv_rect(x0, z, w, 1, K_GRID);                                     /* 0 dB */
    cx = x0 + (int32_t)(fm_clampf(cut, 0.0f, 100.0f) * (float)w * 0.01f);
    cv_rect(cx, y0, 1, h, K_GRID);                                    /* the cutoff */
    for (i = 0; i < n; i++) {
        float db = filt_db(g, cut, 100.0f * (float)i / (float)(n - 1));
        y[i] = (int16_t)(z - (int32_t)(fm_clampf(db, -36.0f, 18.0f) * (float)h / 54.0f));
    }
    plot(x0, 2, y, n, c);
}

/* Play 2: each drone's level (a live meter, KNOB 1 / 3) and its resonance (its filter's curve, the peak growing,
 * KNOB 2 / 4) */
static void pic_play2(int g, int32_t x0, int32_t y0)
{
    int32_t mw = PAN_W - 16, fw;
    float l = dr_level[g], db = l > 1e-4f ? 6.0206f * fm_log2f(l) : -99.0f;
    char t[12];
    cv_round(x0 + 8, y0 + 26, mw, 8, 3, K_GRID);                       /* the level, -48 .. 0 dBFS (on top, as KNOB 1 / 3 comes first) */
    fw = (int32_t)(fm_clampf((db + 48.0f) / 48.0f, 0.0f, 1.0f) * (float)mw);
    if (fw > 3)
        cv_round(x0 + 8, y0 + 26, fw, 8, 3, K_DRONE[g]);
    if (db > -60.0f) {
        char *q = t;
        *q++ = '-';
        itoa_u((uint32_t)(int)(-db + 0.5f), q);
        while (*q)
            q++;
        scopy(q, " dB");
        if (db > -0.5f)
            scopy(t, "0 dB");
    } else {
        scopy(t, ui.playing ? "silent" : "stopped");
    }
    text_c(x0 + PAN_W / 2, y0 + 38, &FONT_S, t, K_TEXT2);
    draw_filter(g, dr_cut[g], x0 + 4, y0 + 66, PAN_W - 8, 52, K_DRONE[g]);
}




/* Tone 2: each drone's noise as it leaves the filter: its colour's slope (0 dB/oct white, -3 pink, level
 * at 100 Hz) through the filter's curve (dim behind it, moving with the LFO as Play 2's), as low as its level
 * is; its colour's name, and its level against the oscillators */
static void pic_tone(int g, int32_t x0, int32_t y0)
{
    int16_t y[PAN_W];
    int i, nv = gpar(g, GP_NOISE), cv = gpar(g, GP_NCOLOR), n = (PAN_W - 8) / 2 + 1;
    int32_t gx = x0 + 4, gy = y0 + 22, gh = 52, z = gy + gh * 18 / 54;
    float slope = -3.0f * (float)cv * 0.01f, share = 20.0f * 0.30103f * fm_log2f(fm_maxf((float)nv * 0.01f, 1e-3f));   /* 100: 0 dB */
    char t[16], *q;
    draw_filter(g, dr_cut[g], gx, gy, PAN_W - 8, gh, K_DRONE_DIM[g]);
    if (nv > 0) {
        for (i = 0; i < n; i++) {
            float f = 100.0f * (float)i / (float)(n - 1);
            float db = share + slope * (f * 0.0981378f - 2.3219281f) + filt_db(g, dr_cut[g], f);   /* 100 Hz: 0 */
            y[i] = (int16_t)(z - (int32_t)(fm_clampf(db, -36.0f, 18.0f) * (float)gh / 54.0f));
        }
        plot(gx, 2, y, n, K_DRONE[g]);
    }
    dr_param_text(DR_GP(g, GP_NCOLOR), cv, t);
    t[0] = (char)(t[0] - 'A' + 'a');
    cv_text(x0 + PAN_W - 6 - text_w(&FONT_S, t), y0 + 3, &FONT_S, t, nv ? K_DRONE[g] : K_DIM);
    if (!nv)                                   /* its level against the oscillators: "noise -12 dB" */
        scopy(t, "no noise");
    else {
        int db = (int)(-share + 0.5f);
        q = scopy(t, db ? "noise -" : "noise 0 dB");
        if (db) {
            itoa_u((uint32_t)db, q);
            while (*q)
                q++;
            scopy(q, " dB");
        }
    }
    text_c(x0 + PAN_W / 2, y0 + PAN_H - 18, &FONT_S, t, nv ? K_TEXT2 : K_DIM);
}

static void pic_osc(int g, int32_t x0, int32_t y0)
{
    static const char *const W[4] = {"Sin", "Tri", "Saw", "Sqr"};
    int x = gpar(g, GP_WAVE), k, sq = x >= 70;   /* PW heard (a tenth of square or more; at Saw, 67, it isn't) */
    int32_t tx0 = x0 + 12, tw = PAN_W - 24;
    char t[16], *q;
    draw_xwave(g, x, x0 + 6, y0 + 42, PAN_W - 12, 22, 1.0f, 0.0f, K_DRONE[g]);
    for (k = 0; k < 4; k++)                    /* where the X-WAVE sits: sine .. square */
        text_c(tx0 + tw * k / 3, y0 + 70, &FONT_XS, W[k], K_DIM);
    cv_rect(tx0, y0 + 88, tw, 2, K_GRID);
    cv_round(tx0 + tw * x / 100 - 3, y0 + 86, 7, 7, 3, K_DRONE[g]);
    /* the pulse width (KNOB 2 / 4): how high the square is, or that there's no square to shape */
    if (sq) {
        q = scopy(t, "PW ");
        itoa_u((uint32_t)(int)(pw_of(g) * 100.0f + 0.5f), q);
        while (*q)
            q++;
        scopy(q, "% high");
    } else {
        scopy(t, "PW: no square");
    }
    text_c(x0 + PAN_W / 2, y0 + 100, &FONT_S, t, sq ? K_TEXT2 : K_DIM);
}


static void pic_partner(int g, int32_t x0, int32_t y0)
{
    int on = gpar(g, GP_PARTNER) != 0, iv = gpar(g, GP_INTERVAL), x = gpar(g, GP_WAVE);
    float semi = DR_IV_SEMI[(unsigned)iv < DR_NINTERVALS ? iv : DR_IV_UNISON];
    float hz = 8.1757989f * fm_exp2f((dr_pitch[g] + semi) / 12.0f);   /* the partner's pitch */
    float beats = hz * (fm_exp2f((float)gpar(g, GP_DETUNE) * 0.003f / 12.0f) - 1.0f), ph;
    char t[24], *q;
    /* the main oscillator, and under it the partner: its cycles at the interval, sliding against the
     * main one at the beating rate (shown at most 2 a second) */
    draw_xwave(g, x, x0 + 6, y0 + 36, PAN_W - 12, 14, 2.0f, 0.0f, K_DRONE[g]);
    ph = (float)(plat_ms() % 100000u) * 0.001f * fm_minf(beats, 2.0f);
    if (on)
        draw_xwave(g, x, x0 + 6, y0 + 76, PAN_W - 12, 14, 2.0f * fm_exp2f(semi / 12.0f), ph - (float)(int)ph,
                   blend(K_PANEL, K_DRONE[g], 2, 3));
    dr_param_text(DR_GP(g, GP_INTERVAL), iv, t);
    cv_text(x0 + PAN_W - 6 - text_w(&FONT_S, t), y0 + 3, &FONT_S, t, on ? K_DRONE[g] : K_DIM);
    if (!on)
        scopy(t, "partner off");
    else if (beats < 0.05f)
        scopy(t, "no beating");
    else {
        q = scopy(t, "beats ");
        q = dec1(fm_minf(beats, 99.0f), q);
        scopy(q, "/s");
    }
    text_c(x0 + PAN_W / 2, y0 + PAN_H - 18, &FONT_S, t, K_DIM);
}

/* --- the pages of both drones' settings: one panel across --- */
#define WIDE_W 228

static void text_r(int32_t xr, int32_t y, const char *s, uint16_t c) { cv_text(xr - text_w(&FONT_S, s), y, &FONT_S, s, c); }

/* Pulse: the gate over its two beats, twice (as set: depth, shape, swing), where it is now, ON / OFF */
static void pic_pulse(int32_t y0)
{
    int on = proj.par[P_PULSE] != 0, i, n = 105;
    int16_t y[105];
    int32_t top = y0 + 26, h = PAN_H - 56;
    uint16_t c = on ? K_PULSE : K_FAINT;
    char t[16];
    for (i = 0; i < n; i++)
        y[i] = (int16_t)(top + h - (int32_t)(dr_pulse_wave(4.0f * (float)i / (float)(n - 1)) * (float)h));
    cv_rect(12, top + h, 208, 1, K_GRID);
    for (i = 1; i < 4; i++)                    /* the beats */
        cv_rect(12 + 52 * i, top, 1, h, K_GRID);
    plot(12, 2, y, n, c);
    if (on) {                                  /* now: a dot on the line, across both bars (it went over */
        static float last;                     /* the first only, then jumped back) */
        static uint8_t odd;
        float ph = dr_pulse_ph;
        int32_t px, py;
        if (ph < last - 1.0f)                  /* a new bar: the other half of the picture */
            odd ^= 1u;
        last = ph;
        px = 12 + (int32_t)((ph + 2.0f * (float)odd) * 52.0f);
        py = top + h - (int32_t)(dr_pulse_wave(ph) * (float)h);
        cv_round(px - 3, py - 3, 7, 7, 3, K_TEXT);
    }
    dr_param_text(P_PULSE_RATE, proj.par[P_PULSE_RATE], t);
    cv_text(12, y0 + 3, &FONT_S, t, c);
    text_r(222, y0 + 3, on ? "ON" : "OFF", on ? K_PULSE : K_DIM);
    text_c(120, y0 + PAN_H - 18, &FONT_S,
           !on ? "tap ARP to start the swell" : proj.par[P_LFO_LOCK] ? "the LFO restarts each bar" : "the swell, over two bars",
           proj.par[P_LFO_LOCK] && on ? K_LFO : K_DIM);
}

/* LFO: the routing as a picture. Each wave on the left (I above, II below: one cycle of its shape, a dot
 * where the LFO is in it now), the five destinations on the right (drone I's oscillator and filter, drone
 * II's, the delay's time), and a line from each wave to what it moves: bright when its depth moves it, dim
 * when routed at depth 0; a route on both waves has two lines. The rate at the bottom */
#define LFO_WX0 12                             /* the waves: x 12 .. 100 */
#define LFO_WW 88
static void pic_lfo(int32_t y0)
{
    static const int32_t WY[2] = {38, 92};     /* the waves' middles (from y0) */
    int tap, d;
    char t[24];
    for (tap = 0; tap < 2; tap++) {
        int sh = proj.par[tap ? P_LFO_W2 : P_LFO_W1], i, n = LFO_WW / 2 + 1;
        int32_t yc = y0 + WY[tap], half = 12;
        int16_t y[LFO_WW / 2 + 1];
        uint16_t c = sh ? K_LFO : K_FAINT;
        /* one cycle across (random: this cycle, its phases 1..2), and a dot where the LFO is in it now */
        float r0 = sh == DR_LFO_RANDOM ? 1.0f : 0.0f;
        for (i = 0; i < n; i++)
            y[i] = (int16_t)(yc - (int32_t)(dr_lfo_wave(tap, sh, r0 + 0.9999f * (float)i / (float)(n - 1)) * (float)half));
        plot(LFO_WX0, 2, y, n, c);
        if (sh) {
            float ph = dr_lfo_ph;
            int32_t px = LFO_WX0 + (int32_t)(ph * (float)LFO_WW);
            int32_t py = yc - (int32_t)(dr_lfo_wave(tap, sh, r0 + ph) * (float)half);
            cv_round(px - 3, py - 3, 7, 7, 3, K_TEXT);
        }
        cv_text(LFO_WX0, yc - 32, &FONT_S, tap ? "II" : "I", c);
        dr_param_text(tap ? P_LFO_W2 : P_LFO_W1, sh, t);
        cv_text(LFO_WX0 + 16, yc - 32, &FONT_S, t, sh ? K_TEXT2 : K_DIM);
    }
    /* the destinations, and the lines to them */
    for (d = 0; d < 5; d++) {
        int g = d / 2, route, depth, w;
        uint16_t col;
        int32_t ny = y0 + 6 + d * 24, ay = ny + 8, ax;
        if (d < 4) {
            int osc = d % 2 == 0;
            route = gpar(g, osc ? GP_OLFO : GP_FLFO);
            depth = gpar(g, osc ? GP_OLFO_DEPTH : GP_FLFO_DEPTH);
            scopy(scopy(t, g ? "II " : "I "), osc ? (gpar(g, GP_OLFO_DEST) == DR_TO_XWAVE ? "wave" : "pitch")
                                                  : (gpar(g, GP_FLFO_DEST) == DR_TO_RESO ? "reso" : "cutoff"));
            col = K_DRONE[g];
        } else {
            route = proj.par[P_DLY_LFO];
            depth = proj.par[P_DLY_LFO_DEPTH];
            scopy(t, "delay");
            col = K_FX;
        }
        ax = 222 - text_w(&FONT_S, t) - 6;
        text_r(222, ny, t, route == DR_LFO_OFF ? K_DIM : depth ? col : blend(K_PANEL, col, 3, 4));   /* AA: 5:1 and up */
        for (w = 0; w < 2; w++) {                  /* a line from each wave the route takes */
            int sh = proj.par[w ? P_LFO_W2 : P_LFO_W1];
            if (route != w + 1 && route != DR_LFO_BOTH)
                continue;
            {
                uint16_t lc = !sh || !depth ? blend(K_PANEL, col, 1, 3) : col;
                int32_t sx = LFO_WX0 + LFO_WW + 4, sy = y0 + WY[w];
                if (sh && depth)                   /* a glow either side, as the plots */
                    cv_line(sx, sy + 1, ax, ay + 1, dim_of(col));
                cv_line(sx, sy, ax, ay, lc);
                cv_round(ax - 1, ay - 2, 5, 5, 2, lc);
            }
        }
    }
    {   /* the rate; locked to the swell, the lock beside it */
        int lk = proj.par[P_LFO_LOCK] != 0;
        int32_t x;
        dr_param_text(P_LFO_RATE, proj.par[P_LFO_RATE], scopy(t, lk ? (proj.par[P_PULSE] ? "locked " : "lock (no swell) ") : "rate "));
        x = cv_text(LFO_WX0, y0 + PAN_H - 18, &FONT_S, t, lk ? K_PULSE : K_LFO);
        if (proj.par[P_MOVE] != 50) {          /* the Movement (OCT- / OCT+) when it isn't as set */
            dr_param_text(P_MOVE, proj.par[P_MOVE], scopy(t, "move "));
            cv_text(x + 10, y0 + PAN_H - 18, &FONT_S, t, K_TEXT2);
        }
    }
}

/* LFO mods: a picture for each of its knobs. Top: the rate mod, the wave that moves the rate drawn as it now
 * runs (bright) over its plain shape (dim), and how far the rate swings (KNOB 1-2). Bottom left: cross-mod,
 * drone II's filter curve moving with drone I's sound, as far as its amount (KNOB 3). Bottom right: the
 * delay's time wobbling on its LFO wave, as far as its depth (KNOB 4; both scaled by the Movement) */
static void pic_lfo2(int32_t y0)
{
    int w[2] = {proj.par[P_LFO_W1], proj.par[P_LFO_W2]}, src = proj.par[P_LFO_RMOD_SRC], d = proj.par[P_LFO_RMOD];
    float rm = (float)d * 0.03f, mv = (float)proj.par[P_MOVE] * 0.02f, ph = 0.0f;
    const float rk = dr_lfo_rmod_k(rm, src, w[0], w[1]);   /* the engine's scale: two cycles still fit */
    int16_t yw[105], yp[105];
    int i, n = 105;
    char t[32], *q;
    /* --- the rate mod (top) --- */
    for (i = 0; i < n; i++) {                  /* 2 plain cycles across: the source wave warped, and plain */
        float v0 = dr_lfo_wave(0, w[0], ph), v1 = dr_lfo_wave(1, w[1], ph);
        float pl = 2.0f * (float)i / (float)(n - 1);
        float s = src == DR_LFO_BOTH ? fm_clampf(v0 + v1, -1.0f, 1.0f) : src == DR_LFO_II ? v1 : v0;
        float sp = src == DR_LFO_BOTH ? fm_clampf(dr_lfo_wave(0, w[0], pl) + dr_lfo_wave(1, w[1], pl), -1.0f, 1.0f)
                 : dr_lfo_wave(src == DR_LFO_II ? 1 : 0, w[src == DR_LFO_II ? 1 : 0], pl);
        yw[i] = (int16_t)(y0 + 38 - (int32_t)(s * 15.0f));
        yp[i] = (int16_t)(y0 + 38 - (int32_t)(sp * 15.0f));
        ph += (2.0f / (float)(n - 1)) * rk * fm_exp2f(rm * s);
        ph -= 2.0f * (float)(int)(ph * 0.5f);
    }
    if (d)
        plot(12, 2, yp, n, K_GRID);
    plot(12, 2, yw, n, d ? K_LFO : K_FAINT);
    cv_text(12, y0 + 2, &FONT_S, "RATE", d ? K_LFO : K_DIM);
    if (d) {                                   /* how far the rate swings: "x0.6 .. x1.7" */
        q = scopy(t, "x");
        q = dec1(rk * fm_exp2f(-fm_fabsf(rm)), q);
        q = scopy(q, " .. x");
        dec1(rk * fm_exp2f(fm_fabsf(rm)), q);
    } else {
        scopy(t, "off");
    }
    text_r(222, y0 + 2, t, d ? K_TEXT2 : K_DIM);
    cv_rect(12, y0 + 62, 210, 1, K_GRID);
    cv_rect(118, y0 + 68, 1, PAN_H - 72, K_GRID);
    /* --- cross-mod (bottom left): drone II's filter moving with drone I's sound --- */
    {
        int on = proj.par[P_XMOD] != 0;
        float a = (float)proj.par[P_XMOD_AMT] * 0.01f, cut = dr_cut[1], k = 12.0f * a * a / 0.0981378f;
        float sft = on ? dr_scope[0][(ui.frame * 7u) % DR_SCOPE_N] * k : 0.0f;
        if (on && a > 0.0f) {                  /* the range drone I's sound pushes it through (a band that widens
                                                * with the amount: one moving line alone hardly showed it), dim */
            float mn = 0.0f, mx = 0.0f;
            uint32_t j;
            for (j = 0; j < DR_SCOPE_N; j++) {
                mn = fm_minf(mn, dr_scope[0][j]);
                mx = fm_maxf(mx, dr_scope[0][j]);
            }
            draw_filter(1, cut + mn * k, 14, y0 + 86, 96, 36, K_DRONE_DIM[1]);
            draw_filter(1, cut + mx * k, 14, y0 + 86, 96, 36, K_DRONE_DIM[1]);
        }
        draw_filter(1, cut + sft, 14, y0 + 86, 96, 36, on ? K_DRONE[1] : K_FAINT);
        cv_text(12, y0 + 66, &FONT_S, on ? "I > II cut" : "x-mod off", on ? K_DRONE[1] : K_DIM);
    }
    /* --- the delay's time on its LFO (bottom right) --- */
    {
        int tap = proj.par[P_DLY_LFO], dd = proj.par[P_DLY_LFO_DEPTH];
        int w1 = tap != DR_LFO_II ? w[0] : 0, w2 = tap >= DR_LFO_II ? w[1] : 0;
        float dep = (float)dd * 0.01f * fm_fabsf((float)dd * 0.01f) * 0.25f * mv;   /* as the engine */
        int16_t yd[49];
        int32_t yc = y0 + 102;
        for (i = 0; i < 49; i++) {             /* the time, +-25 % full height, two cycles */
            float p2 = 2.0f * (float)i / 48.0f, v = dr_lfo_wave(0, w1, p2) + dr_lfo_wave(1, w2, p2);
            yd[i] = (int16_t)(yc - (int32_t)fm_clampf(v * dep / 0.25f * 16.0f, -18.0f, 18.0f));
        }
        cv_rect(126, yc, 96, 1, K_GRID);
        plot(126, 2, yd, 49, dd && (w1 || w2) ? K_FX : K_FAINT);
        q = scopy(t, "delay ");
        if (dd && (w1 || w2)) {                /* "+-25%"; a negative depth wobbles the other way: "-+25%" */
            q = scopy(q, dd < 0 ? "-+" : "+-");
            itoa_u((uint32_t)(fm_fabsf(dep) * 100.0f + 0.5f), q);
            while (*q)
                q++;
            scopy(q, "%");
        } else {
            scopy(q, "still");
        }
        cv_text(126, y0 + 66, &FONT_S, t, dd ? K_FX : K_DIM);
    }
}

/* Setup 2: the unit and the room, a corner each: TUNE (KNOB 1) a needle, -50 .. +50 cents; STEREO (KNOB 2) the
 * drones as far apart as it puts them; PRESSURE (KNOB 3) what a key's press gives, light .. firm; KNOB 4 nothing (the
 * speaker's low cut follows the headphone jack). None is in scenes */
static void pic_setup2(int32_t y0)
{
    int tn = proj.par[P_TUNE], i;
    float wid = (float)proj.par[P_WIDTH] * 0.01f;
    char t[16];
    {   /* TUNE */
        int32_t cx = 60, nx = cx + tn * 40 / 50;
        cv_text(12, y0 + 3, &FONT_S, "TUNE", K_SETUP);
        dr_param_text(P_TUNE, tn, t);
        text_r(110, y0 + 3, t, K_TEXT2);
        cv_rect(cx - 40, y0 + 34, 81, 1, K_GRID);
        cv_rect(cx, y0 + 26, 1, 17, K_FAINT);
        cv_round(nx - 3, y0 + 31, 7, 7, 3, K_SETUP);
    }
    {   /* STEREO: drone I and II, two short upright bars on a line */
        int32_t cx = 176;
        cv_text(128, y0 + 3, &FONT_S, "STEREO", K_SETUP);
        cv_rect(cx - 30, y0 + 34, 61, 1, K_GRID);
        cv_rect(cx - 2 - (int32_t)(wid * 28.0f), y0 + 24, 3, 20, K_DRONE[0]);
        cv_rect(cx + (int32_t)(wid * 28.0f), y0 + 24, 3, 20, K_DRONE[1]);
    }
    cv_rect(12, y0 + 54, 210, 1, K_GRID);
    {   /* PRESSURE: what a press gives, light .. firm */
        int16_t y[46];
        for (i = 0; i < 46; i++)
            y[i] = (int16_t)(y0 + 112 - (int32_t)(press_curve((float)i / 45.0f) * 30.0f));
        cv_text(12, y0 + 60, &FONT_S, "PRESSURE", K_SETUP);
        plot(12, 2, y, 46, K_SETUP);
    }
}

/* the reverb's tail, as measured (to -60 dB after a burst, QA batch 3; doubling the -30 dB time had made
 * the long ones short, and the Chorus hall no longer than the Hall) and its pre-delay: an envelope over 5 s,
 * as high as h */
static const float RV_PRE_S[DR_NREV] = {0.004f, 0.018f, 0.042f, 0.024f};   /* drone_fx.c's pre-delays */
static float rev_secs(int v) { return v >= 100 ? 0.0f : 0.3f * fm_exp2f((float)v * 0.070607f); }   /* 0: Inf */
/* a reverb's tail over 5 s: dB falling in a straight line to -60 at its length (flat at Inf), after the type's
 * pre-delay */
static void draw_tail(int type, int len, int32_t x0, int32_t yb, int32_t w, int32_t h, uint16_t c)
{
    int16_t y[115];
    int i, n = (int)(w / 2) + 1;
    float T = rev_secs(len);
    if (n > 115)
        n = 115;
    for (i = 0; i < n; i++) {
        float s = 5.0f * (float)i / (float)(n - 1) - RV_PRE_S[(unsigned)type < DR_NREV ? type : 2];
        float a = s < 0.0f ? 0.0f : T <= 0.0f ? 1.0f : fm_exp2f(-s / T * 9.966f);   /* 2^-9.966 = -60 dB */
        y[i] = (int16_t)(yb - (int32_t)(a * (float)h));
    }
    plot(x0, 2, y, n, c);
}

/* Delay: the echoes over 2 s, the left side above the line and the right below it (Wide: the right comes
 * sooner; Ping-pong: they take turns), each as loud as Repeats leaves it, darker each time */
static void pic_fx(int32_t y0)
{
    const int ty = (unsigned)proj.par[P_DLY_TYPE] < DR_NDLY ? proj.par[P_DLY_TYPE] : 0;
    float ms = fm_minf(70.0f * fm_exp2f((float)proj.par[P_DLY_TIME] * 0.026845f), 270.0f);   /* fm1-dock (take.py) */
    float fb = fm_minf((float)proj.par[P_DLY_REPEATS] * 0.0115f, 1.0f);
    float mix = (float)proj.par[P_DLY_MIX] * 0.01f, wet = mix > 0.0f ? 0.25f + 0.75f * mix : 0.0f;
    int32_t mid = y0 + 72, h = 34, k, side;
    char t[24], *q;
    cv_rect(12, mid, 208, 1, K_GRID);
    cv_rect(12, mid - h, 3, 2 * h, K_FAINT);   /* the dry sound */
    for (side = 0; side < 2; side++) {
        float a = 1.0f, step = ms * (side && ty == 1 ? 0.75f : 1.0f);
        for (k = 1; k < 40; k++) {
            int32_t x = 12 + (int32_t)(step * (float)k * 0.104f), bh;   /* 208 px = 2 s */
            int heard = ty != 2 || (k & 1) != side;   /* ping-pong: odd repeats on the left, even on the right */
            a *= fb;
            if (x > 218)
                break;
            bh = (int32_t)(a * wet * (float)h);
            if (bh < 1)
                break;
            if (!heard)
                continue;
            cv_rect(x, side ? mid + 2 : mid - bh, 3, bh, blend(K_FX, K_PANEL, k > 6 ? 6 : (int)k, 9));
        }
    }
    cv_text(12, y0 + 3, &FONT_S, "L", K_DIM);
    cv_text(12, y0 + 112, &FONT_S, "R", K_DIM);
    q = scopy(t, DR_DLY_NAME[ty]);
    q = scopy(q, "  ");
    dr_param_text(P_DLY_TIME, proj.par[P_DLY_TIME], q);
    cv_text(28, y0 + 3, &FONT_S, t, mix > 0.0f ? K_FX : K_DIM);
    if (proj.par[P_DLY_REPEATS] > 85)
        text_r(222, y0 + 3, "runaway", K_GOLD);
    else if (ty == 3)
        text_r(222, y0 + 3, "no wow", K_DIM);
}

/* Reverb: the tail over 5 s (its length; flat at Inf, "freeze"), as high as its mix, darker as Damp rises */
static void pic_fx2(int32_t y0)
{
    int ty = (unsigned)proj.par[P_REV_TYPE] < DR_NREV ? proj.par[P_REV_TYPE] : 2, len = proj.par[P_REV_LEN];
    float rmix = (float)proj.par[P_REV_MIX] * 0.01f, damp = (float)proj.par[P_REV_DAMP] * 0.01f;
    uint16_t c = blend(K_FX, K_PANEL, (int)(damp * 4.0f + 0.5f), 9);
    char t[24], *q;
    q = scopy(t, DR_REV_NAME[ty]);
    q = scopy(q, "  ");
    dr_param_text(P_REV_LEN, len, q);
    cv_text(12, y0 + 3, &FONT_S, t, rmix > 0.0f ? K_FX : K_DIM);
    if (len >= 100)
        text_r(222, y0 + 3, "freeze", K_GOLD);
    cv_rect(12, y0 + 110, 208, 1, K_GRID);
    draw_tail(ty, len, 12, y0 + 110, 208, (int32_t)(80.0f * fm_maxf(rmix, 0.08f)), rmix > 0.0f ? c : K_FAINT);
    cv_text(12, y0 + 114, &FONT_XS, "5 s", K_DIM);
}

/* Tone 2: the oscillators' character, a saw from clean to vintage (left, KNOB 1); the feedback loop's level, live
 * (right, KNOB 2); under them AGE (KNOB 3): how worn the sound is, a bar (it wears in slowly), and DRIFT (KNOB 4) */
static void pic_tone2(int32_t y0)
{
    int fb = proj.par[P_FEEDBACK], ch = proj.par[P_CHAR], i;
    {   /* CHARACTER: three cycles of a saw as the oscillators now make it (bent, rounded) */
        float c = (float)ch * 0.01f, r = 0.0f, k2 = 1.0f - 0.85f * c;
        int16_t y[51];
        for (i = 0; i < 51 + 8; i++) {         /* 8 to settle the rounding */
            float ph = (float)(i % 17) / 17.0f, s = 1.0f - 2.0f * ph;
            s += 0.25f * c * s * s - 0.083f * c;
            r += (s - r) * k2;
            if (i >= 8)
                y[i - 8] = (int16_t)(y0 + 62 - (int32_t)(r * 24.0f));
        }
        cv_text(12, y0 + 3, &FONT_S, ch ? "CHARACTER" : "clean", ch ? K_TEXT2 : K_DIM);
        plot(12, 2, y, 51, ch ? K_TEXT2 : K_FAINT);
    }
    {   /* FEEDBACK: a bar, the loop's level now */
        int32_t bh = (int32_t)(fm_clampf(dr_fb, 0.0f, 1.0f) * 72.0f);
        cv_text(128, y0 + 3, &FONT_S, "FEEDBACK", fb ? K_FX : K_DIM);
        cv_round(164, y0 + 28, 14, 56, 4, K_GRID);
        if (fb && bh > 3)
            cv_round(164, y0 + 84 - (bh * 56 / 72), 14, bh * 56 / 72, 4, dr_fb > 0.6f ? K_GOLD : K_FX);
    }
    {   /* AGE (KNOB 3): how worn, a bar */
        int ag = proj.par[P_AGE];
        int32_t w = 56 * ag / 100;
        cv_text(12, y0 + 98, &FONT_S, "AGE", ag ? K_TEXT2 : K_DIM);
        cv_round(52, y0 + 106, 56, 6, 3, K_GRID);
        if (w > 3)
            cv_round(52, y0 + 106, w, 6, 3, K_TEXT2);
    }
    {   /* DRIFT (KNOB 4): a slow wander, as wide as it is set, moving */
        float drift = (float)proj.par[P_DRIFT] * 0.08f;
        int16_t y[21];
        for (i = 0; i < 21; i++) {
            float s = (float)i * 0.35f + (float)(plat_ms() % 60000u) * 0.0008f;
            y[i] = (int16_t)(y0 + 108 - (int32_t)(fm_sinf(s) * fm_sinf(s * 0.37f + 1.0f) * drift * 1.2f));
        }
        cv_text(128, y0 + 98, &FONT_S, "DRIFT", drift > 0.0f ? K_TEXT2 : K_DIM);
        plot(180, 2, y, 21, drift > 0.0f ? K_TEXT2 : K_FAINT);
    }
}

/* THE BLACK KEYS' MAP (SEL held 0.4 s, nothing else asking for the screen: a knob turning, a white key held): the
 * eleven keys drawn where they sit on the keyboard (3-2-3-2-1), lit as the panel's are, and only their groups
 * named. A key touched while SEL is held names itself and its setting, large, and doesn't switch (Evan, 2026-10-19:
 * the eleven names and settings at once were too crowded to read). The LFO pages' layer on those pages */
#define KEYMAP_MS 400u
static int keymap_on(void)
{
    uint32_t white = 0, i;
    for (i = 0; i < NWHITE; i++)
        white |= 1u << WHITE_K[i];
    return (ui.btn >> B_SEL & 1u) && (plat_ms() - ui.sel_t >= KEYMAP_MS || ui.km_key >= 0) && !lit(0) && !lit(1) &&
           !lit(2) && !lit(3) && !(ui.keys & white) && ui.press < 0;
}
static void sw_text(sw_t w, const char **name, const char **st)
{
    static const char *const SUB[5] = {"", "-1 oct", "-2 oct", "1/3", "1/5"}, *const MODE[3] = {"LP", "BP", "HP"};
    static const char *const TAP[4] = {"off", "I", "II", "I+II"};
    const int16_t *p = &proj.par[DR_GP(w.g, 0)];
    int on = sw_get(w);
    *st = on ? "on" : "off";
    switch (w.s) {
    case S_PARTNER: *name = "Partner"; break;
    case S_SUBOCT: *name = "Sub"; *st = SUB[(unsigned)p[GP_SUBOCT] < 5u ? p[GP_SUBOCT] : 1]; break;
    case S_HP: *name = "Filter"; *st = MODE[(unsigned)p[GP_MODE] < 3u ? p[GP_MODE] : 0]; break;
    case S_PLOCK: *name = "Pitch lock"; break;
    case S_LFORESET: *name = "LFO reset"; *st = proj.par[P_LFO_LOCK] ? "locked" : "tap"; break;
    case S_PLINK: *name = "Pitch link"; break;
    case S_SYNC: *name = "Hard sync"; break;
    case S_XMOD: *name = "X-mod"; break;
    case S_OTAP: *name = "Osc LFO"; *st = TAP[(unsigned)p[GP_OLFO] & 3u]; break;
    case S_ODEST: *name = "Osc to"; *st = on ? "X-Wave" : "Pitch"; break;
    case S_FDEST: *name = "Filt to"; *st = on ? "Reso" : "Cutoff"; break;
    case S_FTAP: *name = "Filt LFO"; *st = TAP[(unsigned)p[GP_FLFO] & 3u]; break;
    case S_DTAP: *name = "Dly LFO"; *st = TAP[(unsigned)proj.par[P_DLY_LFO] & 3u]; break;
    default: *name = ""; *st = ""; break;
    }
}
/* the map's groups, each layer's: first key, how many, its name, its colour (0 drone I, 1 drone II, 2 the ink) */
typedef struct {
    uint8_t k0, n;
    const char *name;
    uint8_t col;
} km_group_t;
static const km_group_t KM_NORMAL[4] = {{0, 3, "I", 0}, {3, 2, "BOTH", 2}, {5, 3, "II", 1}, {8, 3, "LINKS", 2}};
static const km_group_t KM_LFO[4] = {{0, 4, "I", 0}, {4, 1, "BOTH", 2}, {5, 4, "II", 1}, {9, 2, "DLY, XMOD", 2}};
static const uint8_t KM_CX[11] = {15, 30, 45, 75, 90, 120, 135, 150, 180, 195, 225};   /* key centres, 15 px a white */
static const km_group_t *km_group(int k)
{
    const km_group_t *g = sw_layer() == SW_LFO ? KM_LFO : KM_NORMAL;
    int i;
    for (i = 0; i < 3; i++)
        if (k < g[i].k0 + g[i].n)
            return &g[i];
    return &g[3];
}
static uint16_t km_colour(const km_group_t *g) { return g->col < 2 ? K_DRONE[g->col] : K_PALE; }

static void keymap_pic(void)                   /* the keyboard, in the picture's place */
{
    const int32_t y = 6, h = 104, bh = 64;
    const km_group_t *gs = sw_layer() == SW_LFO ? KM_LFO : KM_NORMAL;
    int i;
    for (i = 0; i < 16; i++) {                                    /* the white keys, dark */
        cv_rect(i * 15, y, 15, h, K_PANEL);
        cv_rect(i * 15, y, 1, h, K_BG);
    }
    for (i = 0; i < 11; i++) {
        int32_t x = KM_CX[i] - 4;
        uint16_t c = km_colour(km_group(i));
        int on = sw_get(sw_layer()[i]);
        if (sw_layer()[i].s == S_LFORESET)
            on = proj.par[P_LFO_LOCK] != 0;
        if (on)                                                    /* lit as the panel's (a third choice: half) */
            cv_round(x, y, 9, bh, 2, on == 2 ? blend(K_PANEL, c, 1, 2) : c);
        else {
            cv_rect(x, y, 9, bh, K_BG);
            cv_frame(x, y, 9, bh, K_FAINT);
        }
        if (i == ui.km_key) {                                      /* the one touched: outlined, marked */
            cv_frame(x - 1, y, 11, bh + 1, K_TEXT);
            cv_round(x - 2, y + bh + 4, 13, 3, 1, K_TEXT);
        }
    }
    for (i = 0; i < 4; i++) {                                      /* the groups: a line under each, its name */
        int32_t a = KM_CX[gs[i].k0] - 6, b = KM_CX[gs[i].k0 + gs[i].n - 1] + 6;
        int32_t cx = (a + b) / 2, hw = (int32_t)text_w(&FONT_XS, gs[i].name) / 2;
        cv_round(a, y + h + 6, b - a, 2, 1, km_colour(&gs[i]));
        if (cx + hw > 237)                                         /* the last name inside the screen */
            cx = 237 - hw;
        text_c(cx, y + h + 10, &FONT_XS, gs[i].name, km_colour(&gs[i]));
    }
}

static void big_value(int32_t cx, int32_t y, const char *s, uint16_t c);
static void keymap_values(void)                /* the values' place: the key touched, or how to ask */
{
    if (ui.km_key < 0) {
        text_c(120, 22, &FONT_S, "touch a key: what it does", K_TEXT2);
    } else {
        const km_group_t *g = km_group(ui.km_key);
        const char *name, *st;
        char t[32], v[16], *q = t;
        uint16_t c = km_colour(g);
        int i;
        sw_text(sw_layer()[ui.km_key], &name, &st);
        if (g->col < 2)                                            /* "I: PARTNER", "PITCH LOCK" */
            q = scopy(scopy(q, g->name), ": ");
        for (; *name && q < t + sizeof t - 1; name++)
            *q++ = (char)(*name >= 'a' && *name <= 'z' ? *name - 32 : *name);
        *q = 0;
        cv_round(6, 2, WIDE_W, 66, 6, K_CARD);
        cv_frame(6, 2, WIDE_W, 66, c);
        text_c(120, 7, &FONT_XS, t, c);
        for (i = 0; st[i] && i < (int)sizeof v - 1; i++)           /* the setting in capitals: all of it large */
            v[i] = (char)(st[i] >= 'a' && st[i] <= 'z' ? st[i] - 32 : st[i]);
        v[i] = 0;
        big_value(120, 26, v, K_TEXT);
    }
    cv_text(10, 72, &FONT_XS, "with SEL:  D5-G5 bank   OCT wave back", K_DIM);
}

/* Setup 1: the three times, a row each: FADE (KNOB 1, PLAY's fade in and out), SWITCH TIME (KNOB 2, a black key's or
 * a wave's move to its new setting), SCENE TIME (KNOB 3, every scene move); each a bar as far as its knob and its time. None is in scenes */
static void pic_setup(int32_t y0)
{
    static const uint8_t P[3] = {P_FADE, P_MORPH, P_SCENE_T};
    static const char *const N[3] = {"FADE", "SWITCH TIME", "SCENE TIME"},
                             *const W[3] = {"PLAY in, out", "black keys, LFO waves", "between scenes"};
    int r;
    for (r = 0; r < 3; r++) {
        int32_t y = y0 + 4 + r * 38, w = 76 * proj.par[P[r]] / 100;   /* the bars right of the hints */
        char t[16];
        cv_text(12, y, &FONT_S, N[r], K_SETUP);
        cv_text(12, y + 18, &FONT_XS, W[r], K_DIM);
        dr_param_text(P[r], proj.par[P[r]], t);
        text_r(222, y, t, K_TEXT2);
        cv_round(146, y + 24, 76, 6, 3, K_GRID);
        if (w > 3)
            cv_round(146, y + 24, w, 6, 3, K_SETUP);
    }
}

/* the picture: the page's own (on Play, the two waveforms), a banner while a message shows. No interval
 * between the drones (Evan, 2026-10-10: tune by ear) */
/* Moves: SHIFT's move (KNOB 1) on the left: where Shift (E5) takes the drones (the line home, the mark the move, a
 * fifth up or down at the edges; lit while Shift is in); WANDER (KNOB 3, its scale KNOB 4) on the right:
 * drone II's walk around its own pitch, the line its home, the dot where it is now (an octave up or down at the
 * edges) and how far, in semitones (under the scale) */
static void pic_moves(int32_t y0)
{
    float w = dr_wander();
    int wa = proj.par[P_WANDER], mv = proj.par[P_SHIFT_MOVE] - DR_SHIFT_MID;
    int on = ui.g_amt[DR_G_SHIFT] > 0.0f || proj.par[P_GL_0 + DR_G_SHIFT];
    int32_t cy = y0 + 82, dy = (int32_t)(w * 3.0f + (w >= 0.0f ? 0.5f : -0.5f));   /* 3 px a semitone */
    int32_t my = (int32_t)((float)mv * 0.03f + (mv >= 0 ? 0.5f : -0.5f));         /* ... and for Shift */
    char t[16];
    cv_text(12, y0 + 3, &FONT_S, "SHIFT", on ? K_LFO : K_DIM);
    dr_param_text(P_SHIFT_MOVE, proj.par[P_SHIFT_MOVE], t);
    cv_text(12, y0 + 22, &FONT_XS, t, K_TEXT2);
    cv_rect(20, cy, 70, 1, K_GRID);                             /* home */
    cv_rect(20, cy - 21, 70, 1, K_FAINT);                       /* a fifth up */
    cv_rect(20, cy + 21, 70, 1, K_FAINT);                       /* ... and down */
    cv_rect(40, cy - my - 1, 30, 3, on ? K_LFO : K_TEXT2);
    cv_text(12, y0 + 112, &FONT_XS, "key E5", K_DIM);
    cv_text(118, y0 + 3, &FONT_S, "WANDER", wa ? K_DRONE[1] : K_DIM);
    cv_text(118, y0 + 22, &FONT_XS, wa ? DR_WSCALE_NAME[(unsigned)proj.par[P_WSCALE] < DR_NWSCALE ? proj.par[P_WSCALE] : 0]
                                       : "0: drone II still", K_DIM);
    cv_rect(126, cy, 90, 1, K_GRID);                            /* home */
    cv_rect(126, cy - 36, 90, 1, K_FAINT);                      /* an octave up */
    cv_rect(126, cy + 36, 90, 1, K_FAINT);                      /* ... and down */
    cv_round(167, cy - dy - 4, 9, 9, 4, wa || w != 0.0f ? K_DRONE[1] : K_FAINT);
    if (w > -0.05f && w < 0.05f) {
        scopy(t, "home");
    } else {
        char *q = t;
        *q++ = w > 0.0f ? '+' : '-';
        q = dec1(w > 0.0f ? w : -w, q);
        while (*q)
            q++;
        scopy(q, " st");
    }
    if (wa || w != 0.0f)                       /* off and home: "0: drone II still" says it (no overlap) */
        cv_text(222 - text_w(&FONT_XS, t), y0 + 34, &FONT_XS, t, K_TEXT2);   /* under the scale's name */
}

static void draw_pic(void)
{
    int msg = plat_ms() < ui.msg_until;
    uint32_t h = hash(hash(2166136261u, (uint32_t)msg | ui.view << 2 | (uint32_t)keymap_on() << 8),
                      msg ? ui.msg_until : 0);
    if (h == ui.sig[1] && ui.frame % SCOPE_EVERY)
        return;                                /* the waveforms move: redrawn ~20 times a second */
    ui.sig[1] = h;
    cv_begin(240, PIC_H, K_BG);
    if (keymap_on()) {
        keymap_pic();
    } else if (ui.view == V_PULSE || ui.view == V_LFO || ui.view == V_LFO2 || ui.view == V_FX || ui.view == V_FX2 ||
        ui.view == V_SETUP || ui.view == V_SETUP2 || ui.view == V_TONE2 ||
        ui.view == V_MOVES) {
        cv_round(6, 4, WIDE_W, PAN_H, 4, K_PANEL);
        switch (ui.view) {
        case V_PULSE: pic_pulse(4); break;
        case V_LFO: pic_lfo(4); break;
        case V_LFO2: pic_lfo2(4); break;
        case V_FX: pic_fx(4); break;
        case V_FX2: pic_fx2(4); break;
        case V_TONE2: pic_tone2(4); break;
        case V_SETUP2: pic_setup2(4); break;
        case V_MOVES: pic_moves(4); break;
        default: pic_setup(4); break;
        }
    } else {
        int g;
        for (g = 0; g < DR_NGROUPS; g++) {
            int32_t x0 = pan_x(g);
            switch (ui.view) {
            case V_PLAY2: case V_TONE: case V_OSC: case V_PARTNER:
                cv_round(x0, 4, PAN_W, PAN_H, 4, K_PANEL);
                cv_text(x0 + 5, 7, &FONT_S, g ? "II" : "I", K_DRONE[g]);
                break;
            default:
                draw_scope(g, x0, 4, PAN_W, PAN_H);
            }
            switch (ui.view) {
            case V_PLAY2: pic_play2(g, x0, 4); break;
            case V_TONE: pic_tone(g, x0, 4); break;
            case V_OSC: pic_osc(g, x0, 4); break;
            case V_PARTNER: pic_partner(g, x0, 4); break;
            default: break;
            }
        }
    }
    if (msg) {                                 /* a banner across the top of the picture */
        uint16_t c = ui.msg_col ? ui.msg_col : K_GOLD;
        int32_t x;
        cv_round(6, 6, 228, 26, 6, K_CARD);
        cv_frame(6, 6, 228, 26, c);
        x = 120 - (text_w(&FONT_B, ui.msg[0]) + (ui.msg[1][0] ? 8 + text_w(&FONT_B, ui.msg[1]) : 0)) / 2;
        x = cv_text(x, 11, &FONT_B, ui.msg[0], c);
        if (ui.msg[1][0])
            cv_text(x + 8, 11, &FONT_B, ui.msg[1], K_TEXT);
    }
    cv_blit(0, PIC_Y);
}

/* a value as large as it goes: its figures in the big face, its letters ("17s", "x1.1") in the next, on one line,
 * centred on cx (the big face has no lower case) */
static void big_value(int32_t cx, int32_t y, const char *s, uint16_t c)
{
    char run[24];
    int32_t w = 0, x, pass, n;
    const char *q;
    if (s[0] == 'O' && s[1] == 'f' && s[2] == 'f' && !s[3])
        s = "OFF";                             /* (a big O and a small "ff" read as "0ff": QA 2026-10-20) */
    for (pass = 0; pass < 2; pass++) {
        x = cx - w / 2;
        w = 0;
        for (q = s; *q;) {
            int low = *q >= 'a' && *q <= 'z';
            const felucca_font_t *f = low ? &FONT_B : &FONT_M;
            for (n = 0; q[n] && (q[n] >= 'a' && q[n] <= 'z') == low && n < 23; n++)
                run[n] = q[n];
            run[n] = 0;
            q += n;
            if (pass)
                cv_text(x + w, low ? y + (int32_t)(FONT_M.h - FONT_B.h) - 2 : y, f, run, c);
            w += text_w(f, run);
        }
    }
}

/* the page's four values: KNOB 1-2 under drone I, KNOB 3-4 under drone II. At rest, quiet; while a knob
 * moves (and a moment after), its value lights: a card in its colour, the value large, its bar */
static void draw_values(void)
{
    uint32_t h = hash(2166136261u, ui.view | (uint32_t)proj.par[P_PLINK] << 8), i;
    for (i = 0; i < 4; i++) {
        int p = knob_param((int)i);
        h = hash(h, (p < 0 ? 0xFFFFu : (uint32_t)(uint16_t)proj.par[p]) | (uint32_t)lit((int)i) << 16 |
                        (uint32_t)(ui.btn >> B_SEL & 1u) << 17 | (uint32_t)(proj.par[P_PLOCK] != 0) << 18);
    }
    if (keymap_on()) {                         /* SEL held: the map's last two rows here */
        h = hash(h, 0x4B4Du | (uint32_t)(uint8_t)ui.km_key << 16);
        for (i = 0; i < 11u; i++)
            h = hash(h, (uint32_t)sw_get(sw_layer()[i]) | (uint32_t)proj.par[P_LFO_LOCK] << 4);
        if (h == ui.sig[2])
            return;
        ui.sig[2] = h;
        cv_begin(240, VAL_H, K_BG);
        keymap_values();
        cv_blit(0, VAL_Y);
        return;
    }
    if (plat_ms() < ui.glab_until) {           /* SELECT / ALGORITHM / PRESETS turning: its name and value, big, the
                                                * whole row (the review: a top-bar tag was too small to play by) */
        const char *q;
        h = hash(h, 0x474Cu);
        for (q = ui.glab[0]; *q; q++)
            h = hash(h, (uint32_t)*q);
        for (q = ui.glab[1]; *q; q++)
            h = hash(h, (uint32_t)*q << 8);
        if (h == ui.sig[2])
            return;
        ui.sig[2] = h;
        cv_begin(240, VAL_H, K_BG);
        cv_rect(0, 0, 240, 1, K_GRID);
        cv_round(6, 3, WIDE_W, 79, 6, K_CARD);
        cv_frame(6, 3, WIDE_W, 79, K_LFO);
        text_c(120, 9, &FONT_S, ui.glab[0], K_LFO);
        big_value(120, 32, ui.glab[1], K_TEXT);
        cv_blit(0, VAL_Y);
        return;
    }
    if (h == ui.sig[2])
        return;
    ui.sig[2] = h;
    cv_begin(240, VAL_H, K_BG);
    cv_rect(0, 0, 240, 1, K_GRID);
    cv_rect(120, 8, 1, 64, K_FAINT);           /* drone I | drone II */
    for (i = 0; i < 4; i++) {
        int p = knob_param((int)i), lo, hi, v, on = lit((int)i);
        knob_t k = VIEW_KNOB[ui.view][i];
        int32_t x = 6 + (int32_t)i * 58 + (i >= 2 ? 2 : 0), w = 54, cx = x + w / 2, bw;
        uint16_t accent = k.g < G ? K_DRONE[k.g] : k.p == P_XMOD_AMT ? K_DRONE[1] : page_col() == K_DIM ? K_TEXT : page_col();
        int is_pitch = k.p == GP_PITCH && k.g < G, locked = is_pitch && proj.par[P_PLOCK] && on;
        char t[16];
        if (p < 0)
            continue;
        if (locked)                            /* PITCH LOCK: a turned pitch knob does nothing, so its card */
            on = 0;                            /* stays dark (the banner says why) */
        lo = dr_param_info(p)->lo;
        hi = dr_param_info(p)->hi;
        v = proj.par[p];
        if (on) {
            cv_round(x - 2, 3, w + 4, 79, 6, K_CARD);
            cv_frame(x - 2, 3, w + 4, 79, accent);
        }
        text_fit(cx, 5, w + 2, &FONT_S, card_name(p), on ? accent : K_DIM);
        if (p == P_PLINK_OFF) {                /* the link offset as the pitch knobs: the nearest semitone and the
                                                * cents from it, "+8 st +20" (Evan, 2026-10-21) */
            int a = v < 0 ? -v : v, n = (a + 50) / 100, d = (v < 0 ? -1 : 1) * (a - 100 * n);
            char *q = t;
            *q++ = v < 0 ? '-' : '+';
            itoa_u((uint32_t)n, q);
            while (*q)
                q++;
            *q++ = ' ';
            *q++ = 's';
            *q++ = 't';
            if (d) {
                *q++ = ' ';
                *q++ = d > 0 ? '+' : '-';
                itoa_u((uint32_t)(d > 0 ? d : -d), q);
                while (*q)
                    q++;
            }
            *q = 0;
        } else {
            dr_param_text(p, v, t);
        }
        {   /* the value: large while lit, else as large as fits */
            const felucca_font_t *f = fit_face(t, w + 2, on);
            text_fit(cx, f == &FONT_M ? 27 : f == &FONT_XS ? 34 : 30, w + 2, f, t, on ? K_TEXT : K_TEXT2);
        }
        if (is_pitch) {                        /* a pitch: no bar; while it moves, how to fine-tune */
            if (on)
                text_c(cx, 56, &FONT_XS, ui.btn >> B_SEL & 1u ? "FINE" : "SEL: fine", K_LFO);
            else if (locked)
                text_c(cx, 56, &FONT_XS, "locked", K_DIM);
            continue;
        }
        bw = hi > lo ? (int32_t)(42 * (v - lo) / (hi - lo)) : 0;
        cv_round(cx - 21, 62, 42, 4, 2, K_GRID);
        if (hi - lo == 1) {                    /* two ways (White / Pink, Equal / Just): the half it's on */
            cv_round(cx - 21 + (v > lo ? 22 : 0), 62, 20, 4, 2, on ? accent : blend(K_GRID, accent, 1, 2));
        } else if (lo < 0) {                   /* centred values: a bar from the middle */
            int32_t m = cx - 21 + 42 * (0 - lo) / (hi - lo), e = cx - 21 + bw;
            cv_round(e < m ? e : m, 62, (e < m ? m - e : e - m) + 3, 4, 2, on ? accent : blend(K_GRID, accent, 1, 2));
        } else if (bw) {
            cv_round(cx - 21, 62, bw < 4 ? 4 : bw, 4, 2, on ? accent : blend(K_GRID, accent, 1, 2));
        }
    }
    cv_blit(0, VAL_Y);
}

/* a wave -1..1 as a light's level: dark at its bottom, lit at its top */
static uint8_t wave_lvl(float w) { return (uint8_t)(fm_clampf(0.5f + 0.5f * w, 0.0f, 1.0f) * 255.0f + 0.5f); }

static void leds(void)
{
    uint8_t b[NB] = {0}, k[NKEYS] = {0};
    uint32_t i;
    for (i = 0; i < NPAGE_BTN; i++)                  /* the button of the page shown */
        if (on_pages((int)i))
            b[PAGE_BTN[i].btn] = 255;
    if (ui.playing)
        b[B_PLAY] = 255;
    if (ui.dirty)                                      /* SAVE: a dim glow while something isn't saved yet */
        b[B_SAVE] = 90;
    if (proj.par[P_LFO_W1])                            /* OCT- / OCT+: each wave, pulsing with it */
        b[B_OCTDN] = wave_lvl(dr_lfo[0]);
    if (proj.par[P_LFO_W2])
        b[B_OCTUP] = wave_lvl(dr_lfo[1]);
    if (proj.par[P_PULSE]) {                           /* ARP: breathing with the swell, a beat a breath */
        float b1 = 1.0f + (float)proj.par[P_PULSE_SWING] * 0.01f, ph = dr_pulse_ph;
        float u = ph < b1 ? ph / b1 : (ph - b1) / (2.0f - b1);
        b[B_ARP] = wave_lvl(fm_cosf(6.2831853f * u));
    }
    if (ui.loop == L_PLAY || (ui.loop == L_REC && (ui.frame / 8u) & 1u))
        b[B_REC] = 255;                                /* REC: blinking recording, steady looping */
    for (i = 0; i < 11u; i++) {                      /* the black keys: the switches that are on */
        sw_t w = sw_layer()[i];
        int v = w.s == S_LFORESET ? (int)(ui.keys >> BLACK_K[i] & 1u) || proj.par[P_LFO_LOCK] != 0 : sw_get(w);
        if (v == 2)                                    /* a third state (both LFO waves): lit, winking */
            v = plat_ms() % 1200u < 1000u;
        if (v)
            k[BLACK_K[i]] = 255;
    }
    for (i = 0; i < NSCENES; i++)                    /* the scene keys: the bank's slots that hold a scene; the */
        if (scene_get(proj.bank, (int)i) &&            /* one the sound is, blinking slowly */
            (ui.scene_at != (int8_t)i || plat_ms() % 1000u < 600u))
            k[WHITE_K[i]] = 255;
    if (ui.press >= 0)                                 /* the key pressed into: as bright as how far in */
        k[WHITE_K[ui.press]] = (uint8_t)(40.0f + 215.0f * ui.press_x);
    if (ui.flash_t && plat_ms() - ui.flash_t < 480u && ui.scene_at >= 0)   /* taken: three quick flashes */
        k[WHITE_K[ui.scene_at]] = (plat_ms() - ui.flash_t) % 160u < 80u ? 255 : 0;
    else if (ui.flash_t && ui.press >= 0 && plat_ms() - ui.flash_t < 480u)
        k[WHITE_K[ui.press]] = (plat_ms() - ui.flash_t) % 160u < 80u ? 255 : 0;
    for (i = 0; i < 4u; i++)                         /* the gestures: a dim glow, lit as far as each is in */
        k[WHITE_K[NSCENES + i]] = (uint8_t)(40.0f + 215.0f * ui.g_amt[i]);
    plat_leds(b, k);
}

/* AUTOSAVE: a few seconds after the last change, while the drone is stopped and nothing is touched. "The last
 * change", not the last touch: a scene still gliding (or the loop playing) changes values every few frames, and
 * each would be a flash write (Evan's FM-1, 2026-10-19: a long scene switch wrote 660 times in 20 s, the lights
 * flickering with each). And never more often than AUTOSAVE_EVERY, whatever happens, nor again soon after a fail */
#define AUTOSAVE_QUIET 4000u
#define AUTOSAVE_EVERY 30000u
static void autosave(void)
{
    uint32_t now = plat_ms();
    if (!ui.dirty || ui.playing || dr_fade > 0.0f || ui.btn || ui.keys || now - ui.act_t < AUTOSAVE_QUIET ||
        now - ui.chg_t < AUTOSAVE_QUIET || ui.morph >= 0 || ui.press >= 0 ||
        (ui.saved_t && now - ui.saved_t < AUTOSAVE_EVERY))
        return;
    ui.saved_t = now;
    if (project_save() == 0)
        ui.dirty = 0;
}

void ui_init(void)
{
    memset(&ui, 0, sizeof ui);
    ui.hold_btn = -1;
    ui.morph = -1;
    ui.visit = -1;
    ui.press = -1;
    ui.scene_at = -1;
    ui.km_key = -1;
    ui.jack = dr_phones = (uint8_t)plat_headphones();
    ui.save_q = -2;
    ui.splash_t0 = plat_ms();
    lcd_fill(0, 0, 240, 240, K_BG);
}

/* ------------------------------------------------------------- the start --- */
/* As the web page: a red field, two thick bands (violet, and one that darkens what it crosses) drifting in from
 * the right and settling across the name, which turns pale pink where they cross it; it holds still for half a
 * second where it lands, then fades to the Play page slowly. About 3.5 s (the last 1.3 s the fade); any button
 * or key ends it at once (and does what it does). The drones stay silent until PLAY. Not in the host
 * simulator's scenarios, which start at once (the browser emulator has it; OM_SPLASH shows it on the host). */
#if defined(OM_HOST) && !defined(OM_WEB) && !defined(OM_SPLASH)
#define SPLASH_MS 0u
#else
#define SPLASH_MS 3500u
#endif
#define SPLASH_HOLD_AT 1700u                  /* where it lands: held still from here (Evan: half a second) */
#define SPLASH_HOLD 500u
#define SP_RED RGB(254, 0, 0)
#define SP_VIOLET RGB(118, 5, 149)
#define SP_DARK_RED RGB(58, 0, 0)             /* the dark band over the red (multiplied, as on the page) */
#define SP_DARK_VIOLET RGB(27, 0, 49)         /* ... over the violet */
#define SP_INK RGB(20, 0, 15)
#define SP_PALE RGB(255, 214, 240)
static int16_t sp_l[2][240], sp_r[2][240];    /* each band's edges, row by row */

static int sp_in(int32_t x, int32_t y)
{
    return (x >= sp_l[0][y] && x < sp_r[0][y]) || (x >= sp_l[1][y] && x < sp_r[1][y]);
}
static float sp_ease(float x) { x = fm_clampf(x, 0.0f, 1.0f); return 1.0f - (1.0f - x) * (1.0f - x); }

/* text whose colour depends on where it is: near-black on the red, pale over a band; q of 16 faded in */
static void sp_text(int32_t x, int32_t y, int32_t y0, const felucca_font_t *f, const char *s, int q, uint16_t out_c,
                    uint16_t in_c)
{
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), gx, gy, w = f->bw[gi], bpr = (w + 1u) / 2u;
        const uint8_t *gd = f->data + f->off[gi];
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++) {
                uint32_t a = gd[gy * bpr + gx / 2u];
                int32_t px = x - f->pad + (int32_t)gx, sy = y + (int32_t)gy, py = sy - y0;
                a = (gx & 1u) ? (a & 15u) : (a >> 4);
                a = a * (uint32_t)q / 16u;
                if (!a || (uint32_t)px >= cv_w || py < 0 || py >= (int32_t)cv_h || sy < 0 || sy >= 240)
                    continue;
                {
                    uint16_t c = sp_in(px, sy) ? in_c : out_c, bg = swap16(cv_px[(uint32_t)py * cv_w + (uint32_t)px]);
                    cv_px[(uint32_t)py * cv_w + (uint32_t)px] = swap16(blend(bg, c, (int)a, 15));
                }
            }
        x += f->adv[gi];
    }
}

static void draw_splash(uint32_t t0)
{
    /* its own time stops for the hold: everything after it is as before, half a second later */
    const uint32_t t = t0 < SPLASH_HOLD_AT ? t0 : t0 < SPLASH_HOLD_AT + SPLASH_HOLD ? SPLASH_HOLD_AT : t0 - SPLASH_HOLD;
    const float tt = (float)t * 0.001f;
    const float in = sp_ease(tt / 1.4f), fade = fm_clampf(tt / 0.4f, 0.0f, 1.0f),
                out = fm_clampf(((float)t - (float)(SPLASH_MS - SPLASH_HOLD) + 1300.0f) / 1300.0f, 0.0f, 1.0f);
    const int lit = (int)(16.0f * fade * (1.0f - out) + 0.5f);        /* the whole picture, from black and back */
    const int tq = (int)(16.0f * fm_clampf((tt - 0.4f) / 0.8f, 0.0f, 1.0f) + 0.5f);   /* the name */
    const int32_t rise = (int32_t)(8.0f * (1.0f - sp_ease((tt - 0.4f) / 0.8f)));
    static const float LEN[2] = {0.9f, 0.8f}, PH[2] = {0.0f, 0.5f};
    static char ver[40];                                             /* "niagara-drift 0.1" (the build's version) */
    int32_t y, half, b;
    if (!ver[0]) {
        char *q = scopy(ver, "niagara-drift ");
        const char *v = OM_VERSION;
        for (; *v && q < ver + sizeof ver - 1; v++)
            *q++ = (char)(*v >= 'A' && *v <= 'Z' ? *v + 32 : *v);
        *q = 0;
    }
    for (b = 0; b < 2; b++)                                          /* the bands: from off the right, settling */
        for (y = 0; y < 240; y++) {
            float a = 6.2831853f * (float)y / (240.0f * LEN[b]) + PH[b] + tt * 0.6f;
            float cx = 240.0f * (1.15f - 0.6f * in + 0.12f * fm_sinf(a));
            float hw = 240.0f * 0.065f * (1.0f + 0.2f * fm_sinf(a * 1.4f + PH[b]));
            sp_l[b][y] = (int16_t)fm_clampf(cx - hw, -1.0f, 241.0f);
            sp_r[b][y] = (int16_t)fm_clampf(cx + hw, -1.0f, 241.0f);
        }
    for (half = 0; half < 2; half++) {                               /* the screen in two halves (the canvas) */
        const int32_t y0 = half * 120;
        cv_begin(240, 120, K_BG);
        for (y = 0; y < 120; y++) {
            int32_t sy = y0 + y, ol = sp_l[0][sy] > sp_l[1][sy] ? sp_l[0][sy] : sp_l[1][sy],
                    orr = sp_r[0][sy] < sp_r[1][sy] ? sp_r[0][sy] : sp_r[1][sy];
            cv_rect(0, y, 240, 1, SP_RED);
            cv_rect(sp_l[0][sy], y, sp_r[0][sy] - sp_l[0][sy], 1, SP_VIOLET);
            cv_rect(sp_l[1][sy], y, sp_r[1][sy] - sp_l[1][sy], 1, SP_DARK_RED);
            if (orr > ol)
                cv_rect(ol, y, orr - ol, 1, SP_DARK_VIOLET);
        }
        sp_text(14, 64 + rise, y0, &FONT_L, "NIAGARA", tq, SP_INK, SP_PALE);
        sp_text(14, 98 + rise, y0, &FONT_L, "DRIFT", tq, SP_INK, SP_PALE);
        sp_text(14, 214, y0, &FONT_S, ver, tq, SP_INK, SP_PALE);
        if (lit < 16) {                                              /* from black, and back to it */
            uint32_t i, n = cv_w * cv_h;
            for (i = 0; i < n; i++)
                cv_px[i] = swap16(blend(K_BG, swap16(cv_px[i]), lit, 16));
        }
        cv_blit(0, (uint32_t)y0);
    }
}

void ui_frame(void)
{
    input();
    press_run();
    gesture_run();
    arp_run();
    scene_morph();
    autosave();
    if (!ui.splash_done) {
        uint32_t t = plat_ms() - ui.splash_t0;
        if (ui.btn || ui.keys || t >= SPLASH_MS) {                    /* over, or a hand on the FM-1 */
            ui.splash_done = 1;
            ui.sig[0] = ui.sig[1] = ui.sig[2] = 0;                   /* every band drawn afresh */
            lcd_fill(0, 0, 240, 240, K_BG);
        } else {
            draw_splash(t);
            leds();
            ui.frame++;
            return;
        }
    }
    draw_bar();
    draw_pic();
    draw_values();
    leds();
    ui.frame++;
}

void ui_input_only(void) { input(); }
