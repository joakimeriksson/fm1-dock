/* SPDX-License-Identifier: GPL-3.0-only */
/* VECTOR37's FM-1 panel as fm1-dock plays it (included into panel_fm1.c by take.py, after ui.c's sounds): what the
 * SMK-37's buttons, pads and keys did that the FM-1's panel left out.
 *   SAVE tapped     the sound into its USER slot (a factory sound: the first empty one); SAVE held and PRESETS
 *                   turned: the USER slot it goes to (shown), saved as SAVE is let go
 *   EDIT + SAVE     RANDOMIZE the selected planets (held: UNDO), SAVE's job in Vector37's FM-1 panel
 *   PRESETS         every sound in turn: FACTORY 1..16, the USER sounds saved, FACTORY 2 17..32
 *   the keys        play with the velocity the host senses (Vector37's FM-1 panel: 100) */
static uint8_t v37_vel[27];                         /* each key's velocity as it came down (the module's ui()) */
static uint8_t fs_rnd;                              /* this SAVE press is EDIT + SAVE: randomize / undo */
static int8_t fs_pick = -1;                         /* the USER slot SAVE held + PRESETS picked, -1 none */

static uint32_t v37_key_vel(uint32_t k) { return k < 27u && v37_vel[k] ? v37_vel[k] : 100u; }

static void fs_say_slot(const char *what, uint32_t n)
{
    char b[28];
    if (usr_has(n))
        slot_label(b, sizeof b, BK_USER, n);
    else {
        b[0] = 'U';
        fmt_int(b + 1, (int32_t)n + 1);
        str_cat(b, " EMPTY", sizeof b);
    }
    ui_say(what, b);
}
static int32_t fs_target(void)                      /* SAVE's slot: the sound's own, or the first empty one */
{
    uint32_t i;
    if (ui.bank == BK_USER)
        return ui.preset;
    for (i = 0; i < USR_SLOTS; i++)
        if (!usr_has(i))
            return (int32_t)i;
    return -1;
}

static void fs_press(uint32_t edit)                 /* SAVE down (EDIT held or not) */
{
    fs_rnd = (uint8_t)edit;
    fs_pick = -1;
    if (edit)
        ui.btn_used[B_EDIT] = 1;
    else
        ui_say("SAVE: TAP, OR HOLD + PRESETS", 0);
}
static void fs_tap(void)                            /* SAVE let go, quickly, nothing turned */
{
    int32_t n;
    if (fs_rnd) {
        randomize();
        return;
    }
    n = fs_target();
    if (n < 0)
        ui_say("USER FULL: HOLD SAVE, TURN PRESETS", 0);
    else
        sound_save((uint32_t)n);
}
static void fs_release(void)                        /* SAVE let go after PRESETS picked a slot */
{
    if (fs_pick >= 0 && !fs_rnd)
        sound_save((uint32_t)fs_pick);
    fs_pick = -1;
}
static int fs_held_undo(void) { return fs_rnd; }   /* SAVE held 0.6 s: UNDO only as EDIT + SAVE */

static void fp_sound_turn(int32_t s)                /* PRESETS: the sounds; with SAVE held, the slot to save to */
{
    uint32_t list[NFACT + USR_SLOTS], n = 0, i, at = 0;
    char b[28];
    if (ui.btn_t[B_PARA] && !fs_rnd) {
        int32_t p = fs_pick >= 0 ? fs_pick : fs_target() >= 0 ? fs_target() : 0;
        ui.btn_used[B_PARA] = 1;
        fs_pick = (int8_t)(((p + s) % (int32_t)USR_SLOTS + (int32_t)USR_SLOTS) % (int32_t)USR_SLOTS);
        fs_say_slot("SAVE TO ", (uint32_t)fs_pick);
        return;
    }
    for (i = 0; i < NPRESET; i++)                   /* (0..31 factory, 32 + slot a USER sound) */
        list[n++] = i;
    for (i = 0; i < USR_SLOTS; i++)
        if (usr_has(i))
            list[n++] = NFACT + i;
    for (i = NPRESET; i < NFACT; i++)
        list[n++] = i;
    for (i = 0; i < n; i++)
        if (list[i] == (ui.bank == BK_USER ? NFACT + ui.preset : bank_first(ui.bank) + ui.preset))
            at = i;
    i = list[(uint32_t)(((int32_t)at + s) % (int32_t)n + (int32_t)n) % n];
    if (i >= NFACT)
        user_load(i - NFACT);
    else
        preset_load(i);
    preset_label(b, sizeof b);
    ui_say(b, 0);
}
