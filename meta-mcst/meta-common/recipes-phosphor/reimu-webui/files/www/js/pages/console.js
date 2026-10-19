// Serial consoles (host COM ports via reimu-serial): timestamped log with
// full history, search, interactive terminal (xterm.js), baud/break control.

import { h, toast, fmtTime } from "../api.js";
import { SerialClient, keyToBytes } from "../lib/serial-client.js";
import { LogView } from "../lib/logview.js";

export const fill = true;

const BAUDS = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600];
const EOL = { CR: "\r", LF: "\n", CRLF: "\r\n" };

let root;
let ports = []; // {index, name, title, client, view, term, ...}
let active = null;
let prefs;

function loadPrefs() {
  const def = { mode: "log", tz: "local", eol: "CR", date: true, active: 0 };
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
    const seq = keyToBytes(e);
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
    fontFamily: "DejaVu Sans Mono, Liberation Mono, Consolas, monospace",
    fontSize: 14,
    theme: { background: "#111214", foreground: "#dcddde", cursor: "#5a8bb3" },
  });
  p.fit = new window.FitAddon.FitAddon();
  p.term.loadAddon(p.fit);
  p.term.open(p.termWrap);
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
  $("st-online").textContent = !active.client.connected ? "нет связи с BMC" : info && !info.online ? "устройство недоступно" : online ? `${info.baud} бод` : "…";
  $("st-online").style.color = online ? "" : "var(--warn)";
  $("st-lines").textContent = `${active.client.lines.length} / ${active.client.maxLines}`;
  $("st-rx").textContent = `${active.client.stats.rxBytes} Б`;
  if (info) $("con-baud").value = info.baud;
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
