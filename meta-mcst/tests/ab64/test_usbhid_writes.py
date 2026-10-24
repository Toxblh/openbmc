#!/usr/bin/env python3
"""create_usbhid.sh of an image writes each HID report descriptor to configfs
in ONE write() (run in the build container: qemu-arm user mode, unsquashfs).

    test_usbhid_writes.py [ROOTFS_DIR | ROFS.squashfs-xz]

Default: the A/B build's rofs (deploy/images/reimu-5564, squashfs). The
image's own /usr/bin/create_usbhid.sh runs under the image's /bin/sh (bash
on this image) and under its busybox ash, ARM binaries in qemu-arm -strace,
against a fake configfs (directories; the default groups configfs makes are
made by the mkdir wrapper). Every command the script runs is the image's
busybox, through a wrapper that runs it in qemu-arm -strace too; the trace
gives each write() and the file it went to (openat/dup2/fcntl/clone tracked
per process). f_hid takes every write() to report_desc as the whole
descriptor (6.18: f_hid_opts_report_desc_store), so: exactly one write per
descriptor, the whole descriptor (keyboard 63 bytes, mouse 93), and the bytes
the descriptors of hid_desc.py.
"""
import os, re, shutil, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hid_desc as H  # noqa: E402

DEPLOY = "/work/build-ab/tmp/deploy/images/reimu-5564/obmc-phosphor-image-reimu-5564.squashfs-xz"
QEMU = shutil.which("qemu-arm") or "/usr/bin/qemu-arm"
failed = []


def check(cond, what):
    print(("  PASS " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failed.append(what)
    return cond


# mkdir in configfs makes a group's default groups; here the wrapper does it
MKDIR = r'''#!/bin/sh
"$QEMU" -strace -L "$ROOT" "$ROOT/bin/busybox.nosuid" mkdir "$@" || exit $?
for d in "$@"; do
    case "$d" in
        -*) ;;
        */usb_gadget/*/*) case "$d" in
                              */configs/c.*/*) ;;
                              */configs/c.*) mkdir -p "$d/strings" ;;
                          esac ;;
        */usb_gadget/*) mkdir -p "$d/strings" "$d/functions" "$d/configs" "$d/os_desc" ;;
        configs/c.*/*) ;;
        configs/c.*) mkdir -p "$d/strings" ;;
    esac
done
'''
APPLET = '#!/bin/sh\nexec "$QEMU" -strace -L "$ROOT" "$ROOT/bin/busybox.nosuid" %s "$@"\n'

CALL = re.compile(r'(\d+) (openat|open|creat|dup2|dup3|dup|fcntl64|fcntl|close|write|writev|pwrite64|'
                  r'sendfile64|sendfile|splice|clone|fork|vfork)\(([^()]*)\)(?: = (-?\d+))?')


def writes_of(trace):
    """[(path, bytes)] of every write-like call, by following the fds of
    each process (fork copies the table, exec keeps it)"""
    fds, out = {}, []
    for m in CALL.finditer(trace):
        pid, call, args, ret = int(m.group(1)), m.group(2), m.group(3), m.group(4)
        t = fds.setdefault(pid, {})
        a = [x.strip() for x in args.split(",")]
        r = int(ret) if ret is not None else None
        if call in ("openat", "open", "creat") and r is not None and r >= 0:
            p = re.search(r'"([^"]*)"', args)
            t[r] = p.group(1) if p else "?"
        elif call in ("dup2", "dup3") and r is not None and r >= 0:
            t[r] = t.get(int(a[0]), "fd%s" % a[0])
        elif call == "dup" and r is not None and r >= 0:
            t[r] = t.get(int(a[0]), "fd%s" % a[0])
        elif call.startswith("fcntl") and len(a) > 1 and a[1].startswith("F_DUPFD") and r is not None and r >= 0:
            t[r] = t.get(int(a[0]), "fd%s" % a[0])
        elif call == "close":
            t.pop(int(a[0]), None)
        elif call in ("clone", "fork", "vfork") and r is not None and r > 0:
            fds[r] = dict(t)
        elif call in ("write", "writev", "pwrite64", "sendfile64", "sendfile", "splice") and r is not None:
            fd = int(a[2]) if call == "splice" else int(a[0])
            out.append((t.get(fd, "fd%d" % fd), r))
    return out


def run(root, shell, label):
    print("== %s (%s)" % (label, shell), flush=True)
    w = tempfile.mkdtemp(prefix="usbhid-")
    try:
        cfg, plat, bindir = w + "/cfg", w + "/platform", w + "/bin"
        for d in (cfg + "/usb_gadget", plat, bindir, w + "/run"):
            os.makedirs(d)
        applets = subprocess.run([QEMU, "-L", root, root + "/bin/busybox.nosuid", "--list"],
                                 capture_output=True, text=True).stdout.split()
        for a in applets:
            with open(os.path.join(bindir, a), "w") as f:
                f.write(MKDIR if a == "mkdir" else APPLET % a)
            os.chmod(os.path.join(bindir, a), 0o755)
        s = open(root + "/usr/bin/create_usbhid.sh").read()
        s = s.replace("/sys/kernel/config", cfg).replace("/sys/bus/platform/devices", plat)
        s = re.sub(r'(?<![\w/])/run/', w + "/run/", s)
        open(w + "/create_usbhid.sh", "w").write(s)
        argv = [QEMU, "-strace", "-L", root, root + shell] + (["sh"] if shell.endswith("busybox.nosuid") else [])
        p = subprocess.run(argv + [w + "/create_usbhid.sh"], cwd=w, capture_output=True, text=True,
                           env={"PATH": bindir, "QEMU": QEMU, "ROOT": root, "HOME": "/"}, timeout=600)
        if os.environ.get("KEEP_TRACE"):  # KEEP_TRACE=DIR: the qemu-arm -strace output
            open(os.path.join(os.environ["KEEP_TRACE"], "usbhid-%s.trace" % label), "w").write(p.stderr)
        noise = [l for l in p.stderr.splitlines() if not re.match(r"^\d+ |^ = |^\s*$", l) and "=" not in l[:12]]
        check(p.returncode == 0, "create_usbhid.sh exits 0 (rc %d)%s" % (p.returncode,
              "".join("\n         " + l for l in noise[-5:]) if p.returncode else ""))
        ws = writes_of(p.stderr)
        g = cfg + "/usb_gadget/obmc_hid/functions/"
        for fn, (name, want, proto, sub, length) in sorted(H.FUNCTIONS.items()):
            mine = [n for path, n in ws if path.endswith(fn + "/report_desc")]
            check(len(mine) == 1 and mine[0] == len(want),
                  "%s: %s descriptor in one write() of %d bytes (write() calls: %s)"
                  % (fn, name, len(want), mine or "none"))
            try:
                got = open(g + fn + "/report_desc", "rb").read()
            except OSError:
                got = b""
            # configfs keeps only the last write(): what the host would get
            kept = got[-mine[-1]:] if mine and mine[-1] > 0 else b""
            probs = H.check_function(kept, fn)
            for x in probs:
                print("         " + x)
            check(not probs, "%s: configfs would keep the expected %d bytes, a sound descriptor (%d bytes)"
                  % (fn, len(want), len(kept)))
            for attr, v in (("protocol", proto), ("subclass", sub), ("report_length", length)):
                try:
                    have = open(g + fn + "/" + attr).read().strip()
                except OSError:
                    have = "missing"
                check(have == str(v), "%s/%s = %s (%s)" % (fn, attr, v, have))
    finally:
        shutil.rmtree(w, ignore_errors=True)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEPLOY
    tmp = None
    if os.path.isdir(src):
        root = os.path.abspath(src)
    else:
        tmp = tempfile.mkdtemp(prefix="usbhid-rofs-")
        root = tmp + "/r"
        subprocess.run(["unsquashfs", "-q", "-n", "-d", root, os.path.realpath(src)],
                       check=True, stdout=subprocess.DEVNULL)
    try:
        sh = os.readlink(root + "/bin/sh")
        print("rootfs %s: /bin/sh -> %s" % (src, sh))
        run(root, sh if sh.startswith("/") else "/bin/" + sh, "image-sh")
        run(root, "/bin/busybox.nosuid", "busybox-ash")
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d failed" % len(failed) + "".join("\n  " + f for f in failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
