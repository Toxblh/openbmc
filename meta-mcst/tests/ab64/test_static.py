#!/usr/bin/env python3
"""Regression checks without QEMU: the tree, the build's artifacts and the
pure-logic code of REIMU-5564's A/B firmware (see REGRESSIONS.md: each check
names the bug it keeps from coming back).

    test_static.py [options] [CHECK...]        (CHECK: an id such as S06; all by default)

Runs where the tree and the build are, i.e. in the build container:

    docker exec reimu python3 /work/openbmc/meta-mcst/tests/ab64/test_static.py

Options:
  --root DIR      the repository (default: the one this file is in); a tree
                  of an older revision shows which checks it fails:
                    git archive REV meta-mcst | tar -x -C /tmp/rev
                    test_static.py --root /tmp/rev
  --deploy DIR    the build's deploy dir (reimu-ab.tar, reimu-ab-kexec.tar,
                  *.ab.mtd); default /work/build-ab/tmp/deploy/images/reimu-5564
  --image MTD     the A/B flash image whose slot A rootfs is checked (default:
                  the deploy dir's .ab.mtd)
  --rofs DIR      an unpacked rootfs instead (e.g. /work/stock/rofs)
  --kexec TAR     reimu-ab-kexec.tar (default: the deploy dir's)
  --update TAR    reimu-ab.tar (default: the deploy dir's)
  --red           also run every check against its bug put back (a mutant
                  of the input): each must FAIL there, or the check would not
                  see the bug come back. Exit 1 if one doesn't.
Needs: python3, unsquashfs (image checks), gcc with ASan and libfdt-dev
(S14, S35). A check whose input is missing is SKIP, not PASS.
"""
import glob, io, lzma, os, re, shutil, struct, subprocess, sys, tarfile, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ARGS = sys.argv[1:]


def opt(name, default=None):
    if name in ARGS:
        i = ARGS.index(name)
        v = ARGS[i + 1]
        del ARGS[i:i + 2]
        return v
    return default


ROOT = os.path.abspath(opt("--root", os.path.join(HERE, "..", "..", "..")))
DEPLOY = opt("--deploy", "/work/build-ab/tmp/deploy/images/reimu-5564")
IMAGE = opt("--image")
ROFS = opt("--rofs")
KEXEC = opt("--kexec", os.path.join(DEPLOY, "reimu-ab-kexec.tar"))
UPDATE = opt("--update", os.path.join(DEPLOY, "reimu-ab.tar"))
# pristine upstream sources at the pinned revisions (the recipes patch them)
FRUID_SRC = opt("--fruid-src", "/work/mcst-fruid-src")
LIBREIMU_SRC = opt("--libreimu-src", "/work/libreimu-src")
RED = "--red" in ARGS
if RED:
    ARGS.remove("--red")
ONLY = ARGS
MC = os.path.join(ROOT, "meta-mcst")
COMMON = os.path.join(MC, "meta-common")
AB = os.path.join(MC, "meta-ab64")
TMP = tempfile.mkdtemp(prefix="reimu-static-")


class Skip(Exception):
    pass


# ------------------------------------------------------------------ inputs
class Ctx:
    """what the checks read; a mutant changes it (files: path -> text)"""

    def __init__(self):
        self.files = {}      # repo path (relative to ROOT) -> replacement text
        self.rootfs = {}     # rootfs path -> replacement bytes, None = absent
        self.dtb_edit = None
        self.sizes = None    # (kernel, rofs) bytes instead of the update's
        self.fruid_src = FRUID_SRC
        self.fruid_seds = True
        self.libreimu_patched = True
        self.fand_mutant = None

    def src(self, rel):
        if rel in self.files:
            return self.files[rel]
        p = os.path.join(ROOT, rel)
        if not os.path.exists(p):
            raise Skip("no " + rel)
        return open(p, encoding="utf-8", errors="replace").read()

    def src_exists(self, rel):
        return rel in self.files or os.path.exists(os.path.join(ROOT, rel))

    def fs(self, path):
        """a file of the image's rootfs: bytes or None"""
        if path in self.rootfs:
            return self.rootfs[path]
        r = rootfs()
        p = os.path.join(r, path.lstrip("/"))
        if os.path.islink(p):
            t = os.readlink(p)
            return self.fs(t if t.startswith("/") else os.path.join(os.path.dirname(path), t))
        if os.path.isfile(p):
            return open(p, "rb").read()
        return None

    def fs_list(self, d):
        r = rootfs()
        p = os.path.join(r, d.lstrip("/"))
        names = set(os.listdir(p)) if os.path.isdir(p) else set()
        for k, v in self.rootfs.items():
            if os.path.dirname(k) == d.rstrip("/"):
                (names.add if v is not None else names.discard)(os.path.basename(k))
        return sorted(names)


_rootfs = []


def rootfs():
    if _rootfs:
        return _rootfs[0]
    if ROFS:
        _rootfs.append(ROFS)
        return ROFS
    img = IMAGE or next(iter(sorted(glob.glob(os.path.join(DEPLOY, "*.ab.mtd")))), None)
    if not img or not os.path.exists(img) or not shutil.which("unsquashfs"):
        raise Skip("no image (--image/--rofs) or no unsquashfs")
    with open(img, "rb") as f:
        f.seek(0x700000)
        sq = f.read(0x1500000 - 4096)
    p = os.path.join(TMP, "rofs.sqfs")
    open(p, "wb").write(sq)
    d = os.path.join(TMP, "rofs")
    subprocess.run(["unsquashfs", "-no-progress", "-d", d, p], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    os.remove(p)
    if not os.path.isdir(os.path.join(d, "etc")):
        raise Skip("can't unpack the rootfs of " + img)
    _rootfs.append(d)
    return d


def tar_member(tar, name):
    if not tar or not os.path.exists(tar):
        raise Skip("no " + str(tar))
    with tarfile.open(tar) as t:
        return t.extractfile(name).read()


# ------------------------------------------------------------------ FDT
def fdt_nodes(blob):
    """[(path, {prop: bytes})] of a flattened device tree"""
    be = lambda o: struct.unpack(">I", blob[o:o + 4])[0]
    ostruct, ostrings = be(8), be(12)
    p, names, props, out = ostruct, [], [], []
    while True:
        tok = be(p)
        p += 4
        if tok == 1:
            e = blob.index(b"\0", p)
            names.append(blob[p:e].decode())
            p = (e + 4) & ~3
            props.append({})
            out.append(("/" + "/".join(n for n in names if n), props[-1]))
        elif tok == 2:
            names.pop()
            props.pop()
        elif tok == 3:
            ln, no = be(p), be(p + 4)
            name = blob[ostrings + no:blob.index(b"\0", ostrings + no)].decode()
            props[-1][name] = blob[p + 8:p + 8 + ln]
            p += 8 + ((ln + 3) & ~3)
        elif tok == 4:
            continue
        elif tok == 9:
            return out
        else:
            raise ValueError("bad FDT token %d" % tok)


def flash_partitions(blob):
    """{label: read-only?} of every flash partition (the nodes with a label
    and a reg under a flash@N node of either SPI controller)"""
    r = {}
    for path, props in fdt_nodes(blob):
        if "label" in props and "reg" in props and re.search(r"/flash@\d+/.+", path):
            r[props["label"].rstrip(b"\0").decode()] = "read-only" in props
    return r


# ------------------------------------------------------------------ helpers
def strip_comments(text, kind):
    if kind == "c":
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        return re.sub(r"//[^\n]*", "", text)
    return "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("#"))


def unit_get(text, key):
    return [l.split("=", 1)[1].strip() for l in text.splitlines() if re.match(r"\s*%s\s*=" % re.escape(key), l)]


def block(text, start):
    """the {...} after the line starting with start"""
    i = text.index(start)
    j, depth = text.index("{", i), 0
    for k in range(j, len(text)):
        depth += {"{": 1, "}": -1}.get(text[k], 0)
        if depth == 0:
            return text[j:k + 1]
    return text[j:]


CHECKS = []


def check(cid, bug, what):
    def deco(fn):
        CHECKS.append((cid, bug, what, fn, []))
        return fn
    return deco


def mutant(cid, what):
    """a mutant of check cid: fn(ctx) puts the bug back"""
    def deco(fn):
        next(c for c in CHECKS if c[0] == cid)[4].append((what, fn))
        return fn
    return deco


def sub(ctx, rel, old, new, count=0, regex=False):
    """put a bug back into a copy of a repo file"""
    t = ctx.src(rel)
    if regex:
        t2, k = re.subn(old, new, t, count=count)
    else:
        k = t.count(old)
        t2 = t.replace(old, new, count) if count else t.replace(old, new)
    if not k:
        raise Skip("mutant: %r not in %s" % (old, rel))
    ctx.files[rel] = t2


# ------------------------------------------------------------------ the checks
HWI = "meta-mcst/meta-common/recipes-mcst/hwmon-instantiator/files/hwmon-instantiator.c"
OHD = "meta-mcst/meta-common/recipes-mcst/overheatd/files/overheatd.c"
RTC = "meta-mcst/meta-common/recipes-mcst/rtcread/files/rtcread"
FRUBB = "meta-mcst/meta-common/recipes-mcst/mcst-fruid/mcst-fruid_0.1.bb"


@check("S01", 1, "no /sys/class/i2c-adapter (gone in 6.x) in the I2C users' sources, mcst-fruid's recipe rewrites it")
def s01(ctx):
    bad = []
    for rel, kind in ((HWI, "c"), (OHD, "c"), (RTC, "sh")):
        if "/sys/class/i2c-adapter" in strip_comments(ctx.src(rel), kind):
            bad.append(os.path.basename(rel))
    sed = "s#/sys/class/i2c-adapter/#/sys/bus/i2c/devices/#g" in ctx.src(FRUBB)
    return not bad and sed, "uses it: %s; mcst-fruid sed: %s" % (bad or "none", sed)


@mutant("S01", "hwmon-instantiator.c with the 5.10 path")
def _(ctx):
    sub(ctx, HWI, "/sys/bus/i2c/devices/i2c-", "/sys/class/i2c-adapter/i2c-")


@check("S01i", 1, "no program of the image reads /sys/class/i2c-adapter")
def s01i(ctx):
    bad = []
    for d in ("/sbin", "/usr/sbin", "/usr/bin", "/libexec", "/usr/libexec", "/usr/lib"):
        for n in ctx.fs_list(d):
            b = ctx.fs(d + "/" + n)
            if b and b"/sys/class/i2c-adapter" in b:
                bad.append(d + "/" + n)
    return not bad, "reading it: %s" % (bad or "none")


@mutant("S01i", "the stock hwmon-instantiator (5.10 path) in /sbin")
def _(ctx):
    ctx.rootfs["/sbin/hwmon-instantiator"] = b"\0snprintf /sys/class/i2c-adapter/i2c-%d/new_device\0"


DTS = "meta-mcst/meta-ab64/recipes-kernel/linux/linux-aspeed/aspeed-bmc-mcst-reimu-5564.dts"


@check("S02", 2, "DTS: spi1 (the PNS flash) enabled, its flash node too")
def s02(ctx):
    b = block(ctx.src(DTS), "&spi1 {")
    top = re.sub(r"flash@0\s*{.*", "", b, flags=re.S)
    fl = block(b, "flash@0")
    return 'status = "okay"' in top and 'status = "okay"' in fl, "&spi1 okay: %s, flash@0 okay: %s" % (
        'status = "okay"' in top, 'status = "okay"' in fl)


@mutant("S02", "&spi1 left disabled (6.18's aspeed-g5.dtsi default)")
def _(ctx):
    sub(ctx, DTS, '&spi1 {\n\tstatus = "okay";', '&spi1 {\n\tstatus = "disabled";')


BBCFG = "meta-mcst/meta-ab64/recipes-core/busybox/busybox/reimu-coreutils.cfg"
BBAPP = "meta-mcst/meta-ab64/recipes-core/busybox/busybox_%.bbappend"


@check("S03", 3, "busybox has head -c (FEATURE_FANCY_HEAD; gpio-funcs needs head -c1 without coreutils)")
def s03(ctx):
    on = re.search(r"^CONFIG_FEATURE_FANCY_HEAD=y$", ctx.src(BBCFG), re.M) is not None
    used = "file://reimu-coreutils.cfg" in ctx.src(BBAPP)
    return on and used, "FANCY_HEAD=y: %s, cfg in SRC_URI: %s" % (on, used)


@mutant("S03", "the cfg fragment without FANCY_HEAD")
def _(ctx):
    sub(ctx, BBCFG, "CONFIG_FEATURE_FANCY_HEAD=y\n", "")


@check("S04", 4, "the image has a compiled UTF-8 locale (tmux of tmux-logger refuses without one)")
def s04(ctx):
    a = ctx.fs("/usr/lib/locale/locale-archive") or b""
    have = [l for l in (b"ru_RU.utf8", b"en_US.utf8") if l in a]
    tl = ctx.fs("/lib/systemd/system/tmux-logger.service") is not None
    return len(have) == 2 and tl, "locale-archive: %s; tmux-logger.service: %s" % (
        [h.decode() for h in have] or "none", tl)


@mutant("S04", "the image without the locale archive (locale data removed with the locale tools)")
def _(ctx):
    ctx.rootfs["/usr/lib/locale/locale-archive"] = None


KVMU = "meta-mcst/meta-common/recipes-mcst/reimu-kvm/files/reimu-kvm.service"


@check("S05", 5, "reimu-kvm.service keeps trying until bmcweb made its TLS certificate (no start limit)")
def s05(ctx):
    t = ctx.src(KVMU)
    r, sl = unit_get(t, "Restart"), unit_get(t, "StartLimitIntervalSec")
    return r == ["always"] and sl == ["0"] and unit_get(t, "RestartSec"), "Restart=%s StartLimitIntervalSec=%s" % (r, sl)


@mutant("S05", "the unit before 4adffbb20a (no Restart, the default start limit)")
def _(ctx):
    sub(ctx, KVMU, "Restart=always\n", "")
    sub(ctx, KVMU, "StartLimitIntervalSec=0\n", "")


RAB = "meta-mcst/meta-ab64/recipes-mcst/reimu-ab/files/reimu-ab"
FROM = "meta-mcst/tools/ab64/reimu-ab-from-stock"
BACK = "meta-mcst/tools/ab64/reimu-ab-back-to-stock"


@check("S06", 6, "every kexec puts the MAC addresses into the device tree (U-Boot does, kexec doesn't)")
def s06(ctx):
    r = {}
    for rel in (RAB, FROM, BACK):
        t = strip_comments(ctx.src(rel), "sh")
        r[os.path.basename(rel)] = bool(re.search(r"fdtput[^\n]*local-mac-address", t)) and "1e660000" in t and "1e680000" in t
    return all(r.values()), str(r)


@mutant("S06", "reimu-ab ram without fdtput local-mac-address")
def _(ctx):
    sub(ctx, RAB, r"\n[^\n]*fdtput [^\n]*local-mac-address[^\n]*", "", regex=True)


@check("S07", 7, "a unit masked for a kexec (systemd.mask=) is required by nothing (systemd waits forever, the watchdog resets the BMC and the host)")
def s07(ctx):
    masks = set()
    for rel in (RAB, FROM):
        masks |= set(re.findall(r"systemd\.mask=([A-Za-z0-9@_.\\-]+\.service)", strip_comments(ctx.src(rel), "sh")))
    bad = []
    for d in ("/lib/systemd/system", "/etc/systemd/system"):
        for n in ctx.fs_list(d):
            if n.endswith(".requires"):
                for u in ctx.fs_list(d + "/" + n):
                    if u in masks:
                        bad.append("%s/%s" % (n, u))
            elif n.endswith((".service", ".target", ".socket")):
                t = (ctx.fs(d + "/" + n) or b"").decode(errors="replace")
                for key in ("Requires", "BindsTo", "Requisite"):
                    for v in unit_get(t, key):
                        for u in v.split():
                            if u in masks:
                                bad.append("%s %s=%s" % (n, key, u))
    return bool(masks) and not bad, "masked %s; required by: %s" % (sorted(masks), bad or "nothing")


@mutant("S07", "reimu-ab ram masking phosphor-discover-system-state@0 (multi-user.target requires it)")
def _(ctx):
    sub(ctx, RAB, "systemd.mask=set_mac.service", "systemd.mask=phosphor-discover-system-state@0.service systemd.mask=set_mac.service", count=1)


@check("S09", 9, "the test device tree (fdt-ro of reimu-ab-kexec.tar): every flash partition read-only, the whole-chip ones and the PNS flash too")
def s09(ctx):
    blob = tar_member(KEXEC, "fdt-ro")
    parts = flash_partitions(blob)
    if ctx.dtb_edit:
        parts = ctx.dtb_edit(parts)
    rw = sorted(l for l, ro in parts.items() if not ro)
    need = {"bmc", "u-boot", "u-boot-env", "kernel-a", "rofs-a", "kernel-b", "rofs-b", "rwfs", "sys"}
    return need <= set(parts) and not rw, "partitions %s, writable: %s" % (sorted(parts), rw or "none")


@mutant("S09", "fdt-ro marking only the slot partitions (bmc, sys left writable, as first built)")
def _(ctx):
    ctx.dtb_edit = lambda p: dict(p, bmc=False, sys=False)


IMGBB = "meta-mcst/meta-ab64/recipes-phosphor/images/obmc-phosphor-image.bbappend"


@check("S10", 10, "kernel and rofs fit their slot partitions with >= 256 KiB to spare; the build checks it")
def s10(ctx):
    t = ctx.src(IMGBB)
    guard = 'REIMU_AB_MARGIN_KB ?= "256"' in t and re.search(r"bb\.fatal\('A/B: %s leaves", t) is not None
    if ctx.sizes:
        k, r = ctx.sizes
    else:
        k = len(tar_member(UPDATE, "image-kernel"))
        rofs = tar_member(UPDATE, "image-rofs")
        r = int(re.search(rb"reimu-rofs (\d+) ", rofs[-4096:]).group(1))
    kf, rf = (0x600000 - k) // 1024, (0x1500000 - 4096 - r) // 1024
    return guard and kf >= 256 and rf >= 256, "kernel %d KiB free, rofs %d KiB free (partitions 6, 21 MiB - 4 KiB); build guard: %s" % (kf, rf, guard)


@mutant("S10", "the stock 1.0.2 rofs (40 MiB) in a slot")
def _(ctx):
    ctx.sizes = (4796416, 41943040)


@check("S11", 11, "hwmon-instantiator finds the I2C controller by realpath of /sys/bus/i2c/devices/i2c-N (6.18 has no i2c-N/device link)")
def s11(ctx):
    t = strip_comments(ctx.src(HWI), "c")
    uses_link = re.search(r'"/sys/bus/i2c/devices/i2c-%d/device"', t) is not None
    rp = "realpath" in t
    return rp and not uses_link, "realpath: %s, i2c-N/device link: %s" % (rp, uses_link)


@mutant("S11", "hwmon-instantiator.c reading the i2c-N/device link (as at 6b584a0ca7)")
def _(ctx):
    sub(ctx, HWI, 'snprintf(bus_path, 1023, "/sys/bus/i2c/devices/i2c-%d", i2c);',
        'snprintf(bus_path, 1023, "/sys/bus/i2c/devices/i2c-%d/device", i2c);')


FRUSED = [("spi-ops-mtd.c", "O_RDWR", False), ("fruid-ops.c", "    FILE *f = NULL;\n", True),
          ("i2c-ops.c", "/sys/class/i2c-adapter", False)]


@check("S17", 17, "mcst-fruid as built (pinned source + the recipe's seds): the PNS flash opened read-only, no fclose of an unset FILE *, the 6.x I2C path")
def s17(ctx):
    src = ctx.fruid_src
    if not src or not os.path.isdir(src):
        raise Skip("no mcst-fruid source (--fruid-src)")
    w = os.path.join(TMP, "fruid")
    shutil.rmtree(w, ignore_errors=True)
    shutil.copytree(src, w, ignore=shutil.ignore_patterns(".git"))
    if ctx.fruid_seds:  # the recipe's do_configure_prepend seds, as bitbake runs them
        m = re.search(r"do_configure_prepend\(\)\s*{(.*?)\n}", ctx.src(FRUBB), re.S)
        body = m.group(1) if m else "true"
        r = subprocess.run(["sh", "-ec", "S=%s\n%s" % (w, body.replace("${S}", "$S"))], capture_output=True, text=True)
        if r.returncode:
            return False, "the recipe's seds fail: " + r.stderr[-200:]
    bad = []
    for f, s, want in FRUSED:
        have = s in open(os.path.join(w, f)).read()
        if have != want:
            bad.append("%s %s %r" % (f, "lacks" if want else "has", s.strip()))
    return not bad, "; ".join(bad) or "spi-ops-mtd.c O_RDONLY, fruid-ops.c FILE *f = NULL, i2c-ops.c /sys/bus/i2c/devices"


@mutant("S17", "the pinned source without the recipe's seds")
def _(ctx):
    ctx.fruid_seds = False


RTCU = "meta-mcst/meta-common/recipes-mcst/rtcread/files/rtcread.service"


@check("S20", 20, "rtcread: mcp7940x too, no busybox-unparseable date -d, run before allow_host_boot lets the platform out of reset")
def s20(ctx):
    t = strip_comments(ctx.src(RTC), "sh")
    m = "mcp7940x" in t
    d = re.search(r"date [^\n|;]*-d", t) is None
    o = "allow_host_boot.service" in " ".join(unit_get(ctx.src(RTCU), "Before"))
    return m and d and o, "mcp7940x: %s, no date -d: %s, Before=allow_host_boot: %s" % (m, d, o)


@mutant("S20", "rtcread looking for mcp7941x only")
def _(ctx):
    sub(ctx, RTC, "for model in mcp7941x mcp7940x", "for model in mcp7941x")


HWMBB = "meta-mcst/meta-common/recipes-phosphor/phosphor-hwmon/phosphor-hwmon_%.bbappend"


@check("S21", 21, "phosphor-hwmon built without negative-errno-on-fail (a failed read was published as -errno)")
def s21(ctx):
    t = strip_comments(ctx.src(HWMBB), "sh")
    a, b = "-Dnegative-errno-on-fail=false" in t, "-Dupdate-functional-on-fail=false" in t
    return a and b, "negative-errno-on-fail=false: %s, update-functional-on-fail=false: %s" % (a, b)


@mutant("S21", "the meson defaults (true)")
def _(ctx):
    sub(ctx, HWMBB, "-Dnegative-errno-on-fail=false ", "")


HWIU = "meta-mcst/meta-common/recipes-mcst/hwmon-instantiator/files/hwmon-instantiator.service"


@check("S22", 22, "hwmon-instantiator.service cleans up after a start a power-off killed (ExecStopPost, SIGTERM no failure)")
def s22(ctx):
    t = ctx.src(HWIU)
    post = any("hwmon-instantiator stop" in v for v in unit_get(t, "ExecStopPost"))
    stop = any("hwmon-instantiator stop" in v for v in unit_get(t, "ExecStop"))
    sig = "SIGTERM" in " ".join(unit_get(t, "SuccessExitStatus"))
    return post and not stop and sig, "ExecStopPost stop: %s, ExecStop stop: %s, SuccessExitStatus SIGTERM: %s" % (post, stop, sig)


@mutant("S22", "the cleanup in ExecStop (runs only after a start that finished)")
def _(ctx):
    sub(ctx, HWIU, "ExecStopPost=-/sbin/hwmon-instantiator stop", "ExecStop=/sbin/hwmon-instantiator stop")


@check("S23", 23, "update-host-flash (PNS update) works with what the image has: through reimu-pns-flash, nothing missing")
def s23(ctx):
    t = ctx.fs("/usr/sbin/update-host-flash")
    if t is None:
        return False, "no /usr/sbin/update-host-flash"
    t = t.decode(errors="replace")
    missing = [p for p in set(re.findall(r"(/(?:usr/)?(?:s?bin|libexec)/[A-Za-z0-9_.-]+)", strip_comments(t, "sh")))
               if ctx.fs(p) is None]
    via = "reimu-pns-flash" in t
    return via and not missing, "via reimu-pns-flash: %s, missing: %s" % (via, missing or "none")


@mutant("S23", "the stock update-host-flash (firmware-updater.plain, removed on A/B)")
def _(ctx):
    ctx.rootfs["/usr/sbin/update-host-flash"] = (b"#!/bin/sh\n. /libexec/gpio-funcs\n. /libexec/libfirmware-updater\n"
                                                b"/libexec/firmware-updater.plain $DEST /dev/$MTDDEV $FLASHSIZE\n")


@check("S25", 25, "the CA truststore of phosphor-certificate-manager is in the image (Redfish Truststore 500 without it)")
def s25(ctx):
    a = ctx.fs("/usr/share/phosphor-certificate-manager/authority") is not None
    w = "phosphor-certificate-manager@authority.service" in ctx.fs_list("/lib/systemd/system/multi-user.target.wants")
    return a and w, "authority env: %s, enabled: %s" % (a, w)


@mutant("S25", "the image without phosphor-nslcd-authority-cert-config (left out with LDAP)")
def _(ctx):
    ctx.rootfs["/usr/share/phosphor-certificate-manager/authority"] = None
    ctx.rootfs["/lib/systemd/system/multi-user.target.wants/phosphor-certificate-manager@authority.service"] = None


def initrd_files():
    blob = tar_member(KEXEC, "initrd")
    d = lzma.LZMADecompressor()
    data = d.decompress(blob)
    files, p = {}, 0
    while p + 110 <= len(data) and data[p:p + 6] in (b"070701", b"070702"):
        h = data[p:p + 110]
        size, nlen = int(h[54:62], 16), int(h[94:102], 16)
        name = data[p + 110:p + 110 + nlen - 1].decode()
        p = (p + 110 + nlen + 3) & ~3
        files[name] = data[p:p + size]
        p = (p + size + 3) & ~3
        if name == "TRAILER!!!":
            break
    return files


@check("S27", 27, "BMC images only through reimu-ab: no static-layout updater, the initramfs shutdown flasher drops image-* (image-bmc would overwrite both slots)")
def s27(ctx):
    upd = [p for p in ("/usr/bin/phosphor-image-updater", "/usr/bin/obmc-flash-bmc", "/usr/sbin/firmware-updater")
           if ctx.fs(p) is not None]
    abd = ctx.fs("/usr/sbin/reimu-abd") is not None
    u = ctx.rootfs.get("initrd:update")
    if u is None:
        u = initrd_files().get("update", b"")
    guard = b"A/B firmware: " in u
    return not upd and abd and guard, "static updater: %s, reimu-abd: %s, initramfs update guard: %s" % (upd or "none", abd, guard)


@mutant("S27", "the image with phosphor-bmc-code-mgmt and the stock shutdown flasher")
def _(ctx):
    ctx.rootfs["/usr/bin/phosphor-image-updater"] = b"\x7fELF"
    ctx.rootfs["initrd:update"] = b"#!/bin/sh\nimglist=$(echo $image*)\n"


NETW = ["meta-mcst/meta-common/recipes-mcst/net-initial/files/00-bmc-eth0.network",
        "meta-mcst/meta-common/recipes-mcst/net-initial/files/00-bmc-eth1.network"]


@check("S33", 33, "networkd files say DHCP=true (phosphor-networkd reads DHCP=yes as none: lan print Static, web DHCP off)")
def s33(ctx):
    r = {os.path.basename(n): unit_get(ctx.src(n), "DHCP") for n in NETW}
    return all(v == ["true"] for v in r.values()), str(r)


@mutant("S33", "DHCP=yes")
def _(ctx):
    sub(ctx, NETW[0], "DHCP=true", "DHCP=yes")


@check("S36", 36, "phosphor-fan-control and -monitor not in the image (reimu-fand owns the fans)")
def s36(ctx):
    there = [p for p in ("/usr/bin/phosphor-fan-control", "/usr/bin/phosphor-fan-monitor",
                         "/lib/systemd/system/phosphor-fan-control@.service", "/lib/systemd/system/phosphor-fan-monitor@.service")
             if ctx.fs(p) is not None]
    fand = ctx.fs("/usr/sbin/reimu-fand") is not None
    return not there and fand, "fan control besides reimu-fand: %s; reimu-fand: %s" % (there or "none", fand)


@mutant("S36", "obmc-fan-control left in the image")
def _(ctx):
    ctx.rootfs["/usr/bin/phosphor-fan-control"] = b"\x7fELF"


@check("S37", 37, "DTS: multi-master on the buses the platform's masters share (i2c2, i2c3, i2c5, i2c6)")
def s37(ctx):
    t = ctx.src(DTS)
    r = {n: bool(re.search(r"^&%s\s*{[^}]*multi-master;" % n, t, re.M)) for n in ("i2c2", "i2c3", "i2c5", "i2c6")}
    return all(r.values()), str(r)


@mutant("S37", "i2c5 (the PCS of the CPUs) without multi-master")
def _(ctx):
    sub(ctx, DTS, '&i2c5 { status = "okay"; multi-master; };', '&i2c5 { status = "okay"; };')


@check("S40", 40, "no git source of meta-mcst left at ${AUTOREV} in the A/B build: each pinned (here or by a bbappend of meta-ab64)")
def s40(ctx):
    unpinned = []
    for rel in sorted(glob.glob(os.path.join(MC, "meta-*", "recipes-*", "*", "*.bb"))):
        r = os.path.relpath(rel, ROOT)
        t = ctx.src(r)
        if not re.search(r'^SRCREV\s*=\s*"\$\{AUTOREV\}"', t, re.M):
            continue
        pn = os.path.basename(rel).split("_")[0]
        pinned = False
        for app in glob.glob(os.path.join(AB, "recipes-*", "*", pn + "_*.bbappend")):
            if re.search(r'^SRCREV\s*=\s*"[0-9a-f]{40}"', ctx.src(os.path.relpath(app, ROOT)), re.M):
                pinned = True
        if not pinned:
            unpinned.append(pn)
    return not unpinned, "unpinned: %s" % (unpinned or "none")


@mutant("S40", "libreimu back at AUTOREV")
def _(ctx):
    sub(ctx, "meta-mcst/meta-common/recipes-mcst/libreimu/libreimu_0.1.bb", 'SRCREV = "4b706bc6d73e3c384cc995547be7cacbb2069a8a"',
        'SRCREV = "${AUTOREV}"')


DEFCONFIG = "meta-mcst/meta-ab64/recipes-kernel/linux/linux-aspeed/reimu-5564/defconfig"


@check("S43", 43, "kernel: no legacy g_mass_storage (it grabs every free vhub port, e.g. after a vhub reset), the configfs mass storage of the virtual media kept")
def s43(ctx):
    t = ctx.src(DEFCONFIG)
    legacy = re.search(r"^CONFIG_USB_MASS_STORAGE=[ym]", t, re.M)
    cfs = re.search(r"^CONFIG_USB_CONFIGFS_MASS_STORAGE=y", t, re.M)
    return not legacy and bool(cfs), "legacy g_mass_storage: %s; CONFIG_USB_CONFIGFS_MASS_STORAGE=y: %s" % (
        legacy.group(0) if legacy else "not set", bool(cfs))


@mutant("S43", "CONFIG_USB_MASS_STORAGE=y back")
def _(ctx):
    sub(ctx, DEFCONFIG, "# CONFIG_USB_MASS_STORAGE is not set", "CONFIG_USB_MASS_STORAGE=y")


@mutant("S43", "the configfs mass storage dropped with it")
def _(ctx):
    sub(ctx, DEFCONFIG, "CONFIG_USB_CONFIGFS_MASS_STORAGE=y", "# CONFIG_USB_CONFIGFS_MASS_STORAGE is not set")


PATCHES = [
    (29, "meta-mcst/meta-ab64/recipes-phosphor/ipmi/phosphor-ipmi-host_%.bbappend", "0002-Get-Device-ID-firmware-revision-Get-System-GUID.patch"),
    (26, "meta-mcst/meta-ab64/recipes-phosphor/ipmi/phosphor-ipmi-host_%.bbappend", "0001-user-channel-data-reload-when-the-file-was-replaced.patch"),
    (31, "meta-mcst/meta-ab64/recipes-phosphor/bmcweb/bmcweb_%.bbappend", "0001-NetworkProtocol-SSH-from-sshd.socket-ClearLog-without-rsyslog.patch"),
    (34, "meta-mcst/meta-ab64/recipes-phosphor/bmcweb/bmcweb_%.bbappend", "0002-Chassis-sensors-every-sensor-without-an-all_sensors-association.patch"),
    (14, "meta-mcst/meta-common/recipes-mcst/libreimu/libreimu_0.1.bb", "0001-reimu_fdt-free-the-previous-device-tree.patch"),
    (14, "meta-mcst/meta-common/recipes-mcst/libreimu/libreimu_0.1.bb", "0002-reimu_find_filename-copy-the-name-before-closedir.patch"),
    (14, "meta-mcst/meta-common/recipes-mcst/libreimu/libreimu_0.1.bb", "0003-reimu_readfile-without-a-size-read-errors-and-the-end.patch"),
]


@check("S29", "26 29 30 31 32 34 14", "the fix patches (ipmid, bmcweb, libreimu) still in SRC_URI and present")
def s29(ctx):
    bad = []
    for bug, bb, p in PATCHES:
        t = strip_comments(ctx.src(bb), "sh")
        f = os.path.join(os.path.dirname(bb), os.path.basename(bb).split("_")[0], p)
        f2 = os.path.join(os.path.dirname(bb), "files", p)
        if "file://" + p not in t or not (ctx.src_exists(f) or ctx.src_exists(f2)):
            bad.append("%s (bug %d)" % (p, bug))
    return not bad, "missing: %s" % (bad or "none")


@mutant("S29", "bmcweb without the SSH/ClearLog patch")
def _(ctx):
    sub(ctx, PATCHES[2][1], "file://" + PATCHES[2][2], "")


# ------------------------------------------------------------------ native code
def cc(*args):
    return subprocess.run(["gcc"] + list(args), capture_output=True, text=True)


LIBREIMU_TEST = r'''
/* libreimu: the three bugs overheatd ran into (bug 14), under ASan/LSan */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <libfdt.h>
#include "reimu.h"
extern const char *reimu_devtree_path;
static int cb(enum cancel_type_t c, const char *a, int b, int d, int e, const char *f, const void *g) { return 0; }
int main(int argc, char **argv)
{
    char d[] = "/tmp/libreimu-testXXXXXX", p[256];
    if (!mkdtemp(d)) return 2;
    const char *what = argv[1];
    if (!strcmp(what, "find_filename")) {
        /* the name is read after closedir(): heap-use-after-free under ASan */
        snprintf(p, sizeof p, "%s/hwmon7", d); mkdir(p, 0755);
        char *name = NULL;
        int r = reimu_find_filename(0, d, &name);
        printf("find_filename %d %s\n", r, name ? name : "-");
        return r || !name || strcmp(name, "hwmon7");
    }
    if (!strcmp(what, "readfile_text")) {
        /* without a size: NUL-terminated text (ASan fills malloc with 0xbe) */
        snprintf(p, sizeof p, "%s/temp1_input", d);
        FILE *f = fopen(p, "w"); fputs("48000\n", f); fclose(f);
        char *buf = NULL;
        int r = reimu_readfile(p, &buf, NULL);
        size_t n = r ? 0 : strnlen(buf, 4096);
        printf("readfile_text %d len %zu\n", r, n);
        return r || n != 6;
    }
    if (!strcmp(what, "readfile_error")) {
        /* a read error (EIO, as a sysfs attribute of a device that doesn't answer) is an error */
        char *buf = NULL;
        int r = reimu_readfile("/proc/self/mem", &buf, NULL);
        printf("readfile_error %d\n", r);
        return r == 0;
    }
    if (!strcmp(what, "open_dtb")) {
        /* the device tree read on every traversal: the previous copy freed (LSan) */
        char t[1024];
        fdt_create_empty_tree(t, sizeof t);
        snprintf(p, sizeof p, "%s/dtb", d);
        FILE *f = fopen(p, "w"); fwrite(t, 1, fdt_totalsize(t), f); fclose(f);
        reimu_devtree_path = p;
        for (int i = 0; i < 3; i++) reimu_traverse_all_i2c(NULL, cb, BE_SILENT);
        printf("open_dtb traversed 3 times\n");
        return 0;
    }
    return 2;
}
'''


def build_libreimu(patched):
    if not os.path.isdir(LIBREIMU_SRC) or not shutil.which("gcc"):
        raise Skip("no libreimu source (--libreimu-src) or gcc")
    w = os.path.join(TMP, "libreimu-%d" % patched)
    shutil.rmtree(w, ignore_errors=True)
    shutil.copytree(LIBREIMU_SRC, w, ignore=shutil.ignore_patterns(".git"))
    if patched:
        pd = os.path.join(COMMON, "recipes-mcst", "libreimu", "files")
        bb = open(os.path.join(COMMON, "recipes-mcst", "libreimu", "libreimu_0.1.bb")).read()
        for p in re.findall(r"file://([^ \\\n]+\.patch)", bb):
            r = subprocess.run(["patch", "-p1", "-d", w, "-i", os.path.join(pd, p)], capture_output=True, text=True)
            if r.returncode:
                raise Skip("libreimu: %s doesn't apply: %s" % (p, r.stdout[-200:]))
    open(os.path.join(w, "t.c"), "w").write(LIBREIMU_TEST)
    r = cc("-g", "-O0", "-fsanitize=address,undefined", "-I", w, "-o", os.path.join(w, "t"),
           os.path.join(w, "t.c"), os.path.join(w, "reimu.c"), os.path.join(w, "reimu_fdt.c"), "-lfdt")
    if r.returncode:
        raise Skip("libreimu test doesn't build: " + r.stderr[-300:])
    return os.path.join(w, "t")


@check("S14", 14, "libreimu (as patched by the recipe) under ASan: reimu_open_dtb frees the previous DTB, reimu_find_filename no use-after-free, reimu_readfile without a size: text ends, read errors reported")
def s14(ctx):
    t = build_libreimu(ctx.libreimu_patched)
    res = {}
    for case in ("find_filename", "readfile_text", "readfile_error", "open_dtb"):
        r = subprocess.run([t, case], capture_output=True, text=True,
                           env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1", LSAN_OPTIONS="exitcode=23"))
        err = re.search(r"ERROR: (?:Address|Leak)Sanitizer: ([\w-]+)", r.stderr)
        res[case] = "ok" if r.returncode == 0 else (err.group(1) if err else "rc %d %s" % (r.returncode, r.stdout.strip()))
    return all(v == "ok" for v in res.values()), str(res)


@mutant("S14", "libreimu at the pinned revision without the three patches")
def _(ctx):
    ctx.libreimu_patched = False


FAND = os.path.join(COMMON, "recipes-mcst", "reimu-fand", "files")
FAND_MUTANTS = [
    ("35a", "fand.c", "    return 2 * freq;\n}", "    return 255;\n}",
     "LM96163 PWM full scale 255 (the register is 6 bits, 100 % = 2 * PWM_FREQ)"),
    ("35b", "fand.c", "if (cpu->has_tach && cpu->ident != -1) read_tach(f, cpu);",
     "if (cpu->has_tach && cpu->ok) read_tach(f, cpu);", "the tachometer not read after a write that didn't read back"),
    ("35c", "fand.c", "double valid = o->tach_mode_ok ? fmax(c->tach_valid_duty, 0) : c->tach_valid_duty_other;",
     "double valid = 0;", "the tachometer trusted at any duty (its dropouts below 50 % taken as a stopped fan)"),
    ("35d", "fand.c", "    c->tach_mode = 2;\n", "    c->tach_mode = 3;\n", "TACH_MODE left at 11 (least effect), not 10 (accurate)"),
]


@check("S35", 35, "reimu-fand unit tests (tests/test_fand.c, ASan): PWM scale, tach after a failed readback, tach dropouts, tach mode")
def s35(ctx):
    if not shutil.which("gcc"):
        raise Skip("no gcc")
    w = os.path.join(TMP, "fand")
    shutil.rmtree(w, ignore_errors=True)
    shutil.copytree(FAND, w)
    if ctx.fand_mutant:
        f, old, new = ctx.fand_mutant
        t = open(os.path.join(w, f)).read()
        if old not in t:
            raise Skip("mutant: %r not in %s" % (old, f))
        open(os.path.join(w, f), "w").write(t.replace(old, new))
    r = subprocess.run(["make", "-s", "-C", w, "test"], capture_output=True, text=True, timeout=600)
    fails = [l.strip() for l in r.stdout.splitlines() if "FAIL" in l]
    summary = (r.stdout.strip().splitlines() or [""])[-1]
    return r.returncode == 0 and not fails, "%s%s" % (summary[:120], ("; " + " | ".join(fails[:3])[:300]) if fails else "")


for _id, _f, _old, _new, _what in FAND_MUTANTS:
    def _m(ctx, f=_f, old=_old, new=_new):
        ctx.fand_mutant = (f, old, new)
    next(c for c in CHECKS if c[0] == "S35")[4].append(("%s: %s" % (_id, _what), _m))


# ------------------------------------------------------------------ run
def run_one(fn, ctx):
    try:
        ok, detail = fn(ctx)
        return (None if ok is None else bool(ok)), detail
    except Skip as e:
        return None, str(e)
    except (OSError, ValueError, KeyError, AttributeError, subprocess.SubprocessError) as e:
        return False, "error: %r" % e


def main():
    fails = skips = red_missing = 0
    for cid, bug, what, fn, muts in CHECKS:
        if ONLY and cid not in ONLY and cid.rstrip("i") not in ONLY:
            continue
        ok, detail = run_one(fn, Ctx())
        tag = {True: "PASS", False: "FAIL", None: "SKIP"}[ok]
        print("%s %s [bug %s] %s: %s" % (tag, cid, bug, what, detail), flush=True)
        fails += ok is False
        skips += ok is None
        if RED:
            for mwhat, m in muts:
                ctx = Ctx()
                try:
                    m(ctx)
                except Skip as e:
                    print("  RED? %s mutant %s: %s" % (cid, mwhat, e))
                    red_missing += 1
                    continue
                mok, mdetail = run_one(fn, ctx)
                if mok is False:
                    print("  RED  %s with the bug back (%s): FAIL as it must: %s" % (cid, mwhat, mdetail), flush=True)
                else:
                    print("  RED? %s with the bug back (%s): %s, the check doesn't see it: %s" % (
                        cid, mwhat, "PASS" if mok else "SKIP", mdetail), flush=True)
                    red_missing += 1
    print("%d failed, %d skipped%s" % (fails, skips, ", %d mutants not caught" % red_missing if RED else ""))
    shutil.rmtree(TMP, ignore_errors=True)
    return 1 if fails or red_missing else 0


if __name__ == "__main__":
    sys.exit(main())
