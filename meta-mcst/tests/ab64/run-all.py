#!/usr/bin/env python3
"""Every A/B test suite at once: one QEMU per test group, as many as there are
cores, then one table. Runs on the machine with QEMU, not in the build
container (docker is used for the tree, the build and the test_static kind).

    run-all.py [--jobs N] [--only REGEX] [--keep] [--timeout MIN] [--no-fetch]

Results in $REIMU_WORK/runs/<date>/: summary.txt, timings.json, a log per job.
REIMU_WORK (default ~/git/new-reimu/work) also holds fixtures/ (copied into every
job's dir: pns.bin, fru.bin, pns-orig.bin of the board, pns-new.bin, pns-fault.so,
kernel-other.fit, stock-kexec/) and e2e/node_modules. A missing fixture stops
the run: a suite would quietly skip what needs it. The exit status is 0 only if all passed.
"""
import argparse, concurrent.futures as cf, json, os, re, shutil, signal, subprocess, sys, threading, time

WORK = os.path.expanduser(os.environ.get("REIMU_WORK", "~/git/new-reimu/work"))
STOCK = os.path.expanduser(os.environ.get("STOCK", "~/git/new-reimu/backup"))
TREE = "/work/openbmc/meta-mcst"
DOCKER = "reimu"
BASE_PORT = 20000     # above the ports Chrome refuses (5060, 5061, 6000...)
FIXTURES = ["pns.bin", "fru.bin", "pns-orig.bin", "pns-new.bin", "pns-fault.so", "kernel-other.fit", "stock-kexec"]
SLOW = {"ab.restorecut": 120, "ipmi_redfish": 120}     # minutes, instead of --timeout
AB = "/work/openbmc/meta-mcst/tests/ab64"
FLASH_SRC = "/work/openbmc/meta-mcst/meta-ab64/recipes-mcst/reimu-ab/files/reimu-flash.c"
FLASH_TEST = ("d=$(mktemp -d) && gcc -O2 -o $d/reimu-flash %s && gcc -shared -fPIC -o $d/flaky.so %s/flaky-pwrite.c -ldl"
              " && python3 %s/test_reimu_flash.py $d/reimu-flash $d/flaky.so $d/w; r=$?; rm -rf $d; exit $r" % (FLASH_SRC, AB, AB))
CONTAINER_SUITES = {"test_static": ["python3", AB + "/test_static.py"], "test_ab_plan": ["python3", AB + "/test_ab_plan.py"], "test_ab_lock": ["python3", AB + "/test_ab_lock.py"],
                    "test_reimu_fit": ["python3", AB + "/test_reimu_fit.py"],
                    "test_pns_tool": ["python3", AB + "/test_pns_tool.py"],
                    "test_check_image": ["python3", AB + "/test_check_image.py"],
                    "test_ota_tool": ["python3", AB + "/test_ota_tool.py"],
                    "test_ipmi_version": ["python3", AB + "/test_ipmi_version.py"],
                    "test_usbhid_writes": ["python3", AB + "/test_usbhid_writes.py"], "test_reimu_flash": ["sh", "-c", FLASH_TEST]}
WWW = "meta-common/recipes-phosphor/reimu-webui/files/www"
QEMU_SUITES = ["ab", "agentux", "docs_smoke", "fand", "fru_sensors", "hostlink", "ipmi_redfish",
               "kvm_hid", "logs", "overheatd", "pns", "regressions"]
# bug-for-bug: stock + new need both images, the group list below is the new one's
PASS = re.compile(r"^\s*PASS\b", re.M)
FAIL = re.compile(r"^ {0,2}FAIL\b", re.M)     # deeper: a tool's output a test quotes


def groups_of(path):
    """the group names of a suite, one job each; [None]: the suite as one job"""
    s = open(path).read()
    m = re.search(r"^TESTS\s*=\s*\[(.*?)^\]|^TESTS\s*=\s*\[(.*?)\]\s*$", s, re.M | re.S)
    if m:
        return re.findall(r'\(\s*"([a-z0-9_-]+)"\s*,', m.group(1) or m.group(2)) or [None]
    m = re.search(r'^GROUPS\s*=\s*GROUPS_ARG or \[(.*?)\]', s, re.M)
    if m:
        return re.findall(r'"([a-z0-9_-]+)"', m.group(1)) or [None]
    m = re.search(r"^GROUPS\s*=\s*\[(.*?)^\S", s + "\n.", re.M | re.S)     # [("group", [("test", t_test)...
    if m:
        return re.findall(r'\("([a-z0-9_-]+)",\s*t_', m.group(1)) or [None]
    return [None]


class Job:
    def __init__(self, name, cmd=None, suite=None, group=None):
        self.name, self.cmd, self.suite, self.group = name, cmd, suite, group
        self.rc, self.secs, self.passed, self.failed, self.log = None, 0, 0, 0, ""


def run_job(job, slot, a, out, src, img):
    log = os.path.join(out, job.name + ".log")
    job.log = log
    t0 = time.time()
    with open(log, "wb") as f:
        if job.cmd:      # in the container
            p = subprocess.Popen(job.cmd, stdout=f, stderr=subprocess.STDOUT, start_new_session=True)
            d = None
        else:
            d = os.path.join(out, "w", job.name)
            os.makedirs(d, exist_ok=True)
            for n in ("ab.mtd", "ab-build.mtd", "reimu-ab.tar", "reimu-ab-kexec.tar"):
                subprocess.check_call(["cp", "-c", os.path.join(img, n), d])
            for n in FIXTURES:
                subprocess.check_call(["cp", "-Rc", os.path.join(WORK, "fixtures", n), d])
            port = a.port + 10 * slot
            rq = "/tmp/ra/%d" % port
            shutil.rmtree(rq, ignore_errors=True)
            os.makedirs(rq)
            env = dict(os.environ, RQ=rq, AB_SSH=str(port), AB_HTTPS=str(port + 1), AB_IPMI=str(port + 2),
                       HL_PORT=str(port + 3), DOCS_SSH2022=str(port + 4), DOCS_VNC=str(port + 5), STOCK=STOCK,
                       E2E=os.path.join(out, "e2e"), AUX_E2E=os.path.join(out, "e2e"),
                       PYTHONUNBUFFERED="1")
            if job.suite in ("hostlink", "agentux", "overheatd"):
                env["AB_NETDEV_EXTRA"] = ",hostfwd=tcp:127.0.0.1:%d-:5564" % (port + 3)
            ch = chromium()
            if ch:
                env["CHROME"] = ch
            py = os.path.join(src, "meta-mcst/tests/ab64/test_%s_qemu.py" % job.suite)
            cmd = ["python3", "-u", py, d] + ([job.group] if job.group else [])
            p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, env=env, cwd=d, start_new_session=True)
        limit, quiet = SLOW.get(job.name, a.timeout) * 60, a.quiet * 60
        while job.rc is None:
            try:
                job.rc = p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                now = time.time()
                if now - os.path.getmtime(log) > quiet:      # a hang, not a slow test: say so now
                    job.rc = "stalled"
                elif now - t0 > limit:
                    job.rc = "timeout"
    try:
        os.killpg(p.pid, signal.SIGKILL)    # whatever it left, its QEMU first
    except (ProcessLookupError, PermissionError):     # EPERM on macOS: only zombies left
        pass
    if d:
        subprocess.call(["pkill", "-9", "-f", d])
    job.secs = time.time() - t0
    text = open(log, errors="replace").read()
    if job.rc == "stalled":
        print("STALLED %s: nothing in its log for %d min, stopped. Its last lines:\n%s" % (
            job.name, a.quiet, "\n".join("    " + l[:150] for l in text.splitlines()[-4:])), flush=True)
    job.passed, job.failed = len(PASS.findall(text)), len(FAIL.findall(text))
    if job.suite == "ipmi_redfish":     # FAIL lines there: the stock firmware's too, its verdict is these two
        job.failed = 0 if "regressions: none" in text and "MUST checks failed on the new firmware: none" in text else 1
    if d and not a.keep:
        shutil.rmtree(d, ignore_errors=True)
    return job


def chromium():
    root = os.path.expanduser("~/Library/Caches/ms-playwright")
    for d in sorted(os.listdir(root)) if os.path.isdir(root) else []:
        p = os.path.join(root, d, "chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing")
        if d.startswith("chromium-") and os.path.exists(p):
            return p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--only", default="")
    ap.add_argument("--port", type=int, default=BASE_PORT, help="a second run at once: another range")
    ap.add_argument("--quiet", type=int, default=10, help="minutes without a log line: a hang")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--timeout", type=int, default=40)
    ap.add_argument("--no-fetch", action="store_true", help="reuse the image in runs/image")
    a = ap.parse_args()

    try:      # the installers call it; a hung mount here would look like a hang of theirs
        subprocess.run(["sync"], timeout=20)
    except subprocess.TimeoutExpired:
        print("WARNING: sync doesn't end on this machine (a hung network mount?)", flush=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out = os.path.join(WORK, "runs", stamp)
    os.makedirs(out)
    src, img = os.path.join(out, "src"), os.path.join(WORK, "runs", "image")
    os.makedirs(src)
    subprocess.check_call("docker exec %s tar -C %s/.. -cf - meta-mcst/tests meta-mcst/tools meta-mcst/%s | tar -xf - -C %s" % (DOCKER, TREE, WWW, src), shell=True)
    os.makedirs(img, exist_ok=True)
    gone = [n for n in FIXTURES if not os.path.exists(os.path.join(WORK, "fixtures", n))]
    if gone or not chromium() or not os.path.isdir(os.path.join(WORK, "e2e/node_modules/playwright")):
        sys.exit("missing: fixtures %s, chromium %s, e2e/node_modules/playwright" % (gone, chromium()))
    e2e = os.path.join(out, "e2e")      # the browser tests with their modules
    shutil.copytree(os.path.join(src, "meta-mcst/tests/e2e"), e2e, ignore=shutil.ignore_patterns("node_modules"))
    os.symlink(os.path.join(WORK, "e2e/node_modules"), os.path.join(e2e, "node_modules"))
    if not a.no_fetch:
        subprocess.check_call(["sh", os.path.join(src, "meta-mcst/tests/ab64/fetch.sh"), img],
                              stdout=subprocess.DEVNULL, env=dict(os.environ, STOCK=STOCK))
    rev = subprocess.check_output(["docker", "exec", DOCKER, "git", "-C", TREE, "describe", "--always", "--dirty"], text=True).strip()

    jobs = []
    for t, c in CONTAINER_SUITES.items():
        jobs.append(Job(t, ["docker", "exec", DOCKER] + c))
    # no QEMU, on this machine: the web tree in a browser (PY: a python with pyte for real zsh, vim)
    jobs.append(Job("console_size", ["env", "WWW=" + os.path.join(src, "meta-mcst", WWW), "CHROME=" + chromium(),
                                     "node", os.path.join(e2e, "console-size.stub.mjs")]))
    jobs.append(Job("pns_blank", ["env", "WWW=" + os.path.join(src, "meta-mcst", WWW), "CHROME=" + chromium(),
                                  "node", os.path.join(e2e, "pns-blank.stub.mjs")]))
    jobs.append(Job("ota_page", ["env", "WWW=" + os.path.join(src, "meta-mcst", WWW), "CHROME=" + chromium(),
                                 "node", os.path.join(e2e, "ota.stub.mjs")]))
    jobs.append(Job("map", ["env", "WWW=" + os.path.join(src, "meta-mcst", WWW), "CHROME=" + chromium(),
                            "node", os.path.join(e2e, "map.stub.mjs")]))
    for s in QEMU_SUITES:
        py = os.path.join(src, "meta-mcst/tests/ab64/test_%s_qemu.py" % s)
        for g in groups_of(py):
            jobs.append(Job(s + ("." + g if g else ""), suite=s, group=g))
    if a.only:
        jobs = [j for j in jobs if re.search(a.only, j.name)]
    tf = os.path.join(WORK, "runs", "timings.json")
    known = json.load(open(tf)) if os.path.exists(tf) else {}
    jobs.sort(key=lambda j: -known.get(j.name, 1e9))     # the longest first

    slots, lock = list(range(a.jobs)), threading.Lock()
    print("%s: %d jobs, %d at a time, %s" % (rev, len(jobs), a.jobs, out), flush=True)

    def one(j):
        with lock:
            s = slots.pop(0)
        try:
            run_job(j, s, a, out, src, img)
        finally:
            with lock:
                slots.append(s)
        print("%-30s %s %5.0fs %d pass %d FAIL" % (j.name, "ok " if j.rc == 0 else "BAD", j.secs, j.passed, j.failed), flush=True)
        return j

    t0 = time.time()
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        list(ex.map(one, jobs))
    wall = time.time() - t0

    lines = ["revision %s, %.0f s wall, %.0f s of work, %d at a time" % (rev, wall, sum(j.secs for j in jobs), a.jobs), ""]
    for j in sorted(jobs, key=lambda j: j.secs, reverse=True):
        lines.append("%-30s %-7s %5.0fs %5d pass %3d FAIL" % (j.name, "ok" if j.rc == 0 else "rc=%s" % j.rc, j.secs, j.passed, j.failed))
    bad = [j for j in jobs if j.rc != 0 or j.failed]
    lines += ["", "%d jobs, %d bad, %d checks passed, %d failed" % (len(jobs), len(bad), sum(j.passed for j in jobs), sum(j.failed for j in jobs))]
    for j in bad:
        lines.append("  BAD %s: %s" % (j.name, j.log))
    text = "\n".join(lines) + "\n"
    open(os.path.join(out, "summary.txt"), "w").write(text)
    known.update({j.name: j.secs for j in jobs})
    json.dump(known, open(tf, "w"), indent=0)
    print(text)
    shutil.rmtree(os.path.join(out, "w"), ignore_errors=True)
    os.unlink(os.path.join(e2e, "node_modules"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
