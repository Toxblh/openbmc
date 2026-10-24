#!/usr/bin/env python3
"""reimu-fand, the real binary (built natively, --no-dbus), against fake chips:

    test_daemon.py ./reimu-fand-native

start (100 % first, then quiet), SIGTERM -> 100 %, SIGKILL then --failsafe
(what ExecStopPost runs) -> 100 %, host off -> no writes, SIGHUP with a bad
config -> keeps running on the good parts, --check-config.
"""
import os, shutil, signal, subprocess, sys, tempfile, time

BIN = os.path.abspath(sys.argv[1])
D = tempfile.mkdtemp(prefix="fand-daemon-")
failed = []


def check(cond, what):
    print(("  PASS " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failed.append(what)


def dev(bus, addr, regs):
    os.makedirs("%s/i2c-%d" % (D, bus), exist_ok=True)
    b = bytearray(256)
    for r, v in regs.items():
        b[r] = v
    open("%s/i2c-%d/%02x" % (D, bus, addr), "wb").write(b)


def reg(bus, addr, r):
    with open("%s/i2c-%d/%02x" % (D, bus, addr), "rb") as f:
        f.seek(r)
        return f.read(1)[0]


def setreg(bus, addr, r, v):
    with open("%s/i2c-%d/%02x" % (D, bus, addr), "r+b") as f:
        f.seek(r)
        f.write(bytes([v]))


def board():
    lm = {0xfe: 1, 0xff: 0x49, 0x4a: 0x2b, 0x4d: 0x17, 0x03: 0x06, 0x4c: 0x2e, 0x46: 0x8c, 0x47: 0x0a}
    dev(6, 0x4c, {**lm, 0x01: 39})
    dev(2, 0x4c, {**lm, 0x01: 83})
    for b in (6, 2):  # the chip's 6-bit PWM register and tachometer latch (fand-hw.c)
        open("%s/i2c-%d/4c.lm96163" % (D, b), "w").close()
    for a in (0x2c, 0x2e):
        e = {0xfd: 0x34, 0xfe: 0x5d, 0x20: 0x40}
        for n in range(5):
            e[0x30 + 0x10 * n] = 0xff
            e[0x32 + 0x10 * n] = 0x2b
        dev(2, a, e)
    for a in range(0x20, 0x27):
        raw = int((50 + a - 0x20) * 8)
        dev(5, a, {0: raw >> 8, 1: raw & 0xff})
    for a in (0x44, 0x45, 0x46):
        dev(6, a, {0x8d: 65})
    os.makedirs(D + "/leds/platform:red:fanfail", exist_ok=True)
    open(D + "/leds/platform:red:fanfail/trigger", "w").write("none\n")


def full():
    # 100 % on the LM96163s is 2 * PWM_FREQ (0x17): 46
    return (reg(6, 0x4c, 0x4c) == 46 and reg(2, 0x4c, 0x4c) == 46 and
            all(reg(2, a, 0x30 + 0x10 * n) == 0xff for a in (0x2c, 0x2e) for n in range(5)))


CONF = D + "/fand.conf"
HOST = D + "/host"
ENV = dict(os.environ, REIMU_FAND_FAKE=D, REIMU_FAND_HOST=HOST, REIMU_FAND_CONF=CONF, REIMU_FAND_RUN=D + "/run")
ENV.pop("NOTIFY_SOCKET", None)


def conf(extra=""):
    open(CONF, "w").write("POLL_MS=100\nSTALE_SEC=2\nSETTLE_SEC=0\nSTART_FULL_SEC=1\nSLEW_DOWN=100\n" + extra)


def host(s):
    open(HOST, "w").write(s + "\n")


def start():
    log = open(D + "/daemon.log", "a")
    return subprocess.Popen([BIN, "--no-dbus"], env=ENV, stdout=log, stderr=log)


try:
    board()
    conf()
    host("on")
    p = start()
    time.sleep(0.5)
    check(full(), "starts at 100%")
    time.sleep(2.5)
    check(not full() and reg(6, 0x4c, 0x4c) < 0x2e, "then quiet (cpu raw %d of 46)" % reg(6, 0x4c, 0x4c))
    p.send_signal(signal.SIGTERM)
    rc = p.wait(5)
    check(rc == 0 and full(), "SIGTERM: exits (%s), fans 100%%" % rc)

    p = start()
    time.sleep(3)
    check(not full(), "restarted: quiet again")
    p.kill()
    p.wait()
    check(not full(), "SIGKILL: nothing ran, fans where they were")
    r = subprocess.run([BIN, "--failsafe"], env=ENV, capture_output=True, text=True, timeout=10)
    check(r.returncode == 0 and full(), "--failsafe (ExecStopPost): 100%")

    # the host on for a while (since the first start, > 6 s): a restarted
    # daemon takes over at once
    conf("SETTLE_SEC=4\nHOST_PROBE=no\n")
    p = start()
    time.sleep(3)
    check(not full(), "host-on file in the run dir: no settle on restart")
    host("off")
    time.sleep(1)
    check(full(), "host off: one fail-safe write")
    setreg(6, 0x4c, 0x4c, 7)
    time.sleep(2)
    check(reg(6, 0x4c, 0x4c) == 7, "host off: then no writes")
    host("on")
    time.sleep(2)
    check(reg(6, 0x4c, 0x4c) == 7, "host on again: settling, bus left alone")
    p.send_signal(signal.SIGTERM)
    p.wait(5)

    # HOST_PROBE (default): the state says off, but the devices answer
    conf()
    host("off")
    p = start()
    time.sleep(3)
    check(not full(), "state off, devices answer: controlled anyway")
    check("taken as on" in open(D + "/daemon.log").read(), "the contradiction is logged")
    for b in (2, 5, 6):
        os.rename("%s/i2c-%d" % (D, b), "%s/x-i2c-%d" % (D, b))
    time.sleep(11)
    for b in (2, 5, 6):
        os.rename("%s/x-i2c-%d" % (D, b), "%s/i2c-%d" % (D, b))
    setreg(6, 0x4c, 0x4c, 7)
    time.sleep(1)
    check(reg(6, 0x4c, 0x4c) == 7, "devices gone (next probe): off, idle")
    p.send_signal(signal.SIGTERM)
    p.wait(5)
    host("on")

    conf()
    p = start()
    time.sleep(2)
    # a broken config: the good keys apply, the bad ones keep their values
    open(CONF, "a").write("FLOOR=5\nCPU_PCS_CURVE=40:10 75:100\nCPU_FAN=bad\n")
    p.send_signal(signal.SIGHUP)
    time.sleep(2)
    check(p.poll() is None and not full(), "SIGHUP with a bad config: still running and controlling")
    log = open(D + "/daemon.log").read()
    check("FLOOR=5" in log and "CPU_PCS_CURVE" in log, "config errors logged")
    # a fan chip gone at runtime: alarm, the rest 100 %, the LED
    os.rename("%s/i2c-2/2e" % D, "%s/i2c-2/2e.x" % D)
    time.sleep(3)
    check(reg(6, 0x4c, 0x4c) == 46 and open(D + "/leds/platform:red:fanfail/trigger").read().strip() == "default-on",
          "a fan chip gone: the others 100%, LED on")
    os.rename("%s/i2c-2/2e.x" % D, "%s/i2c-2/2e" % D)
    p.send_signal(signal.SIGTERM)
    p.wait(5)

    # --observe: reads, computes, prints, writes nothing (no fan, no LED)
    conf("SETTLE_SEC=0\n")
    for a in (0x2c, 0x2e):
        for n in range(5):
            setreg(2, a, 0x30 + 0x10 * n, 0x33)
    setreg(6, 0x4c, 0x4c, 0x07)
    setreg(6, 0x4c, 0x4a, 0x0b)
    open(D + "/leds/platform:red:fanfail/trigger", "w").write("none\n")
    snap = {f: open(os.path.join(dp, f), "rb").read() for dp, _, fs in os.walk(D) if "/i2c-" in dp for f in fs}
    env = dict(ENV)
    env.pop("REIMU_FAND_HOST")
    o = subprocess.Popen([BIN, "--observe"], env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    time.sleep(6)
    os.rename("%s/i2c-5/23" % D, "%s/i2c-5/23.x" % D)
    time.sleep(6)
    o.send_signal(signal.SIGTERM)
    out = o.communicate(5)[0]
    os.rename("%s/i2c-5/23.x" % D, "%s/i2c-5/23" % D)
    snap2 = {f: open(os.path.join(dp, f), "rb").read() for dp, _, fs in os.walk(D) if "/i2c-" in dp for f in fs}
    check("would set: cpu 39.8% board 46.0%" in out, "--observe: computes what it would set: %s" % out.splitlines()[:1])
    check("ALARM" in out and "0x23" in out, "--observe: reports a missing sensor")
    check(snap == snap2 and reg(6, 0x4c, 0x4a) == 0x0b, "--observe: no register written")
    check(open(D + "/leds/platform:red:fanfail/trigger").read().strip() == "none", "--observe: LED untouched")

    open(CONF, "a").write("FLOOR=5\nCPU_PCS_CURVE=40:10 75:100\n")
    r = subprocess.run([BIN, "--check-config", CONF], env=ENV, capture_output=True, text=True)
    check(r.returncode == 1 and "FLOOR=5" in r.stdout, "--check-config reports the errors")
    conf()
    r = subprocess.run([BIN, "--check-config", CONF], env=ENV, capture_output=True, text=True)
    check(r.returncode == 0 and "CPU_PCS_CURVE=" in r.stdout, "--check-config: a good file")
    r = subprocess.run([BIN, "--bogus"], env=ENV, capture_output=True, text=True)
    check(r.returncode == 2, "unknown option: usage")
finally:
    shutil.rmtree(D, ignore_errors=True)

print("\n%d failed" % len(failed) + "".join("\n  " + f for f in failed))
sys.exit(1 if failed else 0)
