// E2E: the switch of the overheat power actions in the settings (its warning,
// its confirmation) and the badge of the sensors page, against a BMC in QEMU
// (tests/ab64/test_overheatd_qemu.py optout runs it).   node overheat.qemu.mjs
import { chromium } from "playwright";
import { BASE, OUT } from "./common.mjs";
import fs from "node:fs";

fs.mkdirSync(OUT, { recursive: true });
let failed = 0;
const check = (c, w) => {
  console.log(`${c ? "PASS" : "FAIL"} ${w}`);
  if (!c) failed++;
};
const WARNING = "Если отключить, BMC не будет выключать хост при перегреве. Встроенная защита процессора может не сработать.";
const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined, args: ["--ignore-certificate-errors"] });
const ctx = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 1400, height: 1000 }, locale: "ru-RU" });
const page = await ctx.newPage();
const errors = [];
page.on("pageerror", (e) => errors.push(e.message));
const rest = async () => {
  const r = await page.evaluate(async () => (await (await import("/js/api.js")).api.get("/xyz/openbmc_project/reimu/overheat")).data.Json);
  return JSON.parse(r);
};
try {
  await page.goto(BASE + "/");
  await page.fill("#login-pass", "0penBmc");
  await page.click("#login-form button[type=submit]");
  await page.waitForSelector("#app:not(.hidden)");
  await page.goto(`${BASE}/#/settings`);
  const box = page.locator("#overheat-poweroff");
  await box.waitFor({ timeout: 30000 });
  // the test leaves it on or off: from "on"
  if (!(await box.isChecked())) {
    await box.click();
    await page.waitForFunction(() => document.querySelector("#overheat-poweroff")?.checked && !document.querySelector("#overheat-card-off"), null, { timeout: 15000 });
  }
  check((await page.locator("#overheat-card").innerText()).includes(WARNING), "the card shows the warning");

  // cancelled: nothing changes
  let dialogText = "";
  page.once("dialog", (d) => {
    dialogText = d.message();
    d.dismiss();
  });
  await box.click();
  await page.waitForTimeout(1000);
  check(dialogText.includes(WARNING), `the confirmation has the warning`);
  check((await box.isChecked()) && (await rest()).poweroff === true, "cancelled: stays on");

  // confirmed
  page.once("dialog", (d) => d.accept());
  await box.click();
  await page.locator("#overheat-card-off").waitFor({ timeout: 15000 });
  check((await rest()).poweroff === false, "confirmed: off (REST)");
  check(!(await page.locator("#overheat-poweroff").isChecked()), "the switch shows off");
  await page.screenshot({ path: `${OUT}/overheat-settings-off.png` });

  await page.goto(`${BASE}/#/sensors`);
  await page.locator("#overheat-off .badge").waitFor({ timeout: 30000 });
  check((await page.locator("#overheat-off").innerText()).includes("Защита от перегрева отключена"), "the sensors page: the badge");
  await page.locator("#overheat-blind .badge").waitFor({ timeout: 15000 });
  check(true, "the sensors page: blind (the test keeps the sensors silent)");
  await page.screenshot({ path: `${OUT}/overheat-sensors-off.png` });

  // reload: the state is the BMC's
  await page.goto(`${BASE}/#/settings`);
  await page.reload();
  await box.waitFor({ timeout: 30000 });
  check(!(await box.isChecked()), "still off after a reload");

  // back on: no confirmation needed
  let asked = false;
  page.once("dialog", (d) => {
    asked = true;
    d.accept();
  });
  await box.click();
  await page.waitForFunction(() => document.querySelector("#overheat-poweroff")?.checked && !document.querySelector("#overheat-card-off"), null, { timeout: 15000 });
  check(!asked && (await rest()).poweroff === true, "back on, no confirmation");
  await page.goto(`${BASE}/#/sensors`);
  await page.waitForFunction(() => document.querySelector("#sens-body .card"), null, { timeout: 30000 });
  await page.waitForTimeout(1000);
  check((await page.locator("#overheat-off").count()) === 0, "the sensors page: the badge gone");
} catch (e) {
  check(false, `${e.message}`.split("\n")[0]);
  await page.screenshot({ path: `${OUT}/overheat-fail.png` }).catch(() => {});
}
check(errors.length === 0, `no page errors ${JSON.stringify(errors)}`);
await browser.close();
process.exit(failed ? 1 : 0);
