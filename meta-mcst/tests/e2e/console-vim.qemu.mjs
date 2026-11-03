// E2E: full-screen programs (vim) on the platform's COM port in the web
// console's terminal tab. The "platform" is com-pty-bridge.py on the QEMU
// UART socket (COM1), the tty as a serial getty leaves it (HOSTROWS x
// HOSTCOLS); the reference is the same byte stream through pyte at that size.
// BASE, COM1, CHROME: see common.mjs; PY: a python with pyte; TERMS, SIZES.
import { chromium } from "playwright";
import { spawn, execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { BASE, OUT, COM1 } from "./common.mjs";

fs.mkdirSync(OUT, { recursive: true });
const HERE = path.dirname(new URL(import.meta.url).pathname);
const PY = process.env.PY || "python3";
let failed = 0;
const check = (c, what) => { console.log(`${c ? "PASS" : "FAIL"} ${what}`); if (!c) failed++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const W = fs.mkdtempSync(path.join(os.tmpdir(), "convim-"));

const HOSTROWS = Number(process.env.HOSTROWS || 24), HOSTCOLS = Number(process.env.HOSTCOLS || 80);
const file = path.join(W, "f.txt");
let body = "";
for (let i = 1; i <= 50; i++) body += `line ${String(i).padStart(2, "0")} ` + "abcdefghij".repeat(i % 5 === 0 ? 7 : 2) + "\n";
fs.writeFileSync(file, body);

// the size of the browser window, and what a viewer may have: a desktop, a
// laptop's short window, a phone
const SIZES = (process.env.SIZES || "1400x850,1000x420,390x800").split(",").map((s) => s.split("x").map(Number));
const TERMS = (process.env.TERMS || "linux,vt220,xterm").split(",");

const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined, args: ["--ignore-certificate-errors"] });

// the visible rows of xterm.js (DOM renderer), trailing blanks dropped
const termRows = (page) => page.$$eval(".xterm-rows > div", (r) => r.map((d) => d.textContent.replace(/ /g, " ").replace(/\s+$/, "")));
const ref = (log) => JSON.parse(execFileSync(PY, [path.join(HERE, "pyte-screen.py"), log, String(HOSTROWS), String(HOSTCOLS)]).toString());

function diff(a, b) {
  const out = [];
  for (let i = 0; i < b.length; i++) if ((a[i] || "") !== b[i]) out.push(`row ${i + 1}: page ${JSON.stringify((a[i] || "").slice(0, 60))} want ${JSON.stringify(b[i].slice(0, 60))}`);
  return out;
}

async function quiet(log, ms = 700) {
  let last = -1;
  for (;;) {
    const n = fs.statSync(log).size;
    if (n === last) return;
    last = n;
    await sleep(ms);
  }
}

for (const [w, h] of SIZES) {
  for (const term of TERMS) {
    const name = `${w}x${h} TERM=${term}`;
    const log = path.join(W, `${w}-${h}-${term}.log`);
    const ctx = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: w, height: h }, locale: "ru-RU" });
    await ctx.addInitScript(() => localStorage.setItem("reimu-console", JSON.stringify({ mode: "term", keys: false })));
    const page = await ctx.newPage();
    await page.goto(BASE + "/");
    await page.fill("#login-pass", "0penBmc");
    await page.click("#login-form button[type=submit]");
    await page.waitForSelector("#app:not(.hidden)");
    await page.goto(BASE + "/#/console");
    await page.waitForSelector(".xterm-rows");
    await sleep(1000);
    const bridge = spawn(PY, [path.join(HERE, "com-pty-bridge.py"), COM1, log, String(HOSTROWS), String(HOSTCOLS), term, "vim", "-u", "NONE", "-N", file], { stdio: "ignore" });
    await sleep(3000);
    await quiet(log);
    await page.click(".xterm-screen");
    const steps = [
      ["open", null],
      ["go to line 6, insert 3 lines", async () => { await page.keyboard.type("6Go"); await page.keyboard.type("inserted one"); await page.keyboard.press("Enter"); await page.keyboard.type("inserted two"); await page.keyboard.press("Enter"); await page.keyboard.type("inserted three"); await page.keyboard.press("Escape"); }],
      ["page down", async () => page.keyboard.press("Control+f")],
      ["page up", async () => page.keyboard.press("Control+b")],
      ["delete a line", async () => page.keyboard.type("dd")],
      ["long lines: line 10, 20", async () => page.keyboard.type("10G")],
      ["end of file", async () => page.keyboard.type("G")],
      ["back to the top", async () => page.keyboard.type("gg")],
    ];
    for (const [what, act] of steps) {
      if (act) await act();
      await sleep(500);
      await quiet(log);
      await sleep(500);
      const got = await termRows(page);
      const want = ref(log);
      const d = diff(got, want);
      check(d.length === 0, `${name}: ${what}: the screen as the host drew it (page ${got.length} rows)${d.length ? `\n     ${d.length} rows differ: ${d.slice(0, 3).join("\n     ")}` : ""}`);
      // the cursor in view, a window shorter than the terminal scrolling with it
      const seen = await page.evaluate(() => {
        const c = document.querySelector(".xterm-helper-textarea").getBoundingClientRect(), w = document.querySelector(".console-term").getBoundingClientRect();
        return c.top >= w.top - 1 && c.top < w.bottom;
      });
      check(seen, `${name}: ${what}: the cursor in view`);
      if (d.length) await page.screenshot({ path: `${OUT}/vim-${w}x${h}-${term}-${what.replace(/\W+/g, "_")}.png` });
    }
    await page.keyboard.type(":q!");
    await page.keyboard.press("Enter");
    await sleep(500);
    bridge.kill();
    await ctx.close();
  }
}
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 10000))]);
fs.rmSync(W, { recursive: true, force: true });
console.log(failed ? `RESULT: ${failed} FAILED` : "RESULT: ALL PASSED");
process.exit(failed ? 1 : 0);
