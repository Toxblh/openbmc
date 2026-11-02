#!/usr/bin/env python3
"""check-image.py REIMU-AB-TAR: the kernel and the rootfs of an update are of one
build: the release in the kernel is the only directory in /lib/modules and the
vermagic of every module there. A cached build can mix them, an out-of-tree
module can be built for "RELEASE+", and then the module doesn't load on the
board, with no error at the build or at the update."""
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


def modules(rofs, d):
    """{release directory: {module: its vermagic release}}"""
    tool = os.environ.get("UNSQUASHFS") or shutil.which("unsquashfs")
    if not tool:
        found = glob.glob("/work/build*/tmp/sysroots-components/x86_64/squashfs-tools-native/usr/sbin/unsquashfs")
        tool = found[0] if found else fail("no unsquashfs")
    subprocess.run([tool, "-no-progress", "-d", d, rofs, "lib/modules"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    res = {}
    top = os.path.join(d, "lib/modules")
    for rel in os.listdir(top) if os.path.isdir(top) else []:
        res[rel] = {}
        for root, _, names in os.walk(os.path.join(top, rel)):
            for n in names:
                if n.endswith(".ko"):
                    m = re.search(rb"vermagic=(\S+)", open(os.path.join(root, n), "rb").read())
                    res[rel][n] = m.group(1).decode() if m else None
    return res


def main():
    with tempfile.TemporaryDirectory() as d:
        with tarfile.open(sys.argv[1]) as t:
            for n in ("image-kernel", "image-rofs"):
                with open(os.path.join(d, n), "wb") as f:
                    shutil.copyfileobj(t.extractfile(n), f)
        rel = kernel_release(open(os.path.join(d, "image-kernel"), "rb").read())
        mods = modules(os.path.join(d, "image-rofs"), os.path.join(d, "r"))
    if not rel:
        fail("no release found in the kernel")
    if sorted(mods) != [rel]:
        fail("kernel %s, modules in the rootfs for %s" % (rel, sorted(mods) or "nothing"))
    if not mods[rel]:
        fail("kernel %s, no modules in its directory" % rel)
    wrong = {n: v for n, v in mods[rel].items() if v != rel}
    if wrong:
        fail("kernel %s, modules built for another: %s" % (rel, wrong))
    print("check-image: kernel and %d modules %s" % (len(mods[rel]), rel))


main()
