// E2E: the fan card's Турбо and the top bar's Турбо chip, against a BMC in
// QEMU running reimu-fand (tests/ab64/test_fand_qemu.py runs it, the
// platform on).   node fans.qemu.mjs
import { chromium } from "playwright";
import { BASE, OUT } from "./common.mjs";
import fs from "node:fs";

fs.mkdirSync(OUT, { recursive: true });
let failed = 0;
const check = (c, w) => {
  console.log(`${c ? "PASS" : "FAIL"} ${w}`);
  if (!c) failed++;
};
const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined, args: ["--ignore-certificate-errors"] });
const ctx = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 1400, height: 1000 }, locale: "ru-RU" });
const page = await ctx.newPage();
const errors = [];
page.on("pageerror", (e) => errors.push(e.message));
try {
  await page.goto(BASE + "/");
  await page.fill("#login-pass", "0penBmc");
  await page.click("#login-form button[type=submit]");
  await page.waitForSelector("#app:not(.hidden)");
  await page.goto(`${BASE}/#/sensors`);
  const turbo = page.locator("#fans-body #fan-turbo");
  await turbo.waitFor({ timeout: 30000 });
  const chip = page.locator("#turbo-chip");
  await page.waitForFunction(() => {
    const c = document.querySelector("#turbo-chip");
    return c && !c.classList.contains("hidden");
  }, null, { timeout: 15000 });
  check((await chip.innerText()).trim() === "Турбо", `top bar chip, off: "${(await chip.innerText()).trim()}"`);
  await turbo.getByRole("button", { name: "30 мин" }).click();
  await page.locator("#fans-body #fan-turbo .badge").waitFor({ timeout: 15000 });
  const badge = (await page.locator("#fans-body #fan-turbo .badge").innerText()).trim();
  check(/^Турбо: осталось (29|30):\d\d$/.test(badge), `card: "${badge}"`);
  await page.waitForFunction(() => /^Турбо (29|30) мин$/.test(document.querySelector("#turbo-chip").textContent), null, { timeout: 15000 });
  check(true, `top bar chip on: "${(await chip.innerText()).trim()}"`);
  await page.screenshot({ path: `${OUT}/fans-turbo-card.png` });
  // another page: the chip is in the top bar
  await page.goto(`${BASE}/#/`);
  await page.waitForTimeout(1500);
  check(/^Турбо (29|30) мин$/.test((await chip.innerText()).trim()) && (await chip.isVisible()), "the chip on the overview page");
  await page.screenshot({ path: `${OUT}/fans-turbo-chip.png` });
  await chip.click();
  await page.waitForFunction(() => document.querySelector("#turbo-chip").textContent === "Турбо", null, { timeout: 15000 });
  check(true, "chip click: turbo off");
  await page.goto(`${BASE}/#/sensors`);
  await page.locator("#fans-body #fan-turbo").getByRole("button", { name: "30 мин" }).waitFor({ timeout: 15000 });
  check(true, "card: the turbo buttons again");
} catch (e) {
  check(false, `${e.message}`.split("\n")[0]);
  await page.screenshot({ path: `${OUT}/fans-turbo-fail.png` }).catch(() => {});
}
check(errors.length === 0, `no page errors ${JSON.stringify(errors)}`);
await browser.close();
process.exit(failed ? 1 : 0);
