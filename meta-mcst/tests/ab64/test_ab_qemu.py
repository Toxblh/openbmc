#!/usr/bin/env python3
"""A/B firmware of REIMU-5564 in QEMU (ast2500-evb, 64 MiB SPI NOR), with the
U-Boot dumped from the real BMC, as on the board.

    test_ab_qemu.py WORKDIR [TEST...]

WORKDIR holds ab.mtd (the build's .ab.mtd with the real U-Boot spliced in,
see fetch.sh), reimu-ab.tar (the update), kernel-other.fit (optional, for
solcut: the kernel-a partition of another build) and, for the stock tests, the dump of
the real BMC flash (STOCK=dir with u-boot.bin u-boot-env.bin kernel.bin
rofs.bin rwfs.bin SHA256SUMS; default ~/git/new-reimu/backup) and
stock-kexec/ (zImage, initrd, fdt of the stock kernel, for the way back by
kexec; without it a reboot). Every test
starts from a fresh copy of ab.mtd. Tests (all by default):

  boot        slot A comes up: 6.18, rofs checked and in RAM, nothing of the
              flash mounted but the rwfs, layout, boot confirmed
  memory      RAM: total (DTS memory node), available after boot and after
              500 web requests (leaks), no OOM; RSS of the big daemons
  regress     things that broke before: GPIO names -> numbers (head -c1
              without coreutils, base 512 on 6.18), UTF-8 locale for tmux,
              reimu-kvm waiting for the TLS certificate, failed units
  update      install the update into slot B (every write read back), boot
              it, confirmed when healthy
  badkernel   kernel-b damaged after "install": U-Boot falls back to slot A
              at once, which then becomes the default again
  badrofs     rofs-a damaged: the initramfs runs rofs-b, slot B next
  hang        the boot hangs (no init): WDT1 resets, after 3 boots U-Boot
              switches to the other slot
  fallback    rofs-a damaged (kernel=a rofs=b state=fallback): "install b"
              refused (rofs-b is the only intact rofs); the web update
              (Redfish -> reimu-abd -> "reimu-ab install", no slot) writes slot
              A, slot B byte for byte as it was
  fallcut     the same, the power cut (QEMU killed) while "reimu-ab install"
              writes rofs-a: the environment pointed at slot B during the
              write, slot B boots, untouched; then "install b" refused (the
              only intact slot), "install both" writes the damaged A first
  solcut      kernel-a intact but without digests (rofs-a's trailer gone),
              kernel-b damaged: "install" (no slot) refused, "install both"
              writes kernel-b, rofs-a before rofs-b; the flash before and
              halfway through each write booted: always an A/B slot
  flashfail   reimu-flash fails (REIMU_FLASH): "kernel write failed", no rofs
              write, the environment as it was; the same for the rofs
  restorecut  "reimu-ab ram" with an install pending: the environment stays
              as it was (reimu-ab boot leaves it alone); restore-stock stopped
              (as if the power went) before each write and halfway through the
              rofs over slot B: the flash at each point checked (slot B
              untouched until the window), each state booted: what comes up
  restorecut-a  slot B damaged: restore-stock writes slot B's region first,
              slot A boots with the stock firmware half written
  stock       "reimu-ab ram", restore-stock from the dump, the stock
              firmware boots; then the first install from the stock firmware
              (kexec into the new image from RAM, write both slots, the A/B
              environment, the settings), the new firmware boots
"""
import base64, hashlib, json, os, re, shutil, socket, ssl, subprocess, sys, tempfile, threading, time
import urllib.error, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
# reimu-ab-from-stock, reimu-ab-back-to-stock: meta-mcst/tools/ab64 (or next to this file)
TOOLS = next((d for d in (os.path.join(HERE, "..", "..", "tools", "ab64"), HERE)
              if os.path.exists(os.path.join(d, "reimu-ab-from-stock"))), HERE)
W = os.path.abspath(sys.argv[1])
ONLY = sys.argv[2:]
STOCK = os.environ.get("STOCK", os.path.expanduser("~/git/new-reimu/backup"))
RQ = os.environ.get("RQ", "/tmp/rq")
# AB_SSH, AB_HTTPS: other ports for a second run at the same time (with its own RQ)
SSH, HTTPS = int(os.environ.get("AB_SSH", 2422)), int(os.environ.get("AB_HTTPS", 2443))
# AB_IPMI=PORT: also forward UDP 127.0.0.1:PORT to the BMC's IPMI (623);
# AB_NETDEV_EXTRA: appended to the user netdev (",restrict=on" keeps the guest
# from reaching the LAN, but slirp then drops UDP replies: no IPMI over LAN).
# Both unset by default: nothing changes.
IPMI = int(os.environ.get("AB_IPMI", 0))
NETDEV_EXTRA = os.environ.get("AB_NETDEV_EXTRA", "")
failed = []


def check(cond, what):
    print(("  PASS " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failed.append(what)
    return cond


# ------------------------------------------------------------------ flash image
LAYOUT = {"u-boot": (0x0, 0xe0000), "u-boot-env": (0xe0000, 0x20000),
          "kernel-a": (0x100000, 0x600000), "rofs-a": (0x700000, 0x1500000),
          "kernel-b": (0x1c00000, 0x600000), "rofs-b": (0x2200000, 0x1500000),
          "rwfs": (0x3700000, 0x900000)}
sys.path.insert(0, HERE)
import ubenv  # noqa: E402


def env_of(img):
    return ubenv.read(bytearray(open(img, "rb").read()))


def set_env(img, **kv):
    b = bytearray(open(img, "rb").read())
    e = ubenv.read(b)
    for k, v in kv.items():
        if v is None:
            e.pop(k, None)
        else:
            e[k] = v
    ubenv.write(b, e)
    open(img, "r+b").write(b)


def region(img, off, n):
    with open(img, "rb") as f:
        f.seek(off)
        return hashlib.sha256(f.read(n)).hexdigest()


def part_sha(img, part):
    return region(img, *LAYOUT[part])


def damage(img, part, at=0x40000, n=4096):
    o = LAYOUT[part][0] + at
    with open(img, "r+b") as f:
        f.seek(o)
        d = bytearray(f.read(n))
        f.seek(o)
        f.write(bytes(x ^ 0x5a for x in d))


# ------------------------------------------------------------------ QEMU
class Bmc:
    def __init__(self, name, img):
        self.name, self.img = name, img
        self.log = os.path.join(W, name + ".log")
        self.p = None

    def start(self):
        for f in (self.log,):
            if os.path.exists(f):
                os.remove(f)
        os.makedirs(RQ, exist_ok=True)
        self.p = subprocess.Popen(
            ["qemu-system-arm", "-M", "ast2500-evb,fmc-model=mx66l51235f", "-m", "512",
             "-nographic", "-drive", "file=%s,if=mtd,format=raw" % self.img,
             "-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:22,hostfwd=tcp:127.0.0.1:%d-:443" % (SSH, HTTPS)
             + (",hostfwd=udp:127.0.0.1:%d-:623" % IPMI if IPMI else "") + NETDEV_EXTRA,
             "-net", "nic,netdev=n0,macaddr=98:a7:b0:00:00:01",
             "-qmp", "unix:%s/%s.qmp,server,nowait" % (RQ, self.name),
             "-qmp", "unix:%s/%s.qmp2,server,nowait" % (RQ, self.name),
             "-chardev", "socket,id=con,path=%s/%s.con,server=on,wait=off,logfile=%s" % (RQ, self.name, self.log),
             "-serial", "chardev:con", "-serial", "null", "-monitor", "none"],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1)
        # UID button (GPIO N3, active low) held released, or revert-to-mfg wipes rwfs
        self.uid = subprocess.Popen([sys.executable, "-c", UID_LOOP, "%s/%s.qmp" % (RQ, self.name)],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return self

    def stop(self):
        if self.p:
            self.p.kill()
            self.p.wait()
            self.uid.kill()
            self.p = None

    def console(self):
        try:
            return open(self.log, "rb").read().decode(errors="replace")
        except FileNotFoundError:
            return ""

    def wait_console(self, rx, timeout, start=0):
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(rx, self.console()[start:])
            if m:
                return m
            time.sleep(2)
        return None

    def boots(self):
        return self.console().count("Starting kernel")

    def ssh(self, cmd, timeout=120, check_rc=False):
        c, env = ssh_cmd(cmd)
        r = subprocess.run(c, env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=timeout)
        if check_rc and r.returncode:
            print(r.stdout, r.stderr)
        return r

    def scp(self, src, dst):
        # no sftp-server on the BMC (what scp uses now): cat over ssh
        c, env = ssh_cmd("cat > '%s'" % dst)
        with open(src, "rb") as f:
            return subprocess.run(c, env=env, stdin=f, capture_output=True, timeout=600).returncode == 0

    def fwd(self, ip):
        """point the ssh/https forwarding at another guest address (a new MAC
        gets another DHCP lease: 10.0.2.16)"""
        s = socket.socket(socket.AF_UNIX)
        s.connect("%s/%s.qmp2" % (RQ, self.name))
        f = s.makefile("rw")
        f.readline()
        def hmp(line):
            f.write('{"execute": "human-monitor-command", "arguments": {"command-line": "%s"}}\n' % line)
            f.flush()
            return f.readline()
        f.write('{"execute": "qmp_capabilities"}\n'); f.flush(); f.readline()
        for host, guest in ((SSH, 22), (HTTPS, 443)):
            hmp("hostfwd_remove n0 tcp:127.0.0.1:%d" % host)
            hmp("hostfwd_add n0 tcp:127.0.0.1:%d-%s:%d" % (host, ip, guest))
        if IPMI:
            hmp("hostfwd_remove n0 udp:127.0.0.1:%d" % IPMI)
            hmp("hostfwd_add n0 udp:127.0.0.1:%d-%s:623" % (IPMI, ip))
        s.close()

    def wait_ssh(self, timeout=400):
        """until ssh works and systemd finished starting"""
        end = time.time() + timeout
        tries = 0
        while time.time() < end:
            tries += 1
            if tries % 8 == 0:  # no answer for a while: the other lease
                self.fwd("10.0.2.16" if tries % 16 == 0 else "10.0.2.15")
            try:
                r = self.ssh("systemctl is-system-running --wait 2>/dev/null; true", timeout=200)
                if r.returncode == 0 and r.stdout.strip() in ("running", "degraded"):
                    return True
            except subprocess.TimeoutExpired:
                pass
            time.sleep(5)
        return False

    def out(self, cmd, timeout=120):
        return self.ssh(cmd, timeout).stdout


def ssh_cmd(*args):
    """ssh to the QEMU BMC as root/0penBmc: the password through SSH_ASKPASS
    (sshpass gets confused when stdin is a file)"""
    a = os.path.join(W, ".askpass")
    if not os.path.exists(a):
        open(a, "w").write("#!/bin/sh\necho 0penBmc\n")
        os.chmod(a, 0o755)
    env = dict(os.environ, SSH_ASKPASS=a, SSH_ASKPASS_REQUIRE="force", DISPLAY="x")
    return ["ssh", "-p", str(SSH), "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
            "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=20", "root@127.0.0.1"] + list(args), env


UID_LOOP = r'''
import json, socket, sys, time
for i in range(50):
    try:
        s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); break
    except OSError: time.sleep(0.2)
f = s.makefile("rw"); f.readline()
def cmd(c, **a):
    f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r: return r
cmd("qmp_capabilities")
while True:
    cmd("qom-set", path="/machine/soc/gpio", property="gpioN3", value=True); time.sleep(0.5)
'''


def fresh(name, src="ab.mtd"):
    img = os.path.join(W, name + ".mtd")
    shutil.copyfile(os.path.join(W, src), img)
    return img


def ssh_bg(cmd):
    """a command over ssh in the background: (Popen, its output lines so far)"""
    c, env = ssh_cmd(cmd)
    p = subprocess.Popen(c, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True)
    lines = []
    threading.Thread(target=lambda: [lines.append(l.rstrip("\n")) for l in p.stdout], daemon=True).start()
    return p, lines


def redfish(method, path, body=None, ctype="application/json", timeout=180):
    ctx = ssl.create_default_context()
    ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE
    data = body if isinstance(body, (bytes, type(None))) else json.dumps(body).encode()
    rq = urllib.request.Request("https://127.0.0.1:%d%s" % (HTTPS, path), data=data, method=method,
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


def ab_state(b):
    return b.out("cat /run/initramfs/reimu-ab").strip()


def benv(b, *names):
    o = b.out("fw_printenv " + " ".join(names))
    return dict(l.split("=", 1) for l in o.splitlines() if "=" in l)


# ------------------------------------------------------------------ tests
def t_boot():
    b = Bmc("boot", fresh("boot")).start()
    try:
        check(b.wait_console(r"login:", 400) is not None, "boots to a login prompt")
        check(b.wait_ssh(), "ssh up, systemd started")
        check(ab_state(b) == "kernel=a rofs=a state=ok", "slot A, rofs checked (%s)" % ab_state(b))
        check(b.out("uname -r").startswith("6.18."), "kernel 6.18")
        check("reimu.slot=a" in b.out("cat /proc/cmdline"), "U-Boot ran the A/B bootcmd")
        m = b.out("cat /proc/mounts")
        check("/dev/loop0 /run/initramfs/ro squashfs" in m, "rofs mounted from RAM (loop)")
        check(not re.search(r"^/dev/mtdblock(?!7 )", m, re.M), "no flash partition mounted but rwfs")
        check("/dev/mtdblock7 /run/initramfs/rw jffs2" in m, "rwfs (mtd7) is jffs2")
        names = re.findall(r'"([^"]+)"', b.out("cat /proc/mtd"))
        check(names[:8] == ["bmc", "u-boot", "u-boot-env", "kernel-a", "rofs-a", "kernel-b", "rofs-b", "rwfs"],
              "flash layout %s" % names)
        check(names[8:] == ["sys"], "the PNS flash (spi1, QEMU has a chip there) is \"sys\"")
        check(b.out("cat /sys/class/mtd/mtd1/flags").strip() == "0x800", "u-boot partition read-only")
        time.sleep(5)
        e = benv(b, "reimu_slot", "reimu_tries", "ethaddr")
        check(e.get("reimu_slot") == "a" and e.get("reimu_tries") == "0", "boot confirmed (%s)" % e)
        check(b.ssh("reimu-ab verify").returncode == 0, "reimu-ab verify: both slots intact")
        check(b.out("systemctl is-active bmcweb.socket reimu-serial reimu-ab").split() == ["active"] * 3,
              "bmcweb.socket, reimu-serial, reimu-ab active")
        r = subprocess.run(["curl", "-ks", "-o", "/dev/null", "-w", "%{http_code}",
                            "https://127.0.0.1:%d/redfish/v1" % HTTPS], capture_output=True, text=True)
        check(r.stdout == "200", "web (Redfish) answers: %s" % r.stdout)
    finally:
        b.stop()


def t_memory():
    b = Bmc("memory", fresh("memory")).start()
    try:
        check(b.wait_ssh(), "up")
        mi = lambda: dict((l.split(":")[0], int(l.split()[1])) for l in b.out("cat /proc/meminfo").splitlines())
        m = mi()
        print("    MemTotal %d MiB, MemAvailable %d MiB" % (m["MemTotal"] // 1024, m["MemAvailable"] // 1024))
        check(m["MemTotal"] > 470 * 1024, "MemTotal %d MiB > 470 (memory node 0x1f000000)" % (m["MemTotal"] // 1024))
        check(m["MemAvailable"] > 330 * 1024, "MemAvailable %d MiB > 330 after boot" % (m["MemAvailable"] // 1024))
        print("    RSS KiB: " + " ".join(b.out("for s in /proc/[0-9]*/status; do awk '/^Name/{n=$2} /^VmRSS/{print $2, n}' $s; done 2>/dev/null | sort -rn | head -8").split("\n")))
        for i in range(500):
            subprocess.run(["curl", "-ks", "-o", "/dev/null", "https://127.0.0.1:%d/redfish/v1" % HTTPS])
        time.sleep(5)
        m2 = mi()
        d = (m["MemAvailable"] - m2["MemAvailable"]) // 1024
        check(d < 8, "500 web requests: MemAvailable down %d MiB (< 8)" % d)
        k = b.out("dmesg | grep -iE 'out of memory|oom-kill|page allocation failure' | head -3")
        check(not k.strip(), "no OOM in dmesg %s" % k.strip())
    finally:
        b.stop()


def t_regress():
    b = Bmc("regress", fresh("regress")).start()
    try:
        check(b.wait_ssh(), "up")
        n = b.out(". /libexec/gpio-funcs; gpio2num N3").strip()
        base = b.out("cat /sys/class/gpio/gpiochip*/base").strip()
        check(n == str(int(base) + 107), "gpio2num N3 = %s (base %s)" % (n, base))
        check(b.out("echo abc | head -c1; echo; printf 'a\\nb\\n' | head -1").split() == ["a", "a"],
              "head -c1, head -1 (busybox without coreutils)")
        check(b.out("systemctl is-active tmux-logger").strip() == "active", "tmux-logger (needs a UTF-8 locale)")
        check("ru_RU.utf8" in b.out("locale -a 2>/dev/null; ls /usr/lib/locale 2>/dev/null").replace("UTF-8", "utf8")
              or b.ssh("LANG=ru_RU.UTF-8 tmux -V").returncode == 0, "ru_RU.UTF-8 available")
        # reimu-kvm needs the certificate bmcweb makes on its first start
        subprocess.run(["curl", "-ks", "-o", "/dev/null", "https://127.0.0.1:%d/" % HTTPS])
        ok = False
        for i in range(30):
            if b.out("systemctl is-active reimu-kvm").strip() == "active":
                ok = True
                break
            time.sleep(2)
        check(ok, "reimu-kvm starts once bmcweb made its certificate")
        f = b.out("systemctl --failed --no-legend --plain | cut -d' ' -f1").split()
        expected = {"mcst-fruid.service"}  # QEMU has no FRU EEPROM
        check(set(f) <= expected, "failed units %s (QEMU: only %s)" % (f, sorted(expected)))
        check(b.ssh("modprobe l-pcs-i2c && modprobe tinyspi 2>/dev/null; lsmod | grep -q l_pcs").returncode == 0,
              "MCST modules load on 6.18 (l-pcs-i2c)")
        # kernel 6.x has no /sys/class/i2c-adapter (FRU EEPROM, sensors, overheatd broke)
        u = b.out("grep -l i2c-adapter /usr/sbin/* /usr/bin/* /libexec/* /usr/libexec/* 2>/dev/null").split()
        check(not u, "nothing uses /sys/class/i2c-adapter %s" % u)
        check(b.ssh("ls -d /sys/bus/i2c/devices/i2c-3/new_device").returncode == 0, "/sys/bus/i2c/devices/i2c-N/new_device")
    finally:
        b.stop()


def install_b(b):
    check(b.scp(os.path.join(W, "reimu-ab.tar"), "/tmp/reimu-ab.tar"), "update copied")
    r = b.ssh("reimu-ab install /tmp/reimu-ab.tar", timeout=900)
    print("    " + (r.stdout + r.stderr).strip().replace("\n", "\n    "))
    check(r.returncode == 0, "reimu-ab install into slot B")
    e = benv(b, "reimu_slot", "reimu_pending", "reimu_tries")
    check(e == {"reimu_slot": "b", "reimu_pending": "1", "reimu_tries": "0"}, "slot B next, pending (%s)" % e)


def t_update():
    b = Bmc("update", fresh("update")).start()
    try:
        check(b.wait_ssh(), "up")
        # an older slot B: wipe it, so the install has to write all of it
        b.ssh("flash_eraseall /dev/mtd6")
        install_b(b)
        n = b.boots()
        b.ssh("reboot")
        check(b.wait_console(r"reimu\.slot=b", 300, len(b.console()) - 10) is not None, "U-Boot boots slot B")
        check(b.wait_ssh(), "up again")
        check(ab_state(b) == "kernel=b rofs=b state=ok", "running slot B (%s)" % ab_state(b))
        ok = False
        for i in range(60):
            e = benv(b, "reimu_pending", "reimu_tries", "reimu_slot")
            if e.get("reimu_pending") == "0":
                ok = True
                break
            time.sleep(5)
        check(ok and e == {"reimu_pending": "0", "reimu_tries": "0", "reimu_slot": "b"}, "update confirmed (%s)" % e)
    finally:
        b.stop()


def t_badkernel():
    img = fresh("badkernel")
    set_env(img, reimu_slot="b", reimu_pending="1", reimu_tries="0")
    damage(img, "kernel-b", at=0x100000)
    b = Bmc("badkernel", img).start()
    try:
        check(b.wait_ssh(), "up")
        c = b.console()
        i = c.find("reimu.slot=b")
        check(i < 0 and "reimu.slot=a" in c, "U-Boot rejected kernel-b before starting it")
        print("    " + "\n    ".join(l for l in c.splitlines() if re.search(r"hash|Bad|ERROR|slot", l, re.I))[:800])
        check(ab_state(b).startswith("kernel=a"), "booted slot A (%s)" % ab_state(b))
        time.sleep(10)
        e = benv(b, "reimu_slot", "reimu_tries")
        check(e.get("reimu_slot") == "a", "slot A is the default again (%s)" % e)
        check(b.ssh("reimu-ab verify b").returncode != 0, "reimu-ab verify b: damaged")
    finally:
        b.stop()


def t_badrofs():
    img = fresh("badrofs")
    damage(img, "rofs-a", at=0x200000)
    b = Bmc("badrofs", img).start()
    try:
        check(b.wait_ssh(), "up")
        check("rofs digest mismatch" in b.console(), "initramfs found rofs-a damaged")
        check(ab_state(b) == "kernel=a rofs=b state=fallback", "runs rofs-b (%s)" % ab_state(b))
        time.sleep(10)
        check(benv(b, "reimu_slot").get("reimu_slot") == "b", "slot B next")
    finally:
        b.stop()


def t_hang():
    img = fresh("hang")
    e = env_of(img)
    # no init (an initramfs shell waits on the console): the kernel feeds WDT1
    # until watchdog.open_timeout, nobody opens it, the BMC resets
    set_env(img, reimu_args=e["reimu_args"].replace("watchdog.open_timeout=300", "watchdog.open_timeout=20")
            + " rdinit=/bin/sh")
    b = Bmc("hang", img).start()
    try:
        m = None
        end = time.time() + 900
        while time.time() < end:
            slots = re.findall(r"reimu\.slot=([ab])", b.console())
            if len(slots) >= 5:
                break
            time.sleep(5)
        print("    boots: " + " ".join(slots))
        check(len(slots) >= 5, "the watchdog reset the hung boots (%d boots)" % len(slots))
        check(slots[:5] == ["a", "a", "a", "b", "b"], "after 3 failed boots U-Boot switched to slot B")
    finally:
        b.stop()


def t_stock():
    need = ["u-boot-env.bin", "kernel.bin", "rofs.bin", "rwfs.bin", "SHA256SUMS"]
    if not all(os.path.exists(os.path.join(STOCK, f)) for f in need):
        check(False, "stock dump in %s" % STOCK)
        return
    b = Bmc("stock", fresh("stock")).start()
    try:
        check(b.wait_ssh(), "up")
        b.ssh("mkdir -p /tmp/stock")
        for f in need:
            check(b.scp(os.path.join(STOCK, f), "/tmp/stock/" + f), "copied " + f)
        # restart with nothing of the flash mounted (kexec, the platform keeps running)
        b.ssh("nohup reimu-ab ram >/dev/null 2>&1 &", timeout=30)
        time.sleep(20)
        check(b.wait_ssh(), "up again after reimu-ab ram (kexec)")
        check("reimu.ram" in b.out("cat /proc/cmdline"), "RAM mode")
        check(not re.search(r"^/dev/mtdblock", b.out("cat /proc/mounts"), re.M), "nothing of the flash mounted")
        r = b.ssh("ls /tmp/stock >/dev/null 2>&1 || echo gone")
        # /tmp didn't survive the kexec: copy again
        b.ssh("mkdir -p /tmp/stock")
        for f in need:
            b.scp(os.path.join(STOCK, f), "/tmp/stock/" + f)
        r = b.ssh("reimu-ab restore-stock /tmp/stock", timeout=1800)
        print("    " + (r.stdout + r.stderr).strip()[-600:].replace("\n", "\n    "))
        check(r.returncode == 0, "restore-stock: written and read back")
        b.ssh("reboot")
        check(b.wait_console(r"5\.10\.20", 300, len(b.console()) - 10) is not None, "stock kernel 5.10 boots")
        check(b.wait_ssh(500), "stock firmware up")
        check("reimu-1.0.2" in b.out("cat /etc/os-release") and b.out("uname -r").startswith("5.10"),
              "stock REIMU 1.0.2 running")
        stock_key = b.out("cat /etc/ssh/ssh_host_ed25519_key.pub").split()[1:2]

        # the board test: the new firmware from RAM, the flash read-only
        env = dict(os.environ, SSHPASS="0penBmc")
        tool = os.path.join(TOOLS, "reimu-ab-from-stock")
        kx = os.path.join(W, "reimu-ab-kexec.tar")
        r = subprocess.run([tool, "test", "127.0.0.1:%d" % SSH, kx], env=env, capture_output=True, text=True, timeout=1200)
        print("    " + (r.stdout + r.stderr).strip()[-500:].replace("\n", "\n    "))
        check(r.returncode == 0, "test mode: the new firmware started from the stock one (kexec)")
        check(ab_state(b) == "kernel=a rofs=ram state=ram", "rofs from the initrd (%s)" % ab_state(b))
        w = b.ssh("reimu-flash write /dev/mtd7 0 /etc/hostname")
        check(w.returncode != 0, "flash read-only in test mode (%s)" % w.stderr.strip()[:80])
        check(b.ssh("fw_setenv reimu_x 1").returncode != 0, "U-Boot environment read-only in test mode")
        m = b.out("systemctl is-enabled set_mac revert-to-mfg").split()
        check(m == ["masked-runtime"] * 2, "set_mac, revert-to-mfg masked from RAM (%s)" % m)
        st = b.out("systemctl is-system-running").strip()
        check(st in ("running", "degraded"), "systemd reached its target from RAM (%s)" % st)
        r = b.ssh("cat > /tmp/bc.sh; sh /tmp/bc.sh", timeout=120) if False else None
        with open(os.path.join(HERE, "board-check.sh"), "rb") as f:
            c, e2 = ssh_cmd("cat > /tmp/board-check.sh")
            subprocess.run(c, env=e2, stdin=f, capture_output=True)
        print("    " + b.out("sh /tmp/board-check.sh 2>&1 | grep -E '^(FAIL|INFO RSS|INFO memory)'").strip().replace("\n", "\n    "))
        # back to the stock firmware without a reset (kexec)
        sk = os.path.join(W, "stock-kexec")
        if os.path.isdir(sk):
            r = subprocess.run([os.path.join(TOOLS, "reimu-ab-back-to-stock"), "127.0.0.1:%d" % SSH, sk],
                               env=env, capture_output=True, text=True, timeout=900)
            print("    " + (r.stdout + r.stderr).strip()[-300:].replace("\n", "\n    "))
            check(r.returncode == 0 and "reimu-1.0.2" in r.stdout and "5.10" in r.stdout,
                  "back to the stock firmware by kexec (no reset)")
            check(b.wait_ssh(500), "stock up again")
        else:
            b.ssh("reboot")
            check(b.wait_console(r"5\.10\.20", 300, len(b.console()) - 10) is not None, "after the test a reboot is the stock firmware")
            check(b.wait_ssh(500), "stock up again")

        # the first install, as on the board
        env["SETTINGS_KEEP"] = os.path.join(W, "stock-settings.tar")
        r = subprocess.run([tool, "install", "127.0.0.1:%d" % SSH, kx, os.path.join(W, "reimu-ab.tar"), "--reboot"],
                           env=env, capture_output=True, text=True, timeout=3600)
        print("    " + (r.stdout + r.stderr).strip()[-1500:].replace("\n", "\n    "))
        check(r.returncode == 0, "installed from the stock firmware")
        check(b.wait_console(r"reimu\.slot=a", 300, len(b.console()) - 10) is not None, "U-Boot A/B: slot A")
        check(b.wait_ssh(500), "the new firmware up from the flash")
        check(ab_state(b) == "kernel=a rofs=a state=ok", "slot A from flash (%s)" % ab_state(b))
        check(b.out("cat /etc/ssh/ssh_host_ed25519_key.pub").split()[1:2] == stock_key, "ssh host key kept (settings)")
        time.sleep(15)
        e = benv(b, "reimu_slot", "reimu_tries", "reimu_pending")
        check(e == {"reimu_slot": "a", "reimu_tries": "0", "reimu_pending": "0"}, "first boot confirmed (%s)" % e)
        check(b.ssh("reimu-ab verify").returncode == 0, "both slots intact")
    finally:
        b.stop()


def wait_env(b, want, timeout=90):
    """until the environment has these values (reimu-ab.service ran)"""
    end = time.time() + timeout
    while True:
        e = benv(b, *want)
        if all(e.get(k) == v for k, v in want.items()) or time.time() > end:
            return e
        time.sleep(3)


def fallback_boot(name):
    img = fresh(name)
    damage(img, "rofs-a", at=0x200000)
    b = Bmc(name, img).start()
    check(b.wait_ssh(), "up")
    check(ab_state(b) == "kernel=a rofs=b state=fallback", "fallback: kernel A on rofs-b (%s)" % ab_state(b))
    e = wait_env(b, {"reimu_slot": "b", "reimu_tries": "0"})
    check(e == {"reimu_slot": "b", "reimu_tries": "0"}, "slot B next (reimu-ab boot) (%s)" % e)
    return img, b


def t_fallback():
    img, b = fallback_boot("fallback")
    old = {p: part_sha(img, p) for p in ("kernel-b", "rofs-b")}
    try:
        check(b.scp(os.path.join(W, "reimu-ab.tar"), "/tmp/reimu-ab.tar"), "update copied")
        r = b.ssh("reimu-ab install /tmp/reimu-ab.tar b", timeout=900)
        print("    " + (r.stdout + r.stderr).strip().replace("\n", "\n    "))
        check(r.returncode != 0 and "rofs-b is the only intact rofs" in r.stderr,
              "install b refused: rofs-b is the only intact rofs")
        e = benv(b, "reimu_slot", "reimu_pending")
        check(e.get("reimu_slot") == "b" and e.get("reimu_pending") != "1", "environment as it was (%s)" % e)
        b.ssh("rm -f /tmp/reimu-ab.tar")
        # the web update: reimu-abd runs "reimu-ab install IMAGE" (no slot)
        t0 = b.out("date +%s").strip()
        st, r = redfish("POST", "/redfish/v1/UpdateService", open(os.path.join(W, "reimu-ab.tar"), "rb").read(),
                        "application/octet-stream", timeout=600)
        check(st in (200, 202), "upload through Redfish UpdateService: HTTP %d" % st)
        end = time.time() + 1200
        j = {}
        while time.time() < end:
            try:
                j = json.loads(redfish("GET", "/xyz/openbmc_project/reimu/ab")[1]["data"]["Json"])
            except Exception:
                j = {}
            if (j.get("job") or {}).get("state") in ("ok", "failed"):
                break
            time.sleep(5)
        job = j.get("job") or {}
        check(job.get("state") == "ok", "reimu-abd install: %s" % job.get("message"))
        # what reimu-ab said to reimu-abd ("reimu-abd: install: LINE" in its journal)
        jl = b.out("journalctl -o cat --since @%s -u reimu-abd | sed -n 's/^reimu-abd: install: reimu-ab: \\(slot [ab]: [a-z]*\\)$/\\1/p'"
                   % t0).split("\n")
        print("    reimu-ab: " + " | ".join(jl))
        check([l for l in jl if l] == ["slot a: kernel", "slot a: rofs"], "it wrote slot A (the damaged one), not B")
        e = benv(b, "reimu_slot", "reimu_pending", "reimu_tries")
        check(e == {"reimu_slot": "a", "reimu_pending": "1", "reimu_tries": "0"}, "slot A next, pending (%s)" % e)
        check(b.ssh("reimu-ab verify a").returncode == 0, "slot A intact again")
    finally:
        b.stop()
    check({p: part_sha(img, p) for p in old} == old, "slot B byte for byte as it was")
    check(part_sha(img, "rofs-a") == part_sha(os.path.join(W, "ab.mtd"), "rofs-a"), "rofs-a is the update's")


def t_fallcut():
    img, b = fallback_boot("fallcut")
    old = {p: part_sha(img, p) for p in ("kernel-b", "rofs-b")}
    try:
        # all of rofs-a to write (a long write to cut)
        b.ssh("flash_eraseall /dev/mtd/rofs-a >/dev/null 2>&1", timeout=300)
        check(b.scp(os.path.join(W, "reimu-ab.tar"), "/tmp/reimu-ab.tar"), "update copied")
        p, lines = ssh_bg("reimu-ab install /tmp/reimu-ab.tar")
        end = time.time() + 600
        while time.time() < end and not any("slot a: rofs" in l for l in lines) and p.poll() is None:
            time.sleep(1)
        check(any("slot a: rofs" in l for l in lines), "install writes slot A (%s)" % " | ".join(lines))
        time.sleep(1)
        check(p.poll() is None, "rofs-a still being written")
        b.stop()  # the power goes
        p.kill()
        print("    cut after: " + " | ".join(lines))
        check(not any("slot b" in l for l in lines), "slot B not touched")
    finally:
        b.stop()
    check({q: part_sha(img, q) for q in old} == old, "slot B byte for byte as it was")
    check(ubenv.read(bytearray(open(img, "rb").read())).get("reimu_slot") == "b",
          "the environment at the cut: slot B")
    b = Bmc("fallcut2", img).start()
    try:
        check(b.wait_ssh(), "up after the power cut")
        check("reimu.slot=b" in b.console() and ab_state(b) == "kernel=b rofs=b state=ok",
              "slot B boots (%s)" % ab_state(b))
        check(b.ssh("reimu-ab verify a").returncode != 0, "slot A: half written")
        check(b.scp(os.path.join(W, "reimu-ab.tar"), "/tmp/reimu-ab.tar"), "update copied")
        r = b.ssh("reimu-ab install /tmp/reimu-ab.tar b", timeout=900)
        check(r.returncode != 0 and "kernel-b may be the only bootable kernel" in r.stderr,
              "install b refused: %s" % r.stderr.strip()[:140])
        r = b.ssh("reimu-ab install /tmp/reimu-ab.tar both", timeout=1800)
        o = r.stdout + r.stderr
        print("    " + o.strip().replace("\n", "\n    "))
        order = re.findall(r"slot ([ab]): kernel$", o, re.M)
        check(r.returncode == 0 and order == ["a", "b"], "install both: the damaged slot A first (%s)" % order)
        e = benv(b, "reimu_slot", "reimu_pending")
        check(e == {"reimu_slot": "b", "reimu_pending": "1"}, "the last written (B) next, pending (%s)" % e)
        check(b.ssh("reimu-ab verify").returncode == 0, "both slots intact")
    finally:
        b.stop()


FLASH_FAIL = """#!/bin/sh
# the test's reimu-flash: fails the write number $FAIL_AT (exit 1, no "ok")
n=$(( $(cat /tmp/ff.n 2>/dev/null || echo 0) + 1 )); echo $n > /tmp/ff.n
echo "$n $*" >> /tmp/ff.log
if [ $n = "$FAIL_AT" ]; then echo "write 10%"; echo "reimu-flash: simulated failure" >&2; exit 1; fi
exec reimu-flash "$@"
"""


def t_flashfail():
    b = Bmc("flashfail", fresh("flashfail")).start()
    try:
        check(b.wait_ssh(), "up")
        wait_env(b, {"reimu_slot": "a", "reimu_tries": "0"})
        check(b.scp(os.path.join(W, "reimu-ab.tar"), "/tmp/reimu-ab.tar"), "update copied")
        open(os.path.join(W, "flash-fail"), "w").write(FLASH_FAIL)
        b.scp(os.path.join(W, "flash-fail"), "/tmp/flash-fail")
        before = b.out("fw_printenv")
        for at, part in ((1, "kernel"), (2, "rofs")):
            b.ssh("rm -f /tmp/ff.n /tmp/ff.log; chmod +x /tmp/flash-fail")
            r = b.ssh("FAIL_AT=%d REIMU_FLASH=/tmp/flash-fail reimu-ab install /tmp/reimu-ab.tar" % at, timeout=900)
            o = r.stdout + r.stderr
            print("    " + o.strip().replace("\n", "\n    "))
            calls = b.out("cat /tmp/ff.log").strip().split("\n")
            check(r.returncode != 0 and "slot b: %s write failed" % part in r.stderr,
                  "reimu-flash fails on the %s: \"slot b: %s write failed\"" % (part, part))
            check(len(calls) == at, "nothing written after it (%s)" % calls)
            check(at == 2 or "slot b: rofs" not in o, "no rofs write after the kernel failed")
            check(b.out("fw_printenv") == before, "the environment as it was")
    finally:
        b.stop()


# the test's reimu-flash: before each write (and halfway through the ones
# $CUT_HALF names) it waits for the test to copy the flash image (= the power
# going at that moment). A write is named by its offset, or by its device
# (mtd3) at offset 0
FLASH_CUT = """#!/bin/sh
mkdir -p /tmp/cut
n=$(( $(cat /tmp/cut/n 2>/dev/null || echo 0) + 1 )); echo $n > /tmp/cut/n
if [ "$1" = write ]; then
    at=$3; [ "$3" = 0 ] && at=${2##*/}
    echo "$n $at" > /tmp/cut/at; while [ ! -e /tmp/cut/go$n ]; do sleep 1; done
    case " ${CUT_HALF:-} " in *" $at "*)
        sz=$(stat -c %s $4)
        dd if=$4 of=/tmp/cut/half bs=65536 count=$((sz / 65536 / 2)) 2>/dev/null
        reimu-flash write $2 $3 /tmp/cut/half 5 >/dev/null || exit 1
        rm -f /tmp/cut/half
        echo "$n.5 $at" > /tmp/cut/at; while [ ! -e /tmp/cut/go$n.5 ]; do sleep 1; done ;;
    esac
fi
exec reimu-flash "$@"
"""
STOCK_FILES = ["u-boot-env.bin", "kernel.bin", "rofs.bin", "rwfs.bin", "SHA256SUMS"]


def stock_data():
    return {f: open(os.path.join(STOCK, f + ".bin"), "rb").read() for f in ("u-boot-env", "kernel", "rofs", "rwfs")}


def to_ram(b, pending=False):
    """reimu-ab ram (a kexec), the stock dump copied into RAM"""
    b.ssh("nohup reimu-ab ram >/dev/null 2>&1 &", timeout=30)
    time.sleep(20)
    check(b.wait_ssh(), "up again after reimu-ab ram (kexec)")
    check("reimu.ram" in b.out("cat /proc/cmdline"), "RAM mode")
    b.ssh("mkdir -p /tmp/stock")
    ok = all(b.scp(os.path.join(STOCK, f), "/tmp/stock/" + f) for f in STOCK_FILES)
    check(ok, "the stock dump copied")
    open(os.path.join(W, "flash-cut"), "w").write(FLASH_CUT)
    b.scp(os.path.join(W, "flash-cut"), "/tmp/flash-cut")
    b.ssh("chmod +x /tmp/flash-cut; rm -rf /tmp/cut")


def restore_cut(b, img, name, half=None, stop_at=None):
    return run_cut(b, img, name, "reimu-ab restore-stock /tmp/stock", half, stop_at)


def run_cut(b, img, name, cmd, half=None, stop_at=None):
    """CMD with FLASH_CUT as reimu-flash: a copy of the flash at each stop
    [(label, image)]; stop_at: return there (the caller kills QEMU)"""
    b.ssh("rm -rf /tmp/cut")
    p, lines = ssh_bg("CUT_HALF='%s' REIMU_FLASH=/tmp/flash-cut %s 2>&1" % (half or "", cmd))
    snaps, seen = [], set()
    end = time.time() + 3600
    while time.time() < end and p.poll() is None:
        at = b.out("cat /tmp/cut/at 2>/dev/null", timeout=60).split()
        if len(at) == 2 and at[0] not in seen:
            seen.add(at[0])
            time.sleep(2)
            label = ("mid-" if at[0].endswith(".5") else "before-") + at[1]
            while label in [l for l, _ in snaps]:  # the same place again (the rwfs: erased, then written)
                label += "#2"
            snap = os.path.join(W, "%s-%s.mtd" % (name, label))
            shutil.copyfile(img, snap)
            snaps.append((label, snap))
            print("    stop %s: %s" % (at[0], label), flush=True)
            if label == stop_at:
                return snaps, lines, p
            b.ssh("touch /tmp/cut/go%s" % at[0])
        time.sleep(2)
    p.wait(timeout=60)
    print("    " + "\n    ".join(lines[-12:]))
    return snaps, lines, p


def boot_outcome(name, img, timeout=480, watch_wdt=False):
    """boot a flash image: what comes up ("A/B kernel=.. rofs=..", "stock",
    or "nothing" with the console's last words)"""
    b = Bmc(name, img).start()
    try:
        if not b.wait_ssh(timeout):
            c = [l for l in b.console().splitlines() if l.strip()]
            return "nothing (%d boots; %s)" % (b.boots(), " / ".join(c[-3:])[-240:])
        ab = b.out("cat /run/initramfs/reimu-ab 2>/dev/null").strip()
        ver = b.out(". /etc/os-release; echo $VERSION_ID; uname -r").split()
        r = ("A/B " + ab) if ab else "stock %s" % " ".join(ver)
        if watch_wdt:  # still up past WDT1's 300 s (U-Boot's A/B bootcmd armed it)?
            n = b.boots()
            while float(b.out("cut -d' ' -f1 /proc/uptime").strip() or 0) < 340 and b.boots() == n:
                time.sleep(10)
            r += ", WDT1: %s" % ("reset" if b.boots() != n else "up after %s s" % b.out("cut -d. -f1 /proc/uptime").strip())
        return r
    finally:
        b.stop()


def t_restorecut():
    if not all(os.path.exists(os.path.join(STOCK, f)) for f in STOCK_FILES):
        check(False, "stock dump in %s" % STOCK)
        return
    img = fresh("restorecut")
    orig = open(img, "rb").read()
    b = Bmc("restorecut", img).start()
    try:
        check(b.wait_ssh(), "up")
        wait_env(b, {"reimu_slot": "a", "reimu_tries": "0"})
        # an install pending (slot B next, not yet healthy): "reimu-ab ram" must not confirm or cancel it
        b.ssh("fw_setenv reimu_slot b; fw_setenv reimu_pending 1")
        before = b.out("fw_printenv")
        to_ram(b)
        for i in range(30):
            if b.out("systemctl is-active reimu-ab").strip() in ("active", "failed"):
                break
            time.sleep(3)
        time.sleep(20)
        check(b.out("fw_printenv") == before, "reimu-ab ram: the environment as it was (pending=1, slot b)")
        check("started by kexec" in b.out("journalctl -o cat -t reimu-ab"), "reimu-ab boot: started by kexec, nothing to confirm")
        b.ssh("fw_setenv reimu_slot a; fw_setenv reimu_pending")
        snaps, lines, p = restore_cut(b, img, "restorecut", half="0x1c00000 0x3000000")
        check(p.returncode == 0, "restore-stock completed (%s)" % (lines[-1:] or "?"))
        check(any("slot b boots until the stock firmware is complete" in l for l in lines), "slot B kept bootable")
    finally:
        b.stop()
    st = stock_data()
    labels = [l for l, s in snaps]
    want = ["before-0x100000", "before-0x680000", "before-0x1c00000", "mid-0x1c00000", "before-0xe0000",
            "before-0xf0000", "before-0x3000000", "mid-0x3000000", "before-0x3000000#2", "mid-0x3000000#2"]
    check(labels == want, "the writes in order: kernel, rofs to 0x1c00000, the rest of it, env, env, "
          "the rwfs's place erased, rwfs (%s)" % labels)
    ab_b = orig[0x1c00000:0x3700000]
    # the rwfs as "reimu-ab ram" left it (the system wrote it before)
    first = open(snaps[0][1], "rb").read() if snaps else b""
    for label, snap in snaps:
        f = open(snap, "rb").read()
        if label in ("before-0x100000", "before-0x680000", "before-0x1c00000"):
            check(f[0x1c00000:0x3700000] == ab_b and f[0xe0000:0x100000] != st["u-boot-env"][:0x20000],
                  "%s: slot B untouched, the A/B environment" % label)
        if label == "before-0x1c00000":
            check(f[0x100000:0x680000] == st["kernel"] and f[0x680000:0x1c00000] == st["rofs"][:0x1580000],
                  "%s: the stock kernel and rofs over slot A are in" % label)
        if label in ("before-0xe0000", "before-0xf0000", "before-0x3000000", "mid-0x3000000"):
            check(f[0x100000:0x680000] == st["kernel"] and f[0x680000:0x3000000] == st["rofs"],
                  "%s: the stock kernel and rofs complete" % label)
        if label == "before-0xe0000":
            check(f[0x3000000:0x3700000] == orig[0x3000000:0x3700000] and f[0x3700000:] == first[0x3700000:],
                  "%s: the A/B rwfs (and rofs-b's end) untouched yet" % label)
        if label == "before-0xf0000":
            check(f[0xe0000:0xf0000] == st["u-boot-env"][:0x10000], "%s: the first copy of the env is stock" % label)
        if label.startswith("before-0x3") or label.startswith("mid-0x3"):
            check(f[0xe0000:0x100000] == st["u-boot-env"], "%s: the stock environment in, both copies" % label)
        if label == "mid-0x3000000":
            check(f[0x3000000:0x3800000] == b"\xff" * 0x800000 and f[0x3800000:] == first[0x3800000:],
                  "%s: half of the rwfs's place erased" % label)
        if label == "before-0x3000000#2":
            check(f[0x3000000:] == b"\xff" * 0x1000000, "%s: the rwfs's place erased" % label)
        if label == "mid-0x3000000#2":
            check(f[0x3000000:0x3800000] == st["rwfs"][:0x800000] and f[0x3800000:] == b"\xff" * 0x800000,
                  "%s: half the stock rwfs, the rest erased" % label)
    final = open(img, "rb").read()
    check(final[0xe0000:0x100000] == st["u-boot-env"] and final[0x100000:] == st["kernel"] + st["rofs"] + st["rwfs"],
          "complete: the stock firmware byte for byte")
    # what each state boots (as if the power went there); the stock ones
    # watched past 300 s (WDT1, armed by the A/B bootcmd)
    outcomes = {}
    boot = ["before-0x680000", "before-0x1c00000", "mid-0x1c00000", "before-0xe0000", "before-0xf0000",
            "before-0x3000000", "mid-0x3000000", "before-0x3000000#2", "mid-0x3000000#2"]
    for label, snap in snaps:
        if label in boot:
            outcomes[label] = boot_outcome("rc-" + label.replace("#", "-"), snap, watch_wdt=label not in boot[:3])
            print("    power cut %s: %s" % (label, outcomes[label]), flush=True)
    outcomes["complete"] = boot_outcome("rc-complete", img, watch_wdt=True)
    print("    power cut complete: %s" % outcomes["complete"])
    for k in ("before-0x680000", "before-0x1c00000"):
        check(outcomes.get(k, "").startswith("A/B kernel=b rofs=b"), "%s: slot B boots" % k)
    check(outcomes.get("mid-0x1c00000", "").startswith("nothing"), "mid-0x1c00000: the window, nothing boots (recorded)")
    for k in ("before-0xe0000", "before-0xf0000"):
        check(outcomes.get(k, "").startswith("stock reimu-1.0.2"), "%s (stock kernel and rofs complete): the stock firmware boots" % k)
    for k in ("before-0x3000000", "mid-0x3000000", "before-0x3000000#2", "mid-0x3000000#2", "complete"):
        check(outcomes.get(k, "").startswith("stock reimu-1.0.2") and "up after" in outcomes.get(k, ""),
              "%s (the stock environment): the stock firmware boots and stays up" % k)


def t_restorecut_a():
    if not all(os.path.exists(os.path.join(STOCK, f)) for f in STOCK_FILES):
        check(False, "stock dump in %s" % STOCK)
        return
    img = fresh("restorecut-a")
    damage(img, "rofs-b", at=0x200000)
    orig = open(img, "rb").read()
    b = Bmc("restorecut-a", img).start()
    try:
        check(b.wait_ssh(), "up")
        to_ram(b)
        snaps, lines, p = restore_cut(b, img, "restorecut-a", stop_at="before-0x100000")
        print("    " + "\n    ".join(lines[-6:]))
        check(any("slot a boots until the stock firmware is complete" in l for l in lines), "slot A kept bootable")
    finally:
        b.stop()
    st = stock_data()
    labels = [l for l, s in snaps]
    check(labels == ["before-0x1c00000", "before-0x100000"],
          "slot B's region first: the rofs from 0x1c00000, then the kernel (%s)" % labels)
    f = open(snaps[-1][1], "rb").read()
    first = open(snaps[0][1], "rb").read()
    check(f[0x100000:0x1c00000] == orig[0x100000:0x1c00000] and f[0x3000000:0x3700000] == orig[0x3000000:0x3700000]
          and f[0xe0000:0x100000] == first[0xe0000:0x100000] and f[0x3700000:] == first[0x3700000:],
          "slot A, the end of rofs-b, the environment, the rwfs untouched before the kernel")
    check(f[0x1c00000:0x3000000] == st["rofs"][0x1580000:], "the stock rofs over slot B is in")
    o = boot_outcome("rca-before-kernel", snaps[-1][1])
    print("    power cut before the kernel: %s" % o)
    check(o.startswith("A/B kernel=a rofs=a"), "slot A boots")


def sol_image(name):
    """kernel-a intact but rofs-a's digests gone (so is its kernel's), kernel-b
    damaged, rofs-b intact: U-Boot boots kernel-a, the initramfs falls back to
    rofs-b; kernel-a is the only bootable kernel, though no digest says so.
    kernel-a from WORKDIR/kernel-other.fit if there (another build's FIT: an
    install then changes every block of it, as a real update does)"""
    img = fresh(name)
    other_fit = os.path.join(W, "kernel-other.fit")
    with open(img, "r+b") as f:
        if os.path.exists(other_fit):
            f.seek(LAYOUT["kernel-a"][0])
            f.write(open(other_fit, "rb").read()[:LAYOUT["kernel-a"][1]])
        f.seek(LAYOUT["rofs-a"][0] + LAYOUT["rofs-a"][1] - 4096)
        f.write(b"\xff" * 4096)
    damage(img, "kernel-b", at=0x100000)
    return img


def t_solcut():
    img = sol_image("solcut")
    b = Bmc("solcut", img).start()
    snaps = []
    try:
        check(b.wait_ssh(), "up")
        check(ab_state(b) == "kernel=a rofs=b state=fallback", "kernel A on rofs-b (%s)" % ab_state(b))
        wait_env(b, {"reimu_tries": "0"})
        time.sleep(5)
        e = benv(b, "reimu_slot")
        check(e.get("reimu_slot") == "a", "slot A stays the one to boot: kernel-b isn't intact (%s)" % e)
        check(b.scp(os.path.join(W, "reimu-ab.tar"), "/tmp/reimu-ab.tar"), "update copied")
        open(os.path.join(W, "flash-cut"), "w").write(FLASH_CUT)
        b.scp(os.path.join(W, "flash-cut"), "/tmp/flash-cut")
        b.ssh("chmod +x /tmp/flash-cut")
        before = b.out("fw_printenv")
        halves = "mtd3 mtd4 mtd5 mtd6"
        # the web update's command (no slot): slot A would be the fallback's target
        s1, lines, p = run_cut(b, img, "solcut-default", "reimu-ab install /tmp/reimu-ab.tar", halves)
        print("    " + "\n    ".join(l for l in lines if not re.match(r"(write|verify) ", l)))
        check(p.returncode != 0 and not s1 and any("install IMAGE both" in l for l in lines),
              "install (no slot): refused, nothing written, \"both\" suggested (%d writes)" % len(s1))
        check(not any("is damaged" in l for l in lines), "the refusal doesn't call an unverifiable part damaged")
        check(b.out("fw_printenv") == before, "the environment as it was")
        snaps += s1
        # both: the parts in an order that always leaves a bootable kernel and rofs
        s2, lines, p = run_cut(b, img, "solcut-both", "reimu-ab install /tmp/reimu-ab.tar both", halves)
        order = [m.group(1) for m in (re.match(r"reimu-ab: slot ([ab]: (kernel|rofs))$", l) for l in lines) if m]
        print("    both: " + ", ".join(order))
        check(p.returncode == 0, "install both: done (%s)" % (lines[-1:] or "?"))
        check(order[:1] == ["b: kernel"] and order.index("a: rofs") < order.index("b: rofs"),
              "kernel-b (damaged) first, rofs-a (damaged) before rofs-b (the only intact one)")
        check(b.ssh("reimu-ab verify").returncode == 0, "both slots intact")
        snaps += s2
    finally:
        b.stop()
    for label, snap in snaps:
        o = boot_outcome("sc-" + label, snap, timeout=420)
        print("    power cut %s: %s" % (label, o), flush=True)
        check(o.startswith("A/B"), "power cut %s: an A/B slot boots" % label)


TESTS = [("boot", t_boot), ("memory", t_memory), ("regress", t_regress), ("update", t_update),
         ("badkernel", t_badkernel), ("badrofs", t_badrofs), ("hang", t_hang), ("fallback", t_fallback),
         ("fallcut", t_fallcut), ("flashfail", t_flashfail), ("solcut", t_solcut), ("restorecut", t_restorecut),
         ("restorecut-a", t_restorecut_a), ("stock", t_stock)]

if __name__ == "__main__":
    for name, fn in TESTS:
        if ONLY and name not in ONLY:
            continue
        print("== " + name, flush=True)
        t = time.time()
        try:
            fn()
        except Exception as e:  # a test that crashes is a failure, the others still run
            check(False, "%s: %r" % (name, e))
        print("   (%d s)" % (time.time() - t), flush=True)
    print("\n%d failed" % len(failed) + ("".join("\n  " + f for f in failed)))
    sys.exit(1 if failed else 0)
