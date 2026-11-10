// E2E without a BMC: the card «USB-сеть с хостом» on the page Хост
// (js/pages/host.js, the real file), reimu-hostlink's REST answers stubbed:
// UsbNet (reimu-usbnet's status, the BMC's last one: {"pending":true} before
// it has one) and UsbNetSet (the USB network, the host's internet through the
// BMC's port; it answers when the change is done, seconds later) and how the
// agent is connected.
//
//     node host-usbnet.stub.mjs       # WWW: the web tree, CHROME: chromium
import { chromium } from "playwright";
import fs from "node:fs";
import path from "node:path";

const HERE = path.dirname(new URL(import.meta.url).pathname);
const WWW = process.env.WWW || path.join(HERE, "../../meta-common/recipes-phosphor/reimu-webui/files/www");
const ORIGIN = "http://reimu.test";
const HL = "/xyz/openbmc_project/reimu/hostlink";

let failed = 0;
const check = (c, what) => { console.log(`${c ? "PASS" : "FAIL"} ${what}`); if (!c) failed++; };

const HARNESS = `<!doctype html><html><head><meta charset="utf-8"><link rel="stylesheet" href="/css/app.css"></head><body><div id="toasts"></div><main id="page"></main>
<script type="module">import { mount } from "./js/pages/host.js";
mount(document.getElementById("page"));</script></body></html>`;

const USB = { enabled: true, internet: false, ifname: "usb0", port: "1e6a0000.usb-vhub:p5", state: "configured", address: "172.31.83.1/29",
  mac: "98:a7:b0:12:34:7e", host_mac: "9a:a7:b0:12:34:7e", host_ipv4: "172.31.83.2", error: "" };
const state = (via) => ({
  version: "1.0", port: 5564, iface: "eth0 usb0", fingerprint: "ab".repeat(32), mac: "98:a7:b0:12:34:7e", ipv4: "192.168.10.79", paired: true,
  pairing: { active: false, left: 0 }, disk: { on: false, left: 0 },
  agent: { connected: !!via, id: "x", host: "elbrus", version: "1.0", transport: "ipv6-ll", peer: via === "usb" ? "fe80::9aa7:b0ff:fe12:347e%usb0" : "fe80::9aa7:b0ff:fe12:347e%eth0",
    iface: via === "usb" ? "usb0" : "eth0", via: via || "", since: 1793880000, last_seen: 1793880100 },
  heartbeat: { state: via ? "ok" : "never", age: 3 }, policy: { gpu_fan: { mode: "auto" } }, fand_turbo: false, sensors: {}, thresholds: {},
  edac: { ce: 0, ue: 0 }, inventory: null, inventory_age: -1, metrics: null, metrics_age: -1, events: [], results: [],
});

const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });

async function open({ via = "usb", usb = USB, failSet = null, answer = true, pending = 0, setDelay = 0 } = {}) {
  const ctx = await browser.newContext({ locale: "ru-RU", viewport: { width: 1300, height: 1000 } });
  const page = await ctx.newPage();
  const s = { page, ctx, usb: { ...usb }, calls: [], sets: [], dialogs: [], errors: [], failSet };
  page.on("pageerror", (e) => s.errors.push(e.message));
  page.on("dialog", async (d) => { s.dialogs.push(d.message()); if (answer) await d.accept(); else await d.dismiss(); });
  await page.route(`${ORIGIN}/**`, async (route) => {
    const req = route.request();
    const p = new URL(req.url()).pathname;
    const json = (o, status = 200) => route.fulfill({ status, contentType: "application/json", body: JSON.stringify(o) });
    if (p === "/harness.html") return route.fulfill({ contentType: "text/html", body: HARNESS });
    if (p === HL) return json({ data: { Json: JSON.stringify(state(via)), Connected: !!via, Paired: true, Heartbeat: "ok" } });
    if (p === `${HL}/action/UsbNet`) {
      s.calls.push("UsbNet");
      return json({ data: JSON.stringify(s.calls.length <= pending ? { pending: true } : s.usb), message: "200 OK", status: "ok" });
    }
    if (p === `${HL}/action/UsbNetSet`) {
      const [what, on] = req.postDataJSON().data;
      s.sets.push([what, on]);
      if (setDelay) await new Promise((r) => setTimeout(r, setDelay));
      if (s.failSet) return json({ data: { description: "xyz.openbmc_project.Common.Error" }, message: s.failSet, status: "error" }, 400);
      s.usb[what] = on;
      if (what === "enabled" && !on) Object.assign(s.usb, { port: "", state: "", host_ipv4: "" });
      return json({ data: JSON.stringify(s.usb), message: "200 OK", status: "ok" });
    }
    if (/^\/(redfish|xyz)/.test(p)) return json({ message: "404 Not Found", status: "error" }, 404);
    const f = path.join(WWW, p);
    if (f.startsWith(WWW) && fs.existsSync(f) && fs.statSync(f).isFile())
      return route.fulfill({ contentType: p.endsWith(".js") ? "text/javascript" : p.endsWith(".css") ? "text/css" : "text/plain", body: fs.readFileSync(f) });
    return route.fulfill({ status: 404, body: "" });
  });
  await page.goto(`${ORIGIN}/harness.html`);
  s.card = page.locator(".card", { has: page.locator("h3", { hasText: "USB-сеть с хостом" }) });
  await s.card.waitFor({ timeout: 15000 });
  return s;
}
const text = async (l) => (await l.innerText()).replace(/\s+/g, " ");

{
  const s = await open();
  const t = await text(s.card);
  check(/хост подключён/.test(t), `the state: the host has it (${t.slice(0, 120)})`);
  check(t.includes("p5") && t.includes("172.31.83.2") && t.includes("172.31.83.1"), "the hub's port, the host's and the BMC's addresses");
  check(/Агент[^]*по USB-сети/.test(t), "the agent connected over the USB network");
  const agent = await text(s.page.locator(".card", { has: s.page.locator("h3", { hasText: "Агент хоста" }) }));
  check(/USB-сеть/.test(agent) && !/внутренняя сеть платы/.test(agent), `the agent card's Связь: over the USB network, no "board's internal network" (${agent.match(/Связь[^\n]{0,80}/)})`);
  const inet = s.card.getByLabel(/Интернет для хоста через порт BMC/);
  check(!(await inet.isChecked()) && (await inet.isEnabled()), "internet: off, can be turned on");
  check(/100 Мбит\/с/.test(t) && /процессор BMC/.test(t), "the note: 100 Mbit/s, the BMC's CPU");
  await inet.check();
  await s.page.waitForTimeout(800);
  check(JSON.stringify(s.sets) === JSON.stringify([["internet", true]]), `UsbNetSet internet true (${JSON.stringify(s.sets)})`);
  check(await s.card.getByLabel(/Интернет для хоста через порт BMC/).isChecked(), "on after the answer");
  check(/Интернет для хоста включён/.test(await s.page.locator("#toasts").innerText()), "a toast says so");
  // UsbNet: at mount and after a set, not with every 5 s refresh of the page
  const n0 = s.calls.length;
  await s.page.waitForTimeout(6000);
  check(s.calls.length === n0, `no UsbNet with the page's refresh (${n0} -> ${s.calls.length})`);
  check(await s.card.getByLabel(/Интернет для хоста через порт BMC/).isChecked(), "still on after a refresh");
  // the USB network off, with the agent on it: asked first
  await s.card.getByLabel(/^USB-сеть/).uncheck();
  await s.page.waitForTimeout(800);
  check(s.dialogs.length === 1 && /USB/.test(s.dialogs[0]) && /агент/i.test(s.dialogs[0]), `asked first: the agent is on it (${s.dialogs[0]})`);
  check(JSON.stringify(s.sets[1]) === JSON.stringify(["enabled", false]), `UsbNetSet enabled false (${JSON.stringify(s.sets)})`);
  check(/выключена/.test(await text(s.card)), "the card: off");
  check(await s.card.getByLabel(/Интернет для хоста через порт BMC/).isDisabled(), "internet can't be set with the USB network off");
  check(!s.errors.length, `no page errors ${s.errors}`);
  await s.ctx.close();
}
{
  // the BMC has no status yet (hostlink just started): loading, asked again
  const s = await open({ pending: 2 });
  await s.card.getByText("хост подключён").waitFor({ timeout: 10000 });
  check(s.calls.length === 3, `pending twice, then the status (${s.calls.length} UsbNet calls)`);
  // a change takes seconds (the USB card plugged in again): said meanwhile, no second one
  const s2 = await open({ setDelay: 2500 });
  await s2.card.getByLabel(/Интернет для хоста через порт BMC/).check();
  await s2.page.waitForTimeout(500);
  check(/Применяется/.test(await text(s2.card)), "applying: said");
  check(await s2.card.getByLabel(/^USB-сеть/).isDisabled() && await s2.card.getByLabel(/Интернет для хоста/).isDisabled(), "both boxes held meanwhile");
  check(await s2.card.getByLabel(/Интернет для хоста/).isChecked(), "the box shows what is being set");
  await s2.page.waitForTimeout(3000);
  check(!/Применяется/.test(await text(s2.card)) && await s2.card.getByLabel(/Интернет для хоста/).isChecked() && await s2.card.getByLabel(/^USB-сеть/).isEnabled(),
    "done: on, the boxes free");
  check(s2.sets.length === 1, "one change sent");
  await s.ctx.close();
  await s2.ctx.close();
}
{
  // refused at the question: nothing asked, the box back on
  const s = await open({ answer: false });
  await s.card.getByLabel(/^USB-сеть/).click();
  await s.page.waitForTimeout(600);
  check(!s.sets.length && (await s.card.getByLabel(/^USB-сеть/).isChecked()), "refused: nothing set, still on");
  await s.ctx.close();
}
{
  // the agent on the LAN: no question to turn the USB network off
  const s = await open({ via: "lan" });
  check(/по локальной сети/.test(await text(s.card)), "the agent over the LAN");
  await s.card.getByLabel(/^USB-сеть/).uncheck();
  await s.page.waitForTimeout(600);
  check(!s.dialogs.length && s.sets.length === 1, "no question with the agent on the LAN");
  await s.ctx.close();
}
{
  // the BMC failed to set it: says why, the box as it was
  const s = await open({ failSet: "iptables: MASQUERADE not loaded" });
  await s.card.getByLabel(/Интернет для хоста через порт BMC/).click();
  await s.page.waitForTimeout(800);
  check(/MASQUERADE not loaded/.test(await s.page.locator("#toasts").innerText()), "the error in a toast");
  check(!(await s.card.getByLabel(/Интернет для хоста через порт BMC/).isChecked()), "the box back off");
  await s.ctx.close();
}
{
  // no helper / bound nowhere: what is wrong, no toggles to break it more
  const s = await open({ usb: { error: "/usr/libexec/reimu-usbnet: No such file or directory" } });
  const t = await text(s.card);
  check(/недоступна/.test(t) && t.includes("No such file"), `the error shown (${t.slice(0, 160)})`);
  check(!(await s.card.locator("input[type=checkbox]").count()), "no toggles");
  await s.ctx.close();
}
{
  // on, but the host hasn't taken it (not attached: the host off, or no driver)
  const s = await open({ via: "lan", usb: { ...USB, state: "not attached", host_ipv4: "" } });
  const t = await text(s.card);
  check(/хост не подключил/.test(t), `not attached: said (${t.slice(0, 120)})`);
  await s.ctx.close();
}
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 10000))]);
console.log(failed ? `${failed} FAILED` : "all passed");
process.exit(failed ? 1 : 0);
