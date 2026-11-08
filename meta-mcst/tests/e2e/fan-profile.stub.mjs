// E2E without a BMC: the CPU cooling profile on the fans card
// (js/pages/fans.js, the real files), reimu-fand's D-Bus stubbed: quiet,
// normal (the default curve), holding a band (SetTarget), custom.
//
//     node fan-profile.stub.mjs       # WWW: the web tree, CHROME: chromium
import { chromium } from "playwright";
import fs from "node:fs";
import path from "node:path";

const HERE = path.dirname(new URL(import.meta.url).pathname);
const WWW = process.env.WWW || path.join(HERE, "../../meta-common/recipes-phosphor/reimu-webui/files/www");
const ORIGIN = "http://reimu.test";
const FANS = "/xyz/openbmc_project/reimu/fans";
const DEFAULT = [[45, 30], [55, 38], [60, 47], [65, 60], [70, 80], [75, 100]];
const QUIET = [[50, 30], [60, 38], [65, 50], [70, 75], [75, 100]];

let failed = 0;
const check = (c, what) => { console.log(`${c ? "PASS" : "FAIL"} ${what}`); if (!c) failed++; };

const HARNESS = `<!doctype html><html><head><meta charset="utf-8"></head><body><div id="toasts"></div><main id="page"></main>
<script type="module">import { renderFans, fanState } from "./js/pages/fans.js";
const el = document.getElementById("page");
const refresh = async () => renderFans(el, await fanState(), refresh);
window.refresh = refresh;
refresh();</script></body></html>`;

const parse = (s) => s.trim().split(/\s+/).map((p) => p.split(":").map(Number));

function state(s) {
  const input = (id, curve, target) => ({ id, key: `CPU_${id.toUpperCase()}_CURVE`, used: true, temp: 50.2, demand: 80, curve, target, target_key: `CPU_${id.toUpperCase()}_TARGET` });
  return {
    version: 1, host: "on", floor: 30, manual_timeout: 600, turbo: false, turbo_left: 0, alarm: false, config_errors: [],
    zones: [
      { id: "cpu", name: "Процессор (X37)", mode: "auto", duty: 80, demand: 80, manual_duty: null, failsafe: false, hot: false, reason: "",
        inputs: [input("pcs", s.curve, s.target), input("vrm", [[80, 30], [115, 100]], null)],
        fans: [{ dev: "6-004c", chip: "lm96163", ok: true, duty: 56, rpm: 2400, range: [35, 65] }] },
      { id: "board", name: "Плата", mode: "auto", duty: 31, demand: 31, manual_duty: null, failsafe: false, hot: false, reason: "",
        inputs: [], fans: [] },
    ],
  };
}

const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });

async function open(init) {
  const ctx = await browser.newContext({ locale: "ru-RU" });
  const page = await ctx.newPage();
  const s = { page, calls: [], errors: [], curve: init.curve, target: init.target ?? null };
  page.on("pageerror", (e) => s.errors.push(e.message));
  page.on("dialog", (d) => d.accept());
  await page.route(`${ORIGIN}/**`, async (route) => {
    const req = route.request();
    const p = new URL(req.url()).pathname;
    const json = (o, status = 200) => route.fulfill({ status, contentType: "application/json", body: JSON.stringify(o) });
    if (p === "/harness.html") return route.fulfill({ contentType: "text/html", body: HARNESS });
    if (p === FANS) return json({ data: { Json: JSON.stringify(state(s)) } });
    const m = /^\/xyz\/openbmc_project\/reimu\/fans\/action\/(\w+)$/.exec(p);
    if (m) {
      const args = req.postDataJSON().data;
      s.calls.push([m[1], ...args]);
      if (m[1] === "SetCurve" && args[0] === "cpu" && args[1] === "pcs") {
        const c = parse(args[2]);
        if (s.target && s.target[1] >= c[c.length - 1][0]) return json({ message: "full speed not above the target" }, 400);
        s.curve = c;
      }
      if (m[1] === "SetTarget" && args[0] === "cpu" && args[1] === "pcs") {
        if (args[2] === "none") s.target = null;
        else {
          const [lo, hi] = args[2].split("-").map(Number);
          if (!(hi >= lo) || hi - lo > 15 || lo < 30) return json({ message: `${args[2]}: from 30 °C, at most 15 °C wide` }, 400);
          s.target = [lo, hi];
        }
      }
      return json({ data: "ok" });
    }
    if (p.startsWith("/xyz/openbmc_project/reimu/view")) return route.fulfill({ status: 404, body: "" });
    const f = path.join(WWW, p);
    if (fs.existsSync(f) && fs.statSync(f).isFile()) return route.fulfill({ contentType: p.endsWith(".js") ? "text/javascript" : "text/plain", body: fs.readFileSync(f) });
    return route.fulfill({ status: 404, body: "" });
  });
  await page.goto(`${ORIGIN}/harness.html`);
  await page.locator("#cpu-profile").waitFor();
  return s;
}
const active = async (s) => (await s.page.locator("#cpu-profile button.active").allInnerTexts()).join();
const settle = (s) => s.page.waitForTimeout(400);

{
  const s = await open({ curve: DEFAULT });
  check((await active(s)) === "Обычный", `the default curve: Обычный (${await active(s)})`);
  await s.page.getByRole("button", { name: "Удержание" }).click();
  await settle(s);
  check(s.calls.some((c) => c[0] === "SetTarget" && c[1] === "cpu" && c[2] === "pcs" && c[3] === "48-52"), `holding: SetTarget cpu pcs 48-52 (${JSON.stringify(s.calls)})`);
  check((await active(s)) === "Удержание", "then shown as holding");
  const text = await s.page.locator("#cpu-profile").innerText();
  check(text.includes("48") && text.includes("52"), "the band shown");
  check((await s.page.locator(".section").first().innerText()).includes("держит 48–52"), "the input line says it holds 48–52");

  // another band
  s.calls = [];
  await s.page.getByLabel("от", { exact: true }).fill("45");
  await s.page.getByLabel("до", { exact: true }).fill("50");
  await s.page.getByRole("button", { name: "Применить" }).click();
  await settle(s);
  check(s.calls.some((c) => c[0] === "SetTarget" && c[3] === "45-50"), `another band: 45-50 (${JSON.stringify(s.calls)})`);
  // a bad band: the BMC's message
  await s.page.getByLabel("от", { exact: true }).fill("40");
  await s.page.getByLabel("до", { exact: true }).fill("60");
  await s.page.getByRole("button", { name: "Применить" }).click();
  await settle(s);
  check((await s.page.locator("#toasts").innerText()).includes("at most 15"), "a refused band: the BMC's reason");

  // quiet: the target off first, then the curve
  s.calls = [];
  await s.page.getByRole("button", { name: "Тихий" }).click();
  await settle(s);
  const iT = s.calls.findIndex((c) => c[0] === "SetTarget" && c[3] === "none");
  const iC = s.calls.findIndex((c) => c[0] === "SetCurve" && c[2] === "pcs");
  check(iT >= 0 && iC > iT && s.calls[iC][3] === "50:30 60:38 65:50 70:75 75:100", `quiet: target off, then its curve (${JSON.stringify(s.calls)})`);
  check((await active(s)) === "Тихий", "then shown as quiet");
  // normal
  await s.page.getByRole("button", { name: "Обычный" }).click();
  await settle(s);
  check(JSON.stringify(s.curve) === JSON.stringify(DEFAULT) && s.target === null && (await active(s)) === "Обычный", "normal: the default curve back");
  check(!s.errors.length, `no page errors ${s.errors}`);
}
{
  const s = await open({ curve: [[40, 30], [70, 100]] });
  check((await active(s)) === "Свой", "another curve: Свой");
  const cur = await s.page.getByLabel("Кривая").inputValue();
  check(cur === "40:30 70:100", `its points in the editor (${cur})`);
  await s.page.getByLabel("Кривая").fill("42:30 60:60 72:100");
  await s.page.getByRole("button", { name: "Сохранить кривую" }).click();
  await settle(s);
  check(s.calls.some((c) => c[0] === "SetCurve" && c[1] === "cpu" && c[2] === "pcs" && c[3] === "42:30 60:60 72:100"), "the curve saved");
}
{
  const s = await open({ curve: DEFAULT, target: [48, 52] });
  check((await active(s)) === "Удержание", "a target set: holding, whatever the curve");
}
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 10000))]);
console.log(`${failed} failed`);
process.exit(failed ? 1 : 0);
