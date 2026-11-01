#!/usr/bin/env python3
"""Unit tests of reimu-hostd (the host agent) against a fake /sys, /proc, /etc.

    python3 tools/host/tests/test_reimu_hostd.py [-v]

Nothing of the machine it runs on is read or written: every path goes through
the agent's Fs(root) and every command through a recording fake runner.
"""
import gzip
import base64
import time, importlib.machinery, importlib.util, json, os, shutil, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
AGENT = os.path.join(HERE, "..", "reimu-hostd.py")


def load_agent():
    loader = importlib.machinery.SourceFileLoader("reimu_hostd", AGENT)
    spec = importlib.util.spec_from_loader("reimu_hostd", loader)
    m = importlib.util.module_from_spec(spec)
    loader.exec_module(m)
    return m


hd = load_agent()

GPU = "0000:03:00.0"
GPU_DEV = "/sys/devices/pci0000:00/0000:00:01.0/" + GPU


class Tree:
    """a fake root: files, relative symlinks"""

    def __init__(self):
        self.root = tempfile.mkdtemp(prefix="hostd-")

    def p(self, path):
        return self.root + path

    def put(self, path, text):
        f = self.p(path)
        os.makedirs(os.path.dirname(f), exist_ok=True)
        with open(f, "w") as fh:
            fh.write(text if text.endswith("\n") or text == "" else text + "\n")

    def get(self, path):
        with open(self.p(path)) as fh:
            return fh.read().strip()

    def link(self, path, target):
        """path -> target (both absolute in the fake root), as a relative link"""
        f = self.p(path)
        os.makedirs(os.path.dirname(f), exist_ok=True)
        if os.path.lexists(f):
            os.remove(f)
        os.symlink(os.path.relpath(self.p(target), os.path.dirname(f)), f)

    def rm(self, path):
        f = self.p(path)
        if os.path.isdir(f) and not os.path.islink(f):
            shutil.rmtree(f)
        elif os.path.lexists(f):
            os.remove(f)

    def cleanup(self):
        shutil.rmtree(self.root, ignore_errors=True)


def pci_dev(t, slot, parent, vendor, device, cls, driver=None, speed=None, width=None, numa="-1"):
    d = "%s/%s" % (parent, slot)
    t.put(d + "/vendor", "0x" + vendor)
    t.put(d + "/device", "0x" + device)
    t.put(d + "/class", "0x" + cls)
    t.put(d + "/subsystem_vendor", "0x" + vendor)
    t.put(d + "/subsystem_device", "0x0b36")
    t.put(d + "/revision", "0xc1")
    t.put(d + "/numa_node", numa)
    if speed:
        t.put(d + "/current_link_speed", speed)
        t.put(d + "/current_link_width", str(width))
        t.put(d + "/max_link_speed", "16.0 GT/s PCIe")
        t.put(d + "/max_link_width", "16")
    if driver:
        t.put("/sys/bus/pci/drivers/%s/new_id" % driver, "")
        t.link(d + "/driver", "/sys/bus/pci/drivers/" + driver)
    t.link("/sys/bus/pci/devices/" + slot, d)
    return d


def gpu_hwmon(t, n=2, enable=2, pwm=0):
    hw = GPU_DEV + "/hwmon/hwmon%d" % n
    vals = {"name": "amdgpu", "temp1_input": "52000", "temp1_label": "edge", "temp1_crit": "100000",
            "temp2_input": "61000", "temp2_label": "junction", "temp2_crit": "110000",
            "temp3_input": "58000", "temp3_label": "mem", "temp3_crit": "105000",
            "fan1_input": "0", "fan1_min": "0", "fan1_max": "3200", "pwm1": str(pwm), "pwm1_enable": str(enable),
            "pwm1_min": "0", "pwm1_max": "255", "power1_average": "35000000", "power1_cap": "180000000",
            "freq1_input": "800000000", "freq1_label": "sclk", "freq2_input": "875000000", "freq2_label": "mclk"}
    for k, v in vals.items():
        t.put(hw + "/" + k, v)
    t.link(hw + "/device", GPU_DEV)
    t.link("/sys/class/hwmon/hwmon%d" % n, hw)
    return hw


def board(t):
    """the 1Э8СВ-uATX host as seen by the agent: E8C2, 2 NUMA... (one node
    here), EDAC with 2 DIMMs, RX 5700 XT, an NVMe and a SATA disk, 2 NICs"""
    t.put("/proc/cpuinfo", "".join(
        "processor\t: %d\nvendor_id\t: Elbrus-MCST\ncpu family\t: 4\nmodel\t\t: 9\nmodel name\t: E8C2\n"
        "revision\t: 0\ncpu MHz\t\t: 1550.000000\n\n" % i for i in range(8)))
    t.put("/proc/meminfo", "MemTotal:       32864256 kB\nMemFree:        20000000 kB\nMemAvailable:   24000000 kB\n"
          "SwapTotal:       4194300 kB\nSwapFree:        4194300 kB\n")
    t.put("/proc/loadavg", "0.52 0.40 0.31 2/345 6789")
    t.put("/proc/uptime", "3600.50 20000.00")
    t.put("/proc/sys/kernel/osrelease", "6.1.0-3.el-e8c2")
    t.put("/proc/sys/kernel/hostname", "elbrus")
    t.put("/proc/sys/kernel/arch", "e2k")
    t.put("/etc/os-release", 'NAME="Альт Рабочая станция"\nID=altlinux\nVERSION_ID=10.2\nPRETTY_NAME="ALT Workstation 10.2 (e2k)"\n')
    t.put("/sys/devices/system/node/node0/cpulist", "0-7")
    t.put("/sys/devices/system/node/node0/meminfo", "Node 0 MemTotal:       32864256 kB\nNode 0 MemFree: 1 kB\n")
    t.put("/sys/devices/system/node/online", "0")
    # EDAC: one controller, 2 DIMMs, 2 csrows
    mc = "/sys/devices/system/edac/mc/mc0"
    t.put(mc + "/mc_name", "e2k_mc")
    t.put(mc + "/size_mb", "32768")
    t.put(mc + "/ce_count", "3")
    t.put(mc + "/ue_count", "0")
    t.put(mc + "/ce_noinfo_count", "1")
    t.put(mc + "/ue_noinfo_count", "0")
    for i, ce in ((0, 2), (1, 0)):
        t.put(mc + "/dimm%d/dimm_label" % i, "DIMM%d" % i)
        t.put(mc + "/dimm%d/size" % i, "16384")
        t.put(mc + "/dimm%d/dimm_mem_type" % i, "Registered-DDR4")
        t.put(mc + "/dimm%d/dimm_edac_mode" % i, "SECDED")
        t.put(mc + "/dimm%d/dimm_ce_count" % i, str(ce))
        t.put(mc + "/dimm%d/dimm_ue_count" % i, "0")
        t.put(mc + "/csrow%d/ce_count" % i, str(ce))
        t.put(mc + "/csrow%d/ue_count" % i, "0")
        t.put(mc + "/csrow%d/size_mb" % i, "16384")
    # PCIe: the root port, the GPU, its audio, the NIC
    root = "/sys/devices/pci0000:00"
    pci_dev(t, "0000:00:01.0", root, "1fff", "8000", "060400", "pcieport", "8.0 GT/s PCIe", 16, "0")
    pci_dev(t, GPU, root + "/0000:00:01.0", "1002", "731f", "030000", "amdgpu", "8.0 GT/s PCIe", 16)
    for k, v in (("gpu_busy_percent", "7"), ("mem_busy_percent", "3"), ("mem_info_vram_total", str(8 << 30)),
                 ("mem_info_vram_used", str(512 << 20)), ("vbios_version", "113-D1820201-101")):
        t.put(GPU_DEV + "/" + k, v)
    pci_dev(t, "0000:03:00.1", root + "/0000:00:01.0", "1002", "ab38", "040300", "snd_hda_intel", "8.0 GT/s PCIe", 16)
    pci_dev(t, "0000:01:01.1", root, "8086", "4d45", "020000", "l_e1000")
    gpu_hwmon(t)
    # disks: NVMe (serial in sysfs, nvme hwmon), SATA (serial in VPD page 0x80, drivetemp)
    nv = "/sys/devices/pci0000:00/0000:00:02.0/0000:02:00.0/nvme/nvme0"
    t.put(nv + "/model", "Samsung SSD 970 EVO Plus 1TB          ")
    t.put(nv + "/serial", "S4EWNX0R123456      ")
    t.put(nv + "/firmware_rev", "2B2QEXM7")
    t.put(nv + "/nvme0n1/size", str(1953525168))
    t.put(nv + "/nvme0n1/queue/rotational", "0")
    t.link(nv + "/nvme0n1/device", nv)
    t.link("/sys/block/nvme0n1", nv + "/nvme0n1")
    t.put(nv + "/hwmon1/name", "nvme")
    t.put(nv + "/hwmon1/temp1_input", "41850")
    t.put(nv + "/hwmon1/temp1_label", "Composite")
    t.link(nv + "/hwmon1/device", nv)
    t.link("/sys/class/hwmon/hwmon1", nv + "/hwmon1")
    sd = "/sys/devices/pci0000:00/0000:00:03.0/ata1/host0/target0:0:0/0:0:0:0"
    t.put(sd + "/model", "WDC WD40EFRX-68N")
    t.put(sd + "/vendor", "ATA     ")
    with open(t.p(sd) + "/vpd_pg80", "wb") as fh:  # header, then the serial
        fh.write(b"\x00\x80\x00\x14     WD-WCC7K1234567")
    t.put(sd + "/block/sda/size", str(7814037168))
    t.put(sd + "/block/sda/queue/rotational", "1")
    t.link(sd + "/block/sda/device", sd)
    t.link("/sys/block/sda", sd + "/block/sda")
    t.put(sd + "/hwmon/hwmon3/name", "drivetemp")
    t.put(sd + "/hwmon/hwmon3/temp1_input", "35000")
    t.link(sd + "/hwmon/hwmon3/device", sd)
    t.link("/sys/class/hwmon/hwmon3", sd + "/hwmon/hwmon3")
    t.put("/sys/devices/virtual/block/loop0/size", "0")
    t.link("/sys/block/loop0", "/sys/devices/virtual/block/loop0")
    # NICs: f0 down, f1 up (the board's internal LAN to the BMC), lo
    for name, mac, state in (("enp1s1f0", "98:a7:b0:06:0f:60", "down"), ("enp1s1f1", "98:a7:b0:06:0f:61", "up")):
        n = "/sys/devices/pci0000:00/0000:01:01.1/net/" + name
        t.put(n + "/address", mac)
        t.put(n + "/operstate", state)
        t.put(n + "/carrier", "1" if state == "up" else "0")
        t.put(n + "/speed", "1000" if state == "up" else "-1")
        t.put(n + "/mtu", "1500")
        t.put(n + "/type", "1")
        t.link(n + "/device", "/sys/devices/pci0000:00/0000:01:01.1")
        t.link("/sys/class/net/" + name, n)
    t.put("/sys/devices/virtual/net/lo/address", "00:00:00:00:00:00")
    t.put("/sys/devices/virtual/net/lo/operstate", "unknown")
    t.put("/sys/devices/virtual/net/lo/type", "772")
    t.link("/sys/class/net/lo", "/sys/devices/virtual/net/lo")
    usb_dev(t, "4-4", "1d6b", "0107")  # the BMC's virtual hub, autosuspend on (the kernel's default)
    t.put("/proc/net/if_inet6", "fe800000000000009aa7b0fffe060f61 03 40 20 80 enp1s1f1\n"
          "00000000000000000000000000000001 01 80 10 80       lo\n")


class Runner:
    """records commands; answers from a table (argv[0] -> (rc, out))"""

    def __init__(self, table=None):
        self.calls, self.table = [], table or {}

    def __call__(self, argv, timeout=10):
        self.calls.append(list(argv))
        r = self.table.get(argv[0])
        if callable(r):
            return r(argv)
        return r if r else (127, "")


LSPCI = ("Slot:\t0000:03:00.0\nClass:\tVGA compatible controller\nVendor:\tAdvanced Micro Devices, Inc. [AMD/ATI]\n"
         "Device:\tNavi 10 [Radeon RX 5600 OEM/5600 XT / 5700/5700 XT]\nRev:\tc1\n\n"
         "Slot:\t0000:01:01.1\nClass:\tEthernet controller\nVendor:\tIntel Corporation\nDevice:\tEthernet 4d45\n\n")


class Base(unittest.TestCase):
    def setUp(self):
        self.t = Tree()
        board(self.t)
        self.fs = hd.Fs(self.t.root)
        self.run = Runner()

    def tearDown(self):
        self.t.cleanup()


# ------------------------------------------------------------------ inventory
class TestInventory(Base):
    def inv(self):
        self.run.table["lspci"] = (0, LSPCI)
        return hd.Collector(self.fs, self.run, hd.parse_config("")).inventory()

    def test_cpu(self):
        c = self.inv()["cpu"]
        self.assertEqual(c["model"], "E8C2")
        self.assertEqual(c["threads"], 8)
        self.assertEqual(c["mhz"], 1550)
        self.assertEqual(c["numa"], [{"node": 0, "cpus": "0-7", "mem_mib": 32094}])

    def test_memory_and_dimms(self):
        m = self.inv()["memory"]
        self.assertEqual(m["total_mib"], 32094)
        self.assertEqual(m["ecc"], "edac")
        self.assertEqual([d["label"] for d in m["dimms"]], ["DIMM0", "DIMM1"])
        self.assertEqual(m["dimms"][0]["size_mib"], 16384)
        self.assertEqual(m["dimms"][0]["type"], "Registered-DDR4")

    def e2k_edac_board(self):
        """the board's ALT 6.1 e2k_edac: one controller, nothing per DIMM"""
        self.t.rm("/sys/devices/system/edac")
        mc = "/sys/devices/system/edac/mc/mc0"
        for f, v in (("mc_name", "E2K_EDAC_MC.0"), ("size_mb", "0"), ("ce_count", "2"), ("ue_count", "0"),
                     ("ce_noinfo_count", "0"), ("ue_noinfo_count", "0"), ("seconds_since_reset", "512")):
            self.t.put(mc + "/" + f, v)

    def test_memory_e2k_edac_one_controller(self):
        self.e2k_edac_board()
        m = self.inv()["memory"]
        self.assertEqual(m["ecc"], "edac", "a controller registered: ECC present, even with size_mb 0 and no DIMMs")
        self.assertEqual(m["dimms"], [])
        self.assertEqual(m["controllers"], [{"mc": 0, "name": "E2K_EDAC_MC.0", "size_mib": 0}])
        e = hd.Collector(self.fs, self.run, hd.parse_config("")).edac()
        self.assertEqual((e["loaded"], e["ce_total"], [x["name"] for x in e["mcs"]], e["mcs"][0]["dimms"]),
                         (True, 2, ["E2K_EDAC_MC.0"], []))

    def test_inventory_again_when_edac_comes(self):
        """e2k_edac loaded after the agent started (the page's button, or by
        hand): the inventory said "unknown" and was sent again only hourly"""
        self.t.rm("/sys/devices/system/edac")
        self.run.table["lspci"] = (0, LSPCI)

        class Link:
            def __init__(self):
                self.sent = []

            def send(self, m):
                self.sent.append(m)
        ag = hd.Agent(hd.parse_config(""), {}, self.fs, self.run)
        link = Link()
        ag.send_inventory(link, force=True)
        self.assertEqual(link.sent[-1]["data"]["memory"]["ecc"], "unknown")
        ag.after_metrics(link, ag.col.metrics(1.0))
        self.assertEqual(len(link.sent), 1, "nothing changed: no inventory again")
        self.e2k_edac_board()
        ag.after_metrics(link, ag.col.metrics(2.0))
        self.assertEqual(len(link.sent), 2, "EDAC came: the inventory at once")
        self.assertEqual(link.sent[-1]["data"]["memory"]["ecc"], "edac")
        ag.after_metrics(link, ag.col.metrics(3.0))
        self.assertEqual(len(link.sent), 2)

    def test_memory_without_edac(self):
        self.t.rm("/sys/devices/system/edac")
        m = self.inv()["memory"]
        self.assertEqual(m["ecc"], "unknown")
        self.assertEqual(m["dimms"], [])

    def test_pci(self):
        pci = {d["slot"]: d for d in self.inv()["pci"]}
        g = pci[GPU]
        self.assertEqual((g["vendor"], g["device"], g["class"], g["driver"]), ("1002", "731f", "030000", "amdgpu"))
        self.assertEqual(g["link"], {"speed": "8.0 GT/s PCIe", "width": 16, "max_speed": "16.0 GT/s PCIe", "max_width": 16})
        self.assertEqual(g["numa"], -1)
        self.assertEqual(g["name"], "Navi 10 [Radeon RX 5600 OEM/5600 XT / 5700/5700 XT]")
        self.assertEqual(g["vendor_name"], "Advanced Micro Devices, Inc. [AMD/ATI]")
        self.assertNotIn("link", pci["0000:01:01.1"])
        self.assertEqual(pci["0000:00:01.0"]["numa"], 0)

    def test_pci_without_lspci(self):
        pci = {d["slot"]: d for d in hd.Collector(self.fs, self.run, hd.parse_config("")).inventory()["pci"]}
        self.assertNotIn("name", pci[GPU])

    def test_disks(self):
        d = {x["name"]: x for x in self.inv()["disks"]}
        self.assertEqual(sorted(d), ["nvme0n1", "sda"])
        self.assertEqual(d["nvme0n1"]["model"], "Samsung SSD 970 EVO Plus 1TB")
        self.assertEqual(d["nvme0n1"]["serial"], "S4EWNX0R123456")
        self.assertEqual(d["nvme0n1"]["size"], 1953525168 * 512)
        self.assertEqual(d["nvme0n1"]["rotational"], False)
        self.assertEqual(d["sda"]["serial"], "WD-WCC7K1234567")
        self.assertEqual(d["sda"]["rotational"], True)

    def test_nics(self):
        n = {x["name"]: x for x in self.inv()["nics"]}
        self.assertEqual(sorted(n), ["enp1s1f0", "enp1s1f1"])
        self.assertEqual(n["enp1s1f1"]["mac"], "98:a7:b0:06:0f:61")
        self.assertEqual(n["enp1s1f1"]["driver"], "l_e1000")
        self.assertEqual(n["enp1s1f1"]["state"], "up")
        self.assertEqual(n["enp1s1f1"]["speed"], 1000)
        self.assertIn("fe80::9aa7:b0ff:fe06:f61", n["enp1s1f1"]["addrs"])
        self.assertIsNone(n["enp1s1f0"]["speed"])

    def test_os(self):
        o = self.inv()["os"]
        self.assertEqual(o["name"], "ALT Workstation 10.2 (e2k)")
        self.assertEqual(o["kernel"], "6.1.0-3.el-e8c2")
        self.assertEqual(o["arch"], "e2k")
        self.assertEqual(o["boot"], "native-e2k")
        self.assertEqual(o["hostname"], "elbrus")

    def test_gpu(self):
        g = self.inv()["gpu"]
        self.assertEqual(g["slot"], GPU)
        self.assertEqual(g["vram_total"], 8 << 30)
        self.assertEqual(g["vbios"], "113-D1820201-101")

    def test_inventory_is_json(self):
        json.dumps(self.inv())


# ------------------------------------------------------------------ metrics
class TestMetrics(Base):
    def m(self, cfg=""):
        return hd.Collector(self.fs, self.run, hd.parse_config(cfg)).metrics(1000.0)

    def test_load_mem(self):
        m = self.m()
        self.assertEqual(m["load"], [0.52, 0.40, 0.31])
        self.assertEqual(m["mem"]["total_kib"], 32864256)
        self.assertEqual(m["mem"]["avail_kib"], 24000000)
        self.assertEqual(m["uptime"], 3600)

    def test_edac(self):
        e = self.m()["edac"]
        self.assertTrue(e["loaded"])
        self.assertEqual((e["ce_total"], e["ue_total"]), (4, 0))  # 3 + 1 without info
        mc = e["mcs"][0]
        self.assertEqual((mc["mc"], mc["ce"], mc["ue"], mc["ce_noinfo"]), (0, 3, 0, 1))
        self.assertEqual([(d["label"], d["ce"], d["ue"]) for d in mc["dimms"]], [("DIMM0", 2, 0), ("DIMM1", 0, 0)])
        self.assertEqual([(r["csrow"], r["ce"]) for r in mc["csrows"]], [(0, 2), (1, 0)])

    def test_edac_counter_increase(self):
        c = hd.Collector(self.fs, self.run, hd.parse_config(""))
        c.metrics(1)
        self.t.put("/sys/devices/system/edac/mc/mc0/ue_count", "1")
        self.t.put("/sys/devices/system/edac/mc/mc0/dimm1/dimm_ue_count", "1")
        e = c.metrics(2)["edac"]
        self.assertEqual(e["ue_total"], 1)
        self.assertEqual(e["mcs"][0]["dimms"][1]["ue"], 1)

    def test_edac_not_loaded(self):
        self.t.rm("/sys/devices/system/edac")
        e = self.m()["edac"]
        self.assertEqual(e, {"loaded": False, "mcs": [], "ce_total": 0, "ue_total": 0})

    def test_gpu(self):
        g = self.m()["gpu"]
        self.assertEqual((g["temp_edge"], g["temp_junction"], g["temp_mem"]), (52.0, 61.0, 58.0))
        self.assertEqual((g["crit_edge"], g["crit_junction"], g["crit_mem"]), (100.0, 110.0, 105.0))
        self.assertEqual(g["fan_rpm"], 0)
        self.assertEqual((g["pwm"], g["pwm_enable"], g["pwm_pct"]), (0, 2, 0.0))
        self.assertEqual(g["power_w"], 35.0)
        self.assertEqual(g["power_cap_w"], 180.0)
        self.assertEqual((g["sclk_mhz"], g["mclk_mhz"]), (800, 875))
        self.assertEqual((g["busy_pct"], g["mem_busy_pct"]), (7, 3))
        self.assertEqual((g["vram_used"], g["vram_total"]), (512 << 20, 8 << 30))
        self.assertEqual(g["link"], {"speed": "8.0 GT/s PCIe", "width": 16})

    def test_no_gpu(self):
        self.t.rm(GPU_DEV + "/driver")
        self.assertIsNone(self.m()["gpu"])

    def test_hwmon_temps(self):
        temps = {(x["chip"], x["label"]): x["value"] for x in self.m()["temps"]}
        self.assertEqual(temps[("nvme", "Composite")], 41.85)
        self.assertEqual(temps[("amdgpu", "junction")], 61.0)
        self.assertEqual(temps[("drivetemp", "temp1")], 35.0)

    def test_disk_temps(self):
        d = {x["name"]: x for x in self.m()["disks"]}
        self.assertEqual(d["nvme0n1"]["temp"], 41.85)
        self.assertEqual(d["sda"]["temp"], 35.0)
        self.assertIsNone(d["sda"]["smart"])  # no smartctl

    def test_smart(self):
        rep = {"smart_status": {"passed": False}, "temperature": {"current": 47}}
        self.run.table["smartctl"] = lambda argv: (0, json.dumps(rep))
        d = {x["name"]: x for x in self.m()["disks"]}
        self.assertEqual(d["sda"]["smart"], "FAILED")
        self.assertIn(["smartctl", "-j", "-H", "-A", "/dev/sda"], self.run.calls)
        n = len(self.run.calls)
        c = hd.Collector(self.fs, self.run, hd.parse_config(""))
        c.metrics(0)
        c.metrics(60)  # cached: smartctl at most every SMART_SEC
        self.assertEqual(len([x for x in self.run.calls[n:] if x[0] == "smartctl"]), 2)

    def test_smart_off(self):
        self.run.table["smartctl"] = (0, "{}")
        self.m("SMART=no")
        self.assertFalse([c for c in self.run.calls if c[0] == "smartctl"])


# ------------------------------------------------------------------ GPU fan
class TestGpuFan(Base):
    HW = GPU_DEV + "/hwmon/hwmon2"

    def fan(self, floor=30):
        return hd.GpuFan(self.fs, floor=floor)

    def hw(self, k):
        return self.t.get(self.HW + "/" + k)

    def test_manual(self):
        f = self.fan()
        ok, msg = f.set_policy("manual", 50)
        self.assertTrue(ok, msg)
        f.tick(0, True)
        self.assertEqual((self.hw("pwm1_enable"), self.hw("pwm1")), ("1", "128"))

    def test_safety_floor(self):
        f = self.fan()
        ok, msg = f.set_policy("manual", 10)
        self.assertTrue(ok)
        self.assertIn("30", msg)
        f.tick(0, True)
        self.assertEqual(self.hw("pwm1"), "77")  # 30 % of 255
        self.assertEqual(f.state(0)["duty"], 30)

    def test_floor_cannot_be_configured_below_20(self):
        self.assertEqual(hd.parse_config("GPU_FAN_FLOOR=5")["GPU_FAN_FLOOR"], 20)

    def test_bad_mode_and_duty(self):
        f = self.fan()
        self.assertFalse(f.set_policy("rocket", 50)[0])
        self.assertFalse(f.set_policy("manual", "abc")[0])
        self.assertFalse(f.set_policy("manual", 150)[0])
        f.tick(0, True)
        self.assertEqual(self.hw("pwm1_enable"), "2")

    def test_auto(self):
        f = self.fan()
        f.set_policy("manual", 60)
        f.tick(0, True)
        f.set_policy("auto")
        f.tick(1, True)
        self.assertEqual(self.hw("pwm1_enable"), "2")

    def test_reapply_after_gpu_reset(self):
        """the driver resets pwm1_enable to 2 (and the fans stop) after a hang"""
        f = self.fan()
        f.set_policy("manual", 40)
        f.tick(0, True)
        self.t.put(self.HW + "/pwm1_enable", "2")
        self.t.put(self.HW + "/pwm1", "0")
        ev = f.tick(2, True)
        self.assertEqual((self.hw("pwm1_enable"), self.hw("pwm1")), ("1", "102"))
        self.assertEqual([e["kind"] for e in ev], ["gpu_fan_reapplied"])
        self.assertEqual(f.state(2)["reapplied"], 1)

    def test_policy_change_is_not_a_reapply(self):
        """a new duty, turbo on and off: written, but no "re-applied" event"""
        f = self.fan()
        f.set_policy("manual", 40)
        f.tick(0, True)
        f.set_policy("manual", 60)
        ev = f.tick(2, True)
        f.turbo(1, 2)
        ev += f.tick(4, True)
        ev += f.tick(70, True)  # turbo over: back to 60 %
        self.assertEqual(self.hw("pwm1"), "153")
        self.assertEqual(ev, [])
        self.assertEqual(f.state(70)["reapplied"], 0)

    def test_reapply_after_hwmon_renumbered(self):
        f = self.fan()
        f.set_policy("manual", 40)
        f.tick(0, True)
        self.t.rm(self.HW)
        self.t.rm("/sys/class/hwmon/hwmon2")
        gpu_hwmon(self.t, n=5, enable=2, pwm=0)
        ev = f.tick(2, True)
        hw5 = GPU_DEV + "/hwmon/hwmon5"
        self.assertEqual((self.t.get(hw5 + "/pwm1_enable"), self.t.get(hw5 + "/pwm1")), ("1", "102"))
        self.assertTrue(ev)

    def test_gpu_gone_then_back(self):
        f = self.fan()
        f.set_policy("manual", 40)
        f.tick(0, True)
        self.t.rm(GPU_DEV + "/hwmon")
        f.tick(2, True)  # no crash
        self.assertFalse(f.state(2)["present"])
        gpu_hwmon(self.t, n=6)
        ev = f.tick(4, True)
        self.assertEqual(self.t.get(GPU_DEV + "/hwmon/hwmon6/pwm1_enable"), "1")
        self.assertEqual([e["kind"] for e in ev], ["gpu_fan_reapplied"])

    def test_link_lost_returns_to_auto(self):
        f = self.fan()
        f.set_policy("manual", 40)
        f.tick(0, True)
        ev = f.tick(61, False)
        self.assertEqual(self.hw("pwm1_enable"), "2")
        self.assertEqual([e["kind"] for e in ev], ["gpu_fan_auto_link_lost"])
        # and stays so (the driver's) while the link is down
        f.tick(70, False)
        self.assertEqual(self.hw("pwm1_enable"), "2")

    def test_link_lost_restore_retried_after_a_failed_write(self):
        """review: pwm1_enable=2 failing once on link loss was never retried"""
        f = self.fan()
        f.set_policy("manual", 40)
        f.tick(0, True)
        real, fails = f.fs.write, [1]

        def flaky(path, value):
            if path.endswith("pwm1_enable") and fails[0]:
                fails[0] -= 1
                raise OSError(5, "EIO")
            return real(path, value)
        f.fs.write = flaky
        f.tick(61, False)
        self.assertEqual(self.hw("pwm1_enable"), "1")  # the write failed
        f.tick(63, False)
        self.assertEqual(self.hw("pwm1_enable"), "2")  # retried

    def test_link_back_policy_again(self):
        f = self.fan()
        f.set_policy("manual", 40)
        f.tick(0, True)
        f.tick(61, False)
        f.set_policy("manual", 45)  # the BMC sends the policy on reconnect
        f.tick(62, True)
        self.assertEqual((self.hw("pwm1_enable"), self.hw("pwm1")), ("1", "115"))

    def test_restore_auto_on_exit(self):
        f = self.fan()
        f.set_policy("manual", 40)
        f.tick(0, True)
        f.restore_auto("exit")
        self.assertEqual(self.hw("pwm1_enable"), "2")

    def test_restart_finds_its_policy_in_place_then_exit_restores(self):
        """the agent killed (SIGKILL) left manual 40 on the card; the new one
        gets the same policy, writes nothing, and must still hand the fan
        back to the driver when it stops"""
        self.t.put(self.HW + "/pwm1_enable", "1")
        self.t.put(self.HW + "/pwm1", "102")
        f = self.fan()
        f.set_policy("manual", 40)
        f.tick(0, True)
        f.restore_auto("exit")
        self.assertEqual(self.hw("pwm1_enable"), "2")

    def test_restore_auto_leaves_untouched_when_never_manual(self):
        self.t.put(self.HW + "/pwm1_enable", "0")  # someone else's choice
        f = self.fan()
        f.restore_auto("exit")
        self.assertEqual(self.hw("pwm1_enable"), "0")

    def test_turbo(self):
        f = self.fan()
        f.set_policy("manual", 40)
        f.tick(0, True)
        ok, _ = f.turbo(5, 0)
        self.assertTrue(ok)
        f.tick(1, True)
        self.assertEqual((self.hw("pwm1_enable"), self.hw("pwm1")), ("1", "255"))
        self.assertEqual(f.state(1)["mode"], "turbo")
        self.assertEqual(f.state(1)["turbo_left"], 299)
        f.tick(301, True)
        self.assertEqual((self.hw("pwm1_enable"), self.hw("pwm1")), ("1", "102"))
        self.assertEqual(f.state(301)["mode"], "manual")

    def test_turbo_from_auto_back_to_auto(self):
        f = self.fan()
        f.turbo(1, 0)
        f.tick(0, True)
        self.assertEqual(self.hw("pwm1"), "255")
        f.tick(61, True)
        self.assertEqual(self.hw("pwm1_enable"), "2")

    def test_turbo_limits(self):
        f = self.fan()
        self.assertFalse(f.turbo(0, 0)[0])
        self.assertFalse(f.turbo(121, 0)[0])
        self.assertTrue(f.turbo(0, 0, cancel=True)[0])

    def test_turbo_link_lost(self):
        f = self.fan()
        f.turbo(30, 0)
        f.tick(0, True)
        f.tick(65, False)
        self.assertEqual(self.hw("pwm1_enable"), "2")

    def test_no_write_when_already_right(self):
        f = self.fan()
        f.set_policy("manual", 40)
        f.tick(0, True)
        os.chmod(self.t.p(self.HW + "/pwm1"), 0o444)
        try:
            ev = f.tick(2, True)  # nothing to write: no error either
        finally:
            os.chmod(self.t.p(self.HW + "/pwm1"), 0o644)
        self.assertEqual(ev, [])


# ------------------------------------------------------------------ control
class TestControl(Base):
    def ctl(self, cfg=""):
        self.cfg = hd.parse_config(cfg)
        self.fan = hd.GpuFan(self.fs, floor=self.cfg["GPU_FAN_FLOOR"])
        return hd.Controller(self.cfg, self.fan, self.run)

    def test_gpu_fan(self):
        c = self.ctl()
        r = c.handle({"t": "control", "id": 7, "op": "gpu_fan", "mode": "manual", "duty": 55}, 0)
        self.assertEqual((r["t"], r["id"], r["ok"]), ("result", 7, True))
        self.fan.tick(0, True)
        self.assertEqual(self.t.get(GPU_DEV + "/hwmon/hwmon2/pwm1"), "140")

    def test_unknown_op_refused(self):
        r = self.ctl().handle({"t": "control", "id": 1, "op": "exec", "cmd": "rm -rf /"}, 0)
        self.assertFalse(r["ok"])
        self.assertIn("not allowed", r["err"])
        self.assertEqual(self.run.calls, [])

    def test_extra_fields_never_reach_a_command(self):
        c = self.ctl()
        self.run.table["systemctl"] = (0, "")
        r = c.handle({"t": "control", "id": 2, "op": "os_power", "action": "reboot", "argv": ["sh", "-c", "id"],
                      "cmd": "x"}, 0)
        self.assertTrue(r["ok"], r)
        self.assertEqual(self.run.calls, [["systemctl", "reboot"]])

    def test_os_power(self):
        c = self.ctl()
        c.handle({"t": "control", "id": 3, "op": "os_power", "action": "poweroff"}, 0)
        self.assertEqual(self.run.calls, [["systemctl", "poweroff"]])
        r = c.handle({"t": "control", "id": 4, "op": "os_power", "action": "halt; id"}, 0)
        self.assertFalse(r["ok"])
        self.assertEqual(len(self.run.calls), 1)

    def test_os_power_disabled(self):
        c = self.ctl("ALLOW=gpu_fan gpu_turbo")
        r = c.handle({"t": "control", "id": 5, "op": "os_power", "action": "poweroff"}, 0)
        self.assertFalse(r["ok"])
        self.assertIn("disabled", r["err"])
        self.assertEqual(self.run.calls, [])

    # the board's ALT 6.1 kernel ships the driver as e2k_edac.ko (CONFIG_EDAC_E2K):
    # "modprobe edac_e2k" failed there with "Module edac_e2k not found"
    def alt_modprobe(self, argv):
        return (0, "") if argv[1] == "e2k_edac" else (1, "modprobe: FATAL: Module %s not found" % argv[1])

    def diag_ctl(self):
        c = self.ctl()
        c.col = hd.Collector(self.fs, self.run, self.cfg)
        self.run.table["dmesg"] = lambda argv: (0, "kernel line one\n" + "x" * 2000 + "\nkernel line last\n")
        return c

    def report(self, r):
        return json.loads(gzip.decompress(base64.b64decode(r["gz"])).decode())

    def test_diag_report_fixed_set(self):
        r = self.diag_ctl().handle({"t": "control", "id": 8, "op": "diag"}, 0)
        self.assertTrue(r["ok"], r)
        d = self.report(r)
        self.assertEqual(set(d), {"inventory", "kernel_log", "edac", "gpu", "temps", "mem", "uptime", "bmc_usb_hub", "i2c_owner", "agent"})
        self.assertIn("kernel line last", d["kernel_log"])
        self.assertNotIn("gz", r["text"])

    def test_diag_reads_no_shared_bus_and_runs_only_dmesg_and_inventory_tools(self):
        c = self.diag_ctl()
        self.run.calls.clear()
        c.handle({"t": "control", "id": 8, "op": "diag"}, 0)
        self.assertNotIn(["i2cdump"], [x[:1] for x in self.run.calls])
        for argv in self.run.calls:
            self.assertFalse(argv[0].startswith("i2c"), argv)

    def test_diag_size_capped(self):
        c = self.diag_ctl()
        rnd = "".join(chr(33 + (i * 7919) % 90) + str(i) for i in range(400000))
        self.run.table["dmesg"] = lambda argv: (0, rnd)
        r = c.handle({"t": "control", "id": 8, "op": "diag"}, 0)
        self.assertTrue(r["ok"], r)
        self.assertLessEqual(len(r["gz"]), hd.DIAG_MAX_B64)

    def test_diag_disabled_by_allow(self):
        c = self.ctl("ALLOW=gpu_fan")
        r = c.handle({"t": "control", "id": 8, "op": "diag"}, 0)
        self.assertFalse(r["ok"])
        self.assertIn("disabled", r["err"])

    def test_edac_load_real_module_name(self):
        c = self.ctl()
        self.run.table["modprobe"] = self.alt_modprobe
        r = c.handle({"t": "control", "id": 6, "op": "edac_load"}, 0)
        self.assertTrue(r["ok"], r)
        self.assertEqual(self.run.calls[0], ["modprobe", "e2k_edac"])
        self.assertIn("e2k_edac", str(r.get("text")) + str(r.get("err")))

    def test_edac_load_both_names_missing(self):
        c = self.ctl()
        self.run.table["modprobe"] = lambda argv: (1, "modprobe: FATAL: Module %s not found" % argv[1])
        r = c.handle({"t": "control", "id": 6, "op": "edac_load"}, 0)
        self.assertFalse(r["ok"])
        self.assertIn("e2k_edac", r["err"])
        self.assertIn("edac_e2k", r["err"])

    def test_edac_autoload_real_module_name(self):
        self.t.rm("/sys/devices/system/edac")
        self.run.table["modprobe"] = self.alt_modprobe
        hd.edac_autoload(self.fs, self.run, hd.parse_config("EDAC_AUTOLOAD=yes"))
        self.assertIn(["modprobe", "e2k_edac"], self.run.calls)

    def test_edac_load(self):
        c = self.ctl()
        self.run.table["modprobe"] = (0, "")
        r = c.handle({"t": "control", "id": 6, "op": "edac_load"}, 0)
        self.assertTrue(r["ok"])
        self.assertEqual(self.run.calls, [["modprobe", "e2k_edac"]])

    def test_edac_load_failure_reported(self):
        c = self.ctl()
        self.run.table["modprobe"] = (1, "modprobe: FATAL: Module edac_e2k not found")
        r = c.handle({"t": "control", "id": 6, "op": "edac_load"}, 0)
        self.assertFalse(r["ok"])
        self.assertIn("not found", r["err"])

    def test_turbo(self):
        c = self.ctl()
        r = c.handle({"t": "control", "id": 8, "op": "gpu_turbo", "minutes": 10}, 0)
        self.assertTrue(r["ok"])
        self.assertEqual(self.fan.state(0)["mode"], "turbo")

    def test_bad_id_and_types(self):
        c = self.ctl()
        r = c.handle({"t": "control", "id": "x" * 500, "op": "gpu_turbo", "minutes": "ten"}, 0)
        self.assertFalse(r["ok"])
        self.assertLessEqual(len(json.dumps(r)), 300)

    def test_policy_message(self):
        c = self.ctl()
        c.policy({"t": "policy", "gpu_fan": {"mode": "manual", "duty": 35}, "turbo_left": 0}, 0)
        self.fan.tick(0, True)
        self.assertEqual(self.t.get(GPU_DEV + "/hwmon/hwmon2/pwm1"), "89")
        c.policy({"t": "policy", "gpu_fan": {"mode": "manual", "duty": 35}, "turbo_left": 120}, 10)
        self.assertEqual(self.fan.state(10)["turbo_left"], 120)

    def test_policy_respects_allow(self):
        """review: welcome/policy messages bypassed ALLOW"""
        c = self.ctl("ALLOW=os_power edac_load")
        c.policy({"t": "policy", "gpu_fan": {"mode": "manual", "duty": 60}, "turbo_left": 300}, 0)
        self.fan.tick(1, True)
        self.assertEqual(self.t.get(GPU_DEV + "/hwmon/hwmon2/pwm1_enable"), "2")
        self.assertEqual(self.fan.state(1)["mode"], "auto")
        c2 = self.ctl("ALLOW=gpu_fan")  # the duty yes, the turbo no
        c2.policy({"t": "policy", "gpu_fan": {"mode": "manual", "duty": 60}, "turbo_left": 300}, 0)
        self.fan.tick(1, True)
        self.assertEqual(self.t.get(GPU_DEV + "/hwmon/hwmon2/pwm1"), "153")

    def test_edac_autoload(self):
        self.t.rm("/sys/devices/system/edac")
        self.run.table["modprobe"] = (0, "")
        cfg = hd.parse_config("EDAC_AUTOLOAD=yes")
        hd.edac_autoload(self.fs, self.run, cfg)
        self.assertEqual(self.run.calls, [["modprobe", "e2k_edac"]])
        self.run.calls.clear()
        hd.edac_autoload(self.fs, self.run, hd.parse_config(""))
        self.assertEqual(self.run.calls, [])


# ------------------------------------------------------------------ transport
class TestStopRightAfterConnect(Base):
    def test_policy_applied_before_a_stop_is_seen(self):
        """the agent killed left manual 40 on the card; the new one connects and
        gets SIGTERM before its first loop: the fan must still go back to the driver"""
        hw = GPU_DEV + "/hwmon/hwmon2"
        self.t.put(hw + "/pwm1_enable", "1")
        self.t.put(hw + "/pwm1", "102")
        ag = hd.Agent(hd.parse_config(""), {}, self.fs, self.run)
        ag.handshake = lambda link, cand: {"policy": {"gpu_fan": {"mode": "manual", "duty": 40}, "turbo_left": 0}}
        ag.send_inventory = lambda link, force=False: None
        ag.stop = True  # the signal came during the handshake

        class Link:
            def send(self, m):
                pass

        ag.session(Link(), {"addr": "127.0.0.1", "scope": None, "port": 5564, "transport": "ipv4"})
        ag.fan.restore_auto("agent stopped")
        self.assertEqual(self.t.get(hw + "/pwm1_enable"), "2")


class TestCredentials(Base):
    def test_pair_while_running_new_key_used(self):
        """review: a re-pair while the service runs left it on the revoked key"""
        d = tempfile.mkdtemp()
        try:
            sp, cp = os.path.join(d, "secret"), os.path.join(d, "conf")
            hd.save_secret(sp, {"KEY": "aa" * 32, "FINGERPRINT": "bb" * 32, "AGENT_ID": "x"})
            open(cp, "w").write("BMC_IPV4=192.168.10.79\n")
            ag = hd.Agent(hd.load_config(cp), hd.load_secret(sp), self.fs, self.run, secret_path=sp, conf_path=cp)
            ag.revoked = True
            time.sleep(0.01)
            hd.save_secret(sp, {"KEY": "cc" * 32, "FINGERPRINT": "dd" * 32, "AGENT_ID": "x"})
            open(cp, "w").write("BMC_IPV4=10.0.0.9\n")
            os.utime(sp, (time.time() + 2, time.time() + 2))
            self.assertTrue(ag.credentials_changed())
            ag.refresh_credentials()
            self.assertEqual((ag.secret["KEY"], ag.secret["FINGERPRINT"]), ("cc" * 32, "dd" * 32))
            self.assertEqual(ag.cfg["BMC_IPV4"], "10.0.0.9")
            self.assertFalse(ag.revoked)
            self.assertFalse(ag.credentials_changed())
        finally:
            shutil.rmtree(d)


def usb_dev(t, name, vid, pid, control="auto", status="suspended"):
    d = "/sys/devices/platform/ehci/usb4/" + name
    t.put(d + "/idVendor", vid)
    t.put(d + "/idProduct", pid)
    t.put(d + "/power/control", control)
    t.put(d + "/power/runtime_status", status)
    t.link("/sys/bus/usb/devices/" + name, d)
    return d


class TestUsbHub(Base):
    """the BMC's virtual USB hub (1d6b:0107: the KVM keyboard/mouse, virtual
    media) autosuspended by the host when empty is never woken by the BMC (its
    remote wakeup doesn't work on the board): no KVM input after a BMC reboot"""

    def setUp(self):
        super().setUp()
        self.hub = usb_dev(self.t, "4-4", "1d6b", "0107")
        self.root_hub = usb_dev(self.t, "usb4", "1d6b", "0002")

    def test_autosuspend_turned_off(self):
        u = hd.UsbHub(self.fs, hd.parse_config(""))
        ev = u.tick()
        self.assertEqual(self.t.get(self.hub + "/power/control"), "on")
        self.assertEqual([e["kind"] for e in ev], ["bmc_usb_hub_on"])
        self.assertEqual(u.tick(), [], "once")
        self.assertEqual(self.t.get(self.root_hub + "/power/control"), "auto", "other USB devices untouched")

    def test_back_to_auto_fixed_again(self):
        u = hd.UsbHub(self.fs, hd.parse_config(""))
        u.tick()
        self.t.put(self.hub + "/power/control", "auto")  # re-plugged (BMC kexec): the kernel's default
        self.assertTrue(u.tick())
        self.assertEqual(self.t.get(self.hub + "/power/control"), "on")

    def test_not_in_allow(self):
        u = hd.UsbHub(self.fs, hd.parse_config("ALLOW=gpu_fan"))
        self.assertEqual(u.tick(), [])
        self.assertEqual(self.t.get(self.hub + "/power/control"), "auto")

    def test_not_a_bmc_command(self):
        fan = hd.GpuFan(self.fs)
        r = hd.Controller(hd.parse_config(""), fan, self.run).handle({"t": "control", "id": 1, "op": "usb_hub"}, 0)
        self.assertFalse(r["ok"])
        self.assertIn("not allowed", r["err"])

    def test_state_in_metrics_and_inventory(self):
        c = hd.Collector(self.fs, self.run, hd.parse_config(""))
        self.assertEqual(c.metrics(0)["bmc_usb_hub"], {"present": True, "control": "auto", "status": "suspended"})
        self.assertEqual(c.inventory()["bmc_usb_hub"]["present"], True)
        self.t.rm("/sys/bus/usb/devices/4-4")
        self.assertEqual(c.metrics(1)["bmc_usb_hub"], {"present": False, "control": None, "status": None})

    def test_agent_fixes_it_without_the_bmc(self):
        """after a BMC reboot the link is down: the hub is fixed all the same"""
        ag = hd.Agent(hd.parse_config(""), {}, self.fs, self.run)
        ag.local_tick(0, link_ok=False)
        self.assertEqual(self.t.get(self.hub + "/power/control"), "on")

    def test_udev_rule(self):
        self.run.table["udevadm"] = (0, "")
        hd.hub_rule(self.fs, self.run, "install")
        rule = self.t.get("/etc/udev/rules.d/60-reimu-bmc-hub.rules")
        for part in ('SUBSYSTEM=="usb"', 'ATTR{idVendor}=="1d6b"', 'ATTR{idProduct}=="0107"', 'TEST=="power/control"',
                     'ATTR{power/control}="on"'):
            self.assertIn(part, rule)
        self.assertRegex(rule, r'ACTION=="add[|"]')
        self.assertIn(["udevadm", "control", "--reload"], self.run.calls)
        self.assertEqual(self.t.get(self.hub + "/power/control"), "on", "applied to the hub there now")
        hd.hub_rule(self.fs, self.run, "remove")
        self.assertFalse(os.path.exists(self.t.p("/etc/udev/rules.d/60-reimu-bmc-hub.rules")))

    def test_one_source_of_the_rule(self):
        """reimu-host.sh (the KVM side) uses the agent's rule, no copy of its own"""
        text = open(os.path.join(HERE, "..", "reimu-host.sh")).read()
        self.assertNotIn("ATTR{power/control}", text)
        self.assertIn("reimu-hostd.py\" hub-rule install", text)
        self.assertIn("reimu-hostd.py\" hub-rule remove", text)


class KernelFs(hd.Fs):
    """what the kernel does on a write to .../driver/unbind: the device loses
    its driver link; every unbind written down, in order"""

    def __init__(self, root, fail=()):
        super().__init__(root)
        self.unbinds, self.fail = [], set(fail)

    def write(self, path, value):
        if path.endswith("/driver/unbind"):
            if value in self.fail:
                raise OSError(16, "EBUSY")
            self.unbinds.append(value)
            os.remove(self.p(path[:-len("/unbind")]))
            return
        super().write(path, value)


def i2c_client(t, name, driver=None):
    bus = name.split("-")[0]
    d = "/sys/devices/platform/l_i2c/i2c-%s/%s" % (bus, name)
    t.put(d + "/name", driver or "x")
    if driver:
        t.put("/sys/bus/i2c/drivers/%s/unbind" % driver, "")
        t.link(d + "/driver", "/sys/bus/i2c/drivers/" + driver)
    t.link("/sys/bus/i2c/devices/" + name, d)
    return d


def pwm_fan(t, name):
    d = "/sys/devices/platform/" + name
    t.put(d + "/modalias", "platform:pwm-fan")
    t.put("/sys/bus/platform/drivers/pwm-fan/unbind", "")
    t.link(d + "/driver", "/sys/bus/platform/drivers/pwm-fan")
    t.link("/sys/bus/platform/devices/" + name, d)
    return d


def shared_hwmon(t, client, n, chip):
    """a hwmon of an I2C client, in /sys/class/hwmon as the kernel has it"""
    h = client + "/hwmon/hwmon%d" % n
    t.put(h + "/name", chip)
    t.put(h + "/temp1_input", "47000")
    t.link(h + "/device", client)
    t.link("/sys/class/hwmon/hwmon%d" % n, h)


OWNED = ["0-004c", "0-0044", "0-0045", "0-0046", "0-0060", "1-004c", "1-002c", "1-002e", "1-0058", "1-0059"]
DRIVER = {"0-004c": "lm63", "1-004c": "lm63", "1-002c": "emc2305", "1-002e": "emc2305", "0-0060": "pmbus", "1-0058": "pmbus",
          "1-0059": "pmbus", "0-0044": "ir38064", "0-0045": "ir38064", "0-0046": "ir38064"}


class TestBusOwner(Base):
    """the BMC is the only master on the platform I2C buses the host shares:
    both polling put 0x98 in every writable register of lm96163 2-004c"""

    def setUp(self):
        super().setUp()
        self.kfs = KernelFs(self.t.root)
        for n in OWNED:
            i2c_client(self.t, n, DRIVER[n])
        pwm_fan(self.t, "pwm-fan0")
        pwm_fan(self.t, "pwm-fan1")
        self.fru = i2c_client(self.t, "3-0054", "at24")
        self.rtc = i2c_client(self.t, "3-006f", "rtc-ds1307")

    def owner(self, cfg=""):
        return hd.BusOwner(self.kfs, self.run, hd.parse_config(cfg))

    def bound(self, name):
        return os.path.lexists(self.t.p("/sys/bus/i2c/devices/%s/driver" % name))

    def test_unbound_pwm_fan_first(self):
        ev = self.owner().tick()
        self.assertEqual(self.kfs.unbinds[:2], ["pwm-fan0", "pwm-fan1"], "pwm-fan uses the lm63/emc2305 PWMs: first")
        self.assertEqual(sorted(self.kfs.unbinds[2:]), sorted(OWNED))
        self.assertFalse(any(self.bound(n) for n in OWNED))
        self.assertTrue(all(e["kind"] == "bmc_owns_i2c" for e in ev))
        self.assertEqual(len(ev), 12)
        self.assertTrue(any("0-004c" in e["text"] and "lm63" in e["text"] for e in ev))

    def test_nothing_to_do_then_silent(self):
        o = self.owner()
        o.tick()
        n = len(self.kfs.unbinds)
        self.assertEqual(o.tick(), [])
        self.assertEqual(len(self.kfs.unbinds), n)

    def test_other_clients_untouched(self):
        i2c_client(self.t, "0-0050", "at24")  # the same bus, not on the list
        self.owner().tick()
        self.assertTrue(self.bound("3-0054") and self.bound("3-006f") and self.bound("0-0050"))
        self.assertNotIn("3-0054", self.kfs.unbinds)

    def test_not_in_allow(self):
        self.t.put("/proc/4242/comm", "pwmd")
        self.assertEqual(self.owner("ALLOW=gpu_fan usb_hub").tick(), [])
        self.assertEqual(self.kfs.unbinds, [])
        self.assertEqual(self.run.calls, [])

    def test_not_a_bmc_command(self):
        r = hd.Controller(hd.parse_config(""), hd.GpuFan(self.fs), self.run).handle({"t": "control", "id": 1, "op": "bmc_owns_i2c"}, 0)
        self.assertFalse(r["ok"])
        self.assertIn("not allowed", r["err"])

    def test_pwmd_stopped(self):
        self.t.put("/proc/4242/comm", "pwmd")
        self.t.put("/proc/1/comm", "systemd")
        self.run.table["systemctl"] = (0, "")
        ev = self.owner().tick()
        self.assertIn(["systemctl", "stop", "pwmd"], self.run.calls)
        self.assertTrue(any("pwmd" in e["text"] for e in ev))

    def test_pwmd_not_stopping_is_killed_and_told_once(self):
        self.t.put("/proc/4242/comm", "pwmd")
        self.run.table["systemctl"] = (1, "Failed to stop")
        self.run.table["pkill"] = (0, "")
        o = self.owner()
        ev = o.tick()
        self.assertIn(["pkill", "-x", "pwmd"], self.run.calls)
        self.assertEqual(len([e for e in ev if "pwmd" in e["text"]]), 1)
        self.assertEqual([e for e in o.tick() if "pwmd" in e["text"]], [], "still there: no event flood")

    def test_no_pwmd_no_command(self):
        self.owner().tick()
        self.assertEqual(self.run.calls, [])

    def test_host_without_the_devices(self):
        self.t.rm("/sys/bus/i2c")
        self.t.rm("/sys/bus/platform/devices")
        self.assertEqual(self.owner().tick(), [])
        st = hd.bus_owner_state(self.fs, hd.parse_config(""))
        self.assertEqual((st["clean"], st["found"]), (True, []))

    def test_unbind_failure_retried_logged_once(self):
        self.kfs.fail = {"0-004c"}
        msgs = []
        hd.set_logger(msgs.append)
        try:
            o = self.owner()
            o.tick()
            o.tick()
            self.assertEqual(len([m for m in msgs if "0-004c" in m]), 1)
            self.assertTrue(self.bound("0-004c"))
            self.kfs.fail = set()
            ev = o.tick()
            self.assertFalse(self.bound("0-004c"), "tried again")
            self.assertEqual(len(ev), 1)
        finally:
            hd.set_logger(None)

    def test_state_in_inventory_and_metrics(self):
        self.t.put("/proc/4242/comm", "pwmd")
        c = hd.Collector(self.fs, self.run, hd.parse_config(""))
        st = c.metrics(0)["i2c_owner"]
        self.assertFalse(st["clean"])
        self.assertTrue(st["enforced"])
        for x in ("pwm-fan0", "0-004c (lm63)", "1-002e (emc2305)", "pwmd"):
            self.assertIn(x, st["found"])
        self.assertEqual(c.inventory()["i2c_owner"]["clean"], False)
        self.t.rm("/proc/4242")
        self.owner().tick()
        st = c.metrics(1)["i2c_owner"]
        self.assertEqual((st["clean"], st["found"]), (True, []))
        self.assertFalse(hd.bus_owner_state(self.fs, hd.parse_config("ALLOW=gpu_fan"))["enforced"])

    def test_agent_tick_with_the_link_down(self):
        ag = hd.Agent(hd.parse_config(""), {}, self.kfs, self.run)
        ev = ag.local_tick(0, False)
        self.assertEqual(len(self.kfs.unbinds), 12)
        self.assertTrue(any(e["kind"] == "bmc_owns_i2c" for e in ev))

    def test_metrics_do_not_read_the_shared_buses(self):
        d = "/sys/devices/platform/l_i2c/i2c-"
        shared_hwmon(self.t, d + "0/0-004c", 10, "lm63")
        shared_hwmon(self.t, d + "1/1-0058", 11, "pmbus")
        shared_hwmon(self.t, d + "3/3-006f", 12, "rtc")
        chips = {x["chip"] for x in hd.Collector(self.fs, self.run, hd.parse_config("")).temps()}
        self.assertNotIn("lm63", chips)
        self.assertNotIn("pmbus", chips)
        self.assertIn("rtc", chips, "i2c-3 is the host's")
        self.assertIn("amdgpu", chips)
        self.assertIn("nvme", chips)

    # ---- bus-owner install|remove
    CONF = "/etc/modprobe.d/reimu-bmc-owns-i2c.conf"
    STATE = "/etc/reimu-hostd/bus-owner.state"

    def systemd(self, was):
        calls = self.run.table
        calls["systemctl"] = lambda argv: (0 if was == "enabled" else 1, was + "\n") if argv[1] == "is-enabled" else (0, "")
        calls["chkconfig"] = (0, "")

    def test_install_and_remove_enabled(self):
        self.systemd("enabled")
        hd.bus_owner(self.kfs, self.run, "install", hd.parse_config(""))
        conf = self.t.get(self.CONF)
        for m in ("lm63", "emc2305", "pwm_fan", "pmbus", "ir38064"):
            self.assertIn("blacklist %s\n" % m, conf + "\n")
            self.assertIn("install %s /bin/true\n" % m, conf + "\n")
        self.assertIn(["systemctl", "disable", "--now", "pwmd"], self.run.calls)
        self.assertEqual(self.t.get(self.STATE), "pwmd=enabled")
        self.assertEqual(len(self.kfs.unbinds), 12, "at once, too")
        # again (an update), pwmd now disabled: the original state is kept
        self.systemd("disabled")
        hd.bus_owner(self.kfs, self.run, "install", hd.parse_config(""))
        self.assertEqual(self.t.get(self.STATE), "pwmd=enabled")
        n = len(self.kfs.unbinds)
        self.run.calls.clear()
        hd.bus_owner(self.kfs, self.run, "remove", hd.parse_config(""))
        self.assertFalse(os.path.exists(self.t.p(self.CONF)))
        self.assertFalse(os.path.exists(self.t.p(self.STATE)))
        self.assertIn(["systemctl", "enable", "--now", "pwmd"], self.run.calls)
        self.assertEqual(len(self.kfs.unbinds), n, "remove unbinds nothing")

    def test_remove_leaves_pwmd_off_when_it_was_off(self):
        self.systemd("disabled")
        hd.bus_owner(self.kfs, self.run, "install", hd.parse_config(""))
        self.assertEqual(self.t.get(self.STATE), "pwmd=disabled")
        self.run.calls.clear()
        hd.bus_owner(self.kfs, self.run, "remove", hd.parse_config(""))
        self.assertFalse([c for c in self.run.calls if "enable" in c])

    def test_install_no_pwmd(self):
        self.run.table["systemctl"] = (1, "Failed to get unit file state for pwmd.service: No such file or directory\n")
        hd.bus_owner(self.kfs, self.run, "install", hd.parse_config(""))
        self.assertEqual(self.t.get(self.STATE), "pwmd=absent")
        self.assertFalse([c for c in self.run.calls if "disable" in c])
        self.assertTrue(os.path.exists(self.t.p(self.CONF)))

    def test_install_init_script_service(self):
        """a SysV script systemd only generates a unit for: chkconfig has the state"""
        self.systemd("generated")
        self.run.table["chkconfig"] = (0, "")
        hd.bus_owner(self.kfs, self.run, "install", hd.parse_config(""))
        self.assertEqual(self.t.get(self.STATE), "pwmd=generated")
        self.assertIn(["chkconfig", "pwmd", "off"], self.run.calls)
        self.assertIn(["systemctl", "stop", "pwmd"], self.run.calls)
        self.run.calls.clear()
        hd.bus_owner(self.kfs, self.run, "remove", hd.parse_config(""))
        self.assertIn(["chkconfig", "pwmd", "on"], self.run.calls)
        self.assertIn(["systemctl", "start", "pwmd"], self.run.calls)

    def test_install_not_in_allow(self):
        hd.bus_owner(self.kfs, self.run, "install", hd.parse_config("ALLOW=gpu_fan"))
        self.assertFalse(os.path.exists(self.t.p(self.CONF)))
        self.assertEqual(self.kfs.unbinds, [])
        self.assertEqual(self.run.calls, [])

    def test_initramfs_check(self):
        import gzip
        self.t.put("/proc/sys/kernel/osrelease", "6.1.0-e8c2")
        os.makedirs(self.t.p("/boot"))
        with open(self.t.p("/boot/initrd-6.1.0-e8c2.img"), "wb") as f:
            f.write(gzip.compress(b"\0" * 100 + b"lib/modules/6.1.0-e8c2/kernel/drivers/hwmon/lm63.ko.xz\0" + b"ext4.ko\0"))
        r = hd.initramfs_check(self.fs)
        self.assertEqual(r["found"], ["lm63"])
        self.assertIn("initrd-6.1.0-e8c2.img", r["file"])
        with open(self.t.p("/boot/initrd-6.1.0-e8c2.img"), "wb") as f:
            f.write(gzip.compress(b"ext4.ko\0"))
        self.assertEqual(hd.initramfs_check(self.fs)["found"], [])
        with open(self.t.p("/boot/initrd-6.1.0-e8c2.img"), "wb") as f:
            f.write(b"\x28\xb5\x2f\xfd not readable here")  # zstd
        self.assertIsNone(hd.initramfs_check(self.fs)["found"], "unknown, not clean")
        self.t.rm("/boot/initrd-6.1.0-e8c2.img")
        self.assertIsNone(hd.initramfs_check(self.fs)["file"])

    def test_install_says_what_to_do_about_the_initramfs(self):
        import gzip
        self.t.put("/proc/sys/kernel/osrelease", "6.1.0-e8c2")
        os.makedirs(self.t.p("/boot"))
        with open(self.t.p("/boot/initrd-6.1.0-e8c2.img"), "wb") as f:
            f.write(gzip.compress(b"kernel/drivers/hwmon/pmbus/ir38064.ko\0"))
        msgs = []
        hd.set_logger(msgs.append)
        try:
            hd.bus_owner(self.kfs, self.run, "install", hd.parse_config(""))
        finally:
            hd.set_logger(None)
        text = "\n".join(msgs)
        self.assertIn("ir38064", text)
        self.assertIn("make-initrd", text)


class TestTransport(Base):
    def test_link_local_from_mac(self):
        self.assertEqual(hd.ll_from_mac("98:a7:b0:06:0f:7e"), "fe80::9aa7:b0ff:fe06:f7e")
        self.assertEqual(hd.ll_from_mac("00:00:00:00:00:00"), "fe80::200:ff:fe00:0")
        self.assertIsNone(hd.ll_from_mac("zz"))

    def test_candidates_link_local_first(self):
        cfg = hd.parse_config("BMC_MAC=98:a7:b0:06:0f:7e\nBMC_IPV4=192.168.10.79\nBMC_PORT=5564")
        c = hd.candidates(cfg, self.fs, [])
        self.assertEqual([(x["addr"], x["scope"], x["transport"]) for x in c],
                         [("fe80::9aa7:b0ff:fe06:f7e", "enp1s1f1", "ipv6-ll"), ("192.168.10.79", None, "ipv4")])
        self.assertTrue(all(x["port"] == 5564 for x in c))

    def test_candidates_down_interface_skipped_but_listed_iface_kept(self):
        cfg = hd.parse_config("BMC_MAC=98:a7:b0:06:0f:7e\nIFACES=enp1s1f0 enp1s1f1")
        c = hd.candidates(cfg, self.fs, [])
        self.assertEqual([x["scope"] for x in c], ["enp1s1f1"])

    def test_candidates_discovered_and_last_good_first(self):
        cfg = hd.parse_config("BMC_MAC=98:a7:b0:06:0f:7e\nBMC_IPV4=192.168.10.79")
        disc = [{"addr": "fe80::1234", "scope": "enp1s1f1"}]
        c = hd.candidates(cfg, self.fs, disc, last={"addr": "192.168.10.79", "scope": None})
        self.assertEqual([x["addr"] for x in c], ["fe80::9aa7:b0ff:fe06:f7e", "fe80::1234", "192.168.10.79"])

    def test_candidates_explicit_address(self):
        cfg = hd.parse_config("BMC=127.0.0.1:3364")
        c = hd.candidates(cfg, self.fs, [])
        self.assertEqual([(x["addr"], x["port"], x["transport"]) for x in c], [("127.0.0.1", 3364, "ipv4")])

    def test_connect_first_falls_back_to_ipv4(self):
        cfg = hd.parse_config("BMC_MAC=98:a7:b0:06:0f:7e\nBMC_IPV4=192.168.10.79")
        tried = []

        def conn(c):
            tried.append(c["transport"])
            if c["transport"] == "ipv6-ll":
                raise OSError("no route")
            return "sock"

        s, c = hd.connect_first(hd.candidates(cfg, self.fs, []), conn)
        self.assertEqual((s, c["transport"], tried), ("sock", "ipv4", ["ipv6-ll", "ipv4"]))

    def test_ipv4_disappears_stays_on_link_local(self):
        """the cable unplugged: no DHCP, the IPv4 address gone; link-local works"""
        cfg = hd.parse_config("BMC_MAC=98:a7:b0:06:0f:7e\nBMC_IPV4=192.168.10.79")

        def conn(c):
            if c["transport"] == "ipv4":
                raise OSError("network unreachable")
            return "ll"

        s, c = hd.connect_first(hd.candidates(cfg, self.fs, [], last={"addr": "192.168.10.79", "scope": None}), conn)
        self.assertEqual((s, c["transport"], c["scope"]), ("ll", "ipv6-ll", "enp1s1f1"))

    def test_nothing_reachable(self):
        cfg = hd.parse_config("BMC_MAC=98:a7:b0:06:0f:7e")

        def conn(c):
            raise OSError("x")

        self.assertEqual(hd.connect_first(hd.candidates(cfg, self.fs, []), conn), (None, None))

    def test_backoff(self):
        b = hd.Backoff(1, 60)
        self.assertEqual([b.next() for _ in range(8)], [1, 2, 4, 8, 16, 32, 60, 60])
        b.reset()
        self.assertEqual(b.next(), 1)

    def test_discovery_reply_parse(self):
        self.assertEqual(hd.parse_discovery(b"REIMU-HOSTLINK 1 5564 ab12cd"), {"port": 5564, "fp": "ab12cd"})
        self.assertIsNone(hd.parse_discovery(b"garbage"))
        self.assertIsNone(hd.parse_discovery(b"REIMU-HOSTLINK 2 5564 ab"))


# ------------------------------------------------------------------ protocol
class TestProtocol(unittest.TestCase):
    KEY = bytes(range(32))
    N, C = "00112233445566778899aabbccddeeff", "ffeeddccbbaa99887766554433221100"

    def test_auth_vector(self):
        # the same vectors are in the BMC daemon's tests (tests/test_hostlink.c)
        self.assertEqual(hd.auth_mac(self.KEY, self.N, self.C, "host-1"),
                         "8a27b644054d9c44033e6af61345fd572d3ef9a90cab02e2c8bf2a0cb49cbaf5")
        self.assertEqual(hd.server_mac(self.KEY, self.C, self.N),
                         "9100cb9d44c8c48187b343cefa62d490437e319e5973e7e7c03b89c8ac34df6e")

    def test_parse_line(self):
        self.assertEqual(hd.parse_line(b'{"v":1,"t":"ping","future":[1,2]}\n'), {"v": 1, "t": "ping", "future": [1, 2]})
        self.assertIsNone(hd.parse_line(b"not json\n"))
        self.assertIsNone(hd.parse_line(b"[1,2]\n"))
        self.assertIsNone(hd.parse_line(b'{"t":"ping"}\n'))  # no version
        with self.assertRaises(hd.VersionError):
            hd.parse_line(b'{"v":2,"t":"ping"}\n')

    def test_encode(self):
        b = hd.encode({"t": "pong"})
        self.assertTrue(b.endswith(b"\n"))
        self.assertEqual(json.loads(b), {"v": 1, "t": "pong"})

    def test_pairing_string(self):
        s = hd.make_pair_string({"token": "tok", "fp": "ab" * 32, "mac": "98:a7:b0:06:0f:7e", "v4": "192.168.10.79",
                                 "port": 5564})
        p = hd.parse_pair_string(s)
        self.assertEqual(p["token"], "tok")
        self.assertEqual(p["mac"], "98:a7:b0:06:0f:7e")
        self.assertIsNone(hd.parse_pair_string("reimu1:@@@"))

    def test_secret_file_root_only(self):
        d = tempfile.mkdtemp()
        try:
            f = os.path.join(d, "secret")
            hd.save_secret(f, {"KEY": "aa" * 32, "FINGERPRINT": "bb" * 32, "AGENT_ID": "x"})
            self.assertEqual(os.stat(f).st_mode & 0o777, 0o600)
            self.assertEqual(hd.load_secret(f)["KEY"], "aa" * 32)
        finally:
            shutil.rmtree(d)

    def test_log_never_prints_the_key(self):
        msgs = []
        hd.set_logger(msgs.append)
        try:
            hd.log("auth with KEY=%s" % ("aa" * 32))
        finally:
            hd.set_logger(None)
        self.assertNotIn("aa" * 32, msgs[0])

    def test_config_defaults(self):
        c = hd.parse_config("# nothing\nUNKNOWN_KEY=1\n")
        self.assertEqual((c["INTERVAL"], c["GPU_FAN_FLOOR"], c["LINK_LOST_SEC"], c["BMC_PORT"]), (10, 30, 60, 5564))
        self.assertEqual(c["ALLOW"], {"gpu_fan", "gpu_turbo", "os_power", "edac_load", "diag", "usb_hub", "bmc_owns_i2c"})
        self.assertEqual(hd.parse_config("INTERVAL=0")["INTERVAL"], 2)


if __name__ == "__main__":
    unittest.main()
