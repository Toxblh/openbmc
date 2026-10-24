#!/usr/bin/env python3
"""reimu-hostlink's installer USB disk (AgentDisk / AgentDiskOff on D-Bus,
"disk" / "diskoff" on ctl): the daemon built natively without D-Bus, its
DISK_HELPER a fake that writes down what it is asked (the real one,
reimu-agent-disk, has its own tests), the real agent pairing and connecting.

    python3 tests/test_disk.py ./tests/reimu-hostlink-native [TEST...]

About two minutes (the disk's 60 s time-out is waited for).
"""
import json, os, shutil, signal, subprocess, sys, tempfile, time

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


def free_port():
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


HELPER = """#!/bin/sh
# the fake reimu-agent-disk: ACTION and the line on stdin
read -r line || line=
echo "$1 $line" >> "%(log)s"
if [ "$1" = start ] && [ -e "%(fail)s" ]; then echo "reimu-agent-disk: no free USB port (p2, p3 in use)" >&2; exit 1; fi
exit 0
"""


class Daemon:
    def __init__(self, conf=""):
        self.d = tempfile.mkdtemp(prefix="hl-disk-")
        self.port = free_port()
        self.hlog = os.path.join(self.d, "helper.log")
        self.fail = os.path.join(self.d, "fail")
        helper = os.path.join(self.d, "reimu-agent-disk")
        open(helper, "w").write(HELPER % {"log": self.hlog, "fail": self.fail})
        os.chmod(helper, 0o755)
        self.conf = os.path.join(self.d, "conf")
        open(self.conf, "w").write("PORT=%d\nIFACE=\nPING_SEC=2\nLOST_SEC=10\nDISK_HELPER=%s\n%s" % (self.port, helper, conf))
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
            self.p.wait(10)
            self.p = None

    def ctl(self, cmd):
        r = subprocess.run([DAEMON, "--ctl", cmd], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
        try:
            return json.loads(r.stdout)
        except ValueError:
            return {"ok": False, "err": r.stderr.decode()}

    def state(self):
        return self.ctl("state").get("data") or {}

    def disk(self):
        return self.state().get("disk") or {}

    def helper(self):
        try:
            return open(self.hlog).read().splitlines()
        except OSError:
            return []

    def cleanup(self):
        self.stop()
        shutil.rmtree(self.d, ignore_errors=True)


class Agent:
    def __init__(self, port):
        self.root = tempfile.mkdtemp(prefix="hostd-")
        self.conf = os.path.join(self.root, "reimu-hostd.conf")
        self.secret = os.path.join(self.root, "secret")
        open(self.conf, "w").write("BMC=127.0.0.1:%d\nINTERVAL=2\nDISCOVERY=no\n" % port)
        self.env = dict(os.environ, REIMU_HOSTD_ROOT=self.root, REIMU_HOSTD_CONF=self.conf, REIMU_HOSTD_SECRET=self.secret)
        self.p = None

    def pair(self, s):
        return subprocess.run([sys.executable, AGENT, "pair", "--pair", s], env=self.env, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=60)

    def start(self):
        self.p = subprocess.Popen([sys.executable, AGENT, "run"], env=self.env, stdout=open(os.path.join(self.root, "log"), "a"),
                                  stderr=subprocess.STDOUT)

    def cleanup(self):
        if self.p:
            self.p.terminate()
            self.p.wait(20)
        shutil.rmtree(self.root, ignore_errors=True)


def t_installed():
    d = Daemon().start()
    a = Agent(d.port)
    try:
        check(d.helper() == ["stop "], "at start: a disk left by a crash taken away (%s)" % d.helper())
        check(d.disk() == {"on": False, "left": 0}, "Json: no disk")
        r = d.ctl("disk")
        data = r.get("data") or {}
        check(r.get("ok") and data.get("disk") is True and data.get("pair", "").startswith("reimu1:"), "disk: a new pairing (%s)" % r)
        check("https_pin" in data, "with the HTTPS pin for the page")
        check(d.helper()[-1:] == ["start " + data.get("pair", "")], "the helper started with the pairing string on stdin")
        check(d.disk().get("on") and 895 <= d.disk().get("left", 0) <= 900, "Json: the disk on, 15 min (%s)" % d.disk())
        tok = json.loads(__import__("base64").urlsafe_b64decode(data["pair"][7:] + "==="))["token"]
        check(tok not in open(d.log).read(), "the token not in the daemon's log")
        r = a.pair(data["pair"])
        check(r.returncode == 0, "the agent paired with the disk's string: %s" % r.stdout.decode()[-200:])
        time.sleep(2)
        check(d.disk().get("on") and d.helper()[-1].startswith("start"), "paired: the disk stays (the installer runs from it)")
        a.start()
        check(wait(lambda: d.helper()[-1:] == ["stop "], 20), "the agent's service connected: the disk taken away")
        check(not d.disk().get("on"), "Json: off")
        check(any(e["kind"] == "agent_disk" for e in d.state().get("events", [])), "events: the disk on and off")
        # again with an agent connected: a new pairing, the disk again
        r = d.ctl("disk")
        check(r.get("ok") and d.helper()[-1].startswith("start reimu1:"), "disk again")
        r2 = d.ctl("disk")
        check(r2.get("ok") and d.helper()[-2:] == ["stop ", "start " + r2["data"]["pair"]],
              "disk while on: taken away, made again with a new string (%s)" % d.helper()[-3:])
        r = d.ctl("diskoff")
        check(r.get("ok") and d.helper()[-1] == "stop " and not d.disk().get("on"), "diskoff")
        d.ctl("disk")
        d.stop()
        check(d.helper()[-1] == "stop ", "the daemon stopped: the disk taken away")
    finally:
        a.cleanup()
        d.cleanup()


def t_helper_fails():
    d = Daemon().start()
    try:
        open(d.fail, "w").close()
        r = d.ctl("disk")
        check(not r.get("ok") and "no free USB port" in r.get("err", ""), "the helper's error back to the caller: %s" % r)
        check(not d.disk().get("on") and not d.state()["pairing"]["active"], "no disk, no pairing left open")
    finally:
        d.cleanup()


def t_timeout():
    d = Daemon("PAIR_TTL_SEC=60\n").start()
    try:
        r = d.ctl("disk")
        check(r.get("ok") and d.disk().get("left", 0) <= 60, "disk for 60 s (PAIR_TTL_SEC)")
        t0 = time.time()
        check(wait(lambda: d.helper()[-1:] == ["stop "], 75), "nobody installed: taken away when the token expired")
        check(55 <= time.time() - t0 <= 70, "after %.0f s" % (time.time() - t0))
    finally:
        d.cleanup()


TESTS = {"installed": t_installed, "helper_fails": t_helper_fails, "timeout": t_timeout}

if __name__ == "__main__":
    for name, fn in TESTS.items():
        if ONLY and name not in ONLY:
            continue
        print("## " + name, flush=True)
        try:
            fn()
        except Exception as e:  # a crash is a failure, the others still run
            import traceback
            traceback.print_exc()
            failed.append("%s: %s" % (name, e))
    print("%d failed" % len(failed) if failed else "all passed")
    sys.exit(1 if failed else 0)
