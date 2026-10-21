// Serial consoles (host COM ports via reimu-serial): timestamped log with
// full history, search, interactive terminal (xterm.js), baud/break control.

import { h, icon, toast, fmtTime } from "../api.js";
import { SerialClient, keyToBytes } from "../lib/serial-client.js";
import { LogView } from "../lib/lineview.js";
import { keyBar, ARROWS, FKEYS } from "../lib/keybar.js";

export const fill = true;

const BAUDS = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600];
const EOL = { CR: "\r", LF: "\n", CRLF: "\r\n" };

let root;
let ports = []; // {index, name, title, client, view, term, ...}
let active = null;
let prefs;
let keys; // key bar for touch screens
const held = { ctrl: false, alt: false };

const narrow = () => window.matchMedia && matchMedia("(max-width: 700px)").matches;
const touch = () => (window.matchMedia && matchMedia("(pointer: coarse)").matches) || navigator.maxTouchPoints > 0;
// The host formats its output for 80 columns and never learns the size of
// a serial terminal. On a narrow screen the font shrinks until 80 columns
// fit (not below MIN_PX); wider lines pan sideways.
const MIN_COLS = 80;
const MIN_PX = 6;
// input sent and not a byte back for this long: the host is stuck
const NO_REPLY_MS = 5000;

// width of a character of the terminal font, per px of font size
function charRatio() {
  const c = document.createElement("canvas").getContext("2d");
  c.font = `100px ${termFont()}`;
  return c.measureText("M".repeat(10)).width / 1000 || 0.6;
}

// font size (px) for `cols` characters in `width` px, at most the chosen size
function fitPx(width, cols) {
  if (!narrow()) return prefs.fontSize;
  const px = Math.floor((width / (cols * charRatio())) * 2) / 2;
  return Math.max(MIN_PX, Math.min(prefs.fontSize, px));
}

function fitLog(p) {
  // the time column counts too: "10:20:24.528 " or with the date
  const tsCols = !prefs.time ? 0 : prefs.date ? 25 : 14;
  const width = (p.logWrap.clientWidth || p.body.clientWidth || innerWidth) - 16;
  p.view.setFont(fitPx(width, MIN_COLS + tsCols), termFont(), narrow() ? MIN_COLS + tsCols : 0);
}

function fitTerm(p) {
  const px = fitPx(p.termWrap.clientWidth - 8, MIN_COLS);
  if (p.term.options.fontSize !== px) p.term.options.fontSize = px;
  p.fit.fit();
  if (narrow() && p.term.cols < MIN_COLS) p.term.resize(MIN_COLS, p.term.rows);
}

// Shell prompts draw icons from Nerd Fonts (Unicode private use area). With
// no such font in the system they would be empty boxes: show spaces then.
const PUA = /[\uE000-\uF8FF]|[\u{F0000}-\u{10FFFD}]/gu;
let noIcons = false;
function checkIcons() {
  const c = document.createElement("canvas");
  c.width = 40;
  c.height = 40;
  const g = c.getContext("2d");
  const draw = (ch) => {
    g.clearRect(0, 0, 40, 40);
    g.font = `30px ${termFont()}`;
    g.fillText(ch, 4, 32);
    return g.getImageData(0, 0, 40, 40).data.join();
  };
  // U+E0B0 (powerline arrow) is in every Nerd Font, U+E9F0 in none
  noIcons = draw("\ue0b0") === draw("\ue9f0");
  const map = noIcons ? (t) => t.replace(PUA, " ") : null;
  ports.forEach((p) => {
    p.view.mapText = map;
    p.view.setTz(tzValue(p));
  });
}
const termText = (t) => (noIcons ? t.replace(PUA, " ") : t);

const KEYS = [
  ARROWS,
  // ^C/^D/^Z at the far end: a stray ^D logs the session out
  [["Del", "Delete"], ["Home", "Home"], ["End", "End"], ["PgUp", "PageUp"], ["PgDn", "PageDown"], ["Ins", "Insert"], ...FKEYS, ["^C", "^C"], ["^D", "^D"], ["^Z", "^Z"]],
];

function pressKey(k) {
  if (k in held) {
    held[k] = !held[k];
    return;
  }
  if (!active) return;
  const seq = k[0] === "^" ? String.fromCharCode(k.charCodeAt(1) - 64) : keyToBytes({ key: k });
  if (seq) active.client.write((held.alt ? "\x1b" : "") + seq);
  held.ctrl = held.alt = false;
}

// text typed on the phone keyboard after a sticky Ctrl/Alt of the key bar
function withMods(d) {
  if (!(held.ctrl || held.alt) || d.length !== 1) return d;
  let out = d;
  if (held.ctrl) out = keyToBytes({ key: d, ctrlKey: true }) || d;
  if (held.alt) out = "\x1b" + out;
  held.ctrl = held.alt = false;
  if (keys) keys.refresh();
  return out;
}

function loadPrefs() {
  // a phone has no room for the date in front of every line
  const def = { mode: "log", tz: "local", eol: "CR", date: !narrow(), active: 0, fontSize: narrow() ? 12 : 14, bs: "bs", keys: true, time: true };
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
    if (!p.termWrap.classList.contains("hidden")) fitTerm(p);
  });
  ports.forEach(fitLog);
  checkIcons();
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
      h("button", { class: "btn small touch-only", id: "con-keys", title: "Панель клавиш: Esc, Tab, Ctrl, стрелки, F1–F12", "aria-label": "Панель клавиш" }, icon("keyboard")),
      h("div", { class: "tabs", id: "con-tabs" }),
      h("span", { class: "spacer" }),
      h(
        "div",
        { class: "tabs", id: "con-mode" },
        h("button", { "data-mode": "log", title: "Лента строк с метками времени и поиском" }, "Лог"),
        h("button", { "data-mode": "term", title: "Интерактивный терминал (xterm)" }, "Терминал"),
      ),
      h("input", { id: "con-search", type: "search", placeholder: "Поиск…", size: 18 }),
      h("label", { class: "inline small con-only-l" }, h("input", { type: "checkbox", id: "con-only" }), "только найденное"),
      h("button", { class: "btn small m-only-inline", id: "con-more", title: "Остальные настройки", "aria-label": "Остальные настройки" }, icon("more")),
      h(
        "div",
        { class: "con-extra" },
        h(
          "select",
          { id: "con-tz", title: "Часы, в которых показывается время строк" },
          h("option", { value: "local" }, "Время: браузер"),
          h("option", { value: "bmc" }, "Время: BMC"),
          h("option", { value: "utc" }, "Время: UTC"),
        ),
        h("label", { class: "inline small" }, h("input", { type: "checkbox", id: "con-time" }), "время"),
        h("label", { class: "inline small" }, h("input", { type: "checkbox", id: "con-date" }), "дата"),
        h("select", { id: "con-baud", title: "Скорость порта" }, BAUDS.map((b) => h("option", { value: b }, `${b}`))),
        h("button", { class: "btn small", id: "con-break", title: "Послать BREAK (SysRq на Linux)" }, "Break"),
        h("button", { class: "btn small", id: "con-download", title: "Скачать историю с метками времени" }, "Скачать"),
        h("button", { class: "btn small", id: "con-clear", title: "Очистить историю порта на BMC" }, "Очистить"),
      ),
      h(
        "div",
        { class: "dropdown", id: "con-fontmenu" },
        h("button", { class: "btn small", id: "con-font", title: "Шрифт терминала" }, "Терминал ", icon("caret")),
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
    (keys = keyBar(KEYS, pressKey, held)),
    h(
      "div",
      { class: "statusbar", id: "con-status" },
      h("span", {}, "Порт: ", h("b", { id: "st-port" }, "—")),
      h("span", {}, "Состояние: ", h("b", { id: "st-online" }, "—")),
      h("span", {}, "Строк: ", h("b", { id: "st-lines" }, "0")),
      h("span", {}, "Принято: ", h("b", { id: "st-rx" }, "0")),
      h("span", { class: "spacer" }),
      h("span", { id: "st-hint", class: "muted m-hide" }, ""),
    ),
  );

  const $ = (id) => el.querySelector("#" + id);
  $("con-more").addEventListener("click", () => el.classList.toggle("con-extra-open"));
  const showKeys = () => {
    const on = touch() && prefs.keys;
    keys.classList.toggle("hidden", !on);
    $("con-keys").classList.toggle("active", on);
    if (active && active.term && !active.termWrap.classList.contains("hidden")) requestAnimationFrame(() => fitTerm(active));
  };
  $("con-keys").addEventListener("click", () => {
    prefs.keys = !prefs.keys;
    savePrefs();
    showKeys();
  });
  showKeys();
  $("con-tz").value = prefs.tz;
  $("con-date").checked = prefs.date;
  $("con-time").checked = prefs.time;
  $("con-time").addEventListener("change", (e) => {
    prefs.time = e.target.checked;
    savePrefs();
    ports.forEach((p) => {
      p.view.showTime = prefs.time;
      fitLog(p);
    });
  });
  const onResize = () => ports.forEach((p) => p.body.classList.contains("hidden") || fitLog(p));
  window.addEventListener("resize", onResize);
  root._offResize = () => window.removeEventListener("resize", onResize);
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
        fitLog(p);
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

// an "exec:" port of reimu-serial (SSH to the platform), not a COM port
const isExec = (p) => !!(p.client.info && /^exec:/.test(p.client.info.device || ""));

// «хост не отвечает»: input went out and nothing came back within NO_REPLY_MS;
// «сессия завершилась»: a login prompt came after the user typed something
function watchReplies(p) {
  const write = p.client.write.bind(p.client);
  p.client.write = (d) => {
    p.lastInput = Date.now();
    p.ended = 0;
    if (!p.waitSince && !isExec(p)) {
      p.waitSince = Date.now();
      p.waitTimer = setTimeout(() => {
        p.silentSince = p.waitSince;
        clearInterval(p.silentTick);
        p.silentTick = setInterval(() => p === active && updateStatus(), 30000);
        if (p === active) updateStatus();
      }, NO_REPLY_MS);
    }
    return write(d);
  };
  p.client.addEventListener("data", (ev) => {
    clearTimeout(p.waitTimer);
    clearInterval(p.silentTick);
    const was = p.silentSince;
    p.waitSince = p.silentSince = 0;
    if (p.lastInput && !isExec(p)) {
      p.tail = ((p.tail || "") + (p.loginDec ||= new TextDecoder()).decode(ev.detail.bytes, { stream: true })).slice(-200);
      if (/login:\s*$/i.test(p.tail)) p.ended = Date.now();
    }
    if (was && p === active) updateStatus();
  });
}

function addPort(pi) {
  const p = { ...pi, client: new SerialClient(pi.index) };
  watchReplies(p);
  p.body = h("div", { class: "console-body hidden" });
  p.logWrap = h("div", { class: "logview" });
  p.followBtn = h("button", { class: "btn small primary follow-btn hidden" }, "↓ к последним строкам");
  // phone keyboards must not capitalize or "correct" commands
  p.input = h("input", {
    placeholder: narrow() ? "Строка для отправки" : "Строка для отправки (Enter). Или кликните в лог и печатайте прямо в порт.",
    spellcheck: "false",
    autocapitalize: "off",
    autocorrect: "off",
    autocomplete: "off",
    enterkeyhint: "send",
  });
  p.eol = h("select", { title: "Окончание строки" }, Object.keys(EOL).map((k) => h("option", { value: k }, k)));
  p.eol.value = prefs.eol;
  p.termWrap = h("div", { class: "console-term hidden" });
  p.logPane = h("div", { class: "console-body" }, p.logWrap, p.followBtn, h("div", { class: "console-input" }, p.input, p.eol, h("button", { class: "btn small" }, "Отправить")));
  p.body.append(p.logPane, p.termWrap);
  root.querySelector("#con-body").append(p.body);

  p.view = new LogView(p.logWrap, {
    showDate: prefs.date,
    showTime: prefs.time,
    onFollowChange: (f) => p.followBtn.classList.toggle("hidden", f),
  });
  p.followBtn.addEventListener("click", () => p.view.scrollToBottom());
  p.view.mapText = noIcons ? (t) => t.replace(PUA, " ") : null;
  requestAnimationFrame(() => fitLog(p));

  const send = () => {
    p.client.write(p.input.value + EOL[p.eol.value]);
    p.input.value = "";
  };
  p.input.addEventListener("beforeinput", (e) => {
    // Ctrl/Alt of the key bar + a letter goes straight to the port
    if (e.data && e.data.length === 1 && (held.ctrl || held.alt)) {
      e.preventDefault();
      p.client.write(withMods(e.data));
    }
  });
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
    if (p.term) p.term.write(noIcons ? termText((p.dec ||= new TextDecoder()).decode(ev.detail.bytes, { stream: true })) : ev.detail.bytes);
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
  ["con-search", "con-only", "con-tz", "con-date", "con-time"].forEach((id) => (root.querySelector("#" + id).disabled = term));
  if (term) {
    if (!active.term) createTerminal(active);
    requestAnimationFrame(() => {
      fitTerm(active);
      active.term.focus();
    });
  } else {
    fitLog(active);
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
  p.term.onData((d) => p.client.write(withMods(d)));
  p.term.onBinary((d) => p.client.write(Uint8Array.from(d, (c) => c.charCodeAt(0))));
  p.resizeObs = new ResizeObserver(() => !p.termWrap.classList.contains("hidden") && fitTerm(p));
  p.resizeObs.observe(p.termWrap);
  // xterm takes every touch move for its scrollback: pan sideways here
  let t = null;
  p.termWrap.addEventListener("touchstart", (e) => (t = e.touches.length === 1 ? { x: e.touches[0].clientX, y: e.touches[0].clientY, h: null } : null), { capture: true, passive: true });
  p.termWrap.addEventListener(
    "touchmove",
    (e) => {
      if (!t || e.touches.length !== 1) return;
      const x = e.touches[0].clientX, y = e.touches[0].clientY;
      if (t.h === null && Math.hypot(x - t.x, y - t.y) > 6) t.h = Math.abs(x - t.x) > Math.abs(y - t.y);
      if (t.h) p.termWrap.scrollLeft -= x - t.x;
      t.x = x;
      t.y = y;
    },
    { capture: true, passive: true },
  );
  resetTerminal(p);
}

function resetTerminal(p) {
  p.term.reset();
  // History is replayed as plain lines: escape sequences from long ago would
  // only garble the screen.
  const hist = p.client.lines.map((l) => l.text).join("\r\n");
  if (hist) p.term.write(termText(hist) + "\r\n");
  const part = p.client.partialLine();
  if (part) p.term.write(noIcons ? termText(part.raw) : part.raw);
}

function updateStatus() {
  if (!active || !root) return;
  const $ = (id) => root.querySelector("#" + id);
  const info = active.client.info;
  const exec = isExec(active);
  $("st-port").textContent = info && !exec ? `${active.title} (${info.device})` : active.title;
  const online = active.client.connected && info && info.online;
  const st = $("st-online");
  st.textContent = !active.client.connected ? "нет связи с BMC" : info && !info.online ? "устройство недоступно" : online ? (info.legacy ? "штатная прошивка: история только в браузере" : exec ? "сеанс SSH" : `${info.baud} бод`) : "…";
  st.title = "";
  st.style.color = online ? "" : "var(--warn)";
  if (online && active.silentSince) {
    const min = Math.floor((Date.now() - active.silentSince) / 60000);
    st.textContent = `хост не отвечает на ввод${min ? ` (${min} мин)` : ""}`;
    st.title = "Ctrl+Q — если вывод остановлен (XOFF); вкладка SSH — войти по сети; Break (SysRq) действует, только если консоль ядра на этом порту (console=ttyS1)";
    st.style.color = "var(--warn)";
  } else if (online && active.ended) {
    st.textContent = "сессия завершилась: приглашение входа";
    st.style.color = "var(--warn)";
  }
  $("st-lines").textContent = `${active.client.lines.length} / ${active.client.maxLines}`;
  $("st-rx").textContent = `${active.client.stats.rxBytes} Б`;
  if (info && !info.legacy && !exec) $("con-baud").value = info.baud;
  $("con-baud").disabled = !!(info && info.legacy) || exec;
  $("con-break").disabled = exec;
  $("st-hint").textContent = online && active.silentSince ? st.title : prefs.mode === "log" ? "Клик в ленту — ввод прямо в порт; выделение мышью — копирование" : "";
}

export function unmount() {
  if (root && root._offResize) root._offResize();
  ports.forEach((p) => {
    clearTimeout(p.waitTimer);
    clearInterval(p.silentTick);
    p.client.close();
    p.view.destroy();
    if (p.term) p.term.dispose();
    if (p.resizeObs) p.resizeObs.disconnect();
  });
  ports = [];
  active = null;
  root = null;
  keys = null;
}
