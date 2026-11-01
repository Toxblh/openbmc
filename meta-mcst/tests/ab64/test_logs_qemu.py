#!/usr/bin/env python3
"""The BMC's logs on the A/B image in QEMU: a week of journal in RAM, the
persistent event log on the rwfs, the boot record, the diagnostic archive.

    test_logs_qemu.py WORKDIR [TEST...]

WORKDIR as for test_fru_sensors_qemu.py (ab.mtd, pns.bin, fru.bin); RQ, AB_SSH,
AB_HTTPS as for test_ab_qemu.py. E2E=dir with node_modules (playwright),
CHROME=path: the browser test of the web page (skipped without them).

Tests (all by default, on one boot unless they say otherwise):

  journald   effective limits, no forwarding to a syslog nobody needs, the
             per-minute timer's lines gone, no duplicate kernel lines
  flap       a sensor that fails and recovers over and over: its lines limited
  events     warnings reach the log within a flush, info doesn't, critical at
             once, a reboot keeps them
  crash      kill -9 during a flood: readable, nothing lost or doubled
  flood      rate and size limits, the wear estimate (writes per day)
  boot       the boot record: clean reboot, panic (no clean stop), kexec, the
             decode table of the reset registers
  rest       Read over REST, refused to an Operator
  diag       the archive: MANIFEST, contents, canaries (no secret in it), one
             run at a time, a hanging collector, the size cap, under load,
             over REST with its cleanup
  web        the logs page in a browser: event log, the diagnostic button
  retention  a week's worth of the measured rate (long: ~10 min): the oldest
             entry survives, MemAvailable above the floor; the diag size cap
             with that journal
"""
import base64, gzip, http.client, io, json, os, re, shutil, ssl, subprocess, sys, tarfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import test_ab_qemu as T  # noqa: E402  (reads WORKDIR from sys.argv[1])
import test_fru_sensors_qemu as F  # noqa: E402
from test_ab_qemu import check, failed, W, HTTPS  # noqa: E402

ONLY = sys.argv[2:]
EVLOG = "/var/lib/reimu-logd/events.log"
SVC, OBJ, IFACE = "xyz.openbmc_project.Reimu.Logs", "/xyz/openbmc_project/reimu/logs", "xyz.openbmc_project.Reimu.Logs"
# the measured rate of the board (2026-11): 5.6 MiB in 15 h, ~316 lines/h
LINES_PER_WEEK = 316 * 168
MEM_FLOOR_MIB = 250
CTX = ssl.create_default_context()
CTX.check_hostname = False
CTX.verify_mode = ssl.CERT_NONE


def rf(method, path, body=None, auth=("root", "0penBmc"), timeout=120):
    h = {"Accept": "application/json", "Authorization": "Basic " + base64.b64encode(("%s:%s" % auth).encode()).decode()}
    if body is not None:
        body = json.dumps(body)
        h["Content-Type"] = "application/json"
    try:
        c = http.client.HTTPSConnection("127.0.0.1", HTTPS, context=CTX, timeout=timeout)
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        d = r.read()
        c.close()
    except (OSError, http.client.HTTPException) as e:
        return 0, repr(e)
    try:
        return r.status, json.loads(d)
    except ValueError:
        return r.status, d


def action(name, args, auth=("root", "0penBmc")):
    return rf("POST", OBJ + "/action/" + name, {"data": args}, auth=auth)


def wait(fn, timeout, step=2):
    end = time.time() + timeout
    while True:
        v = fn()
        if v or time.time() > end:
            return v
        time.sleep(step)


def log_text(b):
    return b.out("cat %s.1 %s 2>/dev/null" % (EVLOG, EVLOG))


def warn(b, tag, prio="warning"):
    b.ssh("systemd-cat -t ev-test -p %s echo %s" % (prio, tag))


# ------------------------------------------------------------------ journald
def t_journald(b):
    r = b.out("journalctl -b --no-pager -o cat -u systemd-journald | grep 'Runtime Journal' | tail -1")
    m = re.search(r"max ([\d.]+)M, ([\d.]+)M free", r)
    check(m and 48 <= float(m.group(1)) <= 64, "journald's effective runtime limit: %s" % r.strip())
    conf = b.out("cat /lib/systemd/journald.conf.d/*.conf /etc/systemd/journald.conf.d/*.conf 2>/dev/null; ls /lib/systemd/journald.conf.d/")
    last = {}
    for l in conf.splitlines():
        if "=" in l:
            k, v = l.split("=", 1)
            last[k] = v
    print("    " + " ".join("%s=%s" % kv for kv in sorted(last.items())))
    check(last.get("ForwardToSyslog") == "no", "ForwardToSyslog=no")
    check(last.get("ForwardToWall") == "no", "ForwardToWall=no")
    check(last.get("Storage") == "volatile", "Storage=volatile (nothing to the NOR flash)")
    check(last.get("Compress") == "yes", "Compress=yes")
    check(b.ssh("test -d /var/log/journal").returncode != 0, "no /var/log/journal")
    k = int(re.search(r"RuntimeKeepFree=(\d+)", conf).group(1)) if "RuntimeKeepFree=" in conf else 0
    check(k >= 64, "RuntimeKeepFree=%sM" % k)
    time.sleep(130)  # two runs of the per-minute timer
    j = b.out("journalctl -b --no-pager -o cat")
    n = sum("missed" in l and "syslog" in l for l in j.splitlines())
    check(n == 0, "no 'Forwarding to syslog missed' (%d)" % n)
    n = b.out("journalctl -b --no-pager --since '-2min' -o cat | grep -c 'Rotate SoL\\|logrotate-sol'").strip()
    check(n == "0", "the per-minute SoL rotation doesn't log (%s lines in 2 min)" % n)
    check(b.out("systemctl is-active logrotate-sol.timer logrotate-sol").split() == ["active", "active"], "it still runs")
    b.ssh("echo sol-canary-line >> /usr/log/current")
    check(wait(lambda: b.ssh("gunzip -c /usr/log/current.gz | grep -q sol-canary-line").returncode == 0, 340, 10), "the SoL log's copy on the flash still follows (SOL_GZ_EVERY, 5 min)")
    k = b.out("journalctl -b --no-pager -t kernel: | grep -c . ; pidof klogd syslogd | wc -w").split()
    check(k[-1] == "0", "no busybox klogd/syslogd (kernel lines twice in the journal)")
    print("    lines by source: " + b.out("journalctl -b --no-pager | awk '{print $5}' | sed 's/\\[.*//' | sort | uniq -c | sort -rn | head -6").replace("\n", " "))


def t_retention(b):
    b.ssh("systemd-cat -t ret-test -p info echo RETENTION-OLDEST-MARKER")
    m0 = int(re.search(r"MemAvailable:\s+(\d+)", b.out("cat /proc/meminfo")).group(1)) // 1024
    gen = ("i=0; while [ $i -lt %d ]; do j=0; while [ $j -lt 2000 ]; do "
           "echo \"sensor flood line $i $j some typical text of a service, about this long\"; j=$((j+1)); done; "
           "i=$((i+2000)); sleep 6; done" % LINES_PER_WEEK)
    t = time.time()
    r = b.ssh("systemd-cat -t ret-test -p info sh -c '%s'" % gen, timeout=3000)
    print("    %d lines in %d s, rc %d" % (LINES_PER_WEEK, time.time() - t, r.returncode))
    time.sleep(10)
    use = b.out("journalctl --disk-usage").strip()
    print("    " + use)
    mib = float(re.search(r"([\d.]+)M", use).group(1)) if "M" in use else 0
    check(mib <= 72, "journal within its limit: %s" % use)
    check("RETENTION-OLDEST-MARKER" in b.out("journalctl --no-pager -o cat -t ret-test | head -3"),
          "the entry from a week of lines ago is still there")
    first = b.out("journalctl --no-pager -o short-iso | sed -n 2p")
    print("    oldest: " + first.strip()[:80])
    m1 = int(re.search(r"MemAvailable:\s+(\d+)", b.out("cat /proc/meminfo")).group(1)) // 1024
    check(m1 >= MEM_FLOOR_MIB, "MemAvailable %d -> %d MiB (floor %d)" % (m0, m1, MEM_FLOOR_MIB))
    print("    %.0f bytes an entry; %.0f days of the measured rate fit 64 MiB" % (
        mib * 1048576 / max(LINES_PER_WEEK, 1), 64 / max(mib, 0.1) * 7))
    t0 = time.time()
    r = b.ssh("reimu-diag", timeout=200)
    p = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
    size = b.out("wc -c < %s" % p).strip() if p else "0"
    check(r.returncode == 0 and int(size or 0) <= 16 << 20, "diag with a full journal: %s bytes, %d s" % (size, time.time() - t0))
    check(time.time() - t0 < 120, "bounded time")
    d = fetch(b, p) if p else {}
    check(b"truncated" in d.get("MANIFEST", b"") or int(size or 0) < 8 << 20, "truncation noted when it had to")
    b.ssh("rm -f %s" % p)


# ------------------------------------------------------------------ hwmon's lines
def t_flap(b):
    b.transition("On")
    wait(lambda: len(b.sensors()) >= 4, 300, 5)
    time.sleep(10)
    n0 = int(b.out("journalctl -b --no-pager -o cat -t phosphor-hwmon-readd | wc -l").strip() or 0)
    # pcs1 reading 0 is a failing read (l_pcs_i2c EIO, REMOVERCS): 2 min of flapping
    t = time.time()
    while time.time() - t < 120:
        b.temperature("pcs1", 0)
        time.sleep(4)
        F.pcs_warm(b)
        time.sleep(4)
    n1 = int(b.out("journalctl -b --no-pager -o cat -t phosphor-hwmon-readd | wc -l").strip() or 0)
    u = b.out("systemctl show -p LogRateLimitBurst -p LogRateLimitIntervalUSec 'xyz.openbmc_project.Hwmon@-x.service' 2>/dev/null | tr '\\n' ' '")
    print("    %d phosphor-hwmon lines in 2 min of a flapping sensor (%s)" % (n1 - n0, u))
    check(n1 - n0 <= 20, "a flapping sensor's lines are limited (%d in 2 min)" % (n1 - n0))
    F.pcs_warm(b)
    b.transition("Off")
    wait(lambda: not b.sensors(), 120, 5)


# ------------------------------------------------------------------ events
def t_events(b):
    check(b.out("systemctl is-active reimu-logd").strip() == "active", "reimu-logd active")
    warn(b, "EV-WARN-1")
    warn(b, "EV-INFO-1", "info")
    warn(b, "EV-CRIT-1", "crit")
    check(wait(lambda: "EV-CRIT-1" in log_text(b), 15, 1), "critical on the flash within seconds")
    check(wait(lambda: "EV-WARN-1" in log_text(b), 75, 3), "a warning within a minute")
    check("EV-INFO-1" not in log_text(b), "info isn't logged")
    warn(b, "EV-BEFORE-REBOOT")
    b.ssh("reboot")
    time.sleep(25)
    check(b.wait_ssh(600), "BMC back after a reboot")
    t = log_text(b)
    check("EV-WARN-1" in t and "EV-CRIT-1" in t, "events survive a reboot")
    check("EV-BEFORE-REBOOT" in t, "the pending ones flushed at the clean stop")
    print("    " + "\n    ".join(t.splitlines()[-4:])[:400])


def t_crash(b):
    b.ssh("nohup sh -c 'i=0; while [ $i -lt 200 ]; do systemd-cat -t ev-crash -p warning echo CRASH-$i; i=$((i+1)); done' >/dev/null 2>&1 &")
    time.sleep(3)
    pid = b.out("systemctl show -p MainPID --value reimu-logd").strip()
    b.ssh("kill -9 %s" % pid)
    check(wait(lambda: b.out("systemctl is-active reimu-logd").strip() == "active" and
               b.out("systemctl show -p MainPID --value reimu-logd").strip() not in ("0", pid), 30), "restarted after kill -9")
    time.sleep(70)
    t = log_text(b)
    check(all(("\n" not in l) for l in t.splitlines()) and t.endswith("\n"), "the file is readable, ends in a whole line")
    bad = [l for l in t.splitlines() if not re.match(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ ", l)]
    check(not bad, "every line whole (%s)" % bad[:2])
    seen = re.findall(r"CRASH-(\d+)\b", t)
    check(len(seen) == len(set(seen)), "nothing logged twice over the restart (%d lines)" % len(seen))
    b.ssh("dd if=/dev/urandom of=/dev/null bs=1 count=0; printf 'torn line without an end' >> %s" % EVLOG)
    b.ssh("systemctl restart reimu-logd")
    warn(b, "EV-AFTER-TORN", "crit")
    wait(lambda: "EV-AFTER-TORN" in log_text(b), 20, 1)
    check(re.search(r"^.*EV-AFTER-TORN", log_text(b), re.M) and "torn line without an end\n" in log_text(b),
          "a torn last line doesn't swallow the next one")


def stats(b):
    return dict(x.split("=") for x in b.out("cat /run/reimu-logd/stats").split() if "=" in x)


def t_flood(b):
    s0 = stats(b)
    t0 = time.time()
    gen = "i=0; while [ $i -lt 5000 ]; do echo \"FLOOD line $i\"; i=$((i+1)); done"
    b.ssh("systemd-cat -t ev-flood -p warning sh -c '%s'" % gen, timeout=300)
    warn(b, "FLOOD-OTHER-SOURCE")
    warn(b, "FLOOD-CRIT", "crit")
    check(wait(lambda: "FLOOD-CRIT" in log_text(b), 20, 1), "a critical line passes the flood")
    time.sleep(190)
    s1 = stats(b)
    dt = time.time() - t0
    nw, nb = int(s1["writes"]) - int(s0["writes"]), int(s1["bytes"]) - int(s0["bytes"])
    print("    in %d s of flood: %d writes, %d bytes, %s dropped" % (dt, nw, nb, s1.get("dropped")))
    check(nw <= 4 + dt / 60 + 3, "flushes stay at about one a minute (%d)" % nw)
    check(nb <= 40 * 1024, "bytes limited (%d)" % nb)
    t = log_text(b)
    check(sum("FLOOD line" in l for l in t.splitlines()) <= 40, "the flood's lines limited")
    check("dropped" in t, "what was dropped is said")
    check("FLOOD-OTHER-SOURCE" in t, "another source still gets through")
    # size: rotation with the file near its limit
    b.ssh("yes '2026-01-01T00:00:00Z 4 x: padding padding padding padding padding padding' | head -c 262000 >> %s" % EVLOG)
    for i in range(6):
        warn(b, "ROT-%d" % i, "crit")
        time.sleep(6)
    time.sleep(8)
    sz = b.out("cat %s %s.1 | wc -c; ls %s*" % (EVLOG, EVLOG, EVLOG)).split()
    check(int(sz[0]) <= 540 * 1024 and EVLOG + ".1" in sz, "rotated, bounded: %s bytes (%s)" % (sz[0], sz[1:]))
    check("ROT-5" in log_text(b), "the newest line is in the new file")
    # wear
    idle0 = stats(b)
    time.sleep(180)
    idle1 = stats(b)
    idle_w = int(idle1["writes"]) - int(idle0["writes"])
    print("    wear: idle %d writes in 3 min; limits 1 write/min, 4 KiB/min, 32 KiB/h -> worst case 768 KiB/day "
          "= 12 erase blocks a day of 144 (64 KiB), ~0.08 full cycles/day" % idle_w)
    check(idle_w <= 3, "idle: next to no writes (%d in 3 min)" % idle_w)


# ------------------------------------------------------------------ boot record
def boot_line(b):
    ls = [l for l in log_text(b).splitlines() if " boot: " in l]
    return ls[-1] if ls else ""


def t_boot(b):
    table = [  # bootstatus scu3c clean cmdline -> reason
        ("0x20", "0x1", "1", "console=ttyS4", "reboot"),
        ("0x20", "0x5", "0", "console=ttyS4", "watchdog"),
        ("0", "0x1", "0", "console=ttyS4", "power-on"),
        ("0", "0x1", "1", "console=ttyS4", "kexec-or-reboot"),
        ("0", "0x1", "0", "console=ttyS4 reimu.ram", "kexec"),
        ("0", "0x2", "0", "console=ttyS4", "external-reset"),
    ]
    for bs, scu, cl, cmd, want in table:
        r = b.out("reimu-logd --decode-reset %s %s %s '%s'" % (bs, scu, cl, cmd)).strip()
        check(r == want, "decode %s %s clean=%s '%s' -> %s (%s)" % (bs, scu, cl, cmd, want, r))
    b.ssh("reboot")
    time.sleep(25)
    check(b.wait_ssh(600), "up after a reboot")
    wait(lambda: boot_line(b), 30)
    l = boot_line(b)
    print("    " + l)
    check("prev=clean" in l and "SCU3C=0x" in l and "slot=" in l, "the clean reboot's record")
    check(l.split(" boot: ")[1] in b.out("journalctl -b --no-pager -o cat -t reimu-logd"), "the record in the journal too")
    # a panic: no clean stop (QEMU's reset leaves no WDT flag in SCU3C: the table covers that)
    n = b.boots()
    b.ssh("nohup sh -c 'sleep 2; echo c > /proc/sysrq-trigger' >/dev/null 2>&1 &")
    check(wait(lambda: b.boots() > n, 120), "the panic reset the BMC")
    time.sleep(25)
    check(b.wait_ssh(600), "up after the panic")
    wait(lambda: "prev=unclean" in boot_line(b), 30)
    l = boot_line(b)
    print("    " + l)
    check("prev=unclean" in l, "the panic's record says no clean stop")
    # kexec
    b.ssh("nohup reimu-ab ram >/dev/null 2>&1 &", timeout=30)
    time.sleep(30)
    check(b.wait_ssh(600), "up after reimu-ab ram (kexec)")
    wait(lambda: "reset=kexec" in boot_line(b), 40)
    print("    " + boot_line(b))
    check("reset=kexec" in boot_line(b), "the kexec's record")


# ------------------------------------------------------------------ REST
def make_user(name, role):
    pw = "Rq7#xLm2pTz"
    rf("DELETE", "/redfish/v1/AccountService/Accounts/" + name)
    s, _ = rf("POST", "/redfish/v1/AccountService/Accounts", {"UserName": name, "Password": pw, "RoleId": role, "Enabled": True})
    time.sleep(2)
    return (name, pw) if s in (200, 201) else None


def read_all_events():
    out, off = b"", 0
    while True:
        s, j = action("Read", [off, 65536])
        if s != 200:
            return s, out
        r = json.loads(j["data"])
        d = base64.b64decode(r.get("data", ""))
        out += d
        off += len(d)
        if not d or off >= r["size"]:
            return 200, out


def t_rest(b):
    warn(b, "EV-REST", "crit")
    wait(lambda: "EV-REST" in log_text(b), 20, 1)
    s, t = read_all_events()
    check(s == 200 and b"EV-REST" in t, "the event log over REST (%s, %d bytes)" % (s, len(t)))
    op = make_user("evop", "Operator")
    ro = make_user("evro", "ReadOnly")
    for u in (op, ro):
        s, _ = action("Read", [0, 100], auth=u)
        check(s in (401, 403), "Read refused to %s: %s" % (u[0], s))
        s, _ = action("DiagStart", [False], auth=u)
        check(s in (401, 403), "DiagStart refused to %s: %s" % (u[0], s))
        s, _ = action("DiagDownload", [0, 100], auth=u)
        check(s in (401, 403), "DiagDownload refused to %s: %s" % (u[0], s))
    for u in ("evop", "evro"):
        rf("DELETE", "/redfish/v1/AccountService/Accounts/" + u)


# ------------------------------------------------------------------ diag
CANARY = "CANARY7f3a9d"
PLANTS = [
    "/etc/shadow-canary-unused",
]


def fetch(b, path):
    """the archive's members {name: bytes}"""
    c, env = T.ssh_cmd("cat '%s'" % path)
    r = subprocess.run(c, env=env, capture_output=True, stdin=subprocess.DEVNULL, timeout=300)
    if r.returncode or not r.stdout:
        return {}
    out = {}
    with tarfile.open(fileobj=io.BytesIO(r.stdout), mode="r:gz") as t:
        for m in t.getmembers():
            if m.isfile():
                out[m.name.split("/", 1)[-1]] = t.extractfile(m).read()
    return out


def plant(b):
    b.ssh(r"""
    S=%s
    # everywhere a secret lives
    mkdir -p /var/lib/reimu-hostlink /etc/ssl/certs/https /home/root
    cp /etc/shadow /tmp/shadow.keep; echo "canary:$S-shadow:1:0:99999:7:::" >> /etc/shadow
    [ -f /etc/ipmi_pass ] && cp /etc/ipmi_pass /tmp/ipmi_pass.keep; echo "$S-ipmipass" >> /etc/ipmi_pass
    echo "$S-hostlinkkey" > /var/lib/reimu-hostlink/key.canary
    printf -- '-----BEGIN PRIVATE KEY-----\n%%s-pemfile\n-----END PRIVATE KEY-----\n' $S > /etc/ssl/certs/https/canary.pem
    printf -- '-----BEGIN OPENSSH PRIVATE KEY-----\n%%s-sshkey\n-----END OPENSSH PRIVATE KEY-----\n' $S > /etc/ssh/ssh_host_canary_key
    echo "{\"auth_token\": \"$S-session\"}" > /home/root/bmcweb_persistent_data.json.canary
    # in the journal, as lines
    systemd-cat -t canary echo "login password=$S-j1 for x"
    systemd-cat -t canary echo "the token: $S-j2"
    systemd-cat -t canary echo "PASSWORD=$S-j3"
    systemd-cat -t canary echo "secret=$S-j4 Authorization: Bearer $S-j5"
    systemd-cat -t canary echo "-----BEGIN RSA PRIVATE KEY-----"
    systemd-cat -t canary echo "$S-j6"
    systemd-cat -t canary echo "-----END RSA PRIVATE KEY-----"
    # the environment of a unit
    systemctl set-environment REIMU_CANARY_TOKEN=$S-env
    # a network file with a password
    mkdir -p /etc/systemd/network; printf '[Network]\n# Password=%%s-net\n' $S > /etc/systemd/network/99-canary.network.disabled
    """ % CANARY)


def unplant(b):
    b.ssh("rm -f /etc/ssl/certs/https/canary.pem /etc/ssh/ssh_host_canary_key /var/lib/reimu-hostlink/key.canary "
          "/home/root/bmcweb_persistent_data.json.canary /etc/systemd/network/99-canary.network.disabled; "
          "cp /tmp/shadow.keep /etc/shadow; [ -f /tmp/ipmi_pass.keep ] && cp /tmp/ipmi_pass.keep /etc/ipmi_pass; "
          "systemctl unset-environment REIMU_CANARY_TOKEN")


def t_diag(b):
    plant(b)
    secrets = {}
    for f in ("/etc/shadow", "/etc/ssh/ssh_host_ed25519_key", "/etc/ssl/certs/https/server.pem", "/etc/ipmi_pass", "/etc/key_file"):
        c, env = T.ssh_cmd("cat %s 2>/dev/null" % f)
        o = subprocess.run(c, env=env, capture_output=True, stdin=subprocess.DEVNULL, timeout=60).stdout
        ls = [l[:40] for l in o.splitlines() if len(l) >= 16 and b"-----" not in l]
        if ls:
            secrets[f] = ls[0]
    un0 = b.out("for u in bmcweb phosphor-ipmi-host reimu-fand overheatd reimu-serial; do systemctl show -p NRestarts -p MainPID --value $u; done | tr '\\n' ' '")
    t0 = time.time()
    r = b.ssh("reimu-diag", timeout=200)
    dt = time.time() - t0
    p = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
    check(r.returncode == 0 and p.startswith("/run/reimu-diag/") and p.endswith(".tar.gz"), "reimu-diag prints the path: %r (rc %d, %d s)" % (p, r.returncode, dt))
    check(dt < 90, "within the time bound (%d s)" % dt)
    d = fetch(b, p) if p else {}
    names = sorted(d)
    print("    %d files, %s bytes: %s" % (len(names), b.out("wc -c < %s" % p).strip() if p else "?", " ".join(names)[:600]))
    m = d.get("MANIFEST", b"").decode(errors="replace")
    check("collector" in m and "duration" in m.lower(), "MANIFEST with collectors and durations")
    print("    " + "\n    ".join(l[:90] for l in m.splitlines() if re.search(r"\b\d+s\b|skipped", l)))
    for want in ("os-release", "cmdline", "proc-mtd", "reimu-ab-status", "fw_printenv", "journal-current", "events", "dmesg",
                 "systemctl-failed", "list-units", "restart-counts", "top-rss", "meminfo", "ip-addr", "network-config",
                 "udc-state", "reimu-fand-status", "overheatd", "reimu-hostlink", "hwmon", "busctl-sensors", "chassis-host-state",
                 "fru-xml", "boot-record"):
        # (no FRU ID until the platform has been on: the MANIFEST says so)
        check(any(want in n for n in names) or (want == "fru-xml" and "fru-xml" in m), "contains %s" % want)
    # A/B env: only the allowed ones
    env = d.get(next((n for n in names if "fw_printenv" in n), ""), b"").decode(errors="replace")
    keys = {l.split("=", 1)[0] for l in env.splitlines() if "=" in l and not l.startswith("#")}
    check(keys and all(k.startswith("reimu_") or k in ("bootcmd", "bootargs") for k in keys), "fw_printenv reduced to reimu_*, bootcmd, bootargs: %s" % sorted(keys)[:8])
    # secrets: no canary, no real secret, in any file, nor a key block
    alltext = b"\n".join(d.values())
    hits = sorted({m.group(0) for m in re.finditer((CANARY + r"[-\w]*").encode(), alltext)})
    check(not hits, "no canary in the archive %s" % hits)
    for f, s in secrets.items():
        check(s not in alltext, "nothing of %s" % f)
    check(b"PRIVATE KEY-----\n" not in alltext.replace(b"[scrubbed]", b"") or b"BEGIN" not in alltext.split(b"PRIVATE KEY-----")[0][-40:], "no PEM private key block")
    for n in names:
        check(not re.search(r"shadow|ipmi_pass|ipmi_user|server\.pem|persistent_data|ssh_host|_key$", n), "no secret file %s" % n) if re.search(r"shadow|ipmi_pass|ipmi_user|server\.pem|persistent_data|ssh_host|_key$", n) else None
    check(b"login" in alltext, "the journal's lines are still there, scrubbed")
    check(b"98:a7:b0" in alltext.lower(), "MACs stay")
    # no i2c sweep, no GPIO change
    un1 = b.out("for u in bmcweb phosphor-ipmi-host reimu-fand overheatd reimu-serial; do systemctl show -p NRestarts -p MainPID --value $u; done | tr '\\n' ' '")
    check(un0 == un1, "no service stopped or restarted (%s)" % un1)
    src = b.out("cat /usr/libexec/reimu-diag")
    for bad in ("i2cdump", "i2cget", "i2cset", "i2cdetect", "gpioset", "gpiowrite", "gpioset", "systemctl restart", "systemctl stop", "/proc/*/environ"):
        check(bad not in src, "the script doesn't use %s" % bad)
    b.ssh("rm -f %s" % p)
    # one at a time
    b.ssh("nohup reimu-diag >/tmp/d1.out 2>&1 &")
    time.sleep(1)
    r = b.ssh("reimu-diag")
    check(r.returncode != 0 and "running" in (r.stdout + r.stderr), "a second run at the same time refused: %s" % (r.stdout + r.stderr).strip()[:80])
    wait(lambda: b.out("cat /tmp/d1.out").strip().endswith(".tar.gz"), 120)
    b.ssh("rm -f /run/reimu-diag/*.tar.gz")
    # a hanging collector
    b.ssh("mkdir -p /tmp/fake; printf '#!/bin/sh\\nsleep 1000\\n' > /tmp/fake/fw_printenv; chmod +x /tmp/fake/fw_printenv")
    t0 = time.time()
    r = b.ssh("PATH=/tmp/fake:$PATH reimu-diag", timeout=200)
    dt = time.time() - t0
    p = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
    d = fetch(b, p) if p else {}
    m = d.get("MANIFEST", b"").decode(errors="replace")
    check(r.returncode == 0 and re.search(r"fw_printenv.*timeout", m), "a hanging collector times out, the rest goes on (%d s): %s" %
          (dt, next((l for l in m.splitlines() if "fw_printenv" in l), "")))
    check(dt < 90 and any("journal" in n for n in d), "bounded and the others collected")
    check(b.out("ps | grep -c '[/]tmp/fake/fw_printenv'").strip() == "0", "the hung one was killed")
    b.ssh("rm -f %s; rm -rf /tmp/fake" % p)
    # the host's data, off by default; asked for without a hook: said so
    r = b.ssh("reimu-diag --with-host", timeout=200)
    p = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
    d = fetch(b, p) if p else {}
    check(re.search(r"host", d.get("MANIFEST", b"").decode(errors="replace")), "--with-host recorded in the MANIFEST")
    b.ssh("rm -f %s" % p)
    # under load
    u0 = float(b.out("cut -d' ' -f1 /proc/uptime").strip())
    b.ssh("nohup sh -c 'for i in 1 2 3 4; do (timeout 80 sh -c \"while :; do :; done\") & done; wait' >/dev/null 2>&1 &")
    r = b.ssh("reimu-diag", timeout=300)
    p = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
    u1 = float(b.out("cut -d' ' -f1 /proc/uptime").strip())
    check(r.returncode == 0 and u1 > u0, "under CPU load: done, no reset of the BMC (uptime %d -> %d)" % (u0, u1))
    un2 = b.out("for u in bmcweb phosphor-ipmi-host reimu-fand overheatd reimu-serial; do systemctl show -p NRestarts -p MainPID --value $u; done | tr '\\n' ' '")
    check(un0 == un2, "no service restarted under load")
    b.ssh("rm -f %s" % p)
    unplant(b)
    # over REST
    s, j = action("DiagStart", [False])
    check(s == 200, "DiagStart over REST: %s %s" % (s, str(j)[:100]))

    def st():
        s, j = action("DiagStatus", [])
        return json.loads(j["data"]) if s == 200 else {}
    seen = set()

    def done():
        x = st()
        seen.add(x.get("step"))
        return x.get("state") in ("done", "failed") and x
    x = wait(done, 150, 2)
    print("    status: %s; progress seen: %s" % (x, sorted(str(s) for s in seen)[:6]))
    check(x and x["state"] == "done" and x["size"] > 1000, "done, size %s" % (x or {}).get("size"))
    check(len(seen) >= 2, "progress reported")
    out, off = b"", 0
    while True:
        s, j = action("DiagDownload", [off, 524288])
        r = json.loads(j["data"]) if s == 200 else {}
        c = base64.b64decode(r.get("data", ""))
        out += c
        off += len(c)
        if not c or off >= r.get("size", 0):
            break
    with tarfile.open(fileobj=io.BytesIO(out), mode="r:gz") as t:
        check(any("MANIFEST" in n for n in t.getnames()), "downloaded over REST: a tar.gz of %d bytes" % len(out))
    s, j = action("DiagDelete", [])
    check(s == 200 and b.out("ls /run/reimu-diag/ | grep -c tar.gz").strip() == "0", "deleted after the download")
    check(b.out("stat -c %a /run/reimu-diag").strip() == "700", "/run/reimu-diag is 0700")
    # the 15 minutes (shortened)
    b.ssh("mkdir -p /run/systemd/system/reimu-logd.service.d; printf '[Service]\\nEnvironment=REIMU_DIAG_TTL=20\\n' > /run/systemd/system/reimu-logd.service.d/t.conf; systemctl daemon-reload; systemctl restart reimu-logd")
    time.sleep(3)
    action("DiagStart", [False])
    wait(lambda: st().get("state") in ("done", "failed"), 150)
    check(b.out("ls /run/reimu-diag/ | grep -c tar.gz").strip() == "1", "there")
    check(wait(lambda: b.out("ls /run/reimu-diag/ | grep -c tar.gz").strip() == "0", 90), "gone after its time without a download")
    b.ssh("rm -rf /run/systemd/system/reimu-logd.service.d; systemctl daemon-reload; systemctl restart reimu-logd")


def t_web(b):
    e2e, chrome = os.environ.get("E2E"), os.environ.get("CHROME")
    if not e2e or not chrome:
        print("    (no E2E= CHROME=: skipped)")
        return
    make_user("evop", "Operator")
    warn(b, "EV-WEB", "crit")
    wait(lambda: "EV-WEB" in log_text(b), 20, 1)
    r = subprocess.run(["node", os.path.join(e2e, "logs.qemu.mjs")], cwd=e2e, capture_output=True, text=True, timeout=600,
                       env=dict(os.environ, BMC_URL="https://127.0.0.1:%d" % HTTPS, BASE="https://127.0.0.1:%d" % HTTPS, E2E_PASS="0penBmc"))
    print("    " + r.stdout.strip().replace("\n", "\n    ")[-1800:])
    check(r.returncode == 0, "the browser test: %s" % r.stderr.strip()[-200:])
    rf("DELETE", "/redfish/v1/AccountService/Accounts/evop")


TESTS = [("journald", t_journald), ("flap", t_flap), ("events", t_events), ("crash", t_crash), ("flood", t_flood),
         ("rest", t_rest), ("diag", t_diag), ("web", t_web), ("boot", t_boot), ("retention", t_retention)]

if __name__ == "__main__":
    b = F.Platform("logs", T.fresh("logs"), power=False).start()
    try:
        check(b.wait_ssh(900), "up")
        time.sleep(20)
        for name, fn in TESTS:
            if ONLY and name not in ONLY:
                continue
            print("== " + name, flush=True)
            t = time.time()
            try:
                fn(b)
            except Exception as e:  # a test that crashes is a failure, the others still run
                check(False, "%s: %r" % (name, e))
            print("   (%d s)" % (time.time() - t), flush=True)
        print("    RSS reimu-logd %s KiB; rofs: see the build's A/B sizes" % b.out("grep VmRSS /proc/$(pidof reimu-logd)/status 2>/dev/null | tr -s ' ' | cut -d' ' -f2").strip())
    finally:
        b.stop()
    print("\n%d failed" % len(failed) + "".join("\n  " + f for f in failed))
    sys.exit(1 if failed else 0)
