// E2E without a BMC: the board map (js/pages/map.js, js/lib/boardscene.js,
// js/boards/*.js, the real files) in a bare harness, the REST answers stubbed with
// what a real board gave (map-fixture.json, serials and addresses replaced).
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
const LED = "/xyz/openbmc_project/led/groups/enclosure_identify";

let failed = 0;
const check = (c, what) => { console.log(`${c ? "PASS" : "FAIL"} ${what}`); if (!c) failed++; };
const clone = (o) => JSON.parse(JSON.stringify(o));

const hw = (b) => b.replace("./js/pages/map.js", "./js/pages/hardware.js");
const HARNESS = `<!doctype html><html><head><meta charset="utf-8"><link rel="stylesheet" href="/css/reimu.css"></head><body style="padding:12px;overflow:auto;height:auto">
<div id="toasts"></div><main id="page"></main>
<script type="module">import { mount } from "./js/pages/map.js"; mount(document.getElementById("page"));</script></body></html>`;

const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });

async function open(fix, { width = 1300, height = 900, view, led = { Asserted: false }, html = HARNESS, block, slow = 0, noStorage = false } = {}) {
  const ctx = await browser.newContext({ locale: "ru-RU", viewport: { width, height } });
  const page = await ctx.newPage();
  const s = { page, errors: [], fix };
  page.on("pageerror", (e) => s.errors.push(e.message));
  page.on("console", (m) => m.type() === "error" && !/404/.test(m.text()) && s.errors.push(m.text()));
  const j = (v) => ({ contentType: "application/json", body: JSON.stringify({ data: { Json: JSON.stringify(v) } }) });
  await page.route(`${ORIGIN}/**`, async (route) => {
    const p = new URL(route.request().url()).pathname;
    s.requests = (s.requests || 0) + (p === `${P}/sensors` || p === `${P}/view/live` ? 1 : 0);
    if (p === `${P}/view/live`) (s.cycles = s.cycles || []).push(Date.now());
    if (block && block.test(p)) return route.fulfill({ status: 404, body: "" });
    // the live answers of a slow BMC, how many at a time, how many cycles
    if (slow && (p === LED || p.startsWith(`${P}/`) || p.endsWith("/EthernetInterfaces/eth0"))) {
      s.inflight = (s.inflight || 0) + 1;
      s.peak = Math.max(s.peak || 0, s.inflight);
      await new Promise((r) => setTimeout(r, slow));
      s.inflight--;
    }
    if (p === "/harness.html") return route.fulfill({ contentType: "text/html", body: html });
    if (p === "/js/app.js") return route.fulfill({ contentType: "text/javascript", body: 'export const appState = { power: "On", user: "root" };' });
    if (p === "/redfish/v1/Systems/system") return route.fulfill({ contentType: "application/json", body: JSON.stringify({ Model: "M", MemorySummary: { TotalSystemMemoryGiB: 125 } }) });
    if (p === LED) return led ? route.fulfill({ contentType: "application/json", body: JSON.stringify({ data: led }) }) : route.fulfill({ status: 404, body: "" });
    const f2 = s.fix;
    const dbus = { [`${P}/pns`]: f2.pns, [`${P}/sensors`]: f2.sensors, [`${P}/fans`]: f2.fans, [`${P}/hostlink`]: f2.host };
    // the view: what the BMC would answer, from the same fixture (a part
    // missing or blocked is not in it)
    const there = (q) => !(block && block.test(q));
    const obj = (q) => (dbus[q] && there(q) ? { [q]: { Json: JSON.stringify(dbus[q]) } } : {});
    if (p === `${P}/view/live`)
      return route.fulfill(j({ version: 1, now: 0, power: "On", static: "h1", sensors: {},
        objects: { ...obj(`${P}/sensors`), ...obj(`${P}/fans`), ...obj(`${P}/hostlink`), ...(led && there(LED) ? { [LED]: led } : {}) },
        ...(f2.eth && there("/redfish/v1/Managers/bmc/EthernetInterfaces/eth0") ? { eth0: f2.eth } : {}) }));
    if (p === `${P}/view/static`) return route.fulfill(j({ version: 1, hash: "h1", objects: obj(`${P}/pns`), bmc: null,
      // (the memory of the Systems/system above: 125 GiB)
      inventory: { "/xyz/openbmc_project/inventory/system/chassis/motherboard/dimm0": { "xyz.openbmc_project.Inventory.Item.Dimm": { MemorySizeInKB: 125 * 1024 * 1024 } } } }));
    if (p in dbus) return dbus[p] ? route.fulfill(j(dbus[p])) : route.fulfill({ status: 404, body: "" });
    if (p.endsWith("/EthernetInterfaces/eth0")) return f2.eth ? route.fulfill({ contentType: "application/json", body: JSON.stringify(f2.eth) }) : route.fulfill({ status: 404, body: "" });
    const f = path.join(WWW, p);
    if (!fs.existsSync(f) || fs.statSync(f).isDirectory()) return route.fulfill({ status: 404, body: "" });
    const types = { ".js": "text/javascript", ".css": "text/css" };
    return route.fulfill({ contentType: types[path.extname(f)] || "application/octet-stream", body: fs.readFileSync(f) });
  });
  if (noStorage) await page.addInitScript(() => Object.defineProperty(window, "localStorage", { get() { throw new Error("denied"); } }));
  if (view) await page.addInitScript((v) => localStorage.setItem("reimu-map-view", v), view);
  await page.goto(`${ORIGIN}/harness.html`);
  return s;
}
const ready = (s) => s.page.waitForSelector(".bs-svg .bs-part[data-id='X30']");
// the text of everything drawn, the label of a part
const svgText = (s) => s.page.evaluate(() => [...document.querySelectorAll(".bs-svg text")].map((t) => t.textContent).join("\n"));
const label = (s, id) => s.page.evaluate((i) => [...document.querySelectorAll(`.bs-svg .bs-part[data-id='${i}'] text.bs-lbl`)].map((t) => t.textContent).join("|"), id);
// how much of a slot is lit: the width of its clip, null: none lit
const lit = (s, id) => s.page.evaluate((i) => {
  const g = document.querySelector(`.bs-svg .bs-part[data-id='${i}'] [data-lit]`);
  return g ? Number(g.dataset.lit) : null;
}, id);
// the popover of a part: as a click on it pins it
async function pop(s, id) {
  await s.page.evaluate((i) => {
    const el = [...document.querySelectorAll(".bs-svg .bs-part")].find((e) => e.dataset.id === i);
    document.querySelector(".bs-svg").dispatchEvent(new MouseEvent("click", { bubbles: true }));
    el.dispatchEvent(new MouseEvent("click", { bubbles: true, clientX: 300, clientY: 300 }));
  }, id);
  return s.page.locator(".bs-pop").innerText();
}
const partCount = (s, id, sel) => s.page.evaluate(([i, q]) => document.querySelector(`.bs-svg .bs-part[data-id='${i}']`).querySelectorAll(q).length, [id, sel]);
const clear = (s, what) => check(!s.errors.length, `${what}: no page errors ${s.errors.join("; ").slice(0, 200)}`);
// the corners of the plate as drawn: x,y x+w,y x+w,y+h x,y+h in the units of the view
const cornersOf = (s) => s.page.evaluate(() => {
  const e = [...document.querySelectorAll(".bs-svg .bs-part[data-id='BOARD'] polygon")].filter((p) => !p.getAttribute("filter"))[2];
  return [...e.points].map((p) => [p.x, p.y]);
});
const dist = (a, b) => Math.hypot(b[0] - a[0], b[1] - a[1]);
const between = (a, b, c, d) => {
  const v = Math.abs(Math.atan2(b[1] - a[1], b[0] - a[0]) - Math.atan2(d[1] - c[1], d[0] - c[0])) * 180 / Math.PI;
  return Math.min(v, Math.abs(180 - v));
};
const viewOn = (s) => s.page.evaluate(() => [...document.querySelectorAll("[data-view-btn].primary")].map((b) => b.dataset.viewBtn).join());
const stored = (s) => s.page.evaluate(() => localStorage.getItem("reimu-map-view"));
// a button of a view; one that is not there is a failed check, the rest of the run still says its numbers
async function pick(s, v) {
  const b = s.page.locator(`[data-view-btn='${v}']`);
  if (!(await b.count())) { check(false, `there is a button of the view ${v}`); return; }
  await b.click();
}
async function shot(s, name) { if (SHOTS) { await s.page.waitForTimeout(1000); await s.page.screenshot({ path: path.join(SHOTS, name), fullPage: true }); } }

// a real board, the host on, an agent that says all
{
  const s = await open(clone(FIX));
  await ready(s);
  check((await s.page.locator(".bs-title").textContent()).includes("1Э8СВ"), "the board found by its device tree");
  const text = await svgText(s);
  check(/^6[01]°$/m.test(text), "CPU: the hottest of the die sensors");
  check(/^57°$/m.test(text), "КПИ-2 57°");
  check(await s.page.evaluate(() => [...document.querySelectorAll(".bs-part[data-id='S:UNCORE'] circle")].some((c) => c.getAttribute("stroke-dasharray"))), "a place not in the manual has a dashed marker");
  check(await s.page.evaluate(() => !document.querySelector(".bs-part[data-id='S:CPU'] circle")), "the CPU's cloud sits on the lid, no marker");
  check(text.includes("TEST-07.22 PAN"), "the sticker: the FRU's serial word for word");
  check(text.includes("ТВГИ.469555.445") && !text.includes("Вер."), "the plate: from the part number, no version the FRU doesn't have");
  check(/эльбрус|8СВ/.test(text) || (await s.page.locator(".bs-svg [id^='lid-']").count()) > 0, "the CPU's lid is the vector");

  // PCIe: the mode from the root port, not the card's inside
  const x30 = await label(s, "X30");
  check(/x8/.test(x30) && !/x16/.test(x30) && /5600|5700/.test(x30), `the GPU's slot: x8 (${x30})`);
  const px30 = await pop(s, "X30");
  check(/x8 при 5 ГТ\/с/.test(px30) && !/16/.test(px30.replace("1493", "").replace(/1\d{3} об/, "")), `its popover: x8 at 5 GT/s (${px30.replace(/\n/g, " ").slice(0, 160)})`);
  check(/Navi 10/.test(px30) && /29 °C/.test(px30), "the chip and the temperature in it");
  const x31 = await label(s, "X31");
  check(/x4 из x8/.test(x31) && /Samsung SSD 970 PRO/.test(x31), `the disk's slot: x4 of x8, the model of the disk (${x31})`);
  const lX30 = await lit(s, "X30");
  const lX31 = await lit(s, "X31");
  check(lX30 > 80 && Math.abs(lX31 - (2 + 44)) < 1, `lit from the left: x8 whole (${lX30}), x4 half (${lX31})`);
  check((await lit(s, "X32")) === null && /пусто/.test(await label(s, "X32")), "the empty x2 slot: nothing lit");
  check((await pop(s, "X31")).includes("x4 при 5 ГТ/с") && !/переходник/i.test(await s.page.locator(".bs-pop").innerText()), "no word about a riser nobody can see");

  // SATA, M.2
  check(/NT-512/.test(await label(s, "X48")), "M.2: the disk on SATA3 in X48");
  check((await s.page.locator(".bs-part[data-id='X48'] text").allTextContents()).some((t) => t === "NT-512"), "the module is drawn with its name");
  const emptySata = await partCount(s, "X20", "polygon");
  check(emptySata === 3, `an empty SATA port: its body only, no cable (${emptySata})`);
  check(/свободно/.test(await pop(s, "X20")), "the empty port says so");

  // memory: an agent with no modules: not known by slot
  check(/неизвестно/.test(await label(s, "X46")), "memory slots unknown, not busy");
  check((await pop(s, "X46")).includes("только общий объём"), "and why");
  check((await partCount(s, "X46", "text")) === 0, "no module drawn");
  const sum = await s.page.locator("#map-summary").innerText();
  check(/126 ГБ/.test(sum), "the total from the host");

  // the network
  check(/LAN BMC/.test(await label(s, "X4")) && /линк есть/.test(await label(s, "X4")), "X4 with a link, Redfish gives no speed");
  const plugX15 = await partCount(s, "X15", "polygon");
  const plugX14 = await partCount(s, "X14", "polygon");
  check(plugX15 > plugX14, `a plug in the LAN with a link, none in the other (${plugX15}, ${plugX14})`);
  check(/192\.0\.2\.50/.test(await pop(s, "X15")) && /02:00:00:00:00:61/i.test(await s.page.locator(".bs-pop").innerText()), "X15's IP and MAC");
  check(/192\.0\.2\.79/.test(sum) && /02:00:00:00:00:7E/.test(sum), "X4's IP and MAC in the summary");
  check(/нет адреса/.test(sum), "no address on the port without a link");
  // USB
  const usbp = await pop(s, "X4");
  check(!(await s.page.locator("#sv, svg").first().innerHTML()).includes("…"), "no name cut with an ellipsis");
  check(/USB нижний[\s\S]*Ultra Fit/.test(usbp), `USB by port number (${usbp.replace(/\n/g, " ").slice(0, 140)})`);
  // the rows as the ports stand: the upper one first
  check(usbp.indexOf("USB верхний") >= 0 && usbp.indexOf("USB верхний") < usbp.indexOf("USB нижний"), "X4: the upper USB row above the lower one");
  // the BMC's load
  check(/LA 0\.42/.test(await s.page.locator(".bs-part[data-id=BMC]").last().textContent()), "the BMC's load average in a cloud at its chip");
  check(/0\.42 \/ 0\.30 \/ 0\.25/.test(await pop(s, "BMC")), "all three in its popover");
  // X14, X15 checked with a cable on the board: no word of a guess
  check(!/кабелем не проверено/.test(await pop(s, "X15")), "X15: no 'not checked with a cable' note");
  // fans, UID
  check(/1\d{3} об\/мин|\d+ об\/мин/.test(text) && /\d+ %/.test(text), "fans: speed and percent in clouds");
  check((await s.page.locator(".bs-part[data-id='X5']").getAttribute("opacity")) === "0.35", "a fan not connected is dim");
  check(/не горит/.test(await pop(s, "UID")), "UID off");
  check(/Память/i.test(sum) && /PCI Express/i.test(sum) && /Диски/i.test(sum) && /Сеть/i.test(sum), "the summary's four cards");
  check(!/5 накопител/.test(sum) && /3 ?накопител/.test(sum), "the disks there are, no demo");

  // the view: remembered, a camera move
  await s.page.getByRole("button", { name: "Сверху" }).click();
  await s.page.waitForTimeout(1000);
  const vb = await s.page.locator(".bs-svg").getAttribute("viewBox");
  check(/-172\.5 630 345/.test(vb), `from above (${vb})`);
  check((await s.page.evaluate(() => localStorage.getItem("reimu-map-view"))) === "2d", "the view remembered");
  await shot(s, "map-top.png");
  clear(s, "a full board");
  await s.page.context().close();
}

// values change in place, what stands changes the drawing
{
  const fix = clone(FIX);
  const s = await open(fix);
  await ready(s);
  const mark = await s.page.evaluate(() => { window.__sv = document.querySelector(".bs-svg"); return true; });
  const t = fix.sensors.sensors.find((x) => x.src === "6-0044" && x.name === "температура");
  t.value = 88;
  await s.page.waitForFunction(() => [...document.querySelectorAll(".bs-part[data-id='S:UNCORE'] text")].some((e) => e.textContent === "88°"), null, { timeout: 8000 });
  check(mark && await s.page.evaluate(() => window.__sv === document.querySelector(".bs-svg")), "a new temperature: the same drawing");
  fix.host.inventory.disks.push({ name: "sdb", model: "SATA Disk", size: 1000000000, transport: "ata/scsi", bus: { pci: "0000:01:03.0", ata: 2 } });
  await s.page.waitForFunction(() => document.querySelector(".bs-part[data-id='X20']").querySelectorAll("polygon").length > 3, null, { timeout: 8000 });
  check(await s.page.evaluate(() => window.__sv !== document.querySelector(".bs-svg")), "a new disk: drawn again, a cable in X20 (ata2 = SATA1)");
  clear(s, "changes");
  await s.page.context().close();
}

// memory: the modules from the firmware's table; UID on
const MOD = { size_gb: 32, type: "DDR4", registered: true, rated_mts: 3200, configured_mts: 2416, part: "M393A4K40DB3-CWE" };
{
  const fix = clone(FIX);
  fix.host.inventory.memory.modules = [MOD, MOD, MOD, MOD];
  const s = await open(fix, { led: { Asserted: true } });
  await ready(s);
  check(/X46 · 32 ГБ/.test(await label(s, "X46")) && (await partCount(s, "X46", "text")) === 1, "all slots full of one kind: modules drawn");
  const p = await pop(s, "X45");
  check(/32 ГБ DDR4 R-DIMM/.test(p) && /M393A4K40DB3-CWE/.test(p) && /3200 МТ\/с → 2416/.test(p), `the popover says what they are (${p.replace(/\n/g, " ").slice(0, 150)})`);
  const sum = await s.page.locator("#map-summary").innerText();
  check(/128 ГБ/.test(sum) && /DDR4 R-DIMM/.test(sum) && /M393A4K40DB3-CWE/.test(sum) && /2416/.test(sum), "the summary as well");
  check(/горит синим/.test(await pop(s, "UID")), "UID on");
  check(await s.page.evaluate(() => [...document.querySelectorAll(".bs-part[data-id='UID'] circle")].some((c) => c.getAttribute("fill") === "#0a84ff")), "and blue");
  await s.page.context().close();
}
for (const [what, mods, re] of [["fewer than slots", [MOD, MOD], /Установлено 2 из 4, какие разъёмы заняты/], ["different ones", [MOD, MOD, MOD, { ...MOD, size_gb: 16, part: "OTHER" }], /Установлено 4 из 4, модули разные/]]) {
  const fix = clone(FIX);
  fix.host.inventory.memory.modules = mods;
  const s = await open(fix);
  await ready(s);
  check(/неизвестно/.test(await label(s, "X46")) && (await partCount(s, "X46", "text")) === 0, `${what}: nothing assigned to a slot`);
  const sum = await s.page.locator("#map-summary").innerText();
  check(re.test(sum) && new RegExp(`${mods.reduce((a, m) => a + m.size_gb, 0)} ГБ`).test(sum), `${what}: the summary says so, with the total`);
  check(re.test(await pop(s, "X16")), `${what}: and the popover`);
  await s.page.context().close();
}
{
  // EDAC's dimms of a working agent are another thing: not modules
  const fix = clone(FIX);
  fix.host.inventory.memory.dimms = [{ mc: 0, dimm: 0, label: "DIMM0", size_mib: 16384 }];
  const s = await open(fix);
  await ready(s);
  check(/неизвестно/.test(await label(s, "X46")), "EDAC's list isn't taken for modules");
  await s.page.context().close();
}

// names from the host are text
{
  const fix = clone(FIX);
  const evil = '<img src=x onerror="window.pwned=1">';
  fix.host.inventory.pci.find((d) => d.slot === "0000:0a:00.0").name = `${evil} [${evil}]`;
  fix.host.inventory.disks.find((d) => d.name === "sda").model = evil;
  fix.host.inventory.usb[0].product = evil;
  fix.pns.fruXml = fix.pns.fruXml.replace("TEST-07.22 PAN", "&lt;b&gt;x");
  const s = await open(fix);
  await ready(s);
  const p = await pop(s, "X30");
  await pop(s, "X48");
  await pop(s, "X4");
  await s.page.waitForTimeout(200);
  check(p.includes("<img"), "the name shown as it is");
  check(!(await s.page.evaluate(() => window.pwned)) && (await s.page.locator("#page img").count()) === 0, "and it doesn't run");
  await s.page.context().close();
}

// an agent too old to say the topology
{
  const fix = clone(FIX);
  for (const d of fix.host.inventory.pci) { delete d.parent; delete d.link; }
  for (const d of fix.host.inventory.disks) delete d.bus;
  delete fix.host.inventory.usb;
  const s = await open(fix);
  await ready(s);
  check(/неизвестно/.test(await label(s, "X30")) && (await lit(s, "X30")) === null, "PCIe unknown, nothing lit");
  check((await pop(s, "X30")).includes("надо обновить"), "says the agent is old");
  check(/неизвестно/.test(await pop(s, "X20")) && (await partCount(s, "X20", "polygon")) === 3, "SATA unknown, looks empty");
  check((await partCount(s, "X15", "polygon")) > (await partCount(s, "X14", "polygon")), "the LANs still known");
  clear(s, "an old agent");
  await s.page.context().close();
}

// no agent
{
  const fix = clone(FIX);
  fix.host = { agent: { connected: false } };
  const s = await open(fix);
  await ready(s);
  check((await s.page.locator("#map-state").innerText()).includes("Агент хоста не подключён"), "says no agent");
  check(/неизвестно/.test(await label(s, "X15")) && (await partCount(s, "X15", "polygon")) === (await partCount(s, "X14", "polygon")), "the host's LAN unknown, no plug");
  check(/линк есть/.test(await label(s, "X4")), "the BMC's own port is known");
  check(/^6[01]°$/m.test(await svgText(s)), "the sensors are there");
  await shot(s, "map-no-agent.png");
  clear(s, "no agent");
  await s.page.context().close();
}

// the host off
{
  const fix = clone(FIX);
  fix.host = null;
  fix.sensors = { version: 1, platform: "off", sensors: [] };
  fix.fans = null;
  const s = await open(fix);
  await ready(s);
  check((await s.page.locator("#map-state").innerText()).includes("Хост выключен"), "says the host is off");
  check(!/°$/m.test(await svgText(s)), "no temperatures to invent");
  check((await s.page.locator(".bs-part[data-id='X37']").getAttribute("opacity")) === "0.35", "no fan to invent");
  clear(s, "the host off");
  await s.page.context().close();
}

// another board
{
  const fix = clone(FIX);
  fix.pns = { version: 1, fruXml: "<fruinfo><board_info_area><product_name>Другая плата</product_name></board_info_area></fruinfo>", devtree: "" };
  const s = await open(fix);
  await s.page.waitForSelector("#map-unknown");
  check((await s.page.locator("#map-unknown").innerText()).includes("Другая плата"), "another board: says so, no map");
  await s.page.context().close();
}
{
  const fix = clone(FIX);
  fix.pns = null;
  const s = await open(fix);
  await s.page.waitForSelector("#map-unknown");
  check((await s.page.locator("#map-unknown").innerText()).includes("не определена"), "no identification: says so");
  await s.page.context().close();
}

// in the hardware page: above its cards, the memory total said once
{
  const s = await open(clone(FIX), { html: hw(HARNESS) });
  await ready(s);
  await s.page.waitForSelector("#hw-inv .card h3");
  check(await s.page.evaluate(() => document.querySelector("#hw-map").compareDocumentPosition(document.querySelector("#hw-inv")) & 4), "the map above the hardware cards");
  await s.page.waitForFunction(() => document.querySelector(".hw-has-map"));
  check(!(await s.page.locator(".hw-mem-total").isVisible()), "the platform card leaves the memory total to the map");
  clear(s, "in the hardware page");
  await s.page.context().close();
}
// whatever happens to the map, the rest stands; the total back where it was
for (const [what, block, f] of [["the map's code missing", /boards\/1e8sv/, (x) => x], ["another board", null, (x) => { x.pns = { fruXml: "<a><product_name>Другая</product_name></a>", devtree: "" }; return x; }]]) {
  const s = await open(f(clone(FIX)), { html: hw(HARNESS), block });
  await s.page.waitForSelector("#hw-inv .card h3");
  await s.page.waitForFunction(() => document.querySelector("#hw-map").textContent.length > 0);
  check(await s.page.locator(".hw-mem-total").isVisible(), `${what}: the cards there, with the memory total`);
  await s.page.context().close();
}
// away from the page: nothing keeps running
{
  const s = await open(clone(FIX), { html: hw(HARNESS).replace("mount(document.getElementById(\"page\"));", "window.page = await import(\"./js/pages/hardware.js\"); await mount(document.getElementById(\"page\"));") });
  await ready(s);
  await s.page.waitForFunction(() => window.page);
  await s.page.evaluate(() => window.page.unmount());
  const n = s.requests;
  await s.page.waitForTimeout(6000);
  check(s.requests === n, `after unmount no more polling (${n} -> ${s.requests})`);
  await s.page.context().close();
}
{
  // unmounted while it was still loading
  const s = await open(clone(FIX), { html: HARNESS.replace("mount(document.getElementById(\"page\"));", "import(\"./js/pages/map.js\").then((m) => { m.unmount(); window.gone = 1; }); mount(document.getElementById(\"page\"));") });
  await s.page.waitForFunction(() => window.gone);
  await s.page.waitForTimeout(6500);
  check((s.requests || 0) <= 1, `unmounted while loading: no timer left (${s.requests})`);
  await s.page.context().close();
}
// the menu: no page of its own
{
  const index = fs.readFileSync(path.join(WWW, "index.html"), "utf8");
  const app = fs.readFileSync(path.join(WWW, "js/app.js"), "utf8");
  check(!/data-page="map"/.test(index) && !/pages\/map|"map"/.test(app), "no «Карта» in the menu, no route");
}

// the CPU: no substrate around the lid, its textolite is of the same size
{
  const s = await open(clone(FIX), { view: "top" });
  await ready(s);
  const r = await s.page.evaluate(() => {
    const g = document.querySelector(".bs-svg .bs-part[data-id='CPU']");
    const a = [...g.querySelectorAll("polygon, rect, path")].filter((e) => !e.closest(".bs-lid")).map((e) => e.getBoundingClientRect());
    const l = g.querySelector(".bs-lid").getBoundingClientRect();
    return { w: Math.max(...a.map((b) => b.width)) / l.width, h: Math.max(...a.map((b) => b.height)) / l.height };
  });
  check(r.w < 1.02 && r.h < 1.02, `nothing of the CPU wider than its lid (${r.w.toFixed(2)} x ${r.h.toFixed(2)})`);
  await s.page.context().close();
}

// the rear panel against the drawing of the manual (ТВГИ.469555.445РЭ, sizes in mm)
{
  const s = await open(clone(FIX), { view: "persp" });
  await ready(s);
  const r = await s.page.evaluate(() => {
    const g = document.querySelector(".bs-svg .bs-part[data-id='X28']");
    const face = [...g.querySelectorAll("g")].find((e) => e.querySelector("path"));
    const sh = face.querySelector("path"), b = sh.getBBox();
    const pt = (x, y) => { const q = sh.ownerSVGElement.createSVGPoint(); q.x = x; q.y = y; return sh.isPointInFill(q); };
    return { w: b.width, h: b.height, corner: pt(b.x + b.width - 0.5, b.y + b.height - 0.5), mid: pt(b.x + b.width / 2, b.y + b.height / 2),
      posts: [...g.querySelectorAll("circle")].map((c) => Number(c.getAttribute("r"))) };
  });
  check(Math.abs(r.w - 18.3) < 0.3 && Math.abs(r.h - 9.8) < 0.3, `VGA shell 18.3 x 9.8 (${r.w.toFixed(1)} x ${r.h.toFixed(1)})`);
  check(r.mid && !r.corner, "VGA shell with round corners");
  check(r.posts.includes(2.8), `VGA posts of 5.6 (${[...new Set(r.posts)]})`);
  await s.page.context().close();
}

// VGA against the rear view in the manual's drawing (sizes in mm)
{
  const s = await open(clone(FIX), { view: "persp" });
  await ready(s);
  const r = await s.page.evaluate(() => {
    const g = document.querySelector(".bs-svg .bs-part[data-id='X28']");
    const sh = g.querySelector("path"), b = sh.getBBox();
    const pt = (x, y) => { const q = sh.ownerSVGElement.createSVGPoint(); q.x = x; q.y = y; return sh.isPointInFill(q); };
    return { w: b.width, h: b.height, corner: pt(b.x + b.width - 0.5, b.y + b.height - 0.5), mid: pt(b.x + b.width / 2, b.y + b.height / 2),
      posts: [...g.querySelectorAll("circle")].map((c) => Number(c.getAttribute("r"))) };
  });
  check(Math.abs(r.w - 18.3) < 0.3 && Math.abs(r.h - 9.8) < 0.3, `VGA shell 18.3 x 9.8 (${r.w.toFixed(1)} x ${r.h.toFixed(1)})`);
  check(r.mid && !r.corner, "VGA shell with round corners");
  check(r.posts.includes(2.8), `VGA posts of 5.6 (${[...new Set(r.posts)]})`);
  await s.page.context().close();
}

// the height of the map by the window: the cards under it are seen at once, not on a phone
{
  const fits = (s) => s.page.evaluate(() => {
    const b = document.getElementById("map-summary").getBoundingClientRect();
    return { bottom: b.bottom, win: innerHeight, stage: document.querySelector(".bs-stage").style.height, h: document.querySelector(".bs-stage").clientHeight, scroll: document.documentElement.scrollHeight };
  });
  for (const [w, wh] of [[1300, 700], [1920, 900]]) {
    const s = await open(clone(FIX), { width: w, height: wh, view: "persp" });
    await ready(s);
    await s.page.waitForTimeout(300);
    const r = await fits(s);
    check(r.bottom <= r.win, `${w}x${wh}: the cards in the window (bottom ${Math.round(r.bottom)} of ${r.win})`);
    check(r.h >= 300, `${w}x${wh}: the map stays usable (${r.h})`);
    await s.page.context().close();
  }
  { // cards in two rows: the map keeps its minimum, the first row is seen
    const s = await open(clone(FIX), { width: 1000, height: 760, view: "persp" });
    await ready(s);
    await s.page.waitForTimeout(300);
    const top = await s.page.evaluate(() => document.querySelector("#map-summary .card").getBoundingClientRect().bottom);
    check(top <= 760, `1000x760: the first row of the cards in the window (${Math.round(top)})`);
    await s.page.context().close();
  }
  { // a big window: no empty space added around the board
    const s = await open(clone(FIX), { width: 1300, height: 1500, view: "persp" });
    await ready(s);
    await s.page.waitForTimeout(300);
    const r = await fits(s);
    check(r.bottom < r.win && r.h <= 720, `a tall window: the map no higher than the board needs (${r.h})`);
    await s.page.setViewportSize({ width: 1300, height: 700 });
    await s.page.waitForTimeout(300);
    const r2 = await fits(s);
    check(r2.bottom <= 700, `the window made lower: the cards still in it (${Math.round(r2.bottom)})`);
    await s.page.context().close();
  }
  { // a low window: the map keeps a minimum, the page scrolls
    const s = await open(clone(FIX), { width: 1300, height: 420, view: "persp" });
    await ready(s);
    await s.page.waitForTimeout(300);
    const r = await fits(s);
    check(r.h >= 300, `a low window: a minimum of the map (${r.h})`);
    await s.page.context().close();
  }
  { // a phone: by its width, as before
    const s = await open(clone(FIX), { width: 390, height: 844, view: "persp" });
    await ready(s);
    await s.page.waitForTimeout(300);
    const r = await fits(s);
    check(Math.abs(r.h - 390 * 345 / 630) < 30, `a phone: the height by its width (${r.h})`);
    await s.page.context().close();
  }
}

// the switch of the view changes no size: neither the map nor what is under it moves
for (const [w, wh] of [[1300, 700], [1920, 1080], [1300, 1500], [390, 844]]) {
  const s = await open(clone(FIX), { width: w, height: wh, view: "persp" });
  await ready(s);
  await s.page.waitForTimeout(300);
  const size = () => s.page.evaluate(() => ({
    h: Math.round(document.querySelector(".bs-stage").getBoundingClientRect().height),
    card: Math.round(document.getElementById("map-summary").getBoundingClientRect().top),
    fs: document.querySelector(".bs-svg [font-size]") && [...document.querySelectorAll(".bs-svg g[font-size]")].map((e) => e.getAttribute("font-size")).join(),
  }));
  const a = await size();
  const seen = new Set();
  // from each to each of the three
  for (const v of ["iso", "top", "persp", "top", "iso", "persp"]) {
    await pick(s, v);
    for (let i = 0; i < 8; i++) { const r = await size(); seen.add(JSON.stringify(r)); await s.page.waitForTimeout(120); }
  }
  check(seen.size === 1 && seen.has(JSON.stringify(a)), `${w}x${wh}: the views of one size, labels too (${[...seen].join(" ")})`);
  await s.page.context().close();
}

// X14, X15: the opening in the 3D views at the height of the first USB of X4, from above at the edge
{
  const s = await open(clone(FIX), { width: 1400, height: 800 });
  await ready(s);
  const ys = await s.page.evaluate(() => ["X14", "X15"].map((id) => [...document.querySelectorAll(`.bs-svg .bs-part[data-id='${id}'] rect[fill='#121315']`)].slice(0, 2).map((r) => Number(r.getAttribute("y")))));
  // the first two of that fill are the jacks, side and top; a plug adds its own after them
  check(ys.every((a) => a.length === 2 && a[0] === 3.3 && a[1] === 0), `X14, X15: the opening at 3.3 in the 3D views, at 0 from above (${JSON.stringify(ys)})`);
  await s.page.context().close();
}

// the M.2 module: one box, it ends in the middle of its connector; what lies past that stays seen
for (const [key, cam] of [["persp", { th: -30, ph: 55, inv: 1 / 1000, s: 1.2 }], ["iso", { th: -45, ph: (Math.atan(Math.SQRT2) * 180) / Math.PI, inv: 0, s: 1.25 }]]) {
  const s = await open(clone(FIX), { width: 1400, height: 800, view: key });
  await ready(s);
  await s.page.waitForTimeout(300);
  const [th, ph] = [(cam.th * Math.PI) / 180, (cam.ph * Math.PI) / 180];
  const raw = (x, y, z) => {
    const [dx, dy] = [x - 122, y - 122];
    const Y = dx * Math.sin(th) + dy * Math.cos(th);
    const k = 1 / (1 - cam.inv * (Y * Math.sin(ph) + z * Math.cos(ph)));
    return [cam.s * (dx * Math.cos(th) - dy * Math.sin(th)) * k, cam.s * (Y * Math.cos(ph) - z * Math.sin(ph)) * k];
  };
  const ys = [[0, 0], [244, 0], [0, 244], [244, 244]].map((q) => raw(q[0], q[1], 0)[1]);
  const oy = -(Math.min(...ys) + Math.max(...ys)) / 2;
  const at = (x, y, z) => { const q = raw(x, y, z); return [q[0], q[1] + oy]; };
  const polys = await s.page.evaluate(() => [...document.querySelectorAll(".bs-svg .bs-part[data-id='X48'] polygon")].map((e) => [...e.points].map((p) => [p.x, p.y])));
  check(polys.length === 6, `${key}: the M.2 module is one box (${polys.length / 3 - 1})`);
  // the near edge of the top of the module: its corners x + w, y + h and x, y + h
  const edge = polys[5];
  let end = 0, best = Infinity;
  for (let y = 70; y < 95; y += 0.01) {
    const e = dist(edge[2], at(232, y, 3.5)) + dist(edge[3], at(210, y, 3.5));
    if (e < best) { best = e; end = y; }
  }
  check(Math.abs(end - 84.5) < 0.05, `${key}: the M.2 module ends in the middle of its connector, y 84.5 (${end.toFixed(2)})`);
  const seen = await s.page.evaluate(([over, past]) => {
    const m = document.querySelector(".bs-svg").getScreenCTM();
    const mod = [...document.querySelectorAll(".bs-svg .bs-part[data-id='X48'] polygon")].slice(3);
    const hit = (q) => document.elementFromPoint(m.a * q[0] + m.c * q[1] + m.e, m.b * q[0] + m.d * q[1] + m.f);
    return { over: mod.includes(hit(over)), past: mod.includes(hit(past)), part: hit(past).closest(".bs-part")?.dataset.id };
  }, [at(221, 82, 3.5), at(221, 86, 3)]);
  check(seen.over, `${key}: the module is drawn over the connector up to its middle`);
  check(!seen.past && seen.part === "X48", `${key}: past the middle the opening of the connector is seen, the module's side does not cover it (${seen.past ? "covered" : "seen"}, ${seen.part})`);
  await s.page.context().close();
}

// the pins of the heatsink: of two that meet on the screen the nearer is drawn over the farther
{
  const s = await open(clone(FIX), { width: 1400, height: 800, view: "persp" });
  await ready(s);
  await s.page.waitForTimeout(1000);
  const bad = await s.page.evaluate(() => {
    // three faces to a pin, the first box is the base
    const f = [...document.querySelectorAll(".bs-svg .bs-part[data-id='KPI'] polygon")].slice(3).map((e) => e.getBoundingClientRect());
    const a = [];
    for (let i = 0; i + 2 < f.length; i += 3) a.push({ left: Math.min(f[i].left, f[i + 1].left, f[i + 2].left), right: Math.max(f[i].right, f[i + 1].right, f[i + 2].right), bottom: Math.max(f[i].bottom, f[i + 1].bottom, f[i + 2].bottom) });
    let n = 0;
    for (let i = 0; i < a.length; i++) for (let j = i + 1; j < a.length; j++) {
      if (Math.min(a[i].right, a[j].right) - Math.max(a[i].left, a[j].left) > 2 && a[i].bottom > a[j].bottom + 2) n++;
    }
    return { n, of: a.length };
  });
  check(bad.of === 36 && bad.n === 0, `the heatsink: no far pin over a near one (${bad.n}, ${bad.of} pins)`);
  await s.page.context().close();
}

// the board in the middle of the stage, up and down, in both views
for (const [v, key] of [["persp", "persp"], ["iso", "iso"], ["top", "2d"]]) {
  const s = await open(clone(FIX), { width: 1400, height: 800, view: key });
  await ready(s);
  await s.page.waitForTimeout(300);
  const r = await s.page.evaluate(() => {
    const st = document.querySelector(".bs-stage").getBoundingClientRect();
    let best = null;
    for (const e of document.querySelectorAll(".bs-svg polygon, .bs-svg path")) {
      if (e.closest("defs, clipPath") || !e.getBoundingClientRect) continue;
      const b = e.getBoundingClientRect();
      if (getComputedStyle(e).display !== "none" && (!best || b.width * b.height > best.width * best.height)) best = b;
    }
    return { above: best.top - st.top, below: st.bottom - best.bottom, h: best.height };
  });
  check(Math.abs(r.above - r.below) < 8, `${v}: the board in the middle, up and down (${r.above.toFixed(1)} above, ${r.below.toFixed(1)} below)`);
  // the corners themselves, in the units of the view
  const ys = (await cornersOf(s)).map((p) => p[1]);
  check(Math.abs(Math.min(...ys) + Math.max(...ys)) < 0.1, `${v}: its corners centred (${Math.min(...ys).toFixed(2)}, ${Math.max(...ys).toFixed(2)})`);
  await s.page.context().close();
}

// the view of the board: a pinhole camera, turned by 30 degrees and tilted by 55: the far edge is shorter
{
  const { CAM_DIST: D } = await import(path.join(WWW, "js/lib/boardscene.js"));
  check(D > 0, `the camera has a distance (${D} mm)`);
  // the same camera, written here again: millimetres on the board to the screen, around the middle of the board
  const th = (-30 * Math.PI) / 180, ph = (55 * Math.PI) / 180, S = 1.2;
  const pin = (x, y, z = 0) => {
    const X = (x - 122) * Math.cos(th) - (y - 122) * Math.sin(th);
    const Y = (x - 122) * Math.sin(th) + (y - 122) * Math.cos(th);
    const k = 1 / (1 - (Y * Math.sin(ph) + z * Math.cos(ph)) / D);
    return [S * X * k, S * (Y * Math.cos(ph) - z * Math.sin(ph)) * k];
  };
  const len = (a, b) => Math.hypot(b[0] - a[0], b[1] - a[1]);
  // where two lines through a-b and c-d meet
  const meet = (a, b, c, d) => {
    const r = [b[0] - a[0], b[1] - a[1]], q = [d[0] - c[0], d[1] - c[1]], n = r[0] * q[1] - r[1] * q[0];
    const u = ((c[0] - a[0]) * q[1] - (c[1] - a[1]) * q[0]) / n;
    return [a[0] + u * r[0], a[1] + u * r[1]];
  };
  const angle = (a, b, c, d) => {
    const v = Math.abs(Math.atan2(b[1] - a[1], b[0] - a[0]) - Math.atan2(d[1] - c[1], d[0] - c[0])) * 180 / Math.PI;
    return Math.min(v, 180 - v);
  };
  // the top of a box of a part, as drawn: the corners x,y x+w,y x+w,y+h x,y+h in the units of the view
  const topOf = (s, id, n = 0) => s.page.evaluate(([i, k]) => {
    const e = [...document.querySelectorAll(`.bs-svg .bs-part[data-id='${i}'] polygon`)].filter((p) => !p.getAttribute("filter"))[3 * k + 2];
    return [...e.points].map((p) => [p.x, p.y]);
  }, [id, n]);

  const s = await open(clone(FIX), { width: 1400, height: 800, view: "persp" });
  await ready(s);
  await s.page.waitForTimeout(300);
  const [A, B, C, E] = await topOf(s, "BOARD");
  const far = len(A, B), near = len(C, E), right = len(B, C), lft = len(E, A);
  check(D === 1000, `the camera 1000 mm from the middle of the board (${D})`);
  check(Math.abs(far / near - 0.870) < 0.005, `far edge to near edge of the board about 0.870 (${far.toFixed(2)} / ${near.toFixed(2)} = ${(far / near).toFixed(4)})`);
  // in millimetres of the board, the scale of the screen comes after the perspective
  check(Math.abs(far / S - 208.7) < 0.2 && Math.abs(near / S - 239.9) < 0.2, `the long edges in projected mm, 208.7 and 239.9 (${(far / S).toFixed(1)}, ${(near / S).toFixed(1)})`);
  check(Math.abs(lft / S - 173.9) < 0.2 && Math.abs(right / S - 173.6) < 0.2, `the sides in projected mm, 173.9 and 173.6 (${(lft / S).toFixed(1)}, ${(right / S).toFixed(1)})`);
  check(angle(A, B, E, C) > 0.5, `the long edges are not parallel (${angle(A, B, E, C).toFixed(2)} degrees)`);
  check(angle(A, E, B, C) > 0.5, `the short edges are not parallel (${angle(A, E, B, C).toFixed(2)} degrees)`);
  const vl = meet(A, B, E, C), vs = meet(A, E, B, C);
  check(vl[1] < Math.min(A[1], B[1]) && vl[0] > B[0], `the long edges meet far away, up and to the right (${vl.map((v) => v.toFixed(0))})`);
  check(vs[1] < Math.min(A[1], B[1]), `the short edges meet far away, up (${vs.map((v) => v.toFixed(0))})`);
  // the two sides of a turned board are not one length: not a mirror, the turn brings one nearer
  const pa = pin(0, 0), pb = pin(244, 0), pc = pin(244, 244), pe = pin(0, 244);
  check(Math.abs(right / lft - len(pb, pc) / len(pe, pa)) < 0.002, `the side edges as the camera gives them (${right.toFixed(2)}, ${lft.toFixed(2)}; ${(len(pb, pc) / len(pe, pa)).toFixed(4)})`);
  // one size, farther: smaller by what the camera gives
  const m46 = await topOf(s, "X46"), m45 = await topOf(s, "X45");
  const want = len(pin(48, 15, 7.5), pin(195, 15, 7.5)) / len(pin(48, 140, 7.5), pin(195, 140, 7.5));
  const got = len(m46[0], m46[1]) / len(m45[0], m45[1]);
  check(got < 0.97 && Math.abs(got - want) < 0.002, `a memory slot far to one near, X46 / X45 (${got.toFixed(4)}, the camera gives ${want.toFixed(4)})`);
  // a large thing in a plane lies on what it stands on: the frame of a slot on the top of its box, the lid on the CPU
  const corners = await s.page.evaluate(() => {
    const root = document.querySelector(".bs-svg").getScreenCTM().inverse();
    const toRoot = (e, x, y) => { const m = root.multiply(e.getScreenCTM()); return [m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f]; };
    const first = (e) => e.getAttribute("d").match(/^M\s*(-?[\d.]+)[ ,]+(-?[\d.]+)/).slice(1).map(Number);
    // the largest outline of a part, drawn through the camera as a path
    const outline = (id) => [...document.querySelectorAll(`.bs-svg .bs-part[data-id='${id}'] path`)].filter((e) => /^M/.test(e.getAttribute("d"))).sort((p, q) => q.getTotalLength() - p.getTotalLength())[0];
    const fr = outline("X46");
    const lid = outline("CPU");
    return { frame: fr && toRoot(fr, ...first(fr)), lid: lid && toRoot(lid, ...first(lid)) };
  });
  const offLine = (p, a, b) => Math.abs((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])) / len(a, b);
  check(corners.frame && offLine(corners.frame, m46[0], m46[1]) < 0.1, `the frame of a long slot on the edge of its box (${corners.frame && offLine(corners.frame, m46[0], m46[1]).toFixed(3)})`);
  const cpu = await topOf(s, "CPU", 1);
  check(corners.lid && offLine(corners.lid, cpu[0], cpu[1]) < 0.3, `the lid on the CPU (${corners.lid && offLine(corners.lid, cpu[0], cpu[1]).toFixed(3)})`);
  // what the pointer finds is the part drawn there
  const hit = await s.page.evaluate(() => Object.fromEntries(["X46", "X45", "CPU", "KPI", "X31", "X28"].map((id) => {
    const r = document.querySelectorAll(`.bs-svg .bs-part[data-id='${id}'] polygon`);
    const e = r[2];
    const m = e.getScreenCTM();
    const q = [...e.points].map((p) => [m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f]);
    const x = q.reduce((t, p) => t + p[0], 0) / q.length, y = q.reduce((t, p) => t + p[1], 0) / q.length;
    const f = document.elementFromPoint(x, y);
    return [id, f && f.closest(".bs-part") && f.closest(".bs-part").dataset.id];
  })));
  check(Object.entries(hit).every(([k, v]) => k === v || /^S:/.test(v)), `the pointer on a part finds that part (${JSON.stringify(hit)})`);
  // from above: parallel, as it was
  await pick(s, "top");
  await s.page.waitForTimeout(1000);
  const [a0, b0, c0, e0] = await topOf(s, "BOARD");
  check(len(a0, b0) === len(e0, c0) && len(a0, e0) === len(b0, c0) && a0[1] === b0[1] && a0[0] === e0[0], `from above: a square, no perspective (${len(a0, b0).toFixed(2)}, ${len(a0, e0).toFixed(2)})`);
  const silk = await s.page.evaluate(() => {
    const t = [...document.querySelectorAll(".bs-svg text")].find((e) => e.textContent === "PCIE x8");
    const m = t.getScreenCTM();
    return [m.a, m.b, m.c, m.d].map((v) => +v.toFixed(5));
  });
  check(silk[1] === 0 && silk[2] === 0 && silk[0] === silk[3], `from above: what is drawn in a plane has no slant (${silk})`);
  await pick(s, "persp");
  await s.page.waitForTimeout(1000);
  // a long slot has parts behind it that are nearer by its middle
  const order = await s.page.evaluate(() => {
    const all = [...document.querySelectorAll(".bs-svg .bs-part")].map((e) => e.dataset.id);
    return { x5: all.indexOf("X5"), x46: all.indexOf("X46"), x54: all.indexOf("X54"), x48: all.indexOf("X48") };
  });
  check(order.x5 < order.x46, `perspective: a fan connector behind a memory slot drawn before it (${order.x5}, ${order.x46})`);
  // the name on the M.2 module not upside down
  const m2 = await s.page.evaluate(() => {
    const t = [...document.querySelectorAll(".bs-part[data-id='X48'] text")].find((e) => e.textContent === "NT-512" && Number(e.closest("g[opacity]").getAttribute("opacity")) === 1);
    const m = t.getScreenCTM();
    return m.a;
  });
  check(m2 > 0, `perspective: the name on the M.2 module not upside down (${m2.toFixed(2)})`);
  await s.page.context().close();
}

// three views, three buttons; what was chosen comes back; nothing or something else stored: the perspective
{
  const s = await open(clone(FIX), { width: 1400, height: 800 });
  await ready(s);
  const names = await s.page.locator("[data-view-btn]").allTextContents();
  check(names.join() === "Изометрия,Перспектива,Сверху", `three buttons in their order (${names})`);
  check((await viewOn(s)) === "persp", `nothing stored: the perspective (${await viewOn(s)})`);
  const [a, b, c] = await cornersOf(s);
  check(Math.abs(dist(a, b) / dist(c, (await cornersOf(s))[3]) - 0.87) < 0.005, "nothing stored: its numbers");
  for (const [v, key] of [["iso", "iso"], ["top", "2d"], ["persp", "persp"]]) {
    await pick(s, v);
    await s.page.waitForTimeout(1000);
    check((await stored(s)) === key, `${v}: remembered as ${key} (${await stored(s)})`);
    await s.page.reload();
    await ready(s);
    check((await viewOn(s)) === v, `${key}: comes back as ${v} (${await viewOn(s)})`);
    const q = await cornersOf(s);
    const ratio = dist(q[0], q[1]) / dist(q[3], q[2]);
    const want = { iso: 1, top: 1, persp: 0.87 }[v];
    check(Math.abs(ratio - want) < 0.005, `${v}: after a reload far to near ${ratio.toFixed(4)}`);
    const flat = Math.abs(q[0][1] - q[1][1]) < 0.01;
    check(v === "top" ? flat : !flat, `${v}: after a reload ${v === "top" ? "the board square on" : "the board turned"} (${q[0][1].toFixed(2)}, ${q[1][1].toFixed(2)})`);
    if (v === "iso") {
      const d = (Math.abs(Math.atan2(q[1][1] - q[0][1], q[1][0] - q[0][0])) * 180) / Math.PI;
      check(Math.abs(d - 30) < 0.2, `iso: after a reload the edges at 30 degrees (${d.toFixed(2)})`);
    }
    check((await stored(s)) === key, `${v}: still stored as ${key} after a reload (${await stored(s)})`);
  }
  await s.page.context().close();
}
for (const key of ["3d", "top"]) {
  const s = await open(clone(FIX), { width: 1400, height: 800, view: key });
  await ready(s);
  check((await viewOn(s)) === "persp", `${key} stored, unknown: the perspective (${await viewOn(s)})`);
  await s.page.context().close();
}
{
  const s = await open(clone(FIX), { width: 1400, height: 800, view: "2d" });
  await ready(s);
  const vb = await s.page.locator(".bs-svg").getAttribute("viewBox");
  check(vb === "-330 -172.5 630 345", `2d stored: from above (${vb})`);
  check((await viewOn(s)) === "top", `2d stored: its button on (${await viewOn(s)})`);
  await s.page.context().close();
}

// no moving pictures asked for: the view changes at once
{
  const s = await open(clone(FIX), { width: 1400, height: 800 });
  await ready(s);
  await s.page.emulateMedia({ reducedMotion: "reduce" });
  await pick(s, "iso");
  const q = await cornersOf(s);
  check(Math.abs(dist(q[0], q[1]) / dist(q[3], q[2]) - 1) < 0.001, `reduced motion: the isometric view at once (${(dist(q[0], q[1]) / dist(q[3], q[2])).toFixed(4)})`);
  await s.page.context().close();
}
// a browser that keeps nothing: the map works, the buttons too
{
  const s = await open(clone(FIX), { width: 1400, height: 800, noStorage: true });
  await ready(s);
  check((await viewOn(s)) === "persp", `no storage: the perspective (${await viewOn(s)})`);
  await pick(s, "iso");
  check((await viewOn(s)) === "iso" && !s.errors.length, `no storage: a button still works (${await viewOn(s)}; ${s.errors.join(";").slice(0, 120)})`);
  await s.page.context().close();
}

// the isometric view: a parallel projection, the three axes foreshortened alike
{
  const s = await open(clone(FIX), { width: 1400, height: 800, view: "iso" });
  await ready(s);
  await s.page.waitForTimeout(300);
  const q = await cornersOf(s);
  const [A, B, C, E] = q;
  const edges = q.map((p, i) => dist(p, q[(i + 1) % 4]));
  const mean = edges.reduce((t, v) => t + v, 0) / 4;
  check(edges.every((v) => Math.abs(v - mean) / mean < 0.005), `isometric: the edges of the board of one length (${edges.map((v) => v.toFixed(2))})`);
  const slope = q.map((p, i) => { const n = q[(i + 1) % 4]; const a = Math.abs((Math.atan2(n[1] - p[1], n[0] - p[0]) * 180) / Math.PI); return a > 90 ? 180 - a : a; });
  check(slope.every((d) => Math.abs(d - 30) < 0.2), `isometric: the edges at 30 degrees (${slope.map((d) => d.toFixed(2))})`);
  check(between(A, B, E, C) < 0.05 && between(B, C, A, E) < 0.05, `isometric: opposite edges parallel (${between(A, B, E, C).toFixed(3)}, ${between(B, C, A, E).toFixed(3)} degrees)`);
  check(Math.abs(mean / 244 - 0.8165 * 1.25) < 0.005, `isometric: 0.8165 of a millimetre at the scale of 1.25 (${(mean / 244).toFixed(4)})`);
  check(E[1] > A[1] && E[0] > A[0] && B[1] < A[1], `isometric: turned so the rear panel is seen (${[A, B, C, E].map((p) => p.map((v) => v.toFixed(0)))})`);
  const r = await s.page.evaluate(() => {
    const root = document.querySelector(".bs-svg").getScreenCTM().inverse();
    // a plane of the rear panel: its first axis along the edge of the board, its second straight up
    const pl = [...document.querySelectorAll(".bs-svg .bs-part[data-id='X28'] g[transform^='matrix']")].find((g) => Math.abs(g.transform.baseVal[0].matrix.c) < 1e-5);
    if (!pl) return null;
    const m = root.multiply(pl.getScreenCTM());
    return { along: Math.hypot(m.a, m.b), up: Math.hypot(m.c, m.d) };
  });
  const mm = mean / 244;
  check(r && Math.abs(r.up - mm) / mm < 0.005, `isometric: a millimetre up as long as one along the board (${r && r.up.toFixed(4)}, ${mm.toFixed(4)})`);
  check(r && Math.abs(r.along - mm) / mm < 0.005, `isometric: a millimetre along the rear panel the same (${r && r.along.toFixed(4)})`);
  const order = await s.page.evaluate(() => {
    const all = [...document.querySelectorAll(".bs-svg .bs-part")].map((e) => e.dataset.id);
    return { x5: all.indexOf("X5"), x46: all.indexOf("X46") };
  });
  check(order.x5 < order.x46, `isometric: a fan connector behind a memory slot drawn before it (${order.x5}, ${order.x46})`);
  const m2 = await s.page.evaluate(() => {
    const t = [...document.querySelectorAll(".bs-part[data-id='X48'] text")].find((e) => e.textContent === "NT-512" && Number(e.closest("g[opacity]").getAttribute("opacity")) === 1);
    return t.getScreenCTM().a;
  });
  check(m2 > 0, `isometric: the name on the M.2 module not upside down (${m2.toFixed(2)})`);
  const bad = await s.page.evaluate(() => {
    const f = [...document.querySelectorAll(".bs-svg .bs-part[data-id='KPI'] polygon")].slice(3).map((e) => e.getBoundingClientRect());
    const a = [];
    for (let i = 0; i + 2 < f.length; i += 3) a.push({ left: Math.min(f[i].left, f[i + 1].left, f[i + 2].left), right: Math.max(f[i].right, f[i + 1].right, f[i + 2].right), bottom: Math.max(f[i].bottom, f[i + 1].bottom, f[i + 2].bottom) });
    let n = 0;
    for (let i = 0; i < a.length; i++) for (let j = i + 1; j < a.length; j++) {
      if (Math.min(a[i].right, a[j].right) - Math.max(a[i].left, a[j].left) > 2 && a[i].bottom > a[j].bottom + 2) n++;
    }
    return { n, of: a.length };
  });
  check(bad.of === 36 && bad.n === 0, `isometric: the heatsink, no far pin over a near one (${bad.n}, ${bad.of} pins)`);
  await s.page.context().close();
}

// what the pointer finds is the part drawn there, in each view
for (const key of ["persp", "iso", "2d"]) {
  const s = await open(clone(FIX), { width: 1400, height: 800, view: key });
  await ready(s);
  await s.page.waitForTimeout(300);
  const hit = await s.page.evaluate(() => Object.fromEntries(["X46", "X45", "CPU", "KPI", "X31", "X28", "X15"].map((id) => {
    const e = document.querySelectorAll(`.bs-svg .bs-part[data-id='${id}'] polygon`)[2];
    const m = e.getScreenCTM();
    const q = [...e.points].map((p) => [m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f]);
    const f = document.elementFromPoint(q.reduce((t, p) => t + p[0], 0) / q.length, q.reduce((t, p) => t + p[1], 0) / q.length);
    return [id, f && f.closest(".bs-part") && f.closest(".bs-part").dataset.id];
  })));
  check(Object.entries(hit).every(([k, v]) => k === v || /^S:/.test(v)), `${key}: the pointer on a part finds that part (${JSON.stringify(hit)})`);
  await s.page.context().close();
}

// on the way from a view to a view: a camera between the two, what lies in a plane stays on what it lies on
for (const [from, to, a0, a1] of [["persp", "iso", 0.87, 1], ["iso", "persp", 0.87, 1], ["iso", "top", 0.9999, 1.0001], ["top", "persp", 0.87, 1]]) {
  const s = await open(clone(FIX), { width: 1400, height: 800, view: from === "top" ? "2d" : from });
  await ready(s);
  await s.page.waitForTimeout(300);
  await s.page.evaluate(() => {
    window.__f = [];
    const root = document.querySelector(".bs-svg").getScreenCTM().inverse();
    const seen = () => {
      const pl = [...document.querySelectorAll(".bs-svg .bs-part[data-id='BOARD'] polygon")].filter((p) => !p.getAttribute("filter"))[2];
      const q = [...pl.points].map((p) => [p.x, p.y]);
      const box = [...document.querySelectorAll(".bs-svg .bs-part[data-id='X46'] polygon")][2];
      const b = [...box.points].map((p) => [p.x, p.y]);
      const fr = [...document.querySelectorAll(".bs-svg .bs-part[data-id='X46'] path")].filter((e) => /^M/.test(e.getAttribute("d")) && getComputedStyle(e).display !== "none").sort((x, y) => y.getTotalLength() - x.getTotalLength())[0];
      let off = 0;
      if (fr) {
        const m = document.querySelector(".bs-svg").getScreenCTM().inverse().multiply(fr.getScreenCTM());
        const [x, y] = fr.getAttribute("d").match(/^M\s*(-?[\d.]+)[ ,]+(-?[\d.]+)/).slice(1).map(Number);
        const p = [m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f];
        off = Math.abs((b[1][0] - b[0][0]) * (p[1] - b[0][1]) - (b[1][1] - b[0][1]) * (p[0] - b[0][0])) / Math.hypot(b[1][0] - b[0][0], b[1][1] - b[0][1]);
      }
      window.__f.push({ far: Math.hypot(q[1][0] - q[0][0], q[1][1] - q[0][1]), near: Math.hypot(q[2][0] - q[3][0], q[2][1] - q[3][1]), off, y0: q[0][1] });
      if (window.__f.length < 400 && !window.__stop) requestAnimationFrame(seen);
    };
    void root;
    requestAnimationFrame(seen);
  });
  await pick(s, to);
  await s.page.waitForTimeout(1200);
  const f = await s.page.evaluate(() => { window.__stop = 1; return window.__f; });
  const ratios = f.map((x) => x.far / x.near);
  const start = ratios[0], end = ratios[ratios.length - 1];
  const inner = ratios.filter((r) => r > Math.min(start, end) + 1e-3 && r < Math.max(start, end) - 1e-3);
  const y0s = new Set(f.map((x) => x.y0.toFixed(1)));
  check(from === "iso" && to === "top" ? y0s.size > 3 : inner.length > 0, `${from} to ${to}: frames between the two (${f.length} frames, ${inner.length} of them between, far to near ${start.toFixed(3)} to ${end.toFixed(3)})`);
  check(ratios.every((r) => r >= a0 - 0.002 && r <= a1 + 0.002), `${from} to ${to}: far to near stays in its range (${Math.min(...ratios).toFixed(4)}, ${Math.max(...ratios).toFixed(4)})`);
  const off = Math.max(...f.map((x) => x.off));
  check(off < 0.3, `${from} to ${to}: the frame of a long slot on its box in each of the frames (${off.toFixed(3)})`);
  check(!s.errors.length, `${from} to ${to}: no errors (${s.errors.join(";").slice(0, 120)})`);
  await s.page.context().close();
}

// the buttons on a phone: all of them there, nothing sideways
{
  const s = await open(clone(FIX), { width: 390, height: 844 });
  await ready(s);
  await s.page.waitForTimeout(300);
  const r = await s.page.evaluate(() => ({
    sw: document.documentElement.scrollWidth,
    b: [...document.querySelectorAll("[data-view-btn], [data-zoom]")].map((e) => { const q = e.getBoundingClientRect(); return [q.left, q.right]; }),
  }));
  check(r.b.length === 6 && r.b.every(([a, z]) => a >= 0 && z <= 390), `a phone: zoom and the three views in the width (${r.b.length}, right ${Math.round(Math.max(...r.b.map((x) => x[1])))})`);
  check(r.sw <= 390, `a phone: no sideways scroll with them (${r.sw})`);
  await s.page.context().close();
}

// the callouts in each view from the side, in the sizes of a window: none cut, none on another, none on the board
for (const view of ["persp", "iso"]) for (const [w, wh] of [[1000, 692], [1400, 800], [1920, 1080], [390, 844]]) {
  const s = await open(clone(FIX), { width: w, height: wh, view });
  await ready(s);
  await s.page.waitForTimeout(300);
  const r = await s.page.evaluate(() => {
    const st = document.querySelector(".bs-stage").getBoundingClientRect();
    const name = (e) => e.closest(".bs-part").dataset.id;
    const boxes = (q) => [...document.querySelectorAll(q)].map((e) => ({ id: name(e), b: e.getBoundingClientRect() }));
    const lbl = boxes(".bs-svg text.bs-lbl");
    const cloud = boxes(".bs-svg rect.bs-pill");
    const cut = [...lbl, ...cloud].filter(({ b }) => b.left < st.left || b.right > st.right || b.top < st.top || b.bottom > st.bottom).map((x) => x.id);
    const hit = (p, q) => p.left < q.right && q.left < p.right && p.top < q.bottom && q.top < p.bottom;
    const over = [];
    const all = [...lbl, ...cloud];
    all.forEach((x, i) => all.slice(i + 1).forEach((y) => { if (hit(x.b, y.b)) over.push(`${x.id}/${y.id}`); }));
    // the plate as a diamond on the screen: a label with a corner in it lies on the board
    const pl = [...document.querySelectorAll(".bs-svg .bs-part[data-id='BOARD'] polygon")].filter((e) => !e.getAttribute("filter")).map((e) => {
      const m = e.getScreenCTM();
      return [...e.points].map((p) => [m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f]);
    }).sort((p, q) => Math.hypot(q[0][0] - q[2][0], q[0][1] - q[2][1]) - Math.hypot(p[0][0] - p[2][0], p[0][1] - p[2][1]))[0];
    const inside = (x, y) => pl.every((p, i) => { const n = pl[(i + 1) % 4]; return (n[0] - p[0]) * (y - p[1]) - (n[1] - p[1]) * (x - p[0]) > 0; }) || pl.every((p, i) => { const n = pl[(i + 1) % 4]; return (n[0] - p[0]) * (y - p[1]) - (n[1] - p[1]) * (x - p[0]) < 0; });
    const onBoard = lbl.filter(({ b }) => [[b.left, b.top], [b.right, b.top], [b.left, b.bottom], [b.right, b.bottom]].some(([x, y]) => inside(x, y))).map((x) => x.id);
    const xs = pl.map((p) => p[0]), ys = pl.map((p) => p[1]);
    return { cut, over, onBoard, fill: [Math.round(Math.max(...xs) - Math.min(...xs)), Math.round(Math.max(...ys) - Math.min(...ys)), Math.round(st.width), Math.round(st.height)] };
  });
  check(!r.cut.length, `${view} ${w}x${wh}: no callout cut by the stage (${r.cut})`);
  check(!r.over.length, `${view} ${w}x${wh}: no callout on another (${r.over})`);
  check(!r.onBoard.length, `${view} ${w}x${wh}: no callout on the board (${r.onBoard})`);
  check(r.fill[0] > r.fill[2] * 0.45 || r.fill[1] > r.fill[3] * 0.8, `${view} ${w}x${wh}: the board fills the stage (${r.fill})`);
  await s.page.context().close();
}

// a switch: the callouts stand where this view wants them, as if it had been open from the start
for (const [w, wh] of [[1000, 692], [1400, 800]]) {
  const where = (s) => s.page.evaluate(() => [...document.querySelectorAll(".bs-svg text.bs-lbl, .bs-svg rect.bs-pill")].map((e) => [e.closest(".bs-part").dataset.id, Number(e.getAttribute("x")) || 0, Number(e.getAttribute("y")) || 0]));
  for (const [from, to] of [["persp", "iso"], ["iso", "persp"]]) {
    const a = await open(clone(FIX), { width: w, height: wh, view: to });
    await ready(a);
    await a.page.waitForTimeout(300);
    const want = await where(a);
    await a.page.context().close();
    const s = await open(clone(FIX), { width: w, height: wh, view: from });
    await ready(s);
    await s.page.waitForTimeout(300);
    await pick(s, to);
    await s.page.waitForTimeout(1200);
    const got = await where(s);
    const worst = Math.max(...got.map((p, i) => Math.hypot(p[1] - want[i][1], p[2] - want[i][2])));
    check(got.length === want.length && worst < 0.1, `${w}x${wh}, ${from} to ${to}: the callouts as this view lays them out (worst ${worst.toFixed(2)})`);
    await s.page.context().close();
  }
}

// zoom by the buttons
{
  const s = await open(clone(FIX), { width: 1300, height: 800, view: "persp" });
  await ready(s);
  const vb = () => s.page.evaluate(() => document.querySelector(".bs-svg").getAttribute("viewBox").split(" ").map(Number));
  const btn = (n) => s.page.locator(`[data-zoom='${n}']`);
  const v0 = await vb();
  check(await btn("out").isDisabled(), "zoom: nothing to take out at 100 %");
  check((await btn("reset").textContent()).trim() === "100 %", "zoom: shows 100 %");
  await btn("in").click();
  const v1 = await vb();
  check(Math.abs(v1[2] - v0[2] / 1.5) < 0.1, `zoom in: the view 1.5 times less (${v0[2]} -> ${v1[2]})`);
  check((await btn("reset").textContent()).trim() === "150 %", "zoom: shows 150 %");
  await btn("in").click(); await btn("in").click(); await btn("in").click();
  check(await btn("in").isDisabled(), "zoom: a limit, the button off");
  await btn("out").click(); await btn("out").click(); await btn("out").click();
  // a drag moves the view and is not a click on what is under it
  const box = await s.page.locator(".bs-stage").boundingBox();
  const cx = box.x + box.width / 2, cy = box.y + box.height / 2;
  const before = await vb();
  await s.page.mouse.move(cx, cy);
  await s.page.mouse.down();
  await s.page.mouse.move(cx - 120, cy - 60, { steps: 4 });
  await s.page.mouse.up();
  const after = await vb();
  check(after[0] > before[0] && after[1] > before[1], `zoom: a drag moves the view (${before[0]},${before[1]} -> ${after[0]},${after[1]})`);
  await s.page.mouse.move(2, 2);
  check(await s.page.evaluate(() => { const p = document.querySelector(".bs-pop"); return !p || p.hidden; }), "zoom: a drag pins no card of a part");
  // the edge: no flying off the board
  for (let i = 0; i < 6; i++) {
    await s.page.mouse.move(cx, cy);
    await s.page.mouse.down();
    await s.page.mouse.move(cx - 500, cy, { steps: 2 });
    await s.page.mouse.up();
  }
  const far = await vb();
  check(far[0] + far[2] <= v0[0] + v0[2] + 0.1, "zoom: the view stops at the edge");
  // the view switched and the scene built again keep it
  // between the two views from the side the same place is kept, the height of what is shown is one
  const v3d = await vb();
  await pick(s, "iso");
  await s.page.waitForTimeout(900);
  const vi = await vb();
  check(v3d.join() === vi.join(), `zoom: perspective to isometric keeps the place (${v3d}; ${vi})`);
  const before2 = await vb();
  await s.page.mouse.move(cx, cy);
  await s.page.mouse.down();
  await s.page.mouse.move(cx + 80, cy + 40, { steps: 4 });
  await s.page.mouse.up();
  check((await vb())[0] < before2[0], `zoom: a drag works in the isometric view too (${before2[0]} -> ${(await vb())[0]})`);
  await s.page.mouse.move(2, 2);
  await pick(s, "top");
  await s.page.waitForTimeout(900);
  check((await btn("reset").textContent()).trim() === "150 %", "zoom: stays through the switch of the view");
  s.fix = clone(s.fix);
  s.fix.host = null;
  await s.page.evaluate(() => { document.querySelector(".bs-svg").dataset.old = "1"; });
  await s.page.waitForTimeout(5600);
  check(await s.page.evaluate(() => !document.querySelector(".bs-svg").dataset.old), "zoom: the scene was built again");
  const again = await vb();
  check(Math.abs(again[2] - v0[2] / 1.5) < 0.1, `zoom: stays through a rebuilt scene (${again[2]})`);
  await btn("reset").click();
  const back = await vb();
  check(back[2] === v0[2] && back[0] === v0[0], "zoom: the button with the percent goes back to the whole board");
  check(s.errors.length === 0, `zoom: no errors (${s.errors.join(";")})`);
  await s.page.context().close();
}

// a slow BMC, a cycle longer than the period: one request at a time, the next cycle after it
{
  const s = await open(clone(FIX), { slow: 1200 });
  await ready(s);
  await s.page.waitForTimeout(13000);
  check(s.peak === 1, `one live request at a time on a slow BMC (peak ${s.peak})`);
  // (one request a cycle with the view: a cycle starts its period after the
  // answer of the one before, so starts are apart by the answer and the period)
  const gaps = (s.cycles || []).slice(1).map((t, i) => t - s.cycles[i]);
  check(gaps.length >= 1 && gaps.every((g) => g >= 5000 + 1200 - 300), `the next cycle waits for the one before and the period (starts apart ${gaps.join(", ")} ms)`);
  await s.page.context().close();
}

// a phone
{
  const s = await open(clone(FIX), { width: 390, height: 844 });
  await ready(s);
  await s.page.waitForTimeout(300);
  const sw = await s.page.evaluate(() => document.documentElement.scrollWidth);
  check(sw <= 390, `a phone: no sideways scroll (${sw})`);
  check(/x8\/x8/.test(await label(s, "X30")) || /x8/.test(await label(s, "X30")), "the short labels there");
  await shot(s, "map-phone.png");
  await s.page.context().close();
}

await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 10000))]);
console.log(`${failed} failed`);
process.exit(failed ? 1 : 0);
