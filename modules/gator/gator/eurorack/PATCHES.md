# Vendored sources

| | |
|---|---|
| `plaits/` | https://github.com/pichenettes/eurorack, commit `08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4` (2023-08-16), `plaits/dsp/**`, `plaits/resources.*` |
| `stmlib/` | https://github.com/pichenettes/stmlib, commit `d18def816c51d1da0c108236928b2bbd25c17481` (2023-09-03), the files Plaits' DSP uses |

Copyright Emilie Gillet, MIT license (`stmlib/LICENSE`; the same notice heads every file).
Only the DSP code is taken: no drivers, UI, settings or bootloader.
Following the authors' guidelines for derivative works, the firmware built from it does not carry the
"Mutable Instruments" name or the module's name.

## Changes

1. `plaits/dsp/dsp.h`: `kSampleRate` and `kCorrectedSampleRate` = `FW_FS_HZ` from `firmware/sys/fw_rate.h`, the real
   FM-1 codec rate of the build (`dsp.h` includes `fw_rate.h`): 44117.6 or, with `--fs 48`, 48076.9 Hz (was 48000 / 47872.34).
2. `plaits/user_data.h`: replaced by a stub without flash access (`ptr()` returns `NULL`): the 6-op FM engines use
   their built-in DX7 banks, no user wavetables / patches.
