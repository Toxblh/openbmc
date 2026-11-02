// Updates from the releases of the firmware: which are newer than the running
// version and what changed in them. No DOM here.

const RE = /^reimu-(\d+)\.(\d+)(?:\.(\d+))?((?:-[a-zA-Z][a-zA-Z0-9.]*)*?)(?:-(\d+)-g[0-9a-f]+)?(?:-dirty)?$/;

// null: not one of our versions (reimu-webdev-20261019 and the like)
export function parseVersion(v) {
  const m = RE.exec(v || "");
  if (!m) return null;
  const pre = m[4] && m[4].slice(1).split("-").filter((w) => w !== "dirty")[0];
  const p = pre ? /^([a-zA-Z.]*)(\d*)$/.exec(pre) : null;
  return {
    nums: [+m[1], +m[2], +(m[3] || 0)],
    pre: pre ? { name: p ? p[1].toLowerCase() : pre, n: p ? +(p[2] || 0) : 0 } : null,
    commits: +(m[5] || 0),
  };
}

// <0, 0, >0; 2.0-beta < 2.0-beta2 < 2.0-rc1 < 2.0 < 2.0.1; commits on top count
export function compareVersions(a, b) {
  const x = parseVersion(a);
  const y = parseVersion(b);
  if (!x || !y) return 0;
  for (let i = 0; i < 3; i++) if (x.nums[i] !== y.nums[i]) return x.nums[i] - y.nums[i];
  if (!x.pre !== !y.pre) return x.pre ? -1 : 1;
  if (x.pre) {
    if (x.pre.name !== y.pre.name) return x.pre.name < y.pre.name ? -1 : 1;
    if (x.pre.n !== y.pre.n) return x.pre.n - y.pre.n;
  }
  return x.commits - y.commits;
}

export const isPrerelease = (v) => !!(parseVersion(v) || {}).pre;

// the release's text up to the marker: below it, the install notes of the package
export function changesOf(body) {
  return (body || "").split("<!-- ota-end -->")[0].replace(/\r\n/g, "\n").trim();
}

// newest first: not drafts, ours, with the update files, newer than running
export function newerReleases(list, running, showPre) {
  return (Array.isArray(list) ? list : [])
    .filter((r) => r && !r.draft && parseVersion(r.tag_name))
    .filter((r) => showPre || !(r.prerelease || isPrerelease(r.tag_name)))
    .filter((r) => {
      const names = (r.assets || []).map((a) => a.name);
      return names.includes(`${r.tag_name}.tar`) && names.includes(`${r.tag_name}.tar.sha256`);
    })
    .filter((r) => compareVersions(r.tag_name, running) > 0)
    .sort((a, b) => compareVersions(b.tag_name, a.tag_name));
}
