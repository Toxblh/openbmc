// Remote screen (KVM-over-IP) in the browser: noVNC over bmcweb's /kvm/0
// websocket to obmc-ikvm, which streams hardware-compressed JPEG frames
// from the AST2500 video engine and injects keyboard/mouse via USB HID.

import { api, h, toast } from "../api.js";
import { RFB, KeyTable } from "../../vendor/novnc/novnc.js";
import { installAccel, accelStats } from "../lib/novnc-accel.js";

export const fill = true;

let root, rfb, screen, overlay, statTimer;
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
  el.append(
    h(
      "div",
      { class: "kvm-toolbar" },
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
    h(
      "div",
      { class: "statusbar" },
      h("span", {}, "Состояние: ", h("b", { id: "kvm-state" }, "отключено")),
      h("span", {}, "Разрешение: ", h("b", { id: "kvm-res" }, "—")),
      h("span", {}, "Кадров/с: ", h("b", { id: "kvm-fps" }, "0")),
      h("span", {}, "Поток: ", h("b", { id: "kvm-bw" }, "—")),
      h("span", { title: "Декодирование JPEG браузером вне основного потока (createImageBitmap)" }, "Декодер: ", h("b", { id: "kvm-dec" }, accelStats.enabled ? "аппаратный/асинхронный" : "стандартный")),
      h("span", { class: "spacer" }),
      h("span", { class: "muted" }, "Сжатие JPEG — видеодвижок AST2500"),
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
  disconnect();
  clearInterval(statTimer);
  root = null;
}
