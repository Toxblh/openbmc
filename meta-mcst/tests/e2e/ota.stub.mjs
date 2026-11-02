// E2E without a BMC: the update from the releases on the firmware card
// (js/pages/firmware.js, js/lib/ota.js, the real files), REST answers stubbed.
//
//     node ota.stub.mjs       # WWW: the web tree, CHROME: chromium
import { chromium } from "playwright";
import fs from "node:fs";
import path from "node:path";

const HERE = path.dirname(new URL(import.meta.url).pathname);
const WWW = process.env.WWW || path.join(HERE, "../../meta-common/recipes-phosphor/reimu-webui/files/www");
const ORIGIN = "http://reimu.test";
const AB = "/xyz/openbmc_project/reimu/ab";
const FW = "/xyz/openbmc_project/reimu/firmware";

let failed = 0;
const check = (c, what) => { console.log(`${c ? "PASS" : "FAIL"} ${what}`); if (!c) failed++; };

const { compareVersions: cmp, newerReleases, changesOf, parseVersion } = await import(path.join(WWW, "js/lib/ota.js"));
const order = ["reimu-1.0.2", "reimu-1.0.2-68-ga54910e1d0", "reimu-2.0-alpha", "reimu-2.0-beta", "reimu-2.0-beta-3-gabc1234", "reimu-2.0-beta2", "reimu-2.0-rc1", "reimu-2.0", "reimu-2.0.1", "reimu-2.1", "reimu-10.0"];
let sorted = true;
for (let i = 0; i < order.length; i++) for (let j = 0; j < order.length; j++) if (Math.sign(cmp(order[i], order[j])) !== Math.sign(i - j)) { sorted = false; console.log(`  ${order[i]} ? ${order[j]}: ${cmp(order[i], order[j])}`); }
check(sorted, "the versions in order");
check(parseVersion("reimu-webdev-20261019") === null, "the web devkit's tag isn't a version");
check(parseVersion("reimu-2.0-beta-dirty").pre.name === "beta", "-dirty isn't a pre-release name");
check(cmp("reimu-1.0.2-dirty", "reimu-1.0.2") === 0 && !parseVersion("reimu-1.0.2-dirty").pre, "a dirty build: the same version");
check(changesOf("- a\r\n- b\r\n\r\n<!-- ota-end -->\r\nустановка") === "- a\n- b", "the changes stop at the marker");

const rel = (tag, prerelease, body, files) => ({ tag_name: tag, prerelease, draft: false, body, published_at: "2026-11-02T00:00:00Z", assets: (files || [`${tag}.tar`, `${tag}.tar.sha256`]).map((name) => ({ name })) });
const LIST = [
  rel("reimu-2.1", false, "- Карта платы\n\n<!-- ota-end -->\nСЕКРЕТНАЯ ИНСТРУКЦИЯ"),
  rel("reimu-2.0-beta2", true, "- <img src=x onerror=window.xss=1>Правка"),
  rel("reimu-2.0-beta", true, "- Старое, уже стоит"),
  rel("reimu-2.0-rc9", true, "- Без файла обновления", ["reimu-ab-reimu-2.0-rc9.tar.gz"]),
  rel("reimu-webdev-20261019", true, "- Не прошивка"),
  { ...rel("reimu-3.0", false, "- Черновик"), draft: true },
];
check(newerReleases(LIST, "reimu-2.0-beta", true).map((r) => r.tag_name).join() === "reimu-2.1,reimu-2.0-beta2", "newer ones, newest first, only with the files, no drafts");
check(newerReleases(LIST, "reimu-2.0-beta", false).map((r) => r.tag_name).join() === "reimu-2.1", "without pre-releases");

const HARNESS = `<!doctype html><html><head><meta charset="utf-8"></head><body><div id="toasts"></div><main id="page"></main>
<script type="module">import { firmwareCard } from "./js/pages/firmware.js";
document.getElementById("page").append(firmwareCard({}, () => {}));</script></body></html>`;

const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });

async function open(running, { answer = true, otaStart = null, checkReply = null } = {}) {
  const ctx = await browser.newContext({ locale: "ru-RU" });
  const page = await ctx.newPage();
  const s = { page, calls: [], dialogs: [], errors: [], ota: otaStart, list: LIST, job: null };
  page.on("pageerror", (e) => s.errors.push(e.message));
  page.on("dialog", async (d) => { s.dialogs.push(d.message()); if (answer) await d.accept(); else await d.dismiss(); });
  await page.route(`${ORIGIN}/**`, async (route) => {
    const req = route.request();
    const p = new URL(req.url()).pathname;
    const json = (o) => route.fulfill({ contentType: "application/json", body: JSON.stringify(o) });
    if (p === "/harness.html") return route.fulfill({ contentType: "text/html", body: HARNESS });
    if (p === FW) return route.fulfill({ status: 404, contentType: "application/json", body: "{}" });
    if (p === AB) return json({ data: { Json: JSON.stringify({ version: 1, running, booted: "kernel=b rofs=b state=ok", next: "b", pending: false, slots: { a: "reimu-1.0.2", b: running }, job: s.job, upload: null, ota: s.ota }), OtaReleases: JSON.stringify(s.list) } });
    if (p === `${AB}/action/OtaCheck`) { s.calls.push(["check"]); s.ota = checkReply || { state: "checked", checked: 100, progress: 0, tag: "", message: "" }; return json({ data: JSON.stringify({ ok: true, message: "ok" }) }); }
    if (p === `${AB}/action/OtaInstall`) { s.calls.push(["install", ...req.postDataJSON().data]); s.ota = { state: "downloading", checked: 100, progress: 40, tag: req.postDataJSON().data[0], message: "" }; return json({ data: JSON.stringify({ ok: true, message: "ok" }) }); }
    const f = path.join(WWW, p);
    if (fs.existsSync(f) && fs.statSync(f).isFile()) return route.fulfill({ contentType: p.endsWith(".js") ? "text/javascript" : "text/plain", body: fs.readFileSync(f) });
    return route.fulfill({ status: 404, body: "" });
  });
  await page.goto(`${ORIGIN}/harness.html`);
  await page.getByRole("button", { name: "Проверить обновления" }).waitFor();
  return s;
}

{
  const s = await open("reimu-2.0-beta");
  const none = await s.page.getByRole("button", { name: /Скачать и установить/ }).count();
  check(none === 0, "nothing offered before a check");
  await s.page.getByRole("button", { name: "Проверить обновления" }).click();
  const go = s.page.getByRole("button", { name: "Скачать и установить reimu-2.1" });
  await go.waitFor();
  const text = await s.page.locator("#fw-card").innerText();
  check(s.calls[0][0] === "check", "the BMC is asked, not GitHub by the browser");
  check(text.includes("Карта платы") && text.includes("reimu-2.0-beta2"), "the changes from the current version: both newer releases");
  check(!text.includes("Старое, уже стоит") && !text.includes("Без файла") && !text.includes("Не прошивка"), "not the current one, not without files, not a devkit");
  check(!text.includes("СЕКРЕТНАЯ"), "the install notes below the marker are not shown");
  check(text.includes("<img src=x") && !(await s.page.evaluate(() => window.xss)), "the release text stays text, its HTML doesn't run");
  await go.click();
  await s.page.getByText("Скачивание reimu-2.1").waitFor();
  check(s.calls.some((c) => c[0] === "install" && c[1] === "reimu-2.1"), "install asked for the newest");
  check(s.dialogs[0] && s.dialogs[0].includes("сбросит хост") && s.dialogs[0].includes("отката"), "the question says about the host and the way back");
  check(await s.page.getByRole("button", { name: "Проверить обновления" }).isDisabled(), "no second start during a download");
  check(!s.errors.length, `no page errors ${s.errors}`);
}
{
  const s = await open("reimu-2.0-beta", { answer: false });
  await s.page.getByRole("button", { name: "Проверить обновления" }).click();
  await s.page.getByRole("button", { name: /Скачать и установить/ }).click();
  await s.page.waitForTimeout(400);
  check(!s.calls.some((c) => c[0] === "install"), "refused in the question: nothing asked");
}
{
  const s = await open("reimu-2.1");
  await s.page.getByRole("button", { name: "Проверить обновления" }).click();
  await s.page.getByText("Установлена последняя версия").waitFor();
  check(await s.page.getByRole("button", { name: /Скачать и установить/ }).count() === 0, "the newest running: nothing offered");
}
{
  const s = await open("reimu-2.0");
  await s.page.getByRole("button", { name: "Проверить обновления" }).click();
  await s.page.getByRole("button", { name: "Скачать и установить reimu-2.1" }).waitFor();
  check(!(await s.page.locator("#fw-card").innerText()).includes("reimu-2.0-beta2"), "on a stable version pre-releases are hidden");
  await s.page.getByLabel("Показывать бета-версии").check();
  check(true, "the choice is there");
}
{
  const s = await open("reimu-2.0-beta", { checkReply: { state: "failed", checked: 0, progress: 0, tag: "", message: "Проверка не удалась: нет соединения" } });
  await s.page.getByRole("button", { name: "Проверить обновления" }).click();
  await s.page.getByText("Проверка не удалась: нет соединения").waitFor();
  check(true, "a failed check says why");
}
await browser.close();
console.log(`${failed} failed`);
process.exit(failed ? 1 : 0);
