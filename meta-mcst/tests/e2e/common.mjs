// Settings shared by the end-to-end tests (override with environment).
export const BASE = process.env.BASE || "https://127.0.0.1:8443";
export const VMSSH = process.env.VMSSH || new URL("../testvm/vmssh", import.meta.url).pathname;
export const CHROME = process.env.CHROME || "/opt/pw-browsers/chromium-1194/chrome-linux/chrome";
export const OUT = process.env.OUT || new URL("./screenshots", import.meta.url).pathname;
export const WORK = process.env.WORK || "/tmp/reimu-e2e";
export const COM1 = process.env.COM1 || new URL("../testvm/com1.sock", import.meta.url).pathname;
