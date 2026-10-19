// E2E: reliable BMC firmware upgrade (two flash chips, WDT2 fallback) from
// the settings page. The stand (testvm/fixtures/ab-stand.sh) emulates the
// chips with block2mtd, the SCU/WDT registers and the U-Boot environment
// with files; reboots are emulated by "ab-stand.sh boot", the flashing at
// shutdown by "ab-stand.sh flash-staged". Uploads go through the real bmcweb
// Redfish UpdateService to fake-phosphor's static layout updater.
import { chromium } from "playwright";
import { BASE, VMSSH, CHROME, OUT } from "./common.mjs";
import { execSync } from "node:child_process";
import { mkdirSync } from "node:fs";

const IMG = process.env.AB_IMAGES || "/tmp/reimu-e2e/ab"; // make-ab-images.py output
let failed = 0;
const check = (c, what) => {
  console.log(`${c ? "PASS" : "FAIL"} ${what}`);
  if (!c) failed++;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const vm = (cmd) => execSync(`${VMSSH} sh -s`, { input: cmd, encoding: "utf8" }).trim();
const md5 = (chip, from, count) => vm(`dd if=/var/lib/ab-stand/chip${chip}.img bs=1M skip=${from} count=${count} 2>/dev/null | md5sum | cut -c1-32`);
mkdirSync(OUT, { recursive: true });

vm("ab-stand.sh reset /root/ab/chipA.img /root/ab/chipB.img >/dev/null 2>&1");
const rwA = md5("A", 48, 16), rwB = md5("B", 48, 16);

const browser = await chromium.launch({ executablePath: CHROME });
const ctx = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 1400, height: 1000 } });
const page = await ctx.newPage();
const errors = [];
page.on("pageerror", (e) => errors.push(e.message));
page.on("dialog", (d) => d.accept());
const card = () => page.$eval("#fw-card", (e) => e.textContent);
const waitCard = (text, timeout = 120000) => page.waitForFunction((t) => (document.querySelector("#fw-card") || {}).textContent?.includes(t), text, { timeout });
const button = (text) => page.locator("#fw-card button", { hasText: text });
const settings = async () => {
  await page.goto(BASE + "/#/overview");
  await page.goto(BASE + "/#/settings");
  await page.waitForSelector("#fw-card dl");
};

await page.goto(BASE + "/");
await page.fill("#login-pass", "0penBmc");
await page.click("#login-form button[type=submit]");
await page.waitForSelector("#app:not(.hidden)");
await settings();
let t = await card();
if (process.env.DEBUG) console.log(t);
check(t.includes("основной чип") && t.includes("REIMU 5564 2.0.0") && t.includes("REIMU 5564 1.9.0") && t.includes("не проверялось"), "status: primary chip, both versions, sync unknown");
check(t.includes("выключен"), "WDT2 disarmed after a normal boot");

await button("Сверить чипы").click();
await waitCard("отличается");
check(true, "verify: backup differs from the running image");

await button("Обновить резервную копию").click();
await waitCard("выполняется", 10000).then(() => check(true, "sync job progress shown"), () => check(false, "sync job progress shown"));
await waitCard("совпадает с рабочим");
await waitCard("готово");
await page.waitForFunction(() => /Другой чип\s*REIMU/.test(document.querySelector("#fw-card").textContent), null, { timeout: 30000 });
t = await card();
check(t.includes("готово") && /Другой чип\s*REIMU 5564 2\.0\.0/.test(t), "sync done: backup holds 2.0.0");
check(md5("A", 0, 48) === md5("B", 0, 48), "chips identical in the image area (48 MiB)");
check(md5("A", 48, 16) === rwA && md5("B", 48, 16) === rwB, "rwfs of both chips untouched");
await page.screenshot({ path: `${OUT}/fw-synced.png`, fullPage: true });

// ---- healthy update through Redfish UpdateService ----
async function upload(file) {
  await page.setInputFiles("#fw-card input[type=file]", `${IMG}/${file}`);
  await button("Загрузить и активировать").click();
  await page.waitForFunction(() => /Образ принят|не удалось/.test(document.querySelector("#fw-card").textContent) || [...document.querySelectorAll(".toast")].some((x) => /не удалось/.test(x.textContent)), null, { timeout: 60000 });
}
await upload("fw-2.1.0.tar");
t = await card();
check(t.includes("Образ принят"), "upload 2.1.0: accepted and activated");
if (!t.includes("Образ принят")) console.log("  toasts:", await page.$$eval(".toast", (x) => x.map((e) => e.textContent)));
await sleep(1500);
check(vm("ls /run/initramfs") === "image-kernel\nimage-rofs\nimage-rwfs\nimage-u-boot", "image staged in /run/initramfs");
check(vm("for i in $(seq 30); do [ \"$(cat /var/lib/ab-stand/env.A)\" = wdcontinue=1 ] && break; sleep 1; done; cat /var/lib/ab-stand/env.A") === "wdcontinue=1", "path unit guarded the staged update (wdcontinue=1)");
await settings();
check((await card()).includes("ожидает подтверждения"), "card: update pending confirmation");

vm("ab-stand.sh flash-staged >/dev/null; ab-stand.sh boot primary healthy >/dev/null 2>&1");
await settings();
t = await card();
check(t.includes("REIMU 5564 2.1.0") && /Подтверждена\s*2\.1\.0/.test(t), "after reboot: 2.1.0 healthy and committed");
check(/Другой чип\s*REIMU 5564 2\.1\.0/.test(t) && md5("A", 0, 48) === md5("B", 0, 48), "committed image copied to the backup automatically");
check(vm("cat /var/lib/ab-stand/env.A") === "wdcontinue=0", "wdcontinue cleared");

// ---- broken update: health check fails, WDT2 falls back ----
await upload("fw-2.2.0-broken.tar");
check((await card()).includes("Образ принят"), "upload 2.2.0-broken: accepted");
await sleep(1500);
vm("ab-stand.sh flash-staged >/dev/null; ab-stand.sh boot primary broken >/dev/null 2>&1");
await settings();
t = await card();
check(t.includes("REIMU 5564 2.2.0-broken") && t.includes("проверка не пройдена") && /взведён/.test(t), "broken image: health check failed, WDT2 stays armed");
check(vm("ab-stand.sh wdt | grep enabled") === "enabled=1", "WDT2 enabled in the registers");
await page.screenshot({ path: `${OUT}/fw-unhealthy.png`, fullPage: true });

vm("ab-stand.sh boot backup >/dev/null 2>&1"); // WDT2 expired: SoC starts the second chip
await settings();
t = await card();
check(t.includes("резервный чип") && t.includes("BMC загрузился с резервного чипа") && t.includes("REIMU 5564 2.1.0"), "fallback: running the previous image 2.1.0 from the backup chip");
check(/Другой чип\s*REIMU 5564 2\.2\.0-broken/.test(t), "the broken image is on the other (primary) chip");
check(await page.locator("#fw-card button", { hasText: "Загрузить и активировать" }).isDisabled() && t.includes("Недоступно при работе с резервного чипа"), "update disabled while on backup");
check(vm("ab-stand.sh wdt | grep enabled") === "enabled=0", "WDT2 disarmed on the backup (no ping-pong)");
vm("echo x > /run/initramfs/image-rofs; sleep 3");
check(vm("ls /run/initramfs | wc -l") === "0", "an update staged behind the UI's back is removed on backup");
await page.screenshot({ path: `${OUT}/fw-backup.png`, fullPage: true });

await button("Восстановить основной чип").click();
await waitCard("готово");
check(md5("A", 0, 48) === md5("B", 0, 48) && md5("A", 48, 16) === rwA, "primary chip restored from the backup, its rwfs kept");
await button("Загрузиться с основного").click();
await sleep(4000);
check(vm("ab-stand.sh wdt | grep clear") === "clear=00000001", "reboot-primary cleared the boot code selection (and rebooted)");

vm("ab-stand.sh boot primary >/dev/null 2>&1");
await settings();
t = await card();
check(t.includes("основной чип") && t.includes("REIMU 5564 2.1.0") && !t.includes("ожидает"), "back on the primary chip with 2.1.0");
check(errors.length === 0, `no page errors (${errors.join("; ")})`);
await browser.close();
console.log(failed ? `RESULT: ${failed} FAILED` : "RESULT: ALL PASSED");
process.exit(failed ? 1 : 0);
