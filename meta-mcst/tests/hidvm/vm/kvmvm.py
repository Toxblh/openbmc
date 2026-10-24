#!/usr/bin/env python3
"""VM side of the KVM HID tests (runs inside the HID test VM, kernel 6.18 with
dummy_hcd: the VM is both the BMC, with the configfs gadget obmc_hid made by
create_usbhid.sh, and the USB host that enumerates it).

    kvmvm.py gadget-up | gadget-down | kvm-start | kvm-stop | SCENARIO ...

The pieces (a websocket client of reimu-kvm, the host's evdev devices of the
gadget, hidraw for control transfers) are importable for the scenarios.
"""
import fcntl, glob, json, os, re, select, socket, ssl, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
G = "/sys/kernel/config/usb_gadget/obmc_hid"
UDC = "dummy_udc.0"
BB = "/tmp/bb"  # busybox applets: the gadget scripts run as on the BMC
EV_KEY, EV_ABS, EV_REL, EV_SYN, EV_REP = 1, 3, 2, 0, 0x14
KEY_A = 30


def sh(cmd, check=True, **kw):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, **kw)
    if check and r.returncode:
        raise RuntimeError("%s: rc %d %s %s" % (cmd, r.returncode, r.stdout, r.stderr))
    return r


def busybox_path():
    if not os.path.isdir(BB):
        os.makedirs(BB)
        for a in sh("busybox --list").stdout.split():
            os.symlink("/usr/bin/busybox", os.path.join(BB, a))
    return BB


def gadget_up(script):
    """create_usbhid.sh under busybox sh, bound to dummy_udc.0 instead of a vhub port"""
    s = open(script).read()
    s = s.replace('echo "${dev_name}:p${port}" > UDC', 'echo %s > UDC' % UDC)
    open("/tmp/create_usbhid.sh", "w").write(s)
    env = dict(os.environ, PATH=busybox_path())
    r = subprocess.run(["/usr/bin/busybox", "sh", "/tmp/create_usbhid.sh"], capture_output=True, text=True, env=env)
    return r.returncode, r.stdout + r.stderr


def gadget_down():
    if not os.path.exists(G):
        return
    sh("echo '' > %s/UDC" % G, check=False)
    for f in glob.glob(G + "/configs/c.1/hid.*"):
        os.unlink(f)
    for d in [G + "/configs/c.1/strings/0x409", G + "/configs/c.1"] + sorted(glob.glob(G + "/functions/*")) + \
             [G + "/strings/0x409", G]:
        try:
            os.rmdir(d)
        except OSError:
            pass


def udc_state():
    try:
        return open("/sys/class/udc/%s/state" % UDC).read().strip()
    except OSError:
        return "?"


def wait_configured(timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        if udc_state() == "configured" and len(input_devices()) == 2:
            return True
        time.sleep(0.1)
    return False


# ------------------------------------------------------------------ host side
def input_devices():
    """{"kbd": /dev/input/eventN, "ptr": ...} of the gadget (vendor 1d6b product 0104)"""
    out = {}
    for blk in open("/proc/bus/input/devices").read().split("\n\n"):
        if "Vendor=1d6b Product=0104" not in blk:
            continue
        ev = re.search(r"event(\d+)", blk)
        if not ev:
            continue
        dev = "/dev/input/event" + ev.group(1)
        if re.search(r"B: ABS=", blk):
            out["ptr"] = dev
        elif re.search(r"B: LED=", blk) or "Keyboard" in blk or "kbd" in blk:
            out.setdefault("kbd", dev)
    return out


class Events:
    """the host's events of one evdev device"""
    FMT = "llHHi"
    SZ = struct.calcsize(FMT)

    def __init__(self, dev):
        self.fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
        self.ev = []

    def read(self, quiet=0.5, timeout=30):
        """until nothing came for `quiet` seconds"""
        end = time.time() + timeout
        last = time.time()
        while time.time() < end and time.time() - last < quiet:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if not r:
                continue
            try:
                b = os.read(self.fd, self.SZ * 256)
            except BlockingIOError:
                continue
            for i in range(0, len(b), self.SZ):
                s, us, t, c, v = struct.unpack(self.FMT, b[i:i + self.SZ])
                self.ev.append((s + us / 1e6, t, c, v))
            last = time.time()
        return self.ev

    def keys(self, code=None):
        return [(c, v) for _, t, c, v in self.ev if t == EV_KEY and (code is None or c == code)]

    def close(self):
        os.close(self.fd)


# ------------------------------------------------------------------ reimu-kvm
CERT = "/tmp/kvm.pem"


def kvm_start(binary="/tmp/reimu-kvm", args=""):
    if not os.path.exists(CERT):
        sh("openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -subj /CN=kvm "
           "-keyout %s -out %s.crt -days 30 2>/dev/null && cat %s.crt >> %s" % (CERT, CERT, CERT, CERT))
    kvm_stop()
    log = open("/tmp/reimu-kvm.log", "w")
    p = subprocess.Popen([binary, "-c", CERT, "-v", "/dev/novideo"] + args.split(), stdout=log, stderr=log)
    open("/tmp/reimu-kvm.pid", "w").write(str(p.pid))
    for _ in range(50):
        if "reimu-kvm on" in open("/tmp/reimu-kvm.log").read():
            return p.pid
        time.sleep(0.1)
    raise RuntimeError("reimu-kvm did not start: " + open("/tmp/reimu-kvm.log").read())


def kvm_stop(sig="TERM"):
    sh("pkill -%s -x reimu-kvm; sleep 0.3" % sig, check=False)


def ticket():
    out = sh("busctl call xyz.openbmc_project.Reimu.Kvm /xyz/openbmc_project/reimu/kvm "
             "xyz.openbmc_project.Reimu.Kvm Ticket").stdout
    j = json.loads(json.loads(out.split(" ", 1)[1]))
    return j["ticket"], j["port"]


class Ws:
    """a websocket client of reimu-kvm (what directkvm.js speaks)"""

    def __init__(self):
        t, port = ticket()
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE
        self.s = ctx.wrap_socket(socket.create_connection(("127.0.0.1", port)))
        self.s.sendall(("GET /kvm?t=%s HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n" % t).encode())
        h = b""
        while b"\r\n\r\n" not in h:
            h += self.s.recv(1)
        if b" 101 " not in h.split(b"\r\n")[0]:
            raise RuntimeError(h.decode(errors="replace"))
        self.s.setblocking(False)

    @staticmethod
    def frame(payload, op=2):
        m = os.urandom(4)
        n = len(payload)
        h = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        return h + m + bytes(b ^ m[i & 3] for i, b in enumerate(payload))

    def send(self, *payloads):
        """all the messages in one write (one TLS record if it fits)"""
        data = b"".join(self.frame(p) for p in payloads)
        self.s.setblocking(True)
        self.s.sendall(data)
        self.s.setblocking(False)

    def drain(self):
        try:
            while self.s.recv(65536):
                pass
        except (ssl.SSLWantReadError, BlockingIOError):
            pass

    def close(self, clean=True):
        try:
            if clean:
                self.send_close()
            self.s.close()
        except OSError:
            pass

    def send_close(self):
        self.s.setblocking(True)
        self.s.sendall(self.frame(b"\x03\xe8", op=8))


def key(usage, down):
    return bytes([2, usage, 1 if down else 0])


def ptr(buttons, x, y):
    return struct.pack("<BBHH", 3, buttons, x, y)


def wheel(w, p=0):
    return struct.pack("<Bbb", 9, w, p)


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "gadget-up":
        rc, out = gadget_up(sys.argv[2])
        print(out, end="")
        print("rc=%d configured=%s inputs=%s" % (rc, wait_configured(), input_devices()))
        sys.exit(rc)
    elif cmd == "gadget-down":
        gadget_down()
    elif cmd == "kvm-start":
        print(kvm_start(*sys.argv[2:3]))
    elif cmd == "kvm-stop":
        kvm_stop()
