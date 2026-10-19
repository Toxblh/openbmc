#!/usr/bin/env python3
"""xzsizes.py ROOTFS OUT.tsv — per-file size and estimated squashfs (xz, 128 KiB blocks) size."""
import lzma, os, sys
from concurrent.futures import ProcessPoolExecutor
root, out = sys.argv[1], sys.argv[2]
BS = 131072
def est(p):
    try:
        st = os.lstat(p)
    except OSError:
        return p, 0, 0
    if not os.path.isfile(p) or os.path.islink(p):
        return p, 0, 0
    n = st.st_size; c = 0
    with open(p, "rb") as f:
        while True:
            b = f.read(BS)
            if not b: break
            z = len(lzma.compress(b, format=lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA2, "preset": 6, "dict_size": BS}]))
            c += min(z, len(b))
    return p, n, c
files = [os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs]
with ProcessPoolExecutor() as ex, open(out, "w") as o:
    for p, n, c in ex.map(est, files, chunksize=32):
        o.write(f"{os.path.relpath(p, root)}\t{n}\t{c}\n")
