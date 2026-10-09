// sloppy-synth web UI. Talks to the synth over the WebSocket at /ws using the
// JSON protocol in docs/PROTOCOL.md. No build step: plain ES modules.
//
// This file is part of sloppy-synth, a fork of Vital by Matt Tytel.
// Licensed under the GNU General Public License v3 or later, see LICENSE.

const $ = (id) => document.getElementById(id);

const state = {
  values: {},          // parameter name -> current value
  info: {},            // parameter name -> { label, min, max, scale, options, ... }
  patch: { index: -1, name: "", author: "", style: "" },
  macroNames: [],
  macroMidi: { assignments: [], defaults: [], learning: 0 }, // CC per macro; learning is 1-8, or 0
  patches: [],
  modInfo: { sources: [], destinations: [] },
  modulations: [],     // { slot, source, destination }
  vitalWarnings: [],   // what Vital would leave out of this patch
  layout: null,
  view: "play",
  page: null,
  instance: {},        // page id -> chosen instance
  octave: 4,           // lowest key on the keyboard is C(octave - 1) in Vital's naming
  dragging: new Set(), // parameters the user is moving right now; ignore echoes
};

const listeners = new Map(); // parameter name -> Set of update callbacks

function onParam(name, callback) {
  if (!listeners.has(name)) listeners.set(name, new Set());
  listeners.get(name).add(callback);
}

function clearParamListeners(owner) {
  for (const set of listeners.values()) {
    for (const callback of [...set]) {
      if (callback.owner === owner) set.delete(callback);
    }
  }
}

function notifyParam(name) {
  const set = listeners.get(name);
  if (set) for (const callback of set) callback(state.values[name]);
}

// ---- Connection -----------------------------------------------------------

let socket = null;
let reconnectDelay = 500;
const pendingSets = new Map();
let sendScheduled = false;

function connect() {
  const scheme = location.protocol === "https:" ? "wss" : "ws";
  socket = new WebSocket(`${scheme}://${location.host}/ws`);

  socket.addEventListener("open", () => {
    reconnectDelay = 500;
    setConnected(true);
    send({ type: "get_param_info" });
    send({ type: "hello" });
    send({ type: "list_patches" });
    send({ type: "get_mod_info" });
    send({ type: "get_modulations" });
    send({ type: "get_macro_midi" });
  });

  socket.addEventListener("message", (event) => {
    let message;
    try { message = JSON.parse(event.data); } catch { return; }
    handleMessage(message);
  });

  socket.addEventListener("close", () => {
    setConnected(false);
    setTimeout(connect, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 2, 5000);
  });
}

function send(message) {
  if (socket && socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify(message));
}

// Parameter moves are coalesced to one message per parameter per frame.
function setParam(name, value) {
  state.values[name] = value;
  pendingSets.set(name, value);
  notifyParam(name);
  if (!sendScheduled) {
    sendScheduled = true;
    requestAnimationFrame(() => {
      sendScheduled = false;
      for (const [n, v] of pendingSets) send({ type: "set", name: n, value: v });
      pendingSets.clear();
    });
  }
}

function setConnected(connected) {
  const status = $("status");
  status.classList.toggle("connected", connected);
  status.title = connected ? "Connected" : "Disconnected, retrying";
}

function handleMessage(message) {
  switch (message.type) {
    case "param_info":
      state.info = {};
      for (const param of message.params) state.info[param.name] = param;
      render();
      break;
    case "state":
      state.values = message.values;
      state.patch = message.patch;
      state.macroNames = message.macros || [];
      updatePatchHeader();
      render();
      if (state.view === "patches") revealCurrentPatch();
      highlightCurrentPatch(state.view === "patches");
      break;
    case "params":
      for (const [name, value] of Object.entries(message.values)) {
        if (state.dragging.has(name) || pendingSets.has(name)) continue;
        state.values[name] = value;
        notifyParam(name);
      }
      updateChipDots();
      break;
    case "patches":
      state.patches = message.patches;
      patchTree = null;
      if (message.current >= 0) state.patch.index = message.current;
      renderPatchList();
      break;
    case "mod_info":
      state.modInfo = { sources: message.sources, destinations: message.destinations };
      if (state.view === "edit") renderEditPage({ keepScroll: true });
      break;
    case "modulations":
      state.modulations = message.modulations;
      state.vitalWarnings = message.vital_warnings || [];
      updateVitalWarning();
      if (state.view === "edit") renderEditPage({ keepScroll: true });
      break;
    case "macro_midi":
      updateMacroMidi(message);
      break;
    case "error":
      toast(message.message);
      break;
  }
}

// ---- Value display (mirrors Vital's SynthSlider::getAdjustedValue) -------

const MIDI_0_FREQUENCY = 8.1757989156;
const SCALE = { indexed: 0, linear: 1, quadratic: 2, cubic: 3, quartic: 4, squareRoot: 5, exponential: 6 };

function displayValue(name, value) {
  const info = state.info[name];
  if (!info) return String(value);
  if (info.options) {
    const index = Math.round(value - info.min);
    return info.options[index] ?? String(Math.round(value));
  }
  // Frequencies Vital stores as MIDI notes (cutoffs, centers): show Hz, as
  // Vital's own "frequency display" does.
  if (info.units === " semitones" && info.offset === -60 && info.scale === SCALE.linear) {
    const hz = MIDI_0_FREQUENCY * Math.pow(2, value / 12);
    return hz >= 1000 ? `${(hz / 1000).toFixed(2)} kHz` : `${hz.toFixed(hz >= 100 ? 0 : 1)} Hz`;
  }
  let v = value;
  switch (info.scale) {
    case SCALE.quadratic: v = v * v; break;
    case SCALE.cubic: v = v * v * v; break;
    case SCALE.quartic: v = v * v * v * v; break;
    case SCALE.exponential: v = Math.pow(2, v); break;
    case SCALE.squareRoot: v = Math.sqrt(Math.max(v, 0)); break;
  }
  v += info.offset || 0;
  if (info.invert) v = 1 / v;
  v *= info.multiply || 1;
  if (info.scale === SCALE.indexed) return `${Math.round(v)}${info.units || ""}`;
  return `${formatNumber(v)}${info.units || ""}`;
}

function formatNumber(v) {
  if (!isFinite(v)) return "∞";
  if (Math.abs(v) >= 1000) return v.toFixed(0);
  // Three significant digits, without trailing zeros: 0.5, 12.3, 440
  const text = Number(v.toPrecision(3)).toString();
  return text === "-0" ? "0" : text;
}

function labelFor(name) {
  return state.info[name]?.label || name;
}

// Page labels drop the "Oscillator 1" style prefix the page already shows.
function shortLabel(name, prefixes) {
  let label = labelFor(name);
  for (const prefix of prefixes) {
    if (label.startsWith(prefix + " ")) return label.slice(prefix.length + 1);
  }
  return label;
}

// ---- Views ----------------------------------------------------------------

function showView(view) {
  state.view = view;
  for (const tab of document.querySelectorAll(".tabs button")) {
    tab.setAttribute("aria-selected", String(tab.dataset.view === view));
  }
  for (const section of document.querySelectorAll(".view")) {
    section.hidden = section.dataset.view !== view;
  }
  if (view === "patches") {
    revealCurrentPatch();
    highlightCurrentPatch(true);
  }
  try { localStorage.setItem("sloppy.view", view); } catch {}
}

function render() {
  if (!Object.keys(state.info).length || !state.layout) return;
  renderMacros();
  renderEditPage();
}

function updatePatchHeader() {
  $("patch-name").textContent = state.patch.name || "Init";
  const meta = [state.patch.style, state.patch.author].filter(Boolean).join(" · ");
  $("patch-meta").textContent = meta;
  document.title = `${state.patch.name || "Init"} · sloppy-synth`;
  updatePatchWarnings();
}

// Patches from newer Vital releases can use things this engine doesn't have.
// The "!" button lists them; a new patch with warnings also gets a toast.
let warnedPatchKey = "";
function updatePatchWarnings() {
  const warnings = state.patch.warnings || [];
  $("patch-warning").hidden = warnings.length === 0;
  const list = $("warnings-list");
  list.replaceChildren(...warnings.map((text) => {
    const item = document.createElement("li");
    item.textContent = text;
    return item;
  }));
  if (warnings.length === 0) {
    if ($("warnings").open) $("warnings").close();
    warnedPatchKey = "";
    return;
  }
  const key = `${state.patch.index}:${state.patch.name}:${warnings.join("|")}`;
  if (key !== warnedPatchKey) {
    warnedPatchKey = key;
    toast("This patch uses things this engine doesn't have yet. Tap ! for details.");
  }
}

// ---- Knobs (macros) -------------------------------------------------------

const KNOB_START = 135;  // degrees, measured clockwise from 12 o'clock is -135..135
const KNOB_SWEEP = 270;

function arcPath(cx, cy, r, startDeg, endDeg) {
  const toXY = (deg) => {
    const rad = ((deg - 90) * Math.PI) / 180;
    return [cx + r * Math.cos(rad), cy + r * Math.sin(rad)];
  };
  const [x1, y1] = toXY(startDeg);
  const [x2, y2] = toXY(endDeg);
  const large = endDeg - startDeg > 180 ? 1 : 0;
  return `M ${x1} ${y1} A ${r} ${r} 0 ${large} 1 ${x2} ${y2}`;
}

// Mouse wheel and trackpad over a slider or knob change its value: one wheel
// notch moves 2% of the range, or 0.4% with Shift. A wheel that was already
// scrolling the page keeps scrolling it when the pointer crosses a control.
let lastPageScroll = 0;
window.addEventListener("scroll", () => { lastPageScroll = performance.now(); }, { capture: true, passive: true });

function wheelControl(el, name, info, indexed = false) {
  let pending = 0;
  el.addEventListener("wheel", (event) => {
    if (performance.now() - lastPageScroll < 400) return;
    // Shift+wheel arrives as deltaX in some browsers.
    const raw = event.deltaY || event.deltaX;
    const delta = event.deltaMode === 1 ? raw * 33 : event.deltaMode === 2 ? raw * 400 : raw;
    if (!delta) return;
    event.preventDefault();
    const range = info.max - info.min;
    const value = state.values[name] ?? info.default;
    if (indexed) {
      pending -= delta / 100;
      const steps = Math.trunc(pending);
      if (!steps) return;
      pending -= steps;
      setParam(name, clamp(Math.round(value) + steps, info.min, info.max));
    } else {
      const notch = range * (event.shiftKey ? 0.004 : 0.02);
      setParam(name, clamp(value - (delta / 100) * notch, info.min, info.max));
    }
  }, { passive: false });
}

function makeKnob(name, title) {
  const info = state.info[name];
  const el = document.createElement("div");
  el.className = "knob";
  el.setAttribute("role", "slider");
  el.setAttribute("tabindex", "0");
  el.setAttribute("aria-label", title);
  el.setAttribute("aria-valuemin", info.min);
  el.setAttribute("aria-valuemax", info.max);
  el.innerHTML = `
    <svg viewBox="0 0 100 100" aria-hidden="true">
      <path class="track" d="${arcPath(50, 50, 38, -KNOB_START, KNOB_SWEEP - KNOB_START)}" fill="none" stroke-width="9" stroke-linecap="round"/>
      <path class="fill" fill="none" stroke-width="9" stroke-linecap="round"/>
      <circle class="dot" r="5"/>
    </svg>
    <div class="knob-label"></div>
    <div class="knob-value"></div>`;
  el.querySelector(".knob-label").textContent = title;

  const fill = el.querySelector(".fill");
  const dot = el.querySelector(".dot");
  const valueText = el.querySelector(".knob-value");

  const update = (value) => {
    const t = (value - info.min) / (info.max - info.min || 1);
    const end = -KNOB_START + Math.max(t, 0.001) * KNOB_SWEEP;
    fill.setAttribute("d", arcPath(50, 50, 38, -KNOB_START, end));
    const rad = ((end - 90) * Math.PI) / 180;
    dot.setAttribute("cx", 50 + 24 * Math.cos(rad));
    dot.setAttribute("cy", 50 + 24 * Math.sin(rad));
    valueText.textContent = `${Math.round(t * 100)}%`;
    el.setAttribute("aria-valuenow", value);
  };
  update.owner = "macros";
  onParam(name, update);
  update(state.values[name] ?? info.default);

  // Vertical drag: a full knob turn is ~200px; hold Shift for fine control.
  let startY = 0;
  let startValue = 0;
  el.addEventListener("pointerdown", (event) => {
    el.setPointerCapture(event.pointerId);
    startY = event.clientY;
    startValue = state.values[name] ?? info.default;
    state.dragging.add(name);
  });
  el.addEventListener("pointermove", (event) => {
    if (!el.hasPointerCapture(event.pointerId)) return;
    const range = info.max - info.min;
    const pixels = event.shiftKey ? 800 : 200;
    const value = clamp(startValue + ((startY - event.clientY) / pixels) * range, info.min, info.max);
    setParam(name, value);
  });
  const end = () => state.dragging.delete(name);
  el.addEventListener("pointerup", end);
  el.addEventListener("pointercancel", end);
  el.addEventListener("dblclick", () => setParam(name, info.default));
  wheelControl(el, name, info);
  el.addEventListener("keydown", (event) => {
    const step = (info.max - info.min) / (event.shiftKey ? 100 : 20);
    const value = state.values[name] ?? info.default;
    if (event.key === "ArrowUp" || event.key === "ArrowRight") setParam(name, clamp(value + step, info.min, info.max));
    else if (event.key === "ArrowDown" || event.key === "ArrowLeft") setParam(name, clamp(value - step, info.min, info.max));
    else return;
    event.preventDefault();
  });
  return el;
}

function renderMacros() {
  const container = $("macros");
  clearParamListeners("macros");
  container.replaceChildren();
  state.layout.macros.forEach((name, i) => {
    if (!state.info[name]) return;
    const title = state.macroNames[i] || `Macro ${i + 1}`;
    const card = document.createElement("div");
    card.className = "macro";
    const chip = document.createElement("button");
    chip.className = "cc-chip";
    chip.dataset.macro = i + 1;
    chip.addEventListener("click", () => openMacroMidi(i + 1));
    card.append(makeKnob(name, title), chip);
    container.append(card);
  });
  updateCcChips();
}

// Macros 5-8 are sloppy-synth's own; Vital opens the patch without them.
function updateVitalWarning() {
  const el = $("vital-warning");
  const warnings = state.vitalWarnings;
  el.hidden = !warnings.length;
  el.textContent = warnings.length
    ? `Vital has only 4 macros. It still opens this patch, but without the routings from ${
      warnings.map((w) => w.match(/^Macro \d/)?.[0] || w).join(", ")}.`
    : "";
  el.title = warnings.join("\n");
}

// ---- Macro MIDI CCs -------------------------------------------------------
// Each macro follows one MIDI CC, on one channel or any. The assignments
// belong to the synth (the device), not to the patch.

// What the MIDI spec, or Vital, already uses a CC for.
const CC_NAMES = {
  0: "Bank select", 1: "Mod wheel", 2: "Breath", 4: "Foot", 5: "Portamento time", 7: "Volume",
  8: "Balance", 10: "Pan", 11: "Expression", 32: "Bank select LSB", 64: "Sustain pedal",
  65: "Portamento", 66: "Sostenuto", 67: "Soft pedal", 71: "Resonance", 72: "Release",
  73: "Attack", 74: "Brightness, MPE slide", 91: "Reverb send", 93: "Chorus send",
};
// CCs that do something in the synth when no macro takes them.
const CC_TAKEN_OVER = { 0: "patch bank changes", 1: "the mod wheel", 32: "patch folder changes",
  64: "the sustain pedal", 66: "the sostenuto pedal", 74: "MPE slide" };

function describeAssignment(a) {
  if (!a || a.cc < 0) return "No CC";
  return a.channel ? `CC ${a.cc} · Ch ${a.channel}` : `CC ${a.cc}`;
}

function updateMacroMidi(message) {
  const wasLearning = state.macroMidi.learning;
  state.macroMidi = message;
  updateCcChips();
  if (wasLearning && !message.learning) {
    const a = message.assignments[wasLearning - 1];
    if (a && a.cc >= 0) toast(`${macroTitle(wasLearning)} now follows ${describeAssignment(a)}`);
  }
  if ($("macro-midi").open) fillMacroMidiDialog();
}

function macroTitle(macro) {
  const name = state.macroNames[macro - 1];
  return name && !/^MACRO \d$/.test(name) ? name : `Macro ${macro}`;
}

function updateCcChips() {
  for (const chip of document.querySelectorAll(".cc-chip")) {
    const macro = Number(chip.dataset.macro);
    const learning = state.macroMidi.learning === macro;
    const a = state.macroMidi.assignments[macro - 1];
    chip.textContent = learning ? "Learning…" : a ? describeAssignment(a) : "MIDI";
    chip.classList.toggle("learning", learning);
    chip.classList.toggle("none", !learning && (!a || a.cc < 0));
    chip.title = `MIDI CC for ${macroTitle(macro)}`;
    chip.setAttribute("aria-label", `${macroTitle(macro)} MIDI: ${chip.textContent}`);
  }
}

let editingMacro = 0;

function setupMacroMidiDialog() {
  const cc = $("mm-cc");
  const none = document.createElement("option");
  none.value = "-1";
  none.textContent = "None";
  cc.append(none);
  for (let n = 0; n <= 119; n++) {
    const option = document.createElement("option");
    option.value = n;
    option.textContent = CC_NAMES[n] ? `${n} · ${CC_NAMES[n]}` : String(n);
    cc.append(option);
  }
  const channel = $("mm-channel");
  for (let n = 0; n <= 16; n++) {
    const option = document.createElement("option");
    option.value = n;
    option.textContent = n ? `Channel ${n}` : "Any channel";
    channel.append(option);
  }
  const change = () => send({ type: "set_macro_midi", macro: editingMacro,
    cc: Number(cc.value), channel: Number(channel.value) });
  cc.addEventListener("change", change);
  channel.addEventListener("change", change);
  $("mm-learn").addEventListener("click", () => {
    const learning = state.macroMidi.learning === editingMacro;
    send({ type: "learn_macro_midi", macro: learning ? 0 : editingMacro });
  });
  $("mm-reset").addEventListener("click", () => send({ type: "reset_macro_midi" }));
  $("mm-done").addEventListener("click", () => $("macro-midi").close());
  // Learning only makes sense while the dialog is open.
  $("macro-midi").addEventListener("close", () => {
    if (state.macroMidi.learning) send({ type: "learn_macro_midi", macro: 0 });
  });
  // A tap on the backdrop closes it.
  $("macro-midi").addEventListener("click", (event) => {
    if (event.target === $("macro-midi")) $("macro-midi").close();
  });
}

function openMacroMidi(macro) {
  editingMacro = macro;
  fillMacroMidiDialog();
  if (!$("macro-midi").open) $("macro-midi").showModal();
}

function fillMacroMidiDialog() {
  const macro = editingMacro;
  const a = state.macroMidi.assignments[macro - 1] || { cc: -1, channel: 0 };
  const learning = state.macroMidi.learning === macro;
  $("mm-title").textContent = `${macroTitle(macro)} MIDI`;
  $("mm-cc").value = String(a.cc);
  $("mm-channel").value = String(a.channel);
  const learn = $("mm-learn");
  learn.textContent = learning ? "Cancel learn" : "Learn";
  learn.setAttribute("aria-pressed", String(learning));

  const notes = [];
  if (learning) notes.push("Move a knob, fader or wheel on your controller.");
  if (CC_TAKEN_OVER[a.cc] !== undefined) notes.push(`This CC goes to the macro instead of ${CC_TAKEN_OVER[a.cc]}.`);
  const shared = state.macroMidi.assignments
    .map((other, i) => ({ other, i }))
    .filter(({ other, i }) => i !== macro - 1 && a.cc >= 0 && other.cc === a.cc &&
      (!other.channel || !a.channel || other.channel === a.channel))
    .map(({ i }) => macroTitle(i + 1));
  if (shared.length) notes.push(`Also moves ${shared.join(", ")}.`);
  const d = state.macroMidi.defaults[macro - 1];
  if (d) notes.push(`Default: ${describeAssignment(d)}. These settings belong to this synth, not the patch.`);
  $("mm-note").textContent = notes.join(" ");
}

// ---- Keyboard ------------------------------------------------------------

const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const BLACK = new Set([1, 3, 6, 8, 10]);
const heldByPointer = new Map(); // pointerId -> note

// The physical keys that play notes, from C upward, and the letters shown on
// the piano. Labels follow the user's layout where the browser can tell us.
const QWERTY_CODES = ["KeyA", "KeyW", "KeyS", "KeyE", "KeyD", "KeyF", "KeyT", "KeyG", "KeyY",
  "KeyH", "KeyU", "KeyJ", "KeyK", "KeyO", "KeyL", "KeyP", "Semicolon", "Quote"];
let qwertyLabels = QWERTY_CODES.map((code) =>
  code.startsWith("Key") ? code.slice(3) : code === "Semicolon" ? ";" : "'");

async function loadKeyLabels() {
  try {
    const map = await navigator.keyboard?.getLayoutMap?.();
    if (!map) return;
    qwertyLabels = QWERTY_CODES.map((code, i) => (map.get(code) || qwertyLabels[i]).toUpperCase());
    renderKeyboard();
  } catch { /* keep the QWERTY letters */ }
}

function keyboardOctaves() {
  const width = $("keyboard").clientWidth || window.innerWidth;
  return width >= 1100 ? 4 : width >= 700 ? 3 : 2;
}

function renderKeyboard() {
  const keyboard = $("keyboard");
  releaseAllKeys();
  keyboard.replaceChildren();
  const octaves = keyboardOctaves();
  const first = state.octave * 12;
  const whiteCount = octaves * 7 + 1;
  const whiteWidth = 100 / whiteCount;
  let whiteIndex = 0;

  for (let note = first; note <= first + octaves * 12; note++) {
    const pc = note % 12;
    const key = document.createElement("div");
    key.dataset.note = note;
    if (BLACK.has(pc)) {
      key.className = "key black";
      key.style.left = `${(whiteIndex - 0.32) * whiteWidth}%`;
      key.style.width = `${whiteWidth * 0.64}%`;
    } else {
      key.className = "key white";
      key.style.left = `${whiteIndex * whiteWidth}%`;
      key.style.width = `${whiteWidth}%`;
      if (pc === 0) {
        const label = document.createElement("span");
        label.className = "key-label";
        label.textContent = `C${Math.floor(note / 12) - 1}`;
        key.append(label);
      }
      whiteIndex++;
    }
    const index = note - first;
    if (index < QWERTY_CODES.length) {
      const letter = document.createElement("span");
      letter.className = "key-letter";
      letter.textContent = qwertyLabels[index];
      key.append(letter);
    }
    keyboard.append(key);
  }
  $("octave-label").textContent = `C${state.octave - 1}`;
}

function keyAt(x, y) {
  const el = document.elementFromPoint(x, y);
  return el && el.classList.contains("key") ? el : null;
}

// Pressing lower on a key plays louder, like a real key's travel.
function velocityFor(key, y) {
  const rect = key.getBoundingClientRect();
  return clamp(0.35 + 0.65 * ((y - rect.top) / rect.height), 0.05, 1);
}

function noteOn(key, y, pointerId) {
  const note = Number(key.dataset.note);
  heldByPointer.set(pointerId, note);
  key.classList.add("down");
  send({ type: "note", note, velocity: velocityFor(key, y), on: true });
}

function noteOff(pointerId) {
  const note = heldByPointer.get(pointerId);
  if (note === undefined) return;
  heldByPointer.delete(pointerId);
  if (![...heldByPointer.values()].includes(note)) {
    document.querySelector(`.key[data-note="${note}"]`)?.classList.remove("down");
    send({ type: "note", note, on: false });
  }
}

function releaseAllKeys() {
  for (const pointerId of [...heldByPointer.keys()]) noteOff(pointerId);
}

function setupKeyboard() {
  const keyboard = $("keyboard");
  keyboard.addEventListener("pointerdown", (event) => {
    const key = keyAt(event.clientX, event.clientY);
    if (!key) return;
    keyboard.setPointerCapture(event.pointerId);
    noteOn(key, event.clientY, event.pointerId);
  });
  // Sliding across keys plays a glissando.
  keyboard.addEventListener("pointermove", (event) => {
    if (!heldByPointer.has(event.pointerId)) return;
    const key = keyAt(event.clientX, event.clientY);
    if (!key || Number(key.dataset.note) === heldByPointer.get(event.pointerId)) return;
    noteOff(event.pointerId);
    noteOn(key, event.clientY, event.pointerId);
  });
  const up = (event) => noteOff(event.pointerId);
  keyboard.addEventListener("pointerup", up);
  keyboard.addEventListener("pointercancel", up);
  keyboard.addEventListener("contextmenu", (event) => event.preventDefault());

  $("octave-down").addEventListener("click", () => { state.octave = Math.max(0, state.octave - 1); renderKeyboard(); });
  $("octave-up").addEventListener("click", () => { state.octave = Math.min(8, state.octave + 1); renderKeyboard(); });
  $("panic").addEventListener("click", () => { releaseAllKeys(); send({ type: "all_notes_off" }); });

  // Computer keyboard: two rows like most DAWs (A = C, W = C#, ...), Z and X
  // for octaves. Keys are matched by position (event.code), so the layout
  // still works on AZERTY or Dvorak keyboards.
  const held = new Map(); // event.code -> note
  window.addEventListener("keydown", (event) => {
    if (event.repeat || event.metaKey || event.ctrlKey || event.altKey) return;
    if (event.target.matches("input, select, textarea")) return;
    if ($("macro-midi").open) return;
    if (event.code === "KeyZ" || event.code === "KeyX") {
      $(event.code === "KeyZ" ? "octave-down" : "octave-up").click();
      return;
    }
    const index = QWERTY_CODES.indexOf(event.code);
    if (index < 0 || held.has(event.code)) return;
    const note = state.octave * 12 + index;
    if (note > 127) return;
    held.set(event.code, note);
    document.querySelector(`.key[data-note="${note}"]`)?.classList.add("down");
    send({ type: "note", note, velocity: 0.8, on: true });
  });
  const release = (code) => {
    const note = held.get(code);
    if (note === undefined) return;
    held.delete(code);
    if (![...held.values()].includes(note) && ![...heldByPointer.values()].includes(note)) {
      document.querySelector(`.key[data-note="${note}"]`)?.classList.remove("down");
      send({ type: "note", note, on: false });
    }
  };
  window.addEventListener("keyup", (event) => release(event.code));
  window.addEventListener("blur", () => { for (const code of [...held.keys()]) release(code); });
  loadKeyLabels();

  let lastOctaves = 0;
  const resize = () => {
    if (keyboardOctaves() !== lastOctaves) {
      lastOctaves = keyboardOctaves();
      renderKeyboard();
    }
  };
  new ResizeObserver(resize).observe(keyboard);
  window.addEventListener("blur", releaseAllKeys);
  renderKeyboard();
}

// ---- Patches -------------------------------------------------------------

// The library is shown as a tree: bank, then folders, then patches. Only open
// folders are rendered, so big libraries stay quick. Searching switches to a
// flat list of matches with their folder path.

const SEP = "\u0000";
const MAX_SEARCH_RESULTS = 300;
let patchTree = null;
let openFolders = loadOpenFolders();

function loadOpenFolders() {
  try { return new Set(JSON.parse(localStorage.getItem("sloppy.openFolders")) || []); }
  catch { return new Set(); }
}

function saveOpenFolders() {
  try { localStorage.setItem("sloppy.openFolders", JSON.stringify([...openFolders])); } catch {}
}

function patchPath(patch) {
  return [patch.bank || "Patches", ...(patch.folders || [])];
}

function buildPatchTree() {
  const root = { name: "", key: "", folders: new Map(), patches: [], count: 0 };
  for (const patch of state.patches) {
    let node = root;
    node.count++;
    for (const name of patchPath(patch)) {
      if (!node.folders.has(name)) {
        const key = node.key ? node.key + SEP + name : name;
        node.folders.set(name, { name, key, folders: new Map(), patches: [], count: 0 });
      }
      node = node.folders.get(name);
      node.count++;
    }
    node.patches.push(patch);
  }
  return root;
}

function renderPatchList() {
  const list = $("patch-list");
  const query = $("patch-search").value.trim().toLowerCase();
  list.replaceChildren();

  if (!state.patches.length) {
    list.append(emptyNote("No patches in the library yet. Import a bank with sloppy-synth --import-bank."));
    return;
  }
  if (query) {
    renderSearchResults(list, query);
  }
  else {
    patchTree = patchTree || buildPatchTree();
    const tree = document.createElement("div");
    tree.className = "patch-tree";
    tree.setAttribute("role", "tree");
    // A library with a single bank opens straight into it.
    const top = [...patchTree.folders.values()];
    if (top.length === 1) openFolders.add(top[0].key);
    renderFolderChildren(tree, patchTree, 0);
    list.append(tree);
  }
  highlightCurrentPatch();
}

function renderFolderChildren(container, node, depth) {
  for (const folder of node.folders.values()) {
    const open = openFolders.has(folder.key);
    const row = document.createElement("button");
    row.className = "patch-folder";
    row.style.setProperty("--depth", depth);
    row.setAttribute("role", "treeitem");
    row.setAttribute("aria-expanded", String(open));
    const name = document.createElement("span");
    name.className = "name";
    name.textContent = folder.name;
    const count = document.createElement("span");
    count.className = "count";
    count.textContent = folder.count;
    row.append(name, count);
    row.addEventListener("click", () => toggleFolder(folder, row, depth));
    container.append(row);

    const children = document.createElement("div");
    children.className = "patch-children";
    children.setAttribute("role", "group");
    if (open) renderFolderChildren(children, folder, depth + 1);
    container.append(children);
  }
  for (const patch of node.patches) container.append(patchRow(patch, depth));
}

function toggleFolder(folder, row, depth) {
  const children = row.nextElementSibling;
  if (openFolders.has(folder.key)) {
    openFolders.delete(folder.key);
    children.replaceChildren();
  }
  else {
    openFolders.add(folder.key);
    renderFolderChildren(children, folder, depth + 1);
    highlightCurrentPatch();
  }
  row.setAttribute("aria-expanded", String(openFolders.has(folder.key)));
  saveOpenFolders();
}

function patchRow(patch, depth, showPath = false) {
  const item = document.createElement("button");
  item.className = "patch-item";
  item.style.setProperty("--depth", depth);
  item.setAttribute("role", "treeitem");
  item.dataset.index = patch.index;
  const name = document.createElement("span");
  name.className = "name";
  name.textContent = patch.name;
  item.append(name);
  if (showPath) {
    const path = document.createElement("span");
    path.className = "path";
    path.textContent = patchPath(patch).join(" / ");
    item.append(path);
  }
  item.addEventListener("click", () => loadPatch(patch.index));
  return item;
}

function renderSearchResults(list, query) {
  const words = query.split(/\s+/);
  const matches = state.patches.filter((p) => {
    const text = `${p.name} ${patchPath(p).join(" ")}`.toLowerCase();
    return words.every((word) => text.includes(word));
  });
  if (!matches.length) {
    list.append(emptyNote("No patches match."));
    return;
  }
  for (const patch of matches.slice(0, MAX_SEARCH_RESULTS)) list.append(patchRow(patch, 0, true));
  if (matches.length > MAX_SEARCH_RESULTS) {
    list.append(emptyNote(`Showing ${MAX_SEARCH_RESULTS} of ${matches.length} matches. Type more to narrow it down.`));
  }
}

function emptyNote(text) {
  const note = document.createElement("div");
  note.className = "empty";
  note.textContent = text;
  return note;
}

// Opens the folders down to the current patch, so it can be seen.
function revealCurrentPatch() {
  const patch = state.patches[state.patch.index];
  if (!patch || $("patch-search").value.trim()) return;
  let key = "";
  let changed = false;
  for (const name of patchPath(patch)) {
    key = key ? key + SEP + name : name;
    if (!openFolders.has(key)) {
      openFolders.add(key);
      changed = true;
    }
  }
  if (changed) {
    saveOpenFolders();
    renderPatchList();
  }
}

function highlightCurrentPatch(scroll = false) {
  for (const item of document.querySelectorAll(".patch-item")) {
    const current = Number(item.dataset.index) === state.patch.index;
    item.classList.toggle("current", current);
    if (current && scroll) item.scrollIntoView({ block: "center" });
  }
}

function loadPatch(index) {
  if (index < 0 || index >= state.patches.length) return;
  releaseAllKeys();
  send({ type: "load_patch", index });
}

function stepPatch(delta) {
  if (!state.patches.length) {
    toast("No patches in the library yet.");
    return;
  }
  const current = state.patch.index;
  const next = current < 0
    ? (delta > 0 ? 0 : state.patches.length - 1)
    : (current + delta + state.patches.length) % state.patches.length;
  loadPatch(next);
}

// ---- Edit pages ----------------------------------------------------------

function pageInstances(page) {
  return page.instances || [null];
}

function resolve(template, instance) {
  return instance === null ? template : template.replaceAll("{n}", String(instance));
}

function isPageActive(page, instance) {
  if (!page.switch) return true;
  return (state.values[resolve(page.switch, instance)] ?? 0) >= 0.5;
}

function renderPageChips() {
  const chips = $("page-chips");
  chips.replaceChildren();
  for (const page of state.layout.pages) {
    const chip = document.createElement("button");
    chip.className = "chip";
    chip.setAttribute("role", "tab");
    chip.dataset.page = page.id;
    chip.textContent = page.title;
    chip.setAttribute("aria-selected", String(page.id === state.page));
    chip.addEventListener("click", () => {
      state.page = page.id;
      try { localStorage.setItem("sloppy.page", page.id); } catch {}
      renderEditPage();
    });
    chips.append(chip);
  }
  updateChipDots();
}

// A dot on a chip means at least one instance of that page is switched on.
function updateChipDots() {
  if (!state.layout) return;
  for (const chip of document.querySelectorAll(".chip")) {
    const page = state.layout.pages.find((p) => p.id === chip.dataset.page);
    const active = page.switch && pageInstances(page).some((i) => isPageActive(page, i));
    chip.classList.toggle("active-dot", Boolean(active));
  }
}

function renderEditPage({ keepScroll = false } = {}) {
  if (!state.layout || !Object.keys(state.info).length) return;
  if (!state.page || !state.layout.pages.some((p) => p.id === state.page)) state.page = state.layout.pages[0].id;
  const scrollTop = keepScroll ? $("controls").scrollTop : 0;
  renderPageChips();
  const page = state.layout.pages.find((p) => p.id === state.page);
  const instances = pageInstances(page);
  let instance = state.instance[page.id] ?? instances[0];
  if (!instances.includes(instance)) instance = instances[0];

  // Instance selector (Osc 1/2/3, Env 1..6, ...)
  const instanceBar = $("instances");
  instanceBar.replaceChildren();
  if (instances.length > 1) {
    instances.forEach((value, i) => {
      const button = document.createElement("button");
      button.className = "instance";
      button.textContent = page.instanceLabels?.[i] ?? String(value);
      button.setAttribute("aria-selected", String(value === instance));
      button.setAttribute("aria-label", `${page.title} ${button.textContent}`);
      button.addEventListener("click", () => {
        state.instance[page.id] = value;
        renderEditPage();
      });
      instanceBar.append(button);
    });
  } else {
    const title = document.createElement("strong");
    title.textContent = page.title;
    instanceBar.append(title);
  }

  // On/off switch for the page
  clearParamListeners("page");
  const switchLabel = $("page-switch");
  const switchInput = $("page-switch-input");
  const controls = $("controls");
  if (page.switch && state.info[resolve(page.switch, instance)]) {
    const name = resolve(page.switch, instance);
    switchLabel.hidden = false;
    const update = (value) => {
      switchInput.checked = value >= 0.5;
      controls.classList.toggle("disabled", value < 0.5);
      updateChipDots();
    };
    update.owner = "page";
    onParam(name, update);
    update(state.values[name] ?? 0);
    switchInput.onchange = () => setParam(name, switchInput.checked ? 1 : 0);
  } else {
    switchLabel.hidden = true;
    controls.classList.remove("disabled");
  }

  if (page.kind === "modulation") {
    renderModulationPage(controls);
    controls.scrollTop = scrollTop;
    return;
  }

  // Controls
  const prefixes = [];
  const sample = resolve(page.params[0], instance);
  const firstLabel = labelFor(sample);
  const match = firstLabel.match(/^(Oscillator \d|Filter \w+|Envelope \d|LFO \d|Random LFO \d|Random \d)/);
  if (match) prefixes.push(match[1]);
  prefixes.push(page.title);

  controls.replaceChildren();
  if (page.source) controls.append(makeRoutesCard(resolve(page.source, instance)));
  for (const template of page.params) {
    const name = resolve(template, instance);
    if (!state.info[name]) continue;
    controls.append(makeControl(name, shortLabel(name, prefixes)));
  }
  controls.scrollTop = scrollTop;
}

function makeControl(name, label, format = null) {
  const info = state.info[name];
  const el = document.createElement("div");
  el.className = "control";
  const head = document.createElement("div");
  head.className = "control-head";
  const labelEl = document.createElement("span");
  labelEl.className = "control-label";
  labelEl.textContent = label;
  const valueEl = document.createElement("span");
  valueEl.className = "control-value";
  head.append(labelEl, valueEl);
  el.append(head);
  const modulatedBy = state.modulations.filter((m) => m.destination === name);
  if (modulatedBy.length) {
    const badge = document.createElement("div");
    badge.className = "mod-badge";
    badge.textContent = "Modulated by " + modulatedBy.map((m) => sourceLabel(m.source)).join(", ");
    el.append(badge);
  }

  let update;
  if (info.options && info.options.length <= 4) {
    // Few choices: buttons
    const group = document.createElement("div");
    group.className = "segmented";
    info.options.forEach((option, i) => {
      const button = document.createElement("button");
      button.textContent = option;
      button.addEventListener("click", () => setParam(name, info.min + i));
      group.append(button);
    });
    el.append(group);
    update = (value) => {
      const index = Math.round(value - info.min);
      [...group.children].forEach((b, i) => b.setAttribute("aria-pressed", String(i === index)));
      valueEl.textContent = "";
    };
  } else if (info.options) {
    const select = document.createElement("select");
    select.setAttribute("aria-label", label);
    info.options.forEach((option, i) => {
      const opt = document.createElement("option");
      opt.value = String(info.min + i);
      opt.textContent = option;
      select.append(opt);
    });
    select.addEventListener("change", () => setParam(name, Number(select.value)));
    el.append(select);
    update = (value) => {
      select.value = String(Math.round(value));
      valueEl.textContent = "";
    };
  } else {
    const slider = document.createElement("input");
    slider.type = "range";
    slider.min = info.min;
    slider.max = info.max;
    slider.step = info.scale === SCALE.indexed ? 1 : (info.max - info.min) / 1000;
    slider.setAttribute("aria-label", label);
    slider.addEventListener("pointerdown", () => state.dragging.add(name));
    const release = () => state.dragging.delete(name);
    slider.addEventListener("pointerup", release);
    slider.addEventListener("pointercancel", release);
    slider.addEventListener("input", () => setParam(name, Number(slider.value)));
    wheelControl(slider, name, info, info.scale === SCALE.indexed);
    head.addEventListener("dblclick", () => setParam(name, info.default));
    el.append(slider);
    update = (value) => {
      if (!state.dragging.has(name) || document.activeElement !== slider) slider.value = value;
      // Ranges that cross zero (like modulation amounts) fill from the middle.
      const span = info.max - info.min || 1;
      const t = (value - info.min) / span;
      const zero = info.min < 0 && info.max > 0 ? -info.min / span : 0;
      slider.style.setProperty("--from", `${(Math.min(t, zero) * 100).toFixed(1)}%`);
      slider.style.setProperty("--fill", `${(Math.max(t, zero) * 100).toFixed(1)}%`);
      valueEl.textContent = format ? format(value) : displayValue(name, value);
    };
  }

  update.owner = "page";
  onParam(name, update);
  update(state.values[name] ?? info.default);
  return el;
}

// ---- Modulation ------------------------------------------------------------

const SOURCE_LABELS = {
  aftertouch: "Aftertouch", lift: "Lift (release velocity)", mod_wheel: "Mod Wheel", note: "Note (pitch)",
  note_in_octave: "Note in Octave", pitch_wheel: "Pitch Wheel", random: "Note Random", slide: "Slide",
  stereo: "Stereo", velocity: "Velocity",
};

function sourceLabel(source) {
  let match = source.match(/^macro_control_(\d)$/);
  if (match) return state.macroNames[match[1] - 1] || `Macro ${match[1]}`;
  match = source.match(/^(env|lfo|random)_(\d)$/);
  if (match) return `${{ env: "Envelope", lfo: "LFO", random: "Random" }[match[1]]} ${match[2]}`;
  return SOURCE_LABELS[source] || source;
}

function sourceGroup(source) {
  if (/^env_/.test(source)) return "Envelopes";
  if (/^lfo_/.test(source)) return "LFOs";
  if (/^random_/.test(source)) return "Random LFOs";
  if (/^macro_/.test(source)) return "Macros";
  return "Performance";
}

// Destination groups follow the edit pages: "Oscillator 1", "Filter FX", ...
function destinationGroup(name) {
  const label = labelFor(name);
  const match = label.match(/^(Oscillator \d|Filter \w+|Envelope \d|LFO \d|Random LFO \d|Random \d|Sample|Chorus|Delay|Reverb|Distortion|Phaser|Flanger|Compressor|EQ|Macro)/);
  return match ? match[1] : "Voice and global";
}

function percent(value) {
  return `${Math.round(value * 100)}%`;
}

// Makes a <select> with <optgroup>s from [{ value, label, group }].
function groupedSelect(items, placeholder) {
  const select = document.createElement("select");
  const empty = document.createElement("option");
  empty.value = "";
  empty.textContent = placeholder;
  select.append(empty);
  const groups = new Map();
  for (const item of items) {
    if (!groups.has(item.group)) {
      const group = document.createElement("optgroup");
      group.label = item.group;
      groups.set(item.group, group);
      select.append(group);
    }
    const option = document.createElement("option");
    option.value = item.value;
    option.textContent = item.label;
    groups.get(item.group).append(option);
  }
  return select;
}

function sourceItems() {
  const order = ["Envelopes", "LFOs", "Random LFOs", "Macros", "Performance"];
  return state.modInfo.sources
    .map((name) => ({ value: name, label: sourceLabel(name), group: sourceGroup(name) }))
    .sort((a, b) => order.indexOf(a.group) - order.indexOf(b.group) || a.label.localeCompare(b.label, undefined, { numeric: true }));
}

function destinationItems() {
  return state.modInfo.destinations
    .filter((name) => !name.startsWith("modulation_") && state.info[name])
    .map((name) => ({ value: name, label: labelFor(name), group: destinationGroup(name) }))
    .sort((a, b) => a.group.localeCompare(b.group, undefined, { numeric: true }) ||
                    a.label.localeCompare(b.label, undefined, { numeric: true }));
}

function addModulation(source, destination) {
  if (!source || !destination) return;
  if (state.modulations.some((m) => m.source === source && m.destination === destination)) {
    toast(`${sourceLabel(source)} already modulates ${labelFor(destination)}.`);
    return;
  }
  send({ type: "add_modulation", source, destination, amount: 0.5 });
}

// One routing: what it connects, its depth, its options and a remove button.
// `show` picks which end to name: "both", or "destination" on a source's page.
function makeRouting(modulation, show = "both") {
  const card = document.createElement("div");
  card.className = "routing";
  const head = document.createElement("div");
  head.className = "routing-head";
  const title = document.createElement("span");
  title.className = "routing-title";
  title.textContent = show === "destination"
    ? labelFor(modulation.destination)
    : `${sourceLabel(modulation.source)} \u2192 ${labelFor(modulation.destination)}`;
  const remove = document.createElement("button");
  remove.className = "icon-button small";
  remove.setAttribute("aria-label", `Remove ${title.textContent}`);
  remove.textContent = "\u00d7";
  remove.addEventListener("click", () =>
    send({ type: "remove_modulation", source: modulation.source, destination: modulation.destination }));
  head.append(title, remove);
  card.append(head);

  const prefix = `modulation_${modulation.slot}_`;
  if (state.info[prefix + "amount"]) {
    const amount = makeControl(prefix + "amount", "Amount", percent);
    amount.classList.add("bare");
    card.append(amount);
  }
  const toggles = document.createElement("div");
  toggles.className = "segmented toggles";
  for (const [option, label] of [["bipolar", "Bipolar"], ["stereo", "Stereo"], ["bypass", "Bypass"]]) {
    const name = prefix + option;
    if (!state.info[name]) continue;
    const button = document.createElement("button");
    button.textContent = label;
    button.addEventListener("click", () => setParam(name, (state.values[name] ?? 0) >= 0.5 ? 0 : 1));
    const update = (value) => button.setAttribute("aria-pressed", String((value ?? 0) >= 0.5));
    update.owner = "page";
    onParam(name, update);
    update(state.values[name]);
    toggles.append(button);
  }
  card.append(toggles);
  return card;
}

// On an envelope, LFO or random page: where this source is routed.
function makeRoutesCard(source) {
  const card = document.createElement("div");
  card.className = "control routes";
  const head = document.createElement("div");
  head.className = "control-head";
  const label = document.createElement("span");
  label.className = "control-label";
  label.textContent = "Modulates";
  head.append(label);
  card.append(head);

  const routes = state.modulations.filter((m) => m.source === source);
  if (!routes.length) {
    const note = document.createElement("p");
    note.className = "routes-empty";
    note.textContent = `${sourceLabel(source)} isn't routed anywhere yet, so it has no effect on the sound.`;
    card.append(note);
  }
  if (routes.length) {
    const list = document.createElement("div");
    list.className = "routes-list";
    for (const modulation of routes) list.append(makeRouting(modulation, "destination"));
    card.append(list);
  }

  const add = document.createElement("div");
  add.className = "routes-add";
  const destination = groupedSelect(destinationItems(), "Add a destination\u2026");
  destination.setAttribute("aria-label", `Route ${sourceLabel(source)} to`);
  destination.addEventListener("change", () => addModulation(source, destination.value));
  add.append(destination);
  card.append(add);
  return card;
}

// The Mod page: every routing, and a form to add one.
function renderModulationPage(container) {
  container.replaceChildren();

  const form = document.createElement("div");
  form.className = "control routes";
  const head = document.createElement("div");
  head.className = "control-head";
  const label = document.createElement("span");
  label.className = "control-label";
  label.textContent = "Add a routing";
  head.append(label);
  const source = groupedSelect(sourceItems(), "Source\u2026");
  source.setAttribute("aria-label", "Modulation source");
  const destination = groupedSelect(destinationItems(), "Destination\u2026");
  destination.setAttribute("aria-label", "Modulation destination");
  const add = document.createElement("button");
  add.className = "pill";
  add.textContent = "Add";
  add.addEventListener("click", () => {
    if (!source.value || !destination.value) {
      toast("Pick a source and a destination.");
      return;
    }
    addModulation(source.value, destination.value);
  });
  const row = document.createElement("div");
  row.className = "routes-add";
  row.append(source, destination, add);
  form.append(head, row);
  container.append(form);

  const count = document.createElement("p");
  count.className = "routes-count";
  count.textContent = state.modulations.length
    ? `${state.modulations.length} of 64 routings in use`
    : "This patch has no modulation routings.";
  container.append(count);

  const sorted = [...state.modulations].sort((a, b) =>
    sourceLabel(a.source).localeCompare(sourceLabel(b.source), undefined, { numeric: true }) ||
    labelFor(a.destination).localeCompare(labelFor(b.destination), undefined, { numeric: true }));
  for (const modulation of sorted) {
    const card = makeRouting(modulation);
    card.classList.add("control");
    container.append(card);
  }
}

// ---- Misc ----------------------------------------------------------------

function clamp(v, lo, hi) {
  return Math.min(hi, Math.max(lo, v));
}

let toastTimer = 0;
function toast(text) {
  const el = $("toast");
  el.textContent = text;
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 3500);
}

// Inside the Android app, the page can ask the app to import patches and
// banks through a file picker. The app reports back through sloppyImported.
function setupAndroid() {
  const android = window.SloppyAndroid;
  if (!android) return;
  const button = $("import-patches");
  button.hidden = false;
  button.addEventListener("click", () => android.importFile());
  if (android.quit) {
    const quit = $("quit-app");
    quit.hidden = false;
    quit.addEventListener("click", () => android.quit());
  }
  window.sloppyImported = (message, ok) => {
    toast(message);
    if (ok) send({ type: "list_patches" });
  };
}

async function main() {
  for (const tab of document.querySelectorAll(".tabs button")) {
    tab.addEventListener("click", () => showView(tab.dataset.view));
  }
  $("patch-title").addEventListener("click", () => showView("patches"));
  $("prev-patch").addEventListener("click", () => stepPatch(-1));
  $("next-patch").addEventListener("click", () => stepPatch(1));
  $("patch-warning").addEventListener("click", () => $("warnings").showModal());
  $("patch-search").addEventListener("input", renderPatchList);
  setupAndroid();
  setupMacroMidiDialog();

  try {
    state.page = localStorage.getItem("sloppy.page");
    const view = localStorage.getItem("sloppy.view");
    if (view) showView(view);
  } catch {}

  setupKeyboard();

  const response = await fetch("layout.json");
  state.layout = await response.json();
  render();
  connect();
}

main();
