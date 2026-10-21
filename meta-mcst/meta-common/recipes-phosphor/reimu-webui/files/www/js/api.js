// Thin client for bmcweb: session login, XSRF-protected requests, Redfish
// helpers and authenticated websockets.

const listeners = { unauthorized: [] };

function cookie(name) {
  const m = document.cookie.match(new RegExp("(?:^|; )" + name + "=([^;]*)"));
  return m ? decodeURIComponent(m[1]) : "";
}

export class ApiError extends Error {
  constructor(status, message, body) {
    super(message);
    this.status = status;
    this.body = body;
  }
}

function redfishMessage(body) {
  const ext = body && body.error && body.error["@Message.ExtendedInfo"];
  if (ext && ext.length) return ext.map((e) => e.Message).join("; ");
  if (body && body.error && body.error.message) return body.error.message;
  if (body && body.message) return body.message;
  return "";
}

// The BMC is slow at TLS handshakes and bmcweb drops connections whose
// handshake takes more than a few seconds, so a burst of parallel requests
// (which makes the browser open many new connections) must be avoided:
// requests are limited to a few in flight, reusing keep-alive connections.
const MAX_INFLIGHT = 4;
let inflight = 0;
const waiters = [];
async function slot() {
  if (inflight >= MAX_INFLIGHT) await new Promise((r) => waiters.push(r));
  inflight++;
}
function release() {
  inflight--;
  const next = waiters.shift();
  if (next) next();
}

async function request(method, url, data, opts = {}) {
  await slot();
  try {
    return await doRequest(method, url, data, opts);
  } catch (e) {
    // a dropped connection (TypeError from fetch) is retried once for reads
    if (method === "GET" && e instanceof TypeError) return await doRequest(method, url, data, opts);
    throw e;
  } finally {
    release();
  }
}

async function doRequest(method, url, data, opts) {
  const headers = { Accept: "application/json" };
  const init = { method, headers, credentials: "same-origin" };
  if (method !== "GET") headers["X-XSRF-TOKEN"] = cookie("XSRF-TOKEN");
  if (data instanceof Blob || data instanceof ArrayBuffer) {
    headers["Content-Type"] = "application/octet-stream";
    init.body = data;
  } else if (data !== undefined) {
    headers["Content-Type"] = "application/json";
    init.body = JSON.stringify(data);
  }
  const res = await fetch(url, init);
  let body = null;
  const text = await res.text();
  if (text) {
    try {
      body = JSON.parse(text);
    } catch (e) {
      body = text;
    }
  }
  if (res.status === 401 && !opts.noAuthEvent) listeners.unauthorized.forEach((f) => f());
  if (!res.ok) {
    throw new ApiError(res.status, redfishMessage(body) || `${res.status} ${res.statusText}`, body);
  }
  return body;
}

export const api = {
  on(ev, fn) {
    listeners[ev].push(fn);
  },
  get: (url, opts) => request("GET", url, undefined, opts),
  post: (url, data) => request("POST", url, data === undefined ? {} : data),
  patch: (url, data) => request("PATCH", url, data),
  put: (url, data) => request("PUT", url, data),
  del: (url) => request("DELETE", url),

  async login(username, password) {
    // The {"data": [user, password]} form makes bmcweb set the SESSION and
    // XSRF-TOKEN cookies (the {username, password} form only returns a token).
    return request("POST", "/login", { data: [username, password] }, { noAuthEvent: true });
  },
  async logout() {
    try {
      await request("POST", "/logout", {}, { noAuthEvent: true });
    } catch (e) {
      /* session may be gone already */
    }
  },
  // Cheap authenticated probe used at startup. Without the XSRF cookie (set at
  // login) there is no session to probe: no request, no 401 in the console.
  async whoami() {
    if (!cookie("XSRF-TOKEN")) throw new ApiError(401, "no session", null);
    return request("GET", "/redfish/v1/SessionService/Sessions", undefined, { noAuthEvent: true });
  },

  // Fetches a Redfish collection with all members expanded (no $expand
  // support in this bmcweb).
  async collection(url) {
    const col = await this.get(url);
    const members = col.Members || [];
    return Promise.all(members.map((m) => this.get(m["@odata.id"]).catch(() => null))).then((r) => r.filter(Boolean));
  },

  websocket(path) {
    const proto = location.protocol === "https:" ? "wss:" : "ws:";
    const token = cookie("XSRF-TOKEN");
    const ws = token ? new WebSocket(`${proto}//${location.host}${path}`, [token]) : new WebSocket(`${proto}//${location.host}${path}`);
    ws.binaryType = "arraybuffer";
    return ws;
  },

  csrfToken: () => cookie("XSRF-TOKEN"),
};

// Small helpers shared by pages.
export function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k === "class") el.className = v;
    else if (k.startsWith("on") && typeof v === "function") el.addEventListener(k.slice(2), v);
    else if (k === "dataset") Object.assign(el.dataset, v);
    else if (k === "style" && typeof v === "object") Object.assign(el.style, v);
    else if (v === true) el.setAttribute(k, "");
    else el.setAttribute(k, v);
  }
  for (const c of children.flat()) {
    if (c === undefined || c === null || c === false) continue;
    el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return el;
}

// Icons are SVG masks in reimu.css: symbol characters such as ⏻ or ⌨ are
// missing from many system fonts.
export const icon = (name) => h("i", { class: `ic ic-${name}`, "aria-hidden": "true" });

export function toast(text, kind = "") {
  const box = document.getElementById("toasts");
  const t = h("div", { class: `toast ${kind}` }, text);
  box.append(t);
  setTimeout(() => t.remove(), kind === "err" ? 8000 : 4000);
}

export function fmtBytes(n) {
  if (n === undefined || n === null || isNaN(n)) return "—";
  const u = ["Б", "КиБ", "МиБ", "ГиБ", "ТиБ"];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) {
    n /= 1024;
    i++;
  }
  return `${n.toFixed(i ? 1 : 0)} ${u[i]}`;
}

export function pad(n, w = 2) {
  return String(n).padStart(w, "0");
}

// Formats a wall-clock timestamp (ms). tzOffsetMin === null means the
// browser's local time zone, otherwise a fixed offset (e.g. BMC's zone).
export function fmtTime(ms, tzOffsetMin = null, withDate = true) {
  let d;
  if (tzOffsetMin === null) {
    d = new Date(ms);
    const s = `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}.${pad(d.getMilliseconds(), 3)}`;
    return withDate ? `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${s}` : s;
  }
  d = new Date(ms + tzOffsetMin * 60000);
  const s = `${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}:${pad(d.getUTCSeconds())}.${pad(d.getUTCMilliseconds(), 3)}`;
  return withDate ? `${d.getUTCFullYear()}-${pad(d.getUTCMonth() + 1)}-${pad(d.getUTCDate())} ${s}` : s;
}

export function healthBadge(status) {
  const health = status && (status.HealthRollup || status.Health);
  const state = status && status.State;
  if (state && state !== "Enabled") return h("span", { class: "badge" }, state);
  if (health === "OK") return h("span", { class: "badge ok" }, "OK");
  if (health === "Warning") return h("span", { class: "badge warn" }, "Внимание");
  if (health === "Critical") return h("span", { class: "badge err" }, "Авария");
  return h("span", { class: "badge" }, health || "—");
}
