<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/media/fm1-dock-light.svg">
    <img src="docs/media/fm1-dock.svg" width="420" alt="fm1-dock">
  </picture>
</p>

<p align="center"><b>One firmware for the M-VAVE FM-1. Many synths.</b></p>

fm1-dock is installed once. The synths are modules in flash: hold HOME, pick another one, play. No reinstall.
Deploy your own module over USB in seconds, and when something goes wrong, the log, the crash record and the
render load are one click away in the browser.

<p align="center"><img src="docs/media/fm1-on-device.jpg" width="640" alt="The picker on a real FM-1: SINE, CS-1, SCINTILLA, OBERVAVE, FM-1 SID"></p>

**[The page](https://joakimeriksson.github.io/fm1-dock/)**: the video, the console (status, log, deploy and probes over
WebMIDI), the emulator in the browser, the module interface and the protocol.

## What's in the dock

<p align="center"><img src="docs/media/screens.png" width="816" alt="The picker, FM-1 SID, CS-1, Scintilla's laser harp, OberVave and the sine, as fm1-emu draws them"></p>

| | by | voices | |
| --- | --- | --- | --- |
| **CS-1** | twochannels | 8, each two channels (I and II) | A CS-80-style synthesizer: two layers, its tone bars, effects, user presets, velocity and aftertouch from the keys. |
| **Scintilla** | Joakim Eriksson | 8 (the patch's own polyphony, at most 8) | The 1981 Elka Synthex: all 80 factory patches, the laser harp ("Ring mod.") among them. |
| **OberVave** | zvenson | 8, shared by the four tracks | A 4-track analog poly with arpeggiator, sequencer and effects. |
| **FM-106** | PatttF | 6, three on the second core | The Roland Juno-106: six voices, the 128 factory presets, its BBD chorus, arpeggiator and sequencer; three of its voices on the FM-1's second core. [The original](https://github.com/PatttF/fm-106), its app and DSP unchanged (`modules/fm106/take.py`). Not in the package: added with `tools/dock.py deploy`. |
| **FM-1 SID** | Joakim Eriksson | 3, the SID's | C64 music: a cycle-exact 6510, reSID and an own SID model; GoatTracker 2 songs and their editor: [write your first chiptune](https://github.com/joakimeriksson/fm1-sid#your-first-chiptune-step-by-step) on the FM-1. Not in the package: added with one click on [the page](https://joakimeriksson.github.io/fm1-dock/#install), or `tools/dock.py deploy`. |
| **AMB-1** | Charles Vestal | 4 pad voices, 6 bells, a bass, 3 drums | A generative ambient machine after *Music for Airports*: it plays itself. [The original](https://charlesvestal.github.io/fm1-amb/), its code unchanged (`modules/amb`). Not in the package: added with one click on the page. |
| **FoMni** | Charles Vestal | 16 harp strings, a 3-note chord, a bass | A chord harp after the Omnichord: chords on the black keys, strummed on the white ones. [The original](https://charlesvestal.github.io/fm1-fomni/), its code unchanged (`modules/fomni`). Not in the package: added with one click. |
| **SINE** | built in | 4 | Dinghy's 4-voice sine: the fallback when a module fails, and the smallest module to copy. |

## Install

From [the page](https://joakimeriksson.github.io/fm1-dock/#install): **Install on my FM-1**, over USB in Chrome or Edge
(FM-1 SID and Scintilla too, [all three on one page](https://joakimeriksson.github.io/fm1-dock/install/)), then **Add FM-1 SID
to my fm1-dock** puts FM-1 SID next to the other synths. Or download
`fm1-dock-X.Y.fwsc` from the [latest release](https://github.com/joakimeriksson/fm1-dock/releases/latest) and install it
over USB:

```sh
pip3 install mido python-rtmidi
python3 tools/fm1_install.py fm1-dock-0.6.fwsc  # asks before it writes, restarts into fm1-dock
```

**M-Vave SMK-37** (Pro, Elite, MKE-P37, Starrykey 37 Play): see [SMK-37 instructions](SMK37.md).

Back to any other firmware, or to the official one, with the [FM-1 Firmware Hub](https://fm1.designburgapps.com) in
Chrome or Edge: fm1-dock answers the hub's updater like every firmware there. Installing a new fm1-dock keeps your
deployed modules, settings and presets. Installing firmware is at your own risk.

## Play it

| | |
| --- | --- |
| **HOME** | the picker (a module that owns the whole panel: hold HOME alone for 1.5 s) |
| **SELECT / PRESETS** | move in the picker; **PLAY**, **SEL** or **HOME** load the module |
| **OPTIONS** (past the picker's last module) | **SEL** changes the option, **HOME** goes back |
| **CC7 VOLUME** (OPTIONS) | ON: MIDI CC 7 (on any channel, or MIDI CH's) sets the level of whatever module plays, on top of MASTER (127 = as the knob alone, (v/127)² as the MIDI spec has it; back to 127 at every start). OFF by default: CC 7 goes to the module, as before (CS-1 and OberVave have their own) |
| **MASTER CC7** (OPTIONS) | ON: turning MASTER sends CC 7 (its value 0..127) on the keys' channel, over USB and BLE-MIDI; nothing for where it is at power-on, and the knob's noise sends nothing. With CC7 VOLUME also ON, a computer that echoes CC 7 back sets the level twice |
| **MIDI CH** (OPTIONS) | ALL (the default): every channel reaches the module. 1..16 (**SEL** steps, a knob turns either way): only that channel, which the module hears as channel 1, and the keys send on it. Modules that use channels of their own (OberVave's tracks) want ALL |
| **LOCAL** (OPTIONS) | OFF: the keys only send MIDI (on the keys' channel), the module gets none of them, so a computer playing it back gives no double notes; in CS-1, OberVave and FM-106 too (their keys are the host's then). ON by default |
| **EQ** (OPTIONS) | **SEL** opens its page: LOW (a shelf at 150 Hz), MID (a peak at 1 kHz) and HIGH (a shelf at 6 kHz), -12..+12 dB each, on whatever module plays, before MASTER (the speaker and USB audio alike). **SELECT** picks a line, **ALGORITHM** changes it, on the FM-1 **KNOB1..3** turn the bands straight away; **SEL** switches it ON / OFF, or puts a band back to 0 dB; turning a band switches it on. The chip's own hardware EQ filters it, beside the CPU: about 3 % of a block, counted in the module's render. OFF by default; a boost can clip a loud module |
| **KNOBS CC** (OPTIONS) | ON: KNOB1..4 send CC 74, 71, 73, 72 (cutoff, resonance, attack, release) on the keys' channel, over USB and BLE-MIDI, from 64 at power-on; a detent is one step, turned fast four. They are then not the module's knobs: the module hears the CCs as MIDI in (with LOCAL OFF only what the computer sends back). OFF by default |
| **MIDI THRU** (OPTIONS) | ON: what comes in on the TRS MIDI jack (notes, CCs, ...; not clock or SysEx) goes on to USB-MIDI and BLE-MIDI as it came, and the module plays it too. OFF by default |
| **BLUETOOTH** (OPTIONS, the last line) | BLE-MIDI OFF / ON with **SEL** (off by default); a computer, phone or controller connects to `FM1-DOCK xxxx` without pairing, notes go both ways . ON stops the second core until the next power-on (`1CPU`): on an FM-1 the radio and a working second core crashed it within a second |
| **1CPU / 2CPU** (the top line) | how many cores run: the second core starts at power-on and helps the modules that use it (CS-1, FM-106). It stops for Bluetooth, and after a crash with both cores (one core until power-off, instead of crashing again) |
| **OCT+ at power-on** | safe start: the sine only (and the radio not started) |
| **OCT− at power-on** | the USB rescue: no sound, only the updater |

Hold the keys from before you switch on until the screen shows something (half a second is read).

**A whole firmware in the boot slot** (`tools/hbboot.py`, `tools/dock.py deploy --boot`: a developer's
tool) takes over the FM-1 at every power-on, with its own USB. If its USB does not answer, the installers
cannot reach the FM-1 while it runs, so there are two ways back to fm1-dock that need nothing from it:
hold **HOME** while switching on (or while **STARTING** shows), or **switch off while STARTING shows,
twice in a row**: the third start stays in fm1-dock. Only deploy one you know answers the installers.

fm1-dock starts whatever played last. A module that crashes twice in a row is skipped at boot until you pick it again;
one that renders slower than real time is stopped and the sine plays, so USB and the updater keep working.

## Your own module

A module is a table of functions (`render`, `note`, `ui`, `draw`, ...) at the start of its image; it links against
nothing of the host's, and the host checks its header, ABI and CRC before anything of it runs. Start from
[modules/sine/sine.c](modules/sine/sine.c) (about 150 lines); [firmware/dock/hb_module.h](firmware/dock/hb_module.h)
has every call the host gives it.

```sh
python3 tools/hbmod.py modules/mysynth                                  # -> build/modules/mysynth.dock
uv run --with python-rtmidi tools/dock.py deploy build/modules/mysynth.dock   # into the FM-1, running
uv run --with python-rtmidi tools/dock.py modules | logs | select NAME | delete NAME
uv run --with python-rtmidi tools/dock.py send FILE [--arg N]           # a file into the RAM of the module playing
```

A module that sets `HB_F_DATA` gets what the computer sends it in its `data()` call, never written to flash:
[FM-1 SID](https://github.com/joakimeriksson/fm1-sid) takes a `.sid` that way and plays it at once
(`dock.py send tune.sid`, or "Play on my FM-1" on its page).

Or drop the `.dock` on the [console](https://joakimeriksson.github.io/fm1-dock/#console). A module has 272 KiB of RAM,
16 KiB of RAM code, a settings record and flash banks for presets; a deploy file is up to 272 KiB.

**Probes** measure the real chip: a small C program run once from RAM inside fm1-dock, its result back as JSON, the
same probe in [fm1-emu](https://github.com/joakimeriksson/fm1-emu) for comparison:

```sh
tools/probe.py run probes/timing.c --on emu,fm1
```

## Build it

You need Linux on x86-64 (or macOS / Windows with Docker or WSL), Python 3 with Pillow, the JieLi toolchain and three
files of the JieLi AC79 SDK:

```sh
pip3 install Pillow mido python-rtmidi
tools/get_toolchain.sh                      # JieLi's clang for the chip, to ~/.jieli/toolchain
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
./build.sh --modules cs1,scintilla,obervave   # -> build/fm1-dock.fwsc (fails on what would brick the FM-1)
```

In the emulator, without an FM-1 (the same package, and with `--flash` the same flash: deployed modules included):

```sh
fm1-live --fwsc build/fm1-dock.fwsc --flash flash.bin
tools/dock.py emu --fwsc build/fm1-dock.fwsc --deploy fm1sid.dock --ms 6000
```

The platform underneath is Dinghy's: [docs/API.md](docs/API.md) (everything `platform.c` gives), [docs/RULES.md](docs/RULES.md)
(the chip's rules and what the build checks), [docs/MAKE-IT-YOURS.md](docs/MAKE-IT-YOURS.md).

## Credits

fm1-dock, FM-1 SID and Scintilla: Joakim Eriksson. OberVave: zvenson. CS-1: twochannels. AMB-1 and FoMni: Charles Vestal.
FM-106: PatttF. reSID: Dag Lem. The second core: Charles Vestal's X0X, after Kerem Kilic's Melodee, by way of SLOOP
(Andre Rezemini).
fm1-dock stands on [Dinghy](https://github.com/zvenson/dinghy) (zvenson), on the platform of
[Felucca](https://github.com/hugelton/Felucca) (Leo Kuroshita, Hügelton Instruments) as [SLOOP](https://github.com/isod89/sloop-fm1)
(isod89) carries it. The keys' force sensing comes from Velo, by way of CS-1.

GPL-3.0-only: you may change and share it, and whoever gets your firmware gets its source. Details and third-party
parts in [LICENSING.md](LICENSING.md). Not affiliated with M-VAVE.
