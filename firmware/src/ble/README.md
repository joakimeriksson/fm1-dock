# BLE-MIDI: Optimist's stack, as HX

The `ble_*` files and `rf_share.h` are [Optimist](https://github.com/w0ts/optimist)'s (w0ts, GPL-3.0-only OR
MPL-2.0, used here under GPL-3.0-only), taken as they are from its commit `2b559c1d` (2026-11-27; before it from
`5b06939`, 2026-10-25), with the central role (`ble_central.c`, `ble_gattc.c`, `ble_scan.c`, `ble_ll_central.c`,
`ble_hw_wl82_central.c`; `ble_smp_init.c` only at level 1, not built). The changes: `ble_stack.c` takes `BLE_API` from
its includer, and `ble_hw_wl82.c` takes `BLE_HW_RX_OK_EVTCOUNT` (hx.c sets 0: a connection event counts as heard only
with an RX interrupt, see there). `../../hal/fm1_ble.h`, `fm1_ble_rf.h` and `ble_rf_tables_v15.h` stay as taken at
`5b06939`, with the TX trims of Melodee (Kerem Kilic, Charles Vestal) that upstream does not have; `ble_diag.h` too
(upstream dropped its diagnostics). Their design, the hardware facts and the provenance (written from the Bluetooth
Core and BLE-MIDI specifications; the radio's start-up tables captured from stock V15 in an emulator) are Optimist's
`docs/BLE-STACK.md`, `docs/BLE-HW-FACTS.md` and `docs/BLE-DEVICES-DESIGN.md`.

`hx.c` / `hx.h` are fm1-dock's, and `hx_dev.c` (DEVICES: connecting out to an SMK-37, another FM-1 or a
controller; a trimmed port of Optimist's `io/midi/ble_devices.c` and `ble_connect.c`, GPL-3.0-only): the unit around the stack, linked at flash 0x88000 with a header the
host checks before it calls anything (PLAN.md, "BLE-MIDI"). `tests/ble_test.py` runs it in fm1-emu.
