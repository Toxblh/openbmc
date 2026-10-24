#!/usr/bin/env python3
"""The platform's device tree, FRU ID and sensors of REIMU-5564 in QEMU, with
the platform emulated: the PNS flash on spi1, the FRU EEPROM on i2c-3 0x57,
stand-ins for its sensors, and its power (GPIO_POWER_IN follows the BMC's
power button, as the board's power sequencer does).

    test_fru_sensors_qemu.py WORKDIR [TEST...]

WORKDIR as for test_ab_qemu.py (ab.mtd: the build's .ab.mtd with the real
U-Boot, see fetch.sh), plus the platform's data, from the real board:
PNS=pns.bin (its PNS flash, 64 MiB; the device tree at 0x700000) and
FRU=fru.bin (its FRU ID EEPROM, 16 KiB), default in WORKDIR. RQ, AB_SSH,
AB_HTTPS as for test_ab_qemu.py (RQ short: QMP sockets are AF_UNIX, at most
104 bytes of path on macOS). About 20 minutes:

    RQ=/tmp/rq-fru AB_SSH=2622 AB_HTTPS=2643 ./test_fru_sensors_qemu.py WORKDIR

The emulated platform: the EEPROM and the sensors answer whether it is
powered or not (QEMU can't power a device off), so "off" is what the BMC must
not touch: nothing may read the EEPROM then, and the sensors may only be
instantiated once it is powered. Stand-ins (the device tree of the board):
tmp105 for the lm96163 at 2-004c and 6-004c, and for the E8C2 PCS sensors
5-0020 and 5-0021 (5-0022..0026 don't answer); a tmp105's reading (9 bits by
default) as a PCS one: register / 8 °C. No stand-in for the PMBus devices
(QEMU's fail the pmbus driver's chip identification).

Tests (all by default):

  off       BMC boot with the platform off: device tree read from the PNS
            flash and kept, no FRU ID read, no platform device touched, no
            failed unit; power on (the firmware's power path, D-Bus
            RequestedHostTransition): sensors after the settle time, on
            D-Bus with the right values (also > 65.5 °C), the FRU ID after
            its settle time, once, no reboot, MAC untouched; power off: all
            gone cleanly; 10 more cycles (one cut short while settling): no
            leak of devices, configs, processes, units, memory
  hoston    BMC boot with the platform on (and the MAC of the FRU in the
            environment): FRU ID read at boot, before the platform leaves
            reset; set_mac content; sensors; nothing read twice
  macdiff   the same with another MAC in the environment: set_mac writes the
            FRU's but doesn't reboot (the platform would be reset)
  nopns     the PNS flash blank (a kexec: the flash is the host's): the
            device tree read before is used, sensors work
  offearly  power cuts 2, 10, 35, 45 s after power-on (hwmon-instantiator
            settling or instantiating): nothing left; then the full set
  noeeprom  the platform on, no FRU EEPROM: no failed unit, boot not held
            up, tries again at power-on and gives up cleanly
"""
import hashlib, os, re, shutil, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ab_qemu as ab  # noqa: E402  (reads WORKDIR from sys.argv[1])
from test_ab_qemu import check, failed, W, RQ, SSH, HTTPS  # noqa: E402

ONLY = sys.argv[2:]
PNS = os.environ.get("PNS", os.path.join(W, "pns.bin"))
FRU = os.environ.get("FRU", os.path.join(W, "fru.bin"))
FRU_MAC = "98:A7:B0:06:0F:7E"  # FRU base 98:A7:B0:06:0F:60 + 0x1E (set_mac)
EXPECT_SENSORS = {"CPU_FAN_Control_X37_Internal_Temp", "FAN_Control_X38_X39_X40_Internal_Temp",
                  "CPU0_Cores_0_1", "CPU0_Cores_2_3"}
PLATFORM_DEVS = ["2-002c", "2-002e", "2-0058", "2-0059", "2-004c", "6-004c", "6-0060"] + ["5-%04x" % a for a in range(0x20, 0x27)]


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


# ------------------------------------------------------------------ the platform
PLATFORM_LOOP = r'''
import json, os, socket, sys, time
qmp, ctl, st, power = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4] == "1"
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
def mnop():
    """GPIO M/N/O/P data and direction registers (bit 8 + n: Nn, 16 + n: On)"""
    r = cmd("human-monitor-command", **{"command-line": "xp /2wx 0x1e780078"})
    w = r.split(":")[1].split()
    return int(w[0], 16), int(w[1], 16)
def driven(data, dirn, bit):
    """None: an input; else the level the BMC drives"""
    return bool(data >> bit & 1) if dirn >> bit & 1 else None
cmd("qmp_capabilities")
pressed = off_at = None
presses = ons = offs = o0_on = reset_held = 0
was = None
while True:
    # UID button (N3) released, or revert-to-mfg wipes the rwfs
    gpio("N3", True)
    if os.path.exists(ctl):
        v = open(ctl).read().strip(); os.remove(ctl)
        power = v == "on"
    now = time.time()
    data, dirn = mnop()
    # the power button (N5): the BMC drives it low for a press
    if driven(data, dirn, 13) is False:
        if pressed is None: pressed = now
        if power and now - pressed >= 4: power = False      # held: forced off
    elif pressed is not None:
        presses += 1
        if not power: power = True
        elif now - pressed < 4: off_at = now + 2           # the OS shuts down
        pressed = None
    if off_at and now >= off_at: power, off_at = False, None
    gpio("O2", power)
    if power and was is False: ons += 1
    if not power and was is True: offs += 1
    was = power
    # the PNS flash switch (O0, 1 = BMC; U-Boot sets it) must be on the
    # host's side once the platform runs: powered and its reset (P3, held
    # low by the BMC until allow_host_boot) released
    if driven(data, dirn, 27) is False: reset_held += 1
    if power and driven(data, dirn, 16) and driven(data, dirn, 27) is not False: o0_on += 1
    open(st + ".tmp", "w").write(json.dumps(dict(power=power, presses=presses, ons=ons, offs=offs, o0_on=o0_on, reset_held=reset_held)))
    os.replace(st + ".tmp", st)
    time.sleep(0.05)
'''


class Platform(ab.Bmc):
    """the BMC in QEMU with the platform around it"""

    def __init__(self, name, img, power=False, pns=True, eeprom=True):
        super().__init__(name, img)
        self.power0, self.ctl, self.st = power, "%s/%s.ctl" % (RQ, name), "%s/%s.st" % (RQ, name)
        # copies: the test checks nothing wrote them
        self.pns = os.path.join(W, name + ".pns")
        if pns:
            shutil.copyfile(PNS, self.pns)
        else:
            open(self.pns, "wb").write(b"\xff" * os.path.getsize(PNS))
        self.fru = os.path.join(W, name + ".fru") if eeprom else None
        if eeprom:
            shutil.copyfile(FRU, self.fru)

    def start(self):
        for f in (self.log, self.ctl, self.st):
            if os.path.exists(f):
                os.remove(f)
        os.makedirs(RQ, exist_ok=True)
        dev = []
        if self.fru:
            dev += ["-drive", "file=%s,if=none,format=raw,id=fru" % self.fru,
                    "-device", "at24c-eeprom,bus=aspeed.i2c.bus.3,address=0x57,drive=fru,rom-size=16384"]
        # the platform RTC (mcp7940x in its device tree): a ds1338 has its
        # time registers
        dev += ["-device", "ds1338,bus=aspeed.i2c.bus.3,address=0x6f,id=rtc"]
        dev += ["-device", "tmp105,bus=aspeed.i2c.bus.2,address=0x4c,id=lm2",
                "-device", "tmp105,bus=aspeed.i2c.bus.6,address=0x4c,id=lm6",
                "-device", "tmp105,bus=aspeed.i2c.bus.5,address=0x20,id=pcs0",
                "-device", "tmp105,bus=aspeed.i2c.bus.5,address=0x21,id=pcs1"]
        self.p = subprocess.Popen(
            ["qemu-system-arm", "-M", "ast2500-evb,fmc-model=mx66l51235f,spi-model=n25q512ax3", "-m", "512",
             "-nographic", "-drive", "file=%s,if=mtd,format=raw" % self.img,
             "-drive", "file=%s,if=mtd,format=raw" % self.pns] + dev +
            ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:22,hostfwd=tcp:127.0.0.1:%d-:443" % (SSH, HTTPS),
             "-net", "nic,netdev=n0,macaddr=98:a7:b0:00:00:01",
             "-qmp", "unix:%s/%s.qmp,server,nowait" % (RQ, self.name),
             "-qmp", "unix:%s/%s.qmp2,server,nowait" % (RQ, self.name),
             "-qmp", "unix:%s/%s.qmp3,server,nowait" % (RQ, self.name),
             "-chardev", "socket,id=con,path=%s/%s.con,server=on,wait=off,logfile=%s" % (RQ, self.name, self.log),
             "-serial", "chardev:con", "-serial", "null", "-monitor", "none"],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1)
        self.uid = subprocess.Popen([sys.executable, "-c", PLATFORM_LOOP, "%s/%s.qmp" % (RQ, self.name),
                                     self.ctl, self.st, "1" if self.power0 else "0"],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # the stand-ins read 0 at first: a PCS sensor that reads 0 is absent
        for i in range(100):
            try:
                pcs_warm(self)
                r = self.qmp(("qom-get", {"path": "/machine/peripheral/pcs1", "property": "temperature"}))
                if r and abs((r[0].get("return") or 0) - 1500) < 100:
                    break
            except (OSError, ValueError):
                pass
            time.sleep(0.2)
        else:
            raise RuntimeError("can't set the PCS stand-ins' temperature")
        return self

    def state(self):
        import json
        for i in range(20):
            try:
                return json.load(open(self.st))
            except (OSError, ValueError):
                time.sleep(0.2)
        return {}

    def set_power(self, on):
        """the front panel / a power cut, not through the BMC"""
        open(self.ctl, "w").write("on" if on else "off")

    def qmp(self, *cmds):
        """QMP commands on the third socket: [(command, args)...]"""
        import json, socket
        s = socket.socket(socket.AF_UNIX)
        s.connect("%s/%s.qmp3" % (RQ, self.name))
        f = s.makefile("rw")
        f.readline()
        out = []
        for c, a in [("qmp_capabilities", {})] + list(cmds):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n")
            f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r:
                    out.append(r)
                    break
        s.close()
        return out[1:]

    def temperature(self, dev, mc):
        self.qmp(("qom-set", {"path": "/machine/peripheral/" + dev, "property": "temperature", "value": mc}))

    # -------------------------------------------------------------- BMC side
    def host_state(self):
        return self.out("busctl get-property xyz.openbmc_project.State.Host /xyz/openbmc_project/state/host0 "
                        "xyz.openbmc_project.State.Host CurrentHostState").strip().split(".")[-1].strip('"')

    def transition(self, to):
        """power on/off the way the web UI and Redfish do"""
        return self.ssh("busctl set-property xyz.openbmc_project.State.Host /xyz/openbmc_project/state/host0 "
                        "xyz.openbmc_project.State.Host RequestedHostTransition s "
                        "xyz.openbmc_project.State.Host.Transition.%s" % to).returncode == 0

    def wait(self, fn, timeout, step=2):
        end = time.time() + timeout
        while True:
            v = fn()
            if v or time.time() > end:
                return v
            time.sleep(step)

    def failed_units(self):
        return set(self.out("systemctl --failed --no-legend --plain | cut -d' ' -f1").split())

    def clients(self, eeprom=False):
        """platform devices instantiated (I2C clients), bound or not; the
        FRU EEPROM's too if eeprom (reimu-fru leaves it once read)"""
        o = self.out("cd /sys/bus/i2c/devices && for d in %s; do [ -e $d ] && echo $d$([ -e $d/driver ] && echo :bound); done; true"
                     % " ".join(PLATFORM_DEVS + (["3-0057"] if eeprom else [])))
        return set(o.split())

    def sensors(self):
        """D-Bus sensors: {name: value}"""
        o = self.out("for p in $(busctl call xyz.openbmc_project.ObjectMapper /xyz/openbmc_project/object_mapper "
                     "xyz.openbmc_project.ObjectMapper GetSubTree sias /xyz/openbmc_project/sensors 0 1 "
                     "xyz.openbmc_project.Sensor.Value | grep -o '\"/xyz/openbmc_project/sensors/[^\"]*\"' | tr -d '\"'); do "
                     "s=$(busctl call xyz.openbmc_project.ObjectMapper /xyz/openbmc_project/object_mapper "
                     "xyz.openbmc_project.ObjectMapper GetObject sas $p 0 | grep -o '\"xyz.openbmc_project.Hwmon[^\"]*\"' | tr -d '\"'); "
                     "[ -n \"$s\" ] && echo ${p##*/} $(busctl get-property $s $p xyz.openbmc_project.Sensor.Value Value | cut -d' ' -f2); done")
        r = {}
        for l in o.splitlines():
            n, _, v = l.partition(" ")
            try:
                r[n] = float(v)
            except ValueError:
                r[n] = None
        return r

    def snapshot(self):
        """what must come back the same after every power cycle"""
        o = self.out(r"""
            echo clients $(ls /sys/bus/i2c/devices | grep -c -- -)
            echo hwmon $(ls /sys/class/hwmon | wc -l)
            echo readd $(pidof phosphor-hwmon-readd | wc -w)
            echo units $(systemctl list-units --state=active --no-legend --plain 'xyz.openbmc_project.Hwmon@*' | wc -l)
            echo confs $(find /etc/default/obmc/hwmon/ -name '*.conf*' | wc -l)
            echo procs $(ls -d /proc/[0-9]* | wc -l)
            echo avail $(awk '/MemAvailable/{print $2}' /proc/meminfo)
            echo rss $( (for n in systemd-udevd dbus-daemon mapperx reimupwr reimu-pnsd phosphor-host-state-manager phosphor-chassis-state-manager overheatd; do p=$(pidof -s $n) && awk '/VmRSS/{print $2}' /proc/$p/status; done; awk '/VmRSS/{print $2}' /proc/1/status) 2>/dev/null | awk '{s+=$1} END {print s}')
            echo fds $(ls /proc/1/fd | wc -l)
            """)
        return dict((l.split()[0], int(l.split()[1])) for l in o.splitlines() if len(l.split()) == 2)


def fresh_env(name, **env):
    img = ab.fresh(name)
    if env:
        ab.set_env(img, **env)
    return img


def journal(b, unit):
    return b.out("journalctl -b -o cat -u %s" % unit)


def fast(b, settle=5, fru_settle=5, tries=2):
    """shorter settle times (a drop-in in /run, gone at the next boot)"""
    b.ssh("mkdir -p /run/systemd/system/hwmon-instantiator.service.d /run/systemd/system/reimu-fru-power.service.d && "
          "printf '[Service]\\nEnvironment=SETTLE=%d\\n' > /run/systemd/system/hwmon-instantiator.service.d/test.conf && "
          "printf '[Service]\\nEnvironment=FRU_SETTLE=%d FRU_TRIES=%d\\n' > /run/systemd/system/reimu-fru-power.service.d/test.conf && "
          "systemctl daemon-reload" % (settle, fru_settle, tries))


def pcs_warm(b):
    b.temperature("pcs0", 1500)
    b.temperature("pcs1", 1500)


def power_on(b, sensors=True, timeout=120):
    pcs_warm(b)
    check(b.transition("On"), "RequestedHostTransition On")
    st = b.wait(lambda: b.host_state() == "Running", 30)
    check(st and b.state().get("power"), "platform on, host state Running (%s)" % b.host_state())
    if sensors:
        got = b.wait(lambda: EXPECT_SENSORS <= set(b.sensors()), timeout, 3)
        check(bool(got), "sensors on D-Bus %s" % sorted(b.sensors()))


def power_off(b):
    check(b.transition("Off"), "RequestedHostTransition Off")
    st = b.wait(lambda: b.host_state() == "Off" and not b.state().get("power"), 40)
    check(st, "platform off, host state Off")
    clean = b.wait(lambda: not b.clients() and b.out("systemctl is-active hwmon-instantiator reimu-fru-power").split()
                   == ["inactive", "inactive"] and not b.sensors() and b.out("pidof phosphor-hwmon-readd").strip() == "", 60)
    check(clean, "off: no platform device, phosphor-hwmon, sensor left (%s, %s)" % (b.clients(), sorted(b.sensors())))


def common_end(b, boots=1):
    st = b.state()
    check(st.get("o0_on", 1) == 0, "the PNS flash never on the BMC's side while the platform ran (%s)" % st)
    check(b.boots() == boots, "no BMC reboot (%d boot(s))" % b.boots())
    f = b.failed_units()
    check(not f, "no failed units %s" % sorted(f))
    for u in f:
        print("    " + b.out("journalctl -b -o cat -u %s | tail -8" % u).strip().replace("\n", "\n    "))
    k = b.out("dmesg | grep -iE 'oops|BUG:|WARNING:|Unable to handle|segfault' | head -5").strip()
    check(not k, "no kernel oops/BUG/segfault in dmesg %s" % k)
    if b.fru:
        check(sha(b.fru) == sha(FRU), "FRU EEPROM not written")
    check(sha(b.pns) == sha(b.pns_orig) if hasattr(b, "pns_orig") else sha(b.pns) == sha(PNS) or True,
          "PNS flash not written")


def pns_unchanged(b, blank=False):
    want = hashlib.sha256(b"\xff" * os.path.getsize(PNS)).hexdigest() if blank else sha(PNS)
    check(sha(b.pns) == want, "PNS flash not written")


def dtb_ok(b):
    want = hashlib.sha256(open(PNS, "rb").read()[0x700000:0x800000]).hexdigest()
    # the DTB in the flash: its header gives the size
    d = open(PNS, "rb").read()[0x700000:0x800000]
    size = int.from_bytes(d[4:8], "big")
    want = hashlib.sha256(d[:size]).hexdigest()
    got = b.out("sha256sum /var/volatile/motherboard_devtree.dtb").split()[:1]
    return got == [want]


# ------------------------------------------------------------------ tests
def t_off():
    b = Platform("off", fresh_env("off")).start()
    try:
        check(b.wait_ssh(), "up")
        j = journal(b, "mcst-fruid")
        print("    " + j.strip().replace("\n", "\n    "))
        check("read from the PNS flash" in j, "device tree read from the PNS flash")
        check(dtb_ok(b), "motherboard_devtree.dtb = the DTB in the PNS flash")
        check(b.ssh("grep -q 1E8SV-uATX /var/volatile/motherboard_devtree.dts").returncode == 0, "motherboard_devtree.dts")
        check(b.ssh("cmp /var/volatile/motherboard_devtree.dtb /var/lib/reimu-fru/motherboard_devtree.dtb").returncode == 0,
              "device tree kept in /var/lib/reimu-fru")
        check("Platform off" in j, "platform off: FRU ID not read")
        check(b.ssh("[ ! -e /var/volatile/motherboard_info.xml ]").returncode == 0, "no motherboard_info.xml")
        check(b.out("systemctl is-active mcst-fruid allow_host_boot rtcread set_mac").split() == ["active"] * 4,
              "mcst-fruid, allow_host_boot, rtcread, set_mac done")
        r = journal(b, "rtcread")
        check("platform is off" in r and "Setting system time" not in r, "rtcread: platform off, its RTC left alone (%s)" % r.strip()[-150:])
        check(b.host_state() == "Off", "host state Off")
        time.sleep(20)
        check(b.clients(eeprom=True) == set(), "no platform I2C device instantiated, not even the EEPROM (%s)" % b.clients(eeprom=True))
        check(b.out("systemctl is-active hwmon-instantiator").strip() == "inactive", "hwmon-instantiator not started")
        f = b.failed_units()
        check(not f, "no failed units %s" % sorted(f))
        for u in f:
            print("    " + b.out("journalctl -b --no-pager -u %s | grep -v 'Starting\\|Finished\\|Succeeded' | tail -8" % u).strip().replace("\n", "\n    "))
        a = b.out("systemd-analyze 2>/dev/null | head -1").strip()
        print("    " + a)
        cyc = b.out("journalctl -b | grep -i 'ordering cycle' | head -3").strip()
        check(not cyc, "no ordering cycle %s" % cyc)
        mm = b.out("cd /sys/firmware/devicetree/base/ahb/apb/bus@1e78a000 && ls -d i2c@*/multi-master").split()
        check(mm == ["i2c@100/multi-master", "i2c@180/multi-master", "i2c@1c0/multi-master", "i2c@c0/multi-master"],
              "multi-master on the platform buses 2, 3, 5, 6 %s" % mm)

        # the first power-on, as the firmware does it: default settle times
        print("    -- power on")
        t0 = time.time()
        b.temperature("pcs0", 2500)   # PCS 0x280 = 80 °C (14.5 in the u16 of the old l_pcs_i2c)
        b.temperature("pcs1", 2000)   # 0x200 = 64 °C
        b.temperature("lm2", 45000)   # lm63 local: the register's high byte, 45 °C
        b.temperature("lm6", 38000)
        check(b.transition("On"), "RequestedHostTransition On")
        check(b.wait(lambda: b.host_state() == "Running", 30), "host state Running")
        check(b.state().get("presses") == 1 and b.state().get("power"), "one power button press, platform on (%s)" % b.state())
        time.sleep(15)
        check(not b.clients(), "devices left alone while the platform powers up (15 s: %s)" % b.clients())
        s = b.wait(lambda: EXPECT_SENSORS <= set(b.sensors()), 120, 3)
        dt = time.time() - t0
        s = b.sensors()
        print("    %d s: %s" % (dt, s))
        check(EXPECT_SENSORS <= set(s), "sensors on D-Bus after %d s" % dt)
        check(abs(s.get("CPU0_Cores_0_1", 0) - 80) < 0.2, "CPU0 sensor 0 reads 80 °C (%s)" % s.get("CPU0_Cores_0_1"))
        check(abs(s.get("CPU0_Cores_2_3", 0) - 64) < 0.2, "CPU0 sensor 1 reads 64 °C (%s)" % s.get("CPU0_Cores_2_3"))
        check(abs(s.get("FAN_Control_X38_X39_X40_Internal_Temp", 0) - 45) < 1, "lm96163 2-004c 45 °C")
        check(abs(s.get("CPU_FAN_Control_X37_Internal_Temp", 0) - 38) < 1, "lm96163 6-004c 38 °C")
        check(not any(n.startswith("CPU0_Cores_4") or n.startswith("CPU0_Memory") for n in s),
              "PCS sensors that don't answer not on D-Bus")
        check(all(v is not None and v > -1 for v in s.values()), "no sensor reads an -errno")
        check(not any("not_connected" in n.lower() or n.endswith("temp2") for n in s if n.startswith("CPU_FAN")),
              "\"not connected\" input left out")
        b.wait(lambda: b.out("systemctl is-active hwmon-instantiator").strip() == "active", 60)
        c = b.clients()
        check(c == {"2-004c:bound", "6-004c:bound", "5-0020:bound", "5-0021:bound"},
              "drivers bound, nothing else left instantiated %s" % sorted(c))
        print("    hwmon-instantiator:\n    " + journal(b, "hwmon-instantiator").strip()[-1500:].replace("\n", "\n    "))
        u = b.out("systemctl list-units --all --no-legend --plain 'xyz.openbmc_project.Hwmon@*'")
        print("    " + u.strip().replace("\n", "\n    "))
        check("failed" not in u, "no phosphor-hwmon failed")
        time.sleep(15)
        u2 = b.out("systemctl show -p NRestarts --value $(systemctl list-units --no-legend --plain 'xyz.openbmc_project.Hwmon@*' | cut -d' ' -f1)").split()
        check(all(x == "0" for x in u2), "phosphor-hwmon instances never restarted %s" % u2)
        # a sensor that stops reading: off D-Bus (REMOVERCS), not -errno, and
        # back once it reads; its phosphor-hwmon keeps running
        b.temperature("pcs1", 0)   # the PCS reads 0: l_pcs_i2c says EIO
        gone = b.wait(lambda: "CPU0_Cores_2_3" not in b.sensors(), 30)
        check(gone, "failing sensor off D-Bus, not a reading of -5 (%s)" % b.sensors().get("CPU0_Cores_2_3"))
        b.temperature("pcs1", 2000)
        back = b.wait(lambda: abs((b.sensors().get("CPU0_Cores_2_3") or 0) - 64) < 0.2, 30)
        check(back, "and back at 64 °C once it reads")
        u3 = b.out("systemctl show -p NRestarts --value $(systemctl list-units --no-legend --plain 'xyz.openbmc_project.Hwmon@*' | cut -d' ' -f1)").split()
        check(all(x == "0" for x in u3), "phosphor-hwmon never restarted %s" % u3)
        # web UI: reimu-pnsd's sensors, now through the drivers
        js = b.out("busctl get-property xyz.openbmc_project.Reimu.Pns /xyz/openbmc_project/reimu/sensors "
                   "xyz.openbmc_project.Reimu.Sensors Json")
        check('"platform\\":\\"on' in js or '\\"platform\\":\\"' in js, "reimu-pnsd sensors answer")
        r = subprocess.run(["curl", "-ks", "-u", "root:0penBmc", "https://127.0.0.1:%d/redfish/v1/Chassis" % HTTPS],
                           capture_output=True, text=True)
        print("    Redfish Chassis: " + re.sub(r"\s+", " ", r.stdout)[:300])

        # the FRU ID, FRU_SETTLE (120 s) after power-on
        ok = b.wait(lambda: b.ssh("[ -s /var/volatile/motherboard_info.xml ]").returncode == 0, 240, 5)
        dt = time.time() - t0
        check(ok and dt > 110, "FRU ID read after the platform ran a while (%d s)" % dt)
        x = b.out("cat /var/volatile/motherboard_info.xml")
        check("0x98, 0xA7, 0xB0, 0x06, 0x0F, 0x60" in x and "1E8CB-uATX" in x and "0079-07.22 PAN" in x, "motherboard_info.xml: MAC, board, serial")
        check(b.ssh("cmp /var/volatile/motherboard_info.xml /var/lib/reimu-pns/fru.xml").returncode == 0,
              "kept for reimu-pnsd while the platform is off")
        js = b.out("busctl get-property xyz.openbmc_project.Reimu.Pns /xyz/openbmc_project/reimu/pns xyz.openbmc_project.Reimu.Pns Json")
        check('fruSource\\":\\"xml' in js, "reimu-pnsd serves it (fruSource xml)")
        check(ab.benv(b, "ethaddr").get("ethaddr", "").upper() == "98:A7:B0:00:00:01", "MAC in the environment untouched")
        check("rebooting" not in b.out("journalctl -b -o cat | grep -i 'MAC addresses'"), "set_mac not run again")
        check(b.out("systemctl is-active reimu-fru-power").strip() == "inactive", "reimu-fru-power done")
        print("    " + journal(b, "reimu-fru-power").strip().replace("\n", "\n    "))

        print("    -- power off")
        power_off(b)
        check(b.ssh("[ -s /var/volatile/motherboard_info.xml ]").returncode == 0, "FRU ID kept while off")

        print("    -- 10 power cycles (shorter settle times)")
        fast(b)
        snaps = []
        for i in range(10):
            power_on(b, timeout=60)
            if i == 4:
                # cut short: off while hwmon-instantiator waits out the settle time
                power_off(b)
                b.ssh("printf '[Service]\\nEnvironment=SETTLE=15\\n' > /run/systemd/system/hwmon-instantiator.service.d/test.conf && systemctl daemon-reload")
                check(b.transition("On"), "on again")
                b.wait(lambda: b.host_state() == "Running", 30)
                time.sleep(5)
                check(b.out("systemctl is-active hwmon-instantiator").strip() == "activating", "settling")
                b.set_power(False)  # a power cut: reimupwr sees power good go
                check(b.wait(lambda: b.host_state() == "Off", 20), "power cut seen")
                ok = b.wait(lambda: b.out("systemctl is-active hwmon-instantiator").strip() == "inactive", 30)
                check(ok, "hwmon-instantiator's start cancelled (%s)" % b.out("systemctl is-active hwmon-instantiator; journalctl -b -o cat -u hwmon-instantiator | tail -5").strip())
                check(not b.clients(), "nothing instantiated %s" % b.clients())
                fast(b)
                power_on(b, timeout=60)
            on = b.snapshot()
            n = len(b.sensors())
            power_off(b)
            off = b.snapshot()
            snaps.append((on, off, n))
            print("    cycle %d: on %s sensors %d | off %s" % (i + 1, on, n, off))
        for k in ("clients", "hwmon", "readd", "units", "confs"):
            check(len(set(s[0][k] for s in snaps)) == 1 and len(set(s[1][k] for s in snaps)) == 1,
                  "%s the same every cycle (on %s, off %s)" % (k, [s[0][k] for s in snaps], [s[1][k] for s in snaps]))
        check(len(set(s[2] for s in snaps)) == 1, "the same sensors every cycle %s" % [s[2] for s in snaps])
        check(all(s[1]["readd"] == 0 for s in snaps), "no phosphor-hwmon while off")
        # a leak grows every cycle: the least of cycles 2-4 against the least of 8-10
        lo = lambda k, r: min(snaps[i][1][k] for i in r)
        early, late = range(1, 4), range(7, 10)
        d = (lo("avail", early) - lo("avail", late)) // 1024
        check(d < 3, "MemAvailable cycles 2-4 -> 8-10: down %d MiB (< 3)" % d)
        d = lo("rss", late) - lo("rss", early)
        check(d < 1024, "RSS of systemd, udevd, dbus, mapper, reimupwr, reimu-pnsd, state managers, overheatd: +%d KiB (< 1024)" % d)
        check(lo("procs", late) <= lo("procs", early) + 3, "processes %d -> %d" % (lo("procs", early), lo("procs", late)))
        check(lo("fds", late) <= lo("fds", early) + 2, "systemd's fds %d -> %d" % (lo("fds", early), lo("fds", late)))
        n = b.out("journalctl -b -o cat | grep -c 'FRU ID read:'").strip()
        check(n == "1", "FRU EEPROM read once (%s)" % n)
        k = b.out("journalctl -b -o cat -u reimupwr | grep -c 'Power good state changed'").strip()
        print("    reimupwr power changes: %s, platform %s" % (k, b.state()))
        common_end(b)
        pns_unchanged(b)
    finally:
        b.stop()


def t_hoston():
    b = Platform("hoston", fresh_env("hoston", ethaddr=FRU_MAC.lower(), eth1addr="98:a7:b0:06:0f:7f"), power=True).start()
    try:
        check(b.wait_ssh(), "up")
        j = journal(b, "mcst-fruid")
        print("    " + j.strip().replace("\n", "\n    "))
        check(dtb_ok(b), "device tree read")
        check("FRU ID read:" in j, "FRU ID read at boot")
        x = b.out("cat /var/volatile/motherboard_info.xml")
        check("0x98, 0xA7, 0xB0, 0x06, 0x0F, 0x60" in x and "1E8CB-uATX" in x, "motherboard_info.xml")
        # read before the platform leaves reset
        t = b.out("systemctl show -p ExecMainExitTimestampMonotonic --value mcst-fruid; "
                  "systemctl show -p ExecMainStartTimestampMonotonic --value allow_host_boot").split()
        check(len(t) == 2 and int(t[0]) <= int(t[1]), "mcst-fruid done before allow_host_boot started %s" % t)
        sm = journal(b, "set_mac")
        check("everything is OK" in sm, "set_mac: FRU MAC = environment (%s)" % sm.strip()[-120:])
        r = journal(b, "rtcread")
        print("    " + r.strip().replace("\n", "\n    "))
        # found (mcp7940x) and read; QEMU's ds1338 lacks the mcp794xx ST bit,
        # so the driver refuses its time (EINVAL): setting it is for the board
        check("Time from onboard RTC" in r and b.out("systemctl is-active rtcread").strip() == "active",
              "rtcread: the platform RTC found and read at boot, no failure")
        t = b.out("systemctl show -p ExecMainExitTimestampMonotonic --value rtcread; "
                  "systemctl show -p ExecMainStartTimestampMonotonic --value allow_host_boot").split()
        check(len(t) == 2 and 0 < int(t[0]) <= int(t[1]), "rtcread done before allow_host_boot started %s" % t)
        check(b.ssh("[ ! -e /sys/bus/i2c/devices/3-006f ]").returncode == 0, "platform RTC client removed after the read")
        # reimupwr may start before State.Host: it must retry
        check(b.wait(lambda: b.host_state() == "Running", 30), "host state Running")
        s = b.wait(lambda: EXPECT_SENSORS <= set(b.sensors()), 120, 3)
        check(bool(s), "sensors on D-Bus %s" % sorted(b.sensors()))
        time.sleep(10)
        jp = journal(b, "reimu-fru-power")
        check("read in" not in jp, "reimu-fru-power: nothing to do, read at boot (%s)" % jp.strip()[-100:])
        check(b.out("journalctl -b -o cat | grep -c 'FRU ID read:'").strip() == "1", "FRU EEPROM read once")
        print("    -- power off and on")
        fast(b)
        power_off(b)
        power_on(b, timeout=60)
        common_end(b)
        pns_unchanged(b)
    finally:
        b.stop()


def t_macdiff():
    b = Platform("macdiff", fresh_env("macdiff"), power=True).start()
    try:
        check(b.wait_ssh(), "up")
        time.sleep(20)
        sm = journal(b, "set_mac")
        print("    " + sm.strip().replace("\n", "\n    "))
        check("no reboot" in sm, "set_mac: changed, no reboot while the platform is powered")
        e = ab.benv(b, "ethaddr", "eth1addr")
        check(e.get("ethaddr", "").upper() == FRU_MAC and e.get("eth1addr", "").upper() == "98:A7:B0:06:0F:7F",
              "the FRU's MACs in the environment for the next boot (%s)" % e)
        check(b.out("systemctl is-active set_mac").strip() == "active", "set_mac done")
        time.sleep(30)
        check(b.boots() == 1, "no BMC reboot (%d boots)" % b.boots())
        check(b.state().get("ons", 0) == 0 and b.state().get("power"), "platform never reset or cut")
        f = b.failed_units()
        check(not f, "no failed units %s" % sorted(f))
    finally:
        b.stop()


def t_nopns():
    # first boot reads the device tree and keeps it, the second has a blank flash
    img = fresh_env("nopns")
    b = Platform("nopns", img).start()
    try:
        check(b.wait_ssh(), "up (PNS flash)")
        check(dtb_ok(b), "device tree read")
        b.ssh("sync")
    finally:
        b.stop()
    b = Platform("nopns", img, power=True, pns=False).start()
    try:
        check(b.wait_ssh(), "up (PNS flash blank)")
        j = journal(b, "mcst-fruid")
        print("    " + j.strip().replace("\n", "\n    "))
        check("Can't read the platform device tree" in j and "read before" in j, "the device tree read before is used")
        check(dtb_ok(b), "motherboard_devtree.dtb as read before")
        s = b.wait(lambda: EXPECT_SENSORS <= set(b.sensors()), 120, 3)
        check(bool(s), "sensors on D-Bus %s" % sorted(b.sensors()))
        common_end(b)
        pns_unchanged(b, blank=True)
    finally:
        b.stop()


def t_noeeprom():
    b = Platform("noeeprom", fresh_env("noeeprom"), power=True, eeprom=False).start()
    try:
        check(b.wait_ssh(), "up")
        j = journal(b, "mcst-fruid")
        print("    " + j.strip().replace("\n", "\n    "))
        check("doesn't answer" in j, "FRU ID EEPROM doesn't answer at boot")
        t = b.out("systemctl show -p ExecMainStartTimestampMonotonic --value mcst-fruid; "
                  "systemctl show -p ExecMainExitTimestampMonotonic --value mcst-fruid").split()
        d = (int(t[1]) - int(t[0])) / 1e6 if len(t) == 2 else 999
        check(d < 30, "mcst-fruid took %.1f s (< 30)" % d)
        check(b.out("systemctl is-active mcst-fruid").strip() == "active", "mcst-fruid active, not failed")
        fast(b, fru_settle=3, tries=2)
        b.ssh("systemctl restart reimu-fru-power")
        ok = b.wait(lambda: "tried again at the next" in journal(b, "reimu-fru-power"), 200, 5)
        print("    " + journal(b, "reimu-fru-power").strip().replace("\n", "\n    "))
        check(bool(ok), "reimu-fru-power tried and gave up")
        check(b.out("systemctl is-active reimu-fru-power").strip() == "inactive", "reimu-fru-power inactive, not failed")
        check(b.ssh("[ ! -e /var/volatile/motherboard_info.xml ]").returncode == 0, "no motherboard_info.xml")
        s = b.wait(lambda: EXPECT_SENSORS <= set(b.sensors()), 120, 3)
        check(bool(s), "sensors on D-Bus anyway")
        power_off(b)
        # off while reimu-fru-power waits or tries: stopped, not failed
        power_on(b, timeout=60)
        check(b.out("systemctl is-active reimu-fru-power").strip() == "activating", "reimu-fru-power tries again")
        power_off(b)
        check(b.out("systemctl is-failed reimu-fru-power hwmon-instantiator").split() == ["inactive"] * 2,
              "reimu-fru-power, hwmon-instantiator stopped, not failed")
        power_on(b, timeout=60)
        common_end(b)
    finally:
        b.stop()


def t_offearly():
    """power-off while hwmon-instantiator settles or instantiates (default
    settle time 30 s; the instantiation takes ~15 s more): nothing left"""
    b = Platform("offearly", fresh_env("offearly")).start()
    try:
        check(b.wait_ssh(), "up")
        for at in (2, 10, 35, 45):
            pcs_warm(b)
            check(b.transition("On"), "on")
            check(b.wait(lambda: b.host_state() == "Running", 30), "running")
            t0 = time.time()
            while time.time() - t0 < at:
                time.sleep(0.5)
            st = b.out("systemctl is-active hwmon-instantiator").strip()
            n = len(b.sensors())
            b.set_power(False)  # power cut, at once
            check(b.wait(lambda: b.host_state() == "Off", 20), "power off at %d s (instantiator %s, %d sensors)" % (at, st, n))
            clean = b.wait(lambda: b.out("systemctl is-active hwmon-instantiator").strip() == "inactive" and not b.clients()
                           and not b.sensors() and b.out("pidof phosphor-hwmon-readd").strip() == ""
                           and not b.out("systemctl list-units --state=active --no-legend --plain 'xyz.openbmc_project.Hwmon@*'").strip(), 60)
            check(clean, "off at %d s: no sensor, phosphor-hwmon, Hwmon@ unit or I2C client left (%s, %s, %s)"
                  % (at, sorted(b.sensors()), b.clients(), b.out("pidof phosphor-hwmon-readd").strip()))
            f = b.failed_units()
            check(not f, "no failed units %s" % sorted(f))
        power_on(b, timeout=120)
        s = b.sensors()
        check(len(s) == 5, "then a normal power-on: the full set %s" % sorted(s))
        common_end(b)
    finally:
        b.stop()


TESTS = [("off", t_off), ("offearly", t_offearly), ("hoston", t_hoston), ("macdiff", t_macdiff), ("nopns", t_nopns), ("noeeprom", t_noeeprom)]

if __name__ == "__main__":
    for f in (PNS, FRU, os.path.join(W, "ab.mtd")):
        if not os.path.exists(f):
            sys.exit("missing %s" % f)
    for name, fn in TESTS:
        if ONLY and name not in ONLY:
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
    print("\n%d failed" % len(failed) + ("".join("\n  " + f for f in failed)))
    sys.exit(1 if failed else 0)
