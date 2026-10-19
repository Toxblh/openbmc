// E2E: the tmpfs web devkit (tools/webdev) on a BMC with "stock" firmware.
// The test VM is turned into a stock REIMU BMC first: the bmcweb of the
// REIMU revision without this tree's patches (STOCK_BMCWEB, built by
// build-testvm.sh as bmcweb-orig) and an old web UI page are bind mounted.
// reimu-serial stays as the stand-in for obmc-console-server (/console0).
// Then the devkit built with TARGET=x86_64 (DEVKIT) is started from /tmp:
//   --web-only: new UI on the stock bmcweb (console falls back to /console0)
//   full:       devkit bmcweb, nbd-proxy, reimu-serial, reimu-pnsd
// and stopped, which must bring back the stock state exactly.
import { chromium } from "playwright";
import { BASE, VMSSH, CHROME, OUT, COM1 } from "./common.mjs";
import { execSync } from "node:child_process";
import { mkdirSync } from "node:fs";
import net from "node:net";

const DEVKIT = process.env.DEVKIT; // reimu-webdev-*-x86_64.tar.gz
const STOCK_BMCWEB = process.env.STOCK_BMCWEB;
if (!DEVKIT || !STOCK_BMCWEB) throw new Error("set DEVKIT and STOCK_BMCWEB");
let failed = 0;
const check = (c, what) => {
  console.log(`${c ? "PASS" : "FAIL"} ${what}`);
  if (!c) failed++;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const vm = (cmd, input) => execSync(`${VMSSH} sh -s`, { input: input ?? cmd, encoding: "utf8" }).trim();
const vmIn = (cmd, file) => execSync(`${VMSSH} '${cmd}' < '${file}'`, { encoding: "utf8" }).trim();
mkdirSync(OUT, { recursive: true });

// ---- the "stock" BMC ----
vmIn("cat > /root/bmcweb.stock && chmod 755 /root/bmcweb.stock", STOCK_BMCWEB);
vm(`set -e
mkdir -p /root/www.stock && echo '<html><body><h1 id="stock">phosphor-webui (stock)</h1></body></html>' > /root/www.stock/index.html
mountpoint -q /usr/share/www || mount --bind /root/www.stock /usr/share/www
grep -q ' /usr/bin/bmcweb ' /proc/mounts || mount --bind /root/bmcweb.stock /usr/bin/bmcweb
systemctl stop bmcweb.service; systemctl restart bmcweb.socket; sleep 2`);
const stockSum = vm("md5sum /usr/bin/bmcweb /usr/share/www/index.html; systemctl is-active reimu-serial reimu-pnsd");
const page0 = execSync(`curl -sk ${BASE}/`, { encoding: "utf8" });
check(page0.includes("phosphor-webui (stock)"), "stand: stock web UI served by the stock bmcweb");

// a REIMU BMC has /tmp on tmpfs, the VM doesn't
vm("mountpoint -q /tmp/reimu-webdev && umount /tmp/reimu-webdev; mkdir -p /tmp/reimu-webdev && mount -t tmpfs -o size=64m webdev /tmp/reimu-webdev");
vmIn("tar xzf - -C /tmp", DEVKIT);
check(vm("stat -f -c %T /tmp/reimu-webdev") === "tmpfs", "devkit unpacked to tmpfs");

const browser = await chromium.launch({ executablePath: CHROME });
async function login() {
  const ctx = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 1400, height: 900 } });
  const page = await ctx.newPage();
  page.errors = [];
  page.on("pageerror", (e) => page.errors.push(e.message));
  page.on("dialog", (d) => d.accept());
  await page.goto(BASE + "/");
  await page.fill("#login-pass", "0penBmc");
  await page.click("#login-form button[type=submit]");
  await page.waitForSelector("#app:not(.hidden)");
  return page;
}
function com1(text) {
  return new Promise((resolve, reject) => {
    const s = net.createConnection(COM1, () => s.end(text, resolve));
    s.on("error", reject);
  });
}

// ---- --web-only: new UI, stock bmcweb ----
let out = vm("/tmp/reimu-webdev/reimu-webdev start --web-only 2>&1");
check(/running .*web UI only/.test(out), `start --web-only (${out.split("\n").pop()})`);
let page = await login();
await page.waitForSelector("#ov-grid .kv");
check((await page.$eval("#ov-grid", (e) => e.textContent)).includes("E8C-mATX (test)"), "web-only: overview from the stock bmcweb's Redfish");
await page.goto(BASE + "/#/console");
await page.waitForFunction(() => /штатная прошивка/.test(document.querySelector("#st-online")?.textContent || ""), null, { timeout: 30000 });
check(true, "web-only: console falls back to /console0 (no /reimu/serial in the stock bmcweb)");
const marker = `webdev-legacy-${Date.now()}`;
await com1(`${marker}\r\n`);
await page.waitForFunction((m) => document.querySelector(".logview")?.textContent.includes(m), marker, { timeout: 15000 });
check(true, "web-only: platform output arrives over /console0, timestamped in the browser");
await page.screenshot({ path: `${OUT}/webdev-legacy-console.png` });
check(page.errors.length === 0, `web-only: no page errors (${page.errors.join("; ")})`);
await page.context().close();
out = vm("/tmp/reimu-webdev/reimu-webdev stop 2>&1");
check(execSync(`curl -sk ${BASE}/`, { encoding: "utf8" }).includes("phosphor-webui (stock)"), "stop: stock UI back");

// ---- full devkit ----
out = vm('REIMU_WEBDEV_STOP="reimu-serial.service reimu-pnsd.service" /tmp/reimu-webdev/reimu-webdev start 2>&1');
check(/running /.test(out) && !/web UI only/.test(out), `start (${out.split("\n").pop()})`);
const st = vm("/tmp/reimu-webdev/reimu-webdev status");
if (process.env.DEBUG) console.log(st);
check(/\/usr\/bin\/bmcweb/.test(st) && /reimu-webdev-serial\s+active/.test(st) && /reimu-webdev-pnsd\s+active/.test(st), "status: bmcweb bound, devkit units active");
check(vm("readlink /proc/$(systemctl show -p MainPID --value reimu-webdev-serial)/exe").startsWith("/tmp/reimu-webdev/"), "reimu-serial runs from tmpfs");
check(vm("systemctl is-active reimu-serial reimu-pnsd || true") === "inactive\ninactive", "the BMC's own console daemons are stopped");
page = await login();
await page.goto(BASE + "/#/console");
await page.waitForFunction(() => / бод/.test(document.querySelector("#st-online")?.textContent || ""), null, { timeout: 30000 });
const marker2 = `webdev-full-${Date.now()}`;
await com1(`${marker2}\r\n`);
await page.waitForFunction((m) => document.querySelector(".logview")?.textContent.includes(m), marker2, { timeout: 15000 });
check(true, "full: console over /reimu/serial (devkit bmcweb + devkit reimu-serial)");
await page.goto(BASE + "/#/hardware");
await page.waitForFunction(() => /Перечитать флеш ПНС/.test(document.querySelector("#hw-pns")?.textContent || ""), null, { timeout: 30000 });
check(true, "full: ПНС page from the devkit's reimu-pnsd");
await page.goto(BASE + "/#/media");
await page.waitForSelector("#app:not(.hidden)");
await sleep(1500);
check(page.errors.length === 0, `full: no page errors (${page.errors.join("; ")})`);
await page.screenshot({ path: `${OUT}/webdev-full.png` });
await page.context().close();

out = vm("/tmp/reimu-webdev/reimu-webdev stop 2>&1");
await sleep(2000);
check(vm("md5sum /usr/bin/bmcweb /usr/share/www/index.html; systemctl is-active reimu-serial reimu-pnsd") === stockSum, "stop: binaries, UI and the BMC's daemons exactly as before");
check(vm("systemctl list-units --plain --no-legend 'reimu-webdev*' | wc -l; ls /run/reimu-webdev 2>/dev/null | wc -l") === "0\n0", "stop: no devkit units or state left");
check(execSync(`curl -sk ${BASE}/`, { encoding: "utf8" }).includes("phosphor-webui (stock)"), "stop: stock UI answers");

// ---- a broken devkit bmcweb rolls back by itself ----
vm("cp /tmp/reimu-webdev/bin/bmcweb /tmp/bmcweb.good && printf '#!/bin/sh\\nexit 1\\n' > /tmp/reimu-webdev/bin/bmcweb");
out = vm('REIMU_WEBDEV_STOP="reimu-serial.service reimu-pnsd.service" /tmp/reimu-webdev/reimu-webdev start 2>&1; echo rc=$?');
check(/rc=1/.test(out) && /doesn't answer/.test(out), "a devkit whose bmcweb fails: start reports it");
check(vm("md5sum /usr/bin/bmcweb /usr/share/www/index.html; systemctl is-active reimu-serial reimu-pnsd") === stockSum, "...and rolls everything back");
vm("mv /tmp/bmcweb.good /tmp/reimu-webdev/bin/bmcweb");

// ---- leave the VM as it was ----
vm("umount /usr/bin/bmcweb; umount /usr/share/www; umount /tmp/reimu-webdev; systemctl stop bmcweb.service; systemctl restart bmcweb.socket");
await browser.close();
console.log(failed ? `RESULT: ${failed} FAILED` : "RESULT: ALL PASSED");
process.exit(failed ? 1 : 0);
