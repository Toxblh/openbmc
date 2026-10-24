// E2E: the «Хост» page against a BMC in QEMU with the host agent connected
// (tests/ab64/test_hostlink_qemu.py runs it, HL_E2E=DIR).   node host.qemu.mjs
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
const card = (title) => page.locator(".card", { has: page.locator("h3", { hasText: title }) });
try {
  await page.goto(BASE + "/");
  await page.fill("#login-pass", "0penBmc");
  await page.click("#login-form button[type=submit]");
  await page.waitForSelector("#app:not(.hidden)");
  await page.click('#nav a[data-page="host"]');
  await card("Агент хоста").locator(".badge", { hasText: "подключён" }).waitFor({ timeout: 30000 });
  check(true, "agent card: подключён");
  const ag = await card("Агент хоста").innerText();
  check(/IPv4/.test(ag) && /elbrus/.test(ag), "agent: host and transport shown");
  const os = await card("ОС и процессор").innerText();
  check(/E8C2/.test(os) && /ALT Workstation/.test(os) && /Linux E2K/.test(os), "OS card: E8C2, ALT, boot chain");
  const gpu = await card("Видеокарта").innerText();
  check(/кристалл\s*61 °C/.test(gpu) && /об\/мин/.test(gpu), "GPU card: junction 61 °C, fan");
  const ecc = await card("Память ECC").innerText();
  check(/DIMM0/.test(ecc) && /Исправленные \(CE\)\s*4/.test(ecc), "ECC card: CE 4, DIMM0");
  const pci = await card("Устройства PCIe").locator("tbody tr").count();
  check(pci === 4, `PCIe table: ${pci} rows`);
  const disks = await card("Диски").innerText();
  check(/nvme0n1/.test(disks) && /S4EWNX0R123456/.test(disks), "disks: model/serial");
  await page.screenshot({ path: `${OUT}/host-page.png`, fullPage: true });
  // the fan: manual 50 % from the page
  const g = card("Видеокарта");
  await g.locator('input[type="number"]').fill("50");
  await g.getByRole("button", { name: "Ручной" }).click();
  await page.waitForFunction(async () => {
    const r = await fetch("/xyz/openbmc_project/reimu/hostlink", { credentials: "same-origin" }).then((x) => x.json());
    const s = JSON.parse(r.data.Json);
    return s.policy.gpu_fan.mode === "manual" && s.policy.gpu_fan.duty === 50;
  }, null, { timeout: 15000, polling: 1000 });
  check(true, "«Ручной» 50 %: the BMC's policy");
  await g.getByRole("button", { name: "Авто" }).click();
  await page.waitForTimeout(1500);
  // the pairing box
  page.once("dialog", (d) => d.accept());
  await card("Агент хоста").getByRole("button", { name: /Подключить/ }).click();
  await page.locator(".note.pairbox", { hasText: "одноразовый ключ" }).waitFor({ timeout: 15000 });
  const cmd = await page.locator(".note.pairbox input.mono").last().inputValue();
  check(/--pinnedpubkey 'sha256\/\/[A-Za-z0-9+\/=]{44}' -k https:\/\/[0-9.]+\/agent\/install\.sh\).* \| sudo sh -s -- --pair 'reimu1:[A-Za-z0-9_-]+'$/.test(cmd),
    "pairing box: the install command (agent-install.qemu.mjs has the rest)");
  await page.screenshot({ path: `${OUT}/host-pair.png` });
  // the sensors page has the GPU when Redfish lists it (QEMU: the chassis needs the FRU)
  await page.click('#nav a[data-page="sensors"]');
  await page.locator("#sens-body table").first().waitFor({ timeout: 60000 }).catch(() => {});
  await page.locator("#sens-body", { hasText: /Host[ _]GPU[ _]Junction/ }).waitFor({ timeout: 30000 }).catch(() => {});
  const sens = await page.locator("#sens-body").innerText();
  await page.screenshot({ path: `${OUT}/host-sensors.png`, fullPage: true });
  if (!/Host[ _]GPU/.test(sens)) console.log(`INFO the sensors page: ${sens.slice(0, 800).replace(/\s+/g, " ")}`);
  // from Redfish when the chassis lists them (the board), else from the agent's data
  check(/Host[ _]GPU[ _]Junction\s+61(\.0)?\s*°C/.test(sens) && /Host[ _]GPU[ _]Fan\s+0\s*об\/мин/.test(sens), "sensors page: the GPU's temperature and fan");
} catch (e) {
  check(false, `${e.message}`.split("\n")[0]);
  await page.screenshot({ path: `${OUT}/host-fail.png` }).catch(() => {});
}
check(errors.length === 0, `no page errors ${JSON.stringify(errors)}`);
await browser.close();
process.exit(failed ? 1 : 0);
