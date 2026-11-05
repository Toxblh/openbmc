#!/usr/bin/env python3
"""The USB network between the BMC and its host (reimu-usbnet) in the HID
test VM, the VM being both: the gadget on a dummy_udc (the BMC's end, the
script under busybox sh as on the BMC, systemd-networkd with its network
file and DHCP server) and the USB host (cdc_ncm; its interface moved into the
network namespace "host", so the two ends talk over USB, not loopback).
run-usbnet.sh copies the script to /mnt/un:

    usbnetscen.py /mnt/un

Checks: the host's card (driver cdc_ncm, product "REIMU BMC network", the
host MAC = the BMC's with the locally administered bit flipped); IPv6
link-local both ways (the BMC's address from its eth0 MAC, as the agent
computes it); DHCP: an address in the BMC's /29, no router and no DNS with the
internet off, the BMC as the router and its DNS with it on; a TCP transfer
over the link (MB/s: dummy_hcd's speed, not the board's); stop: the card gone
from the host; enabled off: not made at a start.
"""
import os, re, socket, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kvmvm as K  # noqa: E402

UN = sys.argv[1] if len(sys.argv) > 1 else "/mnt/un"
G = "/sys/kernel/config/usb_gadget/reimu_net"
T = "/tmp/usbnet"
MAC = "02:00:00:00:00:7e"       # the fake BMC's eth0 (02: a local MAC in the VM)
HOST_MAC = "00:00:00:00:00:7e"
LL = "fe80::ff:fe00:7e"         # EUI-64 of MAC
failed = []


def check(cond, what):
    print(("  PASS " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failed.append(what)
    return cond


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def usbnet(*a):
    env = dict(os.environ, PATH=K.busybox_path() + ":/usr/bin:/usr/sbin:/bin:/sbin", REIMU_NET_CLASS=T + "/net",
               REIMU_USBNET_CONF=T + "/reimu-usbnet.conf", REIMU_RESOLV=T + "/resolv.conf", REIMU_USBNET_RUN=T + "/run",
               REIMU_IPTABLES="/bin/true", REIMU_SYSCTL="/bin/true")
    r = subprocess.run(["/usr/bin/busybox", "sh", UN + "/reimu-usbnet"] + list(a), capture_output=True, text=True, env=env)
    return r.returncode, r.stdout + r.stderr


def host_if(timeout=15):
    """the host's end: the cdc_ncm interface with the BMC's product string,
    in the root namespace (just plugged) or in "host" (moved)"""
    end = time.time() + timeout
    while time.time() < end:
        for n in os.listdir("/sys/class/net"):
            d = "/sys/class/net/%s/device" % n
            try:
                drv = os.path.basename(os.readlink(d + "/driver"))
                prod = open(d + "/../product").read().strip()
            except OSError:
                continue
            if drv == "cdc_ncm" and prod == "REIMU BMC network":
                return n
        time.sleep(0.3)
    return None


def attach_host(timeout=15):
    """the host's end (just plugged, in the root namespace) into "host" as hnet"""
    hi = host_if(timeout)
    if hi is None:
        return None
    sh("ip netns add host 2>/dev/null")
    r = sh("ip link set %s netns host && ip netns exec host ip link set %s name hnet && ip netns exec host ip link set hnet up && ip netns exec host ip link set lo up" % (hi, hi))
    return hi if r.returncode == 0 else None


def host_gone(timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        if "hnet" not in in_host("ip link").stdout:
            return True
        time.sleep(0.1)
    return False


def in_host(cmd):
    return sh("ip netns exec host " + cmd)


def dhcp():
    """a lease in the host namespace by busybox udhcpc: {ip, router, dns}"""
    s = T + "/udhcpc.sh"
    open(s, "w").write('#!/bin/sh\n[ "$1" = bound ] && echo "LEASE ip=$ip router=$router dns=$dns mask=$mask"\nexit 0\n')
    os.chmod(s, 0o755)
    r = in_host("busybox udhcpc -i hnet -n -q -f -t 8 -T 1 -s %s" % s)
    m = re.search(r"LEASE ip=(\S*) router=(\S*) dns=(.*?) mask=(\S*)", r.stdout)
    return {"ip": m.group(1), "router": m.group(2), "dns": m.group(3).strip(), "mask": m.group(4)} if m else {"out": r.stdout + r.stderr}


def reset():
    usbnet("stop")
    sh("ip netns del host")
    sh("rm -rf %s && mkdir -p %s/net/eth0" % (T, T))
    open(T + "/net/eth0/address", "w").write(MAC + "\n")
    open(T + "/resolv.conf", "w").write("nameserver 10.0.2.3\n")


def main():
    reset()
    rc, out = usbnet("start")
    check(rc == 0, "start: %s" % out.strip())
    udc = open(G + "/UDC").read().strip() if os.path.exists(G + "/UDC") else ""
    check(udc.startswith("dummy_udc."), "bound to %s" % udc)
    hi = host_if()
    if not check(hi is not None, "the host has a cdc_ncm card «REIMU BMC network» (%s)" % hi):
        print(sh("dmesg | tail -30").stdout)
        sys.exit(1)
    check(open("/sys/class/net/%s/address" % hi).read().strip() == HOST_MAC, "its MAC %s: the BMC's, locally administered bit flipped" % HOST_MAC)
    bi = open(G + "/functions/ncm.usb0/ifname").read().strip()
    check(open("/sys/class/net/%s/address" % bi).read().strip() == MAC, "the BMC's end (%s) has eth0's MAC" % bi)
    # the host's end into its own namespace: from here the two ends talk over USB
    check(attach_host() is not None, "the host's end in namespace host")
    # the BMC's end: networkd with reimu-usbnet's file (the address, link-local, DHCP server)
    end = time.time() + 20
    while time.time() < end and "172.31.83.1/29" not in sh("ip -4 addr show dev %s" % bi).stdout:
        time.sleep(0.5)
    a = sh("ip addr show dev %s" % bi).stdout
    check("172.31.83.1/29" in a and LL in a, "networkd set %s: 172.31.83.1/29 and %s (%s)" % (bi, LL, " ".join(re.findall(r"inet6? (\S+)", a))))
    # IPv6 link-local, the agent's way to the BMC: its address from BMC_MAC
    end = time.time() + 15
    ok = False
    while time.time() < end and not ok:
        ok = in_host("busybox ping6 -c 1 -W 1 %s%%hnet" % LL).returncode == 0
    check(ok, "the host reaches the BMC at %s%%(its card) (the address the agent computes from BMC_MAC)" % LL)
    # DHCP, internet off
    l = dhcp()
    check(l.get("ip", "").startswith("172.31.83.") and l.get("mask") in ("29", "255.255.255.248"), "a lease in 172.31.83.0/29: %s" % l)
    check(l.get("router") == "" and l.get("dns") == "", "no router, no DNS with the internet off (the host's routes stay)")
    # TCP over the link
    srv = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("::", 5599, 0, 0))
    srv.listen(1)
    got = [0]

    def rx():
        c, _ = srv.accept()
        while True:
            b = c.recv(1 << 16)
            if not b:
                break
            got[0] += len(b)
        c.close()
    th = threading.Thread(target=rx)
    th.start()
    n = 64 << 20
    t0 = time.time()
    r = in_host("sh -c 'head -c %d /dev/zero | busybox nc -w 5 %s%%hnet 5599'" % (n, LL))
    th.join(30)
    dt = time.time() - t0
    srv.close()
    check(got[0] == n, "64 MiB host -> BMC over USB: %d bytes, %.1f MB/s (dummy_hcd)" % (got[0], n / dt / 1e6))
    # internet on: the card leaves the host's bus and comes back (a cable
    # plugged in again: the host's DHCP at once, the router in the lease)
    t0 = time.time()
    rc, out = usbnet("set", "internet", "on")
    check(rc == 0 and '"internet":true' in out, "set internet on: %s" % out.strip()[-120:])
    check(host_gone(1), "the card went off the host's bus")
    check(attach_host() is not None, "and came back in %.1f s" % (time.time() - t0))
    time.sleep(1)
    l = dhcp()
    check(l.get("router") == "172.31.83.1" and "10.0.2.3" in l.get("dns", ""), "internet on: router 172.31.83.1, DNS from the BMC's (%s)" % l)
    rc, out = usbnet("set", "internet", "off")
    check(host_gone(1) and attach_host() is not None, "internet off: the card plugged in again too")
    time.sleep(1)
    l = dhcp()
    check(l.get("router") == "", "internet off again: no router (%s)" % l)
    # status: the host's address from the neighbours (the lease on the host's
    # end as NetworkManager would put it)
    in_host("ip addr add %s/29 dev hnet" % l.get("ip"))
    in_host("busybox ping -c 1 -W 1 172.31.83.1")
    rc, out = usbnet("status")
    check(rc == 0 and '"host_ipv4":"172.31.83.' in out and '"error":""' in out, "status: %s" % out.strip())
    # stop: the card gone from the host
    usbnet("stop")
    end = time.time() + 10
    while time.time() < end and "hnet" in in_host("ip link").stdout:
        time.sleep(0.3)
    check("hnet" not in in_host("ip link").stdout, "stop: the card gone from the host")
    check(not os.path.exists(G), "stop: no gadget in configfs")
    # enabled off: a start makes nothing
    rc, out = usbnet("set", "enabled", "off")
    check(rc == 0 and '"enabled":false' in out, "set enabled off")
    usbnet("start")
    check(not os.path.exists(G) and host_if(3) is None, "a start with it off: no card")
    usbnet("set", "enabled", "on")
    check(host_if() is not None, "enabled on: the card back")
    reset()
    print("== usbnet: %d failed" % len(failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
