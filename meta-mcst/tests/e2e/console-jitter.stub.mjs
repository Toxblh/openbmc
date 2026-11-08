// E2E without a BMC: the COM terminal must not move. vim on the platform
// redraws with the cursor jumping between a text row and the last row (its
// status line); the terminal's box (its toolbar above, the status bar
// below) must stay where it is, no container of the page scrolling.
//
//     node console-jitter.stub.mjs       # WWW: the web tree, CHROME: chromium
import { chromium } from "playwright";
import fs from "node:fs";
import path from "node:path";

const HERE = path.dirname(new URL(import.meta.url).pathname);
const WWW = process.env.WWW || path.join(HERE, "../../meta-common/recipes-phosphor/reimu-webui/files/www");
const ORIGIN = "http://reimu.test";
let failed = 0;
const check = (c, what) => { console.log(`${c ? "PASS" : "FAIL"} ${what}`); if (!c) failed++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const b64 = (d) => Buffer.from(d).toString("base64");
const MIME = { ".js": "text/javascript", ".css": "text/css", ".html": "text/html", ".svg": "image/svg+xml", ".woff2": "font/woff2" };
const PORTS = [{ index: 0, name: "host", title: "Консоль платформы (COM1)" }];
const HARNESS = `<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="stylesheet" href="css/reimu.css"><link rel="stylesheet" href="vendor/xterm/xterm.css">
<script src="vendor/xterm/xterm.js"></script><script src="vendor/xterm/addon-fit.js"></script></head>
<body style="height:100vh;margin:0;display:flex;flex-direction:column;overflow:hidden">
<main id="page" class="page fill" style="flex:1;min-height:0"></main><div id="toasts"></div>
<script type="module">import * as con from "./js/pages/console.js"; con.mount(document.getElementById("page"));</script></body></html>`;

const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });

async function open(w, h) {
  const ctx = await browser.newContext({ viewport: { width: w, height: h }, locale: "ru-RU", deviceScaleFactor: 2 });
  await ctx.addInitScript(() => localStorage.setItem("reimu-console", JSON.stringify({ mode: "term", keys: false })));
  const page = await ctx.newPage();
  const s = { page, ws: null, errors: [] };
  page.on("pageerror", (e) => s.errors.push(e.message));
  await page.route(`${ORIGIN}/**`, (route) => {
    const p = new URL(route.request().url()).pathname;
    if (p === "/harness.html") return route.fulfill({ contentType: "text/html", body: HARNESS });
    const f = path.join(WWW, p);
    if (!fs.existsSync(f) || fs.statSync(f).isDirectory()) return route.fulfill({ status: 404, body: "" });
    return route.fulfill({ contentType: MIME[path.extname(f)] || "application/octet-stream", body: fs.readFileSync(f) });
  });
  await page.routeWebSocket(/\/reimu\/serial\/\d+$/, (ws) => {
    s.ws = ws;
    ws.onMessage(() => {});
    ws.send(JSON.stringify({ type: "hello", index: 0, name: "host", title: PORTS[0].title, online: true, baud: 115200, device: "/dev/ttyS0", maxLines: 20000, ports: PORTS }) + "\n");
    ws.send(JSON.stringify({ type: "history_end", partial: [0, ""] }) + "\n");
  });
  await page.goto(`${ORIGIN}/harness.html`);
  await page.waitForSelector(".xterm-rows");
  await sleep(300);
  return s;
}
const emit = (s, data) => s.ws.send(JSON.stringify({ type: "data", t: Date.now(), d: b64(data) }) + "\n");

// every scroll of anything on the page, and where the terminal's box is
const watch = (page) =>
  page.evaluate(() => {
    window.moves = [];
    document.addEventListener("scroll", (e) => window.moves.push(`${e.target.className || e.target.nodeName}:${(e.target.scrollTop ?? document.scrollingElement.scrollTop)}`), true);
    const box = () => {
      const t = document.querySelector(".console-term").getBoundingClientRect();
      const tb = document.querySelector(".console-toolbar").getBoundingClientRect();
      return `${Math.round(t.top)},${Math.round(t.bottom)},${Math.round(tb.top)}`;
    };
    window.box0 = box();
    window.boxes = new Set([window.box0]);
    const tick = () => { window.boxes.add(box()); window.boxRaf = requestAnimationFrame(tick); };
    tick();
  });

async function vim(s, rows, rounds) {
  // the alternate screen, every row drawn, then "J" as vim redraws: the text
  // row with the cursor, the status line on the last row, the cursor back
  emit(s, "\x1b[?1049h\x1b[H\x1b[2J");
  let all = "";
  for (let r = 1; r < rows; r++) all += `\x1b[${r};1H${String(r).padStart(3)} zplug "plugins/x${r}",   from:oh-my-zsh\x1b[K`;
  all += `\x1b[${rows};1H\x1b[7m NORMAL  .zshrc                      utf-8   55%% ln :39/70 \x1b[0m`;
  emit(s, all);
  for (let i = 0; i < rounds; i++) {
    const r = 5 + (i % (rows - 8));
    emit(s, `\x1b[${r};1H${String(r).padStart(3)} zplug check || zplug install ${i}\x1b[K` +
      `\x1b[${rows};1H\x1b[7m NORMAL  .zshrc   ${String(40 + i).padStart(3)}%% ln :${r}/70 \x1b[0m\x1b[${r};5H`);
    await sleep(25);
  }
  await sleep(400);
}

for (const [w, h, label] of [[1000, 1045, "the reported window"], [1280, 720, "1280x720"], [1000, 1003, "1000x1003"]]) {
  const s = await open(w, h);
  await s.page.click(".xterm-screen");
  await s.page.getByRole("button", { name: "Размер под окно" }).click();
  await sleep(300);
  const geo = await s.page.evaluate(() => {
    const t = document.querySelector(".console-term");
    return { rows: document.querySelector(".xterm-rows").childElementCount, sh: t.scrollHeight, ch: t.clientHeight };
  });
  await watch(s.page);
  await vim(s, geo.rows, 60);
  const r = await s.page.evaluate(() => ({ moves: window.moves, boxes: [...window.boxes] }));
  check(r.boxes.length === 1, `${label}: the terminal box stays put (${r.boxes.join(" | ")}; rows ${geo.rows}, ${geo.sh}/${geo.ch} px)`);
  check(r.moves.length === 0, `${label}: nothing scrolls (${r.moves.slice(0, 6).join(" ")}${r.moves.length > 6 ? " …" : ""})`);
  check(!s.errors.length, `${label}: no page errors ${s.errors}`);
  await s.page.context().close();
}
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 10000))]);
console.log(`${failed} failed`);
process.exit(failed ? 1 : 0);
