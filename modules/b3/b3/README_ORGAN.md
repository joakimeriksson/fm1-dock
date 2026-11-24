# B3 ORGAN for the M-VAVE FM-1 (on zvenson/dinghy)

A fixed-point tonewheel organ modelled on setBfree. State: **0.5.1, tested on an FM-1 by one person and against a Raspberry Pi running setBfree (see README.md); the host tests run on every build.**

## Build
```
tools/get_toolchain.sh && git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
python3 tools/gen_organ.py      # only if you change the gears or tuning: rewrites firmware/src/organ_tab.h
tests/run_host.sh               # build/host/dinghy.wav + screen PNGs, no FM-1 needed
./build.sh                      # -> build/b3organ.fwsc ; python3 tools/fm1_install.py build/b3organ.fwsc
```
**Identity and version.** This firmware's package identity is `FM-1_953` (`IDENT` in `tools/build.py`), the same for every version: the
FM-1 Firmware Hub takes the name and the version from this repository and its releases and only needs an identity no other firmware uses
(Dinghy itself is `FM-1_99N`; the 95N family is used by other firmwares too, 951, 952 and 958: check the hub's list before you change it).
The version is on the screen, top left: `B3 ORGAN v0.5.1` for a release (`./build.sh --release 0.5.1`,
or a tag `v0.5.1` on GitHub: Actions builds `b3organ-0.5.1.fwsc` and attaches it to the release), `B3 ORGAN vd8362` for any other build:
`d` plus the first four hex digits of the commit it was built from (on GitHub: the commit of the run you downloaded it from).

## Controls
keys (F3..G5, OCT-/OCT+) | SELECT drawbar, ALGORITHM level 0..8 | PRESETS steps through setBfree's registrations
(default.pgm: 57 of them, e.g. 5 Standard B, 11 Brian Auger 3rd, 15 Booker T, 104 Jimmy Smith) |
KNOB1 screen (upper, lower, pedal, test values) | KNOB2 vibrato V1 C1 V2 C2 V3 C3 | KNOB3 overdrive drive 0..20 | KNOB4 reverb amount (0, 1, 2, 4, 8, 16, 32, 64, 127) |
FX vibrato on/off of the screen's manual | LFO Leslie cycle | SEL overdrive on/off | REC the FM-1's keys play upper/lower/pedal |
screen 1 only: ENV percussion | ARP 2nd/3rd | SEQ fast/slow | PLAY soft/normal. EDIT and SAVE are free.

## MIDI (USB and the TRS jack)
Channel 1 plays the upper manual, 2 the lower one, 3 the pedal (setBfree's `midi.upper/lower/pedals.channel` defaults); other channels play
the upper one. Notes fold into C2..C7 (pedal: C1..G3, MIDI 24 to 55). The drawbars 70..78 follow the channel; the pedal has 16' (70) and 8' (72). **Program change** n selects setBfree program n+1 (its `pgm.controller.offset` default), all 110
programs of `default.pgm` are in; a program only changes what it names. Programs for the lower manual, pedals, key
splits are kept but have no effect here (programs set the upper manual). **Controllers** as setBfree's defaults:
1 Leslie slow/stop/fast (0..42 slow, 43..85 stop, 86..127 fast), 7 and 11 swell, 30 vibrato on (lower), 31 (upper), 95 routing (off, lower, upper, both), 64 rotary toggle, 65 overdrive on,
66 and 80 percussion on, 81 soft, 82 fast, 83 2nd harmonic (64 and up), 92 vibrato knob, 93 overdrive drive (0..20), **91 reverb mix** (setBfree's `reverb.mix`, 0..127, default 12),
70..78 drawbars **inverted as in setBfree** (127 = pushed in, 0 = fully out), 120 and 123 notes off.
Not used: 94 convolution. As in setBfree, a program that sets the vibrato knob does not
switch vibrato on by itself (FX button, CC 31 or program 50 do).

**Your own programs 112 to 127.** The web editor (`docs/editor`) stores the whole sound of the FM-1 in one of them: the drawbars of all three
manuals, vibrato type and switches, percussion, Leslie, overdrive (on, drive, character) and reverb, with a name of up to 12 letters. They are
kept in their own flash sector (0xFD000), written when nothing sounds; the PRESETS knob steps through them after setBfree's, program change
111 to 126 selects them, and they set everything they hold (setBfree's programs change only what they name). Program 111 stays free.

**NRPN.** 1/0 (CC 99 = 1, CC 98 = 0, then CC 6 = 0..127): setBfree's `overdrive.character` (16 at start, as fixed until 0.4; saved with
the settings). 2/0 to 2/7 with 14 bits (CC 6 = MSB, then CC 38 = LSB; the value counts when CC 38 arrives): the Leslie, setBfree's `whirl.*` values by
default: 2/0 horn slow, 2/1 horn fast, 2/2 drum slow, 2/3 drum fast in 0.1 rpm (403, 4234, 360, 3573); 2/4 horn acceleration, 2/5 horn
deceleration, 2/6 drum acceleration, 2/7 drum deceleration as the time constant in ms (161, 321, 4127, 1371). Saved with the settings, for all
programs. 3/0 setBfree's `osc.tuning` in 0.1 Hz (4400 = 440 Hz; rounded to whole Hz, 4300..4500), 3/1 `osc.attack.click.level` (the key click) in 0.1 %
(500 = 0.5), 14 bits as well, saved with the settings. An RPN (CC 101 / 100) in between makes data entry go nowhere.

**SysEx (the web editor).** `F0 7D 42 33 CMD ... F7` (7D: the ID for non-commercial use, then "B3"), USB only:

| To the FM-1 | Answer |
| --- | --- |
| `01` read the sound | `41` + upper 9, lower 9, pedal 2 drawbars, vibrato type, upper on, lower on, percussion on, 3rd, fast, soft, Leslie (0 stop, 1 slow, 2 fast), overdrive on, drive, character, reverb, swell, program, keys (0 upper, 1 lower, 2 pedal), the Leslie's 2/0..2/7 the tuning and key click 3/0..3/1 and the equalizer's six values (as `07`) (two bytes each, MSB first), then the version in ASCII |
| `02` read your programs | `42` + number, used (0/1), name (12 bytes, 0 = end), once for each of 112..127 |
| `03` n name[12]: store the sound now as program n | when the flash is written (nothing sounding): `43` n status (0 saved, 1 not 112..127, 2 flash error), then `42` for n |
| `04` n: delete program n | as `03` |
| `05` n: read all of program n (a backup) | `45` + n, used, then 44 bytes: name 12, upper 9, lower 9, pedal 2 drawbars, vibrato type, upper on, lower on, percussion on, 3rd, fast, soft, Leslie, overdrive on, drive, character, reverb |
| `06` n + the 44 bytes of `45`: write program n (a restore) | as `03`; status 3: a value out of range, nothing written |
| `07` the output equalizer: 6 values of 14 bits (bass gain, bass Hz, middle gain, middle Hz, treble gain, treble Hz; gain 0..36 = -9..+9 dB in 0.5 dB steps, 18 = flat; 20..200, 250..3100, 4000..8000 Hz); the FM-1 computes the filters (bytes after the values are ignored) | `47` status (0 taken, 1 out of range); saved with the settings |

**The output equalizer** (firmware 0.6) sits after the Leslie, before the output limiter: a bass shelf, a middle peak and a treble shelf with
Robert Bristow-Johnson's biquads (shelves Q 0.707, the peak Q 1), computed on the FM-1 without floating point (Q29, sine and cosine by their
series; within a few LSB of `tools/eq_coef.py`, at most 0.012 dB apart). A band at 0 dB is not computed, so a flat equalizer is bit-exact and
costs nothing. The levels are on the pedal screen too: SELECT goes past the two pedal drawbars to BASS, MID and TREB, ALGORITHM sets them in
0.5 dB steps; the frequencies only in the web editor.

Regenerate the program table with `python3 tools/gen_programs.py` (reads `data/setbfree-default.pgm`).
`tests/organ_test.c` checks programs and controllers against setBfree's source on a computer, and the level of every key and drawbar against setBfree's own tone generator (`tests/organ_ref.h`).

**Tapering and leakage.** The level of each key and drawbar (setBfree's tapering, -10..+7 dB) and the crosstalk between neighbouring tonewheels (compartment, terminal strip, wiring: -34..-46 dB, setBfree's default model) come from setBfree's compiled play matrix: `tools/dump_contrib.c` dumps it (`data/setbfree-contrib.txt`), `tools/gen_contrib.py` makes `firmware/src/organ_contrib.h`. **Key click and key compression** are setBfree's defaults too: when a wheel's level changes its gain follows one of eight random click envelopes (attack model "click", level 0.5, burst 16..80 samples; the release is a linear ramp), and the output falls by 1.2 dB for 2 keys down to 5 dB for 10 (and 9 dB for 61). Leakage costs wheels: ten keys with all nine drawbars out sound about 49 wheels (check `W .. us` on the display).

## Check this first on the device
The top right of the screen shows `W <wheels sounding>  <peak render time> us`. A block must stay well under
5800 us (RULES.md asks for under 2900). Hold a big chord with many drawbars out and watch it. If it gets close,
lower the wheel count (fewer simultaneous keys, drawbars) or move work out of the per-frame loop.

## Vibrato and chorus
As setBfree's scanner (`vibrato.c`): a delay of 1 + A + A sin(phase) samples, A = 3, 6, 9 for V1/C1 .. V3/C3, at 7.25 Hz; the chorus
mixes the dry and the delayed sound with 1/sqrt(2), the vibrato plays the delayed sound only. setBfree counts these delays in
samples, so they depend on the sample rate; the reference recordings came from a Pi running setBfree at 48 kHz, and the firmware
(44.1 kHz) uses the same delays in seconds. Measured against such a recording (chorus C3, F4 / C5 / F5): pitch swing 14.8 cents
peak-to-peak (setBfree 14.8), amplitude ripple 0.85 / 2.0 / 3.8 dB (setBfree 0.9 / 2.4 / 3.85).

## Percussion
As setBfree's tone generator: the 2nd or 3rd harmonic bus (the 4' or 2 2/3' drawbar's tapering and leakage) times 11 (soft: 0.5012),
through setBfree's high-pass (a first difference: the higher the note, the louder the percussion), an exponential decay, and with
"normal" volume the whole sound 4.4 dB quieter (setBfree's `percDrawbarNormalGain`). The decay is what the plug-in really does: a
factor of 0.9999 (slow) or 0.9995 (fast) per sample, at 48 kHz: -41.7 dB per second slow, about -208 dB per second fast (not the "4 s" and "1 s" of
its documentation, which its plug-in never applies). The 1' drawbar is off while percussion is on, and the envelope restarts when
all keys are up. Level and decay match setBfree's tone generator (`tests/organ_test.c`, 12 combinations of key, harmonic and volume, within 0.4 dB).
The percussion's click is setBfree's too: the first difference also takes the key's attack envelope (a bouncing one, see the key click), so the
wheels' attack comes out as a high-frequency click; the engine adds that term while a wheel's percussion level changes (the first 128 samples
of a key). Measured against the Raspberry Pi (F4, percussion alone, 3rd): the click was 8 dB too weak before, now it is within about 1 dB.

## Overdrive
setBfree's preamp (`b_overdrive/overdrive.c`) in integers (`firmware/src/organ_od.h`): input gain 3.57, a sag that lowers the bias with the level,
an 8-tap interpolation filter, the transfer curve 1 / (1 + x^2) (a 1025-entry table, `OD_T` in `organ_tab.h`) around a bias between a bass
filter before and one after it, global negative feedback, and the 33-tap decimation filter, output gain 0.88. About 20 multiplications per
sample, no division. KNOB 3 / CC 93 set the drive, 0 to 20: setBfree's `overdrive.inputgain` in steps of 1.5 dB. Calibrated against setBfree (the LV2 plug-in on a Raspberry Pi, the same MIDI file, the Leslie fast so that the rotors' stop angle does not count): drive 12 matches setBfree's default within 1 to 2 dB in every third-octave band. Our tone generator feeds the preamp about 6 dB hotter, so drive 12 is setBfree's input gain 3.57 less 6 dB and drive 16 is 3.57 itself (`OD_DRIVE_SETBFREE`; until 0.5.1 drive 12 was 3.57, 6 dB too hot); 20 is 6 dB above that. Settings and your own programs saved before 0.6 (drive 0..16) are raised by 4 steps when loaded, so they sound as before (a program's spare byte marks it as converted); the web editor does the same for a backup file from a release before 0.6. The
character (`overdrive.character`, which sets the two bass filters) is fixed at 16 (`OD_CHARACTER`); until 0.3 the nine steps were the character and the
input gain stood still. The output gain follows the drive (`OD_GOUT`; the preamp alone is 10 to 30 dB quieter than the tone generator) so that five
keys at swell 80 are as loud with the overdrive on as off at every step (`tools/od_drive_levels.c`); other playing stays within about 2 dB. Overdrive off bypasses
the model. A soft clip with a gain stood here until 0.1; it was uniform in level and did not match the Raspberry Pi.
Measured against the Pi (F4 and the chord F4 A4 C5, each step against "off"): within 0.6 dB on a note and 1.4 dB on the chord.
`tests/organ_test.c` compares the integer code with the same model in double precision (41 dB signal to error). The input and output gain
(setBfree's `overdrive.inputgain` and `outputgain`) are fixed at its defaults. What is not compared: the harmonics of a single note at
steps 4 to 6 (the stopped Leslie's random angle hides them).

## Reverb and Leslie
Chain as in setBfree: tone generator, overdrive, **reverb**, **Leslie**. The reverb is setBfree's (four combs, three all-passes,
low-pass, feedback); its impulse response matches setBfree's to -45 dB. The Leslie is setBfree's whirl model: horn and drum
with Doppler shift, the horn's directional filtering, reflections, acceleration and braking between stop, slow and fast.
In the stopped state its output matches setBfree's to about -40 dB; slow matches in waveform, fast in level and spectrum.
The delay lines live in the pool (about 87 KB). Both cost CPU: watch the `W .. us` number on the screen (it covers the whole
audio block); `LES_REFL2 0` in `organ_fx.h` drops the second reflections for about 30 % less Leslie work (less exact).
`tests/organ_test.c` checks both against the data in `tests/fx_ref.h`, `data/setbfree-whirl.txt` (dumped with `tools/dump_whirl.c`,
turned into `organ_leslie.h` by `tools/gen_leslie.py`) holds setBfree's whirl tables. Note: setBfree's LV2 plugin starts with the
Leslie slow, this firmware starts with it stopped.

## What was tuned against the Raspberry Pi
Recordings of the FM-1 and of setBfree on a Raspberry Pi (48 kHz), same notes, one drawbar at 8, Leslie stopped, one effect at a time:
- **Chorus and vibrato:** the delay swings as a sine (it was a triangle) and the chorus mixes at 1/sqrt(2) (it was 1/2, 3 dB too quiet); the swing
  is now 14.8 cents on F4, C5 and F5 (13.1 before), the ripple 0.9 / 2.3 / 3.8 dB against 0.9 / 2.5 / 3.8 dB.
- **Percussion:** level (11 times the bus, soft 0.5012, normal 0.605 on the whole sound), the first-difference high-pass per wheel, the decay per
  sample of the plug-in (0.9999 / 0.9995 at 48 kHz, scaled to 44.1 kHz: -41 dB per second slow, the Pi -42), and the click: the high-pass is
  taken of the envelope too, so the bouncing attack of the wheels (the key click) becomes a click at 2 to 8 kHz. Before, it was 6 dB above the
  tone after it, the Pi 15 dB, now 16 dB (F4, 3rd, nothing else sounding).
- **Reverb:** setBfree's combs and all-passes at the sample rate, mix = CC 91 / 127; the tails of a held F4 at mix 7, 15, 31, 63 agree within
  1 dB. A held F4 gets quieter at the release's first 0.2 s than at the start: setBfree's own comb cancellation at 349 Hz, the Pi does the same (+6 dB at 48 kHz).
- **Leslie:** stop, slow and fast with the acceleration of setBfree; the Raspberry Pi and the FM-1 reach 0.5 to 0.9 Hz (slow) and 5.4 to 5.5 Hz (fast)
  after the same 10 s, with 18 to 22 dB of level ripple at the horn.
- Differences that remain and are not errors: the stopped Leslie's angle is random after every stop (single notes differ by 10 dB or more between
  recordings, and 2nd against 3rd percussion level by up to 20 dB), and the Raspberry Pi's note has more above 2 kHz (its 12th harmonic at
  4190 Hz is 8 dB louder; not understood yet).

## Manuals
The 91 wheels are shared, as on the organ: each wheel is computed once and added to two buses, the swell bus and the vibrato bus (setBfree's
`sgain` and `vgain`), with the levels the manuals' drawbars give it. The lower manual uses the upper one's wiring and tapering (the contribution
table is the same, checked against setBfree's `keyContrib`), the pedal setBfree's pedal wiring (`data/setbfree-contrib-pedal.txt`, only the 16' and
8' buses; the 16' sounds the key's own pitch, as in setBfree). The upper and the lower manual go to the scanner if their vibrato switch is on; the
pedal and the percussion never do (setBfree mixes the swell bus, the scanner's output and the percussion after it). Key compression counts every key
down, the percussion's single trigger only the upper manual's. A wheel on both buses costs one sine and two products. Measured on the host, 10 + 10 + 4 keys
with chorus, percussion, overdrive, Leslie and reverb take 6 % more than 10 upper keys; the extra wheels cost more on the FM-1.

## Not in it (yet)
Tonewheel waveform colouring, setBfree's equalisation curve, the lower and pedal drawbars in programs, key splits.

