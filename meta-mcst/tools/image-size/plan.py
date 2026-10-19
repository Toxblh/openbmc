#!/usr/bin/env python3
"""plan.py BUILDHISTORY_IMAGE_DIR PKGDATA_DIR SIZES.tsv SCALE PLAN.txt

Estimates how much a squashfs rootfs shrinks when packages are removed.

  SIZES.tsv  per-file sizes of the image rootfs (path, raw, xz estimate), see xzsizes.py
  SCALE      real squashfs size / sum of the xz estimates (fragments, dedup)
  PLAN.txt   steps, one per line:  <step name>: <root package> [<root package>...]
             (globs allowed); steps are cumulative: each removes its roots plus everything only
             they (and earlier steps) needed.
"""
import fnmatch, json, os, re, sys

bh, pkgdata, sizes_tsv, scale, plan = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4]), sys.argv[5]

installed = set(open(os.path.join(bh, "installed-package-names.txt")).read().split())
deps = {}
for line in open(os.path.join(bh, "depends.dot")):
    m = re.match(r'\s*"([^"]+)"\s*->\s*"([^"]+)"', line)
    if m and m.group(1) in installed and m.group(2) in installed:
        deps.setdefault(m.group(1), set()).add(m.group(2))
parents = {}
for a, bs in deps.items():
    for b in bs:
        parents.setdefault(b, set()).add(a)
roots = {p for p in installed if not parents.get(p)}

comp = {}
for line in open(sizes_tsv):
    p, n, c = line.rstrip("\n").split("\t")
    comp["/" + p] = int(c)
pkgsize = {}
owned = set()
for p in installed:
    f = os.path.join(pkgdata, "runtime-reverse", p)  # by the installed (renamed) name
    if not os.path.exists(f):
        continue
    for line in open(f):
        if line.startswith("FILES_INFO:"):
            files = json.loads(line.split(":", 1)[1])
            pkgsize[p] = sum(comp.get(path, 0) for path in files)
            owned |= set(files)
total = sum(comp.values())
unowned = sum(c for path, c in comp.items() if path not in owned)


def reachable(start):
    seen, todo = set(), list(start)
    while todo:
        p = todo.pop()
        if p in seen:
            continue
        seen.add(p)
        todo.extend(deps.get(p, ()))
    return seen


MiB = 1048576
print(f"rootfs now: {total * scale / MiB:.2f} MiB squashfs (estimate), {len(installed)} packages, "
      f"{unowned * scale / MiB:.2f} MiB in files of no package (generated at do_rootfs)")
removed_roots, gone_before = set(), set()
for line in open(plan):
    line = line.split("#", 1)[0].strip()
    if not line:
        continue
    name, pkgs = line.split(":", 1)
    pkgs = [m for pat in pkgs.split() for m in (fnmatch.filter(sorted(installed), pat) or [pat])]  # globs allowed
    missing = [p for p in pkgs if p not in installed]
    removed_roots |= set(p for p in pkgs if p in installed)
    keep = reachable(roots - removed_roots)  # a removed package still needed by a kept one stays
    gone = installed - keep
    step = gone - gone_before
    saved = sum(pkgsize.get(p, 0) for p in step)
    left = total - sum(pkgsize.get(p, 0) for p in gone)
    stuck = [p for p in pkgs if p in keep]
    big = sorted(step, key=lambda p: -pkgsize.get(p, 0))[:8]
    print(f"\n{name}: -{saved * scale / MiB:.2f} MiB -> {left * scale / MiB:.2f} MiB")
    print("   " + ", ".join(f"{p} {pkgsize.get(p, 0) * scale / 1024:.0f}K" for p in big) + (" ..." if len(step) > 8 else ""))
    if stuck:
        print("   STILL NEEDED (pulled by others): " + ", ".join(f"{p} <- {' '.join(sorted(parents.get(p, ()))[:4])}" for p in stuck))
    if missing:
        print("   not installed: " + " ".join(missing))
    gone_before = gone
