<p align="center">
  <img src="docs/img/gator.jpg" alt="GATOR" width="320">
</p>

<h1 align="center">GATOR</h1>

<p align="center">
  A macro-oscillator firmware for the M-VAVE FM-1, inspired by Mutable Instruments Plaits<br>
  <a href="https://npi3pak.github.io/fm1-gator/"><b>Web installer</b></a> ·
  <a href="docs/MANUAL.md">Manual</a> ·
  <a href="https://github.com/npi3pak/fm1-gator/releases">Releases</a> ·
  <a href="BUILDING.md">Building</a>
</p>

<p align="center">
  <a href="https://github.com/npi3pak/fm1-gator/releases/latest"><img alt="release" src="https://img.shields.io/github/v/release/npi3pak/fm1-gator?sort=semver"></a>
  <a href="LICENSE"><img alt="license" src="https://img.shields.io/badge/license-GPL--3.0--only-blue"></a>
</p>

GATOR (as in alligator) turns the M-VAVE FM-1 into a monophonic macro-oscillator synth, inspired by Mutable
Instruments Plaits. Its sound engine is the open-source DSP code of Emilie Gillet's Plaits (MIT), unchanged, with the
module's CV inputs replaced by a modulation matrix and its knobs by the FM-1's encoders. GATOR is an independent
project, not affiliated with or endorsed by Mutable Instruments.

- 24 synthesis models in 3 banks: virtual analog, waveshaping, FM (2-op and DX7-style 6-op), formant, harmonic,
  wavetable, chords, speech, granular, noise, physical models and drums
- a low-pass gate with DECAY and COLOUR, OUT / AUX mix and stereo spread
- an ADSR envelope (1 ms … 10 s), two LFOs with six shapes, an 8-slot modulation matrix
  (LFO, envelope, velocity, note, mod wheel, aftertouch, random, gate)
- GATE (the note sounds while held), TRIG (plucked, as the module) and DRONE play modes, HOLD, glide, legato
- 16 presets in flash, USB-MIDI and TRS MIDI in
- 48 kHz audio

![The pages](docs/img/gator-pages.png)

> [!WARNING]
> **Installing custom firmware is at your own risk.** GATOR is unofficial firmware, provided as is, without any
> warranty (GPL-3.0 sections 15 and 16). It is not made, endorsed or supported by M-VAVE. A failed or interrupted
> install can leave the FM-1 unable to start until it is recovered with a USB tool. You can return to the official
> firmware at any time.

## Install

1. Charge the FM-1 and plug it straight into the computer with a data cable. Close apps that use MIDI.
2. Open the **[web installer](https://npi3pak.github.io/fm1-gator/)** in Chrome or Edge (Safari and Firefox have no
   Web MIDI with SysEx).
3. Press **Install**. Leave the cable alone until it says Done.

From the command line, with a package from [Releases](https://github.com/npi3pak/fm1-gator/releases):

```bash
pip install mido python-rtmidi
python3 tools/fm1_install.py gator-0.1.fwsc
```

**Back to the official firmware:** download FM-1 V15 from [m-vave.com/download](https://www.m-vave.com/download) and
write it with the web installer's "Back to the official firmware". GATOR uses the Felucca platform's update loader,
so other Felucca-platform firmware (SLOOP, ORBIT, ChoralRoot, AMB-1) installs over it the usual way.

**If something goes wrong:** an interrupted write resumes when you press Install again. After two failed boots in a
row the FM-1 opens USB RESCUE by itself (or hold OCT− at power-on), and the installer works there too. An FM-1 that
does not start at all can be recovered with [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker).

## Use

See the **[manual](docs/MANUAL.md)**: the pages, the buttons, the modulation matrix, the 24 models, MIDI and the
factory presets.

## Build

See **[BUILDING.md](BUILDING.md)**: the toolchain, `./build.sh`, the host simulator and tests, and how a release is
made. [docs/DEVELOPING.md](docs/DEVELOPING.md) describes the platform and its app API, for writing your own FM-1
firmware on it.

## Credits

GATOR is made by [npi3pak](https://github.com/npi3pak). It stands on the work of:

- **Emilie Gillet**: [Plaits](https://github.com/pichenettes/eurorack) and stmlib, the synthesis DSP (MIT).
  Not affiliated with or endorsed by Mutable Instruments.
- **Leo Kuroshita, Hügelton Instruments**: [Felucca](https://github.com/hugelton/Felucca), the FM-1 platform: HAL,
  update loader, USB-MIDI, OTA, storage and installers (GPL-3.0).
- **dspaudio**: [ORBIT](https://github.com/dspaudio/orbit), the platform GATOR was cut down from.
- **isod89**: [SLOOP](https://github.com/isod89/sloop-fm1), the web install protocol and the stock supply levels.
- **Quixotic7**: [ChoralRoot](https://github.com/Quixotic7/ChoralRootFM1), the audio interrupt fix and the release
  layout; [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker).
- **Charles Vestal**: [AMB-1](https://github.com/charlesvestal/fm1-amb), which showed the FPU in use on the FM-1.
- **Dimitar Toshkov Zhekov**: the Terminus font (SIL OFL 1.1).
- **JieLi Technology**: three AC79 SDK files in every package (Apache-2.0).

## License

GATOR is free software under the [GNU General Public License v3.0 only](LICENSE), provided without any warranty.
The parts it is built from keep their licences and copyright notices:

| Part | Author | Licence | Text |
|---|---|---|---|
| GATOR (the app, web installer, release tooling, logo) | npi3pak | GPL-3.0-only | [LICENSE](LICENSE) |
| Felucca platform: HAL, update loader, USB-MIDI, OTA, storage, tools (through ORBIT and SLOOP, modified) | Leo Kuroshita, Hügelton Instruments | GPL-3.0-only | [LICENSE](LICENSE) |
| Web install protocol `fm1ota.js`, `fm1pkg.js` (from SLOOP 2.7, unchanged) | Leo Kuroshita, Hügelton Instruments; isod89 | GPL-3.0-only | [LICENSE](LICENSE) |
| Plaits and stmlib DSP (`third_party/eurorack`, changes in `PATCHES.md`) | Emilie Gillet | MIT | [LICENSES/MIT-Plaits-stmlib.txt](LICENSES/MIT-Plaits-stmlib.txt) |
| Terminus font (rasterised into the firmware) | Dimitar Toshkov Zhekov | SIL OFL 1.1 | [LICENSES/OFL-Terminus.txt](LICENSES/OFL-Terminus.txt) |
| Three AC79 SDK files in every package (`tools/sdk`, unmodified) | JieLi Technology | Apache-2.0 | [LICENSES/Apache-2.0-JieLi-SDK.txt](LICENSES/Apache-2.0-JieLi-SDK.txt) |

Every release and the web installer carry these licence texts next to the packages, and each release's complete
source is its tag in this repository. [LICENSING.md](LICENSING.md) has the details.

"M-VAVE" and "FM-1" are trademarks of their respective owners. GATOR is not affiliated with, endorsed by or
supported by them.
