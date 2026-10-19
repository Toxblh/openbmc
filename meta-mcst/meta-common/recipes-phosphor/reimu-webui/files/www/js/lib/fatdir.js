// A local folder served to the host as a USB flash drive: a FAT32 volume
// (MBR, one partition) synthesized on the fly, like QEMU's vvfat.
//
// Only the metadata is built in memory (boot sector, FSInfo, directories and
// the cluster runs the FAT is computed from); file data is read from the
// File objects when the host reads it. Every file and directory gets one
// contiguous run of clusters, so the FAT is a sorted list of runs.
//
// The backend itself is read-only; a writable drive is an OverlayBackend on
// top of it, and changedFiles() reads the volume back through the overlay to
// find the files the host created or modified.

const SECTOR = 512;
const PART_START = 2048; // sectors: 1 MiB, the usual alignment
const RESERVED = 32; // sectors before the FATs (boot sector, FSInfo, backup at 6)
const EOC = 0x0fffffff;
const MAX_FILE = 0xffffffff; // FAT32 file size limit (4 GiB - 1)
const MIN_CLUSTERS = 65525 + 16; // below this a volume is FAT16 by definition

/* ------------------------------------------------------------------ */
/* collecting files                                                    */

// From a FileSystemDirectoryHandle (Chromium): [{path, file}]
export async function entriesFromHandle(dir, onProgress) {
  const out = [];
  const walk = async (h, prefix) => {
    for await (const [name, child] of h.entries()) {
      const path = prefix ? `${prefix}/${name}` : name;
      if (child.kind === "directory") {
        out.push({ path, dir: true });
        await walk(child, path);
      } else {
        out.push({ path, file: await child.getFile() });
        if (onProgress && out.length % 200 === 0) onProgress(out.length);
      }
    }
  };
  await walk(dir, "");
  return out;
}

// From <input type=file webkitdirectory> (all browsers): the top folder name
// is stripped from webkitRelativePath. Empty folders are not reported by
// browsers this way, so they don't appear.
export function entriesFromFileList(files) {
  const out = [];
  for (const f of files) {
    const rel = f.webkitRelativePath || f.name;
    const path = rel.includes("/") ? rel.slice(rel.indexOf("/") + 1) : rel;
    if (path) out.push({ path, file: f });
  }
  return out;
}

/* ------------------------------------------------------------------ */
/* names                                                               */

const SFN_OK = /[A-Z0-9!#$%&'()\-@^_`{}~]/;

function sfnPart(s, n) {
  let r = "";
  for (const ch of s.toUpperCase()) {
    if (ch === " " || ch === ".") continue;
    r += SFN_OK.test(ch) ? ch : "_";
    if (r.length === n) break;
  }
  return r;
}

// Unique 8.3 name (11 bytes, space padded) for a long name in one directory.
function shortName(long, used) {
  const dot = long.lastIndexOf(".");
  const base = dot > 0 ? long.slice(0, dot) : long;
  const ext = dot > 0 ? long.slice(dot + 1) : "";
  const b = sfnPart(base, 8) || "_";
  const e = sfnPart(ext, 3);
  const pad = (s, n) => (s + "        ").slice(0, n);
  const exact = b === base && e === ext && base.length <= 8 && ext.length <= 3;
  let cand = pad(b, 8) + pad(e, 3);
  if (exact && !used.has(cand)) {
    used.add(cand);
    return cand;
  }
  for (let i = 1; i < 1e6; i++) {
    const tail = `~${i}`;
    cand = pad(b.slice(0, 8 - tail.length) + tail, 8) + pad(e, 3);
    if (!used.has(cand)) {
      used.add(cand);
      return cand;
    }
  }
  throw new Error("too many similar names");
}

function sfnChecksum(sfn) {
  let s = 0;
  for (let i = 0; i < 11; i++) s = (((s & 1) << 7) + (s >> 1) + sfn.charCodeAt(i)) & 0xff;
  return s;
}

function fatDateTime(ms) {
  const d = new Date(ms || Date.now());
  const y = Math.min(Math.max(d.getFullYear(), 1980), 2107);
  return {
    date: ((y - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate(),
    time: (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1),
  };
}

// Directory entries (LFN + 8.3) for one child.
function dirEntries(name, sfn, attr, cluster, size, mtime) {
  const units = [];
  for (let i = 0; i < name.length; i++) units.push(name.charCodeAt(i));
  const nLfn = Math.ceil((units.length + 1) / 13);
  const out = new Uint8Array((nLfn + 1) * 32);
  const dv = new DataView(out.buffer);
  const sum = sfnChecksum(sfn);
  units.push(0);
  while (units.length % 13) units.push(0xffff);
  const OFF = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30];
  for (let k = 0; k < nLfn; k++) {
    const seq = nLfn - k; // entries go last part first
    const e = k * 32;
    out[e] = seq | (k === 0 ? 0x40 : 0);
    out[e + 11] = 0x0f;
    out[e + 13] = sum;
    for (let j = 0; j < 13; j++) dv.setUint16(e + OFF[j], units[(seq - 1) * 13 + j], true);
  }
  const e = nLfn * 32;
  for (let i = 0; i < 11; i++) out[e + i] = sfn.charCodeAt(i);
  out[e + 11] = attr;
  const { date, time } = fatDateTime(mtime);
  dv.setUint16(e + 14, time, true);
  dv.setUint16(e + 16, date, true);
  dv.setUint16(e + 18, date, true);
  dv.setUint16(e + 20, cluster >>> 16, true);
  dv.setUint16(e + 22, time, true);
  dv.setUint16(e + 24, date, true);
  dv.setUint16(e + 26, cluster & 0xffff, true);
  dv.setUint32(e + 28, size, true);
  return out;
}

function dotEntry(name, cluster, mtime) {
  const sfn = (name + "           ").slice(0, 11);
  const out = new Uint8Array(32);
  const dv = new DataView(out.buffer);
  for (let i = 0; i < 11; i++) out[i] = sfn.charCodeAt(i);
  out[11] = 0x10;
  const { date, time } = fatDateTime(mtime);
  dv.setUint16(14, time, true);
  dv.setUint16(16, date, true);
  dv.setUint16(18, date, true);
  dv.setUint16(20, cluster >>> 16, true);
  dv.setUint16(22, time, true);
  dv.setUint16(24, date, true);
  dv.setUint16(26, cluster & 0xffff, true);
  return out;
}

/* ------------------------------------------------------------------ */
/* the volume                                                          */

export class FatDirBackend {
  // entries: [{path, file} | {path, dir: true}]; opts: {name, label, headroom}
  constructor(entries, { name = "folder", label, headroom } = {}) {
    this.name = name;
    this.readOnly = true;
    this.skipped = []; // [{path, reason}]
    this._build(entries, label || name, headroom);
  }

  _build(entries, label, headroom) {
    // tree
    const root = { name: "", children: new Map(), dir: true, mtime: Date.now() };
    for (const en of entries) {
      const parts = en.path.split("/").filter(Boolean);
      if (!parts.length) continue;
      if (parts.some((p) => p.length > 255)) {
        this.skipped.push({ path: en.path, reason: "имя длиннее 255 символов" });
        continue;
      }
      let d = root;
      for (let i = 0; i < parts.length - 1; i++) {
        let c = d.children.get(parts[i]);
        if (!c) d.children.set(parts[i], (c = { name: parts[i], children: new Map(), dir: true, mtime: en.file ? en.file.lastModified : Date.now() }));
        if (!c.dir) break;
        d = c;
      }
      const leaf = parts[parts.length - 1];
      if (en.dir) {
        if (!d.children.has(leaf)) d.children.set(leaf, { name: leaf, children: new Map(), dir: true, mtime: Date.now() });
      } else if (en.file.size > MAX_FILE) {
        this.skipped.push({ path: en.path, reason: "больше 4 ГиБ (предел FAT32)" });
      } else {
        d.children.set(leaf, { name: leaf, dir: false, file: en.file, size: en.file.size, mtime: en.file.lastModified });
      }
    }

    // directory sizes in bytes (entries + end marker), files in bytes
    const dirs = [];
    const files = [];
    let dataBytes = 0;
    const visit = (d, isRoot) => {
      dirs.push(d);
      let bytes = isRoot ? 32 : 64; // volume label / "." and ".."
      for (const c of d.children.values()) {
        bytes += (Math.ceil((c.name.length + 1) / 13) + 1) * 32;
        if (c.dir) visit(c, false);
        else {
          files.push(c);
          dataBytes += c.size;
        }
      }
      d.bytes = bytes + 32;
    };
    visit(root, true);
    this.fileCount = files.length;
    this.dirCount = dirs.length - 1;
    this.dataBytes = dataBytes;

    // cluster size by volume size (Microsoft's FAT32 defaults)
    const meta = dirs.reduce((a, d) => a + d.bytes, 0);
    const room = headroom !== undefined ? headroom : Math.max(256 * 2 ** 20, Math.min(dataBytes, 4 * 2 ** 30));
    const target = dataBytes + meta + room;
    const spc = target <= 260 * 2 ** 20 ? 1 : target <= 8 * 2 ** 30 ? 8 : target <= 16 * 2 ** 30 ? 16 : target <= 32 * 2 ** 30 ? 32 : 64;
    const csize = spc * SECTOR;
    this.clusterSize = csize;

    // allocate: directories first (root = cluster 2), then files, contiguous runs
    this.runs = []; // {start, count, dir?: Uint8Array, file?: File, node}
    let next = 2;
    for (const d of dirs) {
      const n = Math.max(1, Math.ceil(d.bytes / csize));
      d.cluster = next;
      d.nclusters = n;
      next += n;
    }
    for (const f of files) {
      if (!f.size) {
        f.cluster = 0;
        continue;
      }
      const n = Math.ceil(f.size / csize);
      f.cluster = next;
      this.runs.push({ start: next, count: n, file: f.file, node: f });
      next += n;
    }
    const used = next - 2;
    const clusters = Math.max(MIN_CLUSTERS, used + Math.ceil(room / csize));
    this.clusters = clusters;
    this.freeClusters = clusters - used;
    this.nextFree = next;

    // directory contents (now that every child has a cluster)
    for (const d of dirs) {
      const buf = new Uint8Array(d.nclusters * csize);
      let pos = 0;
      if (d === root) {
        const lbl = (label.toUpperCase().replace(/[^A-Z0-9 _-]/g, "_") || "REIMU").slice(0, 11);
        const le = dotEntry(lbl, 0, Date.now());
        le[11] = 0x08;
        le.fill(0, 20, 22);
        le.fill(0, 26, 28);
        buf.set(le, 0);
        pos = 32;
        this.label = lbl;
      } else {
        buf.set(dotEntry(".", d.cluster, d.mtime), 0);
        buf.set(dotEntry("..", d.parent === root ? 0 : d.parent.cluster, d.mtime), 32);
        pos = 64;
      }
      const usedNames = new Set();
      for (const c of d.children.values()) {
        if (c.dir) c.parent = d;
        const sfn = shortName(c.name, usedNames);
        const ents = dirEntries(c.name, sfn, c.dir ? 0x10 : 0x20, c.dir ? c.cluster : c.cluster, c.dir ? 0 : c.size, c.mtime);
        buf.set(ents, pos);
        pos += ents.length;
      }
      this.runs.push({ start: d.cluster, count: d.nclusters, dir: buf, node: d });
    }
    // parent links are set while filling the parent, which precedes children
    // in visit order; ".." of a child was written with d.parent: fix order
    this.runs.sort((a, b) => a.start - b.start);
    for (const r of this.runs) {
      if (r.dir && r.node !== root) {
        const p = r.node.parent;
        const cl = p === root ? 0 : p.cluster;
        const dv = new DataView(r.dir.buffer);
        dv.setUint16(32 + 20, cl >>> 16, true);
        dv.setUint16(32 + 26, cl & 0xffff, true);
      }
    }
    this.root = root;

    // geometry
    const fatBytes = (clusters + 2) * 4;
    this.fatSectors = Math.ceil(fatBytes / SECTOR);
    this.fatStart = RESERVED;
    this.dataStart = RESERVED + 2 * this.fatSectors;
    this.spc = spc;
    this.partSectors = this.dataStart + clusters * spc;
    this.size = (PART_START + this.partSectors) * SECTOR;
    this._boot = this._bootSector();
    this._fsinfo = this._fsInfo();
    this._mbr = this._mbrSector();
  }

  _mbrSector() {
    const s = new Uint8Array(SECTOR);
    const dv = new DataView(s.buffer);
    dv.setUint32(440, 0x52454d55, true); // disk signature "UMER"
    const p = 446;
    s[p] = 0x00;
    s.set([0xfe, 0xff, 0xff], p + 1); // CHS start: use LBA
    s[p + 4] = 0x0c; // FAT32 LBA
    s.set([0xfe, 0xff, 0xff], p + 5);
    dv.setUint32(p + 8, PART_START, true);
    dv.setUint32(p + 12, this.partSectors, true);
    s[510] = 0x55;
    s[511] = 0xaa;
    return s;
  }

  _bootSector() {
    const s = new Uint8Array(SECTOR);
    const dv = new DataView(s.buffer);
    s.set([0xeb, 0x58, 0x90], 0);
    s.set(new TextEncoder().encode("REIMUFAT"), 3);
    dv.setUint16(11, SECTOR, true);
    s[13] = this.spc;
    dv.setUint16(14, RESERVED, true);
    s[16] = 2; // FATs
    s[21] = 0xf8; // fixed disk
    dv.setUint16(24, 63, true); // sectors per track (CHS, informative)
    dv.setUint16(26, 255, true); // heads
    dv.setUint32(28, PART_START, true); // hidden sectors
    dv.setUint32(32, this.partSectors, true);
    dv.setUint32(36, this.fatSectors, true);
    dv.setUint32(44, 2, true); // root cluster
    dv.setUint16(48, 1, true); // FSInfo sector
    dv.setUint16(50, 6, true); // backup boot sector
    s[64] = 0x80; // drive number
    s[66] = 0x29; // extended boot signature
    dv.setUint32(67, (Date.now() / 1000) >>> 0, true); // volume serial
    s.set(new TextEncoder().encode((this.label + "           ").slice(0, 11)), 71);
    s.set(new TextEncoder().encode("FAT32   "), 82);
    s[510] = 0x55;
    s[511] = 0xaa;
    return s;
  }

  _fsInfo() {
    const s = new Uint8Array(SECTOR);
    const dv = new DataView(s.buffer);
    dv.setUint32(0, 0x41615252, true);
    dv.setUint32(484, 0x61417272, true);
    dv.setUint32(488, this.freeClusters, true);
    dv.setUint32(492, this.nextFree, true);
    dv.setUint32(508, 0xaa550000, true);
    return s;
  }

  // run containing cluster c, or null (free)
  _run(c) {
    const r = this.runs;
    let lo = 0;
    let hi = r.length - 1;
    while (lo <= hi) {
      const m = (lo + hi) >> 1;
      if (c < r[m].start) hi = m - 1;
      else if (c >= r[m].start + r[m].count) lo = m + 1;
      else return r[m];
    }
    return null;
  }

  _fatSector(i) {
    const s = new Uint8Array(SECTOR);
    const dv = new DataView(s.buffer);
    const first = i * (SECTOR / 4);
    for (let k = 0; k < SECTOR / 4; k++) {
      const c = first + k;
      let v = 0;
      if (c === 0) v = 0x0ffffff8;
      else if (c === 1) v = EOC;
      else if (c < this.clusters + 2) {
        const r = this._run(c);
        if (r) v = c === r.start + r.count - 1 ? EOC : c + 1;
      } else break;
      dv.setUint32(k * 4, v, true);
    }
    return s;
  }

  // one sector of the partition, or null for "zeros" / data handled by caller
  _metaSector(ps) {
    if (ps === 0 || ps === 6) return this._boot;
    if (ps === 1 || ps === 7) return this._fsinfo;
    if (ps === 2 || ps === 8) {
      const s = new Uint8Array(SECTOR); // boot sector #2 of FAT32: just the signature
      s[510] = 0x55;
      s[511] = 0xaa;
      return s;
    }
    if (ps >= this.fatStart && ps < this.dataStart) return this._fatSector((ps - this.fatStart) % this.fatSectors);
    return null;
  }

  async read(off, len) {
    const out = new Uint8Array(len);
    let pos = 0;
    while (pos < len) {
      const abs = off + pos;
      const sec = Math.floor(abs / SECTOR);
      if (sec < PART_START) {
        const n = Math.min(len - pos, SECTOR - (abs % SECTOR));
        if (sec === 0) out.set(this._mbr.subarray(abs % SECTOR, (abs % SECTOR) + n), pos);
        pos += n;
        continue;
      }
      const ps = sec - PART_START;
      if (ps < this.dataStart) {
        const n = Math.min(len - pos, SECTOR - (abs % SECTOR));
        const m = this._metaSector(ps);
        if (m) out.set(m.subarray(abs % SECTOR, (abs % SECTOR) + n), pos);
        pos += n;
        continue;
      }
      // data region: by cluster runs, so a long read of one file is one slice
      const dataOff = abs - (PART_START + this.dataStart) * SECTOR;
      const c = 2 + Math.floor(dataOff / this.clusterSize);
      const r = this._run(c);
      if (!r) {
        // free space up to the next run
        const nr = this.runs.find((x) => x.start > c);
        const until = nr ? (PART_START + this.dataStart) * SECTOR + (nr.start - 2) * this.clusterSize : this.size;
        pos += Math.min(len - pos, until - abs);
        continue;
      }
      const runOff = dataOff - (r.start - 2) * this.clusterSize;
      const runLen = r.count * this.clusterSize;
      const n = Math.min(len - pos, runLen - runOff);
      if (r.dir) out.set(r.dir.subarray(runOff, runOff + n), pos);
      else if (runOff < r.file.size) {
        const end = Math.min(r.file.size, runOff + n);
        out.set(new Uint8Array(await r.file.slice(runOff, end).arrayBuffer()), pos);
      }
      pos += n;
    }
    return out;
  }

  async write() {
    throw Object.assign(new Error("read-only"), { errno: 1 });
  }
  async flush() {}
  async trim() {}
  async close() {}
  describe() {
    return `папка «${this.name}» как FAT32: ${this.fileCount} файлов, ${this.dirCount} папок`;
  }
}

/* ------------------------------------------------------------------ */
/* reading the volume back (after the host wrote to it via an overlay) */

// Walks the FAT32 volume of `dev` (any backend with read()), returns
// [{path, size, mtime, read: () => Promise<Uint8Array>}] for every file.
export async function listVolume(dev) {
  const mbr = await dev.read(0, SECTOR);
  const mdv = new DataView(mbr.buffer);
  const pstart = mdv.getUint32(446 + 8, true);
  const bs = await dev.read(pstart * SECTOR, SECTOR);
  const dv = new DataView(bs.buffer);
  const bps = dv.getUint16(11, true);
  const spc = bs[13];
  const reserved = dv.getUint16(14, true);
  const nfats = bs[16];
  const fatSz = dv.getUint32(36, true);
  const rootCl = dv.getUint32(44, true);
  const base = pstart * bps;
  const csize = bps * spc;
  const dataOff = base + (reserved + nfats * fatSz) * bps;
  const fatOff = base + reserved * bps;
  const fatCache = new Map();
  const fatEntry = async (c) => {
    const sec = Math.floor((c * 4) / bps);
    let s = fatCache.get(sec);
    if (!s) fatCache.set(sec, (s = await dev.read(fatOff + sec * bps, bps)));
    return new DataView(s.buffer).getUint32((c * 4) % bps, true) & 0x0fffffff;
  };
  const chain = async (c) => {
    const out = [];
    for (let n = 0; c >= 2 && c < 0x0ffffff8 && n < 1 << 28; n++) {
      out.push(c);
      c = await fatEntry(c);
    }
    return out;
  };
  const readChain = async (start, size) => {
    const cl = await chain(start);
    const buf = new Uint8Array(size !== undefined ? size : cl.length * csize);
    let pos = 0;
    for (const c of cl) {
      if (pos >= buf.length) break;
      const n = Math.min(csize, buf.length - pos);
      buf.set(await dev.read(dataOff + (c - 2) * csize, n), pos);
      pos += n;
    }
    return buf;
  };
  const files = [];
  const walk = async (cl, prefix, depth) => {
    if (depth > 64) return;
    const d = await readChain(cl);
    const ddv = new DataView(d.buffer);
    let lfn = [];
    for (let e = 0; e + 32 <= d.length; e += 32) {
      const first = d[e];
      if (first === 0) break;
      if (first === 0xe5) {
        lfn = [];
        continue;
      }
      const attr = d[e + 11];
      if (attr === 0x0f) {
        const part = [];
        for (const o of [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]) part.push(ddv.getUint16(e + o, true));
        lfn.unshift(part);
        continue;
      }
      let name;
      if (lfn.length) {
        const u = lfn.flat();
        const end = u.indexOf(0);
        name = String.fromCharCode(...(end >= 0 ? u.slice(0, end) : u.filter((x) => x !== 0xffff)));
      } else {
        const raw = String.fromCharCode(...d.subarray(e, e + 11));
        const b = raw.slice(0, 8).trimEnd();
        const x = raw.slice(8).trimEnd();
        name = x ? `${b}.${x}` : b;
        if (d[e + 12] & 0x08) name = name.replace(/^[^.]*/, (m) => m.toLowerCase());
        if (d[e + 12] & 0x10) name = name.replace(/\.[^.]*$/, (m) => m.toLowerCase());
      }
      lfn = [];
      if (attr & 0x08) continue; // volume label
      if (name === "." || name === "..") continue;
      const start = (ddv.getUint16(e + 20, true) << 16) | ddv.getUint16(e + 26, true);
      const date = ddv.getUint16(e + 24, true);
      const time = ddv.getUint16(e + 22, true);
      const mtime = new Date(1980 + (date >> 9), ((date >> 5) & 15) - 1, date & 31, time >> 11, (time >> 5) & 63, (time & 31) * 2).getTime();
      const path = prefix ? `${prefix}/${name}` : name;
      if (attr & 0x10) {
        if (start >= 2) await walk(start, path, depth + 1);
      } else {
        const size = ddv.getUint32(e + 28, true);
        files.push({ path, size, mtime, start, read: () => (size ? readChain(start, size) : Promise.resolve(new Uint8Array(0))) });
      }
    }
  };
  await walk(rootCl, "", 0);
  return files;
}

async function sameContent(f, file) {
  const now = await f.read();
  const was = new Uint8Array(await file.arrayBuffer());
  if (now.length !== was.length) return false;
  for (let i = 0; i < now.length; i++) if (now[i] !== was[i]) return false;
  return true;
}

// Files on `dev` (an OverlayBackend over `fat`) that differ from the folder
// the volume was built from: [{path, size, mtime, status: "new"|"changed",
// read}], plus the paths that were deleted.
export async function changedFiles(fat, dev) {
  const orig = new Map();
  const walk = (d, prefix) => {
    for (const c of d.children.values()) {
      const p = prefix ? `${prefix}/${c.name}` : c.name;
      if (c.dir) walk(c, p);
      else orig.set(p, c);
    }
  };
  walk(fat.root, "");
  const now = await listVolume(dev);
  const dirtyClusters = new Set();
  if (dev.dirty) {
    const blk = 64 * 1024;
    const dataStart = (PART_START + fat.dataStart) * SECTOR;
    for (const b of dev.dirty) {
      const from = Math.max(0, b * blk - dataStart);
      const to = (b + 1) * blk - dataStart;
      if (to <= 0) continue;
      for (let c = 2 + Math.floor(from / fat.clusterSize); c < 2 + Math.ceil(to / fat.clusterSize); c++) dirtyClusters.add(c);
    }
  }
  const changed = [];
  const seen = new Set();
  for (const f of now) {
    seen.add(f.path);
    const o = orig.get(f.path);
    if (!o) {
      changed.push({ ...f, status: "new" });
      continue;
    }
    let touched = o.size !== f.size || o.cluster !== f.start;
    if (!touched && f.size) {
      // the overlay tracks 64 KiB blocks, shared by many small files: a
      // file in a dirty block is compared with the original byte for byte
      const n = Math.ceil(f.size / fat.clusterSize);
      let suspect = false;
      for (let c = f.start; c < f.start + n && !suspect; c++) suspect = dirtyClusters.has(c);
      if (suspect) touched = !(await sameContent(f, o.file));
    }
    if (touched) changed.push({ ...f, status: "changed" });
  }
  const deleted = [...orig.keys()].filter((p) => !seen.has(p));
  return { changed, deleted };
}
