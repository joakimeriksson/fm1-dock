# fm1-dock: roadmap

Where fm1-dock is going, in the order it is planned. What is done is in the
[releases](https://github.com/joakimeriksson/fm1-dock/releases); how it is built is in [PLAN.md](PLAN.md).

## Now: 0.8 (released 2026-11-07)

15 synths (CS-1, Scintilla, OberVave built in; 12 add-ons), Bluetooth MIDI both ways, both CPU cores, the master
EQ and the MIDI options, the Test MIDI page, the FM-1 and the SMK-37 (Pro, Elite, MKE-P37). Played on a real FM-1
and SMK-37.

## Next: 0.8.x

- **FMINI-1** ([#63](https://github.com/joakimeriksson/fm1-dock/pull/63)): FMini-1's engine, its 200 patches and its
  effects with an arpeggiator, as an add-on. Played on an SMK-37; its author is asked to have a look.
- **The player's guide** ([#62](https://github.com/joakimeriksson/fm1-dock/pull/62)): every control of the FM-1 and
  the SMK-37, OPTIONS, each synth's first 30 seconds, troubleshooting.
- **The second core after an install**: the first start after installing over another firmware can show 1CPU
  until a power-off (a marker in RAM that the other firmware left). fm1-dock should trust it only after it has
  run itself since power-on.
- **The SMK-37's own controls**: KNOB BANK, FADER BANK, `<` and `>` now reach a synth that asks for them
  (`HB_F_XBTN`, [#66](https://github.com/joakimeriksson/fm1-dock/pull/66)); Scintilla uses them for its fader banks,
  layers and pages. Next per synth: B3 ORGAN's drawbars on the faders, the pads as the synth's own events.
- **Windows** ([#50](https://github.com/joakimeriksson/fm1-dock/issues/50)): the port pairing in 0.8 has not been
  tried on a Windows PC yet.
- **TRS MIDI**: the jack's fix (0.7) checked with a USB-MIDI interface, with the Test MIDI page.

## 0.9: the open dock

The FM-1's flash below the add-ons is almost full with the built-in synths, and the add-ons share 296 KiB: two
large ones can fill it. 0.9 makes every synth an add-on.

1. **An empty dock**: the firmware carries only the sine. CS-1, Scintilla and OberVave become add-ons like the
   others, removable ([#38](https://github.com/joakimeriksson/fm1-dock/issues/38)).
2. **About 500 KiB for add-ons on the FM-1** instead of 296: a second add-on area where the built-ins were (the
   SMK-37 already has two areas).
3. **The free room joined**: add-ons moved together, so that one that fits in total fits in one piece (today
   Niagara Drift beside PROPHET can leave too little in one piece for B3 ORGAN). Until then the page says which
   add-on to remove.
4. **Safe migration**: from 0.7 / 0.8, no installed add-on is lost, and CS-1, Scintilla and OberVave come over or
   are offered on the page. Tested in fm1-emu first (an older version with add-ons, a power cut half way), then on
   an FM-1 and an SMK-37.
5. **Packs on the page**, one click each, the flash meter showing what fits:
   - **Analog**: Scintilla, CS-1, OberVave, PROPHET, FM-106, FMINI-1
   - **Digital & chip**: FM-1 SID, GATOR, ARTIHIVE, FM6
   - **Ambient & texture**: AMB-1, Niagara Drift, FoMni, MONK-1, DUB-1
   - **Keys & acoustic**: B3 ORGAN, FiMba-1
   - **Empty**: pick your own
6. **On the Firmware Hub**: one package, and maybe one "with the classics" (CS-1, Scintilla and OberVave already
   added) for those who install from the Hub.

**With it**: the host's reverb (the processing section), one for every synth, so a synth need not carry its own:
it wants a little of the lean firmware's flash, and Scintilla's build uses it instead of its own reverb.

## Later

- **SEVENTIES** (seventies-fm1): the synths of a Jarre record in one module, so they play together: the Eminent
  310's strings through a Small Stone, an ARP 2600 lead, a second 2600 with its sequencer for the bass, the Korg
  Mini Pops 7 and a tape echo, on the dock's reverb. Its engines are also modules of their own: ARP 2600,
  MINIPOPS, EMINENT.
- **More synths**: a second look through the Firmware Hub (Elixir, Smileyface, the Dinghy-based ones); the ones too
  big today may fit in 0.9's room. Toward 20.
- **A CMI-style sampler**: 8 voices of 8-bit samples at each voice's own rate, the Fairlight's harmonic synthesis
  and drawn waves. Its famous sounds cannot be shipped (Fairlight's library): an original set, and your own.
- **A Bluetooth keyboard straight into the FM-1** (the central role): no computer between.
- **Split and layer**: two synths at once (the SMK-37's outer keys for a bass). RAM and CPU decide.
- **Videos**: a real-FM-1 tour, a chapter per synth, with 0.9's packs.
