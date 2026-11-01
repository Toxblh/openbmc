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
  ladder    the power actions, with the power sequencer emulated (GPIO_POWER_IN
            follows the power button, as on the board): under the shutdown
            temperature nothing; over it a short press, once, then the long
            one after GRACEFUL_TIMEOUT_SEC; cooling down: no escalation; over
            the power-off temperature the long press at once; power good
            stays: pressed again; a killed daemon goes on; a slow server_ctl
            doesn't hold the polls; config validation
  glitch    what must never power off: a spike, implausible values, one sensor
            alone, a plausible jump for 2 polls, TTRIP# alone briefly, blind, the
            documented SIGUSR1 LED test through its states (TTRIP#: GPIO_TTRIP
            on a free pin, added to the gpiotab here: boards without CPLD,
            like this one, don't have the signal; CPLD boards have it in
            TinySPI, which QEMU doesn't emulate)
  ttrip     TTRIP# with a hot sensor, and held on its own: the long press
  press     a web power-on during a ForceOff (Conflicts= stop the press), systemctl
            stop, SIGKILL: the power button isn't left held
  selfon    the platform powers itself back on right after the forced off (seen
            once on the board): pressed again, the marker stays; a user's
            power-on in that time isn't undone
  blind     blind for BLIND_ALARM_MIN: said again, the page told; no press
  latch     the marker of an emergency power-off: BMC reboot with
            AUTO_POWER_ON=on and with the AlwaysOn restore policy: the host
            stays off; the web's power-on works and clears it; so does the
            front panel button and the D-Bus/REST ClearEmergency
  optout    OVERHEAT_POWEROFF=no through REST (admin only): at once, no press
            at 96 °C, each skipped action in the journal at critical level,
            the alarm stays; survives a reboot; on again: acts. With OH_E2E=DIR
            (node_modules/playwright there; CHROME: the chromium) the browser
            test ../e2e/overheat.qemu.mjs too
  agent     the agent connected (reimu-hostd.py, needs HL_PORT and
            AB_NETDEV_EXTRA as for test_hostlink_qemu.py): OsPower instead of
            the button; not with HL_PORT unset

A factory reset (revert-to-mfg wipes the rwfs) removes the marker with the rest.

OH_BIN=file: run this overheatd instead of the image's (bind-mounted), for a
mutant of the confirmations: the glitch tests must then fail.
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
LATCH = "/var/lib/overheatd/emergency"
OH_BIN = os.environ.get("OH_BIN")
OBJ = "/xyz/openbmc_project/reimu/overheat"
TTRIP_PIN = "D5"  # free on this board, in QEMU
# the power sequencer (test_fru_sensors_qemu.py's PLATFORM_LOOP), and an OS and
# a sequencer that can be told not to react; the second QMP socket stays free
SEQ_LOOP = r'''
import json, os, socket, sys, time
qmp, ctl, st, ttrip_pin = sys.argv[1:5]
for i in range(100):
    try:
        s = socket.socket(socket.AF_UNIX); s.connect(qmp); break
    except OSError: time.sleep(0.2)
f = s.makefile("rw"); f.readline()
def cmd(c, **a):
    f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r: return r.get("return")
def gpio(pin, v):
    cmd("qom-set", path="/machine/soc/gpio", property="gpio" + pin, value=v)
cmd("qmp_capabilities")
c = dict(power=True, short="os", long="off", ttrip=False, selfon=0)
pressed = off_at = on_at = None
n = dict(short=0, long=0, ons=0, offs=0)
was = None
while True:
    if os.path.exists(ctl):
        c.update(json.load(open(ctl))); os.remove(ctl)
    now = time.time()
    r = cmd("human-monitor-command", **{"command-line": "xp /2wx 0x1e780078"})
    w = r.split(":")[1].split()
    data, dirn = int(w[0], 16), int(w[1], 16)
    # the power button (N5): the BMC drives it low for a press
    if dirn >> 13 & 1 and not data >> 13 & 1:
        if pressed is None: pressed = now
        if c["power"] and now - pressed >= 4 and c["long"] == "off":
            c["power"] = False
            # a platform that comes back by itself (once seen on the board)
            if c["selfon"]: c["selfon"] -= 1; on_at = now + 2
    elif pressed is not None:
        held = now - pressed
        n["long" if held >= 4 else "short"] += 1
        if not c["power"] and held < 4: c["power"] = True
        elif held < 4 and c["short"] == "os": off_at = now + 2
        pressed = None
    if off_at and now >= off_at: c["power"], off_at = False, None
    if on_at and now >= on_at: c["power"], on_at = True, None
    gpio("O2", c["power"])
    gpio(ttrip_pin, not c["ttrip"])
    if c["power"] and was is False: n["ons"] += 1
    if not c["power"] and was is True: n["offs"] += 1
    was = c["power"]
    open(st + ".tmp", "w").write(json.dumps(dict(n, power=c["power"], pressed=pressed is not None)))
    os.replace(st + ".tmp", st)
    time.sleep(0.05)
'''
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
                args = args + ["-qmp", "unix:%s/%s.qmp3,server,nowait" % (T.RQ, self.name)]
            return popen(args, **kw)
        subprocess.Popen = with_pcs
        self.ctl, self.st = "%s/%s.ctl" % (T.RQ, self.name), "%s/%s.st" % (T.RQ, self.name)
        for f in (self.ctl, self.st):
            if os.path.exists(f):
                os.remove(f)
        try:
            super().start()
        finally:
            subprocess.Popen = popen
        self.seq_p = popen([sys.executable, "-c", SEQ_LOOP, "%s/%s.qmp3" % (T.RQ, self.name), self.ctl, self.st, TTRIP_PIN],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return self

    def stop(self):
        super().stop()
        self.seq_p.kill()
        self.seq_p.wait()

    def seq(self, **kv):
        """tell the sequencer: power, short ("os": the OS shuts down; "ignore"),
        long ("off"; "ignore"), ttrip (asserted)"""
        while os.path.exists(self.ctl):
            time.sleep(0.05)
        with open(self.ctl + ".tmp", "w") as f:
            json.dump(kv, f)
        os.replace(self.ctl + ".tmp", self.ctl)
        while os.path.exists(self.ctl):
            time.sleep(0.05)
        time.sleep(0.2)

    def seen(self):
        """what the sequencer saw: short, long, ons, offs, power, pressed"""
        for i in range(20):
            try:
                return json.load(open(self.st))
            except (OSError, ValueError):
                time.sleep(0.1)
        return {}

    def use_binary(self):
        """OH_BIN instead of the image's overheatd"""
        if OH_BIN:
            T.Bmc.scp(self, OH_BIN, "/tmp/oh.bin")
            self.ssh("chmod +x /tmp/oh.bin; mount --bind /tmp/oh.bin /usr/sbin/overheatd; systemctl restart overheatd")

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
        self.seq(power=bool(on))

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
            b.ssh("chmod +x /tmp/overheatd.old; (/tmp/overheatd.old >/tmp/overheatd.old.log 2>&1 &)")
            time.sleep(5)
            pid = b.out("pidof overheatd.old").strip()
            if not pid:
                print("    control gone: " + b.out("tail -3 /tmp/overheatd.old.log; dmesg | tail -2").strip().replace("\n", " | "))
            a = b.proc(pid)
            time.sleep(30)
            z = b.proc(pid)
            x = b.out("cat " + STATE)
            # -9: it misses a SIGTERM that comes while it works (fixed since)
            b.ssh("kill -9 %s" % pid)
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


# ------------------------------------------------------------------ the power actions
def all_temps(b, v):
    b.temps(**{"t%x" % a: v for a in PCS})


def ladder_setup(b, **conf):
    """a host on and cool, a fresh overheatd, a fast ladder; returns the sequencer's counts so far"""
    b.ssh("systemctl stop overheatd; rm -f %s; rm -rf /run/overheatd" % LATCH)
    b.temps(**NORMAL)
    # a press going on ends by itself (KillMode=process)
    wait_seen(b, lambda s: not s["pressed"], 10)
    time.sleep(0.5)
    b.seq(power=True, short="ignore", long="off", ttrip=False)
    for a in PCS:
        b.ssh("i2cset -f -y 5 0x%02x 1 0x60 && i2cset -f -y 5 0x%02x 0" % (a, a))
    b.dtb(board_dts())
    c = dict(WAIT_MSEC=1000, GRACEFUL_TIMEOUT_SEC=10)
    c.update(conf)
    b.conf(**c)
    b.ssh("systemctl reset-failed overheatd; systemctl start overheatd")
    b.wait_state(lambda s: s["temps"] == NORMAL_TEMPS)
    return b.seen()


def wait_seen(b, pred, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if pred(b.seen()):
            return True
        time.sleep(0.2)
    return pred(b.seen())


def crit(b, since):
    """the journal of overheatd at critical level and above"""
    return b.out("journalctl -u overheatd -p crit -o cat --no-pager --since @%d" % since)


def latched(b):
    return b.ssh("test -e " + LATCH).returncode == 0


def no_action(b, s0, secs, what):
    t = time.time() + secs
    while time.time() < t:
        time.sleep(0.5)
    s = b.seen()
    check(s["short"] == s0["short"] and s["long"] == s0["long"] and s["power"] and not latched(b),
          "%s: no press in %d s (short %d, long %d), power on, no marker" % (what, secs, s["short"] - s0["short"], s["long"] - s0["long"]))


def t_ladder(b):
    s0 = ladder_setup(b)
    x = b.state()["xml"]
    since = b.now()
    m = re.search(r'<protection enabled="(\w+)" state="(\w+)" limit="(\d+)" shutdown="(\d+)" poweroff="(\d+)" timeout="(\d+)"', x)
    check(m and m.groups() == ("yes", "idle", "85", "90", "95", "10"), "state file: enabled, idle, 85/90/95, the timeout: %s" % (m and m.groups(),))

    # (tmp105s: even values only) under the shutdown temperature: the alarm only
    all_temps(b, 88)
    check(b.wait_led("timer", 15) == "timer", "88 °C: the alarm (LED)")
    no_action(b, s0, 12, "88 °C")

    # over it: a graceful request, once
    all_temps(b, 92)
    t = time.time()
    check(wait_seen(b, lambda s: s["short"] > s0["short"], 20), "92 °C: a short press of the power button (%.0f s)" % (time.time() - t))
    check(b.seen()["long"] == s0["long"] and not latched(b), "... no long press, no marker yet")
    check("92" in crit(b, since) and re.search(r"(?i)graceful", crit(b, since)), "... journal, critical level: %s" % crit(b, since).strip()[-120:])
    check(wait_seen(b, lambda s: s["long"] > s0["long"] and not s["power"], 30), "still hot after the timeout: the long press, power off (%.0f s)" % (time.time() - t))
    s = b.seen()
    check(s["short"] == s0["short"] + 1, "the short press once (%d)" % (s["short"] - s0["short"]))
    check(latched(b), "the marker %s: %s" % (LATCH, b.out("cat %s 2>/dev/null" % LATCH).strip().replace("\n", " | ")))
    check("92" in b.out("cat %s 2>/dev/null" % LATCH), "... with the temperatures")
    e = b.out("busctl call xyz.openbmc_project.Logging /xyz/openbmc_project/logging org.freedesktop.DBus.ObjectManager GetManagedObjects 2>/dev/null | grep -c -i overheat").strip()
    check(e not in ("", "0"), "a phosphor-logging entry")
    s = b.wait_state(lambda s: "emergency" in s["xml"])
    check("<emergency>" in s["xml"], "state file: the reason: %s" % "".join(re.findall(r"<emergency>[^<]*", s["xml"])))

    # the OS obeys: no long press, no marker
    s0 = ladder_setup(b, GRACEFUL_TIMEOUT_SEC=30)
    b.seq(short="os")
    all_temps(b, 92)
    check(wait_seen(b, lambda s: s["short"] > s0["short"] and not s["power"], 25), "the OS obeys the short press: off")
    time.sleep(3)
    check(b.seen()["long"] == s0["long"] and not latched(b), "... no long press, no marker: nothing to latch")

    # cooling down before the timeout: no escalation
    s0 = ladder_setup(b)
    all_temps(b, 92)
    check(wait_seen(b, lambda s: s["short"] > s0["short"], 20), "92 °C: the short press")
    all_temps(b, 60)
    time.sleep(16)
    s = b.seen()
    check(s["long"] == s0["long"] and s["power"] and not latched(b), "cooled to 60 °C: no long press after the timeout, power on, no marker")
    check(s["short"] == s0["short"] + 1, "... and no second short press (%d)" % (s["short"] - s0["short"]))

    # over the power-off temperature: at once
    s0 = ladder_setup(b)
    since = b.now()
    all_temps(b, 96)
    t = time.time()
    check(wait_seen(b, lambda s: not s["power"], 25), "96 °C: forced off (%.0f s)" % (time.time() - t))
    wait_seen(b, lambda s: s["long"] > s0["long"], 5)
    s = b.seen()
    check(s["long"] == s0["long"] + 1 and s["short"] == s0["short"] and latched(b), "the long press directly (short %d, long %d), the marker" % (s["short"] - s0["short"], s["long"] - s0["long"]))
    check("96" in crit(b, since), "journal, critical level: %s" % crit(b, since).strip()[-120:])

    # power good stays: again, loudly
    s0 = ladder_setup(b)
    b.seq(long="ignore")
    since = b.now()
    all_temps(b, 96)
    check(wait_seen(b, lambda s: s["long"] >= s0["long"] + 2, 60), "power good stays on after the long press: pressed again")
    check(re.search(r"(?i)power good", crit(b, since)), "... journal, critical level: %s" % crit(b, since).strip()[-120:])
    b.seq(long="off")
    check(wait_seen(b, lambda s: not s["power"], 40), "... until it goes")

    # a killed daemon goes on, the second press isn't repeated
    s0 = ladder_setup(b, GRACEFUL_TIMEOUT_SEC=20)
    all_temps(b, 92)
    check(wait_seen(b, lambda s: s["short"] > s0["short"], 20), "92 °C: the short press")
    t = time.time()
    b.ssh("kill -9 " + b.pid())
    check(wait_seen(b, lambda s: not s["power"], 45), "overheatd killed during the ladder: restarted, forced off all the same (%.0f s)" % (time.time() - t))
    s = b.seen()
    check(s["short"] == s0["short"] + 1 and latched(b), "... the short press not repeated (%d), the marker" % (s["short"] - s0["short"]))

    # a press killed half way must not leave the button held down
    s0 = ladder_setup(b)
    b.seq(long="ignore")
    all_temps(b, 96)
    check(wait_seen(b, lambda s: s["pressed"], 25), "96 °C: the long press starts")
    time.sleep(1)
    b.ssh("killall -9 server_pwr_off_hard")
    check(wait_seen(b, lambda s: not s["pressed"], 4), "server_ctl killed in the press: the button isn't left held")
    b.seq(long="off")
    check(wait_seen(b, lambda s: not s["power"], 40), "... and it presses again, off")
    # systemctl restart in the press: the press isn't cut
    s0 = ladder_setup(b)
    all_temps(b, 96)
    check(wait_seen(b, lambda s: s["pressed"], 25), "96 °C: the long press starts")
    b.ssh("systemctl restart overheatd")
    check(wait_seen(b, lambda s: not s["pressed"] and not s["power"], 15), "overheatd restarted in the press: it goes on to the end, off")

    # a slow server_ctl: the PNS lock busy, it waits 15 s for it
    s0 = ladder_setup(b)
    b.ssh("mkdir -p /run/reimu-pns; (flock /run/reimu-pns/lock sleep 25 >/dev/null 2>&1 &)")
    pid = b.pid()
    all_temps(b, 92)
    time.sleep(10)
    p0 = b.state()["poll"]
    time.sleep(5)
    p1 = b.state()["poll"]
    check(p1 >= p0 + 3 and b.pid() == pid, "server_ctl waiting for the PNS lock: polls go on (%d in 5 s), same pid" % (p1 - p0))
    b.ssh("true")
    all_temps(b, 50)
    time.sleep(16)

    # config validation
    since = b.now()
    b.conf(CPU_SHUTDOWN_TEMP=99, CPU_POWEROFF_TEMP=95)
    time.sleep(3)
    m = re.search(r'shutdown="(\d+)" poweroff="(\d+)"', b.state()["xml"])
    check(m and m.groups() == ("90", "95"), "POWEROFF < SHUTDOWN: the defaults (%s)" % (m and m.groups(),))
    check("CPU_SHUTDOWN_TEMP" in b.journal(since), "... said in the journal")
    b.conf(CPU_SHUTDOWN_TEMP=abc_value(), CPU_POWEROFF_TEMP=97)
    time.sleep(3)
    m = re.search(r'shutdown="(\d+)" poweroff="(\d+)"', b.state()["xml"])
    check(m and m.groups() == ("90", "95"), "a broken value: the defaults (%s)" % (m and m.groups(),))
    b.conf(CPU_SHUTDOWN_TEMP=88, CPU_POWEROFF_TEMP=92)
    time.sleep(3)
    m = re.search(r'shutdown="(\d+)" poweroff="(\d+)"', b.state()["xml"])
    check(m and m.groups() == ("88", "92"), "valid values taken at once (%s)" % (m and m.groups(),))


RESET_URL = "/redfish/v1/Systems/system/Actions/ComputerSystem.Reset"


def t_press(b):
    """the units' mutual Conflicts= stop a running press: it must end by itself,
    the button released (the line given up half way stays driven low)"""
    def start():
        s0 = ladder_setup(b)
        b.ssh("systemctl stop overheatd")  # only the web's power actions here
        b.seq(long="off", short="ignore")
        c, _ = rest("POST", RESET_URL, {"ResetType": "ForceOff"})
        check(c in (200, 204) and wait_seen(b, lambda s: s["pressed"], 10), "web ForceOff: the long press starts (%s)" % c)
        time.sleep(1)
        return s0

    start()
    c, _ = rest("POST", RESET_URL, {"ResetType": "On"})
    t = time.time()
    check(c in (200, 204), "web power-on during it (%s)" % c)
    released = wait_seen(b, lambda s: not s["pressed"], 15)
    check(released, "the button is released (%.0f s)" % (time.time() - t))
    time.sleep(4)
    check(not b.seen()["pressed"], "... and stays released")
    check(wait_seen(b, lambda s: s["power"], 25), "the host ends up on (what was asked last)")

    start()
    b.ssh("systemctl stop host-poweroff-hard")
    check(wait_seen(b, lambda s: not s["pressed"], 10), "systemctl stop in the press: released")

    # SIGKILL can't be deferred: the next start of a power unit floats the line
    start()
    b.ssh("killall -9 server_pwr_off_hard")
    b.seq(power=True)
    rest("POST", RESET_URL, {"ResetType": "On"})
    check(wait_seen(b, lambda s: not s["pressed"], 10), "server_ctl killed in the press, then a power-on: released")
    b.ssh("systemctl start overheatd")


def t_selfon(b):
    # the platform comes back on by itself right after the forced off (seen once on the board)
    s0 = ladder_setup(b)
    since = b.now()
    b.seq(selfon=1)
    all_temps(b, 96)
    check(wait_seen(b, lambda s: s["offs"] > s0["offs"], 25), "96 °C: forced off")
    check(wait_seen(b, lambda s: s["ons"] > s0["ons"], 15), "... the platform comes back by itself")
    check(wait_seen(b, lambda s: s["long"] >= s0["long"] + 2 and not s["power"], 25), "... pressed again, off (long %d)" % (b.seen()["long"] - s0["long"]))
    check(latched(b), "... the marker stays")
    check(re.search(r"(?i)by itself", crit(b, since)), "... journal, critical level")
    time.sleep(8)
    s = b.seen()
    check(not s["power"] and s["long"] == s0["long"] + 2, "... stays off, no more presses")
    # a user powers it on in that time: not pressed again
    b.temps(**NORMAL)
    c, _ = rest("POST", "/redfish/v1/Systems/system/Actions/ComputerSystem.Reset", {"ResetType": "On"})
    check(c in (200, 204) and wait_seen(b, lambda s: s["power"], 20), "the web's power-on within those 30 s: on (%s)" % c)
    s1 = b.seen()
    time.sleep(10)
    s = b.seen()
    check(s["power"] and s["long"] == s1["long"] and not latched(b), "... not pressed again, the marker gone")



def t_blind(b):
    # blind for long: said again, with what each sensor gave; the page knows
    since = b.now()
    s0 = ladder_setup(b, PCS_BUS=9, BLIND_ALARM_MIN=1)
    s = b.wait_state(lambda s: s["status"] == "blind", 15)
    c = crit(b, since)
    check("bus 9" in c, "blind: journal, critical level, what each sensor gave: %s" % c.strip()[-100:])
    time.sleep(65)
    c = crit(b, since)
    check(len(re.findall(r"(?i)blind", c)) >= 2 and "still blind" in c, "blind for 1 min (BLIND_ALARM_MIN): said again: %s" % c.strip()[-100:])
    j = overheat_json()
    check(j and j.get("blind_sec", 0) >= 60 and j.get("blind_alarm") == 1 and j.get("status") == "blind", "REST: blind_sec %s, blind_alarm %s" % ((j or {}).get("blind_sec"), (j or {}).get("blind_alarm")))
    check(b.seen()["long"] == s0["long"] and b.seen()["short"] == s0["short"], "... no press")



def abc_value():
    return "9x"


def ttrip_setup(b, **conf):
    """GPIO_TTRIP on a free pin: this board has no such line known to the firmware"""
    b.ssh("grep -q GPIO_TTRIP /etc/gpiotab.common || echo GPIO_TTRIP=%s >> /etc/gpiotab.common" % TTRIP_PIN)
    return ladder_setup(b, **conf)


def t_glitch(b):
    # each from a fresh start: a mutant that acts on one fails that one only
    s0 = ladder_setup(b, GRACEFUL_TIMEOUT_SEC=5)
    all_temps(b, 200)
    no_action(b, s0, 12, "all sensors at an impossible 200 °C")
    s0 = ladder_setup(b, GRACEFUL_TIMEOUT_SEC=5)
    all_temps(b, 4000)
    no_action(b, s0, 8, "garbage (4000 °C)")
    s0 = ladder_setup(b, GRACEFUL_TIMEOUT_SEC=5)
    all_temps(b, 2)
    no_action(b, s0, 8, "all sensors at an impossible 2 °C")
    # plausible, but a jump, for less than the 3 polls of a confirmation
    s0 = ladder_setup(b, GRACEFUL_TIMEOUT_SEC=5)
    for i in range(3):
        all_temps(b, 130)
        time.sleep(1.2)
        b.temps(**NORMAL)
        time.sleep(4)
    no_action(b, s0, 1, "a jump to 130 °C for ~1 poll, 3 times")
    # one sensor, the others fine
    s0 = ladder_setup(b, GRACEFUL_TIMEOUT_SEC=5)
    since = b.now()
    b.temps(t23=120)
    no_action(b, s0, 15, "one sensor at 120 °C, the 5 others at 50..60")
    check("unconfirmed" in b.journal(since), "... said in the journal")
    s0 = ladder_setup(b, GRACEFUL_TIMEOUT_SEC=5)
    b.temps(t23=96)
    no_action(b, s0, 10, "one sensor at 96 °C, the 5 others at 50..60")
    # two sensors, but flapping: never 3 polls in a row
    s0 = ladder_setup(b, GRACEFUL_TIMEOUT_SEC=5)
    for i in range(4):
        b.temps(t20=92, t21=92)
        time.sleep(1.5)
        b.temps(t20=50, t21=52)
        time.sleep(1.5)
    no_action(b, s0, 1, "two sensors flapping 92/50 °C every 1.5 s")

    # blind
    s0 = ladder_setup(b, PCS_BUS=9)
    all_temps(b, 96)
    s = b.wait_state(lambda s: s["status"] == "blind", 15)
    check(s["status"] == "blind", "no sensor answers: blind")
    no_action(b, s0, 15, "blind")

    # TTRIP# alone, briefly
    s0 = ttrip_setup(b)
    b.seq(ttrip=True)
    check(b.wait_led("default-on", 10) == "default-on", "TTRIP#: the LED")
    no_action(b, s0, 10, "TTRIP# for 10 s, the sensors at 50..60 °C")
    b.seq(ttrip=False)
    b.ssh("sed -i /GPIO_TTRIP/d /etc/gpiotab.common")

    # the documented LED test (SIGUSR1: simulated overheat, simulated critical, normal) must
    # never cost the host; also with the line of the real critical state absent, as on this board
    s0 = ladder_setup(b, GRACEFUL_TIMEOUT_SEC=5)
    b.ssh("killall -USR1 overheatd")
    check(b.wait_led("timer", 10) == "timer", "SIGUSR1: simulated overheat, the LED blinks")
    no_action(b, s0, 8, "simulated overheat")
    b.ssh("killall -USR1 overheatd")
    check(b.wait_led("default-on", 10) == "default-on", "SIGUSR1 again: simulated critical, the LED on")
    no_action(b, s0, 12, "simulated critical")
    b.ssh("killall -USR1 overheatd")
    check(b.wait_led("none", 10) == "none", "SIGUSR1 again: normal, the LED off")
    no_action(b, s0, 3, "after the SIGUSR1 cycle")


def t_ttrip(b):
    s0 = ttrip_setup(b)
    all_temps(b, 76)
    time.sleep(8)
    b.seq(ttrip=True)
    t = time.time()
    check(wait_seen(b, lambda s: not s["power"], 20), "TTRIP# with sensors at 76 °C: forced off (%.0f s)" % (time.time() - t))
    check(latched(b) and "TTRIP" in b.out("cat %s 2>/dev/null" % LATCH), "the marker, TTRIP# as the reason")
    s0 = ttrip_setup(b)
    b.seq(ttrip=True)
    t = time.time()
    no_action(b, s0, 20, "TTRIP# for the first 20 s, the sensors cool")
    check(wait_seen(b, lambda s: not s["power"], 25), "TTRIP# held on its own: forced off (%.0f s)" % (time.time() - t))
    check(time.time() - t >= 28, "... after about 30 s")
    b.seq(ttrip=False)
    b.ssh("sed -i /GPIO_TTRIP/d /etc/gpiotab.common")


def rest(method, path, body=None, user="root", password="0penBmc"):
    c = ["curl", "-ks", "-u", "%s:%s" % (user, password), "-X", method, "-H", "Content-Type: application/json",
         "-w", "\n%{http_code}", "https://127.0.0.1:%d%s" % (T.HTTPS, path)]
    if body is not None:
        c += ["-d", json.dumps(body)]
    o = subprocess.run(c, capture_output=True, text=True, timeout=60).stdout
    t, _, code = o.rpartition("\n")
    try:
        return int(code), json.loads(t)
    except ValueError:
        return int(code or 0), None


def overheat_json():
    s, j = rest("GET", OBJ)
    return json.loads(j["data"]["Json"]) if s == 200 else None


def reboot(b):
    start = len(b.console())
    b.ssh("sync; (sleep 1; reboot) >/dev/null 2>&1 &")
    b.wait_console(r"Starting kernel", 120, start)
    ok = b.wait_ssh(400)
    b.use_binary()
    return ok


def t_latch(b):
    s0 = ladder_setup(b)
    all_temps(b, 96)
    check(wait_seen(b, lambda s: not s["power"], 25), "96 °C: forced off")
    check(latched(b), "the marker")
    b.temps(**NORMAL)
    j = overheat_json()
    check(j is not None and j.get("emergency"), "REST: the reason: %s" % (j or {}).get("emergency"))

    # automatic power-on: both ways
    b.ssh("cp /etc/auto_power_on /tmp/apo.save; sed -i 's/^AUTO_POWER_ON=.*/AUTO_POWER_ON=on/' /etc/auto_power_on; sync")
    b.ssh("busctl set-property xyz.openbmc_project.Settings /xyz/openbmc_project/control/host0/power_restore_policy "
          "xyz.openbmc_project.Control.Power.RestorePolicy PowerRestorePolicy s xyz.openbmc_project.Control.Power.RestorePolicy.Policy.AlwaysOn")
    s1 = b.seen()
    check(reboot(b), "BMC rebooted")
    time.sleep(15)
    s = b.seen()
    check(not s["power"] and s["short"] == s1["short"] and s["ons"] == s1["ons"], "AUTO_POWER_ON=on, AlwaysOn: the host stays off, nothing pressed")
    check(latched(b), "the marker kept over the reboot")
    j = b.out("journalctl -b -u allow_host_boot -o cat --no-pager")
    check(re.search(r"(?i)overheat|emergency", j), "allow_host_boot says why: %s" % j.strip().split("\n")[-1])
    f = b.out("systemctl --failed --no-legend --plain | cut -d' ' -f1").split()
    check("phosphor-discover-system-state@0.service" not in f and "allow_host_boot.service" not in f, "no failed unit for it %s" % f)

    # the web's power-on
    for a in PCS:
        b.ssh("i2cset -f -y 5 0x%02x 1 0x60 && i2cset -f -y 5 0x%02x 0" % (a, a))
    s, _ = rest("POST", "/redfish/v1/Systems/system/Actions/ComputerSystem.Reset", {"ResetType": "On"})
    check(s in (200, 204), "Redfish Reset On (the web's power on): %s" % s)
    check(wait_seen(b, lambda s: s["power"], 25), "... the host is on")
    check(not latched(b), "... and the marker gone")

    # the front panel button clears it too
    all_temps(b, 96)
    check(wait_seen(b, lambda s: not s["power"], 25) and latched(b), "96 °C again: forced off, the marker")
    all_temps(b, 50)
    time.sleep(32)
    b.seq(power=True)
    end = time.time() + 15
    while latched(b) and time.time() < end:
        time.sleep(1)
    check(not latched(b), "the front panel button (power good without the BMC): the marker gone")

    # cleared without a power-on
    all_temps(b, 96)
    check(wait_seen(b, lambda s: not s["power"], 25) and latched(b), "96 °C again: forced off, the marker")
    all_temps(b, 50)
    check(b.ssh("busctl call xyz.openbmc_project.Reimu.Pns %s xyz.openbmc_project.Reimu.Overheat ClearEmergency" % OBJ).returncode == 0
          and not latched(b), "D-Bus ClearEmergency: the marker gone, the host stays off (%s)" % b.seen()["power"])
    s = b.wait_state(lambda s: "<emergency>" not in s["xml"], 10)
    check("<emergency>" not in s["xml"], "... not in the state file any more")
    b.ssh("cp /tmp/apo.save /etc/auto_power_on; busctl set-property xyz.openbmc_project.Settings "
          "/xyz/openbmc_project/control/host0/power_restore_policy xyz.openbmc_project.Control.Power.RestorePolicy "
          "PowerRestorePolicy s xyz.openbmc_project.Control.Power.RestorePolicy.Policy.AlwaysOff; sync")


def set_poweroff(on, **kw):
    return rest("POST", OBJ + "/action/SetPowerOff", {"data": [bool(on)]}, **kw)


def t_optout(b):
    s0 = ladder_setup(b)
    pid = b.pid()
    for role in ("ReadOnly", "Operator"):
        name = "oh" + role.lower()
        rest("POST", "/redfish/v1/AccountService/Accounts", {"UserName": name, "Password": "0penBmc-" + role, "RoleId": role, "Enabled": True})
        s, _ = rest("GET", OBJ, user=name, password="0penBmc-" + role)
        c, _ = set_poweroff(False, user=name, password="0penBmc-" + role)
        check(s == 200 and c in (401, 403) and overheat_json().get("poweroff") is True, "%s: reads it (%s), can't change it (%s)" % (role, s, c))
        rest("DELETE", "/redfish/v1/AccountService/Accounts/" + name)

    since = b.now()
    c, _ = set_poweroff(False)
    check(c == 200 and overheat_json().get("poweroff") is False, "admin: SetPowerOff(false) (%s)" % c)
    time.sleep(3)
    check("OVERHEAT_POWEROFF=no" in b.out("cat /etc/overheatd.conf"), "in /etc/overheatd.conf")
    check(b.pid() == pid, "no restart of the daemon")
    check('enabled="no"' in b.state()["xml"], "state file: off, at once")
    check(re.search(r"(?i)power actions.*off", crit(b, since)), "journal, critical level: %s" % crit(b, since).strip()[-120:])

    since = b.now()
    all_temps(b, 96)
    no_action(b, s0, 15, "off, 96 °C")
    check(b.wait_led("timer", 5) == "timer", "... the alarm stays (LED)")
    check(b.state()["status"] == "over-limit", "... status over-limit")
    check(re.search(r"(?i)skipped", crit(b, since)), "... the skipped action, critical level: %s" % crit(b, since).strip()[-120:])
    b.temps(**NORMAL)
    time.sleep(2)

    check(reboot(b), "BMC rebooted")
    for a in PCS:
        b.ssh("i2cset -f -y 5 0x%02x 1 0x60 && i2cset -f -y 5 0x%02x 0" % (a, a))
    check("OVERHEAT_POWEROFF=no" in b.out("cat /etc/overheatd.conf") and overheat_json().get("poweroff") is False, "off after the reboot")
    s0 = b.seen()
    all_temps(b, 96)
    no_action(b, s0, 10, "off after the reboot, 96 °C")
    b.temps(**NORMAL)
    time.sleep(3)

    if os.environ.get("OH_E2E"):
        b.conf(PCS_BUS=9, OVERHEAT_POWEROFF="no")
        b.wait_state(lambda s: s["status"] == "blind", 15)
        e2e(b)
        set_poweroff(False)
        b.conf(OVERHEAT_POWEROFF="no")

    c, _ = set_poweroff(True)
    check(c == 200 and overheat_json().get("poweroff") is True, "admin: on again (%s)" % c)
    time.sleep(2)
    s0 = b.seen()
    all_temps(b, 96)
    check(wait_seen(b, lambda s: not s["power"], 25) and latched(b), "on again, 96 °C: acts")
    b.temps(**NORMAL)
    b.ssh("rm -f " + LATCH)
    b.seq(power=True)
    set_poweroff(True)


def e2e(b):
    import shutil
    d = os.environ["OH_E2E"]
    spec = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "e2e", "overheat.qemu.mjs")
    if not os.path.exists(spec):
        spec = os.path.join(os.path.dirname(os.path.abspath(__file__)), "overheat.qemu.mjs")
    shutil.copyfile(spec, os.path.join(d, ".overheat-e2e.mjs"))
    common = os.path.join(os.path.dirname(spec), "common.mjs")
    if not os.path.exists(os.path.join(d, "common.mjs")) and os.path.exists(common):
        shutil.copyfile(common, os.path.join(d, "common.mjs"))
    r = subprocess.run(["node", ".overheat-e2e.mjs"], cwd=d, capture_output=True, text=True, timeout=300,
                       env=dict(os.environ, BASE="https://127.0.0.1:%d" % T.HTTPS, OUT=os.path.join(T.W, "overheat-shots")))
    print("    " + (r.stdout + r.stderr).strip().replace("\n", "\n    "))
    check(r.returncode == 0, "browser: the toggle, its warning and confirmation, the badge")


def t_agent(b):
    if not os.environ.get("HL_PORT"):
        print("    not run: HL_PORT unset")
        return
    import test_hostlink_qemu as hl
    h = hl.Host()
    try:
        check(hl.t_pair(b, h), "the agent paired")
        h.start()
        check(hl.wait(lambda: (hl.state() or {}).get("agent", {}).get("connected"), 60), "the agent connected")
        s0 = ladder_setup(b)
        all_temps(b, 92)
        check(hl.wait(lambda: "systemctl poweroff" in h.commands(), 25), "92 °C with the agent: OsPower -> systemctl poweroff on the host")
        check(b.seen()["short"] == s0["short"], "... no button press")
        check(wait_seen(b, lambda s: not s["power"] and s["long"] == s0["long"] + 1, 30), "the OS doesn't go down: forced off after the timeout")
        b.temps(**NORMAL)
    finally:
        h.stop()
        h.t.cleanup()


TESTS = [("sensors", t_sensors), ("failures", t_failures), ("ladder", t_ladder), ("glitch", t_glitch), ("ttrip", t_ttrip), ("press", t_press), ("selfon", t_selfon), ("blind", t_blind),
         ("latch", t_latch), ("optout", t_optout), ("agent", t_agent), ("memory", t_memory)]

if __name__ == "__main__":
    b = Bmc("overheatd", fresh("overheatd", os.environ.get("OH_IMG", "ab.mtd"))).start()
    try:
        check(b.wait_ssh(), "up")
        b.use_binary()
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
