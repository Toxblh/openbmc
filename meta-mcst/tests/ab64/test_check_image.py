#!/usr/bin/env python3
"""check-image.py: a reimu-ab.tar whose kernel and /lib/modules are of different
builds is refused, the matching one passes. Synthetic images: a stub with an xz
kernel in it (behind a false xz magic, as in a real zImage), a squashfs rootfs."""
import glob, lzma, os, subprocess, sys, tarfile, tempfile

TOOL = "/work/openbmc/meta-mcst/tools/ab64/check-image.py"
SQ = glob.glob("/work/build-ab/tmp/sysroots-components/x86_64/squashfs-tools-native/usr/sbin/mksquashfs")
mksq = SQ[0] if SQ else "mksquashfs"
bad = 0


def case(name, kernel_release, module_dirs, want_rc, want_text, vermagic=None, extra=False):
    global bad
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(d + "/r/etc")
        for m in module_dirs:
            os.makedirs(d + "/r/lib/modules/" + m + "/kernel")
            open(d + "/r/lib/modules/" + m + "/kernel/a.ko", "wb").write(b"\0vermagic=" + m.encode() + b" mod_unload ARMv6 p2v8 \0")
            if vermagic:
                os.makedirs(d + "/r/lib/modules/" + m + "/updates")
                open(d + "/r/lib/modules/" + m + "/updates/l-pcs-i2c.ko", "wb").write(b"\0vermagic=" + vermagic.encode() + b" mod_unload \0")
        subprocess.check_call([mksq, d + "/r", d + "/image-rofs", "-comp", "gzip", "-quiet", "-no-progress"], stdout=subprocess.DEVNULL)
        v = b"Linux version " + kernel_release.encode() + b" (gcc) #1\n" if kernel_release else b"nothing here"
        kern = b"\0" * 4096 + b"\xfd7zXZ\0 stub" + b"\0" * 100 + lzma.compress(os.urandom(200000) + v, format=lzma.FORMAT_XZ)
        open(d + "/image-kernel", "wb").write(kern)
        with tarfile.open(d + "/t.tar", "w") as t:
            t.add(d + "/image-kernel", "image-kernel")
            t.add(d + "/image-rofs", "image-rofs")
        r = subprocess.run([sys.executable, TOOL, d + "/t.tar"], capture_output=True, text=True)
    o = r.stdout + r.stderr
    ok = r.returncode == want_rc and want_text in o
    bad += not ok
    print("  %s %s (rc %d: %s)" % ("PASS" if ok else "FAIL", name, r.returncode, o.strip().replace("\n", " | ")[:170]))


R = "6.18.57-0168b08"
case("the same release: passes", R, [R], 0, R)
case("kernel and modules of different builds: refused", R + "-46c826b", [R + "-08b3335"], 1, "08b3335")
case("two module trees: refused", R, [R, R + "-x"], 1, "modules")
case("no module tree: refused", R, [], 1, "modules")
case("no release in the kernel: refused", None, [R], 1, "release")
case("an out-of-tree module for RELEASE+: refused", R, [R], 1, "l-pcs-i2c.ko", vermagic=R + "+")
print("%d failed" % bad)
sys.exit(1 if bad else 0)
