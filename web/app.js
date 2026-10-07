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
  patches: [],
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
    container.append(makeKnob(name, title));
  });
}

// ---- Keyboard ------------------------------------------------------------

const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const BLACK = new Set([1, 3, 6, 8, 10]);
const heldByPointer = new Map(); // pointerId -> note

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

  // Computer keyboard: two rows like most DAWs (A = C, W = C#, ...).
  const keys = "awsedftgyhujkolp;'";
  const held = new Set();
  window.addEventListener("keydown", (event) => {
    if (event.repeat || event.metaKey || event.ctrlKey || event.target.matches("input, select")) return;
    const index = keys.indexOf(event.key.toLowerCase());
    if (index < 0 || held.has(index)) return;
    held.add(index);
    const note = state.octave * 12 + index;
    document.querySelector(`.key[data-note="${note}"]`)?.classList.add("down");
    send({ type: "note", note, velocity: 0.8, on: true });
  });
  window.addEventListener("keyup", (event) => {
    const index = keys.indexOf(event.key.toLowerCase());
    if (index < 0 || !held.has(index)) return;
    held.delete(index);
    const note = state.octave * 12 + index;
    document.querySelector(`.key[data-note="${note}"]`)?.classList.remove("down");
    send({ type: "note", note, on: false });
  });

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

function renderEditPage() {
  if (!state.page || !state.layout.pages.some((p) => p.id === state.page)) state.page = state.layout.pages[0].id;
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

  // Controls
  const prefixes = [];
  const sample = resolve(page.params[0], instance);
  const firstLabel = labelFor(sample);
  const match = firstLabel.match(/^(Oscillator \d|Filter \w+|Envelope \d|LFO \d|Random LFO \d|Random \d)/);
  if (match) prefixes.push(match[1]);
  prefixes.push(page.title);

  controls.replaceChildren();
  for (const template of page.params) {
    const name = resolve(template, instance);
    if (!state.info[name]) continue;
    controls.append(makeControl(name, shortLabel(name, prefixes)));
  }
}

function makeControl(name, label) {
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
    head.addEventListener("dblclick", () => setParam(name, info.default));
    el.append(slider);
    update = (value) => {
      if (!state.dragging.has(name) || document.activeElement !== slider) slider.value = value;
      const t = (value - info.min) / (info.max - info.min || 1);
      slider.style.setProperty("--fill", `${(t * 100).toFixed(1)}%`);
      valueEl.textContent = displayValue(name, value);
    };
  }

  update.owner = "page";
  onParam(name, update);
  update(state.values[name] ?? info.default);
  return el;
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

async function main() {
  for (const tab of document.querySelectorAll(".tabs button")) {
    tab.addEventListener("click", () => showView(tab.dataset.view));
  }
  $("patch-title").addEventListener("click", () => showView("patches"));
  $("prev-patch").addEventListener("click", () => stepPatch(-1));
  $("next-patch").addEventListener("click", () => stepPatch(1));
  $("patch-search").addEventListener("input", renderPatchList);

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
