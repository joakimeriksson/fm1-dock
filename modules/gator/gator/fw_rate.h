/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 npi3pak */
/* The codec rate: ALNK0 MCLK = 160 MHz / 13 (48 kHz, the default) or 192 MHz / 17 (44.1 kHz family, FW_FS48=0),
 * fs = MCLK / 256.
 * Plain macros: C++ (third_party/eurorack/plaits/dsp/dsp.h) includes this too. */
#ifndef FW_RATE_H
#define FW_RATE_H

#ifndef FW_FS48
#define FW_FS48 1
#endif
#if FW_FS48
#define FW_MCLK_HZ 160000000u
#define FW_MCLK_DIV 13u
#else
#define FW_MCLK_HZ 192000000u
#define FW_MCLK_DIV 17u
#endif
#define FW_FS ((FW_MCLK_HZ + FW_MCLK_DIV * 128u) / (FW_MCLK_DIV * 256u))
#define FW_FS_HZ ((float)FW_MCLK_HZ / ((float)FW_MCLK_DIV * 256.0f))
#define FM1_AUDIO_48K FW_FS48

#endif
