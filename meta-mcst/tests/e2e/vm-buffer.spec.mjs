// Pipelined NBD reads through bmcweb /vm/0/0: several 128 KiB replies from
// the browser arrive back to back. Stock bmcweb (REIMU revision) keeps them in
// a fixed 128 KiB buffer; this checks whether the server survives.
import { chromium } from "playwright";
import { BASE, VMSSH, CHROME, OUT, WORK, COM1 } from "./common.mjs";
import { execSync } from "node:child_process";
const vm = (cmd) => { try { return execSync(`${VMSSH} sh -s`, { input: cmd, encoding: "utf8", timeout: 300000, stdio: ["pipe","pipe","pipe"] }).trim(); } catch (e) { return "ERR " + (e.stderr || ""); } };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const browser = await chromium.launch({ executablePath: CHROME });
const page = await (await browser.newContext({ ignoreHTTPSErrors: true })).newPage();
await page.goto("https://127.0.0.1:8443/");
await page.fill("#login-pass", "0penBmc");
await page.click("#login-form button[type=submit]");
await page.waitForSelector("#app:not(.hidden)");
await page.goto("https://127.0.0.1:8443/#/media");
await page.waitForSelector(".slot");
vm("journalctl --rotate >/dev/null 2>&1; journalctl --vacuum-time=1s >/dev/null 2>&1; true");
const pid0 = vm("systemctl show -p MainPID --value bmcweb");
await page.evaluate(async () => {
  const { NbdServer, OpfsBackend } = await import("/js/lib/nbd-server.js");
  const { api } = await import("/js/api.js");
  const d = await window.reimuMedia.imagesDir();
  const fh = await d.getFileHandle("pipe.img", { create: true });
  const w = await fh.createWritable(); await w.truncate(64 << 20); await w.close();
  window.srv = new NbdServer(api.websocket("/vm/0/0"), await OpfsBackend.open("pipe.img"), { readOnly: true });
});
await sleep(8000);
const out = vm(`for i in 0 1 2 3 4 5 6 7; do dd if=/dev/nbd0 of=/dev/null bs=1M count=8 skip=$((i*8)) iflag=direct 2>&1 | tail -1 & done; wait`);
await sleep(3000);
const pid1 = vm("systemctl show -p MainPID --value bmcweb");
const log = vm("journalctl -u bmcweb --no-pager --since '-2min' | grep -iE 'terminate|length_error|exception|core|Buffer overrun|signal' | head -5");
console.log("dd:", out.split("\n").slice(0, 8).join(" | "));
console.log("bmcweb pid before/after:", pid0, pid1);
console.log("journal:", log || "(nothing)");
console.log(pid0 === pid1 && !/terminate|length_error/.test(log) && !/error/i.test(out) ? "RESULT: survived" : "RESULT: FAILED (crash or I/O errors)");
await browser.close();
