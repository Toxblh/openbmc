// Remote screen (KVM-over-IP) in the browser: noVNC over bmcweb's /kvm/0
// websocket to obmc-ikvm, which streams hardware-compressed JPEG frames
// from the AST2500 video engine and injects keyboard/mouse via USB HID.

import { api, h, toast } from "../api.js";
import { RFB, KeyTable } from "../../vendor/novnc/novnc.js";
import { installAccel, accelStats } from "../lib/novnc-accel.js";

export const fill = true;

let root, rfb, screen, overlay, statTimer, kbd, keysBar;
let wantConnected = false;
let retryTimer = null;
const st = { frames: 0, lastFrames: 0, fps: 0, bytes: 0, lastBytes: 0, bps: 0 };

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
  ["Esc", "Escape"],
  ["Tab", "Tab"],
  ["Ctrl", "ctrl"],
  ["Alt", "alt"],
  ["Win", "win"],
  ["←", "ArrowLeft"],
  ["↑", "ArrowUp"],
  ["↓", "ArrowDown"],
  ["→", "ArrowRight"],
  ["Del", "Delete"],
  ["Home", "Home"],
  ["End", "End"],
  ["PgUp", "PageUp"],
  ["PgDn", "PageDown"],
  ...[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12].map((n) => [`F${n}`, `F${n}`]),
];
const held = { ctrl: false, alt: false, win: false };

function getPrefs() {
  try {
    return Object.assign({ scale: true, viewOnly: false, dot: true }, JSON.parse(localStorage.getItem("reimu-kvm") || "{}"));
  } catch (e) {
    return { scale: true, viewOnly: false, dot: true };
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
      h("button", { class: "btn small touch-only", id: "kvm-kbd", title: "Клавиатура телефона и панель клавиш" }, "⌨ Клавиатура"),
      btn("kvm-connect", "Подключить"),
      btn("kvm-disconnect", "Отключить"),
      h("span", { class: "spacer" }),
      h(
        "div",
        { class: "dropdown" },
        h("button", { class: "btn small" }, "Клавиши ▾"),
        h("div", { class: "dropdown-menu", id: "kvm-combos" }, COMBOS.map((c, i) => h("button", { "data-i": i }, c.label))),
      ),
      btn("kvm-paste", "Вставить текст", "Напечатать текст из буфера на платформе (только латиница/ASCII)"),
      btn("kvm-shot", "Снимок", "Сохранить снимок экрана в PNG"),
      btn("kvm-scale", "Масштаб", "Вписать в окно / 1:1"),
      btn("kvm-view", "Только просмотр", "Не передавать клавиатуру и мышь"),
      btn("kvm-full", "⛶ Полный экран"),
    ),
    (screen = h("div", { class: "kvm-screen", id: "kvm-screen" }, (overlay = h("div", { class: "kvm-overlay" }, "Нет подключения")))),
    (keysBar = h(
      "div",
      { class: "kvm-keys hidden", id: "kvm-keys" },
      KEYBAR.map(([label, k]) => h("button", { class: "btn small", "data-k": k }, label)),
    )),
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
      h("span", {}, "Разрешение: ", h("b", { id: "kvm-res" }, "—")),
      h("span", {}, "Кадров/с: ", h("b", { id: "kvm-fps" }, "0")),
      h("span", { class: "m-hide" }, "Поток: ", h("b", { id: "kvm-bw" }, "—")),
      h("span", { class: "m-hide", title: "Декодирование JPEG браузером вне основного потока (createImageBitmap)" }, "Декодер: ", h("b", { id: "kvm-dec" }, accelStats.enabled ? "аппаратный/асинхронный" : "стандартный")),
      h("span", { class: "spacer" }),
      h("span", { class: "muted m-hide" }, "Сжатие JPEG — видеодвижок AST2500"),
      coarse ? h("span", { class: "muted" }, "тап — клик, 2 пальца — правый, прокрутка — 2 пальцами") : null,
    ),
  );
  const $ = (id) => el.querySelector("#" + id);
  $("kvm-connect").addEventListener("click", () => connect());
  $("kvm-disconnect").addEventListener("click", () => disconnect());
  $("kvm-combos").addEventListener("click", (e) => {
    const b = e.target.closest("button[data-i]");
    if (b) sendCombo(COMBOS[b.dataset.i]);
  });
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
  if (rfb) {
    rfb.scaleViewport = prefs.scale;
    rfb.clipViewport = !prefs.scale;
    rfb.viewOnly = prefs.viewOnly;
    // 1:1 on a touch screen: a finger pans the picture instead of dragging the mouse
    rfb.dragViewport = coarse && !prefs.scale;
    rfb.showDotCursor = prefs.dot;
  }
}

function setState(text, showOverlay, withButton) {
  if (!root) return;
  root.querySelector("#kvm-state").textContent = text;
  overlay.classList.toggle("hidden", !showOverlay);
  overlay.replaceChildren(text);
  if (withButton) {
    const b = h("button", { class: "btn primary" }, "Подключить");
    b.addEventListener("click", () => connect());
    overlay.append(b);
  }
}

// Counts bytes received on the KVM websocket (for the bandwidth indicator)
// without touching noVNC internals: wrap the socket's message delivery.
function countingWebSocket(ws) {
  ws.addEventListener("message", (ev) => {
    st.bytes += ev.data.byteLength || ev.data.length || 0;
  });
  return ws;
}

function connect() {
  wantConnected = true;
  clearTimeout(retryTimer);
  if (rfb) return;
  setState("подключение…", true);
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
    setState("подключено", false);
    toast("KVM подключён", "ok");
    r.focus();
  });
  r.addEventListener("disconnect", (e) => {
    rfb = null;
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
  const updateMods = () => keysBar.querySelectorAll("[data-k]").forEach((b) => b.classList.toggle("active", !!held[b.dataset.k]));
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
  // keep the phone keyboard up while the key bar is tapped
  const keep = (e) => e.preventDefault();
  keysBar.addEventListener("mousedown", keep);
  keysBar.addEventListener("touchstart", (e) => e.target.closest("button") && keep(e), { passive: false });
  keysBar.addEventListener("touchend", (e) => {
    const b = e.target.closest("button[data-k]");
    if (!b) return;
    e.preventDefault();
    pressBarKey(b.dataset.k, updateMods);
  });
  keysBar.addEventListener("click", (e) => {
    const b = e.target.closest("button[data-k]");
    if (b) pressBarKey(b.dataset.k, updateMods);
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

function pressBarKey(k, updateMods) {
  if (k in held) held[k] = !held[k];
  else if (SPECIAL[k]) tapKey(SPECIAL[k][1], SPECIAL[k][0]);
  updateMods();
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
  const frames = accelStats.frames;
  st.fps = frames - st.lastFrames;
  st.lastFrames = frames;
  st.bps = st.bytes - st.lastBytes;
  st.lastBytes = st.bytes;
  $("kvm-fps").textContent = rfb ? `${st.fps}${accelStats.dropped ? ` (пропущено ${accelStats.dropped})` : ""}` : "0";
  $("kvm-bw").textContent = rfb ? `${((st.bps * 8) / 1e6).toFixed(1)} Мбит/с` : "—";
  const canvas = screen.querySelector("canvas");
  $("kvm-res").textContent = rfb && canvas ? `${rfb._fbWidth}×${rfb._fbHeight}` : "—";
  if (accelStats.enabled && rfb) $("kvm-dec").textContent = `асинхронный, ${accelStats.decodeMs.toFixed(1)} мс/кадр`;
}

export function unmount() {
  if (root && root._unlift) root._unlift();
  disconnect();
  clearInterval(statTimer);
  root = null;
}
