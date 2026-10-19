// NBD server running in the browser. bmcweb relays the websocket
// /vm/0/<N> to nbd-proxy, which connects the BMC kernel's nbd-client to us;
// the BMC then exposes /dev/nbd<N> to the host as a USB mass storage device.
//
// Unlike the stock OpenBMC implementation this one supports writes (WRITE,
// FLUSH, FUA, TRIM) on top of pluggable storage backends, see backends below.

/* handshake flags */
const NBD_FLAG_FIXED_NEWSTYLE = 0x1;
const NBD_FLAG_NO_ZEROES = 0x2;
/* transmission flags */
const NBD_FLAG_HAS_FLAGS = 0x1;
const NBD_FLAG_READ_ONLY = 0x2;
const NBD_FLAG_SEND_FLUSH = 0x4;
const NBD_FLAG_SEND_FUA = 0x8;
const NBD_FLAG_SEND_TRIM = 0x20;
/* options */
const NBD_OPT_EXPORT_NAME = 1;
const NBD_REP_ERR_UNSUP = (0x1 << 31) | 1;
/* commands */
const NBD_CMD_READ = 0;
const NBD_CMD_WRITE = 1;
const NBD_CMD_DISC = 2;
const NBD_CMD_FLUSH = 3;
const NBD_CMD_TRIM = 4;
const NBD_CMD_FLAG_FUA = 1;
/* errno */
const EPERM = 1;
const EIO = 5;
const EINVAL = 22;
const ENOSPC = 28;

const MAGIC_REQ = 0x25609513;
const MAGIC_REPLY = 0x67446698;
const MAX_REQUEST = 32 * 1024 * 1024;
const MAX_PARALLEL_READS = 4;

/* ------------------------------------------------------------------ */
/* Backends: {size, readOnly, read(off,len), write(off,data), flush(),  */
/*            trim(off,len), close(), describe()}                       */

// Read-only access to a File/Blob (classic virtual media).
export class BlobBackend {
  constructor(blob, name) {
    this.blob = blob;
    this.name = name || blob.name || "image";
    this.size = blob.size;
    this.readOnly = true;
  }
  async read(off, len) {
    return new Uint8Array(await this.blob.slice(off, off + len).arrayBuffer());
  }
  async write() {
    throw Object.assign(new Error("read-only"), { errno: EPERM });
  }
  async flush() {}
  async trim() {}
  async close() {}
  describe() {
    return "только чтение";
  }
}

// Copy-on-write overlay in RAM on top of another backend. Used both for
// "snapshot" mode (host may write, changes stay in the browser and can be
// downloaded) and as a write cache for local files that are saved back with
// a single FileSystemWritableFileStream transaction.
const BLOCK = 64 * 1024;
export class OverlayBackend {
  constructor(base, { limit = 1024 * 1024 * 1024, name } = {}) {
    this.base = base;
    this.name = name || base.name;
    this.size = base.size;
    this.readOnly = false;
    this.blocks = new Map(); // block index -> Uint8Array(BLOCK)
    this.limit = limit;
    this.dirty = new Set();
  }
  get overlayBytes() {
    return this.blocks.size * BLOCK;
  }
  async _block(i) {
    let b = this.blocks.get(i);
    if (!b) {
      if (this.blocks.size * BLOCK >= this.limit) throw Object.assign(new Error("overlay is full"), { errno: ENOSPC });
      const off = i * BLOCK;
      const len = Math.min(BLOCK, this.size - off);
      b = new Uint8Array(BLOCK);
      b.set(await this.base.read(off, len));
      this.blocks.set(i, b);
    }
    return b;
  }
  async read(off, len) {
    const out = new Uint8Array(len);
    let pos = 0;
    while (pos < len) {
      const abs = off + pos;
      const i = Math.floor(abs / BLOCK);
      const inb = abs % BLOCK;
      const n = Math.min(len - pos, BLOCK - inb);
      const b = this.blocks.get(i);
      if (b) out.set(b.subarray(inb, inb + n), pos);
      else {
        // read a run of consecutive non-overlaid blocks in one go
        let end = pos + n;
        while (end < len && !this.blocks.has(Math.floor((off + end) / BLOCK))) end = Math.min(len, end + BLOCK - ((off + end) % BLOCK));
        out.set(await this.base.read(abs, end - pos), pos);
        pos = end;
        continue;
      }
      pos += n;
    }
    return out;
  }
  async write(off, data) {
    let pos = 0;
    while (pos < data.length) {
      const abs = off + pos;
      const i = Math.floor(abs / BLOCK);
      const inb = abs % BLOCK;
      const n = Math.min(data.length - pos, BLOCK - inb);
      let b;
      if (inb === 0 && n === BLOCK && !this.blocks.has(i)) {
        if (this.blocks.size * BLOCK >= this.limit) throw Object.assign(new Error("overlay is full"), { errno: ENOSPC });
        b = new Uint8Array(BLOCK); // full block overwrite: no need to read base
        this.blocks.set(i, b);
      } else b = await this._block(i);
      b.set(data.subarray(pos, pos + n), inb);
      this.dirty.add(i);
      pos += n;
    }
  }
  async trim(off, len) {
    // zero-fill only whole blocks we already have; trim is advisory
    for (let i = Math.ceil(off / BLOCK); (i + 1) * BLOCK <= off + len; i++) {
      if (this.blocks.has(i)) {
        this.blocks.get(i).fill(0);
        this.dirty.add(i);
      }
    }
  }
  async flush() {}
  async close() {}
  describe() {
    return `снимок в памяти, изменено ${this.dirty.size * 64} КиБ`;
  }
  // Writes dirty blocks to a FileSystemFileHandle (same size as image).
  async saveTo(handle, onProgress) {
    if (!this.dirty.size) return 0;
    const w = await handle.createWritable({ keepExistingData: true });
    let done = 0;
    const idx = [...this.dirty].sort((a, b) => a - b);
    try {
      for (const i of idx) {
        const off = i * BLOCK;
        const len = Math.min(BLOCK, this.size - off);
        await w.write({ type: "write", position: off, data: this.blocks.get(i).subarray(0, len) });
        if (onProgress) onProgress(++done, idx.length);
      }
      await w.close();
    } catch (e) {
      await w.abort().catch(() => {});
      throw e;
    }
    this.dirty.clear();
    return idx.length * BLOCK;
  }
  // Full image with all modifications, for download.
  async toBlob() {
    const parts = [];
    for (let off = 0; off < this.size; off += 16 * BLOCK) {
      parts.push(await this.read(off, Math.min(16 * BLOCK, this.size - off)));
    }
    return new Blob(parts, { type: "application/octet-stream" });
  }
}

// Local file opened with File System Access API in read-write mode
// (Chromium). Writes go to the overlay and are committed to the file by
// flush (NBD FLUSH from the host's sync/umount) and on disconnect.
export class FileHandleBackend extends OverlayBackend {
  static async open(handle) {
    const file = await handle.getFile();
    const be = new FileHandleBackend(new BlobBackend(file), { name: file.name, limit: 512 * 1024 * 1024 });
    be.handle = handle;
    return be;
  }
  async write(off, data) {
    await super.write(off, data);
    // Every commit rewrites the file, so commit in big batches, but don't
    // let unsaved data grow without bound if the host never flushes.
    if (this.dirty.size * BLOCK >= FileHandleBackend.AUTOSAVE) await this.flush();
  }
  async flush() {
    // re-read base lazily from the new file version after commit
    const n = await this.saveTo(this.handle);
    if (n) {
      this.base = new BlobBackend(await this.handle.getFile());
      this.blocks.clear();
    }
  }
  async close() {
    await this.flush();
  }
  static AUTOSAVE = 256 * 1024 * 1024;
  describe() {
    return `чтение/запись в локальный файл${this.dirty.size ? `, не сохранено ${this.dirty.size * 64} КиБ` : ""}`;
  }
}

// Image stored in the browser's Origin Private File System, accessed through
// a worker with FileSystemSyncAccessHandle: true in-place random writes.
export class OpfsBackend {
  static async open(name, { readOnly = false } = {}) {
    const be = new OpfsBackend();
    be.name = name;
    be.readOnly = readOnly;
    be.worker = new Worker(new URL("./opfs-worker.js", import.meta.url));
    be.seq = 0;
    be.pending = new Map();
    be.worker.onmessage = (ev) => {
      const { id, ok, result, error } = ev.data;
      const p = be.pending.get(id);
      if (!p) return;
      be.pending.delete(id);
      if (ok) p.resolve(result);
      else p.reject(Object.assign(new Error(error), { errno: EIO }));
    };
    be.size = await be._call("open", { name });
    return be;
  }
  _call(op, args = {}, transfer = []) {
    return new Promise((resolve, reject) => {
      const id = ++this.seq;
      this.pending.set(id, { resolve, reject });
      this.worker.postMessage({ id, op, ...args }, transfer);
    });
  }
  async read(off, len) {
    return new Uint8Array(await this._call("read", { off, len }));
  }
  async write(off, data) {
    const copy = data.slice();
    await this._call("write", { off, data: copy.buffer }, [copy.buffer]);
  }
  async trim() {}
  async flush() {
    await this._call("flush");
  }
  async close() {
    await this._call("close").catch(() => {});
    this.worker.terminate();
  }
  describe() {
    return this.readOnly ? "образ в браузере, только чтение" : "образ в браузере, чтение/запись";
  }
}

/* ------------------------------------------------------------------ */

export class NbdServer extends EventTarget {
  constructor(ws, backend, { readOnly } = {}) {
    super();
    this.ws = ws;
    this.backend = backend;
    this.readOnly = readOnly || backend.readOnly;
    this.state = "open";
    this.buf = new Uint8Array(0);
    this.queue = [];
    this.inflight = 0;
    this.writing = false;
    this.stats = { reads: 0, writes: 0, flushes: 0, readBytes: 0, writtenBytes: 0, errors: 0 };
    ws.binaryType = "arraybuffer";
    ws.addEventListener("open", () => this._negotiate());
    ws.addEventListener("message", (ev) => this._onMessage(new Uint8Array(ev.data)));
    ws.addEventListener("close", (ev) => this._onClose(ev));
    if (ws.readyState === WebSocket.OPEN) this._negotiate();
  }

  _emit(type, detail = {}) {
    this.dispatchEvent(new CustomEvent(type, { detail }));
  }

  stop() {
    if (this.ws.readyState <= WebSocket.OPEN) this.ws.close();
  }

  async _onClose(ev) {
    this.state = "closed";
    try {
      await this.backend.flush();
    } catch (e) {
      this._emit("error", { message: `не удалось сохранить изменения: ${e.message}` });
    }
    this._emit("close", { code: ev.code, reason: ev.reason });
  }

  _send(buf) {
    if (this.ws.readyState === WebSocket.OPEN) this.ws.send(buf);
  }

  _negotiate() {
    if (this.state !== "open") return;
    const b = new DataView(new ArrayBuffer(18));
    b.setUint32(0, 0x4e42444d); // NBDMAGIC
    b.setUint32(4, 0x41474943);
    b.setUint32(8, 0x49484156); // IHAVEOPT
    b.setUint32(12, 0x454f5054);
    b.setUint16(16, NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES);
    this.state = "cflags";
    this._send(b.buffer);
  }

  _onMessage(data) {
    if (this.buf.length) {
      const n = new Uint8Array(this.buf.length + data.length);
      n.set(this.buf);
      n.set(data, this.buf.length);
      this.buf = n;
    } else this.buf = data;

    for (;;) {
      const used = this._handle(this.buf);
      if (used < 0) {
        this._emit("error", { message: "ошибка протокола NBD" });
        this.stop();
        return;
      }
      if (used === 0) break;
      this.buf = this.buf.subarray(used);
      if (!this.buf.length) {
        this.buf = new Uint8Array(0);
        break;
      }
    }
  }

  _handle(buf) {
    const v = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
    if (this.state === "cflags") {
      if (buf.length < 4) return 0;
      this.clientFlags = v.getUint32(0);
      this.state = "option";
      return 4;
    }
    if (this.state === "option") {
      if (buf.length < 16) return 0;
      if (v.getUint32(0) !== 0x49484156 || v.getUint32(4) !== 0x454f5054) return -1;
      const opt = v.getUint32(8);
      const len = v.getUint32(12);
      if (buf.length < 16 + len) return 0;
      if (opt === NBD_OPT_EXPORT_NAME) {
        const n = this.clientFlags & NBD_FLAG_NO_ZEROES ? 10 : 134;
        const r = new DataView(new ArrayBuffer(n));
        const size = this.backend.size;
        r.setUint32(0, Math.floor(size / 2 ** 32));
        r.setUint32(4, size >>> 0);
        let flags = NBD_FLAG_HAS_FLAGS;
        if (this.readOnly) flags |= NBD_FLAG_READ_ONLY;
        else flags |= NBD_FLAG_SEND_FLUSH | NBD_FLAG_SEND_FUA | NBD_FLAG_SEND_TRIM;
        r.setUint16(8, flags);
        this._send(r.buffer);
        this.state = "transmission";
        this._emit("connected", { readOnly: this.readOnly, size });
      } else {
        // reject anything else (e.g. NBD_OPT_GO): nbd-client falls back to
        // NBD_OPT_EXPORT_NAME
        const r = new DataView(new ArrayBuffer(20));
        r.setUint32(0, 0x0003e889);
        r.setUint32(4, 0x045565a9);
        r.setUint32(8, opt);
        r.setUint32(12, NBD_REP_ERR_UNSUP >>> 0);
        r.setUint32(16, 0);
        this._send(r.buffer);
      }
      return 16 + len;
    }
    if (this.state === "transmission") {
      if (buf.length < 28) return 0;
      if (v.getUint32(0) !== MAGIC_REQ) return -1;
      const req = {
        flags: v.getUint16(4),
        type: v.getUint16(6),
        handle: buf.slice(8, 16),
        offset: v.getUint32(16) * 2 ** 32 + v.getUint32(20),
        length: v.getUint32(24),
      };
      let used = 28;
      if (req.type === NBD_CMD_WRITE) {
        if (req.length > MAX_REQUEST) return -1;
        if (buf.length < 28 + req.length) return 0;
        req.data = buf.slice(28, 28 + req.length);
        used += req.length;
      }
      this.queue.push(req);
      this._pump();
      return used;
    }
    return -1;
  }

  // Ordering rules: reads may run in parallel; a write/flush/trim waits for
  // everything queued before it and blocks everything after it, so the host
  // always reads back what it has written.
  _pump() {
    while (this.queue.length) {
      const req = this.queue[0];
      const barrier = req.type !== NBD_CMD_READ;
      if (this.writing) return;
      if (barrier && this.inflight > 0) return;
      if (!barrier && this.inflight >= MAX_PARALLEL_READS) return;
      this.queue.shift();
      if (barrier) this.writing = true;
      this.inflight++;
      this._exec(req).finally(() => {
        this.inflight--;
        if (barrier) this.writing = false;
        this._pump();
      });
    }
  }

  _reply(req, err, data) {
    const r = new Uint8Array(16 + (data ? data.length : 0));
    const v = new DataView(r.buffer);
    v.setUint32(0, MAGIC_REPLY);
    v.setUint32(4, err);
    r.set(req.handle, 8);
    if (data) r.set(data, 16);
    this._send(r.buffer);
    if (err) this.stats.errors++;
  }

  _check(req) {
    if (req.offset + req.length > this.backend.size) return ENOSPC;
    return 0;
  }

  async _exec(req) {
    try {
      switch (req.type) {
        case NBD_CMD_READ: {
          const e = this._check(req);
          if (e) return this._reply(req, e);
          const data = await this.backend.read(req.offset, req.length);
          this.stats.reads++;
          this.stats.readBytes += req.length;
          this._reply(req, 0, data);
          break;
        }
        case NBD_CMD_WRITE: {
          if (this.readOnly) return this._reply(req, EPERM);
          const e = this._check(req);
          if (e) return this._reply(req, e);
          await this.backend.write(req.offset, req.data);
          if (req.flags & NBD_CMD_FLAG_FUA) await this.backend.flush();
          this.stats.writes++;
          this.stats.writtenBytes += req.length;
          this._reply(req, 0);
          break;
        }
        case NBD_CMD_FLUSH:
          await this.backend.flush();
          this.stats.flushes++;
          this._reply(req, 0);
          break;
        case NBD_CMD_TRIM:
          if (this.readOnly) return this._reply(req, EPERM);
          await this.backend.trim(req.offset, req.length);
          this._reply(req, 0);
          break;
        case NBD_CMD_DISC:
          this.stop();
          break;
        default:
          this._reply(req, EINVAL);
      }
    } catch (e) {
      console.error("nbd:", e);
      this._reply(req, e.errno || EIO);
      this._emit("error", { message: e.message });
    } finally {
      this._emit("stats", this.stats);
    }
  }
}
