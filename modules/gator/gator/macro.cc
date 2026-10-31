// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 npi3pak
#include <new>
#include <algorithm>
#include <math.h>

#include "plaits/dsp/voice.h"
#include "stmlib/utils/buffer_allocator.h"
#include "stmlib/utils/random.h"
#include "macro.h"

#ifdef FW_HOST
#define MACRO_POOL
#else
#define MACRO_POOL __attribute__((section(".pool")))
#endif

using plaits::Voice;
using stmlib::Random;

namespace {

const size_t kSharedSize = 16384;
const uint32_t kEvents = 32;
const uint32_t kNoteStack = 16;
const float kDt = static_cast<float>(plaits::kBlockSize) / plaits::kSampleRate;

enum { EV_OFF = 0, EV_ON = 1, EV_ALL_OFF = 2 };
enum { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_RELEASE };

char shared_buffer[kSharedSize] __attribute__((aligned(8))) MACRO_POOL;
uint8_t voice_storage[sizeof(Voice)] __attribute__((aligned(8))) MACRO_POOL;
Voice* voice;

Voice::Frame block[plaits::kMaxBlockSize];
size_t block_pos = plaits::kBlockSize;

volatile uint32_t events[kEvents];
volatile uint32_t events_w, events_r;
volatile uint8_t ctl_mod_wheel, ctl_aftertouch;
volatile int16_t ctl_bend;

uint8_t note_stack[kNoteStack];
uint32_t note_count;
float note_target = 60.0f, note_current = 60.0f;
float velocity = 0.8f, random_value;
bool gate;
int retrigger_blocks;

struct Lfo {
    float phase, held, previous;
};
Lfo lfo[MACRO_LFOS];

struct Env {
    int stage;
    float value;
};
Env env;

struct Cached {
    int16_t key;
    float value;
};
Cached lfo_hz[MACRO_LFOS], env_rate[3], glide_coef;
float last_src[SRC_COUNT];

macro_monitor_t monitor;

inline float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

float cached(Cached* c, int16_t key, float lo, float log_ratio) {
    if (c->key == key && c->value != 0.0f)
        return c->value;
    c->key = key;
    c->value = lo * expf(log_ratio * static_cast<float>(key) * 0.001f);
    return c->value;
}

// env times as the UI shows them (app.c env_time_us): 1 ms * 10^(4 key / 1000); attack is a linear rise over it,
// decay and release fall exponentially and reach -60 dB of the distance in it
float env_seconds(int16_t key) { return 0.001f * expf(9.2103f * static_cast<float>(key) * 0.001f); }

float env_rise(Cached* c, int16_t key) {
    if (c->key == key && c->value != 0.0f)
        return c->value;
    c->key = key;
    c->value = std::min(1.0f, kDt / env_seconds(key));
    return c->value;
}

float env_fall(Cached* c, int16_t key) {
    if (c->key == key && c->value != 0.0f)
        return c->value;
    c->key = key;
    c->value = 1.0f - expf(-6.9078f * kDt / env_seconds(key));
    return c->value;
}

// Plaits takes LEVEL as a CV and compresses it (1.3 x / (0.3 + x), full gain from 0.77 up): the inverse makes the
// matrix's LEVEL the gain itself, so an envelope's whole range is heard
float level_cv(float gain) {
    const float g = clampf(gain, 0.0f, 1.0f);
    return 0.3f * g / (1.3f - g);
}

float bipolar_random() { return Random::GetFloat() * 2.0f - 1.0f; }

float sine_approx(float phase) {
    const float x = phase < 0.5f ? phase * 4.0f - 1.0f : 3.0f - phase * 4.0f;
    return x * (1.5f - 0.5f * x * x);
}

float lfo_shape(const Lfo& l, int shape) {
    const float p = l.phase;
    if (shape == LFO_SINE)
        return sine_approx(p < 0.75f ? p + 0.25f : p - 0.75f);
    if (shape == LFO_TRI)
        return p < 0.5f ? p * 4.0f - 1.0f : 3.0f - p * 4.0f;
    if (shape == LFO_SAW)
        return 1.0f - p * 2.0f;
    if (shape == LFO_SQUARE)
        return p < 0.5f ? 1.0f : -1.0f;
    if (shape == LFO_SH)
        return l.held;
    return l.previous + (l.held - l.previous) * p;
}

float lfo_process(Lfo* l, int shape, float hz) {
    l->phase += hz * kDt;
    if (l->phase >= 1.0f) {
        l->phase -= static_cast<float>(static_cast<int>(l->phase));
        l->previous = l->held;
        l->held = bipolar_random();
    }
    return lfo_shape(*l, shape);
}

float env_process(const macro_patch_t& p) {
    const float sustain = clampf(static_cast<float>(p.env_s) * 0.001f, 0.0f, 1.0f);
    if (env.stage == ENV_ATTACK) {
        env.value += env_rise(&env_rate[0], p.env_a);
        if (env.value >= 1.0f) {
            env.value = 1.0f;
            env.stage = ENV_DECAY;
        }
        return env.value;
    }
    if (env.stage == ENV_DECAY)
        env.value += (sustain - env.value) * env_fall(&env_rate[1], p.env_d);
    if (env.stage == ENV_RELEASE) {
        env.value -= env.value * env_fall(&env_rate[2], p.env_r);
        if (env.value < 0.001f) {
            env.value = 0.0f;
            env.stage = ENV_IDLE;
        }
    }
    return env.value;
}

void note_remove(uint8_t note) {
    uint32_t w = 0;
    for (uint32_t i = 0; i < note_count; i++) {
        if (note_stack[i] != note)
            note_stack[w++] = note_stack[i];
    }
    note_count = w;
}

void gate_on(const macro_patch_t& p, const macro_global_t& g) {
    if (gate && !g.legato)
        retrigger_blocks = 2;
    if (!gate || !g.legato) {
        env.stage = ENV_ATTACK;
        random_value = bipolar_random();
        for (int i = 0; i < MACRO_LFOS; i++) {
            if (p.lfo_sync[i])
                lfo[i].phase = 0.0f;
        }
    }
    gate = true;
}

void gate_off() {
    gate = false;
    if (env.stage != ENV_IDLE)
        env.stage = ENV_RELEASE;
}

void handle_event(uint32_t ev, const macro_patch_t& p, const macro_global_t& g) {
    const uint32_t type = ev & 0xFFu;
    const uint8_t note = static_cast<uint8_t>(ev >> 8);
    const uint8_t vel = static_cast<uint8_t>(ev >> 16);
    if (type == EV_ALL_OFF) {
        note_count = 0;
        gate_off();
        return;
    }
    if (type == EV_ON) {
        note_remove(note);
        if (note_count == kNoteStack)
            note_remove(note_stack[0]);
        note_stack[note_count++] = note;
        note_target = static_cast<float>(note);
        velocity = static_cast<float>(vel) / 127.0f;
        gate_on(p, g);
        return;
    }
    note_remove(note);
    if (note_count) {
        note_target = static_cast<float>(note_stack[note_count - 1]);
        return;
    }
    gate_off();
}

void render_block(const macro_patch_t& p, const macro_global_t& g) {
    float src[SRC_COUNT];
    float dst[DST_COUNT];
    uint32_t patched = 0;
    plaits::Patch patch;
    plaits::Modulations mod;

    while (events_r != events_w) {
        handle_event(events[events_r % kEvents], p, g);
        events_r = events_r + 1;
    }

    const float glide = p.glide <= 0 ? 1.0f : cached(&glide_coef, p.glide, 1.0f, -9.21f);
    note_current += (note_target - note_current) * glide;

    float lfo_mod[MACRO_LFOS] = {0.0f, 0.0f};
    for (int i = 0; i < MACRO_ROUTES; i++) {
        const macro_route_t& r = p.route[i];
        if ((r.dst == DST_LFO1 || r.dst == DST_LFO2) && r.src > SRC_OFF && r.src < SRC_COUNT)
            lfo_mod[r.dst - DST_LFO1] += last_src[r.src] * static_cast<float>(r.amount) * 0.01f;
    }

    src[SRC_OFF] = 0.0f;
    for (int i = 0; i < MACRO_LFOS; i++) {
        const float hz = cached(&lfo_hz[i], p.lfo_rate[i], 0.02f, 7.6f) *
                         expf(2.7726f * clampf(lfo_mod[i], -1.0f, 1.0f));
        src[SRC_LFO1 + i] = lfo_process(&lfo[i], p.lfo_shape[i], hz);
    }
    src[SRC_ENV] = env_process(p);
    src[SRC_VEL] = velocity;
    src[SRC_NOTE] = clampf((note_current - 60.0f) / 60.0f, -1.0f, 1.0f);
    src[SRC_MODW] = static_cast<float>(ctl_mod_wheel) / 127.0f;
    src[SRC_AFT] = static_cast<float>(ctl_aftertouch) / 127.0f;
    src[SRC_RAND] = random_value;
    src[SRC_GATE] = gate ? 1.0f : 0.0f;
    std::copy(&src[0], &src[SRC_COUNT], &last_src[0]);

    std::fill(&dst[0], &dst[DST_COUNT], 0.0f);
    for (int i = 0; i < MACRO_ROUTES; i++) {
        const macro_route_t& r = p.route[i];
        if (r.src <= SRC_OFF || r.src >= SRC_COUNT || r.dst <= DST_OFF || r.dst >= DST_COUNT || !r.amount)
            continue;
        dst[r.dst] += src[r.src] * static_cast<float>(r.amount) * 0.01f;
        patched |= 1u << r.dst;
    }
    if (p.mode == MODE_GATE && !((patched >> DST_LEVEL) & 1u)) {
        dst[DST_LEVEL] = src[SRC_ENV];
        patched |= 1u << DST_LEVEL;
    }

    const float bend = static_cast<float>(ctl_bend) / 8192.0f * static_cast<float>(g.bend_range);
    patch.note = note_current + static_cast<float>(p.transpose) + bend + static_cast<float>(g.fine) * 0.01f;
    patch.harmonics = clampf(static_cast<float>(p.harmonics) * 0.001f, 0.0f, 1.0f);
    patch.timbre = clampf(static_cast<float>(p.timbre) * 0.001f, 0.0f, 1.0f);
    patch.morph = clampf(static_cast<float>(p.morph) * 0.001f, 0.0f, 1.0f);
    patch.frequency_modulation_amount = (patched >> DST_PITCH) & 1u ? 1.0f : static_cast<float>(p.fm_amt) * 0.01f;
    patch.timbre_modulation_amount = (patched >> DST_TIMBRE) & 1u ? 1.0f : static_cast<float>(p.timbre_amt) * 0.01f;
    patch.morph_modulation_amount = (patched >> DST_MORPH) & 1u ? 1.0f : static_cast<float>(p.morph_amt) * 0.01f;
    patch.engine = std::max(0, std::min(MACRO_ENGINES - 1, static_cast<int>(p.engine)));
    patch.decay = (patched >> DST_LEVEL) & 1u ? 0.0f : clampf(static_cast<float>(p.decay) * 0.001f, 0.0f, 1.0f);
    patch.lpg_colour = clampf(static_cast<float>(p.colour) * 0.001f, 0.0f, 1.0f);

    mod.engine = dst[DST_MODEL] * (8.0f / 23.0f);
    mod.note = 0.0f;
    mod.frequency = dst[DST_PITCH] * 12.0f;
    mod.harmonics = dst[DST_HARM];
    mod.timbre = dst[DST_TIMBRE];
    mod.morph = dst[DST_MORPH];
    mod.level = level_cv(dst[DST_LEVEL]);
    mod.trigger = p.mode != MODE_DRONE && gate && retrigger_blocks == 0 ? 1.0f : 0.0f;
    mod.frequency_patched = (patched >> DST_PITCH) & 1u;
    mod.timbre_patched = (patched >> DST_TIMBRE) & 1u;
    mod.morph_patched = (patched >> DST_MORPH) & 1u;
    mod.trigger_patched = p.mode != MODE_DRONE;
    mod.level_patched = (patched >> DST_LEVEL) & 1u;
    if (retrigger_blocks > 0)
        retrigger_blocks--;

    voice->Render(patch, mod, block, plaits::kBlockSize);

    for (int i = 0; i < SRC_COUNT; i++)
        monitor.src[i] = static_cast<int16_t>(clampf(src[i], -1.0f, 1.0f) * 1000.0f);
    for (int i = 0; i < DST_COUNT; i++)
        monitor.dst[i] = static_cast<int16_t>(clampf(dst[i], -30.0f, 30.0f) * 1000.0f);
    monitor.engine = static_cast<int16_t>(voice->active_engine());
    monitor.note10 = static_cast<int16_t>(patch.note * 10.0f);
    monitor.gate = gate;
    monitor.patched = static_cast<uint8_t>(patched >> 1);
}

void post(uint32_t ev) {
    if (events_w - events_r >= kEvents)
        return;
    events[events_w % kEvents] = ev;
    __asm__ volatile("" ::: "memory");
    events_w = events_w + 1;
}

}

extern "C" void macro_init(void) {
    stmlib::BufferAllocator allocator(shared_buffer, kSharedSize);
    voice = new (voice_storage) Voice();
    voice->Init(&allocator);
    Random::Seed(0x21436587u);
    block_pos = plaits::kBlockSize;
}

extern "C" void macro_render(const macro_patch_t* p, const macro_global_t* g, int32_t* lr, uint32_t frames) {
    const float blend = clampf(static_cast<float>(p->blend) * 0.01f, 0.0f, 1.0f);
    const float spread = clampf(static_cast<float>(p->spread) * 0.01f, 0.0f, 1.0f);
    const float volume = clampf(static_cast<float>(g->volume) / 256.0f, 0.0f, 1.0f);
    for (uint32_t i = 0; i < frames; i++) {
        if (block_pos >= plaits::kBlockSize) {
            render_block(*p, *g);
            block_pos = 0;
        }
        const float out = static_cast<float>(block[block_pos].out);
        const float aux = static_cast<float>(block[block_pos].aux);
        const float mono = out + (aux - out) * blend;
        lr[2 * i] = static_cast<int32_t>((mono + (out - mono) * spread) * volume);
        lr[2 * i + 1] = static_cast<int32_t>((mono + (aux - mono) * spread) * volume);
        block_pos++;
    }
}

extern "C" void macro_note_on(uint8_t note, uint8_t velocity) {
    post(EV_ON | static_cast<uint32_t>(note & 127u) << 8 | static_cast<uint32_t>(velocity & 127u) << 16);
}

extern "C" void macro_note_off(uint8_t note) { post(EV_OFF | static_cast<uint32_t>(note & 127u) << 8); }

extern "C" void macro_all_notes_off(void) { post(EV_ALL_OFF); }

extern "C" void macro_controller(uint8_t mod_wheel, uint8_t aftertouch, int16_t bend) {
    ctl_mod_wheel = mod_wheel;
    ctl_aftertouch = aftertouch;
    ctl_bend = bend;
}

extern "C" void macro_monitor(macro_monitor_t* m) { *m = monitor; }
