#!/usr/bin/env python3
"""The host agent's first install from the BMC, the A/B image in QEMU: the
bundle bmcweb serves under /agent/, the pin of bmcweb's key that Pair()
gives the page, the page's command run on this machine (install.sh into a
temporary prefix, no root) against QEMU's forwards until reimu-hostlink in the
VM has the agent paired and connected; the installer's USB disk on the BMC's
side (QEMU has no USB device controller: the error, the configfs rolled back,
the image).

    test_agentux_qemu.py WORKDIR [TEST...]

WORKDIR, RQ, AB_SSH, AB_HTTPS as for test_ab_qemu.py; the link's forward as
for test_hostlink_qemu.py (AB_NETDEV_EXTRA=",hostfwd=tcp:127.0.0.1:PORT-:5564",
HL_PORT=PORT); AUX_E2E=DIR (node_modules/playwright there): the browser test
../e2e/agent-install.qemu.mjs first, and its commands and its downloaded
installer are the ones run. About 8 minutes:

    RQ=/tmp/rqux AB_SSH=3422 AB_HTTPS=3443 HL_PORT=3464 \\
    AB_NETDEV_EXTRA=",hostfwd=tcp:127.0.0.1:3464-:5564" ./test_agentux_qemu.py WORKDIR

Tests (all by default, one boot): files pin web (the install flow, the page's
layout with the board's data) install linklocal selfcontained disk
"""
import base64, hashlib, http.server, json, os, re, shutil, socket, ssl, subprocess, sys, tempfile, threading, time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ab_qemu as ab  # noqa: E402  (takes WORKDIR from sys.argv[1])
from test_ab_qemu import check, failed, fresh, redfish  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
HL_PORT = int(os.environ.get("HL_PORT", 3364))
OBJ = "/xyz/openbmc_project/reimu/hostlink"
FILES = ("reimu-hostd.py", "reimu-hostd.service", "reimu-hostd.conf")
ONLY = ab.ONLY


def find_tools():
    for d in (os.environ.get("HOSTD_TOOLS", ""), os.path.join(HERE, "..", "..", "tools", "host"), os.path.join(HERE, "host")):
        if d and os.path.exists(os.path.join(d, "reimu-hostd.py")):
            return os.path.abspath(d)
    sys.exit("reimu-hostd.py not found (HOSTD_TOOLS=dir)")


TOOLS = find_tools()
sys.path.insert(0, os.path.join(TOOLS, "tests"))
import test_reimu_hostd as th  # noqa: E402  (the fake board)

BASE = "https://127.0.0.1:%d" % ab.HTTPS
CMDS = {}


def wait(fn, timeout, step=1.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            v = fn()
        except Exception:
            v = None
        if v:
            return v
        time.sleep(step)
    return None


def get(path, auth=False):
    ctx = ssl.create_default_context()
    ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE
    rq = urllib.request.Request(BASE + path, headers={"Accept-Encoding": "identity"})
    try:
        with urllib.request.urlopen(rq, context=ctx, timeout=60) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def state():
    s, j = redfish("GET", OBJ)
    return json.loads(j["data"]["Json"]) if s == 200 else None


def action(method, *args):
    s, j = redfish("POST", "%s/action/%s" % (OBJ, method), {"data": list(args)})
    return s, (j or {}).get("data") if isinstance(j, dict) else j


def tls_pin(host, port):
    """curl's --pinnedpubkey of what the server shows (openssl here or in the build container)"""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE
    with ctx.wrap_socket(socket.create_connection((host, port), timeout=20)) as s:
        pem = ssl.DER_cert_to_PEM_cert(s.getpeercert(binary_form=True))
    pipe = "openssl x509 -pubkey -noout | openssl pkey -pubin -outform der | openssl dgst -sha256 -binary | base64"
    r = subprocess.run(["sh", "-c", pipe], input=pem.encode(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode or not r.stdout.strip():
        r = subprocess.run(["docker", "exec", "-i", "reimu", "sh", "-c", pipe], input=pem.encode(), stdout=subprocess.PIPE)
    return "sha256//" + r.stdout.decode().strip()


class Prefix:
    """a host's root for install.sh (REIMU_ROOT) with fake systemctl/journalctl,
    the agent's config pointing at the QEMU forward of the link"""

    def __init__(self):
        self.root = tempfile.mkdtemp(prefix="aux-root-")
        self.bin = os.path.join(self.root, "fakebin")
        os.makedirs(self.bin)
        for n, body in (("systemctl", 'echo "$*" >> "$REIMU_ROOT/systemctl.log"; [ "$1" = is-active ] && echo active; exit 0'),
                        ("journalctl", 'cat "$REIMU_ROOT/agent.log" 2>/dev/null')):
            p = os.path.join(self.bin, n)
            open(p, "w").write("#!/bin/sh\n" + body + "\n")
            os.chmod(p, 0o755)
        os.makedirs(os.path.join(self.root, "etc"))
        self.conf = os.path.join(self.root, "etc/reimu-hostd.conf")
        open(self.conf, "w").write("# QEMU: the link through the forward\nBMC=127.0.0.1:%d\nDISCOVERY=no\nINTERVAL=3\n" % HL_PORT)
        self.secret = os.path.join(self.root, "etc/reimu-hostd/secret")
        self.lib = os.path.join(self.root, "usr/local/lib/reimu/reimu-hostd.py")
        self.env = dict(os.environ, REIMU_ROOT=self.root, REIMU_SYSTEMCTL=os.path.join(self.bin, "systemctl"),
                        REIMU_JOURNALCTL=os.path.join(self.bin, "journalctl"))
        self.agent = None

    def sh(self, cmd, timeout=180):
        r = subprocess.run(["sh", "-c", cmd], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        return r.returncode, r.stdout.decode(errors="replace")

    def run_agent(self):
        """what systemctl restart would start: the installed agent, its /sys a fake board"""
        t = th.Tree()
        th.board(t)
        self.tree = t
        env = dict(os.environ, REIMU_HOSTD_ROOT=t.root, REIMU_HOSTD_CONF=self.conf, REIMU_HOSTD_SECRET=self.secret)
        self.agent = subprocess.Popen([sys.executable, self.lib, "run"], env=env, stdout=open(os.path.join(self.root, "agent.log"), "a"),
                                      stderr=subprocess.STDOUT)

    def systemctl(self):
        try:
            return open(os.path.join(self.root, "systemctl.log")).read()
        except OSError:
            return ""

    def cleanup(self):
        if self.agent:
            self.agent.terminate()
            self.agent.wait(20)
            self.tree.cleanup()
        shutil.rmtree(self.root, ignore_errors=True)


def as_here(cmd):
    """the page's command for this machine: the BMC through the forward, no
    sudo (REIMU_ROOT), install.sh told where the BMC is (the pairing string has
    10.0.2.15 and the VM's link-local)"""
    c = re.sub(r"https://[0-9.]+/agent/", BASE + "/agent/", cmd)
    c = c.replace("| sudo sh -s -- ", "| sh -s -- --url %s " % BASE)
    return c


# ------------------------------------------------------------------ tests
def t_files(b):
    print("== files", flush=True)
    s, hd, sh = get("/agent/install.sh")
    check(s == 200, "GET /agent/install.sh without a session: %s" % s)
    check("gzip" not in hd.get("Content-Encoding", ""), "served plain (curl | sh): %s" % hd.get("Content-Encoding"))
    text = sh.decode()
    check(text.startswith("#!/bin/sh") and "@SHA256" + "SUMS@" not in text, "install.sh with its hashes written in")
    s, _, sums = get("/agent/SHA256SUMS")
    sums = dict(reversed(l.split()) for l in sums.decode().splitlines() if l.strip()) if s == 200 else {}
    for f in FILES:
        s, _, body = get("/agent/" + f)
        h = hashlib.sha256(body).hexdigest()
        check(s == 200 and h == sums.get(f) and ("%s  %s" % (h, f)) in text, "%s: served, its SHA-256 in SHA256SUMS and install.sh" % f)
        check(body == open(os.path.join(TOOLS, f), "rb").read(), "%s: the tree's" % f)
    s, _, _ = get("/agent/README.txt")
    check(s == 200, "README.txt")
    s, _, _ = get("/agent/../xyz/openbmc_project/reimu/hostlink")
    check(s in (400, 401, 403, 404), "nothing else without a session (%s)" % s)
    r = b.out("ls -l /usr/share/reimu-agent/ /usr/libexec/reimu-agent-disk; cat /usr/share/reimu-agent/disk.offset")
    check("disk.img" in r and "reimu-agent-disk" in r, "the disk image and its helper in the image:\n" + r.strip())


def t_pin(b):
    print("== pin", flush=True)
    s, data = action("Pair")
    p = json.loads(data) if s == 200 else {}
    want = tls_pin("127.0.0.1", ab.HTTPS)
    check(p.get("https_pin") == want, "Pair() gives the pin of bmcweb's key: %s (TLS: %s)" % (p.get("https_pin"), want))
    on = b.out("openssl x509 -in /etc/ssl/certs/https/server.pem -noout -subject 2>&1 | head -1").strip()
    print("    bmcweb's certificate: %s" % on)
    check(p.get("link_local", "").startswith("fe80::") and p.get("ipv4"), "link-local %s and IPv4 %s for the commands" % (
        p.get("link_local"), p.get("ipv4")))
    return p


def t_web(b):
    d = os.environ.get("AUX_E2E")
    if not d:
        print("== web: AUX_E2E not set, the commands made here as the page makes them", flush=True)
        return
    print("== web (browser)", flush=True)
    for f in ("agent-install.qemu.mjs", "common.mjs"):
        src = next(p for p in (os.path.join(HERE, "..", "e2e", f), os.path.join(HERE, f)) if os.path.exists(p))
        shutil.copy(src, os.path.join(d, f))
    out = os.path.join(ab.W, "shots")
    r = subprocess.run(["node", "agent-install.qemu.mjs"], cwd=d, env=dict(os.environ, BASE=BASE, OUT=out, AGENT_DISK="on" if b.out("ls /sys/class/udc").strip() else "error"),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
    o = r.stdout.decode()
    for l in o.splitlines():
        if l.startswith(("PASS", "FAIL", "INFO")):
            print("    " + l)
    check(r.returncode == 0, "browser test of the agent install on the «Хост» page (screenshots in WORKDIR/shots)")
    if r.returncode:
        print(o[-1500:])
    print("== web: the «Хост» page's layout with the board's data (host-layout.qemu.mjs)", flush=True)
    shutil.copy(next(p for p in (os.path.join(HERE, "..", "e2e", "host-layout.qemu.mjs"), os.path.join(HERE, "host-layout.qemu.mjs"))
                     if os.path.exists(p)), os.path.join(d, "host-layout.qemu.mjs"))
    r2 = subprocess.run(["node", "host-layout.qemu.mjs"], cwd=d, env=dict(os.environ, BASE=BASE, OUT=out, TAG="image"),
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
    o2 = r2.stdout.decode()
    for l in o2.splitlines():
        if l.startswith("FAIL"):
            print("    " + l)
    check(r2.returncode == 0, "the page's layout at 1400, 768, 390 px: %d checks passed" % o2.count("PASS"))
    try:
        CMDS.update(json.load(open(os.path.join(out, "agent-commands.json"))))
        CMDS["installer"] = os.path.join(out, "reimu-agent-install.sh")
    except (OSError, ValueError):
        pass


def page_commands(p):
    """as js/pages/host.js makes them (when the browser test didn't run)"""
    v4 = ("s=$(curl -fsS --pinnedpubkey '%s' -k https://%s/agent/install.sh) && printf '%%s\\n' \"$s\" | sudo sh -s -- "
          "--pair '%s'" % (p["https_pin"], p["ipv4"], p["pair"]))
    ll = ("s=; for i in $(ls /sys/class/net); do s=$(curl -fsSg --connect-timeout 3 --pinnedpubkey '%s' -k \"https://[%s%%25$i]/agent/install.sh\" "
          "2>/dev/null) && break; done; if [ -n \"$s\" ]; then printf '%%s\\n' \"$s\" | sudo sh -s -- --pair '%s'; else echo "
          "'BMC не найден по link-local / BMC not found over link-local' >&2; fi" % (p["https_pin"], p["link_local"], p["pair"]))
    return {"v4": v4, "ll": ll, "pair": p["pair"], "pin": p["https_pin"]}


def t_install(b, p):
    print("== install (the page's IPv4 command, here)", flush=True)
    if not CMDS:
        CMDS.update(page_commands(p))
    else:
        # the page's own string went with its last click (the USB disk: a new pairing)
        s, data = action("Pair")
        new = json.loads(data)["pair"]
        for k in ("v4", "ll"):
            CMDS[k] = CMDS[k].replace(CMDS["pair"], new)
        CMDS["pair"] = new
    # a wrong pin: refused before anything is fetched
    x = Prefix()
    try:
        bad = as_here(CMDS["v4"]).replace(CMDS["pin"], "sha256//" + "A" * 43 + "=")
        rc, out = x.sh(bad)
        check(rc != 0 and not os.path.exists(x.lib), "a wrong pin: refused, nothing installed (rc %d: %s)" % (rc, out.strip()[-120:]))
    finally:
        x.cleanup()
    x = Prefix()
    try:
        rc, out = x.sh(as_here(CMDS["v4"]))
        print("    " + out.strip().replace("\n", "\n    ")[-1500:])
        check(rc == 0, "the command: rc %d" % rc)
        check(os.path.exists(x.lib) and open(x.lib, "rb").read() == open(os.path.join(TOOLS, "reimu-hostd.py"), "rb").read(),
              "the agent installed in the prefix")
        check("KEY=" in (open(x.secret).read() if os.path.exists(x.secret) else ""), "paired: the key in etc/reimu-hostd/secret")
        st = state()
        check(st and st["paired"], "the BMC: paired")
        check("enable reimu-hostd.service" in x.systemctl() and "restart reimu-hostd.service" in x.systemctl(), "the service enabled, started")
        x.run_agent()
        st = wait(lambda: (lambda s: s if s["agent"]["connected"] and s["inventory"] else None)(state()), 60)
        check(st, "the installed agent connected to reimu-hostlink in QEMU")
        if st:
            check(st["inventory"]["cpu"]["model"] == "E8C2", "its inventory (E8C2)")
        rc, out = x.sh("printf '%%s\\n' \"$(curl -fsS -k %s/agent/install.sh)\" | sh -s -- --uninstall" % BASE)
        check(rc == 0 and not os.path.exists(x.lib) and not os.path.exists(x.secret), "--uninstall: removed")
    finally:
        x.cleanup()


def t_linklocal():
    """the link-local command's curl part on this machine: [fe80::1%lo0], a
    local copy of the bundle served with bmcweb's... own key here: the pin of
    the local server's key in its place (what's tested: the zone in the URL,
    -g, the loop, install.sh's downloads over a zone)"""
    print("== linklocal (fe80::1%lo0 here)", flush=True)
    if not os.path.exists("/sbin/ifconfig") or "fe80::1%lo0" not in subprocess.run(["ifconfig", "lo0"], stdout=subprocess.PIPE).stdout.decode():
        print("  INFO no fe80::1%lo0 here: skipped")
        return
    d = tempfile.mkdtemp(prefix="aux-ll-")
    try:
        os.makedirs(os.path.join(d, "www", "agent"))
        for f in FILES + ("install.sh", "SHA256SUMS"):
            open(os.path.join(d, "www", "agent", f), "wb").write(get("/agent/" + f)[2])
        key, crt = os.path.join(d, "k.pem"), os.path.join(d, "c.pem")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", crt, "-days", "1",
                        "-subj", "/CN=ll"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
        www = os.path.join(d, "www")

        class H(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *a, **kw):
                super().__init__(*a, directory=www, **kw)

            def log_message(self, *a):
                pass

        class S6(http.server.ThreadingHTTPServer):
            address_family = socket.AF_INET6

        idx = socket.if_nametoindex("lo0")
        srv = S6(("fe80::1", 0, 0, idx), H)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(crt, key)
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
        port = srv.server_address[1]
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        pin = tls_pin_ll(port, idx)
        # the bundle's install.sh pairs nothing here: --print-urls stops after the fetch would start;
        # the download path: install.sh --url over the zone, curl and python3
        s, data = action("Pair")
        p = json.loads(data)
        ll = page_commands(dict(p, https_pin=pin, link_local="fe80::1"))["ll"]
        ll = ll.replace("$(ls /sys/class/net)", "en0 lo0").replace("]/agent/", "]:%d/agent/" % port)
        ll = ll.replace("| sudo sh -s -- ", "| sh -s -- --url 'https://[fe80::1%%25lo0]:%d' " % port)
        x = Prefix()
        try:
            rc, out = x.sh(ll)
            check(rc == 0 and os.path.exists(x.lib), "the link-local loop (en0 fails, lo0 works), the downloads over the zone: rc %d %s" % (
                rc, out.strip()[-300:]))
            check(state()["paired"], "paired (the link through the forward)")
            path = "/usr/bin:/bin:/usr/sbin:/sbin"
            x.env["PATH"] = os.path.join(d, "nocurl") + ":" + path
            os.makedirs(os.path.join(d, "nocurl"))
            os.symlink("/usr/bin/false", os.path.join(d, "nocurl", "curl"))
            s, data = action("Pair")
            rc, out = x.sh("sh %s/www/agent/install.sh --url 'https://[fe80::1%%25lo0]:%d' --pair '%s'" % (d, port, json.loads(data)["pair"]))
            check(rc == 0 and "python3" in out, "python3 as the downloader over the zone (curl failing): %s" % out.strip()[-200:])
        finally:
            x.cleanup()
            srv.shutdown()
    finally:
        shutil.rmtree(d, ignore_errors=True)


def tls_pin_ll(port, idx):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname, ctx.verify_mode = False, ssl.CERT_NONE
    s = socket.socket(socket.AF_INET6)
    s.connect(("fe80::1", port, 0, idx))
    with ctx.wrap_socket(s) as t:
        pem = ssl.DER_cert_to_PEM_cert(t.getpeercert(binary_form=True))
    r = subprocess.run(["sh", "-c", "openssl x509 -pubkey -noout | openssl pkey -pubin -outform der | openssl dgst -sha256 -binary | base64"],
                       input=pem.encode(), stdout=subprocess.PIPE)
    return "sha256//" + r.stdout.decode().strip()


def t_selfcontained():
    print("== selfcontained (the page's «Скачать установщик»)", flush=True)
    f = CMDS.get("installer")
    if not f or not os.path.exists(f):
        print("  INFO no downloaded installer (AUX_E2E not set): skipped")
        return
    s, data = action("Pair")
    x = Prefix()
    try:
        # its own pairing string was used by the command above: a new one on the command line
        rc, out = x.sh("cd / && sh %s --pair '%s'" % (f, json.loads(data)["pair"]))
        check(rc == 0 and "из самого установщика" in out and os.path.exists(x.lib), "installed from the file alone: rc %d %s" % (
            rc, out.strip()[-200:]))
        check(state()["paired"], "paired")
    finally:
        x.cleanup()


def t_disk(b):
    print("== disk (BMC side; QEMU has no USB device controller)", flush=True)
    udc = b.out("ls /sys/class/udc 2>&1").strip()
    print("    /sys/class/udc: %r" % udc)
    s, data = action("AgentDisk")
    st = state()
    if udc:
        port = b.out("cat /sys/kernel/config/usb_gadget/reimu_agent/UDC").strip()
        others = b.out("grep -H . /sys/kernel/config/usb_gadget/*/UDC").strip()
        check(s == 200 and st["disk"]["on"], "AgentDisk: the disk on %s" % port)
        check(port.endswith((":p3", ":p2")), "on p3/p2, never p1 (HID) or p4/p5 (media): %s" % others.replace("\n", "; "))
        lun = b.out("cd /sys/kernel/config/usb_gadget/reimu_agent/functions/mass_storage.agent/lun.0 && cat ro removable file")
        check(lun.split() == ["1", "1", "/run/reimu-agent-disk/disk.img"], "read-only, removable, the image in /run: %s" % lun.split())
        txt = b.out("dd if=/run/reimu-agent-disk/disk.img bs=512 skip=%d count=1 2>/dev/null | head -n 1" % (
            int(b.out("cat /usr/share/reimu-agent/disk.offset").strip()) // 512)).strip()
        check(txt == json.loads(data)["pair"], "PAIRING.TXT: the pairing string AgentDisk returned")
        check(b.out("stat -c %a /run/reimu-agent-disk").strip() == "700", "/run/reimu-agent-disk 0700")
        action("AgentDiskOff")
        check(not state()["disk"]["on"] and "reimu_agent" not in b.out("ls /sys/kernel/config/usb_gadget/"), "AgentDiskOff: gone")
    else:
        check(s >= 400 and "no free USB port" in json.dumps(data), "AgentDisk: the helper's error to the caller (%s %s)" % (s, data))
        check(not st["disk"]["on"] and not st["pairing"]["active"], "no disk, no pairing left open")
    check(b.out("ls /sys/kernel/config/usb_gadget/ 2>&1").find("reimu_agent") < 0, "configfs: no reimu_agent gadget left")
    check(b.out("ls /run/reimu-agent-disk 2>&1").find("No such file") >= 0, "no image with a string left in /run")
    # the gadget made in the real configfs up to the binding (a fake UDC name): rolled back
    r = b.ssh("mkdir -p /tmp/fu/dummy_udc.0 && s=$(busctl call xyz.openbmc_project.Reimu.HostLink %s "
              "xyz.openbmc_project.Reimu.HostLink Pair | sed 's/.*pair\\\\\":\\\\\"\\(reimu1:[A-Za-z0-9_-]*\\).*/\\1/') && "
              "echo \"$s\" | REIMU_UDC_CLASS=/tmp/fu /usr/libexec/reimu-agent-disk start; echo rc=$?; "
              "ls /sys/kernel/config/usb_gadget/" % OBJ)
    check("rc=1" in r.stdout and "reimu_agent" not in r.stdout.split("rc=1")[-1], "binding refused: the configfs gadget taken down again (%s %s)" % (
        r.stdout.strip()[-200:], r.stderr.strip()[-200:]))
    img = os.path.join(ab.W, "disk.img")
    c, env = ab.ssh_cmd("cat /usr/share/reimu-agent/disk.img")
    with open(img, "wb") as f:
        subprocess.run(c, env=env, stdout=f, timeout=120)
    off = int(b.out("cat /usr/share/reimu-agent/disk.offset").strip() or 0)
    data = open(img, "rb").read()
    check(len(data) == 1 << 20 and data[off:off + 25] == b"REIMU-PAIRING-PLACEHOLDER", "the image: 1 MiB, the placeholder at %d" % off)
    check(data[0x2b:0x36] == b"REIMU-AGENT", "label REIMU-AGENT")
    _, _, sh = get("/agent/install.sh")
    check(sh in data, "install.sh in it, as served")


def main():
    b = ab.Bmc("aux", fresh("aux")).start()
    try:
        check(b.wait_ssh(), "BMC up (ssh)")
        if not ONLY or "files" in ONLY:
            t_files(b)
        p = t_pin(b)
        if not ONLY or "web" in ONLY:
            t_web(b)
        if p and (not ONLY or "install" in ONLY):
            t_install(b, p)
        if not ONLY or "linklocal" in ONLY:
            t_linklocal()
        if not ONLY or "selfcontained" in ONLY:
            t_selfcontained()
        if not ONLY or "disk" in ONLY:
            t_disk(b)
    finally:
        b.stop()
    print("%d failed: %s" % (len(failed), failed) if failed else "all passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
