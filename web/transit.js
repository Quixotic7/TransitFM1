// SPDX-License-Identifier: GPL-3.0-only
// The TransitFM1 web installer (index.html): install TransitFM1 on an FM-1 over Web MIDI, finish an install that was
// cut off, read what the FM-1 runs, install another package file. The protocol is fm1ota.js (Felucca's, as
// ChoralRoot carries it), the package reading fm1pkg.js. Everything that decides is here and runs without a page
// (web/test_installer.mjs drives it against a fake FM-1); the DOM wiring is at the end, in the browser only.
import { Updater, classifyFirmware } from "./fm1ota.js";
import { Package, productOf, logicalImage, sha256hex, STOCK_V15_SHA256 } from "./fm1pkg.js";

export const IDENTITY = "FM-1_015";               // TransitFM1's (tools/build.py PRODUCT): the official V15's
const INFO_REQ = [0xF0, 0x7D, 0x46, 0x4C, 0x01, 0xF7];   // the backup protocol's INFO (Felucca, ChoralRoot, ...)

// the version text a Felucca-family firmware answers to INFO, or null (the stock firmware, TransitFM1, others)
export async function deviceInfo(input, output, ms = 1500) {
  try { await input.open(); await output.open(); } catch (_) { return null; }
  return new Promise((resolve) => {
    const done = (v) => { clearTimeout(timer); input.onmidimessage = null; resolve(v); };
    const timer = setTimeout(() => done(null), ms);
    input.onmidimessage = (e) => {
      const d = e.data;
      if (d.length < 6 || d[0] !== 0xF0 || d[1] !== 0x7D || d[2] !== 0x46 || d[3] !== 0x4C || d[4] !== 1) return;
      const a = Array.from(d.slice(5, -1)), end = a.indexOf(0);
      done(String.fromCharCode(...a.slice(0, end < 0 ? a.length : end)) || null);
    };
    try { output.send(INFO_REQ); } catch (_) { done(null); }
  });
}

// what the connected FM-1 runs -> { none } | { several, list } | { loader, identity } | { identity, port, version, fw }
export async function look(access, log = () => {}) {
  const up = new Updater(access, log);
  const all = await up.all();
  if (!all.length) return { none: true };
  if (all.length > 1) return { several: true, list: all.map((d) => `${d.id.text} (${d.name})`) };
  const d = all[0];
  if (/^ota-/i.test(d.id.model)) return { loader: true, identity: d.id.text, port: d.name };
  const version = await deviceInfo(d.input, d.output);
  return { identity: d.id.text, port: d.name, version, fw: classifyFirmware(d.id.text, version, d.name) };
}

// a sentence for "What is on my FM-1?"
export function describe(st) {
  if (st.none) return "No FM-1 found. Connect it with a USB data cable, switch it on, and close any other app that uses its MIDI port.";
  if (st.several) return `Several FM-1s answer (${st.list.join(", ")}). Connect only the one to install.`;
  if (st.loader) return `The FM-1 is in update mode (${st.identity}): an install was cut off. Choose the same firmware and press Install to finish it.`;
  if (st.fw.kind === "stock") return `The FM-1 answers as the official firmware (${st.identity}): stock V15, or TransitFM1, which answers the same.`;
  if (st.fw.kind === "bubba") return `The FM-1 runs Bubba Machine (${st.identity}). Its updater accepts only M-VAVE's own package: go back to V15 with Bubba Machine's installer first.`;
  return `The FM-1 runs ${st.fw.name}` + (st.fw.verdict === "allow" ? "." : `, which is not installed over from here (an install over it has left FM-1s that no longer start).`);
}

// the released package, as the site hosts it: fetched, its SHA-256 the one in releases.json, identity FM-1_015
export async function releasedPackage(rel, fetchFn = fetch) {
  const r = await fetchFn(`firmware/transit-${rel.version}.fwsc`, { cache: "no-cache" });
  if (!r.ok) throw new Error(`The package for TransitFM1 ${rel.version} could not be downloaded (${r.status}).`);
  const bytes = new Uint8Array(await r.arrayBuffer());
  if (await sha256hex(bytes) !== rel.sha256)
    throw new Error(`The file for TransitFM1 ${rel.version} is not the released one (its SHA-256 does not match). Nothing was written. Reload the page.`);
  return packageFrom(bytes, `TransitFM1 ${rel.version}`);
}

// any FM-1 package (a file the user chose): its identity, the image the device reads, a name to show
export async function packageFrom(bytes, label = null) {
  let product;
  try { product = productOf(bytes); } catch (e) { throw new Error("This is not an FM-1 firmware package (.fwsc)."); }
  if (!/^FM-1_\d+$/.test(product)) throw new Error(`This package is not for the FM-1 (its identity is ${product || "empty"}).`);
  let flashSize = 0;
  try { flashSize = new Package(bytes).flashSize; } catch (_) { /* (a layout the reader does not know: the FM-1 decides) */ }
  if (flashSize > 0x93000)   // TransitFM1 writes the app area only (firmware/src/ota.c refuses it too): Baud Girl's grows past it
    throw new Error("This firmware is larger than TransitFM1 can install (its image reaches past the app area, as Baud Girl's does). Install the official V15 first, then this firmware with its own installer.");
  const official = await sha256hex(bytes) === STOCK_V15_SHA256;
  return { product, image: logicalImage(bytes), official,
           label: label || (official ? "the official V15 (M-VAVE's FM-1.fwsc)" : `the package ${product}`) };
}

// install pkg on the connected FM-1, or finish an install that was cut off. ui: { say(text, kind), progress(0..1),
// confirm(text) -> bool, log(text) }; opts.override: install over a firmware that is refused by default. Returns
// "done" | "resumed" | "refused" | "cancelled"; throws with a message the page shows
export async function install(access, pkg, ui, opts = {}) {
  const up = new Updater(access, ui.log);
  const st = await look(access, ui.log);
  if (st.none || st.several) { ui.say(describe(st), "bad"); return "refused"; }
  const step = (k, n) => {
    if (k === "start") ui.say(`Starting the update on ${n}...`);
    if (k === "verify") ui.progress(Math.min(0.05, n / 1500));
    if (k === "loader") ui.say("The FM-1 restarts into update mode...");
    if (k === "write") { ui.say("Writing. Do not unplug the FM-1 or switch it off."); ui.progress(Math.min(0.99, 0.05 + n / 1250)); }
    if (k === "reboot") ui.say("Restarting...");
    if (k === "done") ui.progress(1);
  };
  if (st.loader) {
    ui.say(`The FM-1 is in update mode (${st.identity}): finishing the write of ${pkg.label}...`);
    // TransitFM1's loader answers ota-FM-1_900 writing a Felucca-family package and ota-FM-1_015 writing one with
    // M-VAVE's loader (firmware/src/ota.c OTA_ID_*): the official V15 is resumed as the return to stock; another
    // package of that kind (Baud Girl's) is finished by its own installer, which knows that update mode
    const ok = await up.resume(pkg.image, step, pkg.official ? { product: pkg.product } : { package: pkg.product });
    if (!ok) throw new Error("The write did not finish. Keep the FM-1 connected, switch it off and on if its screen stays black, and press Install again.");
    ui.say(`Done: ${pkg.label} is installed.`, "ok");
    return "resumed";
  }
  if (st.fw.kind === "bubba") { ui.say(describe(st), "bad"); return "refused"; }
  if (st.fw.verdict === "refuse" && !opts.override) { ui.say(describe(st), "bad"); return "refused"; }
  if (!ui.confirm(`Install ${pkg.label} over ${st.fw.kind === "stock" ? "the firmware that answers as V15" : st.fw.name}? ` +
                  "The FM-1 must be charged and connected directly to this computer; do not unplug it while it writes."))
    return "cancelled";
  await up.install(pkg.image, pkg.product, step);
  ui.say(`Done: ${pkg.label} is installed.`, "ok");
  return "done";
}

// ---- the page (the browser only) ----
if (typeof document !== "undefined") {
  const $ = (id) => document.getElementById(id);
  let access = null, rel = null, busy = false;
  const ui = {
    say(text, kind = "") { $("status").textContent = text; $("status").className = "status " + kind; },
    progress(f) { $("bar").hidden = false; $("bar").value = Math.round(f * 1000); },
    confirm: (t) => window.confirm(t),
    log(t) { $("log").textContent += t + "\n"; },
  };
  const midi = async () => {
    if (!navigator.requestMIDIAccess) throw new Error("This browser has no Web MIDI: use Chrome or Edge on a computer.");
    if (!access) access = await navigator.requestMIDIAccess({ sysex: true });
    return access;
  };
  const run = async (what) => {
    if (busy) return;
    busy = true;
    document.body.classList.add("busy");
    $("bar").hidden = true;
    try { await what(); } catch (e) {
      ui.say(e && e.name === "SecurityError" ? "The browser refused MIDI access: allow it (the lock icon in the address bar) and try again." : (e && e.message) || String(e), "bad");
      ui.log(String(e && e.stack || e));
    } finally {
      busy = false;
      document.body.classList.remove("busy");
    }
  };
  $("go").addEventListener("click", () => run(async () => {
    ui.say("Downloading and checking the package...");
    const pkg = await releasedPackage(rel);
    const r = await install(await midi(), pkg, ui, { override: $("override").checked });
    if (r === "refused" && !$("override").checked && !/Bubba/.test($("status").textContent)) $("override-box").hidden = false;
  }));
  $("check").addEventListener("click", () => run(async () => { ui.say("Looking for the FM-1..."); ui.say(describe(await look(await midi(), ui.log))); }));
  $("file").addEventListener("change", () => { $("file-go").disabled = !$("file").files.length; });
  $("file-go").addEventListener("click", () => run(async () => {
    const f = $("file").files[0];
    const pkg = await packageFrom(new Uint8Array(await f.arrayBuffer()), null);
    await install(await midi(), pkg, ui, { override: $("override").checked });
  }));
  fetch("releases.json", { cache: "no-cache" }).then((r) => r.json()).then((list) => {
    rel = list.releases[0];
    $("go").textContent = `Install TransitFM1 ${rel.version}`;
    $("go").disabled = false;
    const put = (id, f) => { const e = $(id); if (e) f(e); };   // (the page's text may change; installing must not)
    put("ver", (e) => { e.textContent = rel.version; });
    put("date", (e) => { e.textContent = rel.date; });
    put("notes", (e) => e.replaceChildren(...rel.notes.map((n) => Object.assign(document.createElement("li"), { textContent: n }))));
  }, (e) => {
    ui.say("The release list could not be loaded. Reload the page.", "bad");
    ui.log(String(e && e.stack || e));
  });
}
