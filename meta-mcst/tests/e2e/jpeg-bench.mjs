// Micro-benchmark: stock noVNC JPEG path (base64 data: URL + <img>) vs the
// REIMU path (createImageBitmap) for 1024x768 JPEG frames like the AST2500
// video engine produces. Measures throughput and main-thread busy time.
import { chromium } from "playwright";
import { BASE, VMSSH, CHROME, OUT, WORK, COM1 } from "./common.mjs";
import fs from "node:fs";
const png = fs.readFileSync(`${OUT}/07-kvm.png`).toString("base64");
const browser = await chromium.launch({ executablePath: CHROME });
const page = await browser.newPage();
await page.setContent("<canvas id=c width=1024 height=768></canvas>");
const res = await page.evaluate(async (pngB64) => {
  // make a JPEG frame (quality 0.8) from a real KVM screenshot
  const img = new Image();
  img.src = "data:image/png;base64," + pngB64;
  await img.decode();
  const src = document.createElement("canvas");
  src.width = 1024; src.height = 768;
  src.getContext("2d").drawImage(img, 0, 0, 1024, 768);
  // distinct frames (like a live video stream), so no decoder cache helps
  const frames = [];
  const sctx = src.getContext("2d");
  for (let i = 0; i < 40; i++) {
    sctx.fillStyle = "#" + ((i * 2654435761) >>> 8 & 0xffffff).toString(16).padStart(6, "0");
    sctx.fillRect(20 + i * 10, 300, 200, 120);
    sctx.fillStyle = "#fff"; sctx.font = "40px sans-serif"; sctx.fillText("frame " + i, 40 + i * 10, 360);
    frames.push(new Uint8Array(await (await new Promise((r) => src.toBlob(r, "image/jpeg", 0.8))).arrayBuffer()));
  }
  const jpeg = frames[0];
  const ctx = document.getElementById("c").getContext("2d");
  const N = 200;
  // every frame unique (bytes after EOI are ignored by decoders): defeats
  // the browser's decoded-image cache, as a live stream would
  let seq = 0;
  const uniq = (u8) => { const o = new Uint8Array(u8.length + 8); o.set(u8); new DataView(o.buffer).setFloat64(u8.length, ++seq + Math.random()); return o; };
  const b64 = (u8) => { let s = ""; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000)); return btoa(s); };
  async function stock() {
    let busy = 0; const t0 = performance.now();
    for (let i = 0; i < N; i++) {
      const s = performance.now();
      const im = new Image(); im.src = "data: image/jpeg;base64," + b64(uniq(frames[i % frames.length]));
      busy += performance.now() - s;
      await new Promise((r) => (im.onload = r));
      const s2 = performance.now(); ctx.drawImage(im, 0, 0); busy += performance.now() - s2;
    }
    return { fps: N / ((performance.now() - t0) / 1000), busyMsPerFrame: busy / N };
  }
  async function accel() {
    let busy = 0; const t0 = performance.now();
    for (let i = 0; i < N; i++) {
      const s = performance.now();
      const p = createImageBitmap(new Blob([uniq(frames[i % frames.length])], { type: "image/jpeg" }));
      busy += performance.now() - s;
      const bmp = await p;
      const s2 = performance.now(); ctx.drawImage(bmp, 0, 0); bmp.close(); busy += performance.now() - s2;
    }
    return { fps: N / ((performance.now() - t0) / 1000), busyMsPerFrame: busy / N };
  }
  await stock(); await accel(); // warm up
  return { jpegBytes: jpeg.length, stock: await stock(), accel: await accel() };
}, png);
console.log(JSON.stringify(res, null, 1));
await browser.close();
