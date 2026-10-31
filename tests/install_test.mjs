// SPDX-License-Identifier: GPL-3.0-only
// docs/install/fm1install.js on node: the package checks (on the real packages in DIR, from
// tools/site_packages.py), and the whole update against a fake FM-1 (after Felucca's web/test_web.mjs FakeFM1,
// by Leo Kuroshita, Hügelton Instruments, GPL-3.0).
//   node tests/install_test.mjs [DIR]
import { readFileSync, existsSync } from "node:fs";
import { checkPackage, packageDamage, logicalImage, productOf, installOn, entryFor, addModule, playTune, tuneSteps,
  inspectDevice, removeModule } from "../docs/install/fm1install.js";
import { pack7, unpack7, Updater, sameKeyboard } from "../docs/install/fm1ota.js";
import { frame, parse, moduleInfo, crc32, CMD } from "../docs/dock.js";

let failed = 0;
const ok = (c, what) => { console.log(`${c ? "ok  " : "FAIL"} ${what}`); if (!c) failed++; };
const eq = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const reject = (p) => p.then(() => null, (e) => e);

// ------------------------------------------------------------------ packages ---
const dir = process.argv[2];
if (dir && existsSync(`${dir}/index.json`)) {
  const index = JSON.parse(readFileSync(`${dir}/index.json`, "utf8"));
  const all = Object.entries(index.firmwares).flatMap(([id, e]) =>
    [[id, e], ...Object.entries(e.variants || {}).map(([model, v]) => [`${id} (${model})`, v])]);   // (the SMK-37's too)
  for (const [id, entry] of all) {
    const raw = new Uint8Array(readFileSync(`${dir}/${entry.file}`));
    const pkg = await checkPackage(raw, entry);
    ok(pkg.product === entry.product && pkg.image.length === raw.length - 20, `${id}: ${entry.file} passes, ${pkg.product}`);
    const bad = raw.slice();
    bad[raw.length - 4000] ^= 0x55;                        // a byte near the end (the update loader)
    ok(packageDamage(logicalImage(bad)) !== null, `${id}: a changed byte is caught (${packageDamage(logicalImage(bad))})`);
    const app = raw.slice();
    app[0x20000] ^= 0x01;                                  // one bit in the firmware's own file
    ok(packageDamage(logicalImage(app)) !== null, `${id}: one bit in the firmware is caught (${packageDamage(logicalImage(app))})`);
    const e = await reject(checkPackage(bad, entry));
    ok(e && e.code === "badpkg", `${id}: ... and refused (${e && e.message})`);
    const e2 = await reject(checkPackage(raw, { ...entry, product: "FM-1_999" }));
    ok(e2 && e2.code === "badpkg", `${id}: another identity than the list's is refused`);
  }
} else console.log("(no package dir: the package checks are skipped)");

// ------------------------------------------------------------------ the update ---
const HS = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7];
const UPGRADE = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7];

// an FM-1 on WebMIDI: identity, then "device asks, host answers" reads of the image; boots into the loader
// after step 1, into the new firmware (identity `product`) after step 2
class FakeFM1 {
  constructor(image, identity, product, loader = "ota-FM-1_900") {
    this.image = image; this.product = product; this.served = 0; this.bad = 0; this.sent = 0; this.upgrades = 0;
    this.loader = loader;                       // the update loader it boots into (the package's: ota-<model>_900)
    this.access = { inputs: new Map(), outputs: new Map() };
    this.boot(identity, identity.startsWith("ota-") ? "Felucca Update" : "Felucca");
  }
  boot(identity, name) {
    this.identity = identity; this.waiting = null; this.queue = [];
    for (const m of [this.access.inputs, this.access.outputs]) { for (const p of m.values()) p.state = "disconnected"; m.clear(); }
    const id = Math.random().toString(36).slice(2);
    this.input = { id: "i" + id, name, state: "connected", onmidimessage: null, open: async () => {} };
    this.output = { id: "o" + id, name, state: "connected", open: async () => {}, send: (d) => {
      if (this.output.state !== "connected") throw new Error("InvalidStateError");
      this.sent++;
      setTimeout(() => this.rx(Array.from(d)), 1);
    } };
    this.access.inputs.set(this.input.id, this.input);
    this.access.outputs.set(this.output.id, this.output);
  }
  tx(bytes) { const i = this.input; setTimeout(() => { if (i.state === "connected" && i.onmidimessage) i.onmidimessage({ data: Uint8Array.from(bytes) }); }, 1); }
  rx(d) {
    if (eq(d, HS)) {
      const t = [...new TextEncoder().encode(this.identity)];
      this.tx([0xF0, ...pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...new Array(28 - t.length).fill(0)]), 0xF7]);
    } else if (eq(d, UPGRADE)) {
      this.upgrades++;
      this.queue = this.identity.startsWith("ota-")
        ? [...Array.from({ length: Math.ceil(this.image.length / 512) }, (_, k) => [k * 512, Math.min(512, this.image.length - k * 512)]), [0xF0000000, 8]]
        : [[0, 64], [0x40, 160], [0x1000, 512], [0xE0000000, 8]];
      this.next();
    } else if (this.waiting) {
      const u = unpack7(d.slice(1, -1));
      const [addr, len] = this.waiting;
      const got = u.slice(14, 14 + (addr >= 0xE0000000 ? 8 : len));
      const want = addr >= 0xE0000000 ? [...new TextEncoder().encode("success"), 0] : Array.from(this.image.subarray(addr, addr + len));
      if (!eq(got, want)) this.bad++;
      this.waiting = null;
      this.served++;
      if (addr === 0xE0000000) setTimeout(() => this.boot(this.loader, "Felucca Update"), 300);
      else if (addr === 0xF0000000) setTimeout(() => this.boot(this.product, "Felucca"), 300);
      else this.next();
    }
  }
  next() {
    const r = this.queue.shift();
    if (!r) return;
    this.waiting = r;
    const [addr, len] = r;
    const u = [0, 0x59, 0x30, 0, 0, 0, 0, addr & 0xFF, (addr >>> 8) & 0xFF, (addr >>> 16) & 0xFF, (addr >>> 24) & 0xFF, len & 0xFF, len >> 8, 0];
    let s = 0;
    for (let i = 6; i < 14; i++) s += u[i];
    u.push(~s & 0xFF);
    this.tx([0xF0, ...pack7(u), 0xF7]);
  }
}

const image = Uint8Array.from({ length: 0x2400 }, (_, i) => (i * 7) & 0xFF);
const pkg = { product: "FM-1_922", image };
const yes = async () => true, no = async () => false;

{
  const dev = new FakeFM1(image, "FM-1_921", "FM-1_922");
  const steps = [];
  const got = await installOn(dev.access, pkg, "FM-1 SID", yes, (k) => steps.push(k));
  ok(got === "FM-1_922" && dev.bad === 0 && steps.includes("write") && steps.at(-1) === "done",
    `install over a running firmware: check, loader, write, back as FM-1_922 (${dev.served} reads, all correct)`);
}
{
  const dev = new FakeFM1(image, "FM-1_921", "FM-1_922");
  const got = await installOn(dev.access, pkg, "FM-1 SID", no, () => {});
  ok(got === null && dev.upgrades === 0, "Cancel at the question: no update command is sent");
}
{
  const dev = new FakeFM1(image, "ota-FM-1_900", "FM-1_922");
  const got = await installOn(dev.access, pkg, "FM-1 SID", yes, () => {});
  ok(got === "FM-1_922" && dev.bad === 0, "an FM-1 left in update mode (ota-FM-1_900): the write is finished");
}
{
  const dev = new FakeFM1(image, "ota-FM-1_015", "FM-1_922");
  const e = await reject(installOn(dev.access, pkg, "FM-1 SID", yes, () => {}));
  ok(e && e.code === "foreign" && dev.upgrades === 0, "M-VAVE's update mode (ota-FM-1_015): refused, nothing sent");
}
{
  const dev = new FakeFM1(image, "FM-1_921", "FM-1_956");
  const e = await reject(installOn(dev.access, pkg, "FM-1 SID", yes, () => {}));
  ok(e && e.code === "mismatch", "back with another identity than the package's: reported (mismatch)");
}
{
  const e = await reject(installOn({ inputs: new Map(), outputs: new Map() }, pkg, "FM-1 SID", yes, () => {}));
  ok(e && e.code === "notfound", "no FM-1: notfound");
}
{
  const dev = new FakeFM1(image, "XY-9_100", "FM-1_922");
  const e = await reject(installOn(dev.access, pkg, "FM-1 SID", yes, () => {}));
  ok(e && e.code === "model", "another device than an FM-1: refused (model)");
}
ok(productOf(new Uint8Array(0x400).fill(0x7D)) === "", "productOf: no marker bytes, no identity");

// ------------------------------------------------------------------ the SMK-37 ---
const smkPkg = { product: "SMK-37 Pro_921", image };
{
  const dev = new FakeFM1(image, "SMK-37 Pro_016", "SMK-37 Pro_921", "ota-SMK-37 Pro_900");
  const got = await installOn(dev.access, smkPkg, "fm1-dock", yes, () => {});
  ok(got === "SMK-37 Pro_921" && dev.bad === 0, "SMK-37 Pro (stock): its package installs, through ota-SMK-37 Pro_900");
}
{
  const dev = new FakeFM1(image, "STARRYKEY-37 PLAY_016", "SMK-37 Pro_921", "ota-SMK-37 Pro_900");
  const got = await installOn(dev.access, smkPkg, "fm1-dock", yes, () => {});
  ok(got === "SMK-37 Pro_921", "Starrykey 37 Play: the SMK-37 Pro's package (the same keyboard)");
}
{
  const dev = new FakeFM1(image, "SMK-37 Pro_016", "FM-1_921");
  const e = await reject(installOn(dev.access, { product: "FM-1_921", image }, "fm1-dock", yes, () => {}));
  ok(e && e.code === "model" && dev.upgrades === 0, "an FM-1 package on an SMK-37: refused, nothing sent");
}
{
  const dev = new FakeFM1(image, "FM-1_921", "SMK-37 Pro_921");
  const e = await reject(installOn(dev.access, smkPkg, "fm1-dock", yes, () => {}));
  ok(e && e.code === "model" && dev.upgrades === 0, "an SMK-37 package on an FM-1: refused, nothing sent");
}
{
  const dev = new FakeFM1(image, "ota-SMK-37 Elite_900", "FM-1_921");
  const e = await reject(installOn(dev.access, { product: "FM-1_921", image }, "fm1-dock", yes, () => {}));
  ok(e && dev.upgrades === 0 && dev.served === 0, "an SMK-37 in update mode, an FM-1 package: nothing written");
}
{
  const dev = new FakeFM1(image, "ota-SMK-37 Pro_900", "SMK-37 Pro_921");
  const done = await new Updater(dev.access).resume(image, () => {});
  ok(done === false && dev.upgrades === 0, "resume without the package's identity: nothing written");
}
ok(sameKeyboard("ota-SMK-37 Elite", "SMK-37 Pro") && !sameKeyboard("FM-1", "SMK-37 Pro") && sameKeyboard("MKE-P37", "STARRYKEY-37 PLAY"),
  "sameKeyboard: the SMK-37 family is one keyboard, never with the FM-1");
{
  const entry = { file: "fm1-dock-0.7.fwsc", product: "FM-1_921", variants: {
    "SMK-37 Pro": { file: "fm1-dock-0.7-smk37.fwsc", product: "SMK-37 Pro_921" },
    "SMK-37 Elite": { file: "fm1-dock-0.7-smk37-elite.fwsc", product: "SMK-37 Elite_921" } } };
  const pick = async (identity, e = entry) => (await entryFor(new FakeFM1(image, identity, "").access, e)).file;
  ok(await pick("FM-1_921") === "fm1-dock-0.7.fwsc", "the package for an FM-1: the FM-1's");
  ok(await pick("SMK-37 Elite_016") === "fm1-dock-0.7-smk37-elite.fwsc", "for an SMK-37 Elite: the Elite's");
  ok(await pick("STARRYKEY-37 PLAY_016") === "fm1-dock-0.7-smk37.fwsc", "for a Starrykey 37 Play: the SMK-37 Pro's");
  ok(await pick("ota-SMK-37 Pro_900") === "fm1-dock-0.7-smk37.fwsc", "for an SMK-37 Pro in update mode: its own");
  const e = await reject(pick("MKE-P37_016"));
  ok(e && e.code === "nosmk", "an MKE-P37 with no package of its own on the site: refused (nosmk)");
  const e2 = await reject(pick("SMK-37 Pro_016", { file: "fm1-sid-0.3.fwsc", product: "FM-1_922" }));
  ok(e2 && e2.code === "nosmk", "an SMK-37 and a firmware without SMK-37 packages: refused (nosmk)");
}

// ------------------------------------------------------------------ a module into fm1-dock ---
// an FM-1 running fm1-dock (firmware/src/dock.c's deploys, as docs/dock.js speaks them), on WebMIDI
class FakeDock {
  constructor({ abi = 2, modules = ["SINE", "CS-1"], name = "Felucca", cur = 0, refuse = 0, largest = 400000 } = {}) {
    this.largest = largest;
    this.abi = abi; this.mods = modules; this.got = null; this.committed = null; this.frames = 0;
    this.cur = cur; this.refuse = refuse; this.tune = null; this.played = null;
    const listeners = new Set();
    this.input = { name, state: "connected", addEventListener: (_, f) => listeners.add(f), removeEventListener: (_, f) => listeners.delete(f) };
    this.output = { name, state: "connected", send: (f) => { this.frames++; setTimeout(() => this.rx(f), 0); } };
    this.reply = (cmd, seq, d) => { const m = frame(cmd | 0x40, seq, Uint8Array.from(d)); setTimeout(() => listeners.forEach((f) => f({ data: m })), 0); };
    const map = (p) => ({ forEach: (fn) => fn(p), values: () => [p].values() });
    this.logText = "10 fm1-dock 0.6 boot, reset 00000001\n20 started SINE\n30 fm1-dock 0.7 boot, reset 00000001\n";
    this.access = { inputs: map(this.input), outputs: map(this.output) };
  }
  rx(f) {
    const r = parse(Uint8Array.from(f)), d = r.d;
    const u32 = (v) => [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255];
    if (r.cmd === CMD.HELLO) this.reply(r.cmd, r.seq, [0, 1, this.abi, this.mods.length, this.cur, 0, 0, 0, ...u32(this.largest), ...u32(500000), ...u32(300000), ...new Array(16).fill(0), ...u32(this.logText.length), ...new Array(4).fill(0)]);
    else if (r.cmd === CMD.LIST) {
      this.mods.forEach((n, i) => this.reply(r.cmd, r.seq, [0, i, 0, 2, ...u32(1), ...u32(1000), ...u32(0), ...new TextEncoder().encode(n.padEnd(24, "\0"))]));
      this.reply(r.cmd, r.seq, [0, 0xff]);
    } else if (r.cmd === CMD.BEGIN) {
      const len = d[0] | (d[1] << 8) | (d[2] << 16) | (d[3] << 24);
      this.got = new Uint8Array(len); this.crc = (d[4] | (d[5] << 8) | (d[6] << 16) | (d[7] << 24)) >>> 0;
      this.reply(r.cmd, r.seq, [0]);
    } else if (r.cmd === CMD.DATA) {
      const off = d[0] | (d[1] << 8) | (d[2] << 16) | (d[3] << 24);
      this.got.set(d.subarray(4), off);
      this.reply(r.cmd, r.seq, [0]);
    } else if (r.cmd === CMD.SELECT) {
      const name = new TextDecoder().decode(d.subarray(0, 24)).split("\0")[0];
      const i = this.mods.indexOf(name);
      if (i >= 0) this.cur = i;
      this.reply(r.cmd, r.seq, [i >= 0 ? 0 : 9]);
    } else if (r.cmd === CMD.MODULE) {
      const rd = (o) => (d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24)) >>> 0;
      if (this.mods[this.cur] !== "FM-1 SID") { this.reply(r.cmd, r.seq, [9]); return; }
      if (this.refuse) { this.reply(r.cmd, r.seq, [11, this.refuse]); return; }
      if (d[0] === 0x42) this.tune = { bytes: new Uint8Array(rd(1)), song: rd(5) };
      else if (d[0] === 0x44) this.tune.bytes.set(d.subarray(5), rd(1));
      else if (d[0] === 0x45) this.played = this.tune;
      this.reply(r.cmd, r.seq, [0]);
    } else if (r.cmd === CMD.LOG) {
      const from = d[0] | (d[1] << 8) | (d[2] << 16) | (d[3] << 24), w = this.logText.length;
      const chunk = [...new TextEncoder().encode(this.logText.slice(from, from + 180))];
      this.reply(r.cmd, r.seq, [0, 0, 0, 0, ...u32(w), ...u32(from), ...chunk]);
    } else if (r.cmd === CMD.DELETE) {
      const name = new TextDecoder().decode(d.subarray(0, 24)).split("\0")[0], i = this.mods.indexOf(name);
      if (i >= 0) { this.mods = this.mods.filter((m) => m !== name); this.deleted = name; }
      this.reply(r.cmd, r.seq, [i >= 0 ? 0 : 9]);
    } else if (r.cmd === CMD.COMMIT) {
      const good = crc32(this.got) === this.crc;
      if (good) this.committed = this.got;
      this.reply(r.cmd, r.seq, [good ? 0 : 1, 0, 0, 0, ...u32(0x96000)]);
    }
  }
}

if (dir && existsSync(`${dir}/index.json`)) {
  const index = JSON.parse(readFileSync(`${dir}/index.json`, "utf8"));
  for (const [id, entry] of Object.entries(index.modules || {})) {
    const blob = new Uint8Array(readFileSync(`${dir}/${entry.file}`));
    const mod = { blob, info: moduleInfo(blob) };
    ok(mod.info.name === entry.name && mod.info.abi === entry.abi, `${id}: ${entry.file} is the module ${entry.name}, ABI ${entry.abi}`);
    const dev = new FakeDock();
    const asked = [];
    const got = await addModule(dev.access, mod, async (q) => { asked.push(q); return true; }, () => {});
    ok(got === entry.name && dev.committed && eq(Array.from(dev.committed), Array.from(blob)),
      `${id}: added to fm1-dock, every byte as in the file (${dev.frames} frames; asked "${asked[0]}")`);
    const dev2 = new FakeDock({ modules: ["SINE", entry.name] });
    const asked2 = [];
    await addModule(dev2.access, mod, async (q) => { asked2.push(q); return true; }, () => {});
    ok(/^Update/.test(asked2[0]), `${id}: already there: asks to update it`);
    const dev3 = new FakeDock();
    ok(await addModule(dev3.access, mod, no, () => {}) === null && !dev3.got, `${id}: Cancel: nothing sent after the question`);
    const e = await reject(addModule(new FakeDock({ abi: 1 }).access, mod, yes, () => {}));
    ok(e && e.code === "abi", `${id}: an fm1-dock of another ABI: refused`);
    const dev4 = new FakeDock({ largest: blob.length - 1 }), asked4 = [];
    const e4 = await reject(addModule(dev4.access, mod, async (q) => { asked4.push(q); return true; }, () => {}));
    ok(e4 && e4.code === "space" && !asked4.length && !dev4.got, `${id}: no room in fm1-dock: said so, nothing sent ("${e4 && e4.message}")`);
  }
}
// ------------------------------------------------------------------ a tune to FM-1 SID ---
{
  const tune = Uint8Array.from({ length: 4700 }, (_, i) => (i * 13) & 0xFF);
  tune.set([0x50, 0x53, 0x49, 0x44]);                    // "PSID"
  const dev = new FakeDock({ modules: ["SINE", "CS-1", "FM-1 SID"], cur: 2 });
  const how = await playTune(dev.access, tune, 7);
  ok(how === "playing" && dev.played && eq(Array.from(dev.played.bytes), Array.from(tune)) && dev.played.song === 7,
    `a tune to FM-1 SID playing: every byte, song 7 (${dev.frames} frames)`);
  const dev2 = new FakeDock({ modules: ["SINE", "CS-1", "FM-1 SID"], cur: 1 });
  const how2 = await playTune(dev2.access, tune, 0);
  ok(how2 === "switched" && dev2.cur === 2 && dev2.played, "CS-1 playing: switched to FM-1 SID, then the tune");
  const e = await reject(playTune(new FakeDock().access, tune, 0));
  ok(e && e.code === "nosid", "no FM-1 SID in the fm1-dock: nosid (add it first)");
  const e2 = await reject(playTune(new FakeDock({ modules: ["FM-1 SID"], refuse: 2 }).access, tune, 0));
  ok(e2 && e2.code === "refused" && /GoatTracker editor/.test(e2.message), `the GoatTracker editor open: "${e2 && e2.message}"`);
  const sidweb = process.env.FM1_SID_WEB;                 // fm1-sid's web/: the same payloads as its page sends
  if (sidweb && existsSync(`${sidweb}/fm1link.js`)) {
    const theirs = (await import(`${sidweb}/fm1link.js`)).tuneSteps(tune, 7), ours = tuneSteps(tune, 7);
    ok(theirs.length === ours.length && theirs.every((x, i) => eq(Array.from(x), Array.from(ours[i]))), "tuneSteps: the same bytes as FM-1 SID's page sends");
  }
}
{
  const blob = new Uint8Array(100);
  const e = await reject(addModule({ inputs: { forEach() {} }, outputs: { forEach() {} } }, { blob, info: { abi: 2, name: "X" } }, yes, () => {}));
  ok(e && e.code === "nodock", "no FM-1 running fm1-dock: nodock");
}

// ------------------------------------------------------------------ the catalog on fm1-dock's page ---
{
  const dev = new FakeDock({ modules: ["SINE", "FM-1 SID"] });
  const got = await inspectDevice(dev.access);
  ok(got.dock && got.dock.version === "0.7" && got.dock.modules.length === 2 && got.dock.hello.largest === 400000 && !got.ota,
    `inspectDevice: fm1-dock's HELLO, its modules, its version from the log's last boot line (${got.dock && got.dock.version})`);
  const frames = dev.frames;
  ok(await removeModule(dev.access, "FM-1 SID", no) === null && dev.mods.includes("FM-1 SID") && dev.frames > frames,
    "removeModule: Cancel at the question: nothing removed");
  ok(await removeModule(dev.access, "FM-1 SID", yes) === "done" && dev.deleted === "FM-1 SID" && !dev.mods.includes("FM-1 SID"),
    "removeModule: DELETE, the module is gone");
  const e = await reject(removeModule(dev.access, "CS-1", yes));
  ok(e && e.code === "notadded", "removeModule: a name fm1-dock has not added: refused, nothing sent");
  const v = await inspectDevice(new FakeDock().access, { log: false });
  ok(v.dock && v.dock.version === null, "inspectDevice { log: false }: no log read");
}
{
  const ota = await inspectDevice(new FakeFM1(image, "ota-FM-1_900", "FM-1_921").access);
  ok(ota.ota && ota.board === "fm1" && !ota.dock && ota.identity === "ota-FM-1_900", "inspectDevice: an FM-1 in update mode, nothing more asked");
  const none = await inspectDevice({ inputs: new Map(), outputs: new Map() });
  ok(!none.identity && !none.dock, "inspectDevice: no device");
}

// ?demo's simulated device (docs/demo.js) behaves as the real one for the page's flows
{
  const { Device, SCENES } = await import("../docs/demo.js");
  const docks = {};
  if (dir && existsSync(`${dir}/index.json`)) {
    const index = JSON.parse(readFileSync(`${dir}/index.json`, "utf8"));
    for (const [id, entry] of Object.entries(index.modules || {})) {
      const blob = new Uint8Array(readFileSync(`${dir}/${entry.file}`));
      docks[id] = { blob, info: moduleInfo(blob) };
    }
  }
  const dev = new Device(SCENES.fm1, image.length, "0.6", "FM-1_922");
  const seen = await inspectDevice(dev.access);
  ok(seen.identity === "FM-1_921" && seen.board === "fm1" && seen.dock && seen.dock.version === "0.5" &&
    seen.dock.modules.some((m) => m.name === "FM-1 SID" && m.kind === "deployed"),
    `demo: an FM-1 with fm1-dock 0.5 and FM-1 SID (free ${seen.dock && seen.dock.hello.free} B, of 296 KiB)`);
  if (docks.amb && docks.fomni) {
    const e = await reject(addModule(dev.access, docks.amb, yes, () => {}));
    ok(e && e.code === "space", "demo: AMB-1 beside FM-1 SID does not fit (space)");
    ok(await removeModule(dev.access, "FM-1 SID", yes) === "done", "demo: FM-1 SID removed");
    ok(await addModule(dev.access, docks.amb, yes, () => {}) === "AMB-1" && await addModule(dev.access, docks.fomni, yes, () => {}) === "FOMNI",
      "demo: then AMB-1 and FoMni both go in");
    const after = await inspectDevice(dev.access, { log: false });
    ok(after.dock.modules.filter((m) => m.kind === "deployed").map((m) => m.name).join() === "AMB-1,FOMNI",
      `demo: its list says so (free ${after.dock.hello.free} B, largest ${after.dock.hello.largest} B)`);
  }
  const stock = new Device(SCENES.stock, image.length, "0.6", "FM-1_922");
  const s0 = await inspectDevice(stock.access, { log: false });
  ok(s0.identity === "FM-1_015" && !s0.dock, "demo: stock firmware, no fm1-dock");
  const got = await installOn(stock.access, pkg, "fm1-dock", yes, () => {});
  const s1 = await inspectDevice(stock.access);
  ok(got === "FM-1_922" && s1.dock && s1.dock.version === "0.6", "demo: fm1-dock installed over the stock firmware, and it answers");
}

console.log(failed ? `INSTALL TESTS FAILED (${failed})` : "install tests passed");
process.exit(failed ? 1 : 0);
