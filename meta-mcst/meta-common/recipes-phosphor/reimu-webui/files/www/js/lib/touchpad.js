// KVM touchpad mode for touch screens: the screen works like a laptop
// touchpad instead of clicking where the finger lands. One finger moves the
// cursor (noVNC's local copy of the host's one; faster swipes move it
// further), a tap clicks, tap and then touch again and slide drags, a two-finger tap is the right button, three
// fingers the middle one, two fingers sliding scroll. The pointer goes to
// noVNC as if it were a mouse over the canvas.

const TAP_MS = 250;
const TAP_SLOP = 8; // px a tap may wander
const DOUBLE_MS = 300;
const SCROLL_STEP = 24; // px of two-finger travel per wheel click
const BTN = { left: 1, middle: 2, right: 4, wheelUp: 8, wheelDown: 16 };

export function touchpad(screen, getRfb) {
  let enabled = false;
  let pos = { x: 0.5, y: 0.5 }; // fraction of the canvas
  let g = null; // current gesture
  let lastTap = 0;

  const canvas = () => {
    const r = getRfb();
    return r && r._rfbConnectionState === "connected" ? screen.querySelector("canvas") : null;
  };
  const at = (c) => ({ x: pos.x * c.clientWidth, y: pos.y * c.clientHeight });
  const draw = () => {
    const c = canvas();
    if (!enabled || !c) return;
    const cr = c.getBoundingClientRect();
    const p = at(c);
    const cur = getRfb()._cursor;
    if (cur) cur.move(cr.left + p.x, cr.top + p.y);
  };
  const move = () => {
    const c = canvas();
    if (!c) return;
    const p = at(c);
    getRfb()._handleMouseMove(p.x, p.y);
    draw();
  };
  const button = (mask) => {
    const c = canvas();
    if (!c) return;
    const p = at(c);
    getRfb()._handleMouseButton(p.x, p.y, mask);
  };
  const click = (b) => {
    button(b);
    button(0);
  };

  const centre = (ts) => {
    let x = 0, y = 0;
    for (const t of ts) {
      x += t.clientX;
      y += t.clientY;
    }
    return { x: x / ts.length, y: y / ts.length };
  };

  const ours = (e) => enabled && canvas() && !e.target.closest("button");

  const start = (e) => {
    if (!ours(e)) return;
    e.preventDefault();
    e.stopPropagation();
    const now = performance.now();
    const c = centre(e.touches);
    if (!g) g = { t0: now, fingers: 0, moved: false, dragArmed: now - lastTap < DOUBLE_MS, dragging: false, lx: c.x, ly: c.y, lt: now, sx: c.x, sy: c.y, scroll: 0 };
    g.fingers = Math.max(g.fingers, e.touches.length);
    // the centre jumps when a finger is added: measure from here
    g.lx = c.x;
    g.ly = c.y;
    if (!g.moved) {
      g.sx = c.x;
      g.sy = c.y;
    }
  };

  const moveEv = (e) => {
    if (!g || !ours(e)) return;
    e.preventDefault();
    e.stopPropagation();
    const now = performance.now();
    const c = centre(e.touches);
    const dx = c.x - g.lx, dy = c.y - g.ly;
    const dt = Math.max(1, now - g.lt);
    g.lx = c.x;
    g.ly = c.y;
    g.lt = now;
    if (!g.moved && Math.hypot(c.x - g.sx, c.y - g.sy) > TAP_SLOP) g.moved = true;
    if (!g.moved) return;
    if (e.touches.length >= 2) {
      // natural scrolling: the content follows the fingers
      g.scroll += dy;
      while (Math.abs(g.scroll) >= SCROLL_STEP) {
        const up = g.scroll > 0;
        click(up ? BTN.wheelUp : BTN.wheelDown);
        g.scroll -= up ? SCROLL_STEP : -SCROLL_STEP;
      }
      return;
    }
    if (g.fingers > 1) return; // a finger left after a two-finger gesture
    const cv = canvas();
    if (g.dragArmed && !g.dragging) {
      g.dragging = true;
      button(BTN.left);
    }
    const speed = Math.hypot(dx, dy) / dt; // px per ms
    const gain = Math.min(3, 1.2 + speed * 0.8);
    pos.x = Math.min(1, Math.max(0, pos.x + (dx * gain) / cv.clientWidth));
    pos.y = Math.min(1, Math.max(0, pos.y + (dy * gain) / cv.clientHeight));
    move();
  };

  const end = (e) => {
    if (!g || !enabled) return;
    e.preventDefault();
    e.stopPropagation();
    if (e.touches.length) {
      // wait for every finger; the centre jumps when one is lifted
      const c = centre(e.touches);
      g.lx = c.x;
      g.ly = c.y;
      return;
    }
    const now = performance.now();
    if (g.dragging) button(0);
    else if (!g.moved && now - g.t0 < TAP_MS * (g.fingers > 1 ? 2 : 1)) {
      if (g.fingers === 1) {
        click(BTN.left);
        lastTap = now;
      } else click(g.fingers === 2 ? BTN.right : BTN.middle);
    }
    if (g.fingers !== 1 || g.moved) lastTap = 0;
    g = null;
  };

  const cancel = () => {
    if (g && g.dragging) button(0);
    g = null;
  };

  const opts = { capture: true, passive: false };
  screen.addEventListener("touchstart", start, opts);
  screen.addEventListener("touchmove", moveEv, opts);
  screen.addEventListener("touchend", end, opts);
  screen.addEventListener("touchcancel", cancel, opts);
  const ro = new ResizeObserver(draw);
  ro.observe(screen);
  const redraw = setInterval(draw, 500); // canvas resizes with the host's video mode

  return {
    set(on) {
      enabled = on;
      cancel();
      draw();
    },
    get enabled() {
      return enabled;
    },
    connected() {
      draw();
    },
    destroy() {
      ro.disconnect();
      clearInterval(redraw);
    },
  };
}
