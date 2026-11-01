#!/usr/bin/env python3
"""reimu-ab boot against install/set-slot running meanwhile (REGRESSIONS 77):
the real do_boot and do_set_slot, the flash and the U-Boot environment faked
(a directory of files).
  A  health comes after set-slot finished: boot must not undo it (slot b, pending 1)
  B  the deadline hits while set-slot is still verifying/writing: boot waits for
     it (no reboot mid-write), then leaves the environment alone, no reboot
  C  a second set-slot while one runs is refused
  D  do_install and do_set_slot take the lock for their whole run

    test_ab_lock.py [REIMU_AB] [SH]
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    HERE, "..", "..", "meta-ab64", "recipes-mcst", "reimu-ab", "files", "reimu-ab")
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
env_set() { printf %%s "$2" > $T/$1; }
booted() { case $1 in kernel|rofs) echo a;; state) echo ok;; esac; }
kexec_started() { return 1; }
systemctl() { echo "$*" >> $T/systemctl; }
logger() { :; }
sleep() { command sleep 0.2; }
healthy() { [ -f $T/healthy ]; }
verify_slot() { command sleep ${VSLEEP:-0}; }
HEALTH_DEADLINE=%(dl)s
"""


def scenario(body, dl=999999999):
    with tempfile.TemporaryDirectory() as t:
        open(t + "/reimu_slot", "w").write("a")
        open(t + "/reimu_pending", "w").write("1")
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

print("\n%d failed" % failed)
sys.exit(1 if failed else 0)
