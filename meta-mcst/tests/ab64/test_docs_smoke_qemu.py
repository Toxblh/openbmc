#!/usr/bin/env python3
"""What the official documentation (docs-ru/reimu.md) promises on the A/B
image in QEMU: things no other test exercises.

    test_docs_smoke_qemu.py WORKDIR [GROUP...]

WORKDIR holds ab.mtd as for test_ab_qemu.py (RQ, AB_SSH, AB_HTTPS too; this
test adds hostfwd for 2022 and 5900 on DOCS_SSH2022, DOCS_VNC). Groups:

  console  ssh -p 2022 attaches to the COM console's tmux session SoL, whatever
           TERM the client has (terminfo in the image: few entries); Ctrl+B D
           detaches; "tmux attach" from a normal session and back to its shell
  sollog   the SoL log's rotation: nothing written to the flash while the log
           doesn't change; current.gz refreshed when it did, no more often
           than SOL_GZ_EVERY seconds
  rtc      (a ds1338 stands in for the platform's RTC) a BMC that lost its
           clock with the platform off reads the RTC once at platform power-on
           (reimu-fru power), then the client goes again; a right clock: no read
  fan      phosphor-fan-presence-tach is not in the image (reimu-fand owns the
           fans), the power-on target doesn't need it
  smoke    VNC on 5900 and its password; server_uid, enclosure_identify,
           IndicatorLED; setled; checkalerts; last_power_state; ip-fallback;
           hwclock; auto_power_on; revert-to-mfg (released: nothing; a pressed
           reading in a sandbox: wipes the cow and reboots, in the sandbox);
           fw_setenv of the MACs in both environment copies; web UI login
"""
import base64, json, os, pty, re, select, socket, ssl, struct, subprocess, sys, termios, time, urllib.request, urllib.error, fcntl

SSH2022, VNC = int(os.environ.get("DOCS_SSH2022", 3324)), int(os.environ.get("DOCS_VNC", 3359))
os.environ["AB_NETDEV_EXTRA"] = os.environ.get("AB_NETDEV_EXTRA", "") + \
    ",hostfwd=tcp:127.0.0.1:%d-:2022,hostfwd=tcp:127.0.0.1:%d-:5900" % (SSH2022, VNC)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ab_qemu as T  # noqa: E402  (takes WORKDIR from sys.argv[1])
from test_ab_qemu import check, fresh  # noqa: E402

ONLY = T.ONLY


# ------------------------------------------------------------------ a terminal
class Term:
    """ssh -tt on a pty: what an interactive user gets"""

    def __init__(self, port, term, *cmd):
        c, env = T.ssh_cmd()
        c = ["ssh", "-tt", "-p", str(port)] + [x for x in c[3:] if x != "root@127.0.0.1"] + ["root@127.0.0.1"] + list(cmd)
        env = dict(env, TERM=term)
        self.m, s = pty.openpty()
        fcntl.ioctl(s, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
        self.p = subprocess.Popen(c, stdin=s, stdout=s, stderr=s, env=env, start_new_session=True)
        os.close(s)
        self.out = b""

    def expect(self, rx, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(rx.encode(), self.out)
            if m:
                return m
            r, _, _ = select.select([self.m], [], [], 0.5)
            if r:
                try:
                    d = os.read(self.m, 65536)
                except OSError:
                    d = b""
                if not d:
                    return re.search(rx.encode(), self.out)
                self.out += d
                # the terminal's answers tmux asks for are not needed here
        return None

    def send(self, s):
        os.write(self.m, s.encode())

    def text(self):
        return re.sub(rb"\x1b[\[\]()?>=0-9;]*[a-zA-Z]?", b"", self.out).decode(errors="replace")

    def close(self):
        try:
            self.p.kill()
            self.p.wait()
            os.close(self.m)
        except OSError:
            pass


def t_console(b):
    for term in ("xterm-256color", "xterm", "putty", "screen", "alacritty"):
        t = Term(SSH2022, term)
        ok = t.expect(r"\[SoL\]", 30)
        check(ok is not None, "ssh -p 2022, TERM=%s: attached to the SoL session%s"
              % (term, "" if ok else " (%s)" % t.text().strip()[-100:]))
        if ok:
            t.send("\x02d")
            check(t.expect(r"detached", 10) is not None, "  Ctrl+B D: detached")
        t.close()
    t = Term(T.SSH, "xterm-256color")
    check(t.expect(r"[#$] ", 30) is not None, "ssh on 22: a shell")
    t.send("echo TERM=$TERM.\n")
    check(t.expect(r"TERM=xterm-256color\.", 10) is not None, "a TERM the image knows is left as it is (%s)" % t.text()[-60:].strip())
    t.send("tmux attach\n")
    ok = t.expect(r"\[SoL\]", 15)
    check(ok is not None, "tmux attach from the shell: the SoL session%s" % ("" if ok else " (%s)" % t.text().strip()[-100:]))
    t.send("\x02d")
    t.expect(r"detached", 10)
    t.send("echo back-$((40+2))\n")
    check(t.expect(r"back-42", 10) is not None, "Ctrl+B D: back in the shell")
    t.close()
    t = Term(T.SSH, "alacritty")
    t.expect(r"[#$] ", 30)
    t.send("tmux attach\n")
    check(t.expect(r"\[SoL\]", 15) is not None, "tmux attach with a TERM the image doesn't know (alacritty)")
    t.close()


# ------------------------------------------------------------------ SoL log
def t_sollog(b):
    gz, st = "/usr/log/current.gz", "/var/lib/logrotate.status"
    # (one pass of whatever the unit runs: a oneshot, or a loop with its drop-in)
    run = "systemctl restart logrotate-sol.service; sleep 2"
    # (the clock set here stays: no NTP; QEMU boots in 1970)
    b.ssh("systemctl stop logrotate-sol.timer systemd-timesyncd; date -s @1700000000 >/dev/null; rm -f %s; sleep 1" % gz)
    check(b.ssh(run).returncode == 0, "logrotate-sol.service runs")
    stamp = lambda f: b.out("stat -c %%Y.%%i %s 2>/dev/null" % f).strip()  # noqa: E731
    g0, s0 = stamp(gz), stamp(st)
    check(bool(g0), "current.gz there (%s)" % g0)
    time.sleep(2)
    b.ssh(run)
    g1, s1 = stamp(gz), stamp(st)
    check(g1 == g0, "the log didn't change: current.gz not written again (%s, %s)" % (g0, g1))
    check(s1 == s0, "the rwfs' logrotate state not written either (%s, %s)" % (s0, s1))
    b.ssh("echo sol-line-1 >> /var/log/sol.log")
    b.ssh(run)
    g2 = stamp(gz)
    check(g2 == g1, "the log changed right after a refresh: not yet (SOL_GZ_EVERY)")
    b.ssh("touch -d '@%s' %s" % (int(b.out("date +%s").strip()) - 3600, gz))
    b.ssh("touch /var/log/sol.log")
    b.ssh(run)
    check("sol-line-1" in b.out("gunzip -c %s" % gz), "older than SOL_GZ_EVERY and the log changed: refreshed, the new line in it")
    check(not b.out("ls %s.tmp 2>/dev/null" % gz).strip(), "no temporary left")
    b.ssh("systemctl start logrotate-sol.timer")


# ------------------------------------------------------------------ RTC at power-on
class RtcBmc(T.Bmc):
    def start(self):
        popen = subprocess.Popen

        def with_rtc(args, **kw):
            if args[0] == "qemu-system-arm":
                args = args + ["-device", "ds1338,bus=aspeed.i2c.bus.2,address=0x6f,id=rtc"]
            return popen(args, **kw)
        subprocess.Popen = with_rtc
        try:
            return super().start()
        finally:
            subprocess.Popen = popen


def t_rtc():
    b = RtcBmc("docs-rtc", fresh("docs-rtc")).start()
    try:
        if not check(b.wait_ssh(), "boots, ssh"):
            return
        fru = "FRU_SETTLE=0 FRU_TRIES=1 /usr/libexec/reimu-fru power"
        xml = "/var/volatile/motherboard_info.xml"
        b.ssh("systemctl stop systemd-timesyncd")  # NTP would set the clock back
        b.ssh("touch %s; [ -s %s ] || echo '<fruinfo></fruinfo>' > %s" % (xml, xml, xml))
        # script level: was the RTC read, how
        b.ssh("cp /libexec/rtcread /tmp/rtcread.real; printf '#!/bin/sh\\necho \"$@\" >> /tmp/rtc.calls\\n' > /libexec/rtcread; chmod +x /libexec/rtcread")
        b.ssh("date -s @1700000000 >/dev/null; " + fru)  # (QEMU boots in 1970)
        check(not b.out("cat /tmp/rtc.calls 2>/dev/null").strip(), "the clock is right: the RTC isn't touched")
        b.ssh("date -s @1000000 >/dev/null; " + fru)
        calls = b.out("cat /tmp/rtc.calls 2>/dev/null").strip()
        check(calls.count("\n") == 0 and bool(calls), "the clock before 2010, FRU ID already read: the RTC read once (%r)" % calls)
        # the real rtcread on the stand-in (mcp794xx's read fails on a ds1338:
        # the model from a fake device tree lookup)
        b.ssh("cp /tmp/rtcread.real /libexec/rtcread; printf '#!/bin/sh\\necho ds1338:6f:2\\n' > /libexec/find_i2c_device-platform; chmod +x /libexec/find_i2c_device-platform")
        b.ssh("date -s @1000000 >/dev/null; " + fru)
        t = int(b.out("date +%s").strip())
        check(t > 1262304000, "the real rtcread: the system clock from the RTC (%d)" % t)
        client = b.out("ls -d /sys/bus/i2c/devices/2-006f 2>/dev/null").strip()
        check(not client, "the client deleted after (%s)" % (client or "gone"))
    finally:
        b.stop()


# ------------------------------------------------------------------ fan presence
def t_fan(b):
    check(not b.out("ls /usr/bin/phosphor-fan-presence-tach /lib/systemd/system/phosphor-fan-presence-tach@.service 2>/dev/null").strip(),
          "phosphor-fan-presence-tach not in the image")
    deps = b.out("systemctl list-dependencies obmc-chassis-poweron@0.target --no-pager")
    check("fan-presence" not in deps, "obmc-chassis-poweron@0 doesn't require it")
    check(b.ssh("systemctl is-enabled reimu-fand").returncode == 0, "reimu-fand owns the fans")


# ------------------------------------------------------------------ VNC's DES
_PC1 = [57,49,41,33,25,17,9,1,58,50,42,34,26,18,10,2,59,51,43,35,27,19,11,3,60,52,44,36,63,55,47,39,31,23,15,7,62,54,46,38,30,22,14,6,61,53,45,37,29,21,13,5,28,20,12,4]  # noqa: E231,E501
_PC2 = [14,17,11,24,1,5,3,28,15,6,21,10,23,19,12,4,26,8,16,7,27,20,13,2,41,52,31,37,47,55,30,40,51,45,33,48,44,49,39,56,34,53,46,42,50,36,29,32]  # noqa: E231,E501
_IP = [58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6,64,56,48,40,32,24,16,8,57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7]  # noqa: E231,E501
_E = [32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,12,13,14,15,16,17,16,17,18,19,20,21,20,21,22,23,24,25,24,25,26,27,28,29,28,29,30,31,32,1]  # noqa: E231,E501
_P = [16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,2,8,24,14,32,27,3,9,19,13,30,6,22,11,4,25]  # noqa: E231,E501
_S = [
    [14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13],  # noqa: E231,E501
    [15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9],  # noqa: E231,E501
    [10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12],  # noqa: E231,E501
    [7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14],  # noqa: E231,E501
    [2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3],  # noqa: E231,E501
    [12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13],  # noqa: E231,E501
    [4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12],  # noqa: E231,E501
    [13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11],  # noqa: E231,E501
]


def _perm(v, n, table):
    r = 0
    for t in table:
        r = r << 1 | (v >> (n - t)) & 1
    return r


def des_ecb(key, block):
    k = _perm(int.from_bytes(key, "big"), 64, _PC1)
    c, d, subs = k >> 28, k & 0xfffffff, []
    for s in (1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1):
        c = (c << s | c >> (28 - s)) & 0xfffffff
        d = (d << s | d >> (28 - s)) & 0xfffffff
        subs.append(_perm(c << 28 | d, 56, _PC2))
    v = _perm(int.from_bytes(block, "big"), 64, _IP)
    l, r = v >> 32, v & 0xffffffff
    for sk in subs:
        x = _perm(r, 32, _E) ^ sk
        o = 0
        for i in range(8):
            six = x >> (42 - 6 * i) & 63
            o = o << 4 | _S[i][((six >> 4) & 2 | six & 1) * 16 + (six >> 1 & 15)]
        l, r = r, l ^ _perm(o, 32, _P)
    out = r << 32 | l
    fp = [0] * 64
    for i, t in enumerate(_IP):
        fp[t - 1] = i + 1
    return _perm(out, 64, fp).to_bytes(8, "big")


def vnc_auth(password):
    """the result of the RFB VNC authentication with this password: True/False"""
    s = socket.create_connection(("127.0.0.1", VNC), timeout=15)
    f = s.makefile("rb")
    ver = f.read(12)
    if not ver.startswith(b"RFB "):
        return "no RFB banner: %r" % ver
    s.sendall(b"RFB 003.008\n")
    n = f.read(1)[0]
    types = list(f.read(n))
    if 2 not in types:
        return "no VNC authentication offered: %s" % types
    s.sendall(bytes([2]))
    ch = f.read(16)
    key = bytes(int("{:08b}".format(c)[::-1], 2) for c in password.encode().ljust(8, b"\0")[:8])
    s.sendall(des_ecb(key, ch[:8]) + des_ecb(key, ch[8:]))
    r = struct.unpack(">I", f.read(4))[0]
    s.close()
    return r == 0


def web(method, path, data=None, headers=None):
    ctx = ssl.create_default_context()
    ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE
    rq = urllib.request.Request("https://127.0.0.1:%d%s" % (T.HTTPS, path), method=method,
                                data=None if data is None else json.dumps(data).encode(),
                                headers=dict({"Content-Type": "application/json"}, **(headers or {})))
    try:
        with urllib.request.urlopen(rq, context=ctx, timeout=60) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


LEDS = "xyz.openbmc_project.LED.GroupManager /xyz/openbmc_project/led/groups/enclosure_identify xyz.openbmc_project.Led.Group"


def t_smoke(b):
    print("  -- VNC (reimu.md:254-256)", flush=True)
    time.sleep(2)
    if check(b.ssh("netstat -ltn | grep -q ':5900 '").returncode == 0, "obmc-ikvm listens on 5900"):
        r = vnc_auth("0penBmc")
        check(r is True, "the default password 0penBmc is accepted (%s)" % r)
        r = vnc_auth("wrong")
        check(r is False, "a wrong one isn't (%s)" % r)

    print("  -- UID (reimu.md:312-319, 329-331, 375-377)", flush=True)
    rc = lambda c: b.ssh(c).returncode  # noqa: E731
    rc("server_uid off")
    check(rc("server_uid query") == 0, "server_uid query: 0 when off")
    check("OFF" in b.out("server_uid show"), "server_uid show says OFF")
    rc("server_uid on")
    check(rc("server_uid query") == 1, "server_uid on, query: 1")
    check("true" in b.out("busctl get-property " + LEDS + " Asserted"), "enclosure_identify Asserted true")
    rc("server_uid off")
    rc("busctl set-property " + LEDS + " Asserted b true")
    time.sleep(1)
    check(rc("server_uid query") == 1, "enclosure_identify set over D-Bus: the UID on")
    st, _ = T.redfish("PATCH", "/redfish/v1/Systems/system", {"IndicatorLED": "Off"})
    time.sleep(1)
    check(st in (200, 204) and rc("server_uid query") == 0, "Redfish IndicatorLED Off: the UID off (%s)" % st)
    st, _ = T.redfish("PATCH", "/redfish/v1/Systems/system", {"IndicatorLED": "Lit"})
    time.sleep(1)
    st2, d = T.redfish("GET", "/redfish/v1/Systems/system")
    check(st in (200, 204) and rc("server_uid query") == 1 and d.get("IndicatorLED") == "Lit",
          "Redfish IndicatorLED Lit: the UID on, reads Lit (%s %s)" % (st, d.get("IndicatorLED")))
    rc("server_uid off")
    bad = []
    for i in range(5):  # the set must take effect each time, not most times
        rc("server_uid on")
        if rc("server_uid query") != 1:
            bad.append("on #%d" % i)
        rc("server_uid off")
        if rc("server_uid query") != 0:
            bad.append("off #%d" % i)
    check(not bad, "server_uid on/off five times: each took effect (%s)" % (bad or "yes"))

    print("  -- setled, checkalerts (reimu.md:409-424)", flush=True)
    check("<ledname>" in b.out("setled --help"), "setled --help")
    trig = "/sys/class/leds/platform:red:alarm/trigger"
    rc("setled alarm on")
    check("[default-on]" in b.out("cat " + trig), "setled alarm on")
    rc("setled alarm off")
    check("[none]" in b.out("cat " + trig), "setled alarm off")
    o = b.ssh("checkalerts")
    check(o.returncode == 0 and "System power" in o.stdout and "failed to query" not in o.stdout, "checkalerts: the signals, none unreadable")

    print("  -- power settings (reimu.md:296-305)", flush=True)
    apo = b.out(". /etc/auto_power_on; echo $AUTO_POWER_ON $POWER_ON_DELAY $PS_DISCHARGE_TIME").split()
    check(len(apo) == 3 and apo[0] in ("off", "on", "auto"), "auto_power_on: %s" % apo)
    check(b.out("systemctl is-active allow_host_boot").strip() == "active", "allow_host_boot ran")
    check(b.out("cat /var/lib/reimu/last_power_state").strip() in ("0", "1"), "last_power_state 0 or 1")

    print("  -- ip-fallback (reimu.md:129)", flush=True)
    to = b.out("grep FallbackTimeout /etc/ip-fallback/fallback-eth0.conf | cut -d= -f2").strip()
    check(to.isdigit(), "fallback-eth0.conf: FallbackTimeout %s" % to)
    check(rc("[ ! -e /sys/class/net/eth1 ]") == 0, "no eth1 (bug 68)")
    check("exited" in b.out("systemctl show -p SubState ip-fallback") or b.out("systemctl is-active ip-fallback").strip() == "active", "ip-fallback ran without eth1")
    check(rc("journalctl -b -u ip-fallback --no-pager | grep -q 'eth1'") != 0, "ip-fallback: nothing about eth1")
    check("192.168.1.1" not in b.out("ip addr show eth0"), "eth0, with DHCP: no fallback address")

    print("  -- hwclock (reimu.md:156)", flush=True)
    check(rc("for i in /dev/rtc?; do hwclock --systohc --rtc $i || exit 1; done") == 0, "the documented hwclock --systohc loop")
    check(rc("hwclock -r -f /dev/rtc0") == 0, "hwclock -r")

    print("  -- revert-to-mfg (reimu.md:170-175)", flush=True)
    val = int(b.out("devmem 0x1e780078").strip(), 16)
    check(val >> 11 & 1 == 1, "the UID button reads released: bit 11 of 0x1e780078 set (%#x)" % val)
    t0 = time.time()
    r = b.ssh("/libexec/revert-to-mfg")
    check(r.returncode == 0 and time.time() - t0 < 5 and "clean RW" not in r.stderr, "released: exits at once, nothing asked")
    check(rc("test -d /run/initramfs/rw/cow") == 0, "the cow is there")
    sandbox = r"""
    set -e
    S=/tmp/sb; rm -rf $S; mkdir -p $S/bin $S/cow; touch $S/cow/settings
    printf '#!/bin/sh\necho 0x00009000\n' > $S/bin/devmem            # bit 11 clear: pressed
    printf '#!/bin/sh\nexit 0\n' > $S/bin/sleep
    printf '#!/bin/sh\ntouch %s/rebooted\n' $S > $S/bin/reboot
    chmod +x $S/bin/*
    sed "s#/run/initramfs/rw/cow/#$S/cow/#" /libexec/revert-to-mfg > $S/revert
    PATH=$S/bin:$PATH bash $S/revert 2>&1 || true
    [ -e $S/cow ] && echo COW-THERE
    [ -e $S/rebooted ] && echo REBOOTED
    """
    o = b.out(sandbox, timeout=60)
    check("COW-THERE" not in o and "REBOOTED" in o, "pressed, in a sandbox: the cow wiped and a reboot (%s)" % o.strip().replace("\n", " | ")[-120:])
    check(rc("test -d /run/initramfs/rw/cow") == 0, "the real cow untouched")

    print("  -- web UI login (reimu.md)", flush=True)
    st, page = web("GET", "/")
    check(st == 200 and b"<" in page, "the web UI's page (%s)" % st)
    st, _ = web("POST", "/login", {"data": ["root", "wrongpassword"]})
    check(st == 401, "login with a wrong password: 401 (%s)" % st)
    st, d = web("POST", "/login", {"data": ["root", "0penBmc"]})
    check(st == 200 and b"logged in" in d, "login as the web UI does it: a session (%s %s)" % (st, d[:60]))

    print("  -- MAC addresses (reimu.md:179-183)", flush=True)
    check(rc("fw_setenv ethaddr; fw_setenv ethaddr; fw_setenv eth1addr; fw_setenv eth1addr; sync") == 0, "the documented fw_setenv lines")
    check(rc("fw_printenv ethaddr") != 0 and rc("fw_printenv eth1addr") != 0, "fw_printenv: both gone")
    time.sleep(1)
    with open(b.img, "rb") as f:
        f.seek(T.LAYOUT["u-boot-env"][0])
        env = f.read(T.LAYOUT["u-boot-env"][1])
    check(b"ethaddr=" not in env and b"eth1addr=" not in env, "neither in the two environment copies on the flash")


def main():
    want = lambda g: not ONLY or g in ONLY  # noqa: E731
    try:
        if any(want(g) for g in ("console", "sollog", "fan", "smoke")):
            b = T.Bmc("docs", fresh("docs")).start()
            try:
                if check(b.wait_ssh(), "boots, ssh"):
                    for name, fn in (("console", t_console), ("sollog", t_sollog), ("fan", t_fan), ("smoke", t_smoke)):
                        if want(name):
                            print("== " + name, flush=True)
                            try:
                                fn(b)
                            except Exception as e:  # noqa: BLE001
                                check(False, "%s crashed: %r" % (name, e))
            finally:
                b.stop()
        if want("rtc"):
            print("== rtc", flush=True)
            t_rtc()
    except Exception as e:  # noqa: BLE001
        check(False, "crashed: %r" % e)
    print("\n%d failed" % len(T.failed) + "".join("\n  " + f for f in T.failed))
    sys.exit(1 if T.failed else 0)


if __name__ == "__main__":
    main()
