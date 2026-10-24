#!/usr/bin/env python3
"""reimu-agent-disk (the BMC's installer USB disk for the host) against a fake
/sys/kernel/config and /sys/class/udc: the image with the pairing string, the
port it takes (never p1: the KVM's keyboard and mouse; p4, p5: virtual
media), the gadget's attributes, refusals. The real configfs and a USB host:
meta-mcst/tests/hidvm (agentdisk). Linux, mtools; the build container:

    python3 tests/test_agent_disk.py [-v]

SH=dash|bash|"busybox sh" (default sh) runs the script.
"""
import base64, json, os, shlex, shutil, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "reimu-agent-disk")
TOOLS = os.path.normpath(os.path.join(HERE, "..", "..", "..", "..", "..", "tools", "host"))
SH = shlex.split(os.environ.get("SH", "sh"))
VHUB = "1e6a0000.usb-vhub"


def pair_string(n=32):
    d = {"token": "ab" * (n // 2), "fp": "c" * 64, "mac": "98:a7:b0:06:0f:7e", "v4": "192.168.10.79", "port": 5564}
    return "reimu1:" + base64.urlsafe_b64encode(json.dumps(d, separators=(",", ":")).encode()).decode().rstrip("=")


class T(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.b = tempfile.mkdtemp(prefix="agentdisk-b-")
        r = subprocess.run(["sh", os.path.join(TOOLS, "mkbundle.sh"), cls.b, "--disk"], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
        assert r.returncode == 0, r.stdout

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.b, ignore_errors=True)

    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="agentdisk-")
        self.cfs = os.path.join(self.d, "config")
        self.udc = os.path.join(self.d, "udc")
        self.run_d = os.path.join(self.d, "run")
        os.makedirs(os.path.join(self.cfs, "usb_gadget"))
        os.makedirs(self.udc)
        for p in range(1, 6):
            os.makedirs(os.path.join(self.udc, "%s:p%d" % (VHUB, p)))
        self.env = dict(os.environ, REIMU_AGENT_DISK_IMG=os.path.join(self.b, "disk.img"),
                        REIMU_AGENT_DISK_OFFSET=os.path.join(self.b, "disk.offset"), REIMU_AGENT_DISK_RUN=self.run_d,
                        REIMU_CONFIGFS=self.cfs, REIMU_UDC_CLASS=self.udc)

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def run_(self, *a, input=""):
        r = subprocess.run(SH + [SCRIPT] + list(a), env=self.env, input=input.encode(), stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=60)
        return r.returncode, r.stdout.decode(), r.stderr.decode()

    def other_gadget(self, name, udc):
        g = os.path.join(self.cfs, "usb_gadget", name)
        os.makedirs(g)
        open(os.path.join(g, "UDC"), "w").write(udc + "\n")

    def g(self, *p):
        return os.path.join(self.cfs, "usb_gadget", "reimu_agent", *p)

    def read(self, *p):
        return open(self.g(*p)).read().strip()

    def test_start(self):
        s = pair_string()
        rc, out, err = self.run_("start", input=s + "\n")
        self.assertEqual(rc, 0, err)
        img = os.path.join(self.run_d, "disk.img")
        self.assertEqual(oct(os.stat(self.run_d).st_mode & 0o777), "0o700")
        r = subprocess.run(["mtype", "-i", img, "::PAIRING.TXT"], stdout=subprocess.PIPE)
        self.assertEqual(r.stdout.split(b"\n")[0].strip().decode(), s, "PAIRING.TXT has the string")
        self.assertEqual(len(r.stdout), 512)
        orig = open(os.path.join(self.b, "disk.img"), "rb").read()
        new = open(img, "rb").read()
        off = int(open(os.path.join(self.b, "disk.offset")).read())
        self.assertEqual(new[:off] + new[off + 512:], orig[:off] + orig[off + 512:], "nothing else of the image changed")
        r = subprocess.run(["mtype", "-i", img, "::install.sh"], stdout=subprocess.PIPE)
        self.assertEqual(r.stdout, open(os.path.join(self.b, "agent", "install.sh"), "rb").read())
        # the gadget
        self.assertEqual(self.read("UDC"), VHUB + ":p3", "p3 first")
        self.assertEqual(self.read("idVendor"), "0x1d6b")
        self.assertEqual(self.read("strings/0x409/product"), "REIMU agent installer")
        f = "functions/mass_storage.agent/"
        self.assertEqual(self.read(f + "lun.0/file"), img)
        self.assertEqual(self.read(f + "lun.0/ro"), "1")
        self.assertEqual(self.read(f + "lun.0/removable"), "1")
        self.assertEqual(self.read(f + "lun.0/cdrom"), "0")
        self.assertEqual(self.read(f + "stall"), "0")
        self.assertEqual(open(self.g(f + "lun.0/inquiry_string")).read(), "REIMU   Agent installer 1.00")
        self.assertTrue(os.path.islink(self.g("configs/c.1/mass_storage.agent")))
        rc, out, err = self.run_("status")
        self.assertEqual(rc, 0)
        self.assertIn("p3", out)
        self.assertNotIn(s, out + err, "the string never printed")

    def test_ports(self):
        self.other_gadget("obmc_hid", VHUB + ":p1")
        self.other_gadget("mass-storage0", VHUB + ":p4")
        self.other_gadget("x", VHUB + ":p3")
        rc, out, err = self.run_("start", input=pair_string() + "\n")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.read("UDC"), VHUB + ":p2", "p3 in use: p2")

    def test_no_port(self):
        self.other_gadget("a", VHUB + ":p2")
        self.other_gadget("b", VHUB + ":p3")
        rc, out, err = self.run_("start", input=pair_string() + "\n")
        self.assertEqual(rc, 1)
        self.assertIn("no free USB port", err)
        self.assertFalse(os.path.exists(os.path.join(self.run_d, "disk.img")), "the image removed again")
        for p in (1, 4, 5):
            self.assertNotIn(":p%d" % p, open(self.g("UDC")).read() if os.path.exists(self.g("UDC")) else "", "never p1/p4/p5")

    def test_other_udc(self):
        """no virtual hub (the test VM): any other free controller"""
        shutil.rmtree(self.udc)
        os.makedirs(os.path.join(self.udc, "dummy_udc.0"))
        rc, out, err = self.run_("start", input=pair_string() + "\n")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.read("UDC"), "dummy_udc.0")

    def test_bad_strings(self):
        for s in ("", "reimu1:", "reimu1:abc$(reboot)", "reimu2:" + "a" * 40, pair_string(32)[:7] + "A" * 600):
            rc, out, err = self.run_("start", input=s + "\n")
            self.assertEqual(rc, 2, s)
            self.assertIn("pairing string", err)
            self.assertFalse(os.path.exists(self.g("UDC")), s)

    def test_damaged_image(self):
        bad = os.path.join(self.d, "bad.img")
        b = bytearray(open(os.path.join(self.b, "disk.img"), "rb").read())
        off = int(open(os.path.join(self.b, "disk.offset")).read())
        b[off:off + 5] = b"XXXXX"
        open(bad, "wb").write(b)
        self.env["REIMU_AGENT_DISK_IMG"] = bad
        rc, out, err = self.run_("start", input=pair_string() + "\n")
        self.assertEqual(rc, 1)
        self.assertIn("placeholder", err)
        self.assertFalse(os.path.exists(self.g("UDC")))

    def test_stop(self):
        rc, out, err = self.run_("start", input=pair_string() + "\n")
        self.assertEqual(rc, 0, err)
        rc, out, err = self.run_("stop")
        self.assertEqual(rc, 0, err)
        self.assertFalse(os.path.exists(self.run_d), "the image with the string gone")
        self.assertTrue(not os.path.exists(self.g("UDC")) or self.read("UDC") == "", "unbound")
        rc, out, err = self.run_("stop")
        self.assertEqual(rc, 0, "twice: " + err)


if __name__ == "__main__":
    unittest.main()
