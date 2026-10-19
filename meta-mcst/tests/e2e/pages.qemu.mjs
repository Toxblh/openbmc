// E2E for a BMC in QEMU or on a board: every page of the web UI loads without
// JS errors (VNC password prompt answered with 0penBmc); screenshots in OUT.
// node pages.qemu.mjs [page...]
import { chromium } from "playwright";
import { BASE, CHROME, OUT } from "./common.mjs";
import fs from "node:fs";
fs.mkdirSync(OUT, { recursive: true });
let failed = 0; const check = (c, w) => { console.log(`${c ? "PASS" : "FAIL"} ${w}`); if (!c) failed++; };
const browser = await chromium.launch({ executablePath: CHROME, args: ["--ignore-certificate-errors"] });
const ctx = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 1400, height: 900 }, locale: "ru-RU" });
const page = await ctx.newPage();
page.on("dialog", (d) => d.accept(d.type() === "prompt" ? "0penBmc" : undefined));
const errors = [];
page.on("pageerror", (e) => errors.push(e.message));
const failedReq = [];
// the firmware card asks reimu-firmwared, which only the new image has: on a
// stock BMC (devkit) its 404 is expected and the card stays hidden
const expected404 = /\/xyz\/openbmc_project\/reimu\/firmware$/;
page.on("response", (r) => r.status() >= 400 && !(r.status() === 404 && expected404.test(r.url())) && failedReq.push(`${r.status()} ${r.url().replace(BASE, "")}`));
page.on("console", (m) => m.type() === "error" && !(/status of 404/.test(m.text()) && !failedReq.some((x) => x.startsWith("404"))) && errors.push(m.text()));
await page.goto(BASE + "/");
await page.fill("#login-pass", "0penBmc");
await page.click("#login-form button[type=submit]");
await page.waitForSelector("#app:not(.hidden)");
for (const p of process.argv.slice(2).length ? process.argv.slice(2) : ["overview", "kvm", "console", "media", "sensors", "hardware", "logs", "settings", "firmware"]) {
  errors.length = 0; failedReq.length = 0;
  await page.goto(`${BASE}/#/${p}`); await page.waitForTimeout(p === "kvm" ? 8000 : 4000);
  await page.screenshot({ path: `${OUT}/qemu-${p}.png` });
  const text = (await page.locator("main, #app, body").first().innerText()).replace(/\s+/g, " ").slice(0, 160);
  check(errors.length === 0, `${p}: no JS errors${errors.length ? " — " + errors.slice(0, 3).join(" | ") : ""}${failedReq.length ? "; HTTP: " + failedReq.slice(0, 4).join(", ") : ""} :: ${text}`);
}
await browser.close();
console.log(failed ? `${failed} FAILED` : "ALL PASSED"); process.exit(failed ? 1 : 0);
