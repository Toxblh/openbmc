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

async function open(fix, { width = 1300, height = 900, view, led = { Asserted: false }, html = HARNESS, block, slow = 0 } = {}) {
  const ctx = await browser.newContext({ locale: "ru-RU", viewport: { width, height } });
  const page = await ctx.newPage();
  const s = { page, errors: [], fix };
  page.on("pageerror", (e) => s.errors.push(e.message));
  page.on("console", (m) => m.type() === "error" && !/404/.test(m.text()) && s.errors.push(m.text()));
  const j = (v) => ({ contentType: "application/json", body: JSON.stringify({ data: { Json: JSON.stringify(v) } }) });
  await page.route(`${ORIGIN}/**`, async (route) => {
    const p = new URL(route.request().url()).pathname;
    s.requests = (s.requests || 0) + (p === `${P}/sensors` ? 1 : 0);
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
    if (p in dbus) return dbus[p] ? route.fulfill(j(dbus[p])) : route.fulfill({ status: 404, body: "" });
    if (p.endsWith("/EthernetInterfaces/eth0")) return f2.eth ? route.fulfill({ contentType: "application/json", body: JSON.stringify(f2.eth) }) : route.fulfill({ status: 404, body: "" });
    const f = path.join(WWW, p);
    if (!fs.existsSync(f) || fs.statSync(f).isDirectory()) return route.fulfill({ status: 404, body: "" });
    const types = { ".js": "text/javascript", ".css": "text/css" };
    return route.fulfill({ contentType: types[path.extname(f)] || "application/octet-stream", body: fs.readFileSync(f) });
  });
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
  const g = document.querySelector(`.bs-svg .bs-part[data-id='${i}'] [clip-path]`);
  if (!g) return null;
  const r = document.querySelector(`${g.getAttribute("clip-path").slice(4, -1)} rect`);
  return Number(r.getAttribute("width"));
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
  // the BMC's load
  check(/LA 0\.42/.test(await s.page.locator(".bs-part[data-id=BMC]").last().textContent()), "the BMC's load average in a cloud at its chip");
  check(/0\.42 \/ 0\.30 \/ 0\.25/.test(await pop(s, "BMC")), "all three in its popover");
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
    const a = [...g.querySelectorAll("polygon, rect, path")].filter((e) => !e.closest("svg svg")).map((e) => e.getBoundingClientRect());
    const l = g.querySelector("svg svg, svg").getBoundingClientRect();
    return { w: Math.max(...a.map((b) => b.width)) / l.width, h: Math.max(...a.map((b) => b.height)) / l.height };
  });
  check(r.w < 1.02 && r.h < 1.02, `nothing of the CPU wider than its lid (${r.w.toFixed(2)} x ${r.h.toFixed(2)})`);
  await s.page.context().close();
}

// the rear panel against the drawing of the manual (ТВГИ.469555.445РЭ, sizes in mm)
{
  const s = await open(clone(FIX), { view: "iso" });
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
  const s = await open(clone(FIX), { view: "iso" });
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
    const s = await open(clone(FIX), { width: w, height: wh, view: "iso" });
    await ready(s);
    await s.page.waitForTimeout(300);
    const r = await fits(s);
    check(r.bottom <= r.win, `${w}x${wh}: the cards in the window (bottom ${Math.round(r.bottom)} of ${r.win})`);
    check(r.h >= 300, `${w}x${wh}: the map stays usable (${r.h})`);
    await s.page.context().close();
  }
  { // cards in two rows: the map keeps its minimum, the first row is seen
    const s = await open(clone(FIX), { width: 1000, height: 760, view: "iso" });
    await ready(s);
    await s.page.waitForTimeout(300);
    const top = await s.page.evaluate(() => document.querySelector("#map-summary .card").getBoundingClientRect().bottom);
    check(top <= 760, `1000x760: the first row of the cards in the window (${Math.round(top)})`);
    await s.page.context().close();
  }
  { // a big window: no empty space added around the board
    const s = await open(clone(FIX), { width: 1300, height: 1500, view: "iso" });
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
    const s = await open(clone(FIX), { width: 1300, height: 420, view: "iso" });
    await ready(s);
    await s.page.waitForTimeout(300);
    const r = await fits(s);
    check(r.h >= 300, `a low window: a minimum of the map (${r.h})`);
    await s.page.context().close();
  }
  { // a phone: by its width, as before
    const s = await open(clone(FIX), { width: 390, height: 844, view: "iso" });
    await ready(s);
    await s.page.waitForTimeout(300);
    const r = await fits(s);
    check(Math.abs(r.h - 390 * 345 / 630) < 30, `a phone: the height by its width (${r.h})`);
    await s.page.context().close();
  }
}

// the switch of the view changes no size: neither the map nor what is under it moves
for (const [w, wh] of [[1300, 700], [1920, 1080], [1300, 1500], [390, 844]]) {
  const s = await open(clone(FIX), { width: w, height: wh, view: "iso" });
  await ready(s);
  await s.page.waitForTimeout(300);
  const size = () => s.page.evaluate(() => ({
    h: Math.round(document.querySelector(".bs-stage").getBoundingClientRect().height),
    card: Math.round(document.getElementById("map-summary").getBoundingClientRect().top),
    fs: document.querySelector(".bs-svg [font-size]") && [...document.querySelectorAll(".bs-svg g[font-size]")].map((e) => e.getAttribute("font-size")).join(),
  }));
  const a = await size();
  const seen = new Set();
  await s.page.locator("[data-view-btn='top']").click();
  for (let i = 0; i < 8; i++) { const r = await size(); seen.add(JSON.stringify(r)); await s.page.waitForTimeout(120); }
  await s.page.locator("[data-view-btn='iso']").click();
  for (let i = 0; i < 8; i++) { const r = await size(); seen.add(JSON.stringify(r)); await s.page.waitForTimeout(120); }
  check(seen.size === 1 && seen.has(JSON.stringify(a)), `${w}x${wh}: the views of one size, labels too (${[...seen].join(" ")})`);
  await s.page.context().close();
}

// zoom by the buttons
{
  const s = await open(clone(FIX), { width: 1300, height: 800, view: "iso" });
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
  await s.page.locator("[data-view-btn='top']").click();
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
  check(s.requests === 2, `the next cycle waits for the one before and the period (${s.requests} cycles in 19 s)`);
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

await browser.close();
console.log(`${failed} failed`);
process.exit(failed ? 1 : 0);
