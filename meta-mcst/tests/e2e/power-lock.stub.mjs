// E2E without a BMC: the whole web UI (index.html, js/app.js, the real files),
// the REST answers stubbed. While a PNS flash job runs, the BMC holds the
// platform's power-on (the lock server_ctl honours): the power menu of every
// page says so and its power-on items are off; the PNS card doesn't repeat it
// under its progress bar.
//
//     node power-lock.stub.mjs       # WWW: the web tree, CHROME: chromium
import { chromium } from "playwright";
import fs from "node:fs";
import path from "node:path";

const HERE = path.dirname(new URL(import.meta.url).pathname);
const WWW = process.env.WWW || path.join(HERE, "../../meta-common/recipes-phosphor/reimu-webui/files/www");
const ORIGIN = "http://reimu.test";
const MIME = { ".js": "text/javascript", ".css": "text/css", ".html": "text/html", ".svg": "image/svg+xml", ".woff2": "font/woff2" };
let failed = 0;
const check = (c, what) => { console.log(`${c ? "PASS" : "FAIL"} ${what}`); if (!c) failed++; };

let job = { job: "write", state: "running", phase: "write", progress: 40, pid: 1, message: "Запись флеша ПНС", started: 1794421687 };
const pns = () => ({ version: 1, now: 2, tool: true, state: job, chip: null, upload: null, kept: null, backups: [] });

const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });
const ctx = await browser.newContext({ locale: "ru-RU", viewport: { width: 1280, height: 900 } });
await ctx.addCookies([{ name: "XSRF-TOKEN", value: "t", url: ORIGIN }]);
const posts = [];
await ctx.route(`${ORIGIN}/**`, (route) => {
  const req = route.request();
  const p = new URL(req.url()).pathname;
  const json = (o) => route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify(o) });
  if (p === "/redfish/v1/SessionService/Sessions") return json({ Members: [] });
  if (p === "/redfish/v1/Systems/system") return json({ PowerState: "Off" });
  if (p === "/xyz/openbmc_project/reimu/pns_flash") return json({ data: { Json: JSON.stringify(pns()) } });
  if (p === "/xyz/openbmc_project/reimu/pns") return json({ data: { Json: JSON.stringify({ fruXml: "", scan: null }) } });
  if (p.startsWith("/redfish/v1/Systems/system/Actions")) { posts.push(p); return json({}); }
  const f = path.join(WWW, p === "/" ? "index.html" : p);
  if (!f.startsWith(WWW) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) return route.fulfill({ status: 404, contentType: "application/json", body: "{}" });
  route.fulfill({ status: 200, contentType: MIME[path.extname(f)] || "application/octet-stream", body: fs.readFileSync(f) });
});
const page = await ctx.newPage();
const errors = [];
page.on("pageerror", (e) => errors.push(e.message));
page.on("dialog", (d) => d.dismiss());
await page.goto(ORIGIN + "/#/");
await page.locator("#app:not(.hidden)").waitFor({ timeout: 15000 });
const items = () => page.evaluate(() => Object.fromEntries([...document.querySelectorAll("#power-menu button[data-reset]")].map((b) => [b.dataset.reset, b.disabled])));
const note = () => page.evaluate(() => { const n = document.getElementById("power-lock"); return n && !n.hidden ? n.textContent : ""; });

await page.waitForFunction(() => { const b = document.querySelector('#power-menu button[data-reset="On"]'); return b && b.disabled; }, null, { timeout: 8000 }).catch(() => {});
let it = await items();
check(it.On === true && it.GracefulRestart === true && it.ForceRestart === true, `a PNS job running: power on, restart, reset off (${JSON.stringify(it)})`);
check(it.GracefulShutdown === false && it.ForceOff === false, "power off stays available");
check(/флеш[а]? ПНС/.test(await note()) && /включ/.test(await note()), `the menu says why (${await note()})`);
await page.locator("#power-menu-btn").click();
await page.locator('#power-menu button[data-reset="On"]').click({ force: true }).catch(() => {});
await page.waitForTimeout(300);
check(!posts.length, `nothing sent for the locked item (${posts})`);

// the card: its progress without the power words, the backup note not an alert
await page.evaluate(() => { location.hash = "#/hardware"; });
await page.waitForSelector("#pns-flash .job", { timeout: 15000 }).catch(() => {});
const card = await page.evaluate(() => { const c = document.getElementById("pns-flash"); return c ? c.textContent : ""; });
check(card && !/включение платформы заблокировано/.test(card) && /40\s?%/.test(card), `the PNS card: progress without the power words (${card.slice(0, 160)})`);

// the job over: the items come back with the next poll
job = { ...job, state: "ok", progress: 100, finished: 1794421999 };
await page.waitForFunction(() => { const b = document.querySelector('#power-menu button[data-reset="On"]'); return b && !b.disabled; }, null, { timeout: 8000 }).catch(() => {});
it = await items();
check(it.On === false && it.GracefulRestart === false && !(await note()), `the job over: power on again (${JSON.stringify(it)})`);
check(!errors.length, `no page errors (${errors})`);

await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 10000))]);
console.log(failed ? `${failed} FAILED` : "all passed");
process.exit(failed ? 1 : 0);
