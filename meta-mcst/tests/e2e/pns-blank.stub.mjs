// E2E without a BMC: the PNS card's button for a chip with no image (REGRESSIONS
// 83). The page (js/pages/pns.js, the real file) in a bare harness, the REST
// answers stubbed: when the button is there, what it asks and sends.
//
//     node pns-blank.stub.mjs       # WWW: the web tree, CHROME: chromium
import { chromium } from "playwright";
import fs from "node:fs";
import path from "node:path";

const HERE = path.dirname(new URL(import.meta.url).pathname);
const WWW = process.env.WWW || path.join(HERE, "../../meta-common/recipes-phosphor/reimu-webui/files/www");
const ORIGIN = "http://reimu.test";
const PF = "/xyz/openbmc_project/reimu/pns_flash";
const BTN = "Записать на флеш без образа";

let failed = 0;
const check = (c, what) => { console.log(`${c ? "PASS" : "FAIL"} ${what}`); if (!c) failed++; };

const HARNESS = (power) => `<!doctype html><html><head><meta charset="utf-8"></head><body><div id="toasts"></div><main id="page"></main>
<script type="module">import { pnsFlashCard } from "./js/pages/pns.js";
document.getElementById("page").append(pnsFlashCard());</script></body></html>`;

const good = { time: 1, source: "read", sha256: "", info: { size: 1, boot: "lintel ::::::: ( unified-boot/x )(E8C2)", board: "E8C2", magic: true, empty: false, dtb: { valid: true, compatible: "mcst,e8c2" } } };
const chip = (o) => ({ ...good, info: { ...good.info, ...o } });
const EMPTY = chip({ boot: "", board: "", empty: true, magic: false, dtb: { valid: false, error: "нет" } });
const GARBAGE = chip({ boot: "", board: "", magic: false });
const STRING_NO_MAGIC = chip({ magic: false });
const OLD_NO_FIELD = (() => { const c = chip({}); delete c.info.magic; return c; })();
const OLD_NO_FIELD_NO_BOOT = (() => { const c = chip({ boot: "" }); delete c.info.magic; return c; })();
const UPLOAD = { name: "upload", size: 4096, time: 1 };

const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });

async function open({ chip: ch, upload = UPLOAD, power = "Off", checkOk = true, job = null, answer = true, image = null, warnings = [] }) {
  const ctx = await browser.newContext({ locale: "ru-RU" });
  const page = await ctx.newPage();
  const s = { page, runs: [], dialogs: [], errors: [] };
  page.on("pageerror", (e) => s.errors.push(e.message));
  page.on("dialog", async (d) => { s.dialogs.push(d.message()); if (answer) await d.accept(); else await d.dismiss(); });
  await page.route(`${ORIGIN}/**`, async (route) => {
    const req = route.request();
    const p = new URL(req.url()).pathname;
    if (p === "/harness.html") return route.fulfill({ contentType: "text/html", body: HARNESS(power) });
    if (p === "/js/app.js") return route.fulfill({ contentType: "text/javascript", body: `export const appState = { power: ${JSON.stringify(power)} };` });
    if (p === PF) return route.fulfill({ contentType: "application/json", body: JSON.stringify({ data: { Json: JSON.stringify({ version: 1, now: 2, tool: true, state: job, chip: ch, upload, kept: null, backups: [] }) } }) });
    if (p === `${PF}/action/Run`) {
      const [action, arg] = req.postDataJSON().data;
      s.runs.push([action, arg]);
      const body = action === "check" ? { ok: checkOk, message: "x", check: { ok: checkOk, errors: [], warnings, image: image || { boot: "b", dtb: {} }, covers: [0, 4096] } } : { ok: true, message: "запущено" };
      return route.fulfill({ contentType: "application/json", body: JSON.stringify({ data: JSON.stringify(body) }) });
    }
    const f = path.join(WWW, p);
    if (!fs.existsSync(f) || fs.statSync(f).isDirectory()) return route.fulfill({ status: 404, body: "" });
    return route.fulfill({ contentType: path.extname(f) === ".js" ? "text/javascript" : "application/octet-stream", body: fs.readFileSync(f) });
  });
  await page.goto(`${ORIGIN}/harness.html`);
  await page.waitForSelector("#pns-flash h3");
  await page.waitForFunction(() => !document.querySelector("#pns-flash").textContent.includes("Загрузка…"));
  return s;
}

// the image checked: "годен", the buttons enabled as for a plain write
async function checked(s) {
  await s.page.getByRole("button", { name: "Проверить", exact: true }).click();
  await s.page.waitForFunction(() => /годен/.test(document.querySelector("#pns-flash").textContent));
}
const btn = (s) => s.page.locator("button", { hasText: BTN });

for (const [what, ch, shown] of [
  ["a chip with an image", good, false],
  ["never read (no chip.json)", null, false],
  ["an empty chip", EMPTY, true],
  ["no magic, no version", GARBAGE, true],
  ["a version string but no magic", STRING_NO_MAGIC, true],
  ["chip.json of an older firmware, a version", OLD_NO_FIELD, false],
  ["chip.json of an older firmware, no version", OLD_NO_FIELD_NO_BOOT, true],
]) {
  const s = await open({ chip: ch });
  await checked(s);
  check((await btn(s).count()) === (shown ? 1 : 0), `${what}: the button ${shown ? "shown" : "not shown"}`);
  check((await s.page.locator("button", { hasText: "Записать во флеш ПНС" }).count()) === 1, `${what}: the plain write stays`);
  check(!s.errors.length, `${what}: no page errors ${s.errors}`);
  await s.page.context().close();
}

// no upload: nothing to write
{
  const s = await open({ chip: EMPTY, upload: null });
  check((await btn(s).count()) === 0, "no upload: no button");
  await s.page.context().close();
}

// asked, then sent
{
  const s = await open({ chip: EMPTY });
  await checked(s);
  await btn(s).click();
  await s.page.waitForTimeout(200);
  check(s.dialogs.length === 1 && /нет образа|пуст/.test(s.dialogs[0]) && /плат/i.test(s.dialogs[0]), `a confirmation that says what it does: ${s.dialogs[0]}`);
  check(JSON.stringify(s.runs.at(-1)) === '["write-blank","upload"]', `Run write-blank upload: ${JSON.stringify(s.runs)}`);
  await s.page.context().close();
}
{
  const s = await open({ chip: EMPTY, answer: false });
  await checked(s);
  await btn(s).click();
  await s.page.waitForTimeout(200);
  check(s.dialogs.length === 1 && !s.runs.some((r) => r[0] === "write-blank"), "the confirmation refused: nothing sent");
  await s.page.context().close();
}

// disabled as the plain write is
for (const [what, o] of [
  ["host on", { power: "On" }],
  ["the image not good", { checkOk: false }],
  ["a job running", { job: { job: "read", state: "running", progress: 1, pid: 1, message: "", started: 1 } }],
]) {
  const s = await open({ chip: EMPTY, ...o });
  if (!o.job) await checked(s);
  check((await btn(s).count()) === 0 || (await btn(s).isDisabled()), `${what}: not enabled`);
  await s.page.context().close();
}

// coreboot (the unofficial e2k port) on the chip and as the image: shown as
// such, no PNS rows it hasn't, a chip with it is a chip with an image
const CB_BOOT = "coreboot 26.09-607-g367eda1a7d50 (Tue Nov 10 16:39:14 UTC 2026)";
const CB_INFO = { kind: "coreboot", boot: CB_BOOT, board: "", cpu: "", vbios: "", part: "1E8SV-uATX", magic: true, empty: false,
                  dtb: { valid: true, version: "2.0_2026-09-15", model: "1E8SV-uATX TVGI.469555.445", compatible: "mcst,e8c2_1e8sv-uatx_445" } };
{
  const s = await open({ chip: chip(CB_INFO), image: CB_INFO, warnings: ["Неофициальная прошивка coreboot (не МЦСТ): " + CB_BOOT] });
  const rows = () => s.page.evaluate(() => [...document.querySelectorAll("#pns-flash dl.kv dt")].map((e) => e.textContent));
  const t = await s.page.evaluate(() => document.querySelector("#pns-flash").textContent);
  const r = await rows();
  check(t.includes(CB_BOOT) && r.includes("Загрузчик") && !r.includes("Версия ПНС") && !r.includes("VBIOS") && !r.includes("Плата (сборка)"),
        `a coreboot chip: "Загрузчик" coreboot, no PNS rows (${r.join(" | ")})`);
  await checked(s);
  const t2 = await s.page.evaluate(() => document.querySelector("#pns-flash").textContent);
  check(/Неофициальная прошивка coreboot/.test(t2) && (await rows()).filter((x) => x === "Загрузчик").length === 2,
        "a coreboot image checked: its loader row, the warning shown");
  check((await btn(s).count()) === 0, "a coreboot chip: no write-onto-a-chip-without-an-image button");
  await s.page.locator("button", { hasText: "Записать во флеш ПНС" }).click();
  await s.page.waitForTimeout(200);
  check(s.dialogs.length === 1 && s.dialogs[0].includes(CB_BOOT) && /неофициальн/i.test(s.dialogs[0]),
        `the confirmation names coreboot, unofficial: ${s.dialogs[0].slice(0, 160)}`);
  check(!s.errors.length, `coreboot: no page errors ${s.errors}`);
  await s.page.context().close();
}

// a file chosen stays chosen: app.js says "reimu-power" every 5 s (the power
// polled, changed or not), and the card re-rendered on it made a new, empty
// file input - the choice was gone before "Загрузить и проверить"
{
  const s = await open({ chip: good, upload: null });
  const input = s.page.locator('#pns-flash input[type="file"]');
  await input.setInputFiles({ name: "coreboot.rom", mimeType: "application/octet-stream", buffer: Buffer.alloc(4096, 1) });
  for (let i = 0; i < 3; i++) {
    await s.page.evaluate(() => document.dispatchEvent(new CustomEvent("reimu-power", { detail: { PowerState: "Off" } })));
    await s.page.waitForTimeout(100);
  }
  const kept = await s.page.evaluate(() => { const f = document.querySelector('#pns-flash input[type="file"]'); return f && f.files.length ? f.files[0].name : null; });
  const send = s.page.locator("button", { hasText: "Загрузить и проверить" });
  check(kept === "coreboot.rom" && !(await send.isDisabled()), `the power polled 3 times: the chosen file kept, upload enabled (${kept})`);
  // and over the card's own refresh (its 15 s poll, here: a power change re-renders)
  await s.page.evaluate(() => document.dispatchEvent(new CustomEvent("reimu-power", { detail: { PowerState: "On" } })));
  await s.page.waitForTimeout(100);
  const kept2 = await s.page.evaluate(() => { const f = document.querySelector('#pns-flash input[type="file"]'); return f && f.files.length ? f.files[0].name : null; });
  check(kept2 === "coreboot.rom", `re-rendered (the power changed): the chosen file still there (${kept2})`);
  check(!s.errors.length, `the file kept: no page errors ${s.errors}`);
  await s.page.context().close();
}

await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 10000))]);
console.log("RESULT:", failed ? `FAILED ${failed}` : "ALL PASSED");
process.exit(failed ? 1 : 0);
