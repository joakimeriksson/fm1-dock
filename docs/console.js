// SPDX-License-Identifier: GPL-3.0-only
// The page's console: fm1-dock on an FM-1 over WebMIDI (dock.js), and the flash map.
import { Dock, CMD, moduleInfo, probeArg, Syms, hex32, sleep } from "./dock.js";
import { pairPorts } from "./install/ports.js";

const $ = (id) => document.getElementById(id);
let access = null, hb = null, syms = null, busy = Promise.resolve(), probeBlob = null, follow = null, lastHello = null;

const esc = (s) => s.replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" })[c]);
const kib = (n) => `${Math.round(n / 1024)} KiB`;
const pcName = (pc) => { const s = syms && syms.lookup(pc); return s ? `${hex32(pc)} ${s}` : hex32(pc); };

// one request at a time (the device handles one frame per main-loop pass)
function serial(fn) {
  const run = busy.then(fn, fn);
  busy = run.catch(() => {});
  return run;
}

function setState(text, cls = "") {
  const s = $("c-state");
  s.textContent = text;
  s.className = "pill " + cls;
}

function enable(on) {
  for (const id of ["c-hello", "c-safe", "c-log", "c-follow", "c-deploy-file", "c-probe-file", "c-probe-arg", "c-probe-result", "c-note"])
    $(id).disabled = !on;
  $("c-probe-run").disabled = !on || !probeBlob;
  for (const b of document.querySelectorAll("[data-probe]")) b.disabled = !on;
}

function traffic(dir, bytes, r) {
  const t = $("c-traffic");
  const name = r ? (Object.keys(CMD).find((k) => CMD[k] === r.cmd) || `0x${r.cmd.toString(16)}`) : "?";
  const head = [...bytes.slice(0, 24)].map((x) => x.toString(16).padStart(2, "0")).join(" ");
  t.textContent += `${dir === "out" ? "→" : "←"} ${name.padEnd(7)} seq ${String(r ? r.seq : "-").padStart(3)}  ${bytes.length} B  ${head}${bytes.length > 24 ? " …" : ""}\n`;
  if (t.textContent.length > 40000) t.textContent = t.textContent.slice(-30000);
  t.scrollTop = t.scrollHeight;
}

const NOTE = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
function midiIn(d) {
  const m = $("c-midi"), st = d[0] & 0xf0, ch = (d[0] & 15) + 1;
  let s;
  if ((st === 0x90 || st === 0x80) && d.length > 2) s = `${st === 0x90 && d[2] ? "note on " : "note off"} ${NOTE[d[1] % 12]}${Math.floor(d[1] / 12) - 1} vel ${d[2]}`;
  else if (st === 0xb0) s = `cc ${d[1]} = ${d[2]}`;
  else if (st === 0xa0) s = `pressure ${NOTE[d[1] % 12]}${Math.floor(d[1] / 12) - 1} ${d[2]}`;
  else if (st === 0xe0) s = `pitch bend ${(d[2] << 7 | d[1]) - 8192}`;
  else if (d[0] >= 0xf8) return;                // clock, active sensing
  else s = [...d].map((x) => x.toString(16).padStart(2, "0")).join(" ");
  m.textContent += `ch ${String(ch).padStart(2)}  ${s}\n`;
  const lines = m.textContent.split("\n");
  if (lines.length > 80) m.textContent = lines.slice(-80).join("\n");
  m.scrollTop = m.scrollHeight;
}

function ports() {
  const outs = [...access.outputs.values()], ins = [...access.inputs.values()];
  const sel = $("c-port"), keep = sel.value;
  sel.innerHTML = "";
  for (const o of outs) sel.add(new Option(o.name, o.id));
  const want = outs.find((o) => o.id === keep) || outs.find((o) => /felucca|fm1-dock|dock|fm-?1/i.test(o.name)) || outs[0];
  if (!want) { sel.add(new Option("no MIDI outputs")); sel.disabled = true; return null; }
  sel.value = want.id;
  sel.disabled = false;
  const pair = pairPorts(access.inputs, access.outputs).find((p) => p.output === want);   // (Windows may name the two apart)
  const inp = pair ? pair.input : ins.find((i) => /felucca|fm1-dock|dock|fm-?1/i.test(i.name));
  return inp ? { out: want, inp } : null;
}

async function open() {
  if (hb) { hb.close(); hb = null; }
  const p = ports();
  if (!p) { setState("no FM-1 found: is it plugged in, running fm1-dock?", "bad"); enable(false); return; }
  hb = new Dock(p.inp, p.out, { onMidi: midiIn, onTraffic: traffic });
  setState(`opening ${p.out.name}…`);
  try {
    await refresh();
    setState(`connected: ${p.out.name}`, "ok");
    enable(true);
  } catch (e) {
    setState(e.message, "bad");
    enable(false);
    $("c-note").disabled = false;
  }
}

async function refresh() {
  const h = await serial(() => hb.hello());
  lastHello = h;
  showHello(h);
  const mods = await serial(() => hb.modules());
  showModules(mods, h);
}

function showHello(h) {
  const pct = (t) => (h.block ? `${(100 * t / h.block).toFixed(0)} % of a block` : `${t} ticks`);
  const rows = [
    ["protocol / ABI", `${h.proto} / ${h.abi}`],
    ["modules", `${h.n}, playing #${h.cur}`],
    ["deploy space", `${kib(h.free)} free, largest gap ${kib(h.largest)}`],
    ["largest file", kib(h.maxFile)],
    ["render worst", `${pct(h.worst)}, ${h.overruns} overruns`],
    ["last crash", h.crashPc ? `${pcName(h.crashPc)} in module ${h.crashModule ? h.crashModule - 1 : "-"}` : "none"],
    ["log", `${h.logW} bytes written`],
  ];
  if (h.receiving) rows.push(["receiving", "a deploy is under way"]);
  $("c-status").innerHTML = rows.map(([k, v]) => `<tr><td>${k}</td><td>${esc(v)}</td></tr>`).join("");
}

function showModules(mods, h) {
  const t = $("c-mods");
  if (!mods.length) { t.innerHTML = `<tr><td class="dim">no modules listed</td></tr>`; return; }
  t.innerHTML = "";
  for (const m of mods) {
    const tr = t.insertRow();
    if (m.index === h.cur) tr.className = "cur";
    tr.innerHTML = `<td>${m.index === h.cur ? "▸" : ""}${m.index}</td><td>${esc(m.name)}</td><td>${m.kind}</td>` +
      `<td>${m.at ? kib(m.size) : ""}</td><td class="${m.state === "ok" ? "" : "st-bad"}">${m.state}</td><td></td>`;
    const cell = tr.cells[5];
    if (m.index !== h.cur) {
      const b = document.createElement("button");
      b.className = "btn small";
      b.textContent = "Play";
      b.title = `SELECT: switch the FM-1 to ${m.name}`;
      b.onclick = () => act(() => hb.command(CMD.SELECT, m.name), `select ${m.name}`);
      cell.append(b);
    }
    if (m.kind === "deployed") {
      const b = document.createElement("button");
      b.className = "btn small danger";
      b.textContent = "Delete";
      b.title = `DELETE: remove ${m.name} from the FM-1's flash`;
      b.onclick = () => { if (confirm(`Delete ${m.name} from the FM-1's flash? Its banks stay.`)) act(() => hb.command(CMD.DELETE, m.name), `delete ${m.name}`); };
      cell.append(" ", b);
    }
  }
}

async function act(fn, what) {
  try {
    const s = await serial(fn);
    setState(`${what}: ${s}`, s === "done" ? "ok" : "bad");
    await sleep(400);
    await refresh();
  } catch (e) { setState(e.message, "bad"); }
}

function colorLog(text) {
  return esc(text).split("\n").map((ln) =>
    /fault|crash|fail|bad|error|struck/i.test(ln) ? `<span class="e">${ln}</span>`
    : /slow|skip|overrun|safe|strike/i.test(ln) ? `<span class="w">${ln}</span>`
    : /started|boot|deployed|saved/i.test(ln) ? `<span class="s">${ln}</span>` : ln).join("\n");
}

async function readLog() {
  const { text, hello } = await serial(() => hb.log());
  lastHello = hello;
  showHello(hello);
  let shown = text.replace(/\n$/, "");
  if (syms) shown = shown.replace(/\b(?:0x)?(0[12][0-9a-f]{6})\b/gi, (m, a) => { const s = syms.lookup(parseInt(a, 16)); return s ? `${m} <${s}>` : m; });
  const el = $("c-logtext"), atEnd = el.scrollTop + el.clientHeight >= el.scrollHeight - 8;
  el.innerHTML = colorLog(shown);
  if (atEnd) el.scrollTop = el.scrollHeight;
}

async function readFile(input) {
  const f = input.files[0];
  input.value = "";
  return f ? { name: f.name, blob: new Uint8Array(await f.arrayBuffer()) } : null;
}

function bar(id, frac) { $(id).style.width = `${Math.round(frac * 100)}%`; }

async function deployFile() {
  const f = await readFile($("c-deploy-file"));
  if (!f) return;
  const msg = $("c-deploy-msg");
  let info;
  try { info = moduleInfo(f.blob); } catch (e) { msg.textContent = e.message; msg.className = "msg bad"; return; }
  if (info.probe) { msg.textContent = `${info.name} is a probe: run it under "Run a probe"`; msg.className = "msg bad"; return; }
  if (info.boot) {                              // it would take over at every power-on, with its own USB
    msg.textContent = `${info.name} is a boot firmware: it takes over the FM-1 at power-on with its own USB. Deploy it with tools/dock.py deploy --boot, only if it answers the installers.`;
    msg.className = "msg bad";
    return;
  }
  if (!confirm(`Deploy ${info.name} v${info.version} (${kib(f.blob.length)}) into the FM-1's flash? It starts playing when it is in.`)) return;
  msg.className = "msg";
  bar("c-deploy-bar", 0);
  try {
    const r = await serial(() => hb.deploy(f.blob, (x, t) => { bar("c-deploy-bar", x); msg.textContent = t; }));
    msg.textContent = `${r.info.name}: in flash at ${hex32(r.at)}, running`;
    msg.className = "msg ok";
    await refresh();
  } catch (e) { msg.textContent = e.message; msg.className = "msg bad"; }
}

function showProbe(r) {
  const out = $("c-probe-out");
  if (r.crash_pc && syms) r.crash_at = syms.lookup(parseInt(r.crash_pc, 16)) || "?";
  out.textContent = JSON.stringify(r, null, 2);
}

async function probeFile(given) {
  const f = given || await readFile($("c-probe-file"));
  if (!f) return;
  try {
    const info = moduleInfo(f.blob);
    if (!info.probe) throw new Error(`${info.name} is a module, not a probe: deploy it instead`);
    probeBlob = f.blob;
    $("c-probe-out").textContent = `${info.name}: ${f.blob.length} B, ready`;
    $("c-probe-run").disabled = !hb;
  } catch (e) { probeBlob = null; $("c-probe-out").textContent = e.message; $("c-probe-run").disabled = true; }
}

async function runProbe() {
  const out = $("c-probe-out");
  let arg;
  try { arg = probeArg($("c-probe-arg").value); } catch { out.textContent = "the argument: 1,2,0x30 or hex:0a0b"; return; }
  bar("c-probe-bar", 0);
  try {
    const r = await serial(() => hb.probe(probeBlob, arg, (x, t) => { bar("c-probe-bar", x); out.textContent = t; }));
    if (r) { showProbe(r); return; }
    out.textContent = "no answer: the probe crashed or hung and the FM-1 resets. Asking for its result once it is back…";
    for (let i = 0; i < 25; i++) {
      await sleep(1000);
      try { await open(); showProbe(await serial(() => hb.result())); return; } catch { /* not back yet */ }
    }
    out.textContent += "\nthe FM-1 did not come back over USB: replug it (fm1-dock keeps the result) and press Last result.";
  } catch (e) { out.textContent = e.message; }
}

async function connect() {
  if (!navigator.requestMIDIAccess) { setState("this browser has no WebMIDI: use Chrome or Edge", "bad"); return; }
  try {
    access = await navigator.requestMIDIAccess({ sysex: true });
  } catch (e) { setState(`no MIDI access: ${e.message}`, "bad"); return; }
  access.onstatechange = (e) => {
    if (e.port.state === "connected" && (!hb || hb.output.state !== "connected")) open();
    if (e.port.state === "disconnected" && hb && e.port.id === hb.output.id) { setState("the FM-1 went away (reset or unplugged)", "bad"); enable(false); }
  };
  $("c-connect").textContent = "Reconnect";
  await open();
}

$("c-connect").onclick = connect;
$("c-port").onchange = open;
$("c-hello").onclick = () => refresh().catch((e) => setState(e.message, "bad"));
$("c-safe").onclick = () => act(() => hb.command(CMD.SAFE), "sine");
$("c-log").onclick = () => readLog().catch((e) => setState(e.message, "bad"));
$("c-follow").onchange = (e) => {
  clearInterval(follow);
  follow = e.target.checked ? setInterval(() => { if (hb) readLog().catch(() => {}); }, 2000) : null;
};
$("c-deploy-file").onchange = deployFile;
$("c-probe-file").onchange = probeFile;
$("c-probe-run").onclick = runProbe;
for (const b of document.querySelectorAll("[data-probe]")) {   // the probes this page carries (docs/probes/, fm1-dock's ABI 2)
  b.onclick = async () => {
    const r = await fetch(`probes/${b.dataset.probe}.dock`);
    if (!r.ok) { $("c-probe-out").textContent = `probes/${b.dataset.probe}.dock: ${r.status}`; return; }
    await probeFile({ name: b.dataset.probe, blob: new Uint8Array(await r.arrayBuffer()) });
    if (probeBlob) runProbe();
  };
}
$("c-probe-result").onclick = () => serial(() => hb.result()).then(showProbe, (e) => { $("c-probe-out").textContent = e.message; });
$("c-note").onclick = () => {
  if (!hb) return;
  hb.output.send([0x90, 60, 100]);
  setTimeout(() => hb.output.send([0x80, 60, 0]), 300);
};
$("c-syms").onchange = async (e) => {
  const f = e.target.files[0];
  if (!f) return;
  syms = new Syms(await f.text());
  $("c-syms-msg").textContent = `${f.name}: ${syms.size} code symbols`;
  $("c-syms-msg").className = syms.size ? "msg ok" : "msg bad";
  if (lastHello) showHello(lastHello);
};
for (const l of document.querySelectorAll(".file")) {
  l.addEventListener("dragenter", () => l.classList.add("drag"));
  for (const ev of ["dragleave", "drop"]) l.addEventListener(ev, () => l.classList.remove("drag"));
}

// the flash map (flash offsets; firmware/src/dock.c)
const FLASH = [
  [0x000000, 0x004000, "boot and the package header", "other"],
  [0x004000, 0x034000, "fm1-dock (the host)", "upd"],
  [0x034000, 0x035000, "slot table", "upd"],
  [0x035000, 0x093000, "image modules", "upd"],
  [0x093000, 0x095000, "deployed table, two copies", "keep"],
  [0x095000, 0x096000, "settings", "keep"],
  [0x096000, 0x0e0000, "deployed modules", "keep"],
  [0x0e0000, 0x0ea000, "not fm1-dock's", "other"],
  [0x0ea000, 0x0fc000, "banks (9 × 8 KiB)", "keep"],
  [0x0fc000, 0x100000, "a boot firmware's sector", "other"],
];
$("fmap").innerHTML = FLASH.map(([a, b, what, cls]) => {
  const h = Math.max(22, Math.round(Math.sqrt((b - a) / 1024) * 6));
  return `<div class="seg"><div class="at">0x${a.toString(16).padStart(5, "0")}</div>` +
    `<div class="bar ${cls}" style="min-height:${h}px"><span>${what}</span><span>${kib(b - a)}</span></div></div>`;
}).join("");
