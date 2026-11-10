#!/usr/bin/env python3
"""reimu-usbnet (the USB network between the BMC and its host) against a fake
/sys/kernel/config, /sys/class/udc and /sys/class/net, with iptables,
sysctl, ip and networkd's reload as stubs that log what they were asked: the
gadget and the port it takes (p2, else p3; never p1: the KVM's keyboard and
mouse; p4, p5: virtual media), its MAC addresses, the network file, the
host's internet through the BMC (NAT) on and off, the setting kept, refusals.
The real configfs and a USB host: meta-mcst/tests/hidvm (usbnet); on the
image: tests/ab64/test_usbnet_qemu.py. Linux; the build container:

    python3 tests/test_usbnet.py [-v]

SH=dash|bash|"busybox sh" (default sh) runs the script.
"""
import json, os, shlex, shutil, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "reimu-usbnet")
SH = shlex.split(os.environ.get("SH", "sh"))
VHUB = "1e6a0000.usb-vhub"
MAC = "98:a7:b0:12:34:7e"
HOST_MAC = "9a:a7:b0:12:34:7e"

STUB = """#!/bin/sh
echo "$(basename "$0") $*" >> "%(log)s"
case "$(basename "$0") $*" in
  "ip neigh show dev usb0"*|"ip -4 neigh show dev usb0"*) cat "%(neigh)s" 2>/dev/null ;;
  "iptables "*"-C "*) exit 1 ;;
esac
exit 0
"""


class T(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="usbnet-")
        j = lambda *p: os.path.join(self.d, *p)
        self.cfs, self.udc, self.net, self.nd = j("config"), j("udc"), j("net"), j("network")
        self.conf, self.log, self.neigh, self.resolv = j("reimu-usbnet.conf"), j("calls.log"), j("neigh"), j("resolv.conf")
        self.arp = j("arp")
        os.makedirs(os.path.join(self.cfs, "usb_gadget"))
        for p in range(1, 6):
            os.makedirs(os.path.join(self.udc, "%s:p%d" % (VHUB, p)))
            open(os.path.join(self.udc, "%s:p%d" % (VHUB, p), "state"), "w").write("not attached\n")
        os.makedirs(os.path.join(self.net, "eth0"))
        open(os.path.join(self.net, "eth0", "address"), "w").write(MAC + "\n")
        open(self.resolv, "w").write("# from DHCP\nnameserver 192.168.10.1\nnameserver 127.0.0.53\nnameserver fd00::1\n")
        os.makedirs(j("bin"))
        for n in ("iptables", "sysctl", "ip", "reload"):
            p = j("bin", n)
            open(p, "w").write(STUB % {"log": self.log, "neigh": self.neigh})
            os.chmod(p, 0o755)
        self.env = dict(os.environ, REIMU_CONFIGFS=self.cfs, REIMU_UDC_CLASS=self.udc, REIMU_NET_CLASS=self.net,
                        REIMU_USBNET_CONF=self.conf, REIMU_NETWORK_DIR=self.nd, REIMU_RESOLV=self.resolv,
                        REIMU_IPTABLES=j("bin", "iptables"), REIMU_SYSCTL=j("bin", "sysctl"),
                        REIMU_NETWORKD_RELOAD=j("bin", "reload"), REIMU_USBNET_RUN=j("run"), REIMU_ARP=self.arp)

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def run_(self, *a):
        r = subprocess.run(SH + [SCRIPT] + list(a), env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=60)
        return r.returncode, r.stdout.decode(), r.stderr.decode()

    def calls(self):
        try:
            return [x.rstrip() for x in open(self.log).read().splitlines()]
        except FileNotFoundError:
            return []

    def g(self, *p):
        return os.path.join(self.cfs, "usb_gadget", "reimu_net", *p)

    def read(self, *p):
        return open(self.g(*p)).read().strip()

    def netfile(self):
        return open(os.path.join(self.nd, "00-reimu-usb0.network")).read()

    def other_gadget(self, name, udc):
        os.makedirs(os.path.join(self.cfs, "usb_gadget", name))
        open(os.path.join(self.cfs, "usb_gadget", name, "UDC"), "w").write(udc + "\n")

    def status(self):
        rc, out, err = self.run_("status")
        self.assertEqual(rc, 0, err)
        self.assertEqual(len(out.strip().splitlines()), 1, "one line: %r" % out)
        return json.loads(out)

    # ------------------------------------------------------------------
    def test_start(self):
        rc, out, err = self.run_("start")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.read("idVendor"), "0x1d6b")
        self.assertEqual(self.read("idProduct"), "0x0104")
        self.assertEqual(self.read("strings", "0x409", "manufacturer"), "OpenBMC")
        self.assertEqual(self.read("strings", "0x409", "product"), "REIMU BMC network")
        self.assertEqual(self.read("strings", "0x409", "serialnumber"), "REIMU-NET")
        # the BMC's end has eth0's MAC: its link-local address is the one the agent knows
        self.assertEqual(self.read("functions", "ncm.usb0", "dev_addr"), MAC)
        self.assertEqual(self.read("functions", "ncm.usb0", "host_addr"), HOST_MAC)
        self.assertTrue(os.path.islink(self.g("configs", "c.1", "ncm.usb0")))
        self.assertEqual(self.read("UDC"), VHUB + ":p2")
        n = self.netfile()
        for want in ("Name=usb0", "Address=172.31.83.1/29", "LinkLocalAddressing=ipv6", "DHCPServer=yes",
                     "EmitRouter=no", "EmitDNS=no", "PoolOffset=1", "ConfigureWithoutCarrier=yes"):
            self.assertIn(want, n)
        self.assertNotIn("IPForward", n)
        c = self.calls()
        self.assertIn("reload", c, "networkd told")
        self.assertFalse([x for x in c if "MASQUERADE" in x and " -A " in x], "no NAT by default: %s" % c)
        s = self.status()
        self.assertEqual((s["enabled"], s["internet"], s["port"], s["ifname"], s["mac"], s["host_mac"], s["error"]),
                         (True, False, VHUB + ":p2", "usb0", MAC, HOST_MAC, ""))
        self.assertEqual(s["address"], "172.31.83.1/29")
        self.assertEqual(s["state"], "not attached")

    def test_start_twice_no_rebind(self):
        self.assertEqual(self.run_("start")[0], 0)
        open(self.g("UDC"), "w").write(VHUB + ":p2\n")
        os.utime(self.g("UDC"), (1, 1))
        rc, out, err = self.run_("start")
        self.assertEqual(rc, 0, err)
        self.assertEqual(os.stat(self.g("UDC")).st_mtime, 1, "the bound gadget left alone (the host would see it go)")

    def test_ports(self):
        self.other_gadget("reimu_agent", VHUB + ":p2")
        self.assertEqual(self.run_("start")[0], 0)
        self.assertEqual(self.read("UDC"), VHUB + ":p3")

    def test_no_port(self):
        self.other_gadget("reimu_agent", VHUB + ":p2")
        self.other_gadget("x", VHUB + ":p3")
        rc, out, err = self.run_("start")
        self.assertEqual(rc, 0, "the service up anyway (a port may free up): %s" % err)
        u = self.g("UDC")
        self.assertEqual(open(u).read().strip() if os.path.exists(u) else "", "", "never p1, p4, p5")
        s = self.status()
        self.assertEqual(s["port"], "")
        self.assertIn("p2", s["error"])

    def test_test_vm_any_udc(self):
        shutil.rmtree(self.udc)
        os.makedirs(os.path.join(self.udc, "dummy_udc.1"))
        self.other_gadget("obmc_hid", "dummy_udc.0")
        os.makedirs(os.path.join(self.udc, "dummy_udc.0"))
        self.assertEqual(self.run_("start")[0], 0)
        self.assertEqual(self.read("UDC"), "dummy_udc.1")

    def test_internet(self):
        self.assertEqual(self.run_("start")[0], 0)
        open(self.log, "w").close()
        rc, out, err = self.run_("set", "internet", "on")
        self.assertEqual(rc, 0, err)
        self.assertTrue(json.loads(out)["internet"])
        self.assertIn("INTERNET=yes", open(self.conf).read())
        n = self.netfile()
        for want in ("EmitRouter=yes", "EmitDNS=yes", "DNS=192.168.10.1", "IPForward=ipv4"):
            self.assertIn(want, n)
        self.assertNotIn("127.0.0.53", n)
        self.assertNotIn("fd00::1", n)
        c = self.calls()
        self.assertTrue([x for x in c if "-t nat" in x and "-A REIMU-USBNET" in x and "-s 172.31.83.0/29" in x
                         and "-o eth0" in x and "MASQUERADE" in x], c)
        self.assertTrue([x for x in c if "-A REIMU-USBNET-FWD" in x and "-i usb0" in x and "ACCEPT" in x], c)
        self.assertTrue([x for x in c if "-A REIMU-USBNET-FWD" in x and "RELATED,ESTABLISHED" in x], c)
        self.assertIn("sysctl -w net.ipv4.ip_forward=1", c)
        self.assertTrue(self.status()["internet"])
        # and off
        open(self.log, "w").close()
        rc, out, err = self.run_("set", "internet", "off")
        self.assertEqual(rc, 0, err)
        self.assertIn("INTERNET=no", open(self.conf).read())
        self.assertIn("EmitRouter=no", self.netfile())
        c = self.calls()
        self.assertFalse([x for x in c if "MASQUERADE" in x and " -A " in x], c)
        self.assertTrue([x for x in c if "-t nat" in x and "-F REIMU-USBNET" in x], c)
        self.assertIn("sysctl -w net.ipv4.ip_forward=0", c)

    def test_internet_kept(self):
        open(self.conf, "w").write("# mine\nINTERNET=yes\n")
        self.assertEqual(self.run_("start")[0], 0)
        self.assertIn("EmitRouter=yes", self.netfile())
        self.assertIn("sysctl -w net.ipv4.ip_forward=1", self.calls())
        rc, out, err = self.run_("set", "internet", "off")
        self.assertEqual(rc, 0, err)
        conf = open(self.conf).read()
        self.assertIn("# mine", conf, "the rest of the file kept")
        self.assertEqual(conf.count("INTERNET="), 1)

    def test_disable(self):
        self.assertEqual(self.run_("start")[0], 0)
        rc, out, err = self.run_("set", "enabled", "off")
        self.assertEqual(rc, 0, err)
        self.assertFalse(os.path.exists(self.g()), "the gadget gone")
        self.assertFalse(os.path.exists(os.path.join(self.nd, "00-reimu-usb0.network")))
        self.assertIn("ENABLED=no", open(self.conf).read())
        s = json.loads(out)
        self.assertEqual((s["enabled"], s["port"]), (False, ""))
        # a boot with it off: nothing made
        rc, out, err = self.run_("start")
        self.assertEqual(rc, 0, err)
        self.assertFalse(os.path.exists(self.g()))
        rc, out, err = self.run_("set", "enabled", "on")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.read("UDC"), VHUB + ":p2")

    def test_disable_with_internet_takes_nat_down(self):
        self.assertEqual(self.run_("set", "internet", "on")[0], 0)
        open(self.log, "w").close()
        self.assertEqual(self.run_("stop")[0], 0)
        c = self.calls()
        self.assertTrue([x for x in c if "-F REIMU-USBNET" in x], c)
        self.assertIn("sysctl -w net.ipv4.ip_forward=0", c)

    def test_host_address(self):
        """from the ARP table: the board's busybox ip has no "neigh" (it printed
        its usage, the address stayed empty)"""
        self.assertEqual(self.run_("start")[0], 0)
        open(self.neigh, "w").write("172.31.83.3 lladdr %s STALE\n" % HOST_MAC)
        open(self.arp, "w").write(
            "IP address       HW type     Flags       HW address            Mask     Device\n"
            "192.168.10.50    0x1         0x0         98:a7:b0:12:34:61     *        eth0\n"
            "172.31.83.4      0x1         0x0         %s     *        usb0\n"
            "172.31.83.2      0x1         0x2         %s     *        usb0\n"
            "192.168.10.1     0x1         0x2         50:ff:20:d4:72:67     *        eth0\n" % (HOST_MAC, HOST_MAC))
        self.assertEqual(self.status()["host_ipv4"], "172.31.83.2", "the complete entry (flags 0x2) of the host's MAC on usb0")

    def test_internet_switch_reconnects_the_card(self):
        """the host learns the router only from a new lease: a lease renewal
        came up to 5 min later (board, 2026-11-05: on at 18:38:24, the host's
        route at 18:42:13). Switching the internet takes the card off the
        host's bus and back (a cable plugged in again: DHCP at once)"""
        self.assertEqual(self.run_("start")[0], 0)
        os.utime(self.g("UDC"), (1, 1))
        rc, out, err = self.run_("set", "internet", "on")
        self.assertEqual(rc, 0, err)
        self.assertNotEqual(os.stat(self.g("UDC")).st_mtime, 1, "UDC written again")
        self.assertEqual(self.read("UDC"), VHUB + ":p2", "bound to its port again")
        self.assertIn("rebind", self.calls_of("UDC"), "unbound, then bound")
        os.utime(self.g("UDC"), (1, 1))
        self.assertEqual(self.run_("set", "internet", "off")[0], 0)
        self.assertNotEqual(os.stat(self.g("UDC")).st_mtime, 1, "and when switched off (the router goes)")
        # the same value again: nothing to tell the host
        os.utime(self.g("UDC"), (1, 1))
        self.assertEqual(self.run_("set", "internet", "off")[0], 0)
        self.assertEqual(os.stat(self.g("UDC")).st_mtime, 1, "no change, no reconnect")

    # ------------------------------------------------------------------ watch
    def regs(self, eps, later=None, after=0):
        """a fake vhub: {ep: (cfg, desc_status)}; the devmem stub answers from
        it (from LATER once AFTER reads were made) and counts its calls"""
        def put(d, eps):
            shutil.rmtree(d, ignore_errors=True)
            os.makedirs(d)
            for n, (cfg, st) in eps.items():
                b = 0x1E6A0200 + n * 0x10
                open(os.path.join(d, "%#x" % b), "w").write("%#010x\n" % cfg)
                open(os.path.join(d, "%#x" % (b + 12)), "w").write("%#010x\n" % st)
        d, d2 = os.path.join(self.d, "regs"), os.path.join(self.d, "regs2")
        put(d, eps)
        put(d2, later if later is not None else eps)
        self.devmem_log = os.path.join(self.d, "devmem.log")
        open(self.devmem_log, "w").close()
        dm = os.path.join(self.d, "bin", "devmem")
        open(dm, "w").write('#!/bin/sh\necho "$1" >> %s\nd=%s\n[ $(wc -l < %s) -gt %d ] && d=%s\n'
                            'f="$d/$(printf %%#x $(($1)))"\n[ -f "$f" ] && cat "$f" || echo 0x00000000\n'
                            % (self.devmem_log, d, self.devmem_log, after, d2))
        os.chmod(dm, 0o755)
        self.env["REIMU_DEVMEM"] = dm

    def devmem_calls(self):
        return len(open(self.devmem_log).read().split())

    def watch(self, rounds):
        e = dict(self.env, REIMU_WATCH_ROUNDS=str(rounds), REIMU_WATCH_POLL="0", REIMU_WATCH_STUCK="3")
        r = subprocess.run(SH + [SCRIPT, "watch"], env=e, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
        return r.returncode, r.stdout.decode() + r.stderr.decode()

    # the board, beta5 under load: ep4 = the NCM bulk IN on p2 (dev 2, ep 1,
    # bulk, IN, 512), its read pointer one behind the write pointer for good
    NCM_IN = 0x02000125      # max 512, ep 1, bulk, IN, dev 2, enabled
    HID_IN = 0x00080143      # max 8, ep 1, interrupt, IN, dev 1

    def test_watch_stalled_in_endpoint_reconnects(self):
        self.assertEqual(self.run_("start")[0], 0)
        self.regs({1: (self.HID_IN, 0x80004242), 4: (self.NCM_IN, 0x00546869)})
        os.utime(self.g("UDC"), (1, 1))
        rc, out = self.watch(5)
        self.assertEqual(rc, 0, out)
        self.assertNotEqual(os.stat(self.g("UDC")).st_mtime, 1, "the card plugged in again: %s" % out)
        self.assertEqual(self.read("UDC"), VHUB + ":p2")
        self.assertIn("stalled", out)
        self.assertIn("ep4", out)

    def test_watch_moving_or_idle_left_alone(self):
        self.assertEqual(self.run_("start")[0], 0)
        os.utime(self.g("UDC"), (1, 1))
        self.regs({4: (self.NCM_IN, 0x80808282)})          # idle: pointers equal
        self.assertEqual(self.watch(5)[0], 0)
        self.assertEqual(os.stat(self.g("UDC")).st_mtime, 1, "idle: left alone")
        # another port's bulk IN stuck (a virtual disk on p4): not ours
        self.regs({4: (0x02000129, 0x00546869)})          # dev 4
        self.assertEqual(self.watch(5)[0], 0)
        self.assertEqual(os.stat(self.g("UDC")).st_mtime, 1, "another port's endpoint: left alone")

    def test_watch_cheap_poll(self):
        """the board, beta6: the watch read all 15 endpoints' registers every
        2 s (16 devmem runs a poll, ~32 processes, ~2 s of the BMC's CPU on a
        busy BMC: load average ~2); the card's endpoint is found once, a poll
        reads its two registers"""
        self.assertEqual(self.run_("start")[0], 0)
        os.utime(self.g("UDC"), (1, 1))
        self.regs({1: (self.HID_IN, 0x80004242), 4: (self.NCM_IN, 0x80808282)})
        self.assertEqual(self.watch(20)[0], 0)
        self.assertEqual(os.stat(self.g("UDC")).st_mtime, 1, "idle: left alone")
        self.assertLessEqual(self.devmem_calls(), 60, "devmem runs in 20 polls")

    def test_watch_endpoint_moved(self):
        """the card plugged in again (internet switched) may get another
        endpoint: the one found before no longer the card's bulk IN (now the
        keyboard's, its pointers apart) is not taken for a stall; the card's
        new one is watched"""
        self.assertEqual(self.run_("start")[0], 0)
        os.utime(self.g("UDC"), (1, 1))
        self.regs({4: (self.NCM_IN, 0x80808282)},
                  later={4: (self.HID_IN, 0x00546869), 6: (self.NCM_IN, 0x00546869)}, after=20)
        rc, out = self.watch(12)
        self.assertEqual(rc, 0, out)
        self.assertNotEqual(os.stat(self.g("UDC")).st_mtime, 1, "the new endpoint's stall seen: %s" % out)
        self.assertIn("ep6", out)
        self.assertNotIn("ep4", out)

    def test_watch_not_bound_nothing(self):
        self.assertEqual(self.run_("set", "enabled", "off")[0], 0)
        self.regs({4: (self.NCM_IN, 0x00546869)})
        rc, out = self.watch(4)
        self.assertEqual(rc, 0, out)
        self.assertFalse(os.path.exists(self.g()))

    def calls_of(self, what):
        try:
            return open(os.path.join(self.d, "run", "rebind.log")).read()
        except FileNotFoundError:
            return ""

    def test_net_setting(self):
        open(self.conf, "w").write("NET=10.9.8.1/29\nINTERNET=yes\n")
        self.assertEqual(self.run_("start")[0], 0)
        self.assertIn("Address=10.9.8.1/29", self.netfile())
        self.assertTrue([x for x in self.calls() if "-s 10.9.8.0/29" in x and "MASQUERADE" in x])

    def test_bad(self):
        for a in (("set", "internet", "maybe"), ("set", "colour", "on"), ("frobnicate",)):
            rc, out, err = self.run_(*a)
            self.assertEqual(rc, 2, a)
            self.assertTrue(err.strip(), a)
        open(self.conf, "w").write("NET=300.1.1.1/29\n")
        rc, out, err = self.run_("start")
        self.assertNotEqual(rc, 0)
        self.assertIn("NET", err)

    def test_no_eth0(self):
        shutil.rmtree(os.path.join(self.net, "eth0"))
        rc, out, err = self.run_("start")
        self.assertNotEqual(rc, 0)
        self.assertIn("eth0", err)


if __name__ == "__main__":
    unittest.main()
