#!/usr/bin/env python3
"""reimu-hostlink over the network, natively: the daemon built without D-Bus
(tests/reimu-hostlink-native) on 127.0.0.1/::1, a TLS client speaking the
protocol by hand, and the real agent (meta-mcst/tools/host/reimu-hostd.py)
against it with a fake /sys.

    python3 tests/test_protocol.py ./tests/reimu-hostlink-native [TEST...]

About a minute and a half (the link-loss tests wait for the timeouts).
"""
import base64, hashlib, hmac, importlib.machinery, importlib.util, json, os, shutil, signal, socket, ssl, subprocess
import sys, tempfile, time

DAEMON = os.path.abspath(sys.argv[1])
ONLY = sys.argv[2:]
HERE = os.path.dirname(os.path.abspath(__file__))


def find_tools():
    d = HERE
    for _ in range(8):
        c = os.path.join(d, "tools", "host", "reimu-hostd.py")
        if os.path.exists(c):
            return os.path.dirname(c)
        d = os.path.dirname(d)
    sys.exit("tools/host/reimu-hostd.py not found above " + HERE)


TOOLS = find_tools()
AGENT = os.path.join(TOOLS, "reimu-hostd.py")
sys.path.insert(0, os.path.join(TOOLS, "tests"))
import test_reimu_hostd as th  # noqa: E402  (the fake board)

failed = []


def check(cond, what):
    print(("  PASS " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failed.append(what)
    return cond


def wait(fn, timeout, step=0.2):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


class Daemon:
    def __init__(self, conf=""):
        self.d = tempfile.mkdtemp(prefix="hl-")
        self.port = free_port()
        self.conf = os.path.join(self.d, "conf")
        open(self.conf, "w").write("PORT=%d\nIFACE=\nPING_SEC=2\nLOST_SEC=10\nAUTH_TIMEOUT_SEC=3\n%s" % (self.port, conf))
        self.env = dict(os.environ, REIMU_HOSTLINK_CONF=self.conf, REIMU_HOSTLINK_DIR=os.path.join(self.d, "state"),
                        REIMU_HOSTLINK_RUN=os.path.join(self.d, "run"), ASAN_OPTIONS="detect_leaks=0")
        self.p = None
        self.log = os.path.join(self.d, "daemon.log")

    def start(self):
        self.p = subprocess.Popen([DAEMON], env=self.env, stdout=open(self.log, "a"), stderr=subprocess.STDOUT)
        ok = wait(lambda: os.path.exists(os.path.join(self.d, "run", "ctl")) and self.ctl("state").get("ok"), 10)
        assert ok, "daemon didn't start: " + open(self.log).read()[-2000:]
        return self

    def stop(self, sig=signal.SIGTERM):
        if self.p:
            self.p.send_signal(sig)
            try:
                self.p.wait(10)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
            self.p = None

    def ctl(self, cmd):
        r = subprocess.run([DAEMON, "--ctl", cmd], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        try:
            return json.loads(r.stdout)
        except ValueError:
            return {"ok": False, "err": r.stderr.decode()}

    def state(self):
        return self.ctl("state").get("data") or {}

    def logs(self):
        return open(self.log).read()

    def cleanup(self):
        self.stop()
        shutil.rmtree(self.d, ignore_errors=True)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Client:
    """the protocol by hand, over TLS"""

    def __init__(self, port, host="127.0.0.1"):
        fam = socket.AF_INET6 if ":" in host else socket.AF_INET
        s = socket.create_connection((host, port), timeout=5) if fam == socket.AF_INET else socket.socket(fam)
        if fam == socket.AF_INET6:
            s.settimeout(5)
            s.connect((host, port))
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE
        self.s = ctx.wrap_socket(s)
        self.fp = hashlib.sha256(self.s.getpeercert(binary_form=True)).hexdigest()
        self.f = self.s.makefile("rwb")
        self.hello = self.read()

    def send(self, obj):
        self.f.write((json.dumps(obj) if isinstance(obj, dict) else obj).encode() + b"\n")
        self.f.flush()

    def read(self, t=5):
        self.s.settimeout(t)
        try:
            l = self.f.readline()
        except (socket.timeout, OSError):
            return None
        return json.loads(l) if l else "EOF"

    def read_type(self, typ, t=5):
        end = time.time() + t
        while time.time() < end:
            m = self.read(max(0.1, end - time.time()))
            if m in (None, "EOF") or m.get("t") == typ:
                return m
        return None

    def closed(self, t=5):
        end = time.time() + t
        while time.time() < end:
            m = self.read(max(0.1, end - time.time()))
            if m == "EOF":
                return True
        return False

    def auth(self, key, aid="host-1", cnonce="ffeeddccbbaa99887766554433221100"):
        mac = hmac.new(key, ("reimu-hostlink-auth-v1\n%s\n%s\n%s" % (self.hello["nonce"], cnonce, aid)).encode(),
                       hashlib.sha256).hexdigest()
        line = {"v": 1, "t": "auth", "agent": "test", "ver": "1.0", "id": aid, "cnonce": cnonce, "mac": mac, "host": "elbrus"}
        self.send(line)
        return line

    def close(self):
        try:
            self.f.close()  # the makefile holds the socket open otherwise
            self.s.close()
        except OSError:
            pass


def pair_string(d):
    r = d.ctl("pair")
    assert r["ok"], r
    s = r["data"]["pair"]
    b = s[7:]
    return s, json.loads(base64.urlsafe_b64decode(b + "=" * (-len(b) % 4)))


def paired_key(d):
    s, p = pair_string(d)
    c = Client(d.port)
    c.send({"v": 1, "t": "pair", "token": p["token"], "id": "host-1", "host": "elbrus"})
    m = c.read_type("paired")
    c.close()
    return bytes.fromhex(m["key"]), p


# ------------------------------------------------------------------ tests
def t_tls(d):
    c = Client(d.port)
    check(c.hello and c.hello.get("t") == "hello" and len(c.hello.get("nonce", "")) == 32, "TLS, then hello with a nonce")
    s, p = pair_string(d)
    check(p["fp"] == c.fp, "the pairing string pins the certificate the server shows")
    check(c.s.version() in ("TLSv1.2", "TLSv1.3"), "TLS version %s" % c.s.version())
    c.close()
    # a client offering only TLS 1.1 is refused
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE
    try:
        ctx.maximum_version = ssl.TLSVersion.TLSv1_1
        ctx.minimum_version = ssl.TLSVersion.TLSv1
        s2 = ctx.wrap_socket(socket.create_connection(("127.0.0.1", d.port), timeout=5))
        s2.close()
        check(False, "TLS 1.1 refused")
    except (ssl.SSLError, OSError, ValueError):
        check(True, "TLS 1.1 refused")
    # plain TCP garbage: closed, the daemon fine
    s3 = socket.create_connection(("127.0.0.1", d.port), timeout=5)
    s3.sendall(b"GET / HTTP/1.0\r\n\r\n")
    s3.settimeout(5)
    try:
        gone = s3.recv(100) is not None
    except OSError:
        gone = True
    s3.close()
    check(gone and d.ctl("state")["ok"], "not TLS: closed, daemon alive")


def t_pair_auth(d):
    key, p = paired_key(d)
    check(len(key) == 32, "paired: a 32-byte key")
    c = Client(d.port)
    c.auth(key)
    w = c.read_type("welcome")
    want = hmac.new(key, ("reimu-hostlink-srv-v1\n%s\n%s" % ("ffeeddccbbaa99887766554433221100", c.hello["nonce"])).encode(),
                    hashlib.sha256).hexdigest()
    check(w and w.get("mac") == want, "welcome proves the key")
    check(wait(lambda: d.state()["agent"]["connected"], 3), "state: connected")
    check(d.state()["agent"]["transport"] == "ipv4", "transport ipv4 (127.0.0.1)")
    c.close()
    # the token was one-time
    c2 = Client(d.port)
    c2.send({"v": 1, "t": "pair", "token": p["token"], "id": "x"})
    m = c2.read_type("error")
    check(m and m.get("code") == "pair" and c2.closed(), "the token works once")
    c2.close()
    check(p["token"] not in d.logs() and key.hex() not in d.logs(), "no token, no key in the log")
    return key


def t_auth_fail(d, key):
    c = Client(d.port)
    c.auth(b"\x01" * 32)
    m = c.read_type("error")
    check(m and m.get("code") == "auth" and c.closed(), "wrong key: error auth, closed")
    c.close()


def t_replay(d, key):
    c = Client(d.port)
    line = c.auth(key)
    check(c.read_type("welcome") is not None, "session 1")
    c.close()
    c2 = Client(d.port)
    c2.send(line)  # session 1's auth, word for word
    m = c2.read_type("error")
    check(m and m.get("code") == "auth" and c2.closed(), "replayed auth refused")
    c2.close()


def t_limits(d, key):
    c = Client(d.port)
    c.f.write(b"x" * 3000)
    c.f.flush()
    check(c.closed(), "3000 bytes before auth without a newline: closed")
    c.close()
    c = Client(d.port)
    c.send("this is not json")
    m = c.read_type("error")
    check(m and m.get("code") == "malformed" and c.closed(), "malformed: error malformed, closed")
    c.close()
    c = Client(d.port)
    c.send({"v": 2, "t": "auth"})
    m = c.read_type("error")
    check(m and m.get("code") == "version" and c.closed(), "version 2: error version, closed")
    c.close()
    c = Client(d.port)
    c.send({"v": 1, "t": "metrics", "data": {}})
    m = c.read_type("error")
    check(m and m.get("code") == "auth" and c.closed(), "metrics before auth: refused")
    c.close()
    c = Client(d.port)
    c.auth(key)
    c.read_type("welcome")
    c.f.write(b'{"v":1,"t":"inventory","data":"' + b"y" * (130 * 1024) + b'"}\n')
    c.f.flush()
    check(c.closed(), "after auth: a 130 KiB line closed")
    c.close()
    c = Client(d.port)
    c.auth(key)
    c.read_type("welcome")
    c.f.write(b'{"v":1,"t":"pong"}\n' * 200)
    c.f.flush()
    m = c.read_type("error")
    check(m and m.get("code") == "rate" and c.closed(), "a flood: error rate, closed")
    c.close()
    # silent connections: closed after AUTH_TIMEOUT; more than 4 pending refused
    raw = [socket.create_connection(("127.0.0.1", d.port), timeout=5) for _ in range(8)]
    time.sleep(0.5)
    c = None
    try:
        c = Client(d.port)
        busy = False
    except (OSError, ssl.SSLError):
        busy = True
    check(busy or c.hello is None, "8 silent connections: the 9th refused (busy)")
    time.sleep(4)
    c2 = Client(d.port)
    check(c2.hello is not None, "after the auth timeout a client gets in again")
    c2.close()
    for s in raw:
        s.close()
    if c:
        c.close()


def t_ipv6(d, key):
    try:
        c = Client(d.port, "::1")
    except OSError as e:
        print("  SKIP no IPv6 here (%s)" % e)
        return
    c.auth(key)
    check(c.read_type("welcome") is not None, "the same listener on IPv6 ::1")
    check(wait(lambda: d.state()["agent"]["transport"] == "ipv6", 3), "transport ipv6")
    c.close()


def t_no_ipv4():
    d = Daemon("ALLOW_IPV4=no\n").start()
    try:
        try:
            c = Client(d.port)
            ok = c.hello is None or c.closed(2)
        except (OSError, ssl.SSLError):
            ok = True
        check(ok, "ALLOW_IPV4=no: an IPv4 peer gets no hello")
    finally:
        d.cleanup()


def t_data_control(d, key):
    c = Client(d.port)
    c.auth(key)
    c.read_type("welcome")
    c.send({"v": 1, "t": "inventory", "data": {"os": {"name": "ALT"}, "cpu": {"model": "E8C2"}}})
    c.send({"v": 1, "t": "metrics", "data": {"gpu": {"temp_edge": 52, "temp_junction": 61.5, "temp_mem": 58, "fan_rpm": 1200,
                                                     "pwm_pct": 40, "power_w": 35},
                                             "disks": [{"name": "sda", "temp": 35}],
                                             "edac": {"loaded": True, "ce_total": 0, "ue_total": 0, "mcs": []}}})
    st = wait(lambda: (lambda s: s if s.get("metrics") else None)(d.state()), 3) or {}
    check((st.get("inventory") or {}).get("cpu", {}).get("model") == "E8C2", "inventory stored")
    check(st.get("sensors", {}).get("Host_GPU_Junction") == 61.5, "sensors from the metrics")
    gt = os.path.join(d.d, "run", "gpu-temp")
    check(wait(lambda: os.path.exists(gt), 2) and open(gt).read().strip() == "61500", "run/gpu-temp for reimu-fand (m°C)")
    r = d.ctl("power reboot")
    m = c.read_type("control")
    check(r["ok"] and m and m.get("op") == "os_power" and m.get("action") == "reboot", "OsPower reaches the agent")
    c.send({"v": 1, "t": "result", "id": m["id"], "op": "os_power", "ok": True, "text": "systemctl reboot"})
    check(wait(lambda: any(x["id"] == m["id"] and x["done"] and x["ok"] for x in d.state()["results"]), 3), "result recorded")
    check(not d.ctl("power 'halt'")["ok"], "a bad action refused at the BMC")
    d.ctl("fan manual 45")
    p = c.read_type("policy")
    check(p and p["gpu_fan"] == {"mode": "manual", "duty": 45}, "policy pushed")
    m = c.read_type("ping", 4)
    check(m is not None, "pings every PING_SEC")
    c.close()
    check(wait(lambda: not d.state()["agent"]["connected"], 3), "disconnected")


# ------------------------------------------------------------------ the real agent
class Host:
    def __init__(self, port):
        self.t = th.Tree()
        th.board(self.t)
        self.root = self.t.root
        self.conf = os.path.join(self.root, "reimu-hostd.conf")
        self.secret = os.path.join(self.root, "secret")
        open(self.conf, "w").write("BMC=127.0.0.1:%d\nINTERVAL=2\nLINK_LOST_SEC=15\nGPU_FAN_FLOOR=30\n" % port)
        self.env = dict(os.environ, REIMU_HOSTD_ROOT=self.root, REIMU_HOSTD_CONF=self.conf, REIMU_HOSTD_SECRET=self.secret)
        self.p = None
        self.log = os.path.join(self.root, "agent.log")
        self.hw = th.GPU_DEV + "/hwmon/hwmon2"

    def run(self, *args, **kw):
        return subprocess.run([sys.executable, AGENT] + list(args), env=self.env, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=kw.get("timeout", 30))

    def start(self):
        self.p = subprocess.Popen([sys.executable, AGENT, "run"], env=self.env, stdout=open(self.log, "a"),
                                  stderr=subprocess.STDOUT)
        return self

    def stop(self):
        if self.p:
            self.p.terminate()
            self.p.wait(15)
            self.p = None

    def pwm(self):
        try:
            return self.t.get(self.hw + "/pwm1_enable"), self.t.get(self.hw + "/pwm1")
        except OSError:
            return None, None

    def cleanup(self):
        self.stop()
        self.t.cleanup()


def t_agent():
    d = Daemon().start()
    h = Host(d.port)
    try:
        s, p = pair_string(d)
        r = h.run("pair", "--pair", s, "--config", h.conf, "--secret", h.secret)
        check(r.returncode == 0, "reimu-hostd.py pair: %s" % r.stdout.decode()[-300:].strip())
        check(os.stat(h.secret).st_mode & 0o777 == 0o600, "the agent's secret file 0600")
        check(p["token"] not in r.stdout.decode(), "the token not printed")
        h.start()
        st = wait(lambda: (lambda x: x if x.get("metrics") and x.get("inventory") else None)(d.state()), 15) or {}
        check(st.get("agent", {}).get("connected"), "the agent connected")
        check((st.get("inventory") or {}).get("cpu", {}).get("model") == "E8C2", "its inventory (E8C2)")
        check(any(x["slot"] == th.GPU and x["driver"] == "amdgpu" for x in (st.get("inventory") or {}).get("pci", [])),
              "PCIe: the GPU with its driver")
        check(st.get("sensors", {}).get("Host_GPU_Edge") == 52.0 and st.get("sensors", {}).get("Host_Disk_Max") == 41.85,
              "GPU and disk sensors")
        check(st.get("metrics", {}).get("edac", {}).get("ce_total") == 4, "EDAC counts")
        check(any(e["kind"] == "ecc_ce" for e in st.get("events", [])), "ECC CE event (4 corrected)")
        # control: manual 45 % reaches the fake card
        d.ctl("fan manual 45")
        check(wait(lambda: h.pwm() == ("1", "115"), 6), "manual 45 %%: pwm1_enable 1, pwm1 115 (%s)" % (h.pwm(),))
        # the driver resets the card's fan (GPU reset): set again
        h.t.put(h.hw + "/pwm1_enable", "2")
        h.t.put(h.hw + "/pwm1", "0")
        check(wait(lambda: h.pwm() == ("1", "115"), 6), "after a 'GPU reset' (pwm1_enable 2) set again")
        check(wait(lambda: any(e["kind"] == "host" and "gpu_fan_reapplied" in e["text"] for e in d.state()["events"]), 6),
              "the BMC got the re-apply event")
        # the floor: 20 % asked, 30 % on the card
        d.ctl("fan manual 20")
        check(wait(lambda: h.pwm() == ("1", "77"), 6), "20 %% asked: the host's floor 30 %% (pwm1 77)")
        d.ctl("turbo 1")
        check(wait(lambda: h.pwm() == ("1", "255"), 6), "turbo: 255")
        d.ctl("turbo 0")
        d.ctl("fan manual 45")
        check(wait(lambda: h.pwm() == ("1", "115"), 6), "turbo off, manual 45 again")
        # fand's Турбо followed
        d.ctl("fandturbo 600")
        check(wait(lambda: h.pwm() == ("1", "255"), 6), "fand's turbo: the GPU fan 255 too")
        d.ctl("fandturbo 0")
        check(wait(lambda: h.pwm() == ("1", "115"), 6), "fand's turbo off: back to manual 45")
        # os_power through the allow-list (the fake runner only writes it down)
        d.ctl("power reboot")
        cl = os.path.join(h.root, "commands.log")
        check(wait(lambda: os.path.exists(cl) and "systemctl reboot" in open(cl).read(), 6), "OsPower reboot -> systemctl reboot")
        check(wait(lambda: any(x["op"] == "os_power" and x["ok"] for x in d.state()["results"]), 6), "its result back")
        d.ctl("edac")
        check(wait(lambda: "modprobe e2k_edac" in open(cl).read(), 6), "LoadEdac -> modprobe e2k_edac")
        # the BMC goes away: the GPU fan back to the driver after LINK_LOST_SEC
        d.stop(signal.SIGKILL)
        t0 = time.time()
        check(wait(lambda: h.pwm()[0] == "2", 25), "BMC gone: the GPU fan back to auto (pwm1_enable 2)")
        print("    after %.0f s (LINK_LOST_SEC=15)" % (time.time() - t0))
        # the BMC back: the agent reconnects by itself and sets the policy again
        d.start()
        check(wait(lambda: d.state()["agent"]["connected"], 40), "BMC back: the agent reconnects")
        check(wait(lambda: h.pwm() == ("1", "115"), 10), "and sets manual 45 again")
        # the agent goes away: the BMC reports it lost after LOST_SEC
        h.stop()
        check(h.pwm()[0] == "2", "agent stopped: the GPU fan back to auto")
        check(wait(lambda: d.state()["heartbeat"]["state"] == "lost", 20), "BMC: heartbeat lost")
        check(any(e["kind"] == "agent_lost" for e in d.state()["events"]), "agent_lost event")
        check(wait(lambda: d.state()["sensors"]["Host_GPU_Edge"] is None, 15), "its sensors without value")
        h.start()
        check(wait(lambda: any(e["kind"] == "agent_back" for e in d.state()["events"]), 40), "agent_back event")
        # revoked: refused, keeps trying slowly
        d.ctl("unpair")
        check(wait(lambda: "refused the key" in open(h.log).read(), 15), "unpaired: the agent is refused")
        check(not d.state()["agent"]["connected"], "and not connected")
        check(h.p.poll() is None, "the agent keeps running (re-pair possible)")
    finally:
        h.cleanup()
        d.cleanup()


TESTS = ["tls", "pair", "auth_fail", "replay", "limits", "ipv6", "no_ipv4", "data", "agent"]

if __name__ == "__main__":
    d = Daemon().start()
    key = None
    try:
        for name in TESTS:
            if ONLY and name not in ONLY and name != "pair":
                continue
            print("== " + name, flush=True)
            t = time.time()
            try:
                if name == "tls":
                    t_tls(d)
                elif name == "pair":
                    key = t_pair_auth(d)
                elif name == "auth_fail":
                    t_auth_fail(d, key)
                elif name == "replay":
                    t_replay(d, key)
                elif name == "limits":
                    t_limits(d, key)
                elif name == "ipv6":
                    t_ipv6(d, key)
                elif name == "no_ipv4":
                    t_no_ipv4()
                elif name == "data":
                    t_data_control(d, key)
                elif name == "agent":
                    t_agent()
            except Exception as e:
                import traceback
                traceback.print_exc()
                check(False, "%s: %r" % (name, e))
            print("   (%d s)" % (time.time() - t), flush=True)
        check(d.p.poll() is None, "the daemon survived all of it")
    finally:
        log = d.logs()
        d.cleanup()
    if failed:
        print(log[-3000:])
    print("\n%d failed" % len(failed) + "".join("\n  " + f for f in failed))
    sys.exit(1 if failed else 0)
