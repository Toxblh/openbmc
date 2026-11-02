#!/usr/bin/env python3
"""check-image.py REIMU-AB-TAR: the kernel and the rootfs of an update are of one
build: the release in the kernel is the only directory in /lib/modules. A cached
build can mix them, and then no module loads on the board, with no error at the
build or at the update."""
import glob, lzma, os, re, shutil, subprocess, sys, tarfile, tempfile


def fail(msg):
    print("check-image: " + msg, file=sys.stderr)
    sys.exit(1)


def kernel_release(b):
    i = -1
    while True:
        i = b.find(b"\xfd7zXZ\0", i + 1)     # the first ones can be in the stub
        if i < 0:
            return None
        z, out = lzma.LZMADecompressor(format=lzma.FORMAT_XZ), []
        try:
            for k in range(i, len(b), 65536):
                out.append(z.decompress(b[k:k + 65536]))
                if z.eof:
                    break
        except lzma.LZMAError:
            pass
        m = re.search(rb"Linux version (\S+)", b"".join(out))
        if m:
            return m.group(1).decode()


def module_dirs(rofs):
    tool = os.environ.get("UNSQUASHFS") or shutil.which("unsquashfs")
    if not tool:
        found = glob.glob("/work/build*/tmp/sysroots-components/x86_64/squashfs-tools-native/usr/sbin/unsquashfs")
        tool = found[0] if found else fail("no unsquashfs")
    o = subprocess.check_output([tool, "-ls", rofs], text=True, stderr=subprocess.DEVNULL)
    return sorted(set(re.findall(r"^squashfs-root/lib/modules/([^/\n]+)$", o, re.M)))


def main():
    with tempfile.TemporaryDirectory() as d:
        with tarfile.open(sys.argv[1]) as t:
            for n in ("image-kernel", "image-rofs"):
                with open(os.path.join(d, n), "wb") as f:
                    shutil.copyfileobj(t.extractfile(n), f)
        rel = kernel_release(open(os.path.join(d, "image-kernel"), "rb").read())
        mods = module_dirs(os.path.join(d, "image-rofs"))
    if not rel:
        fail("no release found in the kernel")
    if mods != [rel]:
        fail("kernel %s, modules in the rootfs for %s" % (rel, mods or "nothing"))
    print("check-image: kernel and modules %s" % rel)


main()
