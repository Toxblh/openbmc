#!/usr/bin/env python3
"""obmc-ikvm's HID writer (ikvm_input.cpp) in the HID test VM:

    ikvmscen.py IKVM_SRC GADGET_SRC [base]

IKVM_SRC: the obmc-ikvm source with the REIMU patches; GADGET_SRC: where
create_usbhid.sh is. "base": the Input of before (no setClients/releaseAll).
"""
import os, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kvmvm as K  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SRC, GSRC = sys.argv[1], sys.argv[2]
BASE = len(sys.argv) > 3 and sys.argv[3] == "base"
KEY_A, KEY_LEFTSHIFT = 30, 42
failed = []


def check(cond, what):
    print(("  PASS " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failed.append(what)
    return cond


def build():
    b = "/tmp/ikvm-build-%s" % ("base" if BASE else "new")
    shutil.rmtree(b, ignore_errors=True)
    os.makedirs(b)
    for f in ("ikvm_input.cpp", "ikvm_input.hpp", "scancodes.hpp"):
        shutil.copy(os.path.join(SRC, f), b)
    shutil.copy(os.path.join(HERE, "ikvmtest/stub/ikvm_server.hpp"), b)
    K.sh("cd %s && g++ -std=c++17 -O1 -w %s -I%s/ikvmtest/stub -I. -o t %s/ikvmtest/ikvm_input_test.cpp ikvm_input.cpp -lvncserver"
         % (b, "-DBASE" if BASE else "", HERE, HERE))
    return b + "/t"


def run(t, what, timeout=10):
    try:
        r = subprocess.run([t, what], capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return -1, "timeout"


def main():
    t = build()
    K.gadget_down()
    rc, out = K.gadget_up(os.path.join(GSRC, "create_usbhid.sh"))
    check(rc == 0 and K.wait_configured(), "gadget up")
    devs = K.input_devices()

    print("== wakeup: the Shift wakeup packet only for a connected client")
    kb = K.Events(devs["kbd"])
    run(t, "wakeup0")
    kb.read(quiet=0.5)
    sh0 = kb.keys(KEY_LEFTSHIFT)
    check(not sh0, "no client: no Shift on the host (%s)" % sh0)
    kb.ev.clear()
    run(t, "wakeup1")
    kb.read(quiet=0.5)
    sh1 = kb.keys(KEY_LEFTSHIFT)
    check(sh1 == [(KEY_LEFTSHIFT, 1), (KEY_LEFTSHIFT, 0)], "a client: Shift pressed and released (%s)" % sh1)

    print("== release: a client that goes lets go of its keys")
    kb.ev.clear()
    run(t, "release")
    kb.read(quiet=0.5)
    a = kb.keys(KEY_A)
    check(a[:1] == [(KEY_A, 1)] and a[-1:] == [(KEY_A, 0)], "A pressed, then released when the client went (%s%s)" % (a[:4], " ..." if len(a) > 4 else ""))
    kb.close()

    print("== rebind: the gadget unbound and bound again: the next key gets through")
    p = subprocess.Popen([t, "rebind"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.8)
    K.sh("echo '' > %s/UDC; sleep 0.5; echo %s > %s/UDC" % (K.G, K.UDC, K.G))
    K.wait_configured()
    kb = K.Events(K.input_devices()["kbd"])
    open("/tmp/ikvm-go", "w").close()
    p.wait(timeout=30)
    kb.read(quiet=0.5)
    b = kb.keys(48)
    check((48, 1) in b and b[-1:] == [(48, 0)], "B reaches the host after the rebind (%s)" % b[:6])
    kb.close()
    devs = K.input_devices()

    print("== block: the host stops taking keyboard reports")
    intf = [d for d in os.listdir("/sys/bus/usb/drivers/usbhid") if d.endswith(":1.0")]
    K.sh("echo %s > /sys/bus/usb/drivers/usbhid/unbind" % intf[0])
    t0 = time.time()
    rc, out = run(t, "block", timeout=8)
    dt = time.time() - t0
    check(rc == 0 and dt < 2, "8 keyboard reports nobody takes: obmc-ikvm goes on (rc %d, %.1f s: %s)" % (rc, dt, out.strip().splitlines()[-1:] if out else ""))
    K.sh("echo %s > /sys/bus/usb/drivers/usbhid/bind" % intf[0])
    print("%d failed" % len(failed))
    sys.exit(min(len(failed), 100))


if __name__ == "__main__":
    main()
