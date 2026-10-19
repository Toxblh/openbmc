// E2E: read-write virtual media. Browser NBD server -> bmcweb /vm/0/N ->
// nbd-proxy -> nbd-client -> /dev/nbdN -> USB mass storage gadget ->
// dummy_hcd -> /dev/sdX (/dev/sr0) inside the test VM acting as the host.
import { chromium } from "playwright";
import { BASE, VMSSH, CHROME, OUT, WORK, COM1 } from "./common.mjs";
import { execSync } from "node:child_process";
import fs from "node:fs";
import crypto from "node:crypto";

fs.mkdirSync(WORK, { recursive: true });
fs.mkdirSync(OUT, { recursive: true });

let failed = 0;
const check = (c, what) => {
  console.log(`${c ? "PASS" : "FAIL"} ${what}`);
  if (!c) failed++;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const vm = (cmd, opts = {}) => {
  try {
    return execSync(`${VMSSH} sh -s`, { input: cmd, encoding: "utf8", timeout: opts.timeout || 120000, stdio: ["pipe", "pipe", "pipe"] }).trim();
  } catch (e) {
    if (opts.allowFail) return `ERR:${(e.stdout || "").trim()} ${(e.stderr || "").trim()}`;
    throw e;
  }
};
async function waitVm(cmd, pred, timeoutMs = 60000) {
  const t0 = Date.now();
  let out = "";
  while (Date.now() - t0 < timeoutMs) {
    out = vm(cmd, { allowFail: true });
    if (pred(out)) return out;
    await sleep(1000);
  }
  return out;
}
// USB disk enumerated by dummy_hcd: find the sd device whose vendor is OpenBMC gadget
const findDisk = `for d in /sys/block/sd*; do [ -e $d ] && echo $(basename $d) $(cat $d/ro) $(cat $d/size); done`;

const browser = await chromium.launch({ executablePath: CHROME });
const ctx = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 1400, height: 900 }, acceptDownloads: true });
const page = await ctx.newPage();
const errors = [];
page.on("pageerror", (e) => errors.push(e.message));
page.on("dialog", (d) => d.accept());
await page.goto(BASE + "/");
await page.fill("#login-pass", "0penBmc");
await page.click("#login-form button[type=submit]");
await page.waitForSelector("#app:not(.hidden)");
await page.goto(BASE + "/#/media");
await page.waitForSelector(".slot");

/* ---------- 1. OPFS image, read-write ---------- */
const imgName = "rw-test.img";
await page.evaluate(async (n) => {
  const d = await window.reimuMedia.imagesDir();
  await d.removeEntry(n).catch(() => {});
}, imgName);
await page.fill('input[placeholder="имя.img"]', imgName);
await page.fill('input[title="МиБ"]', "64");
await page.click('button:has-text("Создать пустой")');
await page.waitForSelector(`td.mono:has-text("${imgName}")`);
check(true, "blank 64 MiB image created in browser storage (OPFS)");

const slot0 = page.locator(".slot").nth(0);
await slot0.locator("select").first().selectOption("opfs");
await slot0.locator("select").nth(1).selectOption(imgName);
await slot0.locator('button:has-text("Подключить")').click();
await page.waitForFunction(() => document.querySelector(".slot .state").textContent.includes("чтение и запись"), null, { timeout: 30000 });
check(true, "slot 0 connected read-write");

let disk = await waitVm(findDisk, (o) => /^sd\w+ 0 131072/m.test(o), 60000);
const sd = (disk.match(/^(sd\w+) 0 131072/m) || [])[1];
check(!!sd, `host sees writable 64 MiB USB disk: ${disk}`);
check(vm("cat /sys/block/nbd0/ro") === "0", "nbd0 is read-write on BMC");
check(vm("cat /sys/kernel/config/usb_gadget/mass-storage0/functions/mass_storage.usb0/lun.0/ro") === "0", "gadget LUN exported read-write");
await page.screenshot({ path: `${OUT}/05-media-connected.png` });

// host formats the disk and writes files
const payload = crypto.randomBytes(8 * 1024 * 1024);
const payloadMd5 = crypto.createHash("md5").update(payload).digest("hex");
fs.writeFileSync(`${WORK}/payload.bin`, payload);
execSync(`${VMSSH} "cat > /root/payload.bin" < ${WORK}/payload.bin`);
const t0 = Date.now();
vm(`mkfs.vfat -n REIMUTEST /dev/${sd} >/dev/null && mkdir -p /mnt/vm && mount /dev/${sd} /mnt/vm && cp /root/payload.bin /mnt/vm/ && echo 'Привет из платформы' > /mnt/vm/hello.txt && sync && umount /mnt/vm`, { timeout: 300000 });
const writeSec = (Date.now() - t0) / 1000;
check(true, `host: mkfs.vfat + 8 MiB file + sync + umount in ${writeSec.toFixed(1)} s`);
// read back after dropping caches: data comes from the browser again
vm("echo 3 > /proc/sys/vm/drop_caches");
const back = vm(`mount -o ro /dev/${sd} /mnt/vm && md5sum /mnt/vm/payload.bin | cut -d' ' -f1 && cat /mnt/vm/hello.txt; umount /mnt/vm`);
check(back.split("\n")[0] === payloadMd5, `read back after cache drop: md5 ${back.split("\n")[0]}`);
check(back.includes("Привет из платформы"), "text file written by host reads back");
const stats = await slot0.locator(".mono").textContent();
check(/запись: [1-9]\d* оп/.test(stats), `browser counted writes: ${stats.replace(/\s+/g, " ")}`);

// raw throughput: sequential read of the whole disk
vm("echo 3 > /proc/sys/vm/drop_caches");
const t1 = Date.now();
vm(`dd if=/dev/${sd} of=/dev/null bs=1M 2>/dev/null`, { timeout: 300000 });
const readMBs = 64 / ((Date.now() - t1) / 1000);
check(readMBs > 0.5, `sequential read through browser: ${readMBs.toFixed(1)} MiB/s (QEMU TCG emulation)`);

// disconnect: host disk disappears
await slot0.locator('button:has-text("Отключить")').click();
await page.waitForFunction(() => document.querySelector(".slot .state").textContent.includes("отключён"), null, { timeout: 30000 });
disk = await waitVm(findDisk, (o) => !/^sd/m.test(o), 30000);
check(!/^sd/m.test(disk), "USB disk removed from host after disconnect");
check(vm("ls /sys/kernel/config/usb_gadget/") === "", "gadget cleaned up on BMC");

// image persisted in OPFS: download and inspect on this machine with mtools
const [dl] = await Promise.all([page.waitForEvent("download"), page.locator(`tr:has-text("${imgName}") button:has-text("Скачать")`).click()]);
const imgPath = `${WORK}/downloaded.img`;
fs.copyFileSync(await dl.path(), imgPath);
const dir = execSync(`MTOOLS_SKIP_CHECK=1 mdir -i ${imgPath} ::`, { encoding: "utf8" });
check(dir.includes("REIMUTEST") && /PAYLOAD\s+BIN|payload\.bin/i.test(dir), "downloaded image is a FAT volume with the host's files");
execSync(`MTOOLS_SKIP_CHECK=1 mcopy -o -i ${imgPath} ::/payload.bin ${WORK}/payload.out`);
check(crypto.createHash("md5").update(fs.readFileSync(`${WORK}/payload.out`)).digest("hex") === payloadMd5, "file extracted from downloaded image matches md5");

// reconnect: data persists across sessions
await slot0.locator('button:has-text("Подключить")').click();
await page.waitForFunction(() => document.querySelector(".slot .state").textContent.includes("чтение и запись"), null, { timeout: 30000 });
disk = await waitVm(findDisk, (o) => /^sd\w+ 0/m.test(o), 60000);
const sd2 = (disk.match(/^(sd\w+) 0/m) || [])[1];
const again = vm(`mount -o ro /dev/${sd2} /mnt/vm && md5sum /mnt/vm/payload.bin | cut -d' ' -f1; umount /mnt/vm`);
check(again === payloadMd5, "data persists after reconnect");
await slot0.locator('button:has-text("Отключить")').click();
await waitVm(findDisk, (o) => !/^sd/m.test(o), 30000);

/* ---------- 2. local file, read-only ---------- */
await slot0.locator("select").first().selectOption("file-ro");
await slot0.locator('input[type="file"]').setInputFiles(imgPath);
await slot0.locator('button:has-text("Подключить")').click();
await page.waitForFunction(() => document.querySelector(".slot .state").textContent.includes("только чтение"), null, { timeout: 30000 });
disk = await waitVm(findDisk, (o) => /^sd\w+ 1/m.test(o), 60000);
const sdro = (disk.match(/^(sd\w+) 1/m) || [])[1];
check(!!sdro, `read-only file: host disk is read-only (${disk})`);
check(vm("cat /sys/block/nbd0/ro") === "1", "nbd0 read-only (NBD_FLAG_READ_ONLY from browser)");
const roWrite = vm(`dd if=/dev/zero of=/dev/${sdro} bs=4k count=1 oflag=direct 2>&1`, { allowFail: true });
check(/ERR|denied|Read-only|Operation not permitted|error/i.test(roWrite), `write to read-only disk rejected: ${roWrite.slice(0, 80)}`);
const roRead = vm(`mount -o ro /dev/${sdro} /mnt/vm && md5sum /mnt/vm/payload.bin | cut -d' ' -f1; umount /mnt/vm`);
check(roRead === payloadMd5, "read-only file serves correct data");
await slot0.locator('button:has-text("Отключить")').click();
await waitVm(findDisk, (o) => !/^sd/m.test(o), 30000);

/* ---------- 3. snapshot mode: writes kept in browser memory ---------- */
const before = crypto.createHash("md5").update(fs.readFileSync(imgPath)).digest("hex");
await slot0.locator("select").first().selectOption("snapshot");
await slot0.locator('input[type="file"]').setInputFiles(imgPath);
await slot0.locator('button:has-text("Подключить")').click();
await page.waitForFunction(() => document.querySelector(".slot .state").textContent.includes("чтение и запись"), null, { timeout: 30000 });
disk = await waitVm(findDisk, (o) => /^sd\w+ 0/m.test(o), 60000);
const sds = (disk.match(/^(sd\w+) 0/m) || [])[1];
vm(`mount /dev/${sds} /mnt/vm && echo snapshot-data > /mnt/vm/snap.txt && sync && umount /mnt/vm`);
await sleep(500);
const dlBtn = slot0.locator('button:has-text("Скачать образ")');
check(await dlBtn.isVisible(), "snapshot has unsaved changes -> 'download image' offered");
const [dl2] = await Promise.all([page.waitForEvent("download"), dlBtn.click()]);
const snapPath = `${WORK}/snapshot.img`;
fs.copyFileSync(await dl2.path(), snapPath);
const snapTxt = execSync(`MTOOLS_SKIP_CHECK=1 mtype -i ${snapPath} ::/snap.txt`, { encoding: "utf8" });
check(snapTxt.trim() === "snapshot-data", "modified image download contains host's write");
check(crypto.createHash("md5").update(fs.readFileSync(imgPath)).digest("hex") === before, "original file untouched in snapshot mode");
await slot0.locator('button:has-text("Отключить")').click();
await waitVm(findDisk, (o) => !/^sd/m.test(o), 30000);

/* ---------- 4. CD-ROM slot with an ISO ---------- */
fs.mkdirSync(`${WORK}/iso`, { recursive: true });
fs.writeFileSync(`${WORK}/iso/README.TXT`, "REIMU virtual CD-ROM test\n");
fs.writeFileSync(`${WORK}/iso/DATA.BIN`, crypto.randomBytes(4 * 1024 * 1024));
execSync(`genisoimage -quiet -V REIMU_CD -J -r -o ${WORK}/test.iso ${WORK}/iso`);
const slot1 = page.locator(".slot").nth(1);
await slot1.locator('input[type="file"]').setInputFiles(`${WORK}/test.iso`);
await slot1.locator('button:has-text("Подключить")').click();
await page.waitForFunction(() => document.querySelectorAll(".slot .state")[1].textContent.includes("подключён"), null, { timeout: 30000 });
const sr = await waitVm('for d in /sys/block/sr*; do grep -q File-CD $d/device/model 2>/dev/null && echo /dev/$(basename $d); done', (o) => /sr\d/.test(o), 60000);
check(/sr\d/.test(sr), `host sees USB CD-ROM: ${sr}`);
const cd = vm(`mount -t iso9660 -o ro ${sr.split("\n")[0]} /mnt/vm && cat /mnt/vm/README.TXT; umount /mnt/vm`, { allowFail: true });
check(cd.includes("REIMU virtual CD-ROM test"), `ISO content readable: ${cd.slice(0, 60)}`);
check(vm("cat /sys/kernel/config/usb_gadget/mass-storage1/functions/mass_storage.usb0/lun.0/cdrom") === "1", "gadget LUN in CD-ROM mode");
await page.screenshot({ path: `${OUT}/06-media-cdrom.png` });
await slot1.locator('button:has-text("Отключить")').click();
await sleep(2000);

check(errors.length === 0, `no JS errors (${errors.join(" | ")})`);
await browser.close();
console.log(failed ? `RESULT: ${failed} FAILED` : "RESULT: ALL PASSED");
process.exit(failed ? 1 : 0);
