// E2E: the PNS flash card ("Оборудование" page) against a BMC in QEMU with a
// PNS flash on spi1 (tests/ab64/test_pns_qemu.py runs it; the platform's
// power is driven there).
//   node pns.qemu.mjs on        the platform is on: the card says so, writes are off
//   node pns.qemu.mjs write     PNS_IMAGE: upload, check, write, the new version shown
//   node pns.qemu.mjs restore   the newest backup written back
import { chromium } from "playwright";
import { BASE, OUT } from "./common.mjs";
import fs from "node:fs";

const mode = process.argv[2] || "write";
fs.mkdirSync(OUT, { recursive: true });
let failed = 0;
const check = (c, w) => {
  console.log(`${c ? "PASS" : "FAIL"} ${w}`);
  if (!c) failed++;
};
const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined, args: ["--ignore-certificate-errors"] });
const ctx = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 1400, height: 1000 }, locale: "ru-RU" });
const page = await ctx.newPage();
const dialogs = [];
page.on("dialog", (d) => {
  dialogs.push(d.message());
  d.accept();
});
const errors = [];
page.on("pageerror", (e) => errors.push(e.message));
await page.goto(BASE + "/");
await page.fill("#login-pass", "0penBmc");
await page.click("#login-form button[type=submit]");
await page.waitForSelector("#app:not(.hidden)");
await page.goto(`${BASE}/#/hardware`);
const card = page.locator("#pns-flash");
await card.locator("dl").first().waitFor({ timeout: 30000 });
await page.waitForTimeout(6000); // the power state (polled every 5 s)
const text = async () => (await card.innerText()).replace(/\s+/g, " ");
check(/Версия ПНС .*unified-boot/.test(await text()), `the chip's version shown: ${(await text()).slice(0, 160)}`);

async function waitJob(re, timeout) {
  const end = Date.now() + timeout;
  while (Date.now() < end) {
    const t = await text();
    if (re.test(t)) return t;
    await page.waitForTimeout(2000);
  }
  return await text();
}

if (mode === "on") {
  const t = await text();
  check(t.includes("Платформа включена"), "the card says the platform is on");
  check(await card.locator("button", { hasText: "Прочитать флеш" }).isDisabled(), "read disabled while on");
} else if (mode === "write") {
  await card.locator("input[type=file]").setInputFiles(process.env.PNS_IMAGE);
  await card.locator("button", { hasText: "Загрузить и проверить" }).click();
  let t = await waitJob(/годен|не годен/, 300000);
  check(/годен/.test(t) && !/не годен/.test(t) && t.includes("f4c822e9"), `uploaded and checked: ${t.match(/Загружен[^]*?Записать/)?.[0]?.slice(0, 200)}`);
  await page.screenshot({ path: `${OUT}/pns-checked.png`, fullPage: true });
  await card.locator("button", { hasText: "Записать во флеш ПНС" }).click();
  t = await waitJob(/Запись флеша: выполняется/, 60000);
  check(/Запись флеша: выполняется/.test(t), "the write runs, progress shown");
  t = await waitJob(/Запись флеша: (готово|ошибка)/, 900000);
  await page.screenshot({ path: `${OUT}/pns-written.png`, fullPage: true });
  check(dialogs.some((d) => d.includes("Записать во флеш ПНС образ")), "a confirmation was asked");
  check(/Запись флеша: готово/.test(t) && /Версия ПНС testing ::::::: \( unified-boot\/main commit f4c822e9/.test(t), `written, the new version shown: ${t.slice(0, 200)}`);
} else if (mode === "restore") {
  await card.locator("tr", { hasText: "backup-" }).first().locator("button", { hasText: "Записать обратно" }).click();
  await waitJob(/Запись флеша: выполняется/, 60000);
  const t = await waitJob(/Запись флеша: (готово|ошибка)/, 900000);
  check(/Запись флеша: готово/.test(t) && /Версия ПНС release-2\.30\.13\.1-E8C2/.test(t), `backup written back: ${t.slice(0, 200)}`);
}
check(errors.length === 0, `no JS errors ${errors.slice(0, 3).join(" | ")}`);
await browser.close();
console.log(failed ? `${failed} FAILED` : "ALL PASSED");
process.exit(failed ? 1 : 0);
