#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// The web Mini Pops (js/minipops-processor.js) rendered in Node at 44.1 kHz, as tests/minipops_check.py
// wants it: `voices OUT.raw` renders each voice alone, one second each, in order; `rhythm KEY OUT.raw`
// four seconds of key KEY (0..9, section A) at 120 bpm with the sliders up. 16-bit mono.
//   minipops_ref.mjs PATH/minipops-processor.js voices|rhythm [KEY] OUT.raw
import { writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const SR = 44100;
const [src, what, ...rest] = process.argv.slice(2);
let Proc = null;
globalThis.sampleRate = SR;
globalThis.currentTime = 0;
globalThis.AudioWorkletProcessor = class { constructor() { this.port = { onmessage: null, postMessage() {} }; } };
globalThis.registerProcessor = (_n, cls) => { Proc = cls; };
const mod = await import(pathToFileURL(resolve(src)).href);
const names = mod.VOICE_NAMES;
const p = new Proc();
const send = (d) => p.port.onmessage({ data: d });
const L = new Float32Array(128), R = new Float32Array(128);
const render = (seconds) => {
  const n = Math.round(seconds * SR), out = new Int16Array(n);
  for (let i = 0; i < n; i += 128) {
    p.process([], [[L, R]]);
    for (let j = 0; j < 128 && i + j < n; j++) out[i + j] = Math.max(-32767, Math.min(32767, Math.round(L[j] * 32767)));
  }
  return out;
};
send({ type: 'preset', params: { volume: 1, balance: 0.5, quijada: 0.7, guiro: 0.7, tambourine: 0.7 } });
let pcm;
if (what === 'voices') {
  const parts = [];
  for (let v = 0; v < names.length; v++) {
    send({ type: 'trigger', voice: v, velocity: 1 });
    parts.push(render(1));
  }
  pcm = new Int16Array(parts.length * SR);
  parts.forEach((x, i) => pcm.set(x, i * SR));
  writeFileSync(rest[0], Buffer.from(pcm.buffer));
} else {
  const key = +rest[0];
  send({ type: 'preset', params: { [`key.${key}`]: true, section: 0, tempo: 120, volume: 1, balance: 0.5, quijada: 0.7, guiro: 0.7, tambourine: 0.7 } });
  send({ type: 'play' });
  pcm = render(4);
  writeFileSync(rest[1], Buffer.from(pcm.buffer));
}
