# fm1-dock on the M-Vave SMK-37

fm1-dock also runs on the **M-Vave SMK-37** keyboards: the SMK-37 Pro, the SMK-37 Elite and the MKE-P37, and the
Donner Starrykey 37 Play (with the SMK-37 Pro's package). They have the FM-1's chip; only the panel and the flash map
differ. It is the same fm1-dock, with the same modules: CS-1, Scintilla, OberVave and the sine in the package, and
FM-1 SID, AMB-1 and FoMni added from [the page](https://joakimeriksson.github.io/fm1-dock/#install). A module does not
need to know: the SMK's panel reaches it as an FM-1's, and what the FM-1 does not have (the outer keys, the pads, the
faders, the wheels) reaches it as MIDI.

> **Status**: built and tested in [fm1-emu](https://github.com/joakimeriksson/fm1-emu), which runs the SMK-37
> (`tests/smk_test.py`: the keys and their velocity, the pads, faders and wheels, the module list, deploys into both
> areas, BLE-MIDI's start-up, the USB rescue). Not yet tried on a real SMK-37.

## Packages and models

| Your keyboard | Package | Identity |
| --- | --- | --- |
| SMK-37 Pro, Starrykey 37 Play | `fm1-dock-X.Y-smk37.fwsc` | `SMK-37 Pro_921` |
| SMK-37 Elite | `fm1-dock-X.Y-smk37-elite.fwsc` | `SMK-37 Elite_921` |
| MKE-P37 | `fm1-dock-X.Y-smk37-mke.fwsc` | `MKE-P37_921` |

Each package reports its own model's name, so M-Vave's own updater still takes that model's factory firmware
afterwards. The installers refuse an FM-1 package on an SMK-37 and an SMK-37 package on an FM-1, before anything is
sent: they read the board from the package itself.

## Before you install

The SMK-37 keeps data in its flash that is **unique to your unit** and cannot be downloaded again: the factory key,
pad and wheel calibration (`0xC1000`), the Bluetooth pairing, the chip's key data. fm1-dock never writes there: its
flash driver has an allow-list of what it may write, and a second, independent guard that refuses every protected
range (`firmware/hal/fm1_flash_map.h`). It **reads** the calibration, for each key's and pad's velocity curve.

1. **Make a full flash backup first**, as genkigenki's kit describes it:
   [fm1-to-smk37, docs/02-update-and-recovery.md, section 3](https://github.com/genkigenki/fm1-to-smk37/blob/main/docs/02-update-and-recovery.md).
   Check that the text `trimdat` is at offset `0xC1224` of the dump.
2. The stock firmware keeps its key and knob mapping in USRFLASH (`0xC2000`), which fm1-dock uses for added modules
   and their banks. Before it first writes there it erases the stock mapping's end marker, so that after a return to
   the stock firmware it rebuilds its factory mapping. The calibration is never touched.

Installing firmware is at your own risk.

## Install

In Chrome or Edge, with the keyboard on USB: **Install fm1-dock on my FM-1** on
[the page](https://joakimeriksson.github.io/fm1-dock/#install). It asks the keyboard for its model and installs that
model's package. Or over USB from a computer:

```sh
python3 tools/fm1_install.py fm1-dock-X.Y-smk37.fwsc        # the Elite / MKE-P37: their package
```

It works from the stock firmware and from the other Felucca-family firmwares.

**USB rescue**: hold **OCTAVE-** while you switch the keyboard on: no sound, only the updater (install again from
there). **The way back to stock**: install your model's factory package with M-Vave's updater (M-UPGRADE), as for any
firmware update.

## Controls

| SMK-37 | fm1-dock | |
| --- | --- | --- |
| Keys F3 .. G5 (the middle 27) | the FM-1's 27 keys | with their velocity (two contacts, the unit's factory curves) |
| Keys C3 .. E3, G#5 .. C6 | | notes below and above them, with the octave and the velocity |
| **K1** | MASTER | the volume |
| **K2 · K3 · K4** | SELECT · ALGORITHM · PRESETS | |
| **K5 · K6 · K7 · K8** | KNOB1 · 2 · 3 · 4 | |
| **PATCH** | HOME | the module list (held 1.5 s alone in a module that has the panel) |
| **SCALE** | SEL | loads the module picked in the list |
| **PARA · CHORD · NOTE REPEAT · GLOBE · BT** | EDIT · ENV · LFO · GLO · SAVE | |
| **ARP · FX · SEQ** | ARP · FX · SEQ | |
| **SEQ PLAY · SEQ REC** | PLAY · REC | |
| **OCTAVE- · OCTAVE+** | OCT- · OCT+ | at power-on: OCTAVE- the USB rescue, OCTAVE+ the safe start (the sine only) |
| **16 pads** | | notes 36 .. 51 on channel 10 (the bottom row 36 .. 43, the top row 44 .. 51, left to right), with their velocity; a hit lights the pad |
| **4 faders** | | CC 70 .. 73 on channel 1 |
| **Mod wheel** | | CC 1 |
| **Pitch wheel** | | the pitch bend |
| KNOB BANK, FADER BANK, < , > | | not used yet |

The outer keys, pads, faders and wheels go to the module running as MIDI, and out over USB-MIDI too: the keyboard
is a controller for your computer as well. A module's own manual applies to the rest (CS-1's pages, FM-1 SID's
editor, ...): where it says KNOB1..4, those are K5..K8 here.

## Added modules

The same `.dock` files as on the FM-1. The SMK-37's flash has room for added modules in two areas around its
protected calibration: 288 KiB (from `0x70000`) and 128 KiB (from `0xC2000`). A module needs one of them whole: FM-1
SID (268 KiB) and AMB-1 (76 KiB) fit together; FoMni (139 KiB) fits in the first area, not beside FM-1 SID.

## BLE-MIDI

As on the FM-1: **BLUETOOTH** at the end of the module list (PATCH, K2 down, SCALE) switches it ON; the keyboard then
shows up as `FM1-DOCK XXXX` among a computer's or a tablet's Bluetooth MIDI devices. It is the same code as the FM-1's
(the chip and its radio are the same), with one difference: where the radio's stored calibration comes from.

The radio starts from a calibration the stock firmware stores in its settings store (VM), as on the FM-1. On the SMK-37
that store's region (`0x9D000`-`0xBFFFF`) is where fm1-dock keeps its added modules, their table, its settings and an
update's staging. So fm1-dock copies the calibration out of it **at its first start**, before it writes anything
there, and keeps the copy with its settings (the log says `BLE: the radio's trims kept`). It looks in the whole
region: where the stock firmware keeps its store on a unit is not measured.

So install fm1-dock **from the stock firmware**, after the stock firmware has run at least once: then the calibration is
there. If the keyboard ran another firmware that wrote over that region, BLUETOOTH says **NO CAL**: install the stock
firmware with M-Vave's updater, switch the keyboard on once, then install fm1-dock again.

What is known, and how (fm1-emu runs the stock firmwares of both on the same chip model):

- Of the four stored records the radio needs, three (the crystal and amplifier trims) are not per unit: stock FM-1 V15
  and stock SMK-37 v016 write the same values on a blank flash in fm1-emu, and they are the values read from a real
  FM-1. Both firmwares carry byte-identical Bluetooth configuration.
- The fourth (187) is the radio's own calibration result, measured by the stock firmware on the unit, which it
  measures again when it is missing: a return to stock loses nothing.
- Not yet tried: the radio of a real SMK-37 started by fm1-dock.

## Not yet on the SMK-37

- **MIDI in from a jack**: the SMK-37 has none (the FM-1's MIDI in pin is a key matrix row there). USB-MIDI works.
- **The boot slot** (a developer's tool) and **solo apps** (FM-1 SID as its own firmware).
- The first click of a knob after power-on may not count (the encoder decoder settles on it).

## Build

```sh
./build.sh --board smk37                    # build/fm1-dock-smk37.fwsc (SMK-37 Pro)
./build.sh --board smk37 --model elite      # build/fm1-dock-smk37-elite.fwsc; --model mke: the MKE-P37's
./build.sh                                  # the FM-1's last: the boards share the build's intermediate files
tests/smk_test.py                           # in fm1-emu (FM1_LIVE)
```

Everything for the SMK-37 is behind `BOARD_SMK37`: the FM-1's build is the same, byte for byte, as without it.

## Credits

The SMK-37 board is genkigenki's work: the [fm1-to-smk37](https://github.com/genkigenki/fm1-to-smk37) kit (the
hardware map, verified on a unit) and its reference port, SLOOP's `smk37` branch, from which fm1-dock takes the key
matrix and velocity (`firmware/hal/smk_input.h`), the flash map and its guards, the LCD's setup, the pad LEDs, the
update loader's and the installers' rules for the SMK-37's models.
