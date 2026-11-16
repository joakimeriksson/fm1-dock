// SPDX-License-Identifier: GPL-3.0-only
// What the ESQ-1 module's page does with the files a visitor brings (joakimeriksson/fm1-esq, site/): the SQ-80's
// wave ROMs, its factory disks' dumps, song files. Nothing here talks to a device or a server: bytes in, bytes out,
// so the same file runs under Node (fm1-esq's tests/pack_js.mjs holds it to tools/esq_pack.py's and
// tools/sq80_disk.py's bytes).
//
// A pack ("ESQP", esq1/esq_module.c): the pages of the ROMs a song's programs read, and the song.
//    0  "ESQP", 1
//    5  flags: 1 a song follows the waves, 2 it plays at once
//    6  what plays: a song 0..19, a sequence 20..79
//    7  n, the runs of pages
//    8  the sequencer's memory's bytes (4, little endian)
//   12  n times: the ROM 0..3, its first page, the pages (2)
//       the runs' bytes as the ROMs have them; the memory, its 512 bytes, the bank's 4080
import { RAW, ZONES } from "./esq1tables.js";       // (generated from fm1-esq's tools/esq_data.py)

export const POOL = 220 * 1024;                     // the module's room for waves and sequences together
export const OWN_SONG = 4096;
export const SNG = 65536 + 512, BANK = 4080, PCB = 102;
export const PACK_ARG = 0x50515345;                 // BEGIN's arg for a pack: "ESQP"
export const ROM_CRC = [0xDFFD538C, 0x9BE8CCEB, 0x4937C6F7, 0x0F917D40];   // 2202.bin .. 2205.bin
export const HALF_CRC = [0x4D04AC87, 0x94C554A3];   // esq1wavlo.bin, esq1wavhi.bin: the first ROM in two

const CRC_T = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) { let c = i; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[i] = c >>> 0; }
  return t;
})();
export function crc32(b) {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < b.length; i++) c = CRC_T[(c ^ b[i]) & 255] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}
const latin = (b) => String.fromCharCode(...b);
const cat = (parts) => {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) { out.set(p, at); at += p.length; }
  return out;
};

// ------------------------------------------------------------------ the sequencer's memory ---

export const ptr = (sng, i) => sng[65536 + 0x39 + 2 * i] << 8 | sng[65536 + 0x3A + 2 * i];
export const length = (sng, p) => (p + 1 < 65536 ? sng[p] << 8 | sng[p + 1] : 0);
export function songs(sng) {
  const out = [];
  for (let n = 0; n < 20; n++) if (sng[65536 + 0x115 + n] !== 0xFF && length(sng, ptr(sng, 60 + n)) >= 10) out.push(n);
  return out;
}
export function sequences(sng) {
  const out = [];
  for (let n = 0; n < 60; n++) if (sng[65536 + 0xD9 + n] !== 0xFF && length(sng, ptr(sng, n)) > 34) out.push(n);
  return out;
}
export const songName = (sng, n) => latin(sng.subarray(65536 + 0x129 + 6 * n, 65536 + 0x12F + 6 * n)).trim();

// the sequences song `what` plays (what 20..79: that sequence alone)
export function songSequences(sng, what) {
  if (what >= 20) return [what - 20];
  const p = ptr(sng, 60 + what), ok = new Set(sequences(sng)), out = new Set();
  for (let q = p + 6; q < Math.min(p + length(sng, p) - 1, 65533); q += 3) if (ok.has(sng[q])) out.add(sng[q]);
  return [...out].sort((a, b) => a - b);
}
export function songPrograms(sng, what) {
  const out = new Set();
  for (const s of songSequences(sng, what)) {
    const p = ptr(sng, s);
    for (let t = 0; t < 8; t++) if (sng[p + 9 + 3 * t] !== 0xFF) out.add(sng[p + 9 + 3 * t]);
  }
  return [...out].sort((a, b) => a - b);
}
// { mem, hdr }: what `what` plays alone, from the memory's start, and the 512 bytes for it
export function compact(sng, what) {
  const keep = songSequences(sng, what).concat(what < 20 ? [60 + what] : []);
  const hdr = sng.slice(65536, 65536 + 512), parts = [];
  let at = 0;
  for (let i = 0; i < 80; i++) { hdr[0xD9 + i] = 0xFF; hdr[0x39 + 2 * i] = hdr[0x3A + 2 * i] = 0; }
  for (const i of keep) {
    const p = ptr(sng, i), part = sng.subarray(p, p + length(sng, p));
    hdr[0x39 + 2 * i] = at >> 8; hdr[0x3A + 2 * i] = at & 255;
    hdr[0xD9 + i] = sng[65536 + 0xD9 + i];
    parts.push(part);
    at += part.length;
  }
  return { mem: cat(parts), hdr };
}

// ------------------------------------------------------------------ the waves a program reads ---

// a page is ROM * 256 + its number in the ROM
function programPages(pcb, into) {
  for (let o = 0; o < 3; o++) {
    const w = pcb[58 + 10 * o + 5];
    for (const r of new Set(ZONES[w < ZONES.length ? w : 0])) {
      const [pg, wsr] = RAW[r], rom = wsr >> 6, n = 1 << ((wsr >> 3) & 7);
      for (let p = pg; p < pg + n; p++) into.add(rom * 256 + p);
    }
  }
}
// { pages: the pages the song wants, other: a track names a program outside the bank (one of the module's own) }
export function songPages(sng, bank, what) {
  const pages = new Set();
  let other = false;
  for (const prog of songPrograms(sng, what)) {
    if (prog < 40) programPages(bank.subarray(prog * PCB, (prog + 1) * PCB), pages);
    else other = true;
  }
  return { pages, other };
}
function runs(pages) {
  const out = [];
  for (const k of [...pages].sort((a, b) => a - b)) {
    const last = out[out.length - 1];
    if (last && last[0] === k >> 8 && last[1] + last[2] === (k & 255)) last[2]++;
    else out.push([k >> 8, k & 255, 1]);
  }
  return out;
}

// rom: the four ROMs' bytes (null where there is none); sng (66048 bytes) and bank (4080), or null for waves alone;
// waves: "song" (the song's tables, and the ESQ-1's ROM whole when there is room), "rom1" (the ESQ-1's ROM alone),
// "none". -> { bytes, notes: what was left out, for whoever reads }
export function build(rom, sng = null, bank = null, what = 0, play = false, waves = "song") {
  const notes = [];
  let pages = new Set(), mem = new Uint8Array(0), hdr = new Uint8Array(0);
  if (sng) ({ mem, hdr } = compact(sng, what));
  const room = Math.floor((POOL - 512 - Math.max(mem.length, OWN_SONG)) / 256);
  const whole = Array.from({ length: 256 }, (_, p) => p);
  if (waves === "rom1") pages = new Set(whole);
  else if (waves === "song" && sng) {
    const sp = songPages(sng, bank, what);
    pages = sp.pages;
    if (sp.other || new Set([...pages, ...whole]).size <= room) for (const p of whole) pages.add(p);
    if (pages.size > room) {                         // more than there is room for: the last tables stay out
      const left = [...pages].sort((a, b) => a - b).slice(room);
      for (const p of left) pages.delete(p);
      notes.push(`${left.length * 256} bytes of waves do not fit: ROM ${(left[0] >> 8) + 1} from page ${left[0] & 255} on is left out`);
    }
  }
  const missing = [...new Set([...pages].map((p) => p >> 8))].filter((r) => !rom[r]).sort();
  if (missing.length) {
    notes.push(`no ROM ${missing.map((r) => r + 1).join(", ")}: its waves are silent`);
    pages = new Set([...pages].filter((p) => rom[p >> 8]));
  }
  const rr = runs(pages);
  if (rr.length > 60) throw new Error(`${rr.length} runs of pages, the module takes 60`);
  const flags = (sng ? 1 : 0) | (play && sng ? 2 : 0), m = mem.length;
  const parts = [Uint8Array.from([0x45, 0x53, 0x51, 0x50, 1, flags, what, rr.length, m & 255, (m >> 8) & 255, (m >> 16) & 255, m >>> 24])];
  for (const [r, pg, n] of rr) parts.push(Uint8Array.of(r, pg, n & 255, n >> 8));
  for (const [r, pg, n] of rr) parts.push(rom[r].subarray(pg * 256, (pg + n) * 256));
  if (sng) parts.push(mem, hdr, bank);
  return { bytes: cat(parts), notes };
}

// the MODULE payloads that send a file to the module playing (hb_module.h): B [size][arg], D [at][bytes], E
export function fileSteps(bytes, arg = PACK_ARG) {
  const u32 = (v) => [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255];
  const steps = [Uint8Array.from([0x42, ...u32(bytes.length), ...u32(arg)])];
  for (let at = 0; at < bytes.length; at += 256) steps.push(cat([Uint8Array.from([0x44, ...u32(at)]), bytes.subarray(at, at + 256)]));
  steps.push(Uint8Array.of(0x45));
  return steps;
}

// ------------------------------------------------------------------ an SQ-80 disk's dump ---

// Rainer Buchty's SQ80 Toolkit format: "!SQ80DISK!", then 80 tracks of two sides, a side five sectors of 1024 bytes
// and one of 512. Where a sequencer file starts (track, side, sector, and whether the side has changed on this track)
const SEQ_START = [[0, 0, 0, 0], [6, 0, 4, 0], [12, 1, 3, 1], [19, 1, 2, 0], [25, 0, 1, 1], [32, 0, 0, 0], [38, 0, 4, 0],
  [44, 1, 3, 1], [51, 1, 2, 0], [57, 0, 1, 1]];
const SEQ_END = [[6, 0], [12, 1], [19, 1], [25, 0], [31, 0], [38, 0], [44, 1], [51, 1], [57, 0], [63, 0]];
const DISK_MAGIC = "!SQ80DISK!";
export const isDisk = (b) => b.length > 10 && latin(b.subarray(0, 10)) === DISK_MAGIC;

// -> { files: [{ name, sng, bank | null }] (the disk's sequencer files; its n-th bank goes with its n-th file),
//      banks: [{ name, bank }] }
export function disk(dump) {
  if (!isDisk(dump)) throw new Error("not an SQ80 Toolkit disk dump");
  const d = dump.subarray(10);
  const sector = (t, h, s) => { const o = t * 11264 + h * 5632 + (s === 5 ? 5120 : s * 1024); return d.subarray(o, o + (s === 5 ? 512 : 1024)); };
  const dir = cat([[0, 0], [0, 1], [1, 1], [1, 0]].map(([t, h]) => sector(t, h, 5)));
  const ent = Array.from({ length: 50 }, (_, i) => ({ type: dir[i * 13], name: latin(dir.subarray(i * 13 + 1, i * 13 + 11)).replace(/[\0 ]+$/, "") }));
  const bank = (n) => {
    const t = 64 + Math.floor((n % 20) * 4 / 5), s = (n % 20) * 4 % 5, h = Math.floor(n / 20);
    return cat([0, 1, 2, 3].map((i) => sector(t + Math.floor((s + i) / 5), h, (s + i) % 5))).slice(0, BANK);
  };
  const seq = (n) => {
    let [t, h, s, hc] = SEQ_START[n];
    const out = [];
    for (let k = 0; k < 64; k++) {
      out.push(sector(t, h, s));
      if (s < 4) s++;
      else { s = 0; if (hc) { t++; hc = 0; } else { h ^= 1; hc = 1; } }
    }
    out.push(sector(SEQ_END[n][0], SEQ_END[n][1], 5));
    return cat(out);
  };
  const banks = [];
  for (let i = 0; i < 40; i++) if (ent[10 + i].name) banks.push({ name: ent[10 + i].name, bank: bank(i) });
  const files = [];
  for (let i = 0; i < 10; i++) if (ent[i].type) files.push({ name: ent[i].name, sng: seq(i), bank: ent[10 + i].name ? bank(i) : null });
  return { files, banks };
}

// ------------------------------------------------------------------ what a file is ---

async function inflate(bytes, format) {
  const s = new Blob([bytes]).stream().pipeThrough(new DecompressionStream(format));
  return new Uint8Array(await new Response(s).arrayBuffer());
}
export const gunzip = (bytes) => inflate(bytes, "gzip");
// a .zip's files: [{ name, bytes }] (stored or deflated ones; the others are left out)
export async function unzip(b) {
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength), out = [];
  let e = b.length - 22;
  while (e >= 0 && v.getUint32(e, true) !== 0x06054b50) e--;
  if (e < 0) throw new Error("not a .zip");
  let at = v.getUint32(e + 16, true);
  for (let n = v.getUint16(e + 10, true); n > 0 && at + 46 <= b.length && v.getUint32(at, true) === 0x02014b50; n--) {
    const method = v.getUint16(at + 10, true), size = v.getUint32(at + 20, true), local = v.getUint32(at + 42, true);
    const nameLen = v.getUint16(at + 28, true);
    const name = latin(b.subarray(at + 46, at + 46 + nameLen));
    at += 46 + nameLen + v.getUint16(at + 30, true) + v.getUint16(at + 32, true);
    if (local + 30 > b.length || name.endsWith("/")) continue;
    const start = local + 30 + v.getUint16(local + 26, true) + v.getUint16(local + 28, true);
    const raw = b.subarray(start, start + size);
    if (method === 0) out.push({ name, bytes: raw.slice() });
    else if (method === 8) out.push({ name, bytes: await inflate(raw, "deflate-raw") });
  }
  return out;
}

// What a visitor's file holds, as a list (a .zip or a disk holds several things):
//   { kind: "rom", rom: 0..3, bytes, sure }       a wave ROM (sure: its CRC is the known dump's; else by its name)
//   { kind: "half", half: 0 | 1, bytes }          esq1wavlo.bin / esq1wavhi.bin: the first ROM in two
//   { kind: "songs", name, sng, bank | null }     a sequencer file (a disk's, a .sqpack, a .sng: bank null)
//   { kind: "bank", name, bank }                  forty programs
//   { kind: "unknown", name }
export async function identify(name, bytes) {
  const base = name.replace(/^.*[\\/]/, "");
  if (bytes[0] === 0x1f && bytes[1] === 0x8b) return identify(base.replace(/\.gz$/i, ""), await gunzip(bytes));
  if (bytes[0] === 0x50 && bytes[1] === 0x4b) {
    const out = [];
    for (const f of await unzip(bytes)) out.push(...await identify(f.name, f.bytes));
    return out;
  }
  if (isDisk(bytes)) {
    const dk = disk(bytes);
    return [...dk.files.map((f) => ({ kind: "songs", name: `${base}: ${f.name}`, sng: f.sng, bank: f.bank })),
      ...dk.banks.map((k) => ({ kind: "bank", name: `${base}: ${k.name}`, bank: k.bank }))];
  }
  const stem = base.replace(/\.[^.]*$/, "");
  if (bytes.length === 65536) {
    const crc = crc32(bytes), byCrc = ROM_CRC.indexOf(crc), byName = /220([2-5])/.exec(base);
    if (byCrc >= 0) return [{ kind: "rom", rom: byCrc, bytes, sure: true }];
    if (byName) return [{ kind: "rom", rom: +byName[1] - 2, bytes, sure: false }];
  }
  if (bytes.length === 32768) {
    const half = HALF_CRC.indexOf(crc32(bytes));
    if (half >= 0) return [{ kind: "half", half, bytes }];
  }
  if (bytes.length === SNG + BANK) return [{ kind: "songs", name: stem, sng: bytes.slice(0, SNG), bank: bytes.slice(SNG) }];
  if (bytes.length === SNG) return [{ kind: "songs", name: stem, sng: bytes, bank: null }];
  if (bytes.length === BANK) return [{ kind: "bank", name: stem, bank: bytes }];
  return [{ kind: "unknown", name: base }];
}
