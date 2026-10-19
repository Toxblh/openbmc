// Serial consoles (host COM ports via reimu-serial): timestamped log with
// full history, search, interactive terminal (xterm.js), baud/break control.

import { h, toast, fmtTime } from "../api.js";
import { SerialClient, keyToBytes } from "../lib/serial-client.js";
import { LogView } from "../lib/lineview.js";

export const fill = true;

const BAUDS = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600];
const EOL = { CR: "\r", LF: "\n", CRLF: "\r\n" };

let root;
let ports = []; // {index, name, title, client, view, term, ...}
let active = null;
let prefs;

function loadPrefs() {
  const def = { mode: "log", tz: "local", eol: "CR", date: true, active: 0, fontSize: 14, bs: "bs" };
  try {
    return Object.assign(def, JSON.parse(localStorage.getItem("reimu-console") || "{}"));
  } catch (e) {
    return def;
  }
}
function savePrefs() {
  try {
    localStorage.setItem("reimu-console", JSON.stringify(prefs));
  } catch (e) {
    /* ignore */
  }
}

// Terminal font. Nothing is shipped in the firmware (a Nerd Font is 1-2.5 MB
// per face): the stack prefers a Nerd Font installed on the viewer's
// computer, and a font file the viewer picks is kept in this browser's
// IndexedDB, never on the BMC.
const USER_FONT = "REIMU user font";
const FONT_STACK =
  '"JetBrainsMono Nerd Font Mono", "JetBrainsMono NFM", "JetBrainsMono Nerd Font", "JetBrains Mono", ' +
  '"DejaVu Sans Mono", "Liberation Mono", Consolas, Menlo, monospace';
let userFontName = null; // file name of the loaded user font

function fontDb(mode, fn) {
  return new Promise((resolve, reject) => {
    let req;
    try {
      req = indexedDB.open("reimu-console", 1);
    } catch (e) {
      return reject(e);
    }
    req.onupgradeneeded = () => req.result.createObjectStore("fonts");
    req.onerror = () => reject(req.error);
    req.onsuccess = () => {
      const tx = req.result.transaction("fonts", mode);
      const r = fn(tx.objectStore("fonts"));
      tx.oncomplete = () => resolve(r && r.result);
      tx.onerror = () => reject(tx.error);
    };
  });
}

async function applyUserFont(name, data) {
  const face = new FontFace(USER_FONT, data);
  await face.load();
  document.fonts.add(face);
  userFontName = name;
}

async function loadSavedFont() {
  try {
    const f = await fontDb("readonly", (st) => st.get("user"));
    if (f && f.data) await applyUserFont(f.name, f.data);
  } catch (e) {
    /* no IndexedDB (private window) or a broken file: the stack is used */
  }
}

function termFont() {
  return userFontName ? `"${USER_FONT}", ${FONT_STACK}` : FONT_STACK;
}

// Firmware menus (ПНС/lintel, BIOS setup) erase only on ^H and ignore DEL,
// which is what xterm.js sends; readline accepts both.
const BS = () => (prefs.bs === "del" ? "\x7f" : "\b");

function markBackspace() {
  if (!root) return;
  root.querySelector("#con-bs-bs").textContent = `${prefs.bs !== "del" ? "✓ " : ""}Backspace = ^H (BIOS, ПНС, большинство shell)`;
  root.querySelector("#con-bs-del").textContent = `${prefs.bs === "del" ? "✓ " : ""}Backspace = ^? (DEL, как в xterm)`;
}

function applyFontToTerminals() {
  ports.forEach((p) => {
    if (!p.term) return;
    p.term.options.fontFamily = termFont();
    p.term.options.fontSize = prefs.fontSize;
    if (!p.termWrap.classList.contains("hidden")) p.fit.fit();
  });
  if (root) root.querySelector("#con-font").title = `Шрифт терминала: ${userFontName || "системный (JetBrains Mono Nerd Font, если установлен)"}`;
}

async function pickFont() {
  const input = h("input", { type: "file", accept: ".ttf,.otf,.woff,.woff2,font/*" });
  input.addEventListener("change", async () => {
    const file = input.files[0];
    if (!file) return;
    if (file.size > 16 * 1024 * 1024) return toast("Файл шрифта больше 16 МБ", "err");
    try {
      const data = await file.arrayBuffer();
      await applyUserFont(file.name, data);
      await fontDb("readwrite", (st) => st.put({ name: file.name, data }, "user")).catch(() =>
        toast("Шрифт применён, но браузер не дал его сохранить", "warn"),
      );
      applyFontToTerminals();
      toast(`Шрифт терминала: ${file.name}`, "ok");
    } catch (e) {
      toast(`Не удалось загрузить шрифт: ${e.message || e}`, "err");
    }
  });
  input.click();
}

async function resetFont() {
  userFontName = null;
  await fontDb("readwrite", (st) => st.delete("user")).catch(() => {});
  applyFontToTerminals();
  toast("Шрифт терминала: системный", "ok");
}

function tzValue(p) {
  if (prefs.tz === "bmc" && p.client.info) return p.client.info.tzOffsetMin;
  if (prefs.tz === "utc") return 0;
  return null;
}

export async function mount(el) {
  root = el;
  prefs = loadPrefs();
  el.append(
    h(
      "div",
      { class: "console-toolbar" },
      h("div", { class: "tabs", id: "con-tabs" }),
      h("span", { class: "spacer" }),
      h(
        "div",
        { class: "tabs", id: "con-mode" },
        h("button", { "data-mode": "log", title: "Лента строк с метками времени и поиском" }, "Лог"),
        h("button", { "data-mode": "term", title: "Интерактивный терминал (xterm)" }, "Терминал"),
      ),
      h("input", { id: "con-search", type: "search", placeholder: "Поиск…", size: 18 }),
      h("label", { class: "inline small" }, h("input", { type: "checkbox", id: "con-only" }), "только найденное"),
      h(
        "select",
        { id: "con-tz", title: "Часы, в которых показывается время строк" },
        h("option", { value: "local" }, "Время: браузер"),
        h("option", { value: "bmc" }, "Время: BMC"),
        h("option", { value: "utc" }, "Время: UTC"),
      ),
      h("label", { class: "inline small" }, h("input", { type: "checkbox", id: "con-date" }), "дата"),
      h("select", { id: "con-baud", title: "Скорость порта" }, BAUDS.map((b) => h("option", { value: b }, `${b}`))),
      h("button", { class: "btn small", id: "con-break", title: "Послать BREAK (SysRq на Linux)" }, "Break"),
      h("button", { class: "btn small", id: "con-download", title: "Скачать историю с метками времени" }, "Скачать"),
      h("button", { class: "btn small", id: "con-clear", title: "Очистить историю порта на BMC" }, "Очистить"),
      h(
        "div",
        { class: "dropdown", id: "con-fontmenu" },
        h("button", { class: "btn small", id: "con-font", title: "Шрифт терминала" }, "Терминал ▾"),
        h(
          "div",
          { class: "dropdown-menu right" },
          h("button", { "data-font": "pick" }, "Загрузить файл шрифта… (.ttf, .otf, .woff2)"),
          h("button", { "data-font": "reset" }, "Системный (JetBrains Mono Nerd Font, если установлен)"),
          h("button", { "data-font": "bigger" }, "Крупнее"),
          h("button", { "data-font": "smaller" }, "Мельче"),
          h("button", { "data-font": "bs", id: "con-bs-bs" }, "Backspace = ^H (BIOS, ПНС, большинство shell)"),
          h("button", { "data-font": "del", id: "con-bs-del" }, "Backspace = ^? (DEL, как в xterm)"),
        ),
      ),
    ),
    h("div", { class: "console-body", id: "con-body" }),
    h(
      "div",
      { class: "statusbar", id: "con-status" },
      h("span", {}, "Порт: ", h("b", { id: "st-port" }, "—")),
      h("span", {}, "Состояние: ", h("b", { id: "st-online" }, "—")),
      h("span", {}, "Строк: ", h("b", { id: "st-lines" }, "0")),
      h("span", {}, "Принято: ", h("b", { id: "st-rx" }, "0")),
      h("span", { class: "spacer" }),
      h("span", { id: "st-hint", class: "muted" }, ""),
    ),
  );

  const $ = (id) => el.querySelector("#" + id);
  $("con-tz").value = prefs.tz;
  $("con-date").checked = prefs.date;
  $("con-mode").addEventListener("click", (e) => {
    const b = e.target.closest("button[data-mode]");
    if (b) setMode(b.dataset.mode);
  });
  $("con-tz").addEventListener("change", (e) => {
    prefs.tz = e.target.value;
    savePrefs();
    ports.forEach((p) => p.view && p.view.setTz(tzValue(p)));
  });
  $("con-date").addEventListener("change", (e) => {
    prefs.date = e.target.checked;
    savePrefs();
    ports.forEach((p) => {
      if (p.view) {
        p.view.showDate = prefs.date;
        p.view.setTz(tzValue(p));
      }
    });
  });
  const applySearch = () => active && active.view.setFilter($("con-search").value, $("con-only").checked);
  $("con-search").addEventListener("input", applySearch);
  $("con-only").addEventListener("change", applySearch);
  $("con-baud").addEventListener("change", (e) => {
    if (active && confirm(`Сменить скорость порта «${active.title}» на ${e.target.value}?`)) active.client.setBaud(e.target.value);
    else if (active && active.client.info) e.target.value = active.client.info.baud;
  });
  $("con-break").addEventListener("click", () => active && active.client.sendBreak());
  $("con-clear").addEventListener("click", () => {
    if (active && confirm(`Очистить историю порта «${active.title}» на BMC?`)) active.client.clearHistory();
  });
  $("con-fontmenu").addEventListener("click", (e) => {
    const b = e.target.closest("button[data-font]");
    if (!b) return;
    const a = b.dataset.font;
    if (a === "pick") pickFont();
    else if (a === "reset") resetFont();
    else if (a === "bs" || a === "del") {
      prefs.bs = a;
      savePrefs();
      markBackspace();
    }
    else {
      prefs.fontSize = Math.max(9, Math.min(28, prefs.fontSize + (a === "bigger" ? 1 : -1)));
      savePrefs();
      applyFontToTerminals();
    }
  });
  loadSavedFont().then(applyFontToTerminals);
  markBackspace();
  $("con-download").addEventListener("click", () => {
    if (!active) return;
    const blob = new Blob([active.view.toText()], { type: "text/plain;charset=utf-8" });
    const a = h("a", { href: URL.createObjectURL(blob), download: `${active.name}-${fmtTime(Date.now()).replace(/[: ]/g, "-")}.log` });
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  });

  // The first port tells us which ports exist.
  const first = addPort({ index: 0, name: "…", title: "Порт 0" });
  first.client.addEventListener(
    "hello",
    (ev) => {
      for (const pi of ev.detail.ports) {
        if (pi.index === 0) Object.assign(first, pi);
        else addPort(pi);
      }
      renderTabs();
      select(ports.find((p) => p.index === prefs.active) || first);
    },
    { once: true },
  );
  select(first);
  renderTabs();
}

function addPort(pi) {
  const p = { ...pi, client: new SerialClient(pi.index) };
  p.body = h("div", { class: "console-body hidden" });
  p.logWrap = h("div", { class: "logview" });
  p.followBtn = h("button", { class: "btn small primary follow-btn hidden" }, "↓ к последним строкам");
  p.input = h("input", { placeholder: "Строка для отправки (Enter). Или кликните в лог и печатайте прямо в порт.", spellcheck: "false" });
  p.eol = h("select", { title: "Окончание строки" }, Object.keys(EOL).map((k) => h("option", { value: k }, k)));
  p.eol.value = prefs.eol;
  p.termWrap = h("div", { class: "console-term hidden" });
  p.logPane = h("div", { class: "console-body" }, p.logWrap, p.followBtn, h("div", { class: "console-input" }, p.input, p.eol, h("button", { class: "btn small" }, "Отправить")));
  p.body.append(p.logPane, p.termWrap);
  root.querySelector("#con-body").append(p.body);

  p.view = new LogView(p.logWrap, {
    showDate: prefs.date,
    onFollowChange: (f) => p.followBtn.classList.toggle("hidden", f),
  });
  p.followBtn.addEventListener("click", () => p.view.scrollToBottom());

  const send = () => {
    p.client.write(p.input.value + EOL[p.eol.value]);
    p.input.value = "";
  };
  p.input.addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
      e.preventDefault();
      send();
    }
  });
  p.logPane.querySelector(".console-input button").addEventListener("click", send);
  p.eol.addEventListener("change", () => {
    prefs.eol = p.eol.value;
    savePrefs();
  });
  // Typing directly into the focused log forwards keys to the port.
  p.logWrap.addEventListener("keydown", (e) => {
    if ((e.ctrlKey || e.metaKey) && (e.key === "c" || e.key === "C") && window.getSelection().toString()) return; // copy
    if ((e.ctrlKey || e.metaKey) && (e.key === "v" || e.key === "V")) return; // paste handled below
    const seq = e.key === "Backspace" && !e.ctrlKey && !e.altKey && !e.metaKey ? BS() : keyToBytes(e);
    if (seq) {
      e.preventDefault();
      p.client.write(seq);
    }
  });
  p.logWrap.addEventListener("paste", (e) => {
    const t = e.clipboardData.getData("text");
    if (t) {
      e.preventDefault();
      p.client.write(t.replace(/\r?\n/g, "\r"));
    }
  });

  let pending = false;
  const refresh = () => {
    if (pending) return;
    pending = true;
    requestAnimationFrame(() => {
      pending = false;
      p.view.update(p.client.partialLine());
      if (p === active) updateStatus();
    });
  };
  p.client.addEventListener("hello", (ev) => {
    Object.assign(p, { name: ev.detail.name, title: ev.detail.title });
    p.view.setTz(tzValue(p));
    renderTabs();
    if (p === active) updateStatus();
  });
  p.client.addEventListener("history", () => {
    p.view.setLines(p.client.lines, p.client.partialLine());
    p.view.scrollToBottom();
    if (p.term) resetTerminal(p);
    if (p === active) updateStatus();
  });
  p.client.addEventListener("data", (ev) => {
    refresh();
    if (p.term) p.term.write(ev.detail.bytes);
  });
  p.client.addEventListener("cleared", () => {
    p.view.setLines(p.client.lines, null);
    if (p.term) p.term.clear();
    updateStatus();
  });
  p.client.addEventListener("status", () => p === active && updateStatus());
  p.client.addEventListener("connection", (ev) => {
    if (p === active) updateStatus();
    if (!ev.detail.connected && ev.detail.reason) root.querySelector("#st-hint").textContent = ev.detail.reason;
  });
  p.client.connect();
  ports.push(p);
  return p;
}

function renderTabs() {
  const tabs = root.querySelector("#con-tabs");
  tabs.replaceChildren(
    ...ports.map((p) => {
      const b = h("button", { class: p === active ? "active" : "", title: `${p.name}` }, p.title);
      b.addEventListener("click", () => select(p));
      return b;
    }),
  );
}

function select(p) {
  active = p;
  prefs.active = p.index;
  savePrefs();
  ports.forEach((x) => x.body.classList.toggle("hidden", x !== p));
  renderTabs();
  setMode(prefs.mode);
  root.querySelector("#con-search").value = "";
  p.view.setFilter("", false);
  updateStatus();
}

function setMode(mode) {
  prefs.mode = mode;
  savePrefs();
  root.querySelectorAll("#con-mode button").forEach((b) => b.classList.toggle("active", b.dataset.mode === mode));
  if (!active) return;
  const term = mode === "term";
  active.logPane.classList.toggle("hidden", term);
  active.termWrap.classList.toggle("hidden", !term);
  ["con-search", "con-only", "con-tz", "con-date"].forEach((id) => (root.querySelector("#" + id).disabled = term));
  if (term) {
    if (!active.term) createTerminal(active);
    requestAnimationFrame(() => {
      active.fit.fit();
      active.term.focus();
    });
  } else {
    active.view.scrollToBottom();
    active.logWrap.focus();
  }
}

function createTerminal(p) {
  if (!window.Terminal) {
    toast("xterm.js не загружен", "err");
    return;
  }
  p.term = new window.Terminal({
    scrollback: p.client.maxLines || 20000,
    convertEol: false,
    cursorBlink: true,
    fontFamily: termFont(),
    fontSize: prefs.fontSize,
    theme: { background: "#111214", foreground: "#dcddde", cursor: "#5a8bb3" },
  });
  p.fit = new window.FitAddon.FitAddon();
  p.term.loadAddon(p.fit);
  p.term.open(p.termWrap);
  p.term.attachCustomKeyEventHandler((e) => {
    if (e.type === "keydown" && e.key === "Backspace" && !e.ctrlKey && !e.altKey && !e.metaKey) {
      p.client.write(BS());
      e.preventDefault();
      return false;
    }
    return true;
  });
  p.term.onData((d) => p.client.write(d));
  p.term.onBinary((d) => p.client.write(Uint8Array.from(d, (c) => c.charCodeAt(0))));
  p.resizeObs = new ResizeObserver(() => !p.termWrap.classList.contains("hidden") && p.fit.fit());
  p.resizeObs.observe(p.termWrap);
  resetTerminal(p);
}

function resetTerminal(p) {
  p.term.reset();
  // History is replayed as plain lines: escape sequences from long ago would
  // only garble the screen.
  const hist = p.client.lines.map((l) => l.text).join("\r\n");
  if (hist) p.term.write(hist + "\r\n");
  const part = p.client.partialLine();
  if (part) p.term.write(part.raw);
}

function updateStatus() {
  if (!active || !root) return;
  const $ = (id) => root.querySelector("#" + id);
  const info = active.client.info;
  $("st-port").textContent = info ? `${active.title} (${info.device})` : active.title;
  const online = active.client.connected && info && info.online;
  $("st-online").textContent = !active.client.connected ? "нет связи с BMC" : info && !info.online ? "устройство недоступно" : online ? (info.legacy ? "штатная прошивка: история только в браузере" : `${info.baud} бод`) : "…";
  $("st-online").style.color = online ? "" : "var(--warn)";
  $("st-lines").textContent = `${active.client.lines.length} / ${active.client.maxLines}`;
  $("st-rx").textContent = `${active.client.stats.rxBytes} Б`;
  if (info && !info.legacy) $("con-baud").value = info.baud;
  $("con-baud").disabled = !!(info && info.legacy);
  $("st-hint").textContent = prefs.mode === "log" ? "Клик в ленту — ввод прямо в порт; выделение мышью — копирование" : "";
}

export function unmount() {
  ports.forEach((p) => {
    p.client.close();
    p.view.destroy();
    if (p.term) p.term.dispose();
    if (p.resizeObs) p.resizeObs.disconnect();
  });
  ports = [];
  active = null;
  root = null;
}
