// E2E: installing the host agent from the «Хост» page (tests/ab64/test_agentux_qemu.py
// runs it, AUX_E2E=DIR): the pairing box (commands that pin bmcweb's key,
// the countdown, «Новый ключ», copy), «Скачать установщик», «USB-диск с
// установщиком». It writes what the page gave to OUT/agent-commands.json and
// OUT/reimu-agent-install.sh for the Python side to run against the BMC.
//   BASE=https://127.0.0.1:3443 OUT=dir node agent-install.qemu.mjs
// AGENT_DISK=on (a virtual hub: QEMU's ast2500 has one, the board) | error (no USB device controller)
import { chromium } from "playwright";
import { BASE, OUT } from "./common.mjs";
import crypto from "node:crypto";
import fs from "node:fs";
import tls from "node:tls";

fs.mkdirSync(OUT, { recursive: true });
let failed = 0;
const check = (c, w) => {
  console.log(`${c ? "PASS" : "FAIL"} ${w}`);
  if (!c) failed++;
};
// bmcweb's key as curl's --pinnedpubkey sees it
const pin = await new Promise((res, rej) => {
  const u = new URL(BASE);
  const s = tls.connect({ host: u.hostname, port: Number(u.port || 443), rejectUnauthorized: false }, () => {
    const c = s.getPeerCertificate();
    s.end();
    const spki = new crypto.X509Certificate(c.raw).publicKey.export({ type: "spki", format: "der" });
    res("sha256//" + crypto.createHash("sha256").update(spki).digest("base64"));
  });
  s.on("error", rej);
});
const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined, args: ["--ignore-certificate-errors"] });
const ctx = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 1400, height: 1000 }, locale: "ru-RU", acceptDownloads: true });
await ctx.grantPermissions(["clipboard-read", "clipboard-write"], { origin: BASE }).catch(() => {});
const page = await ctx.newPage();
const errors = [];
page.on("pageerror", (e) => errors.push(e.message));
const card = (title) => page.locator(".card", { has: page.locator("h3", { hasText: title }) });
const box = () => page.locator(".note.pairbox");
const pairOf = (cmd) => (cmd.match(/--pair '(reimu1:[A-Za-z0-9_-]+)'/) || [])[1];
const out = {};
try {
  await page.goto(BASE + "/");
  await page.fill("#login-pass", "0penBmc");
  await page.click("#login-form button[type=submit]");
  await page.waitForSelector("#app:not(.hidden)");
  await page.click('#nav a[data-page="host"]');
  const agent = card("Агент хоста");
  await agent.waitFor({ timeout: 30000 });
  page.on("dialog", (d) => d.accept());
  await agent.getByRole("button", { name: /Подключить/ }).click();
  await box().waitFor({ timeout: 15000 });
  const text = await box().innerText();
  check(/ключ действует ещё\s*1[45]:\d\d/.test(text), "the countdown: about 15 min");
  const cmds = await box().locator("input.mono").evaluateAll((l) => l.map((x) => x.value));
  const ll = cmds.find((c) => c.includes("/sys/class/net"));
  const v4 = cmds.find((c) => !c.includes("/sys/class/net") && c.includes("/agent/install.sh"));
  check(ll && /^s=; for i in \$\(ls \/sys\/class\/net\); do s=\$\(curl -fsSg --connect-timeout 3 --pinnedpubkey 'sha256\/\/[A-Za-z0-9+\/=]{44}' -k "https:\/\/\[fe80::[0-9a-f:]+%25\$i\]\/agent\/install\.sh" 2>\/dev\/null\) && break; done; /.test(ll),
    `the link-local command: ${ll}`);
  check(v4 && /^s=\$\(curl -fsS --pinnedpubkey 'sha256\/\/[A-Za-z0-9+\/=]{44}' -k https:\/\/[0-9.]+\/agent\/install\.sh\) && printf '%s\\n' "\$s" \| sudo sh -s -- --pair 'reimu1:[A-Za-z0-9_-]+'$/.test(v4),
    `the IPv4 command: ${v4}`);
  check(ll && v4 && ll.includes(`--pinnedpubkey '${pin}'`) && v4.includes(`--pinnedpubkey '${pin}'`), `both pin bmcweb's key ${pin}`);
  check(ll && pairOf(ll) && pairOf(ll) === pairOf(v4), "the same pairing string in both");
  const left1 = await box().locator(".pair-left").innerText();
  await page.waitForTimeout(2200);
  const left2 = await box().locator(".pair-left").innerText();
  check(left1 !== left2, `the countdown runs (${left1} -> ${left2})`);
  // copy
  await box().getByRole("button", { name: "Копировать" }).first().click();
  const clip = await page.evaluate(() => navigator.clipboard.readText()).catch(() => "");
  check(clip === ll, "«Копировать»: the command in the clipboard");
  await page.screenshot({ path: `${OUT}/agent-pair.png` });
  // a new key
  const p1 = pairOf(v4);
  await box().getByRole("button", { name: "Новый ключ" }).click();
  await page.waitForFunction((old) => [...document.querySelectorAll(".pairbox input.mono")].some((i) => i.value.includes("reimu1:") && !i.value.includes(old)), p1, { timeout: 15000 });
  const cmds2 = await box().locator("input.mono").evaluateAll((l) => l.map((x) => x.value));
  const v4b = cmds2.find((c) => !c.includes("/sys/class/net") && c.includes("/agent/install.sh"));
  check(pairOf(v4b) && pairOf(v4b) !== p1, "«Новый ключ»: another pairing string");
  out.ll = cmds2.find((c) => c.includes("/sys/class/net"));
  out.v4 = v4b;
  out.pair = pairOf(v4b);
  out.pin = pin;
  // the self-contained installer
  const [dl] = await Promise.all([page.waitForEvent("download", { timeout: 20000 }), box().getByRole("button", { name: "Скачать установщик" }).click()]);
  check(dl.suggestedFilename() === "reimu-agent-install.sh", `download: ${dl.suggestedFilename()}`);
  const f = `${OUT}/reimu-agent-install.sh`;
  await dl.saveAs(f);
  const sh = fs.readFileSync(f, "utf8");
  check(sh.startsWith("#!/bin/sh") && sh.includes(`\nREIMU_PAIR_DEFAULT='${out.pair}'\n`), "the installer has the current pairing string");
  const payload = sh.split("\n#__REIMU_PAYLOAD__\n")[1] || "";
  check(payload.split("\n").filter(Boolean).map((l) => l.split(" ")[0]).join(" ") === "reimu-hostd.py reimu-hostd.service reimu-hostd.conf", "and the agent's three files");
  // the USB disk
  await agent.getByRole("button", { name: /USB-диск/ }).click();
  if ((process.env.AGENT_DISK || "error") === "error") {
    await page.locator(".toast.err", { hasText: /USB/ }).waitFor({ timeout: 20000 });
    const t = await page.locator(".toast.err").last().innerText();
    check(/USB/.test(t), `no USB controller (QEMU): the error shown: ${t}`);
  } else {
    await page.locator(".note.pairbox", { hasText: "USB-диск с установщиком:" }).waitFor({ timeout: 20000 });
    const t = await box().innerText();
    const d = await box().locator("input.mono").evaluateAll((l) => l.map((x) => x.value));
    check(d.includes("sudo sh /run/media/$USER/REIMU-AGENT/install.sh"), "the disk: the desktop's mount path");
    check(d.some((c) => c.includes("/dev/disk/by-label/REIMU-AGENT")), "and the generic one");
    check(/USB-диск/.test(t), "the disk box");
    await page.screenshot({ path: `${OUT}/agent-disk.png` });
    out.disk = true;
  }
  fs.writeFileSync(`${OUT}/agent-commands.json`, JSON.stringify(out, null, 1));
} catch (e) {
  check(false, `${e.message}`.split("\n")[0]);
  await page.screenshot({ path: `${OUT}/agent-fail.png` }).catch(() => {});
}
check(errors.length === 0, `no page errors ${JSON.stringify(errors)}`);
await browser.close();
process.exit(failed ? 1 : 0);
