// SPDX-License-Identifier: GPL-3.0-only
// docs/install/ports.js on node: the FM-1's input and output paired however a system names them (Windows: "2- x",
// "MIDIIN2 (x)" / "MIDIOUT2 (x)", names cut at 31 characters, " In" / " Out"), and the updater finding the FM-1 by its
// handshake when the names do not pair, or saying that another program holds its port.
//   node tests/ports_test.mjs
import { portKey, pairPorts } from "../docs/install/ports.js";
import { Updater, pack7 } from "../docs/install/fm1ota.js";
import { inspectDevice, explain } from "../docs/install/fm1install.js";

let failed = 0;
const ok = (c, what) => { console.log(`${c ? "ok  " : "FAIL"} ${what}`); if (!c) failed++; };
const port = (name, extra = {}) => ({ id: name + Math.random(), name, state: "connected", open: async () => {}, ...extra });
const access = (ins, outs) => ({ inputs: new Map(ins.map((p) => [p.id, p])), outputs: new Map(outs.map((p) => [p.id, p])) });

// ------------------------------------------------------------------ names ---
for (const [a, b] of [["MIDIIN2 (Felucca)", "MIDIOUT2 (Felucca)"], ["2- FM-1", "FM-1"], ["2- MIDIIN2 (FM-1)", "MIDIOUT2 (FM-1)"],
  ["Felucca In", "Felucca Out"], ["Felucca [1]", "Felucca"], ["felucca", "Felucca "]])
  ok(portKey(a) === portKey(b), `portKey: "${a}" and "${b}" are one device (${portKey(a)})`);
ok(portKey("FM-1") !== portKey("SMK-37 Pro"), "portKey: two devices stay two");

// the systems' port lists: [system, inputs, outputs, the FM-1's input, its output]
const SYSTEMS = [
  ["macOS", ["IAC Driver Bus 1", "Felucca"], ["IAC Driver Bus 1", "Felucca"], "Felucca", "Felucca"],
  ["Android", ["Felucca"], ["Felucca"], "Felucca", "Felucca"],
  ["Linux (ALSA)", ["Midi Through Port-0", "Felucca:Felucca MIDI 1 24:0"], ["Midi Through Port-0", "Felucca:Felucca MIDI 1 24:0"],
    "Felucca:Felucca MIDI 1 24:0", "Felucca:Felucca MIDI 1 24:0"],
  ["Windows, WinMM", ["Felucca"], ["Microsoft GS Wavetable Synth", "Felucca"], "Felucca", "Felucca"],
  ["Windows, a second port", ["MIDIIN2 (Felucca)", "Arturia KeyStep 37"], ["Microsoft GS Wavetable Synth", "Arturia KeyStep 37", "MIDIOUT2 (Felucca)"],
    "MIDIIN2 (Felucca)", "MIDIOUT2 (Felucca)"],
  ["Windows, a second FM-1 of the name", ["FM-1", "2- FM-1"], ["FM-1", "2- FM-1"], "FM-1", "FM-1"],
  ["Windows, stock's name cut at 31", ["MIDIIN2 (M-VAVE FM-1 Synthesizer Ke"], ["MIDIOUT2 (M-VAVE FM-1 Synthesizer K"],
    "MIDIIN2 (M-VAVE FM-1 Synthesizer Ke", "MIDIOUT2 (M-VAVE FM-1 Synthesizer K"],
  ["Windows MIDI Services", ["Felucca In"], ["Felucca Out", "Microsoft GS Wavetable Synth"], "Felucca In", "Felucca Out"],
  ["Windows, a generic name", ["USB Audio Device"], ["Microsoft GS Wavetable Synth", "USB Audio Device"], "USB Audio Device", "USB Audio Device"],
];
for (const [sys, ins, outs, wi, wo] of SYSTEMS) {
  const a = access(ins.map((n) => port(n)), outs.map((n) => port(n)));
  const first = pairPorts(a.inputs, a.outputs)[0];
  ok(first && !first.guess && first.input.name === wi && first.output.name === wo,
    `${sys}: the FM-1's pair first (${first ? first.name : "none"})`);
}
{
  const a = access([port("Felucca"), port("Launchkey")], [port("Launchkey"), port("loopMIDI Port")]);
  const p = pairPorts(a.inputs, a.outputs);
  ok(p[0].guess && p[0].input.name === "Felucca" && p.some((x) => !x.guess && x.name === "Launchkey"),
    "an input with no output of its name: paired with the others only as guesses, its own first");
}
{
  const a = access([port("Felucca", { state: "disconnected" })], [port("Felucca")]);
  ok(pairPorts(a.inputs, a.outputs).length === 0, "a disconnected port is not paired");
}

// ------------------------------------------------------------------ the handshake ---
const HS = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7];
const identity = (text) => { const t = [...new TextEncoder().encode(text)];
  return Uint8Array.from([0xF0, ...pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...new Array(28 - t.length).fill(0)]), 0xF7]); };
// a device: its input and output ports (any names); it answers the handshake on its input
function device(inName, outName, text = "FM-1_921", busy = false) {
  const input = port(inName, { onmidimessage: null, open: async () => { if (busy) throw new Error("InvalidAccessError"); } });
  const output = port(outName, { open: async () => { if (busy) throw new Error("InvalidAccessError"); }, send: (d) => {
    if (Array.from(d).every((b, i) => b === HS[i]) && d.length === HS.length)
      setTimeout(() => input.onmidimessage && input.onmidimessage({ data: identity(text) }), 2);
  } });
  return { input, output };
}
const quiet = (name) => ({ input: port(name, { onmidimessage: null }), output: port(name, { send: () => {} }) });
{
  const d = device("MIDIIN2 (Felucca)", "MIDIOUT2 (Felucca)"), k = quiet("KeyStep");
  const dev = await new Updater(access([k.input, d.input], [k.output, d.output])).find();
  ok(dev && dev.id.text === "FM-1_921" && dev.link.output === d.output, `Windows' MIDIIN2 / MIDIOUT2: found (${dev && dev.name})`);
  if (dev) dev.link.close();
}
{
  const d = device("Felucca", "Felucca MIDI Output"), k = quiet("KeyStep");   // (names that pair by nothing)
  const dev = await new Updater(access([k.input, d.input], [k.output, d.output])).find();
  ok(dev && dev.id.text === "FM-1_921" && dev.link.input === d.input && dev.link.output === d.output,
    `names that do not pair: found by asking each output (${dev && dev.name})`);
  if (dev) dev.link.close();
}
{
  const d = device("Dispositivo 1", "Dispositivo 2");   // (a name of no FM-1 look, the two not paired)
  const narrow = await new Updater(access([d.input], [d.output])).find();
  const wide = await new Updater(access([d.input], [d.output])).find(null, { wide: true });
  ok(!narrow && wide && wide.id.text === "FM-1_921", "a name of no FM-1 look: only a press of Connect (wide) asks it, and finds it");
  if (wide) wide.link.close();
}
{
  const d = device("Felucca", "Felucca", "FM-1_921", true);
  const up = new Updater(access([d.input], [d.output]));
  const dev = await up.find();
  const e = up.missing();
  ok(!dev && e.code === "inuse" && e.detail === "Felucca", `a port another program holds: "inuse" (${e.detail})`);
  ok(/another program or browser tab/.test(explain(e)), "  and the page says so: " + explain(e).slice(0, 60) + "…");
  const ie = await inspectDevice(access([d.input], [d.output])).then(() => null, (x) => x);
  ok(ie && ie.code === "inuse", "inspectDevice (Connect): throws inuse, not 'no FM-1 answered'");
}
{
  const up = new Updater(access([], []));
  ok(!(await up.find()) && up.missing().code === "notfound", "nothing plugged in: notfound");
}

console.log(failed ? `PORT TESTS FAILED (${failed})` : "port tests passed");
process.exit(failed ? 1 : 0);
