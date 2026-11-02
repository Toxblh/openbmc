// A board as a scene: one projection of millimetres on the board, from a view
// from above (t=0) to an isometric one (t=1), so the same drawing serves both and
// the change between them is a camera move. Boxes, slots, cards, labels and
// clouds of values; what stands where is in js/boards/*.js. Text from outside
// only ever goes in through textContent.
const NS = "http://www.w3.org/2000/svg";
const TH = (-20 * Math.PI) / 180;
const PH = (55 * Math.PI) / 180;
const VX = -330;
const VW = 630;
export const ZOOMS = [1, 1.5, 2, 3, 4];

export const svgEl = (t, a, p) => {
  const e = document.createElementNS(NS, t);
  for (const k in a) e.setAttribute(k, a[k]);
  if (p) p.appendChild(e);
  return e;
};
const $ = svgEl;

const pts = (...p) => p.map((q) => `${q[0].toFixed(2)},${q[1].toFixed(2)}`).join(" ");
export const shade = (hex, k) => {
  const n = parseInt(hex.slice(1), 16);
  const f = (v) => Math.round(Math.min(255, v * k)).toString(16).padStart(2, "0");
  return `#${f(n >> 16)}${f((n >> 8) & 255)}${f(n & 255)}`;
};
// temperature: blue to orange; warn and crit are the status colours of the page
export function tempColor(t, status) {
  const css = (v) => getComputedStyle(document.documentElement).getPropertyValue(v).trim();
  if (status === "crit") return css("--err");
  if (status === "warn") return css("--warn");
  const k = Math.max(0, Math.min(1, (t - 20) / 55));
  const a = [10, 132, 255];
  const b = [255, 159, 10];
  return `rgb(${a.map((v, i) => Math.round(v + (b[i] - v) * k)).join(",")})`;
}

const reducedMotion = () => matchMedia("(prefers-reduced-motion: reduce)").matches;

export function createScene(stage, { size = [244, 244], view = "iso", info = () => null, maxHeight = () => null } = {}) {
  const [W, H] = size;
  const R = []; // what follows the camera
  const parts = [];
  const labels = [];
  // a long callout goes on several lines, by words
  const setLines = (tx, str) => {
    const lines = [];
    for (const w of str.split(" ")) {
      const last = lines[lines.length - 1];
      if (last !== undefined && `${last} ${w}`.length <= 38) lines[lines.length - 1] = `${last} ${w}`;
      else lines.push(w);
    }
    tx.replaceChildren(...lines.map((l) => {
      const t = document.createElementNS("http://www.w3.org/2000/svg", "tspan");
      t.textContent = l;
      return t;
    }));
  };
  const clouds = {}; // id -> set(value)
  let T = view === "iso" ? 1 : 0;
  let viewTo = T;
  // the zoom, the middle of the view from the middle of the board, the height given to the stage
  let Z = 1;
  let PX = 0;
  let PY = 0;
  let boxH = null;
  let FS = 5.2;
  let cam;
  let slotN = 0;
  let raf = null;

  const SV = $("svg", { class: "bs-svg", viewBox: `${VX} -130 ${VW} 260`, role: "img" }, null);
  const defs = $("defs", {}, SV);
  const blur = $("filter", { id: "bs-blur", x: "-20%", y: "-20%", width: "140%", height: "140%" }, defs);
  $("feGaussianBlur", { stdDeviation: 5 }, blur);
  const gb = $("linearGradient", { id: "bs-plate", x1: 0, y1: 0, x2: 0, y2: 1 }, defs);
  $("stop", { offset: 0, "stop-color": "#2b2c2f" }, gb);
  $("stop", { offset: 1, "stop-color": "#191a1c" }, gb);
  const pop = document.createElement("div");
  pop.className = "bs-pop";
  pop.hidden = true;
  stage.replaceChildren(SV, pop);

  // the projection
  const P = (c, x, y, z = 0) => [c.s * (x * c.ct - y * c.st) + c.ox, c.s * ((x * c.st + y * c.ct) * c.cp - z * c.sp) + c.oy];
  function mkCam(t) {
    const th = TH * t;
    const ph = PH * t;
    const c = { t, ct: Math.cos(th), st: Math.sin(th), cp: Math.cos(ph), sp: Math.sin(ph), s: 1.3 - 0.1 * t, ox: 0, oy: 0 };
    const q = [[0, 0], [W, 0], [0, H], [W, H]].map((p) => P(c, p[0], p[1]));
    const xs = q.map((p) => p[0]);
    const ys = q.map((p) => p[1]);
    c.ox = -(Math.min(...xs) + Math.max(...xs)) / 2;
    c.oy = -(Math.min(...ys) + Math.max(...ys)) / 2 + 8 * t;
    return c;
  }
  const Mtop = (c, z) => [c.s * c.ct, c.s * c.st * c.cp, -c.s * c.st, c.s * c.ct * c.cp, c.ox, -c.s * z * c.sp + c.oy];
  const Mleft = (c, x) => [-c.s * c.st, c.s * c.ct * c.cp, 0, -c.s * c.sp, c.s * x * c.ct + c.ox, c.s * x * c.st * c.cp + c.oy];
  const Mfront = (c, y) => [c.s * c.ct, c.s * c.st * c.cp, 0, -c.s * c.sp, -c.s * y * c.st + c.ox, c.s * y * c.ct * c.cp + c.oy];
  const plane = (p, fn, a) => {
    const g = $("g", a || {}, p);
    R.push((c) => g.setAttribute("transform", `matrix(${fn(c).map((v) => v.toFixed(4)).join(" ")})`));
    return g;
  };
  const top = (p, z) => plane(p, (c) => Mtop(c, z));
  const left = (p, x) => plane(p, (c) => Mleft(c, x));
  const front = (p, y) => plane(p, (c) => Mfront(c, y));
  // shown in one view only: iso, or from above
  const only = (g, iso) => {
    R.push((c) => g.setAttribute("opacity", iso ? c.t : 1 - c.t));
    return g;
  };
  const txt = (g, x, y, s, fs, fill, anc, w) => {
    const t = $("text", { x, y, "font-size": fs, fill, "text-anchor": anc || "start", "font-weight": w || 400 }, g);
    t.textContent = s;
    return t;
  };

  function box(g, x, y, w, h, z0, hh, col, tf) {
    const pl = $("polygon", { fill: shade(col, 0.78) }, g);
    const pf = $("polygon", { fill: shade(col, 0.6) }, g);
    const pt = $("polygon", { fill: tf || col, stroke: "rgba(255,255,255,.06)", "stroke-width": 0.35 }, g);
    R.push((c) => {
      const z1 = z0 + hh;
      const A = P(c, x, y, z1);
      const B = P(c, x + w, y, z1);
      const C = P(c, x + w, y + h, z1);
      const D = P(c, x, y + h, z1);
      const a = P(c, x, y, z0);
      const d = P(c, x, y + h, z0);
      const e = P(c, x + w, y + h, z0);
      pt.setAttribute("points", pts(A, B, C, D));
      pl.setAttribute("points", pts(a, d, D, A));
      pf.setAttribute("points", pts(d, e, C, D));
    });
    return pt;
  }
  function cyl(g, cx, cy, r, z0, h, col) {
    const side = $("polygon", { fill: shade(col, 0.7) }, g);
    const tp = $("polygon", { fill: col, stroke: "rgba(255,255,255,.08)", "stroke-width": 0.4 }, g);
    R.push((c) => {
      const a0 = Math.atan2(c.ct, c.st);
      const N = 14;
      const lo = [];
      const hi = [];
      const full = [];
      for (let i = 0; i <= N; i++) {
        const a = a0 - Math.PI / 2 + (Math.PI * i) / N;
        const x = cx + r * Math.cos(a);
        const y = cy + r * Math.sin(a);
        lo.push(P(c, x, y, z0));
        hi.push(P(c, x, y, z0 + h));
      }
      for (let i = 0; i < 28; i++) {
        const a = (Math.PI * 2 * i) / 28;
        full.push(P(c, cx + r * Math.cos(a), cy + r * Math.sin(a), z0 + h));
      }
      side.setAttribute("points", pts(...lo, ...hi.reverse()));
      tp.setAttribute("points", pts(...full));
    });
  }
  function fanGlyph(g, cx, cy, r, rpm) {
    const o = $("g", { transform: `translate(${cx} ${cy})` }, g);
    $("circle", { r, fill: "#141517", stroke: "#55575c", "stroke-width": r * 0.1 }, o);
    const b = $("g", {}, o);
    for (let i = 0; i < 3; i++) {
      $("path", { d: `M0,${-r * 0.85}C${r * 0.55},${-r * 0.65} ${r * 0.5},${-r * 0.15} 0,0C${-r * 0.5},${-r * 0.15} ${-r * 0.55},${-r * 0.65} 0,${-r * 0.85}Z`, fill: "#74767b", transform: `rotate(${i * 120})` }, b);
    }
    $("circle", { r: r * 0.2, fill: "#3a3b3f" }, o);
    if (rpm > 0 && !reducedMotion()) {
      $("animateTransform", { attributeName: "transform", type: "rotate", from: "0", to: "360", dur: `${Math.max(0.5, 1600 / rpm).toFixed(2)}s`, repeatCount: "indefinite" }, b);
    }
    return o;
  }
  // a connector from above: frame, a channel with teeth along the long sides,
  // the key; the used part lighter, from the left end
  function slot(g, x, y, w, h, keyAt, used, ears) {
    const horiz = w >= h;
    const L = horiz ? w : h;
    const S = horiz ? h : w;
    const pad = S * 0.2;
    const frac = used === true ? 1 : used || 0;
    const hg = $("g", { transform: horiz ? `translate(${x} ${y})` : `translate(${x + w} ${y}) rotate(90)` }, g);
    const draw = (p, on) => {
      const f = on ? "#a2a4a8" : "#55575c";
      const t = on ? "#c4c6ca" : "#6f7176";
      if (ears) $("rect", { x: -1.4, y: S / 2 - S * 0.14, width: L + 2.8, height: S * 0.28, rx: S * 0.14, fill: f }, p);
      $("rect", { x: 0, y: 0, width: L, height: S, rx: S * 0.22, fill: f }, p);
      $("rect", { x: pad, y: pad, width: L - 2 * pad, height: S - 2 * pad, fill: "#0b0b0d" }, p);
      const n = Math.round((L - 2 * pad) / 1.15);
      for (let i = 0; i < n; i++) {
        const X = pad + (i * (L - 2 * pad)) / n + 0.2;
        $("rect", { x: X, y: pad, width: 0.5, height: S * 0.12, fill: t }, p);
        $("rect", { x: X, y: S - pad - S * 0.12, width: 0.5, height: S * 0.12, fill: t }, p);
      }
      if (on) $("rect", { x: pad, y: S / 2 - S * 0.09, width: L - 2 * pad, height: S * 0.18, fill: "#7a7c80" }, p);
      $("rect", { x: keyAt * L - 0.5, y: pad, width: 1, height: S - 2 * pad, fill: f }, p);
    };
    draw(hg, false);
    if (frac > 0) {
      const id = `bs-sc${slotN++}`;
      const cp = $("clipPath", { id }, defs);
      $("rect", { x: -2, y: -1, width: 2 + L * frac, height: S + 2 }, cp);
      draw($("g", { "clip-path": `url(#${id})` }, hg), true);
    }
  }
  // a device in a connector: a thin board standing up, one height for all
  let clipN = 0;
  function card(g, x, y, len, z0, hh, col, name) {
    box(g, x, y, len, 1, z0, hh, col);
    const m = front(g, y + 1);
    if (name) {
      // longer than the card: from its left end, cut by its edge
      const long = name.length * 1.5 > len - 3;
      const id = `bs-c${clipN++}`;
      $("rect", { x: x + 1, y: z0, width: len - 2, height: hh }, $("clipPath", { id }, SV.querySelector("defs")));
      const t = $("text", { x: long ? x + 1.5 : x + len / 2, y: 0, "font-size": 2.8, fill: "#d8d9dc", "text-anchor": long ? "start" : "middle", "font-weight": 500, transform: `translate(0 ${z0 + hh * 0.42}) scale(1 -1)` }, $("g", { "clip-path": `url(#${id})` }, m));
      t.textContent = name;
    }
  }

  // the plate
  const base = $("g", { class: "bs-part" }, SV);
  base.dataset.id = "BOARD";
  const shd = $("polygon", { fill: "#000", opacity: 0.65, filter: "url(#bs-blur)" }, base);
  R.push((c) => shd.setAttribute("points", pts(...[[0, 0], [W, 0], [W, H], [0, H]].map((q) => {
    const p = P(c, q[0], q[1], -2.5);
    return [p[0], p[1] + 6];
  }))));
  box(base, 0, 0, W, H, -2.5, 2.5, "#1f2023", "url(#bs-plate)");
  const plate = top(base, 0);
  const heat = $("g", {}, plate);
  const sceneG = $("g", {}, SV);
  const LB = $("g", {}, SV);

  // id: what the popover asks about; cx, cy: for the order in depth
  function part(id, cx, cy) {
    const g = $("g", { class: "bs-part" });
    if (id) g.dataset.id = id;
    parts.push({ g, k: cx * Math.sin(TH) + cy * Math.cos(TH) });
    return g;
  }
  function label(id, anc, route, full, short, o = {}) {
    const g = $("g", { class: "bs-part" }, LB);
    g.dataset.id = id;
    const ln = $("polyline", { class: "bs-line" }, g);
    const dot = o.dot ? $("circle", { r: 1.3, class: `bs-dot ${o.dot}` }, g) : null;
    const tx = $("text", { class: `bs-lbl${o.dim ? " dim" : ""}`, "text-anchor": o.end ? "end" : "start", "dominant-baseline": "middle" }, g);
    labels.push({ tx, full, short });
    const place = (X, Y) => {
      tx.setAttribute("x", X);
      tx.setAttribute("y", Y);
      const lines = [...tx.children];
      lines.forEach((t, i) => {
        t.setAttribute("x", X);
        t.setAttribute("y", Y + (i - (lines.length - 1) / 2) * FS * 1.25);
      });
    };
    R.push((c) => {
      const a = P(c, anc[0], anc[1], anc[2] || 0);
      const ps = route.map((p) => P(c, p[0], p[1]));
      ln.setAttribute("points", pts(a, ...ps));
      const L = ps[ps.length - 1];
      const d = o.end ? -1 : 1;
      if (dot) {
        dot.setAttribute("cx", L[0] + 3 * d);
        dot.setAttribute("cy", L[1]);
        place(L[0] + 6.5 * d, L[1]);
      } else place(L[0] + 3 * d, L[1]);
    });
  }
  // a temperature on the board: a marker (dashed: the place is approximate), a cloud, a halo
  function tempCloud(s) {
    const g = $("g", { class: "bs-part" }, LB);
    g.dataset.id = `S:${s.id}`;
    const mk = s.side !== "c" ? $("circle", { r: 1.6, "stroke-width": 0.7 }, g) : null;
    const rc = $("rect", { rx: 3.5, class: "bs-pill" }, g);
    const tx = $("text", { "font-weight": 600, "text-anchor": "middle", "dominant-baseline": "middle" }, g);
    R.push((c) => {
      const p = P(c, s.at[0], s.at[1], s.z || 0);
      const w = FS * 2.9;
      const h = FS * 1.45;
      let x = p[0];
      if (mk) {
        mk.setAttribute("cx", p[0]);
        mk.setAttribute("cy", p[1]);
        x += (s.side === "l" ? -1 : 1) * (w / 2 + 2.4);
      }
      rc.setAttribute("x", x - w / 2);
      rc.setAttribute("y", p[1] - h / 2);
      rc.setAttribute("width", w);
      rc.setAttribute("height", h);
      tx.setAttribute("x", x);
      tx.setAttribute("y", p[1] + FS * 0.08);
      tx.setAttribute("font-size", FS);
    });
    let halo = null;
    if (!s.noHalo) {
      const gr = $("radialGradient", { id: `bs-h-${s.id}` }, defs);
      halo = $("stop", { offset: 0, "stop-opacity": 0.5 }, gr);
      $("stop", { offset: 1, "stop-opacity": 0, "stop-color": "#000" }, gr);
      $("circle", { cx: s.at[0], cy: s.at[1], r: s.r || 13, fill: `url(#bs-h-${s.id})` }, heat);
    }
    const set = (v) => {
      const col = tempColor(v.temp, v.status);
      tx.textContent = `${Math.round(v.temp)}°`;
      tx.setAttribute("fill", col);
      if (mk) {
        mk.setAttribute("fill", s.approx ? "none" : col);
        mk.setAttribute("stroke", col);
        if (s.approx) mk.setAttribute("stroke-dasharray", "1.1 .9");
      }
      if (halo) halo.setAttribute("stop-color", col);
    };
    set(s);
    clouds[`S:${s.id}`] = set;
  }
  // a value beside a part, in the same cloud: at [x, y, z]; side "r": starts there; dy in font sizes
  function valueCloud(key, id, at, side, dy, text) {
    const g = $("g", { class: "bs-part" }, LB);
    g.dataset.id = id;
    const rc = $("rect", { rx: 3.5, class: "bs-pill" }, g);
    const tx = $("text", { class: "bs-pilltx", "text-anchor": "middle", "dominant-baseline": "middle", "font-weight": 600 }, g);
    tx.textContent = text;
    R.push((c) => {
      const p = P(c, at[0], at[1], at[2]);
      const fs = FS * 0.86;
      const w = tx.textContent.length * fs * 0.58 + fs * 1.1;
      const h = FS * 1.45;
      const X = side === "r" ? p[0] + w / 2 : p[0];
      const Y = p[1] + (dy || 0) * FS;
      rc.setAttribute("x", (X - w / 2).toFixed(2));
      rc.setAttribute("y", (Y - h / 2).toFixed(2));
      rc.setAttribute("width", w.toFixed(2));
      rc.setAttribute("height", h.toFixed(2));
      tx.setAttribute("x", X.toFixed(2));
      tx.setAttribute("y", (Y + fs * 0.08).toFixed(2));
      tx.setAttribute("font-size", fs.toFixed(2));
    });
    clouds[key] = (s) => {
      tx.textContent = s;
      if (cam) render();
    };
  }

  function render() {
    cam = mkCam(T);
    for (const f of R) f(cam);
    const h = heightOf(T);
    const vw = VW / Z;
    const vh = h / Z;
    PX = Math.max(-(VW - vw) / 2, Math.min((VW - vw) / 2, PX));
    PY = Math.max(-(h - vh) / 2, Math.min((h - vh) / 2, PY));
    SV.setAttribute("viewBox", [VX + VW / 2 + PX - vw / 2, PY - vh / 2, vw, vh].map((n) => Number(n.toFixed(2))).join(" "));
    SV.classList.toggle("zoomed", Z > 1);
  }
  const heightOf = (t) => 345 - 85 * t;
  // screen pixels in a unit of the board at zoom 1
  const pxScale = () => {
    const w = stage.clientWidth || 600;
    return boxH ? Math.min(w / VW, boxH / heightOf(T)) : w / VW;
  };
  function fit() {
    const w = stage.clientWidth || 600;
    const max = maxHeight();
    boxH = max ? Math.min(max, (w * heightOf(viewTo)) / VW) : null;
    stage.style.height = boxH ? `${boxH.toFixed(0)}px` : "";
    SV.classList.toggle("boxed", !!boxH);
    FS = Math.max(5.2, 7.5 / pxScale());
    LB.setAttribute("font-size", FS);
    const comp = w < 640;
    labels.forEach((l) => setLines(l.tx, comp ? l.short : l.full));
    render();
  }
  function setView(v) {
    const to = v === "iso" ? 1 : 0;
    viewTo = to;
    cancelAnimationFrame(raf);
    if (reducedMotion()) {
      T = to;
      fit();
      return;
    }
    const from = T;
    const t0 = performance.now();
    const step = (n) => {
      let k = Math.min(1, (n - t0) / 750);
      k = k < 0.5 ? 4 * k * k * k : 1 - (-2 * k + 2) ** 3 / 2;
      T = from + (to - from) * k;
      render();
      if (k < 1) raf = requestAnimationFrame(step);
      else fit();
    };
    raf = requestAnimationFrame(step);
  }

  // the popover: on hover, pinned by a click; its text is data, so textContent only
  let cur = null;
  let pinned = false;
  let mx = 0;
  let my = 0;
  function show(id) {
    const d = info(id);
    if (!d) return;
    const head = document.createElement("div");
    head.className = "bs-pt";
    head.textContent = d.title;
    const sub = document.createElement("span");
    sub.textContent = d.sub || "";
    head.append(sub);
    const rows = (d.rows || []).filter(Boolean).map(([k, v]) => {
      const r = document.createElement("div");
      r.className = "bs-pr";
      const a = document.createElement("span");
      a.textContent = k;
      const b = document.createElement("b");
      b.textContent = v;
      r.append(a, b);
      return r;
    });
    const note = d.note ? Object.assign(document.createElement("div"), { className: "bs-pn", textContent: d.note }) : null;
    pop.replaceChildren(head, ...rows, ...(note ? [note] : []));
    pop.hidden = false;
    place();
  }
  function place() {
    const r = stage.getBoundingClientRect();
    let x = mx - r.left + 14;
    let y = my - r.top + 14;
    const w = pop.offsetWidth;
    const h = pop.offsetHeight;
    if (x + w > r.width - 6) x = mx - r.left - w - 14;
    if (y + h > r.height - 6) y = my - r.top - h - 14;
    pop.style.transform = `translate(${Math.max(4, x)}px,${Math.max(4, y)}px)`;
  }
  const hide = () => {
    cur = null;
    pop.hidden = true;
  };
  // zoomed in, a drag moves the view; a press without a move stays a click
  let drag = null;
  let dragged = false;
  SV.addEventListener("pointerdown", (e) => {
    dragged = false;
    if (Z > 1 && e.button === 0) drag = { id: e.pointerId, x: e.clientX, y: e.clientY, px: PX, py: PY };
  });
  SV.addEventListener("pointermove", (e) => {
    mx = e.clientX;
    my = e.clientY;
    if (drag && e.pointerId === drag.id) {
      const dx = e.clientX - drag.x;
      const dy = e.clientY - drag.y;
      if (!dragged && Math.hypot(dx, dy) > 4) {
        dragged = true;
        SV.setPointerCapture(e.pointerId);
        if (!pinned) hide();
      }
      if (dragged) {
        const k = pxScale() * Z;
        PX = drag.px - dx / k;
        PY = drag.py - dy / k;
        render();
        return;
      }
    }
    if (!pinned && cur) place();
  });
  const dragEnd = () => {
    drag = null;
  };
  SV.addEventListener("pointerup", dragEnd);
  SV.addEventListener("pointercancel", dragEnd);
  SV.addEventListener("pointerover", (e) => {
    if (pinned || drag) return;
    const p = e.target.closest(".bs-part");
    if (p && p.dataset.id) {
      cur = p.dataset.id;
      mx = e.clientX;
      my = e.clientY;
      show(cur);
    }
  });
  SV.addEventListener("pointerout", (e) => {
    if (pinned) return;
    const p = e.target.closest(".bs-part");
    if (p && !(e.relatedTarget && p.contains(e.relatedTarget))) hide();
  });
  SV.addEventListener("click", (e) => {
    if (dragged) {
      dragged = false;
      return;
    }
    const p = e.target.closest(".bs-part");
    mx = e.clientX;
    my = e.clientY;
    if (p && p.dataset.id && !(pinned && cur === p.dataset.id)) {
      pinned = true;
      cur = p.dataset.id;
      show(cur);
    } else {
      pinned = false;
      hide();
    }
  });

  const ro = new ResizeObserver(fit);
  return {
    svg: SV, plate, part, label, tempCloud, valueCloud, box, cyl, fanGlyph, slot, card, top, left, front, only, txt,
    // everything drawn: in order of depth, then to the screen
    finish() {
      parts.sort((a, b) => a.k - b.k).forEach((p) => sceneG.appendChild(p.g));
      ro.observe(stage);
      addEventListener("resize", fit);
      fit();
    },
    setView,
    // what around the map changed its height
    refit: fit,
    zoom(dir) {
      const i = ZOOMS.indexOf(Z) + dir;
      Z = dir === 0 ? 1 : ZOOMS[Math.max(0, Math.min(ZOOMS.length - 1, i))];
      if (Z === 1) PX = PY = 0;
      render();
      return Z;
    },
    zoomState: () => ({ Z, PX, PY }),
    setZoomState(z) {
      ({ Z, PX, PY } = z);
    },
    setValue(key, v) {
      if (clouds[key]) clouds[key](v);
    },
    refreshPopover() {
      if (cur) show(cur);
    },
    pinned: () => (pinned ? cur : null),
    pin(id) {
      pinned = true;
      cur = id;
      show(id);
    },
    destroy() {
      cancelAnimationFrame(raf);
      ro.disconnect();
      removeEventListener("resize", fit);
    },
  };
}
