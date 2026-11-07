// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
//
// The FM-1 USB-MIDI update protocol over WebMIDI (after tools/fm1_install.py).
// Step 1: the running firmware (official
// or Felucca) reads parts of the package and stages the update loader. Step 2:
// the loader reads the whole image and writes it. Both steps are "the device
// asks, we answer": cmd 0x30 read requests on the logical image.

import { pairPorts, looksOurs, NOT_DEVICE } from "./ports.js";

const HS_QUERY = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7];
const UPGRADE = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7];
const MAXDATA = 512;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const IS_OTA = (id) => /^ota-/i.test(id.model);
// fm1-dock: the SMK-37 Pro, SMK-37 Elite, MKE-P37 and Starrykey 37 Play are one keyboard under several names; an
// FM-1 and an SMK-37 never are (genkigenki's fm1-to-smk37, its reference port's installer)
const SMK_FAMILY = /^(SMK-37[ +].*|MKE-P37|STARRYKEY-37.*)$/i;
const bare = (model) => model.replace(/^ota-/i, "");
export function sameKeyboard(a, b) {
  const ma = bare(a), mb = bare(b);
  return ma === mb || (SMK_FAMILY.test(ma) && SMK_FAMILY.test(mb));
}

// errors carry a code the pages translate: notfound, inuse (detail: the ports that would not open), model, stopped, noloader, lost, noreturn,
// mismatch (detail: the identity the device reports), badreq
function fail(code, msg, detail = "") { const e = new Error(msg); e.code = code; e.detail = detail; return e; }

export function pack7(data) {
  const out = []; let acc = 0, nb = 0;
  for (const b of data) {
    acc |= b << nb; nb += 8;
    while (nb >= 7) { out.push(acc & 0x7F); acc >>>= 7; nb -= 7; }
  }
  if (nb) out.push(acc & 0x7F);
  return out;
}

export function unpack7(s) {
  const out = []; let acc = 0, nb = 0;
  for (const b of s) {
    acc |= (b & 0x7F) << nb; nb += 7;
    while (nb >= 8) { out.push(acc & 0xFF); acc >>>= 8; nb -= 8; }
  }
  return out;
}

function response(addr, data, fl = 0) {
  const len = data.length;
  const body = [0x00, 0x59, 0x30, (len + 8) & 0xFF, ((len + 8) >> 8) & 0xFF, 0, fl,
    addr & 0xFF, (addr >>> 8) & 0xFF, (addr >>> 16) & 0xFF, (addr >>> 24) & 0xFF,
    len & 0xFF, (len >> 8) & 0xFF, 0, ...data];
  let s = 0;
  for (let i = 6; i < body.length; i++) s += body[i];
  body.push(~s & 0xFF);
  return [0xF0, ...pack7(body), 0xF7];
}

function parseRequest(pkt) {
  if (pkt[0] !== 0xF0 || pkt[pkt.length - 1] !== 0xF7) return null;
  const u = unpack7(pkt.slice(1, -1));
  if (u.length !== 15 || u[0] !== 0 || u[1] !== 0x59 || u[2] !== 0x30) return null;
  let s = 0;
  for (let i = 6; i < 14; i++) s += u[i];
  if ((~s & 0xFF) !== u[14]) return null;
  return { fl: u[6], addr: (u[7] | (u[8] << 8) | (u[9] << 16) | (u[10] << 24)) >>> 0, len: u[11] | (u[12] << 8) | (u[13] << 16) };
}

function parseIdentity(pkt) {
  const d = unpack7(pkt.slice(1, -1));
  if (d.length !== 34 || d[0] !== 0 || d[1] !== 0x59 || d[2] !== 0x11) return null;
  const txt = String.fromCharCode(...d.slice(6, 33)).replace(/\0+$/, "");
  const m = /^([^_]+)_(\d+)$/.exec(txt);
  return m ? { text: txt, model: m[1], version: parseInt(m[2], 10) } : null;
}

// one MIDI in/out pair with a SysEx queue
class Link {
  constructor(input, output) {
    this.input = input; this.output = output; this.q = []; this.waiter = null;
    input.onmidimessage = (ev) => {
      if (ev.data[0] !== 0xF0) return;
      this.q.push(Array.from(ev.data));
      if (this.waiter) { const w = this.waiter; this.waiter = null; w(); }
    };
  }
  get lost() { return this.input.state === "disconnected" || this.output.state === "disconnected"; }
  // false when the port is gone (unplugged): WebMIDI throws on a disconnected output
  send(bytes) {
    try { this.output.send(bytes); return true; } catch (_) { return false; }
  }
  drain() { this.q.length = 0; }
  async read(ms) {
    if (!this.q.length) {
      let tm;
      await Promise.race([new Promise((r) => { this.waiter = r; }), new Promise((r) => { tm = setTimeout(r, ms); })]);
      clearTimeout(tm);
      this.waiter = null;
    }
    return this.q.shift() || null;
  }
  close() { this.input.onmidimessage = null; }
}

async function handshake(link, tries = 3) {
  link.drain();
  for (let t = 0; t < tries; t++) {
    if (!link.send(HS_QUERY)) return null;
    const end = Date.now() + 1000;
    while (Date.now() < end) {
      const p = await link.read(end - Date.now());
      if (!p) break;
      const id = parseIdentity(p);
      if (id) return id;
    }
  }
  return null;
}

export class Updater {
  constructor(access, log) {
    this.access = access;
    this.log = log || (() => {});
  }

  // the first in/out pair that answers the handshake (and filter): the pairs of names that look like the device's
  // (ports.js), then its ports left unpaired tried input against output; wide: then every other port's too but the
  // systems' own (a press of Connect: the handshake is a SysEx of no one's). A port that would not open (another
  // program or tab holds it, as Windows allows only one) is noted in this.busy.
  async find(filter, { wide = false } = {}) {
    this.busy = new Set();
    const pairs = pairPorts(this.access.inputs, this.access.outputs);
    for (const p of pairs.filter((x) => !x.guess && looksOurs(x))) {
      const dev = await this.ask(p.input, p.output, filter);
      if (dev) return dev;
    }
    const left = pairs.filter((x) => x.guess && (looksOurs(x) ||
      (wide && !NOT_DEVICE.test(x.input.name || "") && !NOT_DEVICE.test(x.output.name || ""))));
    return left.length ? this.cross(left, filter) : null;
  }

  async open(port) {
    if (typeof port.open !== "function") return true;   // (opened by its first use)
    try { await port.open(); return true; } catch (_) { this.busy.add(port.name || "?"); return false; }
  }

  async ask(input, output, filter) {
    if (!await this.open(input) || !await this.open(output)) return null;
    const link = new Link(input, output);
    const id = await handshake(link, 2);
    if (id && (!filter || filter(id))) return { link, id, name: input.name === output.name ? input.name : `${input.name} / ${output.name}` };
    link.close();
    return null;
  }

  // unpaired ports: each output asked once while every input listens; the input that answers is its pair
  async cross(guesses, filter) {
    const ins = [...new Set(guesses.map((g) => g.input))], outs = [...new Set(guesses.map((g) => g.output))];
    const heard = [], ears = [];
    const stop = () => { ears.forEach((i) => { i.onmidimessage = null; }); ears.length = 0; };
    for (const i of ins) {
      if (!await this.open(i)) continue;
      i.onmidimessage = (ev) => { if (ev.data[0] === 0xF0) heard.push([i, Array.from(ev.data)]); };
      ears.push(i);
    }
    try {
      for (const o of outs) {
        if (!ears.length) break;
        if (!ins.some((i) => guesses.some((g) => g.input === i && g.output === o)) || !await this.open(o)) continue;
        heard.length = 0;
        try { o.send(HS_QUERY); } catch (_) { continue; }
        for (const end = Date.now() + 700; Date.now() < end;) {
          await sleep(20);
          const hit = heard.map(([i, p]) => [i, parseIdentity(p)]).find(([i, id]) => id && guesses.some((g) => g.input === i && g.output === o));
          if (!hit) continue;
          const [i, id] = hit;
          if (filter && !filter(id)) break;
          stop();
          return { link: new Link(i, o), id, name: `${i.name} / ${o.name}` };
        }
      }
    } finally { stop(); }
    return null;
  }

  // the error for no device found: another program holding its port, or none there
  missing(msg = "FM-1 not found (USB cable, and no other app using it?)") {
    return this.busy && this.busy.size ? fail("inuse", msg, [...this.busy].join(", ")) : fail("notfound", msg);
  }

  async waitFor(filter, ms) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
      const dev = await this.find(filter);
      if (dev) return dev;
      await sleep(1000);
    }
    return null;
  }

  // serve read requests until the device asks for one of the finish addresses; stops early
  // when the port disappears (lost: true) instead of waiting out idleMs
  async serve(link, image, finishAddr, idleMs, onProgress) {
    let served = 0, last = Date.now();
    for (;;) {
      const pkt = await link.read(1000);
      if (!pkt) {
        if (link.lost) return { served, finished: false, lost: true };
        if (Date.now() - last > idleMs) return { served, finished: false };
        continue;
      }
      const r = parseRequest(pkt);
      if (!r) continue;
      last = Date.now();
      if (r.addr === 0xE0000000 || r.addr === 0xF0000000) {
        link.send(response(r.addr, [...new TextEncoder().encode("success"), 0]));
        if (r.addr === finishAddr) return { served, finished: true };
        continue;
      }
      if (r.len > MAXDATA || r.addr + r.len > image.length) throw fail("badreq", `bad request ${r.addr.toString(16)}+${r.len}`);
      await sleep(10);
      if (!link.send(response(r.addr, Array.from(image.subarray(r.addr, r.addr + r.len)), r.fl))) {
        return { served, finished: false, lost: true };
      }
      served++;
      if (onProgress) onProgress(served, r.addr);
    }
  }

  // full install: running firmware -> loader -> new firmware
  async install(image, product, onStep) {
    const step = onStep || (() => {});
    const dev = await this.find((id) => !IS_OTA(id));
    if (!dev) throw this.missing();
    const [model] = product.split("_");
    if (!sameKeyboard(dev.id.model, model)) { dev.link.close(); throw fail("model", `the device is ${dev.id.text}, the package is for ${product}`); }
    step("start", dev.id.text);
    dev.link.send(UPGRADE);
    await sleep(2000);
    const s1 = await this.serve(dev.link, image, 0xE0000000, 8000, (n) => step("verify", n));
    dev.link.close();
    if (!s1.finished) throw fail(s1.lost ? "lost" : "stopped", `the device ${s1.lost ? "was disconnected" : "stopped"} after ${s1.served} requests: nothing was written`);
    step("loader");
    await sleep(3000);
    const ota = await this.waitFor((id) => id.model === "ota-" + model, 30000);
    if (!ota) throw fail("noloader", "the update loader did not appear. Replug the USB cable and press Install again: the device stays in update mode until it is finished.");
    const s2 = await this.write(ota, image, step);
    if (!s2.finished) throw fail(s2.lost ? "lost" : "stopped", `the loader ${s2.lost ? "was disconnected" : "stopped"} after ${s2.served} requests. Replug and press Install again to resume.`);
    step("reboot");
    await sleep(3000);
    const back = await this.waitFor((id) => !IS_OTA(id), 40000);
    if (!back) throw fail("noreturn", "the device did not come back: power-cycle it");
    back.link.close();
    if (back.id.text !== product) throw fail("mismatch", `installed, but the device reports ${back.id.text}`, back.id.text);
    step("done", back.id.text);
    return back.id.text;
  }

  // step 2 with the loader found by find()/waitFor()
  async write(ota, image, step) {
    ota.link.send(UPGRADE);
    await sleep(2000);
    const s2 = await this.serve(ota.link, image, 0xF0000000, 180000, (n) => step("write", n));
    ota.link.close();
    return s2;
  }

  // resume: the device is already in update mode (loader) -> true when the write finished. fm1-dock: only with the
  // package's identity, and only a loader of the same keyboard's (nothing is written to an SMK-37 from an FM-1
  // package, nor the other way round)
  async resume(image, onStep, product = null) {
    if (!product) return false;
    const model = product.split("_")[0];
    const ota = await this.find((id) => IS_OTA(id) && sameKeyboard(id.model, model));
    if (!ota) return false;
    const step = onStep || (() => {});
    const result = await this.write(ota, image, step);
    if (!result.finished) return false;
    if (product) {
      step("reboot");
      await sleep(3000);
      const back = await this.waitFor((id) => !IS_OTA(id) && sameKeyboard(id.model, model), 40000);
      if (!back) throw fail("noreturn", "The FM-1 did not return after resuming the write.");
      back.link.close();
      if (back.id.text !== product) throw fail("mismatch", "The FM-1 reports a different firmware after resuming.", back.id.text);
      step("done", back.id.text);
    }
    return true;
  }
}
