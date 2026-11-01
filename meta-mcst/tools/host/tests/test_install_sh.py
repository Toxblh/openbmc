#!/usr/bin/env python3
"""The host agent's installer (install.sh) and its bundle (mkbundle.sh) as the
BMC serves them, against a fake BMC: an HTTPS server with the bundle under
/agent/ (its own self-signed key, pinned as the web page pins bmcweb's) and,
for the pairing, the real reimu-hostlink built natively without D-Bus.

    HOSTLINK_NATIVE=.../reimu-hostlink-native python3 tools/host/tests/test_install_sh.py [-v] [TEST...]

Linux (the build container): curl, wget, python3, openssl, mtools. Nothing of
the machine is installed: REIMU_ROOT=DIR puts every path under DIR, systemctl
and journalctl are fakes that write down their arguments. Without
HOSTLINK_NATIVE the tests that pair are skipped.
"""
import base64, hashlib, http.server, json, os, shutil, socket, ssl, subprocess, sys, tempfile, threading, time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
MKBUNDLE = os.path.join(TOOLS, "mkbundle.sh")
INSTALL_SRC = os.path.join(TOOLS, "install.sh")
DAEMON = os.environ.get("HOSTLINK_NATIVE", "")
FILES = ("reimu-hostd.py", "reimu-hostd.service", "reimu-hostd.conf", "reimu-hostd.init")


def sh(cmd, env=None, timeout=120, input=None, cwd=None):
    r = subprocess.run(cmd, shell=isinstance(cmd, str), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=timeout, input=input, cwd=cwd)
    return r.returncode, r.stdout.decode(errors="replace")


def pair_string(d):
    return "reimu1:" + base64.urlsafe_b64encode(json.dumps(d, separators=(",", ":")).encode()).decode().rstrip("=")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


# ------------------------------------------------------------------ fakes
class Bmc:
    """HTTPS on 127.0.0.1 with DIR served at /; the pin of its key"""

    def __init__(self, root):
        self.root = root
        self.d = tempfile.mkdtemp(prefix="fakebmc-")
        key, crt = os.path.join(self.d, "key.pem"), os.path.join(self.d, "cert.pem")
        rc, out = sh(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
                      "-keyout", key, "-out", crt, "-days", "2", "-subj", "/CN=reimu-test"])
        assert rc == 0, out
        rc, out = sh("openssl x509 -in %s -pubkey -noout | openssl pkey -pubin -outform der | openssl dgst -sha256 -binary"
                     " | base64" % crt)
        self.pin = "sha256//" + out.strip()
        self.requests = []
        bmc = self

        class H(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *a, **kw):
                super().__init__(*a, directory=bmc.root, **kw)

            def log_message(self, *a):
                bmc.requests.append(self.path)

        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(crt, key)
        self.srv.socket = ctx.wrap_socket(self.srv.socket, server_side=True)
        self.url = "https://127.0.0.1:%d" % self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def close(self):
        self.srv.shutdown()
        self.srv.server_close()
        shutil.rmtree(self.d, ignore_errors=True)


class Hostlink:
    """reimu-hostlink native (no D-Bus), TLS on 127.0.0.1:PORT, ctl socket"""

    def __init__(self):
        self.d = tempfile.mkdtemp(prefix="hl-")
        self.port = free_port()
        conf = os.path.join(self.d, "conf")
        open(conf, "w").write("PORT=%d\nIFACE=\nAUTH_TIMEOUT_SEC=5\n" % self.port)
        self.env = dict(os.environ, REIMU_HOSTLINK_CONF=conf, REIMU_HOSTLINK_DIR=os.path.join(self.d, "state"),
                        REIMU_HOSTLINK_RUN=os.path.join(self.d, "run"), ASAN_OPTIONS="detect_leaks=0")
        self.log = os.path.join(self.d, "log")
        self.p = subprocess.Popen([DAEMON], env=self.env, stdout=open(self.log, "a"), stderr=subprocess.STDOUT)
        end = time.time() + 10
        while time.time() < end and not (os.path.exists(os.path.join(self.d, "run", "ctl")) and self.ctl("state").get("ok")):
            time.sleep(0.2)

    def ctl(self, cmd):
        r = subprocess.run([DAEMON, "--ctl", cmd], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        try:
            return json.loads(r.stdout)
        except ValueError:
            return {"ok": False}

    def pair(self):
        return self.ctl("pair")["data"]["pair"]

    def paired(self):
        return bool((self.ctl("state").get("data") or {}).get("paired"))

    def close(self):
        self.p.terminate()
        self.p.wait(10)
        shutil.rmtree(self.d, ignore_errors=True)


def bin_without(names):
    """a PATH directory with every command of the system's but NAMES"""
    d = tempfile.mkdtemp(prefix="bin-")
    for src in ("/usr/local/bin", "/usr/bin", "/bin", "/usr/sbin", "/sbin"):
        if not os.path.isdir(src):
            continue
        for n in os.listdir(src):
            if n in names or os.path.lexists(os.path.join(d, n)):
                continue
            os.symlink(os.path.join(src, n), os.path.join(d, n))
    return d


# ------------------------------------------------------------------ the tests
class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp(prefix="bundle-")
        rc, log = sh(["sh", MKBUNDLE, cls.out, "--disk"])
        assert rc == 0, "mkbundle.sh: " + log
        cls.www = os.path.join(cls.out, "www")
        os.makedirs(cls.www)
        shutil.copytree(os.path.join(cls.out, "agent"), os.path.join(cls.www, "agent"))
        cls.bmc = Bmc(cls.www)

    @classmethod
    def tearDownClass(cls):
        cls.bmc.close()
        shutil.rmtree(cls.out, ignore_errors=True)

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="root-")
        self.fake = tempfile.mkdtemp(prefix="fake-")
        for n, body in (("systemctl", 'echo "$*" >> "$REIMU_ROOT/systemctl.log"; [ "$1" = is-active ] && echo active; '
                                      'if [ "$1" = is-enabled ]; then [ -f "$REIMU_ROOT/pwmd-state" ] || exit 1; cat "$REIMU_ROOT/pwmd-state"; fi; exit 0'),
                        ("journalctl", 'echo "connected to the BMC at 127.0.0.1:5564 (ipv4)"')):
            p = os.path.join(self.fake, n)
            open(p, "w").write("#!/bin/sh\n" + body + "\n")
            os.chmod(p, 0o755)
        self.env = dict(os.environ, REIMU_ROOT=self.root, REIMU_SYSTEMCTL=os.path.join(self.fake, "systemctl"),
                        REIMU_JOURNALCTL=os.path.join(self.fake, "journalctl"), LC_ALL="C.UTF-8")
        self.hl = None

    def tearDown(self):
        if self.hl:
            self.hl.close()
        shutil.rmtree(self.root, ignore_errors=True)
        shutil.rmtree(self.fake, ignore_errors=True)

    def need_hostlink(self):
        if not DAEMON:
            self.skipTest("HOSTLINK_NATIVE not set")
        self.hl = Hostlink()
        # the native daemon has no IFACE: no MAC or IPv4 in the pairing
        # string; the agent's config sends it to 127.0.0.1 (kept by install.sh)
        os.makedirs(os.path.join(self.root, "etc"), exist_ok=True)
        open(self.conf(), "w").write("# test\nBMC=127.0.0.1:%d\nDISCOVERY=no\n" % self.hl.port)

    # paths in the fake root
    def lib(self):
        return os.path.join(self.root, "usr/local/lib/reimu/reimu-hostd.py")

    def unit(self):
        return os.path.join(self.root, "etc/systemd/system/reimu-hostd.service")

    def conf(self):
        return os.path.join(self.root, "etc/reimu-hostd.conf")

    def secret(self):
        return os.path.join(self.root, "etc/reimu-hostd/secret")

    def systemctl(self):
        try:
            return open(os.path.join(self.root, "systemctl.log")).read()
        except OSError:
            return ""

    def installed_script(self):
        """install.sh as downloaded from the fake BMC"""
        p = os.path.join(self.fake, "install.sh")
        shutil.copy(os.path.join(self.www, "agent", "install.sh"), p)
        return p

    def one_liner(self, pstr, pin=None):
        """what the web page shows for IPv4 (minus sudo: REIMU_ROOT)"""
        return ("s=$(curl -fsS --pinnedpubkey '%s' -k %s/agent/install.sh) && printf '%%s\\n' \"$s\" | sh -s -- --url %s"
                " --pair '%s'" % (pin or self.bmc.pin, self.bmc.url, self.bmc.url, pstr))


class Bundle(Base):
    def test_bundle_files_and_sums(self):
        a = os.path.join(self.out, "agent")
        for f in FILES + ("install.sh", "SHA256SUMS", "README.txt"):
            self.assertTrue(os.path.isfile(os.path.join(a, f)), f)
        sums = dict(reversed(l.split()) for l in open(os.path.join(a, "SHA256SUMS")).read().splitlines() if l.strip())
        for f in FILES:
            self.assertEqual(sums[f], hashlib.sha256(open(os.path.join(a, f), "rb").read()).hexdigest(), f)
            self.assertEqual(open(os.path.join(a, f), "rb").read(), open(os.path.join(TOOLS, f), "rb").read(), f)
        text = open(os.path.join(a, "install.sh")).read()
        for f in FILES:
            self.assertIn("%s  %s" % (sums[f], f), text, "the hashes are in install.sh itself")
        self.assertNotIn("@SHA256SUMS@", text)

    def test_disk_image(self):
        img, off = os.path.join(self.out, "disk.img"), int(open(os.path.join(self.out, "disk.offset")).read())
        rc, out = sh(["mlabel", "-s", "-i", img, "::"])
        self.assertIn("REIMU-AGENT", out)
        rc, out = sh(["mdir", "-b", "-i", img, "::"])
        names = sorted(os.path.basename(l).upper() for l in out.split())
        for f in ("INSTALL.SH", "README.TXT", "PAIRING.TXT") + tuple(x.upper() for x in FILES):
            self.assertIn(f, names)
        data = open(img, "rb").read()
        self.assertEqual(off % 512, 0, "PAIRING.TXT starts on a sector")
        self.assertTrue(data[off:off + 512].startswith(b"REIMU-PAIRING-PLACEHOLDER"), data[off:off + 40])
        rc, out = sh(["mtype", "-i", img, "::install.sh"])
        self.assertEqual(out.encode(), open(os.path.join(self.out, "agent", "install.sh"), "rb").read())


class Network(Base):
    def test_pinned_one_liner_installs_and_pairs(self):
        self.need_hostlink()
        rc, out = sh(self.one_liner(self.hl.pair()), env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertIn("curl", out)
        self.assertEqual(open(self.lib(), "rb").read(), open(os.path.join(TOOLS, "reimu-hostd.py"), "rb").read())
        self.assertEqual(os.stat(self.lib()).st_mode & 0o777, 0o755)
        self.assertEqual(os.stat(self.unit()).st_mode & 0o777, 0o644)
        self.assertIn("BMC=127.0.0.1:%d" % self.hl.port, open(self.conf()).read(), "an existing config is kept")
        self.assertEqual(os.stat(self.secret()).st_mode & 0o777, 0o600)
        self.assertIn("KEY=", open(self.secret()).read())
        self.assertTrue(self.hl.paired())
        log = self.systemctl()
        for c in ("daemon-reload", "enable reimu-hostd.service", "restart reimu-hostd.service"):
            self.assertIn(c, log)
        self.assertIn("connected to the BMC", out, "prints the agent's status")

    def test_wrong_pin_refused(self):
        n = len(self.bmc.requests)
        rc, out = sh(self.one_liner(pair_string({"token": "0" * 32, "fp": "a" * 64}), pin="sha256//" + "A" * 43 + "="),
                     env=self.env)
        self.assertFalse(os.path.exists(self.lib()), out)
        self.assertFalse(os.path.exists(self.unit()), out)
        self.assertEqual(self.systemctl(), "")
        self.assertNotEqual(rc, 0)
        self.assertEqual(self.bmc.requests[n:], [], "curl stops before the request: nothing fetched")

    def test_hash_mismatch_refused(self):
        self.need_hostlink()
        p = os.path.join(self.www, "agent", "reimu-hostd.py")
        good = open(p, "rb").read()
        try:
            open(p, "ab").write(b"\nimport os; os.system('touch /tmp/pwned')\n")
            rc, out = sh(self.one_liner(self.hl.pair()), env=self.env)
        finally:
            open(p, "wb").write(good)
        self.assertNotEqual(rc, 0, out)
        self.assertIn("SHA-256", out)
        self.assertFalse(os.path.exists(self.lib()), out)
        self.assertFalse(self.hl.paired())
        self.assertEqual(self.systemctl(), "")

    def downloader(self, without, name):
        self.need_hostlink()
        script = self.installed_script()
        env = dict(self.env, PATH=bin_without(without))
        try:
            rc, out = sh(["sh", script, "--url", self.bmc.url, "--pair", self.hl.pair()], env=env)
        finally:
            shutil.rmtree(env["PATH"], ignore_errors=True)
        self.assertEqual(rc, 0, out)
        self.assertIn("downloaded with %s" % name, out)
        self.assertTrue(self.hl.paired())
        self.assertTrue(os.path.exists(self.lib()))

    def test_curl(self):
        self.downloader((), "curl")

    def test_wget_without_curl(self):
        self.downloader(("curl",), "wget")

    def test_python_without_curl_and_wget(self):
        self.downloader(("curl", "wget"), "python3")

    def test_idempotent(self):
        self.need_hostlink()
        script = self.installed_script()
        rc, out = sh(["sh", script, "--url", self.bmc.url, "--pair", self.hl.pair()], env=self.env)
        self.assertEqual(rc, 0, out)
        k1 = open(self.secret()).read()
        open(self.conf(), "a").write("INTERVAL=5\n")
        rc, out = sh(["sh", script, "--url", self.bmc.url, "--pair", self.hl.pair()], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertNotEqual(open(self.secret()).read(), k1, "paired again: a new key")
        self.assertIn("INTERVAL=5", open(self.conf()).read(), "the config is the user's")
        k2 = open(self.secret()).read()
        rc, out = sh(["sh", script, "--url", self.bmc.url], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertEqual(open(self.secret()).read(), k2, "no --pair: the key is kept (an update)")
        self.assertEqual(self.systemctl().count("restart reimu-hostd.service"), 3)
        self.assertEqual(sorted(os.listdir(os.path.dirname(self.lib()))), ["reimu-hostd.py"], "no leftovers")

    def test_used_token_refused(self):
        self.need_hostlink()
        script = self.installed_script()
        s = self.hl.pair()
        rc, out = sh(["sh", script, "--url", self.bmc.url, "--pair", s], env=self.env)
        self.assertEqual(rc, 0, out)
        log = self.systemctl()
        rc, out = sh(["sh", script, "--url", self.bmc.url, "--pair", s], env=self.env)
        self.assertEqual(rc, 4, out)
        self.assertIn("pairing refused", out)
        self.assertIn("Новый ключ", out, "says where to get a new one")
        self.assertEqual(self.systemctl(), log, "the service left alone")

    def test_not_paired_not_started(self):
        rc, out = sh(["sh", self.installed_script(), "--url", self.bmc.url], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertTrue(os.path.exists(self.lib()))
        self.assertNotIn("restart", self.systemctl())
        self.assertIn("--pair", out, "tells how to pair")

    def test_usb_hub_rule(self):
        """the BMC's virtual USB hub (1d6b:0107) kept out of autosuspend: the
        udev rule installed (paired or not) and applied to the hub there now;
        removed by --uninstall"""
        hub = os.path.join(self.root, "sys/bus/usb/devices/4-4")
        os.makedirs(os.path.join(hub, "power"))
        for f, v in (("idVendor", "1d6b"), ("idProduct", "0107"), ("power/control", "auto")):
            open(os.path.join(hub, f), "w").write(v + "\n")
        rule = os.path.join(self.root, "etc/udev/rules.d/60-reimu-bmc-hub.rules")
        script = self.installed_script()
        rc, out = sh(["sh", script, "--url", self.bmc.url], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertTrue(os.path.exists(rule), out)
        self.assertIn('ATTR{idVendor}=="1d6b", ATTR{idProduct}=="0107"', open(rule).read())
        self.assertIn('ATTR{power/control}="on"', open(rule).read())
        self.assertEqual(open(os.path.join(hub, "power/control")).read().strip(), "on")
        rc, out = sh(["sh", script, "--uninstall"], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertFalse(os.path.exists(rule), "removed by --uninstall")

    def bus_owner_paired(self, pwmd):
        """a host already paired (the key there), pwmd in the given state"""
        os.makedirs(os.path.dirname(self.secret()))
        open(self.secret(), "w").write("KEY=" + "a" * 64 + "\nFINGERPRINT=" + "b" * 64 + "\n")
        if pwmd:
            open(os.path.join(self.root, "pwmd-state"), "w").write(pwmd + "\n")
        return (os.path.join(self.root, "etc/modprobe.d/reimu-bmc-owns-i2c.conf"),
                os.path.join(self.root, "etc/reimu-hostd/bus-owner.state"))

    def test_bus_owner_install_uninstall(self):
        """the BMC the only master of the shared I2C buses: modules kept out,
        pwmd off and its state written down; --uninstall gives both back,
        unbinding nothing; the agent stopped first, or it would stop pwmd again"""
        conf, state = self.bus_owner_paired("enabled")
        cl = os.path.join(self.root, "sys/bus/i2c/devices/0-004c")
        drv = os.path.join(self.root, "sys/bus/i2c/drivers/lm63")
        os.makedirs(cl)
        os.makedirs(drv)
        open(os.path.join(drv, "unbind"), "w").close()
        os.symlink(drv, os.path.join(cl, "driver"))
        script = self.installed_script()
        rc, out = sh(["sh", script, "--url", self.bmc.url], env=self.env)
        self.assertEqual(rc, 0, out)
        text = open(conf).read()
        for m in ("lm63", "emc2305", "pwm_fan", "pmbus", "ir38064"):
            self.assertIn("install %s /bin/true" % m, text)
        self.assertIn("blacklist lm63", text)
        self.assertEqual(open(state).read().strip(), "pwmd=enabled")
        self.assertIn("disable --now pwmd", self.systemctl())
        self.assertEqual(open(os.path.join(drv, "unbind")).read(), "0-004c", "unbound at once")
        self.assertIn("BMC", out)
        self.assertLess(self.systemctl().index("disable --now pwmd"), self.systemctl().index("restart reimu-hostd.service"))
        # an update: the original state stays
        open(os.path.join(self.root, "pwmd-state"), "w").write("disabled\n")
        rc, out = sh(["sh", script, "--url", self.bmc.url], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertEqual(open(state).read().strip(), "pwmd=enabled")
        open(os.path.join(drv, "unbind"), "w").close()
        rc, out = sh(["sh", script, "--uninstall"], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertFalse(os.path.exists(conf))
        self.assertFalse(os.path.exists(state))
        log = self.systemctl()
        self.assertLess(log.index("disable --now reimu-hostd.service"), log.index("enable --now pwmd"))
        self.assertEqual(open(os.path.join(drv, "unbind")).read(), "", "unbinds nothing")
        self.assertIn("перезагруз", out.lower(), "a reboot gives the host its drivers back")
        self.assertFalse(os.path.exists(os.path.dirname(self.secret())), "no leftovers")

    def test_bus_owner_pwmd_was_off(self):
        conf, state = self.bus_owner_paired("disabled")
        script = self.installed_script()
        rc, out = sh(["sh", script, "--url", self.bmc.url], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertEqual(open(state).read().strip(), "pwmd=disabled")
        rc, out = sh(["sh", script, "--uninstall"], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertNotIn("enable --now pwmd", self.systemctl())
        self.assertFalse(os.path.exists(conf))

    def test_bus_owner_not_before_paired(self):
        """not paired: no agent will watch, the host's own fan control stays"""
        rc, out = sh(["sh", self.installed_script(), "--url", self.bmc.url], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertFalse(os.path.exists(os.path.join(self.root, "etc/modprobe.d/reimu-bmc-owns-i2c.conf")))
        self.assertNotIn("pwmd", self.systemctl())

    def test_bus_owner_config_says_no(self):
        conf, state = self.bus_owner_paired("enabled")
        open(self.conf(), "w").write("ALLOW=gpu_fan gpu_turbo\n")
        rc, out = sh(["sh", self.installed_script(), "--url", self.bmc.url], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertFalse(os.path.exists(conf))
        self.assertNotIn("pwmd", self.systemctl())

    def test_bad_pairing_string(self):
        rc, out = sh(["sh", self.installed_script(), "--url", self.bmc.url, "--pair", "reimu1:@@;rm -rf /"], env=self.env)
        self.assertEqual(rc, 2, out)
        self.assertIn("not a pairing string", out)
        self.assertFalse(os.path.exists(self.lib()))

    def test_unbuilt_installer_refuses_the_network(self):
        rc, out = sh(["sh", INSTALL_SRC, "--url", self.bmc.url, "--pair", pair_string({"token": "0" * 32, "fp": "a" * 64})],
                     env=self.env)
        self.assertEqual(rc, 3, out)
        self.assertIn("unbuilt", out)
        self.assertFalse(os.path.exists(self.lib()))

    def test_partial_download_does_nothing(self):
        text = open(os.path.join(self.www, "agent", "install.sh"), "rb").read()
        for cut in (len(text) // 3, len(text) // 2, len(text) - 20, len(text) - 2):
            rc, out = sh(["sh", "-s", "--", "--url", self.bmc.url, "--uninstall"], env=self.env, input=text[:cut])
            self.assertEqual(self.systemctl(), "", "cut at %d: ran something: %s" % (cut, out))

    def test_not_root(self):
        env = dict(self.env)
        del env["REIMU_ROOT"]
        os.chmod(self.fake, 0o755)
        rc, out = sh(["runuser", "-u", "nobody", "--", "sh", self.installed_script(), "--url", self.bmc.url], env=env)
        self.assertNotEqual(rc, 0)
        self.assertIn("sudo", out)

    def test_uninstall(self):
        self.need_hostlink()
        script = self.installed_script()
        rc, out = sh(["sh", script, "--url", self.bmc.url, "--pair", self.hl.pair()], env=self.env)
        self.assertEqual(rc, 0, out)
        rc, out = sh(["sh", script, "--uninstall"], env=self.env)
        self.assertEqual(rc, 0, out)
        for p in (self.lib(), self.unit(), self.secret()):
            self.assertFalse(os.path.exists(p), p)
        self.assertTrue(os.path.exists(self.conf()), "the config kept")
        self.assertIn("disable --now reimu-hostd.service", self.systemctl())
        self.assertIn("Отключить агент", out, "says how to revoke on the BMC")
        rc, out = sh(["sh", script, "--uninstall"], env=self.env)
        self.assertEqual(rc, 0, "twice: " + out)
        rc, out = sh(["sh", script, "--uninstall", "--purge"], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertFalse(os.path.exists(self.conf()))
        self.assertFalse(os.path.exists(os.path.dirname(self.lib())))

    def test_bsd_grep(self):
        """macOS/BSD grep (and others with RE_DUP_MAX 255) refuse {16,2000}:
        found running the page's command on a Mac"""
        g = os.path.join(self.fake, "grep")
        open(g, "w").write('#!/bin/sh\nfor a; do case $a in *{*,[0-9][0-9][0-9][0-9]*}*|*{*,[3-9][0-9][0-9]}*) '
                           'echo "grep: maximum repetition exceeds 255" >&2; exit 2 ;; esac; done\nexec /bin/grep "$@"\n')
        os.chmod(g, 0o755)
        s = pair_string({"token": "0" * 32, "fp": "a" * 64, "v4": "192.168.10.79"})
        rc, out = sh(["sh", self.installed_script(), "--print-urls", "--pair", s],
                     env=dict(self.env, PATH=self.fake + ":" + os.environ["PATH"]))
        self.assertEqual((rc, out.strip()), (0, "https://192.168.10.79"), out)

    def test_urls_link_local_first(self):
        sysd = os.path.join(self.fake, "sys")
        for n, state, typ in (("lo", "unknown", "772"), ("enp1s1f1", "up", "1"), ("eth9", "down", "1"),
                              ("enp1s1f0", "up", "1")):
            os.makedirs(os.path.join(sysd, "class/net", n))
            open(os.path.join(sysd, "class/net", n, "operstate"), "w").write(state + "\n")
            open(os.path.join(sysd, "class/net", n, "type"), "w").write(typ + "\n")
        s = pair_string({"token": "0" * 32, "fp": "a" * 64, "mac": "98:a7:b0:06:0f:7e", "v4": "192.168.10.79", "port": 5564})
        rc, out = sh(["sh", self.installed_script(), "--print-urls", "--pair", s], env=dict(self.env, REIMU_SYS=sysd))
        self.assertEqual(rc, 0, out)
        self.assertEqual(out.split(), ["https://[fe80::9aa7:b0ff:fe06:f7e%25enp1s1f0]", "https://[fe80::9aa7:b0ff:fe06:f7e%25enp1s1f1]",
                                       "https://192.168.10.79"])


class Offline(Base):
    def usb_dir(self, pstr):
        """the virtual USB disk's files, PAIRING.TXT as the BMC writes it"""
        d = os.path.join(self.fake, "usb")
        os.makedirs(d)
        for f in FILES + ("install.sh", "README.txt"):
            shutil.copy(os.path.join(self.out, "agent", f), d)
        open(os.path.join(d, "PAIRING.TXT"), "w").write((pstr + "\n").ljust(511) + "\n")
        return d

    def test_usb_disk_without_network(self):
        self.need_hostlink()
        d = self.usb_dir(self.hl.pair())
        n = len(self.bmc.requests)
        rc, out = sh(["sh", os.path.join(d, "install.sh")], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertNotIn("downloaded", out)
        self.assertTrue(self.hl.paired())
        self.assertEqual(len(self.bmc.requests), n, "nothing fetched")

    def test_usb_tampered_file_refused(self):
        self.need_hostlink()
        d = self.usb_dir(self.hl.pair())
        open(os.path.join(d, "reimu-hostd.service"), "a").write("ExecStartPre=/bin/sh -c 'id'\n")
        rc, out = sh(["sh", os.path.join(d, "install.sh")], env=self.env)
        self.assertEqual(rc, 3, out)
        self.assertIn("SHA-256", out)
        self.assertFalse(self.hl.paired())

    def test_self_contained(self):
        """the page's «Скачать установщик»: install.sh with the files appended
        (base64, one line each after #__REIMU_PAYLOAD__) and the pairing
        string set in REIMU_PAIR_DEFAULT (js/lib/agent-pack.js does the same)"""
        self.need_hostlink()
        text = open(os.path.join(self.out, "agent", "install.sh")).read()
        pstr = self.hl.pair()
        self.assertIn("\nREIMU_PAIR_DEFAULT=''\n", text)
        text = text.replace("\nREIMU_PAIR_DEFAULT=''\n", "\nREIMU_PAIR_DEFAULT='%s'\n" % pstr, 1)
        text += "#__REIMU_PAYLOAD__\n" + "".join(
            "%s %s\n" % (f, base64.b64encode(open(os.path.join(self.out, "agent", f), "rb").read()).decode()) for f in FILES)
        p = os.path.join(self.fake, "reimu-agent-install.sh")
        open(p, "w").write(text)
        rc, out = sh(["sh", p], env=self.env, cwd="/")
        self.assertEqual(rc, 0, out)
        self.assertTrue(self.hl.paired())
        self.assertEqual(open(self.lib(), "rb").read(), open(os.path.join(TOOLS, "reimu-hostd.py"), "rb").read())


# ------------------------------------------------------------------ SysV init
FAKE_PY = r"""#!/bin/sh
# the agent's run and restore-auto faked; anything else the real python3
case " $* " in
    *" run "*)
        echo "$$" >> "$REIMU_ROOT/agents.log"
        [ -f "$REIMU_ROOT/agent-silent" ] || echo "connected to the BMC at 127.0.0.1:5564 (ipv4)"
        trap 'exit 0' TERM
        while :; do sleep 1; done ;;
    *" restore-auto "*)
        echo "restore" >> "$REIMU_ROOT/restore.log"
        exit 0 ;;
esac
exec /usr/bin/python3 "$@"
"""


def _alive_with(text):
    """pids (not zombies) whose command line has TEXT"""
    out = []
    for d in os.listdir("/proc"):
        if d.isdigit():
            try:
                c = open("/proc/%s/cmdline" % d, "rb").read().replace(b"\0", b" ").decode(errors="replace")
            except OSError:
                continue
            if text in c:
                out.append(int(d))
    return out


class SysvBase(Base):
    BIN = None

    def setUp(self):
        super().setUp()
        if SysvBase.BIN is None:
            SysvBase.BIN = bin_without(("systemctl", "journalctl", "update-rc.d", "chkconfig", "insserv"))
        self.pybin = os.path.join(self.fake, "pybin")
        self.rcbin = os.path.join(self.fake, "rcbin")
        os.makedirs(self.pybin)
        os.makedirs(self.rcbin)
        p = os.path.join(self.pybin, "python3")
        open(p, "w").write(FAKE_PY)
        os.chmod(p, 0o755)
        self.env = dict(os.environ, REIMU_ROOT=self.root, LC_ALL="C.UTF-8", PATH=self.pybin + ":" + self.BIN,
                        REIMU_RUN_SYSTEMD=os.path.join(self.root, "run/systemd/system"), REIMU_RC_BIN=self.rcbin,
                        REIMU_RESTART_SEC="1", REIMU_LINK_WAIT="3")
        self.env.pop("REIMU_SYSTEMCTL", None)
        self.env.pop("REIMU_JOURNALCTL", None)

    def tearDown(self):
        for pid in _alive_with(self.root):
            try:
                os.kill(pid, 9)
            except OSError:
                pass
        super().tearDown()

    def p(self, rel):
        return os.path.join(self.root, rel)

    def rc_tool(self, name, rc=0, link=None):
        """link: the boot link the tool makes when it enables, as a real one does."""
        f = os.path.join(self.rcbin, name)
        make = ('case "$*" in *remove*|--del*|-r*) ;; *) ln -sf x "$REIMU_ROOT/%s" ;; esac\n' % link) if link else ""
        open(f, "w").write('#!/bin/sh\necho "%s $*" >> "$REIMU_ROOT/rc.log"\n%sexit %d\n' % (name, make, rc))
        os.chmod(f, 0o755)

    def rc_log(self):
        try:
            return open(self.p("rc.log")).read()
        except OSError:
            return ""

    def make_paired(self):
        os.makedirs(self.p("etc/reimu-hostd"), exist_ok=True)
        open(self.secret(), "w").write("KEY=00\nFINGERPRINT=00\n")

    def mkrc(self, base="etc"):
        for n in ("rc0.d", "rc3.d", "rc5.d", "rc6.d"):
            os.makedirs(self.p(os.path.join(base, n)), exist_ok=True)

    def install(self, *args, check=True):
        rc, out = sh(["sh", self.installed_script(), "--url", self.bmc.url] + list(args), env=self.env)
        if check:
            self.assertEqual(rc, 0, out)
        return rc, out

    def agents(self):
        return _alive_with(self.p("usr/local/lib/reimu/reimu-hostd.py run"))


class SysvInstall(SysvBase):
    def test_detection(self):
        self.make_paired()
        fake = os.path.join(self.fake, "systemctl")
        # the systemd directory, systemctl: systemd
        os.makedirs(self.env["REIMU_RUN_SYSTEMD"])
        self.env["REIMU_SYSTEMCTL"] = fake
        self.install()
        self.assertIn("restart reimu-hostd.service", self.systemctl())
        self.assertFalse(os.path.exists(self.p("etc/init.d/reimu-hostd")))
        self.assertTrue(os.path.exists(self.unit()))
        # the directory but no systemctl: SysV
        del self.env["REIMU_SYSTEMCTL"]
        self.install()
        self.assertTrue(os.path.exists(self.p("etc/init.d/reimu-hostd")))
        sh(["sh", self.installed_script(), "--uninstall"], env=self.env)
        # systemctl but no directory (a stub on a SysV system): SysV
        os.rmdir(self.env["REIMU_RUN_SYSTEMD"])
        self.env["REIMU_SYSTEMCTL"] = fake
        open(os.path.join(self.root, "systemctl.log"), "w").close()
        self.make_paired()
        self.install()
        self.assertEqual(self.systemctl(), "", "not called")
        self.assertTrue(os.path.exists(self.p("etc/init.d/reimu-hostd")))

    def test_install_without_systemd(self):
        self.make_paired()
        self.mkrc()
        rc, out = self.install()
        for bad in ("systemctl", "journalctl", "не найдена", "not found", "не запустилась"):
            self.assertNotIn(bad, out)
        init = self.p("etc/init.d/reimu-hostd")
        self.assertTrue(os.access(init, os.X_OK))
        self.assertFalse(os.path.exists(self.unit()), "no systemd unit where there is no systemd")
        self.assertEqual(open(init, "rb").read(), open(os.path.join(TOOLS, "reimu-hostd.init"), "rb").read())
        self.assertIn("connected to the BMC", out)
        self.assertEqual(len(self.agents()), 1)
        self.assertIn("connected to the BMC", open(self.p("var/log/reimu-hostd.log")).read())
        rc, st = sh(["sh", init, "status"], env=self.env)
        self.assertEqual(rc, 0, st)

    def test_not_paired_not_started(self):
        rc, out = self.install()
        self.assertTrue(os.path.exists(self.p("etc/init.d/reimu-hostd")), "the files in place before pairing")
        self.assertIn("--pair", out)
        self.assertEqual(self.agents(), [])
        self.assertFalse(os.path.exists(self.p("etc/rc3.d")))

    def test_no_link_names_the_log_not_the_journal(self):
        self.make_paired()
        open(self.p("agent-silent"), "w").close()
        rc, out = self.install()
        self.assertIn("/var/log/reimu-hostd.log", out)
        self.assertNotIn("journalctl", out)

    def test_old_log_lines_dont_count(self):
        self.make_paired()
        os.makedirs(self.p("var/log"))
        open(self.p("var/log/reimu-hostd.log"), "w").write("connected to the BMC long ago\n")
        open(self.p("agent-silent"), "w").close()
        rc, out = self.install()
        self.assertNotIn("Готово", out)

    def test_start_failure_names_the_log_and_the_command(self):
        self.make_paired()
        os.makedirs(self.p("var"))
        open(self.p("var/run"), "w").close()  # no pidfile can be written
        rc, out = self.install(check=False)
        self.assertEqual(rc, 1, out)
        self.assertIn("/var/log/reimu-hostd.log", out)
        self.assertIn("/etc/init.d/reimu-hostd status", out)
        self.assertNotIn("journalctl", out)

    def test_boot_tools(self):
        for tool, want in (("update-rc.d", "update-rc.d reimu-hostd defaults"),
                           ("chkconfig", "chkconfig --add reimu-hostd\nchkconfig reimu-hostd on"),
                           ("insserv", "insserv reimu-hostd")):
            with self.subTest(tool):
                for t in os.listdir(self.rcbin):
                    os.remove(os.path.join(self.rcbin, t))
                self.rc_tool(tool, link="etc/rc3.d/S50reimu-hostd")
                self.make_paired()
                self.mkrc()
                self.install()
                self.assertIn(want, self.rc_log())
                self.assertEqual(os.listdir(self.p("etc/rc3.d")), ["S50reimu-hostd"], "the tool's job")
                rc, out = sh(["sh", self.installed_script(), "--uninstall"], env=self.env)
                self.assertEqual(rc, 0, out)
                self.assertRegex(self.rc_log(), {"update-rc.d": r"update-rc.d -f reimu-hostd remove",
                                                 "chkconfig": r"chkconfig --del reimu-hostd",
                                                 "insserv": r"insserv -r reimu-hostd"}[tool])
                self.assertFalse(os.path.exists(self.p("etc/init.d/reimu-hostd")))
                os.remove(self.p("rc.log"))

    def test_boot_tool_says_ok_but_makes_no_link(self):
        # its exit code is no proof: without a link the agent would not start
        # at boot while the installer says it will
        self.make_paired()
        self.mkrc()
        self.rc_tool("chkconfig")
        rc, out = self.install()
        self.assertTrue(os.path.lexists(self.p("etc/rc3.d/S99reimu-hostd")), out)
        self.assertIn("/rc?.d", out)
        self.assertNotIn("(chkconfig)", out)

    def elbrus_95_tree(self):
        # as on Elbrus Linux 9.5.5 (its owner's listing): init.d and rc?.d in
        # /etc, /etc/rc.d only has a link back, rcS.d a link to rcsysinit.d
        for n in ("rc0.d", "rc1.d", "rc2.d", "rc3.d", "rc4.d", "rc5.d", "rc6.d", "rcsysinit.d", "init.d", "rc.d"):
            os.makedirs(self.p("etc/" + n), exist_ok=True)
        os.symlink("../init.d", self.p("etc/rc.d/init.d"))
        os.symlink("rcsysinit.d", self.p("etc/rcS.d"))

    def test_elbrus_95_tree(self):
        self.elbrus_95_tree()
        self.make_paired()
        self.rc_tool("chkconfig", rc=1)
        self.rc_tool("insserv", rc=1)
        self.install()
        self.assertTrue(os.path.isfile(self.p("etc/init.d/reimu-hostd")))
        self.assertTrue(os.path.islink(self.p("etc/rc.d/init.d")), "the link stays a link")
        self.assertEqual(os.readlink(self.p("etc/rc3.d/S99reimu-hostd")), "/etc/init.d/reimu-hostd")
        self.assertEqual(os.readlink(self.p("etc/rc0.d/K01reimu-hostd")), "/etc/init.d/reimu-hostd")
        self.assertFalse(os.path.exists(self.p("etc/rc.d/rc3.d")))
        self.assertEqual(len(self.agents()), 1)
        rc, out = sh(["sh", self.installed_script(), "--uninstall"], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertEqual(os.listdir(self.p("etc/rc3.d")), [])
        self.assertFalse(os.path.exists(self.p("etc/init.d/reimu-hostd")))
        self.assertEqual(self.agents(), [])

    def test_boot_tool_order_and_failure_falls_through(self):
        self.make_paired()
        self.mkrc()
        self.rc_tool("update-rc.d", rc=1)
        self.rc_tool("chkconfig", rc=1)
        self.rc_tool("insserv", rc=1)
        self.install()
        log = self.rc_log()
        self.assertLess(log.index("update-rc.d"), log.index("chkconfig"))
        self.assertLess(log.index("chkconfig"), log.index("insserv"))
        self.assertTrue(os.path.lexists(self.p("etc/rc3.d/S99reimu-hostd")), "the links, last")

    def test_symlink_fallback(self):
        for base in ("etc", "etc/rc.d"):
            with self.subTest(base):
                self.make_paired()
                self.mkrc(base)
                self.install()
                for link, in (("rc3.d/S99reimu-hostd",), ("rc5.d/S99reimu-hostd",), ("rc0.d/K01reimu-hostd",), ("rc6.d/K01reimu-hostd",)):
                    self.assertEqual(os.readlink(self.p(os.path.join(base, link))), "/etc/init.d/reimu-hostd", link)
                rc, out = sh(["sh", self.installed_script(), "--uninstall"], env=self.env)
                self.assertEqual(rc, 0, out)
                self.assertEqual(os.listdir(self.p(os.path.join(base, "rc3.d"))), [])
                shutil.rmtree(self.p(base))

    def test_rc_d_init_d_when_no_etc_init_d(self):
        self.make_paired()
        self.mkrc("etc/rc.d")
        os.makedirs(self.p("etc/rc.d/init.d"))
        self.install()
        self.assertTrue(os.path.exists(self.p("etc/rc.d/init.d/reimu-hostd")))
        self.assertFalse(os.path.exists(self.p("etc/init.d")))
        self.assertEqual(os.readlink(self.p("etc/rc.d/rc3.d/S99reimu-hostd")), "/etc/rc.d/init.d/reimu-hostd")

    def test_no_way_to_start_at_boot_is_said(self):
        self.make_paired()
        rc, out = self.install()
        self.assertIn("НЕ будет запускаться при загрузке", out)
        self.assertIn("will NOT start at boot", out)
        self.assertEqual(len(self.agents()), 1, "it runs now all the same")

    def test_reinstall(self):
        self.make_paired()
        self.mkrc()
        self.install()
        first = self.agents()
        self.install()
        now = self.agents()
        self.assertEqual(len(now), 1)
        self.assertNotEqual(first, now, "restarted: the new code runs")

    def test_uninstall_removes_the_gpu_policy(self):
        self.make_paired()
        self.mkrc()
        self.install()
        d = self.p("var/lib/reimu-hostd")
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, "policy"), "w").write('{"mode": "manual", "duty": 50}\n')
        rc, out = sh(["sh", self.installed_script(), "--uninstall"], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertFalse(os.path.exists(d), "the file and the directory it leaves empty")
        self.install()
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, "policy"), "w").write('{"mode": "auto", "duty": null}\n')
        open(os.path.join(d, "not-ours"), "w").write("x")
        rc, out = sh(["sh", self.installed_script(), "--uninstall"], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertEqual(os.listdir(d), ["not-ours"])

    def test_uninstall_removes_everything(self):
        self.make_paired()
        self.mkrc()
        self.install()
        rc, out = sh(["sh", self.installed_script(), "--uninstall"], env=self.env)
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.agents(), [])
        self.assertEqual(_alive_with("_loop"), [p for p in _alive_with("_loop") if self.root not in
                                                open("/proc/%d/cmdline" % p).read()])
        self.assertTrue(os.path.exists(self.p("restore.log")), "the GPU fan back to the driver")
        for f in ("etc/init.d/reimu-hostd", "etc/rc3.d/S99reimu-hostd", "etc/rc5.d/S99reimu-hostd", "etc/rc0.d/K01reimu-hostd",
                  "etc/rc6.d/K01reimu-hostd", "var/run/reimu-hostd.pid", "var/run/reimu-hostd.agent.pid",
                  "var/log/reimu-hostd.log", "var/log/reimu-hostd.log.1"):
            self.assertFalse(os.path.lexists(self.p(f)), f)
        self.assertFalse(os.path.exists(self.lib()))
        self.assertFalse(os.path.exists(self.secret()))
        self.assertNotIn("journalctl", out)
        self.assertNotIn("systemctl", out)
        rc, out = sh(["sh", self.installed_script(), "--uninstall"], env=self.env)
        self.assertEqual(rc, 0, "twice: " + out)

    def test_tampered_init_refused(self):
        d = os.path.join(self.fake, "usb")
        os.makedirs(d)
        for f in FILES + ("install.sh",):
            shutil.copy(os.path.join(self.out, "agent", f), d)
        open(os.path.join(d, "reimu-hostd.init"), "a").write("# evil\n")
        rc, out = sh(["sh", os.path.join(d, "install.sh")], env=self.env)
        self.assertEqual(rc, 3, out)
        self.assertIn("SHA-256", out)
        self.assertFalse(os.path.exists(self.p("etc/init.d/reimu-hostd")))

    def test_python_too_old_is_said(self):
        p = os.path.join(self.pybin, "python3")
        open(p, "w").write('#!/bin/sh\ncase "$*" in *version_info*) exit 1 ;; esac\nexit 0\n')
        rc, out = self.install(check=False)
        self.assertEqual(rc, 1, out)
        self.assertIn("3.6", out)

    def test_pwmd_on_sysv(self):
        self.make_paired()
        self.mkrc()
        os.makedirs(self.p("etc/init.d"))
        open(self.p("etc/init.d/pwmd"), "w").close()
        os.symlink("../init.d/pwmd", self.p("etc/rc3.d/S20pwmd"))
        rc, out = self.install()
        self.assertEqual(open(self.p("etc/reimu-hostd/bus-owner.state")).read().strip(), "pwmd=enabled")
        self.assertFalse(os.path.lexists(self.p("etc/rc3.d/S20pwmd")))
        self.assertTrue(os.path.lexists(self.p("etc/rc3.d/K20pwmd")))
        self.assertIn("только у BMC", out)
        sh(["sh", self.installed_script(), "--uninstall"], env=self.env)
        self.assertTrue(os.path.lexists(self.p("etc/rc3.d/S20pwmd")), "given back")

    def test_pwmd_not_stoppable_is_said_by_the_installer(self):
        self.make_paired()
        os.makedirs(self.p("proc/4242"))
        open(self.p("proc/4242/comm"), "w").write("pwmd\n")
        rc, out = self.install()  # the fake runner runs nothing: nothing can stop it
        self.assertIn("ERROR", out)
        self.assertIn("pwmd", out)
        self.assertIn("Не удалось передать шины", out)
        self.assertNotIn("только у BMC", out)
        self.assertEqual(len(self.agents()), 1, "the agent itself runs: it keeps trying and tells the BMC")


class InitScript(SysvBase):
    """the init script itself, for real: a fake agent, in a temp dir"""
    SHELL = "/bin/sh"

    def setUp(self):
        super().setUp()
        if not shutil.which(self.SHELL.split()[0]):
            self.skipTest(self.SHELL)
        self.init = self.p("etc/init.d/reimu-hostd")
        os.makedirs(os.path.dirname(self.init))
        shutil.copy(os.path.join(TOOLS, "reimu-hostd.init"), self.init)
        os.makedirs(os.path.dirname(self.lib()))
        open(self.lib(), "w").close()
        self.env["REIMU_SH"] = self.SHELL

    def do(self, action, env=None):
        return sh(self.SHELL.split() + [self.init, action], env=env or self.env, timeout=40)

    def wait_agents(self, n, not_in=(), secs=10):
        end = time.time() + secs
        while time.time() < end:
            a = [x for x in self.agents() if x not in not_in]
            if len(a) == n:
                return a
            time.sleep(0.1)
        self.fail("agents: %r, wanted %d" % (self.agents(), n))

    def loops(self):
        return _alive_with(self.init + " _loop")

    def test_lsb_header(self):
        t = open(self.init).read()
        for k in ("### BEGIN INIT INFO", "# Provides: reimu-hostd", "# Default-Start:", "# Default-Stop:", "### END INIT INFO",
                  "# chkconfig:"):
            self.assertIn(k, t)
        self.assertTrue(t.startswith("#!/bin/sh\n"))

    def test_start_status_stop(self):
        self.assertEqual(self.do("status")[0], 3)
        rc, out = self.do("start")
        self.assertEqual(rc, 0, out)
        self.wait_agents(1)
        self.assertEqual(len(self.loops()), 1)
        rc, out = self.do("status")
        self.assertEqual(rc, 0, out)
        self.assertEqual(open(self.p("var/run/reimu-hostd.pid")).read().strip(), str(self.loops()[0]))
        rc, out = self.do("stop")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.agents(), [])
        self.assertEqual(self.loops(), [])
        self.assertIn("restore", open(self.p("restore.log")).read(), "stop: the GPU fan to the driver")
        self.assertFalse(os.path.exists(self.p("var/run/reimu-hostd.pid")))
        self.assertEqual(self.do("status")[0], 3)
        self.assertEqual(self.do("stop")[0], 0, "stopped again: fine")

    def test_double_start_one_copy(self):
        self.do("start")
        first = self.wait_agents(1)
        rc, out = self.do("start")
        self.assertEqual(rc, 0, out)
        time.sleep(1.5)
        self.assertEqual(self.agents(), first)
        self.assertEqual(len(self.loops()), 1)

    def test_concurrent_starts_one_copy(self):
        ps = [subprocess.Popen(self.SHELL.split() + [self.init, "start"], env=self.env, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL) for _ in range(6)]
        for x in ps:
            x.wait(40)
        self.wait_agents(1)
        time.sleep(1.5)
        self.assertEqual(len(self.agents()), 1)
        self.assertEqual(len(self.loops()), 1)

    def test_restart(self):
        self.do("start")
        first = self.wait_agents(1)
        rc, out = self.do("restart")
        self.assertEqual(rc, 0, out)
        self.wait_agents(1, not_in=first)
        self.assertEqual(len(self.agents()), 1)
        self.assertEqual(len(self.loops()), 1)

    def test_agent_dies_and_comes_back(self):
        self.do("start")
        first = self.wait_agents(1)
        os.kill(first[0], 9)
        new = self.wait_agents(1, not_in=first)
        self.assertNotEqual(new, first)
        self.assertIn("restore", open(self.p("restore.log")).read(), "restore-auto between the runs")
        self.assertEqual(open(self.p("var/run/reimu-hostd.agent.pid")).read().strip(), str(new[0]))
        self.assertEqual(self.do("status")[0], 0)

    def test_stale_pidfile_of_someone_else(self):
        os.makedirs(self.p("var/run"))
        other = subprocess.Popen(["sleep", "30"])
        try:
            open(self.p("var/run/reimu-hostd.pid"), "w").write("%d\n" % other.pid)
            open(self.p("var/run/reimu-hostd.agent.pid"), "w").write("%d\n" % other.pid)
            self.assertEqual(self.do("status")[0], 1, "a dead service with its pidfile")
            rc, out = self.do("start")
            self.assertEqual(rc, 0, out)
            self.wait_agents(1)
            self.assertIsNone(other.poll(), "not ours: not killed")
            self.do("stop")
            self.assertIsNone(other.poll())
        finally:
            other.kill()
            other.wait()

    def test_stale_lock(self):
        os.makedirs(self.p("var/run/reimu-hostd.lock"))
        open(self.p("var/run/reimu-hostd.lock/pid"), "w").write("999999\n")
        rc, out = self.do("start")
        self.assertEqual(rc, 0, out)
        self.wait_agents(1)

    # the lock's holder: a pid file with a live action of this script in it
    def lock_dir(self, pid=None):
        d = self.p("var/run/reimu-hostd.lock")
        os.makedirs(d)
        if pid is not None:
            open(os.path.join(d, "pid"), "w").write(pid)
        return d

    def holder(self, secs):
        """an action of this script, by name, holding the lock (`restart` over a stubborn agent)"""
        d = self.p("var/run/reimu-hostd.lock")
        f = self.p("var/tmp/reimu-hostd")
        os.makedirs(os.path.dirname(f))
        open(f, "w").write('mkdir "%s"\necho $$ > "%s/pid"\nsleep %d\nrm -rf "%s"\n' % (d, d, secs, d))
        os.makedirs(self.p("var/run"), exist_ok=True)
        p = subprocess.Popen(self.SHELL.split() + [f, "restart"], env=self.env)
        self.addCleanup(lambda: (p.kill(), p.wait()))
        end = time.time() + 5
        while not os.path.exists(os.path.join(d, "pid")) and time.time() < end:
            time.sleep(0.05)
        return p, d

    def bg(self, action):
        p = subprocess.Popen(self.SHELL.split() + [self.init, action], env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.addCleanup(lambda: (p.poll() is None and p.kill(), p.wait(), p.stdout.close()))
        return p

    def test_lock_live_holder_is_waited_for_however_long(self):
        h, d = self.holder(40)
        t0 = time.time()
        start = self.bg("start")
        time.sleep(36)  # the old rule broke in at 30 s
        self.assertIsNone(start.poll(), "the start must still wait")
        self.assertEqual(open(os.path.join(d, "pid")).read().strip(), str(h.pid), "the holder's lock still its own")
        self.assertEqual(self.loops(), [])
        self.assertFalse(os.path.exists(self.p("var/run/reimu-hostd.pid")))
        out = start.communicate(timeout=30)[0].decode()
        self.assertEqual(start.returncode, 0, out)
        self.assertEqual(h.wait(timeout=1), 0)
        self.assertGreaterEqual(time.time() - t0, 39, "started only when the holder was done")
        self.wait_agents(1)
        self.assertEqual(len(self.loops()), 1)

    def timed_start(self):
        t0 = time.time()
        rc, out = self.do("start")
        self.assertEqual(rc, 0, out)
        self.wait_agents(1)
        return time.time() - t0

    def test_lock_holder_pid_is_a_stranger(self):
        p = subprocess.Popen(["sleep", "100"])
        self.addCleanup(lambda: (p.kill(), p.wait()))
        self.lock_dir("%d\n" % p.pid)
        self.assertLess(self.timed_start(), 10, "not ours: taken at once")

    def test_lock_holder_pid_reused_by_our_own_agent(self):
        """the name alone isn't an action of the script: it would wait for ever"""
        p = subprocess.Popen(["sh", "-c", "sleep 100; :", "/elsewhere/reimu-hostd.py", "run"])
        self.addCleanup(lambda: (p.kill(), p.wait()))
        self.lock_dir("%d\n" % p.pid)
        self.assertLess(self.timed_start(), 10)

    def test_lock_garbage_pid(self):
        self.lock_dir("not a pid\n")
        self.assertLess(self.timed_start(), 10)

    def test_lock_empty_pid_taken_after_a_short_while(self):
        self.lock_dir()
        self.assertLess(self.timed_start(), 15)

    def test_lock_empty_pid_then_its_holder_writes_it(self):
        """between mkdir and echo: the holder is alive, the bound must not break in"""
        p = subprocess.Popen(self.SHELL.split() + ["-c", "sleep 8; :", self.init, "restart"], env=self.env)
        self.addCleanup(lambda: (p.poll() is None and p.kill(), p.wait()))
        d = self.lock_dir()
        start = self.bg("start")
        time.sleep(1)
        open(os.path.join(d, "pid"), "w").write("%d\n" % p.pid)
        time.sleep(5)
        self.assertIsNone(start.poll(), "waits for the holder")
        self.assertTrue(os.path.isdir(d))
        p.wait()
        shutil.rmtree(d)
        self.assertEqual(start.wait(timeout=20), 0)

    def test_supervisor_killed_orphan_agent_no_second_copy(self):
        self.do("start")
        first = self.wait_agents(1)
        os.kill(self.loops()[0], 9)
        rc, out = self.do("start")
        self.assertEqual(rc, 0, out)
        new = self.wait_agents(1, not_in=first)
        time.sleep(0.5)
        self.assertEqual(self.agents(), new, "the orphan gone")

    def test_stop_kills_a_stubborn_agent(self):
        p = os.path.join(self.pybin, "python3")
        open(p, "w").write(FAKE_PY.replace("trap 'exit 0' TERM", "trap '' TERM"))
        self.env["REIMU_STOP_SEC"] = "2"
        self.do("start")
        self.wait_agents(1)
        rc, out = self.do("stop")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.agents(), [])
        self.assertEqual(self.loops(), [])

    def test_term_to_the_loop_stops_the_agent(self):
        self.do("start")
        self.wait_agents(1)
        os.kill(self.loops()[0], 15)
        self.wait_agents(0)
        end = time.time() + 5
        while self.loops() and time.time() < end:
            time.sleep(0.1)
        self.assertEqual(self.loops(), [])
        self.assertIn("restore", open(self.p("restore.log")).read())

    def test_log_kept_small(self):
        log = self.p("var/log/reimu-hostd.log")
        os.makedirs(os.path.dirname(log))
        open(log + ".1", "w").write("older\n")
        with open(log, "wb") as f:
            f.write(b"x" * (1024 * 1024 + 1))
        self.do("start")
        self.wait_agents(1)
        time.sleep(0.3)
        self.assertEqual(os.path.getsize(log + ".1"), 1024 * 1024 + 1)
        self.assertLess(os.path.getsize(log), 1000)
        self.assertIn("connected", open(log).read())

    def test_small_log_appended(self):
        log = self.p("var/log/reimu-hostd.log")
        os.makedirs(os.path.dirname(log))
        open(log, "w").write("before\n")
        self.do("start")
        self.wait_agents(1)
        time.sleep(0.3)
        self.assertTrue(open(log).read().startswith("before\n"))
        self.assertFalse(os.path.exists(log + ".1"))

    def test_without_setsid(self):
        d = bin_without(("systemctl", "journalctl", "setsid"))
        self.env["PATH"] = self.pybin + ":" + d
        rc, out = self.do("start")
        self.assertEqual(rc, 0, out)
        self.wait_agents(1)
        self.assertEqual(self.do("stop")[0], 0)
        self.assertEqual(self.agents(), [])

    def test_with_setsid_its_own_session(self):
        self.do("start")
        self.wait_agents(1)
        loop = self.loops()[0]
        self.assertEqual(os.getsid(loop), loop)

    def test_no_python_says_so(self):
        d = bin_without(("python3", "systemctl", "journalctl"))
        self.env["PATH"] = d
        rc, out = sh("PATH=%s; export PATH; %s %s start" % (d, self.SHELL, self.init), env=self.env)
        self.assertEqual(rc, 5, out)
        self.assertIn("python3", out)

    def test_syntax(self):
        rc, out = sh(self.SHELL.split() + ["-n", self.init])
        self.assertEqual(rc, 0, out)

    def test_unknown_action(self):
        rc, out = self.do("frobnicate")
        self.assertEqual(rc, 2, out)
        self.assertIn("start", out)


class InitScriptDash(InitScript):
    SHELL = "dash"


class InitScriptBashPosix(InitScript):
    SHELL = "bash --posix"


class InitScriptBusybox(InitScript):
    SHELL = "busybox sh"


if __name__ == "__main__":
    import warnings
    warnings.simplefilter("ignore", ResourceWarning)
    unittest.main()
