// Settings shared by the end-to-end tests (override with environment).
export const BASE = process.env.BASE || "https://127.0.0.1:8443";
export const VMSSH = process.env.VMSSH || new URL("../testvm/vmssh", import.meta.url).pathname;
export const CHROME = process.env.CHROME || "/opt/pw-browsers/chromium-1194/chrome-linux/chrome";
export const OUT = process.env.OUT || new URL("./screenshots", import.meta.url).pathname;
export const WORK = process.env.WORK || "/tmp/reimu-e2e";
export const COM1 = process.env.COM1 || new URL("../testvm/com1.sock", import.meta.url).pathname;

// The login form, or everything known about why it is not there: every request with its times, what
// has no answer yet, the state of the page, a fresh request past the page, a screenshot.
export function traceRequests(page) {
  const t0 = Date.now();
  const list = [];
  const at = new Map();
  const ms = () => Date.now() - t0;
  page.on("request", (r) => { const e = { url: r.url().replace(/^https?:\/\/[^/]+/, ""), start: ms() }; at.set(r, e); list.push(e); });
  page.on("response", (r) => { const e = at.get(r.request()); if (e) { e.status = r.status(); e.head = ms(); } });
  page.on("requestfinished", (r) => { const e = at.get(r); if (e) e.end = ms(); });
  page.on("requestfailed", (r) => { const e = at.get(r); if (e) { e.end = ms(); e.error = r.failure() && r.failure().errorText; } });
  page._reimuTrace = { list, ms };
}

export async function loginReady(page, timeout = 30000) {
  const t = page._reimuTrace;
  const t0 = t ? t.ms() : 0;
  try {
    await page.locator("#login-pass").waitFor({ state: "visible", timeout });
    if (t) console.log(`  login form after ${t.ms()} ms from the first request, ${t.list.length} requests`);
  } catch (e) {
    console.log(`  LOGIN FORM NOT THERE after ${timeout} ms, waited from ${t0} ms`);
    if (t) {
      for (const r of t.list) console.log(`    ${String(r.start).padStart(7)} .. ${String(r.head ?? "-").padStart(7)} .. ${String(r.end ?? "NO END").padStart(7)} ${r.status ?? "-"} ${r.error ?? ""} ${r.url}`);
      console.log(`    no answer yet: ${t.list.filter((r) => r.end === undefined).map((r) => r.url).join(" ") || "none"}`);
    }
    const state = await page.evaluate(() => ({
      ready: document.readyState, url: location.href,
      login: document.getElementById("login") && document.getElementById("login").className,
      app: document.getElementById("app") && document.getElementById("app").className,
      cookie: document.cookie.replace(/=[^;]*/g, "=*"), xterm: typeof window.Terminal,
    })).catch((x) => `evaluate failed: ${x.message}`);
    console.log(`    page: ${JSON.stringify(state)}`);
    const s0 = Date.now();
    const again = await page.request.get(new URL("/js/app.js", page.url()).href, { timeout: 30000 }).then((r) => `${r.status()}`).catch((x) => `failed: ${x.message.split("\n")[0]}`);
    console.log(`    /js/app.js fetched again by another way: ${again} in ${Date.now() - s0} ms`);
    if (process.env.OUT) await page.screenshot({ path: `${process.env.OUT}/login-missing-${Date.now()}.png`, timeout: 10000 }).catch(() => {});
    throw e;
  }
}

