// SPDX-License-Identifier: GPL-3.0-only
// fm1-dock's page with ?demo: a simulated device behind navigator.requestMIDIAccess, so that the page's flows (connect,
// install, add, remove, the console) can be tried and tested without an FM-1. The browser's own WebMIDI is replaced
// before anything on the page asks for it: nothing reaches a real MIDI port. The device answers as
// firmware/src/dock.c's deploys and the updater's handshake and reads (fm1ota.js) do; tests/install_test.mjs has
// the same in node.
//   ?demo, ?demo=fm1   an FM-1 running fm1-dock 0.5, FM-1 SID added
//   ?demo=fresh        an FM-1 running fm1-dock, nothing added
//   ?demo=smk          an SMK-37 Pro running fm1-dock, FM-1 SID and AMB-1 added
//   ?demo=stock        an FM-1 with M-VAVE's firmware: no fm1-dock
//   ?demo=none         nothing plugged in
import { frame, parse, crc32, CMD } from "./dock.js";
import { pack7, unpack7 } from "./install/fm1ota.js";

const HS = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7];
const UPGRADE = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7];
const AREAS = { fm1: [[0x96000, 0xE0000]], smk37: [[0x70000, 0xB8000], [0xC2000, 0xE2000]] };
const PACKAGE = [["SINE", 0, 0], ["CS-1", 1, 53000], ["SCINTILLA", 1, 89000], ["OBERVAVE", 1, 152000]];
const SCENES = {
  fm1: { model: "FM-1", identity: "FM-1_921", dock: "0.5", added: [["FM-1 SID", 272640]] },
  fresh: { model: "FM-1", identity: "FM-1_921", dock: "0.6", added: [] },
  smk: { model: "SMK-37 Pro", identity: "SMK-37 Pro_921", dock: "0.6", added: [["FM-1 SID", 272640], ["AMB-1", 77764]] },
  stock: { model: "FM-1", identity: "FM-1_015", dock: null, added: [] },
  none: null,
};
const eq = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const u32 = (v) => [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255];
const rd32 = (d, o) => (d[o] | d[o + 1] << 8 | d[o + 2] << 16 | d[o + 3] << 24) >>> 0;
const name24 = (s) => { const b = new Array(24).fill(0); for (let i = 0; i < Math.min(23, s.length); i++) b[i] = s.charCodeAt(i); return b; };
const cstr = (d) => { let s = ""; for (const c of d) { if (!c) break; s += String.fromCharCode(c); } return s; };
const later = (ms, fn) => setTimeout(fn, ms);

class Port extends EventTarget {
  constructor(dev, type, name) {
    super();
    Object.assign(this, { dev, type, name, id: `demo-${type}-${Math.random().toString(36).slice(2)}`, manufacturer: "demo",
      state: "connected", connection: "closed", onmidimessage: null });
  }
  async open() { this.connection = "open"; return this; }
  async close() { this.connection = "closed"; return this; }
  deliver(bytes) {                              // device -> page
    if (this.state !== "connected") return;
    const ev = new Event("midimessage");
    ev.data = Uint8Array.from(bytes);
    if (this.onmidimessage) this.onmidimessage(ev);
    this.dispatchEvent(ev);
  }
  send(data) {                                  // page -> device
    if (this.state !== "connected") throw new DOMException("the port is disconnected", "InvalidStateError");
    const d = Array.from(data);
    later(1, () => this.dev.rx(d));
  }
}

class Access extends EventTarget {
  constructor() { super(); this.inputs = new Map(); this.outputs = new Map(); this.sysexEnabled = true; this.onstatechange = null; }
  changed(port) {
    const ev = new Event("statechange");
    ev.port = port;
    if (this.onstatechange) this.onstatechange(ev);
    this.dispatchEvent(ev);
  }
}

class Device {
  constructor(scene, imageLen, latest, product) {
    this.scene = scene; this.access = new Access(); this.imageLen = imageLen; this.latest = latest; this.product = product;
    this.board = /^SMK/.test(scene.model) ? "smk37" : "fm1";
    this.deployed = [];
    for (const [name, size] of scene.added) {
      const gap = this.gaps().find((x) => x[1] >= Math.ceil(size / 4096) * 4096);
      if (gap) this.deployed.push({ name, size, at: gap[0] });
    }
    this.cur = 0; this.logText = ""; this.logBase = 0; this.rxd = null; this.queue = []; this.waiting = null;
    this.boot(scene.identity, scene.dock);
  }

  boot(identity, dockVersion) {
    this.identity = identity; this.dockOn = !!dockVersion; this.queue = []; this.waiting = null;
    for (const m of [this.access.inputs, this.access.outputs]) for (const p of [...m.values()]) {
      p.state = "disconnected"; m.delete(p.id); this.access.changed(p);
    }
    const name = identity.startsWith("ota-") ? "Felucca Update" : "Felucca";
    this.input = new Port(this, "input", name);
    this.output = new Port(this, "output", name);
    this.access.inputs.set(this.input.id, this.input);
    this.access.outputs.set(this.output.id, this.output);
    this.access.changed(this.input); this.access.changed(this.output);
    if (this.dockOn) this.log(`fm1-dock ${dockVersion} boot, reset 00000001`, "slots: package 4, deployed " + this.deployed.length,
      `started ${this.modules()[this.cur].name}`);
  }

  log(...lines) {
    for (const l of lines) this.logText += `${Math.floor(performance.now())} ${l}\n`;
    if (this.logText.length > 2048) { const cut = this.logText.length - 2048; this.logBase += cut; this.logText = this.logText.slice(cut); }
  }

  modules() {
    return [...PACKAGE.map(([name, kind, size], i) => ({ name, kind, size, at: kind ? 0x02035000 + i * 0x10000 : 0 })),
      ...this.deployed.map((m) => ({ name: m.name, kind: 2, size: m.size, at: 0x02000000 + m.at }))];
  }

  gaps() {                                      // dock.c's dp_place: the free pieces of the deploy areas
    const out = [];
    for (const [lo, hi] of AREAS[this.board]) {
      const mods = this.deployed.filter((m) => m.at >= lo && m.at < hi).sort((a, b) => a.at - b.at);
      let at = lo;
      for (const m of mods) { if (m.at > at) out.push([at, m.at - at]); at = m.at + Math.ceil(m.size / 4096) * 4096; }
      if (hi > at) out.push([at, hi - at]);
    }
    return out;
  }

  tx(bytes) { later(3, () => this.input.deliver(bytes)); }
  reply(cmd, seq, payload, ms = 3) { const f = frame(cmd | 0x40, seq, Uint8Array.from(payload)); later(ms, () => this.input.deliver(f)); }

  rx(d) {
    if (eq(d, HS)) {
      const t = [...new TextEncoder().encode(this.identity)];
      this.tx([0xF0, ...pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...new Array(28 - t.length).fill(0)]), 0xF7]);
    } else if (eq(d, UPGRADE)) this.upgrade();
    else if (d[0] === 0xF0 && d[1] === 0x7D && d[2] === 0x48 && d[3] === 0x42) { if (this.dockOn) this.dock(parse(Uint8Array.from(d))); }
    else if (d[0] === 0xF0 && this.waiting) this.served(d);
  }

  // ---- the update (fm1ota.js's other side): the running firmware reads a little and starts the loader, the loader
  // reads the whole image and starts the new firmware
  upgrade() {
    const ota = this.identity.startsWith("ota-");
    const len = this.imageLen;
    this.queue = ota
      ? [...Array.from({ length: Math.ceil(len / 512) }, (_, k) => [k * 512, Math.min(512, len - k * 512)]), [0xF0000000, 8]]
      : [[0, 64], [0x40, 160], [0x1000, 512], [0xE0000000, 8]];
    later(50, () => this.next());
  }
  next() {
    const r = this.queue.shift();
    if (!r) return;
    this.waiting = r;
    const [addr, len] = r;
    const u = [0, 0x59, 0x30, 0, 0, 0, 0, ...u32(addr), len & 0xFF, len >> 8, 0];
    let s = 0;
    for (let i = 6; i < 14; i++) s += u[i];
    u.push(~s & 0xFF);
    this.tx([0xF0, ...pack7(u), 0xF7]);
  }
  served(d) {
    const u = unpack7(d.slice(1, -1));
    if (u[1] !== 0x59 || u[2] !== 0x30) return;
    const [addr] = this.waiting;
    this.waiting = null;
    const model = this.scene.model;
    if (addr === 0xE0000000) later(400, () => this.boot(`ota-${model}_900`, null));
    else if (addr === 0xF0000000) {
      later(600, () => {
        this.cur = 0;
        this.boot(this.product || (this.board === "fm1" ? "FM-1_921" : `${model}_921`), this.latest || "0.6");
      });
    } else this.next();
  }

  // ---- fm1-dock's deploys (firmware/src/dock.c, "deploys")
  dock(r) {
    if (!r) return;
    const { cmd, seq, d } = r;
    const mods = this.modules();
    switch (cmd) {
      case CMD.HELLO: {
        const g = this.gaps(), free = g.reduce((a, x) => a + x[1], 0), largest = Math.max(0, ...g.map((x) => x[1]));
        const logW = this.logBase + this.logText.length;
        this.reply(cmd, seq, [0, 1, 2, mods.length, this.cur, this.rxd ? 1 : 0, 0, 0, ...u32(largest), ...u32(free), ...u32(272 * 1024),
          ...u32(0), ...u32(0), ...u32(61000), ...u32(0), ...u32(logW), ...u32(139296), ...new Array(16).fill(0)]);
        break;
      }
      case CMD.LIST:
        mods.forEach((m, i) => this.reply(cmd, seq, [0, i, 0, m.kind, ...u32(1), ...u32(m.size), ...u32(m.at), ...name24(m.name)], 2 + i));
        this.reply(cmd, seq, [0, 0xFF], 4 + mods.length);
        break;
      case CMD.LOG: {
        const w = this.logBase + this.logText.length;
        let from = Math.max(rd32(d, 0), this.logBase);
        if (from > w) from = w;
        const chunk = [...this.logText.slice(from - this.logBase, from - this.logBase + 180)].map((c) => c.charCodeAt(0) & 0xFF);
        this.reply(cmd, seq, [0, 0, 0, 0, ...u32(w), ...u32(from), ...chunk]);
        break;
      }
      case CMD.BEGIN: {
        this.rxd = { size: rd32(d, 0), crc: rd32(d, 4), name: cstr(d.subarray(8, 32)) };
        this.rxd.got = new Uint8Array(this.rxd.size);
        this.log(`deploy: receiving ${this.rxd.size.toString(16).padStart(8, "0")}`, this.rxd.name);
        this.reply(cmd, seq, [0], 20);
        break;
      }
      case CMD.DATA: {
        if (!this.rxd) { this.reply(cmd, seq, [8]); break; }
        const off = rd32(d, 0);
        this.rxd.got.set(d.subarray(4), off);
        this.reply(cmd, seq, [0, 0, 0, 0, ...u32(off)], 6);
        break;
      }
      case CMD.COMMIT: {
        const rx = this.rxd;
        this.rxd = null;
        if (!rx) { this.reply(cmd, seq, [8]); break; }
        if (crc32(rx.got) !== rx.crc) { this.reply(cmd, seq, [1], 300); break; }
        const old = this.deployed.findIndex((m) => m.name === rx.name);
        if (old >= 0) this.deployed.splice(old, 1);   // (an update takes the old one's place)
        const need = Math.ceil(rx.size / 4096) * 4096, gap = this.gaps().find((x) => x[1] >= need);
        if (!gap) { this.reply(cmd, seq, [4], 300); this.log("deploy: no space"); break; }
        this.deployed.push({ name: rx.name, size: rx.size, at: gap[0] });
        this.cur = this.modules().findIndex((m) => m.name === rx.name);
        this.log("deploy: done", `started ${rx.name}`);
        this.reply(cmd, seq, [0, 0, 0, 0, ...u32(0x02000000 + gap[0])], 900);
        break;
      }
      case CMD.ABORT: this.rxd = null; break;
      case CMD.DELETE: {
        const name = cstr(d.subarray(0, 24)), i = this.deployed.findIndex((m) => m.name === name);
        if (i < 0) { this.reply(cmd, seq, [9]); break; }
        const playing = mods[this.cur] && mods[this.cur].name;
        this.deployed.splice(i, 1);
        const now = this.modules().findIndex((m) => m.name === playing);
        this.cur = now >= 0 ? now : 0;
        this.log("deploy: deleted", name);
        this.reply(cmd, seq, [0], 400);
        break;
      }
      case CMD.SAFE: this.cur = 0; this.reply(cmd, seq, [0]); break;
      case CMD.SELECT: {
        const i = mods.findIndex((m) => m.name === cstr(d.subarray(0, 24)));
        if (i >= 0) { this.cur = i; this.log(`started ${mods[i].name}`); }
        this.reply(cmd, seq, [i >= 0 ? 0 : 9], 200);
        break;
      }
      case CMD.MODULE: this.reply(cmd, seq, [mods[this.cur] && mods[this.cur].name === "FM-1 SID" ? 0 : 9]); break;
      default: this.reply(0x3F, seq, [10]);
    }
  }
}

export { Device, Access, SCENES };          // (tests/install_test.mjs)

// the simulated device for ?demo=scene, in place of the browser's WebMIDI
export async function start(scene) {
  const sc = SCENES[scene || "fm1"] === undefined ? SCENES.fm1 : SCENES[scene || "fm1"];
  let imageLen = 0x2400, latest = null, product = null;
  try {                                         // the image the update reads: the size of the package on this site
    const idx = await (await fetch("fw/index.json", { cache: "no-cache" })).json();
    const e = idx.firmwares["fm1-dock"];
    latest = e.version;
    const v = sc && /^SMK/.test(sc.model) ? (e.variants || {})[sc.model] : e;
    if (v && v.size) { imageLen = v.size - 20; product = v.product; }
  } catch (_) { /* no fw/ here: a small image */ }
  const access = sc ? new Device(sc, imageLen, latest, product).access : new Access();
  Object.defineProperty(navigator, "requestMIDIAccess", { configurable: true, value: async () => access });
  const banner = document.createElement("div");
  banner.className = "demo-banner";
  banner.setAttribute("role", "note");
  banner.textContent = sc ? `Demo: a simulated ${sc.model} (${sc.dock ? `fm1-dock ${sc.dock}` : sc.identity}). Nothing is sent to a real device.`
    : "Demo: no device plugged in. Nothing is sent to a real device.";
  document.body.prepend(banner);
  window.fm1demo = access;                      // for a test's eyes
}
