#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// The Mini Pops' tables from the web engine (ai-smarthome/audio/software-synth/js/minipops-processor.js,
// the reference: the SR-95 service manual's voices and rhythms, fitted to recordings), as integer
// coefficients at 44.1 kHz for mp_engine.c:
//   modules/minipops/take.mjs ~/work/ai-smarthome/audio/software-synth/js/minipops-processor.js
// writes mp_tables.h beside this file. Run it again when the web engine changes; tests/minipops_check.py
// renders both and compares them.
import { writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const SR = 44100;
const src = process.argv[2];
if (!src) { console.error('take.mjs PATH/minipops-processor.js'); process.exit(2); }
globalThis.sampleRate = SR;
globalThis.currentTime = 0;
globalThis.AudioWorkletProcessor = class { constructor() { this.port = { onmessage: null, postMessage() {} }; } };
globalThis.registerProcessor = () => {};
const mod = await import(pathToFileURL(resolve(src)).href);
const { VOICE_NAMES, SPEC, RHYTHMS } = mod;
// CAL, OUT_REF and EXTRA are not exported: read them out of the source text
const text = (await import('node:fs')).readFileSync(resolve(src), 'utf8');
const CAL = Function(`return ${text.match(/const CAL = (\{[\s\S]*?\});/)[1]};`)();
const OUT_REF = +text.match(/const OUT_REF = ([\d.]+);/)[1];
const EXTRA = Function(`const all16 = [...Array(16).keys()], all12 = [...Array(12).keys()]; ${text.match(/const EXTRA = (\{[\s\S]*?\n\});/)[0]}; return EXTRA;`)();

const LN10 = Math.log(10);
const q = (x, bits) => Math.round(x * 2 ** bits);
const KIND = { drum: 0, bell: 1, noise: 2, ratchet: 3 };

function resonator(freq, tau) {
  const w = 2 * Math.PI * Math.min(freq, SR * 0.45) / SR;
  const r = Math.exp(-1 / (Math.max(0.0005, tau) * SR));
  return { a1: q(2 * r * Math.cos(w), 28), a2: q(-r * r, 28), gain: q(Math.sin(w), 28) };
}
function bandpass(freq, qf) {
  const g = Math.tan(Math.PI * Math.min(Math.max(freq, 10), SR * 0.45) / SR);
  const k = 1 / Math.max(0.5, qf);
  const a1 = 1 / (1 + g * (g + k));
  return { a1: q(a1, 30), a2: q(g * a1, 30), a3: q(g * g * a1, 30) };
}

const voices = VOICE_NAMES.map((name) => {
  const s = SPEC[name], tau = s.t20 / LN10;
  const v = { name, kind: KIND[s.kind], res: { a1: 0, a2: 0, gain: 0 }, res2: { a1: 0, a2: 0, gain: 0 },
    bp: { a1: 0, a2: 0, a3: 0 }, envMul: 1 << 30, life: 0, period: 1, tickK: 1 << 30, useLp: 0, jitter: 0,
    drive: q(s.drive || 1, 16), c2: 0, click: q(s.click || 0, 16), mix: 0,
    gain: q(s.out / OUT_REF / CAL[name], 16), seed: (0x2545F491 ^ (name.length * 7919)) >>> 0 };
  switch (s.kind) {
    case 'drum':
      v.res = resonator(s.f, tau);
      if (s.body) { v.res2 = resonator(s.body, (s.bodyT20 || s.t20) / LN10); v.mix = q(s.bodyMix || 0.5, 16); }
      v.c2 = q(0.3 * (s.drive - 1), 16);
      v.life = Math.round((tau * 7 + 0.01) * SR);
      break;
    case 'bell':
      v.res = resonator(s.f, tau);
      v.res2 = resonator(s.f2, tau * (s.t2 || 0.4));
      v.mix = q(s.mix2 || 0.45, 16);
      v.drive = q(s.drive || 1.6, 16);
      v.life = Math.round((tau * 7 + 0.01) * SR);
      break;
    case 'noise':
      v.bp = bandpass(s.f, s.q);
      if (s.f2) throw new Error(`${name}: a second noise band is not ported`);
      if (s.body) { v.res = resonator(s.body, s.bodyT20 ? s.bodyT20 / LN10 : 0.012); v.mix = q(s.bodyMix || 0.28, 16); }
      v.envMul = q(Math.exp(-1 / (tau * SR)), 30);
      v.life = Math.round(tau * 7 * SR);
      break;
    case 'ratchet':
      v.bp = bandpass(s.f, s.q || 4);
      v.tickK = s.tickLp ? q(1 - Math.exp(-2 * Math.PI * s.tickLp / SR), 30) : 1 << 30;
      v.useLp = s.tickLp ? 1 : 0;
      v.period = Math.max(1, Math.round(SR / s.rate));
      v.envMul = tau > 0 ? q(Math.exp(-1 / (tau * SR)), 30) : 1 << 30;
      v.life = tau > 0 ? Math.round(tau * 7 * SR) : 0;
      v.jitter = q(s.jitter || 0, 16);
      break;
  }
  return v;
});

// the rhythms: hits as (voice, position x 100); a section's keys all down is the biggest merge
const V = Object.fromEntries(VOICE_NAMES.map((n, i) => [n, i]));
const hits = [], rhythms = [];
for (const sec of ['a', 'b']) {
  for (const r of RHYTHMS[sec]) {
    const first = hits.length;
    for (const name in r.parts) for (const p of r.parts[name]) hits.push([V[name], Math.round(p * 100)]);
    rhythms.push({ name: r.name, len: r.len, div: r.div, first, n: hits.length - first });
  }
}
let maxMerge = 0;
for (const sec of ['a', 'b']) {
  for (const len of [12, 16]) {
    const seen = new Set();
    for (const r of RHYTHMS[sec]) for (const name in r.parts) for (const p of r.parts[name]) if (p < len) seen.add(`${name}/${p}`);
    const ex = EXTRA[`${len}/${len === 12 ? 3 : 2}`];
    maxMerge = Math.max(maxMerge, seen.size + len + ex.QU.length + ex.GU.length);
  }
}
const grids = ['16/2', '12/2', '12/3'];
const qu = [], gu = [], extra = [];
for (const g of grids) {
  const e = EXTRA[g];
  extra.push({ grid: g, quFirst: qu.length, quN: e.QU.length, guFirst: gu.length, guN: e.GU.length });
  for (const p of e.QU) qu.push(Math.round(p * 100));
  for (const [p, gate] of e.GU) gu.push([Math.round(p * 100), Math.round(gate * 100)]);
}

const lines = [];
lines.push('/* SPDX-License-Identifier: GPL-3.0-only */');
lines.push(`/* Generated by take.mjs from minipops-processor.js at ${SR} Hz; do not edit. */`);
lines.push('#ifndef MP_TABLES_H', '#define MP_TABLES_H', '#include <stdint.h>');
lines.push(`#define MP_SR ${SR}`, `#define MP_VOICES ${VOICE_NAMES.length}`, `#define MP_RHYTHMS ${rhythms.length}`,
  `#define MP_MAX_HITS ${Math.ceil(maxMerge / 16) * 16}`, `#define MP_KBAL ${q(1 - Math.exp(-2 * Math.PI * 800 / SR), 30)}   /* the balance split at 800 Hz, Q30 */`);
lines.push('enum { MP_DRUM, MP_BELL, MP_NOISE, MP_RATCHET };');
lines.push('enum { ' + VOICE_NAMES.map((n) => `MP_${n}`).join(', ') + ' };');
lines.push('typedef struct {');
lines.push('    uint8_t kind, use_lp;');
lines.push('    int32_t r_a1, r_a2, r_gain;                 /* the resonator (drum, bell; a noise voice\'s body), Q28 */');
lines.push('    int32_t r2_a1, r2_a2, r2_gain;              /* the second one (a drum\'s body, the bell\'s tone) */');
lines.push('    int32_t b_a1, b_a2, b_a3;                   /* the bandpass (noise, ratchet), Q30 */');
lines.push('    int32_t env_mul, tick_k;                    /* Q30 */');
lines.push('    int32_t life, period, jitter;               /* samples, samples, Q16 */');
lines.push('    int32_t drive, c2, click, mix, gain;        /* Q16: saturation, its square term, the trigger leak, the second resonator\'s share, the level */');
lines.push('    uint32_t seed;');
lines.push('} mp_spec_t;');
lines.push('static const mp_spec_t MP_SPEC[MP_VOICES] = {');
for (const v of voices) {
  lines.push(`    /* ${v.name} */ { ${v.kind}, ${v.useLp}, ${v.res.a1}, ${v.res.a2}, ${v.res.gain}, ${v.res2.a1}, ${v.res2.a2}, ${v.res2.gain}, ` +
    `${v.bp.a1}, ${v.bp.a2}, ${v.bp.a3}, ${v.envMul}, ${v.tickK}, ${v.life}, ${v.period}, ${v.jitter}, ` +
    `${v.drive}, ${v.c2}, ${v.click}, ${v.mix}, ${v.gain}, ${v.seed}u },`);
}
lines.push('};');
lines.push('typedef struct { uint8_t voice; uint16_t pos100; } mp_hit_t;   /* position in steps x 100 (33: the late triplet eighth, 50: a sixteenth) */');
lines.push('typedef struct { const char *name; uint8_t len, div; uint16_t first, n; } mp_rhythm_t;');
lines.push(`static const mp_hit_t MP_HITS[${hits.length}] = {`);
for (let i = 0; i < hits.length; i += 12) lines.push('    ' + hits.slice(i, i + 12).map(([v, p]) => `{${v},${p}}`).join(', ') + ',');
lines.push('};');
lines.push('static const mp_rhythm_t MP_RHYTHM[MP_RHYTHMS] = {   /* key 0..9 of section A, then of B */');
for (const r of rhythms) lines.push(`    { "${r.name}", ${r.len}, ${r.div}, ${r.first}, ${r.n} },`);
lines.push('};');
lines.push('/* the additional rhythms, by the running grid (16/2, 12/2, 12/3): the tambourine on every step, the');
lines.push(' * quijada\'s positions, the guiro\'s positions with the gate in steps x 100 */');
lines.push('typedef struct { uint8_t qu_first, qu_n, gu_first, gu_n; } mp_extra_t;');
lines.push(`static const uint16_t MP_QU_POS[${qu.length}] = { ${qu.join(', ')} };`);
lines.push(`static const uint16_t MP_GU_POS[${gu.length}][2] = { ${gu.map(([p, g]) => `{${p},${g}}`).join(', ')} };`);
lines.push('static const mp_extra_t MP_EXTRA[3] = { ' + extra.map((e) => `{ ${e.quFirst}, ${e.quN}, ${e.guFirst}, ${e.guN} }`).join(', ') + ' };');
lines.push('#endif', '');
const out = resolve(dirname(fileURLToPath(import.meta.url)), 'mp_tables.h');
writeFileSync(out, lines.join('\n'));
console.log(`mp_tables.h: ${voices.length} voices, ${rhythms.length} rhythms, ${hits.length} hits, the biggest merge ${maxMerge}`);
