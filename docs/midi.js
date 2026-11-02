// SPDX-License-Identifier: GPL-3.0-only
// fm1-dock's Test MIDI page: a keyboard that plays the FM-1 over USB-MIDI (WebMIDI, no SysEx) and Bluetooth MIDI (Web
// Bluetooth, the BLE-MIDI service straight, no pairing), a monitor of what it sends back, and a test of its TRS MIDI IN
// (notes out of a USB-MIDI interface into the jack, back over USB with MIDI THRU on, timed). Only channel messages are
// sent: nothing here installs or changes anything on the device.

const $ = (s) => document.querySelector(s);
const BLE_SVC = "03b80e5a-ede8-4b33-a751-6ce34ec4c700", BLE_CHR = "7772e5db-3868-4112-a1a9-f2669d106bf3";
const OURS = /felucca|fm-?1|dinghy|smk|mke-?p37|starry|m-?vave/i;   // the device's port names (fm1-dock's is Felucca's)
const NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const noteName = (n) => NAMES[n % 12] + (Math.floor(n / 12) - 1);
const S = { midi: null, usbIn: null, usbOut: null, ble: null, bleDev: null, base: 60, held: new Map(), demo: false, trs: null };

// ------------------------------------------------------------------ messages ---

const dataLen = (st) => (st >= 0xF8 ? 0 : st >= 0xF0 ? ({ 0xF1: 1, 0xF2: 2, 0xF3: 1 }[st] || 0) : (st & 0xE0) === 0xC0 ? 1 : 2);
function describe(m) {
  const [st, a, b] = m, ch = (st & 15) + 1;
  switch (st & 0xF0) {
    case 0x90: return b ? `note on  ${noteName(a)} (${a}) vel ${b}  ch ${ch}` : `note off ${noteName(a)} (${a})  ch ${ch}`;
    case 0x80: return `note off ${noteName(a)} (${a})  ch ${ch}`;
    case 0xA0: return `poly pressure ${noteName(a)} ${b}  ch ${ch}`;
    case 0xB0: return `CC ${a} = ${b}  ch ${ch}`;
    case 0xC0: return `program ${a + 1}  ch ${ch}`;
    case 0xD0: return `pressure ${a}  ch ${ch}`;
    case 0xE0: return `pitch bend ${((b << 7) | a) - 8192}  ch ${ch}`;
  }
  return { 0xF8: "clock", 0xFA: "start", 0xFB: "continue", 0xFC: "stop", 0xFE: "active sensing", 0xF0: "SysEx" }[st]
    || "0x" + [...m].map((x) => x.toString(16).padStart(2, "0")).join(" ");
}
const isOn = (m) => (m[0] & 0xF0) === 0x90 && m[2] > 0;
const isOff = (m) => (m[0] & 0xF0) === 0x80 || ((m[0] & 0xF0) === 0x90 && m[2] === 0);

// BLE-MIDI: [0x80 | ts high] then per message [0x80 | ts low] status data..., running status allowed
function blePacket(msgs) {
  const t = Math.floor(performance.now()) & 0x1FFF, out = [0x80 | (t >> 7)];
  for (const m of msgs) out.push(0x80 | (t & 0x7F), ...m);
  return new Uint8Array(out);
}
function bleParse(b) {
  const msgs = [];
  let st = 0, i = 1;
  while (i < b.length) {
    if (b[i] & 0x80) {                          // a timestamp, then a status or a running status' data
      i++;
      if (i < b.length && b[i] & 0x80) {
        const s = b[i++];
        if (s >= 0xF8) { msgs.push([s]); continue; }   // real time: the running status stays
        if (s === 0xF0) { while (i < b.length && b[i] !== 0xF7 && !(b[i] & 0x80)) i++; msgs.push([0xF0]); st = 0; continue; }
        st = s;
      }
    }
    if (!st) { i++; continue; }
    const n = dataLen(st);
    if (i + n > b.length) break;
    msgs.push([st, ...b.slice(i, i + n)]);
    i += n;
    if (st >= 0xF0) st = 0;
  }
  return msgs;
}

// ------------------------------------------------------------------ the monitor ---

// one element per event (a time column, the source, the message), newest at the bottom, the last 300 kept; it follows
// the newest unless scrolled up
const MON_MAX = 300;
function line(src, text, cls) {
  const log = $("#mon"), el = document.createElement("div");
  el.className = "ev" + (cls ? " " + cls : "");
  for (const [c, t] of [["t", performance.now().toFixed(0)], ["src", src], ["msg", text]]) {
    const s = document.createElement("span");
    s.className = c;
    s.textContent = t;
    el.appendChild(s);
  }
  const follow = log.scrollTop + log.clientHeight >= log.scrollHeight - 8;
  log.appendChild(el);
  while (log.childElementCount > MON_MAX) log.firstElementChild.remove();
  if (follow) log.scrollTop = log.scrollHeight;
}
function mon(src, m, cls) {
  if ((m[0] === 0xF8 || m[0] === 0xFE) && !$("#mon-clock").checked) return;
  if (src === "out" && !$("#mon-out").checked) return;
  line(src, describe(m), cls);
}
const say = (text, cls = "s") => line("", text, cls);

function fromDevice(src, m) {
  mon(src, m, src === "USB" ? "" : "ble");
  if (isOn(m)) light(m[1], "in", true);
  else if (isOff(m)) light(m[1], "in", false);
  if (src === "USB" && S.trs) S.trs.got(m);
}

// ------------------------------------------------------------------ sending ---

function targets() {
  const t = [];
  if (S.usbOut) t.push("USB");
  if (S.ble) t.push("Bluetooth");
  return t;
}
let bleQ = [], bleBusy = false;
async function blePump() {
  if (bleBusy) return;
  bleBusy = true;
  while (bleQ.length && S.ble) {
    const p = bleQ.shift();
    try { await (S.ble.writeValueWithoutResponse ? S.ble.writeValueWithoutResponse(p) : S.ble.writeValue(p)); }
    catch (e) { say("Bluetooth: " + e.message, "e"); }
  }
  bleBusy = false;
}
function send(...msgs) {
  if (S.usbOut) for (const m of msgs) S.usbOut.send(m);
  if (S.ble) { bleQ.push(blePacket(msgs)); blePump(); }
  if (S.usbOut || S.ble) for (const m of msgs) mon("out", m, "w");
}
const ch = () => +$("#ch").value;
function noteOn(n, v) { send([0x90 | ch(), n, v ?? +$("#vel").value]); light(n, "on", true); }
function noteOff(n) { send([0x80 | ch(), n, 0]); light(n, "on", false); }

function sync() {
  const t = targets();
  $("#keys-to").textContent = t.length ? "to: " + t.join(" + ") : "to: nothing yet";
  $("#keys-to").className = "pill" + (t.length ? " ok" : "");
  $("#demo").disabled = $("#panic").disabled = !t.length;
}

// ------------------------------------------------------------------ USB ---

function pill(el, text, cls) { el.textContent = text; el.className = "pill" + (cls ? " " + cls : ""); }

async function usbConnect() {
  if (!navigator.requestMIDIAccess) return;
  if (!S.midi) {
    try { S.midi = await navigator.requestMIDIAccess({ sysex: false }); }
    catch (_) { pill($("#usb-pill"), "MIDI not allowed", "bad"); say("MIDI was not allowed: the icon in the address bar, then Connect again.", "e"); return; }
    S.midi.addEventListener("statechange", () => { ports(); });
  }
  ports(true);
}

// the FM-1's port: the one whose name looks like the device's, an input and an output of the same name
function ports(pick) {
  const ins = [...S.midi.inputs.values()].filter((p) => p.state !== "disconnected");
  const outs = [...S.midi.outputs.values()].filter((p) => p.state !== "disconnected");
  const names = [...new Set(ins.map((p) => p.name).filter((n) => outs.some((o) => o.name === n)))];
  const sel = $("#usb-port"), was = S.usbOut ? S.usbOut.name : sel.value;
  sel.innerHTML = "";
  for (const n of names) sel.add(new Option(n, n));
  sel.disabled = !names.length;
  if (!names.length) sel.add(new Option("no MIDI ports"));
  const want = names.includes(was) ? was : names.find((n) => OURS.test(n)) || "";
  if (want) sel.value = want;
  if (pick || (S.usbOut && !names.includes(S.usbOut.name))) usePort(want);
  trsPorts();
}
function usePort(name) {
  if (S.usbIn) S.usbIn.onmidimessage = null;
  S.usbIn = S.usbOut = null;
  if (name) {
    S.usbIn = [...S.midi.inputs.values()].find((p) => p.name === name && p.state !== "disconnected") || null;
    S.usbOut = [...S.midi.outputs.values()].find((p) => p.name === name && p.state !== "disconnected") || null;
  }
  if (S.usbIn) S.usbIn.onmidimessage = (e) => fromDevice("USB", e.data);
  if (S.usbOut) {
    pill($("#usb-pill"), "connected: " + name, "ok");
    $("#usb-connect").textContent = "Connected over USB";
    $("#usb-connect").classList.remove("primary");
    say("USB: " + name);
  } else {
    pill($("#usb-pill"), S.midi ? "no FM-1 port picked" : "not connected", S.midi ? "bad" : "");
    $("#usb-connect").textContent = "Connect over USB";
    $("#usb-connect").classList.add("primary");
  }
  sync();
  trsPorts();
}

// ------------------------------------------------------------------ Bluetooth ---

async function bleConnect() {
  if (S.bleDev && S.bleDev.gatt.connected) { S.bleDev.gatt.disconnect(); return; }
  try {
    S.bleDev = await navigator.bluetooth.requestDevice({ filters: [{ services: [BLE_SVC] }] });
  } catch (e) {
    if (e.name !== "NotFoundError") say("Bluetooth: " + e.message, "e");   // (NotFoundError: the list was closed)
    return;
  }
  S.bleDev.addEventListener("gattserverdisconnected", () => {
    S.ble = null; bleQ = [];
    pill($("#ble-pill"), "disconnected", "bad");
    $("#ble-connect").textContent = "Connect over Bluetooth";
    say("Bluetooth: disconnected", "w");
    sync();
  });
  pill($("#ble-pill"), "connecting…", "acc");
  try {
    const srv = await S.bleDev.gatt.connect();
    const c = await (await srv.getPrimaryService(BLE_SVC)).getCharacteristic(BLE_CHR);
    c.addEventListener("characteristicvaluechanged", (e) => {
      for (const m of bleParse(new Uint8Array(e.target.value.buffer))) fromDevice("BLE", m);
    });
    await c.startNotifications();
    S.ble = c;
    pill($("#ble-pill"), "connected: " + (S.bleDev.name || "BLE-MIDI"), "ok");
    $("#ble-connect").textContent = "Disconnect";
    say("Bluetooth: " + (S.bleDev.name || "connected"));
  } catch (e) {
    pill($("#ble-pill"), "failed", "bad");
    say("Bluetooth: " + e.message, "e");
    try { S.bleDev.gatt.disconnect(); } catch (_) { /* (already gone) */ }
  }
  sync();
}

// ------------------------------------------------------------------ the keyboard ---

// three octaves from C3, two from C4 where the keys would be too narrow (a phone)
const kb = $("#kb"), keyEl = {}, narrow = matchMedia("(max-width: 560px)");
const BLACK = [1, 3, 6, 8, 10];
let LOW = 48, HIGH = 84;
function build() {
  [LOW, HIGH] = narrow.matches ? [60, 84] : [48, 84];
  kb.replaceChildren();
  for (const n of Object.keys(keyEl)) delete keyEl[n];
  const whites = [];
  for (let n = LOW; n <= HIGH; n++) if (!BLACK.includes(n % 12)) whites.push(n);
  const w = 100 / whites.length;
  let wi = 0;
  for (let n = LOW; n <= HIGH; n++) {
    const black = BLACK.includes(n % 12), k = document.createElement("div");
    k.className = "key " + (black ? "b" : "w");
    k.style.left = (black ? wi * w - w * 0.32 : wi * w) + "%";
    k.style.width = (black ? w * 0.64 : w) + "%";
    if (!black) wi++;
    if (n % 12 === 0) k.innerHTML = `<span>C${n / 12 - 1}</span>`;
    k.dataset.n = n;
    keyEl[n] = k;
    kb.appendChild(k);
  }
}
function light(n, cls, on) { if (keyEl[n]) keyEl[n].classList.toggle(cls, on); }

const ptr = new Map();
kb.addEventListener("pointerdown", (e) => {
  const k = e.target.closest(".key");
  if (!k || !targets().length) return;
  e.preventDefault();
  kb.setPointerCapture(e.pointerId);
  ptr.set(e.pointerId, +k.dataset.n);
  noteOn(+k.dataset.n);
});
kb.addEventListener("pointermove", (e) => {
  if (!ptr.has(e.pointerId)) return;
  const k = document.elementFromPoint(e.clientX, e.clientY)?.closest(".key");
  const n = k ? +k.dataset.n : 0;
  if (n && n !== ptr.get(e.pointerId)) { noteOff(ptr.get(e.pointerId)); ptr.set(e.pointerId, n); noteOn(n); }
});
const lift = (e) => { if (ptr.has(e.pointerId)) { noteOff(ptr.get(e.pointerId)); ptr.delete(e.pointerId); } };
kb.addEventListener("pointerup", lift);
kb.addEventListener("pointercancel", lift);

const MAP = "awsedftgyhujkolp;'";
addEventListener("keydown", (e) => {
  if (e.repeat || e.metaKey || e.ctrlKey || e.altKey || e.target.matches?.("select, textarea, input[type=text], input[type=search]")) return;
  const k = e.key.toLowerCase();
  if (k === "z" || k === "x") {
    S.base = Math.max(36, Math.min(84, S.base + (k === "z" ? -12 : 12)));
    $("#oct").textContent = "C" + (S.base / 12 - 1);
    return;
  }
  const i = MAP.indexOf(k);
  if (i < 0 || !targets().length || S.held.has(k)) return;
  const n = S.base + i;
  S.held.set(k, n);
  noteOn(n);
});
addEventListener("keyup", (e) => {
  const k = e.key.toLowerCase();
  if (S.held.has(k)) { noteOff(S.held.get(k)); S.held.delete(k); }
});
addEventListener("blur", () => { for (const n of S.held.values()) noteOff(n); S.held.clear(); });

function panic() {
  S.demo = false;
  for (const n of Object.keys(keyEl)) { light(+n, "on", false); light(+n, "in", false); }
  send([0xB0 | ch(), 123, 0], [0xB0 | ch(), 120, 0]);
  for (let n = LOW; n <= HIGH; n++) send([0x80 | ch(), n, 0]);
}

// a short piece: C, Am, F, G arpeggios with a tune on top, twice, and a last chord
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function demo() {
  if (S.demo) { S.demo = false; return; }
  S.demo = true;
  $("#demo").textContent = "Stop demo";
  const prog = [[48, 52, 55, 60], [45, 52, 57, 60], [41, 48, 53, 57], [43, 50, 55, 59]];
  const tune = [[72, 74, 76, 79], [76, 74, 72, 69], [72, 69, 65, 69], [71, 74, 79, 74]];
  for (let rep = 0; rep < 2 && S.demo; rep++)
    for (let c = 0; c < 4 && S.demo; c++)
      for (let s = 0; s < 8 && S.demo; s++) {
        const a = prog[c][s % 4] + (s >= 4 ? 12 : 0), t = tune[c][s >> 1];
        noteOn(a, 80);
        if (s % 2 === 0) noteOn(t, 110);
        await sleep(220);
        noteOff(a);
        if (s % 2 === 1) noteOff(t);
      }
  if (S.demo) {
    const last = [48, 60, 64, 67, 72];
    last.forEach((n) => noteOn(n, 95));
    await sleep(1600);
    last.forEach(noteOff);
  }
  S.demo = false;
  $("#demo").textContent = "Play demo";
}

// ------------------------------------------------------------------ the TRS test ---
// Out of the interface into the jack, back over the FM-1's USB (MIDI THRU forwards the jack's channel messages as they
// came). A message is on time when it is back before the next one is sent; one back only after the next was sent is
// late: the jack's reader held it until more bytes came (fm1-dock 0.6: "press a key, nothing; release, a stuck note").

const TRS_NOTES = [60, 62, 64, 65, 67, 69, 71, 72], GAP = 300, WAIT = 800;

function trsPorts() {
  const sel = $("#trs-port");
  if (!S.midi) return;
  const outs = [...S.midi.outputs.values()].filter((p) => p.state !== "disconnected" && (!S.usbOut || p.name !== S.usbOut.name));
  const was = sel.value;
  sel.innerHTML = "";
  for (const p of outs) sel.add(new Option(p.name, p.id));
  if (!outs.length) sel.add(new Option("no other MIDI output: plug in the interface"));
  const pick = outs.find((p) => p.id === was) || outs.find((p) => !/iac|virtual|network|session/i.test(p.name) && !OURS.test(p.name)) || outs[0];
  if (pick) sel.value = pick.id;
  sel.disabled = !outs.length || !!S.trs;
  $("#trs-run").disabled = !outs.length || !S.usbIn || !!S.trs;
}

async function trsRun() {
  const out = S.midi.outputs.get($("#trs-port").value);
  if (!out || !S.usbIn) return;
  const msgs = [];
  for (const n of TRS_NOTES) msgs.push([0x90, n, 100], [0x80, n, 0]);
  const rec = msgs.map((m) => ({ m, sent: 0, back: 0, nextSent: 0 }));
  const verdict = $("#trs-verdict"), rows = $("#trs-rows");
  rows.innerHTML = "";
  $("#trs-out").hidden = false;
  verdict.hidden = false;
  verdict.className = "say mt-verdict";
  verdict.textContent = "Running…";
  S.trs = {
    got(m) {
      const r = rec.find((x) => x.sent && !x.back && x.m[1] === m[1] && (isOn(x.m) ? isOn(m) : isOff(m)));
      if (r) r.back = performance.now();
    },
  };
  trsPorts();
  say(`TRS test: ${msgs.length} messages out of ${out.name}`);
  try {
    for (let i = 0; i < rec.length; i++) {
      if (i) rec[i - 1].nextSent = performance.now();
      rec[i].sent = performance.now();
      out.send(rec[i].m);
      await sleep(GAP);
    }
    await sleep(WAIT);
    const last = rec[rec.length - 1];
    if (!last.back) {                           // the last one: does it come with another message after it?
      last.nextSent = performance.now();
      out.send([0x80, 0, 0]);
      await sleep(WAIT);
    }
  } finally {
    S.trs = null;
    trsPorts();
  }
  let ok = 0, late = 0, lost = 0;
  const times = [];
  rec.forEach((r, i) => {
    const t = r.back ? r.back - r.sent : 0;
    let res, cls;
    if (!r.back) { res = "never came back"; cls = "bad"; lost++; }
    else if (r.nextSent && r.back >= r.nextSent) { res = "late: came only with the next message"; cls = "bad"; late++; }
    else { res = "on time"; cls = "ok"; ok++; times.push(t); }
    const tr = document.createElement("tr");
    tr.innerHTML = `<td>${i + 1}</td><td>${describe(r.m)}</td><td>${r.back ? t.toFixed(1) + " ms" : "–"}</td><td class="${cls}">${res}</td>`;
    rows.appendChild(tr);
  });
  times.sort((a, b) => a - b);
  const med = times.length ? times[times.length >> 1].toFixed(1) : "";
  let text, cls;
  if (ok === rec.length) { text = `The TRS jack works: all ${ok} messages came back on time (${med} ms typical, the interface and USB included).`; cls = "ok"; }
  else if (!ok && !late) { text = "Nothing came back. Is MIDI THRU ON, MIDI CH on ALL, the interface's port picked? Did the module play the notes? If it did not, try the other adapter type (A / B)."; cls = "bad"; }
  else if (late) { text = `${late} of ${rec.length} messages came only when the next one was sent: the jack's bytes are held until more come, the bug of fm1-dock 0.6 (a key plays on its release, the last note hangs).`; cls = "bad"; }
  else { text = `${lost} of ${rec.length} messages never came back: bytes are lost between the interface and the FM-1. Check the cable and the adapter.`; cls = "bad"; }
  verdict.textContent = text;
  verdict.className = "say mt-verdict " + cls;
  say("TRS test: " + text, cls === "ok" ? "s" : "e");
  send([0xB0, 123, 0]);
}

// ------------------------------------------------------------------ start ---

for (let i = 1; i <= 16; i++) $("#ch").add(new Option(String(i), String(i - 1)));
$("#vel").addEventListener("input", () => { $("#vel-v").textContent = $("#vel").value; });
build();
narrow.addEventListener("change", build);
if (!navigator.requestMIDIAccess) { $("#mt-nomidi").hidden = false; $("#usb-connect").disabled = true; }
if (!navigator.bluetooth) { $("#mt-noble").hidden = false; $("#ble-connect").disabled = true; }
$("#usb-connect").addEventListener("click", usbConnect);
$("#usb-port").addEventListener("change", (e) => usePort(e.target.value));
$("#ble-connect").addEventListener("click", bleConnect);
$("#demo").addEventListener("click", demo);
$("#panic").addEventListener("click", panic);
$("#mon-clear").addEventListener("click", () => { $("#mon").replaceChildren(); });
$("#trs-run").addEventListener("click", trsRun);
sync();
