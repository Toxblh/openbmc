#!/usr/bin/env python3
"""reimu-flash on files, with writes that go wrong (flaky-pwrite.so).

Usage: test_reimu_flash.py <reimu-flash> <flaky-pwrite.so> <workdir>
"""
import os, random, shutil, subprocess, sys

FLASH, FLAKY, W = sys.argv[1:4]
shutil.rmtree(W, ignore_errors=True)
os.makedirs(W)
failed = 0


def check(cond, what):
    global failed
    print(("PASS " if cond else "FAIL ") + what)
    failed += not cond


def run(*a, bad=None):
    env = dict(os.environ)
    if bad is not None:
        env.update(LD_PRELOAD=FLAKY, FLAKY_BAD=str(bad))
    return subprocess.run([FLASH] + list(a), capture_output=True, text=True, env=env)


dev = os.path.join(W, "dev")
img = os.path.join(W, "img")
open(dev, "wb").write(b"\xff" * (1 << 20))
data = bytes(random.Random(1).getrandbits(8) for _ in range(150000))
open(img, "wb").write(data)


def on_dev(off):
    d = open(dev, "rb").read()
    return d[off:off + len(data)] == data and d[off + len(data):off + 155648] == b"\xff" * (155648 - len(data))


r = run("write", dev, "0x10000", img)
check(r.returncode == 0 and on_dev(0x10000), "write, read back, padded with 0xff")
check(run("verify", dev, "0x10000", img).returncode == 0, "verify: identical")
r = run("write", dev, "0x10000", img)
check(r.returncode == 0 and r.stdout.strip().endswith("ok 0"), "same data again: nothing rewritten")

open(dev, "wb").write(b"\xff" * (1 << 20))
r = run("write", dev, "0x20000", img, bad=3)
check(r.returncode == 0 and on_dev(0x20000), "3 bad writes: found on reading back, written again")
check("differ after writing" in r.stderr, "the rewrite is reported")

open(dev, "wb").write(b"\xff" * (1 << 20))
r = run("write", dev, "0x20000", img, "3", bad=-1)
check(r.returncode != 0 and "giving up" in r.stderr, "writes always bad: fails after 3 tries")
check(run("verify", dev, "0x20000", img).returncode == 1, "verify: differs")

with open(dev, "r+b") as f:
    f.seek(0x10000 + 5000)
    f.write(b"\x00")
check(run("verify", dev, "0x10000", img).returncode == 1, "one changed byte: verify fails")

check(run("write", dev, "0x10001", img).returncode != 0, "offset not on an erase block: refused")
check(run("write", dev, "0xf0000", img).returncode != 0, "past the end: refused")
check(run("write", dev, "0", os.devnull).returncode != 0, "empty image: refused")

sys.exit(1 if failed else 0)
