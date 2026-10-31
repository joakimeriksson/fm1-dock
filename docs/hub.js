// SPDX-License-Identifier: GPL-3.0-only
// fm1-dock's page: the device panel (connect, what runs on the device, install fm1-dock) and the module catalog
// (modules/modules.json: cards, filters, add to and remove from the fm1-dock on the device, the deploy area's meter).
// Over WebMIDI with install/fm1install.js and dock.js, the same protocols as the console and tools/dock.py.
// Nothing is sent before a button is pressed, and nothing is written before the question under it is answered.
// ?demo[=fm1|fresh|smk|stock|none]: a simulated device instead of WebMIDI (demo.js), for trying and testing the page.
//
// A module in modules/modules.json (the key is its id; fw/index.json's modules, from tools/site_packages.py, use it):
//   title, module (its name on the device, as in its .dock), by, author_url, original (its own page or repository),
//   links [{label, url}], license, voices, description (one line), tags [...] (the page adds "large" to an add-on
//   over 200 KiB: most of an FM-1's 296 KiB on its own), screenshot (media/...png), alt;
//   packaged: true for one in fm1-dock's package (no file, no button; size_kib for the card);
//   else file (the .dock next to modules.json), version, source, songs; pending: why the file is not here yet.
import { catalog, installButton, tuneButtons, inspectDevice, addModule, removeModule, loadModule, explain } from "./install/fm1install.js";
import { moduleInfo } from "./dock.js";

const DEMO = new URLSearchParams(location.search).get("demo");
if (DEMO !== null) await (await import("./demo.js")).start(DEMO);

const REPO = "https://github.com/joakimeriksson/fm1-dock";
// the deploy area, as firmware/src/dock.c has it (DP_LO..DP_HI; the SMK-37's two areas)
const BOARD = {
  fm1: { name: "FM-1", area: 0xE0000 - 0x96000, note: "" },
  smk37: { name: "SMK-37", area: (0xB8000 - 0x70000) + (0xE2000 - 0xC2000), note: " in two areas, 288 + 128 KiB" },
};
const SIGNED_FROM = [0, 7];                     // releases built and attested on GitHub (zvenson/fm1-build)
const PAGE = 4096;                               // a module takes whole 4 KiB flash sectors
const LARGE = 200 * 1024, LARGE_TAG = "large";   // an add-on that needs most of an FM-1's deploy area on its own
const VIDEO = [   // the tunes in the video, as FM-1 SID played them there
  { title: "Arkanoid", by: "Martin Galway", path: "MUSICIANS/G/Galway_Martin/Arkanoid.sid" },
  { title: "Wizball", by: "Martin Galway", path: "MUSICIANS/G/Galway_Martin/Wizball.sid", song: 7 },
  { title: "Commando", by: "Rob Hubbard", path: "MUSICIANS/H/Hubbard_Rob/Commando.sid" },
];

const $ = (s) => document.querySelector(s);
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
const url = (u) => (/^https?:\/\//.test(u || "") ? esc(u) : "");      // only web links from the catalog's data
const kib = (n) => `${Math.round(n / 1024)} KiB`;
const sectors = (n) => Math.ceil(n / PAGE) * PAGE;
const vparts = (v) => String(v || "").replace(/^v/, "").split(/[.+-]/).map((x) => parseInt(x, 10) || 0);
function vcmp(a, b) {
  const x = vparts(a), y = vparts(b);
  for (let i = 0; i < Math.max(x.length, y.length); i++) if ((x[i] || 0) !== (y[i] || 0)) return (x[i] || 0) - (y[i] || 0);
  return 0;
}
const fail = (code) => Object.assign(new Error(code), { code });

const S = {
  mods: [], index: null, midi: null, dev: null, devErr: null, connecting: false, gone: false,
  busy: null, msg: {}, filter: "all", q: "", hover: null,
};

// ------------------------------------------------------------------ the inline question ---

const dlg = $("#ask");
export function ask(text, yes = "Yes") {
  return new Promise((resolve) => {
    $("#ask-text").textContent = text;
    $("#ask-yes").textContent = yes;
    dlg.returnValue = "";
    dlg.addEventListener("close", () => resolve(dlg.returnValue === "yes"), { once: true });
    dlg.showModal();
    $("#ask-yes").focus();
  });
}

// ------------------------------------------------------------------ the catalog's data ---

async function load() {
  const [mods, index] = await Promise.all([
    fetch("modules/modules.json", { cache: "no-cache" }).then((r) => { if (!r.ok) throw new Error(`modules.json: HTTP ${r.status}`); return r.json(); }),
    catalog().catch(() => null),              // fw/index.json: on the published site (tools/site_packages.py)
  ]);
  S.index = index;
  S.mods = Object.entries(mods).map(([id, m]) => ({ id, ...m, addon: !m.packaged, tags: m.tags || [] }));
  await Promise.all(S.mods.filter((m) => m.addon).map(async (m) => {
    const e = index && index.modules && index.modules[m.id];
    if (e) {                                   // the site's checked copy: its size, version and name
      Object.assign(m, { entry: e, size: e.size, version: e.version, module: e.name || m.module, available: true });
      return;
    }
    if (!m.file) { m.available = false; return; }
    try {                                      // a local copy of the page: the file next to modules.json
      const r = await fetch(`modules/${encodeURIComponent(m.file)}`, { method: "HEAD", cache: "no-cache" });
      m.available = r.ok;
      if (r.ok) m.size = Number(r.headers.get("content-length")) || null;
    } catch (_) { m.available = false; }
  }));
  for (const m of S.mods) if (m.addon && m.size && sectors(m.size) > LARGE && !m.tags.includes(LARGE_TAG)) m.tags.push(LARGE_TAG);
}

async function moduleBlob(m) {
  if (m.loaded) return m.loaded;
  if (m.entry) m.loaded = await loadModule(m.entry);     // checked against the SHA-256 and ABI of index.json
  else {
    const r = await fetch(`modules/${encodeURIComponent(m.file)}`);
    if (!r.ok) throw new Error(`${m.file}: HTTP ${r.status}`);
    const blob = new Uint8Array(await r.arrayBuffer());
    m.loaded = { blob, info: moduleInfo(blob) };
  }
  return m.loaded;
}

// ------------------------------------------------------------------ the device ---

const latest = () => (S.index && S.index.firmwares && S.index.firmwares["fm1-dock"]) || null;
const dock = () => (S.dev && S.dev.dock) || null;
const boardOf = () => BOARD[(S.dev && S.dev.board) || "fm1"];
const devName = () => (S.dev && S.dev.board ? BOARD[S.dev.board].name : "FM-1");
const onDevice = (m) => { const d = dock(); return d ? d.modules.find((x) => x.name === m.module) : null; };

async function midiAccess() {
  if (S.midi) return S.midi;
  if (!navigator.requestMIDIAccess) throw fail("nomidi");
  try { S.midi = await navigator.requestMIDIAccess({ sysex: true }); } catch (_) { throw fail("denied"); }
  // a device unplugged or replugged: only noted here, never answered with MIDI (an install may be under way)
  const ids = () => [...S.midi.inputs.values()].filter((p) => p.state === "connected").map((p) => p.id).sort().join();
  let seen = ids();
  S.midi.addEventListener("statechange", () => {
    const now = ids();
    if (now === seen) return;
    seen = now;
    if (document.querySelector("[data-fm1-install][data-busy]")) return;   // (an install restarts the device: its own line says how far)
    if (S.dev && !S.busy && !S.connecting) { S.gone = true; renderDevice(); }
  });
  return S.midi;
}

const ACCESS = {
  nomidi: "This browser has no WebMIDI: open the page in Chrome or Edge on a computer.",
  denied: "MIDI (SysEx) was not allowed. Allow MIDI for this site (the icon in the address bar) and press Connect again.",
};

async function connect({ log = true } = {}) {
  if (S.connecting) return;
  S.connecting = true; S.gone = false;
  renderDevice();
  try {
    const keep = S.dev && S.dev.dock ? S.dev.dock.version : null;
    S.dev = await inspectDevice(await midiAccess(), { log });
    if (!log && S.dev.dock && !S.dev.dock.version) S.dev.dock.version = keep;
    S.devErr = null;
  } catch (e) {
    S.dev = null;
    S.devErr = ACCESS[e.code] || explain(e);
    console.error(e);
  }
  S.connecting = false;
  renderDevice();
  renderCatalog();
}

function renderDevice() {
  const pill = $("#dev-pill"), info = $("#dev-info"), btn = $("#dev-connect");
  btn.disabled = S.connecting || !!S.busy;
  btn.textContent = S.dev ? "Check again" : "Connect over USB";
  if (S.connecting) {
    pill.textContent = "asking…"; pill.className = "pill";
    info.innerHTML = `<p class="say">Asking the device what it runs…</p>`;
    return;
  }
  if (S.devErr) {
    pill.textContent = "not connected"; pill.className = "pill bad";
    info.innerHTML = `<p class="say bad">${esc(S.devErr)}</p>`;
    return;
  }
  const dv = S.dev;
  if (!dv) { pill.textContent = "not connected"; pill.className = "pill"; info.innerHTML = ""; return; }
  const gone = S.gone ? `<p class="say bad">The device went away or came back (a reset, or the cable): press Check again.</p>` : "";
  if (!dv.identity && !dv.dock) {
    pill.textContent = "no device"; pill.className = "pill bad";
    info.innerHTML = `<p class="say bad">No FM-1 or SMK-37 answered. Plug it in with a USB data cable (not a charge-only one),
      close other apps that use MIDI, and press Check again.</p>`;
    return;
  }
  const name = devName();
  if (dv.ota) {
    pill.textContent = "update mode"; pill.className = "pill bad";
    info.innerHTML = `${gone}<p class="say bad">The ${esc(name)} is in update mode (${esc(dv.identity)}): an install was cut off.
      Press Install fm1-dock below to finish it.</p>`;
    return;
  }
  if (!dv.dock) {
    pill.textContent = `${name}, no fm1-dock`; pill.className = "pill";
    info.innerHTML = `${gone}<table class="dev-table"><tr><th>Device</th><td>${esc(name)} <span class="dim mono">${esc(dv.identity)}</span></td></tr>
      <tr><th>Firmware</th><td>${/_921$/.test(dv.identity || "") ? "fm1-dock's identity, but it did not answer: an old fm1-dock?" : "not fm1-dock"}</td></tr></table>
      <p class="dim small" style="margin:10px 0 0">Install fm1-dock below. Its own firmware, or any other, is one install away
      on the <a href="https://thefirmwarehub.com">FM-1 Firmware Hub</a>.</p>`;
    return;
  }
  const d = dv.dock, rel = latest();
  let ver = d.version ? `fm1-dock ${esc(d.version)}` : "fm1-dock";
  let state = "";
  if (d.version === "0.0") state = `<span class="pill">a development build</span>`;
  else if (d.version && rel) state = vcmp(d.version, rel.version) >= 0 ? `<span class="pill ok">up to date</span>`
    : `<span class="pill acc">${esc(rel.version)} is out</span>`;
  else if (!d.version) ver += ` <span class="dim small">(its version is no longer in the log)</span>`;
  const mods = d.modules.map((m) => `<span class="${m.kind === "deployed" ? "added" : ""}${m.index === d.hello.cur ? " cur" : ""}"
    title="${esc(m.kind)}${m.index === d.hello.cur ? ", playing" : ""}">${esc(m.name)}</span>`).join("");
  pill.textContent = `${name} · fm1-dock`; pill.className = "pill ok";
  info.innerHTML = `${gone}<table class="dev-table">
    <tr><th>Device</th><td>${esc(name)} <span class="dim mono">${esc(dv.identity || "")}</span></td></tr>
    <tr><th>Firmware</th><td><div class="dev-row">${ver} ${state}</div></td></tr>
    <tr><th>Modules</th><td><div class="dev-mods">${mods}</div></td></tr>
    <tr><th>Room for add-ons</th><td>${kib(d.hello.free)} free of ${kib(boardOf().area)}</td></tr></table>`;
}

function renderRelease() {
  const rel = latest(), tag = $("#rel-ver"), prov = $("#prov");
  const shield = `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="M12 3 4 6v6c0 5 3.5 8 8 9 4.5-1 8-4 8-9V6z"/><path d="m9 12 2 2 4-4"/></svg>`;
  const relLink = rel && rel.release ? url(rel.release) : `${REPO}/releases/latest`;
  if (rel) { tag.textContent = rel.version; tag.hidden = false; }
  const signed = rel && vcmp(rel.version, SIGNED_FROM.join(".")) >= 0;
  const what = rel ? `<a href="${relLink}">fm1-dock ${esc(rel.version)}</a>, the latest release` : `<a href="${relLink}">The latest release</a>`;
  prov.innerHTML = `${shield}<span>${what}, checked against GitHub's SHA-256 and its own checksums before anything is sent.
    ${signed ? `Built from source and signed on GitHub (<a href="https://github.com/zvenson/fm1-build">zvenson/fm1-build</a>),
      with a <a href="${REPO}/attestations">build attestation</a>.`
    : `From v0.7 on, releases are built from source and signed on GitHub
      (<a href="https://github.com/zvenson/fm1-build">zvenson/fm1-build</a>), with a build attestation.`}
    SMK-37: <a href="${REPO}/blob/main/SMK37.md">read this first</a>.</span>`;
}

// ------------------------------------------------------------------ the meter ---

function renderMeter() {
  const el = $("#meter"), d = dock(), b = boardOf();
  const ghostOf = (total, free, largest) => {
    const m = S.hover;
    if (!m || !m.size || onDevice(m)) return null;
    const need = sectors(m.size);
    return { m, need, fits: largest == null ? need <= total : need <= largest, w: Math.min(need, free) };
  };
  let head, segs = [], free, total, largest = null, legend = [];
  if (!d) {
    total = b.area; free = total;
    head = `<h3>Room for add-ons</h3><span class="figs"><b>${kib(BOARD.fm1.area)}</b> on an FM-1 · <b>${kib(BOARD.smk37.area)}</b> on an SMK-37 ·
      connect to see yours</span>`;
  } else {
    total = b.area; free = d.hello.free; largest = d.hello.largest;
    const sizeOf = (x) => { const c = S.mods.find((m) => m.module === x.name && m.size); return sectors(c ? c.size : x.size); };
    segs = d.modules.filter((x) => x.kind === "deployed").map((x) => ({ name: x.name, n: sizeOf(x) }));
    const used = Math.max(0, total - free), sum = segs.reduce((a, s) => a + s.n, 0);
    if (sum > used && sum) segs.forEach((s) => { s.n = s.n * used / sum; });
    else if (used - sum > 8 * 1024) segs.push({ name: "other", n: used - sum, other: true });
    head = `<h3>Room for add-ons on your ${esc(b.name)}</h3><span class="figs"><b>${kib(used)}</b> used · <b>${kib(free)}</b> free
      (largest piece ${kib(largest)}) of ${kib(total)}${esc(b.note)}</span>`;
  }
  const g = ghostOf(total, free, largest);
  const pct = (n) => `${(100 * n / total).toFixed(2)}%`;
  const bar = segs.map((s) => `<div class="seg${s.other ? " other" : ""}" style="width:${pct(s.n)}" title="${esc(s.name)}: ${kib(s.n)}">${esc(s.name)}</div>`).join("")
    + (g ? `<div class="seg ghost${g.fits ? "" : " nofit"}" style="width:${pct(Math.max(g.w, total * 0.02))}" title="${esc(g.m.title)}: ${kib(g.need)}">${esc(g.m.title)}</div>` : "");
  legend = segs.map((s) => `<li><i class="${s.other ? "other" : ""}"></i>${esc(s.name)} <b>${kib(s.n)}</b></li>`);
  if (g) legend.push(`<li><i class="ghost"></i>${esc(g.m.title)} would take <b>${kib(g.need)}</b>${g.fits ? "" : ": it does not fit"}</li>`);
  if (d) legend.push(`<li><i class="free"></i>free <b>${kib(free)}</b></li>`);
  else if (!g) legend.push(`<li>Point at an add-on's card to see how much room it takes.</li>`);
  el.innerHTML = `<div class="meter-h">${head}</div>
    <div class="meter-bar" role="img" aria-label="${d ? `${kib(total - free)} of ${kib(total)} used` : `${kib(total)}, nothing known yet`}">${bar}</div>
    <ul class="legend">${legend.join("")}</ul>`;
}

// ------------------------------------------------------------------ the cards ---

function chips() {
  const tags = [...new Set(S.mods.flatMap((m) => m.tags))].sort();
  const kinds = [["all", "All"], ["builtin", "Built in"], ["addon", "Add-ons"]];
  if (dock()) kinds.push(["here", `On my ${devName()}`]);
  if (!kinds.some(([k]) => k === S.filter) && !tags.includes(S.filter)) S.filter = "all";
  const chip = ([k, label], i) => `<button type="button" class="chip${i === 0 && k !== "all" ? " sep" : ""}" data-f="${esc(k)}"
    aria-pressed="${S.filter === k}">${esc(label)}</button>`;
  $("#chips").innerHTML = kinds.map((k) => chip(k, 1)).join("") + tags.map((t, i) => chip([t, t], i)).join("");
}

function shown(m) {
  const f = S.filter;
  if (f === "builtin" && m.addon) return false;
  if (f === "addon" && !m.addon) return false;
  if (f === "here" && !onDevice(m)) return false;
  if (!["all", "builtin", "addon", "here"].includes(f) && !m.tags.includes(f)) return false;
  if (!S.q) return true;
  const hay = [m.title, m.module, m.by, m.description, m.voices, m.license, ...m.tags].join(" ").toLowerCase();
  return S.q.toLowerCase().split(/\s+/).every((w) => hay.includes(w));
}

function card(m) {
  const shot = m.screenshot
    ? `<img src="${esc(m.screenshot)}" width="480" height="480" alt="${esc(m.alt || `${m.title}'s screen`)}" loading="lazy">`
    : `<div class="noshot" role="img" aria-label="No screenshot of ${esc(m.title)} yet">${esc(m.title)}<small>no screenshot yet</small></div>`;
  const kind = m.builtin ? "In the host" : m.packaged ? "Built in" : "Add-on";
  const by = m.author_url ? `<a href="${url(m.author_url)}">${esc(m.by)}</a>` : esc(m.by);
  const links = [m.original && `<a href="${url(m.original)}">original</a>`,
    ...(m.links || []).map((l) => url(l.url) && `<a href="${url(l.url)}">${esc(l.label)}</a>`)].filter(Boolean);
  const size = m.addon ? (m.size ? kib(m.size) : "—") : m.builtin ? "in the host" : m.size_kib ? `${m.size_kib} KiB, in the package` : "in the package";
  const a = document.createElement("article");
  a.className = `mod ${m.addon ? "addon" : "builtin"}`;
  a.dataset.id = m.id;
  a.setAttribute("aria-labelledby", `m-${m.id}`);
  a.innerHTML = `<div class="shot">${shot}</div>
    <div class="mod-body">
      <div class="mod-head">
        <h3 id="m-${esc(m.id)}">${esc(m.title)}${m.addon && m.available && m.version ? `<span class="ver">${esc(m.version)}</span>` : ""}</h3>
        <span class="kind">${kind}</span>
      </div>
      <p class="by">by ${by}</p>
      <ul class="tags" aria-label="Tags">${m.tags.map((t) => `<li${t === LARGE_TAG ? ` class="large" title="${esc(largeNote(m))}"` : ""}>${esc(t)}</li>`).join("")}</ul>
      <p class="desc">${esc(m.description)}</p>
      <dl class="facts"><dt>Voices</dt><dd>${esc(m.voices || "—")}</dd><dt>Size</dt><dd>${size}</dd><dt>Licence</dt><dd>${esc(m.license || "—")}</dd></dl>
      ${links.length ? `<p class="links">${links.join("")}</p>` : ""}
      <div class="act"></div>
    </div>`;
  a.querySelector(".act").append(...actions(m));
  const hover = (on) => { if (!m.addon) return; S.hover = on ? m : (S.hover === m ? null : S.hover); renderMeter(); };
  a.addEventListener("mouseenter", () => hover(true));
  a.addEventListener("mouseleave", () => hover(false));
  a.addEventListener("focusin", () => hover(true));
  a.addEventListener("focusout", (e) => { if (!a.contains(e.relatedTarget)) hover(false); });
  return a;
}

// what a large add-on costs: most of an FM-1's deploy area (296 KiB), so little else fits beside it
function largeNote(m) {
  return `A large one: it needs ${kib(sectors(m.size))} of the FM-1's ${kib(BOARD.fm1.area)} for add-ons on its own, so little else fits beside it.`;
}

function button(text, cls, fn, disabled = false) {
  const b = Object.assign(document.createElement("button"), { type: "button", className: `btn ${cls}`, textContent: text, disabled });
  b.addEventListener("click", fn);
  return b;
}
function node(tag, cls, text) { return Object.assign(document.createElement(tag), { className: cls, textContent: text }); }

// the card's buttons and its line of state, for what is known of the device
function actions(m) {
  const out = [], msg = S.msg[m.id], name = devName(), d = dock(), here = onDevice(m);
  const row = node("div", "row2", "");
  if (!m.addon) {
    row.append(node("span", `state${here ? " ok" : ""}`, here ? `✓ On your ${name}` : "Comes with fm1-dock"));
    out.push(row);
  } else if (!m.available) {
    row.append(node("span", "state", "Not on this page yet"));
    out.push(row, node("p", "note", "Coming soon to the catalog; until then, tools/dock.py deploy with its .dock."));
  } else {
    const busy = S.busy === m.id, other = S.busy && !busy;
    if (here && here.kind === "deployed") {
      row.append(node("span", "state ok", "✓ Installed"), button(busy ? "Removing…" : "Remove", "danger", () => remove(m), !!S.busy));
    } else if (here) {
      row.append(node("span", "state ok", `✓ On your ${name} (in its package)`));
    } else {
      const fits = !d || !m.size || sectors(m.size) <= d.hello.largest;
      const blocked = !navigator.requestMIDIAccess || (S.dev && !d) || !fits || other || busy;
      row.append(button(busy ? "Adding…" : `Add to my ${name}`, "primary wide", () => add(m), blocked));
      if (!msg) {
        const large = m.tags.includes(LARGE_TAG);
        if (S.dev && !d) out.push(node("p", "note", "Install fm1-dock first (above)."));
        else if (!fits) out.push(node("p", "note bad", `Needs ${kib(sectors(m.size))}; the largest free piece is ${kib(d.hello.largest)}. Remove another add-on first.`));
        else if (d && m.size) out.push(node("p", `note${large ? " warn" : ""}`, `Takes ${kib(sectors(m.size))} of the ${kib(d.hello.free)} free.`
          + (large && boardOf() === BOARD.fm1 ? ` A large one: it needs most of the FM-1's ${kib(BOARD.fm1.area)} on its own.` : "")));
        else if (large) out.push(node("p", "note warn", largeNote(m)));
      }
    }
    out.unshift(row);
    if (busy) {
      const p = Object.assign(document.createElement("progress"), { max: 100, value: msg && msg.x ? Math.round(msg.x * 100) : 0 });
      p.setAttribute("aria-label", `${m.title}: sending`);
      out.push(p);
    }
  }
  if (msg) { const p = node("p", `note ${msg.cls || ""}`, msg.text); p.setAttribute("role", "status"); out.push(p); }
  return out;
}

function say(m, text, cls = "", x = null) {
  S.msg[m.id] = { text, cls, x };
  const a = document.querySelector(`.mod[data-id="${CSS.escape(m.id)}"]`);
  if (!a) return;
  const act = a.querySelector(".act"), p = act.querySelector("progress"), note = act.querySelector('[role="status"]');
  if (x !== null && p && note && !cls) { p.value = Math.round(x * 100); note.textContent = text; return; }   // (a deploy's progress)
  act.replaceChildren(...actions(m));
}

function renderCatalog() {
  chips();
  const list = S.mods.filter(shown), grid = $("#catalog");
  grid.replaceChildren(...list.map(card));
  if (!list.length) grid.innerHTML = `<p class="empty">No module matches. <button type="button" class="btn small" id="clear">Show all</button></p>`;
  $("#count").textContent = `${list.length} of ${S.mods.length} modules`;
  renderMeter();
}

async function add(m) {
  if (S.busy || S.connecting) return;
  if (!dock()) {                                // first what is on the device: then the card knows if it is there, if it fits
    await connect();
    const no = !dock() ? S.devErr || "No fm1-dock answered: see Your device, above." : onDevice(m) ? `Already on your ${devName()}.` : null;
    if (no) S.msg[m.id] = { text: no, cls: onDevice(m) ? "ok" : "bad" };
    if (no || (m.size && sectors(m.size) > dock().hello.largest)) { renderCatalog(); return; }
  }
  S.busy = m.id;
  S.msg[m.id] = { text: "Fetching the module…" };
  renderCatalog();
  renderDevice();
  try {
    const mod = await moduleBlob(m);
    const midi = await midiAccess();
    say(m, `Looking for fm1-dock on the ${devName()}…`);
    const got = await addModule(midi, mod, (q) => ask(q, /^Update/.test(q) ? "Update" : "Add"),
      (x, t) => say(m, `Sending: ${t}`, "", x));
    if (got === null) say(m, "Cancelled: nothing was sent.");
    else say(m, `Done: ${got} is in fm1-dock. Hold HOME on the ${devName()} and pick it.`, "ok");
  } catch (e) {
    say(m, ACCESS[e.code] || explain(e), "bad");
    console.error(e);
  }
  S.busy = null;
  await connect({ log: !(dock() && dock().version) });
}

async function remove(m) {
  if (S.busy) return;
  S.busy = m.id;
  delete S.msg[m.id];
  renderCatalog();
  renderDevice();
  try {
    const st = await removeModule(await midiAccess(), m.module, (q) => ask(q, "Remove"));
    if (st === null) say(m, "Cancelled: nothing was changed.");
    else if (st === "done") say(m, `Removed: its ${kib(sectors(m.size || 0))} are free again.`, "ok");
    else say(m, `fm1-dock said: ${st}`, "bad");
  } catch (e) {
    say(m, ACCESS[e.code] || explain(e), "bad");
    console.error(e);
  }
  S.busy = null;
  await connect({ log: !(dock() && dock().version) });
}

// ------------------------------------------------------------------ start ---

$("#dev-connect").addEventListener("click", () => connect());
$("#chips").addEventListener("click", (e) => {
  const b = e.target.closest("[data-f]");
  if (!b) return;
  S.filter = S.filter === b.dataset.f && b.dataset.f !== "all" ? "all" : b.dataset.f;
  renderCatalog();
});
$("#catalog").addEventListener("click", (e) => { if (e.target.id === "clear") { S.filter = "all"; S.q = ""; $("#q").value = ""; renderCatalog(); } });
$("#q").addEventListener("input", (e) => { S.q = e.target.value.trim(); renderCatalog(); });
if (!navigator.requestMIDIAccess) { $("#nomidi").hidden = false; $("#dev-connect").disabled = true; }

for (const el of document.querySelectorAll("[data-fm1-install]"))
  installButton(el, el.dataset.fm1Install, { buttonClass: "btn primary big", ask: (q) => ask(q, "Install"),
    onDone: () => setTimeout(() => connect(), 2500) });
for (const el of document.querySelectorAll("[data-fm1-tunes]")) tuneButtons(el, VIDEO, { buttonClass: "btn" });

renderDevice();
try {
  await load();
} catch (e) {
  $("#catalog").innerHTML = `<p class="empty">The module list did not load (${esc(e.message)}).</p>`;
  console.error(e);
}
renderRelease();
renderCatalog();
