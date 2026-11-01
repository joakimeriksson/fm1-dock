/* SPDX-License-Identifier: GPL-3.0-only */
/* What Melodee's engine reads of its factory bank (Melodee: tools/gen_prophet_factory.py's output, Sequential's 200
 * v1.03 programs, bit-packed): here none. Melodee carries those programs as Sequential's, no licence granted, so this
 * module does not; a player sends their own copy of a bank to it (prophet_module.c, HB_F_DATA). The engine's
 * p5_factory / p5_preset_make stay unused. */
#define P5_FACTORY_N 0u
#define P5_FACTORY_BYTES 0u
static const uint8_t P5_FPACK[1];
static const uint8_t P5_FWIDTH[P5_RAW_MAX];
static const uint8_t P5_FBASE[P5_RAW_MAX];
