// node test-build.mjs <folder> <out.img>: builds the FAT32 image of a folder
// with FatDirBackend and writes it (sparse) to a file.
import fs from "node:fs";
import path from "node:path";
import { FatDirBackend } from "../../meta-common/recipes-phosphor/reimu-webui/files/www/js/lib/fatdir.js";

const [, , dir, out] = process.argv;
const entries = [];
const walk = async (d, rel) => {
  for (const de of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, de.name);
    const r = rel ? `${rel}/${de.name}` : de.name;
    if (de.isDirectory()) {
      entries.push({ path: r, dir: true });
      await walk(p, r);
    } else {
      const st = fs.statSync(p);
      const blob = await fs.openAsBlob(p);
      entries.push({ path: r, file: new File([blob], de.name, { lastModified: st.mtimeMs }) });
    }
  }
};
await walk(dir, "");
const be = new FatDirBackend(entries, { name: path.basename(dir) });
console.log(be.describe(), "size", be.size, "cluster", be.clusterSize, "clusters", be.clusters, "skipped", be.skipped);
const fd = fs.openSync(out, "w");
fs.ftruncateSync(fd, be.size);
const CH = 4 * 1024 * 1024;
// metadata + all runs; free space stays sparse
const ranges = [[0, (2048 + be.dataStart) * 512]];
for (const r of be.runs) ranges.push([(2048 + be.dataStart) * 512 + (r.start - 2) * be.clusterSize, r.count * be.clusterSize]);
for (const [o, l] of ranges)
  for (let p = 0; p < l; p += CH) {
    const n = Math.min(CH, l - p);
    fs.writeSync(fd, await be.read(o + p, n), 0, n, o + p);
  }
fs.closeSync(fd);
// random-offset reads must equal the written image
const img = fs.readFileSync(out);
for (let i = 0; i < 300; i++) {
  const o = Math.floor(Math.random() * Math.min(be.size, ranges.at(-1)[0] + ranges.at(-1)[1] + 1e6));
  const l = 1 + Math.floor(Math.random() * 200000);
  const a = await be.read(o, Math.min(l, be.size - o));
  if (Buffer.compare(Buffer.from(a), img.subarray(o, o + a.length))) throw new Error(`mismatch at ${o}+${l}`);
}
console.log("random reads ok");
