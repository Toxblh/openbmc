#!/usr/bin/env python3
"""overheatd (the overheat protection) in QEMU: memory over a long run, the
CPU sensors of both device tree formats, the failures it must live through.

    test_overheatd_qemu.py WORKDIR [TEST...]

WORKDIR holds ab.mtd as for test_ab_qemu.py (RQ, AB_SSH, AB_HTTPS too).
OVERHEATD_OLD=file: the stock overheatd (from its rpm), run first as the
control of the memory test: with the board's device tree it must stay flat
now (its leak was libreimu's), reading old-format sensors it must grow (its
own leak: the test sees one).
OH_MINUTES: length of the memory run (default 12).

The E8C2 PCS of CPU 0 (BMC i2c-5, 0x20 + sensor) is emulated by tmp105s at
0x20..0x25 (0x26, 0x27 absent: no ACK). A 2-byte read of a tmp105 gives its
temperature register, T * 256, which the PCS format reads as 1/8 °C: 32 * T
(12-bit mode, set over I2C, so even values). The host power (GPIO_POWER_IN,
GPIO O2) is set over QMP. Tests (all by default):

  memory    the installed overheatd polling as fast as it can (WAIT_MSEC=0:
            ~1 board day of 5 s polls in 2 min) through normal, overheat,
            host off, blind (no sensor answers), no device tree and a broken
            config, in turn: RSS, heap and fds stay flat, no restart
  sensors   board device tree (pcs@0): temperatures, overheat -> LED blinks,
            a "hot" trip of its thermal zone lowers the limit, host off: no
            I2C, power on: waits PCS_SETTLE_SEC, no device tree: CPU 0 probed
            anyway, nothing readable: error + LED, old format (l_pcs_i2c
            hwmon): read, device bound again (new hwmon number) still read;
            <status> ok / over-limit / blind / host-off, blind flashes
            otherwise; the poll time with no sensor answering
  failures  broken config: keeps working, SIGTERM stops it at once,
            SIGKILL: systemd starts it again, OOM score -1000, LimitAS,
            a hang (SIGSTOP): the watchdog restarts it, TINYSPI=yes without
            TinySPI: keeps running on the GPIOs
"""
import json, math, os, re, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ab_qemu as T  # noqa: E402  (takes WORKDIR from sys.argv[1])
from test_ab_qemu import check, fresh  # noqa: E402

PCS = list(range(0x20, 0x26))  # tmp105 there
OLD = os.environ.get("OVERHEATD_OLD")
MINUTES = int(os.environ.get("OH_MINUTES", 12))
DTB = "/var/volatile/motherboard_devtree.dtb"
STATE = "/var/volatile/systemstate.xml"
LED = "/sys/class/leds/platform:red:overheat/trigger"
# the hwmon of the l_pcs_i2c device at 5-0020, once it's there
HWMON20 = "for i in 1 2 3 4 5 6 7 8 9 10; do ls /sys/bus/i2c/devices/5-0020/hwmon 2>/dev/null | grep . && break; sleep 1; done"


class Bmc(T.Bmc):
    """test_ab_qemu's BMC with the PCS sensors on i2c-5"""
    def start(self):
        popen = subprocess.Popen

        def with_pcs(args, **kw):
            if args[0] == "qemu-system-arm":
                for a in PCS:
                    args = args + ["-device", "tmp105,bus=aspeed.i2c.bus.5,address=0x%02x,id=pcs%02x" % (a, a)]
            return popen(args, **kw)
        subprocess.Popen = with_pcs
        try:
            return super().start()
        finally:
            subprocess.Popen = popen

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
        """temps(t20=50, ...): °C as the PCS reads it"""
        self.qom(*[("/machine/peripheral/pcs" + k[1:], "temperature", math.ceil(v * 1000 / 32))
                   for k, v in by_addr.items()])

    def conf(self, **kv):
        c = dict(OVERHEATD_ENABLED="yes", DEBUG="off", CPU_TEMP_LIMIT=85, WAIT_MSEC=200, BLINK_MSEC=500,
                 PCS_SETTLE_SEC=0)
        c.update(kv)
        body = "".join("%s=%s\n" % (k, v) for k, v in c.items() if v is not None)
        self.ssh("cat > /etc/overheatd.conf.new && mv /etc/overheatd.conf.new /etc/overheatd.conf", input=body)

    def ssh(self, cmd, timeout=120, input=None):
        c, env = T.ssh_cmd(cmd)
        return subprocess.run(c, env=env, input=input, stdin=None if input is not None else subprocess.DEVNULL,
                              capture_output=True, text=True, timeout=timeout)

    def dtb(self, dts):
        if dts is None:
            return self.ssh("rm -f " + DTB)
        r = self.ssh("dtc -q -I dts -O dtb -o %s.new - && mv %s.new %s" % (DTB, DTB, DTB), input=dts)
        assert r.returncode == 0, r.stderr

    def state(self):
        for i in range(5):
            x = self.out("cat " + STATE)
            m = re.search(r'poll="(\d+)"', x)
            if m:
                break
            time.sleep(0.5)
        return {"poll": int(m.group(1)) if m else -1,
                "temps": {(int(c), int(s)): float(v) for c, s, v in
                          re.findall(r'<temperature cpu="(\d+)" sensor="(\d+)">([-\d.]+)<', x)},
                "cpus": re.findall(r"<cpus>([^<]*)</cpus>", x),
                "error": re.findall(r"<error>([^<]*)</error>", x),
                "status": "".join(re.findall(r"<status>([^<]*)</status>", x)), "xml": x}

    def wait_state(self, pred, timeout=30):
        """the first state (of a poll after this call) that pred accepts, or the last one"""
        p0 = self.state()["poll"]
        end = time.time() + timeout
        while True:
            s = self.state()
            if s["poll"] > p0 + 1 and pred(s) or time.time() > end:
                return s
            time.sleep(1)

    def led(self):
        m = re.search(r"\[([^\]]+)\]", self.out("cat " + LED))
        return m.group(1) if m else "?"

    def blink(self):
        d = "/sys/class/leds/platform:red:overheat/"
        return self.out("cat %sdelay_on %sdelay_off 2>/dev/null" % (d, d)).split()

    def wait_led(self, want, timeout=30):
        end = time.time() + timeout
        while self.led() != want and time.time() < end:
            time.sleep(1)
        return self.led()

    def pid(self):
        return self.out("systemctl show -p MainPID --value overheatd").strip()

    def proc(self, pid):
        st = dict(l.split(":", 1) for l in self.out("cat /proc/%s/status" % pid).splitlines() if ":" in l)
        kb = lambda k: int(st.get(k, "0 kB").split()[0])
        # a poll opens files, GPIO lines, /dev/i2c-N for a moment: the fewest of a few looks
        fds = int(self.out("for i in 1 2 3 4 5 6 7 8 9 10; do ls /proc/%s/fd | wc -l; done | sort -n | head -1" % pid))
        return {"rss": kb("VmRSS"), "data": kb("VmData"), "fds": fds}

    def journal(self, since):
        return self.out("journalctl -u overheatd -o cat --no-pager --since @%d" % since)

    def now(self):
        return int(self.out("date +%s").strip())


# ------------------------------------------------------------------ device trees
def board_dts(trips="", pcs=True, old=False):
    """like the ПНС device tree of the board (1.0_2025-02-06): pcs@0 with
    a thermal zone of "active" trips (fan steps); old=True: CPU sensors 0, 1
    as l_pcs_i2c nodes on /bmc i2c6 (BMC bus 5) instead"""
    active = "".join('alert0_%d { temperature = <%d>; hysteresis = <3000>; type = "active"; };\n' % (i, t)
                     for i, t in enumerate((40000, 50000, 55000, 60000, 65000, 70000, 75000)))
    return """/dts-v1/;
/ {
    version = "1.0_2025-02-06";
    l_i2c@0 {
        reg = <0 0>; compatible = "l_i2c"; label = "iohub2 i2c";
        #address-cells = <1>; #size-cells = <0>;
        i2c@0 { reg = <0>; #address-cells = <1>; #size-cells = <0>;
            lm96163@4c { compatible = "national,lm96163"; reg = <0x4c>; label = "CPU FAN Control (X37)"; };
            ncp4200@60 { compatible = "pmbus"; reg = <0x60>; label = "Power Control"; };
        };
        i2c@1 { reg = <1>; #address-cells = <1>; #size-cells = <0>;
            emc2305@2e { compatible = "smsc,emc2305"; reg = <0x2e>; label = "FAN: PWM[0..4] TACH[0..4]"; };
            power@58 { compatible = "pmbus"; reg = <0x58>; label = "Power supply"; };
        };
        i2c@3 { reg = <3>; #address-cells = <1>; #size-cells = <0>;
            fruid@57 { compatible = "atmel,24c128"; reg = <0x57>; label = "FRUID"; };
        };
        i2c@4 { reg = <4>; #address-cells = <1>; #size-cells = <0>;
            %s
        };
    };
    /* the board's has the CPUs, PCI, IRQs and fan maps too: 13 KB */
    filler { data = [%s]; };
    bmc { i2c3 = "/l_i2c@0/i2c@1"; i2c4 = "/l_i2c@0/i2c@3"; i2c7 = "/l_i2c@0/i2c@0"; %s };
    %s
};
""" % ("".join('pcs@%x { compatible = "mcst,l_pcs_i2c"; reg = <0x%x>; label = "CPU0 T%d"; };' % (a, a, a - 0x20)
               for a in (0x20, 0x21)) if old else "",
       " ".join(["00"] * 11000),
       'i2c6 = "/l_i2c@0/i2c@4";' if old else "",
       """thermal-zones { cpu-thermal_0 { polling-delay-passive = <250>; polling-delay = <1000>;
            thermal-sensors = <&pcs0 0>; trips { %s %s }; }; };
       pcs0: pcs@0 { reg = <0 0>; label = "E8C2 Power Control System Node 0"; #thermal-sensor-cells = <1>; };"""
       % (active, trips) if pcs and not old else "")


NORMAL = dict(t20=50, t21=52, t22=54, t23=56, t24=58, t25=60)
NORMAL_TEMPS = {(0, a - 0x20): float(NORMAL["t%x" % a]) for a in PCS}


def setup(b):
    """sensors in 12-bit mode at normal temperatures, host on, board device tree"""
    b.ssh("systemctl stop overheatd")
    b.temps(**NORMAL)
    b.power(True)
    for a in PCS:
        b.ssh("i2cset -f -y 5 0x%02x 1 0x60 && i2cset -f -y 5 0x%02x 0" % (a, a))
    b.dtb(board_dts())
    b.conf()
    b.ssh("systemctl reset-failed overheatd; systemctl start overheatd")


# ------------------------------------------------------------------ tests
def t_memory(b):
    setup(b)
    if OLD:
        b.ssh("systemctl stop overheatd")
        b.conf(WAIT_MSEC=100)
        check(b.scp(OLD, "/tmp/overheatd.old"), "control binary copied")

        def run_old(what):
            b.ssh("chmod +x /tmp/overheatd.old; (/tmp/overheatd.old >/dev/null 2>&1 &)")
            time.sleep(5)
            pid = b.out("pidof overheatd.old").strip()
            a = b.proc(pid)
            time.sleep(30)
            z = b.proc(pid)
            x = b.out("cat " + STATE)
            b.ssh("kill %s" % pid)
            print("    control %s, %s, WAIT_MSEC=100, 30 s: RSS %d -> %d KiB, heap %d -> %d KiB"
                  % (os.path.basename(OLD), what, a["rss"], z["rss"], a["data"], z["data"]))
            return z["rss"] - a["rss"], x
        # the board's case: the device tree (pcs@N, 13 KB) has no l_pcs_i2c node, so nothing is
        # monitored, yet the stock overheatd grew by the device tree a poll: that leak was libreimu's
        grow, x = run_old("board device tree")
        check("<temperature" not in x, "control: the stock overheatd monitors no CPU with the board's device tree")
        check(grow <= 64, "... and with the fixed libreimu no longer grows (%+d KiB in 30 s)" % grow)
        # its own leak (the reading's buffer) shows with the old format: the test sees a leak
        b.ssh("modprobe l-pcs-i2c")
        b.dtb(board_dts(old=True))
        for a in (0x20, 0x21):
            b.ssh("echo l_pcs_i2c 0x%02x > /sys/bus/i2c/devices/i2c-5/new_device" % a)
        grow, x = run_old("old format, 2 sensors")
        check("<temperature" in x and grow > 1024, "control: the stock overheatd reading sensors grows (%+d KiB in 30 s)"
              % grow)
        for a in (0x20, 0x21):
            b.ssh("echo 0x%02x > /sys/bus/i2c/devices/i2c-5/delete_device" % a)
        b.dtb(board_dts())
        b.ssh("systemctl start overheatd")

    b.conf(WAIT_MSEC=0)
    b.ssh("systemctl restart overheatd")
    time.sleep(30)  # warm up: heap, LED, device tree read once
    pid = b.pid()
    phases = [("normal", lambda: None),
              ("overheat", lambda: b.temps(t23=96)),
              ("host off", lambda: b.power(False)),
              ("blind", lambda: b.conf(WAIT_MSEC=0, PCS_BUS=9)),
              ("no devtree", lambda: b.dtb(None)),
              ("bad config", lambda: b.ssh("echo CPU_TEMP_LIMIT=abc > /etc/overheatd.conf"))]

    def back():
        b.temps(**NORMAL)
        b.power(True)
        b.dtb(board_dts())
        b.conf(WAIT_MSEC=0)
    p0, t0, first = b.state()["poll"], time.time(), b.proc(pid)
    print("    %-10s %8s %8s %8s %4s" % ("phase", "polls", "RSS KiB", "heap KiB", "fds"))
    print("    %-10s %8d %8d %8d %4d" % ("start", 0, first["rss"], first["data"], first["fds"]))
    samples = []
    per = 60 * MINUTES / (2 * len(phases))
    for i in range(2 * len(phases)):
        name, enter = phases[i % len(phases)]
        enter()
        time.sleep(per)
        back()
        time.sleep(3)
        s, p = b.state(), b.proc(pid)
        samples.append(p)
        print("    %-10s %8d %8d %8d %4d" % (name, s["poll"] - p0, p["rss"], p["data"], p["fds"]), flush=True)
    polls, secs = b.state()["poll"] - p0, time.time() - t0
    print("    %d polls in %d s = %.1f board days at WAIT_MSEC=5000" % (polls, secs, polls * 5 / 86400))
    check(b.pid() == pid and b.out("systemctl show -p NRestarts --value overheatd").strip() == "0",
          "overheatd ran all the time (pid %s, no restart)" % pid)
    check(polls > 1000 * MINUTES, "%d polls (> %d)" % (polls, 1000 * MINUTES))
    grow = max(p["rss"] for p in samples) - first["rss"]
    check(grow <= 64, "RSS flat: %d -> max %d KiB (%+d, <= 64)" % (first["rss"], max(p["rss"] for p in samples), grow))
    # the second round of the phases against the first
    half = len(phases)
    grow2 = max(p["data"] for p in samples[half:]) - max(p["data"] for p in samples[:half])
    check(grow2 <= 0, "heap flat: the second round of phases needs no more than the first (%+d KiB)" % grow2)
    check(all(p["fds"] <= first["fds"] for p in samples), "no fd leak (%d -> %d)" % (first["fds"], samples[-1]["fds"]))
    s = b.wait_state(lambda s: s["temps"] == NORMAL_TEMPS)
    check(s["temps"] == NORMAL_TEMPS and b.wait_led("none") == "none", "still monitoring after the run")


def t_sensors(b):
    since = b.now()
    setup(b)
    s = b.wait_state(lambda s: s["temps"] == NORMAL_TEMPS)
    check(s["temps"] == NORMAL_TEMPS, "board device tree (pcs@0): CPU 0 sensors 0..5 read %s" % sorted(s["temps"].values()))
    check(s["cpus"] == ["0"] and not s["error"] and s["status"] == "ok", "<cpus>0</cpus>, no error, status ok")
    check(b.wait_led("none") == "none", "LED off")
    check("CPU sensors from device tree" in b.journal(since),
          "journal: sensors from the device tree")

    b.temps(t23=96)
    check(b.wait_led("timer") == "timer", "sensor 3 at 96 °C (limit 85): LED blinks")
    check(b.blink() == ["500", "500"] and b.state()["status"] == "over-limit",
          "... BLINK_MSEC on/off %s, status over-limit" % b.blink())
    b.temps(**NORMAL)
    check(b.wait_led("none") == "none", "back to 56 °C: LED off")

    b.dtb(board_dts(trips='hot0 { temperature = <45000>; hysteresis = <2000>; type = "hot"; };'))
    check(b.wait_led("timer") == "timer", "\"hot\" trip at 45 °C in the thermal zone of pcs@0: 50 °C blinks")
    b.dtb(board_dts())
    check(b.wait_led("none") == "none", "only \"active\" trips (fan steps, 40..75 °C): the limit is CPU_TEMP_LIMIT")

    b.conf(PCS_SETTLE_SEC=15)
    b.power(False)
    s = b.wait_state(lambda s: not s["temps"])
    check(not s["temps"] and not s["error"] and b.led() == "none" and s["status"] == "host-off",
          "host off: no sensor read, no error, LED off, status host-off")
    b.power(True)
    t = time.time()
    s = b.wait_state(lambda s: s["temps"], timeout=40)
    dt = time.time() - t
    check(s["temps"] == NORMAL_TEMPS and dt >= 12, "host on: sensors read again after PCS_SETTLE_SEC (%.0f s)" % dt)
    b.conf()

    since = b.now()
    b.dtb(None)
    time.sleep(3)
    s = b.wait_state(lambda s: s["temps"] == NORMAL_TEMPS)
    check(s["temps"] == NORMAL_TEMPS, "no device tree (mcst-fruid failed): CPU 0 sensors probed anyway")
    check("Warning: no device tree" in b.journal(since), "journal: warning about it")
    b.dtb(board_dts())

    since = b.now()
    b.conf(PCS_BUS=9)
    s = b.wait_state(lambda s: s["error"])
    check(s["error"] and not s["temps"], "no sensor answers (bus 9): %s" % s["error"])
    check(b.wait_led("timer") == "timer" and b.blink() == ["100", "1900"] and s["status"] == "blind",
          "... status blind, LED flashes otherwise than for an overheat (on/off %s ms)" % b.blink())
    check("overheat protection is blind" in b.journal(since), "... journal: error")
    # the worst poll in QEMU: every sensor read without an answer
    b.conf(WAIT_MSEC=0, PCS_BUS=9)
    p0, t = b.state()["poll"], time.time()
    time.sleep(10)
    n = b.state()["poll"] - p0
    print("    poll with no sensor answering: %.1f ms (%d in 10 s)" % (10000.0 * (time.time() - t) / 10 / max(n, 1), n))
    b.conf()
    s = b.wait_state(lambda s: s["temps"] == NORMAL_TEMPS and not s["error"])
    check(s["temps"] == NORMAL_TEMPS and b.wait_led("none") == "none", "sensors back: error gone, LED off")

    # the old format: l_pcs_i2c bound by hwmon-instantiator, read through hwmon
    since = b.now()
    b.ssh("modprobe l-pcs-i2c")
    b.dtb(board_dts(old=True))
    for a in (0x20, 0x21):
        b.ssh("echo l_pcs_i2c 0x%02x > /sys/bus/i2c/devices/i2c-5/new_device" % a)
    want = {k: v for k, v in NORMAL_TEMPS.items() if k[1] < 2}
    s = b.wait_state(lambda s: s["temps"] == want)
    hw0 = b.out(HWMON20).strip()
    check(s["temps"] == want, "old format (l_pcs_i2c on /bmc i2c6, %s): %s" % (hw0, s["temps"]))
    # bound again: hwmon numbers are reused lowest first, so a device at 0x27 (no chip) takes the old one
    b.ssh("echo 0x20 > /sys/bus/i2c/devices/i2c-5/delete_device; sleep 1; "
          "echo l_pcs_i2c 0x27 > /sys/bus/i2c/devices/i2c-5/new_device; sleep 1; "
          "echo l_pcs_i2c 0x20 > /sys/bus/i2c/devices/i2c-5/new_device")
    hw1 = b.out(HWMON20).strip()
    b.temps(t20=62)
    s = b.wait_state(lambda s: s["temps"].get((0, 0)) == 62.0)
    check(hw0 and hw1 and hw1 != hw0 and s["temps"].get((0, 0)) == 62.0, "device bound again (%s -> %s): still read" % (hw0, hw1))
    j = b.journal(since)
    check("Failure reading" not in j or "readable again" in j, "journal: no failure left")
    for a in (0x20, 0x21, 0x27):
        b.ssh("echo 0x%02x > /sys/bus/i2c/devices/i2c-5/delete_device" % a)
    b.temps(**NORMAL)
    b.dtb(board_dts())
    s = b.wait_state(lambda s: s["temps"] == NORMAL_TEMPS)
    check(s["temps"] == NORMAL_TEMPS, "board device tree again")


def t_failures(b):
    setup(b)
    pid = b.pid()
    since = b.now()
    b.ssh("echo CPU_TEMP_LIMIT=abc > /etc/overheatd.conf")
    p0 = b.state()["poll"]
    time.sleep(5)
    s = b.state()
    check(b.pid() == pid and s["poll"] > p0 + 5 and s["temps"] == NORMAL_TEMPS,
          "broken config: still polling with the previous settings")
    check("keeping the previous settings" in b.journal(since), "journal: said so (once: %d)"
          % b.journal(since).count("keeping the previous"))
    b.conf()
    t = time.time()
    b.ssh("systemctl stop overheatd")
    check(time.time() - t < 10 and b.out("systemctl is-active overheatd").strip() == "inactive",
          "SIGTERM stops it at once (%.1f s)" % (time.time() - t))
    b.ssh("systemctl start overheatd")
    pid = b.pid()
    b.ssh("kill -9 " + pid)
    time.sleep(10)
    check(b.pid() not in ("", "0", pid) and b.out("systemctl is-active overheatd").strip() == "active",
          "SIGKILL: systemd starts it again (Restart=always)")
    s = b.wait_state(lambda s: s["temps"] == NORMAL_TEMPS)
    check(s["temps"] == NORMAL_TEMPS, "and it monitors")

    # last-resort protection: the OOM killer takes anything else first, its own memory is bounded
    pid = b.pid()
    adj = b.out("cat /proc/%s/oom_score_adj" % pid).strip()
    # (no memory cgroup in this kernel, so MemoryMax= would do nothing: an address space limit)
    lim = re.findall(r"Max address space\s+(\d+)", b.out("cat /proc/%s/limits" % pid))

    check(adj == "-1000", "oom_score_adj %s" % adj)
    check(lim == [str(32 << 20)], "address space limited to 32 MiB %s (it needs %s KiB)"
          % (lim, b.out("grep VmSize /proc/%s/status" % pid).split()[1:2]))

    # hang: no watchdog ping -> systemd kills and restarts it (60 s; 10 s for the test)
    wd = b.out("systemctl show -p WatchdogUSec --value overheatd").strip()
    check(wd == "1min", "WatchdogSec %s" % wd)
    b.ssh("mkdir -p /run/systemd/system/overheatd.service.d && printf '[Service]\\nWatchdogSec=10\\nTimeoutAbortSec=5\\n' "
          "> /run/systemd/system/overheatd.service.d/test.conf && systemctl daemon-reload && systemctl restart overheatd")
    time.sleep(15)
    pid = b.pid()
    n0 = int(b.out("systemctl show -p NRestarts --value overheatd").strip())
    since = b.now()
    b.ssh("kill -STOP " + pid)
    end = time.time() + 60
    while time.time() < end and b.pid() in ("", "0", pid):
        time.sleep(2)
    check(b.pid() not in ("", "0", pid) and "atchdog timeout" in b.out("journalctl -u overheatd --no-pager --since @%d" % since),
          "hung (SIGSTOP): watchdog timeout, restarted (%d -> %s restarts)"
          % (n0, b.out("systemctl show -p NRestarts --value overheatd").strip()))
    s = b.wait_state(lambda s: s["temps"] == NORMAL_TEMPS)
    check(s["temps"] == NORMAL_TEMPS, "and it monitors again")
    b.ssh("rm /run/systemd/system/overheatd.service.d/test.conf && systemctl daemon-reload && systemctl restart overheatd")

    # TinySPI configured but not there: no exit (no restart loop unprotected), the GPIOs instead
    tspi = b.ssh("ls -d /sys/kernel/tinyspi").returncode == 0
    old = b.out("cat /var/volatile/reimu.conf")
    since = b.now()
    b.ssh("echo TINYSPI=yes > /var/volatile/reimu.conf && systemctl restart overheatd")
    time.sleep(8)
    pid = b.pid()
    time.sleep(8)
    s = b.wait_state(lambda s: s["temps"] == NORMAL_TEMPS)
    check(not tspi and b.pid() == pid and b.out("systemctl is-active overheatd").strip() == "active"
          and s["temps"] == NORMAL_TEMPS, "TINYSPI=yes without TinySPI: keeps running and monitoring")
    check("TinySPI is enabled, but no API files" in b.journal(since), "... journal: said so")
    b.ssh("cat > /var/volatile/reimu.conf && systemctl restart overheatd", input=old)


TESTS = [("sensors", t_sensors), ("failures", t_failures), ("memory", t_memory)]

if __name__ == "__main__":
    b = Bmc("overheatd", fresh("overheatd")).start()
    try:
        check(b.wait_ssh(), "up")
        for name, fn in TESTS:
            if T.ONLY and name not in T.ONLY:
                continue
            print("== " + name, flush=True)
            t = time.time()
            try:
                fn(b)
            except Exception as e:  # a test that crashes is a failure, the others still run
                check(False, "%s: %r" % (name, e))
            print("   (%d s)" % (time.time() - t), flush=True)
        f = b.out("journalctl -u overheatd -o cat --no-pager | grep -ciE 'segfault|core dump|out of memory'").strip()
        check(f in ("", "0"), "no crash in the journal")
    finally:
        b.stop()
    print("\n%d failed" % len(T.failed) + ("".join("\n  " + f for f in T.failed)))
    sys.exit(1 if T.failed else 0)
