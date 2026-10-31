// SPDX-License-Identifier: GPL-3.0-only
// The catalog's "Try it" (hub.js): a module in fm1-emu's browser emulator on this site, running fm1-dock with the
// module installed and picked. tools/tryit.py makes tryit/<id>.flash (the deployed table and the module as a deploy
// leaves them) and tryit/<id>.txt (SELECT into the DIN jack), listed in tryit/index.json; fm1-emu's page takes them
// as ?flash= and ?script= from its own origin only, so the emulator has to be on this page's origin
// (joakimeriksson.github.io/fm1-emu/ beside /fm1-dock/; locally, the two side by side). Nothing goes to a device.

export const EMU = "../fm1-emu/";               // fm1-emu's page, beside this one

// tryit/index.json, or null (a page without it, as docs/ served alone, has no Try it)
export async function tryIndex(fetchFn = fetch, base = "tryit/") {
  try {
    const r = await fetchFn(`${base}index.json`, { cache: "no-cache" });
    if (!r.ok) return null;
    const j = await r.json();
    return j && j.modules && typeof j.modules === "object" ? j : null;
  } catch (_) { return null; }
}

// fm1-emu's page for module id (an entry of tryit/index.json), as a URL on page's origin; embed: its window-only view
export function emulatorUrl(page, index, id, { embed = true, base = "tryit/" } = {}) {
  const e = index && index.modules && index.modules[id];
  if (!e || !e.script) return null;
  const file = (f) => new URL(base + f, page).pathname;      // (a path: the emulator takes only its own origin's)
  const u = new URL(EMU, page);
  if (embed) u.searchParams.set("embed", "1");
  u.searchParams.set("fw", index.firmware || "fm1dock");
  if (e.flash) u.searchParams.set("flash", file(e.flash));
  u.searchParams.set("script", file(e.script));
  return u;
}

const STATES = ["loading", "booting", "playing", "start", "error"];
// a message from the emulator's window about the try-out: {state, text}, or null if it is not one
export function tryOutNews(e, frame, origin) {
  const d = e && e.data;
  if (!e || e.origin !== origin || (frame && e.source !== frame) || !d || d.source !== "fm1-emu" || d.type !== "tryout") return null;
  return STATES.includes(d.state) ? { state: d.state, text: typeof d.text === "string" ? d.text.slice(0, 300) : "" } : null;
}
