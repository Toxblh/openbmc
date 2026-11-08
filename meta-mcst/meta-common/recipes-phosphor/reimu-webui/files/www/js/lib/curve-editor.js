// A fan curve as a graph: temperature (°C) along the bottom, duty (%) up
// the side, its points dragged by mouse, pen or finger (pointer events).
// A point sticks to a multiple of 5 within 1.5 of it, else whole numbers;
// it stays between its neighbours (temperatures rising, duties not
// falling), not below the floor; the last point is always 100 %, moved
// only sideways. Double click (tap) on empty space adds a point on the
// curve there, on a point removes it (not the last; at least 2 stay).
//
// readOnly: only shown (no handles, no dragging); band: [lo, hi] °C shaded
// (holding a temperature: the curve is then only the safety); nowDuty: the
// duty the current temperature has now, when it isn't the curve's (holding).
//
//   const ed = curveEditor({ points: [[45, 30], [75, 100]], floor: 30, temp: 52.1, onChange(points) {} });
//   el.append(ed.el); ed.set(points)

const NS = "http://www.w3.org/2000/svg";
export const GEOM = { w: 560, h: 260, l: 40, r: 14, t: 12, b: 30, t0: 20, t1: 90 };
GEOM.pw = GEOM.w - GEOM.l - GEOM.r;
GEOM.ph = GEOM.h - GEOM.t - GEOM.b;
const MAX_POINTS = 8;
const STICK = 1.5;

export function stick(v) {
  const s = Math.round(v / 5) * 5;
  return Math.abs(v - s) <= STICK ? s : Math.round(v);
}

const svgEl = (tag, attrs = {}, text) => {
  const e = document.createElementNS(NS, tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  if (text !== undefined) e.textContent = text;
  return e;
};
const X = (t) => GEOM.l + ((t - GEOM.t0) / (GEOM.t1 - GEOM.t0)) * GEOM.pw;
const Y = (d) => GEOM.t + (1 - d / 100) * GEOM.ph;
const T = (x) => GEOM.t0 + ((x - GEOM.l) / GEOM.pw) * (GEOM.t1 - GEOM.t0);
const D = (y) => (1 - (y - GEOM.t) / GEOM.ph) * 100;

function evalCurve(pts, t) {
  if (t <= pts[0][0]) return pts[0][1];
  for (let i = 1; i < pts.length; i++)
    if (t <= pts[i][0]) return pts[i - 1][1] + ((t - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0])) * (pts[i][1] - pts[i - 1][1]);
  return 100;
}

export function curveEditor({ points, floor = 30, temp = null, onChange = () => {}, readOnly = false, band = null, nowDuty = null }) {
  let pts = points.map((p) => [...p]);
  let drag = null; // { i, id }
  const svg = svgEl("svg", { viewBox: `0 0 ${GEOM.w} ${GEOM.h}`, class: `curve-editor${readOnly ? " readonly" : ""}`, role: "img", style: readOnly ? "" : "touch-action: none", "aria-label": "График вентилятора: скорость от температуры" });
  const grid = svgEl("g", { class: "ce-grid" });
  // the floor: no duty below it
  grid.append(svgEl("rect", { x: GEOM.l, y: Y(floor), width: GEOM.pw, height: Y(0) - Y(floor), class: "ce-below" }));
  for (let t = GEOM.t0; t <= GEOM.t1; t += 10) {
    grid.append(svgEl("line", { x1: X(t), x2: X(t), y1: GEOM.t, y2: Y(0) }));
    const lastT = t + 10 > GEOM.t1;
    grid.append(svgEl("text", { x: X(t), y: GEOM.h - 10, "text-anchor": lastT ? "end" : "middle", class: "ce-tick" }, lastT ? `${t} °C` : `${t}`));
  }
  for (let d = 0; d <= 100; d += 20) {
    grid.append(svgEl("line", { x1: GEOM.l, x2: GEOM.l + GEOM.pw, y1: Y(d), y2: Y(d) }));
    grid.append(svgEl("text", { x: GEOM.l - 6, y: Y(d) + 4, "text-anchor": "end", class: "ce-tick" }, `${d}`));
  }
  grid.append(svgEl("text", { x: GEOM.l - 6, y: GEOM.t - 2, "text-anchor": "end", class: "ce-tick" }, "%"));
  if (band) grid.append(svgEl("rect", { x: X(band[0]), y: GEOM.t, width: Math.max(2, X(band[1]) - X(band[0])), height: Y(0) - GEOM.t, class: "ce-band" }));
  const now = svgEl("g", { class: "ce-now" });
  const line = svgEl("polyline", { class: "ce-line" });
  const handles = svgEl("g");
  const tip = svgEl("text", { class: "ce-tip", "text-anchor": "middle" });
  svg.append(grid, now, line, handles, tip);

  // graph units per CSS pixel: handles, labels and the reach of a finger
  // keep their size on a narrow screen
  const k = () => Math.min(2.2, Math.max(1, GEOM.w / (svg.getBoundingClientRect().width || GEOM.w)));
  function draw() {
    const kk = k();
    svg.style.setProperty("--ce-font", `${11 * kk}px`);
    line.setAttribute("points", [[GEOM.t0, pts[0][1]], ...pts, [GEOM.t1, 100]].map(([t, d]) => `${X(t)},${Y(d)}`).join(" "));
    svg.dataset.curve = pts.map(([t, d]) => `${t}:${d}`).join(" ");
    handles.replaceChildren(
      ...pts.map(([t, d], i) =>
        readOnly ? svgEl("circle", { cx: X(t), cy: Y(d), r: 3.5 * kk, class: "pt-ro" }) : svgEl("circle", { cx: X(t), cy: Y(d), r: 7 * kk, class: `pt${drag && drag.i === i ? " on" : ""}`, "data-i": i }),
      ),
    );
    now.replaceChildren();
    if (temp !== null && temp !== undefined && temp >= GEOM.t0 && temp <= GEOM.t1) {
      now.append(svgEl("line", { x1: X(temp), x2: X(temp), y1: GEOM.t, y2: Y(0) }));
      now.append(svgEl("circle", { cx: X(temp), cy: Y(nowDuty !== null && nowDuty !== undefined ? nowDuty : evalCurve(pts, temp)), r: 4 * k() }));
    }
    if (drag) {
      const [t, d] = pts[drag.i];
      tip.textContent = `${t} °C · ${d} %`;
      tip.setAttribute("x", Math.min(Math.max(X(t), GEOM.l + 50), GEOM.l + GEOM.pw - 50));
      tip.setAttribute("y", Y(d) > GEOM.t + 30 ? Y(d) - 14 : Y(d) + 24);
    } else tip.textContent = "";
  }

  // pointer -> graph coordinates
  function local(e) {
    const p = svg.createSVGPoint();
    p.x = e.clientX;
    p.y = e.clientY;
    return p.matrixTransform(svg.getScreenCTM().inverse());
  }
  function nearest(q, within = 18 * k()) {
    let best = -1, bd = within;
    pts.forEach(([t, d], i) => {
      const dist = Math.hypot(X(t) - q.x, Y(d) - q.y);
      if (dist <= bd) (bd = dist), (best = i);
    });
    return best;
  }
  function moveTo(i, q) {
    const last = i === pts.length - 1;
    const tlo = i ? pts[i - 1][0] + 1 : GEOM.t0, thi = last ? GEOM.t1 : pts[i + 1][0] - 1;
    const dlo = i ? Math.max(floor, pts[i - 1][1]) : floor, dhi = last ? 100 : pts[i + 1][1];
    const t = Math.min(thi, Math.max(tlo, stick(T(q.x))));
    const d = last ? 100 : Math.min(dhi, Math.max(dlo, stick(D(q.y))));
    pts[i] = [t, d];
  }

  // a double click or tap, told by time and place (a browser sends no
  // dblclick after a pointerdown it was told to leave alone)
  let lastDown = null;
  function double(q) {
    const i = nearest(q, 14 * k());
    if (i >= 0) {
      if (i === pts.length - 1 || pts.length <= 2) return;
      pts.splice(i, 1);
    } else {
      if (pts.length >= MAX_POINTS) return;
      const t = Math.round(T(q.x));
      if (t <= GEOM.t0 || t >= pts[pts.length - 1][0] || pts.some((p) => Math.abs(p[0] - t) < 1)) return;
      const d = Math.max(floor, Math.round(evalCurve(pts, t)));
      pts.push([t, d]);
      pts.sort((a, b) => a[0] - b[0]);
    }
    draw();
    onChange(pts.map((p) => [...p]));
  }
  svg.addEventListener("pointerdown", (e) => {
    if (readOnly) return;
    const q = local(e);
    const at = performance.now();
    if (lastDown && at - lastDown.at < 400 && Math.hypot(q.x - lastDown.x, q.y - lastDown.y) < 14 * k()) {
      e.preventDefault();
      lastDown = null;
      return double(q);
    }
    lastDown = { at, x: q.x, y: q.y };
    const i = nearest(q);
    if (i < 0) return;
    e.preventDefault();
    drag = { i, id: e.pointerId };
    try {
      svg.setPointerCapture(e.pointerId);
    } catch (_) {}
    svg.classList.add("dragging");
    draw();
  });
  svg.addEventListener("pointermove", (e) => {
    if (!drag || e.pointerId !== drag.id) return;
    e.preventDefault();
    moveTo(drag.i, local(e));
    draw();
  });
  const end = (e) => {
    if (!drag || e.pointerId !== drag.id) return;
    drag = null;
    svg.classList.remove("dragging");
    draw();
    onChange(pts.map((p) => [...p]));
  };
  svg.addEventListener("pointerup", end);
  svg.addEventListener("pointercancel", end);
  if (typeof ResizeObserver === "function") new ResizeObserver(() => drag || draw()).observe(svg);
  draw();
  return {
    el: svg,
    get points() {
      return pts.map((p) => [...p]);
    },
    set(p) {
      if (drag) return;
      pts = p.map((x) => [...x]);
      draw();
    },
  };
}
