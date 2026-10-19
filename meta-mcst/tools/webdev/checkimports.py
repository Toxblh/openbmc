#!/usr/bin/env python3
"""Checks that every symbol a devkit binary imports is exported by the
libraries it needs on the BMC (the dynamic linker would refuse to start it
otherwise: "symbol lookup error").

Usage: checkimports.py <BMC rootfs> <binary>...
"""
import os, re, subprocess, sys

root, bins = sys.argv[1], sys.argv[2:]
cache = {}


def readelf(*args):
    return subprocess.run(["readelf", "-W", *args], capture_output=True, text=True, check=True).stdout


def exports(soname):
    if soname not in cache:
        path = next((p for p in (os.path.join(root, d, soname) for d in ("lib", "usr/lib")) if os.path.exists(p)), None)
        syms = set()
        if path:
            for line in readelf("--dyn-syms", path).splitlines():
                f = line.split()
                if len(f) >= 8 and f[0].endswith(":") and f[6] != "UND" and f[4] in ("GLOBAL", "WEAK"):
                    syms.add(re.sub(r"@.*", "", f[7]))
        cache[soname] = (path, syms)
    return cache[soname]


bad = 0
for b in bins:
    needed = re.findall(r"Shared library: \[(.*?)\]", readelf("-d", b))
    avail = set()
    for n in needed:
        path, syms = exports(n)
        if not path:
            print(f"{b}: {n} is not on the BMC")
            bad += 1
        avail |= syms
    for line in readelf("--dyn-syms", b).splitlines():
        f = line.split()
        if len(f) >= 8 and f[0].endswith(":") and f[6] == "UND" and f[4] == "GLOBAL":
            if re.sub(r"@.*", "", f[7]) not in avail:
                print(f"{b}: {f[7]} is not exported by {' '.join(needed)} on the BMC")
                bad += 1
sys.exit(1 if bad else 0)
