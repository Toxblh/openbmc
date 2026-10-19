#!/usr/bin/env python3
"""Scenario tests for reimu-bootguard with emulated hardware: a register file
instead of /dev/mem (SCU strap, WDT2), two files as the BMC flash chips and
stub fw_printenv/fw_setenv.

Usage: test_reimu_bootguard.py <reimu-bootguard> <reimu-wdt2> <reimu-flashcopy> <workdir>
"""
import hashlib, json, os, shutil, struct, subprocess, sys, time

SCRIPT, WDT2, FLASHCOPY, WORK = sys.argv[1:5]
shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK)
MIRROR = 4 << 20       # 4 MiB "image" area (48 MiB on hardware)
CHIP = 6 << 20         # chip size; the tail plays the rwfs partition
failed = 0


def check(cond, what):
    global failed
    print(("PASS " if cond else "FAIL ") + what)
    failed += not cond


# ---- emulated registers: page 0 = SCU, page 1 = WDT block ----
REGS = os.path.join(WORK, "devmem")


def regs(strap=True, secondary=False, enabled=True):
    b = bytearray(0x2000)
    struct.pack_into("<I", b, 0x70, (1 << 17) if strap else 0)
    struct.pack_into("<I", b, 0x1000 + 0x20 + 0x04, 300000000)
    struct.pack_into("<I", b, 0x1000 + 0x20 + 0x00, 280000000)
    struct.pack_into("<I", b, 0x1000 + 0x20 + 0x0c, 0x13 if enabled else 0x12)
    struct.pack_into("<I", b, 0x1000 + 0x20 + 0x10, 0x2 if secondary else 0)
    open(REGS, "wb").write(b)


def wdt_enabled():
    return struct.unpack_from("<I", open(REGS, "rb").read(), 0x1000 + 0x2c)[0] & 1


# ---- emulated U-Boot environment ----
ENVF = os.path.join(WORK, "env")
for name, body in (("fw_printenv", 'v=$(sed -n "s/^$2=//p" %s); [ -n "$v" ] && echo "$v"' % ENVF),
                   ("fw_setenv", 'grep -v "^$1=" %s > %s.t 2>/dev/null; echo "$1=$2" >> %s.t; mv %s.t %s' % ((ENVF,) * 5))):
    p = os.path.join(WORK, name)
    open(p, "w").write("#!/bin/sh\n" + body + "\n")
    os.chmod(p, 0o755)


def env(k):
    for l in open(ENVF).read().splitlines() if os.path.exists(ENVF) else []:
        if l.startswith(k + "="):
            return l.split("=", 1)[1]
    return None


# ---- emulated flash chips ----
BMC, BMC2 = os.path.join(WORK, "bmc.img"), os.path.join(WORK, "bmc2.img")


def chip(path, seed):
    data = bytearray(hashlib.sha256(seed.encode()).digest() * (CHIP // 32))
    open(path, "wb").write(data)


def md5(path, n=MIRROR):
    return hashlib.md5(open(path, "rb").read(n)).hexdigest()


def tail(path):
    return hashlib.md5(open(path, "rb").read()[MIRROR:]).hexdigest()


HEALTH = os.path.join(WORK, "health")
open(HEALTH + ".sh", "w").write("#!/bin/sh\n[ -f %s ]\n" % HEALTH)
os.chmod(HEALTH + ".sh", 0o755)
OSREL = os.path.join(WORK, "os-release")
open(OSREL, "w").write('VERSION_ID="1.1.0-test"\nPRETTY_NAME="REIMU test 1.1.0"\n')

ENV = dict(os.environ, REIMU_DEVMEM=REGS, WDT2=WDT2, FLASHCOPY=FLASHCOPY, FW_PRINTENV=os.path.join(WORK, "fw_printenv"),
           FW_SETENV=os.path.join(WORK, "fw_setenv"), MTD_BMC=BMC, MTD_BMC2=BMC2, ROFS_OTHER=os.path.join(WORK, "none"),
           RUN=os.path.join(WORK, "run"), STATE=os.path.join(WORK, "state"), MIRROR_SIZE=str(MIRROR),
           HEALTH_UNITS="", HEALTH_URL="", HEALTH_CMD=HEALTH + ".sh", HEALTH_DEADLINE="999999999", OS_RELEASE=OSREL,
           STAGED=os.path.join(WORK, "initramfs"), PATH=WORK + ":" + os.environ["PATH"])
STAGED = ENV["STAGED"]
os.makedirs(STAGED)


def run(*args, timeout=600, **extra):
    e = dict(ENV, **extra)
    p = subprocess.run(["bash", SCRIPT, *args], env=e, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def status():
    rc, out = run("status", "--json")
    return json.loads(out.strip().splitlines()[-1])


# 1. no strap: nothing to do
regs(strap=False)
rc, out = run("boot")
check(rc == 0 and "no dual boot strap" in out, "no strap: boot exits quietly")

# 2. normal boot from primary: disarm right away
regs()
chip(BMC, "A"); chip(BMC2, "B")
rc, out = run("boot")
check(rc == 0 and not wdt_enabled(), "normal boot: WDT2 disarmed")

# 3. prepare-update mirrors the running image to the backup chip first
regs()
rwfs2 = tail(BMC2)
rc, out = run("prepare-update")
check(rc == 0, f"prepare-update succeeded ({out.strip().splitlines()[-1]})")
check(md5(BMC) == md5(BMC2), "backup chip now holds the running image")
check(tail(BMC2) == rwfs2, "backup chip's rwfs area untouched")
check(env("wdcontinue") == "1", "wdcontinue=1 set for the next boot")
check(os.path.exists(os.path.join(WORK, "state", "mirror")), "mirror record written")

# 3b. an update staged by Redfish / phosphor-updater without prepare-update
open(ENVF, "w").write("")
chip(BMC2, "B")
open(os.path.join(STAGED, "image-rofs"), "w").write("x")
rc, out = run("guard-activation")
check(rc == 0 and md5(BMC) == md5(BMC2) and env("wdcontinue") == "1", "staged update guarded: backup mirrored, wdcontinue=1")
rc, out = run("guard-activation")
check(rc == 0 and os.listdir(STAGED), "guard-activation is idempotent when already prepared")
os.unlink(os.path.join(STAGED, "image-rofs"))

# 4. a second prepare while pending is refused
rc, out = run("prepare-update")
check(rc != 0 and "pending" in out, "prepare-update refused while an update is pending")

# 5. "flash" the new image to the primary, boot, health fails until the deadline
old_image = md5(BMC)
chip(BMC, "NEW")
regs()
if os.path.exists(HEALTH):
    os.unlink(HEALTH)
rc, out = run("boot", HEALTH_DEADLINE="0")
check(rc == 1 and wdt_enabled(), "unhealthy new image: WDT2 left armed (backup will start)")
check(env("wdcontinue") == "1", "unhealthy new image: update not committed")
check(md5(BMC2) == old_image, "backup still holds the previous image")
check(status()["health"] == f"{HEALTH}.sh failed", "status reports why the image is unhealthy")

# 6. WDT2 fired: booted from the backup chip -> disarm, no further fallback
regs(secondary=True)
rc, out = run("boot")
check(rc == 0 and not wdt_enabled(), "booted from backup: WDT2 disarmed (no ping-pong)")
st = status()
check(st["bootedFrom"] == "backup" and st["fallback"] is not None, "status: running from backup, fallback recorded")
rc, out = run("prepare-update")
check(rc != 0 and "restore the primary" in out, "update refused while running from backup")
rc, out = run("sync-backup")
check(rc != 0, "sync-backup refused while running from backup")
open(os.path.join(STAGED, "image-rofs"), "w").write("x")
rc, out = run("guard-activation")
check(rc != 0 and not os.listdir(STAGED), "update staged by another updater on backup: image removed")

# 7. restore-primary: on backup, "bmc" is the backup chip (swapped by hardware)
#    and "bmc2" the broken primary: copy running -> other
ENV_SWAPPED = dict(MTD_BMC=BMC2, MTD_BMC2=BMC)
rc, out = run("restore-primary", **ENV_SWAPPED)
check(rc == 0 and md5(BMC) == old_image, "restore-primary copies the working image onto the primary chip")
rc, out = run("reboot-primary", **ENV_SWAPPED)
clr = struct.unpack_from("<I", open(REGS, "rb").read(), 0x1000 + 0x34)[0]
check(rc == 0 and "reboot" in out and clr == 1, "reboot-primary clears the boot code selection and reboots")
regs()
rc, out = run("reboot-primary")
check(rc != 0, "reboot-primary refused when running from the primary chip")

# 8. healthy pending update: commit, then the backup is refreshed automatically
chip(BMC, "NEW2")
new_image = md5(BMC)
regs()
open(ENVF, "w").write("wdcontinue=1\n")
open(HEALTH, "w").write("ok")
t0 = time.time()
rc, out = run("boot")
check(rc == 0 and not wdt_enabled(), "healthy new image: WDT2 disarmed")
check(env("wdcontinue") == "0", "healthy new image: update committed (wdcontinue=0)")
check(md5(BMC2) == new_image, f"backup refreshed with the confirmed image ({time.time()-t0:.1f}s)")
st = status()
check(st["committed"]["version"] == "1.1.0-test" and st["job"]["state"] == "ok", "status: commit and sync job recorded")

# 9. background job with progress, and only one job at a time
chip(BMC, "NEW3")
rc, out = run("job", "sync-backup")
check(rc == 0 and "started" in out, "job started in background")
rc2, out2 = run("job", "sync-backup")
check(rc2 != 0 and "another job" in out2, "second concurrent job refused")
for _ in range(120):
    st = status()
    if st["job"] and st["job"]["state"] != "running":
        break
    time.sleep(0.5)
check(st["job"]["state"] == "ok" and md5(BMC2) == md5(BMC), f"job finished: {st['job']['message']}")
rc, out = run("status", "--json", "--verify")
check(json.loads(out)["inSync"] is True, "status --verify: chips identical")
chip(BMC2, "garbage")
rc, out = run("status", "--json", "--verify")
check(json.loads(out)["inSync"] is False, "status --verify detects a differing backup")

# 10. a write error is reported and the job marked failed
os.chmod(BMC2, 0o444)
if os.geteuid() != 0:
    rc, out = run("sync-backup")
    check(rc != 0 and "write failed" in out, "write error reported")
os.chmod(BMC2, 0o644)

# 11. the full-size mirror (48 MiB) as on hardware: timing only
chip(BMC, "BIG"); chip(BMC2, "BIG2")
big = 48 << 20
for p, s in ((BMC, "X"), (BMC2, "Y")):
    open(p, "wb").write(hashlib.sha256(s.encode()).digest() * ((big + (1 << 20)) // 32))
t0 = time.time()
rc, out = run("sync-backup", MIRROR_SIZE=str(big))
check(rc == 0 and md5(BMC, big) == md5(BMC2, big), f"48 MiB mirror ({time.time()-t0:.1f}s on this host)")
t0 = time.time()
rc, out = run("sync-backup", MIRROR_SIZE=str(big))
check(rc == 0 and "0 blocks written" in out, f"repeated mirror writes nothing ({time.time()-t0:.1f}s)")

print("RESULT:", "FAILED" if failed else "ALL PASSED")
sys.exit(1 if failed else 0)
