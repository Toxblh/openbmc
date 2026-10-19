#!/usr/bin/env python3
"""Writes a synthetic 16 MiB host boot firmware (PNS) flash image: partition
table (magic 0xB007E2E2 in the last 4 KiB sector of the first 16 MiB, as read
by mcst-fruid) with three entries, one of them a DTB at 0x200000."""
import struct
import sys

size = 16 << 20
img = bytearray(b"\xff" * size)
dtb_off, dtb_len = 0x200000, 0x3000
img[dtb_off:dtb_off + dtb_len] = struct.pack(">II", 0xd00dfeed, dtb_len) + b"\0" * (dtb_len - 8)
pt = size - 4096
hdr = struct.pack("<III", 0xb007e2e2, 1, 3) + struct.pack("<8I", 0xcafebabe, *[0] * 7) + b"\0" * 20
ents = [(0x00000001, 0, 0x180000, 0x11111111), (0xff000003, dtb_off, 0x10000, 0x22222222), (0x00000002, 0x400000, 0x800000, 0x33333333)]
body = b"".join(struct.pack("<III", s, o, l) + struct.pack("<8I", c, *[0] * 7) + b"\0" * 20 for s, o, l, c in ents)
img[pt:pt + 64 + len(body)] = hdr + body
open(sys.argv[1], "wb").write(img)
