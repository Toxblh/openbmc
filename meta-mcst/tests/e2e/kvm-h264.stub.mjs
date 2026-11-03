// E2E without a BMC: the KVM page (js/pages/kvm.js, js/lib/directkvm.js, the
// real files) in a bare harness, reimu-kvm's websocket faked in the page: what
// the page does with the host's H.264 as reimu-kvm reports it (stat.host: the
// address of reimu-h264, from the host agent or its own announcement;
// stat.hoststate: why there is none, the agent's word).
//
//     node kvm-h264.stub.mjs       # WWW: the web tree, CHROME: chromium
//
// The agent sets the capture up itself now (reimu-hostd kvm install): no hint
// to run reimu-host.sh or to type a host address anywhere; "Авто" takes the
// host's stream when there is one and says nothing when there is none for a
// plain reason (no video card); a message only when it can't work: H.264
// chosen and no host (with the agent's reason), or the agent's capture failed.
import { chromium } from "playwright";
import fs from "node:fs";
import path from "node:path";

const HERE = path.dirname(new URL(import.meta.url).pathname);
const WWW = process.env.WWW || path.join(HERE, "../../meta-common/recipes-phosphor/reimu-webui/files/www");
const ORIGIN = "https://reimu.test"; // a secure context: WebCodecs

let failed = 0;
const check = (c, what) => { console.log(`${c ? "PASS" : "FAIL"} ${what}`); if (!c) failed++; };

const HARNESS = `<!doctype html><html><head><meta charset="utf-8"><link rel="stylesheet" href="/css/reimu.css"></head><body>
<div id="toasts"></div><main id="page" style="height:600px"></main>
<script type="module">import { mount } from "./js/pages/kvm.js"; mount(document.getElementById("page"));</script></body></html>`;

// reimu-kvm in the page: a websocket that opens, sends stat every 300 ms
// (window.__stat merged in) and records what the page sends
const FAKE_WS = () => {
  window.__sent = [];
  window.__toasts = [];
  window.__stat = { host: "", hoststate: "" };
  const watch = () => new MutationObserver((ms) => ms.forEach((m) => m.addedNodes.forEach((n) => window.__toasts.push(n.textContent))))
    .observe(document.getElementById("toasts"), { childList: true });
  document.addEventListener("DOMContentLoaded", watch);
  window.WebSocket = class {
    constructor(url) {
      this.url = url;
      this.readyState = 0;
      setTimeout(() => {
        this.readyState = 1;
        this.onopen && this.onopen({});
        this._t = setInterval(() => {
          const m = { t: "stat", cap: 30, same: 0, kbps: 1, sent: 1, skipped: 0, w: 1920, h: 1080, fps: 30, q: 0, sub420: 1, viewers: 1, signal: 1, leds: 0, ...window.__stat };
          this.onmessage && this.onmessage({ data: JSON.stringify(m) });
        }, 300);
      }, 50);
    }
    send(d) {
      const u = d instanceof Uint8Array ? d : new Uint8Array(d.buffer || d);
      window.__sent.push(Array.from(u));
      // the host stream asked for: reimu-kvm's answer
      if (u[0] === 7 && window.__answer) setTimeout(() => this.onmessage({ data: JSON.stringify(window.__answer) }), 50);
    }
    close() {
      clearInterval(this._t);
      this.readyState = 3;
      setTimeout(() => this.onclose && this.onclose({ code: 1000 }), 0);
    }
  };
};

const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });

async function open({ prefs = {}, stat = {}, answer = null } = {}) {
  const ctx = await browser.newContext({ locale: "ru-RU" });
  const page = await ctx.newPage();
  const s = { page, errors: [], prompts: [] };
  page.on("pageerror", (e) => s.errors.push(e.message));
  page.on("dialog", async (d) => { s.prompts.push(d.message()); await d.dismiss(); });
  await page.route(`${ORIGIN}/**`, async (route) => {
    const p = new URL(route.request().url()).pathname;
    if (p === "/harness.html") return route.fulfill({ contentType: "text/html", body: HARNESS });
    if (p === "/js/app.js") return route.fulfill({ contentType: "text/javascript", body: 'export const appState = { power: "On", user: "root" };' });
    if (p === "/xyz/openbmc_project/reimu/kvm/action/Ticket")
      return route.fulfill({ contentType: "application/json", body: JSON.stringify({ data: JSON.stringify({ port: 8443, ticket: "t" }) }) });
    const f = path.join(WWW, p);
    if (!fs.existsSync(f) || fs.statSync(f).isDirectory()) return route.fulfill({ status: 404, body: "" });
    const types = { ".js": "text/javascript", ".css": "text/css" };
    return route.fulfill({ contentType: types[path.extname(f)] || "application/octet-stream", body: fs.readFileSync(f) });
  });
  await page.addInitScript(FAKE_WS);
  await page.addInitScript(({ prefs, stat, answer }) => {
    localStorage.setItem("reimu-kvm", JSON.stringify(prefs));
    window.__initStat = stat;
    window.__answer = answer;
  }, { prefs, stat, answer });
  await page.addInitScript(() => { Object.assign(window.__stat, window.__initStat); });
  await page.goto(`${ORIGIN}/harness.html`);
  await page.waitForFunction(() => document.querySelector("#kvm-state")?.textContent === "подключено", null, { timeout: 15000 });
  return s;
}
const asked = (s) => s.page.evaluate(() => window.__sent.filter((x) => x[0] === 7).map((x) => new TextDecoder().decode(new Uint8Array(x.slice(1)))));
const toasts = (s) => s.page.evaluate(() => window.__toasts.join("\n"));
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

// 1. Авто, the agent's host there: the page takes it by itself
{
  const s = await open({ stat: { host: "10.0.2.2:8555" }, answer: { t: "h264", codec: "avc1.640028", w: 1920, h: 1080, fps: 60 } });
  await wait(1500);
  const a = await asked(s);
  check(a.length === 1 && a[0] === "", `Авто, host there: the host's stream asked for at once (${JSON.stringify(a)})`);
  check(/H\.264 с хоста/.test(await s.page.$eval("#kvm-src", (e) => e.textContent)), "the source shown: H.264 с хоста");
  check(!s.errors.length, `no page errors (${s.errors})`);
  await s.page.context().close();
}
// 2. H.264 chosen, the agent not connected: why, and no reimu-host.sh, no address to type
{
  const s = await open({ prefs: { source: "h264" }, stat: { hoststate: "no_agent" } });
  await wait(5000);
  const t = await toasts(s);
  check(/агент хоста не подключён/.test(t), `H.264, no agent: the page says the agent is not connected (${t.replace(/\n/g, " | ")})`);
  check(!/reimu-host\.sh|sudo/.test(t), "no reimu-host.sh / sudo hint");
  check((await asked(s)).length === 0, "nothing asked for");
  await s.page.context().close();
}
// 3. H.264 chosen, the agent says no video card
{
  const s = await open({ prefs: { source: "h264" }, stat: { hoststate: "no_gpu" } });
  await wait(5000);
  const t = await toasts(s);
  check(/нет видеокарты/.test(t) && !/reimu-host\.sh/.test(t), `H.264, no video card: said so (${t.replace(/\n/g, " | ")})`);
  await s.page.context().close();
}
// 4. Авто, no video card (a plain reason): JPEG, no error toast
{
  const s = await open({ stat: { hoststate: "no_gpu" } });
  await wait(4500);
  const t = await toasts(s);
  check(!/H\.264|видеокарт|агент/.test(t), `Авто, no video card: no message (${t.replace(/\n/g, " | ")})`);
  await s.page.context().close();
}
// 5. Авто, the agent's capture failed: said once
{
  const s = await open({ stat: { hoststate: "failed" } });
  await wait(4500);
  const t = await toasts(s);
  const n = (t.match(/захват/g) || []).length;
  check(n === 1, `Авто, the capture service failed: one message (${n}: ${t.replace(/\n/g, " | ")})`);
  await s.page.context().close();
}
// 6. the host address dialog: no reimu-host.sh in it
{
  const s = await open({ stat: { host: "" } });
  await s.page.evaluate(() => document.querySelector("#kvm-src-menu [data-host]").click());
  await wait(300);
  check(s.prompts.length === 1 && !/reimu-host\.sh|sudo/.test(s.prompts[0]), `the address dialog without reimu-host.sh (${s.prompts[0]})`);
  await s.page.context().close();
}

await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 10000))]);
console.log(failed ? `${failed} FAILED` : "all passed");
process.exit(failed ? 1 : 0);
