// SPDX-License-Identifier: GPL-3.0-only
//
// The FM-1's MIDI ports among a browser's. One device is an input and an output, and the two need not have the same
// name: macOS, Android and Linux give both the device's name, Windows does not always. Its WinMM numbers a second
// device of a name ("2- FM-1"), gives a device with more ports "MIDIIN2 (FM-1)" / "MIDIOUT2 (FM-1)", and cuts every
// name at 31 characters; Windows MIDI Services may add " In" / " Out" or a port number. So the ports are paired by
// a key of the name (portKey), then by a cut name's start, and what is left is tried input against output.

// a name an FM-1, an SMK-37 or their update loaders show (fm1-dock's is Felucca's; stock's in update mode "ota"), or
// Windows' name for a USB-MIDI device whose own name it did not take
export const LOOKS = /felucca|fm1-dock|fm-?1|dinghy|smk|elite|mke-?p37|starry|m-?vave|sinco|composite|ota|usb[- ]?midi|usb[- ]?audio|midi ?device|audio ?device/i;
// ports that are never a device: the operating systems' own and virtual ones
export const NOT_DEVICE = /\biac\b|gs wavetable|microsoft|midi through|virtual|network|session|loopmidi|logic pro/i;

const WRAP = /^MIDI(?:IN|OUT)\d*\s*\((.*?)\)?$/i;          // "MIDIIN2 (FM-1)", ")" lost when cut at 31
export function portKey(name) {
  let s = String(name || "").trim();
  for (let k = 0; k < 2; k++) {                             // ("2- MIDIIN2 (FM-1)": both)
    s = s.replace(/^\d+\s*-\s*/, "");                       // "2- FM-1"
    const m = WRAP.exec(s);
    if (m) s = m[1];
  }
  s = s.replace(/\s*\[\d+\]$/, "").replace(/\s+(?:in|out|input|output)(?:\s*\d+)?$/i, "");   // " [2]", " In", " Out 1"
  return s.replace(/\s+/g, " ").trim().toLowerCase();
}

// a name cut at 31 characters on one side: the shorter key the start of the longer (and long enough to mean it)
const near = (a, b) => a !== b && Math.min(a.length, b.length) >= 6 && (a.startsWith(b) || b.startsWith(a));

const live = (ports) => { const a = []; ports.forEach((p) => { if (p.state !== "disconnected") a.push(p); }); return a; };
export const looksOurs = (p) => LOOKS.test(p.input.name || "") || LOOKS.test(p.output.name || "");

// -> [{ input, output, name, guess }]: pairs by the same name, by the same key, by a cut name; then (guess: true)
// every input left with every output left. The device's names first, guesses after pairs.
export function pairPorts(inputs, outputs) {
  const ins = live(inputs), outs = live(outputs), used = new Set(), pairs = [];
  const add = (i, o, guess = false) => {
    pairs.push({ input: i, output: o, name: i.name === o.name ? i.name : `${i.name} / ${o.name}`, guess });
    if (!guess) { used.add(i); used.add(o); }
  };
  for (const match of [(i, o) => i.name === o.name, (i, o) => portKey(i.name) === portKey(o.name),
    (i, o) => near(portKey(i.name), portKey(o.name))]) {
    for (const i of ins) {
      if (used.has(i)) continue;
      const o = outs.find((x) => !used.has(x) && match(i, x));
      if (o) add(i, o);
    }
  }
  const restIn = ins.filter((i) => !used.has(i)), restOut = outs.filter((o) => !used.has(o));
  for (const i of restIn) for (const o of restOut) add(i, o, true);
  const rank = (p) => (looksOurs(p) ? 2 : 0) + (p.guess ? 0 : 1);
  return pairs.map((p, k) => [p, k]).sort((a, b) => rank(b[0]) - rank(a[0]) || a[1] - b[1]).map(([p]) => p);
}
