// E2E without a BMC: the login page (index.html and js/app.js, the real files),
// every API call answered 401 (no session).
//
//     node login.stub.mjs       # WWW: the web tree, CHROME: chromium
//
// No link to the old web UI (it is not in the firmware). On a phone the page
// does not put the focus in the password field itself: a focus nobody tapped
// for makes password managers (their autofill over the field) close the
// keyboard while the password is typed. On a computer it does, as before.
import { chromium, devices } from "playwright";
import fs from "node:fs";
import path from "node:path";

const HERE = path.dirname(new URL(import.meta.url).pathname);
const WWW = process.env.WWW || path.join(HERE, "../../meta-common/recipes-phosphor/reimu-webui/files/www");
const ORIGIN = "http://reimu.test";
const MIME = { ".js": "text/javascript", ".css": "text/css", ".html": "text/html", ".svg": "image/svg+xml", ".woff2": "font/woff2" };

let failed = 0;
const check = (c, what) => { console.log(`${c ? "PASS" : "FAIL"} ${what}`); if (!c) failed++; };

const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });
async function open(opts) {
  const ctx = await browser.newContext({ locale: "ru-RU", ...opts });
  await ctx.route(`${ORIGIN}/**`, (route) => {
    const p = new URL(route.request().url()).pathname;
    const f = path.join(WWW, p === "/" ? "index.html" : p);
    if (/^\/(redfish|xyz|login|logout)/.test(p) || !f.startsWith(WWW) || !fs.existsSync(f))
      return route.fulfill({ status: 401, contentType: "application/json", body: "{}" });
    route.fulfill({ status: 200, contentType: MIME[path.extname(f)] || "application/octet-stream", body: fs.readFileSync(f) });
  });
  const page = await ctx.newPage();
  await page.goto(ORIGIN + "/");
  await page.locator("#login-pass").waitFor({ state: "visible", timeout: 15000 });
  await page.waitForTimeout(500);
  return { ctx, page, active: await page.evaluate(() => document.activeElement && document.activeElement.id) };
}

// the old web UI: not linked from anywhere
const html = fs.readFileSync(path.join(WWW, "index.html"), "utf8");
check(!/legacy/i.test(html) && !/Старый интерфейс/.test(html), "no link to the old web UI in index.html");

const phone = await open(devices["Pixel 7"]);
check(phone.active !== "login-pass", `phone: the password field not focused by the page (active: ${phone.active})`);
await phone.page.locator("#login-pass").tap();
await phone.page.keyboard.type("0penBmc");
check((await phone.page.locator("#login-pass").inputValue()) === "0penBmc", "phone: tapped and typed");
await phone.ctx.close();

const pc = await open({ viewport: { width: 1400, height: 1000 } });
check(pc.active === "login-pass", `computer: the password field focused (active: ${pc.active})`);
await pc.ctx.close();

// (closing hangs at times on a loaded machine)
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 10000))]);
console.log(failed ? `${failed} FAILED` : "all passed");
process.exit(failed ? 1 : 0);
