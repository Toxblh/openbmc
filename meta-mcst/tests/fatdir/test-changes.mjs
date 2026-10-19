// node test-changes.mjs <folder> <orig.img> <modified.img>: what changedFiles()
// finds when the host turned orig into modified (dirty = differing 64K blocks)
import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { FatDirBackend, changedFiles } from "../../meta-common/recipes-phosphor/reimu-webui/files/www/js/lib/fatdir.js";
const [, , dir, origPath, modPath] = process.argv;
const entries = [];
const walk = async (d, rel) => {
  for (const de of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, de.name), r = rel ? `${rel}/${de.name}` : de.name;
    if (de.isDirectory()) { entries.push({ path: r, dir: true }); await walk(p, r); }
    else entries.push({ path: r, file: new File([await fs.openAsBlob(p)], de.name, { lastModified: fs.statSync(p).mtimeMs }) });
  }
};
await walk(dir, "");
// same folder, same build => same layout as orig.img (label/serial aside)
const fat = new FatDirBackend(entries, { name: path.basename(dir) });
const orig = fs.readFileSync(origPath), mod = fs.readFileSync(modPath);
const BLK = 65536, dirty = new Set();
for (let b = 0; b * BLK < mod.length; b++) if (Buffer.compare(orig.subarray(b * BLK, (b + 1) * BLK), mod.subarray(b * BLK, (b + 1) * BLK))) dirty.add(b);
const dev = { dirty, read: async (o, l) => new Uint8Array(mod.subarray(o, o + l)) };
const { changed, deleted } = await changedFiles(fat, dev);
for (const f of changed) console.log(f.status, f.path, f.size, crypto.createHash("md5").update(await f.read()).digest("hex"));
console.log("deleted:", deleted);
