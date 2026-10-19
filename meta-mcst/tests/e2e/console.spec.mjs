// E2E: web console against real bmcweb + reimu-serial in the test VM.
// The platform COM1 is the QEMU chardev socket testvm/com1.sock (COM1).
import { chromium } from "playwright";
import { BASE, VMSSH, CHROME, OUT, WORK, COM1 } from "./common.mjs";
import net from "node:net";
import fs from "node:fs";

fs.mkdirSync(OUT, { recursive: true });

let failed = 0;
const check = (c, what) => {
  console.log(`${c ? "PASS" : "FAIL"} ${what}`);
  if (!c) failed++;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function com(path) {
  const s = net.createConnection(path);
  s.rx = "";
  s.on("data", (d) => (s.rx += d.toString("utf8")));
  return new Promise((res) => s.on("connect", () => res(s)));
}

const com1 = await com(COM1);
const marker = `run${Date.now() % 100000}`;
// 25000 lines with Cyrillic and ANSI colors, as a booting Elbrus would print
let chunk = "";
for (let i = 0; i < 25000; i++) chunk += `\x1b[32m[ OK ]\x1b[0m ${marker} строка ${String(i).padStart(5, "0")} загрузка сервиса\r\n`;
await new Promise((r) => com1.write(chunk, r));
com1.write(`${marker} prompt-без-перевода-строки# `);
await sleep(4000);

const browser = await chromium.launch({ executablePath: CHROME, args: ["--ignore-certificate-errors"] });
const ctx = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 1400, height: 850 }, locale: "ru-RU", timezoneId: "Europe/Moscow" });
const page = await ctx.newPage();
const errors = [];
page.on("pageerror", (e) => errors.push(e.message));
page.on("console", (m) => m.type() === "error" && !/status of (401|500)/.test(m.text()) && errors.push(m.text()));

await page.goto(BASE + "/");
await page.fill("#login-pass", "0penBmc");
await page.click("#login-form button[type=submit]");
await page.waitForSelector("#app:not(.hidden)");
check(true, "login via cookie session");
await page.screenshot({ path: `${OUT}/01-overview.png` });

await page.goto(BASE + "/#/console");
await page.waitForFunction(() => document.querySelector("#st-lines") && /^20000 \//.test(document.querySelector("#st-lines").textContent), null, { timeout: 30000 });
check(true, "console loaded 20000 lines of history");
const tabs = await page.$$eval("#con-tabs button", (b) => b.map((x) => x.textContent));
check(tabs.length === 2 && tabs[0].includes("COM1") && tabs[1].includes("Второй"), `port tabs from daemon: ${JSON.stringify(tabs)}`);

const visible = await page.$$eval(".logline", (rows) => rows.filter((r) => r.style.display !== "none").map((r) => r.textContent));
const last = visible.filter((t) => t.includes(marker)).pop() || "";
check(last.includes("prompt-без-перевода-строки"), `partial line shown at bottom: ${last.slice(0, 80)}`);
check(visible.some((t) => t.includes(`${marker} строка 24999`)), "last complete line visible (follow mode)");
check(!visible.some((t) => t.includes("\x1b") || t.includes("[32m")), "ANSI escapes stripped in log view");
const ts = await page.$eval(".logline .ts", (e) => e.textContent);
check(/^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}$/.test(ts), `per-line timestamp with ms: ${ts}`);
await page.screenshot({ path: `${OUT}/02-console-log.png` });

// live line
com1.write(`\r\n${marker} LIVE привет\r\n`);
await page.waitForFunction((m) => [...document.querySelectorAll(".logline")].some((r) => r.textContent.includes(`${m} LIVE привет`)), marker, { timeout: 10000 });
check(true, "live line appears");

// send a command from the input line
com1.rx = "";
await page.fill(".console-input input", "uname -a");
await page.press(".console-input input", "Enter");
await sleep(1500);
check(com1.rx === "uname -a\r", `input line sent to COM port: ${JSON.stringify(com1.rx)}`);

// raw typing into the focused log
com1.rx = "";
await page.click(".logview");
await page.keyboard.type("ls");
await page.keyboard.press("Enter");
await page.keyboard.press("Control+C");
await page.keyboard.press("ArrowUp");
await sleep(1500);
check(com1.rx === "ls\r\x03\x1b[A", `raw keys typed into port: ${JSON.stringify(com1.rx)}`);

// search
await page.fill("#con-search", `${marker} строка 12345`);
await sleep(500);
const hit = await page.$$eval(".logline.hit", (r) => r.map((x) => x.textContent));
check(hit.length >= 1 && hit[0].includes("12345"), "search scrolls to and highlights a historic line");
await page.check("#con-only");
await sleep(300);
const only = await page.$$eval(".logline", (r) => r.filter((x) => x.style.display !== "none").map((x) => x.textContent));
check(only.length === 1, `"only matches" filter: ${only.length} rows`);
await page.screenshot({ path: `${OUT}/03-console-search.png` });
await page.uncheck("#con-only");
await page.fill("#con-search", "");

// BMC/UTC time zone switch changes the rendering
const tsLocal = await page.$eval(".logline .ts", (e) => e.textContent);
await page.selectOption("#con-tz", "utc");
await sleep(300);
const tsUtc = await page.$eval(".logline .ts", (e) => e.textContent);
check(tsLocal !== tsUtc, `time zone switch (local ${tsLocal} vs UTC ${tsUtc})`);
await page.selectOption("#con-tz", "local");

// download with timestamps
const [dl] = await Promise.all([page.waitForEvent("download"), page.click("#con-download")]);
const text = fs.readFileSync(await dl.path(), "utf8");
const lines = text.trim().split("\n");
check(lines.length >= 20000 && /^\[\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}\] /.test(lines[0]), `download: ${lines.length} timestamped lines`);

// terminal mode (xterm.js)
await page.click('#con-mode button[data-mode="term"]');
await page.waitForSelector(".xterm-rows");
await sleep(1000);
const termText = await page.$eval(".xterm-rows", (e) => e.textContent);
check(termText.includes("LIVE привет") || termText.includes(marker), "xterm shows history");
com1.rx = "";
await page.keyboard.type("top");
await page.keyboard.press("Enter");
await sleep(1500);
check(com1.rx === "top\r", `keys from xterm reach the port: ${JSON.stringify(com1.rx)}`);
com1.write("\x1b[2J\x1b[H\x1b[1;31mTERMINAL OK\x1b[0m\r\n");
await sleep(1000);
const termText2 = await page.$eval(".xterm-rows", (e) => e.textContent);
check(termText2.includes("TERMINAL OK"), "xterm renders live escape sequences");
await page.screenshot({ path: `${OUT}/04-console-terminal.png` });

// second port
await page.click('#con-mode button[data-mode="log"]');
await page.click("#con-tabs button:nth-child(2)");
await sleep(1000);
const st = await page.$eval("#st-port", (e) => e.textContent);
check(st.includes("ttyS2"), `second port selected: ${st}`);

check(errors.length === 0, `no JS errors (${errors.join(" | ")})`);
await browser.close();
com1.end();
console.log(failed ? `RESULT: ${failed} FAILED` : "RESULT: ALL PASSED");
process.exit(failed ? 1 : 0);
