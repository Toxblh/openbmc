// Virtualized view of timestamped serial lines. Renders only visible rows,
// so tens of thousands of lines stay responsive on any machine.

import { fmtTime } from "../api.js";

const ROW = 18;

export class LogView {
  constructor(container, opts = {}) {
    this.el = container;
    this.el.classList.add("logview");
    this.el.tabIndex = 0;
    this.inner = document.createElement("div");
    this.inner.className = "logview-inner";
    this.el.append(this.inner);
    this.lines = [];
    this.partial = null;
    this.view = []; // indexes into lines (after filter)
    this.filter = "";
    this.onlyHits = false;
    this.tz = null; // null = browser local time
    this.showDate = opts.showDate !== false;
    this.follow = true;
    this.pool = [];
    this.onFollowChange = opts.onFollowChange || (() => {});
    this._raf = 0;
    this.el.addEventListener("scroll", () => {
      const atBottom = this.el.scrollTop + this.el.clientHeight >= this.el.scrollHeight - ROW;
      if (atBottom !== this.follow) {
        this.follow = atBottom;
        this.onFollowChange(this.follow);
      }
      this._schedule();
    });
    this._ro = new ResizeObserver(() => this._schedule());
    this._ro.observe(this.el);
  }

  destroy() {
    this._ro.disconnect();
    cancelAnimationFrame(this._raf);
  }

  setLines(lines, partial) {
    this.lines = lines;
    this.partial = partial;
    this._rebuild();
  }

  // lines array was appended to (and maybe trimmed from the head)
  update(partial) {
    this.partial = partial;
    this._rebuild();
  }

  setFilter(text, onlyHits) {
    this.filter = text.toLowerCase();
    this.onlyHits = onlyHits;
    this._rebuild();
    if (this.filter && !this.onlyHits) {
      // jump to the most recent match
      for (let i = this.lines.length - 1; i >= 0; i--) {
        if (this._hit(this.lines[i])) {
          this.scrollToRow(i);
          break;
        }
      }
    }
  }

  setTz(tz) {
    this.tz = tz;
    this._render(true);
  }

  scrollToBottom() {
    this.follow = true;
    this.onFollowChange(true);
    this.el.scrollTop = this.el.scrollHeight;
    this._schedule();
  }

  scrollToRow(row) {
    this.follow = false;
    this.onFollowChange(false);
    this.el.scrollTop = Math.max(0, row * ROW - this.el.clientHeight / 2);
  }

  _hit(l) {
    return this.filter && l && l.text.toLowerCase().includes(this.filter);
  }

  _rebuild() {
    if (this.filter && this.onlyHits) {
      this.view = [];
      for (let i = 0; i < this.lines.length; i++) if (this._hit(this.lines[i])) this.view.push(i);
    } else {
      this.view = null; // identity
    }
    const rows = this._count();
    this.inner.style.height = `${rows * ROW}px`;
    if (this.follow) this.el.scrollTop = this.el.scrollHeight;
    this._schedule(true);
  }

  _count() {
    const n = this.view ? this.view.length : this.lines.length;
    return n + (this.partial && !(this.filter && this.onlyHits) ? 1 : 0);
  }

  _lineAt(row) {
    const n = this.view ? this.view.length : this.lines.length;
    if (row < n) return this.lines[this.view ? this.view[row] : row];
    return this.partial;
  }

  _schedule(force) {
    if (force) this._force = true;
    if (this._raf) return;
    this._raf = requestAnimationFrame(() => {
      this._raf = 0;
      this._render(this._force);
      this._force = false;
    });
  }

  _render(force) {
    const top = this.el.scrollTop;
    const h = this.el.clientHeight;
    const first = Math.max(0, Math.floor(top / ROW) - 5);
    const last = Math.min(this._count(), Math.ceil((top + h) / ROW) + 5);
    const need = last - first;
    while (this.pool.length < need) {
      const row = document.createElement("div");
      row.className = "logline";
      const ts = document.createElement("span");
      ts.className = "ts";
      const tx = document.createElement("span");
      tx.className = "text";
      row.append(ts, tx);
      this.inner.append(row);
      this.pool.push({ row, ts, tx, key: null });
    }
    for (let i = 0; i < this.pool.length; i++) {
      const p = this.pool[i];
      const r = first + i;
      if (i >= need) {
        if (p.row.style.display !== "none") p.row.style.display = "none";
        p.key = null;
        continue;
      }
      const l = this._lineAt(r);
      const key = `${r}|${l ? l.t : ""}|${l ? l.text.length : 0}`;
      if (!force && p.key === key) continue;
      p.key = key;
      p.row.style.display = "";
      p.row.style.top = `${r * ROW}px`;
      p.ts.textContent = l && l.t ? fmtTime(l.t, this.tz, this.showDate) : "";
      p.tx.textContent = l ? l.text : "";
      p.row.classList.toggle("hit", !!this._hit(l));
      p.row.classList.toggle("partial", !!(l && l.partial));
    }
  }

  // Plain text export with timestamps in the selected time zone.
  toText() {
    const out = this.lines.map((l) => `[${fmtTime(l.t, this.tz, true)}] ${l.text}`);
    if (this.partial) out.push(`[${fmtTime(this.partial.t, this.tz, true)}] ${this.partial.text}`);
    return out.join("\n") + "\n";
  }
}
