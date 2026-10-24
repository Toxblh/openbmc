#!/usr/bin/env python3
"""Redfish and IPMI of REIMU-5564 in QEMU: the new A/B firmware (kernel 6.18)
against the stock firmware (REIMU 1.0.2, kernel 5.10), check by check.

    test_ipmi_redfish_qemu.py WORKDIR [stock] [new] [GROUP...]

WORKDIR as for test_ab_qemu.py: ab.mtd (the A/B build with the real U-Boot,
see fetch.sh). The stock image is put together from the dump of the real BMC
(STOCK, default ~/git/new-reimu/backup: u-boot.bin u-boot-env.bin kernel.bin
rofs.bin) with an erased rwfs: factory settings, as a fresh A/B image, and
nothing of the real board's configuration in QEMU. Both images get the same
checks; the table at the end puts them side by side. The test fails when the
new firmware does worse than the stock one on anything the stock one can do
(KNOWN lists the conscious differences), or fails a check that must pass on
the new firmware whatever the stock one does (MUST: no crash, no leak, a
firmware upload that can't brick the BMC).

Groups (all by default): redfish, accounts, crawl, ipmi, sol, power, stress,
update, host, restart (kill -9 of every daemon the BMC needs, 3 times in a
row: back, and the BMC answers), reset (the BMC reboots: last).

IPMI over LAN goes through UDP 127.0.0.1:AB_IPMI (default 2423) -> 623 of
the guest. ipmitool runs on this machine if it has one, otherwise in the
docker container IPMI_DOCKER (default "reimu") against host.docker.internal;
SOL needs the latter (a socat relay there gives ipmitool port 623, which it
insists on for the SOL payload). Ports: AB_SSH, AB_HTTPS, AB_IPMI, RQ as for
test_ab_qemu.py. IR_ATTACH=1: use an already running BMC on those ports
(one image name on the command line) instead of booting one.

"platform" on the command line: the new firmware with the platform emulated
(test_fru_sensors_qemu.py's Platform, PNS=pns.bin FRU=fru.bin in WORKDIR):
the board in Redfish Chassis with its sensors and in IPMI fru print / sdr,
through platform power on, off, 3 cycles and a BMC reboot.

Nothing leaves QEMU but through the forwards: no real BMC is touched.
"""
import base64, concurrent.futures, http.client, json, os, re, shutil, socket, ssl, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("AB_IPMI", "2423")
ARGS = sys.argv[2:]
sys.argv = sys.argv[:2]
import test_ab_qemu as T  # noqa: E402  (reads WORKDIR, the ports, RQ)

W, SSH, HTTPS, IPMI = T.W, T.SSH, T.HTTPS, T.IPMI
STOCK = T.STOCK
PLATFORM = "platform" in ARGS
IMAGES = [a for a in ARGS if a in ("stock", "new")] or ([] if PLATFORM else ["stock", "new"])
GROUPS = [a for a in ARGS if a not in ("stock", "new", "platform")]
ATTACH = os.environ.get("IR_ATTACH") == "1"
USER, PASS = "root", "0penBmc"
DOCKER = os.environ.get("IPMI_DOCKER", "reimu")

# key -> why the new firmware may do worse there (or differ) than the stock one
KNOWN = {
    "redfish.account.ldap": "LDAP/Kerberos removed from the A/B image for space (conscious)",
    "crawl./redfish/v1/AccountService/LDAP": "LDAP removed",
}
# keys that must pass on the new firmware, whatever the stock one does
MUST = {"redfish.root", "redfish.auth.basic", "redfish.auth.bad_password", "redfish.session.login",
        "redfish.session.logout", "redfish.json.valid", "ipmi.c17.mc_info",
        "ipmi.bad_password", "stress.redfish.mem", "stress.redfish.alive", "stress.sessions.alive",
        "stress.concurrent", "stress.ipmi.alive", "stress.ipmi.mem", "update.junk.rejected",
        "update.static.not_staged", "update.flash_unchanged", "update.alive", "host.bt.dev",
        "host.ipmid.active", "host.dbus", "restart.redfish", "restart.ipmi", "restart.login",
        "restart.ipmi_chassis", "restart.sol", "reset.back", "reset.redfish", "reset.ipmi", "crashes"}

R = {}       # image -> key -> (ok, detail)   ok: True / False / None (information)
CUR = None   # image being tested


def rec(key, ok, detail=""):
    ok = None if ok is None else bool(ok)
    R.setdefault(CUR, {})[key] = (ok, str(detail)[:160])
    print("  %-4s %-34s %s" % ({True: "ok", False: "FAIL", None: "info"}[ok], key, str(detail)[:120]), flush=True)
    return ok


# ------------------------------------------------------------------ Redfish
CTX = ssl.create_default_context()
CTX.check_hostname = False
CTX.verify_mode = ssl.CERT_NONE


def rf(method, path, body=None, auth=(USER, PASS), token=None, headers=None, timeout=60, raw=False):
    """-> (status, headers dict, JSON or text); status 0 when the connection failed"""
    h = {"Accept": "application/json"}
    if token:
        h["X-Auth-Token"] = token
    elif auth:
        h["Authorization"] = "Basic " + base64.b64encode(("%s:%s" % auth).encode()).decode()
    if body is not None and not raw:
        body = json.dumps(body)
        h["Content-Type"] = "application/json"
    h.update(headers or {})
    try:
        c = http.client.HTTPSConnection("127.0.0.1", HTTPS, context=CTX, timeout=timeout)
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        data = r.read()
        hd = {k.lower(): v for k, v in r.getheaders()}
        c.close()
    except (OSError, http.client.HTTPException) as e:
        return 0, {}, repr(e)
    try:
        return r.status, hd, json.loads(data) if data else None
    except ValueError:
        return r.status, hd, data.decode(errors="replace")


def get(path, **kw):
    s, h, j = rf("GET", path, **kw)
    return s, j


def wait_redfish(timeout=300):
    end = time.time() + timeout
    while time.time() < end:
        s, j = get("/redfish/v1/Managers/bmc", timeout=20)
        if s == 200:
            return True
        time.sleep(5)
    return False


def odata_ids(j):
    if isinstance(j, dict):
        for k, v in j.items():
            if k == "@odata.id" and isinstance(v, str):
                yield v
            else:
                yield from odata_ids(v)
    elif isinstance(j, list):
        for v in j:
            yield from odata_ids(v)


# ------------------------------------------------------------------ IPMI
LOCAL_IPMITOOL = shutil.which("ipmitool")


def ipmi_base(cipher=17, user=USER, password=PASS, host=None, port=None):
    if LOCAL_IPMITOOL:
        pre, h = [LOCAL_IPMITOOL], host or "127.0.0.1"
    else:
        pre, h = ["docker", "exec", "-i", DOCKER, "ipmitool"], host or "host.docker.internal"
    return pre + ["-I", "lanplus", "-H", h, "-p", str(port or IPMI), "-U", user, "-P", password,
                  "-C", str(cipher), "-N", "3", "-R", "2"]


def ipmi(*args, cipher=17, timeout=90, **kw):
    try:
        r = subprocess.run(ipmi_base(cipher, **kw) + list(args), capture_output=True, text=True,
                           timeout=timeout, stdin=subprocess.DEVNULL)
        return r.returncode, r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return -1, "timeout"


def wait_ipmi(timeout=240):
    end = time.time() + timeout
    while time.time() < end:
        rc, o = ipmi("raw", "6", "1", timeout=60)
        if rc == 0:
            return True
        time.sleep(5)
    return False


# ------------------------------------------------------------------ BMC helpers
class Attached:
    """an already running BMC (IR_ATTACH=1)"""
    name = os.environ.get("IR_NAME", "attached")  # its RQ/NAME.qmp2 for re-pointing the forwards
    p = None

    def stop(self):
        pass


for m in ("ssh", "out", "wait_ssh", "fwd", "scp"):
    setattr(Attached, m, getattr(T.Bmc, m))


def rss(b, name):
    o = b.out("p=$(pidof %s | cut -d' ' -f1); [ -n \"$p\" ] && echo $p $(grep VmRSS /proc/$p/status | tr -s ' ' | cut -d' ' -f2)" % name)
    try:
        pid, kb = o.split()
        return int(pid), int(kb)
    except ValueError:
        return None, None


def mem_avail(b):
    m = re.search(r"MemAvailable:\s+(\d+)", b.out("cat /proc/meminfo"))
    return int(m.group(1)) if m else 0


def stock_image():
    parts = [("u-boot.bin", 0), ("u-boot-env.bin", 0xe0000), ("kernel.bin", 0x100000), ("rofs.bin", 0x680000)]
    img = os.path.join(W, "stock-1.0.2-factory.mtd")
    if not os.path.exists(img):
        f = bytearray(b"\xff" * 0x4000000)
        for n, o in parts:
            d = open(os.path.join(STOCK, n), "rb").read()
            f[o:o + len(d)] = d
        open(img, "wb").write(f)  # rwfs (0x3000000..) erased: factory settings
    return img


# ------------------------------------------------------------------ Redfish checks
def g_redfish(b):
    s, j = get("/redfish/v1", auth=None)
    rec("redfish.root", s == 200 and isinstance(j, dict) and "RedfishVersion" in j,
        "%s RedfishVersion %s" % (s, j.get("RedfishVersion") if isinstance(j, dict) else j))
    rec("redfish.root.version", None, j.get("RedfishVersion") if isinstance(j, dict) else "")
    s, _ = get("/redfish/v1/Systems", auth=None)
    rec("redfish.auth.required", s == 401, "no credentials: %s" % s)
    s, _ = get("/redfish/v1/Systems")
    rec("redfish.auth.basic", s == 200, "basic auth: %s" % s)
    s, _ = get("/redfish/v1/Systems", auth=(USER, "wrong-password"))
    rec("redfish.auth.bad_password", s == 401, "wrong password: %s" % s)

    # sessions
    s, h, j = rf("POST", "/redfish/v1/SessionService/Sessions", {"UserName": USER, "Password": PASS}, auth=None)
    tok, loc = h.get("x-auth-token"), h.get("location")
    rec("redfish.session.login", s in (200, 201) and bool(tok), "POST Sessions %s, token %s" % (s, bool(tok)))
    s2, _ = get("/redfish/v1/Systems", auth=None, token=tok)
    rec("redfish.session.token", s2 == 200, "GET with the token: %s" % s2)
    s3 = rf("DELETE", loc or "/redfish/v1/SessionService/Sessions/x", auth=None, token=tok)[0]
    s4, _ = get("/redfish/v1/Systems", auth=None, token=tok)
    rec("redfish.session.logout", s3 in (200, 204) and s4 == 401, "DELETE %s, token afterwards %s" % (s3, s4))
    s, h, j = rf("POST", "/redfish/v1/SessionService/Sessions", {"UserName": USER, "Password": "nope"}, auth=None)
    rec("redfish.session.bad_password", s == 401, "POST Sessions, wrong password: %s" % s)
    s, j = get("/redfish/v1/SessionService")
    rec("redfish.sessionservice", s == 200, "SessionTimeout %s" % (j.get("SessionTimeout") if s == 200 else s))

    # systems
    s, j = get("/redfish/v1/Systems/system")
    rec("redfish.system", s == 200 and "PowerState" in j, "PowerState %s" % (j.get("PowerState") if s == 200 else s))
    act = (j.get("Actions", {}) if s == 200 else {}).get("#ComputerSystem.Reset", {})
    vals = act.get("ResetType@Redfish.AllowableValues")
    if not vals and act.get("@Redfish.ActionInfo"):
        s5, ai = get(act["@Redfish.ActionInfo"])
        vals = [v for p in (ai.get("Parameters", []) if s5 == 200 else []) for v in p.get("AllowableValues", [])]
    rec("redfish.system.reset_values", bool(vals), " ".join(vals or []))
    s, j = get("/redfish/v1/Systems/system/ResetActionInfo")
    rec("redfish.system.reset_actioninfo", s == 200, s)
    s, j = get("/redfish/v1/Systems/system")
    rec("redfish.system.boot", s == 200 and "Boot" in j, json.dumps(j.get("Boot", {}))[:100] if s == 200 else s)
    s, h, _ = rf("PATCH", "/redfish/v1/Systems/system",
                 {"Boot": {"BootSourceOverrideTarget": "Pxe", "BootSourceOverrideEnabled": "Once"}})
    s2, j = get("/redfish/v1/Systems/system")
    rec("redfish.system.boot_override", s in (200, 204) and j.get("Boot", {}).get("BootSourceOverrideTarget") == "Pxe",
        "PATCH %s -> %s" % (s, j.get("Boot", {}).get("BootSourceOverrideTarget")))
    rf("PATCH", "/redfish/v1/Systems/system", {"Boot": {"BootSourceOverrideEnabled": "Disabled"}})

    # manager
    s, j = get("/redfish/v1/Managers/bmc")
    fw = j.get("FirmwareVersion") if s == 200 else None
    rec("redfish.manager", s == 200 and bool(fw), "FirmwareVersion %s" % fw)
    ma = (j.get("Actions", {}) if s == 200 else {}).get("#Manager.Reset", {})
    rec("redfish.manager.reset_listed", bool(ma.get("target")), " ".join(ma.get("ResetType@Redfish.AllowableValues", [])))
    for k in ("UUID", "DateTime", "PowerState", "Status"):
        rec("redfish.manager." + k, (s == 200 and k in j) or None, str(j.get(k)) if s == 200 else "")
    s, j = get("/redfish/v1/Managers/bmc/EthernetInterfaces")
    ids = [m["@odata.id"] for m in j.get("Members", [])] if s == 200 else []
    rec("redfish.manager.eth", bool(ids), " ".join(i.rsplit("/", 1)[1] for i in ids))
    ip = ""
    for i in ids[:1]:
        s, e = get(i)
        ip = ",".join(a.get("Address", "") for a in e.get("IPv4Addresses", [])) if s == 200 else ""
        rec("redfish.manager.eth0", s == 200 and "10.0.2." in ip, "IPv4 %s MAC %s DHCP %s" % (
            ip, e.get("MACAddress"), e.get("DHCPv4", {}).get("DHCPEnabled")))
    s, j = get("/redfish/v1/Managers/bmc/NetworkProtocol")
    pr = {k: v.get("ProtocolEnabled") for k, v in j.items() if isinstance(v, dict) and "ProtocolEnabled" in v} if s == 200 else {}
    # (SSH reads False on both firmwares: bmcweb looks for dropbear, the BMC runs OpenSSH)
    rec("redfish.manager.netproto", s == 200 and pr.get("HTTPS") is True, " ".join("%s=%s" % kv for kv in sorted(pr.items())))
    rec("redfish.manager.netproto.ipmi", pr.get("IPMI") is True or None, "IPMI ProtocolEnabled %s" % pr.get("IPMI"))
    rec("redfish.manager.netproto.ntp", s == 200 and "NTP" in j, json.dumps(j.get("NTP", {}))[:100] if s == 200 else "")

    # chassis
    s, j = get("/redfish/v1/Chassis")
    ch = [m["@odata.id"] for m in j.get("Members", [])] if s == 200 else []
    rec("redfish.chassis", s == 200 and bool(ch), " ".join(c.rsplit("/", 1)[1] for c in ch))
    for c in ch[:3]:
        n = c.rsplit("/", 1)[1]
        s, j = get(c)
        rec("redfish.chassis.%s" % n, s == 200, "PowerState %s" % (j.get("PowerState") if s == 200 else s))
        for sub in ("Sensors", "Thermal", "Power"):
            if s == 200 and sub in j:
                s2, k = get(j[sub]["@odata.id"])
                cnt = len(k.get("Members", [])) if "Members" in (k or {}) else \
                    len(k.get("Temperatures", []) + k.get("Fans", [])) if sub == "Thermal" else \
                    len(k.get("Voltages", []) + k.get("PowerSupplies", [])) if sub == "Power" else 0
                rec("redfish.chassis.%s.%s" % (n, sub), s2 == 200, "%s, %d entries" % (s2, cnt))
            else:
                rec("redfish.chassis.%s.%s" % (n, sub), None, "not listed")

    # services
    for name, path in (("update", "/redfish/v1/UpdateService"), ("fwinventory", "/redfish/v1/UpdateService/FirmwareInventory"),
                       ("eventlog", "/redfish/v1/Systems/system/LogServices/EventLog/Entries"),
                       ("logservices", "/redfish/v1/Systems/system/LogServices"),
                       ("bmclog", "/redfish/v1/Managers/bmc/LogServices"),
                       ("certificates", "/redfish/v1/CertificateService"),
                       ("certlocations", "/redfish/v1/CertificateService/CertificateLocations"),
                       ("httpscert", "/redfish/v1/Managers/bmc/NetworkProtocol/HTTPS/Certificates"),
                       ("tasks", "/redfish/v1/TaskService"), ("tasklist", "/redfish/v1/TaskService/Tasks"),
                       ("events", "/redfish/v1/EventService"), ("registries", "/redfish/v1/Registries"),
                       ("schemas", "/redfish/v1/JsonSchemas"), ("metadata", "/redfish/v1/$metadata"),
                       ("odata", "/redfish/v1/odata")):
        s, j = get(path)
        extra = ""
        if isinstance(j, dict) and "Members@odata.count" in j:
            extra = "%d members" % j["Members@odata.count"]
        rec("redfish.svc." + name, s == 200, "%s %s" % (s, extra))
    s, j = get("/redfish/v1/UpdateService")
    rec("redfish.update.pushuri", None, "HttpPushUri %s" % (j.get("HttpPushUri") if s == 200 else s))
    s, j = get("/redfish/v1/UpdateService/FirmwareInventory")
    fws = []
    for m in (j.get("Members", []) if s == 200 else []):
        s2, f = get(m["@odata.id"])
        fws.append("%s=%s" % (m["@odata.id"].rsplit("/", 1)[1], f.get("Version") if s2 == 200 else s2))
    rec("redfish.update.inventory", bool(fws), " ".join(fws))
    s, h, j = rf("POST", "/redfish/v1/Systems/system/LogServices/EventLog/Actions/LogService.ClearLog", {})
    rec("redfish.eventlog.clear", s in (200, 204), s)

    # what reimu-webui (A/B image only; the stock one has phosphor-webui) reads
    # through bmcweb's D-Bus REST besides Redfish
    for n in ("sensors", "pns", "kvm", "journal", "firmware"):
        s, j = get("/xyz/openbmc_project/reimu/" + n)
        # firmware: reimu-firmwared, boards with two BMC chips only (the web UI takes 404 as "none")
        rec("webui." + n, (s == 200) if CUR == "new" and n != "firmware" else None, s)

    # plain HTTP
    try:
        c = socket.create_connection(("127.0.0.1", HTTPS), timeout=15)
        c.sendall(b"GET /redfish/v1 HTTP/1.1\r\nHost: x\r\n\r\n")
        c.settimeout(15)
        d = c.recv(200)
        c.close()
    except OSError as e:
        d = repr(e).encode()
    rec("redfish.plain_http_on_443", None, "answer to plain HTTP: %r" % d[:60])
    p80 = b.out("netstat -tln | grep -cE ':80 '").strip()
    rec("redfish.port80", None, "listening on :80: %s" % ("yes" if p80 not in ("", "0") else "no"))


def g_accounts(b):
    s, j = get("/redfish/v1/AccountService")
    rec("redfish.account", s == 200, "MinPasswordLength %s MaxPasswordLength %s" % (
        j.get("MinPasswordLength"), j.get("MaxPasswordLength")) if s == 200 else s)
    orig = {k: j.get(k) for k in ("AccountLockoutThreshold", "AccountLockoutDuration")} if s == 200 else {}
    rec("redfish.account.lockout_policy", s == 200 and None not in orig.values(), json.dumps(orig))
    rec("redfish.account.ldap", s == 200 and "LDAP" in j, "LDAP %s" % ("present" if s == 200 and "LDAP" in j else "absent"))
    s, j = get("/redfish/v1/AccountService/Accounts")
    names = [m["@odata.id"].rsplit("/", 1)[1] for m in j.get("Members", [])] if s == 200 else []
    rec("redfish.account.list", USER in names, " ".join(names))
    s, j = get("/redfish/v1/AccountService/Roles")
    roles = [m["@odata.id"].rsplit("/", 1)[1] for m in j.get("Members", [])] if s == 200 else []
    rec("redfish.account.roles", {"Administrator", "Operator", "ReadOnly"} <= set(roles), " ".join(roles))

    u, pw, pw2 = "rtest", "Rq7#xLm2pTz", "Zp4%wNk8sVb"
    rf("DELETE", "/redfish/v1/AccountService/Accounts/" + u)
    s, h, j = rf("POST", "/redfish/v1/AccountService/Accounts", {"UserName": u, "Password": pw, "RoleId": "Operator", "Enabled": True})
    rec("redfish.account.create", s in (200, 201), s)
    time.sleep(2)
    s, _ = get("/redfish/v1/Systems/system", auth=(u, pw))
    rec("redfish.account.login_new", s == 200, "the new Operator logs in: %s" % s)
    s = rf("PATCH", "/redfish/v1/AccountService/Accounts/" + u, {"RoleId": "ReadOnly"})[0]
    s2, j = get("/redfish/v1/AccountService/Accounts/" + u)
    rec("redfish.account.modify_role", s in (200, 204) and j.get("RoleId") == "ReadOnly", "PATCH %s -> %s" % (s, j.get("RoleId")))
    s = rf("PATCH", "/redfish/v1/Systems/system", {"Boot": {"BootSourceOverrideEnabled": "Disabled"}}, auth=(u, pw))[0]
    rec("redfish.account.readonly_denied", s in (401, 403), "ReadOnly PATCHes a system: %s" % s)
    s = rf("PATCH", "/redfish/v1/AccountService/Accounts/" + u, {"Password": pw2})[0]
    time.sleep(2)
    s2, _ = get("/redfish/v1/Systems/system", auth=(u, pw2))
    s3, _ = get("/redfish/v1/Systems/system", auth=(u, pw))
    rec("redfish.account.password", s in (200, 204) and s2 == 200 and s3 == 401, "PATCH %s, new %s, old %s" % (s, s2, s3))
    s = rf("POST", "/redfish/v1/AccountService/Accounts", {"UserName": "rshort", "Password": "a", "RoleId": "ReadOnly"})[0]
    rec("redfish.account.weak_password", s >= 400, "1-character password: %s" % s)
    rf("DELETE", "/redfish/v1/AccountService/Accounts/rshort")

    # lockout: 3 wrong passwords lock the account for a while
    s = rf("PATCH", "/redfish/v1/AccountService", {"AccountLockoutThreshold": 3, "AccountLockoutDuration": 60})[0]
    for i in range(4):
        get("/redfish/v1/Systems/system", auth=(u, "bad-%d" % i))
    s2, _ = get("/redfish/v1/Systems/system", auth=(u, pw2))
    rec("redfish.account.lockout", s in (200, 204) and s2 == 401, "PATCH policy %s, right password after 4 wrong: %s" % (s, s2))
    if orig and None not in orig.values():
        rf("PATCH", "/redfish/v1/AccountService", orig)
    s = rf("DELETE", "/redfish/v1/AccountService/Accounts/" + u)[0]
    time.sleep(2)
    s2, _ = get("/redfish/v1/Systems/system", auth=(u, pw2))
    rec("redfish.account.delete", s in (200, 204) and s2 == 401, "DELETE %s, login afterwards %s" % (s, s2))


def g_crawl(b):
    """every resource reachable from the service root (bounded), JSON sanity"""
    seen, todo, bad, res = set(), ["/redfish/v1"], [], {}
    while todo and len(seen) < 400:
        u = todo.pop(0).split("#")[0].rstrip("/") or "/redfish/v1"
        if u in seen or not u.startswith("/redfish/v1"):
            continue
        seen.add(u)
        # one of each kind of log entry / schema / registry is enough
        if re.search(r"/(Entries|JsonSchemas|Registries|Tasks|Sessions|Certificates)/[^/]+$", u) and \
                sum(1 for x in seen if x.rsplit("/", 1)[0] == u.rsplit("/", 1)[0]) > 2:
            continue
        s, j = get(u, timeout=60)
        res[u] = s
        if s == 200 and isinstance(j, dict):
            oid = (j.get("@odata.id") or "").rstrip("/")
            if not ("$metadata" in u or "odata" == u.rsplit("/", 1)[1]) and (oid != u or "@odata.type" not in j):
                bad.append(u)
            todo.extend(x for x in odata_ids(j) if x.split("#")[0].rstrip("/") not in seen)
        elif s == 200 and not ("$metadata" in u or u.endswith("/odata")):
            bad.append(u + " (not JSON)")
    ok = sorted(u for u, s in res.items() if s == 200)
    rec("redfish.json.valid", not bad, "%d resources, %d bad: %s" % (len(res), len(bad), " ".join(bad[:4])))
    rec("crawl.count", None, "%d resources answer 200, %d other" % (len(ok), len(res) - len(ok)))
    for u in ok:
        R[CUR]["crawl." + re.sub(r"/(Entries|JsonSchemas|Registries|Tasks|Sessions|Certificates|FirmwareInventory)/[^/]+$", r"/\1/*", u)] = (True, "200")
    for u, s in res.items():
        if s != 200:
            R[CUR].setdefault("crawl." + u, (False, str(s)))


# ------------------------------------------------------------------ IPMI checks
def g_ipmi(b):
    for c in (17, 3):
        p = "ipmi.c%d." % c
        rc, o = ipmi("mc", "info", cipher=c)
        m = re.search(r"Firmware Revision\s*:\s*(\S+)", o)
        rec(p + "mc_info", rc == 0 and "IPMI Version" in o, "fw %s available %s" % (
            m.group(1) if m else "-", (re.search(r"Device Available\s*:\s*(\S+)", o) or [None, "-"])[1]) if rc == 0 else o.strip()[-80:])
        rc, o = ipmi("raw", "6", "1", cipher=c)
        rec(p + "raw_device_id", rc == 0 and len(o.split()) >= 11, o.strip()[:60])
    rc, o = ipmi("mc", "guid")
    rec("ipmi.mc_guid", rc == 0, o.strip()[-80:])
    rc, o = ipmi("chassis", "status")
    rec("ipmi.chassis_status", rc == 0 and "System Power" in o, (re.search(r"System Power\s*:\s*(\S+)", o) or [None, o[-60:]])[1])
    rc, o = ipmi("chassis", "bootdev", "pxe")
    rc2, o2 = ipmi("chassis", "bootparam", "get", "5")
    rec("ipmi.bootdev", rc == 0 and rc2 == 0 and "Force PXE" in o2, (o + o2).strip().splitlines()[0] if (o + o2).strip() else "")
    ipmi("chassis", "bootdev", "none")
    rc, o = ipmi("sdr", "list")
    rec("ipmi.sdr_list", rc == 0, "%d records" % len(o.strip().splitlines()) if rc == 0 else o.strip()[-80:])
    rc, o = ipmi("sensor", "list", timeout=300)
    rec("ipmi.sensor_list", rc == 0, "%d sensors" % len(o.strip().splitlines()) if rc == 0 else o.strip()[-80:])
    rc, o = ipmi("fru", "print")
    rec("ipmi.fru_print", "FRU Device Description" in o, "%d FRU devices (QEMU: no FRU EEPROM)" % o.count("FRU Device Description"))
    rc, o = ipmi("sel", "info")
    rec("ipmi.sel_info", rc == 0 and "SEL Information" in o, (re.search(r"Entries\s*:\s*(\d+)", o) or [None, "-"])[1] + " entries")
    rc, o = ipmi("sel", "list")
    rec("ipmi.sel_list", rc == 0, o.strip().splitlines()[0][:80] if o.strip() else "")
    rc, o = ipmi("sel", "clear")
    rec("ipmi.sel_clear", rc == 0, o.strip()[:80])
    rc, o = ipmi("user", "list", "1")
    rec("ipmi.user_list", rc == 0 and re.search(r"^1\s+root", o, re.M) is not None, "root listed" if rc == 0 else o[-80:])
    rc, o = ipmi("lan", "print", "1")
    ip = re.search(r"IP Address\s+:\s*(\S+)", o)
    rec("ipmi.lan_print", rc == 0 and ip is not None, "IP %s, %s" % (ip.group(1) if ip else "-", (re.search(r"IP Address Source\s*:\s*(.*)", o) or [None, ""])[1].strip()))
    rc, o = ipmi("channel", "info", "1")
    rec("ipmi.channel_info", rc == 0 and "802.3 LAN" in o, (re.search(r"Session Support\s*:\s*(\S+)", o) or [None, o[-60:]])[1])
    rc, o = ipmi("channel", "getciphers", "ipmi", "1")
    rec("ipmi.ciphers", rc == 0, " ".join(re.findall(r"^(\d+)\s", o, re.M)))
    rc, o = ipmi("sol", "info", "1")
    rec("ipmi.sol_info", rc == 0 and "Enabled" in o, (re.search(r"Enabled\s*:\s*(\S+)", o) or [None, o[-60:]])[1])
    rc, o = ipmi("session", "info", "all")
    rec("ipmi.session_info", "session handle" in o, (re.search(r"slot count\s*:\s*(\d+)", o) or [None, "-"])[1] + " slots")
    rc, o = ipmi("mc", "info", password="wrong-password")
    rec("ipmi.bad_password", rc != 0, "wrong password refused" if rc else "ACCEPTED")
    rc, o = ipmi("dcmi", "discover")
    rec("ipmi.dcmi", None, "dcmi discover rc %d" % rc)

    # a user: set name, password, privilege, enable; log in with it; remove
    uid = "5"
    # (in a row, as a provisioning script does: netipmid used to keep a stale
    # user table when ipmid rewrote it within the same second)
    r1 = ipmi("user", "set", "name", uid, "itest")[0]
    r3 = ipmi("channel", "setaccess", "1", uid, "link=on", "ipmi=on", "callin=on", "privilege=4")[0]
    r4 = ipmi("user", "enable", uid)[0]
    r2 = ipmi("user", "set", "password", uid, "Ip7#qWx2kLm")[0]
    rc, o = ipmi("mc", "info", user="itest", password="Ip7#qWx2kLm")
    rec("ipmi.user_set_password", r1 == r2 == r3 == r4 == 0 and rc == 0, "set %s/%s/%s/%s, login %s" % (r1, r2, r3, r4, rc))
    rc, o = ipmi("mc", "info", user="itest", password="Ip7#qWx2kLX")
    rec("ipmi.user_bad_password", rc != 0, "wrong password for the new user: %s" % ("refused" if rc else "ACCEPTED"))
    ipmi("user", "disable", uid)
    ipmi("raw", "6", "0x45", uid, *(["0"] * 16))  # name cleared: the user goes
    s, j = get("/redfish/v1/AccountService/Accounts/itest")
    rec("ipmi.user_in_redfish", None, "the IPMI user in Redfish after removal: %s" % s)
    rf("DELETE", "/redfish/v1/AccountService/Accounts/itest")

    # session limit: shells kept open, each holds one session
    procs = []
    for i in range(18):
        p = subprocess.Popen(ipmi_base() + ["shell"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True)
        p.stdin.write("raw 6 1\n")
        p.stdin.flush()
        procs.append(p)
        time.sleep(0.3)
    time.sleep(10)
    held = b.out("busctl tree --list xyz.openbmc_project.Ipmi.Channel.eth0 | grep -c '/session/eth0/[^0]'").strip()
    rc, o = ipmi("raw", "6", "1", timeout=60)
    for p in procs:
        try:
            p.stdin.write("exit\n")
            p.stdin.close()
        except OSError:
            pass
    for p in procs:
        try:
            p.wait(30)
        except subprocess.TimeoutExpired:
            p.kill()
    rec("ipmi.session_limit", None, "18 shells: %s sessions held, one more: %s" % (held, "refused" if rc else "taken"))
    time.sleep(5)
    ok = wait_ipmi(120)
    rec("ipmi.after_session_limit", ok, "a new session after the shells closed: %s" % ok)


def g_sol(b):
    if LOCAL_IPMITOOL:
        rec("ipmi.sol_activate", None, "needs the docker relay (port 623)")
        return
    relay = "socat UDP4-LISTEN:623,bind=127.0.0.2,fork,reuseaddr UDP4:host.docker.internal:%d" % IPMI
    sh = ("pkill -x socat; %s & R=$!; sleep 1; "
          "I='ipmitool -I lanplus -H 127.0.0.2 -U %s -P %s -C 17'; $I sol deactivate >/dev/null 2>&1; "
          "(sleep 8; printf '\\r~.') | timeout 30 $I sol activate 2>&1; echo rc=$?; "
          "$I sol deactivate >/dev/null 2>&1; kill $R") % (relay, USER, PASS)
    r = subprocess.run(["docker", "exec", DOCKER, "sh", "-c", sh], capture_output=True, text=True, timeout=120)
    o = r.stdout + r.stderr
    rec("ipmi.sol_activate", "SOL Session operational" in o, " ".join(l for l in o.splitlines() if "tcgetattr" not in l)[:100])
    who = b.out("grep -c '@obmc-console' /proc/net/unix; for f in /proc/[0-9]*/fd/*; do case $(readlink $f) in /dev/ttyS0) cat ${f%/fd/*}/comm;; esac; done 2>/dev/null | sort -u | tr '\\n' ' '").split()
    rec("ipmi.sol_console", None, "obmc-console socket %s, host UART ttyS0 held by %s" % (
        "up" if who and who[0] != "0" else "absent", " ".join(who[1:]) or "nobody"))


# ------------------------------------------------------------------ power
def host_state(b):
    o = b.out("busctl get-property xyz.openbmc_project.State.Host /xyz/openbmc_project/state/host0 "
              "xyz.openbmc_project.State.Host CurrentHostState; busctl get-property xyz.openbmc_project.State.Chassis "
              "/xyz/openbmc_project/state/chassis0 xyz.openbmc_project.State.Chassis CurrentPowerState")
    return " ".join(x.rsplit(".", 1)[-1].strip('"') for x in o.split() if "." in x)


def g_power(b):
    def watch(secs):
        seen = []
        end = time.time() + secs
        while time.time() < end:
            s, j = get("/redfish/v1/Systems/system")
            st = "%s/%s" % (j.get("PowerState") if s == 200 else s, host_state(b))
            if not seen or seen[-1] != st:
                seen.append(st)
            time.sleep(4)
        return " > ".join(seen)
    for rt in ("On", "GracefulRestart", "ForceOff"):
        s, h, j = rf("POST", "/redfish/v1/Systems/system/Actions/ComputerSystem.Reset", {"ResetType": rt})
        rec("power.redfish.%s" % rt, s in (200, 204), "POST %s; Redfish/host/chassis: %s" % (s, watch(40)))
    for cmd in ("on", "cycle", "off"):
        rc, o = ipmi("chassis", "power", cmd)
        time.sleep(15)
        rc2, o2 = ipmi("chassis", "power", "status")
        rec("power.ipmi.%s" % cmd, rc == 0, "%s; status: %s; host %s" % (o.strip()[-50:], o2.strip()[-30:], host_state(b)))
    rec("power.alive", b.ssh("true").returncode == 0 and wait_redfish(60), "BMC fine after the power actions")


# ------------------------------------------------------------------ stress
def g_stress(b):
    pid0, r0 = rss(b, "bmcweb")
    m0 = mem_avail(b)
    paths = ["/redfish/v1", "/redfish/v1/Systems/system", "/redfish/v1/Managers/bmc",
             "/redfish/v1/Chassis", "/redfish/v1/AccountService/Accounts"]
    bad = 0
    t = time.time()
    for i in range(500):
        if get(paths[i % len(paths)])[0] != 200:
            bad += 1
    dt = time.time() - t
    pid1, r1 = rss(b, "bmcweb")
    rec("stress.redfish.alive", bad == 0 and pid1 == pid0, "500 GETs in %d s, %d failed, bmcweb pid %s -> %s" % (dt, bad, pid0, pid1))
    # warm-up allocations are fine, a leak is not: a second round must not grow it much
    for i in range(300):
        get(paths[i % len(paths)])
    pid2, r2 = rss(b, "bmcweb")
    rec("stress.redfish.mem", r2 is not None and r1 is not None and r2 - r1 < 1024,
        "bmcweb RSS %s -> %s (500) -> %s KiB (+300)" % (r0, r1, r2))

    # many sessions: how many are taken, and the memory they keep
    toks, refused = [], None
    for i in range(120):
        s, h, j = rf("POST", "/redfish/v1/SessionService/Sessions", {"UserName": USER, "Password": PASS}, auth=None)
        if s in (200, 201):
            toks.append(h.get("location"))
        else:
            refused = s
            break
    pid3, r3 = rss(b, "bmcweb")
    for l in toks:
        rf("DELETE", l)
    pid4, r4 = rss(b, "bmcweb")
    rec("stress.sessions", None, "%d sessions opened%s; RSS %s -> %s -> %s KiB after DELETE" % (
        len(toks), ", then %s" % refused if refused else "", r2, r3, r4))
    rec("stress.sessions.alive", pid4 == pid0 and get("/redfish/v1/Managers/bmc")[0] == 200, "bmcweb pid %s -> %s" % (pid0, pid4))

    # concurrent clients
    # (a connection that hasn't sent its request 5 s after the accept is
    # dropped by bmcweb: with 8 TLS handshakes at once on the emulated ARM11
    # a few are; an error answer or a crash is what must not happen)
    def worker(n):
        return [get(paths[(i + n) % len(paths)], timeout=120)[0] for i in range(20)]
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        sts = [x for r in ex.map(worker, range(8)) for x in r]
    pid5, r5 = rss(b, "bmcweb")
    errs = [x for x in sts if x not in (200, 0)]
    rec("stress.concurrent", not errs and pid5 == pid0 and sts.count(0) <= len(sts) // 10,
        "8 clients x 20 GETs: %d ok, %d dropped connections, errors %s, RSS %s KiB" % (
            sts.count(200), sts.count(0), sorted(set(errs)) or "none", r5))

    # IPMI sessions in a row
    n = 300 if os.environ.get("IR_QUICK") else 500
    ip0, ir0 = rss(b, "netipmid")
    hp0, hr0 = rss(b, "ipmid")
    t = time.time()
    if LOCAL_IPMITOOL:
        fails = sum(1 for i in range(n) if ipmi("raw", "6", "1", timeout=60)[0] != 0)
    else:
        base = " ".join(ipmi_base()[4:])  # after "docker exec -i reimu"
        sh = "f=0; for i in $(seq %d); do %s raw 6 1 >/dev/null 2>&1 || f=$((f+1)); done; echo $f" % (n, base)
        r = subprocess.run(["docker", "exec", DOCKER, "sh", "-c", sh], capture_output=True, text=True, timeout=7200)
        fails = int(r.stdout.strip() or n)
    dt = time.time() - t
    ip1, ir1 = rss(b, "netipmid")
    hp1, hr1 = rss(b, "ipmid")
    rec("stress.ipmi.alive", fails <= n // 100 and ip1 == ip0 and hp1 == hp0,
        "%d sessions in %d s, %d failed; pids netipmid %s->%s ipmid %s->%s" % (n, dt, fails, ip0, ip1, hp0, hp1))
    rec("stress.ipmi.mem", None not in (ir0, ir1, hr0, hr1) and ir1 - ir0 < 1024 and hr1 - hr0 < 1024,
        "RSS KiB netipmid %s -> %s, ipmid %s -> %s" % (ir0, ir1, hr0, hr1))
    m1 = mem_avail(b)
    rec("stress.memavailable", None, "MemAvailable %d -> %d MiB" % (m0 // 1024, m1 // 1024))
    u = b.out("for s in bmcweb phosphor-ipmi-host phosphor-ipmi-net@eth0; do systemctl show -p Restart -p NRestarts $s | tr '\\n' ' '; echo; done")
    rec("stress.restart_policy", None, " | ".join(l.strip() for l in u.splitlines()))


# ------------------------------------------------------------------ update
def mtd_sums(b):
    o = b.out("for m in /sys/class/mtd/mtd*/name; do n=$(cat $m); d=${m%/name}; d=${d##*/}; "
              "case $n in rwfs|bmc|sys) ;; *) echo $n $(md5sum < /dev/$d | cut -c1-12);; esac; done", timeout=600)
    return dict(l.split() for l in o.splitlines() if len(l.split()) == 2)


def g_update(b):
    fwdir = os.path.join(W, "fw-test")
    os.makedirs(fwdir, exist_ok=True)
    junk = os.path.join(fwdir, "junk.bin")
    if not os.path.exists(junk):
        open(junk, "wb").write(os.urandom(1 << 20))
    # a stock-format (static layout) BMC image: MANIFEST + image-kernel/rofs/rwfs/u-boot
    static = os.path.join(fwdir, "static.tar")
    if not os.path.exists(static):
        st = os.path.join(fwdir, "st")
        os.makedirs(st, exist_ok=True)
        for src, dst in (("kernel.bin", "image-kernel"), ("rofs.bin", "image-rofs"), ("u-boot.bin", "image-u-boot")):
            shutil.copyfile(os.path.join(STOCK, src), os.path.join(st, dst))
        open(os.path.join(st, "image-rwfs"), "wb").close()
        open(os.path.join(st, "MANIFEST"), "w").write(
            "purpose=xyz.openbmc_project.Software.Version.VersionPurpose.BMC\nversion=reimu-1.0.2-regress-test\n"
            "KeyType=OpenBMC\nHashType=RSA-SHA256\nMachineName=reimu-5564\n")
        subprocess.run(["tar", "--format", "ustar", "-cf", static, "-C", st, "MANIFEST", "image-kernel", "image-rofs",
                        "image-rwfs", "image-u-boot"], check=True, env=dict(os.environ, COPYFILE_DISABLE="1"))
    before = mtd_sums(b)
    pid0 = rss(b, "bmcweb")[0]
    s, h, j = rf("POST", "/redfish/v1/UpdateService", open(junk, "rb").read(), raw=True,
                 headers={"Content-Type": "application/octet-stream"}, timeout=300)
    msg = json.dumps(j)[:300] if not isinstance(j, str) else j[:100]
    rec("update.junk.rejected", 400 <= s < 500, "%s %s" % (s, re.search(r'"message": "([^"]*)', msg).group(1) if '"message"' in msg else msg[:80]))
    t = time.time()
    s, h, j = rf("POST", "/redfish/v1/UpdateService", open(static, "rb").read(), raw=True,
                 headers={"Content-Type": "application/octet-stream"}, timeout=400)
    msg = json.dumps(j) if not isinstance(j, str) else j
    m = re.search(r'"message": "([^"]*)', msg)
    rec("update.static.answer", None, "%s in %d s: %s" % (s, time.time() - t, m.group(1) if m else msg[:90]))
    time.sleep(20)
    staged = b.out("ls /run/initramfs | grep '^image-' | tr '\\n' ' '; ls /tmp/images 2>/dev/null | wc -l").split()
    acts = b.out("for p in $(busctl tree --list xyz.openbmc_project.Software.BMC.Updater | grep 'software/'); do "
                 "echo $p $(busctl get-property xyz.openbmc_project.Software.BMC.Updater $p xyz.openbmc_project.Software.Activation Activation 2>/dev/null | cut -d. -f6); done")
    rec("update.static.result", None, "staged: %s; versions: %s" % (" ".join(staged[:-1]) or "nothing", " ".join(acts.split())))
    if CUR == "new":
        rec("update.static.not_staged", not staged[:-1] and 400 <= s < 500,
            "A/B: a static-layout image refused (%s), nothing in /run/initramfs" % s)
    else:
        # the stock firmware takes it (staged for the flasher at the next reboot): undo
        b.ssh("rm -f /run/initramfs/image-*")
        for p in acts.splitlines():
            if p.split()[1:] == ["Active"] and "regress" in b.out("busctl get-property xyz.openbmc_project.Software.BMC.Updater %s xyz.openbmc_project.Software.Version Version" % p.split()[0]):
                b.ssh("busctl call xyz.openbmc_project.Software.BMC.Updater %s xyz.openbmc_project.Object.Delete Delete" % p.split()[0])
        rec("update.static.stock", None, "stock: %s" % ("staged for the reboot flasher (removed again)" if staged[:-1] else "not taken"))
    s, j = get("/redfish/v1/UpdateService/FirmwareInventory")
    rec("update.inventory_after", s == 200, "%s members" % j.get("Members@odata.count") if s == 200 else s)
    after = mtd_sums(b)
    rec("update.flash_unchanged", before == after and bool(before), " ".join("%s=%s" % kv for kv in sorted(after.items())))
    left = b.out("du -sk /tmp/images 2>/dev/null | cut -f1").strip()
    pid1 = rss(b, "bmcweb")[0]
    rec("update.alive", pid1 == pid0 and get("/redfish/v1/Managers/bmc")[0] == 200,
        "bmcweb pid %s -> %s, /tmp/images %s KiB" % (pid0, pid1, left))
    if CUR == "new":
        r = b.ssh("reimu-ab verify")
        rec("update.ab_verify", r.returncode == 0, "reimu-ab verify: both slots intact")
        ab_upload(b)


def ab_upload(b):
    """the A/B update package (WORKDIR/reimu-ab.tar) through Redfish: written
    into the other slot (reimu-abd -> reimu-ab install), boots next"""
    tar = os.path.join(W, "reimu-ab.tar")
    if not os.path.exists(tar):
        rec("update.ab_package", None, "no reimu-ab.tar in WORKDIR")
        return
    b.ssh("flash_eraseall /dev/mtd6 >/dev/null 2>&1")  # an older slot B: all of it written
    t = time.time()
    s, h, j = rf("POST", "/redfish/v1/UpdateService", open(tar, "rb").read(), raw=True,
                 headers={"Content-Type": "application/octet-stream"}, timeout=400)
    task = j.get("@odata.id") if isinstance(j, dict) else None
    state, msgs = None, ""
    end = time.time() + 1200
    while task and time.time() < end:
        s2, tj = get(task)
        state = tj.get("TaskState") if s2 == 200 else s2
        if state in ("Completed", "Exception", "Killed", "Cancelled"):
            msgs = " ".join(m.get("MessageId", "").rsplit(".", 1)[-1] for m in tj.get("Messages", [])[-2:])
            break
        time.sleep(10)
    e = T.benv(b, "reimu_slot", "reimu_pending")
    v = b.ssh("reimu-ab verify b")
    rec("update.ab_package", s == 202 and state == "Completed" and e == {"reimu_slot": "b", "reimu_pending": "1"}
        and v.returncode == 0, "POST %s, task %s in %d s (%s), env %s, slot B %s" % (
            s, state, time.time() - t, msgs, e, "verified" if v.returncode == 0 else "BAD"))


# ------------------------------------------------------------------ host side (BT)
def g_host(b):
    rec("host.bt.dev", b.ssh("test -c /dev/ipmi-bt-host").returncode == 0, "/dev/ipmi-bt-host")
    rec("host.kcs.dev", None, b.out("ls /dev | grep -iE 'ipmi|kcs' | tr '\\n' ' '"))
    rec("host.ipmid.active", b.out("systemctl is-active phosphor-ipmi-host").strip() == "active", "phosphor-ipmi-host")
    bridge = b.out("for f in /proc/[0-9]*/fd/*; do case $(readlink $f) in /dev/ipmi-bt-host|/dev/ipmi-kcs*) cat ${f%/fd/*}/comm;; esac; done 2>/dev/null | sort -u").split()
    rec("host.bt.bridge", bool(bridge) or None, "process with the BT device open: %s" % (" ".join(bridge) or "none (no btbridged)"))
    o = b.out("busctl list --no-pager | grep -oE 'xyz.openbmc_project.(Ipmi.Host|Control.Host|Ipmi.Channel.eth0)|org.openbmc.HostIpmi' | sort -u | tr '\\n' ' '")
    rec("host.dbus", "Ipmi.Host" in o and "Control.Host" in o, o)
    r = b.ssh("busctl call xyz.openbmc_project.Ipmi.Host /xyz/openbmc_project/Ipmi xyz.openbmc_project.Ipmi.Server "
              "execute yyyaya{sv} 6 0 1 0 0")
    rec("host.ipmid.execute", r.returncode == 0 and r.stdout.startswith("(yyyyay) 7 0 1 0"), "Get Device ID over D-Bus: %s" % r.stdout.strip()[:60])


# ------------------------------------------------------------------ restarts
# the daemons the BMC needs 24/7 (reimu-restart-policy on the A/B image)
RESTART_UNITS = [
    "bmcweb", "phosphor-ipmi-host", "phosphor-ipmi-net@eth0", "xyz.openbmc_project.State.Host",
    "xyz.openbmc_project.State.Chassis", "xyz.openbmc_project.State.BMC", "reimupwr", "reimu-serial",
    "reimu-kvm", "start-ipkvm", "reimu-pnsd", "xyz.openbmc_project.User.Manager", "xyz.openbmc_project.Network",
    "phosphor-certificate-manager@bmcweb", "phosphor-certificate-manager@authority",
    "xyz.openbmc_project.ObjectMapper", "xyz.openbmc_project.Settings", "xyz.openbmc_project.Logging",
    "xyz.openbmc_project.Logging.IPMI", "xyz.openbmc_project.Inventory.Manager", "xyz.openbmc_project.Time.Manager",
    "xyz.openbmc_project.LED.GroupManager", "xyz.openbmc_project.led.controller@sys-class-leds-platform:blue:uid",
    "xyz.openbmc_project.Chassis.Buttons", "phosphor-button-handler", "obmc-fru-fault-monitor",
    "phosphor-systemd-target-monitor", "xyz.openbmc_project.FruDevice", "rngd", "busybox-syslog",
    "busybox-klogd", "triggerhappy", "systemd-networkd"]
KILL3 = r"""u="$1"; for i in 1 2 3; do p=$(systemctl show -p MainPID --value "$u"); [ "$p" -gt 0 ] && kill -9 $p
t=0; while [ $t -lt 25 ]; do sleep 1; t=$((t+1)); q=$(systemctl show -p MainPID --value "$u"); [ "$q" -gt 0 ] && [ "$q" != "$p" ] && break; done; done
t=0; while [ $t -lt 60 ] && [ "$(systemctl is-active "$u")" != active ]; do sleep 1; t=$((t+1)); done
echo $(systemctl is-active "$u") $(systemctl show -p NRestarts --value "$u")"""


def g_restart(b):
    """kill -9 each daemon 3 times in a row (within ~30 s): back, not failed,
    and the BMC still answers on Redfish, IPMI and SOL afterwards"""
    c, env = T.ssh_cmd("cat > /tmp/kill3.sh")
    subprocess.run(c, env=env, input=KILL3, text=True, capture_output=True, timeout=60)
    present = set(b.out("systemctl list-units --no-legend --plain --type=service | cut -d' ' -f1").split())
    for u in RESTART_UNITS:
        if u + ".service" not in present:
            rec("restart." + u, None, "not running here")
            continue
        o = b.out("sh /tmp/kill3.sh '%s.service'" % u, timeout=200).split()
        rec("restart." + u, o[:1] == ["active"], "after 3 x kill -9: %s, NRestarts %s" % (
            o[0] if o else "?", o[1] if len(o) > 1 else "?"))
    rec("restart.redfish", wait_redfish(180), "Redfish answers afterwards")
    rec("restart.ipmi", wait_ipmi(180), "IPMI answers afterwards")
    s, h, j = rf("POST", "/redfish/v1/SessionService/Sessions", {"UserName": USER, "Password": PASS}, auth=None)
    rec("restart.login", s in (200, 201), "Redfish login afterwards: %s" % s)
    rc, o = ipmi("chassis", "status")
    rec("restart.ipmi_chassis", rc == 0, "chassis status afterwards (state managers back): rc %d" % rc)
    if not LOCAL_IPMITOOL:
        sol_key = "ipmi.sol_activate"
        before = R[CUR].get(sol_key)
        g_sol(b)
        R[CUR]["restart.sol"] = R[CUR].pop(sol_key)
        R[CUR].pop("ipmi.sol_console", None)
        if before:
            R[CUR][sol_key] = before
    time.sleep(10)
    f = set(b.out("systemctl --failed --no-legend --plain | cut -d' ' -f1").split())
    # QEMU: no FRU EEPROM, no host power-good
    rec("restart.failed_units", f <= {"mcst-fruid.service", "host-poweron.service"},
        "failed units afterwards: %s" % (" ".join(sorted(f)) or "none"))


# ------------------------------------------------------------------ BMC reset
def g_reset(b):
    s, h, j = rf("POST", "/redfish/v1/Managers/bmc/Actions/Manager.Reset", {"ResetType": "GracefulRestart"})
    rec("reset.request", s in (200, 204), s)
    time.sleep(30)
    ok = b.wait_ssh(600)
    rec("reset.back", ok, "ssh back after Manager.Reset")
    up = b.out("cat /proc/uptime").split()[:1]
    rec("reset.rebooted", bool(up) and float(up[0]) < 900, "uptime %s s" % (up[0] if up else "-"))
    rec("reset.redfish", wait_redfish(300), "Redfish answers again")
    rec("reset.ipmi", wait_ipmi(300), "IPMI answers again")
    if CUR == "new":
        rec("reset.slot", None, "after the reset: %s" % T.ab_state(b))


GROUP_FUNCS = [("redfish", g_redfish), ("accounts", g_accounts), ("crawl", g_crawl), ("ipmi", g_ipmi),
               ("sol", g_sol), ("power", g_power), ("stress", g_stress), ("update", g_update),
               ("host", g_host), ("restart", g_restart), ("reset", g_reset)]


def run_image(name):
    global CUR
    CUR = name
    R.setdefault(name, {})
    print("== " + name, flush=True)
    if ATTACH:
        b = Attached()
    else:
        img = T.fresh("ir-" + name, os.path.basename(stock_image()) if name == "stock" else "ab.mtd")
        b = T.Bmc("ir-" + name, img).start()
    try:
        if not rec("boot", b.wait_ssh(900), "ssh up, systemd started"):
            return
        rec("boot.version", None, b.out("uname -r; sed -n 's/^VERSION_ID=//p' /etc/os-release").replace("\n", " "))
        rec("boot.redfish_up", wait_redfish(), "Redfish answers")
        rec("boot.ipmi_up", wait_ipmi(), "IPMI over LAN answers")
        for g, fn in GROUP_FUNCS:
            if GROUPS and g not in GROUPS:
                continue
            # the journal is in RAM: look before the reboot, and before the
            # restart group kills daemons on purpose
            if g in ("restart", "reset") and "crashes" not in R[CUR]:
                health(b, "crashes")
            print(" -- " + g, flush=True)
            t = time.time()
            try:
                fn(b)
            except Exception as e:  # a group that crashes is a failure, the others still run
                rec(g + ".crashed", False, repr(e))
            print("    (%d s)" % (time.time() - t), flush=True)
        if "reset" in [g for g, f in GROUP_FUNCS if not GROUPS or g in GROUPS] or "crashes" not in R[CUR]:
            health(b, "crashes.end" if "crashes" in R[CUR] else "crashes")
    finally:
        b.stop()


def health(b, key):
    f = b.out("systemctl --failed --no-legend --plain | cut -d' ' -f1").split()
    rec("units.failed" + key[7:], None, " ".join(f))
    k = b.out("dmesg | grep -ciE 'out of memory|oom-kill|segfault|page allocation failure'").strip()
    c = b.out("journalctl --no-pager -b | grep -E 'dumped core|Main process exited, code=(killed|dumped)' | cut -c17-")
    rec(key, k == "0" and not c.strip(), "dmesg OOM/segfault %s; %s" % (
        k, c.strip().replace("\n", " | ") or "no unit killed/dumped"))


# ------------------------------------------------------------------ platform
def platform_run():
    """the new firmware with the platform emulated (test_fru_sensors_qemu.py's
    Platform: PNS flash, FRU EEPROM, sensor stand-ins, power): the board in
    Redfish (Chassis with its sensors) and IPMI (fru print, sdr), sensors
    coming and going with the platform power without errors"""
    global CUR
    CUR = "new"
    R.setdefault(CUR, {})
    print("== platform (new firmware)", flush=True)
    import test_fru_sensors_qemu as F
    for f in (F.PNS, F.FRU):
        if not os.path.exists(f):
            rec("platform.data", False, "%s missing (PNS=, FRU=)" % f)
            return
    b = F.Platform("ir-plat", T.fresh("ir-plat"), power=False).start()
    try:
        b.qmp(("human-monitor-command", {"command-line": "hostfwd_add n0 udp:127.0.0.1:%d-10.0.2.15:623" % IPMI}))
        if not rec("platform.boot", b.wait_ssh(900), "up, platform off"):
            return
        wait_redfish()
        wait_ipmi()

        def chassis():
            s, j = get("/redfish/v1/Chassis")
            return [m["@odata.id"] for m in j.get("Members", [])] if s == 200 else None

        def rf_sensors():
            out = {}
            for sub, key in (("Sensors", "Members"), ("Thermal", "Temperatures")):
                s, j = get("/redfish/v1/Chassis/motherboard/" + sub)
                out[sub] = (s, len(j.get(key, [])) if s == 200 else -1)
            return out

        def dbus_sensors():
            return len(b.sensors())

        def settled():
            """sensors up and hwmon-instantiator done with them"""
            return dbus_sensors() >= 4 and b.out("systemctl show -p ActiveState --value hwmon-instantiator").strip() != "activating"

        def fru():
            rc, o = ipmi("fru", "print", "0")
            return rc, o

        def sdr():
            rc, o = ipmi("sdr", "list")
            return rc, o
        rec("platform.off.chassis", None, "Chassis while never read: %s" % chassis())
        b.transition("On")
        sens = b.wait(settled, 300, 5)
        rec("platform.on.dbus_sensors", bool(sens), "%d sensors on D-Bus" % dbus_sensors())
        ch = b.wait(lambda: chassis(), 400, 10)
        rec("platform.on.chassis", ch == ["/redfish/v1/Chassis/motherboard"], "Chassis %s (after the FRU ID read)" % ch)
        s, j = get("/redfish/v1/Chassis/motherboard")
        rec("platform.on.chassis_asset", s == 200 and j.get("Manufacturer") == "Пантес" and j.get("SerialNumber"),
            "%s %s %s SN %s PN %s" % (s, j.get("Manufacturer"), j.get("Model"), j.get("SerialNumber"), j.get("PartNumber")))
        n = dbus_sensors()
        rs = rf_sensors()
        # (this bmcweb lists temperatures, fans under Thermal, voltages under
        # Power; Sensors only power, current, utilization)
        rec("platform.on.redfish_sensors", rs["Sensors"][0] == 200 and rs["Thermal"] == (200, n),
            "D-Bus %d temperatures, Redfish %s" % (n, rs))
        rc, o = fru()
        rec("platform.on.fru_print", rc == 0 and "Пантес" in o and "0079-07.22" in o,
            " | ".join(l.strip() for l in o.splitlines() if "Board" in l)[:150])
        rc, o = sdr()
        temps = [l for l in o.splitlines() if "degrees C" in l]
        rec("platform.on.sdr", rc == 0 and len(temps) >= 4, "%d temperatures read: %s" % (
            len(temps), " | ".join(t.split("|")[0].strip() + "=" + t.split("|")[1].strip() for t in temps)[:120]))

        b.transition("Off")
        gone = b.wait(lambda: dbus_sensors() == 0, 300, 5)
        time.sleep(10)
        rec("platform.off.dbus_sensors", bool(gone) or dbus_sensors() == 0, "%d sensors left" % dbus_sensors())
        rs = rf_sensors()
        rec("platform.off.redfish_sensors", rs["Sensors"] == (200, 0) and rs["Thermal"] == (200, 0), "%s" % rs)
        s, j = get("/redfish/v1/Chassis/motherboard/Power")
        rec("platform.off.redfish_power", s == 200, "Power %s" % s)
        rc, o = sdr()
        rec("platform.off.sdr", rc == 0, "rc %d, temperatures without reading: %d" % (
            rc, sum(1 for l in o.splitlines() if "/xyz" not in l and ("ns" in l.split("|")[-1] or "no reading" in l))))
        rc, o = fru()
        rec("platform.off.fru_print", rc == 0 and "Пантес" in o, "the board stays (inventory): rc %d" % rc)
        rec("platform.off.chassis", chassis() == ["/redfish/v1/Chassis/motherboard"], "Chassis %s" % chassis())

        for i in range(3):
            b.transition("On")
            on = b.wait(settled, 300, 5)
            rs_on = rf_sensors()
            b.transition("Off")
            off = b.wait(lambda: dbus_sensors() == 0, 300, 5)
            time.sleep(10)
            rs_off = rf_sensors()
            rec("platform.cycle%d" % (i + 1), bool(on) and bool(off) and rs_on["Sensors"][0] == 200
                and rs_on["Thermal"][0] == 200 and rs_on["Thermal"][1] > 0 and rs_off["Sensors"] == (200, 0) and rs_off["Thermal"] == (200, 0),
                "on %s, off %s" % (rs_on, rs_off))
        # off while hwmon-instantiator is still instantiating (the first
        # sensors just up): everything must go all the same
        b.transition("On")
        b.wait(lambda: dbus_sensors() >= 1, 300, 2)
        busy = b.out("systemctl show -p ActiveState --value hwmon-instantiator").strip()
        b.transition("Off")
        b.wait(lambda: dbus_sensors() == 0, 90, 5)
        left = dbus_sensors()
        # not prefixed platform.: hwmon-instantiator is another agent's (reported)
        rec("issue.off_during_instantiation", left == 0, "off while hwmon-instantiator was %s: %d sensors left on D-Bus after 90 s" % (busy, left))
        if left:
            b.ssh("systemctl stop 'xyz.openbmc_project.Hwmon@*'")
        # a BMC reboot with the platform off: the board from the inventory kept
        b.ssh("reboot")
        time.sleep(30)
        rec("platform.reboot", b.wait_ssh(900) and wait_redfish(), "BMC back, platform off")
        rec("platform.reboot.chassis", chassis() == ["/redfish/v1/Chassis/motherboard"], "Chassis %s" % chassis())
        wait_ipmi()
        rc, o = fru()
        rec("platform.reboot.fru_print", rc == 0 and "Пантес" in o, "rc %d" % rc)
        rec("platform.reboot.eeprom_untouched", not b.clients(eeprom=True) & {"3-0057", "3-0057:bound"},
            "EEPROM client while off: %s" % sorted(b.clients(eeprom=True)))
        health(b, "platform.crashes")
    finally:
        b.stop()


def report():
    imgs = [i for i in ("stock", "new") if i in R]
    keys = []
    for i in imgs:
        for k in R[i]:
            if k not in keys:
                keys.append(k)
    sym = {True: "ok", False: "FAIL", None: "-"}
    regress, better, failed_must = [], [], []
    print("\n%-40s %-6s %-6s  %s" % ("check", "stock", "new", "new detail / stock detail"))
    for k in keys:
        st = R.get("stock", {}).get(k)
        nw = R.get("new", {}).get(k)
        a = sym[st[0]] if st else "n/a"
        c = sym[nw[0]] if nw else "n/a"
        note = ""
        if "stock" in R and "new" in R and st and st[0] is True and (not nw or nw[0] is False):
            if k in KNOWN or any(k.startswith(x) for x in KNOWN if x.endswith("*")):
                note = "  [known: %s]" % KNOWN[k]
            else:
                regress.append(k)
                note = "  [REGRESSION]"
        if st and nw and st[0] is False and nw[0] is True:
            better.append(k)
            note = "  [better]"
        if "new" in R and ((k in MUST and (not nw or nw[0] is not True)) or
                           (k.startswith(("restart.", "platform.")) and nw and nw[0] is False)):
            failed_must.append(k)
            note += "  [MUST]"
        d = (nw[1] if nw else "") + (" / " + st[1] if st and (not nw or st[1] != nw[1]) else "")
        if not k.startswith("crawl.") or note or a != c:
            print("%-40s %-6s %-6s  %s%s" % (k[:40], a, c, d[:150], note))
    print("\n(crawl.* rows equal on both left out)")
    print("better on the new firmware: %s" % (" ".join(better) or "-"))
    print("known differences: %s" % (" ".join(k for k in KNOWN if k in keys) or "-"))
    print("regressions: %s" % (" ".join(regress) or "none"))
    print("MUST checks failed on the new firmware: %s" % (" ".join(failed_must) or "none"))
    json.dump(R, open(os.path.join(W, "ipmi-redfish-results.json"), "w"), indent=1)
    return not regress and not failed_must


if __name__ == "__main__":
    for i in IMAGES:
        run_image(i)
    if PLATFORM:
        platform_run()
    sys.exit(0 if report() else 1)
