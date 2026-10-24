#!/usr/bin/env python3
"""The installer's USB disk (reimu-agent-disk) in the HID test VM, the VM being
the USB host (run-agentdisk.sh copies the pieces to /mnt/ad):

    agentdiskscen.py /mnt/ad

The script runs under busybox sh as on the BMC, bound to a dummy_udc (no
virtual hub here). On the host side: the SCSI disk (INQUIRY, read-only,
removable), the FAT label REIMU-AGENT, its files, PAIRING.TXT with the string;
install.sh run from the mounted disk into a prefix (its files from the disk,
checked; the pairing fails: no BMC here, rc 4); the disk taken away while
install.sh runs from it (the shell has read all of it: no error); stop: the
gadget gone from configfs, the device from the host.
"""
import base64, glob, json, os, re, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kvmvm as K  # noqa: E402

AD = sys.argv[1] if len(sys.argv) > 1 else "/mnt/ad"
G = "/sys/kernel/config/usb_gadget/reimu_agent"
MNT = "/tmp/reimu-agent-mnt"
failed = []


def check(cond, what):
    print(("  PASS " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failed.append(what)
    return cond


def pair_string():
    d = {"token": "5" * 32, "fp": "d" * 64, "mac": "02:00:00:00:00:99", "v4": "127.0.0.1", "port": 9}
    return "reimu1:" + base64.urlsafe_b64encode(json.dumps(d, separators=(",", ":")).encode()).decode().rstrip("=")


def disk(action, stdin=""):
    env = dict(os.environ, PATH=K.busybox_path(), REIMU_AGENT_DISK_IMG=AD + "/disk.img", REIMU_AGENT_DISK_OFFSET=AD + "/disk.offset")
    r = subprocess.run(["/usr/bin/busybox", "sh", AD + "/reimu-agent-disk", action], input=stdin, capture_output=True, text=True, env=env)
    return r.returncode, r.stdout + r.stderr


def host_disk(timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        for sg in glob.glob("/sys/class/scsi_generic/*"):
            try:
                model = open(sg + "/device/model").read().strip()
            except OSError:
                continue
            if model == "Agent installer":
                blk = glob.glob(sg + "/device/block/*")
                if blk:
                    return "/dev/" + os.path.basename(sg), os.path.basename(blk[0])
        time.sleep(0.3)
    return None, None


def main():
    disk("stop")
    K.sh("umount %s" % MNT, check=False)
    s = pair_string()
    rc, out = disk("start", s + "\n")
    check(rc == 0, "start: %s" % out.strip())
    udc = open(G + "/UDC").read().strip() if os.path.exists(G + "/UDC") else ""
    check(udc.startswith("dummy_udc."), "bound to %s" % udc)
    sg, blk = host_disk()
    if not check(sg is not None, "the host sees the disk (%s %s)" % (sg, blk)):
        print(K.sh("dmesg | tail -20", check=False).stdout)
        disk("stop")
        sys.exit(1)
    q = K.sh("sg_inq %s" % sg, check=False).stdout
    check("REIMU" in q and "Agent installer" in q and "RMB=1" in q, "INQUIRY: REIMU, Agent installer, removable (%s)" % (
        " ".join(l.strip() for l in q.splitlines() if "identification" in l or "RMB" in l)))
    check(open("/sys/block/%s/ro" % blk).read().strip() == "1", "read-only on the host")
    check(open("/sys/block/%s/removable" % blk).read().strip() == "1", "removable")
    lab = K.sh("blkid -o value -s LABEL /dev/%s" % blk, check=False).stdout.strip()
    check(lab == "REIMU-AGENT", "FAT label %s" % lab)
    os.makedirs(MNT, exist_ok=True)
    r = K.sh("mount -o ro /dev/%s %s" % (blk, MNT), check=False)
    check(r.returncode == 0, "mount -o ro: %s" % r.stderr.strip())
    names = sorted(os.listdir(MNT))
    check(names == sorted(["PAIRING.TXT", "README.txt", "install.sh", "reimu-hostd.conf", "reimu-hostd.py", "reimu-hostd.service"]),
          "files: %s" % names)
    check(open(MNT + "/PAIRING.TXT").read().split("\n")[0].strip() == s, "PAIRING.TXT: the pairing string")
    check(open(MNT + "/install.sh", "rb").read() == open(AD + "/agent/install.sh", "rb").read(), "install.sh as the BMC serves it")
    # install.sh from the disk into a prefix: its files from the disk, checked
    root = "/tmp/reimu-agent-root"
    K.sh("rm -rf %s" % root)
    env = dict(os.environ, REIMU_ROOT=root, REIMU_SYSTEMCTL="/bin/true", PATH="/usr/bin:/bin:/usr/sbin:/sbin")
    r = subprocess.run(["sh", MNT + "/install.sh"], capture_output=True, text=True, env=env, timeout=120)
    out = r.stdout + r.stderr
    check("взяты из " + MNT in out, "the files from the disk (%s)" % out.strip().splitlines()[0] if out.strip() else "")
    lib = root + "/usr/local/lib/reimu/reimu-hostd.py"
    check(os.path.exists(lib) and open(lib, "rb").read() == open(AD + "/agent/reimu-hostd.py", "rb").read(), "the agent installed, checked")
    check(r.returncode == 4 and "pairing refused or the BMC not reachable" in out, "the pairing tried with PAIRING.TXT's string (no BMC here: rc %d)" % r.returncode)
    # taken away while install.sh runs from it, as the BMC does: once the
    # pairing is on (the files are copied off by then; the shell read all of
    # install.sh before running any of it)
    K.sh("rm -rf %s" % root)
    p = subprocess.Popen(["sh", MNT + "/install.sh"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    out = ""
    while "pairing with the BMC" not in out:
        l = p.stdout.readline()
        if not l:
            break
        out += l
    rc, o2 = disk("stop")
    check(rc == 0, "stop while it runs (pairing)")
    out += p.communicate(timeout=120)[0]
    check(p.returncode == 4 and not re.search(r"Syntax error|I/O error|not found|No such file", out),
          "install.sh ran on without its disk (rc %d): %s" % (p.returncode, out.strip()[-300:]))
    K.sh("umount -l %s" % MNT, check=False)
    check(not os.path.exists(G), "configfs: the gadget gone")
    check(not os.path.exists("/run/reimu-agent-disk"), "the image with the string gone")
    time.sleep(1)
    check(host_disk(timeout=2)[0] is None, "the host: the disk gone")
    print("%d failed" % len(failed))
    sys.exit(min(len(failed), 100))


if __name__ == "__main__":
    main()
