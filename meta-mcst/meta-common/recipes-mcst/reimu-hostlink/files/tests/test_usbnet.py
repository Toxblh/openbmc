#!/usr/bin/env python3
"""reimu-hostlink and the USB network with the host: its listening sockets on
each interface of IFACE (a list, "eth0 usb0" on the board), and UsbNet /
UsbNetSet on D-Bus ("usbnet", "usbnet WHAT on|off" on ctl) passed to
USBNET_HELPER (reimu-usbnet, here a fake that writes down what it is asked;
the real one has its own tests). The daemon built natively without D-Bus.

    python3 tests/test_usbnet.py ./tests/reimu-hostlink-native [TEST...]

About half a minute.
"""
import json, os, shutil, signal, socket, subprocess, sys, tempfile, time

DAEMON = os.path.abspath(sys.argv[1])
ONLY = sys.argv[2:]
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_disk import Agent, check, failed, free_port, wait  # noqa: E402

STATUS = '{"enabled":%s,"internet":%s,"ifname":"usb0","port":"1e6a0000.usb-vhub:p5","state":"configured",' \
         '"address":"172.31.83.1/29","mac":"98:a7:b0:06:0f:7e","host_mac":"9a:a7:b0:06:0f:7e","host_ipv4":"172.31.83.2","error":""}'

# the fake reimu-usbnet: what it is asked into the log; the settings in a file;
# "fail" there: a set fails as the real one says why
HELPER = """#!/bin/sh
echo "$*" >> "%(log)s"
st="%(dir)s/st"; [ -e "$st" ] || echo "true false" > "$st"
read -r en inet < "$st"
case "$1 $2 $3" in
  "set internet on")  [ -e "%(fail)s" ] && { echo "iptables: MASQUERADE not loaded" >&2; exit 1; }; inet=true ;;
  "set internet off") inet=false ;;
  "set enabled on")   en=true ;;
  "set enabled off")  en=false ;;
  "status  ") ;;
  *) echo "usage" >&2; exit 2 ;;
esac
echo "$en $inet" > "$st"
printf '%(status)s\\n' "$en" "$inet"
"""


class Daemon:
    def __init__(self, conf="", iface="", helper=True):
        self.d = tempfile.mkdtemp(prefix="hl-usbnet-")
        self.port = free_port()
        self.hlog = os.path.join(self.d, "helper.log")
        self.fail = os.path.join(self.d, "fail")
        path = os.path.join(self.d, "reimu-usbnet")
        if helper:
            open(path, "w").write(HELPER % {"log": self.hlog, "dir": self.d, "fail": self.fail,
                                            "status": STATUS})
            os.chmod(path, 0o755)
        self.conf = os.path.join(self.d, "conf")
        open(self.conf, "w").write("PORT=%d\nIFACE=%s\nPING_SEC=2\nLOST_SEC=10\nUSBNET_HELPER=%s\nDISK_HELPER=/bin/true\n%s"
                                   % (self.port, iface, path, conf))
        self.env = dict(os.environ, REIMU_HOSTLINK_CONF=self.conf, REIMU_HOSTLINK_DIR=os.path.join(self.d, "state"),
                        REIMU_HOSTLINK_RUN=os.path.join(self.d, "run"), ASAN_OPTIONS="detect_leaks=0")
        self.p = None
        self.log = os.path.join(self.d, "daemon.log")

    def start(self):
        self.p = subprocess.Popen([DAEMON], env=self.env, stdout=open(self.log, "a"), stderr=subprocess.STDOUT)
        ok = wait(lambda: os.path.exists(os.path.join(self.d, "run", "ctl")) and self.ctl("state").get("ok"), 10)
        assert ok, "daemon didn't start: " + open(self.log).read()[-2000:]
        return self

    def ctl(self, cmd, timeout=20):
        r = subprocess.run([DAEMON, "--ctl", cmd], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        try:
            return json.loads(r.stdout)
        except ValueError:
            return {"ok": False, "err": r.stderr.decode()}

    def state(self):
        return self.ctl("state").get("data") or {}

    def helper(self):
        try:
            return open(self.hlog).read().splitlines()
        except OSError:
            return []

    def logs(self):
        return open(self.log).read()

    def cleanup(self):
        if self.p:
            self.p.send_signal(signal.SIGTERM)
            self.p.wait(10)
        shutil.rmtree(self.d, ignore_errors=True)


def t_status():
    d = Daemon().start()
    try:
        r = d.ctl("usbnet")
        data = r.get("data") or {}
        check(r.get("ok") and data.get("port") == "1e6a0000.usb-vhub:p5" and data.get("internet") is False,
              "usbnet: the helper's status as it is (%s)" % r)
        check(d.helper() == ["status"], "the helper asked for its status (%s)" % d.helper())
        check(not [e for e in d.state().get("events", []) if e["kind"] == "usbnet"], "a status is not an event")
    finally:
        d.cleanup()


def t_set():
    d = Daemon().start()
    try:
        r = d.ctl("usbnet internet on")
        check(r.get("ok") and (r.get("data") or {}).get("internet") is True, "internet on: the new status back (%s)" % r)
        check(d.helper() == ["set internet on"], "the helper asked to set it (%s)" % d.helper())
        ev = [e for e in d.state().get("events", []) if e["kind"] == "usbnet"]
        check(ev and "internet for the host on" in ev[-1]["text"], "an event says so (%s)" % ev)
        r = d.ctl("usbnet internet off")
        check(r.get("ok") and r["data"]["internet"] is False, "internet off (%s)" % r)
        r = d.ctl("usbnet enabled off")
        check(r.get("ok") and r["data"]["enabled"] is False and d.helper()[-1] == "set enabled off", "the USB network off (%s)" % d.helper())
        ev = [e["text"] for e in d.state().get("events", []) if e["kind"] == "usbnet"]
        check(len(ev) == 3 and "USB network" in ev[-1] and "off" in ev[-1], "an event for each (%s)" % ev)
        for bad in ("usbnet reboot on", "usbnet internet maybe", "usbnet internet", "usbnet internet on;reboot",
                    "usbnet ../x on", "usbnet internet on extra"):
            r = d.ctl(bad)
            check(not r.get("ok") and r.get("err"), "refused: %r (%s)" % (bad, r.get("err")))
        check(len(d.helper()) == 3, "the refused ones never reached the helper (%s)" % d.helper())
        open(d.fail, "w").close()
        r = d.ctl("usbnet internet on")
        check(not r.get("ok") and "MASQUERADE" in (r.get("err") or ""), "the helper failed: its message (%s)" % r)
        ev = [e for e in d.state().get("events", []) if e["kind"] == "usbnet"]
        check(len(ev) == 3, "a failed set is no change event")
    finally:
        d.cleanup()


def t_no_helper():
    d = Daemon(helper=False).start()
    try:
        r = d.ctl("usbnet")
        check(r.get("ok") and (r.get("data") or {}).get("error"), "no helper: a status with the error, not a failure (%s)" % r)
        r = d.ctl("usbnet internet on")
        check(not r.get("ok") and r.get("err"), "no helper: a set fails (%s)" % r.get("err"))
        check(d.ctl("state").get("ok"), "the daemon alive")
    finally:
        d.cleanup()


def ifaces():
    return [n for n in os.listdir("/sys/class/net") if n != "lo"]


def t_ifaces():
    # an interface that isn't there (usb0 before reimu-usbnet made it): the others listen
    d = Daemon(iface="lo nosuch0").start()
    try:
        st = d.state()
        check(st.get("iface") == "lo nosuch0", "iface: the list (%s)" % st.get("iface"))
        s = socket.create_connection(("127.0.0.1", d.port), timeout=5)
        s.close()
        check(True, "listening on lo with nosuch0 missing")
        logs = d.logs()
        check("nosuch0" in logs and "on lo" in logs, "the log says which: %s" % [l for l in logs.splitlines() if "listen" in l])
        # the real agent, paired: its connection's interface and the way it came
        r = d.ctl("pair")
        a = Agent(d.port)
        try:
            p = a.pair(r["data"]["pair"])
            check(p.returncode == 0, "the agent paired over lo")
            a.start()
            ok = wait(lambda: d.state().get("agent", {}).get("connected"), 30)
            ag = d.state().get("agent", {})
            check(ok and ag.get("iface") == "lo" and ag.get("via") == "lan", "connected on lo: iface lo, via lan (%s)" % ag)
        finally:
            a.cleanup()
    finally:
        d.cleanup()
    other = ifaces()
    if not other:
        print("  INFO no interface but lo here: two interfaces on one port not tried", flush=True)
        return
    # two interfaces on the same port (a socket each, SO_BINDTODEVICE)
    d = Daemon(iface="lo %s" % other[0]).start()
    try:
        ok = wait(lambda: "on %s" % other[0] in d.logs() and "on lo" in d.logs(), 5)
        check(ok, "lo and %s both listening on port %d: %s" % (other[0], d.port, [l for l in d.logs().splitlines() if "listen" in l]))
        s = socket.create_connection(("127.0.0.1", d.port), timeout=5)
        s.close()
        check(True, "connected")
    finally:
        d.cleanup()


TESTS = {"status": t_status, "set": t_set, "no_helper": t_no_helper, "ifaces": t_ifaces}

if __name__ == "__main__":
    for name, fn in TESTS.items():
        if ONLY and name not in ONLY:
            continue
        print("## " + name, flush=True)
        try:
            fn()
        except Exception as e:
            import traceback
            traceback.print_exc()
            failed.append("%s: %s" % (name, e))
    print("%d failed" % len(failed) if failed else "all passed")
    sys.exit(1 if failed else 0)
