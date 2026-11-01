#!/usr/bin/env python3
"""reimu-ab boot against install/set-slot running meanwhile (REGRESSIONS 77),
what a manual restart of it or a non-A/B bootcmd does: the real do_boot,
do_install and do_set_slot, the flash and the U-Boot environment faked (a
directory of files).
  A  health comes after set-slot finished: boot must not undo it (slot b, pending 1)
  B  the deadline hits while set-slot is still verifying/writing: boot waits for
     it (no reboot mid-write), then leaves the environment alone, no reboot
  C  a second set-slot while one runs is refused
  D  do_install and do_set_slot take the lock for their whole run
  E  A with a failing logger: "changed" still reads as changed
  F  reimu-ab boot again after an install decided this boot (systemctl restart):
     the environment stays; G  without one, a pending slot is still confirmed
  H  set-slot, I  install with a non-A/B bootcmd: refused, the environment stays;
     J  from RAM with the stock bootcmd: refused, after setup-env it installs;
     K  reimu-ab-from-stock runs setup-env before the install, restore-stock and
     rwfs-init (which leave or never need the A/B bootcmd) aren't guarded

    test_ab_lock.py [REIMU_AB] [SH]
"""
import hashlib, os, re, subprocess, sys, tarfile, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    HERE, "..", "..", "meta-ab64", "recipes-mcst", "reimu-ab", "files", "reimu-ab")
FROM_STOCK = os.path.join(HERE, "..", "..", "tools", "ab64", "reimu-ab-from-stock")
AB_BOOTCMD, STOCK_BOOTCMD = "run reimu_ab", "bootm 20100000"
SH = sys.argv[2] if len(sys.argv) > 2 else "sh"
failed = 0


def check(c, what):
    global failed
    print(("ok   " if c else "FAIL ") + what)
    failed += not c


PRE = r"""
T=%(t)s; export T
REIMU_AB_LIB=1 . %(tool)s
LOCK=$T/lock; REIMU_AB_LOCK=$LOCK; DECIDED=$T/decided; REIMU_AB_DECIDED=$DECIDED
env_get() { cat $T/$1 2>/dev/null; }
env_set() { printf %%s "${2:-}" > $T/$1; }
booted() { case $1 in kernel|rofs) echo a;; state) echo ok;; esac; }
kexec_started() { return 1; }
systemctl() { echo "$*" >> $T/systemctl; }
logger() { :; }
sleep() { command sleep 0.2; }
healthy() { [ -f $T/healthy ]; }
verify_slot() { command sleep ${VSLEEP:-0}; }
HEALTH_DEADLINE=%(dl)s
"""


def scenario(body, dl=999999999, slot="a", bootcmd=AB_BOOTCMD):
    with tempfile.TemporaryDirectory() as t:
        open(t + "/reimu_slot", "w").write(slot)
        open(t + "/reimu_pending", "w").write("1")
        open(t + "/bootcmd", "w").write(bootcmd)
        s = PRE % {"t": t, "tool": TOOL, "dl": dl} + body
        r = subprocess.run([SH], input=s, capture_output=True, text=True, timeout=60)
        g = lambda n: open(t + "/" + n).read() if os.path.exists(t + "/" + n) else ""
        return g("reimu_slot"), g("reimu_pending"), g("systemctl").strip(), r


# A
sl, pe, rb, r = scenario("""
do_boot & b=$!
command sleep 0.5
( do_set_slot b ) >/dev/null
touch $T/healthy
wait $b
""")
check((sl, pe, rb) == ("b", "1", ""), "A: set-slot b finished, then healthy: slot %s pending %s reboot '%s' (want b 1 none) %s" % (sl, pe, rb, r.stderr[-200:]))

# B
sl, pe, rb, r = scenario("""
( VSLEEP=2 do_set_slot b ) >/dev/null &
s=$!
command sleep 0.5
do_boot &
b=$!
command sleep 1
[ -f $T/systemctl ] && echo REBOOT-MID-WRITE > $T/mid
wait $s; wait $b
""", dl=0)
mid = os.path.exists  # noqa
check((sl, pe, rb) == ("b", "1", ""), "B: deadline during set-slot: slot %s pending %s reboot '%s' (want b 1 none)" % (sl, pe, rb))

# C
with tempfile.TemporaryDirectory() as t:
    open(t + "/reimu_slot", "w").write("a")
    open(t + "/bootcmd", "w").write(AB_BOOTCMD)
    s = PRE % {"t": t, "tool": TOOL, "dl": 999999999} + """
( VSLEEP=2 do_set_slot b ) >/dev/null &
command sleep 0.5
( do_set_slot a ) >/dev/null 2>$T/err; echo $? > $T/rc
wait
"""
    subprocess.run([SH], input=s, capture_output=True, text=True, timeout=60)
    rc = open(t + "/rc").read().strip() if os.path.exists(t + "/rc") else "?"
    check(rc not in ("0", "?") and open(t + "/reimu_slot").read() == "b", "C: second set-slot refused (rc %s)" % rc)

# D
src = open(TOOL).read()
for fn in ("do_install", "do_set_slot"):
    b = src[src.index(fn + "() {"):]
    b = b[:b.index("\n}\n")]
    check(re.search(r"^\s*lock_\w+", b, re.M) is not None, "D: %s takes the lock" % fn)

# E
sl, pe, rb, r = scenario("""
logger() { return 1; }
do_boot & b=$!
command sleep 0.5
( do_set_slot b ) >/dev/null
touch $T/healthy
wait $b
""")
check((sl, pe, rb) == ("b", "1", ""), "E: logger fails, set-slot b finished, then healthy: slot %s pending %s reboot '%s' (want b 1 none) %s" % (sl, pe, rb, r.stderr[-200:]))

# F: booted a, an install chose b this boot, reimu-ab.service restarted
sl, pe, rb, r = scenario("""
echo decided > $DECIDED
touch $T/healthy
do_boot; echo "rc $?"
""", slot="b")
check((sl, pe, rb) == ("b", "1", "") and "rc 0" in r.stdout, "F: boot again after a decided install: slot %s pending %s reboot '%s' %s (want b 1 none, rc 0)" % (sl, pe, rb, r.stdout.strip()))

# G
sl, pe, rb, r = scenario("""
touch $T/healthy
do_boot
""")
check((sl, pe) == ("a", "0"), "G: nothing decided: a pending slot is confirmed when healthy: slot %s pending %s" % (sl, pe))

# H
sl, pe, rb, r = scenario("""
printf 0 > $T/reimu_pending
( do_set_slot b ) >/dev/null; echo "rc $?"
""", bootcmd=STOCK_BOOTCMD)
check((sl, pe) == ("a", "0") and "rc 1" in r.stdout and "setup-env first" in r.stderr,
      "H: set-slot with a non-A/B bootcmd refused: slot %s pending %s %s %s" % (sl, pe, r.stdout.strip(), r.stderr[-100:]))

# I, J: do_install with a real tar; only the writes and the slots' checks faked
INSTALL = """
choose() { slots=b; plan=rofs-b; good="kernel-a rofs-a"; }
mtd() { echo /dev/$1; }
size_of() { echo 4; }
flash_write() { :; }
"""


def install_tar(d):
    sums = ""
    for n in ("image-kernel", "image-rofs"):
        open(os.path.join(d, n), "w").write("data")
        sums += "%s  %s\n" % (hashlib.sha256(b"data").hexdigest(), n)
    open(os.path.join(d, "SHA256SUMS"), "w").write(sums)
    t = os.path.join(d, "up.tar")
    with tarfile.open(t, "w") as f:
        for n in ("image-kernel", "image-rofs", "SHA256SUMS"):
            f.add(os.path.join(d, n), arcname=n)
    return t


with tempfile.TemporaryDirectory() as d:
    tar = install_tar(d)
    sl, pe, rb, r = scenario(INSTALL + """
printf 0 > $T/reimu_pending
( do_install %s ) >/dev/null; echo "rc $?"
""" % tar, bootcmd=STOCK_BOOTCMD)
    check(pe == "0" and "rc 1" in r.stdout and "setup-env first" in r.stderr,
          "I: install with a non-A/B bootcmd refused: pending %s %s %s" % (pe, r.stdout.strip(), r.stderr[-100:]))
    sl, pe, rb, r = scenario(INSTALL + """
printf 0 > $T/reimu_pending
( do_install %s ) >/dev/null; echo "rc $?"
""" % tar)
    check(pe == "1" and "rc 0" in r.stdout, "I: with the A/B bootcmd it installs: pending %s %s %s" % (pe, r.stdout.strip(), r.stderr[-200:]))
    # J: what reimu-ab-from-stock's kexec gives (state=ram, reimu.ram), stock environment
    sl, pe, rb, r = scenario(INSTALL + """
kexec_started() { return 0; }
booted() { case $1 in kernel) echo a;; rofs) echo ram;; state) echo ram;; esac; }
printf 0 > $T/reimu_pending
( do_install %s ) >/dev/null; echo "before $?"
env_set bootcmd "run reimu_ab"   # do_setup_env, its file aside
( do_install %s ) >/dev/null; echo "after $?"
""" % (tar, tar), bootcmd=STOCK_BOOTCMD)
    check("before 1" in r.stdout and "after 0" in r.stdout and pe == "1",
          "J: RAM, stock bootcmd: refused before setup-env, installs after: %s pending %s %s" % (r.stdout.split(), pe, r.stderr[-200:]))

# K
fs = open(FROM_STOCK).read()
se, ins = fs.find("r 'reimu-ab setup-env'"), fs.find("r 'reimu-ab install ")
check(0 <= se < ins, "K: reimu-ab-from-stock: setup-env (%d) before install (%d)" % (se, ins))
for fn in ("do_rwfs_init", "do_restore_stock", "do_setup_env"):
    b = src[src.index(fn + "() {"):]
    b = b[:b.index("\n}\n")]
    check("ab_bootcmd" not in b, "K: %s doesn't need the A/B bootcmd" % fn)

print("\n%d failed" % failed)
sys.exit(1 if failed else 0)
