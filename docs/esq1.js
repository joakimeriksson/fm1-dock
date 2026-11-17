// SPDX-License-Identifier: GPL-3.0-only
// The ESQ-1 module's page on fm1-dock's site (joakimeriksson/fm1-esq, site/): adds the module, takes the visitor's
// own wave ROMs and SQ-80 disk dumps (esq1pack.js reads them; they are kept in this browser's IndexedDB and go
// nowhere but to the FM-1), and sends a song with the wave tables it plays to the module's memory (a pack, "ESQP":
// esq1/esq_module.c's data()).
import { CMD, moduleInfo } from "./dock.js";
import { findDock, addModule, explain } from "./install/fm1install.js";
import * as P from "./esq1pack.js";

const NAME = "ESQ-1";
const $ = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const fail = (code, msg) => Object.assign(new Error(msg), { code });
// what the module's data() answers (esq_module.c)
const REFUSED = { 1: "a frame it does not know", 2: "not a file it can play", 3: "this ESQ-1 is an older one that takes no packs: press Add ESQ-1 to update it",
  4: "the file did not arrive whole", 6: "it has no song", 7: "more waves than it has room for", 8: "a frame out of order" };

// ------------------------------------------------------------------ the visitor's files ---

const have = { rom: [null, null, null, null], half: [null, null], songs: [] };   // songs: { name, sng, bank }

function idb() {
  return new Promise((resolve, reject) => {
    const r = indexedDB.open("fm1-esq1", 1);
    r.onupgradeneeded = () => r.result.createObjectStore("files");
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
}
async function store(mode, fn) {
  try {
    const db = await idb();
    return await new Promise((resolve, reject) => {
      const tx = db.transaction("files", mode), req = fn(tx.objectStore("files"));
      tx.oncomplete = () => resolve(req && req.result);
      tx.onerror = tx.onabort = () => reject(tx.error);
    });
  } catch (_) { return null; }                      // (a private window: the files last as long as the page)
}
const save = () => store("readwrite", (s) => s.put(have, "have"));
async function restore() {
  const h = await store("readonly", (s) => s.get("have"));
  if (h && Array.isArray(h.rom)) Object.assign(have, { rom: h.rom, half: h.half || [null, null], songs: h.songs || [] });
}

function take(item) {
  if (item.kind === "rom") have.rom[item.rom] = item.bytes;
  else if (item.kind === "half") have.half[item.half] = item.bytes;
  else if (item.kind === "songs" && item.bank && P.songs(item.sng).length) {
    have.songs = have.songs.filter((s) => s.name !== item.name);
    have.songs.push({ name: item.name, sng: item.sng, bank: item.bank });
  } else return false;
  if (have.half[0] && have.half[1] && !have.rom[0]) {
    have.rom[0] = new Uint8Array(65536);
    have.rom[0].set(have.half[0]);
    have.rom[0].set(have.half[1], 32768);
  }
  return true;
}

async function addFiles(files) {
  const said = [];
  for (const f of files) {
    try {
      const items = await P.identify(f.name, new Uint8Array(await f.arrayBuffer()));
      const taken = items.filter(take);
      const roms = taken.filter((i) => i.kind === "rom" || i.kind === "half").length, songs = taken.filter((i) => i.kind === "songs");
      said.push(`${f.name}: ` + ([roms ? `${roms === 1 ? "a wave ROM" : `${roms} wave ROMs`}` : "",
        songs.length ? `${songs.reduce((n, s) => n + P.songs(s.sng).length, 0)} songs` : ""].filter(Boolean).join(", ")
        || "nothing this page knows (a wave ROM, a disk dump or a .sqpack is)"));
    } catch (e) { said.push(`${f.name}: ${e.message}`); }
  }
  $("esq-filed").textContent = said.join(" · ");
  await save();
  show();
}

// ------------------------------------------------------------------ to the FM-1 ---

let busy = false;
const say = (text) => { $("esq-status").textContent = text; };
const bar = (x) => { $("esq-bar").style.width = `${Math.round(100 * x)}%`; };
const midiAccess = async () => {
  try { return await navigator.requestMIDIAccess({ sysex: true }); } catch (_) { throw fail("denied", ""); }
};

// the frames to the ESQ-1 in the fm1-dock on the FM-1, switched to it first when another synth plays
async function toEsq(frames, progress = () => {}) {
  const found = await findDock(await midiAccess());
  if (!found) throw fail("nodock", "");
  const { dock, hello } = found;
  try {
    const mod = (await dock.modules()).find((m) => m.name === NAME);
    if (!mod) throw fail("noesq", "ESQ-1 is not in the fm1-dock on the FM-1: press Add ESQ-1 first.");
    $("esq-pill").textContent = "on the FM-1";
    $("esq-pill").className = "pill ok";
    if (hello.cur !== mod.index) {
      const st = await dock.command(CMD.SELECT, NAME);
      if (st !== "done") throw fail("esq", `fm1-dock did not switch to the ESQ-1: ${st}.`);
      await sleep(1500);
    }
    for (let i = 0; i < frames.length; i++) {
      const d = await dock.ask(CMD.MODULE, frames[i], { timeout: 2500, tries: 3 });
      if (d[0] === 11) throw fail("esq", `The ESQ-1 did not take it: ${REFUSED[d[1]] || `its error ${d[1]}`}.`);
      if (d[0]) throw fail("esq", `fm1-dock did not pass it on (status ${d[0]}): is the ESQ-1 the synth playing?`);
      progress((i + 1) / frames.length);
    }
  } finally { dock.close(); }
}

async function run(what, fn) {
  if (busy) return;
  busy = true;
  document.body.dataset.busy = "1";
  for (const b of document.querySelectorAll("#main button")) b.dataset.was = b.disabled ? "1" : "", b.disabled = true;
  bar(0);
  try {
    say(`${what} …`);
    say(await fn() || "");
  } catch (e) {
    say(e.code === "esq" || e.code === "noesq" ? e.message : explain(e));
    console.error(e);
  }
  bar(0);
  for (const b of document.querySelectorAll("#main button")) b.disabled = b.dataset.was === "1";
  delete document.body.dataset.busy;
  busy = false;
  show();
}

const romNote = () => {
  const n = have.rom.filter(Boolean).length;
  return n === 4 ? "" : n ? ` (ROM ${have.rom.map((r, i) => (r ? "" : i + 1)).filter(Boolean).join(", ")} missing: their waves are silent)`
    : " (no wave ROM here: the stand-in waves play, and the SQ-80's are silent)";
};

function playSong(file, n) {
  const title = P.songName(file.sng, n) || `song ${n + 1}`;
  run(`Sending ${title}`, async () => {
    const { bytes, notes } = P.build(have.rom, file.sng, file.bank, n, true, have.rom.some(Boolean) ? "song" : "none");
    await toEsq(P.fileSteps(bytes), (x) => { bar(x); say(`Sending ${title}: ${Math.round(100 * x)} % of ${Math.round(bytes.length / 1024)} KB`); });
    return `${title} is playing on your FM-1${romNote()}${notes.length && have.rom.every(Boolean) ? ` (${notes.join("; ")})` : ""}.`;
  });
}

// ------------------------------------------------------------------ the page ---

function show() {
  $("esq-have").innerHTML = "";
  have.rom.forEach((r, i) => {
    const li = document.createElement("li");
    li.className = `pill ${r ? "ok" : ""}`;
    li.textContent = `ROM ${i + 1}${i ? "" : " (the ESQ-1's)"}: ${r ? "here" : "not yet"}`;
    $("esq-have").append(li);
  });
  $("esq-rom1").disabled = busy || !have.rom[0];
  $("esq-forget").hidden = !have.rom.some(Boolean) && !have.songs.length;
  const box = $("esq-packs");
  box.innerHTML = have.songs.length ? "" : '<p class="dim">No disk yet: drop one above.</p>';
  for (const file of have.songs) {
    const div = document.createElement("div"), h = document.createElement("h3"), row = document.createElement("div");
    div.className = "panel";
    h.textContent = file.name;
    row.className = "esq-songs";
    for (const n of P.songs(file.sng)) {
      const b = Object.assign(document.createElement("button"), { type: "button", className: "btn", disabled: busy });
      const kb = Math.round(P.songPages(file.sng, file.bank, n).pages.size / 4);
      b.innerHTML = `<span aria-hidden="true">▶</span> <span></span> <small>${kb} KB of waves</small>`;
      b.children[1].textContent = P.songName(file.sng, n) || `song ${n + 1}`;
      b.addEventListener("click", () => playSong(file, n));
      row.append(b);
    }
    div.append(h, row);
    box.append(div);
  }
}

$("esq-add").addEventListener("click", () => run("Adding ESQ-1", async () => {
  const r = await fetch("modules/esq1.dock", { cache: "no-cache" });
  if (!r.ok) throw new Error(`esq1.dock: HTTP ${r.status}`);
  const blob = new Uint8Array(await r.arrayBuffer());
  const got = await addModule(await midiAccess(), { blob, info: moduleInfo(blob) }, async (q) => confirm(q),
    (x) => { bar(x); say(`Writing ESQ-1: ${Math.round(100 * x)} % (do not unplug the FM-1)`); });
  if (!got) return "Nothing was changed.";
  $("esq-pill").textContent = "on the FM-1";
  $("esq-pill").className = "pill ok";
  $("esq-waves-pill").textContent = "stand-in waves";
  return "ESQ-1 is in the fm1-dock on your FM-1 and playing: HOME on the FM-1 lists the synths.";
}));
$("esq-own").addEventListener("click", () => run("Starting The Entertainer", async () => {
  await toEsq([Uint8Array.of(0x4f), Uint8Array.of(0x50, 0)]);       // O: its own waves and song; P 0: play
  $("esq-waves-pill").textContent = "stand-in waves";
  return "The Entertainer is playing on your FM-1, with the synth's own waves.";
}));
$("esq-stop").addEventListener("click", () => run("Stopping", async () => { await toEsq([Uint8Array.of(0x53)]); return "Stopped."; }));
$("esq-rom1").addEventListener("click", () => run("Sending the ESQ-1's waves", async () => {
  const { bytes } = P.build(have.rom, null, null, 0, false, "rom1");
  await toEsq(P.fileSteps(bytes), (x) => { bar(x); say(`Sending the ESQ-1's waves: ${Math.round(100 * x)} %`); });
  $("esq-waves-pill").textContent = "the ESQ-1's waves";
  $("esq-waves-pill").className = "pill ok";
  return "The ESQ-1's own waves are in the synth, until the FM-1 is switched off.";
}));
$("esq-forget").addEventListener("click", async () => {
  Object.assign(have, { rom: [null, null, null, null], half: [null, null], songs: [] });
  await store("readwrite", (s) => s.delete("have"));
  $("esq-filed").textContent = "The files are gone from this browser.";
  show();
});
$("esq-file").addEventListener("change", (e) => { addFiles([...e.target.files]); e.target.value = ""; });
const drop = $("esq-drop");
for (const ev of ["dragenter", "dragover"]) drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.add("over"); });
for (const ev of ["dragleave", "drop"]) drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.remove("over"); });
drop.addEventListener("drop", (e) => addFiles([...e.dataTransfer.files]));

if (!navigator.requestMIDIAccess) {
  $("esq-nomidi").hidden = false;
  for (const id of ["esq-add", "esq-own", "esq-stop"]) $(id).disabled = true;
}
await restore();
show();
