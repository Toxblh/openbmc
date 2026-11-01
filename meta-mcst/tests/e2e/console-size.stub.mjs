// E2E without a BMC: the console page's terminal size against what the host
// believes. A serial line carries no size: the host formats for 80 columns
// (rows 24; columns 80 on its tty) until someone tells it otherwise.
//
//     node console-size.stub.mjs       # WWW: the web tree, CHROME: chromium
//
// The page (js/pages/console.js, the real file) runs in a bare harness with
// the /reimu/serial websocket stubbed: a "program" writing at 80 columns, the
// cursor position query (CSI 6n) telling what xterm.js really does. With PY (a
// python with pyte) also real programs on an 80-column pty, paced at 115200
// (com-pty-bridge.py), against the same bytes through pyte: zsh and vim.
import { chromium } from "playwright";
import { spawn, execFileSync } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";

const HERE = path.dirname(new URL(import.meta.url).pathname);
const WWW = process.env.WWW || path.join(HERE, "../../meta-common/recipes-phosphor/reimu-webui/files/www");
const PY = process.env.PY || "python3";
const ORIGIN = "http://reimu.test";
const W = fs.mkdtempSync(path.join(os.tmpdir(), "consize-"));

let failed = 0;
const check = (c, what) => { console.log(`${c ? "PASS" : "FAIL"} ${what}`); if (!c) failed++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const b64 = (d) => Buffer.from(d).toString("base64");
const MIME = { ".js": "text/javascript", ".css": "text/css", ".html": "text/html", ".svg": "image/svg+xml", ".woff2": "font/woff2" };
const PORTS = [{ index: 0, name: "host", title: "COM1" }, { index: 1, name: "com2", title: "Второй" }];
const HARNESS = `<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="stylesheet" href="css/reimu.css"><link rel="stylesheet" href="vendor/xterm/xterm.css">
<script src="vendor/xterm/xterm.js"></script><script src="vendor/xterm/addon-fit.js"></script></head>
<body style="height:100vh;margin:0;display:flex;flex-direction:column;overflow:hidden">
<main id="page" class="page fill" style="flex:1;min-height:0"></main><div id="toasts"></div>
<script type="module">import * as con from "./js/pages/console.js";
window.con = con; window.remount = () => { con.unmount(); document.getElementById("page").replaceChildren(); return con.mount(document.getElementById("page")); };
con.mount(document.getElementById("page"));</script></body></html>`;

const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });

async function open(w, h, prefs = {}) {
  const ctx = await browser.newContext({ viewport: { width: w, height: h }, locale: "ru-RU" });
  await ctx.addInitScript((p) => localStorage.setItem("reimu-console", JSON.stringify(Object.assign({ mode: "term", keys: false }, p))), prefs);
  const page = await ctx.newPage();
  const s = { ctx, page, ws: {}, tx: { 0: "", 1: "" }, all: { 0: "", 1: "" }, onTx: null, errors: [] };
  page.on("pageerror", (e) => s.errors.push(e.stack || e.message));
  await page.route(`${ORIGIN}/**`, (route) => {
    const p = new URL(route.request().url()).pathname;
    if (p === "/harness.html") return route.fulfill({ contentType: "text/html", body: HARNESS });
    const f = path.join(WWW, p);
    if (!fs.existsSync(f) || fs.statSync(f).isDirectory()) return route.fulfill({ status: 404, body: "" });
    return route.fulfill({ contentType: MIME[path.extname(f)] || "application/octet-stream", body: fs.readFileSync(f) });
  });
  await page.routeWebSocket(/\/reimu\/serial\/\d+$/, (ws) => {
    const n = Number(ws.url().match(/(\d+)$/)[1]);
    s.ws[n] = ws;
    ws.onMessage((m) => {
      for (const line of String(m).split("\n")) {
        if (!line) continue;
        const o = JSON.parse(line);
        if (o.type !== "write") continue;
        const d = Buffer.from(o.d, "base64");
        s.tx[n] += d.toString();
        s.all[n] += d.toString();
        if (s.onTx) s.onTx(n, d);
      }
    });
    ws.send(JSON.stringify({ type: "hello", index: n, name: PORTS[n].name, title: PORTS[n].title, online: true, baud: 115200, device: n ? "/dev/ttyS2" : "/dev/ttyS0", maxLines: 20000, ports: PORTS }) + "\n");
    ws.send(JSON.stringify({ type: "history_end", partial: [0, ""] }) + "\n");
  });
  await page.goto(`${ORIGIN}/harness.html`);
  await page.waitForSelector(".xterm-rows");
  await sleep(300);
  return s;
}

const emit = (s, n, data) => s.ws[n].send(JSON.stringify({ type: "data", t: Date.now(), d: b64(data) }) + "\n");

// what the terminal answers about its cursor: nothing else knows its true size
async function cpr(s, n, before = "", after = "") {
  s.tx[n] = "";
  emit(s, n, before + "\x1b[6n" + after);
  for (let i = 0; i < 200; i++) {
    const m = s.tx[n].match(/\x1b\[(\d+);(\d+)R/);
    if (m) {
      s.tx[n] = s.tx[n].replace(m[0], "");
      s.all[n] = s.all[n].replace(m[0], "");
      return { row: +m[1], col: +m[2] };
    }
    await sleep(10);
  }
  throw new Error("no answer to the cursor position query");
}
async function grid(s, n = 0) {
  await sleep(150); // a ResizeObserver's or an animation frame's fit
  const c = await cpr(s, n, "\x1b7\x1b[999;999H", "\x1b8");
  return { rows: c.row, cols: c.col };
}
const rowsText = (page) => page.$$eval(".xterm-rows > div", (r) => r.map((d) => d.textContent.replace(/ /g, " ").replace(/\s+$/, "")));
const metrics = (page) => page.evaluate(() => {
  const scr = document.querySelector(".xterm-screen").getBoundingClientRect(), wrap = document.querySelector(".console-term:not(.hidden)");
  const b = wrap.getBoundingClientRect();
  return { px: parseFloat(getComputedStyle(document.querySelector(".xterm-rows")).fontSize), screen: scr.width, left: scr.left - b.left, wrap: wrap.clientWidth, wrapH: wrap.clientHeight, screenH: scr.height };
});

// ---------------------------------------------------------- the desktop, no sync
{
  const s = await open(1400, 850);
  const { page } = s;
  let g = await grid(s);
  check(g.cols === 80, `1400x850, nothing told the host: terminal has the host's 80 columns (${g.cols})`);
  check(g.rows > 24, `...and the window's rows (${g.rows})`);
  let m = await metrics(page);
  check(m.px === 14, `...at the chosen font size (${m.px}px), not stretched`);
  check(m.left + m.screen <= m.wrap, `...80 columns fit the window (${m.left + m.screen} of ${m.wrap})`);

  emit(s, 0, "\x1b[2J\x1b[H");
  const c = await cpr(s, 0, "A".repeat(100));
  check(c.row === 2 && c.col === 21, `a 100-character line wraps at 80: the cursor at 2;21 (${c.row};${c.col})`);
  // what zsh does: it counts 80 columns, goes up one row and redraws the prompt
  emit(s, 0, "\x1b[2J\x1b[H% " + "x".repeat(98));
  await cpr(s, 0, "\x1b[1A\rP ");
  let t = await rowsText(page);
  check(t[0] === "P " + "x".repeat(78) && t[1] === "x".repeat(20), `a program's reposition lands where it counted (row 1 ${JSON.stringify(t[0].slice(0, 12))}, ${t[0].length} long; row 2 ${t[1].length} long)`);

  // the window changes: never a word to the host
  await page.setViewportSize({ width: 1000, height: 420 });
  g = await grid(s);
  check(g.cols === 80 && g.rows === 24, `a short window 1000x420: 80x24, the window scrolls (${g.cols}x${g.rows})`);
  await page.setViewportSize({ width: 1400, height: 850 });
  g = await grid(s);
  check(g.cols === 80 && g.rows > 24, `back to 1400x850: 80 columns, the window's rows (${g.cols}x${g.rows})`);
  check(s.all[0] === "", `resizing typed nothing into the port (${JSON.stringify(s.all[0].slice(0, 40))})`);

  // ------------------------------------------------------ the explicit button
  const btn = await page.$("#con-winsize");
  check(!!btn, "the button «Размер под окно» is there");
  if (btn) {
    check((await btn.textContent()).includes("Размер под окно"), "...named so");
    check(/stty/.test(await btn.getAttribute("title")) && /запущено/.test(await btn.getAttribute("title")), "...its title warns it types into whatever is running");
    await btn.click();
    await sleep(200);
    const typed = s.all[0];
    const mm = typed.match(/^stty rows (\d+) cols (\d+)\r$/);
    check(!!mm, `the click types exactly the stty line and Enter: ${JSON.stringify(typed)}`);
    const R = mm ? +mm[1] : 0, C = mm ? +mm[2] : 0;
    g = await grid(s);
    check(g.cols === C && g.rows === R && C > 80, `the terminal then has that size: ${g.cols}x${g.rows}, typed ${C}x${R}`);
    m = await metrics(page);
    const cell = m.screen / C;
    check(m.px === 14 && m.left + m.screen <= m.wrap && (C + 3) * cell > m.wrap - 30, `...the window's width at the chosen font (${C} columns, ${m.px}px, ${m.left + m.screen} of ${m.wrap})`);
    emit(s, 0, "\x1b[2J\x1b[H");
    const c2 = await cpr(s, 0, "A".repeat(100));
    check(c2.row === 1 && c2.col === 101, `a 100-character line now fits one row: ${c2.row};${c2.col}`);

    s.all[0] = "";
    await page.setViewportSize({ width: 1000, height: 420 });
    g = await grid(s);
    check(g.cols === C && g.rows === R, `the window changes after it: the size the host knows stays (${g.cols}x${g.rows}), a window can only pan`);
    await page.setViewportSize({ width: 1400, height: 850 });
    g = await grid(s);
    check(g.cols === C && g.rows === R && s.all[0] === "", `...nothing typed by it (${g.cols}x${g.rows}, ${JSON.stringify(s.all[0])})`);

    await page.click("#con-tabs button:nth-child(2)");
    g = await grid(s, 1);
    check(g.cols === 80, `another port: the default 80 columns (${g.cols})`);
    check(s.all[1] === "", "...nothing typed there");
    await page.click("#con-tabs button:nth-child(1)");
    g = await grid(s);
    check(g.cols === C, `the first port again: still known (${g.cols})`);

    await s.ws[0].close();
    await sleep(1200);
    g = await grid(s);
    check(g.cols === C, `a new connection of this port, still known in this page (${g.cols})`);
    await page.evaluate(() => window.remount());
    await page.waitForSelector(".xterm-rows");
    await sleep(800);
    g = await grid(s);
    check(g.cols === C, `leaving the page and back, still known (${g.cols})`);

    // a new login on that tty: getty and login start with a size of their own
    emit(s, 0, "\r\nhost login: ");
    g = await grid(s);
    check(g.cols === 80, `a login prompt on the port: back to the default (${g.cols})`);
    await page.click("#con-mode button[data-mode=log]");
    await sleep(100);
    check(await page.$eval("#con-winsize", (b) => b.disabled), "the log view: the button is off");
    await page.click("#con-mode button[data-mode=term]");
    await sleep(100);
    // no device behind the port: nothing typed, nothing changed
    s.all[0] = "";
    s.ws[0].send(JSON.stringify({ type: "status", baud: 115200, online: false }) + "\n");
    await sleep(100);
    await page.click("#con-winsize");
    g = await grid(s);
    check(s.all[0] === "" && g.cols === 80, `the port offline: the click types nothing, the size stays (${g.cols}, ${JSON.stringify(s.all[0])})`);
  }
  check(s.errors.length === 0, `no page errors ${s.errors.join("; ")}`);
  await s.ctx.close();
}

// ---------------------------------------------------------- a big font, a phone
{
  const s = await open(900, 600, { fontSize: 24 });
  const g = await grid(s);
  const m = await metrics(s.page);
  check(g.cols === 80 && m.px < 24 && m.left + m.screen <= m.wrap, `900 wide, font 24: the font shrinks until 80 columns fit (${g.cols}, ${m.px}px, ${m.left + m.screen} of ${m.wrap})`);
  await s.ctx.close();
}
{
  const s = await open(390, 800);
  const g = await grid(s);
  const m = await metrics(s.page);
  check(g.cols === 80 && m.px < 12 && m.px >= 6, `a phone: 80 columns at a smaller font (${g.cols}, ${m.px}px, ${m.left + m.screen} of ${m.wrap})`);
  await s.page.click("#con-more");
  const btn = await s.page.$("#con-winsize");
  if (btn && await btn.isVisible()) {
    await btn.click();
    await sleep(200);
    const mm = s.all[0].match(/^stty rows (\d+) cols (\d+)\r$/);
    check(mm && +mm[2] === 80, `a phone, the button: never under 80 columns (${JSON.stringify(s.all[0])})`);
  } else check(false, "a phone: the button under «more»");
  await s.ctx.close();
}

// ---------------------------------------------------------- real programs
let pyte = false;
try { execFileSync(PY, ["-c", "import pyte"], { stdio: "ignore" }); pyte = true; } catch (e) { /* the stub part stands alone */ }
const has = (c) => { try { execFileSync("sh", ["-c", `command -v ${c}`], { stdio: "ignore" }); return true; } catch (e) { return false; } };
const ref = (log, rows = 24, cols = 80) => JSON.parse(execFileSync(PY, [path.join(HERE, "pyte-screen.py"), log, String(rows), String(cols)]).toString());
function diff(a, b) {
  const out = [];
  for (let i = 0; i < b.length; i++) if ((a[i] || "") !== b[i]) out.push(`row ${i + 1}: page ${JSON.stringify((a[i] || "").slice(0, 50))} want ${JSON.stringify(b[i].slice(0, 50))}`);
  return out;
}
async function quiet(log, ms = 500) {
  let last = -1;
  for (;;) {
    const n = fs.statSync(log).size;
    if (n === last) return;
    last = n;
    await sleep(ms);
  }
}

// a program on an 80-column pty, as com-pty-bridge.py does it behind the UART
async function platform(s, name, env, argv) {
  const sock = path.join(W, `${name}.sock`), log = path.join(W, `${name}.log`);
  let peer;
  const srv = net.createServer((c) => {
    peer = c;
    c.on("data", (d) => emit(s, 0, d));
    c.on("error", () => {});
  });
  await new Promise((r) => srv.listen(sock, r));
  s.onTx = (n, d) => { if (n === 0 && peer) peer.write(d); };
  const child = spawn(PY, [path.join(HERE, "com-pty-bridge.py"), sock, log, "24", "80", env.TERM || "xterm-256color", ...argv], { stdio: "ignore", env: { ...process.env, ...env } });
  return { log, stop: () => { child.kill(); srv.close(); if (peer) peer.destroy(); } };
}

if (pyte && has("zsh")) {
  const s = await open(1400, 850);
  const { page } = s;
  const zd = path.join(W, "zdot"), buf = path.join(W, "buffer");
  fs.mkdirSync(zd);
  fs.writeFileSync(path.join(zd, ".zshrc"), `PROMPT='%% '\nunsetopt prompt_cr prompt_sp\nzle-line-finish() { print -r -- "$BUFFER" >> ${buf} }\nzle -N zle-line-finish\n`);
  const z = await platform(s, "zsh", { ZDOTDIR: zd }, ["zsh", "-d", "-i"]);
  await sleep(1500);
  await page.click(".xterm-screen");
  const same = async (what) => {
    await sleep(300);
    await quiet(z.log);
    await sleep(300);
    const d = diff(await rowsText(page), ref(z.log));
    check(d.length === 0, `real zsh, 80 columns: ${what}: the screen as the host drew it${d.length ? `\n     ${d.length} rows differ: ${d.slice(0, 3).join("\n     ")}` : ""}`);
  };
  await same("the prompt");
  await page.keyboard.type("echo " + "abcdefghij".repeat(12));
  await same("a 125-character line typed");
  for (let i = 0; i < 60; i++) await page.keyboard.press("Backspace");
  await same("back over the wrap with Backspace");
  for (let i = 0; i < 30; i++) await page.keyboard.press("ArrowLeft");
  await page.keyboard.type("XYZ");
  await same("an insert in the middle");
  await page.keyboard.press("Control+u");
  await same("the line killed");

  // a paste: xterm brackets it (zsh asked for it), zsh gets every character
  const text = "echo " + Array.from({ length: 500 }, (_, i) => String(i).padStart(9, "0")).join(" ");
  s.tx[0] = "";
  await page.evaluate((t) => {
    const dt = new DataTransfer();
    dt.setData("text/plain", t);
    document.querySelector(".xterm-helper-textarea").dispatchEvent(new ClipboardEvent("paste", { clipboardData: dt, bubbles: true, cancelable: true }));
  }, text);
  await sleep(300);
  check(s.tx[0] === `\x1b[200~${text}\x1b[201~`, `a ${text.length}-character paste goes as one bracketed write (${s.tx[0].length} bytes, starts ${JSON.stringify(s.tx[0].slice(0, 8))})`);
  await same(`the ${text.length}-character paste`);
  await page.keyboard.press("Enter");
  await sleep(1500);
  await quiet(z.log);
  const lines = fs.existsSync(buf) ? fs.readFileSync(buf, "utf8").split("\n").filter(Boolean) : [];
  check(lines[lines.length - 1] === text, `zsh's line is the pasted text, whole (${(lines[lines.length - 1] || "").length} characters)`);
  z.stop();
  await s.ctx.close();
} else console.log("SKIP real zsh: PY with pyte or zsh missing");

if (pyte && has("vim")) {
  const file = path.join(W, "f.txt");
  let body = "";
  for (let i = 1; i <= 50; i++) body += `line ${String(i).padStart(2, "0")} ` + "abcdefghij".repeat(i % 5 === 0 ? 10 : 2) + "\n";
  fs.writeFileSync(file, body);
  for (const [w, h] of [[1400, 850], [1000, 420], [390, 800]]) {
    const s = await open(w, h);
    const v = await platform(s, `vim${w}`, { TERM: "xterm" }, ["vim", "-u", "NONE", "-N", file]);
    await sleep(2000);
    await quiet(v.log);
    await s.page.click(".xterm-screen");
    for (const [what, keys] of [["open", ""], ["page down", "\x06"], ["line 10, wrapped at 80", "10G"], ["end", "G"], ["top", "gg"]]) {
      if (keys) await s.page.keyboard.type(keys === "\x06" ? "" : keys);
      if (keys === "\x06") await s.page.keyboard.press("Control+f");
      await sleep(300);
      await quiet(v.log);
      await sleep(300);
      const d = diff(await rowsText(s.page), ref(v.log));
      check(d.length === 0, `real vim ${w}x${h}: ${what}: the screen as the host drew it${d.length ? `\n     ${d.length} rows differ: ${d.slice(0, 2).join("\n     ")}` : ""}`);
    }
    await s.page.keyboard.type(":q!");
    await s.page.keyboard.press("Enter");
    await sleep(300);
    v.stop();
    await s.ctx.close();
  }
} else console.log("SKIP real vim: PY with pyte or vim missing");

await browser.close();
fs.rmSync(W, { recursive: true, force: true });
console.log(failed ? `RESULT: ${failed} FAILED` : "RESULT: ALL PASSED");
process.exit(failed ? 1 : 0);
