/* SPDX-License-Identifier: GPL-3.0-only */
/* The Synthex engine's build options, the same for every file that sees its types (engines/sc_*.h change with
 * them): included before any engines/sc_ header.
 *   SC_VOICES 2   one layer of two voices: a lead line with its releases overlapping (the laser harp is played a beam,
 *                 a note, at a time; four voices beside the strings measured 79.6 % of a block)
 *   SC_LANES 1    one render lane: one Synthex, on one core
 * Not SC_SMALL_OSC: it runs every oscillator through the synced loop, 10-18 % of a block more (dock's module.cfg).
 * SC_HOST_REVERB is for an engine that has the switch; the one taken now (dock's origin/main) always carries its
 * reverb (64 KB of RAM), which the adapter switches off in every patch: the session's reverb serves every part. */
#ifndef MD_SYNTHEX_CONFIG_H
#define MD_SYNTHEX_CONFIG_H
#ifndef SC_VOICES
#define SC_VOICES 2
#endif
#ifndef SC_LANES
#define SC_LANES 1
#endif
#ifndef SC_HOST_REVERB
#define SC_HOST_REVERB 1
#endif
#endif
