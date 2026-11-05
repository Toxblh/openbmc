#!/usr/bin/env python3
"""The USB network to the host (reimu-usbnet: the BMC as a CDC NCM card on its
virtual hub) on the A/B image in QEMU. QEMU has no USB host on the vhub: the
gadget binds, usb0 appears without a carrier; the data path itself is
tests/hidvm/run-usbnet.sh.

    test_usbnet_qemu.py WORKDIR [TEST...]

WORKDIR holds ab.mtd as for test_ab_qemu.py (RQ, AB_SSH, AB_HTTPS too).
Tests (all by default, one boot):

  boot      reimu-usbnet and its watcher active (the watcher leaves a
            card that sends alone); the gadget reimu_net: ncm.usb0 with eth0's
            MAC at the BMC's end, the host's end with the locally
            administered bit flipped, "REIMU BMC network", on vhub p2
            (obmc_hid still on p1); usb0 has 172.31.83.1/29 from
            networkd (its link-local address comes with a host on the port:
            not in QEMU); phosphor-networkd leaves it alone: no
            00-bmc-usb0.network, no D-Bus object, not in Redfish; the
            host agent's link listens on usb0
  ports     the agent installer's disk (reimu-agent-disk) still gets a port
            (p3) next to it
  internet  set internet on: the NAT (masquerade of 172.31.83.0/29 out of
            eth0), forwarding, the lease's router; kept over a restart of the
            service; off: all of it gone
  enabled   set enabled off: the gadget and usb0 gone; on: back on p2
  vhubreset reimu-hid-watch's reset of the virtual hub: reimu_net bound to
            its port again
"""
import json, os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ab_qemu as T  # noqa: E402  (takes WORKDIR from sys.argv[1])
from test_ab_qemu import check, fresh  # noqa: E402

G = "/sys/kernel/config/usb_gadget/reimu_net"
VHUB = "1e6a0000.usb-vhub"
ONLY = T.ONLY


def ll_of(mac):
    b = [int(x, 16) for x in mac.split(":")]
    b[0] ^= 2
    w = [b[0] << 8 | b[1], b[2] << 8 | 0xff, 0xfe << 8 | b[3], b[4] << 8 | b[5]]
    return "fe80::" + ":".join("%x" % x for x in w)


def status(b):
    try:
        return json.loads(b.out("/usr/libexec/reimu-usbnet status").strip())
    except ValueError:
        return {}


def wait_addr(b, want, timeout=30):
    end = time.time() + timeout
    a = ""
    while time.time() < end:
        a = b.out("ip addr show dev usb0 2>&1")
        if want in a:
            return a
        time.sleep(1)
    return a


def t_boot(b):
    end = time.time() + 120
    while time.time() < end and b.out("systemctl is-active reimu-usbnet").strip() != "active":
        time.sleep(2)
    check(b.out("systemctl is-active reimu-usbnet").strip() == "active", "reimu-usbnet active")
    mac = b.out("cat /sys/class/net/eth0/address").strip()
    host = "%02x" % (int(mac[:2], 16) ^ 2) + mac[2:]
    # (f_ncm prints them with a NUL at the end)
    d = b.out("cat %s/functions/ncm.usb0/dev_addr 2>&1" % G).strip("\x00 \n")
    h = b.out("cat %s/functions/ncm.usb0/host_addr 2>&1" % G).strip("\x00 \n")
    check(d == mac, "the BMC's end has eth0's MAC %s (%r)" % (mac, d))
    check(h == host, "the host's end %s (%r)" % (host, h))
    check(b.out("cat %s/strings/0x409/product" % G).strip() == "REIMU BMC network", "product «REIMU BMC network»")
    udc = b.out("cat %s/UDC" % G).strip()
    check(udc == VHUB + ":p2", "on vhub p2 (%s)" % udc)
    end = time.time() + 120
    hid = ""
    while time.time() < end and not hid:
        hid = b.out("cat /sys/kernel/config/usb_gadget/obmc_hid/UDC 2>/dev/null").strip()
        time.sleep(0 if hid else 2)
    check(hid == VHUB + ":p1", "the KVM's keyboard and mouse still on p1 (%s)" % hid)
    check(b.out("cat /sys/class/net/usb0/address 2>&1").strip() == mac, "usb0 with eth0's MAC")
    ll = ll_of(mac)
    a = wait_addr(b, "172.31.83.1/29")
    check("172.31.83.1/29" in a, "networkd: usb0 172.31.83.1/29 without a host on the port (%s)" % " ".join(re.findall(r"inet6? (\S+)", a)))
    # the link-local address comes with the carrier (a host that configured the
    # card): none in QEMU; tests/hidvm/run-usbnet.sh checks it with a USB host
    print("  INFO usb0 carrier %s; link-local %s %s" % (b.out("cat /sys/class/net/usb0/carrier 2>&1").strip(), ll,
                                                         "present" if ll in a else "not yet (no carrier)"))
    nf = b.out("cat /run/systemd/network/00-reimu-usb0.network")
    check("DHCPServer=yes" in nf and "EmitRouter=no" in nf, "the network file: a DHCP server, no router")
    # phosphor-networkd: not its interface
    check(b.ssh("test -e /etc/systemd/network/00-bmc-usb0.network").returncode != 0, "no 00-bmc-usb0.network (no DHCP client on usb0)")
    tree = b.out("busctl tree --list xyz.openbmc_project.Network")
    check("/xyz/openbmc_project/network/eth0" in tree and "usb0" not in tree, "phosphor-networkd: eth0 on D-Bus, usb0 not")
    r = b.out("curl -sk -u root:0penBmc https://127.0.0.1/redfish/v1/Managers/bmc/EthernetInterfaces")
    check('"eth0"' in r or "/eth0" in r, "Redfish lists eth0")
    check("usb0" not in r, "Redfish doesn't list usb0")
    # the agent's link on usb0 too
    end = time.time() + 60
    ok = False
    while time.time() < end and not ok:
        ok = bool(re.search(r"listening.*usb0|usb0.*listening", b.out("journalctl -u reimu-hostlink --no-pager | tail -50")))
        time.sleep(0 if ok else 2)
    check(ok, "reimu-hostlink listens on usb0")
    # the watcher of the card's sending endpoint: running, and it leaves a
    # card that isn't stuck alone (no "plugged in again" in 15 s)
    check(b.out("systemctl is-active reimu-usbnet-watch").strip() == "active", "reimu-usbnet-watch active")
    time.sleep(15)
    j = b.out("journalctl -u reimu-usbnet-watch --no-pager")
    check("stalled" not in j and not b.ssh("test -e /run/reimu-usbnet/rebind.log").returncode == 0,
          "the watcher left the card alone (%s)" % j.strip().splitlines()[-1:])
    s = status(b)
    check(s.get("enabled") is True and s.get("internet") is False and s.get("port") == VHUB + ":p2" and s.get("error") == "",
          "status: %s" % s)


def t_ports(b):
    pair = "reimu1:" + "A" * 40
    r = b.ssh("echo %s | /usr/libexec/reimu-agent-disk start && cat /sys/kernel/config/usb_gadget/reimu_agent/UDC; "
              "/usr/libexec/reimu-agent-disk stop" % pair, timeout=60)
    check(r.returncode == 0 and VHUB + ":p3" in r.stdout, "the agent installer's disk on p3 next to it (%s)" % (r.stdout + r.stderr).strip()[-200:])
    check(b.out("cat %s/UDC" % G).strip() == VHUB + ":p2", "the USB network still on p2")


def nat(b):
    return b.out("iptables -w -t nat -S REIMU-USBNET 2>&1; iptables -w -S REIMU-USBNET-FWD 2>&1; cat /proc/sys/net/ipv4/ip_forward")


def t_internet(b):
    r = b.ssh("/usr/libexec/reimu-usbnet set internet on", timeout=60)
    check(r.returncode == 0 and '"internet":true' in r.stdout, "set internet on (%s)" % (r.stdout + r.stderr).strip()[-200:])
    n = nat(b)
    check(re.search(r"-A REIMU-USBNET -s 172\.31\.83\.0/29 -o eth0 -j MASQUERADE", n) is not None, "the NAT: masquerade 172.31.83.0/29 out of eth0")
    check("RELATED,ESTABLISHED" in n and n.strip().endswith("1"), "forwarding on, only answers back in")
    nf = b.out("cat /run/systemd/network/00-reimu-usb0.network")
    check("EmitRouter=yes" in nf and "IPForward=ipv4" in nf, "the lease names the BMC as the router")
    check("INTERNET=yes" in b.out("cat /etc/reimu-usbnet.conf"), "kept in /etc/reimu-usbnet.conf")
    b.ssh("systemctl restart reimu-usbnet", timeout=60)
    check("MASQUERADE" in nat(b), "on again after a restart of the service")
    r = b.ssh("/usr/libexec/reimu-usbnet set internet off", timeout=60)
    n = nat(b)
    check(r.returncode == 0 and "MASQUERADE" not in n and n.strip().endswith("0"), "set internet off: no NAT, no forwarding")
    check("EmitRouter=no" in b.out("cat /run/systemd/network/00-reimu-usb0.network"), "no router in the lease")


def t_enabled(b):
    r = b.ssh("/usr/libexec/reimu-usbnet set enabled off", timeout=60)
    check(r.returncode == 0 and '"enabled":false' in r.stdout, "set enabled off")
    time.sleep(2)
    check(b.ssh("test -e %s || test -e /sys/class/net/usb0" % G).returncode != 0, "the gadget and usb0 gone")
    r = b.ssh("/usr/libexec/reimu-usbnet set enabled on", timeout=60)
    check(r.returncode == 0 and b.out("cat %s/UDC" % G).strip() == VHUB + ":p2", "set enabled on: back on p2")
    check("172.31.83.1/29" in wait_addr(b, "172.31.83.1/29"), "usb0 configured again")


def t_vhubreset(b):
    out = b.out("systemctl stop reimu-hid-watch; /usr/sbin/reimu-hid-watch reset udc 2>&1; systemctl start reimu-hid-watch", timeout=90)
    check("gadgets bound again" in out, "the virtual hub reset (%s)" % out.strip().replace("\n", " | ")[-300:])
    check(b.out("cat %s/UDC" % G).strip() == VHUB + ":p2", "reimu_net bound to p2 again")
    check("172.31.83.1/29" in wait_addr(b, "172.31.83.1/29"), "usb0 configured again")


def main():
    b = T.Bmc("usbnet", fresh("usbnet")).start()
    try:
        if not check(b.wait_ssh(), "boots, ssh"):
            return
        for name, fn in (("boot", t_boot), ("ports", t_ports), ("internet", t_internet), ("enabled", t_enabled),
                         ("vhubreset", t_vhubreset)):
            if name == "boot" or not ONLY or name in ONLY:
                print("== " + name, flush=True)
                fn(b)
    except Exception as e:  # noqa: BLE001  a crash is a failure
        check(False, "crashed: %r" % e)
    finally:
        b.stop()
    print("\n%d failed" % len(T.failed) + "".join("\n  " + f for f in T.failed))
    sys.exit(1 if T.failed else 0)


if __name__ == "__main__":
    main()
