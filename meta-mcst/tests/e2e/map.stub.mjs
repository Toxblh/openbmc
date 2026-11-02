// E2E without a BMC: the board map (js/pages/map.js and js/boards/*.js, the real
// files) in a bare harness, the REST answers stubbed with what a real board gave
// (map-fixture.json, serials and addresses removed).
//
//     node map.stub.mjs [SHOTS_DIR]     # WWW: the web tree, CHROME: chromium
import { chromium } from "playwright";
import fs from "node:fs";
import path from "node:path";

const HERE = path.dirname(new URL(import.meta.url).pathname);
const WWW = process.env.WWW || path.join(HERE, "../../meta-common/recipes-phosphor/reimu-webui/files/www");
const FIX = JSON.parse(fs.readFileSync(process.env.FIXTURE || path.join(HERE, "map-fixture.json"), "utf8"));
const SHOTS = process.argv[2];
const ORIGIN = "http://reimu.test";
const P = "/xyz/openbmc_project/reimu";

let failed = 0;
const check = (c, what) => { console.log(`${c ? "PASS" : "FAIL"} ${what}`); if (!c) failed++; };

const HARNESS = `<!doctype html><html><head><meta charset="utf-8"><link rel="stylesheet" href="/css/reimu.css"></head><body style="padding:12px;overflow:auto;height:auto">
<div id="toasts"></div><main id="page"></main>
<script type="module">import { mount } from "./js/pages/map.js"; mount(document.getElementById("page"));</script></body></html>`;

const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });

async function open(fix, { width = 1300, height = 900, view } = {}) {
  const ctx = await browser.newContext({ locale: "ru-RU", viewport: { width, height } });
  const page = await ctx.newPage();
  const s = { page, errors: [] };
  page.on("pageerror", (e) => s.errors.push(e.message));
  page.on("console", (m) => m.type() === "error" && !/404/.test(m.text()) && s.errors.push(m.text()));
  const j = (v) => ({ contentType: "application/json", body: JSON.stringify({ data: { Json: JSON.stringify(v) } }) });
  await page.route(`${ORIGIN}/**`, async (route) => {
    const p = new URL(route.request().url()).pathname;
    if (p === "/harness.html") return route.fulfill({ contentType: "text/html", body: HARNESS });
    const dbus = { [`${P}/pns`]: fix.pns, [`${P}/sensors`]: fix.sensors, [`${P}/fans`]: fix.fans, [`${P}/hostlink`]: fix.host };
    if (p in dbus) return dbus[p] ? route.fulfill(j(dbus[p])) : route.fulfill({ status: 404, body: "" });
    if (p.endsWith("/EthernetInterfaces/eth0")) return fix.eth ? route.fulfill({ contentType: "application/json", body: JSON.stringify(fix.eth) }) : route.fulfill({ status: 404, body: "" });
    const f = path.join(WWW, p);
    if (!fs.existsSync(f) || fs.statSync(f).isDirectory()) return route.fulfill({ status: 404, body: "" });
    const types = { ".js": "text/javascript", ".css": "text/css" };
    return route.fulfill({ contentType: types[path.extname(f)] || "application/octet-stream", body: fs.readFileSync(f) });
  });
  if (view) await page.addInitScript((v) => localStorage.setItem("reimu-map-view", v), view);
  await page.goto(`${ORIGIN}/harness.html`);
  return s;
}
const ready = (s) => s.page.waitForSelector('.bm-box[data-id="X30"][data-state]');
const state = (s, id) => s.page.locator(`.bm-box[data-id="${id}"]`).getAttribute("data-state");
const tag = (s, id) => s.page.locator(`.bm-mk[data-id="${id}"] .bm-tag`).textContent();
async function shot(s, name) { if (SHOTS) { await s.page.waitForTimeout(1000); await s.page.screenshot({ path: path.join(SHOTS, name), fullPage: true }); } }

// a real board, the host on, an agent that says all
{
  const s = await open(FIX);
  await ready(s);
  check((await s.page.locator("h2").first().textContent()).includes("1Э8СВ"), "the board found by its device tree");
  check((await tag(s, "cpu")).includes("61") || (await tag(s, "cpu")).includes("60"), `CPU: the hottest of the die sensors (${await tag(s, "cpu")})`);
  check((await tag(s, "iohub")).includes("57"), `КПИ-2 ${await tag(s, "iohub")}`);
  check((await tag(s, "vrm-uncore")).includes("≈"), "a position not in the manual says so");
  check(await s.page.locator('.bm-mk[data-id="vrm-uncore"]').evaluate((e) => e.classList.contains("approx")), "and looks different");
  check(!(await s.page.locator('.bm-mk[data-id="cpu"]').evaluate((e) => e.classList.contains("approx"))), "the CPU itself is on the drawing");
  check(await s.page.locator(".bm-mk").count() === 8, "one marker per place: no sensors drawn on the die");
  check(await s.page.locator('.bm-box[data-id="X37"] .bm-spin.on').count() === 1, "X37 spins");
  check((await s.page.locator('.bm-box[data-id="X37"] .bm-val').textContent()).length > 2, "X37 with its RPM");
  check(await s.page.locator('.bm-box[data-id="X5"] .bm-spin.on').count() === 0 && await state(s, "X5") === "empty", "X5: no fan, no spin");
  check(await state(s, "X38") === "idle", "X38: a tach that reads 0");
  check((await s.page.locator('.bm-box[data-id="X39"] .bm-val').textContent()).includes("%"), "X39, no tach: the power, not RPM");
  check(await state(s, "X30") === "used" && await state(s, "X31") === "used", "two cards on the two x8 ports: both slots taken");
  check(await state(s, "X32") === "empty", "X32: nothing on its ports");
  check(await state(s, "X48") === "used", "M.2: the SATA disk on ata4");
  check(await state(s, "X26") === "empty", "X26: nothing on its port");
  check(await state(s, "X15") === "used" && await state(s, "X14") === "empty", "host LAN: the link on interface 1");
  check(await state(s, "X4") === "used", "BMC LAN up");
  check(await state(s, "X19") === "empty", "X19: no USB device");
  check(await state(s, "X16") === "unknown", "memory by slot: unknown, the host says only the total");
  await s.page.locator('.bm-box[data-id="X30"]').click();
  const d = await s.page.locator("#map-detail").textContent();
  check(d.includes("Какая в каком") && d.includes("Navi 10"), "a slot's card: the cards, and that which is where isn't known");
  await s.page.locator('.bm-box[data-id="X4"]').click();
  const u = await s.page.locator("#map-detail").textContent();
  check(u.includes("Ultra Fit") && u.includes("есть линк"), "X4: its USB device and the link");
  check(!u.includes("virtual_input"), "the BMC's own hub isn't on X4");
  const sum = await s.page.locator("#map-summary").textContent();
  check(sum.includes("X48") && sum.includes("970 PRO") && sum.includes("Ultra Fit") && !sum.includes("virtual_input"), "summary: disks with places, USB without the BMC's");
  await s.page.locator('.bm-box[data-id="X4"]').click();
  check(await s.page.locator(".bm-wrap").getAttribute("data-view") === "iso", "3D by default");
  await shot(s, "map-3d.png");
  await s.page.getByRole("button", { name: "Чертёж" }).click();
  check(await s.page.locator(".bm-wrap").getAttribute("data-view") === "2d", "the drawing");
  await shot(s, "map-2d.png");
  const box = await s.page.locator('.bm-box[data-id="X5"]').boundingBox(), wrap = await s.page.locator(".bm-wrap").boundingBox();
  check(box.x >= wrap.x && box.x + box.width <= wrap.x + wrap.width, "the drawing fits");
  await s.page.getByLabel("Тепло").uncheck();
  check(await s.page.locator(".bm-halo").first().isHidden(), "the heat layer off");
  check(!s.errors.length, `no page errors ${s.errors.join("; ")}`);
}
// a phone
{
  const s = await open(FIX, { width: 390, height: 800 });
  await ready(s);
  const over = await s.page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
  check(over <= 0, `phone: no sideways scroll (${over})`);
  await shot(s, "map-phone-3d.png");
  await s.page.getByRole("button", { name: "Чертёж" }).click();
  await shot(s, "map-phone-2d.png");
  check(!s.errors.length, `phone: no page errors ${s.errors.join("; ")}`);
}
// an agent from before these fields: nothing guessed
{
  const old = JSON.parse(JSON.stringify(FIX));
  const inv = old.host.inventory;
  inv.pci.forEach((d) => delete d.parent);
  inv.disks.forEach((d) => delete d.bus);
  inv.nics.forEach((d) => delete d.pci);
  delete inv.usb;
  const s = await open(old);
  await ready(s);
  check(await state(s, "X30") === "unknown" && await state(s, "X48") === "unknown" && await state(s, "X19") === "unknown", "an old agent: slots, SATA, USB unknown");
  check(await state(s, "X15") === "used", "an old agent: LAN by the interface name");
  check(!s.errors.length, `old agent: no page errors ${s.errors.join("; ")}`);
}
// no agent
{
  const none = { ...FIX, host: { agent: { connected: false }, heartbeat: { state: "none" }, paired: false, sensors: {} } };
  const s = await open(none);
  await ready(s);
  check(await state(s, "X30") === "unknown" && await state(s, "X14") === "unknown", "no agent: the host's side unknown");
  check(await state(s, "X4") === "used", "no agent: the BMC's port still known");
  check((await tag(s, "cpu")).includes("°"), "no agent: sensors still there");
  check((await s.page.locator("#map-state").textContent()).includes("Агент хоста не подключён"), "no agent: said so");
  await shot(s, "map-no-agent.png");
  check(!s.errors.length, `no agent: no page errors ${s.errors.join("; ")}`);
}
// the host off
{
  const off = { ...FIX, host: { agent: { connected: false }, heartbeat: { state: "none" }, sensors: {} },
    sensors: { version: 1, platform: "off", sensors: [] }, eth: { LinkStatus: "LinkDown" } };
  const s = await open(off);
  await ready(s);
  check((await tag(s, "cpu")) === "—", "host off: no reading, no number");
  check(await s.page.locator('.bm-halo').first().isHidden(), "host off: no heat");
  check((await s.page.locator("#map-state").textContent()).includes("Хост выключен"), "host off: said so");
  check(await state(s, "X4") === "empty", "BMC LAN down");
  check(!s.errors.length, `host off: no page errors ${s.errors.join("; ")}`);
}
// another board
{
  const s = await open({ ...FIX, pns: { fruXml: "<product_name>Другая плата</product_name>", devtree: "" } });
  await s.page.waitForSelector("#map-unknown");
  check((await s.page.locator("#map-unknown").textContent()).includes("Другая плата"), "a board with no map: says so");
  const n = await open({ ...FIX, pns: null });
  await n.page.waitForSelector("#map-unknown");
  check((await n.page.locator("#map-unknown").textContent()).includes("не определена"), "no board data: says so");
}
await browser.close();
console.log(`${failed} failed`);
process.exit(failed ? 1 : 0);
