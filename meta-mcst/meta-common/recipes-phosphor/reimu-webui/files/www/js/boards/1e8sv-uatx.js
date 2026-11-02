// Панель 1Э8СВ-uATX, ТВГИ.469555.445. Положения: рисунки 1, 2 и 9 руководства
// ТВГИ.469555.445РЭ и чертёж платы, мм от левого верхнего угла. approx: на
// чертеже такого нет, место выбрано рядом.
import { svgEl as $ } from "../lib/boardscene.js";
import { LID } from "./lid-elbrus-8sv.js";
import { ASPEED_LOGO } from "../lib/chip-aspeed.js";

const MEM = [["X46", 48, 15, 147, 5, 3], ["X44", 48, 24, 147, 5, 2], ["X16", 48, 131, 147, 5, 0], ["X45", 48, 140, 147, 5, 1]];
const PCI = [["X31", 43, 159, 88, 7], ["X30", 43, 180, 88, 7], ["X32", 43, 220, 88, 7]];
const SATA = [["X26", 231, 138, 0], ["X20", 209, 101, 1], ["X21", 220, 120, 2], ["X22", 231, 120, 4], ["X23", 220, 138, 5], ["X24", 231, 101, 6], ["X25", 220, 101, 7]];
const FANS = [
  { id: "X37", r: [195, 43, 10, 5], chan: "LM96163 процессора", src: "6-004c", name: "вентилятор", at: [207, 45.5, 3], side: "r" },
  { id: "X38", r: [175, 150, 11, 4], chan: "LM96163 КПИ-2", src: "2-004c", name: "вентилятор", at: [180.5, 152, 3], dy: 1.5 },
  { id: "X39", r: [186, 150, 10, 4], chan: "LM96163 КПИ-2", noTach: "2-004c", at: [191, 150, 3], dy: -1.5 },
  { id: "X40", r: [164, 150, 10, 4], chan: "LM96163 КПИ-2", noTach: "2-004c", at: [169, 150, 3], dy: -1.5 },
  { id: "X5", r: [63, 1, 6, 10], chan: "EMC2305 0, канал 0", src: "2-002e", name: "вентилятор 0" },
  { id: "X6", r: [71, 1, 6, 10], chan: "EMC2305 0, канал 1", src: "2-002e", name: "вентилятор 1" },
  { id: "X41", r: [79, 1, 6, 10], chan: "EMC2305 0, канал 2", src: "2-002e", name: "вентилятор 2" },
  { id: "X54", r: [234, 25, 10, 4], chan: "EMC2305 0, канал 3", src: "2-002e", name: "вентилятор 3" },
  { id: "X55", r: [234, 33, 10, 4], chan: "EMC2305 0, канал 4", src: "2-002e", name: "вентилятор 4" },
  { id: "X56", r: [234, 41, 10, 4], chan: "EMC2305 1, канал 0", src: "2-002c", name: "вентилятор 5" },
  { id: "X57", r: [234, 66, 10, 4], chan: "EMC2305 1, канал 1", src: "2-002c", name: "вентилятор 6" },
  { id: "X58", r: [234, 74, 10, 4], chan: "EMC2305 1, канал 2", src: "2-002c", name: "вентилятор 7" },
  { id: "X59", r: [237, 188, 4, 10], chan: "EMC2305 1, канал 3", src: "2-002c", name: "вентилятор 8" },
  { id: "X60", r: [237, 207, 4, 10], chan: "EMC2305 1, канал 4", src: "2-002c", name: "вентилятор 9" },
];
const MEMROUTE = { X46: [[254, 17.5]], X44: [[205, 30.5], [254, 30.5]], X16: [[205, 136.5], [254, 136.5]], X45: [[205, 160], [254, 160]] };

// a D-sub, RJ45, USB on a port's face: u along the edge, v up from the board
// Each kind of connector has one size, here, in mm, by its standard: a port says
// only where it is. RJ45 opening, USB-A opening, DE-15 shell, its screws and flange.
const JACK = { rj45: { w: 11.7, h: 8.2 }, usb: { w: 12.5, h: 5.1 }, vga: { w: 16.9, h: 8.3, screws: 25, flange: 30.8 } };

function rj(g, cu, y, link) {
  const { w, h } = JACK.rj45, x = cu - w / 2;
  $("rect", { x, y, width: w, height: h, rx: 0.5, fill: "#121315" }, g);
  for (let i = 0; i < 8; i++) $("rect", { x: x + w * 0.2 + i * (w * 0.6 / 7) - 0.15, y: y + h - 2.2, width: 0.3, height: 1.6, fill: "#c9b26a" }, g);
  $("rect", { x: x + w / 2 - 2, y, width: 4, height: 1.4, fill: "#2a2b2e" }, g);
  $("rect", { x: x + 0.6, y: y + h + 0.6, width: 1.6, height: 1, rx: 0.2, fill: link ? "#30d158" : "#3a3b3e" }, g);
  $("rect", { x: x + w - 2.2, y: y + h + 0.6, width: 1.6, height: 1, rx: 0.2, fill: link ? "#ff9f0a" : "#3a3b3e" }, g);
}
function usb(g, cu, y) {
  const { w, h } = JACK.usb, x = cu - w / 2;
  $("rect", { x, y, width: w, height: h, rx: 0.4, fill: "#121315" }, g);
  $("rect", { x: x + 1.4, y: y + h * 0.52, width: w - 2.8, height: h * 0.3, fill: "#3a3b3e" }, g);
}
function vga(g, cx, cy) {
  const hw = JACK.vga.w / 2, hh = JACK.vga.h / 2, n = hw - 1.15;
  $("path", { d: `M${cx - hw + 0.9},${cy + hh}H${cx + hw - 0.9}Q${cx + hw + 0.3},${cy + hh} ${cx + hw},${cy + hh - 1.2}L${cx + n},${cy - hh + 1.1}Q${cx + n - 0.3},${cy - hh} ${cx + n - 1.4},${cy - hh}H${cx - n + 1.4}Q${cx - n + 0.3},${cy - hh} ${cx - n},${cy - hh + 1.1}L${cx - hw},${cy + hh - 1.2}Q${cx - hw - 0.3},${cy + hh} ${cx - hw + 0.9},${cy + hh}Z`, fill: "#121315", stroke: "#6e706d", "stroke-width": 0.35 }, g);
  [[2, -4.58], [0, -5.725], [-2, -4.58]].forEach(([dy, x0]) => {
    for (let i = 0; i < 5; i++) $("circle", { cx: cx + x0 + i * 2.29, cy: cy + dy, r: 0.45, fill: "#8e9095" }, g);
  });
  [-1, 1].forEach((d) => $("circle", { cx: cx + d * JACK.vga.screws / 2, cy, r: 1, fill: "#3a3b3e" }, g));
}
const led = (g, u, v, on, col) => {
  if (on) $("circle", { cx: u, cy: v, r: 2.6, fill: col, opacity: 0.35 }, g);
  $("circle", { cx: u, cy: v, r: 1.2, fill: on ? col : "#3a3b3e" }, g);
};
const clip = (s, n) => s.slice(0, n);

function draw(sc, m) {
  const { plate: PL, part, box, slot, card, top, left, only, txt, label } = sc;
  const silk = "#8d9096";
  // silkscreen, stickers
  txt(PL, 95, 157.6, "PCIE x8", 2.6, silk);
  txt(PL, 95, 178.6, "PCIE x8", 2.6, silk);
  txt(PL, 95, 218.6, "PCIE x2", 2.6, silk);
  txt(PL, 210, 92.6, "M2 SATA3", 2.4, silk);
  txt(PL, 176, 176.8, "КПИ-2", 3, silk, "middle", 500);
  txt(PL, 142, 226.5, "1Э8СВ-uATX", 7.2, "#a6a8ad", "start", 600);
  $("rect", { x: 63, y: 149.3, width: 17, height: 6, rx: 0.5, fill: "#2a2b2f" }, PL);
  txt(PL, 71.5, 153.7, "МЦСТ:", 3, "#d4d5d8", "middle", 600);
  const sn = $("g", { class: "bs-part" }, PL);
  sn.dataset.id = "SN";
  $("rect", { x: 82, y: 149.3, width: 30, height: 6, rx: 0.6, fill: "#ececea" }, sn);
  if (m.board.serial) txt(sn, 97, 153.6, m.board.serial, 3, "#1d1d1f", "middle", 500);
  const pn = $("g", { class: "bs-part" }, PL);
  pn.dataset.id = "PN";
  $("rect", { x: 92, y: 229.2, width: 51, height: 6.2, rx: 0.6, fill: "#ececea" }, pn);
  if (m.board.part) txt(pn, 117.5, 233.6, m.board.part, 2.9, "#1d1d1f", "middle", 500);
  // holes: mounting, and around the CPU for the cooler
  [[33, 6], [165, 7], [237, 6], [10, 164], [165, 164], [237, 164], [10, 210], [165, 232], [237, 237], [85, 52], [179, 52], [85, 108], [179, 108]].forEach(([x, y]) => {
    $("circle", { cx: x, cy: y, r: 2.4, fill: "#000" }, PL);
    $("circle", { cx: x, cy: y, r: 3.5, fill: "none", stroke: "#757b80", "stroke-width": 0.6 }, PL);
  });
  [[93, 41, -43], [171, 41, 43], [93, 119, 43], [171, 119, -43]].forEach(([x, y, a]) =>
    $("rect", { x: x - 3, y: y - 5, width: 6, height: 10, rx: 3, transform: `rotate(${a} ${x} ${y})`, fill: "#000", stroke: "#85898a", "stroke-width": 0.6 }, PL));

  // memory
  MEM.forEach(([id, x, y, w, h]) => {
    const d = m.mem.slots[id];
    const g = part(id, x + w / 2, y + h / 2);
    const busy = d.state === "busy";
    box(g, x, y, w, h, 0, 7.5, "#232427");
    slot(top(g, 7.5), x, y, w, h, 0.42, busy, true);
    if (busy) card(g, x + 3, y + h / 2 - 0.5, w - 6, 7.5, 6, "#2b2c30", `${d.size} ГБ`);
    const s = busy ? `${d.size} ГБ` : d.state === "empty" ? "пусто" : "неизвестно";
    label(id, [x + w, y + h / 2, 7.5], MEMROUTE[id], `${id} · ${s}`, `${id} · ${busy ? `${d.size} ГБ` : "—"}`, { dim: !busy });
  });
  // PCIe: the lit part and the card as long as the lanes in use
  PCI.forEach(([id, x, y, w, h]) => {
    const d = m.pcie[id];
    const g = part(id, x + w / 2, y + h / 2);
    const busy = d.state === "busy";
    box(g, x, y, w, h, 0, 11, "#232427");
    slot(top(g, 11), x, y, w, h, 0.14, busy ? d.width / d.slot : 0, false);
    let full;
    let short;
    if (busy) {
      const len = ((w - 3) * d.width) / d.slot;
      card(g, x + 1.5, y + h / 2 - 0.5, len, 11, 10, d.kind === "gpu" ? "#34363b" : "#2b3330", `${d.short} · x${d.width}`);
      full = `${id} · x${d.width}${d.width !== d.slot ? ` из x${d.slot}` : ""} · ${d.dev}`;
      short = `${id} · x${d.width}/x${d.slot}`;
    } else {
      full = `${id} · x${d.slot} · ${d.state === "empty" ? "пусто" : "неизвестно"}`;
      short = `${id} · x${d.slot}`;
    }
    label(id, [x, y + h / 2, 11], [[-10, y + h / 2]], full, short, { end: true, dim: !busy });
  });
  // M.2 SATA X48: the connector and a 2280 module lying up from it
  {
    const d = m.m2.X48;
    const g = part("X48", 221, 50);
    const busy = d.state === "busy";
    box(g, 210, 80, 22, 9, 0, 3, "#131416");
    slot(top(g, 3), 210, 80, 22, 9, 0.3, busy, false);
    if (busy) {
      box(only($("g", {}, g), true), 210, 4, 22, 77.5, 1.5, 2, "#1b1c1f");
      box(only($("g", {}, g), false), 210, 4, 22, 80.5, 1.5, 2, "#1b1c1f");
      const t = top(g, 3.5);
      $("rect", { x: 213.5, y: 16, width: 15, height: 40, rx: 0.8, fill: "#303135" }, t);
      $("rect", { x: 213.5, y: 62, width: 15, height: 9, rx: 0.5, fill: "#3a3b40" }, t);
      $("circle", { cx: 221, cy: 6.5, r: 1.2, fill: "#000", stroke: "#6e706d", "stroke-width": 0.3 }, t);
      $("text", { "font-size": 2.8, fill: "#a0a2a7", "text-anchor": "middle", "font-weight": 500, transform: "translate(222 36) rotate(-90)" }, t).textContent = clip(d.short, 22);
    }
    label("X48", [232, 84.5, 3], [[254, 84.5]], busy ? `X48 · ${d.dev} · ${d.size}` : `X48 · ${d.state === "empty" ? "пусто" : "неизвестно"}`, busy ? `X48 · ${d.size}` : "X48 · —", { dim: !busy });
  }
  // SATA, as on the photo; a short cable straight up in a port with a disk
  SATA.forEach(([id, x, y]) => {
    const busy = m.sata[id].state === "busy";
    const g = part(id, x + 3, y + 7.5);
    box(g, x, y, 6, 15, 0, 5, "#0e0e10");
    const t = top(g, 5);
    $("path", { d: `M${x + 0.9},${y}H${x + 5.1}L${x + 6},${y + 0.9}V${y + 15}H${x}V${y + 0.9}Z`, fill: "#17181a", stroke: busy ? "#8a8c90" : "#3c3d41", "stroke-width": 0.2 }, t);
    $("rect", { x: x + 0.9, y: y + 1.1, width: 4.2, height: 12.6, rx: 0.3, fill: "#050506", stroke: busy ? "#8a8c90" : "#2c2d30", "stroke-width": 0.2 }, t);
    $("path", { d: `M${x + 1.8},${y + 2.5}H${x + 4.2}V${y + 12.6}H${x + 3.3}V${y + 3.4}H${x + 1.8}Z`, fill: busy ? "#c4c6ca" : "#6a6c71" }, t);
    if (busy) {
      box(g, x + 1, y + 2, 4, 11, 5, 3, "#2c2e32");
      box(g, x + 2.2, y + 6, 1.6, 3, 8, 12, "#1f2024");
    }
  });
  // battery, BMC, the USB header
  {
    const g = part("X42", 99.5, 206.5);
    sc.cyl(g, 99.5, 206.5, 10, 0, 3.2, "#b9bbb8");
    const t = top(g, 3.2);
    txt(t, 99.5, 204.5, "+", 4, "#4a4c4b", "middle", 700);
    txt(t, 99.5, 210.5, "CR2032", 2.6, "#6b6d6c", "middle", 500);
  }
  {
    const g = part("BMC", 33.5, 77);
    box(g, 24, 68, 19, 18, 0, 2, "#141517");
    const t = top(g, 2);
    $("path", { d: ASPEED_LOGO, fill: "#e8e9ea", "fill-rule": "evenodd", transform: "translate(25.9 72.2) scale(.152)" }, t);
    txt(t, 33.5, 80.6, "AST2500", 2.3, "#e8e9ea", "middle", 500);
  }
  if (m.bmcLoad) sc.valueCloud("B:load", "BMC", [33.5, 90, 0], "c", 0, `LA ${m.bmcLoad[0].toFixed(2)}`);
  box(part("X19", 38, 237), 30, 234, 16, 6, 0, 5, "#2c2d31");
  // the CPU
  {
    const g = part("CPU", 131.5, 80);
    box(g, 97, 53, 69, 54, 0, 1, "#232427");
    box(g, 102, 58, 59, 44, 1, 1.5, "#b4b6b3");
    const inner = $("g", { transform: "translate(102 58) scale(0.1475 0.14667)" }, top(g, 2.5));
    const doc = new DOMParser().parseFromString(LID, "image/svg+xml");
    inner.appendChild(document.importNode(doc.documentElement, true));
  }
  // КПИ-2 with its pin heatsink
  {
    const g = part("KPI", 176, 197);
    box(g, 158, 179, 36, 36, 0, 2.5, "#45474b");
    for (let i = 0; i < 6; i++) for (let j = 0; j < 6; j++) box(g, 159.6 + i * 6, 180.6 + j * 6, 3.2, 3.2, 2.5, 7, "#52545a");
  }
  // fans
  FANS.forEach((f) => {
    const [x, y, w, h] = f.r;
    const d = m.fans[f.id] || {};
    const g = part(f.id, x + w / 2, y + h / 2);
    if (!d.conn) g.setAttribute("opacity", 0.35);
    box(g, x, y, w, h, 0, 3, d.conn ? "#d3d4d8" : "#7c7d82");
    if (d.conn) {
      sc.fanGlyph(top(g, 3), x + w / 2, y + h / 2, Math.min(w, h) * 0.48, d.rpm !== null ? d.rpm : (d.pct || 0) * 25);
      sc.valueCloud(`F:${f.id}`, f.id, f.at || [x + w / 2, y - 2, 3], f.side, f.dy, fanText(d));
    }
  });
  // the rear panel, along the edge as on the drawing: X4 (two USB, LAN over them),
  // the button, VGA, X15, X14, UID. From above: the bodies as they are, on them
  // from the edge the port itself in its size (its height lies along x)
  const port = (id, y, depth, h, hh, face, fromAbove) => {
    const g = part(id, depth / 2, y + h / 2);
    box(g, 0, y, depth, h, 0, hh, "#8e9093");
    face(only(left(g, 0), true));
    (fromAbove || face)($("g", { transform: "matrix(0 1 1 0 1 0)" }, only(top(g, hh), false)));
    return g;
  };
  // a cable in a LAN port with a link: a plug at the edge, a short cord out
  const plug = (g, cu, z, hh, v) => {
    const { w, h } = JACK.rj45, u = cu - w / 2;
    const a = only($("g", {}, g), true);
    box(a, -7, u + 0.6, 7, w - 1.2, z + 0.5, h - 1, "#d9dadd");
    box(a, -22, u + w / 2 - 1.3, 15, 2.6, z + h / 2 - 1.3, 2.6, "#c2c4c8");
    const t = only(top(g, hh), false);
    const fr = 0.8;
    $("rect", { x: 1 + v, y: u, width: h, height: w, rx: 0.5, fill: "#121315" }, t);
    $("rect", { x: 1 + v + fr, y: u + fr, width: h - 2 * fr, height: w - 2 * fr, rx: 0.5, fill: "#d9dadd" }, t);
    $("rect", { x: -22, y: u + w / 2 - 1.3, width: 23 + v + h * 0.3, height: 2.6, fill: "#b5b7bb" }, t);
    $("rect", { x: 1 + v + h * 0.3, y: u + w * 0.3, width: h * 0.4, height: w * 0.4, rx: 0.3, fill: "#b5b7bb" }, t);
  };
  // a USB cable; dy: its cord from above moved aside from the LAN one
  const usbPlug = (g, cu, z, hh, v, dy) => {
    const { w, h } = JACK.usb, u = cu - w / 2;
    const a = only($("g", {}, g), true);
    box(a, -6, u + 1, 6, w - 2, z + 0.4, h - 0.8, "#2c2e32");
    box(a, -20, u + w / 2 - 1, 14, 2, z + h / 2 - 1, 2, "#3a3c41");
    const t = only(top(g, hh), false);
    $("rect", { x: 1 + v, y: u, width: h, height: w, rx: 0.4, fill: "#2c2e32", stroke: "#8a8c90", "stroke-width": 0.25 }, t);
    $("rect", { x: 1 + v + h * 0.3, y: u + w * 0.25, width: h * 0.4, height: w * 0.5, rx: 0.2, fill: "#6a6c71" }, t);
    $("rect", { x: -20, y: u + w / 2 + dy - 1, width: 21 + v + h * 0.6, height: 2, fill: "#6a6c71" }, t);
  };
  const N = m.net;
  {
    const u = 28.5, H = JACK.usb.h, gap = 1; // from above the three lie side by side from the edge
    const g = port("X4", 20, 26, 17, 30.5, (f) => { usb(f, u, 3.3); usb(f, u, 11.4); rj(f, u, 20.3, N.X4.link); },
      (f) => { usb(f, u, 0); usb(f, u, H + gap); rj(f, u, 2 * (H + gap), N.X4.link); });
    if (m.usb.X4[0]) usbPlug(g, u, 3.3, 30.5, 0, 3.1);
    if (m.usb.X4[1]) usbPlug(g, u, 11.4, 30.5, H + gap, -3.1);
    if (N.X4.link) plug(g, u, 20.3, 30.5, 2 * (H + gap));
  }
  const btn = (cy) => (f) => $("circle", { cx: 46.6, cy, r: 1.9, fill: "#121315", stroke: "#6e706d", "stroke-width": 0.3 }, f);
  port("BTN", 42.5, 6, 8.2, 4.7, btn(2.4), btn(1.9));
  port("X28", 74.6, 15, 31.4, 13, (f) => vga($("g", { transform: "translate(0 6.2)" }, f), 90.3, 0), (f) => vga($("g", { transform: `translate(0 ${JACK.vga.h / 2})` }, f), 90.3, 0));
  // the two single ports are one part: same body, same place in it
  const lan = (id, y) => {
    const cu = y + 16.7 / 2;
    const g = port(id, y, 21, 16.7, 14, (f) => rj(f, cu, 5.2, N[id].link), (f) => rj(f, cu, 0, N[id].link));
    if (N[id].link) plug(g, cu, 5.2, 14, 0);
  };
  lan("X15", 108.8);
  lan("X14", 128);
  const uid = (y, h, v) => (f) => {
    $("rect", { x: 145.6, y, width: 8.8, height: h, fill: "#6f7175" }, f);
    led(f, 150, v, m.uid.on, "#0a84ff");
  };
  port("UID", 145, 6.5, 10, 6.5, uid(0.6, 5.3, 3.2), uid(0, 5, 2.5));

  const lanLabel = (id, y, z, name, short) => {
    const d = N[id];
    const s = !d.known ? "неизвестно" : d.link ? (d.speed ? `${d.speed} Мбит/с` : "линк есть") : "нет линка";
    label(id, [d.link ? -22 : 0, y, z], [[-30, y]], `${name} · ${s}`, short, { end: true, dot: d.known ? (d.link ? "on" : "off") : null, dim: !d.link });
  };
  lanLabel("X4", 28.5, 24, "LAN BMC · USB ×2", "BMC");
  label("X28", [0, 90.3, 5.6], [[-30, 90.3]], "VGA", "VGA", { end: true });
  lanLabel("X15", 117.15, 9, "LAN GE1", "GE1");
  lanLabel("X14", 136.35, 9, "LAN GE0", "GE0");

  m.sensors.forEach((s) => sc.tempCloud(s));
}

export const fanText = (d) => (d.rpm !== null ? `${d.rpm} об/мин` : `${Math.round(d.pct)} %`);

export default {
  name: "1Э8СВ-uATX",
  doc: "ТВГИ.469555.445РЭ",
  size: [244, 244],
  draw,
  fans: FANS,
  memory: Object.fromEntries(MEM.map(([id, , , , , ch]) => [id, ch])),
  sataPorts: Object.fromEntries(SATA.map(([id, , , ch]) => [id, ch])),
  m2: { X48: 3 }, // M.2 2280 SATA, канал 3
  // what the board's sensors say; heat: a halo around a real sensor, r in mm
  temps: [
    { id: "CPU", title: "Процессор Эльбрус-8СВ", at: [148, 96], z: 2.5, side: "c", r: 44, srcs: /^5-00/, note: "Показан максимум по кристаллу." },
    { id: "KPI", title: "КПИ-2", at: [176, 197], z: 9.5, side: "c", r: 30, src: "2-004c", name: "IOHUB Temp" },
    { id: "LM_CPU", title: "LM96163 процессора", at: [200, 55], side: "r", approx: true, src: "6-004c", name: "Internal Temp" },
    { id: "LM_KPI", title: "LM96163 КПИ-2", at: [193, 158], side: "r", approx: true, src: "2-004c", name: "Internal Temp" },
    { id: "UNCORE", title: "Источник +UNCORE", at: [64, 94], side: "l", approx: true, src: "6-0044", name: "температура" },
    { id: "PWR", title: "Power Control", at: [64, 59], side: "l", approx: true, src: "6-0060", name: "температура" },
    { id: "V12_01", title: "Источник +1V2_01", at: [195, 121], side: "l", approx: true, src: "6-0045", name: "температура" },
    { id: "V12_23", title: "Источник +1V2_23", at: [193, 34], side: "l", approx: true, src: "6-0046", name: "температура" },
  ],
  // the cloud of a graphics card's temperature, by its slot's place
  gpuCloud: (slotId) => ({ X31: [40, 166.5], X30: [40, 187.5], X32: [40, 227.5] })[slotId],
  // Рисунок 9: GE0 -> X14, GE1 -> X15. PCI-функция -> GE: по порядку, кабелем не проверено
  nics: { "0000:01:01.0": "X14", "0000:01:01.1": "X15", enp1s1f0: "X14", enp1s1f1: "X15" },
  // AHCI numbers its ports over both controllers: ataN = SATA N-1 (рисунок 9); checked on one: M.2 at ata4
  ata: (n) => n - 1,
  // Рисунок 9: X19 USB01, USB03; X4 USB10, USB12; USB13 is the BMC's. EHCI 01:0a: USB0x, 01:0b: USB1x
  // (checked: the BMC's virtual hub is 4-4). Root hub -> controller, port N -> USBx(N-1).
  // In X4 USB10 is the lower port, USB12 the upper: not in the manual, checked on the board (2026-11-02).
  usb: { roots: { 1: 0, 2: 0, 3: 1, 4: 1 }, ports: { "01": "X19", "03": "X19", 10: "X4", 12: "X4", 13: "BMC" }, order: { X4: ["10", "12"] } },
  // Слоты по рисунку 9 и шелкографии: x8, x8, x2. Какой порт в каком разъёме: X30 и X31 сказал
  // владелец платы, по видеокарте и NVMe; X32: порты x4, на которых нет ничего встроенного (01:05.0 это BMC)
  pcie: {
    gen: "2.0",
    slots: {
      X30: { roots: ["0000:01:08.0"], width: 8 },
      X31: { roots: ["0000:01:09.0"], width: 8 },
      X32: { roots: ["0000:01:04.0", "0000:01:06.0", "0000:01:07.0"], width: 2, inferred: true },
    },
  },
  fixed: {
    X28: ["X28", "VGA, задняя панель"],
    X42: ["X42", "батарея CR2032"],
    X19: ["X19", "USB 2.0, передняя панель"],
    BTN: ["Кнопка", "на задней панели"],
    BMC: ["BMC", "AST2500, удалённое управление"],
    CPU: ["Эльбрус-8СВ", "микропроцессор · 1891ВМ12Я"],
    KPI: ["КПИ-2", "контроллер периферийных интерфейсов · 1991ВГ2Я"],
  },
};
