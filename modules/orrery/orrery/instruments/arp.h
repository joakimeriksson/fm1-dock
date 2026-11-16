/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MD_ARP_H
#define MD_ARP_H
#include "core/instrument.h"
#include "arp_engine.h"   /* fm1-dock: the engine of this tree (take.py) */
#include "adapter_common.h"
#include "builtin_ids.h"
#define MD_ARP_SETTINGS_VERSION 1u
#define MD_ARP_SETTINGS_BYTES 36u
/* IDs1..12 are the initial curated row; appended IDs expose the full sound.
 * All0..100 controls retain engine-native normalized units (detune50=center).
 * Octaves0..4 use explicit -2..+2 labels; reverb is reserved to the session. */
enum {
    MD_ARP_CUTOFF=1, MD_ARP_RESONANCE, MD_ARP_GLIDE, MD_ARP_LEVEL,
    MD_ARP_FILTER_ENV, MD_ARP_DRIVE, MD_ARP_FILTER_ATTACK, MD_ARP_FILTER_DECAY,
    MD_ARP_AMP_ATTACK, MD_ARP_AMP_RELEASE, MD_ARP_OSC1_LEVEL, MD_ARP_OSC2_LEVEL,
    MD_ARP_OSC1_WAVE, MD_ARP_OSC1_OCTAVE, MD_ARP_OSC1_PULSE_WIDTH,
    MD_ARP_OSC2_WAVE, MD_ARP_OSC2_OCTAVE, MD_ARP_OSC2_DETUNE,
    MD_ARP_NOISE_LEVEL, MD_ARP_RING_LEVEL, MD_ARP_SYNC, MD_ARP_SUB_LEVEL,
    MD_ARP_KEYBOARD_TRACK, MD_ARP_LFO_FILTER, MD_ARP_SAMPLE_HOLD_FILTER,
    MD_ARP_FILTER_SUSTAIN, MD_ARP_FILTER_RELEASE, MD_ARP_AMP_DECAY, MD_ARP_AMP_SUSTAIN,
    MD_ARP_LFO_WAVE, MD_ARP_LFO_RATE, MD_ARP_LFO_PITCH, MD_ARP_LFO_PULSE_WIDTH,
    MD_ARP_ACCENT_AMOUNT, MD_ARP_ENVELOPE_PITCH
};
typedef struct { arp_t engine; md_adapter_voice_t voice; } md_arp_state_t;
extern const md_instrument_desc_t MD_ARP2600;
#endif
