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
FILES = ("reimu-hostd.py", "reimu-hostd.service", "reimu-hostd.conf")


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
        for n, body in (("systemctl", 'echo "$*" >> "$REIMU_ROOT/systemctl.log"; [ "$1" = is-active ] && echo active; exit 0'),
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


if __name__ == "__main__":
    import warnings
    warnings.simplefilter("ignore", ResourceWarning)
    unittest.main()
