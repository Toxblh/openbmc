#!/usr/bin/env python3
"""reimu-hostd: the Elbrus host's agent for the REIMU BMC (reimu-hostlink).

Like the host agents of the big servers (iDRAC Service Module, HPE AMS,
XClarity): the host tells its BMC what it is and how it is doing, and the BMC
may ask it for a few things.

  * inventory: CPU, NUMA, memory and its DIMMs (EDAC), PCIe devices (link
    speed/width, driver, NUMA node), disks, network cards, the OS
  * every INTERVAL s: load, memory, hwmon temperatures, ECC errors (EDAC) per
    controller/csrow/DIMM, the video card (amdgpu: temperatures, fan, power,
    clocks, load, VRAM, PCIe link), disk temperatures and SMART health
  * from the BMC, only this list (ALLOW), never a command line: the video
    card's fan auto / manual DUTY % (never below GPU_FAN_FLOOR) / "turbo"
    (100 % for N minutes); the OS shut down or rebooted (systemctl, or shutdown without systemd);
    modprobe e2k_edac (or edac_e2k)
  * on its own (host-local, in ALLOW as usb_hub, no BMC message): the BMC's
    virtual USB hub (1d6b:0107: the KVM keyboard/mouse, virtual media) kept
    out of autosuspend: the host suspends it while it is empty and the BMC
    can't wake it (its remote wakeup doesn't work on the board), so after a
    BMC reboot or kexec the KVM input would never come back. The udev rule
    (hub-rule install, by install.sh and reimu-host.sh) and a check every 2 s
    (power/control "auto" -> "on"); its state in the inventory and metrics
  * on its own too (bmc_owns_i2c in ALLOW, default on; the BMC can't command
    it): the BMC is the only master of the platform I2C buses the host shares
    (host i2c-0 = BMC i2c-6, host i2c-1 = BMC i2c-2): both sides polling put
    0x98 in every writable register of an lm96163. Every 2 s any pwm-fan and
    any driver bound to those clients (BUS_OWNED) is unbound and pwmd stopped;
    these buses' hwmon is never read; bus-owner install keeps the modules out
    and pwmd disabled. With the BMC down or on its stock firmware nobody drives
    the fans then (README.md). Not enforceable from an x86 OS under Lintel

The video card's fan: the policy the BMC asks for is kept by the agent. The
driver resets it after a GPU reset (amdgpu hang: pwm1_enable back to 2, the
fans stopped while the IOHUB behind the card heated to 90 °C), and the card's
hwmon may come back under another number: it is checked every 2 s and set
again. With no word from the BMC for LINK_LOST_SEC a manual policy stays, at
least GPU_FAN_LINK_LOST (the IOHUB behind the card needs the air), unless the
card is hot or its temperature unknown: then, and when the agent stops, the
fan goes back to the driver's automatic control.

The link: TLS to the BMC's TCP BMC_PORT (5564), the BMC's certificate pinned
(its SHA-256, from the pairing), the agent proven by HMAC-SHA256 of the BMC's
challenge with the key it got at pairing. Tried in this order: the BMC's IPv6
link-local address (from BMC_MAC, on every interface that is up: the board's
internal LAN works without a cable and without DHCP), BMC link-local addresses
found by a multicast probe (UDP ff02::1 port BMC_PORT), then BMC_IPV4. One
JSON object per line, {"v": 1, "t": TYPE, ...}; unknown types and fields are
ignored. See README.md next to this file.

  sudo ./reimu-hostd.py install --pair 'reimu1:...'   pair (the string from
                     the BMC's web page Хост → «Подключить агент хоста»),
                     install into /usr/local/lib/reimu, enable the service
  sudo ./reimu-hostd.py pair --pair 'reimu1:...'      pair again only
  sudo ./reimu-hostd.py uninstall
       ./reimu-hostd.py inventory | metrics            print what it would send
       ./reimu-hostd.py status
       ./reimu-hostd.py run                            the service itself
       ./reimu-hostd.py restore-auto                   the GPU fan to the driver
  sudo ./reimu-hostd.py hub-rule install|remove       the udev rule keeping the
                     BMC's USB hub (KVM keyboard/mouse) out of autosuspend
  sudo ./reimu-hostd.py bus-owner install|remove      the BMC the only master of
                     the shared I2C buses: the modules kept out, pwmd disabled
                     (its state written down; remove gives both back)

Config /etc/reimu-hostd.conf (KEY=VALUE), the key /etc/reimu-hostd/secret
(root only). REIMU_HOSTD_ROOT=DIR: every /sys, /proc, /etc, /dev path under
DIR and no command executed (logged to DIR/commands.log): the tests.
"""
import base64, binascii, glob, gzip, hashlib, hmac, io, json, os, re, select, shutil, signal, socket, ssl, struct, subprocess
import sys, time, uuid

VERSION = "1.0"
PROTO = 1
CONF = "/etc/reimu-hostd.conf"
SECRET = "/etc/reimu-hostd/secret"
LIB = "/usr/local/lib/reimu"
UNIT = "/etc/systemd/system/reimu-hostd.service"
MAX_LINE = 64 * 1024        # from the BMC
PING_SEC = 10
SMART_SEC = 600
INVENTORY_SEC = 3600
GPU_TICK_SEC = 2
OPS = ("gpu_fan", "gpu_turbo", "os_power", "edac_load", "diag")
# the diag op's report on the wire (the BMC takes lines up to 128 KiB)
DIAG_MAX_B64 = 96 * 1024
DIAG_LOG_TAIL = 256 * 1024
# done by the agent itself, never asked for by the BMC (ALLOW can turn it off)
LOCAL_OPS = ("usb_hub", "bmc_owns_i2c")

# ------------------------------------------------------------------ logging
_logger = None
_HEX_SECRET = re.compile(r"[0-9a-fA-F]{32,}")


def set_logger(fn):
    global _logger
    _logger = fn


def log(msg):
    """to the journal (stdout); long hex strings (keys, tokens, MACs of the
    challenge) never: masked"""
    msg = _HEX_SECRET.sub(lambda m: m.group(0)[:4] + "…", str(msg))
    if _logger:
        _logger(msg)
    else:
        print(msg, flush=True)


def _gzip(data):
    """no timestamp in it (gzip.compress has mtime= only from Python 3.8)"""
    b = io.BytesIO()
    with gzip.GzipFile(fileobj=b, mode="wb", compresslevel=6, mtime=0) as f:
        f.write(data)
    return b.getvalue()


def log_error(msg):
    """an error, in any log; under systemd the journal has its priority too"""
    log(("<3>" if os.environ.get("JOURNAL_STREAM") else "") + "ERROR: " + str(msg))


# ------------------------------------------------------------------ config
DEFAULTS = {
    "BMC": "",                # HOST[:PORT] instead of the list below (tests, a routed BMC)
    "BMC_MAC": "",            # the BMC's eth0 MAC: its link-local address
    "BMC_IPV4": "",
    "BMC_PORT": 5564,
    "IFACES": "",             # where to look for the BMC's link-local address (default: all up)
    "DISCOVERY": True,        # multicast probe for the BMC on the interfaces
    "INTERVAL": 10,           # s between metrics
    "GPU_FAN_FLOOR": 30,      # % the manual duty never goes below
    "LINK_LOST_SEC": 60,      # no word from the BMC this long: see GpuFan
    "GPU_FAN_LINK_LOST": 40,  # % held meanwhile while the card is cool; 0: to the driver
    "ALLOW": set(OPS + LOCAL_OPS),
    "EDAC_AUTOLOAD": False,   # modprobe e2k_edac at start when EDAC is absent
    "SMART": True,            # smartctl (when installed) every SMART_SEC
}


def _bool(v):
    return str(v).strip().lower() in ("1", "yes", "true", "on")


def parse_config(text):
    c = dict(DEFAULTS)
    c["ALLOW"] = set(DEFAULTS["ALLOW"])
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        k, v = k.strip(), v.strip().strip('"').strip("'")
        if k not in DEFAULTS:
            continue
        d = DEFAULTS[k]
        try:
            if isinstance(d, bool):
                c[k] = _bool(v)
            elif isinstance(d, int):
                c[k] = int(v)
            elif isinstance(d, set):
                c[k] = set(x for x in re.split(r"[\s,]+", v) if x in OPS + LOCAL_OPS)
            else:
                c[k] = v
        except ValueError:
            log("config: %s=%s: not a number, %s kept" % (k, v, d))
    c["INTERVAL"] = min(max(c["INTERVAL"], 2), 3600)
    c["GPU_FAN_FLOOR"] = min(max(c["GPU_FAN_FLOOR"], 20), 100)
    c["LINK_LOST_SEC"] = min(max(c["LINK_LOST_SEC"], 15), 3600)
    c["GPU_FAN_LINK_LOST"] = min(max(c["GPU_FAN_LINK_LOST"], 0), 100)
    return c


def load_config(path=CONF):
    try:
        with open(path) as f:
            return parse_config(f.read())
    except OSError:
        return parse_config("")


def load_secret(path=SECRET):
    try:
        with open(path) as f:
            return dict(l.strip().split("=", 1) for l in f if "=" in l and not l.startswith("#"))
    except OSError:
        return {}


def save_secret(path, kv):
    d = os.path.dirname(path)
    os.makedirs(d, mode=0o700, exist_ok=True)
    tmp = path + ".new"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write("# reimu-hostd: the key of the pairing with the BMC (root only)\n")
        for k, v in kv.items():
            f.write("%s=%s\n" % (k, v))
        f.flush()
        os.fsync(f.fileno())
    os.chmod(tmp, 0o600)
    os.rename(tmp, path)


# ------------------------------------------------------------------ files
DMI_MEMORY_TYPES = {0x13: "SDRAM", 0x14: "DDR", 0x15: "DDR2", 0x18: "DDR3", 0x1A: "DDR4", 0x1B: "LPDDR", 0x1C: "LPDDR2",
                    0x1D: "LPDDR3", 0x1E: "LPDDR4", 0x22: "DDR5", 0x23: "LPDDR5"}


def _dmi_memory_modules(t):
    """the installed modules out of a raw SMBIOS table: type 17 records, offsets of
    the spec (3.3). A record that doesn't end in the table isn't one."""
    def word(rec, off, size=2):
        return int.from_bytes(rec[off:off + size], "little") if off + size <= len(rec) else None

    out = []
    i = 0
    while i + 4 <= len(t):
        kind, length = t[i], t[i + 1]
        end = t.find(b"\0\0", i + length) if length >= 4 else -1
        if end < 0 or i + length > len(t):
            break
        rec = t[i:i + length]
        strings = t[i + length:end].split(b"\0")
        i = end + 2
        if kind == 127:
            break
        if kind != 17:
            continue
        size = word(rec, 0x0C)
        if size in (None, 0, 0xFFFF):
            continue
        if size == 0x7FFF:
            mb = word(rec, 0x1C, 4)
            if mb is None:
                continue
        else:
            mb = (size & 0x7FFF) / 1024 if size & 0x8000 else size
        gb = mb / 1024
        rated = word(rec, 0x15)
        conf = word(rec, 0x20)
        if rated == 0xFFFF:
            rated = word(rec, 0x54, 4)
        if conf == 0xFFFF:
            conf = word(rec, 0x58, 4)
        n = rec[0x1A] if len(rec) > 0x1A else 0
        part = strings[n - 1].decode("ascii", "replace").strip() if 0 < n <= len(strings) else ""
        detail = word(rec, 0x13) or 0
        out.append({"size_gb": int(gb) if gb == int(gb) else round(gb, 2),
                    "type": DMI_MEMORY_TYPES.get(rec[0x12]) if len(rec) > 0x12 else None,
                    "registered": bool(detail & 0x2000), "rated_mts": rated or None, "configured_mts": conf or None,
                    "part": part or None})
    return out


class Fs:
    """the machine's files, or a fake tree under root (tests)"""

    def __init__(self, root=""):
        self.root = root.rstrip("/")

    def p(self, path):
        return self.root + path

    def un(self, path):
        return path[len(self.root):] if self.root and path.startswith(self.root) else path

    def read(self, path, default=None):
        try:
            with open(self.p(path), "rb") as f:
                return f.read(1 << 20).decode("utf-8", "replace").strip()
        except OSError:
            return default

    def readb(self, path, limit=4096):
        try:
            with open(self.p(path), "rb") as f:
                return f.read(limit)
        except OSError:
            return None

    def int(self, path, default=None, base=10):
        v = self.read(path)
        try:
            return int(v, base)
        except (TypeError, ValueError):
            return default

    def write(self, path, value):
        with open(self.p(path), "w") as f:
            f.write(str(value))

    def write_atomic(self, path, value):
        """the whole new file or the old one, also across a power cut"""
        p = self.p(path)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p + ".new", "w") as f:
            f.write(str(value))
            f.flush()
            os.fsync(f.fileno())
        os.rename(p + ".new", p)

    def glob(self, pattern):
        return sorted(self.un(x) for x in glob.glob(self.p(pattern)))

    def exists(self, path):
        return os.path.exists(self.p(path))

    def isdir(self, path):
        return os.path.isdir(self.p(path))

    def link_name(self, path):
        try:
            return os.path.basename(os.readlink(self.p(path)))
        except OSError:
            return None

    def realpath(self, path):
        return self.un(os.path.realpath(self.p(path)))


PCI_ADDR = re.compile(r"[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]")


def _natkey(s):
    return [int(x) if x.isdigit() else x for x in re.split(r"(\d+)", s)]


SYS_PATH = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"


def run_cmd(argv, timeout=10):
    """a command of the allow-list (fixed argv, no shell): (rc, output)"""
    exe = shutil.which(argv[0], path=SYS_PATH)
    if not exe:
        return 127, "%s: not installed" % argv[0]
    try:
        p = subprocess.Popen([exe] + list(argv[1:]), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, close_fds=True)
        out, _ = p.communicate(timeout=timeout)
        return p.returncode, out.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        return 124, "%s: timed out" % argv[0]
    except OSError as e:
        return 126, str(e)


def fake_runner(root):
    """REIMU_HOSTD_ROOT: commands are only written down (the allow-list's ones
    "succeed", the probes - lspci, ip, smartctl - are "not installed")"""
    def run(argv, timeout=10):
        with open(os.path.join(root, "commands.log"), "a") as f:
            f.write(" ".join(argv) + "\n")
        if argv[0] == "systemctl" and os.environ.get("REIMU_SYSTEMCTL"):  # the installer's tests
            return run_cmd([os.environ["REIMU_SYSTEMCTL"]] + list(argv[1:]), timeout)
        if argv[0] in ("systemctl", "modprobe"):
            return 0, ""
        return 127, "%s: not run (REIMU_HOSTD_ROOT)" % argv[0]
    return run


# ------------------------------------------------------------------ init systems
INIT_DIRS = ("/etc/init.d", "/etc/rc.d/init.d")
RC_BASES = ("/etc/rc.d", "/etc")


def detect_init(fs, which=None):
    """systemd only when it runs here (as install.sh decides)"""
    which = which or (lambda n: shutil.which(n, path=SYS_PATH))
    return "systemd" if fs.exists("/run/systemd/system") and which("systemctl") else "sysv"


def init_system(fs):
    """REIMU_INIT: the installer's word; a test tree without it: systemd, as
    install.sh's REIMU_ROOT"""
    forced = os.environ.get("REIMU_INIT")
    if forced in ("systemd", "sysv"):
        return forced
    return "systemd" if getattr(fs, "root", "") else detect_init(fs)


class Init:
    """services, by whichever init system this host has. SysV: the script,
    `service`; at boot update-rc.d, chkconfig, insserv, or the rc?.d links
    themselves, which are also what "enabled" means there"""

    def __init__(self, fs, run):
        self.fs, self.run, self.kind = fs, run, init_system(fs)

    def script(self, name):
        for d in INIT_DIRS:
            if self.fs.exists(d + "/" + name):
                return d + "/" + name
        return None

    def ctl(self, action, name, timeout=60):
        if self.kind == "systemd":
            return self.run(["systemctl", action, name], timeout=timeout)
        s = self.script(name)
        if s:
            r = self.run([s, action], timeout=timeout)
            if r[0] not in (126, 127):
                return r
        return self.run(["service", name, action], timeout=timeout)

    def status(self, name):
        if self.kind == "systemd":
            return self.run(["systemctl", "--no-pager", "status", name + ".service"], timeout=30)
        return self.ctl("status", name, timeout=30)

    def _rc_base(self):
        for b in RC_BASES:
            if self.fs.exists(b + "/rc3.d"):
                return b
        return None

    def _links(self, name, kind="S", levels="2-5"):
        out = []
        for b in RC_BASES:
            out += [x for x in self.fs.glob("%s/rc[%s].d/%s[0-9][0-9]%s" % (b, levels, kind, name)) if x not in out]
        return out

    def _flip(self, links, to):
        """S20name <-> K20name, in place"""
        for x in links:
            d, n = os.path.split(x)
            try:
                os.rename(self.fs.p(x), self.fs.p(d + "/" + to + n[1:]))
            except OSError as e:
                log("%s: %s" % (x, e))

    def _tool(self, cmds):
        """the commands of a boot tool, in order: True when it did them; False
        when it failed or isn't here"""
        for c in cmds:
            if self.run(c, timeout=30)[0] != 0:
                return False
        return True

    def state(self, name):
        """enabled, disabled, absent, or systemd's own word"""
        if self.kind == "systemd":
            _, out = self.run(["systemctl", "is-enabled", name], timeout=15)
            for line in reversed(out.splitlines()):
                if line.strip() in PWMD_WORDS:
                    return line.strip()
            return "absent"
        if self._links(name):
            return "enabled"
        return "disabled" if self.script(name) else "absent"

    def enable(self, name):
        if self.kind == "systemd":
            return self.run(["systemctl", "enable", name], timeout=60)[0] == 0
        done = (self._tool([["update-rc.d", name, "defaults"], ["update-rc.d", name, "enable"]])
                or self._tool([["chkconfig", "--add", name], ["chkconfig", name, "on"]])
                or self._tool([["insserv", name]]))
        if self._links(name):
            return True
        off = self._links(name, "K")
        if off:
            self._flip(off, "S")
        elif self._rc_base() and not done:
            b, target = self._rc_base(), self.script(name) or INIT_DIRS[0] + "/" + name
            try:
                for lev, n in (("3", "S99"), ("5", "S99"), ("0", "K01"), ("6", "K01")):
                    os.makedirs(self.fs.p("%s/rc%s.d" % (b, lev)), exist_ok=True)
                    link = self.fs.p("%s/rc%s.d/%s%s" % (b, lev, n, name))
                    if not os.path.lexists(link):
                        os.symlink(target, link)
            except OSError as e:
                log("%s at boot: %s" % (name, e))
        return bool(self._links(name)) or done

    def disable(self, name):
        if self.kind == "systemd":
            return self.run(["systemctl", "disable", name], timeout=60)[0] == 0
        done = (self._tool([["update-rc.d", name, "disable"]]) or self._tool([["chkconfig", name, "off"]])
                or self._tool([["insserv", "-r", name]]))
        self._flip(self._links(name), "K")  # whatever the tool said, the links are the truth
        if self._links(name):
            return False
        return bool(done or self._rc_base() or not self.script(name))

    def enable_now(self, name):
        if self.kind == "systemd":
            return self.run(["systemctl", "enable", "--now", name], timeout=60)[0] == 0
        ok = self.enable(name)
        self.ctl("start", name)
        return ok

    def disable_now(self, name):
        if self.kind == "systemd":
            return self.run(["systemctl", "disable", "--now", name], timeout=60)[0] == 0
        ok = self.disable(name)
        self.ctl("stop", name)  # a running one is the BusOwner's to check
        return ok

    def remove_boot(self, name):
        if self.kind == "systemd":
            return
        self._tool([["update-rc.d", "-f", name, "remove"]])
        self._tool([["chkconfig", "--del", name]])
        self._tool([["insserv", "-r", name]])
        for x in self._links(name, "[SK]", "0-6"):
            try:
                os.remove(self.fs.p(x))
            except OSError:
                pass

    def power(self, action):
        """(rc, what was done, its output): action poweroff or reboot"""
        if self.kind == "systemd":
            rc, out = self.run(["systemctl", action], timeout=30)
            return rc, "systemctl " + action, out
        argv = ["shutdown", "-h" if action == "poweroff" else "-r", "now"]
        rc, out = self.run(argv, timeout=30)
        if rc in (126, 127):
            argv = [action]
            rc, out = self.run(argv, timeout=30)
        return rc, " ".join(argv), out


# ------------------------------------------------------------------ collection
def _kv_colon(text):
    out = {}
    for line in (text or "").splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            out.setdefault(k.strip(), v.strip())
    return out


def _meminfo(text):
    out = {}
    for line in (text or "").splitlines():
        m = re.match(r"(?:Node \d+ )?(\w+):\s+(\d+)", line)
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


def _milli(v):
    return None if v is None else round(v / 1000.0, 2)


class Collector:
    def __init__(self, fs, run, cfg):
        self.fs, self.run, self.cfg = fs, run, cfg
        self.smart = {}       # disk -> (time, health, temp)
        self.lspci = None

    # ---- inventory
    def inventory(self):
        return {"os": self.os_info(), "cpu": self.cpu(), "memory": self.memory(), "pci": self.pci(),
                "disks": self.disks(), "nics": self.nics(), "gpu": self.gpu_info(), "usb": self.usb(),
                "bmc_usb_hub": bmc_usb_hub_state(self.fs), "i2c_owner": bus_owner_state(self.fs, self.cfg),
                "agent": {"version": VERSION, "proto": PROTO}}

    def os_info(self):
        fs = self.fs
        rel = {}
        for line in (fs.read("/etc/os-release") or "").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                rel[k] = v.strip().strip('"')
        arch = fs.read("/proc/sys/kernel/arch") or (os.uname().machine if not fs.root else "")
        # the boot chain: PNS (boot firmware) -> native E2K Linux, or Lintel
        # (the x86 binary translator) -> an x86 OS; this agent is the native one
        boot = "native-e2k" if arch == "e2k" else ("x86" if arch in ("x86_64", "i686", "i386") else arch)
        up = fs.read("/proc/uptime")
        return {"name": rel.get("PRETTY_NAME") or rel.get("NAME"), "id": rel.get("ID"), "version": rel.get("VERSION_ID"),
                "kernel": fs.read("/proc/sys/kernel/osrelease"), "arch": arch, "boot": boot,
                "hostname": fs.read("/proc/sys/kernel/hostname"),
                "uptime": int(float(up.split()[0])) if up else None}

    def cpu(self):
        text = self.fs.read("/proc/cpuinfo") or ""
        blocks = [_kv_colon(b) for b in text.split("\n\n") if b.strip()]
        procs = [b for b in blocks if "processor" in b]
        first = procs[0] if procs else (blocks[0] if blocks else {})
        mhz = first.get("cpu MHz")
        numa = []
        for n in self.fs.glob("/sys/devices/system/node/node[0-9]*"):
            mi = _meminfo(self.fs.read(n + "/meminfo"))
            numa.append({"node": int(re.sub(r"\D", "", os.path.basename(n))), "cpus": self.fs.read(n + "/cpulist"),
                         "mem_mib": mi.get("MemTotal", 0) // 1024})
        sockets = len(set(b.get("physical id") for b in procs if "physical id" in b)) or None
        return {"model": first.get("model name") or first.get("cpu model") or first.get("model"),
                "vendor": first.get("vendor_id"), "threads": len(procs), "sockets": sockets,
                "mhz": int(float(mhz)) if mhz else None, "numa": numa}

    def memory(self):
        mi = _meminfo(self.fs.read("/proc/meminfo"))
        dimms = []
        mcs = self.fs.glob("/sys/devices/system/edac/mc/mc[0-9]*")
        for mc in mcs:
            for d in sorted(self.fs.glob(mc + "/dimm[0-9]*") + self.fs.glob(mc + "/rank[0-9]*"), key=_natkey):
                dimms.append({"mc": int(mc.rsplit("mc", 1)[1]), "dimm": int(re.sub(r"\D", "", os.path.basename(d))),
                              "label": self.fs.read(d + "/dimm_label"), "size_mib": self.fs.int(d + "/size"),
                              "type": self.fs.read(d + "/dimm_mem_type"), "edac_mode": self.fs.read(d + "/dimm_edac_mode"),
                              "location": self.fs.read(d + "/dimm_location")})
        # a registered controller is ECC present, whatever it tells per DIMM:
        # the board's e2k_edac has only mc0 (size_mb 0, no dimm/csrow entries)
        ctls = [{"mc": int(mc.rsplit("mc", 1)[1]), "name": self.fs.read(mc + "/mc_name"),
                 "size_mib": self.fs.int(mc + "/size_mb", 0)} for mc in sorted(mcs, key=_natkey)]
        out = {"total_mib": mi.get("MemTotal", 0) // 1024, "ecc": "edac" if mcs else "unknown", "dimms": dimms,
               "controllers": ctls}
        table = self.fs.readb("/sys/firmware/dmi/tables/DMI", 1 << 20)     # root only
        if table:
            out["modules"] = _dmi_memory_modules(table)
        return out

    def _lspci(self):
        if self.lspci is None:
            self.lspci = {}
            rc, out = self.run(["lspci", "-vmm", "-D"], timeout=10)
            if rc == 0:
                for b in out.split("\n\n"):
                    kv = {}
                    for line in b.splitlines():
                        if ":\t" in line:
                            k, v = line.split(":\t", 1)
                            kv.setdefault(k, v)
                    if "Slot" in kv:
                        self.lspci[kv["Slot"]] = kv
        return self.lspci

    def pci(self):
        names = self._lspci()
        out = []
        for d in self.fs.glob("/sys/bus/pci/devices/*"):
            slot = os.path.basename(d)
            hx = lambda k: (self.fs.read(d + "/" + k) or "").replace("0x", "")
            e = {"slot": slot, "vendor": hx("vendor"), "device": hx("device"), "class": hx("class"),
                 "subsys": "%s:%s" % (hx("subsystem_vendor"), hx("subsystem_device")), "rev": hx("revision"),
                 "driver": self.fs.link_name(d + "/driver"), "numa": self.fs.int(d + "/numa_node", -1)}
            parent = os.path.basename(os.path.dirname(self.fs.realpath(d)))
            if PCI_ADDR.fullmatch(parent):
                e["parent"] = parent
            sp = self.fs.read(d + "/current_link_speed")
            if sp and sp != "Unknown":
                e["link"] = {"speed": sp, "width": self.fs.int(d + "/current_link_width"),
                             "max_speed": self.fs.read(d + "/max_link_speed"),
                             "max_width": self.fs.int(d + "/max_link_width")}
            n = names.get(slot)
            if n:
                e["name"], e["vendor_name"], e["class_name"] = n.get("Device"), n.get("Vendor"), n.get("Class")
            out.append(e)
        return out

    def _disk_bus(self, dev):
        """where a disk hangs: its PCI function, the ATA port or USB device"""
        path = self.fs.realpath(dev)
        fns = PCI_ADDR.findall(path)
        bus = {"pci": fns[-1]} if fns else {}
        m = re.search(r"/ata(\d+)/", path)
        if m:
            bus["ata"] = int(m.group(1))
        usb = re.findall(r"/(\d+-[\d.]+)(?=/)", path)
        if usb:
            bus["usb"] = usb[-1]
        return bus or None

    def usb(self):
        """USB devices past the root hubs, no serial numbers"""
        out = []
        for d in self.fs.glob("/sys/bus/usb/devices/*"):
            name = os.path.basename(d)
            if name.startswith("usb") or ":" in name or not self.fs.exists(d + "/idVendor"):
                continue
            out.append({"name": name, "id": "%s:%s" % (self.fs.read(d + "/idVendor"), self.fs.read(d + "/idProduct")),
                        "product": self.fs.read(d + "/product"), "speed": self.fs.int(d + "/speed"),
                        "hub": (self.fs.int(d + "/maxchild") or 0) > 0})
        return out

    def _disk_dirs(self):
        for b in self.fs.glob("/sys/block/*"):
            name = os.path.basename(b)
            if re.match(r"(loop|ram|zram|sr|fd|dm-|md|nbd)", name) or not self.fs.exists(b + "/device"):
                continue
            yield name, b

    def disks(self):
        out = []
        for name, b in self._disk_dirs():
            dev = b + "/device"
            serial = self.fs.read(dev + "/serial")
            if not serial:
                vpd = self.fs.readb(dev + "/vpd_pg80")
                if vpd and len(vpd) > 4:
                    serial = vpd[4:4 + vpd[3]].decode("ascii", "replace").strip()
            vendor = (self.fs.read(dev + "/vendor") or "").strip()
            out.append({"name": name, "model": (self.fs.read(dev + "/model") or "").strip() or None,
                        "vendor": vendor if vendor and vendor != "ATA" else None, "serial": serial or None,
                        "firmware": self.fs.read(dev + "/firmware_rev") or self.fs.read(dev + "/rev"),
                        "size": (self.fs.int(b + "/size") or 0) * 512,
                        "rotational": self.fs.read(b + "/queue/rotational") == "1",
                        "transport": "nvme" if name.startswith("nvme") else ("mmc" if name.startswith("mmc") else "ata/scsi"),
                        "bus": self._disk_bus(dev)})
        return out

    def _addrs(self):
        """interface -> addresses: `ip -j addr` when there, else the IPv6 ones"""
        out = {}
        rc, txt = self.run(["ip", "-j", "addr"], timeout=5)
        if rc == 0:
            try:
                for i in json.loads(txt):
                    out[i.get("ifname")] = ["%s/%s" % (a.get("local"), a.get("prefixlen")) if a.get("family") == "inet"
                                            else a.get("local") for a in i.get("addr_info", [])]
                return out
            except (ValueError, AttributeError, TypeError):
                out = {}
        for line in (self.fs.read("/proc/net/if_inet6") or "").splitlines():
            f = line.split()
            if len(f) == 6:
                try:
                    a = socket.inet_ntop(socket.AF_INET6, binascii.unhexlify(f[0]))
                except (ValueError, OSError):
                    continue
                out.setdefault(f[5], []).append(a)
        return out

    def nics(self):
        addrs = self._addrs()
        out = []
        for n in self.fs.glob("/sys/class/net/*"):
            name = os.path.basename(n)
            if self.fs.read(n + "/type") == "772" or not self.fs.exists(n + "/device"):
                continue
            sp = self.fs.int(n + "/speed")
            out.append({"name": name, "mac": self.fs.read(n + "/address"), "state": self.fs.read(n + "/operstate"),
                        "carrier": self.fs.read(n + "/carrier") == "1", "speed": sp if sp and sp > 0 else None,
                        "mtu": self.fs.int(n + "/mtu"), "driver": self.fs.link_name(n + "/device/driver"),
                        "pci": self.fs.link_name(n + "/device"),
                        "addrs": addrs.get(name, [])})
        return out

    def gpu_dev(self):
        for d in self.fs.glob("/sys/bus/pci/devices/*"):
            if self.fs.link_name(d + "/driver") == "amdgpu":
                return d
        return None

    def gpu_info(self):
        d = self.gpu_dev()
        if not d:
            return None
        return {"slot": os.path.basename(d), "vbios": self.fs.read(d + "/vbios_version"),
                "vram_total": self.fs.int(d + "/mem_info_vram_total"),
                "name": (self._lspci().get(os.path.basename(d)) or {}).get("Device")}

    # ---- metrics
    def metrics(self, now):
        fs = self.fs
        mi = _meminfo(fs.read("/proc/meminfo"))
        la = (fs.read("/proc/loadavg") or "0 0 0").split()
        up = fs.read("/proc/uptime")
        return {"uptime": int(float(up.split()[0])) if up else None, "load": [float(x) for x in la[:3]],
                "mem": {"total_kib": mi.get("MemTotal"), "avail_kib": mi.get("MemAvailable"),
                        "swap_total_kib": mi.get("SwapTotal"), "swap_free_kib": mi.get("SwapFree")},
                "temps": self.temps(), "edac": self.edac(), "gpu": self.gpu(), "disks": self.disk_health(now),
                "bmc_usb_hub": bmc_usb_hub_state(self.fs), "i2c_owner": bus_owner_state(self.fs, self.cfg)}

    def temps(self):
        out = []
        for h in self.fs.glob("/sys/class/hwmon/hwmon*"):
            chip = self.fs.read(h + "/name") or os.path.basename(h)
            # the platform's shared buses are the BMC's: a read here is a
            # transfer on them, even with a driver left bound
            if chip == "pwmfan" or SHARED_BUS_CLIENT.match(os.path.basename(self.fs.realpath(h + "/device"))):
                continue
            for t in sorted(self.fs.glob(h + "/temp*_input"), key=_natkey):
                v = self.fs.int(t)
                if v is None:
                    continue
                base = t[:-len("_input")]
                out.append({"chip": chip, "label": self.fs.read(base + "_label") or os.path.basename(base),
                            "value": _milli(v), "crit": _milli(self.fs.int(base + "_crit"))})
        return out

    def edac(self):
        fs = self.fs
        mcs = []
        ce_t = ue_t = 0
        for mc in fs.glob("/sys/devices/system/edac/mc/mc[0-9]*"):
            e = {"mc": int(mc.rsplit("mc", 1)[1]), "name": fs.read(mc + "/mc_name"),
                 "ce": fs.int(mc + "/ce_count", 0), "ue": fs.int(mc + "/ue_count", 0),
                 "ce_noinfo": fs.int(mc + "/ce_noinfo_count", 0), "ue_noinfo": fs.int(mc + "/ue_noinfo_count", 0),
                 "dimms": [], "csrows": []}
            for d in sorted(fs.glob(mc + "/dimm[0-9]*") + fs.glob(mc + "/rank[0-9]*"), key=_natkey):
                e["dimms"].append({"dimm": int(re.sub(r"\D", "", os.path.basename(d))), "label": fs.read(d + "/dimm_label"),
                                   "ce": fs.int(d + "/dimm_ce_count", 0), "ue": fs.int(d + "/dimm_ue_count", 0)})
            for r in sorted(fs.glob(mc + "/csrow[0-9]*"), key=_natkey):
                e["csrows"].append({"csrow": int(re.sub(r"\D", "", os.path.basename(r))),
                                    "ce": fs.int(r + "/ce_count", 0), "ue": fs.int(r + "/ue_count", 0)})
            ce_t += e["ce"] + e["ce_noinfo"]
            ue_t += e["ue"] + e["ue_noinfo"]
            mcs.append(e)
        return {"loaded": bool(mcs), "mcs": mcs, "ce_total": ce_t, "ue_total": ue_t}

    def gpu(self):
        d = self.gpu_dev()
        if not d:
            return None
        fs = self.fs
        hws = [h for h in fs.glob(d + "/hwmon/hwmon*") if fs.read(h + "/name") == "amdgpu"]
        g = {"slot": os.path.basename(d)}
        if hws:
            h = hws[0]
            labels = {}
            for t in fs.glob(h + "/temp*_input"):
                base = t[:-len("_input")]
                labels[fs.read(base + "_label") or os.path.basename(base)] = base
            for lab, key in (("edge", "edge"), ("junction", "junction"), ("mem", "mem")):
                b = labels.get(lab)
                g["temp_" + key] = _milli(fs.int(b + "_input")) if b else None
                g["crit_" + key] = _milli(fs.int(b + "_crit")) if b else None
            pwm, mx = fs.int(h + "/pwm1"), fs.int(h + "/pwm1_max", 255) or 255
            g.update({"fan_rpm": fs.int(h + "/fan1_input"), "fan_max_rpm": fs.int(h + "/fan1_max"), "pwm": pwm,
                      "pwm_enable": fs.int(h + "/pwm1_enable"),
                      "pwm_pct": round(pwm * 100.0 / mx, 1) if pwm is not None else None})
            pw = fs.int(h + "/power1_average")
            if pw is None:
                pw = fs.int(h + "/power1_input")
            cap = fs.int(h + "/power1_cap")
            g["power_w"] = round(pw / 1e6, 1) if pw is not None else None
            g["power_cap_w"] = round(cap / 1e6, 1) if cap is not None else None
            freqs = {}
            for f in fs.glob(h + "/freq*_input"):
                base = f[:-len("_input")]
                freqs[fs.read(base + "_label") or os.path.basename(base)] = fs.int(f)
            g["sclk_mhz"] = freqs["sclk"] // 1000000 if freqs.get("sclk") is not None else None
            g["mclk_mhz"] = freqs["mclk"] // 1000000 if freqs.get("mclk") is not None else None
        g.update({"busy_pct": fs.int(d + "/gpu_busy_percent"), "mem_busy_pct": fs.int(d + "/mem_busy_percent"),
                  "vram_used": fs.int(d + "/mem_info_vram_used"), "vram_total": fs.int(d + "/mem_info_vram_total"),
                  "link": {"speed": fs.read(d + "/current_link_speed"), "width": fs.int(d + "/current_link_width")}})
        return g

    def disk_health(self, now):
        temps = {}
        for h in self.fs.glob("/sys/class/hwmon/hwmon*"):
            if self.fs.read(h + "/name") in ("nvme", "drivetemp"):
                temps[self.fs.realpath(h + "/device")] = _milli(self.fs.int(h + "/temp1_input"))
        out = []
        for name, b in self._disk_dirs():
            dev = self.fs.realpath(b + "/device")
            t = temps.get(dev)
            smart = None
            if self.cfg["SMART"]:
                c = self.smart.get(name)
                if c is None or now - c[0] >= SMART_SEC:
                    c = (now,) + self._smart(name)
                    self.smart[name] = c
                smart = c[1]
                if t is None:
                    t = c[2]
            out.append({"name": name, "temp": t, "smart": smart})
        return out

    def _smart(self, name):
        rc, out = self.run(["smartctl", "-j", "-H", "-A", "/dev/" + name], timeout=20)
        if rc == 127 or rc == 126:
            return None, None
        try:
            j = json.loads(out)
        except ValueError:
            return None, None
        st = (j.get("smart_status") or {}).get("passed")
        temp = (j.get("temperature") or {}).get("current")
        return (None if st is None else ("PASSED" if st else "FAILED")), temp


# CONFIG_EDAC_E2K builds e2k_edac.ko (the board's ALT 6.1 kernel); edac_e2k
# is kept as the other spelling some kernels use
EDAC_MODULES = ("e2k_edac", "edac_e2k")


def edac_modprobe(run):
    """(ok, text): the first of EDAC_MODULES that loads"""
    errs = []
    for m in EDAC_MODULES:
        rc, out = run(["modprobe", m], timeout=30)
        if rc == 0:
            return True, "modprobe %s" % m
        errs.append("%s: %s" % (m, out.strip()[:120]))
    return False, "modprobe " + "; ".join(errs)


# ------------------------------------------------------------------ the BMC's USB hub
HUB_ID = ("1d6b", "0107")
HUB_RULE = "/etc/udev/rules.d/60-reimu-bmc-hub.rules"
HUB_RULE_TEXT = (
    "# REIMU: the BMC's virtual USB hub (KVM keyboard/mouse, virtual media) never\n"
    "# autosuspended: the BMC can't wake it (reimu-hostd.py hub-rule install)\n"
    'ACTION=="add|change", SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="1d6b", '
    'ATTR{idProduct}=="0107", TEST=="power/control", ATTR{power/control}="on"\n')


def bmc_usb_hubs(fs):
    for d in fs.glob("/sys/bus/usb/devices/*"):
        if (fs.read(d + "/idVendor"), fs.read(d + "/idProduct")) == HUB_ID and fs.exists(d + "/power/control"):
            yield d


def bmc_usb_hub_state(fs):
    for d in bmc_usb_hubs(fs):
        return {"present": True, "control": fs.read(d + "/power/control"), "status": fs.read(d + "/power/runtime_status")}
    return {"present": False, "control": None, "status": None}


class UsbHub:
    def __init__(self, fs, cfg):
        self.fs, self.cfg = fs, cfg

    def tick(self):
        events = []
        if "usb_hub" not in self.cfg["ALLOW"]:
            return events
        for d in bmc_usb_hubs(self.fs):
            if self.fs.read(d + "/power/control") != "auto":
                continue
            try:
                self.fs.write(d + "/power/control", "on")
            except OSError as e:
                log("BMC USB hub %s: %s" % (os.path.basename(d), e))
                continue
            text = "the BMC's USB hub %s: autosuspend off (KVM keyboard/mouse)" % os.path.basename(d)
            log(text)
            events.append({"kind": "bmc_usb_hub_on", "text": text})
        return events


def hub_rule(fs, run, action):
    """install / remove the udev rule; install also sets the hub there now"""
    path = fs.p(HUB_RULE)
    if action == "remove":
        if os.path.exists(path):
            os.remove(path)
            run(["udevadm", "control", "--reload"], timeout=10)
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".new", "w") as f:
        f.write(HUB_RULE_TEXT)
    os.chmod(path + ".new", 0o644)
    os.rename(path + ".new", path)
    run(["udevadm", "control", "--reload"], timeout=10)
    UsbHub(fs, parse_config("")).tick()


# ------------------------------------------------------------------ the shared I2C buses
# host i2c-0 = BMC i2c-6: lm96163, ir38064 x3, pmbus; host i2c-1 = BMC i2c-2:
# lm96163, emc2305 x2, pmbus x2. i2c-3 (FRU, RTC) and l_pcs are the host's.
BUS_OWNED = ("0-004c", "0-0044", "0-0045", "0-0046", "0-0060", "1-004c", "1-002c", "1-002e", "1-0058", "1-0059")
SHARED_BUS_CLIENT = re.compile(r"^[01]-[0-9a-f]{4}$")
BUS_MODULES = ("lm63", "emc2305", "pwm_fan", "pmbus", "ir38064")
BUS_CONF = "/etc/modprobe.d/reimu-bmc-owns-i2c.conf"
BUS_STATE = "/etc/reimu-hostd/bus-owner.state"
# "install" too: pwmd's init script runs modprobe by name, past a blacklist
BUS_CONF_TEXT = ("# REIMU: the BMC is the only master of the platform I2C buses the host shares\n"
                 "# (reimu-hostd.py bus-owner install; removed by uninstalling the agent)\n" +
                 "".join("blacklist %s\ninstall %s /bin/true\n" % (m, m) for m in BUS_MODULES))
PWMD_ON = ("enabled", "enabled-runtime", "indirect", "alias", "linked", "static")
PWMD_WORDS = PWMD_ON + ("disabled", "masked", "generated")


def shared_bus_bound(fs):
    """(device, driver, its unbind file): the pwm-fans first, they use the
    PWMs of the lm63 and emc2305 behind them"""
    out = []
    for d in fs.glob("/sys/bus/platform/devices/*"):
        if fs.link_name(d + "/driver") == "pwm-fan":
            out.append((os.path.basename(d), "pwm-fan", d + "/driver/unbind"))
    for n in BUS_OWNED:
        d = "/sys/bus/i2c/devices/" + n
        drv = fs.link_name(d + "/driver")
        if drv:
            out.append((n, drv, d + "/driver/unbind"))
    return out


def pwmd_running(fs):
    return any(fs.read(c) == "pwmd" for c in fs.glob("/proc/[0-9]*/comm"))


def bus_owner_state(fs, cfg):
    found = [n if drv == "pwm-fan" else "%s (%s)" % (n, drv) for n, drv, _ in shared_bus_bound(fs)]
    if pwmd_running(fs):
        found.append("pwmd (NOT stopped)" if cfg.get("_pwmd_error") else "pwmd")
    if cfg.get("_pwmd_boot"):
        found.append("pwmd (enabled at boot, NOT disabled)")
    return {"enforced": "bmc_owns_i2c" in cfg["ALLOW"], "clean": not found, "found": found,
            "modprobe_conf": fs.exists(BUS_CONF)}


def bus_state_keep(fs, was):
    """how pwmd was before we touched it, written down once: bus_owner remove
    gives it back from this"""
    state = fs.p(BUS_STATE)
    if not os.path.exists(state):
        os.makedirs(os.path.dirname(state), mode=0o700, exist_ok=True)
        with open(state + ".new", "w") as f:
            f.write("pwmd=%s\n" % was)
        os.rename(state + ".new", state)


class BusOwner:
    SETTLE = 3  # seconds bus_owner gives a stopped pwmd to be gone

    def __init__(self, fs, run, cfg):
        self.fs, self.run, self.cfg = fs, run, cfg
        self.failed = set()
        self.pwmd_told = False
        self.pwmd_err_told = False
        self.pwmd_strikes = 0
        self.boot_checked = False

    def _kill_pwmd(self):
        """by its exact name only, as the /proc check found it"""
        r = self.run(["pkill", "-x", "pwmd"], timeout=10)
        if r[0] in (126, 127):
            pids = [c.split("/")[2] for c in self.fs.glob("/proc/[0-9]*/comm") if self.fs.read(c) == "pwmd"]
            r = self.run(["kill"] + pids, timeout=10) if pids else (0, "")
        return r

    def _boot_check(self):
        """SysV: enabled at boot again, or never disabled: the links say so"""
        self.boot_checked = True
        svc = Init(self.fs, self.run)
        cur = svc.state("pwmd") if svc.kind == "sysv" else None
        if cur not in PWMD_ON:
            return []
        try:
            bus_state_keep(self.fs, cur)
        except OSError as e:  # one I2C master comes first
            log_error("pwmd is being disabled at boot, but how it was can't be written down, --uninstall won't "
                      "give it back: %s" % e)
        if svc.disable("pwmd"):
            log("the BMC is the only I2C master here: pwmd disabled at boot")
            return []
        text = "the BMC is NOT sure to be the only I2C master: pwmd stays enabled at boot, could not disable it"
        log_error(text)
        self.cfg["_pwmd_boot"] = text
        return [{"kind": "bmc_owns_i2c_failed", "text": text}]

    def _pwmd(self):
        events = []
        if not pwmd_running(self.fs):
            if self.pwmd_err_told:
                text = "the BMC is the only I2C master here: pwmd no longer runs"
                log(text)
                events.append({"kind": "bmc_owns_i2c", "text": text})
            self.pwmd_told = self.pwmd_err_told = False
            self.pwmd_strikes = 0
            self.cfg.pop("_pwmd_error", None)
            return events
        self.pwmd_strikes += 1
        svc = Init(self.fs, self.run)
        rc, out = svc.ctl("stop", "pwmd", timeout=30)
        krc = 0
        if rc != 0:
            krc, kout = self._kill_pwmd()
            out = (out or "").strip() + " " + (kout or "").strip()
        failed = rc != 0 and krc != 0 and pwmd_running(self.fs)
        if failed or self.pwmd_strikes >= 3:  # 3: every command says yes, and there it is again
            if not self.pwmd_err_told:
                self.pwmd_err_told = self.pwmd_told = True
                text = ("the BMC is NOT the only I2C master here: pwmd runs and could not be stopped (%s): a second "
                        "master corrupts the fan chips" % (out.strip()[:120] or "it comes back"))
                log_error(text)
                self.cfg["_pwmd_error"] = text
                events.append({"kind": "bmc_owns_i2c_failed", "text": text})
        elif not self.pwmd_told:
            self.pwmd_told = True
            if svc.kind == "sysv" and not svc.script("pwmd"):
                text = ("the BMC is the only I2C master here: a pwmd process with no service for it was killed by "
                        "name; whatever started it may start it again, then it is killed again")
            else:
                text = "the BMC is the only I2C master here: pwmd stopped"
            log(text)
            events.append({"kind": "bmc_owns_i2c", "text": text})
        return events

    def tick(self):
        events = []
        if "bmc_owns_i2c" not in self.cfg["ALLOW"]:
            return events
        for name, drv, unbind in shared_bus_bound(self.fs):
            try:
                self.fs.write(unbind, name)
            except OSError as e:
                if name not in self.failed:  # tried again every tick, said once
                    self.failed.add(name)
                    log("the BMC's I2C buses: %s (%s) not unbound: %s" % (name, drv, e))
                continue
            self.failed.discard(name)
            text = "the BMC is the only I2C master here: %s (%s) unbound" % (name, drv)
            log(text)
            events.append({"kind": "bmc_owns_i2c", "text": text})
        if not self.boot_checked:
            events += self._boot_check()
        events += self._pwmd()
        return events


def _initramfs_data(raw):
    """its uncompressed bytes (an early cpio may be in front), None = can't"""
    try:  # distributions package these apart: without one, "can't be read"
        import bz2, lzma, zlib
    except ImportError:
        return raw if raw[:6] == b"070701" else None
    out = raw if raw[:6] == b"070701" else b""
    for magic, new in ((b"\x1f\x8b\x08", lambda: zlib.decompressobj(31)), (b"\xfd7zXZ", lzma.LZMADecompressor),
                       (b"BZh", bz2.BZ2Decompressor)):
        at = 0 if raw.startswith(magic) else (raw.find(magic) if out else -1)
        if at >= 0:
            try:
                return out + new().decompress(raw[at:])
            except (OSError, EOFError, ValueError, zlib.error, lzma.LZMAError):
                return out or None
    return out or None


def initramfs_check(fs):
    """{"file": the running kernel's initramfs, "found": those modules in it
    (None: it can't be read here)}"""
    k = fs.read("/proc/sys/kernel/osrelease") or ""
    for f in ("/boot/initrd-%s.img" % k, "/boot/initramfs-%s.img" % k, "/boot/initrd.img-%s" % k, "/boot/initrd-%s" % k,
              "/boot/initrd.img", "/boot/initrd"):
        try:
            with open(fs.p(f), "rb") as h:
                raw = h.read()
        except OSError:
            continue
        data = _initramfs_data(raw)
        if data is None:
            return {"file": f, "found": None}
        mods = re.findall(rb"(lm63|emc2305|pwm[-_]fan|pmbus|ir38064)\.ko", data)
        return {"file": f, "found": sorted(set(m.decode().replace("-", "_") for m in mods))}
    return {"file": None, "found": None}


def bus_owner(fs, run, action, cfg):
    """install: the modules kept out, pwmd disabled (how it was written down
    once, an update keeps that), the drivers unbound now. remove: both given
    back, nothing unbound (the drivers come back with the next boot). False:
    pwmd not dealt with, said at error level"""
    svc = Init(fs, run)
    conf, state = fs.p(BUS_CONF), fs.p(BUS_STATE)
    if action == "remove":
        if os.path.exists(conf):
            os.remove(conf)
        try:
            with open(state) as f:
                was = f.read().strip().partition("=")[2]
            os.remove(state)
        except OSError:
            was = ""
        if was in PWMD_ON or (was == "generated" and svc.kind == "sysv"):
            svc.enable_now("pwmd")
        elif was == "generated":
            run(["chkconfig", "pwmd", "on"], timeout=15)
            run(["systemctl", "start", "pwmd"], timeout=60)
        return True
    if "bmc_owns_i2c" not in cfg["ALLOW"]:
        log("bmc_owns_i2c is not in ALLOW: the host keeps its own drivers on the shared I2C buses")
        return True
    os.makedirs(os.path.dirname(conf), exist_ok=True)
    with open(conf + ".new", "w") as f:
        f.write(BUS_CONF_TEXT)
    os.chmod(conf + ".new", 0o644)
    os.rename(conf + ".new", conf)
    cur = svc.state("pwmd")
    bus_state_keep(fs, cur)
    ok = True
    if cur in PWMD_ON:
        ok = svc.disable_now("pwmd")
        if not ok:
            log_error("pwmd stays enabled at boot, could not disable it: the BMC is NOT sure to be the only I2C master")
    elif cur == "generated":  # an init script: chkconfig has its state
        run(["chkconfig", "pwmd", "off"], timeout=15)
        run(["systemctl", "stop", "pwmd"], timeout=60)
    owner = BusOwner(fs, run, cfg)
    owner.boot_checked = True  # just done
    owner.tick()
    end = time.monotonic() + owner.SETTLE
    while pwmd_running(fs) and not owner.pwmd_err_told and time.monotonic() < end:
        time.sleep(0.2)
    if pwmd_running(fs):
        ok = False
        if not owner.pwmd_err_told:
            log_error("pwmd still runs: the BMC is NOT the only I2C master yet (the agent keeps trying and tells the BMC)")
    r = initramfs_check(fs)
    k = fs.read("/proc/sys/kernel/osrelease") or "$(uname -r)"
    how = "make-initrd -k %s (ALT; or dracut -f --kver %s)" % (k, k)
    if r["found"]:
        log("%s has %s: they can load at boot, until the agent unbinds them. To keep them out from the start, "
            "rebuild it (the modprobe.d file goes in): %s" % (r["file"], " ".join(r["found"]), how))
    elif r["file"] and r["found"] is None:
        log("%s can't be read here: if it has %s (lsinitrd), rebuild it: %s" % (r["file"], " ".join(BUS_MODULES), how))
    return ok


def edac_autoload(fs, run, cfg):
    if cfg["EDAC_AUTOLOAD"] and not fs.glob("/sys/devices/system/edac/mc/mc[0-9]*"):
        ok, text = edac_modprobe(run)
        log("EDAC: %s" % (text + ": ok" if ok else text))


# ------------------------------------------------------------------ GPU fan
# the policy across a restart of the agent or of the host (so not in /run): with
# the BMC down a new process has nothing else, and auto would let the idle driver
# stop the fan. The BMC sends its own at every connection, which overwrites this
GPU_POLICY = "/var/lib/reimu-hostd/policy"


def remove_gpu_policy(fs):
    for f in (GPU_POLICY, GPU_POLICY + ".new"):
        try:
            os.remove(fs.p(f))
        except OSError:
            pass
    try:
        os.rmdir(fs.p(os.path.dirname(GPU_POLICY)))
    except OSError:
        pass


class GpuFan:
    """the policy the BMC asked for, kept on the card: auto (the driver's),
    manual DUTY (>= floor), turbo (100 % until a time, then the policy)"""

    # no word from the BMC: at least this much while the card is cool (the idle
    # driver stops the fan, and the IOHUB behind the card relies on its air);
    # a card this hot goes to the driver, back below the second
    LOST_HOT, LOST_COOL = 70000, 60000

    def __init__(self, fs, floor=30, link_lost_duty=40, load=True):
        self.fs, self.floor, self.link_lost_duty = fs, floor, link_lost_duty
        self.lost_hot = False
        self.mode, self.duty = "auto", None
        self.turbo_until = None
        self.touched = False     # pwm1_enable written by us since the last auto
        self.link_lost = False
        self.reapplied = 0
        self.present = False
        self.last_err = None
        self.applied = None      # (enable, pwm) as last written/seen right
        self.saved = None        # what GPU_POLICY holds
        if load:
            self._load_policy()

    def _load_policy(self):
        text = self.fs.read(GPU_POLICY)
        if text is None:
            return
        try:
            p = json.loads(text)
        except (ValueError, RuntimeError):
            p = None
        mode, duty = (p.get("mode"), p.get("duty")) if isinstance(p, dict) else (None, None)
        number = isinstance(duty, (int, float)) and not isinstance(duty, bool)
        if mode == "auto" or (mode == "manual" and number and self.set_policy(mode, duty, save=False)[0]):
            self.saved = {"mode": mode, "duty": self.duty}
            log("GPU fan: the policy from before this start: %s" % (mode if mode == "auto" else "manual %g %%" % self.duty))
        else:
            log("GPU fan: %s is not a policy: ignored" % GPU_POLICY)

    def _save_policy(self):
        want = {"mode": self.mode, "duty": self.duty}
        if want == self.saved:
            return
        try:
            self.fs.write_atomic(GPU_POLICY, json.dumps(want) + "\n")
            self.saved = want
        except OSError as e:
            log("GPU fan: the policy not saved: %s" % e)

    def hwmon(self):
        for d in self.fs.glob("/sys/bus/pci/devices/*"):
            if self.fs.link_name(d + "/driver") != "amdgpu":
                continue
            for h in self.fs.glob(d + "/hwmon/hwmon*"):
                if self.fs.exists(h + "/pwm1_enable"):
                    return h
        return None

    def set_policy(self, mode, duty=None, save=True):
        if mode == "auto":
            self.mode, self.duty = "auto", None
            if save:
                self._save_policy()
            return True, "auto"
        if mode != "manual":
            return False, "mode must be auto or manual"
        try:
            d = float(duty)
        except (TypeError, ValueError):
            return False, "duty must be a number"
        if not 0 <= d <= 100:
            return False, "duty must be 0..100"
        msg = "manual %g %%" % d
        if d < self.floor:
            msg = "manual %d %% (raised from %g to the floor %d %%)" % (self.floor, d, self.floor)
            d = self.floor
        self.mode, self.duty = "manual", d
        if save:
            self._save_policy()
        return True, msg

    def turbo(self, minutes, now, cancel=False):
        if cancel:
            self.turbo_until = None
            return True, "turbo off"
        try:
            m = float(minutes)
        except (TypeError, ValueError):
            return False, "minutes must be a number"
        if not 0 < m <= 120:
            return False, "minutes must be 1..120"
        self.turbo_until = now + m * 60
        return True, "turbo %g min" % m

    def target(self, now):
        """(pwm1_enable, pwm1) wanted, None = leave the driver alone"""
        if self.turbo_until is not None and now >= self.turbo_until:
            self.turbo_until = None
        if self.link_lost:
            if self.lost_hold():
                return 1, int(max(self.duty, self.link_lost_duty) * 255 / 100.0 + 0.5)
            return None
        if self.turbo_until is not None:
            return 1, 255
        if self.mode == "manual":
            return 1, int(self.duty * 255 / 100.0 + 0.5)
        return None

    def lost_hold(self):
        """link lost: hold the card's fan ourselves? Not with an auto policy,
        a hot card or no temperature: the driver knows better"""
        if self.mode != "manual" or self.link_lost_duty <= 0:
            return False
        h = self.hwmon()
        t = self.fs.int(h + "/temp1_input") if h else None
        if t is None:
            self.lost_hot = True
        elif t >= self.LOST_HOT:
            self.lost_hot = True
        elif t < self.LOST_COOL:
            self.lost_hot = False
        return not self.lost_hot

    def tick(self, now, link_ok):
        events = []
        if not link_ok:
            first = not self.link_lost
            self.link_lost = True
            if self.lost_hold():
                if first:
                    events.append({"kind": "gpu_fan_floor_link_lost",
                                   "text": "no word from the BMC: the GPU fan held at %d %% or more" % self.link_lost_duty})
            # until the driver has it back: a failed write is tried again
            elif self.touched and self._to_auto():
                events.append({"kind": "gpu_fan_auto_link_lost", "text": "no word from the BMC: the GPU fan back to the driver"})
        else:
            self.link_lost = False
            self.lost_hot = False
        h = self.hwmon()
        self.present = h is not None
        if not h:
            return events  # what was applied is kept: its return is a re-apply
        want = self.target(now)
        if want is None:
            if self.touched and not self.link_lost:
                self._to_auto()
            return events
        cur = (self.fs.int(h + "/pwm1_enable"), self.fs.int(h + "/pwm1"))
        if cur[0] == want[0] and cur[1] is not None and abs(cur[1] - want[1]) <= 2:
            # already so (e.g. left by this agent before a restart): ours now,
            # handed back to the driver when the link or the agent goes
            self.applied, self.touched = want, True
            return events
        # the same target as last time, yet the card differs: the driver
        # (a GPU reset) undid it; a new target is just a policy change
        was_ours = self.applied == want and self.touched
        try:
            if cur[0] != want[0]:
                self.fs.write(h + "/pwm1_enable", want[0])
            self.fs.write(h + "/pwm1", want[1])
            self.touched, self.last_err = True, None
        except OSError as e:
            self.last_err = str(e)
            log("GPU fan: %s: %s" % (h, e))
            return events
        if was_ours:
            self.reapplied += 1
            events.append({"kind": "gpu_fan_reapplied",
                           "text": "GPU fan policy set again (the driver had pwm1_enable=%s pwm1=%s)" % cur})
            log("GPU fan: set again: pwm1_enable %s -> %d, pwm1 %s -> %d" % (cur[0], want[0], cur[1], want[1]))
        self.applied = want
        return events

    def _to_auto(self):
        h = self.hwmon()
        self.applied = None
        if not h:
            return False
        try:
            self.fs.write(h + "/pwm1_enable", 2)
            self.touched = False
            return True
        except OSError as e:
            log("GPU fan: back to auto: %s" % e)
            return False

    def restore_auto(self, why):
        if self.touched and self._to_auto():
            log("GPU fan: back to the driver's automatic control (%s)" % why)

    def state(self, now):
        mode = "turbo" if self.turbo_until is not None and now < self.turbo_until else self.mode
        return {"mode": mode, "policy": self.mode, "duty": self.duty,
                "turbo_left": int(self.turbo_until - now) if mode == "turbo" else 0,
                "floor": self.floor, "present": self.present, "link_lost": self.link_lost,
                "reapplied": self.reapplied, "error": self.last_err}


# ------------------------------------------------------------------ control
class Controller:
    """the BMC's requests: only OPS, each with its own checked arguments"""

    def __init__(self, cfg, fan, run, col=None):
        self.cfg, self.fan, self.run, self.col = cfg, fan, run, col
        self.diag_gz = None

    def handle(self, msg, now):
        rid = msg.get("id")
        if not isinstance(rid, int):
            rid = None
        op = msg.get("op")
        res = {"t": "result", "id": rid, "op": op if op in OPS else None}
        if op not in OPS:
            res.update(ok=False, err="op not allowed")
            log("control: refused an unknown op")
            return res
        if op not in self.cfg["ALLOW"]:
            res.update(ok=False, err="%s disabled on this host (ALLOW in %s)" % (op, CONF))
            return res
        try:
            ok, text = getattr(self, "op_" + op)(msg, now)
        except Exception as e:  # never let a request kill the agent
            ok, text = False, "error: %s" % type(e).__name__
        res.update(ok=ok, err=None if ok else text, text=text if ok else None)
        if op == "diag" and ok:
            res["gz"] = self.diag_gz
        log("control: %s: %s" % (op, text))
        return res

    def op_gpu_fan(self, msg, now):
        return self.fan.set_policy(msg.get("mode"), msg.get("duty"))

    def op_gpu_turbo(self, msg, now):
        return self.fan.turbo(msg.get("minutes"), now, cancel=msg.get("cancel") is True)

    def op_os_power(self, msg, now):
        a = msg.get("action")
        if a not in ("poweroff", "reboot"):
            return False, "action must be poweroff or reboot"
        rc, what, out = Init(self.fan.fs, self.run).power(a)
        return rc == 0, "%s%s" % (what, "" if rc == 0 else ": " + out.strip()[:200])

    def op_edac_load(self, msg, now):
        return edac_modprobe(self.run)

    def op_diag(self, msg, now):
        """the report for the BMC's diagnostic archive: a fixed set, no user
        files; temps() leaves the shared I2C buses alone. gzip+base64, the
        kernel log's tail shortened until it fits a line."""
        if not self.col:
            return False, "no collector"
        col, fs = self.col, self.col.fs
        rc, klog = self.run(["dmesg"], timeout=10)
        klog = (klog if rc == 0 else "dmesg: %s" % klog[:200])[-DIAG_LOG_TAIL:]
        up = fs.read("/proc/uptime")
        d = {"inventory": col.inventory(), "edac": col.edac(), "gpu": col.gpu(), "temps": col.temps(),
             "mem": _meminfo(fs.read("/proc/meminfo")), "uptime": up.split()[0] if up else None,
             "bmc_usb_hub": bmc_usb_hub_state(fs), "i2c_owner": bus_owner_state(fs, self.cfg),
             "agent": {"version": VERSION, "proto": PROTO}}
        while True:
            d["kernel_log"] = klog
            z = base64.b64encode(_gzip(json.dumps(d).encode())).decode()
            if len(z) <= DIAG_MAX_B64 or not klog:
                break
            klog = klog[-len(klog) // 2:]
        if len(z) > DIAG_MAX_B64:
            return False, "report too large"
        self.diag_gz = z
        return True, "report of %d bytes" % len(z)

    def policy(self, msg, now):
        """the BMC's standing policy (sent at every connection and change);
        each part only when ALLOW has it, like the control requests"""
        g = msg.get("gpu_fan")
        if isinstance(g, dict) and "gpu_fan" in self.cfg["ALLOW"]:
            ok, text = self.fan.set_policy(g.get("mode"), g.get("duty"))
            if not ok:
                log("policy: %s" % text)
        tl = msg.get("turbo_left")
        if isinstance(tl, (int, float)) and not isinstance(tl, bool) and "gpu_turbo" in self.cfg["ALLOW"]:
            if tl > 0:
                self.fan.turbo_until = now + min(tl, 43200)  # reimu-fand's Турбо: up to 12 h
            else:
                self.fan.turbo_until = None


# ------------------------------------------------------------------ protocol
class VersionError(Exception):
    pass


def encode(obj):
    d = {"v": PROTO}
    d.update(obj)
    return (json.dumps(d, separators=(",", ":"), ensure_ascii=False) + "\n").encode()


def parse_line(line):
    """a message from the BMC: a dict, None if it is not one; VersionError when
    its major version isn't ours"""
    try:
        m = json.loads(line.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        return None
    if not isinstance(m, dict) or not isinstance(m.get("v"), int) or not isinstance(m.get("t"), str):
        return None
    if m["v"] != PROTO:
        raise VersionError("protocol %s, this agent speaks %d" % (m["v"], PROTO))
    return m


def auth_mac(key, nonce, cnonce, agent_id):
    return hmac.new(key, ("reimu-hostlink-auth-v1\n%s\n%s\n%s" % (nonce, cnonce, agent_id)).encode(), hashlib.sha256).hexdigest()


def server_mac(key, cnonce, nonce):
    return hmac.new(key, ("reimu-hostlink-srv-v1\n%s\n%s" % (cnonce, nonce)).encode(), hashlib.sha256).hexdigest()


def make_pair_string(d):
    return "reimu1:" + base64.urlsafe_b64encode(json.dumps(d, separators=(",", ":")).encode()).decode().rstrip("=")


def parse_pair_string(s):
    s = (s or "").strip()
    if not s.startswith("reimu1:"):
        return None
    b = s[7:]
    try:
        d = json.loads(base64.urlsafe_b64decode(b + "=" * (-len(b) % 4)).decode())
    except (ValueError, binascii.Error, UnicodeDecodeError):
        return None
    if not isinstance(d, dict) or not isinstance(d.get("token"), str) or not re.match(r"^[0-9a-f]{64}$", d.get("fp", "")):
        return None
    return d


# ------------------------------------------------------------------ transport
def ll_from_mac(mac):
    """the EUI-64 link-local address of a MAC (the BMC's eth0)"""
    try:
        b = bytearray(int(x, 16) for x in mac.split(":"))
    except (ValueError, AttributeError):
        return None
    if len(b) != 6:
        return None
    b[0] ^= 2
    eui = bytes(b[:3]) + b"\xff\xfe" + bytes(b[3:])
    return socket.inet_ntop(socket.AF_INET6, b"\xfe\x80" + b"\0" * 6 + eui)


def up_ifaces(fs, only=""):
    want = only.split() if only else None
    out = []
    for n in fs.glob("/sys/class/net/*"):
        name = os.path.basename(n)
        if fs.read(n + "/type") == "772" or name == "lo":
            continue
        if want is not None and name not in want:
            continue
        if fs.read(n + "/operstate") in ("up", "unknown") and fs.read(n + "/carrier") != "0":
            out.append(name)
    return out


def candidates(cfg, fs, discovered, last=None):
    """where to look for the BMC, best first: its link-local address on each
    interface that is up, link-local ones found by the probe, its IPv4 address"""
    port = cfg["BMC_PORT"]
    if cfg["BMC"]:
        h, _, p = cfg["BMC"].rpartition(":") if cfg["BMC"].count(":") == 1 or "]" in cfg["BMC"] else (cfg["BMC"], "", "")
        h = h.strip("[]")
        scope = None
        if "%" in h:
            h, scope = h.split("%", 1)
        return [{"addr": h, "scope": scope, "port": int(p) if p else port,
                 "transport": "ipv6-ll" if h.startswith("fe80:") else ("ipv6" if ":" in h else "ipv4")}]
    out = []
    ll = ll_from_mac(cfg["BMC_MAC"]) if cfg["BMC_MAC"] else None
    ifs = up_ifaces(fs, cfg["IFACES"])
    if ll:
        out += [{"addr": ll, "scope": i, "port": port, "transport": "ipv6-ll"} for i in ifs]
    for d in discovered:
        c = {"addr": d["addr"], "scope": d["scope"], "port": d.get("port", port), "transport": "ipv6-ll"}
        if not any(x["addr"] == c["addr"] and x["scope"] == c["scope"] for x in out):
            out.append(c)
    if last and last.get("transport") == "ipv6-ll":
        out.sort(key=lambda x: not (x["addr"] == last["addr"] and x["scope"] == last["scope"]))
    if cfg["BMC_IPV4"]:
        out.append({"addr": cfg["BMC_IPV4"], "scope": None, "port": port, "transport": "ipv4"})
    return out


def connect_first(cands, connector):
    for c in cands:
        try:
            return connector(c), c
        except (OSError, ssl.SSLError, ValueError) as e:
            log("BMC at %s: %s" % (fmt_cand(c), e))
    return None, None


def fmt_cand(c):
    if ":" in c["addr"]:
        return "[%s%s]:%d" % (c["addr"], "%" + c["scope"] if c["scope"] else "", c["port"])
    return "%s:%d" % (c["addr"], c["port"])


class Backoff:
    def __init__(self, first=1, cap=60):
        self.first, self.cap, self.cur = first, cap, None

    def next(self):
        self.cur = self.first if self.cur is None else min(self.cur * 2, self.cap)
        return self.cur

    def reset(self):
        self.cur = None


DISCOVERY_Q = b"REIMU-HOSTLINK? 1"


def parse_discovery(data):
    m = re.match(rb"^REIMU-HOSTLINK 1 (\d{1,5}) ([0-9a-f]{4,64})$", data.strip())
    return {"port": int(m.group(1)), "fp": m.group(2).decode()} if m else None


def discover(ifaces, port, fp, timeout=1.0):
    """the BMC's link-local address by a probe to all nodes (ff02::1) on each
    interface; only answers with our pinned certificate's fingerprint count"""
    out = []
    for i in ifaces:
        try:
            idx = socket.if_nametoindex(i)
            s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        except OSError:
            continue
        try:
            s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_IF, idx)
            s.sendto(DISCOVERY_Q, ("ff02::1", port, 0, idx))
            end = time.time() + timeout
            while time.time() < end:
                r, _, _ = select.select([s], [], [], max(0, end - time.time()))
                if not r:
                    break
                data, src = s.recvfrom(256)
                d = parse_discovery(data)
                if d and fp.startswith(d["fp"]) and src[0].startswith("fe80"):
                    out.append({"addr": src[0].split("%")[0], "scope": i, "port": d["port"]})
        except OSError:
            pass
        finally:
            s.close()
    return out


def tls_connect(c, fp, timeout=5):
    """TLS to a candidate, the certificate's SHA-256 must be the pinned one"""
    fam = socket.AF_INET6 if ":" in c["addr"] else socket.AF_INET
    s = socket.socket(fam, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        if fam == socket.AF_INET6:
            scope = socket.if_nametoindex(c["scope"]) if c["scope"] else 0
            s.connect((c["addr"], c["port"], 0, scope))
        else:
            s.connect((c["addr"], c["port"]))
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE   # self-signed: pinned below instead
        try:
            ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        except AttributeError:
            pass
        s = ctx.wrap_socket(s)
        got = hashlib.sha256(s.getpeercert(binary_form=True)).hexdigest()
        if not hmac.compare_digest(got, fp):
            raise ValueError("certificate %s… is not the paired BMC's (%s…): not connecting" % (got[:12], fp[:12]))
        return s
    except BaseException:
        s.close()
        raise


# ------------------------------------------------------------------ the link
class Link:
    """one TLS connection: line framing, both ways"""

    def __init__(self, sock):
        self.s, self.buf, self.out = sock, b"", b""
        self.eof = False
        self.s.setblocking(False)

    def fileno(self):
        return self.s.fileno()

    def send(self, obj):
        self.out += encode(obj)
        self.flush()

    def flush(self):
        while self.out:
            try:
                n = self.s.send(self.out[:16384])
            except (ssl.SSLWantWriteError, ssl.SSLWantReadError, BlockingIOError):
                return
            self.out = self.out[n:]

    def lines(self):
        """complete lines received; EOFError when the BMC closed (after the
        lines it sent before closing: "paired" is followed by the close)"""
        while not self.eof:
            try:
                d = self.s.recv(16384)
            except (ssl.SSLWantReadError, ssl.SSLWantWriteError, BlockingIOError):
                break
            if not d:
                self.eof = True
                break
            self.buf += d
            if len(self.buf) > MAX_LINE and b"\n" not in self.buf:
                raise EOFError("line from the BMC too long")
        out = []
        while b"\n" in self.buf:
            l, self.buf = self.buf.split(b"\n", 1)
            out.append(l)
        if self.eof and not out:
            raise EOFError("closed by the BMC")
        return out

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


CRED_KEYS = ("BMC", "BMC_MAC", "BMC_IPV4", "BMC_PORT", "IFACES")


def _mtime(path):
    try:
        return os.stat(path).st_mtime_ns
    except (OSError, TypeError):
        return None


class Agent:
    def __init__(self, cfg, secret, fs, run, secret_path=None, conf_path=None):
        self.cfg, self.secret, self.fs, self.run = cfg, secret, fs, run
        self.secret_path, self.conf_path = secret_path, conf_path
        self.cred_mtimes = (_mtime(secret_path), _mtime(conf_path))
        self.col = Collector(fs, run, cfg)
        # not allowed: the BMC's word is ignored, so is what it once said
        self.fan = GpuFan(fs, floor=cfg["GPU_FAN_FLOOR"], link_lost_duty=cfg["GPU_FAN_LINK_LOST"], load="gpu_fan" in cfg["ALLOW"])
        self.hub = UsbHub(fs, cfg)
        self.bus = BusOwner(fs, run, cfg)
        self.ctl = Controller(cfg, self.fan, run, self.col)
        self.backoff = Backoff(1, 60)
        self.last_cand = None
        self.last_rx = time.monotonic()  # the link-loss clock starts at start-up
        self.stop = False
        self.revoked = False
        self.inv_hash = None
        self.inv_edac = None

    def handshake(self, link, cand):
        """hello -> auth -> welcome; raises on failure"""
        key = binascii.unhexlify(self.secret["KEY"])
        hello = self._wait(link, "hello", 10)
        nonce = hello.get("nonce")
        if not isinstance(nonce, str) or not re.match(r"^[0-9a-f]{32,64}$", nonce):
            raise ValueError("bad hello")
        cnonce = binascii.hexlify(os.urandom(16)).decode()
        aid = self.secret.get("AGENT_ID", "host")
        link.send({"t": "auth", "agent": "reimu-hostd", "ver": VERSION, "id": aid, "cnonce": cnonce,
                   "mac": auth_mac(key, nonce, cnonce, aid), "host": self.fs.read("/proc/sys/kernel/hostname"),
                   "transport": cand["transport"]})
        w = self._wait(link, ("welcome", "error"), 10)
        if w["t"] == "error":
            if w.get("code") == "auth":
                self.revoked = True
            raise ValueError("BMC: %s" % str(w.get("code") or w.get("text"))[:100])
        if not hmac.compare_digest(str(w.get("mac", "")), server_mac(key, cnonce, nonce)):
            raise ValueError("the BMC didn't prove the key")
        self.revoked = False
        return w

    def _wait(self, link, types, timeout):
        types = (types,) if isinstance(types, str) else types
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            r, _, _ = select.select([link], [], [], 0.5) if not link.s.pending() else ([link], [], [])
            if not r:
                continue
            for l in link.lines():
                m = parse_line(l)
                if m and m["t"] in types:
                    return m
        raise ValueError("no %s from the BMC" % "/".join(types))

    def local_tick(self, now, link_ok):
        """every 2 s, linked or not: the GPU fan policy, the BMC's USB hub, its I2C buses"""
        return self.fan.tick(now, link_ok) + self.hub.tick() + self.bus.tick()

    def credentials_changed(self):
        """a pairing (pair --pair, install.sh) wrote a new key or address"""
        return self.secret_path is not None and (_mtime(self.secret_path), _mtime(self.conf_path)) != self.cred_mtimes

    def refresh_credentials(self):
        self.cred_mtimes = (_mtime(self.secret_path), _mtime(self.conf_path))
        s = load_secret(self.secret_path)
        if s.get("KEY") and s.get("FINGERPRINT"):
            self.secret = s
        if self.conf_path:
            c = load_config(self.conf_path)
            for k in CRED_KEYS:
                self.cfg[k] = c[k]
        self.revoked = False
        self.last_cand = None
        self.backoff.reset()
        log("paired again: the new key and BMC address in use")

    def connect(self):
        fp = self.secret["FINGERPRINT"]
        disc = []
        if self.cfg["DISCOVERY"] and not self.cfg["BMC"]:
            disc = discover(up_ifaces(self.fs, self.cfg["IFACES"]), self.cfg["BMC_PORT"], fp)
        cands = candidates(self.cfg, self.fs, disc, self.last_cand)
        if not cands:
            log("no BMC address: BMC_MAC/BMC_IPV4 in %s (pair again)" % CONF)
            return None, None
        return connect_first(cands, lambda c: tls_connect(c, fp))

    def session(self, link, cand):
        w = self.handshake(link, cand)
        log("connected to the BMC at %s (%s)" % (fmt_cand(cand), cand["transport"]))
        self.last_cand = cand
        self.backoff.reset()
        self.last_rx = time.monotonic()
        if isinstance(w.get("policy"), dict):
            self.ctl.policy(w["policy"], time.monotonic())
        # now, not in the loop: a stop signal may already be waiting, and what a
        # killed agent left on the card must be ours to hand back
        self.fan.tick(time.monotonic(), True)
        self.send_inventory(link, force=True)
        next_metrics = next_gpu = 0
        next_inv = time.monotonic() + INVENTORY_SEC
        while not self.stop:
            now = time.monotonic()
            if now >= next_gpu:
                next_gpu = now + GPU_TICK_SEC
                for e in self.local_tick(now, True):
                    link.send({"t": "event", "data": e})
            if now >= next_metrics:
                next_metrics = now + self.cfg["INTERVAL"]
                m = self.col.metrics(now)
                if m.get("gpu") is not None:
                    m["gpu"]["policy"] = self.fan.state(now)
                m["agent"] = {"version": VERSION, "interval": self.cfg["INTERVAL"], "transport": cand["transport"]}
                link.send({"t": "metrics", "data": m})
                self.after_metrics(link, m)
            if now >= next_inv:
                next_inv = now + INVENTORY_SEC
                self.send_inventory(link)
            if now - self.last_rx > self.cfg["LINK_LOST_SEC"]:
                raise EOFError("no word from the BMC for %d s" % (now - self.last_rx))
            link.flush()
            wait = max(0.05, min(next_gpu, next_metrics) - time.monotonic())
            r, _, _ = ([link], [], []) if link.s.pending() else select.select([link], [link] if link.out else [], [], wait)
            if r:
                for l in link.lines():
                    self.last_rx = time.monotonic()
                    self.on_message(link, l)

    def on_message(self, link, l):
        m = parse_line(l)
        if m is None:
            return
        now = time.monotonic()
        t = m["t"]
        if t == "ping":
            link.send({"t": "pong"})
        elif t == "policy":
            self.ctl.policy(m, now)
        elif t == "control":
            res = self.ctl.handle(m, now)
            self.fan.tick(now, True)
            if m.get("op") == "gpu_fan" or m.get("op") == "gpu_turbo":
                res["state"] = self.fan.state(now)
            link.send(res)
        elif t == "inventory?":
            self.send_inventory(link, force=True)
        # anything else: a newer BMC's, ignored

    def after_metrics(self, link, m):
        """EDAC came or went since the inventory (e2k_edac loaded by the
        page's button or by hand): the inventory again at once, not in an hour"""
        if bool((m.get("edac") or {}).get("loaded")) != self.inv_edac:
            self.send_inventory(link)

    def send_inventory(self, link, force=False):
        inv = self.col.inventory()
        self.inv_edac = (inv.get("memory") or {}).get("ecc") == "edac"
        h = hashlib.sha256(json.dumps(inv, sort_keys=True).encode()).hexdigest()
        if force or h != self.inv_hash:
            self.inv_hash = h
            self.col.lspci = None
            link.send({"t": "inventory", "data": inv})

    def run_forever(self):
        edac_autoload(self.fs, self.run, self.cfg)
        while not self.stop:
            link = None
            if self.credentials_changed():
                self.refresh_credentials()
            try:
                sock, cand = self.connect()
                if sock:
                    link = Link(sock)
                    self.session(link, cand)
            except VersionError as e:
                log("BMC: %s: update the agent" % e)
            except (OSError, EOFError, ValueError, ssl.SSLError) as e:
                log("link: %s" % e)
            finally:
                if link:
                    link.close()
            if self.stop:
                break
            # off the link: the fan policy times out on its own clock
            delay = 300 if self.revoked else self.backoff.next()
            if self.revoked:
                log("the BMC refused the key (unpaired?): pair again; next try in %d s" % delay)
            end = time.monotonic() + delay
            while not self.stop and time.monotonic() < end and not self.credentials_changed():
                self.local_tick(time.monotonic(), time.monotonic() - self.last_rx <= self.cfg["LINK_LOST_SEC"])
                time.sleep(min(GPU_TICK_SEC, max(0.05, end - time.monotonic())))
        self.fan.restore_auto("agent stopped")


# ------------------------------------------------------------------ pairing / install
def pair(pstr, fs, cfg_path=CONF, secret_path=SECRET, timeout=10):
    p = parse_pair_string(pstr)
    if not p:
        raise SystemExit("not a pairing string (reimu1:...): copy it from the BMC's web page Хост")
    cfg = load_config(cfg_path)
    if p.get("mac"):
        cfg["BMC_MAC"] = p["mac"]
    if p.get("v4"):
        cfg["BMC_IPV4"] = p["v4"]
    if p.get("port"):
        cfg["BMC_PORT"] = int(p["port"])
    if p.get("bmc"):
        cfg["BMC"] = p["bmc"]
    disc = discover(up_ifaces(fs, cfg["IFACES"]), cfg["BMC_PORT"], p["fp"]) if cfg["DISCOVERY"] and not cfg["BMC"] else []
    sock, cand = connect_first(candidates(cfg, fs, disc), lambda c: tls_connect(c, p["fp"]))
    if not sock:
        raise SystemExit("the BMC is not reachable (see above)")
    link = Link(sock)
    try:
        ag = Agent(cfg, {}, fs, run_cmd)
        hello = ag._wait(link, "hello", timeout)
        aid = load_secret(secret_path).get("AGENT_ID") or str(uuid.uuid4())
        link.send({"t": "pair", "token": p["token"], "id": aid, "host": fs.read("/proc/sys/kernel/hostname"),
                   "nonce": hello.get("nonce")})
        r = ag._wait(link, ("paired", "error"), timeout)
        if r["t"] == "error" or not re.match(r"^[0-9a-f]{64}$", str(r.get("key", ""))):
            raise SystemExit("pairing refused by the BMC: %s" % str(r.get("code") or r.get("text"))[:100])
        save_secret(secret_path, {"KEY": r["key"], "FINGERPRINT": p["fp"], "AGENT_ID": aid})
    finally:
        link.close()
    set_conf(cfg_path, {"BMC_MAC": cfg["BMC_MAC"], "BMC_IPV4": cfg["BMC_IPV4"], "BMC_PORT": cfg["BMC_PORT"],
                        "BMC": cfg["BMC"]})
    log("paired with the BMC at %s (%s)" % (fmt_cand(cand), cand["transport"]))




def set_conf(path, kv):
    try:
        with open(path) as f:
            lines = f.read().splitlines()
    except OSError:
        lines = ["# reimu-hostd (reimu-hostd.conf next to reimu-hostd.py has every setting)"]
    for k, v in kv.items():
        line = "%s=%s" % (k, v)
        for i, l in enumerate(lines):
            if re.match(r"^#?\s*%s=" % k, l) and (not l.startswith("#") or v):
                lines[i] = line if v != "" or not l.startswith("#") else l
                break
        else:
            if v != "":
                lines.append(line)
    tmp = path + ".new"
    with open(tmp, "w") as f:
        f.write("\n".join(lines) + "\n")
    os.chmod(tmp, 0o644)
    os.rename(tmp, path)




def install_service(fs, run, here):
    """the unit or the init script from HERE in place, on at boot, (re)started"""
    svc = Init(fs, run)
    if svc.kind == "systemd":
        dst = fs.p(UNIT)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy(os.path.join(here, "reimu-hostd.service"), dst)
        os.chmod(dst, 0o644)
        os.sync()  # a BMC reboot resets this host: no empty files
        run(["systemctl", "daemon-reload"], timeout=60)
        run(["systemctl", "enable", "reimu-hostd.service"], timeout=60)
        run(["systemctl", "restart", "reimu-hostd.service"], timeout=60)
        return
    d = next((x for x in INIT_DIRS if fs.exists(x)), INIT_DIRS[0])
    os.makedirs(fs.p(d), exist_ok=True)
    dst = fs.p(d + "/reimu-hostd")
    shutil.copy(os.path.join(here, "reimu-hostd.init"), dst)
    os.chmod(dst, 0o755)
    os.sync()
    if not svc.enable("reimu-hostd"):
        log_error("no update-rc.d, chkconfig, insserv or rc3.d directory: the agent will NOT start at boot")
    svc.ctl("restart", "reimu-hostd")


def remove_service(fs, run):
    svc = Init(fs, run)
    if svc.kind == "systemd":
        run(["systemctl", "disable", "--now", "reimu-hostd.service"], timeout=60)
        if fs.exists(UNIT):
            os.remove(fs.p(UNIT))
        run(["systemctl", "daemon-reload"], timeout=60)
        return
    svc.ctl("stop", "reimu-hostd")
    svc.remove_boot("reimu-hostd")
    s = svc.script("reimu-hostd")
    if s:
        os.remove(fs.p(s))


def do_install(pstr):
    if os.geteuid() != 0:
        raise SystemExit("run with sudo")
    here = os.path.dirname(os.path.abspath(__file__))
    svc = Init(Fs(), run_cmd)
    for f in ("reimu-hostd.service" if svc.kind == "systemd" else "reimu-hostd.init", "reimu-hostd.conf"):
        if not os.path.exists(os.path.join(here, f)):
            raise SystemExit("%s must be next to %s" % (f, __file__))
    os.makedirs(LIB, exist_ok=True)
    shutil.copy(os.path.abspath(__file__), os.path.join(LIB, "reimu-hostd.py"))
    os.chmod(os.path.join(LIB, "reimu-hostd.py"), 0o755)
    if not os.path.exists(CONF):
        shutil.copy(os.path.join(here, "reimu-hostd.conf"), CONF)
        os.chmod(CONF, 0o644)
    if pstr:
        pair(pstr, Fs())
    if load_secret().get("KEY"):
        bus_owner(Fs(), run_cmd, "install", load_config())
    install_service(Fs(), run_cmd, here)
    time.sleep(3)
    print(svc.status("reimu-hostd")[1])


def do_uninstall():
    if os.geteuid() != 0:
        raise SystemExit("run with sudo")
    remove_service(Fs(), run_cmd)
    bus_owner(Fs(), run_cmd, "remove", load_config())
    remove_gpu_policy(Fs())
    for f in (os.path.join(LIB, "reimu-hostd.py"), SECRET):
        if os.path.exists(f):
            os.remove(f)
    log("removed (%s kept)" % CONF)


def main(argv):
    root = os.environ.get("REIMU_HOSTD_ROOT", "")
    fs = Fs(root)
    run = fake_runner(root) if root else run_cmd
    cmd = argv[1] if len(argv) > 1 else ""
    opt = dict(zip(argv[2::2], argv[3::2]))
    conf = opt.get("--config", os.environ.get("REIMU_HOSTD_CONF", CONF))
    secret = opt.get("--secret", os.environ.get("REIMU_HOSTD_SECRET", SECRET))
    cfg = load_config(conf)
    if cmd == "inventory":
        print(json.dumps(Collector(fs, run, cfg).inventory(), indent=1, ensure_ascii=False))
    elif cmd == "metrics":
        print(json.dumps(Collector(fs, run, cfg).metrics(time.monotonic()), indent=1, ensure_ascii=False))
    elif cmd == "hub-rule":
        if len(argv) < 3 or argv[2] not in ("install", "remove"):
            raise SystemExit("usage: reimu-hostd.py hub-rule install|remove")
        try:
            hub_rule(fs, run, argv[2])
        except OSError as e:
            print("the udev rule: %s" % e)
            return 1
    elif cmd == "bus-owner":
        if len(argv) < 3 or argv[2] not in ("install", "remove"):
            raise SystemExit("usage: reimu-hostd.py bus-owner install|remove")
        try:
            if not bus_owner(fs, run, argv[2], cfg):
                return 1
        except OSError as e:
            log_error("the BMC's I2C buses: %s" % e)
            return 1
    elif cmd == "restore-auto":
        f = GpuFan(fs, load=False)
        f.touched = True
        f.restore_auto("restore-auto")
    elif cmd == "pair":
        if "--pair" not in opt:
            raise SystemExit("usage: reimu-hostd.py pair --pair 'reimu1:...'")
        pair(opt["--pair"], fs, conf, secret)
    elif cmd == "install":
        do_install(opt.get("--pair"))
    elif cmd == "uninstall":
        do_uninstall()
    elif cmd == "status":
        s = load_secret(secret)
        print("config %s: BMC_MAC=%s BMC_IPV4=%s BMC=%s port %d" % (conf, cfg["BMC_MAC"], cfg["BMC_IPV4"], cfg["BMC"], cfg["BMC_PORT"]))
        print("paired: %s" % ("yes, certificate %s…" % s["FINGERPRINT"][:16] if s.get("KEY") else "no"))
        print("link-local candidates: %s" % ", ".join(fmt_cand(c) for c in candidates(cfg, fs, [])))
        print(Init(fs, run).status("reimu-hostd")[1])
    elif cmd == "run":
        s = load_secret(secret)
        if not s.get("KEY") or not s.get("FINGERPRINT"):
            raise SystemExit("not paired: reimu-hostd.py pair --pair 'reimu1:...' (the BMC's web page Хост)")
        ag = Agent(cfg, s, fs, run, secret_path=secret, conf_path=conf)

        def stop(sig, frm):
            ag.stop = True
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        log("reimu-hostd %s: interval %d s, GPU fan floor %d %%, allowed: %s" % (
            VERSION, cfg["INTERVAL"], cfg["GPU_FAN_FLOOR"], " ".join(sorted(cfg["ALLOW"])) or "nothing"))
        ag.run_forever()
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
