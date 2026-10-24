#!/usr/bin/env python3
"""The KVM's USB keyboard and mouse (obmc_hid, made by create_usbhid.sh in
start-ipkvm's ExecStartPre) on the A/B image in QEMU.

    test_kvm_hid_qemu.py WORKDIR [TEST...]

WORKDIR holds ab.mtd as for test_ab_qemu.py (RQ, AB_SSH, AB_HTTPS too).
Tests (all by default, one boot):

  gadget   start-ipkvm up; hid.0 (keyboard) and hid.1 (mouse): report_desc
           byte for byte the descriptors of hid_desc.py (built there from the
           HID items, not from the script), parsed (known item tags,
           collections balanced, the input report = report_length: keyboard
           8, mouse 7), protocol, subclass; /dev/hidg0, /dev/hidg1; the
           gadget bound to a vhub port (the state: "configured" needs a USB
           host, QEMU has none on the vhub: INFO)
  restart  systemctl restart start-ipkvm, then obmc-ikvm killed (-9) three
           times (systemd brings it back, ExecStartPre runs again): the
           descriptors unchanged, the gadget still bound to the same port,
           never unbound (no new gadget/vhub messages in dmesg)
  vhubreset no legacy g_mass_storage in dmesg (it takes every free vhub
           port); a vhub reset by reimu-hid-watch (forced: HOST_ON=true,
           GRACE=2; QEMU has no USB host, so the 3 s address wait runs out):
           obmc_hid on its port again, still no g_mass_storage; then a
           virtual media gadget of reimu_state_hook (on a loop device in
           place of /dev/nbd1) binds to a free port
"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ab_qemu as T  # noqa: E402  (takes WORKDIR from sys.argv[1])
from test_ab_qemu import check, fresh  # noqa: E402
import hid_desc as H  # noqa: E402

G = "/sys/kernel/config/usb_gadget/obmc_hid"
ONLY = T.ONLY


def snapshot(b):
    """{hid.N: (descriptor bytes, protocol, subclass, report_length)}, UDC"""
    out = b.out("for f in hid.0 hid.1; do d=%s/functions/$f; echo \"$f $(cat $d/protocol) $(cat $d/subclass) "
                "$(cat $d/report_length) $(od -An -tx1 -v $d/report_desc | tr -s ' \\n' '  ')\"; done; "
                "echo \"UDC $(cat %s/UDC)\"" % (G, G))
    fns, udc = {}, None
    for l in out.splitlines():
        w = l.split()
        if w and w[0] == "UDC":
            udc = w[1] if len(w) > 1 else ""
        elif len(w) >= 4 and w[0].startswith("hid."):
            try:
                fns[w[0]] = (bytes(int(x, 16) for x in w[4:]), int(w[1]), int(w[2]), int(w[3]))
            except ValueError:
                fns[w[0]] = (b"", -1, -1, -1)
    return fns, udc


def check_snapshot(fns, when):
    ok = True
    for fn, (name, want, proto, sub, length) in sorted(H.FUNCTIONS.items()):
        if not check(fn in fns, "%s: %s %s exists" % (when, fn, name)):
            ok = False
            continue
        desc, p, s, rl = fns[fn]
        probs = H.check_function(desc, fn, rl)
        for x in probs:
            print("         " + x)
        ok &= check(not probs, "%s: %s %s descriptor: the expected %d bytes, parses, report %d bytes (%d bytes)"
                    % (when, fn, name, len(want), length, len(desc)))
        ok &= check((p, s) == (proto, sub), "%s: %s protocol %d subclass %d (%d %d)" % (when, fn, proto, sub, p, s))
    return ok


def gadget_msgs(b):
    return int(b.out("dmesg | grep -ciE 'vhub|gadget|udc|hidg' ; true").strip() or 0)


def wait_active(b, timeout=60, other_than=None):
    end = time.time() + timeout
    while time.time() < end:
        pid = b.out("systemctl show -p MainPID --value start-ipkvm; systemctl is-active start-ipkvm").split()
        if len(pid) == 2 and pid[1] == "active" and pid[0] not in ("0", other_than):
            return pid[0]
        time.sleep(1)
    return None


def t_gadget(b):
    check(wait_active(b, 120) is not None, "start-ipkvm active")
    pre = b.out("systemctl show -p ExecStartPre start-ipkvm")
    m = re.search(r"status=(\d+)", pre)
    check(m is not None and m.group(1) == "0", "create_usbhid.sh (ExecStartPre) exit status 0 (%s)"
          % (m.group(1) if m else pre.strip()[:80]))
    fns, udc = snapshot(b)
    check_snapshot(fns, "boot")
    check(b.ssh("test -c /dev/hidg0 && test -c /dev/hidg1").returncode == 0, "/dev/hidg0 and /dev/hidg1")
    check(bool(udc) and udc.startswith("1e6a0000.usb-vhub:p"), "gadget bound to a vhub port (UDC=%s)" % udc)
    if udc:
        st = b.out("cat /sys/class/udc/%s/state 2>&1" % udc).strip()
        check(b.ssh("test -e /sys/class/udc/%s" % udc).returncode == 0, "/sys/class/udc/%s exists" % udc)
        print("  INFO UDC %s state: %s (configured only with a USB host on the vhub)" % (udc, st))
    return fns, udc


def t_restart(b, base):
    fns0, udc0 = base
    msgs0 = gadget_msgs(b)
    check(b.ssh("systemctl restart start-ipkvm").returncode == 0, "systemctl restart start-ipkvm")
    pid = wait_active(b, 60)
    check(pid is not None, "start-ipkvm active again")
    fns, udc = snapshot(b)
    check(fns == fns0, "after restart: descriptors and attributes unchanged")
    check(udc == udc0, "after restart: still bound to %s (%s)" % (udc0, udc))
    for i in range(3):
        b.ssh("kill -9 $(systemctl show -p MainPID --value start-ipkvm)")
        new = wait_active(b, 60, other_than=pid)
        check(new is not None and new != pid, "kill -9 #%d: obmc-ikvm back (pid %s -> %s)" % (i + 1, pid, new))
        pid = new or pid
        fns, udc = snapshot(b)
        check_snapshot(fns, "kill #%d" % (i + 1))
        check(fns == fns0 and udc == udc0, "kill -9 #%d: descriptors unchanged, still bound to %s (%s)"
              % (i + 1, udc0, udc))
    msgs = gadget_msgs(b)
    check(msgs == msgs0, "the gadget never unbound/rebound (gadget messages in dmesg: %d -> %d)" % (msgs0, msgs))
    print("  INFO start-ipkvm restarts: %s" % b.out("systemctl show -p NRestarts --value start-ipkvm").strip())


def t_vhubreset(b):
    check("g_mass_storage" not in b.out("dmesg"), "after boot: no legacy g_mass_storage in dmesg")
    udc0 = b.out("cat %s/UDC" % G).strip()
    out = b.out("systemctl stop reimu-hid-watch; HOST_ON=true GRACE=2 POLL=1 timeout 15 /usr/sbin/reimu-hid-watch 2>&1;"
                " systemctl start reimu-hid-watch", timeout=60)
    check(out.count("resetting the virtual hub") == 1 and "gadgets bound again" in out,
          "reimu-hid-watch reset the vhub once (%s)" % out.strip().replace("\n", " | ")[:200])
    udc = b.out("cat %s/UDC" % G).strip()
    check(udc == udc0, "obmc_hid bound to %s again (%s)" % (udc0, udc))
    d = [l for l in b.out("dmesg").splitlines() if "g_mass_storage" in l]
    check(not d, "after the vhub reset: no legacy g_mass_storage in dmesg%s" % ("".join("\n         " + l for l in d[:3])))
    r = b.ssh("dd if=/dev/zero of=/tmp/cd.iso bs=1M count=4 2>/dev/null && losetup /dev/loop1 /tmp/cd.iso && "
              "sed 's/nbd1/loop1/g; s/nbd0/loop0/g' /etc/nbd-proxy/state > /tmp/state && sh /tmp/state start 1 && "
              "cat /sys/kernel/config/usb_gadget/mass-storage1/UDC && sh /tmp/state stop 1; losetup -d /dev/loop1", timeout=60)
    check(r.returncode == 0 and "usb-vhub:p" in r.stdout,
          "virtual media (configfs mass storage) binds to a vhub port (%s)" % (r.stdout + r.stderr).strip().replace("\n", " | ")[:200])


def main():
    b = T.Bmc("kvmhid", fresh("kvmhid")).start()
    try:
        if not check(b.wait_ssh(), "boots, ssh"):
            return
        print("== gadget", flush=True)
        base = t_gadget(b)
        if not ONLY or "restart" in ONLY:
            print("== restart", flush=True)
            t_restart(b, base)
        if not ONLY or "vhubreset" in ONLY:
            print("== vhubreset", flush=True)
            t_vhubreset(b)
    except Exception as e:  # noqa: BLE001  a crash is a failure
        check(False, "crashed: %r" % e)
    finally:
        b.stop()
    print("\n%d failed" % len(T.failed) + "".join("\n  " + f for f in T.failed))
    sys.exit(1 if T.failed else 0)


if __name__ == "__main__":
    main()
