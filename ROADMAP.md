# fm1-dock: roadmap

Where fm1-dock is going, in the order it is planned. What is done is in the
[releases](https://github.com/joakimeriksson/fm1-dock/releases); how it is built is in [PLAN.md](PLAN.md).

## Now: 1.0 (released 2026-11-25)

Every synth is an add-on: 29 in the catalog and eight effects, with up to 744 KiB for add-ons on an FM-1 (the
Empty dock). Presets in a file, `.syx` for FM6 and PROPHET, data packs, the SMK-37's own keys and pads, MIDI clock
and transport, and the newer FM-1s' display. Played on a real FM-1 and SMK-37. The plans of 0.8 and 0.9 (the open
dock, the packs, the room) are done; the [releases](https://github.com/joakimeriksson/fm1-dock/releases) say what
each version brought.

## Next: 1.0.x

- **VECTOR37 on the page**: the module and its wavetable pack (V37-WT) work with 1.0 (`dock.py deploy`, the pack
  first), but the page cannot yet add a synth together with its pack, so VECTOR37 has no card yet.
- **The newer FM-1s' display**: 1.0 sends M-Vave's own display setup; still to be seen on a late-2026 unit.
- **The SMK-37 Elite and MKE-P37, and the newer flash chip** ([#99](https://github.com/joakimeriksson/fm1-dock/issues/99),
  [#112](https://github.com/joakimeriksson/fm1-dock/issues/112)): the installer accepts them; nobody has run 1.0 on
  them yet.

## After 1.0: a fresh catalog

The catalog holds the synths as they were when each was added, and several authors have moved on (checked
2026-11-24 against their repositories: B3 ORGAN 0.2 against 0.6.1, Niagara Drift 0.2.1 against 0.3.1, ARTIHIVE
2.0.3 against 2.0.6, FMINI-1 2.5.13 against 2.6.0, FM-106 Beta 6 against 1.02, DUB-1, ATEK303, AMB-1, FoMni,
MONK-1 and FiMba-1 a release or more behind; [#128](https://github.com/joakimeriksson/fm1-dock/issues/128),
[#113](https://github.com/joakimeriksson/fm1-dock/issues/113)). A refresh needs no new firmware: rebuild a module,
play it on an FM-1, publish it with the page.

1. **A refresh in batches**, B3 ORGAN and Niagara Drift first (people asked), then the ones that moved most
   recently. Each rebuilt with its `take.py`, played on a real FM-1 and checked for the room and the CPU.
2. **The page shows what is stale**: a card says "on your unit: 0.2, in the catalog: 0.6.1", with an Update
   button, so nobody finds out by reading an issue. It needs the installed module's version from the unit.
3. **Say which commit a module is built from** for every entry (CS-1 and OberVave do not yet), so the next check
   is a script and not a search.

## Later

- **ORRERY** past its preview ([orrery-fm1](https://github.com/joakimeriksson/orrery-fm1) 0.1.0 is in the
  catalog: the synths of a Jarre record in one module, so they play together): a run on an FM-1 with both cores,
  then a release. Its engines are also modules of their own: ARP 2600, EMINENT, MINIPOPS, ENSEMBLE.
- **More synths**: a second look through the Firmware Hub (Elixir, Smileyface, the Dinghy-based ones); the ones too
  big before may fit in 1.0's room.
- **A CMI-style sampler**: 8 voices of 8-bit samples at each voice's own rate, the Fairlight's harmonic synthesis
  and drawn waves. Its famous sounds cannot be shipped (Fairlight's library): an original set, and your own.
- **A Bluetooth keyboard straight into the FM-1** (the central role): no computer between.
- **Split and layer**: two synths at once (the SMK-37's outer keys for a bass). RAM and CPU decide.
- **Videos**: after the 1.0 film (made in the emulator), a real-FM-1 tour, a chapter per synth.
