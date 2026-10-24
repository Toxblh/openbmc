#!/usr/bin/env python3
"""A/B firmware of REIMU-5564 in QEMU (ast2500-evb, 64 MiB SPI NOR), with the
U-Boot dumped from the real BMC, as on the board.

    test_ab_qemu.py WORKDIR [TEST...]

WORKDIR holds ab.mtd (the build's .ab.mtd with the real U-Boot spliced in,
see fetch.sh), reimu-ab.tar (the update) and, for the stock tests, the dump of
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
  stock       "reimu-ab ram", restore-stock from the dump, the stock
              firmware boots; then the first install from the stock firmware
              (kexec into the new image from RAM, write both slots, the A/B
              environment, the settings), the new firmware boots
"""
import os, re, shutil, socket, subprocess, sys, tempfile, time

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


TESTS = [("boot", t_boot), ("memory", t_memory), ("regress", t_regress), ("update", t_update),
         ("badkernel", t_badkernel), ("badrofs", t_badrofs), ("hang", t_hang), ("stock", t_stock)]

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
