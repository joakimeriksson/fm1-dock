/* SPDX-License-Identifier: GPL-3.0-only */
/* HX's DEVICES (BLE_CENTRAL 3, MINIMAL: no pairing): the FM-1 connects out to a BLE-MIDI device - an SMK-37 or another
 * FM-1 running fm1-dock, a controller - as Optimist's io/midi/ble_devices.c and ble_connect.c do (w0ts/optimist,
 * GPL-3.0-only; docs/BLE-DEVICES-DESIGN.md there), cut to what fm1-dock needs: one remembered device (LAST), no
 * pairing, no bonds, no private addresses. Part of hx.c (included there). Main-loop code (hx_tick and the host's calls);
 * the stack's calls with the BLE interrupts held, as hx_set's.
 *   the list    while the host's DEVICES page is open (hx_scan) the radio scans: NONE, LAST (when there is one), then
 *               the BLE-MIDI devices heard nearby;
 *   a pick      connects to that device: 10 s to find it ("NOT FOUND"), a link that is not made or lost early made
 *               again up to RC_TRIES times; ready, it becomes LAST and the choice (the host saves the record, hx_rec);
 *   LAST        while BLUETOOTH is ON, LAST is the choice, the page is closed and no link of either role is up: searched
 *               for (initiating 2 s, advertising 1 s, for 30 s; then 1 s every 10 s). A computer connecting to the FM-1
 *               meanwhile is accepted as always (the search waits);
 *   NONE        our central link and the search end; the FM-1 stays visible to computers. */

enum { RC_OFF, RC_WAIT, RC_TRY, RC_PICK, RC_LINK, RC_HELD };
#define RC_FAST_MS 30000u
#define RC_TRY_FAST_MS 2000u
#define RC_GAP_FAST_MS 1000u
#define RC_TRY_SLOW_MS 1000u
#define RC_GAP_SLOW_MS 9000u
#define RC_PICK_MS 10000u
#define RC_TRIES 6u
#define RC_RETRY_MS 2000u

static struct ble_dev_store dstore;               /* LAST and the choice (the host keeps its octets: hx_rec) */
static uint8_t dstore_new;                        /* changed: the host saves it */
static struct ble_scan_tab found;
static uint8_t devs_open;
static uint32_t age_ms;
static struct {
    uint8_t phase, to_last, was_ready, failed, tries;
    uint8_t addr[6], addr_rand;
    uint32_t t_end, next_try, search_t0;
    struct ble_found pick;
    char msg[32];
} brc;

static uint32_t now_ms(void) { return hx_ms; }
static void txt(char *out, uint32_t room, const char *a, const char *b)
{
    uint32_t n = 0;
    while (a && *a && n + 1u < room)
        out[n++] = *a++;
    while (b && *b && n + 1u < room)
        out[n++] = *b++;
    out[n] = 0;
}
static void dstore_changed(void) { dstore_new = 1; }
static int is_last(const uint8_t a[6], uint8_t rnd)
{
    const struct ble_dev *d = &dstore.dev;
    return ble_store_has_last(&dstore) && (d->info & BLE_DEV_RANDOM ? 1u : 0u) == rnd && ble_eq(d->addr, a, 6);
}
static void rc_failed(const char *a, const char *b)
{
    txt(brc.msg, sizeof brc.msg, a, b);
    brc.failed = 1;
}
static void rc_name(char out[BLE_NAME_MAX + 1u])
{
    if (brc.to_last)
        ble_store_name(&dstore, out);
    else if (brc.pick.name[0])
        ble_cpy((uint8_t *)out, (const uint8_t *)brc.pick.name, BLE_NAME_MAX + 1u);
    else
        ble_addr_text(brc.pick.addr, out);
}
static int rc_connect(const uint8_t a[6], uint8_t rnd)
{
    struct ble_peer p;
    int ok;
    ble_zero((uint8_t *)&p, sizeof p);
    ble_cpy(p.addr, a, 6);
    p.addr_rand = rnd;
    ble_cpy(brc.addr, a, 6);
    brc.addr_rand = rnd;
    brc.was_ready = 0;
    fm1_ble_irqs_hold(1);
    ok = ble_central_connect(&p);
    fm1_ble_irqs_hold(0);
    brc.tries++;
    return ok;
}
static void rc_stop(void)                         /* initiating for the search or a pick: back to advertising */
{
    fm1_ble_irqs_hold(1);
    if (ble_ll_initiating())
        ble_central_cancel();
    fm1_ble_irqs_hold(0);
}
static int rc_retry(void)
{
    if (brc.tries >= RC_TRIES || !rc_connect(brc.addr, brc.addr_rand))
        return 0;
    brc.t_end = now_ms() + RC_RETRY_MS;
    brc.phase = brc.search_t0 ? RC_TRY : RC_PICK;
    return 1;
}
static void rc_wait(void)
{
    brc.next_try = now_ms() + (now_ms() - brc.search_t0 < RC_FAST_MS ? RC_GAP_FAST_MS : RC_GAP_SLOW_MS);
    brc.phase = RC_WAIT;
}
static void rc_try(void)
{
    const struct ble_dev *d = &dstore.dev;
    brc.to_last = 1;
    brc.tries = 0;
    brc.t_end = now_ms() + (now_ms() - brc.search_t0 < RC_FAST_MS ? RC_TRY_FAST_MS : RC_TRY_SLOW_MS);
    if (rc_connect(d->addr, (uint8_t)(d->info & BLE_DEV_RANDOM ? 1u : 0u)))
        brc.phase = RC_TRY;
    else
        rc_wait();
}
static const char *rc_why(uint8_t f)
{
    switch (f) {
    case BLE_CF_NO_MIDI: return "NO MIDI";
    case BLE_CF_NEED_PAIR: return "NEEDS PAIRING";
    case BLE_CF_GATT: return "GATT ERROR";
    default: return ble_central_code() == 0x3Eu ? "NO LINK" : "LINK LOST";
    }
}
static void rc_ready(void)
{
    brc.was_ready = 1;
    brc.failed = 0;
    if (!brc.to_last) {
        ble_store_set_last(&dstore, brc.pick.addr, brc.pick.addr_rand, brc.pick.name, brc.pick.kind);
        brc.to_last = 1;
    }
    ble_store_select(&dstore, BLE_SEL_LAST);
    dstore_changed();
}
static void rc_link(void)
{
    uint8_t st = ble_central_state(), f;
    char nm[BLE_NAME_MAX + 1u];
    if (st == BLE_CS_READY && !brc.was_ready)
        rc_ready();
    if (st != BLE_CS_IDLE)
        return;
    rc_name(nm);
    if (brc.was_ready) {
        rc_failed("LOST ", nm);
        brc.phase = RC_OFF;                       /* (LAST chosen: a new search starts) */
        return;
    }
    f = ble_central_fail();
    if (f == BLE_CF_LOST && rc_retry())
        return;
    rc_failed("FAILED: ", rc_why(f));
    if (f == BLE_CF_NEED_PAIR)
        brc.phase = RC_HELD;
    else if (brc.search_t0)
        rc_wait();
    else
        brc.phase = RC_OFF;
}
static void rc_pick(const struct ble_found *e)
{
    if (!on || !up)
        return;
    if (ble_connected() && !ble_ll_central()) {
        rc_failed("BUSY: ", "A HOST IS CONNECTED");
        return;
    }
    rc_stop();
    if (ble_ll_central()) {                       /* (one link: the device we are on goes first) */
        fm1_ble_irqs_hold(1);
        ble_central_cancel();
        fm1_ble_irqs_hold(0);
    }
    brc.pick = *e;
    brc.to_last = (uint8_t)is_last(e->addr, e->addr_rand);
    brc.search_t0 = 0;
    brc.failed = brc.tries = 0;
    brc.t_end = now_ms() + RC_PICK_MS;
    brc.phase = RC_PICK;
    if (ble_ll_central() || !rc_connect(e->addr, e->addr_rand))
        brc.was_ready = 2;                        /* (the old link still closing: tried again in the poll) */
}
static void rc_none(void)
{
    if (up && (ble_ll_central() || ble_ll_initiating())) {
        fm1_ble_irqs_hold(1);
        ble_central_cancel();
        fm1_ble_irqs_hold(0);
    }
    brc.failed = 0;
    brc.phase = RC_OFF;
}
static void rc_poll(void)
{
    int want = on && up && ble_store_has_last(&dstore) && dstore.sel == BLE_SEL_LAST && !devs_open;
    switch (brc.phase) {
    case RC_OFF:
        if (want && !ble_connected() && !ble_ll_initiating()) {
            brc.search_t0 = now_ms() | 1u;
            brc.next_try = now_ms();
            brc.phase = RC_WAIT;
        }
        return;
    case RC_HELD:
        if (!on) {
            brc.failed = 0;
            brc.phase = RC_OFF;
        }
        return;
    case RC_WAIT:
        if (!want)
            brc.phase = RC_OFF;
        else if (!ble_connected() && (int32_t)(now_ms() - brc.next_try) >= 0)
            rc_try();
        return;
    case RC_TRY:
    case RC_PICK:
        if (brc.was_ready == 2u) {
            if (!ble_ll_central() && rc_connect(brc.pick.addr, brc.pick.addr_rand))
                brc.was_ready = 0;
            else if ((int32_t)(now_ms() - brc.t_end) >= 0) {
                rc_failed("FAILED: ", "BUSY");
                brc.phase = RC_OFF;
            }
            return;
        }
        if (ble_central_state() == BLE_CS_SETUP || ble_central_state() == BLE_CS_READY ||
            (!ble_ll_initiating() && ble_central_fail() != BLE_CF_NONE)) {
            if (brc.phase == RC_PICK)
                brc.search_t0 = 0;
            brc.phase = RC_LINK;
            rc_link();
            return;
        }
        if (brc.phase == RC_TRY && !want) {
            rc_stop();
            brc.phase = RC_OFF;
        } else if ((int32_t)(now_ms() - brc.t_end) >= 0 || !ble_ll_initiating()) {
            rc_stop();
            if (brc.phase == RC_PICK) {
                if (brc.tries > 1u && rc_retry())
                    return;
                rc_failed("FAILED: ", brc.tries > 1u ? "LINK LOST" : "NOT FOUND");
                brc.phase = RC_OFF;
            } else
                rc_wait();
        }
        return;
    default:                                      /* RC_LINK */
        rc_link();
        return;
    }
}

/* ---- the host's side (hx.h) */
static void hx_tick(void)                         /* the main loop: the scan's reports into the list, connecting out */
{
    uint8_t pdu[2 + 37], n;
    uint16_t rssi;
    uint32_t k;
    if (!up)
        return;
    for (k = 0; k < 16u && (n = ble_ll_scan_take(pdu, &rssi)) != 0; k++)
        ble_scan_add(&found, pdu, n, rssi, now_ms());
    if (now_ms() - age_ms >= 1000u) {
        age_ms = now_ms();
        ble_scan_age(&found, now_ms());
    }
    rc_poll();
}

static void hx_scan(uint32_t open)                /* the DEVICES page open: scan (and no search for LAST) */
{
    devs_open = (uint8_t)(open != 0u);
    if (!up)
        return;
    if (open)
        ble_scan_clear(&found);
    fm1_ble_irqs_hold(1);
    ble_ll_scan((int)devs_open);
    fm1_ble_irqs_hold(0);
}

/* the list's rows: 0 NONE, then LAST when there is one, then the devices heard nearby (LAST left out) -> how many */
static uint32_t rows(uint8_t near[BLE_SCAN_N], uint32_t *n_near, int *last)
{
    uint32_t i, k = 0, m = ble_scan_list(&found, near);
    *last = ble_store_has_last(&dstore) ? 1 : -1;
    for (i = 0; i < m; i++)
        if (!is_last(found.e[near[i]].addr, found.e[near[i]].addr_rand))
            near[k++] = near[i];
    *n_near = k;
    return 1u + (*last > 0 ? 1u : 0u) + k;
}

static uint32_t hx_list(hx_dev_t *o, uint32_t max)
{
    uint8_t near[BLE_SCAN_N];
    uint32_t n_near, n, r, i;
    int last;
    n = rows(near, &n_near, &last);
    for (r = 0; r < n && r < max; r++) {
        hx_dev_t *d = &o[r];
        ble_zero((uint8_t *)d, sizeof *d);
        if (r == 0) {
            txt(d->name, sizeof d->name, "NONE", 0);
            d->chosen = dstore.sel == BLE_SEL_NONE;
            continue;
        }
        if ((int)r == last) {
            ble_store_name(&dstore, d->name);
            d->chosen = dstore.sel == BLE_SEL_LAST;
            d->tag = brc.to_last && brc.was_ready == 1u && ble_central_state() == BLE_CS_READY ? HX_TAG_CONNECTED :
                     brc.to_last && (brc.phase == RC_PICK || brc.phase == RC_TRY) ? HX_TAG_CONNECTING : HX_TAG_LAST;
            for (i = 0; i < BLE_SCAN_N; i++)
                if (found.e[i].used && found.e[i].midi && is_last(found.e[i].addr, found.e[i].addr_rand))
                    d->bars = (uint8_t)ble_scan_bars(&found, i);
            continue;
        }
        {
            const struct ble_found *e = &found.e[near[r - 1u - (last > 0 ? 1u : 0u)]];
            txt(d->name, sizeof d->name, e->name[0] ? e->name : "?", 0);
            if (!e->name[0])
                ble_addr_text(e->addr, d->name);
            d->bars = (uint8_t)ble_scan_bars(&found, (uint32_t)(e - found.e));
            d->tag = brc.phase == RC_PICK && !brc.to_last && brc.pick.addr_rand == e->addr_rand &&
                     ble_eq(brc.pick.addr, e->addr, 6) ? HX_TAG_CONNECTING : HX_TAG_NONE;
        }
    }
    return n;
}

static void hx_pick(uint32_t row)
{
    uint8_t near[BLE_SCAN_N];
    uint32_t n_near, n, i;
    int last;
    n = rows(near, &n_near, &last);
    if (row >= n)
        return;
    if (row == 0) {                               /* NONE: visible, no connecting out */
        rc_none();
        ble_store_select(&dstore, BLE_SEL_NONE);
        dstore_changed();
        return;
    }
    if ((int)row == last) {                       /* LAST: the choice; connected to now when heard, else searched for */
        ble_store_select(&dstore, BLE_SEL_LAST);
        dstore_changed();
        brc.failed = 0;
        if (brc.phase == RC_HELD)
            brc.phase = RC_OFF;
        for (i = 0; i < BLE_SCAN_N; i++)
            if (found.e[i].used && found.e[i].midi && is_last(found.e[i].addr, found.e[i].addr_rand)) {
                rc_pick(&found.e[i]);
                return;
            }
        return;
    }
    rc_pick(&found.e[near[row - 1u - (last > 0 ? 1u : 0u)]]);
}

/* the status line: HX_CS_* (out: its text) */
static uint32_t hx_cstatus(char *out, uint32_t room)
{
    char nm[BLE_NAME_MAX + 1u];
    uint8_t st = up ? ble_central_state() : BLE_CS_IDLE;
    out[0] = 0;
    if (brc.phase == RC_PICK || (brc.phase == RC_LINK && st != BLE_CS_READY && !brc.failed) ||
        (brc.phase == RC_TRY && brc.to_last)) {
        rc_name(nm);
        txt(out, room, brc.phase == RC_TRY ? "SEARCHING " : "CONNECTING ", nm);
        return HX_CS_INFO;
    }
    if (brc.phase == RC_LINK && st == BLE_CS_READY && brc.was_ready == 1u) {
        rc_name(nm);
        txt(out, room, "CONNECTED ", nm);
        return HX_CS_GOOD;
    }
    if (brc.failed) {
        txt(out, room, brc.msg, 0);
        return HX_CS_BAD;
    }
    return HX_CS_NONE;
}

/* the device record (BLE_DEV_STORE_SIZE octets the host keeps with its settings): set at start-up (dir 1), or the
 * changed one handed over (dir 0 -> 1 when there was a change) */
static int hx_rec(uint8_t raw[HX_DEVREC], uint32_t dir)
{
    if (dir) {
        ble_store_load(&dstore, raw);
        return 1;
    }
    if (!dstore_new)
        return 0;
    dstore_new = 0;
    ble_store_save(&dstore, raw);
    return 1;
}
