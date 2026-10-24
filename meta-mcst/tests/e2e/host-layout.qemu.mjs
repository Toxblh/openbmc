// E2E: the «Хост» page's layout with the real board's data (the host agent's
// Json as the board's host reported it: 33 PCIe devices, NVMe/SATA/USB disks,
// two NICs, the e2k_edac controller without DIMM entries) served in place of
// reimu-hostlink's by a route; the page itself from the BMC (QEMU).
//   BASE=https://127.0.0.1:3443 OUT=dir node host-layout.qemu.mjs
// WWW=dir: the page's js/ and css/ from that directory instead (a working
// tree before it is built into the image). At 1400×1000, 768×1024, 390×844:
// no card wider than the window, tables inside their card (or scrolling in
// it), sizes/speeds/addresses on one line, the PCIe header never floating
// over the rows, no sideways scroll of the page, the top bar inside the
// window; the ECC card per controller. Screenshots OUT/layout-*.png.
import { chromium } from "playwright";
import { BASE, OUT } from "./common.mjs";
import fs from "node:fs";
import path from "node:path";

fs.mkdirSync(OUT, { recursive: true });
const TAG = process.env.TAG || "run";
let failed = 0;
const check = (c, w) => {
  console.log(`${c ? "PASS" : "FAIL"} ${w}`);
  if (!c) failed++;
};

// ------------------------------------------------------------------ the board's data
const pciNames = [
  ["0000:00:00.0", "Host bridge", "MCST", "Elbrus E8C2 PCIe root", null],
  ["0000:00:01.0", "PCI bridge", "MCST", "Elbrus PCIe bridge", "pcieport"],
  ["0000:01:00.0", "Non-Volatile memory controller", "Samsung Electronics Co Ltd", "NVMe SSD Controller SM981/PM981/PM983", "nvme"],
  ["0000:01:01.0", "Ethernet controller", "MCST", "Elbrus Ethernet 1 Gb/s (e1000-like)", "l_e1000"],
  ["0000:01:01.1", "Ethernet controller", "MCST", "Elbrus Ethernet 1 Gb/s (e1000-like)", "l_e1000"],
  ["0000:01:02.0", "SATA controller", "MCST", "Elbrus SATA AHCI controller", "ahci"],
  ["0000:01:03.0", "USB controller", "MCST", "Elbrus USB 2.0 EHCI controller", "ehci-pci"],
  ["0000:01:03.1", "USB controller", "MCST", "Elbrus USB 1.1 OHCI controller", "ohci-pci"],
  ["0000:01:04.0", "Audio device", "MCST", "Elbrus HD Audio controller", "snd_hda_intel"],
  ["0000:03:00.0", "VGA compatible controller", "Advanced Micro Devices, Inc. [AMD/ATI]", "Navi 10 [Radeon RX 5600 OEM/5600 XT / 5700/5700 XT]", "amdgpu"],
  ["0000:03:00.1", "Audio device", "Advanced Micro Devices, Inc. [AMD/ATI]", "Navi 10 HDMI Audio", "snd_hda_intel"],
];
const pci = [];
for (let i = 0; pci.length < 33; i++) {
  const [slot, cls, vendor_name, name, driver] = pciNames[i % pciNames.length];
  pci.push({ slot: i < pciNames.length ? slot : `0000:${(4 + i).toString(16).padStart(2, "0")}:00.0`, class_name: cls, vendor_name, name, driver,
    vendor: "1c1f", device: "8019", link: { speed: "8.0 GT/s PCIe", width: 4, max_speed: "16.0 GT/s PCIe", max_width: 16 }, numa: 0 });
}
const state = {
  version: "1.0", port: 5564, iface: "eth0", fingerprint: "8b7ad2b089b953ea3faff61f854fdedb" + "0".repeat(32), mac: "98:a7:b0:06:0f:7e",
  ipv4: "192.168.10.79", paired: true, pairing: { active: false, left: 0 }, disk: { on: false, left: 0 },
  agent: { connected: true, id: "x", host: "elbrus", version: "1.0", transport: "ipv6-ll", peer: "fe80::9aa7:b0ff:fe06:f61%eth0", since: 1793000000, last_seen: 1793000100 },
  heartbeat: { state: "ok", age: 3 }, policy: { gpu_fan: { mode: "auto", duty: null }, turbo_left: 0 }, fand_turbo: false,
  sensors: { Host_GPU_Edge: 52, Host_GPU_Junction: 61, Host_GPU_Memory: 64, Host_GPU_Fan: 0, Host_GPU_Fan_PWM: 0, Host_GPU_Power: 9, Host_Disk_Max: 41.85 },
  thresholds: { edge: [85, 95], junction: [95, 105], memory: [95, 105] }, edac: { ce: 2, ue: 0 },
  inventory: {
    os: { name: "ALT Workstation 10.2 (e2k)", kernel: "6.1.0-3.el-e8c2", arch: "e2k", boot: "native-e2k", hostname: "elbrus" },
    cpu: { model: "E8C2", threads: 8, mhz: 1550, numa: [{ node: 0, cpus: "0-7", mem_mib: 32094 }] },
    memory: { total_mib: 32094, ecc: "edac", dimms: [], controllers: [{ mc: 0, name: "E2K_EDAC_MC.0", size_mib: 0 }] },
    pci,
    disks: [
      { name: "nvme0n1", model: "Samsung SSD 970 PRO 512GB", serial: "S463NF0M123456A", size: 512110190592 },
      { name: "sda", model: "NT-512 2280", serial: "2022113000123", size: 512110190592 },
      { name: "sdb", model: "Ultra Fit", serial: "4C530001230105117044", size: 15376000000 },
    ],
    nics: [
      { name: "enp1s1f0", state: "down", speed: null, mac: "98:a7:b0:06:0f:60", addrs: [] },
      { name: "enp1s1f1", state: "up", speed: 1000, mac: "98:a7:b0:06:0f:61", addrs: ["192.168.10.50/24", "fd00:1234:5678:9abc:9aa7:b0ff:fe06:f61/64", "fe80::9aa7:b0ff:fe06:f61/64"] },
    ],
    gpu: { name: "Radeon RX 5700 XT" },
  },
  inventory_age: 10,
  metrics: {
    uptime: 86400, load: [0.52, 0.4, 0.31], mem: { total_kib: 32864256, avail_kib: 30000000, swap_total_kib: 0, swap_free_kib: 0 },
    edac: { loaded: true, ce_total: 2, ue_total: 0, mcs: [{ mc: 0, name: "E2K_EDAC_MC.0", ce: 2, ue: 0, ce_noinfo: 0, ue_noinfo: 0, dimms: [], csrows: [] }] },
    gpu: { temp_edge: 52, temp_junction: 61, temp_mem: 64, fan_rpm: 0, pwm_pct: 0, pwm_enable: 2, power_w: 9, sclk_mhz: 800, mclk_mhz: 100, busy_pct: 0, mem_busy_pct: 0,
      vram_used: 300000000, vram_total: 8573157376, link: { speed: "8.0 GT/s PCIe", width: 16 }, slot: "0000:03:00.0", policy: { mode: "auto" } },
    disks: [{ name: "nvme0n1", temp: 41.85, smart: "PASSED" }, { name: "sda", temp: 35, smart: "PASSED" }, { name: "sdb", temp: null, smart: null }],
  },
  metrics_age: 3,
  events: [{ time: 1793000000, sev: "warning", kind: "ecc_ce", text: "host memory: corrected ECC errors 0 -> 2 (E2K_EDAC_MC.0)" }],
  results: [],
};

const browser = await chromium.launch({ executablePath: process.env.CHROME || undefined, args: ["--ignore-certificate-errors"] });
const SIZES = [[1400, 1000], [768, 1024], [390, 844]];
for (const [w, hgt] of SIZES) {
  const ctx = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: w, height: hgt }, locale: "ru-RU", isMobile: w < 500, hasTouch: w < 500 });
  const page = await ctx.newPage();
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  await page.route("**/xyz/openbmc_project/reimu/hostlink", (r) =>
    r.request().method() === "GET" ? r.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ data: { Json: JSON.stringify(state) } }) }) : r.continue());
  if (process.env.WWW) {
    await page.route(/\/(js|css)\/.*\.(js|css)(\?.*)?$/, (r) => {
      const p = path.join(process.env.WWW, new URL(r.request().url()).pathname);
      if (!fs.existsSync(p)) return r.continue();
      r.fulfill({ status: 200, contentType: p.endsWith(".css") ? "text/css" : "application/javascript", body: fs.readFileSync(p) });
    });
  }
  const at = `${w}×${hgt}`;
  try {
    await page.goto(BASE + "/#/host");
    await page.fill("#login-pass", "0penBmc");
    await page.click("#login-form button[type=submit]");
    await page.waitForSelector("#app:not(.hidden)");
    await page.goto(BASE + "/#/host");
    const card = (t) => page.locator(".card", { has: page.locator("h3", { hasText: t }) });
    await card("Устройства PCIe").locator("tbody tr").nth(32).waitFor({ timeout: 30000 });
    await page.waitForTimeout(500);
    await page.screenshot({ path: `${OUT}/layout-${TAG}-${w}.png`, fullPage: false });
    // every card: inside the window; its tables inside it, or scrolling inside it
    const cards = await page.evaluate(() => {
      const out = [];
      for (const c of document.querySelectorAll("#host-body .card")) {
        const r = c.getBoundingClientRect();
        const title = (c.querySelector("h3") || {}).textContent || "?";
        const tables = [...c.querySelectorAll("table")].map((t) => {
          const tr = t.getBoundingClientRect();
          let sc = t.parentElement;
          while (sc && sc !== c && !/(auto|scroll)/.test(getComputedStyle(sc).overflowX)) sc = sc.parentElement;
          const scr = sc && sc !== c ? sc.getBoundingClientRect() : null;
          return { right: tr.right, scroller: !!scr, scrRight: scr ? scr.right : null };
        });
        out.push({ title: title.trim().slice(0, 20), left: r.left, right: r.right, tables });
      }
      return out;
    });
    for (const c of cards) {
      check(c.right <= w + 0.5, `${at} card «${c.title}» inside the window (right ${Math.round(c.right)})`);
      c.tables.forEach((t, i) => {
        const inside = t.right <= c.right + 0.5 || (t.scroller && t.scrRight <= c.right + 0.5);
        check(inside, `${at} «${c.title}» table ${i}: inside its card (table right ${Math.round(t.right)}, card ${Math.round(c.right)}${t.scroller ? ", in a scroller" : ""})`);
      });
    }
    if (w < 700) check(new Set(cards.map((c) => Math.round(c.left))).size === 1, `${at} one column (lefts ${[...new Set(cards.map((c) => Math.round(c.left)))]})`);
    // sizes, speeds, addresses, MACs, links: one line each
    const multi = await page.evaluate(() => {
      const rx = /^(\d+([.,]\d+)?\s*(Б|КиБ|МиБ|ГиБ|ТиБ|Мбит\/с|°C)|[0-9a-f]{2}(:[0-9a-f]{2}){5}|[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.\d|\d+(\.\d+)? GT\/s PCIe x\d+.*)$/i;
      const bad = [];
      for (const el of document.querySelectorAll("#host-body td, #host-body td span, #host-body td div")) {
        const t = el.textContent.trim();
        if (!rx.test(t) && !/Мбит\/с$/.test(t)) continue;
        // the lines its text runs over (text nodes only; a badge sits a pixel or two off the line)
        const tops = [];
        const walk = document.createTreeWalker(el, NodeFilter.SHOW_TEXT);
        for (let n = walk.nextNode(); n; n = walk.nextNode()) {
          if (!n.textContent.trim()) continue;
          const range = document.createRange();
          range.selectNodeContents(n);
          for (const r of range.getClientRects()) if (r.width && !tops.some((x) => Math.abs(x - r.top) < 6)) tops.push(r.top);
        }
        if (tops.length > 1) bad.push(`${t} (${tops.length} lines)`);
      }
      return bad;
    });
    check(multi.length === 0, `${at} sizes/speeds/addresses on one line${multi.length ? ": " + multi.slice(0, 6).join("; ") : ""}`);
    // the page never scrolls sideways; the top bar fits
    const sw = await page.evaluate(() => ({
      doc: document.scrollingElement.scrollWidth, win: innerWidth,
      page: document.getElementById("page").scrollWidth, pageW: document.getElementById("page").clientWidth,
      bar: document.querySelector(".topbar").scrollWidth, barW: document.querySelector(".topbar").clientWidth,
      barRight: Math.max(...[...document.querySelectorAll(".topbar-right > *")].filter((e) => e.offsetParent).map((e) => e.getBoundingClientRect().right)),
    }));
    check(sw.doc <= sw.win && sw.page <= sw.pageW, `${at} no sideways scroll of the page (document ${sw.doc}/${sw.win}, .page ${sw.page}/${sw.pageW})`);
    check(sw.bar <= sw.barW && sw.barRight <= w + 0.5, `${at} the top bar fits (${sw.bar}/${sw.barW}, its right ${Math.round(sw.barRight)})`);
    // the PCIe header, the page scrolled into the table: attached to its table or right under the top bar
    const pc = card("Устройства PCIe");
    await pc.locator("tbody tr").nth(10).scrollIntoViewIfNeeded();
    await page.evaluate(() => {
      const t = [...document.querySelectorAll("#host-body .card")].find((c) => /PCIe/.test(c.querySelector("h3").textContent)).querySelector("tbody tr:nth-child(12)");
      const p = document.getElementById("page");
      p.scrollTop += t.getBoundingClientRect().top - document.querySelector(".topbar").getBoundingClientRect().bottom;
    });
    await page.waitForTimeout(300);
    const st = await page.evaluate(() => {
      const card = [...document.querySelectorAll("#host-body .card")].find((c) => /PCIe/.test(c.querySelector("h3").textContent));
      const th = card.querySelector("thead tr").getBoundingClientRect();
      const row = card.querySelector("tbody tr").getBoundingClientRect();
      const bar = document.querySelector(".topbar").getBoundingClientRect();
      return { th: th.top, thb: th.bottom, row: row.top, bar: bar.bottom };
    });
    const attached = Math.abs(st.thb - st.row) <= 1.5;
    const stuck = Math.abs(st.th - st.bar) <= 1.5;
    check(attached || stuck, `${at} the PCIe header attached to its table or right under the top bar (header ${Math.round(st.th)}..${Math.round(st.thb)}, first row ${Math.round(st.row)}, top bar ${Math.round(st.bar)})`);
    await page.screenshot({ path: `${OUT}/layout-${TAG}-${w}-pcie.png` });
    // ECC: the controller's counts, and why there are no DIMMs
    const ecc = await card("Память ECC").innerText();
    check(/E2K_EDAC_MC\.0/.test(ecc) && /по модулям/.test(ecc), `${at} ECC: per controller, no per-DIMM breakdown said`);
  } catch (e) {
    check(false, `${at} ${e.message}`.split("\n")[0]);
    await page.screenshot({ path: `${OUT}/layout-${TAG}-${w}-fail.png` }).catch(() => {});
  }
  check(errors.length === 0, `${at} no page errors ${JSON.stringify(errors)}`);
  await ctx.close();
}
await browser.close();
process.exit(failed ? 1 : 0);
