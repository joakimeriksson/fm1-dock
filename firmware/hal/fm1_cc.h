/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* FM1_INLINE: register helpers that must inline into their caller, so the
 * SFR access sequence (and its timing, in ISRs and IRQ-off sections) is the
 * one written at the call site; -Os would otherwise out-line a helper with
 * several callers. RAM-resident code (.ram_text) may only use these, never
 * a called helper (build.py checks .ram_text for calls). */
#pragma once

#define FM1_INLINE static inline __attribute__((always_inline))

/* The board: 0 = M-Vave FM-1 (default), 1 = M-Vave SMK-37 (Pro / Elite, MKE-P37: the FM-1's AC791N, SPL
 * and update loader behind a different panel; genkigenki's fm1-to-smk37 kit). tools/build.py --board
 * smk37 sets it; SMK_MODEL is the model name the build reports (--model elite / mke). */
#ifndef BOARD_SMK37
#define BOARD_SMK37 0
#endif
#if BOARD_SMK37 && !defined(SMK_MODEL)
#define SMK_MODEL "SMK-37 Pro"
#endif
