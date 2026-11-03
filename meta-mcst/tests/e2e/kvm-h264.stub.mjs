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
// H.265 when the browser decodes it (VideoDecoder.isConfigSupported, faked
// here: window.__hevc), H.264 otherwise; the host says which it sends. The
// source menu: no host address item while the agent gives the address, the
// JPEG quality items only while the JPEG is shown.
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
  // what the browser decodes: H.264 always, H.265 when window.__hevc (or what
  // it really decodes: window.__realHevc)
  if (window.VideoDecoder && !window.__realHevc) {
    VideoDecoder.isConfigSupported = async (c) => ({ supported: /^avc1/.test(c.codec) || (!!window.__hevc && /^(hev1|hvc1)/.test(c.codec)), config: c });
    // no real frames come: configure() must not fail on a codec this machine lacks
    const configure = VideoDecoder.prototype.configure;
    VideoDecoder.prototype.configure = function (c) {
      return configure.call(this, { ...c, codec: /^(hev1|hvc1)/.test(c.codec) ? "avc1.640028" : c.codec });
    };
  }
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
      // then one key frame (type 6, key 1, the access unit)
      if (u[0] === 7 && window.__frame) {
        const au = Uint8Array.from(atob(window.__frame), (c) => c.charCodeAt(0));
        setTimeout(() => this.onmessage({ data: new Uint8Array([6, 1, ...au]).buffer }), 300);
      }
    }
    close() {
      clearInterval(this._t);
      this.readyState = 3;
      setTimeout(() => this.onclose && this.onclose({ code: 1000 }), 0);
    }
  };
};

const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined });

async function open({ prefs = {}, stat = {}, answer = null, hevc = false, realHevc = false, frame = null } = {}) {
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
  await page.addInitScript((r) => { window.__realHevc = r; }, realHevc);
  await page.addInitScript(FAKE_WS);
  await page.addInitScript(({ prefs, stat, answer, hevc, realHevc, frame }) => {
    localStorage.setItem("reimu-kvm", JSON.stringify(prefs));
    window.__initStat = stat;
    window.__answer = answer;
    window.__hevc = hevc;
    window.__realHevc = realHevc;
    window.__frame = frame;
  }, { prefs, stat, answer, hevc, realHevc, frame });
  await page.addInitScript(() => { Object.assign(window.__stat, window.__initStat); });
  await page.goto(`${ORIGIN}/harness.html`);
  await page.waitForFunction(() => document.querySelector("#kvm-state")?.textContent === "подключено", null, { timeout: 15000 });
  return s;
}
const asked = (s) => s.page.evaluate(() => window.__sent.filter((x) => x[0] === 7).map((x) => new TextDecoder().decode(new Uint8Array(x.slice(1)))));
// the source menu: the items not hidden, as text
const menu = (s) => s.page.evaluate(() => [...document.querySelectorAll("#kvm-src-menu > *")].filter((e) => !e.hidden).map((e) => e.textContent.trim()));
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
  const m = await menu(s);
  check(!m.some((x) => /Адрес хоста/.test(x)), `the agent gives the address: no host address item (${m.join(" | ")})`);
  check(!m.some((x) => /4:2:0|4:4:4|Прямой JPEG$/.test(x)), `on H.264: no JPEG quality items (${m.join(" | ")})`);
  check(m.filter((x) => /Авто|с хоста|видеодвижок|VNC/.test(x)).length === 4, `the four sources (${m.join(" | ")})`);
  await s.page.context().close();
}
// 1b. the browser decodes H.265: asked for, and shown when the host sends it
{
  const s = await open({ hevc: true, stat: { host: "10.0.2.2:8555" }, answer: { t: "h264", codec: "hev1.1.6.L123.B0", w: 1920, h: 1080, fps: 60 } });
  await wait(1500);
  const a = await asked(s);
  check(a.length === 1 && a[0] === "\nhevc", `H.265 decodable: the host's stream asked for as H.265 (${JSON.stringify(a)})`);
  check(/H\.265 с хоста/.test(await s.page.$eval("#kvm-src", (e) => e.textContent)), "the source shown: H.265 с хоста");
  check(!s.errors.length, `no page errors (${s.errors})`);
  await s.page.context().close();
}
// 1c. H.265 asked for, an older host agent sends H.264: shown as H.264
{
  const s = await open({ hevc: true, stat: { host: "10.0.2.2:8555" }, answer: { t: "h264", codec: "avc1.640028", w: 1920, h: 1080, fps: 60 } });
  await wait(1500);
  check(/H\.264 с хоста/.test(await s.page.$eval("#kvm-src", (e) => e.textContent)), "an older agent's H.264: the source shown: H.264 с хоста");
  await s.page.context().close();
}
// 1d. JPEG shown (no host): the quality items there, the host address item
// too (no agent to give it)
{
  const s = await open({ stat: { hoststate: "no_agent" } });
  await wait(1000);
  const m = await menu(s);
  check(m.some((x) => /Плавно/.test(x)) && m.some((x) => /Адрес хоста/.test(x)), `on JPEG without an agent: quality and host address items (${m.join(" | ")})`);
  await s.page.context().close();
}
// 1e. an address typed before, the agent there now: the item stays (to clear it)
{
  const s = await open({ prefs: { h264Host: "192.168.10.50:8555" }, stat: { host: "10.0.2.2:8555" }, answer: { t: "h264", codec: "avc1.640028", w: 1920, h: 1080, fps: 60 } });
  await wait(1000);
  const m = await menu(s);
  check(m.some((x) => /Адрес хоста.*192\.168\.10\.50/.test(x)), `an address set by hand: its item shown (${m.join(" | ")})`);
  await s.page.context().close();
}
// 1f. a real H.265 key frame of the host's card (radeonsi, hevc_vaapi) of a
// 1366x768 desktop, a red box in its bottom right corner: Mesa writes a wrong
// crop window (the frame decodes as 1406x768); the page shows 1366x768 as the
// header says, the red corner where it is
const HEVC_1366 = "AAAAAUYBEAAAAAFAAQwB//8BYAAAAwCwAAADAAADAHssCQAAAAFCAQEBYAAAAwCwAAADAAADAHugAsCAMB14kuSQpWjAWgIAAAMAAgAAAwB5iAAAAAFEAeDxgZkgAAAAASYBr3jrJX/4/rQaUKfuvkGkSTcnNvFKxgAY4HmIcAAAAwAAAwAAAwAAAwAAAwCudqRsp//E/w/hf1ncCaugAgpgJDdX8M8mQuz8bilbZ/6+6gI6ZyG3f6NAAAADAAADAAADAAADAAADAAADAb5NAAADAAADAAADAAADAAADAAAhW6AAAAMAAAMAAAMAAAMAAAMAA5cuAAADAAADAAADAAADAAADAABItIAAAAMAAAMAAAMAAAMAAAMABNNiAAADAAADAAADAAADAAADAABT4VAAAAMAAAMAAAMAAAMAAAMABh+BAAADAAADAAADAAADAAADAAB76VAAAAMAAAMAAAMAAAMAAAMACLbEAAADAAADAAADAAADAAADAAOacGhT/jE4bW9QbH3Sqr3Zv/Zj+mNCOG0rbabr94bsQ7JuFKM0MsuIL7SVxTJ9ak841uIwnddu5WCSKsZuruHrNCwDhA1CjcwAAAMAAAMAAAMAAAMAAAMAADyxLfzjJynZuAaiesEHOVTmfzp4vHFjrRibuePK4xceEyyeXcCSiVfCmXkm4A==";
{
  const s = await open({ realHevc: true, stat: { host: "10.0.2.2:8555" }, frame: HEVC_1366,
    answer: { t: "h264", codec: "hev1.1.6.L123.B0", w: 1366, h: 768, fps: 60 } });
  if (!(await s.page.evaluate(() => VideoDecoder.isConfigSupported({ codec: "hev1.1.6.L123.B0" }).then((r) => r.supported)))) {
    console.log("INFO this browser decodes no H.265: the crop not checked");
  } else {
    await s.page.waitForFunction(() => { const c = document.querySelector("#kvm-screen canvas"); return c && c.width > 300; }, null, { timeout: 10000 }).catch(() => {});
    const r = await s.page.evaluate(() => {
      const c = [...document.querySelectorAll("#kvm-screen canvas")].find((x) => x.width > 300);
      if (!c) return null;
      const px = c.getContext("2d").getImageData(c.width - 3, c.height - 3, 1, 1).data;
      return { w: c.width, h: c.height, corner: Array.from(px).slice(0, 3) };
    });
    check(r && r.w === 1366 && r.h === 768, `H.265 of a 1366x768 desktop shown as 1366x768 (${JSON.stringify(r)})`);
    check(r && r.corner[0] > 180 && r.corner[1] < 80 && r.corner[2] < 80, `its bottom right corner is the red box, no junk (${r && r.corner})`);
  }
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
