// Bar of keys phone keyboards lack (Esc, Tab, Ctrl, arrows, F1-F12...),
// shared by the KVM and the serial console. The first row fits a phone
// screen; the second one is swiped sideways. A tap presses a key without
// taking the focus (and the phone keyboard) from the field being typed
// into; a swipe only scrolls.

import { h } from "../api.js";

// rows: [[label, key], ...] per row; held: {key: bool} of sticky modifiers
export function keyBar(rows, onKey, held = {}) {
  const bar = h(
    "div",
    { class: "keybar hidden" },
    rows.map((row) => h("div", { class: "keybar-row" }, row.map(([label, k]) => h("button", { class: "btn small", "data-k": k }, label)))),
  );
  const refresh = () => bar.querySelectorAll("[data-k]").forEach((b) => b.classList.toggle("active", !!held[b.dataset.k]));
  const press = (k) => {
    onKey(k);
    refresh();
  };
  let t0 = null;
  bar.addEventListener(
    "touchstart",
    (e) => {
      const p = e.touches[0];
      t0 = e.touches.length === 1 ? { x: p.clientX, y: p.clientY } : null;
    },
    { passive: true },
  );
  bar.addEventListener(
    "touchmove",
    (e) => {
      const p = e.touches[0];
      if (t0 && Math.hypot(p.clientX - t0.x, p.clientY - t0.y) > 8) t0 = null;
    },
    { passive: true },
  );
  bar.addEventListener(
    "touchend",
    (e) => {
      const b = e.target.closest("button[data-k]");
      if (!b) return;
      // no click and no focus change follow a cancelled touchend
      e.preventDefault();
      if (t0) press(b.dataset.k);
      t0 = null;
    },
    { passive: false },
  );
  // mouse: keep the focus where it is, press on click
  bar.addEventListener("mousedown", (e) => e.preventDefault());
  bar.addEventListener("click", (e) => {
    const b = e.target.closest("button[data-k]");
    if (b) press(b.dataset.k);
  });
  bar.refresh = refresh;
  return bar;
}

export const ARROWS = [
  ["Esc", "Escape"],
  ["Tab", "Tab"],
  ["Ctrl", "ctrl"],
  ["Alt", "alt"],
  ["←", "ArrowLeft"],
  ["↑", "ArrowUp"],
  ["↓", "ArrowDown"],
  ["→", "ArrowRight"],
];
export const FKEYS = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12].map((n) => [`F${n}`, `F${n}`]);
