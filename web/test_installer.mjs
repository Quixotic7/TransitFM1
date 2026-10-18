// SPDX-License-Identifier: GPL-3.0-only
// The web installer's decisions (web/transit.js) against a fake FM-1 on a fake Web MIDI: the released package checked,
// installs over the firmwares that answer as V15 and over ChoralRoot, Bubba Machine and Sloop refused (Sloop only
// without the override), an install cut off finished from update mode, a cancel that sends nothing.
//   node web/test_installer.mjs build/release-0.1/transit-0.1.fwsc [FM-1.fwsc]
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { pack7, unpack7 } from "./fm1ota.js";
import { install, look, describe, packageFrom, releasedPackage, IDENTITY } from "./transit.js";

const realTimeout = setTimeout;                    // the protocol's waits (seconds) shortened 20 times
globalThis.setTimeout = (f, ms, ...a) => realTimeout(f, ms >= 200 ? ms / 20 : ms, ...a);

let fails = 0, checks = 0;
const ok = (cond, what) => { checks++; if (!cond) fails++; console.log(`${what.padEnd(78)} ${cond ? "ok" : "FAIL"}`); };
const eq = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const HS = [0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7];
const UPGRADE = [0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7];

// a FM-1: the handshake, INFO (version: the backup protocol's answer, or none), step 1 (four reads and "success"),
// then the loader (six reads and the finish), then the firmware written
class FakeFM1 {
  constructor(image, { identity = "FM-1_015", name = "FM-1", version = null, loader = "ota-FM-1_900", final = IDENTITY } = {}) {
    this.image = image; this.served = 0; this.bad = 0; this.upgrades = 0; this.version = version;
    this.loader = loader; this.final = final;
    this.access = { inputs: new Map(), outputs: new Map() };
    this.boot(identity, name);
  }
  boot(identity, name) {
    this.identity = identity; this.waiting = null; this.queue = [];
    for (const m of [this.access.inputs, this.access.outputs]) { for (const p of m.values()) p.state = "disconnected"; m.clear(); }
    const id = Math.random().toString(36).slice(2);
    this.input = { id: "i" + id, name, state: "connected", onmidimessage: null, open: async () => {} };
    this.output = { id: "o" + id, name, state: "connected", open: async () => {}, send: (d) => {
      if (this.output.state !== "connected") throw new Error("InvalidStateError");
      realTimeout(() => this.rx(Array.from(d)), 1);
    } };
    this.access.inputs.set(this.input.id, this.input);
    this.access.outputs.set(this.output.id, this.output);
  }
  tx(bytes) { const i = this.input; realTimeout(() => { if (i.state === "connected" && i.onmidimessage) i.onmidimessage({ data: Uint8Array.from(bytes) }); }, 1); }
  rx(d) {
    if (d[0] === 0xF0 && d[1] === 0x7D && d[2] === 0x46 && d[3] === 0x4C) {     // INFO
      if (d[4] === 1 && this.version && !this.identity.startsWith("ota-"))
        this.tx([0xF0, 0x7D, 0x46, 0x4C, 1, ...new TextEncoder().encode(this.version), 0, 0xF7]);
      return;
    }
    if (eq(d, HS)) {
      const t = [...new TextEncoder().encode(this.identity)];
      this.tx([0xF0, ...pack7([0, 0x59, 0x11, 0, 0, 0, ...t, ...new Array(28 - t.length).fill(0)]), 0xF7]);
    } else if (eq(d, UPGRADE)) {
      this.upgrades++;
      this.queue = this.identity.startsWith("ota-")
        ? [...Array.from({ length: 6 }, (_, k) => [k * 512, 512]), [0xF0000000, 8]]
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
      if (addr === 0xE0000000) realTimeout(() => this.boot(this.loader, "Felucca Update"), 20);
      else if (addr === 0xF0000000) realTimeout(() => this.boot(this.final, "FM-1"), 20);
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

const UI = (answer = true) => {
  const u = { said: [], kinds: [], confirms: 0, prog: 0 };
  return Object.assign(u, { say(t, k = "") { u.said.push(t); u.kinds.push(k); }, progress(f) { u.prog = f; },
                            confirm() { u.confirms++; return answer; }, log() {} });
};

// the page: every element transit.js looks up is in index.html (0.1.1's first page lost #ver: no install button)
{
  const page = readFileSync(new URL("./index.html", import.meta.url), "utf8");
  const js = readFileSync(new URL("./transit.js", import.meta.url), "utf8");
  const ids = [...new Set([...js.matchAll(/(?:\$|put)\("([\w-]+)"/g)].map((m) => m[1]))];
  const missing = ids.filter((id) => !new RegExp(`id="${id}"`).test(page));
  ok(ids.length >= 10 && !missing.length, `the page has every element transit.js uses (${ids.length})${missing.length ? ": missing " + missing.join(", ") : ""}`);
}

const pkgPath = process.argv[2], v15Path = process.argv[3];
if (!pkgPath) { console.log("usage: node web/test_installer.mjs transit-X.Y.fwsc [FM-1.fwsc]"); process.exit(2); }
const raw = new Uint8Array(readFileSync(pkgPath));
const sha = createHash("sha256").update(raw).digest("hex");
const rel = { version: "9.9", sha256: sha };
const fakeFetch = (bytes) => async (url) => ({ ok: url === "firmware/transit-9.9.fwsc", status: 404, arrayBuffer: async () => bytes.buffer });

// the package
const pkg = await releasedPackage(rel, fakeFetch(raw));
ok(pkg.product === IDENTITY && !pkg.official && pkg.label === "TransitFM1 9.9", `the released package: identity ${pkg.product}, labelled`);
let err = "";
try { const bad = raw.slice(); bad[5000] ^= 1; await releasedPackage(rel, fakeFetch(bad)); } catch (e) { err = e.message; }
ok(/not the released one/.test(err), "a package whose SHA-256 differs is refused before anything");
err = "";
try { await packageFrom(new Uint8Array(4000)); } catch (e) { err = e.message; }
ok(/not an FM-1|not for the FM-1/.test(err), "a file that is not an FM-1 package is refused");
if (v15Path) {
  const v15 = await packageFrom(new Uint8Array(readFileSync(v15Path)));
  ok(v15.official && v15.product === "FM-1_015" && /official V15/.test(v15.label), "M-VAVE's FM-1.fwsc: recognised as the official V15");
}

// what is on the FM-1
const st = async (o) => describe(await look(new FakeFM1(pkg.image, o).access));
ok(/official firmware \(FM-1_015\).*TransitFM1/.test(await st({})), "look: FM-1_015 is stock V15 or TransitFM1");
ok(/ChoralRoot 0\.25\.1\./.test(await st({ identity: "FM-1_920", name: "ChoralRoot FM-1", version: "ChoralRoot 0.25.1" })),
   "look: ChoralRoot by its INFO");
ok(/Bubba Machine/.test(await st({ identity: "FM-1_003", name: "FM-1 Bubba Machine" })), "look: Bubba Machine by its port");
ok(/update mode/.test(await st({ identity: "ota-FM-1_900", name: "Felucca Update" })), "look: update mode");
ok(/No FM-1 found/.test(describe(await look({ inputs: new Map(), outputs: new Map() }))), "look: nothing connected");

// installs
const run = async (o, opts = {}, answer = true) => {
  const dev = new FakeFM1(pkg.image, o), ui = UI(answer);
  let r, e = null;
  try { r = await install(dev.access, pkg, ui, opts); } catch (x) { e = x; }
  return { dev, ui, r, e };
};
let t = await run({});
ok(t.r === "done" && t.dev.upgrades === 2 && t.dev.bad === 0 && t.dev.identity === IDENTITY && t.ui.confirms === 1 &&
   t.ui.prog === 1 && t.ui.kinds.at(-1) === "ok", "over the stock V15 (or TransitFM1): installed, both steps served right");
t = await run({ identity: "FM-1_920", name: "ChoralRoot FM-1", version: "ChoralRoot 0.25.1" });
ok(t.r === "done" && t.dev.bad === 0 && t.dev.identity === IDENTITY, "over ChoralRoot: installed");
t = await run({ identity: "FM-1_910", name: "Felucca", version: "FELUCCA v1.0.1" });
ok(t.r === "done" && t.dev.bad === 0, "over Felucca 1.0.1: installed");
t = await run({ identity: "FM-1_003", name: "FM-1 Bubba Machine" }, { override: true });
ok(t.r === "refused" && t.dev.upgrades === 0 && t.ui.confirms === 0 && /Bubba/.test(t.ui.said.at(-1)),
   "over Bubba Machine: refused, even with the override; nothing sent");
t = await run({ identity: "FM-1_900", name: "Felucca", version: "FELUCCA SLOOP 2.2" });
ok(t.r === "refused" && t.dev.upgrades === 0 && /Sloop/.test(t.ui.said.at(-1)), "over Sloop: refused, nothing sent");
t = await run({ identity: "FM-1_900", name: "Felucca", version: "FELUCCA SLOOP 2.2" }, { override: true });
ok(t.r === "done" && t.dev.bad === 0, "over Sloop with the override: installed");
t = await run({}, {}, false);
ok(t.r === "cancelled" && t.dev.upgrades === 0, "the confirmation declined: nothing sent");
t = await run({ identity: "ota-FM-1_900", name: "Felucca Update" });
ok(t.r === "resumed" && t.dev.upgrades === 1 && t.dev.bad === 0 && t.dev.identity === IDENTITY && t.ui.confirms === 0,
   "in update mode (an install cut off): the write finished, no question");
t = await run({ identity: "ota-FM-1_015", name: "ota-FM-1" });
ok(t.e && /another firmware/.test(t.e.message) && t.dev.upgrades === 0,
   "in the official loader's update mode: not written (finish it with M-VAVE's own package)");

console.log(fails ? `installer: ${fails} of ${checks} FAILED` : `installer tests passed (${checks})`);
process.exit(fails ? 1 : 0);
