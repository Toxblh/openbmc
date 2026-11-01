#!/usr/bin/env python3
"""reimu-pns-flash and the D-Bus methods of reimu-pnsflash.c without QEMU
(REGRESSIONS 80-83): the real script with the chip a file, its hooks for the
switch and the power, the real reimu-flash behind a wrapper that can fail,
interrupt or lie; the real C file through unit/pnsflash_harness.c.
  keep   a half-written chip never replaces the good backup on the BMC flash
  pin    the job writes the file it checked, not what the path holds later;
         Upload, upload-done and discard refused while a job runs
  warn   a backup that couldn't be kept is told, the write goes on
  blank  Run write-blank: --blank-ok; the board check stays
  format what reimu-flash prints is what do_write's arithmetic takes

    test_pns_tool.py [keep|pin|warn|blank|format ...]     (SH: the shell, default sh)
"""
import base64, gzip, hashlib, json, os, re, shutil, signal, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
META = os.path.join(HERE, "..", "..")
TOOL = os.path.join(META, "meta-ab64/recipes-mcst/reimu-pns-flash/files/reimu-pns-flash")
FLASH_C = os.path.join(META, "meta-ab64/recipes-mcst/reimu-ab/files/reimu-flash.c")
PNS_DIR = os.path.join(META, "meta-common/recipes-mcst/reimu-pns/files")
SH = os.environ.get("SH", "sh")
ONLY = sys.argv[1:]
CHIP = 0x800000
B4K = 4096
OFF = "xyz.openbmc_project.State.Host.HostState.Off"
RUNNING = "xyz.openbmc_project.State.Host.HostState.Running"
failed = 0


def check(c, what):
    global failed
    print(("PASS " if c else "FAIL ") + what)
    failed += not c


def sha(b):
    return hashlib.sha256(b).hexdigest()


def image(fill=1, tag="E8C2", compat="mcst,e8c2-test", size=0x710000, dtb=True):
    """what check_image takes for a PNS image; every byte depends on fill"""
    d = bytearray(bytes([fill]) * size)
    d[0:8] = bytes.fromhex("12500000200000c0")
    v = b"\0lintel-%s ::::::: ( unified-boot/rel-%d )(%s)\0" % (tag.encode(), fill, tag.encode())
    d[0x1000:0x1000 + len(v)] = v
    d[0x48fff8:0x490000] = b"deadbeef"
    if dtb:
        t = b"\0compatible=%s\0version=1.%d\0model=M\0" % (compat.encode(), fill)
        d[0x700000:0x700008] = struct.pack(">II", 0xd00dfeed, 0x1000)
        d[0x700008:0x700008 + len(t)] = t
    return bytes(d)


def chipped(img):
    return img + b"\xff" * (CHIP - len(img))


A, B, C = image(1), image(2), image(3)
ERASED = b"\xff" * CHIP
OTHER = image(4, tag="E16C", compat="mcst,e16c-other")

STUBS = {
    "fdtget": r"""#!/bin/sh
f=
for a in "$@"; do case $a in -t|s|/) ;; *) if [ -z "$f" ]; then f=$a; else p=$a; fi ;; esac; done
v=$(grep -a -o "$p=[^[:cntrl:]]*" "$f" | head -1)
[ -n "$v" ] || exit 1
echo "${v#*=}"
""",
    "logger": "#!/bin/sh\n",
    "sleep": "#!/bin/sh\nexec %(sleep)s 0.05\n",
    "sync": '#!/bin/sh\necho "sync marker=$([ -e $KEEP/write-unfinished ] && echo 1 || echo 0)" >> $T/log\n',
    "cp": r"""#!/bin/sh
if [ -f $T/cp-fail ]; then
    for a in "$@"; do case $a in $(cat $T/cp-fail)) exit 1 ;; esac; done
fi
exec %(cp)s "$@"
""",
    "ln": '#!/bin/sh\n[ -f $T/ln-fail ] && exit 1\nexec %(ln)s "$@"\n',
    # write DEV 0 FILE TRIES; "fail N": N blocks of FILE written, an error;
    # "cut N": the same, then everything of the job is killed; "lie": nothing
    # written, says it was
    "flash": r"""#!/bin/sh
echo "flash marker=$([ -e $KEEP/write-unfinished ] && echo 1 || echo 0) file=$4 inode=$(stat -L -c %%i "$4")" >> $T/log
[ -f $T/hook-flash ] && { sh $T/hook-flash; rm -f $T/hook-flash; }
mode=
[ -f $T/flash-mode ] && read -r mode n < $T/flash-mode
case "$mode" in
    fail) dd if="$4" of="$2" bs=4096 count=$n conv=notrunc 2>/dev/null; echo "reimu-flash: injected" >&2; exit 1 ;;
    cut) dd if="$4" of="$2" bs=4096 count=$n conv=notrunc 2>/dev/null; kill -9 -$(cut -d' ' -f5 /proc/$$/stat) ;;
    lie) echo "ok 1"; exit 0 ;;
esac
exec %(real)s write "$2" "$3" "$4" "$5"
""",
    "switch": '#!/bin/sh\n[ "$1" = 1 ] && [ -f $T/hook-switch ] && { sh $T/hook-switch; rm -f $T/hook-switch; }\n'
              '[ "$1" = 0 ] && [ -f $T/switch-stuck ] && exit 1\nexit 0\n',
    "power": "#!/bin/sh\necho 0\n",
    "systemctl": '#!/bin/sh\nn=$(cat $T/job-states 2>/dev/null | head -1)\n'
                 '[ -n "$n" ] && sed -i 1d $T/job-states\necho "${n:-$(cat $T/job-state)}"\n',
    "systemd-run": r"""#!/bin/sh
while [ "$1" != "$REIMU_PNS_FLASH" ]; do shift; done
shift
echo "$*" >> $T/job-args
[ -f $T/job-real ] && "$REIMU_PNS_FLASH" "$@" >> $T/job-out 2>&1
exit 0
""",
}


def build(d):
    real = os.path.join(d, "reimu-flash")
    subprocess.check_call(["gcc", "-O2", "-o", real, FLASH_C])
    h = os.path.join(d, "harness")
    subprocess.check_call(["gcc", "-w", "-O1", "-g", "-I" + PNS_DIR, "-I" + os.path.join(HERE, "unit/pnsflash-stubs"),
                           "-o", h, os.path.join(HERE, "unit/pnsflash_harness.c")])
    return real, h


class Env:
    def __init__(self, chip=A):
        self.t = tempfile.mkdtemp(dir=TOP)
        for n in ("run", "work", "keep", "bin"):
            os.makedirs(self.p(n))
        self.run_d, self.work, self.keep = self.p("run"), self.p("work"), self.p("keep")
        self.chip_f, self.bin = self.p("chip"), self.p("bin")
        self.tool = self.p("reimu-pns-flash")
        shutil.copy(TOOL, self.tool)
        os.chmod(self.tool, 0o755)
        subst = {"real": REAL, "sleep": shutil.which("sleep"), "cp": shutil.which("cp"), "ln": shutil.which("ln")}
        for n, s in STUBS.items():
            f = os.path.join(self.bin, n)
            open(f, "w").write(s % subst if "%(" in s else s)
            os.chmod(f, 0o755)
        self.set_chip(chipped(chip))
        self.env = dict(os.environ, T=self.t, KEEP=self.keep, REIMU_PNS_RUN=self.run_d, REIMU_PNS_WORK=self.work,
                        REIMU_PNS_KEEP=self.keep, REIMU_FLASH=self.bin + "/flash", REIMU_PNS_DEV=self.chip_f,
                        REIMU_PNS_SWITCH=self.bin + "/switch", REIMU_PNS_POWER=self.bin + "/power",
                        REIMU_PNS_FLASH=self.tool, H_SYSTEMCTL=self.bin + "/systemctl",
                        H_SYSTEMD_RUN=self.bin + "/systemd-run", PATH=self.bin + ":" + os.environ["PATH"])
        self.job_state("inactive")

    def p(self, *a):
        return os.path.join(self.t, *a)

    def put(self, name, data):
        f = self.p(name) if not name.startswith("/") else name
        with open(f + ".new", "wb") as o:
            o.write(data)
        os.rename(f + ".new", f)
        return f

    def flag(self, name, text=""):
        open(self.p(name), "w").write(text)

    def set_chip(self, data):
        open(self.chip_f, "wb").write(data)

    def chip(self):
        return open(self.chip_f, "rb").read()

    def fail(self, n=16):
        self.flag("flash-mode", "fail %d\n" % n)

    def good(self):
        if os.path.exists(self.p("flash-mode")):
            os.unlink(self.p("flash-mode"))

    def job_state(self, s):
        self.flag("job-state", s + "\n")

    def tool_write(self, *a):
        r = subprocess.run([SH, self.tool, "write"] + list(a), env=self.env, capture_output=True, text=True, timeout=300)
        return r.returncode, r.stdout + r.stderr

    def state(self):
        try:
            return json.load(open(self.run_d + "/state.json"))
        except (OSError, ValueError):
            return {}

    def last(self):
        """what backup-last.bin.gz unpacks to, None without it"""
        try:
            return gzip.open(self.keep + "/backup-last.bin.gz").read()
        except OSError:
            return None

    def chip_json(self):
        try:
            return json.load(open(self.keep + "/chip.json"))
        except OSError:
            return {"info": {}}

    def marker(self):
        return os.path.exists(self.keep + "/write-unfinished")

    def log(self):
        try:
            return open(self.p("log")).read()
        except OSError:
            return ""

    def call(self, *a):
        r = subprocess.run([HARNESS] + list(a), env=self.env, capture_output=True, text=True, timeout=120)
        try:
            return json.loads(r.stdout.strip().splitlines()[-1])
        except (ValueError, IndexError):
            return {"ok": False, "message": "harness: rc %d %s %s" % (r.returncode, r.stdout, r.stderr)}

    def uploaded(self, data):
        return self.put(self.work + "/upload.bin", data)

    def done(self):
        shutil.rmtree(self.t, ignore_errors=True)


def first_write(e, img=B):
    """a real write of A -> ... that fails half way: the good backup is kept
    from the chip it found, the chip is left half-written (it still has its
    magic: written from offset 0)"""
    f = e.put("first.bin", img)
    e.fail()
    rc, o = e.tool_write(f)
    return rc, o, f


def half(img, old=A, n=16):
    return chipped(img)[:n * B4K] + chipped(old)[n * B4K:]


# ------------------------------------------------------------------ keep
def t_keep():
    # failed write, then "last" again and again
    e = Env()
    rc, o, f = first_write(e)
    check(rc != 0 and e.chip() == half(B), "[keep] the write fails half way, the chip half-written with its magic")
    check(e.chip()[:8] == A[:8], "[keep]   (the half-written chip has the magic)")
    check(e.last() == chipped(A), "[keep]   the good chip kept on the BMC flash")
    check(e.marker(), "[keep]   the marker is on the BMC flash after the failed write")
    rc, o = e.tool_write(e.keep + "/backup-last.bin.gz")
    check(rc != 0 and e.last() == chipped(A), "[keep] failed write, retry of \"last\" fails too: \"last\" still the good chip (%s)"
          % ("intact" if e.last() == chipped(A) else "a half-written dump"))
    check("write-unfinished" in o or "unfinished" in o, "[keep]   the skip is logged")
    check(not e.state().get("warning"), "[keep]   the skip is no warning (%s)" % e.state().get("warning"))
    e.good()
    rc, o = e.tool_write(e.keep + "/backup-last.bin.gz")
    check(rc == 0 and e.chip() == chipped(A), "[keep] the retry that works: the good chip back (%s)" % o[-120:])
    check(e.last() == chipped(A) and not e.marker(), "[keep]   \"last\" intact, the marker gone")
    e.done()

    # failed write, then another image
    e = Env()
    first_write(e)
    fc = e.put("c.bin", C)
    e.fail(8)
    rc, o = e.tool_write(fc)
    check(rc != 0 and e.last() == chipped(A), "[keep] failed write, another image fails too: \"last\" still the good chip")
    e.good()
    rc, o = e.tool_write(fc)
    check(rc == 0 and e.chip() == chipped(C) and e.last() == chipped(A) and not e.marker(),
          "[keep] failed write, another image written: \"last\" still the good chip, marker gone")
    rc, o = e.tool_write(e.p("first.bin"))
    check(rc == 0 and e.chip() == chipped(B) and e.last() == chipped(C),
          "[keep] the next write keeps a backup again: \"last\" = the chip before it")
    e.done()

    # the BMC is reset in the middle: the job killed, RAM gone, the flash stays
    e = Env()
    f = e.put("first.bin", B)
    e.flag("flash-mode", "cut 16\n")
    p = subprocess.Popen([SH, e.tool, "write", f], env=e.env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    rc = p.wait(timeout=300)
    check(rc == -signal.SIGKILL and e.chip() == half(B), "[keep] power cut in the write: the chip half-written")
    for d in (e.run_d, e.work):
        shutil.rmtree(d)
        os.makedirs(d)
    check(e.marker(), "[keep] power cut: the marker survives on the BMC flash")
    e.fail(8)
    rc, o = e.tool_write(e.p("first.bin"))
    check(rc != 0 and e.last() == chipped(A), "[keep] power cut, the retry fails: \"last\" still the good chip")
    e.good()
    rc, o = e.tool_write(e.keep + "/backup-last.bin.gz")
    check(rc == 0 and e.chip() == chipped(A) and not e.marker(), "[keep] power cut, then \"last\" written: the good chip back, marker gone")
    e.done()

    # a chip with no image: never "the good copy", marker or not
    for name, content in (("erased", ERASED), ("garbage", b"\x55" * CHIP)):
        e = Env()
        f = e.put("first.bin", B)
        e.tool_write(f)  # "last" = A, the chip = B
        check(e.last() == chipped(A), "[keep] (%s) a good \"last\" to start with" % name)
        e.set_chip(content)
        rc, o = e.tool_write(e.put("c.bin", C), "--blank-ok", "--any-board")
        check(rc == 0 and e.chip()[:len(C)] == C, "[keep] %s chip, no marker: written (%s)" % (name, o[-120:]))
        check(e.last() == chipped(A), "[keep] %s chip, no marker: its dump doesn't replace \"last\"" % name)
        check("не сохран" not in e.state().get("warning", ""), "[keep] %s chip: the skip is no warning (%s)" % (name, e.state().get("warning")))
        e.done()

    # the chip already identical, the marker from an earlier job: known good
    e = Env()
    first_write(e)
    e.set_chip(chipped(B))
    rc, o = e.tool_write(e.p("first.bin"))
    check(rc == 0 and "уже содержит" in o and e.last() == chipped(A), "[keep] chip identical to the image: nothing written, \"last\" untouched")
    check(not e.marker(), "[keep] chip identical to the image: the marker cleared")
    e.done()

    # the marker is there until the independent read passed
    e = Env()
    f = e.put("first.bin", B)
    e.flag("flash-mode", "lie\n")
    rc, o = e.tool_write(f)
    check(rc != 0 and "Сверка не прошла" in o and e.marker(), "[keep] reimu-flash says ok, the final read differs: the marker stays")
    e.done()

    # on the disk before the first write
    e = Env()
    f = e.put("first.bin", B)
    rc, o = e.tool_write(f)
    lines = e.log().splitlines()
    fl = [i for i, l in enumerate(lines) if l.startswith("flash ")]
    sy = [i for i, l in enumerate(lines) if l == "sync marker=1"]
    check(rc == 0 and fl and "marker=1" in lines[fl[0]], "[keep] the marker exists when reimu-flash starts (%s)" % lines)
    check(fl and sy and sy[0] < fl[0], "[keep] ... and was synced before it")
    check(not e.marker() and lines[-1:] == ["sync marker=0"], "[keep] gone after the good write, that synced too")
    e.done()

    # the marker can't be made: nothing is touched
    if os.geteuid() != 0:
        e = Env()
        f = e.put("first.bin", B)
        os.chmod(e.keep, 0o555)
        rc, o = e.tool_write(f)
        os.chmod(e.keep, 0o755)
        check(rc != 0 and e.chip() == chipped(A) and "flash " not in e.log(), "[keep] the marker can't be written: refused, the chip not touched (%s)" % o[-160:])
        e.done()


# ------------------------------------------------------------------ pin
def t_pin():
    # the upload swapped under the job: after the check (at the switch), and
    # between the backup and the write
    for where in ("hook-switch", "hook-flash"):
        e = Env()
        up = e.uploaded(B)
        evil = e.put("evil.bin", C)
        e.flag(where, "mv %s %s\n" % (evil, up))
        rc, o = e.tool_write(up)
        check(rc == 0 and e.chip() == chipped(B), "[pin] upload.bin swapped at %s: the checked image written (rc %d, %s)"
              % (where, rc, "C on the chip" if e.chip()[:len(C)] == C else o[-100:]))
        check(not os.path.exists(up), "[pin]   upload.bin removed at the end, as before")
        e.done()
    # a console's file, the other board's, after the board was to be checked
    e = Env()
    up = e.put("i.bin", B)
    evil = e.put("evil.bin", OTHER)
    e.flag("hook-switch", "mv %s %s\n" % (evil, up))
    rc, o = e.tool_write(up)
    check(rc == 0 and e.chip() == chipped(B), "[pin] FILE swapped for another board's at the switch: the checked one written")
    check(open(up, "rb").read() == OTHER, "[pin]   a FILE other than the upload is left alone")
    e.done()
    # by hard link when it can: no second copy in RAM; the path handed over is a snapshot
    e = Env()
    up = e.uploaded(B)
    ino = os.stat(up).st_ino
    rc, o = e.tool_write(up)
    line = [l for l in e.log().splitlines() if l.startswith("flash ")][0]
    check(rc == 0 and "inode=%d" % ino in line, "[pin] a hard link: no copy in RAM (%s)" % line)
    check("upload.bin" not in line, "[pin] reimu-flash gets the snapshot, not the path")
    check(not [n for n in os.listdir(e.work) if n.startswith(".tmp.")], "[pin] the snapshot gone at the end")
    e.done()
    # no hard link: a copy
    e = Env()
    up = e.uploaded(B)
    evil = e.put("evil.bin", C)
    e.flag("ln-fail")
    e.flag("hook-flash", "mv %s %s\n" % (evil, up))
    rc, o = e.tool_write(up)
    check(rc == 0 and e.chip() == chipped(B), "[pin] ln fails: a copy, the checked image written (%s)" % o[-100:])
    e.done()
    # neither: no job
    e = Env()
    up = e.uploaded(B)
    e.flag("ln-fail")
    e.flag("cp-fail", "*upload.bin*")
    rc, o = e.tool_write(up)
    check(rc != 0 and e.chip() == chipped(A) and "flash " not in e.log(), "[pin] no snapshot possible: refused, the chip not touched (%s)" % o[-120:])
    e.done()
    # a symlink isn't pinned by a link of itself
    e = Env()
    real = e.put("real.bin", B)
    lnk = e.p("link.bin")
    os.symlink(real, lnk)
    evil = e.put("evil.bin", C)
    e.flag("hook-flash", "mv %s %s\n" % (evil, real))
    rc, o = e.tool_write(lnk)
    check(rc == 0 and e.chip() == chipped(B), "[pin] FILE a symlink, its target swapped: the checked image written (%s)" % o[-100:])
    e.done()
    # not there: the same refusal as before
    e = Env()
    rc, o = e.tool_write(e.p("nothing.bin"))
    check(rc != 0 and "не найден" in o and e.chip() == chipped(A), "[pin] no such file: said so (%s)" % o[-120:])
    e.done()
    # the identical path removes the upload too
    e = Env(chip=B)
    up = e.uploaded(B)
    rc, o = e.tool_write(up)
    check(rc == 0 and "уже содержит" in o and not os.path.exists(up), "[pin] nothing to write: upload.bin removed, as before")
    e.done()

    # the daemon: a job running
    e = Env()
    up = e.uploaded(B)
    data = base64.b64encode(b"x" * 4096).decode()
    for st in ("active", "activating"):
        e.job_state(st)
        e.uploaded(B)
        open(up + ".part", "wb").write(C)
        r = e.call("upload", "0", data)
        check(not r["ok"] and "операц" in r["message"] and os.path.getsize(up + ".part") == len(C),
              "[pin] job %s: Upload refused (%s)" % (st, r["message"]))
        open(up + ".part", "wb").write(C)
        r = e.call("run", "upload-done", sha(C))
        check(not r["ok"] and "операц" in r["message"] and open(up, "rb").read() == B,
              "[pin] job %s: upload-done refused, upload.bin the same (%s)" % (st, r["message"]))
        r = e.call("run", "discard", "")
        check(not r["ok"] and "операц" in r["message"] and os.path.exists(up) and os.path.exists(up + ".part"),
              "[pin] job %s: discard refused, upload.bin there (%s)" % (st, r["message"]))
    e.uploaded(B)
    e.job_state("active")
    r = e.call("run", "check", "")
    check("check" in r, "[pin] job active: check not refused (%s)" % r["message"])
    check(e.call("json").get("upload"), "[pin] job active: the status is read")
    e.done()
    # not known: as if a job runs
    e = Env()
    up = e.uploaded(B)
    e.env["H_SYSTEMCTL"] = e.p("no-such-program")
    r = e.call("run", "discard", "")
    check(not r["ok"] and os.path.exists(up), "[pin] systemctl can't be asked: refused (%s)" % r["message"])
    e.done()
    # no job
    for st in ("inactive", "failed"):
        e = Env()
        up = e.uploaded(B)
        e.job_state(st)
        r = e.call("upload", "0", base64.b64encode(b"x" * 4096).decode())
        check(r["ok"] and os.path.getsize(up + ".part") == 4096, "[pin] job %s: Upload works (%s)" % (st, r["message"]))
        open(up + ".part", "wb").write(C)
        r = e.call("run", "upload-done", sha(C))
        check(r["ok"] and open(up, "rb").read() == C, "[pin] job %s: upload-done works (%s)" % (st, r["message"]))
        r = e.call("run", "discard", "")
        check(r["ok"] and not os.path.exists(up), "[pin] job %s: discard works" % st)
        e.done()
    # the unit ending (ExecStopPost): waited for, like start_job does
    e = Env()
    open(e.p("job-states"), "w").write("deactivating\ndeactivating\n")
    r = e.call("upload", "0", base64.b64encode(C[:4096]).decode())
    check(r["ok"], "[pin] job deactivating, then gone: Upload waits for it and works (%s)" % r["message"])
    e.done()


# ------------------------------------------------------------------ warn
def t_warn():
    W = "не сохран"
    # over KEEP_MAX
    e = Env()
    e.env["REIMU_PNS_KEEP_MAX"] = "10"
    f = e.put("first.bin", B)
    rc, o = e.tool_write(f)
    s = e.state()
    check(rc == 0 and s.get("state") == "ok" and e.chip() == chipped(B), "[warn] over KEEP_MAX: the write goes on")
    check(e.last() is None, "[warn]   nothing kept")
    check(W in s.get("warning", ""), "[warn] over KEEP_MAX: the warning in the state (%r)" % s.get("warning"))
    check(W in s.get("message", ""), "[warn] over KEEP_MAX: in the finish message (%r)" % s.get("message"))
    check("внимание: " in o and W in o, "[warn] over KEEP_MAX: on the console")
    check(not e.marker(), "[warn]   the marker works all the same")
    e.done()
    # cp fails, on the gzip and on the sidecar
    for pat in ("*backup-last.bin.gz.tmp", "*backup-last.bin.sha256.tmp"):
        e = Env()
        e.flag("cp-fail", pat)
        rc, o = e.tool_write(e.put("first.bin", B))
        s = e.state()
        check(rc == 0 and e.chip() == chipped(B) and W in s.get("warning", ""), "[warn] cp to %s fails: warned, written (%r)" % (pat, s.get("warning")))
        check(not [n for n in os.listdir(e.keep) if n.endswith(".tmp")], "[warn]   no .tmp left")
        e.done()
    # the warning that was there stays: an image without a device tree
    e = Env()
    e.env["REIMU_PNS_KEEP_MAX"] = "10"
    rc, o = e.tool_write(e.put("short.bin", image(5, size=0x490000, dtb=False)))
    s = e.state()
    check(rc == 0 and "Образ без device tree" in s.get("warning", "") and W in s.get("warning", ""),
          "[warn] appended to the image's warning (%r)" % s.get("warning"))
    e.done()
    # ... and so does ours when the switch back fails afterwards
    e = Env()
    e.env["REIMU_PNS_KEEP_MAX"] = "10"
    e.flag("switch-stuck")
    rc, o = e.tool_write(e.put("first.bin", B))
    s = e.state()
    check(W in s.get("warning", "") and "переключить" in s.get("warning", ""), "[warn] the failed switch back doesn't erase it (%r)" % s.get("warning"))
    e.done()
    # a write that fails: told as well
    e = Env()
    e.env["REIMU_PNS_KEEP_MAX"] = "10"
    rc, o, f = first_write(e)
    s = e.state()
    check(rc != 0 and s.get("state") == "failed" and W in s.get("warning", ""), "[warn] a failed write: the warning in the state (%r)" % s.get("warning"))
    e.done()
    # kept fine: no warning
    e = Env()
    rc, o = e.tool_write(e.put("first.bin", B))
    s = e.state()
    check(rc == 0 and not s.get("warning") and W not in s.get("message", "") and W not in o, "[warn] kept: no warning (%r)" % s.get("warning"))
    e.done()


# ------------------------------------------------------------------ blank
def no_image_keeps_dtb(img=A):
    """the first block erased, the device tree there"""
    c = bytearray(chipped(img))
    c[0:0x10000] = b"\xff" * 0x10000
    return bytes(c)


def t_blank():
    # Run write-blank -> the job's arguments
    e = Env()
    up = e.uploaded(B)
    r = e.call("run", "write-blank", "upload")
    args = open(e.p("job-args")).read().strip() if os.path.exists(e.p("job-args")) else ""
    check(r["ok"] and args == "write %s --blank-ok" % up, "[blank] write-blank upload: the job gets --blank-ok (%s; %s)" % (r["message"], args))
    e.done()
    for act, opt in (("write", ""), ("write-any-board", " --any-board")):
        e = Env()
        up = e.uploaded(B)
        e.call("run", act, "upload")
        check(open(e.p("job-args")).read().strip() == "write %s%s" % (up, opt), "[blank] %s as before" % act)
        e.done()
    # the same gates as write
    e = Env()
    r = e.call("run", "write-blank", "upload")
    check(not r["ok"] and "Нет такого образа" in r["message"], "[blank] write-blank with no upload: refused (%s)" % r["message"])
    e.uploaded(B)
    r = e.call("run", "write-blank", "../../etc/passwd")
    check(not r["ok"] and "Нет такого образа" in r["message"], "[blank] write-blank with a bad name: refused")
    e.env["H_HOST"] = RUNNING
    r = e.call("run", "write-blank", "upload")
    check(not r["ok"] and "Платформа включена" in r["message"] and not os.path.exists(e.p("job-args")), "[blank] write-blank, host on: refused (%s)" % r["message"])
    e.done()
    # really: through the daemon's job onto a chip without an image, the device tree there
    e = Env()
    e.set_chip(no_image_keeps_dtb())
    e.flag("job-real")
    e.uploaded(B)
    e.call("run", "write", "upload")
    check(e.chip() == no_image_keeps_dtb() and "читается пустым" in open(e.p("job-out")).read(), "[blank] plain write onto a chip with no image: refused")
    chipjson = e.chip_json()
    check(chipjson["info"].get("magic") is False, "[blank] the refusal leaves the chip as read in chip.json: the page knows (%s)" % chipjson["info"])
    r = e.call("run", "write-blank", "upload")
    st = e.state()
    check(r["ok"] and st.get("state") == "ok" and e.chip()[:len(B)] == B, "[blank] write-blank: the re-uploaded image written (%s)" % st.get("message"))
    chipjson = e.chip_json()
    check(chipjson["info"].get("magic") is True, "[blank] chip.json: an image now")
    e.done()
    # an image of another board: the device tree on the chip still says no
    e = Env()
    e.set_chip(no_image_keeps_dtb())
    e.flag("job-real")
    e.uploaded(OTHER)
    e.call("run", "write-blank", "upload")
    st = e.state()
    check(st.get("state") == "failed" and "другой платы" in st.get("message", "") and e.chip() == no_image_keeps_dtb(),
          "[blank] write-blank, another board's image: refused by the board check (%s)" % st.get("message"))
    e.done()
    # nothing on the chip to check the board against: as today, not written
    e = Env(chip=A)
    e.set_chip(ERASED)
    e.flag("job-real")
    e.uploaded(B)
    e.call("run", "write-blank", "upload")
    st = e.state()
    check(st.get("state") == "failed" and "Плата неизвестна" in st.get("message", "") and e.chip() == ERASED,
          "[blank] write-blank onto an erased chip without a device tree: the board check refuses (%s)" % st.get("message"))
    e.done()


# ------------------------------------------------------------------ format
def t_format():
    """the "while read" in do_write does arithmetic on what reimu-flash prints:
    only these lines, whatever the image"""
    e = Env(chip=ERASED)
    f = e.put("first.bin", B)
    r = subprocess.run([REAL, "write", e.chip_f, "0", f, "5"], capture_output=True, text=True)
    lines = r.stdout.splitlines()
    bad = [l for l in lines if not re.fullmatch(r"(write|verify) \d+%|ok \d+", l)]
    check(r.returncode == 0 and lines and not bad, "[format] reimu-flash's stdout: %d lines, all numeric (%s)" % (len(lines), bad))
    e.done()


TOP = tempfile.mkdtemp(prefix="pnstool-", dir=os.environ.get("TMPDIR"))
REAL, HARNESS = build(TOP)
try:
    for name, fn in (("keep", t_keep), ("pin", t_pin), ("warn", t_warn), ("blank", t_blank), ("format", t_format)):
        if not ONLY or name in ONLY:
            fn()
finally:
    shutil.rmtree(TOP, ignore_errors=True)
print("RESULT:", "FAILED %d" % failed if failed else "ALL PASSED")
sys.exit(1 if failed else 0)
