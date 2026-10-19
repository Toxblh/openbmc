// Stress / robustness for read-write virtual media.
import { chromium } from "playwright";
import { BASE, VMSSH, CHROME, OUT, WORK, COM1 } from "./common.mjs";
import { execSync } from "node:child_process";

let failed = 0;
const check = (c, what) => {
  console.log(`${c ? "PASS" : "FAIL"} ${what}`);
  if (!c) failed++;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const vm = (cmd, opts = {}) => {
  try {
    return execSync(`${VMSSH} sh -s`, { input: cmd, encoding: "utf8", timeout: opts.timeout || 600000, stdio: ["pipe", "pipe", "pipe"] }).trim();
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
const findDisk = `for d in /sys/block/sd*; do [ -e $d ] && echo $(basename $d) $(cat $d/ro) $(cat $d/size); done`;

const PROFILE = `${WORK}/chrome-profile`;
// a persistent profile: OPFS images survive a browser crash like for a real user
const launch = () => chromium.launchPersistentContext(PROFILE, { executablePath: CHROME, ignoreHTTPSErrors: true });

async function session(browser) {
  const ctx = browser;
  const page = ctx.pages()[0] || (await ctx.newPage());
  page.on("dialog", (d) => d.accept());
  page.on("pageerror", (e) => console.log("pageerror", e.message));
  await page.goto(BASE + "/");
  await page.fill("#login-pass", "0penBmc");
  await page.click("#login-form button[type=submit]");
  await page.waitForSelector("#app:not(.hidden)");
  await page.goto(BASE + "/#/media");
  await page.waitForSelector(".slot");
  return { ctx, page };
}

async function connectOpfs(page, name, mib, fresh) {
  if (fresh) {
    await page.evaluate(
      async ([n, m]) => {
        const d = await window.reimuMedia.imagesDir();
        await d.removeEntry(n).catch(() => {});
        const fh = await d.getFileHandle(n, { create: true });
        const w = await fh.createWritable();
        await w.truncate(m * 1024 * 1024);
        await w.close();
        await window.reimuMedia.refreshLibrary();
      },
      [name, mib],
    );
  }
  await page.evaluate(() => window.reimuMedia.refreshLibrary());
  const slot = page.locator(".slot").nth(0);
  await slot.locator("select").first().selectOption("opfs");
  await slot.locator("select").nth(1).selectOption(name);
  await slot.locator('button:has-text("Подключить")').click();
  await page.waitForFunction(() => document.querySelector(".slot .state").textContent.includes("чтение и запись"), null, { timeout: 30000 });
  const disk = await waitVm(findDisk, (o) => /^sd\w+ 0/m.test(o), 60000);
  return (disk.match(/^(sd\w+) 0/m) || [])[1];
}

/* 1. concurrent random writes, verified after reconnect */
let browser = await launch();
let { page } = await session(browser);
let sd = await connectOpfs(page, "stress.img", 128, true);
check(!!sd, `128 MiB RW disk attached as ${sd}`);
vm("head -c 25165824 /dev/urandom > /root/rand.bin");
const t0 = Date.now();
// 3 parallel writers to different regions with O_DIRECT, plus 1 reader
vm(
  `dd if=/root/rand.bin of=/dev/${sd} bs=64k count=128 seek=0 oflag=direct conv=notrunc 2>/dev/null &
   dd if=/root/rand.bin of=/dev/${sd} bs=64k count=128 skip=128 seek=640 oflag=direct conv=notrunc 2>/dev/null &
   dd if=/root/rand.bin of=/dev/${sd} bs=64k count=128 skip=256 seek=1280 oflag=direct conv=notrunc 2>/dev/null &
   dd if=/dev/${sd} of=/dev/null bs=1M count=32 iflag=direct 2>/dev/null &
   wait; sync`,
  { timeout: 900000 },
);
const sec = (Date.now() - t0) / 1000;
check(true, `24 MiB written by 3 parallel O_DIRECT writers + 1 reader in ${sec.toFixed(1)} s (${(24 / sec).toFixed(2)} MiB/s under TCG)`);
const expect = vm(`
  dd if=/root/rand.bin bs=64k count=128 2>/dev/null | sha256sum | cut -c1-16
  dd if=/root/rand.bin bs=64k count=128 skip=128 2>/dev/null | sha256sum | cut -c1-16
  dd if=/root/rand.bin bs=64k count=128 skip=256 2>/dev/null | sha256sum | cut -c1-16`);
await page.locator(".slot").nth(0).locator('button:has-text("Отключить")').click();
await waitVm(findDisk, (o) => !/^sd/m.test(o), 30000);
sd = await connectOpfs(page, "stress.img", 128, false);
const got = vm(`
  dd if=/dev/${sd} bs=64k count=128 skip=0 iflag=direct 2>/dev/null | sha256sum | cut -c1-16
  dd if=/dev/${sd} bs=64k count=128 skip=640 iflag=direct 2>/dev/null | sha256sum | cut -c1-16
  dd if=/dev/${sd} bs=64k count=128 skip=1280 iflag=direct 2>/dev/null | sha256sum | cut -c1-16`);
check(got === expect && expect.split("\n").length === 3, `all three regions intact after reconnect (${got.replace(/\n/g, " ")})`);
const gapZero = vm(`dd if=/dev/${sd} bs=64k count=1 skip=200 iflag=direct 2>/dev/null | tr -d '\\0' | wc -c`);
check(gapZero === "0", "untouched region still zero (no misplaced writes)");

/* 2. filesystem integrity: ext4 with journal, many small files, fsck */
vm(`mkfs.ext4 -q -F /dev/${sd} && mkdir -p /mnt/vm && mount /dev/${sd} /mnt/vm && for i in $(seq 1 300); do head -c $((i*97)) /dev/urandom > /mnt/vm/f$i; done && sha256sum /mnt/vm/f* > /root/sums && rm /mnt/vm/f1?? && sync && umount /mnt/vm`, { timeout: 900000 });
const fsck = vm(`fsck.ext4 -fn /dev/${sd} 2>&1; echo rc=$?`, { allowFail: true });
check(/rc=0/.test(fsck), `fsck clean after 300 files + deletes: ${fsck.split("\n").pop()}`);
const verify = vm(`mount -o ro /dev/${sd} /mnt/vm && cd / && grep -v '/f1[0-9][0-9]$' /root/sums | sha256sum -c --quiet 2>&1 | wc -l; umount /mnt/vm`);
check(verify === "0", "all remaining files verify by sha256");

/* 3. abrupt browser death while the host has the disk mounted */
vm(`mount /dev/${sd} /mnt/vm && echo before-crash > /mnt/vm/crash.txt && sync`);
await browser.close(); // kills the tab: websocket drops without NBD_CMD_DISC
const gone = await waitVm(`${findDisk}; ls /sys/kernel/config/usb_gadget/; pgrep -c nbd-proxy || true`, (o) => !/^sd/m.test(o) && !/mass-storage/.test(o), 60000);
check(!/mass-storage/.test(gone), `BMC removed the gadget after the browser died (${gone.replace(/\n/g, " ")})`);
vm("umount -l /mnt/vm 2>/dev/null; true", { allowFail: true });
check(vm("cat /sys/block/nbd0/size") === "0", "nbd0 released");
browser = await launch();
({ page } = await session(browser));
sd = await connectOpfs(page, "stress.img", 128, false);
const after = vm(`mount -o ro /dev/${sd} /mnt/vm && cat /mnt/vm/crash.txt; umount /mnt/vm`, { allowFail: true });
check(after.includes("before-crash"), `new session after crash works, synced data survived: ${after}`);
await page.locator(".slot").nth(0).locator('button:has-text("Отключить")').click();
await waitVm(findDisk, (o) => !/^sd/m.test(o), 30000);

/* 4. second client is rejected while a session is active */
sd = await connectOpfs(page, "stress.img", 128, false);
const b2 = await chromium.launch({ executablePath: CHROME });
const ctx2 = await b2.newContext({ ignoreHTTPSErrors: true });
const p2 = await ctx2.newPage();
await p2.goto(BASE + "/");
await p2.fill("#login-pass", "0penBmc");
await p2.click("#login-form button[type=submit]");
await p2.waitForSelector("#app:not(.hidden)");
const reason = await p2.evaluate(
  () =>
    new Promise((res) => {
      const m = document.cookie.match(/XSRF-TOKEN=([^;]*)/);
      const ws = new WebSocket(`wss://${location.host}/vm/0/0`, [m[1]]);
      ws.onclose = (e) => res(e.reason || String(e.code));
      setTimeout(() => res("still open"), 5000);
    }),
);
check(/already/i.test(reason), `second session on the same slot rejected: "${reason}"`);
check(vm(findDisk).startsWith(sd), "first session unaffected");
await page.locator(".slot").nth(0).locator('button:has-text("Отключить")').click();
await waitVm(findDisk, (o) => !/^sd/m.test(o), 30000);

await b2.close();
await browser.close();
console.log(failed ? `RESULT: ${failed} FAILED` : "RESULT: ALL PASSED");
process.exit(failed ? 1 : 0);
