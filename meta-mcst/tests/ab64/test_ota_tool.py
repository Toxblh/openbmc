#!/usr/bin/env python3
"""reimu-ota against a local HTTP server: the list, the download, and everything
that must leave nothing for reimu-abd to install."""
import hashlib, http.server, os, subprocess, sys, tempfile, threading

TOOL = "/work/openbmc/meta-mcst/meta-ab64/recipes-mcst/reimu-ab/files/reimu-ota"
TAG = "reimu-2.0-beta2"
BLOB = os.urandom(300000)
routes = {}
seen = []


class H(http.server.BaseHTTPRequestHandler):
    def _go(self, send_body):
        seen.append(self.path)
        code, body, length = routes.get(self.path, (404, b"", None))
        self.send_response(code)
        self.send_header("Content-Length", str(length if length is not None else len(body)))
        self.end_headers()
        if send_body:
            self.wfile.write(body)

    def do_GET(self):
        self._go(True)

    def do_HEAD(self):
        self._go(False)

    def log_message(self, *a):
        pass


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
URL = "http://127.0.0.1:%d" % srv.server_port
bad = 0


def check(c, what):
    global bad
    bad += not c
    print("  %s %s" % ("PASS" if c else "FAIL", what))


def good():
    routes.clear()
    routes["/api"] = (200, b'[{"tag_name":"x"}]', None)
    routes["/dl/%s/%s.tar" % (TAG, TAG)] = (200, BLOB, None)
    routes["/dl/%s/%s.tar.sha256" % (TAG, TAG)] = (200, (hashlib.sha256(BLOB).hexdigest() + "  x.tar\n").encode(), None)


def run(d, *args, proto="=http"):
    conf = d + "/conf"
    open(conf, "w").write('RUN=%s/run WORK=%s/work IMAGES=%s/images\nAPI=%s/api\nDL=%s/dl\nPROTO="%s"\n' % (d, d, d, URL, URL, proto))
    r = subprocess.run(["sh", TOOL] + list(args), capture_output=True, text=True, env=dict(os.environ, REIMU_OTA_CONF=conf), timeout=60)
    return r.returncode, r.stdout + r.stderr


def images(d):
    return sorted(os.listdir(d + "/images")) if os.path.isdir(d + "/images") else []


def case_fetch_refused(name, want, change=None, tag=TAG):
    good()
    if change:
        change()
    with tempfile.TemporaryDirectory() as d:
        rc, o = run(d, "fetch", tag)
        check(rc != 0 and want in o and not images(d) and not os.path.exists(d + "/work"), "%s (rc %d: %s)" % (name, rc, o.strip().splitlines()[-1] if o.strip() else ""))


with tempfile.TemporaryDirectory() as d:
    good()
    rc, o = run(d, "check")
    check(rc == 0 and open(d + "/run/releases.json").read().startswith("["), "check: the list saved (rc %d)" % rc)
    routes["/api"] = (403, b"{}", None)
    rc, o = run(d, "check")
    check(rc != 0 and "лимит" in o and open(d + "/run/releases.json").read().startswith("["), "check: an error of the server said, the old list kept (%s)" % o.strip())
    routes["/api"] = (200, b"<html>captive portal</html>", None)
    rc, o = run(d, "check")
    check(rc != 0 and "неожиданный" in o, "check: not a list refused (%s)" % o.strip())
    rc, o = run(d, "check", proto="=https")
    check(rc != 0, "check: http refused by default, https only (%s)" % o.strip())

with tempfile.TemporaryDirectory() as d:
    good()
    rc, o = run(d, "fetch", TAG)
    p = d + "/images/ota-%s.tar" % TAG
    check(rc == 0 and os.path.exists(p) and open(p, "rb").read() == BLOB, "fetch: the file whole in IMAGES (rc %d: %s)" % (rc, o.strip().splitlines()[-1]))
    check("PROGRESS 100" in o, "fetch: progress for the page")
    check("cannot open" not in o and "No such file" not in o, "fetch: no shell errors for the page to show as a message")
    check(not os.path.exists(d + "/work"), "fetch: nothing left behind")

K = "/dl/%s/%s.tar" % (TAG, TAG)
S = K + ".sha256"
case_fetch_refused("wrong checksum", "не совпала", lambda: routes.__setitem__(K, (200, BLOB[:-1] + b"x", None)))
case_fetch_refused("no checksum file", "не скачана", lambda: routes.pop(S))
case_fetch_refused("checksum file with junk", "неожиданный", lambda: routes.__setitem__(S, (200, b"<html>\n", None)))
case_fetch_refused("no image file", "не скачан", lambda: routes.pop(K))
case_fetch_refused("connection cut in the middle", "не скачан", lambda: routes.__setitem__(K, (200, BLOB[:1000], len(BLOB))))
n = len(seen)
case_fetch_refused("a path in the tag", "Неверное", tag="reimu-1/../../x")
case_fetch_refused("a shell word in the tag", "Неверное", tag="reimu-1;reboot")
case_fetch_refused("a tag not of ours", "Неверное", tag="reimu-webdev-20261019")
check(len(seen) == n, "refused tags: nothing asked of the server")
print("%d failed" % bad)
sys.exit(1 if bad else 0)
