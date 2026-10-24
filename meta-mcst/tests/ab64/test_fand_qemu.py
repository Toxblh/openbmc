#!/usr/bin/env python3
"""reimu-fand (the fan control) on the A/B image in QEMU.

    test_fand_qemu.py WORKDIR [TEST...]

WORKDIR holds ab.mtd as for test_ab_qemu.py (RQ, AB_SSH, AB_HTTPS too).
FAND_MINUTES: length of the memory run (default 4). Tests (all by default):

  bare   no fan chips, no sensors (QEMU as it is): the service starts, host
         off -> idle; host on -> after SETTLE_SEC it fails safe (every zone
         100 %, alarm, fan fault LED), no failed units; SIGKILL, SIGABRT
         (as the watchdog), stop/start: ExecStopPost runs, systemd brings it
         back; memory and fds flat over a fast-polling run
  board  the board's chips emulated: the PCS of CPU 0 (BMC i2c-5,
         0x20..0x26) by tmp105s as in test_overheatd_qemu.py; both lm96163
         (i2c-6 and i2c-2, 0x4c), both EMC2305 (i2c-2 0x2c, 0x2e) and the
         VRMs (i2c-6 0x44..0x46) by at24c EEPROMs holding their registers.
         Host on: 100 % first, then quiet by the curves; the EMC2305
         watchdog on; bmcweb serves the D-Bus state; manual duty, its
         timeout, full mode, curves checked and saved; a sensor reading 0:
         100 %; the CPU fan stopping: alarm, LED, 100 %; SIGKILL: 100 % at
         once (ExecStopPost); host off: one 100 % write, then no I2C;
         HOST_PROBE: off but devices answer -> controlled; memory flat
"""
import json, math, os, re, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ab_qemu as T  # noqa: E402  (takes WORKDIR from sys.argv[1])
from test_ab_qemu import check, fresh  # noqa: E402

MINUTES = float(os.environ.get("FAND_MINUTES", 4))
PCS = list(range(0x20, 0x27))
LED = "/sys/class/leds/platform:red:fanfail/trigger"
SVC = "xyz.openbmc_project.Reimu.Fans"
OBJ = "/xyz/openbmc_project/reimu/fans"
FAST = dict(POLL_MS=200, STALE_SEC=2, SETTLE_SEC=3, START_FULL_SEC=2, SLEW_DOWN=100, MANUAL_TIMEOUT_SEC=10,
            TACH_ALARM_SEC=3, TACH_CLEAR_SEC=5)


# ------------------------------------------------------------------ chips
def lm96163_regs(remote):
    r = bytearray(256)
    r[0xfe], r[0xff] = 0x01, 0x49        # National, LM96163
    r[0x4a], r[0x4d], r[0x03] = 0x2b, 0x17, 0x06  # manual, PWM_FREQ 23, tach on
    r[0x4c] = 0x2e
    r[0x01] = remote
    r[0x46], r[0x47] = 0x8c, 0x0a        # 2700 counts: 2000 rpm
    return r


def emc2305_regs():
    r = bytearray(256)
    r[0xfd], r[0xfe], r[0x20] = 0x34, 0x5d, 0x40
    for n in range(5):
        r[0x30 + 0x10 * n], r[0x32 + 0x10 * n] = 0xff, 0x2b
    return r


def vrm_regs(t):
    r = bytearray(256)
    r[0x8d], r[0x8e] = t & 0xff, 0      # LINEAR11, exponent 0
    return r


CHIPS = [(6, 0x4c, lambda: lm96163_regs(39)), (2, 0x4c, lambda: lm96163_regs(83)),
         (2, 0x2c, emc2305_regs), (2, 0x2e, emc2305_regs)] + [(6, a, lambda: vrm_regs(65)) for a in (0x44, 0x45, 0x46)]


class Bmc(T.Bmc):
    def __init__(self, name, img, chips=False):
        super().__init__(name, img)
        self.chips = chips

    def start(self):
        popen = subprocess.Popen
        extra = []
        if self.chips:
            for a in PCS:
                extra += ["-device", "tmp105,bus=aspeed.i2c.bus.5,address=0x%02x,id=pcs%02x" % (a, a)]
            # 256 bytes of RAM, one address byte: a register file (loaded
            # by load_chips(); a raw drive would be 512 bytes, and with that
            # size QEMU drops address bit 7)
            for bus, addr, regs in CHIPS:
                extra += ["-device", "at24c-eeprom,bus=aspeed.i2c.bus.%d,address=0x%02x,rom-size=256" % (bus, addr)]

        def with_chips(args, **kw):
            if args[0] == "qemu-system-arm":
                args = args + extra
            return popen(args, **kw)
        subprocess.Popen = with_chips
        try:
            return super().start()
        finally:
            subprocess.Popen = popen

    def load_chips(self):
        """the registers of every emulated chip, over I2C from the BMC"""
        cmds = []
        for bus, addr, regs in CHIPS:
            cmds += ["i2cset -f -y %d 0x%02x 0x%02x 0x%02x" % (bus, addr, r, v) for r, v in enumerate(regs()) if v]
        r = self.ssh(" && ".join(cmds), timeout=300)
        return r.returncode == 0

    def ssh(self, cmd, timeout=120, input=None):
        c, env = T.ssh_cmd(cmd)
        return subprocess.run(c, env=env, input=input, stdin=None if input is not None else subprocess.DEVNULL,
                              capture_output=True, text=True, timeout=timeout)

    def qom(self, *sets):
        s = socket.socket(socket.AF_UNIX)
        s.connect("%s/%s.qmp2" % (T.RQ, self.name))
        f = s.makefile("rw")
        f.readline()

        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n")
            f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r:
                    return r
        cmd("qmp_capabilities")
        for path, prop, val in sets:
            r = cmd("qom-set", path=path, property=prop, value=val)
            assert "return" in r, r
        s.close()

    def power(self, on):
        self.qom(("/machine/soc/gpio", "gpioO2", bool(on)))

    def temps(self, **by_addr):
        """temps(t20=50, ...): °C as the PCS reads it (tmp105 T * 256 = 1/8 °C units)"""
        self.qom(*[("/machine/peripheral/pcs" + k[1:], "temperature", math.ceil(v * 1000 / 32))
                   for k, v in by_addr.items()])

    def conf(self, **kv):
        body = "".join("%s=%s\n" % (k, v) for k, v in kv.items())
        self.ssh("cat > /etc/reimu-fand.conf", input=body)

    def restart(self):
        self.ssh("systemctl restart reimu-fand")
        time.sleep(1)

    def state(self):
        r = self.ssh("reimu-fand --status")
        try:
            return json.loads(r.stdout)
        except ValueError:
            return {}

    def wait_state(self, pred, timeout=40):
        end = time.time() + timeout
        s = {}
        while time.time() < end:
            s = self.state()
            try:
                if s and pred(s):
                    return s
            except (KeyError, IndexError, TypeError):
                pass
            time.sleep(1)
        return s

    def reg(self, bus, addr, r):
        o = self.out("i2cget -f -y %d 0x%02x 0x%02x" % (bus, addr, r)).strip()
        return int(o, 16) if o.startswith("0x") else -1

    def setreg(self, bus, addr, r, v):
        self.ssh("i2cset -f -y %d 0x%02x 0x%02x 0x%02x" % (bus, addr, r, v))

    def busctl(self, *args):
        return self.ssh("busctl call %s %s %s %s" % (SVC, OBJ, SVC, " ".join(args)))

    def led(self):
        m = re.search(r"\[([^\]]+)\]", self.out("cat " + LED))
        return m.group(1) if m else "?"

    def pid(self):
        return self.out("systemctl show -p MainPID --value reimu-fand").strip()

    def proc(self, pid):
        st = dict(l.split(":", 1) for l in self.out("cat /proc/%s/status" % pid).splitlines() if ":" in l)
        kb = lambda k: int(st.get(k, "0 kB").split()[0])
        # the fewest of a few looks: sd_notify() has a socket open for a
        # moment every poll
        fds = min(int(x) for x in self.out("for i in 1 2 3 4 5 6; do ls /proc/%s/fd | wc -l; sleep 0.3; done" % pid).split())
        return {"rss": kb("VmRSS"), "data": kb("VmData"), "fds": fds}

    def journal(self, since):
        return self.out("journalctl -u reimu-fand -o cat --no-pager --since @%d" % since)

    def now(self):
        return int(self.out("date +%s").strip())

    def failed_units(self):
        return self.out("systemctl --failed --no-legend --plain | cut -d' ' -f1").split()


def zone(s, z):
    return next(x for x in s.get("zones", []) if x["id"] == z)


FAN_REGS = [(6, 0x4c, 0x4c), (2, 0x4c, 0x4c)] + [(2, a, 0x30 + 0x10 * n) for a in (0x2c, 0x2e) for n in range(5)]


def full_regs(b):
    """every fan drive register, in one ssh call"""
    o = b.out("; ".join("i2cget -f -y %d 0x%02x 0x%02x" % x for x in FAN_REGS))
    v = [int(x, 16) for x in o.split() if x.startswith("0x")]
    # 100 %: the LM96163s 2 * PWM_FREQ (0x17) = 46, the EMC2305s 255
    want = [46, 46] + [0xff] * 10
    return v == want, v


def memory_run(b, minutes, work=None):
    """RSS, heap, fds of reimu-fand: after a warm-up and after `minutes`"""
    pid = b.pid()
    time.sleep(30)
    p0 = b.proc(pid)
    end = time.time() + minutes * 60
    while time.time() < end:
        if work:
            work()
        time.sleep(5)
    p1 = b.proc(pid)
    print("    reimu-fand: RSS %d -> %d KiB, data %d -> %d KiB, fds %d -> %d" %
          (p0["rss"], p1["rss"], p0["data"], p1["data"], p0["fds"], p1["fds"]))
    check(b.pid() == pid, "no restart during the run")
    check(p1["rss"] - p0["rss"] <= 64 and p1["data"] - p0["data"] <= 64, "RSS and heap flat (+%d, +%d KiB)" %
          (p1["rss"] - p0["rss"], p1["data"] - p0["data"]))
    if not check(p1["fds"] == p0["fds"], "fds flat (%d -> %d)" % (p0["fds"], p1["fds"])):
        print("    " + b.out("ls -l /proc/%s/fd" % pid).replace("\n", "\n    "))


# ------------------------------------------------------------------ tests
def t_bare():
    b = Bmc("fand-bare", fresh("fand-bare")).start()
    try:
        check(b.wait_ssh(), "up")
        check(b.out("systemctl is-active reimu-fand").strip() == "active", "reimu-fand active")
        s = b.state()
        check(s.get("version") == 1, "D-Bus Json answers")
        print("    host at boot: %s" % s.get("host"))
        b.power(False)
        s = b.wait_state(lambda s: s["host"] == "off", 20)
        check(s.get("host") == "off", "host off: idle (%s)" % s.get("host"))
        r = subprocess.run(["curl", "-ks", "-u", "root:0penBmc", "https://127.0.0.1:%d%s" % (T.HTTPS, OBJ)],
                           capture_output=True, text=True)
        try:
            web = json.loads(json.loads(r.stdout)["data"]["Json"])
        except (ValueError, KeyError, TypeError):
            web = {}
        check(web.get("version") == 1, "bmcweb serves it (the web UI's card): %s" % r.stdout[:120])

        b.conf(**FAST)
        b.restart()
        t0 = b.now()
        b.power(True)
        s = b.wait_state(lambda s: s["host"] == "on" and s["alarm"], 40)
        check(s.get("host") == "on", "host on: control after SETTLE_SEC (%s)" % s.get("host"))
        check(s.get("alarm") and all(z["failsafe"] or z["duty"] == 100 for z in s.get("zones", [])),
              "no sensors, no fan chips: alarm, every zone 100%% (%s)" % s.get("alarm_text"))
        check(all(z["duty"] == 100 for z in s.get("zones", [])), "duty 100%%: %s" % [z["duty"] for z in s.get("zones", [])])
        check(b.led() == "default-on", "fan fault LED on (%s)" % b.led())
        # the inputs fail safe at once (nothing read yet), the fan chips once
        # not driven for STALE_SEC
        for i in range(15):
            j = b.journal(t0)
            if "FAN CONTROL FAILURE" in j:
                break
            time.sleep(1)
        if not check("FAN CONTROL FAILURE" in j and "zones at 100%" in j, "logged"):
            print("    journal:\n      " + "\n      ".join(j.strip().splitlines()[-20:]))
        check("reimu-fand.service" not in b.failed_units(), "not failed")

        # crashes: SIGKILL, SIGABRT (what the watchdog sends), stop/start
        n0 = int(b.out("systemctl show -p NRestarts --value reimu-fand").strip() or 0)
        for sig in ("KILL", "ABRT", "SEGV"):
            pid = b.pid()
            t1 = b.now()
            b.ssh("kill -%s %s" % (sig, pid))
            ok = False
            for i in range(20):
                time.sleep(1)
                if b.out("systemctl is-active reimu-fand").strip() == "active" and b.pid() not in ("0", pid):
                    ok = True
                    break
            check(ok, "SIG%s: started again" % sig)
            check("fans set to 100%" in b.journal(t1), "SIG%s: ExecStopPost ran (--failsafe)" % sig)
        n1 = int(b.out("systemctl show -p NRestarts --value reimu-fand").strip() or 0)
        check(n1 - n0 == 3, "NRestarts +3 (%d -> %d)" % (n0, n1))
        t1 = b.now()
        b.ssh("systemctl stop reimu-fand")
        check("fans set to 100%" in b.journal(t1) and "stopped: fans 100%" in b.journal(t1), "stop: 100% (daemon and ExecStopPost)")
        b.ssh("systemctl start reimu-fand")
        time.sleep(2)
        check(b.out("systemctl is-active reimu-fand").strip() == "active", "started again")
        t1 = b.now()
        b.ssh("systemctl reload reimu-fand")
        time.sleep(1)
        check("SIGHUP" in b.journal(t1) and b.out("systemctl is-active reimu-fand").strip() == "active", "reload: SIGHUP, still running")

        # memory: polling as fast as allowed, the D-Bus state read meanwhile
        b.conf(**dict(FAST, POLL_MS=100))
        b.restart()
        b.wait_state(lambda s: s["host"] == "on", 20)
        memory_run(b, MINUTES, lambda: b.ssh("for i in $(seq 20); do reimu-fand --status >/dev/null; done"))
        b.power(False)
        s = b.wait_state(lambda s: s["host"] == "off", 20)
        check(s.get("host") == "off" and not s.get("alarm"), "host off: idle, no alarm")
        f = b.failed_units()
        expected = {"mcst-fruid.service"}  # QEMU has no FRU EEPROM
        check(set(f) <= expected, "failed units %s (QEMU: only %s)" % (f, sorted(expected)))
        k = b.out("dmesg | grep -iE 'out of memory|oom-kill' | head -3")
        check(not k.strip(), "no OOM %s" % k.strip())
    finally:
        b.stop()


def t_board():
    b = Bmc("fand-board", fresh("fand-board"), chips=True).start()
    try:
        check(b.wait_ssh(), "up")
        b.power(False)
        # the chips are empty RAM until loaded: the daemon off meanwhile (its
        # host probe would see them answer)
        b.ssh("systemctl stop reimu-fand")
        check(b.load_chips(), "emulated chips loaded")
        # tmp105 12-bit mode, pointer back at the temperature
        for a in PCS:
            b.ssh("i2cset -f -y 5 0x%02x 1 0x60 && i2cset -f -y 5 0x%02x 0" % (a, a))
        b.temps(**{"t%x" % a: 50 + a - 0x20 for a in PCS})
        check(b.reg(2, 0x2c, 0xfd) == 0x34 and b.reg(6, 0x4c, 0xff) == 0x49, "emulated chips answer")
        b.conf(HOST_PROBE="no", **dict(FAST, START_FULL_SEC=8))
        b.ssh("systemctl start reimu-fand")
        time.sleep(1)
        b.setreg(6, 0x4c, 0x4c, 0x05)
        t0 = b.now()
        b.power(True)
        s = b.wait_state(lambda s: s["host"] == "on", 30)
        check(s.get("host") == "on", "host on")
        full, v = full_regs(b)
        check(full, "taking control: 100%% first (%s)" % v)
        s = b.wait_state(lambda s: "full_left" not in s and zone(s, "cpu")["duty"] < 100, 30)
        b.conf(HOST_PROBE="no", **FAST)
        b.ssh("systemctl reload reimu-fand")
        time.sleep(2)
        s = b.state()
        cpu, board = zone(s, "cpu"), zone(s, "board")
        # PCS max 56 °C: 39.8 %; IOHUB 83 °C: 46 %
        print("    cpu %.1f%% board %.1f%%, inputs %s" % (cpu["duty"], board["duty"],
              [(i["id"], i["temp"]) for i in s.get("inputs", [])]))
        check(abs(cpu["duty"] - 39.8) < 0.2 and abs(board["duty"] - 46) < 0.2, "then the curves: cpu %.1f, board %.1f" %
              (cpu["duty"], board["duty"]))
        check(not s.get("alarm"), "no alarm (%s)" % s.get("alarm_text"))
        check(b.reg(6, 0x4c, 0x4c) == round(39.8 * 46 / 100), "cpu fan register %d of 46" % b.reg(6, 0x4c, 0x4c))
        check(b.reg(2, 0x2e, 0x70) == round(46 * 255 / 100), "case fan register %d of 255" % b.reg(2, 0x2e, 0x70))
        check(b.reg(2, 0x2c, 0x20) & 0x20 and b.reg(2, 0x2e, 0x20) & 0x20, "EMC2305 watchdog on")
        check(b.reg(6, 0x4c, 0x4a) == 0x2a and b.reg(2, 0x4c, 0x4a) == 0x2b,
              "TACH_MODE accurate on the CPU fan chip (0x4A 0x2b -> 0x2a), the other untouched")
        check(abs((cpu["fans"][0]["rpm"] or 0) - 2000) < 1, "CPU fan rpm %s" % cpu["fans"][0]["rpm"])
        check(b.led() == "none", "LED off")

        # D-Bus control
        check(b.busctl("SetDuty", "sd", "cpu", "60").returncode == 0, "SetDuty cpu 60")
        time.sleep(1)
        # zone 60 % -> the CPU fan's range 35..65 %: 47.9 % = 22 of 46
        check(b.reg(6, 0x4c, 0x4c) == 22, "manual 60%%: CPU fan register %d (its range)" % b.reg(6, 0x4c, 0x4c))
        r = b.busctl("SetDuty", "sd", "cpu", "10")
        check(r.returncode != 0 and "outside" in r.stderr, "SetDuty below the floor refused: %s" % r.stderr.strip())
        s = b.wait_state(lambda s: zone(s, "cpu")["mode"] == "auto", 20)
        check(zone(s, "cpu")["mode"] == "auto", "manual not confirmed: auto after MANUAL_TIMEOUT_SEC")
        r = b.ssh("busctl set-property %s %s/board %s.Zone Mode s full" % (SVC, OBJ, SVC))
        time.sleep(1)
        check(r.returncode == 0 and b.reg(2, 0x2e, 0x30) == 0xff and b.reg(2, 0x4c, 0x4c) == 46,
              "Mode property: board full -> 100%")
        b.busctl("SetMode", "ss", "board", "auto")
        r = b.busctl("SetCurve", "sss", "cpu", "pcs", "'40:10 75:100'")
        check(r.returncode != 0, "a curve below the floor refused: %s" % r.stderr.strip())
        r = b.busctl("SetCurve", "sss", "cpu", "pcs", "'40:35 60:60 75:100'")
        check(r.returncode == 0 and 'CPU_PCS_CURVE="40:35 60:60 75:100"' in b.out("cat /etc/reimu-fand.conf"),
              "SetCurve applied and saved: %s" % r.stdout.strip())
        time.sleep(2)
        check(zone(b.state(), "cpu")["duty"] > 50, "the new curve drives it")

        # a sensor reading 0 ("no sensor")
        b.temps(t23=0)
        s = b.wait_state(lambda s: zone(s, "cpu")["failsafe"], 10)
        check(zone(s, "cpu")["failsafe"] and b.reg(6, 0x4c, 0x4c) == 46, "PCS 0x23 reads 0: 100%% (%s)" % zone(s, "cpu")["reason"])
        b.temps(t23=53)
        s = b.wait_state(lambda s: not zone(s, "cpu")["failsafe"], 10)
        check(not zone(s, "cpu")["failsafe"], "back")

        # the CPU fan stops
        b.setreg(6, 0x4c, 0x46, 0xfc)
        b.setreg(6, 0x4c, 0x47, 0xff)
        s = b.wait_state(lambda s: s["tach_alarm"], 15)
        full, v = full_regs(b)
        check(s.get("tach_alarm") and full and b.led() == "default-on", "CPU fan stopped: alarm, every fan 100%%, LED (%s)" %
              s.get("alarm_text"))
        b.setreg(6, 0x4c, 0x46, 0x8c)
        b.setreg(6, 0x4c, 0x47, 0x0a)
        s = b.wait_state(lambda s: not s["tach_alarm"], 20)
        check(not s.get("tach_alarm") and b.led() == "none", "turning again: cleared after TACH_CLEAR_SEC")

        # SIGKILL: ExecStopPost at once
        time.sleep(3)
        check(b.reg(6, 0x4c, 0x4c) != 46, "quiet before the kill")
        b.ssh("kill -9 %s" % b.pid())
        time.sleep(1)
        full, v = full_regs(b)
        check(full, "SIGKILL: every fan 100%% (ExecStopPost) %s" % v)
        s = b.wait_state(lambda s: s["host"] == "on" and zone(s, "cpu")["duty"] < 100, 30)
        check(zone(s, "cpu")["duty"] < 100, "restarted (no settle: the host was on), quiet again")

        # host off: one 100 % write, then no I2C
        b.power(False)
        s = b.wait_state(lambda s: s["host"] == "off", 10)
        full, v = full_regs(b)
        check(s.get("host") == "off" and full, "host off: fans 100%% once")
        b.setreg(6, 0x4c, 0x4c, 0x05)
        time.sleep(3)
        check(b.reg(6, 0x4c, 0x4c) == 0x05, "then nothing written")
        # HOST_PROBE: off, but the devices answer
        t1 = b.now()
        b.conf(**FAST)
        b.ssh("systemctl reload reimu-fand")
        s = b.wait_state(lambda s: s["host"] == "unknown" and zone(s, "cpu")["duty"] < 100, 40)
        check("taken as on" in b.journal(t1) and s.get("host") == "unknown", "HOST_PROBE: off but devices answer: controlled")
        b.power(True)
        s = b.wait_state(lambda s: s["host"] == "on", 20)
        check(s.get("host") == "on", "on again")

        b.conf(**dict(FAST, POLL_MS=100))
        b.restart()
        b.wait_state(lambda s: s["host"] == "on" and zone(s, "cpu")["duty"] < 100, 30)
        memory_run(b, MINUTES, lambda: b.ssh("for i in $(seq 20); do reimu-fand --status >/dev/null; done"))
        f = b.failed_units()
        expected = {"mcst-fruid.service"}
        check(set(f) <= expected, "failed units %s (QEMU: only %s)" % (f, sorted(expected)))
        j = b.journal(t0)
        print("    journal (last 15):\n      " + "\n      ".join(j.strip().splitlines()[-15:]))
    finally:
        b.stop()


TESTS = [("bare", t_bare), ("board", t_board)]

if __name__ == "__main__":
    for name, fn in TESTS:
        if T.ONLY and name not in T.ONLY:
            continue
        print("== " + name, flush=True)
        t = time.time()
        try:
            fn()
        except Exception as e:  # a test that crashes is a failure, the others still run
            import traceback
            traceback.print_exc()
            check(False, "%s: %r" % (name, e))
        print("   (%d s)" % (time.time() - t), flush=True)
    print("\n%d failed" % len(T.failed) + ("".join("\n  " + f for f in T.failed)))
    sys.exit(1 if T.failed else 0)
