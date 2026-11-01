// E2E: the logs page's event log and the diagnostic button against a BMC in QEMU
// (tests/ab64/test_logs_qemu.py runs it: it planted an event with EV-WEB and made
// the Operator evop).
import { chromium } from "playwright";
import { BASE, OUT } from "./common.mjs";
import fs from "node:fs";
import zlib from "node:zlib";

fs.mkdirSync(OUT, { recursive: true });
let failed = 0;
const check = (c, w) => {
  console.log(`${c ? "PASS" : "FAIL"} ${w}`);
  if (!c) failed++;
};
const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined, args: ["--ignore-certificate-errors"] });

async function open(user, pass, viewport) {
  const ctx = await browser.newContext({ ignoreHTTPSErrors: true, viewport, locale: "ru-RU", acceptDownloads: true });
  const page = await ctx.newPage();
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  await page.goto(BASE + "/");
  if (await page.locator("#login-user").count()) await page.fill("#login-user", user);
  await page.fill("#login-pass", pass);
  await page.click("#login-form button[type=submit]");
  await page.waitForSelector("#app:not(.hidden)");
  await page.goto(`${BASE}/#/logs`);
  await page.waitForSelector("#log-svc");
  return { ctx, page, errors };
}

// the administrator
{
  const { page, ctx, errors } = await open("root", "0penBmc", { width: 1400, height: 1000 });
  await page.selectOption("#log-svc", "bmc-events");
  await page.waitForSelector("#ev-rows", { timeout: 30000 });
  check((await page.locator("#ev-rows").innerText()).includes("EV-WEB"), "the event log shows the event");
  check((await page.locator("#ev-rows").innerText()).includes("boot: reset="), "and the boot record");
  const [save] = await Promise.all([page.waitForEvent("download", { timeout: 15000 }), page.click("#log-ev-save")]);
  check(save.suggestedFilename() === "reimu-events.log", "the event log's download");
  check((await page.locator("#diag-card").innerText()).includes("В архиве есть серийные номера, MAC и IP; паролей и ключей нет"), "the page says what's in it");
  check(!(await page.locator("#diag-host").isChecked()), "the host's data off by default");
  const seen = new Set();
  const poll = setInterval(async () => {
    try {
      seen.add(await page.locator("#diag-state").innerText());
    } catch (e) {
      /* the page goes on */
    }
  }, 300);
  const [dl] = await Promise.all([page.waitForEvent("download", { timeout: 200000 }), page.click("#diag-go")]);
  clearInterval(poll);
  const f = await dl.path();
  const tar = zlib.gunzipSync(fs.readFileSync(f));
  check(dl.suggestedFilename().startsWith("reimu-diag-") && tar.includes(Buffer.from("MANIFEST")), `the archive downloaded: ${dl.suggestedFilename()}, ${tar.length} bytes unpacked`);
  check([...seen].some((s) => /Сбор данных/.test(s)), `progress shown: ${[...seen].slice(0, 3).join(" | ")}`);
  await page.waitForFunction(() => /Готово/.test(document.querySelector("#diag-state").textContent), null, { timeout: 30000 });
  await page.screenshot({ path: `${OUT}/logs-diag.png` });
  check(!errors.length, `no page errors ${errors}`);
  await ctx.close();
}
// a phone
{
  const { page, ctx } = await open("root", "0penBmc", { width: 390, height: 800 });
  const over = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
  check(over <= 1, `no sideways scroll on a phone (${over})`);
  await page.locator("#diag-go").scrollIntoViewIfNeeded();
  const box = await page.locator("#diag-go").boundingBox();
  check(box && box.x >= 0 && box.x + box.width <= 390, "the button fits a phone");
  await page.screenshot({ path: `${OUT}/logs-diag-phone.png` });
  await ctx.close();
}
// an Operator: refused
{
  const { page, ctx } = await open("evop", "Rq7#xLm2pTz", { width: 1400, height: 1000 });
  await page.click("#diag-go");
  await page.waitForFunction(() => /администратор/.test(document.querySelector("#diag-state").textContent), null, { timeout: 30000 });
  check(true, "an Operator is refused the archive");
  await ctx.close();
}
await browser.close();
console.log(`${failed} failed`);
process.exit(failed ? 1 : 0);
