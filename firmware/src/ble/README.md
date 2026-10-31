# BLE-MIDI: Optimist's stack, as HX

The `ble_*` files, `rf_share.h` and `../../hal/fm1_ble.h`, `fm1_ble_rf.h`, `ble_rf_tables_v15.h` are
[Optimist](https://github.com/w0ts/optimist)'s (w0ts, GPL-3.0-only), taken as they are from its commit
`5b06939` (2026-10-25); the changes: `ble_stack.c` takes `BLE_API` from its includer, and `ble_hw_wl82.c` takes
`BLE_HW_RX_OK_EVTCOUNT` (hx.c sets 0: a connection event counts as heard only with an RX interrupt, see there). Their design,
the hardware facts and the provenance (written from the Bluetooth Core and BLE-MIDI specifications; the
radio's start-up tables captured from stock V15 in an emulator) are Optimist's `docs/BLE-STACK.md` and
`docs/BLE-HW-FACTS.md`.

`hx.c` / `hx.h` are fm1-dock's: the unit around the stack, linked at flash 0x88000 with a header the
host checks before it calls anything (PLAN.md, "BLE-MIDI"). `tests/ble_test.py` runs it in fm1-emu.
