# fm1-dock: plan

One firmware for the M-VAVE FM-1 that is installed once. Synths are modules sent over
USB-MIDI into flash slots; one runs at a time, as many are stored as fit. A broken module can
never take down the host or the USB rescue, and logs, CPU load and crashes are visible from the
computer.

Built on [Dinghy](https://github.com/zvenson/dinghy) 0.3 (GPL-3.0, its history kept, remote
`upstream`). Developed and tested first in [fm1-emu](https://github.com/joakimeriksson/fm1-emu),
then on hardware.

## What exists already (2026-10-20)

- **Dinghy 0.3** runs on the FM-1: USB-MIDI in/out, USB audio, the updater (M-UPGRADE over
  SysEx, staged at flash 0xE0000), USB rescue (OCT- at power-on, and by itself after two crashes
  at boot), a crash screen with the PC, an 8 s watchdog, settings in their own sector (0xFC000),
  host mode (`tests/run_host.sh`). Its loader (`firmware/loader`) writes only the app area.
- **Dinghy's flash driver** (`firmware/hal/fm1_flash.h`) already has what the slots need:
  - a configurable **plain (unencrypted) XIP window**, `fl_plain_window_init`: from flash
    0x93000 to the end, data and code are read as stored, no chip-key scrambling;
  - **cache invalidation** after a write, `fl_inval` (`flushinv` per 32-byte line);
  - a range guard, `FL_RANGE_OK`, for every erase and program; RAM-resident erase/program
    (`.ram_text`), interrupts off.
- **Memory** (`firmware/app.ld`): code from XIP 0x02000120 (up to 0x8DFBC bytes); RAM text
  0x01C00000 (24 KiB); RAM 96 KiB at 0x01C08000; pool 336 KiB at 0x01C20000; NOINIT
  0x01C7C000 (survives resets). Audio: `app_audio(int32_t *out, n = 256)` at 44.1 kHz.
- **Barge** ([tradiuz/fm1-barge](https://github.com/tradiuz/fm1-barge), GPL-3.0, one commit,
  not maintained): a Dinghy fork with statically linked instruments and a
  [plugin plan](https://github.com/tradiuz/fm1-barge/blob/main/docs/PLUGIN-ARCHITECTURE-PLAN.md)
  that leaves runtime loading open. Ideas reused with credit: the descriptor, the bounded event
  queue, switching at block boundaries, a fallback instrument, core 0 first.
- Nobody loads native code after the build on a real FM-1 yet.

## Design

| Topic | Decision | Why |
|---|---|---|
| Base | Dinghy 0.3; its loader, updater and rescue untouched in v1 | the rescue path is hardware-tested; our bugs must not reach it |
| Running | one module at a time; as many stored as fit | one real synth uses most of the CPU and the pool |
| Where modules run | in place from flash, in the plain window | no RAM for code, no scrambling |
| Placement | anywhere in the slot area: relocated when written (below) | slots of any size, gaps can be closed |
| Module RAM | one fixed region in the pool, shared by all modules | only one runs, so RAM addresses never need relocating |
| Host <-> module | tables only: the module's descriptor, the host's service table passed to `init` | no link-time dependency on the host build; the ABI version is checked |
| Hot code | an optional `.ram_text` in the module, copied to RAM on load | inner loops away from XIP cache misses |
| Audio | `app_audio` calls `module->render` | Dinghy's timing unchanged |
| Cores | core 0 for the host; core 1 lent to the module's render, one job at a time (`c1_run` / `c1_wait`, `src/cpu1.c`, X0X's HAL) | some units' core 1 misreads RAM (fm1-emu `--flaky-core1`): screened in the first audio blocks, checked around each job, held at the first misread |
| Commit of a deploy | the slot-table entry is written last; no valid entry = no module | a cut at any moment leaves a bootable host |
| Debug | a log ring and crash record in NOINIT, sent as SysEx | survives the reset; fm1-emu reads it directly |

### Relocation

Calls and branches inside a module are PC-relative (pi32v2's 23-bit call range). Absolute
addresses are pointers to the module's own constants and tables, each a 32-bit word (`c0 ff
<u32>` loads a whole word). The build links every module twice, at two flash bases, and compares:
each word that differs by exactly the distance between the bases is a relocation. The list ships
in the module file; whoever writes the module (the host, or the deploy tool) adds the slot's
offset to those words. Words that change by any other amount (an address split over two
instructions) fail the build until they are handled. No reliance on `-fPIC` in JieLi's
clang 4.0.1.

### Module file (v1, little endian)

    magic "HRBR" | abi | flags | name[24] | version
    code_size | ram_size (.data + .bss) | data_size | ram_text_size
    worst_cycles_per_block (measured in fm1-emu) | entry offsets (descriptor)
    n_relocs | relocs[] (offsets of words to add the slot base to)
    CRC32 over all of the above and the image
    image: .text + .rodata, then .data's initial values, then .ram_text

### Interface v1

- Descriptor: `abi`, name, `init(const host_t *)`, `release()`, `note(ch, key, vel)`,
  `cc(ch, num, val)`, `render(int32_t *out, uint32_t n)`, `ui(event)` (keys, buttons,
  encoders), `draw(canvas)`, parameter names and ranges for a generic host page.
- Service table (`host_t`): time, log, settings record per module, canvas drawing, LEDs,
  random, and the helpers a compiler may call (memcpy, memset, division) so a module links
  against nothing.

### Switching modules

Mute at a block boundary, `release()` the old one, copy the new one's `.data` and `.ram_text`,
clear `.bss`, `init()`, unmute. Notes are cut; flash is not touched. Each module's settings
record survives switching away and back.

### Flash

    0x00000-0x04000  boot chain (uboot.boot, isd_config.ini)   never written
    0x04000-...      host (Dinghy + fm1-dock)                    written by updates only
    ...-0xE0000      slot table + module slot area             written by deploys only
    0xE0000-0xE5000  update staging (Dinghy's ota.c)           as Dinghy
    0xFC000          settings                                  as Dinghy

The plain window moves down from 0x93000 to just past the host (measured in P0). The deploy
path's `FL_RANGE_OK` allows the slot area and nothing else.

## Status

2026-10-21, tested in fm1-emu only (`tests/emu_test.py`, `tests/deploy_test.py`, headless fm1-live;
nothing on hardware):

- [x] **P0**: Docker Desktop (hung since Oct 9) restarted; the three SDK files fetched (gitee, hashes
      match); the package builds reproducibly from a clean `build/`; boots in fm1-emu (`--bin` and the
      `.fwsc`). The host is 59.8 KB of the 191.7 KB it may take (the slot table is at XIP 0x02030000).
- [~] **P1**: the log ring, the crash record, the strikes and the render stats are in NOINIT;
      `tools/dock.py log` reads them from a RAM dump (fm1-live `FM1_DUMPRAM`), `dock.py logs` /
      `modules` over USB-MIDI SysEx (LOG, HELLO). Not yet: the crash test (fm1-emu has no fault /
      reset model: it halts on a wild jump).
- [x] **P2**: interface v1 (`firmware/dock/hb_module.h`), the double-link relocation (`tools/hbmod.py`;
      also C++ and libm / libc, static constructors refused), modules packed at build time into
      the app slot (scrambled with the host: no plain window needed yet). The sine from its slot is
      bit-identical to the built-in one, at two slot addresses.
- [~] **P3**: slot table, header / ABI / CRC / size checks, the picker (HOME), switching, the built-in
      sine as the fallback, per-module settings, OCT+ safe boot, two strikes skip a module. Tested:
      a bad CRC is refused. Not testable in fm1-emu yet: a crash or watchdog reset and the reboot after it.
- [~] **P4 deploy** (`firmware/src/dock.c` "deploys", `tools/hbdeploy.py`, `dock.py deploy`):
  - SysEx `F0 7D 48 42 cmd seq pack7(payload, CRC-32) F7`: HELLO, LIST, BEGIN / DATA (256 B) / COMMIT,
    ABORT, DELETE, SELECT, SAFE (the sine), LOG. Over USB each frame is answered; over the DIN jack
    (and a `.syx`) nothing comes back, the sender paces itself.
  - Flash: the deployed table at 0x93000 / 0x94000 (two copies, sequence number, header written last),
    the host's settings at 0x95000, modules at 0x96000-0xE0000 (296 KiB, 4 KiB aligned, in Dinghy's
    plain window, first fit). An update
    writes only below 0x93000, so deployed modules survive host updates (refused if the ABI changes).
  - The file is received whole into the module RAM (the running module stopped) and checked (CRC,
    header, relocations) before any erase; relocated to where it goes, written, read back through XIP
    and checked as at boot; then the table. A deployed module of the same name stands in for the
    image's. No room beside the old copy: the table without it goes first (a cut then loses that
    module, never the host). Not yet: compaction, the WebMIDI page.
  - Tested over DIN in fm1-emu (the package booted, `--flash`): a new module in, running, after a
    reboot, DELETE; a damaged chunk refused at COMMIT with no flash touched; a power cut half way
    through each of the 12 erases / programs of a first deploy and of a replacing one: the host
    boots and plays every time, the table lists the old module or the new, never a broken one;
    CS-1 (50 KB, 619 relocations) deployed over DIN in 20 s and playing. Not tested: USB (fm1-emu
    has no USB device model; the frames and replies go through Dinghy's SysEx path, as the updater's).
- **Brick safety** (beyond Dinghy's loader, rescue and boot guard, all unchanged):
  - the host's flash writes: 0x93000 up only (deploys, banks, Dinghy's staging) and its settings;
  - a crash or watchdog in a slot module does not count towards the boot guard (so it never lands in
    the USB rescue by itself), a crash of the built-in sine does (it is the host's own);
  - a module slower than real time for 64 blocks is stopped: the main loop, USB and the updater keep
    running; HELLO / SAFE / SELECT / DELETE reach a running fm1-dock from the computer;
  - banks are 8 KiB by module name (`HB_BANK_MAX` 8176; CS-1's 4 KiB bank did not fit the old 4 KiB
    sector with its header, so its user presets were never saved).
- **The keys' force** (velocity and polyphonic pressure): Velo's sensing (Charles Vestal,
  `charlesvestal/fm1-velocity`, GPL-3.0; `firmware/src/sense.c` as CS-1 carries it) in the host's
  key scan, with the analog rows (PA7 / PA8, 20 keys; the top row has no converter) and the row
  drive taken onto Dinghy's scanner. Sensed keys play with their landing's velocity (KEY_DOWN's arg,
  note() for the others), held ones report HB_EV_KEY_PRESSURE; CS-1 gets the readings
  (`key_sense`) for its own pressure and sets the sensing from its KEYS pages (`sense_set`).
  The MASTER pot is converted in the scan too. fm1-emu models the pills (branch `fm1-key-pressure`,
  on `fm1-live-flash-file`): `ms:ID@VEL`, `ms:ID~VEL`, the mouse's height on a key. Fitted to CS-1's
  measurements: CS-1's own firmware in the emulator senses 8 / 19 / 50 / 80 / 101 for 1 / 20 / 64 /
  100 / 127. Not on hardware.
- Identity: CS-1's own package is also `FM-1_950`: fm1-dock needs another before any release.
- **Probes** (`tools/probe.py`, `probes/`, `tests/probe_test.py`): a program sent as a deploy is
  (BEGIN / DATA) but run once from RAM (RUN) instead of written (COMMIT refuses it, and the boot
  refuses one in a slot). Its code runs where it was received (HB_MOD_RAM up), its .data / .bss at
  HB_PROBE_RAM; its output (typed records, `probes/probe.h`) is in NOINIT with the crash PC, so
  RESULT gets it after a crash or the watchdog, which do not count towards the boot guard.
  `probe.py run X.c --on emu,fm1` builds, runs on both, prints JSON and the time ratios;
  `probe.py exec "r3 = r1 * r2" --regs r1=5,r2=7` runs instruction lines in a register wrapper
  (`probes/exec.c`). Probes so far: HELLO (the path itself), TIMING (instructions, RAM, flash,
  calls), EXEC, CRASH (FM-1 only). Tested in fm1-emu; on an FM-1 they are the way to do P5 and to
  fit fm1-emu's timing (it charges one instruction per 240 MHz cycle) and its instructions.
- **Boot slot** (`tools/hbboot.py`, `tests/boot_test.py`): a whole firmware, rebuilt from its source to
  run at flash 0x15000 (`hbboot.py relink`: its app.ld, its PC guard, its build's start address),
  deployed as a module is (streamed: each 4 KiB sector encrypted with the chip key and written as
  it arrives; the table entry last). At power-on fm1-dock hands the FM-1 over in fm1_cstart, after its
  50 ms key scan and before its guard, screen, USB and audio (platform.c `APP_BOOT_HOOK`): the
  firmware has both cores, all RAM, its own USB and flash areas. HOME (or OCT-/OCT+) held: fm1-dock
  stays; a firmware that fails its CRC (read back through the SFC) or was started twice without a
  power-on between (a counter at 0x01C7FD40, app.ld `.hbgo`): fm1-dock stays. Picking it in fm1-dock's
  list reboots into it. Modules and a boot firmware exclude each other (its data is where they go).
  - Felucca 1.6.1 (511 KB): relinked, deployed over DIN in fm1-emu (218 s), its own UI and sound at
    power-on; HOME gives fm1-dock's list; a flipped bit, a module deploy and DELETE behave (6 cases).
  - For it: the host's settings moved off 0xFC000 (Felucca's) to 0x95000; the fonts packed to 1 bit
    per pixel (`tools/font_1bpp.py`: they are bitmap fonts) to bring the host from 75 to 51 KB, so the
    slot can start at 0x14000; Felucca leaves 4.6 KiB of it free.
  - fm1-emu (branch `fm1-sfc-window`, on `fm1-key-pressure`): the SFC's decryption below the plain
    window the firmware sets, the window rebuilt from a `--flash` file, `FM1_BENCH_KEYS=@FILE`.
  - A boot firmware that keeps nothing in flash outside its slot (`hbboot.py make --no-data`,
    HB_F_BOOT_NODATA) leaves the deployed modules in: they deploy beside it, and it beside them.
    FM-1 SID (fm1-emu's fm1-sid, relinked for the slot, its tunes in its pack: 485 KiB with 15) with
    CS-1 and SCINTILLA deployed (`tests/trio_test.py`, both orders): power-on starts the last pick
    (FM-1 SID, or fm1-dock with a module), HOME held keeps fm1-dock, picking FM-1 SID in the list
    stores it and reboots into it.
  - Not yet: SLOOP and the others (each one's flash map and NOINIT checked: Felucca's NOINIT ends at
    0x01C7FCD0), a resend of the deploy over USB (nothing tested over USB), a firmware's own updater
    (it would write over fm1-dock: an install, as from the Hub).
- [ ] P5 hardware: not started.
- **Interface v2** (ABI 2): a module may take the whole screen (HB_F_SCREEN) and the whole panel
  (HB_F_PANEL: every key and button as events, OCT and HOME too; HOME held 1.5 s alone opens the
  module list), raw MIDI (midi()), blit, text_w, and an 8 KiB flash bank of its own (by module
  name, 0xEA000-0xFC000, Felucca's USR area). `modules/lib/hb_fm1.h` gives a ported firmware its
  platform calls (fm1_in, fm1_input_edges, fm1_enc_take, midi_get, ...) on top of them.
- **Modules, with their own UIs**:
  - SID: FM-1 SID's synth mode as it is (`modules/sid/fm1sid/take.py` takes its main.c by line
    ranges, each change checked): its C64 screen, the step sequencer, the FX page; reSID 0.16
    only (one chip, as FM-1 SID), the own model gone; 15 % of a block. Not here: the PSID player.
  - CS-1: CS-1's own app (cs1.c unchanged but for its platform include, `cs1_platform.h`): its
    pages, sounds, user presets (in the bank), MIDI learn and clock; 8 voices, the odd ones and the
    reverb on the second core (core 0's worst 38 % of a block in fm1-emu; 61 % on one core, where a
    unit whose core 1 misreads falls back); the keys' velocity and pressure from the host's sensing.
  - SCINTILLA: joakimeriksson/scintilla-fm1's app and engine (the Elka Synthex of synthex-web, in
    integers), taken by `modules/scintilla/take.py` (its app_main split into app_init / app_loop,
    the first sound loaded while the audio is muted, the DIN jack the host's): 80 presets, its
    pages and joystick, MIDI; 89 KB of code, 106 KB of RAM.
  - SINE: Dinghy's, and the built-in fallback.
- **fm1-emu**: test with its current master. The binaries in `~/work/fm1-emu/target/release` were
  from 2026-10-12 and cannot run a pair move (`r5_r4 = r11_r10 #`) that master can;
  `tests/emu_test.py` checks every instruction with the emulator's `--check-decode` first.

## Next modules (survey 2026-10-21)

Popularity from the Firmware Hub's catalog (`installs` from its own page, and GitHub stars). A port
needs the source: a shim of the firmware's platform API on `modules/lib/hb_fm1.h`, as CS-1's.
- **OberVave** (545 installs, Dinghy, 1 core, 120 KiB): CS-1's pattern again; Shine On after it.
- **FoMni** (X0X platform, 1 core, 156 KiB): one shim of that small API also ports FiMba, HYP-1,
  FuMi, MIN-1, DUB-1 and Fart Machine.
- **AMB-1** (679 installs, fm1-base, MPL-2.0): about 281 KiB of buffers against 272 KiB of module
  RAM (an int16 reverb, a shorter tape, or more RAM); then Mutable Dreams on the same shim.
- **FM-106** (Dinghy, a single-core build): measure its load on one core first.
- Not as modules: the Felucca-family whole firmwares (SLOOP, Felucca, JIANT, GHOULBOX, Skiff,
  ChoralRoot, Melodee, Tonitor: 430-570 KiB, both cores, their own USB audio and flash layouts);
  closed or unlicensed ones (April OS, SLOOP ALG, the stock firmware).

## BLE-MIDI (plan, 2026-10-25)

The stack: **Optimist's** (w0ts/optimist, GPL-3.0-only, `docs/BLE-STACK.md` there), written from the
Core and BLE-MIDI specifications: link layer, host, ATT, BLE-MIDI and a driver for the WL82 baseband
engine, no vendor code, no malloc. A Mac connects and MIDI goes both ways (Optimist on an FM-1, 2026-10).
JieLi's own stack (btctrler.a / btstack.a, closed bitcode around its RTOS, 120-200 KiB) cannot go
into a GPL firmware; fm1-base and Hortator use it.
- **Where:** `firmware/src/ble/` (Optimist's files as they are, its headers kept) as a unit of its
  own, **HX**, linked at the top of the image area (flash 0x88000-0x92000, 40 KiB; the app slot ends at 0x920DC) with a header
  (magic, size, CRC, its entry points). The host calls it only through that header, and only after
  the CRC is good, as it does a module; HX copies its own .data and RAM code. A boot firmware may
  write over it: then there is no BLE (the host says so), the rest works. A boot deploy stops BLE
  before it writes there. The host keeps the glue only (`dock.c`: the setting, the rings).
- **MIDI:** BLE in goes into `midi_in_q` (to a module as USB's); `midi_send` goes out over BLE too
  while a central listens; notes a central left on are ended when it goes.
- **Off by default:** SYSTEM's BT MIDI OFF / ON. Off, the radio never starts.
- **The radio's trims:** the unit's factory calibration, which the stock firmware keeps in its VM at
  flash 0xE8000. Optimist reads them there, keeps a copy with the settings, and never starts the
  radio without them. fm1-dock never writes 0xE7000-0xE9FFF (the flash guard refuses it).
- **Status (2026-10-25, branch `ble-midi`):** steps 1-3 done in fm1-emu. HX 31 KiB of its 40,
  8 KiB of RAM; the host +1 KiB. `tests/ble_test.py`: OFF from boot; ON without trims stays NO CAL
  and is kept; ON with stock's VM planted at 0xE8000 runs the radio's start-up against the
  emulator's empty BT registers and advertises (from boot after a reboot), the VM's sectors left
  as they were; a damaged HX is refused. The other tests pass with it (emu, deploy, probe, boot).
  **On an FM-1 (2026-10-25)**, from stock V15: its trims intact at 0xE8000 (probes/radiocal.c;
  backed up), FM1-DOCK advertises (-45 dBm at the desk), a Mac (tools/blemidi.py, bleak) connects
  without pairing, notes go in, and the keys come out with their velocity and pressure (188
  messages in 20 s). The first build crashed at boot on the hardware: HX attached its two vectors
  after Dinghy's write limit had locked the vector table (hal/fm1_guard.h); fm1-emu does not model
  the write limits. Now HX starts only at the first ON, and its start opens and locks that window.
- **Steps:**
  1. The stack as HX, built and linked (stub driver and WL82 driver); the header check.
  2. The glue, the setting, the status on the screen; a settings record (on, the address, the
     trims copy).
  3. fm1-emu: boots with BT ON and no trims (the radio stays off), and with trims planted at
     0xE8000 (the start-up program runs against the emulator's empty BT registers and gives up
     within its bounds). Later, in fm1-emu: a model of the baseband engine with a scripted central.
  4. On an FM-1 (the user's call): a probe reads 0xE7000-0xE9FFF (trims there? a backup); a Mac
     connects; MIDI both ways; render headroom with BLE on (CS-1, SID); a USB deploy while connected.
  5. Later: deploys over BLE (modules); the central role, so a BLE keyboard or controller plays the
     FM-1 with no computer between (Optimist's BLE_CENTRAL, 20.8 KiB of flash and 2.2 KiB of RAM,
     pairing BLE_BOND 3.9 + 0.5, the devices line ~1-2 KiB: HX about 57 KiB, so a bigger region
     and one package module less); until then a controller can be the central itself.
  6. fm1-emu: the write limits (WR_LIMIT, C0_WR_LIMIT_EN), which would have caught the crash above.

## Phases

Each phase ends with a test that has to pass before the next starts.

**P0: Setup.** Repo from Dinghy's history; JieLi toolchain (`tools/get_toolchain.sh`, Docker
amd64 on macOS); build Dinghy; run it in host mode and in fm1-emu; measure the host's real size.
*Pass:* the build is reproducible (same bytes twice) and boots with sound in fm1-emu.

**P1: Debug channel.** `hlog()` into a NOINIT ring; a crash record (PC, registers, cause) from
the exception handler; stats (render cycles per block, worst, missed blocks); SysEx requests for
log / stats / crash; `fm1-dock log` CLI. fm1-emu: `--log-ring` prints the ring live.
*Pass:* in fm1-emu, logs show, and a deliberate crash's record is there after the reboot.

**P2: Interface v1 and relocation, no loader yet.** Descriptor and service table; Dinghy's sine
synth moved into a separately linked module, placed in the image at package time; the
double-link relocation tool; a build check for undefined symbols and hidden helper calls.
*Pass:* audio bit-identical to stock Dinghy in an fm1-emu WAV, from two different slot addresses.

**P3: Slot table, loader, picker.** Slot table; header and CRC checks before any call; safe boot
(OCT+ at power-on skips modules; two crashes in a module disable it); the built-in sine as the
fallback; a module picker on the device; switching.
*Pass:* in fm1-emu, a valid module plays, a bad CRC falls back to the sine, a crashing module is
skipped on the next boot, switching between two modules works.

**P4: Deploy.** SysEx framing (7-bit packing, sequence numbers, CRC per chunk, ack/nak,
resume); erase and program inside the slot area only; verify through XIP; invalidate the cache;
table entry last; first-fit placement and compaction (moving modules down, relocated). The same
protocol on USB-MIDI and the DIN jack (fm1-emu has DIN, not USB). `fm1-dock deploy mod.dock`
(Python) and a WebMIDI page.
*Pass:* end-to-end deploy in fm1-emu over DIN; a power-cut test, stopped at 200 random points,
always boots the host.

**P5: Hardware.** H1 logs over USB-MIDI; H2 code runs from the plain window; H3 a replaced module
runs the new code without a reboot; H4 erase/program timing and the audio pause; H5 USB rescue
and OCT- rescue still work. Have the RP2040 rescue dongle at hand.
*Pass:* all five on one unit. If H2 fails: the writer scrambles modules with the chip key.

**P6: Real synths.** CS-1 first (77 KiB, single core, GPL), then OberVave and FM-1 SID: the
engine lifted out of its platform onto the service table.
*Pass:* the CS-1 module and the CS-1 firmware give the same WAV from the same MIDI script in
fm1-emu.

**P7: Budgets and SDK.** fm1-emu measures each module's worst render cycles and size; the host
enforces the declared budget (after N overruns: mute and log); a module template repo, docs,
release 0.1.
*Pass:* someone outside the project builds and deploys a module from the docs alone.

**P8: fm1-emu integration.** Catalog entry; "Deploy module…" on the page and in the plugin
writes the emulated slot area.

## fm1-emu work (separate PRs there)

1. The plain-window registers (`SFCENC_CON`, `SFCENC_UNENC_L/H`): today the emulator assumes
   the flash past the image reads plain.
2. `flushinv`, and dropping JIT-compiled blocks of flash that was rewritten (a deploy into flash
   that ran code earlier in the same run would run the old code).
3. `--log-ring`; cycle counts per function (module budgets).
4. Later: a USB-MIDI device model (Dinghy's `usb.c` as the reference), which would also let it
   test updaters (and fm1-dock's deploys over USB, with their replies).
5. Done on a branch (`fm1-live-flash-file`, a worktree, not pushed): `fm1-live --flash FILE` (the
   flash from FILE, back to it at the end) and `FM1_FLASH_CUT=N` (a power cut half way through the
   Nth erase / program), for `tests/deploy_test.py`. Until it is merged the tests need that build.

## Risks

| Risk | Mitigation |
|---|---|
| Code does not run from the plain window | H2 early; fallback: scramble with the chip key when writing |
| The compiler splits addresses or calls hidden helpers | the P2 relocation check and symbol check; helpers in the service table |
| Installing the host bricks a unit | Dinghy's loader and rescue unchanged; H5; the RP2040 dongle |
| Less flash than hoped | measured in P0; one 160 KiB module is enough for a CS-1-sized synth |
| Licence | the host is GPL-3.0, so modules are too in practice; said in the README |
