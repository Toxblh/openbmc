// Direct KVM stream from reimu-kvm on the BMC: every JPEG frame of the AST2500
// video engine pushed as one websocket message (wss://BMC:8443), keyboard and
// mouse sent back as USB HID usages. Several times the frame rate of VNC
// through bmcweb, which pulls frame by frame through a proxy.
//
// While the platform's OS runs, the picture can come instead from reimu-h264 on
// the host: the desktop encoded as H.264 by its video card (60 frames/s at a few
// Mbit/s), relayed by reimu-kvm and decoded here by the browser's hardware
// decoder (WebCodecs). When that stream ends, reimu-kvm sends JPEG again.
//
// DirectKvm mimics the bits of noVNC's RFB the KVM page and the touchpad use
// (sendKey, viewOnly, scaleViewport, _handleMouseMove/_handleMouseButton,
// _cursor, the connect/disconnect events), so both sources share the page.

import { api } from "../api.js";

// KeyboardEvent.code -> USB HID usage (keyboard page)
const USAGE = {
  Enter: 0x28, Escape: 0x29, Backspace: 0x2a, Tab: 0x2b, Space: 0x2c, Minus: 0x2d, Equal: 0x2e,
  BracketLeft: 0x2f, BracketRight: 0x30, Backslash: 0x31, Semicolon: 0x33, Quote: 0x34, Backquote: 0x35,
  Comma: 0x36, Period: 0x37, Slash: 0x38, CapsLock: 0x39, PrintScreen: 0x46, ScrollLock: 0x47, Pause: 0x48,
  Insert: 0x49, Home: 0x4a, PageUp: 0x4b, Delete: 0x4c, End: 0x4d, PageDown: 0x4e,
  ArrowRight: 0x4f, ArrowLeft: 0x50, ArrowDown: 0x51, ArrowUp: 0x52, NumLock: 0x53,
  NumpadDivide: 0x54, NumpadMultiply: 0x55, NumpadSubtract: 0x56, NumpadAdd: 0x57, NumpadEnter: 0x58,
  NumpadDecimal: 0x63, IntlBackslash: 0x64, ContextMenu: 0x65,
  ControlLeft: 0xe0, ShiftLeft: 0xe1, AltLeft: 0xe2, MetaLeft: 0xe3, ControlRight: 0xe4, ShiftRight: 0xe5, AltRight: 0xe6, MetaRight: 0xe7,
};
for (let i = 0; i < 26; i++) USAGE[`Key${String.fromCharCode(65 + i)}`] = 0x04 + i;
for (let i = 1; i <= 9; i++) USAGE[`Digit${i}`] = 0x1d + i;
USAGE.Digit0 = 0x27;
for (let i = 1; i <= 12; i++) USAGE[`F${i}`] = 0x39 + i;
for (let i = 1; i <= 9; i++) USAGE[`Numpad${i}`] = 0x58 + i;
USAGE.Numpad0 = 0x62;

// X keysyms the page sends without a code (typed text, the key bar)
const KEYSYM = {
  0xff0d: "Enter", 0xff1b: "Escape", 0xff08: "Backspace", 0xff09: "Tab", 0xffff: "Delete", 0xff63: "Insert",
  0xff50: "Home", 0xff57: "End", 0xff55: "PageUp", 0xff56: "PageDown",
  0xff51: "ArrowLeft", 0xff52: "ArrowUp", 0xff53: "ArrowRight", 0xff54: "ArrowDown",
  0xffe3: "ControlLeft", 0xffe9: "AltLeft", 0xffeb: "MetaLeft", 0xffe1: "ShiftLeft", 0xff61: "PrintScreen", 0xff15: "PrintScreen",
};
for (let i = 1; i <= 12; i++) KEYSYM[0xffbd + i] = `F${i}`;

// printable ASCII on a US keyboard: [code, shift]
const ASCII = {};
const row = (plain, shifted, codes) => [...plain].forEach((ch, i) => { ASCII[ch] = [codes[i], false]; ASCII[shifted[i]] = [codes[i], true]; });
row("1234567890", "!@#$%^&*()", ["Digit1", "Digit2", "Digit3", "Digit4", "Digit5", "Digit6", "Digit7", "Digit8", "Digit9", "Digit0"]);
row("-=[]\\;',./`", "_+{}|:\"<>?~", ["Minus", "Equal", "BracketLeft", "BracketRight", "Backslash", "Semicolon", "Quote", "Comma", "Period", "Slash", "Backquote"]);
for (let i = 0; i < 26; i++) {
  ASCII[String.fromCharCode(97 + i)] = [`Key${String.fromCharCode(65 + i)}`, false];
  ASCII[String.fromCharCode(65 + i)] = [`Key${String.fromCharCode(65 + i)}`, true];
}
ASCII[" "] = ["Space", false];

export const directStats = { frames: 0, bytes: 0, dropped: 0, decodeMs: 0, server: null, codec: "jpeg" };

export const h264Supported = () => typeof VideoDecoder === "function" && typeof EncodedVideoChunk === "function";

// a ticket for one connection, or null when reimu-kvm does not run
export async function directTicket() {
  try {
    const r = await api.post("/xyz/openbmc_project/reimu/kvm/action/Ticket", { data: [] });
    return JSON.parse(r.data);
  } catch (e) {
    return null;
  }
}

export class DirectKvm extends EventTarget {
  constructor(screen, ticket) {
    super();
    this._screen = screen;
    this._rfbConnectionState = "connecting";
    this._fbWidth = 0;
    this._fbHeight = 0;
    this.viewOnly = false;
    this.scaleViewport = true;
    this.clipViewport = false;
    this.dragViewport = false;
    this.showDotCursor = false;
    this.focusOnClick = true;
    this.background = "#000";
    this._buttons = 0;
    this._down = new Set();
    this._decoding = false;
    this._next = null;

    this._wrap = document.createElement("div");
    this._wrap.className = "dk-wrap";
    this._canvas = document.createElement("canvas");
    this._canvas.className = "dk-canvas";
    this._canvas.tabIndex = 0;
    this._ctx = this._canvas.getContext("2d", { alpha: false });
    this._cursorEl = document.createElement("i");
    this._cursorEl.className = "dk-cursor hidden";
    this._wrap.append(this._canvas, this._cursorEl);
    screen.prepend(this._wrap);
    this._cursor = { move: (cx, cy) => this._moveCursor(cx, cy) };
    this._bindInput();

    const ws = (this._ws = new WebSocket(`wss://${location.hostname}:${ticket.port}/kvm?t=${ticket.ticket}`));
    ws.binaryType = "arraybuffer";
    ws.onopen = () => {
      this._rfbConnectionState = "connected";
      this._applyView();
      this.dispatchEvent(new CustomEvent("connect"));
    };
    ws.onclose = (e) => {
      this._h264Stop();
      const was = this._rfbConnectionState;
      this._rfbConnectionState = "disconnected";
      this._unbindInput();
      this._wrap.remove();
      this.dispatchEvent(new CustomEvent("disconnect", { detail: { clean: this._closing || false, opened: was === "connected", code: e.code } }));
    };
    ws.onmessage = (e) => this._message(e.data);
  }

  _message(data) {
    if (typeof data === "string") {
      let m;
      try {
        m = JSON.parse(data);
      } catch (e) {
        return;
      }
      if (m.t === "stat") directStats.server = m;
      if (m.t === "h264") this._h264Start(m);
      if (m.t === "h264off") this._h264Stop();
      this.dispatchEvent(new CustomEvent("status", { detail: m }));
      return;
    }
    const d = new DataView(data);
    if (d.getUint8(0) === 6) return this._h264Frame(d.getUint8(1), new Uint8Array(data, 2), data.byteLength);
    if (d.getUint8(0) !== 1) return;
    directStats.bytes += data.byteLength;
    const w = d.getUint16(1, true), hgt = d.getUint16(3, true);
    const blob = new Blob([new Uint8Array(data, 9)], { type: "image/jpeg" });
    // decode one frame at a time; a newer frame replaces a waiting one
    if (this._next) directStats.dropped++;
    this._next = { blob, w, h: hgt };
    if (!this._decoding) this._decode();
  }

  async _decode() {
    this._decoding = true;
    while (this._next && this._rfbConnectionState === "connected") {
      const f = this._next;
      this._next = null;
      const t0 = performance.now();
      let bmp;
      try {
        bmp = await createImageBitmap(f.blob);
      } catch (e) {
        continue;
      }
      directStats.decodeMs = directStats.decodeMs * 0.9 + (performance.now() - t0) * 0.1;
      if (this._canvas.width !== bmp.width || this._canvas.height !== bmp.height) {
        this._canvas.width = this._fbWidth = bmp.width;
        this._canvas.height = this._fbHeight = bmp.height;
        this._applyView();
      }
      this._ctx.drawImage(bmp, 0, 0);
      bmp.close();
      directStats.frames++;
    }
    this._decoding = false;
  }

  // ---- H.264 from the host
  // ask reimu-kvm to take the host's stream ("host:port" of reimu-h264)
  useHost(addr) {
    if (this._ws.readyState !== 1) return;
    this._ws.send(new Uint8Array([7, ...new TextEncoder().encode(addr)]));
  }

  useEngine() {
    if (this._ws.readyState === 1) this._ws.send(new Uint8Array([8]));
    this._h264Stop();
  }

  _h264Start(m) {
    this._h264Stop();
    this._needKey = true;
    this._ts = 0;
    const dec = (this._dec = new VideoDecoder({
      output: (frame) => {
        if (this._dec !== dec) return frame.close();
        const w = frame.displayWidth, hh = frame.displayHeight;
        if (this._canvas.width !== w || this._canvas.height !== hh) {
          this._canvas.width = this._fbWidth = w;
          this._canvas.height = this._fbHeight = hh;
          this._applyView();
        }
        this._ctx.drawImage(frame, 0, 0);
        frame.close();
        directStats.frames++;
        if (this._t0.length) directStats.decodeMs = directStats.decodeMs * 0.9 + (performance.now() - this._t0.shift()) * 0.1;
      },
      error: (e) => {
        // a broken frame: wait for the next key frame (one a second)
        console.warn("H.264 decoder:", e.message);
        if (this._dec === dec) this._h264Start(m);
      },
    }));
    this._t0 = [];
    // Annex B input: no description
    dec.configure({ codec: m.codec, optimizeForLatency: true, hardwareAcceleration: "prefer-hardware" });
    directStats.codec = "h264";
  }

  _h264Stop() {
    if (this._dec) {
      try {
        this._dec.close();
      } catch (e) {
        /* closed */
      }
    }
    this._dec = null;
    directStats.codec = "jpeg";
  }

  _h264Frame(key, au, size) {
    const dec = this._dec;
    if (!dec || dec.state !== "configured") return;
    directStats.bytes += size;
    if (this._needKey && !key) return;
    // the decoder falls behind: let it catch up at the next key frame
    if (dec.decodeQueueSize > 4 && !key) {
      directStats.dropped++;
      this._needKey = true;
      return;
    }
    this._needKey = false;
    this._t0.push(performance.now());
    if (this._t0.length > 30) this._t0.shift();
    dec.decode(new EncodedVideoChunk({ type: key ? "key" : "delta", timestamp: (this._ts += 16667), data: au }));
  }

  // ---- view
  _applyView() {
    this._wrap.classList.toggle("dk-fit", !!this.scaleViewport);
  }

  // canvas content box in client coordinates (the picture is letterboxed when fitted)
  _box() {
    const r = this._canvas.getBoundingClientRect();
    const w = this._fbWidth || 1, h = this._fbHeight || 1;
    const s = Math.min(r.width / w, r.height / h);
    const bw = w * s, bh = h * s;
    return { left: r.left + (r.width - bw) / 2, top: r.top + (r.height - bh) / 2, width: bw, height: bh };
  }

  _moveCursor(cx, cy) {
    this._cursorEl.classList.remove("hidden");
    const w = this._wrap.getBoundingClientRect();
    this._cursorEl.style.transform = `translate(${cx - w.left}px, ${cy - w.top}px)`;
  }

  // ---- output
  _send(bytes) {
    if (this._ws.readyState === 1 && !this.viewOnly) this._ws.send(new Uint8Array(bytes));
  }

  _key(code, down) {
    const u = USAGE[code];
    if (!u) return;
    if (down) this._down.add(code);
    else this._down.delete(code);
    this._send([2, u, down ? 1 : 0]);
  }

  // noVNC's sendKey(keysym, code, down): without down, a press and a release
  sendKey(sym, code, down) {
    if (this.viewOnly) return;
    let shift = false;
    if (!code || !USAGE[code]) {
      if (KEYSYM[sym]) code = KEYSYM[sym];
      else if (sym >= 0x20 && sym < 0x7f && ASCII[String.fromCharCode(sym)]) [code, shift] = ASCII[String.fromCharCode(sym)];
      else return;
    }
    const press = (d) => {
      if (shift && d) this._key("ShiftLeft", true);
      this._key(code, d);
      if (shift && !d) this._key("ShiftLeft", false);
    };
    if (down === undefined) {
      press(true);
      press(false);
    } else press(down);
  }

  // x, y: CSS px over the canvas element (as noVNC gets them)
  _pointer(x, y) {
    const r = this._canvas.getBoundingClientRect();
    const b = this._box();
    const px = (r.left + x - b.left) / b.width, py = (r.top + y - b.top) / b.height;
    const cl = (v) => Math.round(Math.min(1, Math.max(0, v)) * 32767);
    return [cl(px), cl(py)];
  }

  _sendPointer(x, y) {
    this._send([3, this._buttons, x & 0xff, x >> 8, y & 0xff, y >> 8]);
  }

  _handleMouseMove(x, y) {
    const [px, py] = this._pointer(x, y);
    this._last = [px, py];
    this._sendPointer(px, py);
  }

  // mask as noVNC: 1 left, 2 middle, 4 right, 8/16 wheel up/down (the touchpad's
  // two-finger scroll); the wheel is a second USB mouse on the BMC
  _handleMouseButton(x, y, mask) {
    const [px, py] = this._pointer(x, y);
    if (mask & 24 && !(this._wheelMask & 24)) this._wheel(mask & 8 ? 1 : -1, 0);
    this._wheelMask = mask;
    this._buttons = mask & 7;
    this._sendPointer(px, py);
  }

  // clicks: wheel > 0 up, pan > 0 right
  _wheel(wheel, pan) {
    const cl = (v) => Math.max(-127, Math.min(127, v)) & 0xff;
    this._send([9, cl(wheel), cl(pan)]);
  }

  // ---- input from this browser's keyboard and mouse
  _bindInput() {
    const c = this._canvas;
    const local = (e) => {
      const r = c.getBoundingClientRect();
      return [e.clientX - r.left, e.clientY - r.top];
    };
    const btnMask = (b) => (b & 1) | ((b & 4) >> 1) | ((b & 2) << 1); // browser buttons -> noVNC mask
    this._h = {
      keydown: (e) => {
        if (this.viewOnly || !USAGE[e.code]) return;
        e.preventDefault();
        // a held HID key repeats on the host by itself
        if (!e.repeat) this._key(e.code, true);
      },
      keyup: (e) => {
        if (!USAGE[e.code]) return;
        e.preventDefault();
        this._key(e.code, false);
      },
      blur: () => this.releaseAll(),
      pointermove: (e) => {
        if (e.pointerType === "touch" && this._touchpadOwned) return;
        const [x, y] = local(e);
        this._handleMouseMove(x, y);
      },
      pointerdown: (e) => {
        if (this.focusOnClick) c.focus({ preventScroll: true });
        if (e.pointerType === "touch" && this._touchpadOwned) return;
        e.preventDefault();
        c.setPointerCapture(e.pointerId);
        const [x, y] = local(e);
        this._handleMouseButton(x, y, btnMask(e.buttons));
      },
      pointerup: (e) => {
        if (e.pointerType === "touch" && this._touchpadOwned) return;
        const [x, y] = local(e);
        this._handleMouseButton(x, y, btnMask(e.buttons));
      },
      contextmenu: (e) => e.preventDefault(),
      wheel: (e) => {
        if (this.viewOnly) return;
        e.preventDefault();
        // a mouse notch is 100 px (Chrome) or 3 lines (Firefox); trackpads add up
        const step = e.deltaMode === 1 ? 3 : e.deltaMode === 2 ? 0.3 : 100;
        const acc = (this._wheelAcc ||= [0, 0]);
        acc[0] += e.deltaY / step;
        acc[1] += e.deltaX / step;
        const w = Math.trunc(acc[0]), p = Math.trunc(acc[1]);
        if (!w && !p) return;
        acc[0] -= w;
        acc[1] -= p;
        this._wheel(-w, p);
      },
    };
    c.addEventListener("keydown", this._h.keydown);
    c.addEventListener("keyup", this._h.keyup);
    c.addEventListener("blur", this._h.blur);
    c.addEventListener("pointermove", this._h.pointermove);
    c.addEventListener("pointerdown", this._h.pointerdown);
    c.addEventListener("pointerup", this._h.pointerup);
    c.addEventListener("contextmenu", this._h.contextmenu);
    c.addEventListener("wheel", this._h.wheel, { passive: false });
  }

  _unbindInput() {
    const c = this._canvas;
    for (const [k, f] of Object.entries(this._h || {})) c.removeEventListener(k, f);
  }

  releaseAll() {
    this._down.clear();
    this._buttons = 0;
    if (this._ws.readyState === 1) this._ws.send(new Uint8Array([4]));
  }

  // fps 1..60, quality 0..11, sub420 true/false; undefined keeps the setting
  settings({ fps, quality, sub420 } = {}) {
    if (this._ws.readyState !== 1) return;
    this._ws.send(new Uint8Array([5, fps || 0, quality === undefined ? 0 : quality + 1, sub420 === undefined ? 0 : sub420 ? 2 : 1]));
  }

  focus() {
    this._canvas.focus({ preventScroll: true });
  }

  disconnect() {
    this._closing = true;
    try {
      this.releaseAll();
    } catch (e) {
      /* closed already */
    }
    this._ws.close();
  }
}
