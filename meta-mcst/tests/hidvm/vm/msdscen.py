#!/usr/bin/env python3
"""Virtual media (jsnbd's reimu_state_hook gadgets) in the HID test VM, the VM
being the USB host:

    msdscen.py HOOK

The hook runs under busybox sh as on the BMC, on loop devices in place of
/dev/nbd0 (disk) and /dev/nbd1 (CD-ROM, a sparse 5 GiB image: reads past
4 GiB), bound to dummy_udc.N. Checked on the host: SCSI INQUIRY (vendor,
product, removable, CD-ROM type), the data, with the hook's stall setting
and with the other one; on the gadget: stall, nofua, removable.
"""
import glob, os, re, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kvmvm as K  # noqa: E402

HOOK = sys.argv[1]
failed = []
W = "/tmp/msd"


def check(cond, what):
    print(("  PASS " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failed.append(what)
    return cond


def prepare():
    os.makedirs(W, exist_ok=True)
    K.sh("losetup -D")
    with open(W + "/disk.img", "wb") as f:
        for i in range(64 * 1024):
            f.write(i.to_bytes(4, "little") * 256)
    with open(W + "/cd.iso", "wb") as f:
        f.truncate(5 << 30)
        f.seek((9 << 29) - 2048)  # 4.5 GiB: the last 2 KiB below it
        f.write(b"REIMU-BEYOND-4G" + b"\0" * (2048 - 15))
        f.write(b"REIMU-AT-4.5G!!" + b"\0" * (2048 - 15))
    K.sh("losetup /dev/loop0 %s/disk.img && losetup -r /dev/loop1 %s/cd.iso" % (W, W))


def hook(action, config, stall=None):
    s = open(HOOK).read().replace("nbd0", "loop0").replace("nbd1", "loop1")
    if stall is not None:
        s = re.sub(r"echo [01] > functions/mass_storage.usb0/stall", "echo %d > functions/mass_storage.usb0/stall" % stall, s)
    open("/tmp/state_hook", "w").write(s)
    env = dict(os.environ, PATH=K.busybox_path())
    r = subprocess.run(["/usr/bin/busybox", "sh", "/tmp/state_hook", action, str(config)], capture_output=True, text=True, env=env)
    return r.returncode, (r.stdout + r.stderr).strip()


def host_disk(product, timeout=15):
    """(sg device, block device) of the gadget's LUN on the host"""
    end = time.time() + timeout
    while time.time() < end:
        for sg in glob.glob("/sys/class/scsi_generic/sg*"):
            try:
                model = open(sg + "/device/model").read().strip()
            except OSError:
                continue
            if model == product.strip() or (product == "*" and "Gadget" in model):
                blk = glob.glob(sg + "/device/block/*")
                if blk:
                    return "/dev/" + os.path.basename(sg), "/dev/" + os.path.basename(blk[0])
        time.sleep(0.3)
    return None, None


def inq(sg):
    out = K.sh("sg_inq %s" % sg, check=False).stdout
    g = lambda k: (re.search(k + r"\s*:\s*(.*)", out) or [None, ""])[1].strip()  # noqa: E731
    rmb = re.search(r"RMB=(\d)", out)
    pdt = re.search(r"Peripheral device type: (.*)", out)
    return g("Vendor identification"), g("Product identification"), g("Product revision level"), \
        rmb and rmb.group(1), pdt and pdt.group(1).strip()


def attrs():
    f = "/sys/kernel/config/usb_gadget/mass-storage%d/functions/mass_storage.usb0"
    out = {}
    for c in (0, 1):
        d = f % c
        if os.path.isdir(d):
            out[c] = {a: open(d + "/" + a).read().strip() for a in ("stall",)}
            out[c].update({a: open(d + "/lun.0/" + a).read().strip() for a in ("removable", "nofua", "cdrom", "ro", "inquiry_string")})
    return out


def read_at(dev, off, n):
    fd = os.open(dev, os.O_RDONLY)
    try:
        os.lseek(fd, off, 0)
        return os.read(fd, n)
    except OSError as e:
        print("         read %s at %d: %s (size %s)" % (dev, off, e, K.sh("blockdev --getsize64 " + dev, check=False).stdout.strip()))
        return b""
    finally:
        os.close(fd)


def main():
    K.gadget_down()
    hook("stop", 0), hook("stop", 1)
    time.sleep(0.5)
    prepare()
    for stall in (None, "other"):
        for config, product, want_pdt in ((1, "Virtual CD-ROM", "cd/dvd"), (0, "Virtual Disk", "disk")):
            hook("stop", config)
            s = None
            if stall == "other":
                cur = attrs_stall_of_hook()
                s = 1 - cur
            rc, out = hook("start", config, s)
            label = "%s%s" % (product, "" if s is None else " (stall=%d)" % s)
            print("== %s" % label)
            if not check(rc == 0, "hook start %d (%s)" % (config, out)):
                continue
            a = attrs().get(config, {})
            sg, blk = host_disk(product)
            if not sg:
                sg, blk = host_disk("*", timeout=3)
            check(sg is not None, "the host sees the LUN (%s %s)" % (sg, blk))
            if not sg:
                hook("stop", config)
                continue
            v, p, r, rmb, pdt = inq(sg)
            if s is None:
                check(v == "REIMU" and p == product, "INQUIRY: vendor REIMU, product %s (%r %r %r)" % (product, v, p, r))
                check(rmb == "1", "INQUIRY: removable medium (RMB=%s)" % rmb)
                check(pdt is not None and want_pdt in pdt.lower(), "INQUIRY: device type %s (%s)" % (want_pdt, pdt))
                check(a.get("stall") == "0", "gadget: stall 0 (%s)" % a.get("stall"))
                check(a.get("nofua") == "1", "gadget: nofua 1 (%s)" % a.get("nofua"))
                check(a.get("removable") == "1", "gadget: removable 1")
            if config == 1:
                d = read_at(blk, (9 << 29) - 2048, 4096)
                check(d[:15] == b"REIMU-BEYOND-4G" and d[2048:2063] == b"REIMU-AT-4.5G!!", "CD-ROM: reads at 4.5 GiB of a 5 GiB image")
            else:
                d = read_at(blk, 1 << 20, 4096)
                want = b"".join(((1 << 20) // 1024 + i).to_bytes(4, "little") * 256 for i in range(4))
                check(d == want, "disk: the data at 1 MiB")
            hook("stop", config)
            time.sleep(0.5)
    K.sh("losetup -D")
    print("%d failed" % len(failed))
    sys.exit(min(len(failed), 100))


def attrs_stall_of_hook():
    m = re.search(r"echo ([01]) > functions/mass_storage.usb0/stall", open(HOOK).read())
    return int(m.group(1)) if m else 1  # f_mass_storage's default: stall 1


if __name__ == "__main__":
    main()
