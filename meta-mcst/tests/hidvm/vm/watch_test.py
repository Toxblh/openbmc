#!/usr/bin/env python3
"""reimu-hid-watch's policy against a fake /sys and a model of the vhub and
the host, under busybox sh:

    watch_test.py SCRIPT

What the board showed (2026-10-24):
  - after a kexec the gadget stays "not attached" with nothing written to
    /dev/hidg (reimu-kvm's "usboff" 0): that counts as "the host lost it" too;
  - a vhub reset must bind obmc_hid's UDC right after the vhub driver comes
    back, before the host's hub driver reads the port status: 2 s later the
    host saw an empty hub, suspended it at once and never saw the keyboard.
    And not too early either: bound within 20 ms of the driver bind the host
    took the hub at full speed (its USB 1.1 companion). So: after the host
    has addressed the hub (high speed decided: AST_VHUB_CONF != 0) and
    before its port scan (hub_activate waits >= 100 ms after power-on).

The model: unbind clears every gadget's UDC and the vhub's ports; bind brings
the ports (/sys/class/udc/*) back after PROBE s, the host addresses the hub
ADDR s later (CONF = 5, USBSTS bit 27 = high speed unless hs=False) and scans
its ports SCAN s after that: obmc_hid bound by then = keyboard enumerated
("configured", usboff 0), otherwise an empty hub, suspended for good.
"""
import os, shutil, stat, subprocess, sys, tempfile, threading, time

SCRIPT = sys.argv[1]
PROBE, ADDR, SCAN = 0.05, 0.15, 0.15
PORT = "1e6a0000.usb-vhub:p1"
failed = []


def check(cond, what):
    print(("  PASS " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failed.append(what)
    return cond


class Fake:
    def __init__(self, off=99, state="configured", host=True, media=False, hs=True, addr=True):
        self.w = tempfile.mkdtemp(prefix="hidwatch-")
        self.hs, self.addr_comes = hs, addr
        self.g = self.w + "/sys/kernel/config/usb_gadget"
        os.makedirs(self.g + "/obmc_hid")
        self.udc_file = self.g + "/obmc_hid/UDC"
        open(self.udc_file, "w").write(PORT + "\n")
        os.makedirs(self.g + "/mass-storage0")
        open(self.g + "/mass-storage0/UDC", "w").write("1e6a0000.usb-vhub:p4\n" if media else "")
        self.ports(True, state)
        os.makedirs(self.w + "/regs")
        self.reg("0x1E6A0004", 5)
        self.reg("0x1E6A0024", 1 << 27)
        self.set_off(off)
        d = self.w + "/sys/bus/platform/drivers/aspeed_vhub"
        os.makedirs(d)
        self.events = []  # (time, what)
        self.enumerated = None
        for f in ("bind", "unbind"):
            os.mkfifo(d + "/" + f)
            threading.Thread(target=self.driver, args=(d + "/" + f, f), daemon=True).start()
        threading.Thread(target=self.watch_udc, daemon=True).start()
        os.makedirs(self.w + "/bin")
        dm = self.w + "/bin/devmem"
        open(dm, "w").write('#!/bin/sh\ncat "%s/regs/$1" 2>/dev/null || echo 0x00000000\n' % self.w)
        os.chmod(dm, 0o755)
        self.host(host)
        env = dict(os.environ, ROOT=self.w, POLL="1", GRACE="2", HOST_ON="test -f %s/host_on" % self.w,
                   USBOFF="cat %s/usboff" % self.w, DEVMEM=dm, PATH="/tmp/bb")
        self.log = open(self.w + "/log", "w")
        self.p = subprocess.Popen(["/usr/bin/busybox", "sh", SCRIPT], env=env, stdout=self.log, stderr=self.log)

    def reg(self, a, v):
        open(self.w + "/regs/" + a, "w").write("0x%08X\n" % v)

    def ports(self, on, state="not attached"):
        for n in range(1, 6):
            p = self.w + "/sys/class/udc/1e6a0000.usb-vhub:p%d" % n
            if on:
                os.makedirs(p, exist_ok=True)
                open(p + "/state", "w").write(state + "\n")
            else:
                shutil.rmtree(p, ignore_errors=True)

    def ev(self, what):
        self.events.append((time.time(), what))

    def driver(self, fifo, what):
        while True:
            with open(fifo) as f:
                data = f.read().strip()
            if not data:
                continue
            self.ev(what)
            if what == "unbind":
                self.ports(False)
                for u in (self.udc_file, self.g + "/mass-storage0/UDC"):
                    open(u, "w").write("")
                self.reg("0x1E6A0004", 0)
                self.reg("0x1E6A0024", 0)
            else:
                threading.Thread(target=self.probe, daemon=True).start()

    def probe(self):
        time.sleep(PROBE)
        self.ports(True)
        self.ev("ports")
        if not self.addr_comes:
            return
        time.sleep(ADDR)
        self.reg("0x1E6A0024", (1 << 27) if self.hs else 0)
        self.reg("0x1E6A0004", 5)
        self.ev("addressed")
        time.sleep(SCAN)
        self.ev("scan")
        bound = open(self.udc_file).read().strip() == PORT
        self.enumerated = bound
        if bound:
            self.ports(True, "configured")
            self.set_off(0)

    def watch_udc(self):
        last = None
        while True:
            try:
                v = open(self.udc_file).read().strip()
            except OSError:
                v = None
            if v != last:
                if v:
                    self.ev("udc " + v)
                last = v
            time.sleep(0.002)

    def at(self, what):
        return [t for t, w in self.events if w == what]

    def set_off(self, n):
        open(self.w + "/usboff", "w").write("%d\n" % n)

    def host(self, on):
        f = self.w + "/host_on"
        if on:
            open(f, "w").close()
        elif os.path.exists(f):
            os.unlink(f)

    def resets(self):
        return self.text().count("resetting the virtual hub")

    def text(self):
        return open(self.w + "/log").read()

    def stop(self):
        self.p.kill()
        self.p.wait()
        shutil.rmtree(self.w, ignore_errors=True)


def order_ok(f):
    """unbind, bind, the ports back, the host addressing the hub, obmc_hid's
    UDC written, all before the host's port scan"""
    u, b, a, s, k = f.at("unbind"), f.at("bind"), f.at("addressed"), f.at("scan"), f.at("udc " + PORT)
    if not (u and b and a and s and k):
        return False, "events %s" % [w for _, w in f.events]
    k = [t for t in k if t > b[0]]
    ok = bool(k) and u[0] < b[0] < a[0] <= k[0] < s[0]
    return ok, "UDC %.0f ms after the bind, %.0f ms after the hub was addressed, %.0f ms before the scan" % (
        (k[0] - b[0]) * 1000 if k else -1, (k[0] - a[0]) * 1000 if k else -1, (s[0] - k[0]) * 1000 if k else -1)


def main():
    if not os.path.isdir("/tmp/bb"):
        os.makedirs("/tmp/bb")
        for a in subprocess.run(["/usr/bin/busybox", "--list"], capture_output=True, text=True).stdout.split():
            os.symlink("/usr/bin/busybox", "/tmp/bb/" + a)

    print("== host off: nothing")
    f = Fake(host=False, state="not attached")
    time.sleep(6)
    check(f.resets() == 0, "no reset with the host off (%d)" % f.resets())
    f.stop()

    print("== after a kexec: 'not attached', nothing written to hidg (usboff 0), host on: one reset")
    f = Fake(off=0, state="not attached")
    time.sleep(5)
    check(f.resets() == 1, "one reset (%d)" % f.resets())
    ok, why = order_ok(f)
    check(ok, "the UDC bound after the host addressed the hub, before its port scan (%s)" % why)
    check(f.enumerated is True, "the host enumerated the keyboard behind the hub")
    time.sleep(4)
    check(f.resets() == 1, "and no more resets (%d)" % f.resets())
    f.stop()

    print("== the host lost the keyboard (usboff >= GRACE, state still 'configured'): one reset")
    f = Fake(off=99, state="configured")
    time.sleep(5)
    check(f.resets() == 1, "one reset (%d)" % f.resets())
    ok, why = order_ok(f)
    check(ok, "the same order (%s)" % why)
    check(f.enumerated is True, "enumerated")
    print("== the host off and on again: one more (if still lost)")
    f.set_off(99)
    f.ports(True, "not attached")
    f.host(False)
    time.sleep(2)
    f.host(True)
    time.sleep(5)
    check(f.resets() == 2, "a second reset after the power cycle (%d)" % f.resets())
    f.stop()

    print("== the host has it (configured, usboff 0): nothing")
    f = Fake(off=0, state="configured")
    time.sleep(5)
    check(f.resets() == 0, "no reset (%d)" % f.resets())
    f.stop()

    print("== not attached, virtual media bound: nothing")
    f = Fake(off=0, state="not attached", media=True)
    time.sleep(5)
    check(f.resets() == 0 and "virtual media is in use" in f.text(), "no reset while the media gadget is bound")
    f.stop()

    print("== the host takes the hub at full speed: said so")
    f = Fake(off=0, state="not attached", hs=False)
    time.sleep(5)
    check(f.resets() == 1 and "full speed" in f.text(), "one reset, 'full speed' in the log")
    f.stop()

    print("== the host never addresses the hub: the UDC bound anyway")
    f = Fake(off=0, state="not attached", addr=False)
    time.sleep(8)
    k = f.at("udc " + PORT)
    b = f.at("bind")
    check(f.resets() == 1 and b and k and k[-1] > b[0], "obmc_hid bound again after the wait (%s)" % [w for _, w in f.events])
    f.stop()
    print("%d failed" % len(failed))
    sys.exit(min(len(failed), 100))


if __name__ == "__main__":
    main()
