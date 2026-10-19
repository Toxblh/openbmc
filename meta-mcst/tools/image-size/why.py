#!/usr/bin/env python3
"""why.py depends.dot installed-package-sizes.txt PKG...: who pulls PKG into the image
(reverse RDEPENDS tree from buildhistory, depth $D, default 3)."""
import re, sys
dot, sizes, targets = sys.argv[1], sys.argv[2], sys.argv[3:]
edges = {}; rev = {}
for line in open(dot):
    m = re.match(r'\s*"([^"]+)"\s*->\s*"([^"]+)"', line)
    if m:
        a, b = m.groups(); edges.setdefault(a, set()).add(b); rev.setdefault(b, set()).add(a)
size = {}
for line in open(sizes):
    f = line.split()
    if len(f) >= 3: size[f[2]] = int(f[0])
def tree(p, depth=0, seen=None, maxd=4):
    seen = seen or set()
    parents = sorted(rev.get(p, []))
    for q in parents:
        print("  " * (depth + 1) + "<- " + q)
        if q not in seen and depth < maxd:
            seen.add(q); tree(q, depth + 1, seen, maxd)
for t in targets:
    print(f"== {t} ({size.get(t, 0)} KiB)")
    tree(t, maxd=int(__import__('os').environ.get("D", "3")))
