// Client for reimu-serial websocket (/reimu/serial/<N>), newline-delimited
// JSON protocol (see reimu-serial.c). Keeps a client-side ring of
// timestamped lines and emits raw byte chunks for terminal emulators.

import { api } from "../api.js";

const b64dec = (s) => {
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
};
const b64enc = (bytes) => {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(s);
};

const utf8 = new TextDecoder("utf-8");
const encoder = new TextEncoder();

// Makes a raw serial line readable: drops ANSI/VT escape sequences and
// control characters, emulates carriage return overwrite (progress bars).
// eslint-disable-next-line no-control-regex
const ANSI_RE = /\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07\x1b]*(?:\x07|\x1b\\)|[PX^_][^\x1b]*\x1b\\|[()][0-9A-Za-z]|[@-Z\\-_])/g;
// eslint-disable-next-line no-control-regex
const CTRL_RE = /[\x00-\x08\x0b-\x1f\x7f]/g;
export function cleanLine(text) {
  let t = text.replace(ANSI_RE, "");
  if (t.includes("\r")) {
    // each segment after CR overwrites the beginning of the line
    let out = "";
    for (const seg of t.split("\r")) out = seg + out.slice(seg.length);
    t = out;
  }
  return t.replace(CTRL_RE, "");
}

export class SerialClient extends EventTarget {
  constructor(index) {
    super();
    this.index = index;
    this.info = null;
    this.lines = []; // {t, raw: string(decoded utf8, untouched), text: cleaned}
    this.maxLines = 20000;
    this.partial = { t: 0, bytes: [] };
    this.connected = false;
    this.historyDone = false;
    this.closedByUser = false;
    this.retry = 0;
    this._rx = "";
    this._dec = new TextDecoder("utf-8");
    this.stats = { rxBytes: 0, txBytes: 0 };
  }

  connect() {
    this.closedByUser = false;
    this.historyDone = false;
    const ws = (this.ws = api.websocket(`/reimu/serial/${this.index}`));
    ws.onopen = () => {
      this.connected = true;
      this.retry = 0;
      this._emit("connection", { connected: true });
    };
    ws.onmessage = (ev) => this._onData(ev.data);
    ws.onclose = (ev) => {
      this.connected = false;
      this._emit("connection", { connected: false, reason: ev.reason });
      if (!this.closedByUser) {
        const delay = Math.min(10000, 500 * 2 ** this.retry++);
        this._timer = setTimeout(() => this.connect(), delay);
      }
    };
  }

  close() {
    this.closedByUser = true;
    clearTimeout(this._timer);
    if (this.ws) this.ws.close();
  }

  _emit(type, detail) {
    this.dispatchEvent(new CustomEvent(type, { detail }));
  }

  _onData(data) {
    this._rx += typeof data === "string" ? data : this._dec.decode(new Uint8Array(data), { stream: true });
    let nl;
    while ((nl = this._rx.indexOf("\n")) >= 0) {
      const line = this._rx.slice(0, nl);
      this._rx = this._rx.slice(nl + 1);
      if (!line) continue;
      let msg;
      try {
        msg = JSON.parse(line);
      } catch (e) {
        console.warn("serial: bad message", e);
        continue;
      }
      this._onMessage(msg);
    }
  }

  _pushLine(t, bytes) {
    const raw = utf8.decode(bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes));
    const l = { t, raw: raw.endsWith("\r") ? raw.slice(0, -1) : raw };
    l.text = cleanLine(l.raw);
    this.lines.push(l);
    if (this.lines.length > this.maxLines) this.lines.splice(0, this.lines.length - this.maxLines);
    return l;
  }

  _onMessage(m) {
    switch (m.type) {
      case "hello":
        this.info = m;
        this.maxLines = m.maxLines || 20000;
        this.lines = [];
        this._emit("hello", m);
        break;
      case "history":
        for (const [t, d] of m.lines) this._pushLine(t, b64dec(d));
        break;
      case "history_end": {
        const [t, d] = m.partial;
        this.partial = { t, bytes: Array.from(b64dec(d)) };
        this.historyDone = true;
        this._emit("history", { count: this.lines.length });
        break;
      }
      case "data": {
        const bytes = b64dec(m.d);
        this.stats.rxBytes += bytes.length;
        const added = [];
        for (const b of bytes) {
          if (!this.partial.bytes.length) this.partial.t = m.t;
          if (b === 10) {
            added.push(this._pushLine(this.partial.t, this.partial.bytes));
            this.partial = { t: 0, bytes: [] };
          } else {
            this.partial.bytes.push(b);
            if (this.partial.bytes.length >= 2048) {
              added.push(this._pushLine(this.partial.t, this.partial.bytes));
              this.partial = { t: 0, bytes: [] };
            }
          }
        }
        this._emit("data", { t: m.t, bytes, added });
        break;
      }
      case "status":
        if (this.info) Object.assign(this.info, { baud: m.baud, online: m.online });
        this._emit("status", m);
        break;
      case "cleared":
        this.lines = [];
        this.partial = { t: 0, bytes: [] };
        this._emit("cleared", {});
        break;
      case "pong":
        this._emit("pong", m);
        break;
    }
  }

  partialLine() {
    if (!this.partial.bytes.length) return null;
    const raw = utf8.decode(new Uint8Array(this.partial.bytes));
    return { t: this.partial.t, raw, text: cleanLine(raw), partial: true };
  }

  _send(obj) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(obj) + "\n");
      return true;
    }
    return false;
  }

  write(data) {
    const bytes = typeof data === "string" ? encoder.encode(data) : data;
    if (!bytes.length) return;
    this.stats.txBytes += bytes.length;
    this._send({ type: "write", d: b64enc(bytes) });
  }

  sendBreak() {
    this._send({ type: "break" });
  }

  setBaud(baud) {
    this._send({ type: "baud", value: Number(baud) });
  }

  clearHistory() {
    this._send({ type: "clear" });
  }
}

// Converts browser key events to the byte sequences a VT100/xterm-like
// terminal would send. Used by the log view in "type into the port" mode.
export function keyToBytes(e) {
  if (e.isComposing) return null;
  const k = e.key;
  if (e.ctrlKey && !e.altKey && !e.metaKey && k.length === 1) {
    const c = k.toUpperCase().charCodeAt(0);
    if (c >= 64 && c <= 95) return String.fromCharCode(c - 64);
    if (k === " ") return "\x00";
    return null;
  }
  const map = {
    Enter: "\r",
    Backspace: "\x7f",
    Tab: "\t",
    Escape: "\x1b",
    ArrowUp: "\x1b[A",
    ArrowDown: "\x1b[B",
    ArrowRight: "\x1b[C",
    ArrowLeft: "\x1b[D",
    Home: "\x1b[H",
    End: "\x1b[F",
    Insert: "\x1b[2~",
    Delete: "\x1b[3~",
    PageUp: "\x1b[5~",
    PageDown: "\x1b[6~",
    F1: "\x1bOP",
    F2: "\x1bOQ",
    F3: "\x1bOR",
    F4: "\x1bOS",
    F5: "\x1b[15~",
    F6: "\x1b[17~",
    F7: "\x1b[18~",
    F8: "\x1b[19~",
    F9: "\x1b[20~",
    F10: "\x1b[21~",
    F11: "\x1b[23~",
    F12: "\x1b[24~",
  };
  if (map[k]) return map[k];
  if (k.length === 1 && !e.ctrlKey && !e.metaKey) return (e.altKey ? "\x1b" : "") + k;
  return null;
}
