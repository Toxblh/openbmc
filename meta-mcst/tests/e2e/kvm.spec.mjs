// E2E: web KVM through real bmcweb /kvm/0. In the test VM an x11vnc server
// with VNC password stands in for obmc-ikvm (QEMU has no ASPEED video engine).
import { chromium } from "playwright";
import { BASE, VMSSH, CHROME, OUT, WORK, COM1 } from "./common.mjs";
import { execSync } from "node:child_process";
import fs from "node:fs";

let failed = 0;
const check = (c, what) => {
  console.log(`${c ? "PASS" : "FAIL"} ${what}`);
  if (!c) failed++;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const vm = (cmd) => execSync(`${VMSSH} sh -s`, { input: cmd, encoding: "utf8" }).trim();

const browser = await chromium.launch({ executablePath: CHROME });
const ctx = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 1400, height: 900 }, acceptDownloads: true });
const page = await ctx.newPage();
const errors = [];
page.on("pageerror", (e) => errors.push(e.message));
let prompts = 0;
page.on("dialog", async (d) => {
  if (d.type() === "prompt" && /VNC/.test(d.message())) {
    prompts++;
    await d.accept(prompts === 1 ? "wrong-password" : "0penBmc");
  } else await d.accept();
});
await page.goto(BASE + "/");
await page.fill("#login-pass", "0penBmc");
await page.click("#login-form button[type=submit]");
await page.waitForSelector("#app:not(.hidden)");

vm("rm -f /tmp/kvm-typed.txt; systemctl restart testvnc; sleep 3");
await page.goto(BASE + "/#/kvm");
// first attempt with a wrong VNC password must fail cleanly
await page.waitForFunction(() => /отключено|потеряно/.test(document.querySelector("#kvm-state")?.textContent) || document.querySelector("#kvm-state")?.textContent === "подключено", null, { timeout: 30000 });
const st1 = await page.$eval("#kvm-state", (e) => e.textContent);
check(prompts === 1 && st1 !== "подключено", `wrong VNC password rejected (state: ${st1})`);
await page.click("#kvm-connect");
await page.waitForFunction(() => document.querySelector("#kvm-state")?.textContent === "подключено", null, { timeout: 30000 });
check(prompts === 2, "VNC password prompted again and accepted");

await sleep(5000);
const res = await page.$eval("#kvm-res", (e) => e.textContent);
check(res === "1024×768", `remote resolution: ${res}`);
const stats = await page.evaluate(async () => {
  const m = await import("/js/lib/novnc-accel.js");
  return { ...m.accelStats };
});
check(stats.enabled, "createImageBitmap JPEG path installed");
const fpsText = await page.$eval("#kvm-fps", (e) => e.textContent);
const bw = await page.$eval("#kvm-bw", (e) => e.textContent);
console.log(`     JPEG frames decoded: ${stats.frames}, dropped as superseded: ${stats.dropped}, avg decode ${stats.decodeMs.toFixed(1)} ms, fps now: ${fpsText}, stream: ${bw}`);
check(stats.frames > 0, `JPEG rectangles decoded via createImageBitmap (${stats.frames})`);

// the canvas really shows the remote desktop: sample pixels
const px = await page.evaluate(() => {
  const c = document.querySelector("#kvm-screen canvas");
  const g = c.getContext("2d");
  const d = g.getImageData(0, 0, c.width, c.height).data;
  let nonBlack = 0;
  const colors = new Set();
  for (let i = 0; i < d.length; i += 4 * 97) {
    if (d[i] + d[i + 1] + d[i + 2] > 30) nonBlack++;
    colors.add((d[i] >> 4) * 256 + (d[i + 1] >> 4) * 16 + (d[i + 2] >> 4));
  }
  return { nonBlack, colors: colors.size, w: c.width, h: c.height };
});
check(px.nonBlack > 100 && px.colors > 5, `canvas shows remote desktop (${px.w}x${px.h}, ${px.colors} colour buckets)`);
await page.screenshot({ path: `${OUT}/07-kvm.png` });

// keyboard and mouse: move pointer over the "cat" xterm and type
const box = await page.$eval("#kvm-screen canvas", (c) => {
  const r = c.getBoundingClientRect();
  return { x: r.x, y: r.y, w: r.width, h: r.height, cw: c.width };
});
const scale = box.w / box.cw;
await page.mouse.move(box.x + 200 * scale, box.y + 100 * scale);
await page.mouse.click(box.x + 200 * scale, box.y + 100 * scale);
await sleep(500);
await page.keyboard.type("Hello KVM 123", { delay: 40 });
await page.keyboard.press("Enter");
await sleep(2000);
const typed = vm("cat /tmp/kvm-typed.txt 2>/dev/null");
check(typed === "Hello KVM 123", `keyboard reaches the remote host: "${typed}"`);

// "paste as keystrokes"
page.removeAllListeners("dialog");
page.on("dialog", (d) => d.accept(d.type() === "prompt" ? "echo pasted-OK" : undefined));
await page.click("#kvm-paste");
await sleep(2500);
await page.keyboard.press("Enter");
await sleep(1500);
const typed2 = vm("cat /tmp/kvm-typed.txt 2>/dev/null");
check(typed2.endsWith("echo pasted-OK"), `paste-as-keys typed text: "${typed2.split("\n").pop()}"`);

// screenshot button saves PNG of the remote screen
const [dl] = await Promise.all([page.waitForEvent("download"), page.click("#kvm-shot")]);
const png = fs.readFileSync(await dl.path());
check(png.slice(1, 4).toString() === "PNG" && png.length > 5000, `screenshot PNG ${png.length} bytes`);

// scale toggle switches between fit and 1:1
await page.click("#kvm-scale");
await sleep(500);
const scaled = await page.$eval("#kvm-scale", (b) => b.classList.contains("active"));
check(scaled === false, "scale toggled to 1:1");

// disconnect / reconnect
await page.click("#kvm-disconnect");
await page.waitForFunction(() => document.querySelector("#kvm-state")?.textContent === "отключено", null, { timeout: 10000 });
check(true, "disconnect");
await page.click("#kvm-connect");
await page.waitForFunction(() => document.querySelector("#kvm-state")?.textContent === "подключено", null, { timeout: 30000 });
check(true, "reconnect reuses the session's VNC password (no prompt)");

check(errors.length === 0, `no JS errors (${errors.join(" | ")})`);
await browser.close();
console.log(failed ? `RESULT: ${failed} FAILED` : "RESULT: ALL PASSED");
process.exit(failed ? 1 : 0);
