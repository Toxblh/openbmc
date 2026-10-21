// Remote screen (KVM-over-IP) in the browser. Sources:
//  - direct: reimu-kvm pushes every JPEG frame of the AST2500 video engine
//    over its own wss://BMC:8443 (keyboard/mouse go back as USB HID usages);
//  - H.264 from the host: while the platform's OS runs, reimu-h264 there
//    encodes the desktop on the video card, reimu-kvm relays it (60 frames/s);
//  - VNC: noVNC over bmcweb's /kvm/0 websocket to obmc-ikvm (works everywhere).
// "Auto" takes H.264 when a host address is set and it answers, the direct
// JPEG stream when reimu-kvm runs and the browser accepts its port, VNC otherwise.

import { api, h, icon, toast } from "../api.js";
import { RFB, KeyTable } from "../../vendor/novnc/novnc.js";
import { installAccel, accelStats } from "../lib/novnc-accel.js";
import { keyBar, ARROWS, FKEYS } from "../lib/keybar.js";
import { touchpad } from "../lib/touchpad.js";
import { DirectKvm, directTicket, directStats, h264Supported } from "../lib/directkvm.js";

export const fill = true;

let root, rfb, screen, overlay, statTimer, kbd, keysBar, tp;
let wantConnected = false;
let retryTimer = null;
const st = { frames: 0, lastFrames: 0, fps: 0, bytes: 0, lastBytes: 0, bps: 0 };
let source = null; // "h264" | "direct" | "vnc" while connected
let directFailed = false; // the direct stream did not open: fall back to VNC in auto
const SOURCES = { auto: "Авто", h264: "H.264 с хоста (видеокарта)", direct: "Прямой JPEG (видеодвижок BMC)", vnc: "VNC" };
const QUALITY = [
  ["Экономно (≈30 к/с, 4:2:0)", { fps: 30, quality: 0, sub420: true }],
  ["Плавно (до 60 к/с, 4:2:0)", { fps: 60, quality: 0, sub420: true }],
  ["Чётко (до 60 к/с, 4:4:4)", { fps: 60, quality: 2, sub420: false }],
  ["Максимум качества (4:4:4)", { fps: 60, quality: 6, sub420: false }],
];

const COMBOS = [
  { label: "Ctrl+Alt+Del", keys: [["ControlLeft", KeyTable.XK_Control_L], ["AltLeft", KeyTable.XK_Alt_L], ["Delete", KeyTable.XK_Delete]] },
  { label: "Ctrl+Alt+Backspace", keys: [["ControlLeft", KeyTable.XK_Control_L], ["AltLeft", KeyTable.XK_Alt_L], ["Backspace", KeyTable.XK_BackSpace]] },
  ...[1, 2, 3, 4, 5, 6, 7].map((n) => ({
    label: `Ctrl+Alt+F${n}`,
    keys: [["ControlLeft", KeyTable.XK_Control_L], ["AltLeft", KeyTable.XK_Alt_L], [`F${n}`, KeyTable[`XK_F${n}`]]],
  })),
  { label: "Alt+Tab", keys: [["AltLeft", KeyTable.XK_Alt_L], ["Tab", KeyTable.XK_Tab]] },
  { label: "Alt+F4", keys: [["AltLeft", KeyTable.XK_Alt_L], ["F4", KeyTable.XK_F4]] },
  { label: "Print Screen", keys: [["PrintScreen", KeyTable.XK_Print]] },
  { label: "Alt+SysRq+R", keys: [["AltLeft", KeyTable.XK_Alt_L], ["PrintScreen", KeyTable.XK_Sys_Req], ["KeyR", KeyTable.XK_r]] },
  { label: "Win (Super)", keys: [["MetaLeft", KeyTable.XK_Super_L]] },
  { label: "Esc", keys: [["Escape", KeyTable.XK_Escape]] },
  { label: "F2", keys: [["F2", KeyTable.XK_F2]] },
  { label: "F12", keys: [["F12", KeyTable.XK_F12]] },
];

// Touch screens: a phone's keyboard comes up only for a real text field, so
// a hidden textarea takes the typing and forwards it as key presses; a key
// bar adds what phone keyboards lack. Ctrl/Alt/Win are sticky: they apply
// to the next key.
const coarse = (window.matchMedia && matchMedia("(pointer: coarse)").matches) || navigator.maxTouchPoints > 0;
const K = KeyTable;
const SPECIAL = {
  Enter: ["Enter", K.XK_Return],
  Backspace: ["Backspace", K.XK_BackSpace],
  Tab: ["Tab", K.XK_Tab],
  Escape: ["Escape", K.XK_Escape],
  Delete: ["Delete", K.XK_Delete],
  Insert: ["Insert", K.XK_Insert],
  Home: ["Home", K.XK_Home],
  End: ["End", K.XK_End],
  PageUp: ["PageUp", K.XK_Page_Up],
  PageDown: ["PageDown", K.XK_Page_Down],
  ArrowLeft: ["ArrowLeft", K.XK_Left],
  ArrowRight: ["ArrowRight", K.XK_Right],
  ArrowUp: ["ArrowUp", K.XK_Up],
  ArrowDown: ["ArrowDown", K.XK_Down],
  ...Object.fromEntries([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12].map((n) => [`F${n}`, [`F${n}`, K[`XK_F${n}`]]])),
};
const MODS = { ctrl: ["ControlLeft", K.XK_Control_L], alt: ["AltLeft", K.XK_Alt_L], win: ["MetaLeft", K.XK_Super_L] };
const KEYBAR = [
  ARROWS,
  [["Win", "win"], ["Del", "Delete"], ["Home", "Home"], ["End", "End"], ["PgUp", "PageUp"], ["PgDn", "PageDown"], ["Ins", "Insert"], ...FKEYS],
];
const held = { ctrl: false, alt: false, win: false };

function getPrefs() {
  try {
    return Object.assign({ scale: true, viewOnly: false, dot: true, touchpad: true, source: "auto", quality: 1, h264Host: "" }, JSON.parse(localStorage.getItem("reimu-kvm") || "{}"));
  } catch (e) {
    return { scale: true, viewOnly: false, dot: true, touchpad: true, source: "auto", quality: 1, h264Host: "" };
  }
}
let prefs = getPrefs();
const savePrefs = () => {
  try {
    localStorage.setItem("reimu-kvm", JSON.stringify(prefs));
  } catch (e) {
    /* ignore */
  }
};

export async function mount(el) {
  root = el;
  installAccel();
  const btn = (id, text, title) => h("button", { class: "btn small", id, title }, text);
  el.style.position = "relative";
  el.append(
    h(
      "div",
      { class: "kvm-toolbar" },
      h("button", { class: "btn small touch-only", id: "kvm-kbd", title: "Клавиатура телефона и панель клавиш" }, icon("keyboard"), h("span", { class: "m-hide-inline" }, "Клавиатура")),
      h("button", { class: "btn small touch-only", id: "kvm-tp", title: "Тачпад: палец водит курсор, тап — клик. Выключено: клик там, где коснулись" }, icon("touchpad"), h("span", {}, "Тачпад")),
      h("span", { class: "m-hide-inline" }, btn("kvm-connect", "Подключить"), " ", btn("kvm-disconnect", "Отключить")),
      h("span", { class: "spacer" }),
      h(
        "div",
        { class: "dropdown" },
        h("button", { class: "btn small", title: "Откуда берётся картинка" }, h("span", { id: "kvm-src-btn" }, "Источник"), " ", icon("caret")),
        h(
          "div",
          { class: "dropdown-menu", id: "kvm-src-menu" },
          h("div", { class: "dropdown-label" }, "Источник"),
          Object.entries(SOURCES).map(([k, v]) => h("button", { "data-src": k }, v)),
          h("div", { class: "dropdown-label" }, "H.264 с хоста"),
          h("button", { "data-host": "1", title: "Адрес reimu-h264 на платформе (её IP и порт, по умолчанию 8555)" }, "Адрес хоста…"),
          h("div", { class: "dropdown-label" }, "Прямой JPEG"),
          QUALITY.map(([label], i) => h("button", { "data-q": i }, label)),
        ),
      ),
      h(
        "div",
        { class: "dropdown" },
        h("button", { class: "btn small" }, "Клавиши ", icon("caret")),
        h("div", { class: "dropdown-menu", id: "kvm-combos" }, COMBOS.map((c, i) => h("button", { "data-i": i }, c.label))),
      ),
      h("span", { class: "m-hide-inline" }, btn("kvm-paste", "Вставить текст", "Напечатать текст из буфера на платформе (только латиница/ASCII)"), " ", btn("kvm-shot", "Снимок", "Сохранить снимок экрана в PNG")),
      btn("kvm-scale", "Масштаб", "Вписать в окно / 1:1"),
      h("span", { class: "m-hide-inline" }, btn("kvm-view", "Только просмотр", "Не передавать клавиатуру и мышь")),
      h("button", { class: "btn small", id: "kvm-full", title: "Полный экран" }, icon("fullscreen"), h("span", { class: "m-hide-inline" }, "Полный экран")),
      // the phone has no room for the rarer buttons
      h(
        "div",
        { class: "dropdown m-only-inline" },
        h("button", { class: "btn small", title: "Ещё", "aria-label": "Ещё" }, icon("more")),
        h(
          "div",
          { class: "dropdown-menu right", id: "kvm-more" },
          ["kvm-connect", "kvm-disconnect", "kvm-paste", "kvm-shot", "kvm-view"].map((id) => h("button", { "data-for": id }, { "kvm-connect": "Подключить", "kvm-disconnect": "Отключить", "kvm-paste": "Вставить текст", "kvm-shot": "Снимок экрана", "kvm-view": "Только просмотр" }[id])),
        ),
      ),
    ),
    (screen = h("div", { class: "kvm-screen", id: "kvm-screen" }, (overlay = h("div", { class: "kvm-overlay" }, "Нет подключения")))),
    (keysBar = keyBar(KEYBAR, pressBarKey, held)),
    (kbd = h("textarea", {
      class: "kvm-kbd-input",
      "aria-label": "Ввод с клавиатуры телефона",
      autocapitalize: "off",
      autocorrect: "off",
      autocomplete: "off",
      spellcheck: "false",
      enterkeyhint: "enter",
    })),
    h(
      "div",
      { class: "statusbar" },
      h("span", {}, "Состояние: ", h("b", { id: "kvm-state" }, "отключено")),
      h("span", { title: "Откуда идёт картинка: прямой поток reimu-kvm или VNC через bmcweb" }, "Источник: ", h("b", { id: "kvm-src" }, "—")),
      h("span", {}, "Разрешение: ", h("b", { id: "kvm-res" }, "—")),
      h("span", { title: "Показано браузером / снято видеодвижком (прямой поток)" }, "Кадров/с: ", h("b", { id: "kvm-fps" }, "0")),
      h("span", { class: "m-hide" }, "Поток: ", h("b", { id: "kvm-bw" }, "—")),
      h("span", { class: "m-hide", title: "Декодирование JPEG браузером вне основного потока (createImageBitmap)" }, "Декодер: ", h("b", { id: "kvm-dec" }, accelStats.enabled ? "аппаратный/асинхронный" : "стандартный")),
      h("span", { class: "spacer" }),
      h("span", { class: "muted m-hide", id: "kvm-engine" }, "Сжатие JPEG — видеодвижок AST2500"),
      coarse ? h("span", { class: "muted", id: "kvm-touch-hint" }) : null,
    ),
  );
  const $ = (id) => el.querySelector("#" + id);
  $("kvm-connect").addEventListener("click", () => connect());
  $("kvm-disconnect").addEventListener("click", () => disconnect());
  $("kvm-combos").addEventListener("click", (e) => {
    const b = e.target.closest("button[data-i]");
    if (b) sendCombo(COMBOS[b.dataset.i]);
  });
  $("kvm-more").addEventListener("click", (e) => {
    const b = e.target.closest("button[data-for]");
    if (b) $(b.dataset.for).click();
  });
  $("kvm-src-menu").addEventListener("click", (e) => {
    const b = e.target.closest("button");
    if (!b) return;
    if (b.dataset.src) {
      prefs.source = b.dataset.src;
      directFailed = false;
      savePrefs();
      applyPrefs();
      reconnect();
    } else if (b.dataset.host) {
      if (askHost()) reconnect();
    } else if (b.dataset.q !== undefined) {
      prefs.quality = +b.dataset.q;
      savePrefs();
      applyPrefs();
      if (source === "direct") rfb.settings(QUALITY[prefs.quality][1]);
    }
  });
  // the page on the direct port tells when its certificate has been accepted
  window.addEventListener("message", onCertMessage);
  $("kvm-tp").addEventListener("click", () => {
    prefs.touchpad = !prefs.touchpad;
    savePrefs();
    applyPrefs();
  });
  tp = touchpad(screen, () => rfb);
  $("kvm-paste").addEventListener("click", pasteText);
  $("kvm-shot").addEventListener("click", screenshot);
  $("kvm-scale").addEventListener("click", () => {
    prefs.scale = !prefs.scale;
    savePrefs();
    applyPrefs();
  });
  $("kvm-view").addEventListener("click", () => {
    prefs.viewOnly = !prefs.viewOnly;
    savePrefs();
    applyPrefs();
  });
  setupTouchKeyboard($);
  // iPhone Safari has no element fullscreen
  if (!document.fullscreenEnabled) $("kvm-full").classList.add("hidden");
  $("kvm-full").addEventListener("click", () => {
    if (document.fullscreenElement) document.exitFullscreen();
    else screen.requestFullscreen().catch((e) => toast(e.message, "err"));
  });
  statTimer = setInterval(updateStats, 1000);
  connect();
}

function applyPrefs() {
  const $ = (id) => root.querySelector("#" + id);
  $("kvm-scale").classList.toggle("active", prefs.scale);
  $("kvm-view").classList.toggle("active", prefs.viewOnly);
  $("kvm-tp").classList.toggle("active", prefs.touchpad);
  $("kvm-more").querySelector('[data-for="kvm-view"]').textContent = `${prefs.viewOnly ? "✓ " : ""}Только просмотр`;
  for (const b of root.querySelectorAll("#kvm-src-menu [data-src]")) b.textContent = `${b.dataset.src === prefs.source ? "✓ " : ""}${SOURCES[b.dataset.src]}`;
  for (const b of root.querySelectorAll("#kvm-src-menu [data-q]")) b.textContent = `${+b.dataset.q === prefs.quality ? "✓ " : ""}${QUALITY[b.dataset.q][0]}`;
  $("kvm-src-btn").textContent = { auto: "Источник", h264: "H.264", direct: "JPEG", vnc: "VNC" }[prefs.source];
  $("kvm-src-menu").querySelector("[data-host]").textContent = `Адрес хоста: ${prefs.h264Host || "не задан"}…`;
  const hint = $("kvm-touch-hint");
  if (hint) hint.textContent = prefs.touchpad ? "тап — клик · 2 пальца — правый клик, прокрутка · двойной тап и вести — перетащить" : "тап — клик · 2 пальца — правый клик, прокрутка";
  if (tp) tp.set(coarse && prefs.touchpad && !prefs.viewOnly);
  if (rfb) {
    rfb.scaleViewport = prefs.scale;
    rfb.clipViewport = !prefs.scale;
    rfb.viewOnly = prefs.viewOnly;
    // 1:1 on a touch screen: a finger pans the picture instead of dragging the mouse
    rfb.dragViewport = coarse && !prefs.scale && !prefs.touchpad;
    rfb.showDotCursor = prefs.dot;
    if (rfb._applyView) rfb._applyView();
    // touches drive the touchpad, not the direct stream's own pointer handling
    rfb._touchpadOwned = !!(tp && tp.enabled);
  }
}

function setState(text, showOverlay, withButton, ...extra) {
  if (!root) return;
  root.querySelector("#kvm-state").textContent = text;
  overlay.classList.toggle("hidden", !showOverlay);
  overlay.replaceChildren(text);
  if (withButton) {
    const b = h("button", { class: "btn primary" }, "Подключить");
    b.addEventListener("click", () => connect());
    overlay.append(b);
  }
  overlay.append(...extra);
}

const SRC_TEXT = {
  h264: "H.264 с хоста",
  direct: "прямой JPEG",
  vnc: "VNC",
};
const ENGINE_TEXT = {
  h264: "H.264 видеокарты хоста (reimu-h264) → BMC → браузер",
  direct: "JPEG видеодвижка AST2500 → браузер (reimu-kvm, :8443)",
  vnc: "JPEG видеодвижка AST2500 → obmc-ikvm → bmcweb",
};

function askHost() {
  const v = prompt("Адрес reimu-h264 на платформе (IP[:порт], порт по умолчанию 8555). На платформе один раз: sudo reimu-host.sh install (tools/host)", prefs.h264Host || "");
  if (v === null) return false;
  prefs.h264Host = v.trim() && !/:\d+$/.test(v.trim()) ? `${v.trim()}:8555` : v.trim();
  savePrefs();
  applyPrefs();
  return true;
}

function setSource(s) {
  source = s;
  // the counters differ per source: start the per-second deltas afresh
  st.lastFrames = s === "direct" ? directStats.frames : accelStats.frames;
  st.lastBytes = s === "direct" ? directStats.bytes : st.bytes;
  if (!root) return;
  root.querySelector("#kvm-src").textContent = s ? SRC_TEXT[s] : "—";
  root.querySelector("#kvm-engine").textContent = ENGINE_TEXT[s] || "";
}

function reconnect() {
  if (rfb) {
    const r = rfb;
    r._reconnect = true;
    r.disconnect();
  } else connect();
}

function onCertMessage(e) {
  if (e.data === "reimu-kvm-cert" && root) {
    directFailed = false;
    reconnect();
  }
}

// the browser refused the direct port (its certificate not accepted yet):
// offer to accept it in a new tab, or to go on with VNC
function certHelp(port) {
  const url = `https://${location.hostname}:${port}/`;
  const accept = h("button", { class: "btn primary" }, "Разрешить прямой поток");
  accept.addEventListener("click", () => window.open(url, "reimu-kvm-cert"));
  const vnc = h("button", { class: "btn" }, "Продолжить через VNC");
  vnc.addEventListener("click", () => {
    directFailed = true;
    connect();
  });
  return h(
    "div",
    { class: "kvm-cert" },
    h("p", {}, `Браузер не открыл прямой поток на порту ${port}: обычно это значит, что для него ещё не принят сертификат BMC (тот же самоподписанный, что у веб-интерфейса).`),
    h("p", { class: "muted" }, "Нажмите «Разрешить прямой поток», в новой вкладке подтвердите переход — она закроется сама и KVM переподключится."),
    h("div", { class: "row" }, accept, vnc),
  );
}

// Counts bytes received on the KVM websocket (for the bandwidth indicator)
// without touching noVNC internals: wrap the socket's message delivery.
function countingWebSocket(ws) {
  ws.addEventListener("message", (ev) => {
    st.bytes += ev.data.byteLength || ev.data.length || 0;
  });
  return ws;
}

async function connect() {
  wantConnected = true;
  clearTimeout(retryTimer);
  if (rfb || connecting) return;
  setState("подключение…", true);
  if (prefs.source === "h264" || prefs.source === "direct" || (prefs.source === "auto" && !directFailed)) {
    connecting = true;
    const ticket = await directTicket();
    connecting = false;
    if (!root || rfb) return;
    if (ticket) return connectDirect(ticket);
    if (prefs.source !== "auto") {
      setState("прямой поток недоступен: сервис reimu-kvm не запущен на BMC", true, true);
      return;
    }
  }
  connectVnc();
}

let connecting = false;

function connectDirect(ticket) {
  const r = (rfb = new DirectKvm(screen, ticket));
  applyPrefs();
  r.addEventListener("connect", () => {
    setSource("direct");
    setState("подключено", false);
    r.settings(QUALITY[prefs.quality][1]);
    if (tp) tp.connected();
    r.focus();
    const wantH264 = prefs.source === "h264" || (prefs.source === "auto" && prefs.h264Host);
    if (wantH264 && !h264Supported()) toast("Браузер не умеет WebCodecs (H.264): показываю JPEG видеодвижка", "err");
    else if (prefs.source === "h264" && !prefs.h264Host && !askHost()) toast("Адрес хоста не задан: показываю JPEG видеодвижка", "err");
    if (wantH264 && h264Supported() && prefs.h264Host) r.useHost(prefs.h264Host);
    else toast("KVM подключён: прямой поток", "ok");
  });
  r.addEventListener("status", (e) => {
    const m = e.detail;
    if (m.t === "h264") {
      setSource("h264");
      setState("подключено", false);
      toast(`KVM: H.264 с хоста, ${m.w}×${m.h} до ${m.fps} к/с`, "ok");
      return;
    }
    if (m.t === "h264off") {
      if (source === "h264" || prefs.source === "h264") toast(`H.264 с хоста недоступен (${m.reason}): показываю JPEG видеодвижка`, "err");
      setSource("direct");
      return;
    }
    if (m.t === "nosignal") setState("нет сигнала от видеокарты платформы (экран выключен или платформа не включена)", true);
    else if (m.t === "stat" && m.signal && !overlay.classList.contains("hidden") && root.querySelector("#kvm-state").textContent.startsWith("нет сигнала")) setState("подключено", false);
    else if (m.t !== "stat") setState(`прямой поток: ${m.t}`, true);
  });
  r.addEventListener("disconnect", (e) => {
    rfb = null;
    setSource(null);
    if (r._reconnect) return connect();
    if (!e.detail.opened && !e.detail.clean) {
      // never opened: most likely the certificate of the port
      if (prefs.source === "auto" && directFailed) return connect();
      setState("прямой поток не открылся", true, false, certHelp(ticket.port));
      return;
    }
    setState(e.detail.clean ? "отключено" : "соединение потеряно", true, true);
    if (!e.detail.clean && wantConnected && root) retryTimer = setTimeout(connect, 3000);
  });
}

function connectVnc() {
  let ws;
  try {
    ws = countingWebSocket(api.websocket("/kvm/0"));
  } catch (e) {
    setState(`ошибка: ${e.message}`, true, true);
    return;
  }
  const r = (rfb = new RFB(screen, ws, { shared: true, wsProtocols: [] }));
  r.background = "#000";
  r.focusOnClick = true;
  r.qualityLevel = 8;
  r.compressionLevel = 2;
  applyPrefs();

  r.addEventListener("connect", () => {
    setSource("vnc");
    setState("подключено", false);
    if (tp) tp.connected();
    toast("KVM подключён: VNC", "ok");
    r.focus();
  });
  r.addEventListener("disconnect", (e) => {
    rfb = null;
    setSource(null);
    if (r._reconnect) return connect();
    const clean = e.detail.clean;
    setState(clean ? "отключено" : "соединение потеряно", true, true);
    if (!clean && wantConnected && root) retryTimer = setTimeout(connect, 3000);
  });
  r.addEventListener("credentialsrequired", () => {
    let pw = "";
    try {
      pw = sessionStorage.getItem("reimu-vnc") || "";
    } catch (e) {
      /* ignore */
    }
    if (!pw || r._reimuTriedSaved) pw = prompt("Пароль VNC (по умолчанию 0penBmc; меняется командой vncpasswd на BMC):", "") || "";
    r._reimuTriedSaved = true;
    try {
      sessionStorage.setItem("reimu-vnc", pw);
    } catch (e) {
      /* ignore */
    }
    r.sendCredentials({ password: pw });
  });
  r.addEventListener("securityfailure", (e) => {
    try {
      sessionStorage.removeItem("reimu-vnc");
    } catch (err) {
      /* ignore */
    }
    toast(`KVM: ошибка авторизации VNC (${e.detail.reason || e.detail.status})`, "err");
    wantConnected = false;
  });
  r.addEventListener("desktopname", (e) => (document.title = `REIMU — ${e.detail.name}`));
}

function setupTouchKeyboard($) {
  const on = () => !keysBar.classList.contains("hidden");
  const updateMods = () => keysBar.refresh();
  $("kvm-kbd").addEventListener("click", () => {
    // a tap on the screen takes the focus (and the phone keyboard) away:
    // then the button brings the keyboard back instead of closing the bar
    if (on() && document.activeElement === kbd) {
      keysBar.classList.add("hidden");
      kbd.blur();
    } else {
      keysBar.classList.remove("hidden");
      kbd.focus(); // synchronously in the tap: iOS shows the keyboard only then
    }
    $("kvm-kbd").classList.toggle("active", on());
  });
  kbd.addEventListener("keydown", (e) => {
    if (e.isComposing) return;
    const sp = SPECIAL[e.key];
    if (sp) {
      e.preventDefault();
      tapKey(sp[1], sp[0]);
    } else if ((e.ctrlKey || e.altKey || e.metaKey) && e.key.length === 1) {
      // a hardware keyboard on an iPad
      e.preventDefault();
      held.ctrl ||= e.ctrlKey;
      held.alt ||= e.altKey;
      held.win ||= e.metaKey;
      tapChar(e.key);
    }
    updateMods();
  });
  let warned = false;
  kbd.addEventListener("input", () => {
    const t = kbd.value;
    kbd.value = "";
    let bad = 0;
    for (const ch of t) {
      if (ch === "\n") tapKey(K.XK_Return, "Enter");
      else if (/[\x20-\x7e]/.test(ch)) tapChar(ch);
      else bad++;
    }
    if (bad && !warned) {
      warned = true;
      toast("KVM передаёт только латиницу (US-раскладка): переключите клавиатуру телефона на English", "err");
    }
    updateMods();
  });

  // the iOS keyboard covers the page instead of resizing it: lift the
  // screen and the key bar above it
  if (window.visualViewport) {
    const vv = window.visualViewport;
    const lift = () => {
      if (!root) return;
      const covered = Math.max(0, Math.round(window.innerHeight - vv.height - vv.offsetTop));
      document.getElementById("page").style.paddingBottom = covered > 40 ? `${covered}px` : "";
    };
    vv.addEventListener("resize", lift);
    vv.addEventListener("scroll", lift);
    root._unlift = () => {
      vv.removeEventListener("resize", lift);
      vv.removeEventListener("scroll", lift);
      document.getElementById("page").style.paddingBottom = "";
    };
  }
}

function pressBarKey(k) {
  if (k in held) held[k] = !held[k];
  else if (SPECIAL[k]) tapKey(SPECIAL[k][1], SPECIAL[k][0]);
}

// one key press, wrapped in the held modifiers, which are then released
function tapKey(sym, code) {
  if (!rfb || prefs.viewOnly) return;
  const mods = Object.keys(MODS).filter((m) => held[m]);
  for (const m of mods) rfb.sendKey(MODS[m][1], MODS[m][0], true);
  rfb.sendKey(sym, code, true);
  rfb.sendKey(sym, code, false);
  for (const m of mods.reverse()) rfb.sendKey(MODS[m][1], MODS[m][0], false);
  for (const m of mods) held[m] = false;
}

function tapChar(ch) {
  // Ctrl+C is Ctrl with "c", whatever case the phone typed
  tapKey((held.ctrl || held.alt ? ch.toLowerCase() : ch).charCodeAt(0), null);
}

function disconnect() {
  wantConnected = false;
  clearTimeout(retryTimer);
  if (rfb) rfb.disconnect();
}

function sendCombo(c) {
  if (!rfb) return;
  for (const [code, sym] of c.keys) rfb.sendKey(sym, code, true);
  for (const [code, sym] of [...c.keys].reverse()) rfb.sendKey(sym, code, false);
  rfb.focus();
}

// Types text as key presses. obmc-ikvm maps keysyms to a US keyboard, so
// only printable ASCII, Tab and Enter are reliable.
async function pasteText() {
  if (!rfb) return;
  let text = "";
  try {
    // the permission prompt may never be answered: don't wait for it forever
    text = await Promise.race([navigator.clipboard.readText(), new Promise((r) => setTimeout(() => r(""), 1500))]);
  } catch (e) {
    /* clipboard may be unavailable over plain http or denied */
  }
  text = prompt("Текст для набора на платформе (ASCII):", text);
  if (!text) return;
  const bad = [...text].filter((ch) => !/[\x20-\x7e\t\n]/.test(ch));
  if (bad.length) toast(`Пропущено символов вне ASCII: ${bad.length}`, "err");
  for (const ch of text) {
    if (!rfb) break;
    if (ch === "\n") {
      rfb.sendKey(KeyTable.XK_Return, "Enter");
    } else if (ch === "\t") {
      rfb.sendKey(KeyTable.XK_Tab, "Tab");
    } else if (/[\x20-\x7e]/.test(ch)) {
      rfb.sendKey(ch.charCodeAt(0), null);
    }
    await new Promise((r) => setTimeout(r, 15));
  }
  // give the keyboard back to the remote screen
  if (rfb) rfb.focus();
}

function screenshot() {
  const canvas = screen.querySelector("canvas");
  if (!canvas) return;
  canvas.toBlob((blob) => {
    const a = h("a", { href: URL.createObjectURL(blob), download: `screen-${new Date().toISOString().replace(/[:.]/g, "-")}.png` });
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  }, "image/png");
}

function updateStats() {
  if (!root) return;
  const $ = (id) => root.querySelector("#" + id);
  const direct = source === "direct" || source === "h264";
  const frames = direct ? directStats.frames : accelStats.frames;
  const bytes = direct ? directStats.bytes : st.bytes;
  st.fps = frames - st.lastFrames;
  st.lastFrames = frames;
  st.bps = bytes - st.lastBytes;
  st.lastBytes = bytes;
  const srv = direct && directStats.server;
  $("kvm-fps").textContent = rfb
    ? direct
      ? `${st.fps}${srv && source === "direct" ? ` / ${Math.round(srv.cap)}` : ""}`
      : `${st.fps}${accelStats.dropped ? ` (пропущено ${accelStats.dropped})` : ""}`
    : "0";
  $("kvm-bw").textContent = rfb ? `${((st.bps * 8) / 1e6).toFixed(1)} Мбит/с` : "—";
  const canvas = screen.querySelector("canvas");
  $("kvm-res").textContent = rfb && canvas && rfb._fbWidth ? `${rfb._fbWidth}×${rfb._fbHeight}` : "—";
  if (rfb) $("kvm-dec").textContent = `асинхронный, ${(direct ? directStats.decodeMs : accelStats.decodeMs).toFixed(1)} мс/кадр`;
}

export function unmount() {
  if (root && root._unlift) root._unlift();
  window.removeEventListener("message", onCertMessage);
  disconnect();
  clearInterval(statTimer);
  if (tp) tp.destroy();
  tp = null;
  root = null;
}
