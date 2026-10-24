#!/usr/bin/env python3
"""The PNS (host boot firmware) flash and the BMC update of the A/B image, in
QEMU: ast2500-evb with the A/B build (see test_ab_qemu.py) and a second SPI
NOR, an n25q512ax3 like the board's, on spi1 ("sys"), loaded with a copy of
the board's PNS dump.

    test_pns_qemu.py WORKDIR [TEST...]

WORKDIR holds what test_ab_qemu.py needs (ab.mtd, reimu-ab.tar) and
  pns-orig.bin   the 64 MiB dump of the board's PNS flash (unified-boot
                 release-2.30.13.1-E8C2, device tree 1.0_2025-02-06)
  pns-new.bin    another image (the colleagues' 7 MiB pns+devtree_2.bin)
  pns-fault.so   LD_PRELOAD that breaks writes (pns-fault.c; built with the
                 A/B build's cross compiler when missing and docker is there)
Environment: RQ, AB_SSH, AB_HTTPS as for test_ab_qemu.py (RQ short: unix
socket paths); E2E=DIR with node_modules/playwright for the browser test
(../e2e/pns.qemu.mjs is copied there; CHROME: the chromium to use);
PNS_TOOL=FILE puts a newer reimu-pns-flash over the image's (development).

The GPIO switch of the PNS flash (GPIO_SPI_CONNECT = O0) and the power
button (GPIO_PWR_BTN = N5) are watched from QMP (the GPIO registers, every
~20 ms); the platform's power good (GPIO_POWER_IN = O2) is driven from QMP.
QEMU can't disconnect the flash with O0, so "connected only while ..." is
checked on the GPIO, and the chip's contents in the drive file.

Tests (all by default; the first three share one BMC, the last two another):
  tool        versions read at boot, check (good, truncated, oversized,
              garbage, other board), read (backup = the chip), write the other
              image and the original back: exactly the image's region changes,
              then nothing to do the second time; update-host-flash
  faults      writes that read back wrong (rewritten), a chip that can't be
              written and I/O errors (failed, reported, backup kept, the flash
              back with the host), kill -9 in the middle (guard), kill -9 of
              everything (power-on switches the flash back), every time
              restored from the backup
  power       refused while the platform is on (O0 never set); power-on
              blocked during a write (server_pwr_on, Redfish), allowed after;
              the platform powering up anyway during a write
  web         the REST endpoints of the web UI's PNS card: upload in chunks
              (sha256), check, write, download a backup, write it back,
              refusal while on; with E2E: the page itself in a browser
  bmcupdate   the web BMC update of the A/B image: Redfish UpdateService with
              reimu-ab.tar -> reimu-abd -> reimu-ab install into slot B, the
              task, garbage refused at once, reboot into slot B, confirmed
"""
import base64, hashlib, json, os, random, re, shutil, socket, ssl, subprocess, sys, threading, time
import urllib.request, urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ARGS = sys.argv[1:]
import test_ab_qemu as T  # noqa: E402  (reads WORKDIR from sys.argv)

W = T.W
ONLY = ARGS[1:]
check = T.check
MiB = 1 << 20
ORIG = os.path.join(W, "pns-orig.bin")
NEW = os.path.join(W, "pns-new.bin")
FAULT = os.path.join(W, "pns-fault.so")
E2E = os.environ.get("E2E", "")
DTB_OFF, BOOT_END = 0x700000, 0x490000


def sha(b):
    return hashlib.sha256(b).hexdigest()


# ------------------------------------------------------------------ QMP
class Qmp:
    def __init__(self, path):
        for i in range(50):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.2)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.lock = threading.Lock()
        self.cmd("qmp_capabilities")

    def cmd(self, c, **a):
        with self.lock:
            self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n")
            self.f.flush()
            while True:
                r = json.loads(self.f.readline())
                if "return" in r or "error" in r:
                    return r

    def hmp(self, line):
        return self.cmd("human-monitor-command", **{"command-line": line}).get("return", "")


class Gpio(threading.Thread):
    """GPIO M/N/O/P data (0x1e780078) and direction (0x1e78007c) every ~20 ms:
    O0 driven high = the flash is the BMC's, N5 driven low = the power button
    pressed"""

    def __init__(self, qmp):
        super().__init__(daemon=True)
        self.q, self.samples, self.stop_ = qmp, [], False

    def read(self):
        m = re.search(r":\s*0x([0-9a-f]+)\s+0x([0-9a-f]+)", self.q.hmp("xp /2wx 0x1e780078"))
        if not m:
            return None
        data, dirn = int(m.group(1), 16), int(m.group(2), 16)
        o0 = bool(dirn & 1 << 16) and bool(data & 1 << 16)
        n5 = bool(dirn & 1 << 13) and not data & 1 << 13
        return time.time(), o0, n5

    def run(self):
        while not self.stop_:
            try:
                s = self.read()
            except (ValueError, OSError):
                return
            if s:
                self.samples.append(s)
            time.sleep(0.02)

    def mark(self):
        return time.time()

    def since(self, t):
        return [s for s in self.samples if s[0] >= t]

    def o0_now(self):
        time.sleep(0.1)
        return self.samples[-1][1]

    def power_in(self, on):
        self.q.cmd("qom-set", path="/machine/soc/gpio", property="gpioO2", value=bool(on))


class PnsBmc(T.Bmc):
    """test_ab_qemu's BMC with the PNS flash on spi1 and a third QMP socket"""

    def __init__(self, name, img, pns):
        super().__init__(name, img)
        self.pns = pns

    def start(self):
        if os.path.exists(self.log):
            os.remove(self.log)
        os.makedirs(T.RQ, exist_ok=True)
        # drives in order: index 0 = FMC CS0 (the BMC flash), 1 = SPI1 CS0
        self.p = subprocess.Popen(
            ["qemu-system-arm", "-M", "ast2500-evb,fmc-model=mx66l51235f,spi-model=n25q512ax3", "-m", "512",
             "-nographic", "-drive", "file=%s,if=mtd,format=raw,index=0" % self.img,
             "-drive", "file=%s,if=mtd,format=raw,index=1" % self.pns,
             "-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:22,hostfwd=tcp:127.0.0.1:%d-:443" % (T.SSH, T.HTTPS),
             "-net", "nic,netdev=n0,macaddr=98:a7:b0:00:00:01",
             "-qmp", "unix:%s/%s.qmp,server,nowait" % (T.RQ, self.name),
             "-qmp", "unix:%s/%s.qmp2,server,nowait" % (T.RQ, self.name),
             "-qmp", "unix:%s/%s.qmp3,server,nowait" % (T.RQ, self.name),
             "-chardev", "socket,id=con,path=%s/%s.con,server=on,wait=off,logfile=%s" % (T.RQ, self.name, self.log),
             "-serial", "chardev:con", "-serial", "null", "-monitor", "none"],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1)
        self.uid = subprocess.Popen([sys.executable, "-c", T.UID_LOOP, "%s/%s.qmp" % (T.RQ, self.name)],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.qmp = Qmp("%s/%s.qmp3" % (T.RQ, self.name))
        self.gpio = Gpio(self.qmp)
        self.gpio.start()
        return self

    def stop(self):
        if self.p:
            self.gpio.stop_ = True
        super().stop()

    def chip(self):
        return open(self.pns, "rb").read()

    def sh(self, cmd, timeout=900):
        r = self.ssh(cmd, timeout)
        return r.returncode, (r.stdout + r.stderr).strip()

    def state(self):
        for i in range(3):
            try:
                return json.loads(self.out("cat /run/reimu-pns/state.json"))
            except (ValueError, subprocess.TimeoutExpired):
                time.sleep(1)
        return {}

    def host_state(self):
        return self.out("busctl get-property xyz.openbmc_project.State.Host /xyz/openbmc_project/state/host0 "
                        "xyz.openbmc_project.State.Host CurrentHostState").strip()

    def power(self, on):
        self.gpio.power_in(on)
        want = "Running" if on else "Off"
        for i in range(30):
            if self.host_state().endswith(want + '"'):
                return True
            time.sleep(1)
        return False


def fresh_pns(name):
    p = os.path.join(W, name + "-pns.bin")
    shutil.copyfile(ORIG, p)
    os.chmod(p, 0o644)
    return p


def put(b, data, dst):
    """bytes -> a file on the BMC"""
    tmp = os.path.join(W, ".put")
    open(tmp, "wb").write(data)
    ok = b.scp(tmp, dst)
    os.remove(tmp)
    return ok


def ensure_fault_so():
    if os.path.exists(FAULT):
        return True
    # the A/B build's cross compiler (meta-mcst/tests/ab64/pns-fault.c)
    c = r'''set -e
C=/work/build-ab/tmp/sysroots-components; B=/tmp/pns-fault-cc; mkdir -p $B/bin
for t in as ld; do ln -sf $C/x86_64/binutils-cross-arm/usr/bin/arm-openbmc-linux-gnueabi/arm-openbmc-linux-gnueabi-$t $B/bin/$t; done
G=$C/arm1176jzs/libgcc/usr/lib/arm-openbmc-linux-gnueabi/10.2.0
cat > $B/pns-fault.c
$C/x86_64/gcc-cross-arm/usr/bin/arm-openbmc-linux-gnueabi/arm-openbmc-linux-gnueabi-gcc -marm -mcpu=arm1176jz-s \
  -mfloat-abi=soft --sysroot=$C/arm1176jzs/glibc -isystem $C/arm1176jzs/linux-libc-headers/usr/include \
  -B$B/bin/ -B$G -L$G -L$C/arm1176jzs/libgcc/lib -shared -fPIC -O2 -o $B/pns-fault.so $B/pns-fault.c -ldl
cat $B/pns-fault.so'''
    try:
        r = subprocess.run(["docker", "exec", "-i", "reimu", "sh", "-c", c],
                           input=open(os.path.join(HERE, "pns-fault.c"), "rb").read(), capture_output=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return False
    if r.returncode or not r.stdout.startswith(b"\x7fELF"):
        print("    can't build pns-fault.so: " + r.stderr.decode(errors="replace")[-300:])
        return False
    open(FAULT, "wb").write(r.stdout)
    return True


# ------------------------------------------------------------------ images
def images():
    orig, new = open(ORIG, "rb").read(), open(NEW, "rb").read()
    rnd = random.Random(7)
    bad = {}
    bad["truncated"] = new[:3 * MiB]
    bad["truncated-dtb"] = new[:DTB_OFF + 0x1000]
    bad["oversized"] = orig + b"\xff" * 4096
    bad["garbage"] = bytes(rnd.getrandbits(8) for _ in range(256 * 1024)) * 28
    bad["zeros"] = b"\0" * (8 * MiB)
    # the same image for another board: device tree and build tag (same lengths)
    ob = bytearray(new)
    ob[DTB_OFF:DTB_OFF + 0x4000] = bytes(ob[DTB_OFF:DTB_OFF + 0x4000]).replace(b"uatx_445", b"uatx_446")
    bad["other-board"] = bytes(ob)
    ot = bytearray(new)
    ot[:BOOT_END] = bytes(ot[:BOOT_END]).replace(b")(uatx_445)", b")(uatx_999)")
    bad["other-tag"] = bytes(ot)
    return orig, new, bad


# ------------------------------------------------------------------ tests
def wait_booted(b):
    check(b.wait_ssh(600), "up")
    # PNS_TOOL: a newer reimu-pns-flash over the image's (development only)
    if os.environ.get("PNS_TOOL"):
        b.scp(os.environ["PNS_TOOL"], "/usr/sbin/reimu-pns-flash")
    # the boot read of the versions ran (or skipped) before allow_host_boot
    for i in range(30):
        if b.ssh("systemctl is-active -q reimu-pns-version allow_host_boot").returncode == 0:
            break
        time.sleep(2)


def t_tool(b):
    orig, new, bad = images()
    rc, o = b.sh("systemctl is-active reimu-pns-version; cat /var/lib/reimu-pns/chip.json")
    chip = json.loads(o.split("\n", 1)[1]) if "\n" in o else {}
    info = chip.get("info", {})
    check(chip.get("source") == "boot" and info.get("boot", "").startswith("release-2.30.13.1-E8C2 ::::::: ( unified-boot/"),
          "versions read at BMC boot: %s" % info.get("boot"))
    check(info.get("dtb", {}).get("version") == "1.0_2025-02-06" and info.get("vbios") == "1.10.01"
          and info.get("dtb", {}).get("compatible") == "mcst,e8c2_1e8sv-uatx_445",
          "device tree 1.0_2025-02-06, VBIOS 1.10.01, mcst,e8c2_1e8sv-uatx_445")
    check(not b.gpio.o0_now(), "the flash is the host's after boot (O0 = 0)")

    for name, data in (("new", new), ("orig", orig)):
        put(b, data, "/tmp/%s.bin" % name)
    rc, o = b.sh("reimu-pns-flash version /tmp/new.bin")
    check(rc == 0 and "boot=testing ::::::: ( unified-boot/main commit f4c822e9, 202601311406 M)(uatx_445)" in o
          and "devtree=1.0_2025-02-06" in o and "board=uatx_445" in o, "version of the colleagues' image")
    for name in ("new", "orig"):
        rc, o = b.sh("reimu-pns-flash check /tmp/%s.bin --json" % name)
        j = json.loads(o.splitlines()[-1])
        check(rc == 0 and j["ok"] and j["image"]["dtb"]["valid"], "check accepts %s (covers %s)" % (name, j.get("covers")))
    for name, data in bad.items():
        put(b, data, "/tmp/bad-%s.bin" % name)
        rc, o = b.sh("reimu-pns-flash check /tmp/bad-%s.bin; rc=$?; [ %s = other-board ] || rm -f /tmp/bad-%s.bin; exit $rc"
                     % (name, name, name))
        check(rc != 0 and "ошибка:" in o, "check rejects %s: %s" % (name, o.splitlines()[0] if o else ""))
    rc, o = b.sh("reimu-pns-flash check /tmp/bad-other-board.bin --any-board")
    check(rc == 0 and "внимание: Образ для другой платы" in o, "--any-board: another board's image is a warning")

    # read: the backup is the chip
    t0 = b.gpio.mark()
    rc, o = b.sh("reimu-pns-flash read /tmp/reimu-pns/backup-test.bin && sha256sum /tmp/reimu-pns/backup-test.bin")
    s = b.gpio.since(t0)
    check(rc == 0 and sha(orig) in o, "read: the backup equals the chip (sha256)")
    b.ssh("rm -f /tmp/reimu-pns/backup-test.bin*")
    check(any(x[1] for x in s) and not b.gpio.o0_now(), "read: the flash was the BMC's only meanwhile (O0 1 -> 0)")
    st = b.state()
    check(st.get("state") == "ok" and st.get("job") == "read", "state: read ok (%s)" % st.get("message"))

    # write the other image: only its region changes
    t0 = b.gpio.mark()
    rc, o = b.sh("reimu-pns-flash write /tmp/new.bin")
    print("    " + o.replace("\n", "\n    "))
    c = b.chip()
    region = (len(new) + 4095) // 4096 * 4096
    check(rc == 0 and c[:len(new)] == new, "write: the chip holds the new image")
    check(c[len(new):] == orig[len(new):], "write: nothing else changed (0x%x-0x4000000 as before)" % len(new))
    check(not b.gpio.o0_now() and any(x[1] for x in b.gpio.since(t0)), "write: O0 back to 0")
    st = b.state()
    check(st.get("state") == "ok" and st.get("backup", "").startswith("/tmp/reimu-pns/backup-"), "state: write ok, backup %s" % st.get("backup"))
    rc, o = b.sh("cat /var/lib/reimu-pns/chip.json")
    check('"source":"write"' in o and "f4c822e9" in o and sha(c) in o, "chip.json: the new version and the chip's sha256")
    rc, o = b.sh("gzip -dc /var/lib/reimu-pns/backup-last.bin.gz | sha256sum")
    check(sha(orig) in o, "the chip before the write kept on the BMC flash (backup-last.bin.gz)")
    # the original back, as an image (not a restore)
    rc, o = b.sh("reimu-pns-flash write /tmp/orig.bin")
    check(rc == 0 and b.chip() == orig, "write the original back: the chip is the dump again")
    rc, o = b.sh("reimu-pns-flash write /tmp/orig.bin")
    check(rc == 0 and "уже содержит" in o and b.state().get("changedBlocks") == 0, "the same image again: nothing written")
    # the stock interface
    rc, o = b.sh("update-host-flash file:///tmp/new.bin")
    check(rc == 0 and b.chip()[:len(new)] == new, "update-host-flash file:///... writes through reimu-pns-flash")
    rc, o = b.sh("update-host-flash file:///tmp/bad-other-board.bin")
    check(rc != 0 and b.chip()[:len(new)] == new, "update-host-flash refuses another board's image")
    rc, o = b.sh("reimu-pns-flash write /tmp/orig.bin")
    check(rc == 0 and b.chip() == orig, "original restored")
    b.ssh("rm -f /tmp/orig.bin /tmp/bad-*.bin")


KILLALL = rb"""for p in /proc/[0-9]*; do
    c=$(tr '\000' ' ' < $p/cmdline 2>/dev/null)
    case "$c" in *bin/reimu-pns-flash\ write*|reimu-flash\ write*) kill -9 ${p#/proc/} ;; esac
done
"""


def restore(b, orig, what):
    rc, o = b.sh("ls -t /tmp/reimu-pns/backup-*.bin.gz | head -1")
    rc, o2 = b.sh("reimu-pns-flash write %s" % o.strip())
    check(rc == 0 and b.chip() == orig, "%s: restored from the backup %s" % (what, o.strip()))
    if rc:
        print("    " + o2.replace("\n", "\n    "))


def t_faults(b):
    orig, new, bad = images()
    if not ensure_fault_so():
        check(False, "pns-fault.so (LD_PRELOAD) for the fault tests")
        return
    b.scp(FAULT, "/tmp/pns-fault.so")
    put(b, new, "/tmp/new.bin")
    pre = "LD_PRELOAD=/tmp/pns-fault.so "

    # writes that come back wrong: found by the read-back, rewritten
    rc, o = b.sh(pre + "PNS_FAULT=flip:3 reimu-pns-flash write /tmp/new.bin")
    check(rc == 0 and "3 blocks differ after writing, writing them again" in o and b.chip()[:len(new)] == new,
          "3 writes lost: detected, rewritten, verified")
    check(not b.gpio.o0_now(), "O0 = 0 after")
    rc, o = b.sh("reimu-pns-flash write /tmp/reimu-pns/$(ls -t /tmp/reimu-pns | grep -m1 'backup-.*bin.gz$')")
    check(rc == 0 and b.chip() == orig, "restored (a backup of this chip: restore)")

    # a chip that can't be written
    rc, o = b.sh(pre + "PNS_FAULT=flipall REIMU_PNS_TRIES=2 reimu-pns-flash write /tmp/new.bin")
    st = b.state()
    check(rc != 0 and st.get("state") == "failed" and "Запись не удалась" in st.get("message", "")
          and "Резервная копия флеша: /tmp/reimu-pns/backup-" in st.get("message", ""),
          "every write lost: failed, reported with the backup (%s)" % st.get("message", "")[:120])
    check(not b.gpio.o0_now(), "O0 = 0 after the failure")
    rc, o = b.sh("gzip -dc %s | sha256sum" % st.get("backup"))
    check(sha(orig) in o, "the backup is the chip before the write")
    restore(b, orig, "after the failed write")

    # I/O errors (every reimu-flash run fails at its first write)
    rc, o = b.sh(pre + "PNS_FAULT=eio:1 reimu-pns-flash write /tmp/new.bin")
    st = b.state()
    check(rc != 0 and st.get("state") == "failed" and "Input/output error" in o, "I/O errors: failed, reported")
    check(not b.gpio.o0_now(), "O0 = 0 after the I/O errors")
    restore(b, orig, "after the I/O errors")

    # killed in the middle of the write: the guard switches the flash back
    b.ssh(pre + "PNS_FAULT=slow:40 setsid nohup reimu-pns-flash write /tmp/new.bin >/tmp/w.log 2>&1 &", timeout=30)
    ok = False
    for i in range(120):
        st = b.state()
        if st.get("phase") == "write" and st.get("state") == "running" and st.get("progress", 0) >= 42:
            ok = True
            break
        time.sleep(1)
    check(ok, "write running (%s %s%%)" % (st.get("phase"), st.get("progress")))
    check(b.gpio.o0_now(), "O0 = 1 while writing")
    b.ssh("kill -9 %d" % st.get("pid", 0))
    t0 = time.time()
    while time.time() - t0 < 15 and b.gpio.o0_now():
        time.sleep(0.5)
    check(not b.gpio.o0_now(), "kill -9: the guard switched the flash back to the host in %.1f s" % (time.time() - t0))
    time.sleep(2)
    st = b.state()
    check(st.get("state") == "interrupted", "state: interrupted (%s)" % st.get("message"))
    rc, o = b.sh("ps | grep -c '[r]eimu-flash write'")
    check(o.strip() == "0", "no reimu-flash left writing")
    check(b.ssh("ls %s" % st.get("backup")).returncode == 0, "backup there: %s" % st.get("backup"))
    rc, o = b.sh("reimu-pns-flash read /tmp/reimu-pns/after-kill.bin >/dev/null; reimu-pns-flash version /tmp/reimu-pns/after-kill.bin; rm -f /tmp/reimu-pns/after-kill.bin*")
    check(rc == 0, "the next job runs (lock free): chip is a mix now")
    restore(b, orig, "after kill -9")

    # everything killed (the guard too): the next power-on switches it back
    b.ssh(pre + "PNS_FAULT=slow:40 setsid nohup reimu-pns-flash write /tmp/new.bin >/tmp/w.log 2>&1 &", timeout=30)
    for i in range(120):
        st = b.state()
        if st.get("phase") == "write" and st.get("progress", 0) >= 42:
            break
        time.sleep(1)
    # (a script: its own command line must not match)
    put(b, KILLALL, "/tmp/killall-pns.sh")
    for i in range(5):
        b.ssh("sh /tmp/killall-pns.sh 2>/dev/null")
        if b.out("ps -o args | grep -c '^[r]eimu-flash write\\|bin/[r]eimu-pns-flash write'").strip() == "0":
            break
        time.sleep(1)
    time.sleep(3)
    check(b.gpio.o0_now(), "everything killed: O0 left at 1")
    t0 = b.gpio.mark()
    rc, o = b.sh("server_pwr_on; echo rc=$?")
    s = b.gpio.since(t0)
    press = [x for x in s if x[2]]
    check(press and not any(x[1] for x in s if x[0] >= press[0][0]),
          "power-on: O0 = 0 before the power button is pressed (%s)" % o.replace("\n", " "))
    restore(b, orig, "after killing everything")
    st = b.state()
    check(st.get("state") == "ok", "the next job ran (the stale one marked interrupted)")


def redfish(method, path, body=None, ctype="application/json", timeout=180):
    ctx = ssl.create_default_context()
    ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE
    data = body if isinstance(body, (bytes, type(None))) else json.dumps(body).encode()
    rq = urllib.request.Request("https://127.0.0.1:%d%s" % (T.HTTPS, path), data=data, method=method,
                                headers={"Authorization": "Basic " + base64.b64encode(b"root:0penBmc").decode(),
                                         "Content-Type": ctype})
    try:
        with urllib.request.urlopen(rq, context=ctx, timeout=timeout) as r:
            t = r.read()
            return r.status, json.loads(t) if t else None
    except urllib.error.HTTPError as e:
        t = e.read()
        try:
            return e.code, json.loads(t)
        except ValueError:
            return e.code, t.decode(errors="replace")


PF = "/xyz/openbmc_project/reimu/pns_flash"


def pf_call(method, *args):
    st, r = redfish("POST", "%s/action/%s" % (PF, method), {"data": list(args)})
    try:
        return json.loads(r["data"])
    except (TypeError, KeyError, ValueError):
        return {"ok": False, "message": "HTTP %s %s" % (st, str(r)[:200])}


def pf_json():
    st, r = redfish("GET", PF)
    return json.loads(r["data"]["Json"]) if st == 200 else {}


def pf_upload(data, chunk=512 * 1024, wrong_sha=False):
    for off in range(0, len(data), chunk):
        part = data[off:off + chunk]
        r = pf_call("Upload", off, "ff:%d" % len(part) if part == b"\xff" * len(part) else base64.b64encode(part).decode())
        if not r.get("ok"):
            return r
    return pf_call("Run", "upload-done", "0" * 64 if wrong_sha else sha(data))


def pf_job():
    """the job state now (to tell the next one from it: BMC and host clocks differ)"""
    s = pf_json().get("state") or {}
    return (s.get("pid"), s.get("started"))


def pf_wait(before, timeout=900):
    end = time.time() + timeout
    while time.time() < end:
        s = pf_json().get("state") or {}
        if (s.get("pid"), s.get("started")) != before and s.get("state") in ("ok", "failed", "interrupted"):
            return s
        time.sleep(2)
    return {}


def t_power(b):
    orig, new, bad = images()
    put(b, new, "/tmp/new.bin")
    # the platform on: refused, the switch never touched
    check(b.power(True), "platform on (POWER_IN = 1): CurrentHostState Running")
    t0 = b.gpio.mark()
    for cmd in ("read", "write /tmp/new.bin", "version --read"):
        rc, o = b.sh("reimu-pns-flash " + cmd)
        check(rc != 0 and "Платформа включена" in o, "refused while on: %s (%s)" % (cmd, o.splitlines()[-1] if o else ""))
    r = pf_call("Run", "read", "")
    check(not r.get("ok") and "Платформа включена" in r.get("message", ""), "web: read refused while on (%s)" % r.get("message"))
    check(not any(x[1] for x in b.gpio.since(t0)) and b.chip() == orig, "O0 never 1, chip unchanged")
    check(b.power(False), "platform off again")

    # a write in progress: nothing powers the platform on
    if ensure_fault_so():
        b.scp(FAULT, "/tmp/pns-fault.so")
        slow = "LD_PRELOAD=/tmp/pns-fault.so PNS_FAULT=slow:30 "
    else:
        slow = ""
    t0 = b.gpio.mark()
    b.ssh(slow + "setsid nohup reimu-pns-flash write /tmp/new.bin >/tmp/w.log 2>&1 &", timeout=30)
    running = False
    for i in range(60):
        if b.ssh("flock -n /run/reimu-pns/lock true").returncode != 0:
            running = True
            break
        time.sleep(1)
    time.sleep(3)
    check(running and b.state().get("state") == "running", "write running (the lock held)")
    rc, o = b.sh("server_pwr_on; echo rc=$?")
    check("rc=53" in o and "can't be powered on" in o, "server_pwr_on refused during the write (%s)" % o.splitlines()[0])
    n0 = b.out("journalctl -u host-poweron --no-pager | grep -c \"can't be powered on\"").strip()
    s, r = redfish("POST", "/redfish/v1/Systems/system/Actions/ComputerSystem.Reset", {"ResetType": "On"})
    time.sleep(4)
    n1 = b.out("journalctl -u host-poweron --no-pager | grep -c \"can't be powered on\"").strip()
    check(n1.isdigit() and n0.isdigit() and int(n1) > int(n0), "Redfish power on (HTTP %s): host-poweron refused by server_ctl" % s)
    rc, o = b.sh("busctl set-property xyz.openbmc_project.State.Host /xyz/openbmc_project/state/host0 "
                 "xyz.openbmc_project.State.Host RequestedHostTransition s xyz.openbmc_project.State.Host.Transition.On")
    time.sleep(3)
    # the platform powers up anyway (its own button): the write goes on
    b.gpio.power_in(True)
    for i in range(600):
        st = b.state()
        if st.get("state") != "running":
            break
        time.sleep(1)
    b.gpio.power_in(False)
    s = b.gpio.since(t0)
    during = [x for x in s if x[0] <= time.time()]
    check(not any(x[2] for x in during), "the power button was never pressed during the write")
    check(st.get("state") == "ok" and "Платформа включилась во время записи" in st.get("warning", ""),
          "the platform came up during the write: write finished, reported (%s)" % st.get("warning"))
    check(b.chip()[:len(new)] == new and not b.gpio.o0_now(), "chip written, O0 = 0")
    b.power(False)
    # (the job's guard keeps the lock up to a second after it ended)
    b.ssh("for i in 1 2 3 4 5; do flock -n /run/reimu-pns/lock true && break; sleep 1; done")
    t0 = b.gpio.mark()
    rc, o = b.sh("server_pwr_on; echo rc=$?")
    s = b.gpio.since(t0)
    check("rc=0" in o and any(x[2] for x in s) and not any(x[1] for x in s),
          "after the write: power-on works, O0 = 0 (%s)" % o.replace("\n", " "))
    rc, o = b.sh("reimu-pns-flash write /tmp/reimu-pns/$(ls -t /tmp/reimu-pns | grep -m1 'backup-.*bin.gz$')")
    check(rc == 0 and b.chip() == orig, "restored")


def t_web(b):
    orig, new, bad = images()
    j = pf_json()
    check(j.get("tool") is True and (j.get("chip") or {}).get("info", {}).get("dtb", {}).get("version") == "1.0_2025-02-06",
          "GET %s: the chip's versions" % PF)
    r = pf_upload(new, wrong_sha=True)
    check(not r.get("ok") and "sha256" in r.get("message", ""), "upload with a wrong sha256: refused (%s)" % r.get("message"))
    r = pf_upload(bad["other-board"])
    check(not r.get("ok") and any("другой платы" in e for e in r.get("check", {}).get("errors", [])),
          "another board's image: the check fails (%s)" % (r.get("check", {}).get("errors")))
    t0 = pf_job()
    r = pf_call("Run", "write", "upload")
    s = pf_wait(t0, 300)
    check(s.get("state") == "failed" and b.chip() == orig, "its write fails in the tool too, chip unchanged")
    r = pf_upload(new)
    c = r.get("check", {})
    check(r.get("ok") and c.get("image", {}).get("board") == "uatx_445" and c.get("covers") == [0, len(new)],
          "upload in chunks + sha256: checked ok, covers 0x0-0x%x" % len(new))
    j = pf_json()
    check((j.get("upload") or {}).get("size") == len(new), "the upload is listed")
    t0 = pf_job()
    r = pf_call("Run", "write", "upload")
    check(r.get("ok"), "write started (%s)" % r.get("message"))
    r2 = pf_call("Run", "read", "")
    check(not r2.get("ok"), "a second job meanwhile refused (%s)" % r2.get("message"))
    s = pf_wait(t0)
    check(s.get("state") == "ok" and b.chip()[:len(new)] == new and b.chip()[len(new):] == orig[len(new):],
          "written and verified: %s" % s.get("message", "")[:100])
    j = pf_json()
    bk = j.get("backups") or []
    check(bk and bk[0].get("sha256") == sha(orig), "the backup before the write is listed with its sha256")
    # download it like the page: chunks, the erased ones without data
    name, size = bk[0]["name"], None
    check(name.endswith(".bin.gz") and bk[0]["size"] < 4 << 20, "kept gzipped in RAM: %s, %d KiB" % (name, bk[0]["size"] >> 10))
    out = bytearray()
    n_ff = 0
    off = 0
    while size is None or off < size:
        r = pf_call("Download", name, off, 1 << 20)
        size = r.get("size", 0)
        off += r.get("length", 0) or (1 << 20)
        if r.get("ff"):
            n_ff += 1
            out += b"\xff" * r["length"]
        else:
            out += base64.b64decode(r["data"])
    check(bytes(out) == orig, "download of the backup = the original dump (%d of %d MiB sent as 0xff)" % (n_ff, size >> 20))
    check(b.out("ls /tmp/reimu-pns/.download.bin 2>/dev/null").strip() == "", "the unpacked copy dropped after the last chunk")
    rc, o = b.sh("df -k /tmp | tail -1; ls -la /tmp/reimu-pns")
    print("    " + o.replace("\n", "\n    "))
    r = pf_call("Download", "../../etc/shadow", 0, 100)
    check(not r.get("ok"), "download of another file refused")
    t0 = pf_job()
    r = pf_call("Run", "write", name)
    s = pf_wait(t0)
    check(s.get("state") == "ok" and b.chip() == orig, "write the backup back from the web: the original again")
    # "last": the chip before the last write (that one wrote the original
    # over the new image), kept gzipped on the BMC flash
    t0 = pf_job()
    r = pf_call("Run", "write", "last")
    s = pf_wait(t0)
    check(s.get("state") == "ok" and b.chip() == new + orig[len(new):],
          "write \"last\" (the gzip copy on the BMC flash): the chip before the last write (%s; %s)"
          % (r.get("message"), s.get("message", "")[:120]))
    t0 = pf_job()
    r = pf_call("Run", "write", (pf_json().get("backups") or [{}])[0].get("name", "?"))
    s = pf_wait(t0)
    check(s.get("state") == "ok" and b.chip() == orig, "and the newest backup back: the original")
    check(b.power(True), "platform on")
    # (the upload is gone: written; a backup instead)
    r = pf_call("Run", "write", (pf_json().get("backups") or [{}])[0].get("name", "?"))
    check(not r.get("ok") and "Платформа включена" in r.get("message", ""), "web: write refused while on (%s)" % r.get("message"))
    check(not (pf_json().get("upload")), "the written upload was removed from RAM")
    if E2E:
        e2e(b, new)
    check(b.power(False), "platform off")


def e2e(b, new):
    spec = os.path.join(HERE, "..", "e2e", "pns.qemu.mjs")
    if not os.path.exists(spec):
        spec = os.path.join(HERE, "pns.qemu.mjs")
    d = os.path.join(E2E, ".pns-e2e.mjs")
    shutil.copyfile(spec, d)
    if not os.path.exists(os.path.join(E2E, "common.mjs")):
        shutil.copyfile(os.path.join(os.path.dirname(spec), "common.mjs"), os.path.join(E2E, "common.mjs"))
    put_local = os.path.join(W, "pns-new-e2e.bin")
    shutil.copyfile(NEW, put_local)
    env = dict(os.environ, BASE="https://127.0.0.1:%d" % T.HTTPS, PNS_IMAGE=put_local, OUT=os.path.join(W, "shots"))
    # the page while the platform is on, then off: the write
    r = subprocess.run(["node", d, "on"], env=env, capture_output=True, text=True, timeout=600)
    print("    " + (r.stdout + r.stderr).strip().replace("\n", "\n    "))
    check(r.returncode == 0, "browser: the PNS card while the platform is on")
    b.power(False)
    r = subprocess.run(["node", d, "write"], env=env, capture_output=True, text=True, timeout=1500)
    print("    " + (r.stdout + r.stderr).strip().replace("\n", "\n    "))
    check(r.returncode == 0 and b.chip()[:len(new)] == new, "browser: upload, check, write of the new image")
    r = subprocess.run(["node", d, "restore"], env=env, capture_output=True, text=True, timeout=1500)
    print("    " + (r.stdout + r.stderr).strip().replace("\n", "\n    "))
    check(r.returncode == 0 and b.chip() == open(ORIG, "rb").read(), "browser: the backup written back")


def t_bmcupdate(b):
    tar = os.path.join(W, "reimu-ab.tar")
    st, r = redfish("GET", "/redfish/v1/Managers/bmc")
    ver = b.out(". /etc/os-release; echo $VERSION_ID").strip()
    check(st == 200 and r.get("FirmwareVersion") == ver, "Managers/bmc FirmwareVersion from reimu-abd: %s" % r.get("FirmwareVersion"))
    st, r = redfish("GET", "/redfish/v1/UpdateService/FirmwareInventory")
    check(st == 200 and r.get("Members@odata.count", 0) >= 1, "FirmwareInventory: %s" % r.get("Members"))
    # garbage: refused at once
    t0 = time.time()
    st, r = redfish("POST", "/redfish/v1/UpdateService", os.urandom(300000), "application/octet-stream")
    check(400 <= st < 500 and time.time() - t0 < 30, "a non-update upload: HTTP %d in %.0f s (%s)" % (st, time.time() - t0, str(r)[:160]))
    j = json.loads(redfish("GET", "/xyz/openbmc_project/reimu/ab")[1]["data"]["Json"])
    check(j.get("upload", {}).get("ok") is False and "отклонён" in j["upload"].get("message", ""),
          "the reason for the web UI: %s" % j.get("upload", {}).get("message"))
    check(b.out("ls /tmp/images | wc -l").strip() == "0", "nothing left in /tmp/images")
    # the update
    b.ssh("flash_eraseall /dev/mtd/rofs-b >/dev/null 2>&1; true", timeout=300)
    t0 = time.time()
    st, r = redfish("POST", "/redfish/v1/UpdateService", open(tar, "rb").read(), "application/octet-stream", timeout=600)
    check(st in (200, 202) and isinstance(r, dict) and "TaskState" in json.dumps(r), "upload of reimu-ab.tar: HTTP %d, a task" % st)
    task = r.get("@odata.id") if isinstance(r, dict) else None
    end = time.time() + 1200
    j = {}
    while time.time() < end:
        j = json.loads(redfish("GET", "/xyz/openbmc_project/reimu/ab")[1]["data"]["Json"])
        if (j.get("job") or {}).get("state") in ("ok", "failed"):
            break
        time.sleep(5)
    job = j.get("job") or {}
    check(job.get("state") == "ok" and job.get("progress") == 100, "reimu-ab install through reimu-abd: %s" % job.get("message"))
    if task:
        st, t = redfish("GET", task)
        check(st == 200 and t.get("TaskState") == "Completed", "Redfish task %s: %s" % (task, t.get("TaskState")))
    e = T.benv(b, "reimu_slot", "reimu_pending", "reimu_tries")
    check(e == {"reimu_slot": "b", "reimu_pending": "1", "reimu_tries": "0"}, "slot B next, pending (%s)" % e)
    check(b.ssh("reimu-ab verify b").returncode == 0, "slot B intact")
    check(b.out("ls /tmp/images | wc -l").strip() == "0", "the image removed from RAM")
    # the reboot (the web UI's button: Manager.Reset)
    st, r = redfish("POST", "/redfish/v1/Managers/bmc/Actions/Manager.Reset", {"ResetType": "GracefulRestart"})
    check(st in (200, 204), "Manager.Reset: %d" % st)
    check(b.wait_console(r"reimu\.slot=b", 400, len(b.console()) - 10) is not None, "U-Boot boots slot B")
    check(b.wait_ssh(600), "up again")
    check(T.ab_state(b) == "kernel=b rofs=b state=ok", "running slot B (%s)" % T.ab_state(b))
    ok = False
    for i in range(80):
        e = T.benv(b, "reimu_pending", "reimu_slot")
        if e.get("reimu_pending") == "0":
            ok = True
            break
        time.sleep(5)
    check(ok and e.get("reimu_slot") == "b", "update confirmed (%s)" % e)


GROUPS = [("pnsflash", [("tool", t_tool), ("faults", t_faults), ("power", t_power)]),
          ("pnsweb", [("web", t_web), ("bmcupdate", t_bmcupdate)])]

if __name__ == "__main__":
    for f in (ORIG, NEW):
        if not os.path.exists(f):
            print("missing %s" % f)
            sys.exit(2)
    for gname, tests in GROUPS:
        tests = [(n, f) for n, f in tests if not ONLY or n in ONLY]
        if not tests:
            continue
        b = PnsBmc(gname, T.fresh(gname), fresh_pns(gname)).start()
        try:
            print("== boot (%s)" % gname, flush=True)
            wait_booted(b)
            for name, fn in tests:
                print("== " + name, flush=True)
                t = time.time()
                try:
                    fn(b)
                except Exception as e:
                    import traceback
                    traceback.print_exc()
                    check(False, "%s: %r" % (name, e))
                print("   (%d s)" % (time.time() - t), flush=True)
        finally:
            b.stop()
    failed = T.failed
    print("\n%d failed" % len(failed) + "".join("\n  " + f for f in failed))
    sys.exit(1 if failed else 0)
