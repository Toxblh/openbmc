#!/usr/bin/env python3
"""reimu-fit (the FIT reader of "reimu-ab ram") on crafted images: built with
AddressSanitizer here, each bad one must end soon with a non-zero exit and a
message, the good one must still give kernel, fdt and ramdisk.

    test_reimu_fit.py [reimu-fit.c]
"""
import os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    HERE, "..", "..", "meta-ab64", "recipes-mcst", "reimu-ab", "files", "reimu-fit.c")
BEGIN, END, PROP, NOP, FEND = 1, 2, 3, 4, 9
failed = 0


def check(c, what):
    global failed
    print(("ok   " if c else "FAIL ") + what, flush=True)
    failed += not c


def u32(*v):
    return struct.pack(">%dI" % len(v), *v)


def pad(b):
    return b + b"\0" * (-len(b) % 4)


class Fit:
    """a flattened device tree, the fields a crafted one breaks left open"""

    def __init__(self):
        self.strings = b""
        self.off = {}
        self.body = b""

    def name(self, n):
        if n not in self.off:
            self.off[n] = len(self.strings)
            self.strings += n.encode() + b"\0"
        return self.off[n]

    def begin(self, n):
        self.body += u32(BEGIN) + pad(n.encode() + b"\0")
        return self

    def end(self):
        self.body += u32(END)
        return self

    def prop(self, n, v):
        if isinstance(v, str):
            v = v.encode() + b"\0"
        self.body += u32(PROP, len(v), self.name(n)) + pad(v)
        return self

    def image(self, n, typ, data):
        return self.begin(n).prop("data", data).prop("type", typ).end()

    def build(self, ostruct=None, ostrings=None):
        s = 40 + len(self.body)
        hdr = u32(0xd00dfeed, s + len(self.strings), 40 if ostruct is None else ostruct,
                  s if ostrings is None else ostrings, 0, 17, 16, 0, len(self.strings), len(self.body))
        return hdr + self.body + self.strings

    def build_struct_last(self):
        """the struct runs to the very end of the image"""
        o = 40 + len(self.strings)
        hdr = u32(0xd00dfeed, o + len(self.body), o, 40, 0, 17, 16, 0, len(self.strings), len(self.body))
        return hdr + self.strings + self.body


def images(f):
    return f.begin("").begin("images")


def good():
    f = images(Fit())
    f.image("kernel-1", "kernel", b"KERNEL-DATA").image("fdt-1", "flat_dt", b"FDT!").image("rd-1", "ramdisk", b"RD")
    return f.end().end()


def run(exe, blob):
    with tempfile.TemporaryDirectory() as d:
        fit = os.path.join(d, "fit")
        open(fit, "wb").write(blob)
        os.mkdir(d + "/out")
        try:
            r = subprocess.run([exe, fit, d + "/out"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5,
                               env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"))
        except subprocess.TimeoutExpired:
            return "hang", "", {}
        out = {n: open(os.path.join(d, "out", n), "rb").read() for n in os.listdir(d + "/out")}
        return r.returncode, r.stderr.decode("utf-8", "replace"), out


def bad_cases():
    g = good()
    yield "header: struct offset beyond the image", g.build(ostruct=0xfffffff0)
    yield "header: struct offset not aligned", g.build(ostruct=41)
    yield "header: strings offset beyond the image", g.build(ostrings=0xfffffff0)
    f = images(Fit())
    f.body += u32(PROP, 0xfffffff4, f.name("data"))
    yield "PROP len 0xfffffff4 (p wraps: loops for ever)", f.build()
    f = images(Fit())
    f.body += u32(PROP, 0xffffffff, f.name("data"))
    yield "PROP len 0xffffffff", f.build()
    f = images(Fit()).begin("kernel-1")
    f.body += u32(PROP, 0x100000, f.name("data")) + b"short"
    yield "PROP data beyond the image", f.build()
    f = Fit()
    f.name("xyz")
    f.body += u32(PROP)
    yield "PROP at the very end: no len, nameoff", f.build_struct_last()
    for off, what in ((0x7fffffff, "beyond the image"), (0xffffffff, "wraps")):
        f = images(Fit())
        f.name("x")
        f.body += u32(PROP, 0, off)
        yield "PROP nameoff " + what, f.build()
    f = images(Fit())
    f.name("x")
    f.body += u32(PROP, 0, 0)
    yield "property name without a NUL to the end of the image", f.build()[:-1] + b"y"
    f = Fit()
    f.body += u32(BEGIN) + b"no-terminator-to-the-end"
    yield "BEGIN_NODE name without a NUL to the end of the image", f.build_struct_last()
    f = images(Fit()).begin("kernel-1").prop("data", b"KERNEL")
    f.body += u32(PROP, 8, f.name("type")) + b"kernelxx" + u32(END)
    yield "type property without a NUL", f.build()
    # good images first: what they gave must not count once the rest is bad
    f = images(good())
    f.body += u32(PROP, 0xfffffff4, f.name("data"))
    yield "a bad property after valid images", f.build()


def main():
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "reimu-fit")
        subprocess.check_call(["gcc", "-g", "-O1", "-fsanitize=address", "-o", exe, SRC])
        rc, err, out = run(exe, good().build())
        check(rc == 0 and out == {"kernel": b"KERNEL-DATA", "fdt": b"FDT!", "ramdisk": b"RD"},
              "a valid FIT: rc %s, %s %s" % (rc, sorted(out), err[-100:]))
        # the padding FDT_END / NOP a real FIT has
        f = good()
        f.body += u32(NOP, FEND)
        rc, err, out = run(exe, f.build())
        check(rc == 0 and sorted(out) == ["fdt", "kernel", "ramdisk"], "a valid FIT with NOP, FDT_END: rc %s %s" % (rc, err[-100:]))
        for what, blob in bad_cases():
            rc, err, out = run(exe, blob)
            clean = rc == 1 and "corrupt" in err and "Sanitizer" not in err
            check(clean, "%s: rc %s %s" % (what, rc, "ASan: " + err.split("ERROR: ")[1][:60] if "ERROR: " in err else err.strip()[:80]))
    print("\n%d failed" % failed)
    return 1 if failed else 0


sys.exit(main())
