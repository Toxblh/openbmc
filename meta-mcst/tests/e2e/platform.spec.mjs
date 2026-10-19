// E2E: hardware pages (power, sensors, inventory, PNS) against real bmcweb
// Redfish/REST code; phosphor D-Bus services are emulated by fake-phosphor.py
// and the PNS flash is a block2mtd device with a synthetic image.
import { chromium } from "playwright";
import { BASE, VMSSH, CHROME, OUT, WORK, COM1 } from "./common.mjs";
import { execSync } from "node:child_process";

let failed = 0;
const check = (c, what) => {
  console.log(`${c ? "PASS" : "FAIL"} ${what}`);
  if (!c) failed++;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const vm = (cmd) => execSync(`${VMSSH} sh -s`, { input: cmd, encoding: "utf8" }).trim();

vm(`busctl set-property xyz.openbmc_project.State.Host /xyz/openbmc_project/state/host0 xyz.openbmc_project.State.Host RequestedHostTransition s xyz.openbmc_project.State.Host.Transition.Off; rm -f /var/lib/reimu-pns/scan.json /tmp/spi-gpio-log; sleep 2`);

const browser = await chromium.launch({ executablePath: CHROME });
const ctx = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 1400, height: 900 } });
const page = await ctx.newPage();
const errors = [];
page.on("pageerror", (e) => errors.push(e.message));
page.on("dialog", (d) => d.accept());
const toasts = () => page.$$eval(".toast", (t) => t.map((x) => x.textContent));

await page.goto(BASE + "/");
await page.fill("#login-pass", "0penBmc");
await page.click("#login-form button[type=submit]");
await page.waitForSelector("#app:not(.hidden)");
await page.waitForFunction(() => document.querySelector("#power-text").textContent === "Выключена", null, { timeout: 20000 });
check(true, "power state chip: platform off");
await page.waitForSelector("#ov-grid .kv");
const ov = await page.$eval("#ov-grid", (e) => e.textContent);
check(ov.includes("E8C-mATX (test)") && ov.includes("TEST-0001"), "overview: platform model/serial from inventory");
check(ov.includes("reimu-1.0.2-web (test VM)"), "overview: BMC firmware version");

// sensors while the platform is off: only BMC's own
await page.goto(BASE + "/#/sensors");
await page.waitForSelector("#sens-body .tbl");
let sens = await page.$eval("#sens-body", (e) => e.textContent);
check(sens.includes("BMC Temp") && !sens.includes("CPU0 Temp"), "sensors (off): only BMC sensors");

// power on through the menu (Redfish ComputerSystem.Reset)
await page.click("#power-menu-btn");
await page.click('#power-menu button[data-reset="On"]');
await page.waitForFunction(() => document.querySelector("#power-text").textContent === "Включена", null, { timeout: 30000 });
check(true, "power on via Redfish Reset: chip shows 'Включена'");
check((await toasts()).some((t) => t.includes("включить")), "power action toast");
await sleep(6000); // sensors auto-refresh every 5 s
sens = await page.$eval("#sens-body", (e) => e.textContent);
check(sens.includes("CPU0 Temp") && sens.includes("FAN1") && sens.includes("P12V"), "sensors (on): platform temps, fans and voltages appear");
const cpuRow = await page.$$eval("#sens-body tr", (rows) => rows.map((r) => [...r.cells].map((c) => c.textContent)).find((r) => r[0] === "CPU0 Temp"));
check(cpuRow && /°C/.test(cpuRow[1]) && cpuRow[5] === "75.00" && cpuRow[6] === "90.00", `CPU0 reading and thresholds: ${cpuRow && cpuRow.join(" | ")}`);
await page.fill('input[placeholder="Фильтр по имени…"]', "fan");
await sleep(300);
const filtered = await page.$$eval("#sens-body tbody tr", (r) => r.length);
check(filtered === 2, `sensor filter "fan": ${filtered} rows`);
await page.fill('input[placeholder="Фильтр по имени…"]', "");
await page.screenshot({ path: `${OUT}/08-sensors.png`, fullPage: true });

// hardware inventory + PNS
await page.goto(BASE + "/#/hardware");
await page.waitForSelector("#hw-pns .grid");
await page.waitForFunction(() => document.querySelector("#hw-inv").textContent.includes("Модули памяти"), null, { timeout: 30000 }).catch(() => {});
const hw = await page.$eval("#hw-inv", (e) => e.textContent);
check(/Процессоры: 2/.test(hw), "hardware: 2 processors from inventory");
check(/Модули памяти: 4/.test(hw), "hardware: 4 DIMMs from inventory");
const pns = await page.$eval("#hw-pns", (e) => e.textContent);
check(pns.includes("MCST (test fixture)") && pns.includes("E8C-mATX (test)"), "PNS: FRU from mcst-fruid shown");
check(pns.includes("MCST E8C-mATX (test fixture)"), "PNS: device tree model shown");
// rescan while the platform is on must be refused
await page.click('button:has-text("Перечитать флеш ПНС")');
await sleep(3000);
check((await toasts()).some((t) => t.includes("Платформа включена")), "PNS rescan refused while platform is on");
check(vm("cat /tmp/spi-gpio-log 2>/dev/null | wc -l") === "0", "SPI switch untouched while host is on");
// power off, then rescan
await page.click("#power-menu-btn");
await page.click('#power-menu button[data-reset="ForceOff"]');
await page.waitForFunction(() => document.querySelector("#power-text").textContent === "Выключена", null, { timeout: 30000 });
await page.click('button:has-text("Перечитать флеш ПНС")');
await page.waitForFunction(() => document.querySelector("#hw-pns").textContent.includes("Таблица разделов"), null, { timeout: 30000 });
const pns2 = await page.$eval("#hw-pns", (e) => e.textContent);
check(pns2.includes("версия 1, 3 записей") && pns2.includes("device tree (DTB)") && pns2.includes("0x00200000"), "PNS flash partition table read via MTD");
check(pns2.includes("16.0 МиБ"), "PNS flash size");
const gpio = vm("cat /tmp/spi-gpio-log | cut -d' ' -f2 | tr '\\n' ' '");
check(gpio === "1 0", `SPI attached for the scan and released after: "${gpio}"`);
await page.screenshot({ path: `${OUT}/09-hardware-pns.png`, fullPage: true });

// event log & settings render
await page.goto(BASE + "/#/logs");
await page.waitForSelector("#log-body");
await sleep(2000);
check(!(await page.$eval("#log-body", (e) => e.textContent)).includes("Ошибка"), "logs page renders");
await page.goto(BASE + "/#/settings");
await page.waitForSelector("#set-grid .card h3");
await sleep(1500);
const st = await page.$eval("#set-grid", (e) => e.textContent);
check(st.includes("root") && st.includes("NTP"), "settings: accounts and NTP");
await page.screenshot({ path: `${OUT}/10-settings.png`, fullPage: true });
await page.goto(BASE + "/#/");
await page.waitForSelector("#ov-grid .kv");
await page.screenshot({ path: `${OUT}/01-overview.png` });

check(errors.length === 0, `no JS errors (${errors.join(" | ")})`);
await browser.close();
console.log(failed ? `RESULT: ${failed} FAILED` : "RESULT: ALL PASSED");
process.exit(failed ? 1 : 0);
