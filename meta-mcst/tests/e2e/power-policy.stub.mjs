// E2E without a BMC: what the host does when the board gets power again
// (the power restore policy) on the settings page (js/pages/settings.js,
// the real files), REST answers stubbed. Read from the D-Bus REST object
// (Redfish's Systems/system comes from the view, which hasn't it), written
// through Redfish.
//
//     node power-policy.stub.mjs       # WWW: the web tree, CHROME: chromium
import { chromium } from "playwright";
import fs from "node:fs";
import path from "node:path";

const HERE = path.dirname(new URL(import.meta.url).pathname);
const WWW = process.env.WWW || path.join(HERE, "../../meta-common/recipes-phosphor/reimu-webui/files/www");
const ORIGIN = "http://reimu.test";
const POLICY = "/xyz/openbmc_project/control/host0/power_restore_policy";
const SYSTEM = "/redfish/v1/Systems/system";
const IFACE = "xyz.openbmc_project.Control.Power.RestorePolicy.Policy.";

let failed = 0;
const check = (c, what) => { console.log(`${c ? "PASS" : "FAIL"} ${what}`); if (!c) failed++; };

const HARNESS = `<!doctype html><html><head><meta charset="utf-8"></head><body><div id="toasts"></div><main id="page"></main>
<script type="module">import { powerPolicyCard } from "./js/pages/settings.js";
document.getElementById("page").append(powerPolicyCard());</script></body></html>`;

const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });

async function open({ policy = "AlwaysOff", readFails = false, patchFails = false } = {}) {
  const ctx = await browser.newContext({ locale: "ru-RU" });
  const page = await ctx.newPage();
  const s = { page, patches: [], errors: [], policy };
  page.on("pageerror", (e) => s.errors.push(e.message));
  page.on("dialog", (d) => d.accept());
  await page.route(`${ORIGIN}/**`, async (route) => {
    const req = route.request();
    const p = new URL(req.url()).pathname;
    const json = (o, status = 200) => route.fulfill({ status, contentType: "application/json", body: JSON.stringify(o) });
    if (p === "/harness.html") return route.fulfill({ contentType: "text/html", body: HARNESS });
    if (p === POLICY && req.method() === "GET") {
      if (readFails) return json({ status: "error", message: "404 Not Found" }, 404);
      return json({ data: { PowerRestorePolicy: IFACE + s.policy }, message: "200 OK", status: "ok" });
    }
    if (p === SYSTEM && req.method() === "PATCH") {
      const body = req.postDataJSON();
      s.patches.push(body);
      if (patchFails) return json({ error: { message: "Internal Server Error" } }, 500);
      const back = { AlwaysOn: "AlwaysOn", AlwaysOff: "AlwaysOff", LastState: "Restore" }[body.PowerRestorePolicy];
      if (back) s.policy = back;
      return route.fulfill({ status: 204, body: "" });
    }
    const f = path.join(WWW, p);
    if (fs.existsSync(f) && fs.statSync(f).isFile()) return route.fulfill({ contentType: p.endsWith(".js") ? "text/javascript" : "text/plain", body: fs.readFileSync(f) });
    return route.fulfill({ status: 404, body: "" });
  });
  await page.goto(`${ORIGIN}/harness.html`);
  await page.locator("#power-policy").waitFor();
  await page.waitForFunction(() => !document.querySelector("#power-policy").textContent.includes("Загрузка"));
  return s;
}
const sel = (s) => s.page.getByLabel("После подачи питания");

{
  const s = await open({ policy: "AlwaysOff" });
  const text = await s.page.locator("#power-policy").innerText();
  check(text.includes("Питание хоста"), "the card is there");
  check((await sel(s).inputValue()) === "AlwaysOff", "the current policy shown (AlwaysOff)");
  const opts = await sel(s).locator("option").evaluateAll((o) => o.map((x) => x.value));
  check(opts.join() === "LastState,AlwaysOn,AlwaysOff", `the three choices (${opts})`);
  await sel(s).selectOption("LastState");
  await s.page.getByRole("button", { name: "Сохранить" }).click();
  await s.page.waitForFunction(() => document.querySelector("#toasts").textContent.length > 0);
  check(s.patches.length === 1 && s.patches[0].PowerRestorePolicy === "LastState" && Object.keys(s.patches[0]).length === 1, `PATCH Systems/system with LastState only (${JSON.stringify(s.patches)})`);
  check(s.policy === "Restore", "the BMC has it");
  check(!s.errors.length, `no page errors ${s.errors}`);
}
{
  const s = await open({ policy: "Restore" });
  check((await sel(s).inputValue()) === "LastState", "D-Bus Restore shown as the last state");
  const s2 = await open({ policy: "AlwaysOn" });
  check((await sel(s2).inputValue()) === "AlwaysOn", "AlwaysOn shown");
}
{
  const s = await open({ readFails: true });
  const text = await s.page.locator("#power-policy").innerText();
  check(text.includes("не удалось прочитать"), "a failed read is said");
  check(await s.page.getByRole("button", { name: "Сохранить" }).isDisabled(), "and nothing to save then");
}
{
  const s = await open({ patchFails: true });
  await sel(s).selectOption("AlwaysOn");
  await s.page.getByRole("button", { name: "Сохранить" }).click();
  await s.page.waitForFunction(() => document.querySelector("#toasts").textContent.length > 0);
  const t = await s.page.locator("#toasts").innerText();
  check(t.includes("Ошибка"), `a failed save is said (${t.trim()})`);
}
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 10000))]);
console.log(`${failed} failed`);
process.exit(failed ? 1 : 0);
