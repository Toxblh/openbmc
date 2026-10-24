#!/usr/bin/env python3
"""KVM HID scenarios, run inside the HID test VM by test_kvm_hid_vm.py:

    kvmscen.py SRC_DIR SCENARIO...

SRC_DIR holds create_usbhid.sh and the reimu-kvm binary (reimu-kvm). Each
scenario prints PASS/FAIL lines; the exit status is the number of FAILs.
"""
import array, ctypes, fcntl, os, struct, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kvmvm as K  # noqa: E402

SRC = sys.argv[1]
failed = []
KEY_A, KEY_B, BTN_LEFT, KEY_CAPSLOCK, KEY_NUMLOCK = 30, 48, 0x110, 58, 69
HID_A = 0x04


def check(cond, what):
    print(("  PASS " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failed.append(what)
    return cond


def fresh_gadget():
    K.sh("echo 0 > /sys/bus/usb/devices/1-0:1.0/usb1-port1/disable", check=False)
    K.gadget_down()
    rc, out = K.gadget_up(os.path.join(SRC, "create_usbhid.sh"))
    ok = K.wait_configured()
    if not check(rc == 0 and ok, "gadget up and enumerated by the host (rc %d, %s)%s" % (rc, K.udc_state(), out.strip() and ": " + out.strip())):
        raise SystemExit(1)
    return K.input_devices()


def dmesg_mark():
    return K.sh("dmesg | wc -l").stdout.strip()


def dmesg_since(mark):
    return K.sh("dmesg | tail -n +%d" % (int(mark) + 1)).stdout


# ------------------------------------------------------------------ 1. no lost reports
def s_burst():
    print("== burst: 200 key presses and a click in one write")
    devs = fresh_gadget()
    K.kvm_start(os.path.join(SRC, "reimu-kvm"))
    kb, pt = K.Events(devs["kbd"]), K.Events(devs["ptr"])
    w = K.Ws()
    msgs = []
    for _ in range(200):
        msgs += [K.key(HID_A, 1), K.key(HID_A, 0)]
    msgs += [K.ptr(1, 1000, 1000), K.ptr(0, 1000, 1000)]
    w.send(*msgs)
    kb.read(quiet=1.5, timeout=60)
    pt.read(quiet=1.0, timeout=10)
    a = kb.keys(KEY_A)
    down, up, rep = (sum(1 for c, v in a if v == x) for x in (1, 0, 2))
    check(down == 200 and up == 200 and rep == 0,
          "the host got 200 presses and 200 releases of A, no repeats (%d / %d / %d)" % (down, up, rep))
    seq_ok = all(v == (1 if i % 2 == 0 else 0) for i, (c, v) in enumerate(a))
    check(seq_ok, "A alternates down/up")
    btn = pt.keys(BTN_LEFT)
    check(btn == [(BTN_LEFT, 1), (BTN_LEFT, 0)], "the click: left button down then up (%s)" % btn)
    w.close()
    kb.close(), pt.close()
    K.kvm_stop()


# ------------------------------------------------------------------ 3. release-all hygiene
def held(kb, code=KEY_A):
    """is the key down on the host, by its last event"""
    st = None
    for c, v in kb.keys(code):
        st = v
    return st in (1, 2)


def s_release():
    print("== release: keys let go when a viewer connects or leaves, on SIGTERM")
    devs = fresh_gadget()
    K.kvm_start(os.path.join(SRC, "reimu-kvm"))
    kb = K.Events(devs["kbd"])
    a = K.Ws()
    a.send(K.key(HID_A, 1))
    kb.read(quiet=0.3)
    check(held(kb), "A held down by viewer 1")
    b = K.Ws()
    kb.read(quiet=0.5)
    check(not held(kb), "a second viewer connects: A released on the host")
    a.send(K.key(HID_A, 1))
    kb.read(quiet=0.3)
    a.close(clean=False)
    kb.read(quiet=0.8)
    check(not held(kb), "viewer 1 drops (viewer 2 still there): A released")
    b.send(K.key(HID_A, 1))
    kb.read(quiet=0.3)
    check(held(kb), "A held down by viewer 2")
    K.kvm_stop("TERM")
    kb.read(quiet=0.8)
    check(not held(kb), "SIGTERM: A released before reimu-kvm exits")
    b.close(clean=False)
    kb.close()


# ------------------------------------------------------------------ 8. UDC rebind
def s_rebind():
    print("== rebind: the gadget unbound and bound again while reimu-kvm runs")
    devs = fresh_gadget()
    K.kvm_start(os.path.join(SRC, "reimu-kvm"))
    w = K.Ws()
    kb = K.Events(devs["kbd"])
    w.send(K.key(HID_A, 1), K.key(HID_A, 0))
    kb.read(quiet=0.5)
    check(len(kb.keys(KEY_A)) == 2, "a key before the rebind")
    kb.close()
    K.sh("echo '' > %s/UDC" % K.G)
    time.sleep(0.5)
    # a key while unbound (written into nothing), then bind again
    w.send(K.key(HID_A, 1), K.key(HID_A, 0))
    time.sleep(0.5)
    K.sh("echo %s > %s/UDC" % (K.UDC, K.G))
    ok = K.wait_configured()
    check(ok, "enumerated again (%s)" % K.udc_state())
    devs = K.input_devices()
    kb = K.Events(devs["kbd"])
    time.sleep(0.3)
    w.send(K.key(0x05, 1), K.key(0x05, 0))
    kb.read(quiet=1.0)
    b = kb.keys(KEY_B)
    check(b == [(KEY_B, 1), (KEY_B, 0)], "the next key after the rebind reaches the host (%s)" % b)
    w.close()
    kb.close()
    K.kvm_stop()


# ------------------------------------------------------------------ 4. LEDs
EVIOCGLED = lambda n: 0x80004519 | (n << 16)  # noqa: E731  _IOC(_IOC_READ, 'E', 0x19, len)


def s_leds():
    print("== leds: Caps Lock / Num Lock toggled 40 times by the host, Caps Lock left on")
    devs = fresh_gadget()
    K.kvm_start(os.path.join(SRC, "reimu-kvm"))
    w = K.Ws()
    mark = dmesg_mark()
    fd = os.open(devs["kbd"], os.O_WRONLY)
    state = 0
    for i in range(81):  # ends with Caps Lock on, Num Lock off
        for led in ((0, 1) if i < 80 else (1,)):  # LED_NUML 0, LED_CAPSL 1
            state ^= 1 << led
            ev = struct.pack("llHHi", 0, 0, 0x11, led, (state >> led) & 1) + struct.pack("llHHi", 0, 0, 0, 0, 0)
            os.write(fd, ev)
            time.sleep(0.05)
    os.close(fd)
    time.sleep(1.5)
    msgs = dmesg_since(mark)
    bad = [l for l in msgs.splitlines() if "usbhid" in l or "hid-generic" in l or "queue" in l or "timeout" in l.lower()]
    # the host's LED reports all delivered: none left in its output queue
    check(not bad, "no usbhid errors in the host's dmesg%s" % ("".join("\n         " + l for l in bad[:5])))
    st = K.sh("busctl call xyz.openbmc_project.Reimu.Kvm /xyz/openbmc_project/reimu/kvm "
              "xyz.openbmc_project.Reimu.Kvm Status", check=False).stdout
    want = state  # num lock bit 0, caps lock bit 1 (HID LED report)
    import json, re
    m = re.search(r'\\"leds\\":(\d+)|"leds":(\d+)', st)
    got = int(next(g for g in m.groups() if g)) if m else None
    check(got == want, "reimu-kvm saw the LED report %s (Status: %s)" % (want, got))
    w.close()
    K.kvm_stop()


# ------------------------------------------------------------------ 5. interface fields, SET_PROTOCOL
def s_iface():
    print("== iface: the mouse is not a boot mouse; SET_PROTOCOL")
    fresh_gadget()
    import usb.core
    d = usb.core.find(idVendor=0x1d6b, idProduct=0x0104)
    cfg = d.get_active_configuration()
    f = {}
    for intf in cfg:
        f[intf.bInterfaceNumber] = (intf.bInterfaceClass, intf.bInterfaceSubClass, intf.bInterfaceProtocol)
    check(f.get(0) == (3, 1, 1), "interface 0: HID, boot subclass, keyboard (%s)" % (f.get(0),))
    check(f.get(1) == (3, 0, 0), "interface 1 (absolute mouse): HID, no boot subclass, protocol 0 (%s)" % (f.get(1),))

    def set_protocol(intf, proto):
        try:
            if d.is_kernel_driver_active(intf):
                d.detach_kernel_driver(intf)
            d.ctrl_transfer(0x21, 0x0b, proto, intf, None, timeout=1000)  # SET_PROTOCOL
            return "ok"
        except usb.core.USBError as e:
            return "stall" if e.errno == 32 else str(e)
        finally:
            try:
                d.attach_kernel_driver(intf)
            except usb.core.USBError:
                pass
    check(set_protocol(0, 0) == "ok", "keyboard: SET_PROTOCOL(boot) accepted (the PNS boot menu's keyboard)")
    r = set_protocol(1, 0)
    check(r == "stall", "absolute mouse: SET_PROTOCOL(boot) refused (STALL), so no firmware reads x/y as dx/dy (%s)" % r)


# ------------------------------------------------------------------ 6. GET_REPORT
def s_getreport():
    print("== getreport: a GET_REPORT(input) of each interface answered at once")
    devs = fresh_gadget()
    K.kvm_start(os.path.join(SRC, "reimu-kvm"))
    w = K.Ws()
    w.send(K.key(HID_A, 1))  # a report the GET_REPORT should return
    time.sleep(0.5)
    import glob
    for hr in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        if "1D6B:0104" not in os.path.realpath(hr):
            continue
        dev = "/dev/" + os.path.basename(hr)
        ifn = "1.0" if ":1.0/" in os.path.realpath(hr) else "1.1"
        n = 8 if ifn == "1.0" else 7
        buf = array.array("B", [0] * (n + 1))
        fd = os.open(dev, os.O_RDWR)
        t = time.time()
        try:
            # HIDIOCGINPUT(len) = _IOC(READ|WRITE, 'H', 0x0A, len)
            fcntl.ioctl(fd, 0xC000480A | ((n + 1) << 16), buf, True)
            res = bytes(buf)
        except OSError as e:
            res = "error %s" % e
        dt = (time.time() - t) * 1000
        os.close(fd)
        check(dt < 50, "%s (interface %s): GET_REPORT answered in %.0f ms (%s)" % (dev, ifn, dt, res if isinstance(res, str) else res.hex(" ")))
        if ifn == "1.0" and not isinstance(res, str):
            check(res[1 + 2] == HID_A, "the keyboard's GET_REPORT is the current report (A down)")
    w.send(K.key(HID_A, 0))
    w.close()
    K.kvm_stop()


# ------------------------------------------------------------------ keyboard = stock
def s_kbdpin():
    """The host's boot firmware (PNS) takes the stock firmware's KVM keyboard
    in its boot menu: everything it sees of the keyboard stays as in the stock
    gadget (5.10 f_hid defaults: IN and OUT interrupt endpoints, bInterval 4
    at high speed). Red if any of it changes."""
    print("== kbdpin: the keyboard as the stock firmware's (the PNS boot menu depends on it)")
    fresh_gadget()
    import usb.core, usb.util, hid_desc
    d = usb.core.find(idVendor=0x1d6b, idProduct=0x0104)
    check(d is not None, "device 1d6b:0104")
    check((d.bcdUSB, d.bcdDevice, d.bDeviceClass) == (0x0200, 0x0100, 0),
          "bcdUSB 0x0200, bcdDevice 0x0100, class per interface (%04x %04x %d)" % (d.bcdUSB, d.bcdDevice, d.bDeviceClass))
    strs = tuple(usb.util.get_string(d, i) for i in (d.iManufacturer, d.iProduct, d.iSerialNumber))
    check(strs == ("OpenBMC", "virtual_input", "OBMC0001"), "strings %s" % (strs,))
    check(d.bNumConfigurations == 1, "one configuration")
    c = d.get_active_configuration()
    check((c.bmAttributes, c.bMaxPower, c.bNumInterfaces) == (0x80, 100, 2),
          "config: bmAttributes 0x80, 200 mA, 2 interfaces (%#x %d %d)" % (c.bmAttributes, c.bMaxPower, c.bNumInterfaces))
    i0 = c[(0, 0)]
    check((i0.bInterfaceClass, i0.bInterfaceSubClass, i0.bInterfaceProtocol, i0.bNumEndpoints) == (3, 1, 1, 2),
          "interface 0: HID, boot subclass 1, keyboard protocol 1, 2 endpoints (%d %d %d %d)"
          % (i0.bInterfaceClass, i0.bInterfaceSubClass, i0.bInterfaceProtocol, i0.bNumEndpoints))
    eps = sorted((e.bEndpointAddress & 0x80, e.bmAttributes, e.wMaxPacketSize, e.bInterval) for e in i0)
    check(eps == [(0, 3, 8, 4), (0x80, 3, 8, 4)], "interface 0 endpoints: interrupt OUT and IN, 8 bytes, bInterval 4 (%s)" % eps)
    hidd = list(i0.extra_descriptors)
    check(bytes(hidd[:9]) == bytes([9, 0x21, 0x01, 0x01, 0, 1, 0x22, 63, 0]),
          "interface 0 HID descriptor: HID 1.01, one 63-byte report descriptor (%s)" % bytes(hidd[:9]).hex(" "))
    import glob
    rd = [open(p, "rb").read() for p in glob.glob("/sys/bus/hid/devices/0003:1D6B:0104.*/report_descriptor")
          if ":1.0/" in os.path.realpath(p)]
    check(rd and rd[0] == hid_desc.KEYBOARD, "interface 0 report descriptor: the stock 63 bytes")
    i1 = c[(1, 0)]
    check(i1.bInterfaceNumber == 1 and i1.bInterfaceClass == 3, "the mouse stays interface 1, after the keyboard")


# ------------------------------------------------------------------ 7. the watcher, for real
def s_watch():
    print("== watch: the host drops the gadget; reimu-hid-watch resets the UDC once, keys flow again")
    devs = fresh_gadget()
    K.kvm_start(os.path.join(SRC, "reimu-kvm"))
    w = K.Ws()
    # bound again while the host doesn't look (its port disabled: as when it
    # never woke the hub for the new gadget); the port comes back with the
    # reset (on the board the hub's unplug/replug is what the host sees)
    port = "/sys/bus/usb/devices/1-0:1.0/usb1-port1/disable"
    K.sh("echo '' > %s/UDC; echo 1 > %s; sleep 0.3; echo %s > %s/UDC" % (K.G, port, K.UDC, K.G))
    time.sleep(0.5)
    st = K.udc_state()
    # dummy_udc has no AST_VHUB_CONF/USBSTS: "addressed, high speed" at once
    open("/tmp/fake-devmem", "w").write("#!/bin/sh\necho 0x08000005\n")
    os.chmod("/tmp/fake-devmem", 0o755)
    env = dict(os.environ, ROOT="", POLL="1", GRACE="2", VHUB="dummy_udc.0", VHUB_DRIVER="dummy_udc",
               HOST_ON="true", DEVMEM="/tmp/fake-devmem", PATH="/tmp/bb:/usr/bin")
    p = subprocess.Popen(["/usr/bin/busybox", "sh", os.path.join(SRC, "reimu-hid-watch")], env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    os.set_blocking(p.stdout.fileno(), False)

    def more():
        try:
            return os.read(p.stdout.fileno(), 65536).decode()
        except BlockingIOError:
            return ""
    out, ok, enabled = "", False, False
    for _ in range(150):
        time.sleep(0.2)
        out += more()
        if not enabled and "resetting the virtual hub" in out:
            K.sh("echo 0 > %s" % port)
            enabled = True
        if enabled and K.udc_state() == "configured" and len(K.input_devices()) == 2:
            ok = True
            break
    time.sleep(3)
    out += more()
    p.kill()
    p.wait()
    K.sh("echo 0 > %s" % port)
    check(ok, "the host enumerated the gadget again (state was %s)" % st)
    check(out.count("resetting the virtual hub") == 1, "exactly one reset (%s)" % out.strip().replace("\n", " | ")[:200])
    kb = K.Events(K.input_devices()["kbd"])
    w.send(K.key(HID_A, 1), K.key(HID_A, 0))
    kb.read(quiet=0.8)
    check(kb.keys(KEY_A) == [(KEY_A, 1), (KEY_A, 0)], "reimu-kvm's next key reaches the host (%s)" % kb.keys(KEY_A))
    kb.close()
    w.close()
    K.kvm_stop()


def s_watch_kexec():
    """the board after a kexec: the UDC made anew ("not attached"), the
    gadget bound, the host not looking, nothing written to hidg (no viewer:
    usboff 0): the watcher must reset anyway"""
    print("== watch-kexec: 'not attached' and no viewer: reimu-hid-watch resets once, the host gets the keyboard")
    fresh_gadget()
    K.kvm_start(os.path.join(SRC, "reimu-kvm"))
    port = "/sys/bus/usb/devices/1-0:1.0/usb1-port1/disable"
    drv = "/sys/bus/platform/drivers/dummy_udc"
    # as after a kexec: the UDC and the gadget made anew (a fresh f_hid: reads
    # give EAGAIN, not ESHUTDOWN), the host's port off
    K.kvm_stop()
    K.sh("echo 1 > %s" % port)
    K.gadget_down()
    K.sh("echo dummy_udc.0 > %s/unbind; sleep 0.3; echo dummy_udc.0 > %s/bind; sleep 0.3" % (drv, drv))
    rc, out = K.gadget_up(os.path.join(SRC, "create_usbhid.sh"))
    K.kvm_start(os.path.join(SRC, "reimu-kvm"))
    time.sleep(1.5)
    usboff = K.sh("busctl call xyz.openbmc_project.Reimu.Kvm /xyz/openbmc_project/reimu/kvm "
                  "xyz.openbmc_project.Reimu.Kvm Status", check=False).stdout
    check('\\"usboff\\":0' in usboff, "reimu-kvm can't tell (usboff 0, as on the board)")
    st = K.udc_state()
    check(st == "not attached", "the UDC anew: 'not attached' (%s)" % st)
    open("/tmp/fake-devmem", "w").write("#!/bin/sh\necho 0x08000005\n")
    os.chmod("/tmp/fake-devmem", 0o755)
    env = dict(os.environ, ROOT="", POLL="1", GRACE="2", VHUB="dummy_udc.0", VHUB_DRIVER="dummy_udc",
               HOST_ON="true", DEVMEM="/tmp/fake-devmem", PATH="/tmp/bb:/usr/bin")
    p = subprocess.Popen(["/usr/bin/busybox", "sh", os.path.join(SRC, "reimu-hid-watch")], env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    os.set_blocking(p.stdout.fileno(), False)

    def more():
        try:
            return os.read(p.stdout.fileno(), 65536).decode()
        except BlockingIOError:
            return ""
    out, ok, enabled = "", False, False
    for _ in range(150):
        time.sleep(0.2)
        out += more()
        if not enabled and "resetting the virtual hub" in out:
            K.sh("echo 0 > %s" % port)
            enabled = True
        if enabled and K.udc_state() == "configured" and len(K.input_devices()) == 2:
            ok = True
            break
    time.sleep(3)
    out += more()
    p.kill()
    p.wait()
    K.sh("echo 0 > %s" % port)
    check(ok, "the host enumerated keyboard and mouse after the reset")
    check(out.count("resetting the virtual hub") == 1, "exactly one reset (%s)" % out.strip().replace("\n", " | ")[:220])
    K.kvm_stop()


SCEN = {"watch": s_watch, "watch-kexec": s_watch_kexec, "kbdpin": s_kbdpin, "burst": s_burst, "release": s_release, "rebind": s_rebind, "leds": s_leds, "iface": s_iface,
        "getreport": s_getreport}

if __name__ == "__main__":
    for name in sys.argv[2:]:
        try:
            SCEN[name]()
        except SystemExit:
            raise
        except Exception as e:  # noqa: BLE001
            check(False, "%s crashed: %r" % (name, e))
        finally:
            K.kvm_stop()
    print("%d failed" % len(failed))
    sys.exit(min(len(failed), 100))
