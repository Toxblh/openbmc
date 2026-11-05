#!/usr/bin/env python3
"""reimu-hostlink (the BMC's end of the host agent) on the A/B image in QEMU,
with the real agent reimu-hostd (meta-mcst/tools/host) on this machine
against a fake /sys of the Elbrus host.

    test_hostlink_qemu.py WORKDIR [TEST...]

WORKDIR, RQ, AB_SSH, AB_HTTPS as for test_ab_qemu.py, plus the forward of the
link's port: AB_NETDEV_EXTRA=",hostfwd=tcp:127.0.0.1:PORT-:5564" and
HL_PORT=PORT (default 3364). QEMU's user network has no link-local path to
this machine: the agent comes in over IPv4 (the forward), the IPv6
link-local listener is checked inside the VM. About 10 minutes:

    RQ=/tmp/rqhl AB_SSH=3322 AB_HTTPS=3343 HL_PORT=3364 \\
    AB_NETDEV_EXTRA=",hostfwd=tcp:127.0.0.1:3364-:5564" ./test_hostlink_qemu.py WORKDIR

Tests (all by default, one boot):

  service   active, Type=notify, listening on [::]:5564 (TCP, UDP) bound to
            eth0, IPv6 link-local inside the VM reaches it, the discovery
            answer; usb0 (reimu-usbnet) in IFACE too; UsbNet, UsbNetSet on
            D-Bus; D-Bus service, sensors objects, REST
  pair      Pair over REST (the web button): the pairing string pins the
            certificate the forward shows; reimu-hostd.py pair with it
  agent     the agent runs: connected (REST Json), inventory, metrics; the
            D-Bus sensors (Sensor.Value) and Redfish; run/gpu-temp; ECC CE
            warning in the journal and in phosphor-logging
  control   SetGpuFan over REST -> the fake pwm1 on this machine; a "GPU
            reset" re-applied; GpuTurbo; reimu-fand's SetTurbo followed;
            OsPower -> systemctl (the fake runner); bad arguments refused
  kvm       the agent reports its reimu-h264 (a fake on this machine at
            HL_PORT+3): reimu-hostlink tells reimu-kvm, the stat carries the
            host, the page's request is relayed (a key frame through a wss
            client on a forward HL_PORT+4 -> 8443 added by the monitor); its
            service gone: hoststate not_installed. KVM_BINS=DIR: reimu-kvm and
            reimu-hostlink from DIR over the image's
  drop      the daemon stopped: the agent returns the GPU to the driver
            (pwm1_enable 2) after LINK_LOST_SEC; started: it reconnects and
            sets the policy again; SIGKILL: systemd brings it back; the agent
            killed: agent_lost (warning) after LOST_SEC, back again
  web       the page js/pages/host.js and the "Хост" link served; with
            HL_E2E=DIR (node_modules/playwright there) the browser test
            ../e2e/host.qemu.mjs of the page (copied there; CHROME: chromium)
  memory    RSS of reimu-hostlink, its fds steady
"""
import base64, hashlib, json, os, re, signal, socket, ssl, struct, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ab_qemu as ab  # noqa: E402  (takes WORKDIR from sys.argv[1])
from test_ab_qemu import check, failed, fresh, redfish  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
HL_PORT = int(os.environ.get("HL_PORT", 3364))
SVC = "xyz.openbmc_project.Reimu.HostLink"
OBJ = "/xyz/openbmc_project/reimu/hostlink"


def find_tools():
    for d in (os.environ.get("HOSTD_TOOLS", ""), os.path.join(HERE, "..", "..", "tools", "host"), os.path.join(HERE, "host")):
        if d and os.path.exists(os.path.join(d, "reimu-hostd.py")):
            return os.path.abspath(d)
    sys.exit("reimu-hostd.py not found (HOSTD_TOOLS=dir)")


TOOLS = find_tools()
AGENT = os.path.join(TOOLS, "reimu-hostd.py")
sys.path.insert(0, os.path.join(TOOLS, "tests"))
import test_reimu_hostd as th  # noqa: E402  (the fake board)


def note(cond, what):
    """reported, not a failure (what QEMU lacks: no FRU, no chassis)"""
    print(("  PASS " if cond else "  INFO ") + what, flush=True)


def wait(fn, timeout, step=1.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            v = fn()
        except Exception:
            v = None
        if v:
            return v
        time.sleep(step)
    try:
        return fn()
    except Exception:
        return None


def state():
    s, j = redfish("GET", OBJ)
    return json.loads(j["data"]["Json"]) if s == 200 else None


def action(method, *args):
    s, j = redfish("POST", "%s/action/%s" % (OBJ, method), {"data": list(args)})
    return s, (j or {}).get("data") if isinstance(j, dict) else j


class Host:
    def __init__(self):
        self.t = th.Tree()
        th.board(self.t)
        self.root = self.t.root
        self.conf = os.path.join(self.root, "reimu-hostd.conf")
        self.secret = os.path.join(self.root, "secret")
        open(self.conf, "w").write("BMC=127.0.0.1:%d\nINTERVAL=3\nLINK_LOST_SEC=15\n" % HL_PORT)
        self.env = dict(os.environ, REIMU_HOSTD_ROOT=self.root, REIMU_HOSTD_CONF=self.conf, REIMU_HOSTD_SECRET=self.secret)
        self.p = None
        self.log = os.path.join(ab.W, "hostd.log")
        self.hw = th.GPU_DEV + "/hwmon/hwmon2"

    def run(self, *a):
        return subprocess.run([sys.executable, AGENT] + list(a), env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=60)

    def start(self):
        self.p = subprocess.Popen([sys.executable, AGENT, "run"], env=self.env, stdout=open(self.log, "a"), stderr=subprocess.STDOUT)

    def stop(self, sig=signal.SIGTERM):
        if self.p:
            self.p.send_signal(sig)
            self.p.wait(20)
            self.p = None

    def pwm(self):
        try:
            return self.t.get(self.hw + "/pwm1_enable"), self.t.get(self.hw + "/pwm1")
        except OSError:
            return None, None

    def commands(self):
        try:
            return open(os.path.join(self.root, "commands.log")).read()
        except OSError:
            return ""


def tls_fp():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE
    with ctx.wrap_socket(socket.create_connection(("127.0.0.1", HL_PORT), timeout=10)) as s:
        return hashlib.sha256(s.getpeercert(binary_form=True)).hexdigest()


def prop(b, path, iface, name):
    r = b.ssh("busctl get-property %s %s %s %s" % (SVC, path, iface, name))
    return r.stdout.strip() if r.returncode == 0 else None


def main():
    b = ab.Bmc("hl", fresh("hl")).start()
    h = Host()
    try:
        check(b.wait_ssh(), "BMC up (ssh)")
        t_service(b)
        p = t_pair(b, h)
        if p:
            h.start()
            t_agent(b, h)
            t_control(b, h)
            t_e2e()
            t_kvm(b, h)
            t_drop(b, h)
        t_web(b)
        t_memory(b)
    finally:
        h.stop()
        b.stop()
        h.t.cleanup()


def t_service(b):
    print("== service", flush=True)
    check(b.out("systemctl is-active reimu-hostlink").strip() == "active", "reimu-hostlink active")
    sh = set(b.out("systemctl show -p Type,Restart,WatchdogUSec reimu-hostlink").split())
    check(sh == {"Type=notify", "Restart=always", "WatchdogUSec=30s"}, "Type=notify, Restart=always, watchdog 30 s: %s" % sorted(sh))
    ns = b.out("netstat -ltnu 2>/dev/null | grep ':5564 '")
    check(":::5564" in ns and "tcp" in ns and "udp" in ns, "listening on [::]:5564 TCP and UDP: %s" % " | ".join(ns.split("\n")))
    j = b.out("journalctl -u reimu-hostlink -b --no-pager -o cat")
    check("listening on TCP+UDP [::]:5564 on eth0" in j, "bound to eth0 (journal)")
    ll = b.out("ip -6 addr show dev eth0 scope link | sed -n 's/.*inet6 \\([^/]*\\).*/\\1/p'").strip()
    check(ll.startswith("fe80::"), "eth0 link-local %s" % ll)
    # TCP to the link-local address inside the VM (scope eth0): the listener
    # takes it (busybox nc: plain connect, no TLS -> "handshake failed")
    b.ssh("(sleep 2) | nc %s%%eth0 5564; sleep 1" % ll)
    j = b.out("journalctl -u reimu-hostlink -b --no-pager -o cat")
    check("(%s%%eth0) closed" % ll in j, "a connection to [%s%%eth0]:5564 reaches it (IPv6 link-local)" % ll)
    r = b.ssh("(sleep 1) | nc ::1 5564")
    check("refused" in r.stderr, "[::1]:5564 refused: bound to eth0 only (%s)" % r.stderr.strip())
    # the discovery probe (bash's /dev/udp: busybox nc has no UDP)
    probe = ("exec 3<>/dev/udp/%s/5564 && printf 'REIMU-HOSTLINK? 1' >&3 && timeout 2 dd bs=128 count=1 <&3 2>/dev/null")
    r = b.ssh(probe % (ll + "%eth0"))
    fp = (state() or {}).get("fingerprint", "")
    check(r.stdout.startswith("REIMU-HOSTLINK 1 5564 ") and fp.startswith(r.stdout.split()[-1]),
          "discovery answer over link-local, the fingerprint's prefix: %r" % r.stdout.strip())
    r = b.ssh(probe % "10.0.2.15")
    check(r.stdout.strip() == "", "no discovery answer to a non-link-local peer")
    # the USB network with the host (reimu-usbnet's usb0) is in IFACE too: listened
    # on, or waited for while it isn't there
    check(re.search(r"(listening on TCP\+UDP \[::\]:5564|not listening) on usb0", j) is not None,
          "usb0 in IFACE: listened on or waited for (journal)")
    r = b.ssh("busctl call %s %s %s UsbNet" % (SVC, OBJ, SVC))
    check(r.returncode == 0 and r.stdout.startswith('s "{') and ("enabled" in r.stdout or "error" in r.stdout),
          "D-Bus UsbNet: reimu-usbnet's status as JSON (%s)" % r.stdout.strip()[:160])
    r = b.ssh("busctl call %s %s %s UsbNetSet sb reboot true" % (SVC, OBJ, SVC))
    check(r.returncode != 0 and "unknown setting" in r.stderr, "UsbNetSet: only internet / enabled (%s)" % r.stderr.strip())
    check(prop(b, OBJ, SVC, "Paired") == "b false", "D-Bus: Paired false")
    sens = b.out("busctl tree %s | grep -o '/xyz/openbmc_project/sensors/[^ ]*Host_[A-Za-z_]*'" % SVC).split()
    check(len(sens) == 7 and "/xyz/openbmc_project/sensors/temperature/Host_GPU_Junction" in sens, "7 sensor objects: %s" % sens)
    st = state()
    check(st is not None and st["paired"] is False and st["heartbeat"]["state"] == "never", "REST: the Json")
    check(st and st.get("mac", "").startswith("98:a7:b0"), "its eth0 MAC for the pairing (%s)" % (st or {}).get("mac"))


def t_pair(b, h):
    print("== pair", flush=True)
    s, data = action("Pair")
    ok = check(s == 200 and data, "REST Pair: %s" % s)
    if not ok:
        return None
    p = json.loads(data)
    raw = p["pair"][7:]
    d = json.loads(base64.urlsafe_b64decode(raw + "=" * (-len(raw) % 4)))
    check(d["fp"] == tls_fp(), "the pairing string pins the certificate the link shows")
    check(d["mac"] == state()["mac"] and p["link_local"].startswith("fe80::"), "the BMC's MAC and link-local (%s)" % p["link_local"])
    check("reimu-hostd.py install --pair" in p["command"], "the install command")
    j = b.out("journalctl -b --no-pager -o cat | grep -c %s" % d["token"]).strip()
    check(j == "0", "the token not in the BMC's journal")
    r = h.run("pair", "--pair", p["pair"], "--config", h.conf, "--secret", h.secret)
    ok = check(r.returncode == 0, "reimu-hostd.py pair: %s" % r.stdout.decode().strip()[-200:])
    check(prop(b, OBJ, SVC, "Paired") == "b true", "D-Bus: Paired true")
    key = b.out("cat /var/lib/reimu-hostlink/key").strip()
    check(len(key) == 64 and b.out("stat -c %a /var/lib/reimu-hostlink/key").strip() == "600", "the key on the rwfs, 0600")
    check(b.out("stat -c %a /var/lib/reimu-hostlink").strip() == "700", "its directory 0700")
    return ok


def t_agent(b, h):
    print("== agent", flush=True)
    st = wait(lambda: (lambda s: s if s["agent"]["connected"] and s["metrics"] and s["inventory"] else None)(state()), 60)
    check(st is not None, "the agent connected, inventory and metrics in")
    if not st:
        print(open(h.log).read()[-2000:])
        return
    check(st["agent"]["transport"] == "ipv4", "over IPv4 (the QEMU forward): %s %s" % (st["agent"]["transport"], st["agent"]["peer"]))
    check(st["inventory"]["cpu"]["model"] == "E8C2" and len(st["inventory"]["pci"]) == 4, "inventory: E8C2, 4 PCIe devices")
    hub = wait(lambda: (state()["metrics"] or {}).get("bmc_usb_hub"), 15) or {}
    check(hub.get("control") == "on" and h.t.get("/sys/bus/usb/devices/4-4/power/control") == "on",
          "the BMC's USB hub (fake 1d6b:0107 at auto): kept awake by the agent, the BMC sees it: %s" % hub)
    check(st["sensors"]["Host_GPU_Edge"] == 52 and st["sensors"]["Host_GPU_Junction"] == 61, "sensors in the Json")
    v = wait(lambda: prop(b, "/xyz/openbmc_project/sensors/temperature/Host_GPU_Junction", "xyz.openbmc_project.Sensor.Value", "Value"), 10)
    check(v == "d 61", "D-Bus Sensor.Value Host_GPU_Junction: %s" % v)
    v = prop(b, "/xyz/openbmc_project/sensors/fan_tach/Host_GPU_Fan", "xyz.openbmc_project.Sensor.Value", "Unit")
    check(v == 's "xyz.openbmc_project.Sensor.Value.Unit.RPMS"', "fan in RPMS: %s" % v)
    v = prop(b, "/xyz/openbmc_project/sensors/temperature/Host_GPU_Junction", "xyz.openbmc_project.Sensor.Threshold.Critical",
             "CriticalHigh")
    check(v == "d 105", "threshold critical 105: %s" % v)
    # the mapper sees them (Redfish, IPMI find sensors through it)
    r = b.out("busctl call xyz.openbmc_project.ObjectMapper /xyz/openbmc_project/object_mapper "
              "xyz.openbmc_project.ObjectMapper GetSubTreePaths sias /xyz/openbmc_project/sensors 0 1 "
              "xyz.openbmc_project.Sensor.Value")
    check("Host_GPU_Edge" in r, "the mapper lists the host sensors")
    r = b.out("busctl get-property xyz.openbmc_project.ObjectMapper "
              "/xyz/openbmc_project/inventory/system/chassis/motherboard/all_sensors xyz.openbmc_project.Association endpoints")
    note("Host_GPU_Edge" in r, "chassis all_sensors association: %s" % (r.strip()[:120] or "no chassis object (no FRU in QEMU)"))
    s, j = redfish("GET", "/redfish/v1/Chassis/motherboard/Thermal")
    names = [x.get("Name") for x in (j or {}).get("Temperatures", [])] if s == 200 else []
    (check if s == 200 else note)(any("Host_GPU" in (n or "") for n in names),
          "Redfish Chassis motherboard Thermal: %s" % (names if s == 200 else "HTTP %s (no chassis in QEMU)" % s))
    t = wait(lambda: b.out("cat /run/reimu-hostlink/gpu-temp").strip(), 10)
    check(t == "61000", "run/gpu-temp for reimu-fand: %s" % t)
    j = b.out("journalctl -u reimu-hostlink -b --no-pager -o cat")
    check("corrected ECC errors 0 -> 4" in j, "journal: ECC CE warning")
    e = wait(lambda: "REIMU host: host memory: corrected ECC" in b.out(
        "busctl tree xyz.openbmc_project.Logging | grep -o '/xyz/openbmc_project/logging/entry/[0-9]*' | "
        "while read p; do busctl get-property xyz.openbmc_project.Logging $p xyz.openbmc_project.Logging.Entry Message; done"), 15)
    check(e, "phosphor-logging entry for it (Redfish EventLog)")
    check(prop(b, OBJ, SVC, "Heartbeat") == 's "ok"', "Heartbeat ok")


def t_control(b, h):
    print("== control", flush=True)
    s, r = action("SetGpuFan", "manual", 45)
    check(s == 200, "REST SetGpuFan manual 45: %s %s" % (s, r))
    check(wait(lambda: h.pwm() == ("1", "115"), 15), "the host's pwm1 115, pwm1_enable 1 (%s)" % (h.pwm(),))
    h.t.put(h.hw + "/pwm1_enable", "2")
    h.t.put(h.hw + "/pwm1", "0")
    check(wait(lambda: h.pwm() == ("1", "115"), 10), "after a 'GPU reset' set again by the agent")
    check(wait(lambda: any("gpu_fan_reapplied" in e["text"] for e in state()["events"]), 15), "the BMC has the event")
    s, r = action("SetGpuFan", "manual", 5)
    check(s >= 400, "duty 5 refused at the BMC (%s)" % s)
    s, r = action("SetGpuFan", "jet", 50)
    check(s >= 400, "a bad mode refused (%s)" % s)
    s, r = action("GpuTurbo", 1)
    check(s == 200 and wait(lambda: h.pwm() == ("1", "255"), 15), "GpuTurbo 1 min: 255")
    action("GpuTurbo", 0)
    check(wait(lambda: h.pwm() == ("1", "115"), 15), "turbo off: 45 % again")
    r = b.ssh("busctl call xyz.openbmc_project.Reimu.Fans /xyz/openbmc_project/reimu/fans xyz.openbmc_project.Reimu.Fans SetTurbo u 1")
    if r.returncode == 0:
        check(wait(lambda: h.pwm() == ("1", "255"), 20), "reimu-fand's Турбо: the GPU fan 255 too")
        b.ssh("busctl call xyz.openbmc_project.Reimu.Fans /xyz/openbmc_project/reimu/fans xyz.openbmc_project.Reimu.Fans SetTurbo u 0")
        check(wait(lambda: h.pwm() == ("1", "115"), 20), "fand's Турбо off: back to 45 %")
    else:
        note(False, "reimu-fand SetTurbo not available here: %s" % r.stderr.strip()[:100])
    s, r = action("OsPower", "reboot")
    check(s == 200 and wait(lambda: "systemctl reboot" in h.commands(), 15), "OsPower reboot -> systemctl reboot on the host")
    s, r = action("OsPower", "rm -rf /")
    check(s >= 400, "OsPower with another action refused (%s)" % s)
    s, r = action("LoadEdac")
    check(s == 200 and wait(lambda: "modprobe e2k_edac" in h.commands(), 15), "LoadEdac -> modprobe e2k_edac (the module of the board's kernel)")
    check(wait(lambda: sum(1 for x in state()["results"] if x["done"] and x["ok"]) >= 2, 15), "both results back")
    # the host's report in the diagnostic archive (off by default)
    r = b.ssh("reimu-diag --with-host", timeout=200)
    p = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
    o = b.out("cd /tmp && rm -rf hd && mkdir hd && cd hd && tar -xzf '%s' && cat */host-report.json; grep -h host-report */MANIFEST; rm -f '%s'" % (p, p), timeout=60)
    check(r.returncode == 0 and '"kernel_log"' in o and '"inventory"' in o, "reimu-diag --with-host: the agent's report in the archive (%s)" % o[-120:].strip())


def t_e2e():
    d = os.environ.get("HL_E2E")
    if not d:
        return
    print("== e2e (browser)", flush=True)
    import shutil
    for f in ("host.qemu.mjs", "common.mjs"):
        src = next(p for p in (os.path.join(HERE, "..", "e2e", f), os.path.join(HERE, f)) if os.path.exists(p))
        if not os.path.exists(os.path.join(d, f)) or not os.path.samefile(src, os.path.join(d, f)):
            shutil.copy(src, os.path.join(d, f))
    r = subprocess.run(["node", "host.qemu.mjs"], cwd=d, env=dict(os.environ, BASE="https://127.0.0.1:%d" % ab.HTTPS,
                       OUT=os.path.join(ab.W, "shots")), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
    out = r.stdout.decode()
    for l in out.splitlines():
        if l.startswith(("PASS", "FAIL", "INFO")):
            print("    " + l)
    check(r.returncode == 0, "browser test of the «Хост» page (screenshots in WORKDIR/shots)")
    if r.returncode:
        print(out[-1500:])
    action("SetGpuFan", "manual", 45)  # the page test left it on auto


def hmp(b, line):
    """a monitor command of the BMC's QEMU (its second QMP socket)"""
    s = socket.socket(socket.AF_UNIX)
    s.connect("%s/%s.qmp2" % (ab.RQ, b.name))
    f = s.makefile("rw")
    f.readline()
    f.write('{"execute": "qmp_capabilities"}\n')
    f.flush()
    f.readline()
    f.write('{"execute": "human-monitor-command", "arguments": {"command-line": "%s"}}\n' % line)
    f.flush()
    r = f.readline()
    s.close()
    return r


class FakeH264:
    """reimu-h264 on this machine (the guest reaches it as 10.0.2.2): the
    BMC's request line, then the stream header and one key frame (H.265 when
    the request says "hevc")"""

    def __init__(self, port):
        self.port, self.requests = port, []
        self.s = socket.socket()
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind(("127.0.0.1", port))
        self.s.listen(4)
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            try:
                c, _ = self.s.accept()
            except OSError:
                return
            c.settimeout(3)
            try:
                req = c.recv(64).decode(errors="replace").strip()
                self.requests.append(req)
                if "hevc" in req.split()[1:]:
                    c.sendall(b'{"codec":"hev1.1.6.L123.B0","w":1920,"h":1080,"fps":60,"audio":""}\n')
                    au = b"\x00\x00\x00\x01\x26\x01" + b"\x88" * 100  # IDR_W_RADL
                else:
                    c.sendall(b'{"codec":"avc1.640028","w":1920,"h":1080,"fps":60,"audio":""}\n')
                    au = b"\x00\x00\x00\x01\x65" + b"\x88" * 100
                c.sendall(struct.pack(">IB", len(au), 1) + au)
                time.sleep(2)
            except OSError:
                pass
            c.close()

    def close(self):
        self.s.close()


class Ws:
    """a websocket client of reimu-kvm (wss, the ticket from D-Bus)"""

    def __init__(self, port, ticket):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE
        self.s = ctx.wrap_socket(socket.create_connection(("127.0.0.1", port), timeout=20))
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET /kvm?t=%s HTTP/1.1\r\nHost: bmc\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (ticket, key)).encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            d = self.s.recv(4096)
            if not d:
                raise OSError("closed in the handshake")
            self.buf += d
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise OSError(head.split(b"\r\n")[0].decode())

    def _need(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d:
                raise OSError("closed")
            self.buf += d

    def recv(self):
        """(opcode, payload)"""
        self._need(2)
        op, ln = self.buf[0] & 15, self.buf[1] & 127
        off = 2
        if ln == 126:
            self._need(4)
            ln, off = struct.unpack(">H", self.buf[2:4])[0], 4
        elif ln == 127:
            self._need(10)
            ln, off = struct.unpack(">Q", self.buf[2:10])[0], 10
        self._need(off + ln)
        p, self.buf = self.buf[off:off + ln], self.buf[off + ln:]
        return op, p

    def send_bin(self, data):
        mask = os.urandom(4)
        self.s.sendall(bytes([0x82, 0x80 | len(data)]) + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(data)))

    def until(self, fn, timeout):
        """the first message fn(op, payload) takes (returns not None), or None"""
        end = time.time() + timeout
        self.s.settimeout(2)
        while time.time() < end:
            try:
                op, p = self.recv()
            except socket.timeout:
                continue
            v = fn(op, p)
            if v is not None:
                return v
        return None

    def close(self):
        self.s.close()


def stat_of(op, p):
    if op == 1:
        m = json.loads(p)
        if m.get("t") == "stat":
            return m
    return None


def t_kvm(b, h):
    """the host's H.264 from the agent's word: the agent reports its
    reimu-h264 (kvm.h264 in the metrics), reimu-hostlink tells reimu-kvm the
    agent's address, the page's request (7, no address) is relayed. Before:
    only reimu-h264's own UDP announcement (an old reimu-h264 had none: the
    board's KVM page stayed on JPEG, 2026-11-03). KVM_BINS=DIR: reimu-kvm and
    reimu-hostlink from DIR bind-mounted over the image's (a build of the
    tree before the image)"""
    print("== kvm", flush=True)
    bins = os.environ.get("KVM_BINS")
    if bins:
        ok = True
        for n in ("reimu-kvm", "reimu-hostlink"):
            ok &= b.scp(os.path.join(bins, n), "/tmp/%s.new" % n)
            b.ssh("chmod +x /tmp/%s.new; umount /usr/sbin/%s 2>/dev/null; mount --bind /tmp/%s.new /usr/sbin/%s" % (n, n, n, n))
        b.ssh("systemctl restart reimu-hostlink reimu-kvm")
        check(ok, "KVM_BINS: reimu-kvm and reimu-hostlink of %s in place" % bins)
        wait(lambda: state()["agent"]["connected"], 60)
    hport, kport = HL_PORT + 3, HL_PORT + 4
    fake = FakeH264(hport)
    h.t.put("/etc/systemd/system/reimu-h264.service",
            "[Service]\nExecStart=/usr/bin/python3 /usr/local/lib/reimu/reimu-h264.py --port %d\n" % hport)
    ws = None
    try:
        k = wait(lambda: ((state()["metrics"] or {}).get("kvm") or {}).get("h264", {}).get("state") == "active", 40)
        check(k, "the agent reports its reimu-h264 active (metrics kvm.h264)")
        want = "10.0.2.2:%d" % hport
        j = wait(lambda: want in b.out("journalctl -u reimu-kvm -b --no-pager -o cat | grep 'host agent'"), 30)
        check(j, "reimu-kvm: host agent: %s (from reimu-hostlink)" % want)
        hmp(b, "hostfwd_remove n0 tcp:127.0.0.1:%d" % kport)
        hmp(b, "hostfwd_add n0 tcp:127.0.0.1:%d-:8443" % kport)
        t = b.out("busctl call xyz.openbmc_project.Reimu.Kvm /xyz/openbmc_project/reimu/kvm xyz.openbmc_project.Reimu.Kvm Ticket")
        try:
            tk = json.loads(json.loads(t.split(" ", 1)[1]))
        except (ValueError, IndexError):
            tk = {}
        if not check(tk.get("ticket"), "a ticket from reimu-kvm: %s" % t.strip()[:80]):
            return
        ws = Ws(kport, tk["ticket"])
        st = ws.until(lambda op, p: (lambda m: m if m and m.get("host") else None)(stat_of(op, p)), 20)
        check(st and st["host"] == want and st.get("hoststate") == "",
              "the page's stat: host %s, no hoststate (%r)" % (want, st and (st.get("host"), st.get("hoststate"))))
        ws.send_bin(b"\x07")  # the page in Авто: the host that reimu-kvm knows
        hdr = ws.until(lambda op, p: (lambda m: m if m.get("t") in ("h264", "h264off") else None)(json.loads(p)) if op == 1 else None, 20)
        check(hdr and hdr["t"] == "h264" and hdr.get("w") == 1920, "the host's stream starts: %s" % hdr)
        fr = ws.until(lambda op, p: p if op == 2 and p[:1] == b"\x06" else None, 15)
        check(fr is not None and fr[1] == 1 and fr[2:7] == b"\x00\x00\x00\x01\x65", "a key frame relayed (type 6)")
        check(fake.requests[:1] == ["VIDEO"], "reimu-h264 got the BMC's VIDEO request: %s" % fake.requests)
        # a page that decodes H.265 says so after the address (none: the
        # agent's host); reimu-kvm passes it on, the host's header says what comes
        ws.send_bin(b"\x07\nhevc")
        hdr = ws.until(lambda op, p: (lambda m: m if m.get("t") in ("h264", "h264off") else None)(json.loads(p)) if op == 1 else None, 20)
        check(hdr and hdr["t"] == "h264" and str(hdr.get("codec", "")).startswith("hev1"), "H.265 asked for: the host's H.265 header relayed: %s" % hdr)
        fr = ws.until(lambda op, p: p if op == 2 and p[:1] == b"\x06" else None, 15)
        check(fr is not None and fr[1] == 1 and fr[2:8] == b"\x00\x00\x00\x01\x26\x01", "an H.265 key frame relayed (type 6)")
        check(fake.requests[1:2] == ["VIDEO hevc"], "reimu-h264 got the BMC's request for H.265: %s" % fake.requests)
        ws.close()
        ws = None
        # the agent says why there is none
        h.t.rm("/etc/systemd/system/reimu-h264.service")
        tk = json.loads(json.loads(b.out("busctl call xyz.openbmc_project.Reimu.Kvm /xyz/openbmc_project/reimu/kvm "
                                         "xyz.openbmc_project.Reimu.Kvm Ticket").split(" ", 1)[1]))
        ws = Ws(kport, tk["ticket"])
        st = ws.until(lambda op, p: (lambda m: m if m and m.get("hoststate") else None)(stat_of(op, p)), 40)
        check(st and st["hoststate"] == "not_installed" and st["host"] == "",
              "its service gone: no host, hoststate not_installed (%r)" % ((st and (st.get("host"), st.get("hoststate"))),))
    finally:
        if ws:
            ws.close()
        fake.close()
        hmp(b, "hostfwd_remove n0 tcp:127.0.0.1:%d" % kport)
        h.t.rm("/etc/systemd/system/reimu-h264.service")


def t_drop(b, h):
    print("== drop", flush=True)
    b.ssh("systemctl stop reimu-hostlink")
    # bug 69: a manual policy holds at least GPU_FAN_LINK_LOST: the card's the only airflow over the IOHUB
    time.sleep(25)
    p = h.pwm()
    check(p[0] == "1" and int(p[1]) >= 102, "BMC daemon stopped, link lost after 25 s: the agent holds the GPU fan: %s" % (p,))
    check(b.out("test -e /run/reimu-hostlink/gpu-temp && echo yes").strip() == "", "gpu-temp gone with it")
    b.ssh("systemctl start reimu-hostlink")
    check(wait(lambda: state()["agent"]["connected"], 90), "started: the agent reconnects")
    check(wait(lambda: h.pwm() == ("1", "115"), 20), "and sets 45 % again (the policy kept on the rwfs)")
    pid = b.out("systemctl show -p MainPID --value reimu-hostlink").strip()
    b.ssh("kill -9 %s" % pid)
    check(wait(lambda: b.out("systemctl is-active reimu-hostlink").strip() == "active" and
               b.out("systemctl show -p MainPID --value reimu-hostlink").strip() not in ("0", pid), 20), "SIGKILL: systemd restarts it")
    check(wait(lambda: state()["agent"]["connected"], 90), "the agent back after the restart")
    h.stop(signal.SIGKILL)  # the host crashed
    t0 = time.time()
    check(wait(lambda: state()["heartbeat"]["state"] == "lost", 150, 3), "the agent killed: heartbeat lost")
    print("    after %.0f s (LOST_SEC=60, +TCP keepalive / ping)" % (time.time() - t0))
    j = b.out("journalctl -u reimu-hostlink -b --no-pager -o cat")
    check("agent_lost: host agent lost" in j, "journal: agent_lost")
    check(state()["sensors"]["Host_GPU_Edge"] is None and prop(
        b, "/xyz/openbmc_project/sensors/temperature/Host_GPU_Edge", "xyz.openbmc_project.State.Decorator.Availability",
        "Available") == "b false", "its sensors: no value, Available false")
    h.start()
    check(wait(lambda: any(e["kind"] == "agent_back" for e in state()["events"]), 60), "agent_back")
    h.stop()
    check(h.pwm()[0] == "2", "the agent stopped (SIGTERM): the GPU fan back to the driver")


def t_web(b):
    print("== web", flush=True)
    import urllib.request
    ctx = ssl.create_default_context()
    ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE
    def get(p):
        rq = urllib.request.Request("https://127.0.0.1:%d%s" % (ab.HTTPS, p), headers={"Accept-Encoding": "gzip"})
        with urllib.request.urlopen(rq, context=ctx, timeout=30) as r:
            d = r.read()
            if r.headers.get("Content-Encoding") == "gzip":
                import gzip
                d = gzip.decompress(d)
            return r.status, d.decode()
    s, t = get("/js/pages/host.js")
    check(s == 200 and "reimu/hostlink" in t, "js/pages/host.js served")
    s, t = get("/")
    check(s == 200 and 'data-page="host"' in t, "the «Хост» link in the menu")


def t_memory(b):
    print("== memory", flush=True)
    pid = b.out("systemctl show -p MainPID --value reimu-hostlink").strip()
    # its own memory (what a leak grows), not the pages of the shared libraries
    # it maps: with OpenSSL 3 (a larger libcrypto) those went from ~4 to ~6 MB
    rss = b.out("grep VmRSS /proc/%s/status" % pid).split()
    anon = b.out("grep RssAnon /proc/%s/status" % pid).split()
    print("  INFO %s" % " ".join(b.out("grep -E '^(RssAnon|RssFile|RssShmem)' /proc/%s/status" % pid).split()), flush=True)
    fds = b.out("ls /proc/%s/fd | wc -l" % pid).strip()
    check(anon and int(anon[1]) < 3000, "its own memory (RssAnon) %s kB, RSS %s kB" % (anon[1] if anon else "?", rss[1] if rss else "?"))
    check(fds.isdigit() and int(fds) < 20, "%s fds" % fds)
    sz = b.out("ls -l /usr/sbin/reimu-hostlink").split()
    print("    /usr/sbin/reimu-hostlink %s bytes" % (sz[4] if len(sz) > 4 else "?"))


if __name__ == "__main__":
    t = time.time()
    try:
        main()
    except Exception as e:
        import traceback
        traceback.print_exc()
        check(False, "crashed: %r" % e)
    print("   (%d s)" % (time.time() - t))
    print("\n%d failed" % len(failed) + "".join("\n  " + f for f in failed))
    sys.exit(1 if failed else 0)
