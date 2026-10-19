// E2E for a BMC in QEMU (tests/qemu/run-bmc.sh: no USB host to see the disk):
// the browser's read-write NBD path, checked from the BMC side through /dev/nbd0.
import { chromium } from "playwright";
import { BASE, VMSSH, CHROME } from "./common.mjs";
import { execSync } from "node:child_process";
const vm = (c) => execSync(`${VMSSH} sh -s`, { input: c, encoding: "utf8", timeout: 300000 }).trim();
let failed = 0; const check = (c, w) => { console.log(`${c ? "PASS" : "FAIL"} ${w}`); if (!c) failed++; };
const browser = await chromium.launch({ executablePath: CHROME, args: ["--ignore-certificate-errors"] });
const ctx = await browser.newContext({ ignoreHTTPSErrors: true });
const page = await ctx.newPage();
page.on("dialog", (d) => d.accept());
await page.goto(BASE + "/");
await page.fill("#login-pass", "0penBmc");
await page.click("#login-form button[type=submit]");
await page.waitForSelector("#app:not(.hidden)");
await page.goto(BASE + "/#/media"); await page.waitForSelector(".slot");
const img = "nbd-rw.img";
await page.evaluate(async (n) => { const d = await window.reimuMedia.imagesDir(); await d.removeEntry(n).catch(() => {}); }, img);
await page.fill('input[placeholder="имя.img"]', img); await page.fill('input[title="МиБ"]', "16");
await page.click('button:has-text("Создать пустой")'); await page.waitForSelector(`td.mono:has-text("${img}")`);
const slot0 = page.locator(".slot").nth(0);
await slot0.locator("select").first().selectOption("opfs"); await slot0.locator("select").nth(1).selectOption(img);
await slot0.locator('button:has-text("Подключить")').click();
await page.waitForFunction(() => document.querySelector(".slot .state").textContent.includes("чтение и запись"), null, { timeout: 30000 });
check(true, "slot 0 connected read-write");
// 4 MiB at offset 1 MiB written through /dev/nbd0 (O_DIRECT: nothing cached on the BMC)
const out = vm(`set -e
dd if=/dev/urandom of=/tmp/pat bs=1M count=4 2>/dev/null
t0=$(date +%s%N); dd if=/tmp/pat of=/dev/nbd0 bs=1M seek=1 oflag=direct conv=fsync 2>/dev/null; t1=$(date +%s%N)
dd if=/dev/nbd0 of=/tmp/back bs=1M skip=1 count=4 iflag=direct 2>/dev/null; t2=$(date +%s%N)
echo $(md5sum < /tmp/pat | cut -c1-32) $(md5sum < /tmp/back | cut -c1-32) $(( (t1-t0)/1000000 )) $(( (t2-t1)/1000000 ))`);
const [m1, m2, wms, rms] = out.split(" ");
check(m1 === m2, `4 MiB written and read back through NBD (write ${wms} ms, read ${rms} ms)`);
const md5browser = await page.evaluate(async (n) => {
  const d = await window.reimuMedia.imagesDir(); const f = await (await d.getFileHandle(n)).getFile();
  const buf = new Uint8Array(await f.slice(1 << 20, 5 << 20).arrayBuffer());
  return Array.from(buf.slice(0, 16)).join(",");
}, img);
const bmcHead = vm(`dd if=/tmp/pat bs=16 count=1 2>/dev/null | od -An -tu1 | tr -s ' ' ',' | sed 's/^,//; s/,$//'`);
check(md5browser === bmcHead, "the data is in the browser's image (OPFS)");
check(vm("cat /sys/block/nbd0/ro") === "0", "nbd0 read-write");
await slot0.locator(`button:has-text("Отключить")`).click({ timeout: 10000 });
let gone = "";
for (let i = 0; i < 30 && gone !== "gone|0"; i++) { await page.waitForTimeout(1000); gone = vm("(cat /sys/kernel/config/usb_gadget/mass-storage0/UDC 2>/dev/null || echo gone); echo \"|$(pgrep nbd-proxy | wc -l)\"").replace(/\n/g, ""); }
check(gone === "gone|0", `gadget removed and nbd-proxy exited after disconnect (${gone})`);
await browser.close();
console.log(failed ? `${failed} FAILED` : "ALL PASSED"); process.exit(failed ? 1 : 0);
