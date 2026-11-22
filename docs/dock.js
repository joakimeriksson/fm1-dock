// SPDX-License-Identifier: GPL-3.0-only
// fm1-dock's SysEx protocol in the browser (WebMIDI): what tools/hbdeploy.py and tools/probe.py do,
// for the console on index.html. The device side: firmware/src/dock.c, "deploys".
//   frame    F0 7D 48 42 cmd seq pack7(payload + CRC-32 of the payload, little endian) F7
//   replies  cmd | 0x40 (0x7F: refused), the same seq; the payload starts with a status byte

export const CMD = { HELLO: 0x01, LIST: 0x02, INFO: 0x03, BEGIN: 0x10, DATA: 0x11, COMMIT: 0x12, ABORT: 0x13, DELETE: 0x14,
  LOG: 0x20, SAFE: 0x21, SELECT: 0x22, MODULE: 0x23, BANKS: 0x24, BANK_READ: 0x25, BANK_WRITE: 0x26, BANK_ERASE: 0x27,
  RUN: 0x30, RESULT: 0x31 };
export const STATUS = ["done", "bad CRC", "bad header", "too big", "no space", "flash error", "verify failed",
  "data missing", "not started", "not found", "bad message", "refused"];
export const STATE = ["ok", "bad CRC", "bad header", "too big", "crashed", "init failed", "too slow"];
export const KIND = ["built in", "image", "deployed", "boot", "effect", "effect (image)"];   // (4, 5: effects, their places 0x80 up)
// a module the player added (a synth, or an effect: hb_module.h "Effects"): it can be removed, and takes the add-ons' room
export const isAdded = (m) => m.kind === "deployed" || m.kind === "effect";
export const PROBE_STATE = ["none", "running", "done", "crashed", "hung"];
const MAN = [0x7d, 0x48, 0x42];
const CHUNK = 256;
const HRBR_MAGIC = 0x52425248;
const F_PROBE = 0x100, F_BOOT = 0x200, F_FX = 0x800;
const TICKS_PER_US = 24;

const CRC_TAB = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

export function crc32(b) {
  let c = 0xffffffff;
  for (let i = 0; i < b.length; i++) c = CRC_TAB[(c ^ b[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

export function pack7(b) {                      // 8-to-7 bits, LSB first (ota.c's ota_pack7)
  const out = [];
  let acc = 0, nb = 0;
  for (const x of b) {
    acc |= x << nb;
    nb += 8;
    while (nb >= 7) { out.push(acc & 0x7f); acc >>>= 7; nb -= 7; }
  }
  if (nb) out.push(acc & 0x7f);
  return out;
}

export function unpack7(b) {
  const out = [];
  let acc = 0, nb = 0;
  for (const x of b) {
    acc |= (x & 0x7f) << nb;
    nb += 7;
    if (nb >= 8) { out.push(acc & 0xff); acc >>>= 8; nb -= 8; }
  }
  return Uint8Array.from(out);
}

const u32 = (v) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
const rd32 = (d, o) => (d[o] | d[o + 1] << 8 | d[o + 2] << 16 | d[o + 3] << 24) >>> 0;
const rdi32 = (d, o) => rd32(d, o) | 0;
const cat = (...parts) => { const a = []; for (const p of parts) a.push(...p); return Uint8Array.from(a); };
export const hex32 = (v) => "0x" + (v >>> 0).toString(16).padStart(8, "0");

export function name24(s) {
  const b = new Uint8Array(24);
  for (let i = 0; i < Math.min(23, s.length); i++) b[i] = s.charCodeAt(i) & 0xff;
  return b;
}

const cstr = (d, from, to) => {
  let s = "";
  for (let i = from; i < to && d[i]; i++) s += String.fromCharCode(d[i]);
  return s;
};

export function frame(cmd, seq, payload = new Uint8Array(0)) {
  const body = cat(payload, u32(crc32(payload)));
  return Uint8Array.from([0xf0, ...MAN, cmd, seq & 0x7f, ...pack7(body), 0xf7]);
}

export function parse(msg) {                    // a reply: {cmd, seq, d}, or null if not fm1-dock's / damaged
  if (msg.length < 7 || msg[0] !== 0xf0 || msg[msg.length - 1] !== 0xf7) return null;
  if (msg[1] !== MAN[0] || msg[2] !== MAN[1] || msg[3] !== MAN[2]) return null;
  const d = unpack7(msg.subarray(6, msg.length - 1));
  if (d.length < 4 || crc32(d.subarray(0, d.length - 4)) !== rd32(d, d.length - 4)) return null;
  return { cmd: msg[4] & ~0x40, seq: msg[5], d: d.subarray(0, d.length - 4) };
}

export function moduleInfo(blob) {              // a .dock's header (hb_file_t), or throws
  if (blob.length < 80 || rd32(blob, 0) !== HRBR_MAGIC) throw new Error("not a fm1-dock module file (.dock)");
  const flags = rd32(blob, 8);
  return { abi: rd32(blob, 4), flags, name: cstr(blob, 12, 36), version: rd32(blob, 36),
    code: rd32(blob, 40), data: rd32(blob, 44), bss: rd32(blob, 48), rt: rd32(blob, 52),
    probe: !!(flags & F_PROBE), boot: !!(flags & F_BOOT), fx: !!(flags & F_FX) };
}

export const status = (d) => { const s = d.length ? d[0] : 10; return STATUS[s] || `status ${s}`; };

// the probe's records (probes/probe.h) -> {key: value}; a key twice -> a list
export function decodeProbe(out) {
  const res = {};
  let i = 0;
  while (i + 4 <= out.length) {
    const t = out[i], k = out[i + 1];
    const key = cstr(out, i + 2, i + 2 + k);
    const n = out[i + 2 + k] | out[i + 3 + k] << 8;
    const v = out.subarray(i + 4 + k, i + 4 + k + n);
    i += 4 + k + n;
    let val;
    if (t === 1) val = rd32(v, 0);
    else if (t === 2) val = rdi32(v, 0);
    else if (t === 3) val = hex32(rd32(v, 0));
    else if (t === 5) val = String.fromCharCode(...v);
    else if (t === 6) { val = []; for (let j = 0; j + 4 <= v.length; j += 4) val.push(hex32(rd32(v, j))); }
    else if (t === 7) {
      const ticks = rdi32(v, 0), runs = rd32(v, 4), m = /_x(\d+)$/.exec(key);
      const per = runs * (m ? Number(m[1]) : 1);
      const ns = per ? ticks * 1000 / TICKS_PER_US / per : 0;
      val = { ticks, runs, ns: Math.round(ns * 100) / 100, emu_cycles: Math.round(ns * 240 / 10) / 100 };
    } else val = [...v].map((x) => x.toString(16).padStart(2, "0")).join("");
    if (key in res) res[key] = (Array.isArray(res[key]) && t !== 6 ? res[key] : [res[key]]).concat([val]);
    else res[key] = val;
  }
  return res;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// One FM-1 running fm1-dock, over a WebMIDI input / output pair.
export class Dock {
  constructor(input, output, { onMidi, onTraffic } = {}) {
    this.input = input;
    this.output = output;
    this.seq = 0;
    this.waiters = new Set();
    this.onMidi = onMidi || (() => {});
    this.onTraffic = onTraffic || (() => {});
    this.handler = (e) => this.receive(e.data);
    input.addEventListener("midimessage", this.handler);
  }

  close() { this.input.removeEventListener("midimessage", this.handler); }

  receive(data) {
    if (data[0] !== 0xf0) { this.onMidi(data); return; }
    const r = parse(data);
    this.onTraffic("in", data, r);
    if (!r) return;
    for (const w of this.waiters) w(r);
  }

  send(cmd, payload) {
    this.seq = (this.seq + 1) & 0x7f;
    const f = frame(cmd, this.seq, payload);
    this.onTraffic("out", f, { cmd, seq: this.seq });
    this.output.send(f);
    return this.seq;
  }

  // every reply to seq until `idle` ms pass without one; each goes to fn, which returns true to stop
  collect(seq, idle, fn) {
    return new Promise((resolve) => {
      let timer;
      const done = (ok) => { clearTimeout(timer); this.waiters.delete(w); resolve(ok); };
      const arm = () => { clearTimeout(timer); timer = setTimeout(() => done(false), idle); };
      const w = (r) => { if (r.seq !== seq) return; arm(); if (fn(r)) done(true); };
      this.waiters.add(w);
      arm();
    });
  }

  async ask(cmd, payload = new Uint8Array(0), { timeout = 1000, tries = 3 } = {}) {
    for (let t = 0; t < tries; t++) {
      const seq = this.send(cmd, payload);
      let got = null;
      await this.collect(seq, timeout, (r) => {
        if (r.cmd === cmd || r.cmd === 0x3f) { got = r.d; return true; }
        return false;
      });
      if (got) return got;
    }
    throw new Error(`no answer from the FM-1 (command 0x${cmd.toString(16).padStart(2, "0")}): is fm1-dock running?`);
  }

  async hello() {
    const d = await this.ask(CMD.HELLO);
    const w = (i) => rd32(d, 8 + 4 * i);
    // room: every add-on area's bytes, imageEnd: where the image's room starts (dock.c room); null from an fm1-dock before it
    // fx: the effects' ABI this fm1-dock takes (0: one before effects)
    return { proto: d[1], abi: d[2], n: d[3], cur: d[4], receiving: d[5], fx: d[6], largest: w(0), free: w(1),
      maxFile: w(2), crashPc: w(3), crashModule: w(4), worst: w(5), overruns: w(6), logW: w(7), block: w(8),
      room: d.length >= 68 ? rd32(d, 60) : null, imageEnd: d.length >= 68 ? rd32(d, 64) : null };
  }

  // INFO (fm1-dock after 0.8; dock.c info_fill): the build, the board, the chips, the room. null from an older one:
  // it answers "bad message", or nothing in time; asked once, so an older device connects as before
  async info() {
    let d;
    try { d = await this.ask(CMD.INFO, new Uint8Array(0), { timeout: 400, tries: 1 }); } catch (_) { return null; }
    if (d.length < 88 || d[0] !== 0 || d[1] < 1) return null;
    const hex = (v, n) => v.toString(16).toUpperCase().padStart(n, "0"), fl = rd32(d, 12), log2 = fl & 255;
    return { format: d[1], board: d[2] ? "smk37" : "fm1", cpu1: d[3], cores: d[3] === 3 || d[3] === 5 ? 2 : 1, reset: d[4],
      bleAddr: d[5] ? [...d.slice(6, 12)].reverse().map((x) => hex(x, 2)).join(":") : null,
      flashId: hex(fl, 6), flashSize: log2 >= 16 && log2 <= 26 ? 2 ** log2 : null, free: rd32(d, 16), largest: rd32(d, 20),
      modRam: rd32(d, 24), modRamUsed: rd32(d, 28), mhz: rd32(d, 32), soc: cstr(d, 36, 44), version: cstr(d, 44, 60),
      build: cstr(d, 60, 72), model: cstr(d, 72, 88), room: d.length >= 96 ? rd32(d, 88) : null,
      imageEnd: d.length >= 96 ? rd32(d, 92) : null };
  }

  async modules() {
    const seq = this.send(CMD.LIST);
    const out = [];
    await this.collect(seq, 1000, (r) => {
      if (r.cmd !== CMD.LIST) return false;
      const d = r.d;
      if (d[1] === 0xff) return true;
      out.push({ index: d[1], state: STATE[d[2]] ?? String(d[2]), kind: KIND[d[3]] ?? String(d[3]),
        version: rd32(d, 4), size: rd32(d, 8), at: rd32(d, 12), name: cstr(d, 16, 40) });
      return false;
    });
    return out;
  }

  async log() {
    const h = await this.hello();
    let from = Math.max(0, h.logW - 2048);
    const bytes = [];
    while (from < h.logW) {
      const d = await this.ask(CMD.LOG, Uint8Array.from(u32(from)));
      const at = rd32(d, 8), chunk = d.subarray(12);
      if (!chunk.length) break;
      bytes.push(...chunk);
      from = at + chunk.length;
    }
    return { text: String.fromCharCode(...bytes), hello: h };
  }

  async command(cmd, name) {
    const d = await this.ask(cmd, name === undefined ? new Uint8Array(0) : name24(name), { timeout: 10000, tries: 1 });
    return status(d);
  }

  // a module (or, with run, a probe) into the FM-1: BEGIN, DATA..., COMMIT / RUN; progress(0..1, text)
  async deploy(blob, progress = () => {}, run = null) {
    const info = moduleInfo(blob);
    const h = await this.hello();
    if (h.abi !== info.abi) throw new Error(`the FM-1's fm1-dock speaks ABI ${h.abi}, ${info.name} was built for ${info.abi}`);
    if (!info.boot && blob.length > h.maxFile) throw new Error(`${info.name}: ${blob.length} B, fm1-dock takes up to ${h.maxFile} B`);
    const begin = cat(u32(blob.length), u32(crc32(blob)), name24(info.name));
    let d = await this.ask(CMD.BEGIN, begin, { timeout: 2000, tries: 4 });
    if (d[0]) throw new Error(`${info.name}: ${status(d)} (at BEGIN)`);
    for (let off = 0; off < blob.length; off += CHUNK) {
      d = await this.ask(CMD.DATA, cat(u32(off), blob.subarray(off, off + CHUNK)), { timeout: 2000, tries: 4 });
      if (d[0]) { this.send(CMD.ABORT); throw new Error(`${info.name}: ${status(d)} (at ${off})`); }
      progress(Math.min(1, (off + CHUNK) / blob.length), `${Math.min(blob.length, off + CHUNK)} / ${blob.length} B`);
    }
    if (run) return run();
    progress(1, "writing the flash…");
    d = await this.ask(CMD.COMMIT, new Uint8Array(0), { timeout: 30000, tries: 1 });
    if (d[0]) throw new Error(`${info.name}: ${status(d)} (at COMMIT)`);
    return { at: rd32(d, 4), info };
  }

  // ---- the modules' banks (their user presets; dock.c "the banks from the computer", tools/hbdeploy.py)

  // the banks in use: { banks: [{ slot, name (null: of no module on the unit), idx, key, size, crc }], slots, free },
  // or null from an fm1-dock without the commands (0.9 and before: it answers "bad message", or nothing in time)
  async banks() {
    const seq = this.send(CMD.BANKS);
    const banks = [];
    let end = null;
    await this.collect(seq, 600, (r) => {
      if (r.cmd !== CMD.BANKS) return r.cmd === 0x3f;
      const d = r.d;
      if (d[0] !== 0 || d.length < 4) return true;
      if (d[1] === 0xff) { end = { slots: d[2], free: d[3] }; return true; }
      banks.push({ slot: d[1], name: d[2] === 0xff ? null : cstr(d, 20, 44), idx: rd32(d, 16), key: rd32(d, 4),
        size: rd32(d, 8), crc: rd32(d, 12) });
      return false;
    });
    return end ? { banks, ...end } : null;
  }

  // bank b's bytes (an entry of banks()), checked against its CRC
  async bankRead(b) {
    const out = new Uint8Array(b.size);
    for (let at = 0; at < b.size;) {
      const d = await this.ask(CMD.BANK_READ, cat([b.slot, 0, 0, 0], u32(at)));
      if (d[0] !== 0 || d.length <= 8 || rd32(d, 4) !== at) throw new Error(`bank ${b.slot}: ${status(d)}`);
      out.set(d.subarray(8, 8 + Math.min(d.length - 8, b.size - at)), at);
      at += d.length - 8;
    }
    if (crc32(out) !== b.crc) throw new Error("the presets changed while they were read (the synth saved?): try again");
    return out;
  }

  // one bank onto the unit, in place of that bank of the module `name`; the module is started again if it plays.
  // -> "done", or the status fm1-dock gave ("not found": the module is not on the unit, "no space": no bank free)
  async bankWrite(name, idx, data) {
    let d = await this.ask(CMD.BANK_WRITE, cat([0x42, 0, idx & 0xff, idx >> 8], u32(data.length), u32(crc32(data)), name24(name)));
    if (d[0]) return status(d);
    for (let at = 0; at < data.length; at += CHUNK) {
      d = await this.ask(CMD.BANK_WRITE, cat([0x44, 0, 0, 0], u32(at), data.subarray(at, at + CHUNK)));
      if (d[0]) return status(d);
    }
    d = await this.ask(CMD.BANK_WRITE, Uint8Array.of(0x45), { timeout: 5000, tries: 1 });
    return status(d);
  }

  async bankErase(b) {
    return status(await this.ask(CMD.BANK_ERASE, cat([b.slot, 0, 0, 0], u32(b.key)), { timeout: 3000, tries: 1 }));
  }

  async result() {
    let from = 0, head = null;
    const out = [];
    for (;;) {
      const d = await this.ask(CMD.RESULT, Uint8Array.from(u32(from)));
      head = { state: d[1], id: rd32(d, 4), rc: rdi32(d, 8), ticks: rd32(d, 12), outN: rd32(d, 16), pc: rd32(d, 20) };
      const at = rd32(d, 24), chunk = d.subarray(28);
      out.push(...chunk);
      from = at + chunk.length;
      if (!chunk.length || from >= head.outN) break;
    }
    const r = { state: PROBE_STATE[head.state] ?? head.state, id: head.id, rc: head.rc,
      us: Math.round(head.ticks / TICKS_PER_US * 10) / 10 };
    if (head.state === 3) r.crash_pc = hex32(head.pc);
    r.result = decodeProbe(Uint8Array.from(out.slice(0, head.outN)));
    return r;
  }

  // a probe: sent as a deploy, run from RAM (nothing goes into flash), its result
  async probe(blob, arg, progress = () => {}, waitMs = 4000) {
    const id = (Date.now() / 1000) & 0x7fffffff;
    return this.deploy(blob, progress, async () => {
      progress(1, "running…");
      const seq = this.send(CMD.RUN, cat(u32(id), arg));
      let reply = null;
      await this.collect(seq, waitMs + 2000, (r) => {
        if (r.cmd === CMD.RUN || r.cmd === 0x3f) { reply = r.d; return true; }
        return false;
      });
      if (reply && reply[0]) throw new Error(`probe: ${status(reply)}`);
      if (reply) return this.result();
      return null;                              // crashed or hung: the FM-1 resets; ask RESULT once it is back
    });
  }
}

// ---- a .hbk: banks in a file (a backup of a unit, or one synth's presets), as tools/hbdeploy.py hbk_pack has it:
// "HBKF", u16 format 1, u16 count, 8 bytes 0; then per bank the module's name[24], u16 idx, u16 0, u32 the module's
// version (0: not known), u32 length, u32 CRC-32, the bytes
export const BANK_MAX = 8176;

export function hbkPack(entries) {              // [{ name, idx, version, data }]
  const u16 = (v) => [v & 0xff, (v >> 8) & 0xff];
  const parts = [[0x48, 0x42, 0x4b, 0x46], u16(1), u16(entries.length), new Uint8Array(8)];
  for (const e of entries)
    parts.push(name24(e.name), u16(e.idx), u16(0), u32(e.version || 0), u32(e.data.length), u32(crc32(e.data)), e.data);
  return cat(...parts);
}

export function hbkUnpack(blob) {               // -> [{ name, idx, version, data }], or throws
  if (blob.length < 16 || blob[0] !== 0x48 || blob[1] !== 0x42 || blob[2] !== 0x4b || blob[3] !== 0x46)
    throw new Error("not a preset file of fm1-dock (.hbk)");
  if ((blob[4] | blob[5] << 8) !== 1) throw new Error("a preset file of a newer format: update this page");
  const out = [];
  let at = 16;
  for (let n = blob[6] | blob[7] << 8; n; n--) {
    if (at + 40 > blob.length) throw new Error("the preset file is cut short");
    const size = rd32(blob, at + 32), data = blob.subarray(at + 40, at + 40 + size);
    if (!size || size > BANK_MAX || data.length !== size || crc32(data) !== rd32(blob, at + 36))
      throw new Error("the preset file is damaged");
    out.push({ name: cstr(blob, at, at + 24), idx: blob[at + 24] | blob[at + 25] << 8, version: rd32(blob, at + 28), data });
    at += 40 + size;
  }
  return out;
}

// a probe's argument: "1,2,0x30" (32-bit words) or "hex:0a0b0c"
export function probeArg(s) {
  s = s.trim();
  if (!s) return new Uint8Array(0);
  if (s.startsWith("hex:")) {
    const h = s.slice(4).replace(/\s+/g, "");
    return Uint8Array.from(h.match(/../g) || [], (x) => parseInt(x, 16));
  }
  return cat(...s.split(",").filter((x) => x.trim()).map((x) => u32(Number(x.trim()) >>> 0)));
}

// fm1-dock.syms (llvm-objdump -t): the code's symbols, for a PC
export class Syms {
  constructor(text) {
    this.list = [];
    for (const ln of text.split("\n")) {
      const m = /^([0-9a-f]{8})\s.*?\s(\.\S*text\S*)\s+([0-9a-f]{8})\s+(\S+)$/.exec(ln.trim());
      if (m) this.list.push({ at: parseInt(m[1], 16), size: parseInt(m[3], 16), name: m[4] });
    }
    this.list.sort((a, b) => a.at - b.at);
  }

  get size() { return this.list.length; }

  // the symbol at or before pc (a size of 0: an assembly label, up to the next one)
  lookup(pc) {
    let lo = 0, hi = this.list.length - 1, i = -1;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      if (this.list[mid].at <= pc) { i = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (i < 0) return null;
    const s = this.list[i], next = this.list[i + 1];
    if (s.size ? pc - s.at >= s.size : next && pc >= next.at) return null;
    return pc === s.at ? s.name : `${s.name}+0x${(pc - s.at).toString(16)}`;
  }
}

export { sleep, rd32 };
