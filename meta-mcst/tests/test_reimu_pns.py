#!/usr/bin/env python3
"""Tests reimu-pnsd --parse against synthetic ПНС flash images."""
import json, struct, subprocess, sys, os
BIN, WORK = sys.argv[1], sys.argv[2]
os.makedirs(WORK, exist_ok=True)
failed = False
def check(c, what):
    global failed
    print(("PASS " if c else "FAIL ") + what); failed |= not c

def image(size, with_pt=True, dtb_off=0x700000, dtb_len=0x2345):
    img = bytearray(b"\xff" * size)
    dtb = struct.pack(">II", 0xd00dfeed, dtb_len) + b"\0" * (dtb_len - 8)
    img[dtb_off:dtb_off + dtb_len] = dtb
    if with_pt:
        pt = min(size, 0x1000000) - 4096
        hdr = struct.pack("<III", 0xb007e2e2, 1, 2) + struct.pack("<8I", 0xdeadbeef, *[0]*7) + b"\0" * 20
        e1 = struct.pack("<III", 0xff000003, dtb_off, 0x10000) + struct.pack("<8I", 0x12345678, *[0]*7) + b"\0" * 20
        e2 = struct.pack("<III", 0x00000001, 0, 0x100000) + b"\0" * 32 + b"\0" * 20
        img[pt:pt + 192] = hdr + e1 + e2
    return img

def run(img, name):
    p = os.path.join(WORK, name)
    open(p, "wb").write(img)
    out = subprocess.run([BIN, "--parse", p], capture_output=True, text=True)
    return out.returncode, json.loads(out.stdout)

rc, j = run(image(16 << 20, dtb_off=0x200000), "16m.bin")
check(rc == 0 and j["ok"] and j["size"] == 16 << 20, "16 MiB flash parsed")
pt = j["partitionTable"]
check(pt["present"] and pt["offset"] == (16 << 20) - 4096 and pt["entries"] == 2, "partition table found at size-4K")
check(pt["partitions"][0]["name"] == "device tree (DTB)" and pt["partitions"][0]["offset"] == 0x200000, "DTB partition decoded")
check(pt["partitions"][0]["checksum"].startswith("12345678"), "checksum hex")
check(j["dtb"]["valid"] and j["dtb"]["size"] == 0x2345 and j["dtb"]["address"] == 0x200000, "DTB header validated via partition")
rc, j = run(image(32 << 20), "32m.bin")
check(j["partitionTable"]["offset"] == 0x1000000 - 4096, "32 MiB flash: table at 16M-4K")
rc, j = run(image(8 << 20, with_pt=False), "nopt.bin")
check(not j["partitionTable"]["present"] and j["dtb"]["valid"] and j["dtb"]["address"] == 0x700000, "no table: default DTB address")
img = image(8 << 20, with_pt=False); img[0x700000:0x700004] = b"\0\0\0\0"
rc, j = run(img, "nodtb.bin")
check(j["dtb"]["valid"] is False, "missing DTB detected")
out = subprocess.run([BIN, "--parse", "/nonexistent"], capture_output=True, text=True)
check(out.returncode == 1 and json.loads(out.stdout)["ok"] is False, "missing device reported as JSON error")
print("RESULT:", "FAILED" if failed else "ALL PASSED"); sys.exit(failed)
