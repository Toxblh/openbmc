#!/usr/bin/env python3
"""Regression tests in QEMU for bugs the other suites had no check (or no
red run) for; REGRESSIONS.md lists every bug and the test that keeps it out.

    test_regressions_qemu.py WORKDIR [GROUP...]

WORKDIR as for test_ab_qemu.py (ab.mtd with the real U-Boot, see fetch.sh);
the platform group also wants pns.bin and fru.bin there (as
test_fru_sensors_qemu.py). RQ, AB_SSH, AB_HTTPS, AB_IPMI as for
test_ab_qemu.py; IPMI over LAN as test_ipmi_redfish_qemu.py does it.

REG_IMAGE=FILE: boot that image instead of WORKDIR/ab.mtd. The bugs of
2026-10-23/24 are in the A/B image delivered before them (version
reimu-1.0.2-50-g4adffbb20a): REG_IMAGE=that image shows each of their checks
FAIL (red), WORKDIR/ab.mtd PASS (green).
REG_RED=1: also put the older bugs back on the running BMC (a file of the
time restored into the RAM overlay, a mutant of a script in /tmp) and expect
the check to FAIL then ("red ok"); the bug's FIX must make it PASS again in
the same run. Needs docker with the build container (REIMU_DOCKER, default
"reimu") for files from the git history.

Groups (all by default):
  base     [4] tmux-logger with the UTF-8 locale (red: the locale archive
           hidden); [5] reimu-kvm starts once bmcweb has made its TLS
           certificate (red: the unit before 4adffbb20a, without the drop-in
           of reimu-restart-policy); [13] overheatd
           survives a broken config, comes back after SIGKILL; [23]
           update-host-flash checks an image through reimu-pns-flash; [25]
           Redfish Truststore certificates 200; [29] mc info Firmware
           Revision from the version (not 0.00); [31] Redfish SSH enabled
           with OpenSSH; [33] DHCP on eth0 in Redfish and lan print; [36] no
           phosphor-fan-control/-monitor, reimu-fand runs
  kexec    [6] the MAC addresses of the environment survive "reimu-ab ram"
           (no red in QEMU: its NIC keeps the address over a kexec, the
           board's doesn't; board-check.sh checks it there); [7] (red only) a mask of a
           unit multi-user.target requires: systemd never gets there; [8]
           restore-stock checks only its four files' lines of SHA256SUMS
           (red: sha256sum -c of the whole file)
  platform [15] l_pcs_i2c above 65.5 C and a failed read (not 0 C); [38]
           reimu-pns-scan never has the flash on the BMC's side while the
           platform powers up
  fruid    [17] mcst-fruid opens the PNS flash read-only (an LD_PRELOAD
           logger of open(), openlog.c, cross-built) and survives an EEPROM
           that fails the header check (red: the mcst-fruid of the image
           before the fixes, from OLD_IMAGE, default WORKDIR/../old/ab.mtd)
  hwmon    [12] the lm96163 sensors (device tree label props) get configs
           (red: today's hwmon-instantiator source with only that fix taken
           out, cross-built; the unchanged source cross-built is the
           control); [21] a failing PCS read never published as a value,
           also without REMOVERCS (red: phosphor-hwmon-readd built with the
           negative-errno-on-fail at its default, REG_HWMON_ERRNO=file)
  pns      [39] reimu-pns-flash switches the PNS flash back to the host when
           a job fails (red: its switch() through gpiowrite in the EXIT trap)
"""
import os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("AB_IPMI", "2423")
GROUPS_ARG = sys.argv[2:]
sys.argv = sys.argv[:2]
import test_ab_qemu as T  # noqa: E402  (reads WORKDIR, the ports, RQ)
import test_ipmi_redfish_qemu as IR  # noqa: E402  (ipmi(), get(), rf())

W, check = T.W, T.check
IMAGE = os.environ.get("REG_IMAGE", os.path.join(W, "ab.mtd"))
RED = os.environ.get("REG_RED") == "1"
DOCKER = os.environ.get("REIMU_DOCKER", "reimu")
GROUPS = GROUPS_ARG or ["base", "kexec", "platform", "fruid", "hwmon", "pns"]
red_missing = []


def red(cond, what):
    """a check run with the bug put back: it must FAIL"""
    print(("  red ok (FAIL as it must) " if not cond else "  RED MISSING (passes with the bug back) ") + what, flush=True)
    if cond:
        red_missing.append(what)


def git_show(rev, path):
    r = subprocess.run(["docker", "exec", DOCKER, "git", "-C", "/work/openbmc", "show", "%s:%s" % (rev, path)],
                       capture_output=True, timeout=60)
    return r.stdout if r.returncode == 0 else None


def put(b, data, dst):
    c, env = T.ssh_cmd("cat > '%s'" % dst)
    return subprocess.run(c, env=env, input=data, capture_output=True, timeout=120).returncode == 0


def image(name):
    img = os.path.join(W, name + ".mtd")
    with open(IMAGE, "rb") as s, open(img, "wb") as d:
        d.write(s.read())
    return img


def until(fn, timeout, step=2):
    end = time.time() + timeout
    while True:
        v = fn()
        if v or time.time() > end:
            return v
        time.sleep(step)


# ------------------------------------------------------------------ base
def tmux_ok(b):
    b.ssh("systemctl reset-failed tmux-logger; systemctl restart tmux-logger")
    time.sleep(4)
    return b.out("systemctl is-active tmux-logger").strip() == "active"


def t_locale(b):
    if RED:
        b.ssh(": > /tmp/no-locale; mount --bind /tmp/no-locale /usr/lib/locale/locale-archive")
        red(tmux_ok(b), "[4] tmux-logger with the locale archive hidden")
        b.ssh("umount /usr/lib/locale/locale-archive")
    check(tmux_ok(b), "[4] tmux-logger (tmux needs a UTF-8 locale)")


CERT = "/etc/ssl/certs/https/server.pem"


def kvm_after_cert(b):
    """reimu-kvm started before bmcweb has its certificate: up once it's there?"""
    b.ssh("systemctl stop reimu-kvm; systemctl reset-failed reimu-kvm; mv %s /tmp/server.pem.keep; "
          "systemctl start reimu-kvm" % CERT)
    time.sleep(15)  # it fails meanwhile: no certificate
    early = b.out("systemctl is-active reimu-kvm").strip()
    b.ssh("mv /tmp/server.pem.keep %s" % CERT)
    ok = until(lambda: b.out("systemctl is-active reimu-kvm").strip() == "active", 40)
    return bool(ok), early


def t_kvmcert(b):
    if RED:
        old = git_show("6e92451485", "meta-mcst/meta-common/recipes-mcst/reimu-kvm/files/reimu-kvm.service")
        # and reimu-restart-policy's drop-in (the second fix: no start limit)
        # hidden by an empty one of the same name
        if old and put(b, old, "/run/systemd/system/reimu-kvm.service"):
            b.ssh("mkdir -p /run/systemd/system/reimu-kvm.service.d; "
                  ": > /run/systemd/system/reimu-kvm.service.d/reimu-restart.conf; systemctl daemon-reload")
            ok, early = kvm_after_cert(b)
            red(ok, "[5] reimu-kvm.service of 6e92451485 (before the fix): up once the certificate is there (meanwhile %s)" % early)
            b.ssh("rm -rf /run/systemd/system/reimu-kvm.service /run/systemd/system/reimu-kvm.service.d; "
                  "systemctl daemon-reload")
        else:
            red(True, "[5] the unit of 6e92451485 from git (docker %s)" % DOCKER)
    ok, early = kvm_after_cert(b)
    check(ok, "[5] reimu-kvm up once bmcweb's certificate is there (without it: %s)" % early)


def t_overheatd(b):
    b.ssh("echo CPU_TEMP_LIMIT=abc > /etc/overheatd.conf; systemctl restart overheatd")
    time.sleep(10)
    a = b.out("pidof overheatd").strip()
    check(bool(a), "[13] overheatd keeps running with a broken config (pid %s, %s)" % (
        a or "none", b.out("systemctl is-active overheatd").strip()))
    b.ssh("rm -f /etc/overheatd.conf; systemctl reset-failed overheatd; systemctl restart overheatd")
    time.sleep(5)
    p0 = b.out("pidof overheatd").strip()
    b.ssh("kill -9 %s" % p0 if p0 else "true")
    p1 = until(lambda: (lambda p: p if p and p != p0 else None)(b.out("pidof overheatd").strip()), 20)
    check(bool(p0) and bool(p1), "[13] overheatd back after SIGKILL (%s -> %s)" % (p0 or "none", p1 or "none"))


def t_uhf(b):
    b.ssh("head -c 65536 /dev/urandom > /tmp/junk.bin")
    r = b.ssh("update-host-flash file:///tmp/junk.bin", timeout=300)
    o = (r.stdout + r.stderr).strip()
    check(r.returncode != 0 and "Не образ ПНС" in o and "not found" not in o,
          "[23] update-host-flash: the image checked by reimu-pns-flash and refused (rc %d: %s)" % (
              r.returncode, o.replace("\n", " | ")[-160:]))
    b.ssh("rm -f /tmp/junk.bin /tmp/firmware")


def t_truststore(b):
    s, j = IR.get("/redfish/v1/Managers/bmc/Truststore/Certificates")
    check(s == 200, "[25] Redfish Managers/bmc/Truststore/Certificates: %s" % s)


def t_mcinfo(b):
    v = b.out("sed -n 's/^VERSION_ID=//p' /etc/os-release").strip().strip('"')
    m = re.match(r"reimu-(\d+)\.(\d+)\.(\d+)", v)
    want = "%d.%02d" % (int(m.group(1)), int(m.group(2)) * 10 + int(m.group(3))) if m else "?"
    rc, o = IR.ipmi("mc", "info")
    got = (re.search(r"Firmware Revision\s*:\s*(\S+)", o) or [None, "-"])[1]
    check(rc == 0 and got == want, "[29] mc info Firmware Revision %s for %s (want %s)" % (got, v, want))


def t_ssh(b):
    s, j = IR.get("/redfish/v1/Managers/bmc/NetworkProtocol")
    ssh = (j.get("SSH") or {}).get("ProtocolEnabled") if s == 200 else None
    sock = b.out("systemctl is-active sshd.socket dropbear.socket 2>/dev/null | head -1").strip()
    check(ssh is True and sock == "active", "[31] Redfish NetworkProtocol SSH.ProtocolEnabled %s (sshd.socket %s)" % (ssh, sock))


def t_dhcp(b):
    s, j = IR.get("/redfish/v1/Managers/bmc/EthernetInterfaces/eth0")
    d = (j.get("DHCPv4") or {}).get("DHCPEnabled") if s == 200 else None
    rc, o = IR.ipmi("lan", "print", "1")
    src = (re.search(r"IP Address Source\s*:\s*(.*)", o) or [None, "-"])[1].strip()
    check(d is True and src == "DHCP Address", "[33] eth0 DHCP: Redfish DHCPEnabled %s, lan print '%s'" % (d, src))


def t_fans(b):
    u = b.out("systemctl list-units --all --no-legend --plain 'phosphor-fan-control*' 'phosphor-fan-monitor*' | cut -d' ' -f1").split()
    p = b.out("pidof phosphor-fan-control phosphor-fan-monitor").split()
    f = b.out("systemctl is-active reimu-fand").strip()
    check(not u and not p and f == "active", "[36] fans: phosphor-fan units %s, processes %s, reimu-fand %s" % (u or "none", p or "none", f))


def g_base():
    b = T.Bmc("reg-base", image("reg-base")).start()
    try:
        if not check(b.wait_ssh(600), "up (%s)" % os.path.basename(IMAGE)):
            return
        print("    " + b.out("sed -n 's/^VERSION_ID=//p' /etc/os-release; uname -r").replace("\n", " "))
        IR.wait_redfish()
        IR.wait_ipmi()
        subprocess.run(["curl", "-ks", "-o", "/dev/null", "https://127.0.0.1:%d/" % T.HTTPS])  # bmcweb: its certificate
        for fn in (t_locale, t_kvmcert, t_overheatd, t_uhf, t_truststore, t_mcinfo, t_ssh, t_dhcp, t_fans):
            try:
                fn(b)
            except Exception as e:  # one check that breaks doesn't stop the others
                check(False, "%s: %r" % (fn.__name__, e))
    finally:
        b.stop()


# ------------------------------------------------------------------ kexec
ENV_MAC = ("98:a7:b0:00:00:33", "98:a7:b0:00:00:34")  # QEMU's NIC: 98:a7:b0:00:00:01


def macs(b):
    return b.out("cat /sys/class/net/eth0/address /sys/class/net/eth1/address 2>/dev/null").split()


def ram(b, tool):
    """reimu-ab ram (kexec, rwfs in RAM) with TOOL: up again?"""
    n = len(b.console())
    b.ssh("nohup %s ram >/tmp/ram.log 2>&1 &" % tool, timeout=30)
    T.Bmc.wait_console(b, r"Booting Linux|Starting kernel|Linux version", 120, n)
    time.sleep(20)
    return b.wait_ssh(420)


def mutant_ab(b, name, sed):
    """a copy of the BMC's reimu-ab with a bug put back"""
    b.ssh("sed %s /usr/sbin/reimu-ab > /tmp/%s && chmod 755 /tmp/%s" % (sed, name, name))
    return b.ssh("! cmp -s /usr/sbin/reimu-ab /tmp/%s" % name).returncode == 0


STOCK_FILES = (("u-boot-env", 131072), ("kernel", 5767168), ("rofs", 43515904), ("rwfs", 16777216))


def restore_sums(b, tool):
    """restore-stock with SHA256SUMS listing a file it doesn't write (u-boot.bin,
    absent, as in the board's dump) and a damaged rwfs.bin: it must get past
    the first and stop at the second, nothing written"""
    b.ssh("rm -rf /tmp/st; mkdir -p /tmp/st; cd /tmp/st; " +
          "; ".join("dd if=/dev/zero of=%s.bin bs=4096 count=%d 2>/dev/null" % (n, s // 4096) for n, s in STOCK_FILES) +
          "; sha256sum u-boot-env.bin kernel.bin rofs.bin rwfs.bin > SHA256SUMS; "
          "echo 0000000000000000000000000000000000000000000000000000000000000000  u-boot.bin >> SHA256SUMS; "
          "echo x | dd of=rwfs.bin conv=notrunc 2>/dev/null", timeout=300)
    before = b.out("md5sum /dev/mtd0 | cut -c1-32", timeout=300).strip()
    r = b.ssh("%s restore-stock /tmp/st" % tool, timeout=600)
    o = (r.stdout + r.stderr).strip()
    after = b.out("md5sum /dev/mtd0 | cut -c1-32", timeout=300).strip()
    b.ssh("rm -rf /tmp/st")
    return r.returncode != 0 and "rwfs.bin: damaged" in o and before == after, "rc %d: %s; flash %s" % (
        r.returncode, o.replace("\n", " | ")[-140:], "unchanged" if before == after else "WRITTEN")


def g_kexec():
    img = image("reg-kexec")
    T.set_env(img, ethaddr=ENV_MAC[0], eth1addr=ENV_MAC[1])
    b = T.Bmc("reg-kexec", img).start()
    try:
        if not check(b.wait_ssh(600), "up"):
            return
        m = macs(b)
        check(m == list(ENV_MAC), "[6] MACs from the environment at a U-Boot boot %s" % m)
        if RED:
            if mutant_ab(b, "reimu-ab-mask", "'s/systemd.mask=set_mac.service/systemd.mask=phosphor-discover-system-state@0.service &/'"):
                n = b.boots()
                up = ram(b, "/tmp/reimu-ab-mask")
                st = b.out("systemctl is-system-running").strip() if up else "no ssh"
                inram = up and "reimu.ram" in b.out("cat /proc/cmdline")
                # frozen, the watchdog resets the BMC (on the board: the host too)
                # and U-Boot boots the flash: up, but not from RAM
                red(inram and st in ("running", "degraded") and b.boots() == n, "[7] kexec masking "
                    "phosphor-discover-system-state@0: systemd reaches its target from RAM (%s, %s; U-Boot boots %d -> %d)"
                    % (st, "from RAM" if inram else "not from RAM", n, b.boots()))
                # frozen: the watchdog resets it into a normal boot, or a reboot does
                if b.out("cat /proc/cmdline").find("reimu.ram") >= 0:
                    b.ssh("systemctl reboot -ff &", timeout=30)
                    time.sleep(30)
                b.wait_ssh(600)
            if mutant_ab(b, "reimu-ab-nomac", "'/fdtput .*local-mac-address/d'"):
                up = ram(b, "/tmp/reimu-ab-nomac")
                m = macs(b) if up else []
                # QEMU's ftgmac100 keeps the address in its registers over the
                # kexec: no red here (the board loses it: board-check.sh)
                print("    info [6] reimu-ab ram without fdtput: MACs after the kexec %s (QEMU keeps them anyway)" % m)
                b.ssh("reboot", timeout=30)
                time.sleep(30)
                check(b.wait_ssh(600), "back from the flash")
        up = ram(b, "reimu-ab")
        st = b.out("systemctl is-system-running").strip() if up else "no ssh"
        check(up and "reimu.ram" in b.out("cat /proc/cmdline") and st in ("running", "degraded"),
              "[7] reimu-ab ram: up from RAM, systemd reached its target (%s)" % st)
        m = macs(b)
        check(m == list(ENV_MAC), "[6] MACs after the kexec = the environment's %s" % m)
        if RED and mutant_ab(b, "reimu-ab-sums", "'/^do_restore_stock() {/a (cd \"$1\"; sha256sum -c SHA256SUMS >/dev/null) || die \"$1: damaged (SHA256SUMS)\"'"):
            ok, d = restore_sums(b, "/tmp/reimu-ab-sums")
            red(ok, "[8] restore-stock checking every line of SHA256SUMS: %s" % d)
        ok, d = restore_sums(b, "reimu-ab")
        check(ok, "[8] restore-stock: SHA256SUMS lines of other files ignored, each of its four checked (%s)" % d)
    finally:
        b.stop()


# ------------------------------------------------------------------ platform
def g_platform():
    import test_fru_sensors_qemu as F
    b = F.Platform("reg-plat", image("reg-plat")).start()
    try:
        if not check(b.wait_ssh(600), "up, platform off"):
            return
        F.fast(b)
        # [15] the PCS sensor of l_pcs_i2c: 1/8 C over I2C, m C in hwmon
        b.temperature("pcs0", 2500)  # the stand-in's register: 80 C as a PCS reads it
        b.temperature("pcs1", 2500)
        b.transition("On")
        b.wait(lambda: b.host_state() == "Running", 40)
        h = "ls -d /sys/bus/i2c/devices/5-0020/hwmon/hwmon* 2>/dev/null | head -1"
        got = b.wait(lambda: b.out(h).strip(), 90, 3)
        if not got:  # no hwmon-instantiator that works: the driver by hand
            b.ssh("modprobe l-pcs-i2c; echo l_pcs_i2c 0x20 > /sys/bus/i2c/devices/i2c-5/new_device")
            got = b.wait(lambda: b.out(h).strip(), 20)
            print("    5-0020 instantiated by hand: %s" % (got or "no hwmon"))
        time.sleep(3)
        v = b.out("cat %s/temp1_input" % got).strip() if got else ""
        check(v == "80000", "[15] l_pcs_i2c reads 80 C as 80000 (u16 wrapped at 65.5 C) (%s)" % (v or "-"))
        b.temperature("pcs0", 0)  # a PCS reading 0: no sensor, a failed read
        time.sleep(3)
        r = b.ssh("cat %s/temp1_input" % got) if got else None
        check(r is not None and r.returncode != 0, "[15] a failed PCS read is an error, not 0 C (%s)" % (
            ("rc %d %s" % (r.returncode, (r.stdout + r.stderr).strip()[-60:])) if r else "-"))
        b.temperature("pcs0", 1500)
        # [38] reimu-pns-scan while the platform is asked to power up
        b.transition("Off")
        b.wait(lambda: b.host_state() == "Off" and not b.state().get("power"), 60)
        time.sleep(5)
        o0 = b.state().get("o0_on", 0)
        c, env = T.ssh_cmd("/usr/libexec/reimu-pns-scan 2>&1; echo scan rc $?")
        p = subprocess.Popen(c, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        time.sleep(0.5)
        b.transition("On")
        lines = p.communicate(timeout=300)[0].strip().splitlines()
        b.wait(lambda: b.state().get("power"), 60)
        time.sleep(5)
        st = b.state()
        check(any("scan rc" in l for l in lines) and "not found" not in " ".join(lines) and st.get("o0_on", 0) == o0, "[38] the PNS flash never on the BMC's side while the platform powered up "
              "(o0_on %d -> %d, power %s; %s)" % (o0, st.get("o0_on", 0), st.get("power"), " | ".join(lines)[-120:]))
    finally:
        b.stop()


# ------------------------------------------------------------------ ARM builds
XCC = r"""set -e
C=/work/build-ab/tmp/sysroots-components; B=/tmp/reg-xcc; mkdir -p $B/bin
for t in as ld; do ln -sf $C/x86_64/binutils-cross-arm/usr/bin/arm-openbmc-linux-gnueabi/arm-openbmc-linux-gnueabi-$t $B/bin/$t; done
G=$C/arm1176jzs/libgcc/usr/lib/arm-openbmc-linux-gnueabi/10.2.0
cat > $B/src.c
$C/x86_64/gcc-cross-arm/usr/bin/arm-openbmc-linux-gnueabi/arm-openbmc-linux-gnueabi-gcc -marm -mcpu=arm1176jz-s \
  -mfloat-abi=soft --sysroot=$C/arm1176jzs/glibc -isystem $C/arm1176jzs/linux-libc-headers/usr/include \
  -I$C/arm1176jzs/libreimu/usr/include -I$C/arm1176jzs/dtc/usr/include -L$C/arm1176jzs/libreimu/usr/lib \
  -L$C/arm1176jzs/dtc/usr/lib -B$B/bin/ -B$G -L$G -L$C/arm1176jzs/libgcc/lib -O2 -o $B/out $B/src.c %s
cat $B/out"""


def cross(src, args):
    """an ARM binary built with the A/B build's cross compiler (docker)"""
    r = subprocess.run(["docker", "exec", "-i", DOCKER, "sh", "-c", XCC % args], input=src, capture_output=True,
                       timeout=300)
    if r.returncode or not r.stdout.startswith(b"\x7fELF"):
        print("    can't build: " + r.stderr.decode(errors="replace")[-300:])
        return None
    return r.stdout


def docker_file(path):
    r = subprocess.run(["docker", "exec", DOCKER, "cat", path], capture_output=True, timeout=120)
    return r.stdout if r.returncode == 0 and r.stdout else None


def old_rootfs_file(path):
    """a file of the A/B image before the fixes of 2026-10-23/24 (OLD_IMAGE,
    default WORKDIR/../old/ab.mtd), unpacked in the build container"""
    img = os.environ.get("OLD_IMAGE", os.path.join(W, "..", "old", "ab.mtd"))
    if not os.path.exists(img):
        return None
    d = "/tmp/reg-oldrofs"
    if not docker_file(d + "/etc/os-release"):
        with open(img, "rb") as f:
            f.seek(0x700000)
            sq = f.read(0x1500000 - 4096)
        subprocess.run(["docker", "exec", "-i", DOCKER, "sh", "-c", "cat > /tmp/reg-old.sqfs && rm -rf %s && "
                        "unsquashfs -no-progress -d %s /tmp/reg-old.sqfs >/dev/null; rm -f /tmp/reg-old.sqfs" % (d, d)],
                       input=sq, capture_output=True, timeout=300)
    return docker_file(d + path)


HWI_SRC = "meta-mcst/meta-common/recipes-mcst/hwmon-instantiator/files/hwmon-instantiator.c"


# ------------------------------------------------------------------ fruid
def fruid_run(b, binary, tag):
    """mcst-fruid once, the open() flags of /dev/mtd* logged; (rc, opens, crashed)"""
    n0 = b.out("dmesg | wc -l").strip()
    b.ssh("rm -f /tmp/ol /var/volatile/motherboard_info.xml /tmp/fru.xml /core* /tmp/core*")
    r = b.ssh("cd /tmp && OPENLOG=/tmp/ol LD_PRELOAD=/tmp/openlog.so FRU_XML=/tmp/fru.xml %s >/tmp/fruid-%s.log 2>&1; echo $?"
              % (binary, tag), timeout=180)
    rc = int((r.stdout.strip().splitlines() or ["-1"])[-1])
    opens = b.out("sort -u /tmp/ol 2>/dev/null").split("\n")
    opens = [o for o in opens if o.strip()]
    seg = b.out("dmesg | tail -n +%d | grep -iE 'segfault|unhandled|Alignment trap' | head -3" % (int(n0 or 0) + 1)).strip()
    return rc, opens, seg or (rc >= 128 and "killed by signal %d" % (rc - 128))


def eeprom(b, data=None):
    """the FRU EEPROM (i2c-3 0x57): read its first 256 bytes, or write data there"""
    b.ssh("echo 24c128 0x57 > /sys/bus/i2c/devices/i2c-3/new_device 2>/dev/null; sleep 1")
    if data is None:
        out = b.ssh("dd if=/sys/bus/i2c/devices/3-0057/eeprom bs=256 count=1 2>/dev/null | od -An -tx1 -v").stdout
        d = bytes(int(x, 16) for x in out.split())
    else:
        put(b, data, "/tmp/ee.bin")
        b.ssh("dd if=/tmp/ee.bin of=/sys/bus/i2c/devices/3-0057/eeprom bs=256 count=1 conv=notrunc 2>/dev/null")
        d = data
    b.ssh("echo 0x57 > /sys/bus/i2c/devices/i2c-3/delete_device 2>/dev/null")
    return d


def g_fruid():
    """[17] mcst-fruid: the PNS flash only read, an EEPROM that fails the header
    check (all zeros) no crash; red: the mcst-fruid of the image before the fixes"""
    import test_fru_sensors_qemu as F
    so = cross(open(os.path.join(HERE, "openlog.c"), "rb").read(), "-shared -fPIC -ldl")
    b = F.Platform("reg-fruid", image("reg-fruid")).start()
    try:
        if not check(b.wait_ssh(600), "up") or not check(so is not None and put(b, so, "/tmp/openlog.so"), "open() logger"):
            return
        good = eeprom(b)
        check(good[:1] == b"\x01", "the FRU EEPROM holds the board's FRU ID (header %s)" % good[:8].hex())
        bins = [("image", "/usr/sbin/mcst-fruid")]
        if RED:
            old = old_rootfs_file("/usr/sbin/mcst-fruid")
            if old and put(b, old, "/tmp/mcst-fruid.old"):
                b.ssh("chmod 755 /tmp/mcst-fruid.old")
                bins.insert(0, ("old", "/tmp/mcst-fruid.old"))
            else:
                red(True, "[17] the old mcst-fruid (OLD_IMAGE)")
        for tag, path in bins:
            res = []
            eeprom(b, good)
            rc, opens, crash = fruid_run(b, path, tag + "-good")
            ro = bool(opens) and all(o.endswith("O_RDONLY") for o in opens)
            res.append((ro, "[17] %s mcst-fruid, the board's FRU ID: rc %d, opens of the flash %s" % (tag, rc, opens or "none")))
            eeprom(b, bytes(256))
            rc2, _, crash2 = fruid_run(b, path, tag + "-bad")
            res.append((not crash2 and rc2 < 128, "[17] %s mcst-fruid, an EEPROM that fails the header check: rc %d, %s"
                        % (tag, rc2, crash2 or "no crash")))
            print("    " + b.out("tail -3 /tmp/fruid-%s-bad.log" % tag).strip().replace("\n", " | ")[-200:])
            for k, (ok, what) in enumerate(res):
                if tag != "old":
                    check(ok, what)
                elif k == 0:
                    red(ok, what)
                else:
                    # the old get_fruid() fcloses an unset FILE *: what it holds
                    # depends on the registers/stack left by main() (0 here,
                    # so nothing happens); the logger would report any other
                    print("    info (old, not a red: undefined, here harmless) " + what)
        eeprom(b, good)
    finally:
        b.stop()


# ------------------------------------------------------------------ hwmon
def hwi_conf(b, dev):
    return b.out("cat $(find /etc/default/obmc/hwmon/ -name '%s.conf') 2>/dev/null" % dev)


def restart_instantiator(b):
    b.ssh("systemctl stop hwmon-instantiator; systemctl reset-failed 'xyz.openbmc_project.Hwmon@*' hwmon-instantiator; "
          "systemctl start hwmon-instantiator", timeout=300)


def g_hwmon():
    """[12] hwmon-instantiator: the lm96163 inputs named by the device tree's
    temp1_label props; [21] phosphor-hwmon: a PCS read error never a value.
    Red (REG_RED=1): today's hwmon-instantiator source with only bug 12 put
    back (bug 11's fix kept), cross-built; phosphor-hwmon-readd built with
    only negative-errno-on-fail at its meson default, true (REG_HWMON_ERRNO=file, from
    the build container; see REGRESSIONS.md for how it was built)"""
    import test_fru_sensors_qemu as F
    b = F.Platform("reg-hwmon", image("reg-hwmon")).start()
    try:
        if not check(b.wait_ssh(600), "up, platform off"):
            return
        F.fast(b)
        F.power_on(b)
        lm = {"FAN_Control_X38_X39_X40_Internal_Temp", "CPU_FAN_Control_X37_Internal_Temp"}

        def lm_state(tag):
            got = b.wait(lambda: lm <= set(b.sensors()), 60, 3)
            c = hwi_conf(b, "2-004c")
            return bool(got) and "LABEL_temp1=" in c, "[12] %s: lm96163 sensors %s, 2-004c.conf %r" % (
                tag, sorted(lm & set(b.sensors())), c.strip().replace("\n", " ")[:120])

        if RED:
            src = open(os.path.join(HERE, "..", "..", "..", HWI_SRC), "rb").read() if os.path.exists(
                os.path.join(HERE, "..", "..", "..", HWI_SRC)) else docker_file("/work/openbmc/" + HWI_SRC)
            mut = src.replace(b"    if (!s_sensors)\n", b"    if (0) /* bug 12 back */\n") if src else None
            for tag, s in (("today's source, cross-built (the control)", src), ("only bug 12 put back", mut)):
                bn = cross(s, "-lfdt -lreimu -lreimu_fdt") if s and not (s is mut and mut == src) else None
                if not bn or not put(b, bn, "/tmp/hwi"):
                    red(True, "[12] build: " + tag)
                    continue
                b.ssh("chmod 755 /tmp/hwi; mount --bind /tmp/hwi /sbin/hwmon-instantiator")
                restart_instantiator(b)
                ok, what = lm_state(tag)
                (red(ok, what) if s is mut else check(ok, what))
                b.ssh("umount /sbin/hwmon-instantiator")
            restart_instantiator(b)
        ok, what = lm_state("the image's hwmon-instantiator")
        check(ok, what)

        def errno_state(tag, strip_rcs=False):
            if strip_rcs:
                b.ssh("for f in $(find /etc/default/obmc/hwmon/ -name '5-0021.conf'); do sed -i '/^REMOVERCS=/d' $f; done")
            # every phosphor-hwmon again: the binary bind-mounted now is the one that runs
            b.ssh("systemctl restart 'xyz.openbmc_project.Hwmon@*'", timeout=120)
            b.wait(lambda: "CPU0_Cores_2_3" in b.sensors(), 60, 3)
            before = b.sensors().get("CPU0_Cores_2_3")
            b.temperature("pcs1", 0)  # the PCS reads 0: l_pcs_i2c says EIO
            seen = set()
            end = time.time() + 30
            while time.time() < end:
                seen.add(b.sensors().get("CPU0_Cores_2_3"))
                time.sleep(2)
            b.temperature("pcs1", 2000)
            neg = sorted(v for v in seen if v is not None and v < 0)
            exe = b.out("for p in $(pidof phosphor-hwmon-readd); do md5sum < /proc/$p/exe; done | cut -c1-8 | sort -u").split()
            return not neg, "[21] %s: CPU0_Cores_2_3 %s, while its read fails %s (readd md5 %s)" % (
                tag, before, sorted(seen, key=lambda v: (v is None, v or 0)), exe)

        ex = os.environ.get("REG_HWMON_ERRNO")
        if RED and ex and os.path.exists(ex) and put(b, open(ex, "rb").read(), "/tmp/readd"):
            b.ssh("chmod 755 /tmp/readd; mount --bind /tmp/readd /usr/bin/phosphor-hwmon-readd")
            ok, what = errno_state("phosphor-hwmon with negative-errno-on-fail at its default, REMOVERCS as written")
            red(ok, what)
            ok, what = errno_state("phosphor-hwmon with negative-errno-on-fail at its default, no REMOVERCS", True)
            red(ok, what)
            # a binary being run can't be unmounted
            b.ssh("systemctl stop 'xyz.openbmc_project.Hwmon@*'; umount /usr/bin/phosphor-hwmon-readd", timeout=120)
            restart_instantiator(b)
        elif RED:
            red(True, "[21] REG_HWMON_ERRNO (the errno build of phosphor-hwmon-readd)")
        ok, what = errno_state("the image's phosphor-hwmon, REMOVERCS as written")
        check(ok, what)
        ok, what = errno_state("the image's phosphor-hwmon, no REMOVERCS", True)
        check(ok, what)
    finally:
        b.stop()


# ------------------------------------------------------------------ pns
SWITCH_FIX = """    [ -d $g ] || echo $n > /sys/class/gpio/export 2>/dev/null
    echo out > $g/direction 2>/dev/null
    echo $1 > $g/value 2>/dev/null
"""
SWITCH_BUG = """    gpiowrite $n $1
"""


def o0(b):
    return b.out(". /libexec/gpio-funcs; cat /sys/class/gpio/gpio$(gpio2num $GPIO_SPI_CONNECT)/value 2>/dev/null").strip()


def failed_job(b, tool):
    """a job stopped with the flash on the BMC's side (SIGTERM during
    with-chip: its EXIT trap must switch the flash back); O0 during the job,
    right after it and 20 s later"""
    r = b.ssh("%s with-chip sleep 8 & p=$!; sleep 4; . /libexec/gpio-funcs; "
              "echo during $(cat /sys/class/gpio/gpio$(gpio2num $GPIO_SPI_CONNECT)/value); "
              "kill -TERM $p; wait $p; echo rc $?" % tool, timeout=120)
    a = o0(b)
    time.sleep(20)
    out = (r.stdout + r.stderr).strip()
    during = (re.search(r"during (\d)", out) or [None, "?"])[1]
    return during, a, o0(b), out.replace("\n", " | ")[-120:]


def g_pns():
    """[39] reimu-pns-flash gives the PNS flash back to the host when a job
    fails (its EXIT trap). Red (REG_RED=1): the image's tool with switch()
    writing the GPIO through gpio-funcs' gpiowrite again (whose
    gpioavailable's bare "return" in an EXIT trap returns the failing exit
    status: nothing written)"""
    b = T.Bmc("reg-pns", image("reg-pns")).start()
    try:
        if not check(b.wait_ssh(600), "up"):
            return
        if RED:
            t = b.out("cat /usr/sbin/reimu-pns-flash", timeout=60)
            if SWITCH_FIX in t and put(b, t.replace(SWITCH_FIX, SWITCH_BUG).encode(), "/tmp/reimu-pns-flash.trap"):
                b.ssh("chmod 755 /tmp/reimu-pns-flash.trap")
                d, a, c, o = failed_job(b, "/tmp/reimu-pns-flash.trap")
                red(d != "1" or (a == "0" and c == "0"), "[39] gpiowrite in the EXIT trap: O0 during the job %s, after it %s, "
                    "20 s later %s (%s)" % (d, a, c, o))
                b.ssh("/usr/sbin/reimu-pns-flash disconnect >/dev/null 2>&1")
            else:
                red(True, "[39] the switch() of reimu-pns-flash as expected (mutant)")
        d, a, c, o = failed_job(b, "/usr/sbin/reimu-pns-flash")
        check(d == "1" and a == "0" and c == "0", "[39] O0 during the job %s, after SIGTERM %s, 20 s later %s (%s)" % (d, a, c, o))
    finally:
        b.stop()


if __name__ == "__main__":
    t0 = time.time()
    for g in GROUPS:
        print("== %s (%s%s)" % (g, os.path.basename(IMAGE), ", red runs" if RED else ""), flush=True)
        t = time.time()
        {"base": g_base, "kexec": g_kexec, "platform": g_platform, "fruid": g_fruid, "hwmon": g_hwmon, "pns": g_pns}[g]()
        print("   (%d s)" % (time.time() - t), flush=True)
    print("%d failed%s (%d s)" % (len(T.failed), ", %d red runs that passed" % len(red_missing) if RED else "",
                                  time.time() - t0))
    if T.failed:
        print("FAILED: " + "; ".join(T.failed))
    sys.exit(1 if T.failed or red_missing else 0)
