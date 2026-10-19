#!/usr/bin/env python3
"""Creates a link-time stub of a shared library: same soname, same exported
function/object names and symbol versions, empty bodies. The real library on
the BMC is used at run time; the stub only lets the cross linker resolve
symbols (with the right versions) without the BMC's sysroot.

Usage: mkstub.py <real .so (any ARM ABI)> <out dir> <cc ...>
"""
import os, re, subprocess, sys, tempfile

lib, out, cc = sys.argv[1], sys.argv[2], sys.argv[3:]
dyn = subprocess.run(["readelf", "-W", "--dyn-syms", lib], capture_output=True, text=True, check=True).stdout
soname = re.search(r"\(SONAME\)\s+Library soname: \[(.*)\]", subprocess.run(["readelf", "-d", lib], capture_output=True, text=True).stdout).group(1)
versions = {}
for line in dyn.splitlines():
    f = line.split()
    if len(f) < 8 or not f[0].endswith(":") or f[6] in ("UND", "ABS") or f[4] not in ("GLOBAL", "WEAK"):
        continue
    if f[3] not in ("FUNC", "OBJECT", "IFUNC"):
        continue
    name = f[7]
    # libgcc helpers some distro libraries re-export (Ubuntu armhf libsystemd
    # exports __aeabi_uldivmod...): the binary must take them from the static
    # compiler-rt, the BMC's libraries don't have them
    if re.match(r"__(aeabi|gnu)_", name):
        continue
    m = re.match(r"([^@]+)(@@?)(.*)", name)
    sym, ver = (m.group(1), m.group(3)) if m else (name, None)
    if not m or m.group(2) == "@@" or ver not in versions.get(sym, {}):
        versions.setdefault(sym, {})[ver] = (f[3], m.group(2) == "@@" if m else True)
with tempfile.TemporaryDirectory() as d:
    src, vs = [], {}
    for sym, vv in versions.items():
        kind = next(iter(vv.values()))[0]
        src.append(f"char {sym}[4];" if kind == "OBJECT" else f"void {sym}(void) {{}}")
        default = [v for v, (_, dflt) in vv.items() if dflt and v]
        if default:
            vs.setdefault(default[0], []).append(sym)
    open(f"{d}/s.c", "w").write("\n".join(src) + "\n")
    names = sorted(vs, key=lambda v: [int(x) if x.isdigit() else x for x in re.split(r"(\d+)", v)])
    script, prev = [], None
    for v in names:
        body = "".join(f"  {s};\n" for s in vs[v])
        # local: everything else, i.e. the compiler-rt that zig links into
        # every shared object (otherwise the stub "exports" __aeabi_* helpers)
        local = "" if prev else " local: *;\n"
        script.append(f"{v} {{\n global:\n{body}{local}}}{' ' + prev if prev else ''};")
        prev = v
    open(f"{d}/v.map", "w").write("\n".join(script) + "\n" if script else "{ global: *; };\n")
    os.makedirs(out, exist_ok=True)
    subprocess.run(cc + ["-shared", "-fPIC", "-nostdlib", "-o", f"{out}/{soname}", f"{d}/s.c", f"-Wl,-soname,{soname}",
                         f"-Wl,--version-script={d}/v.map"], check=True)
base = re.sub(r"\.so\..*", ".so", soname)
if os.path.lexists(f"{out}/{base}"):
    os.unlink(f"{out}/{base}")
os.symlink(soname, f"{out}/{base}")
print(f"{out}/{soname}: {len(versions)} symbols, versions {', '.join(names) or '-'}")
