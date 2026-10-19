#!/usr/bin/env python3
"""EDID 1.3 of a virtual analog monitor for the BMC's VGA (no monitor attached):
preferred 1920x1200@60 reduced blanking (CVT-RB, 154 MHz), established and
standard timings down to 640x480. Writes reimu-1920x1200.bin."""
import struct

def dtd(pclk_khz, ha, hbl, hfp, hsw, va, vbl, vfp, vsw, hmm, vmm, flags):
    return struct.pack("<H", pclk_khz // 10) + bytes([
        ha & 0xff, hbl & 0xff, ((ha >> 8) << 4) | (hbl >> 8),
        va & 0xff, vbl & 0xff, ((va >> 8) << 4) | (vbl >> 8),
        hfp & 0xff, hsw & 0xff, ((vfp & 0xf) << 4) | (vsw & 0xf),
        ((hfp >> 8) << 6) | ((hsw >> 8) << 4) | ((vfp >> 4) << 2) | (vsw >> 4),
        hmm & 0xff, vmm & 0xff, ((hmm >> 8) << 4) | (vmm >> 8), 0, 0, flags])

def text(tag, s):
    b = s.encode()[:13]
    if len(b) < 13:
        b += b"\n" + b" " * (12 - len(b))
    return bytes([0, 0, 0, tag, 0]) + b

def std(h, aspect, hz=60):  # aspect: 0=16:10, 1=4:3, 2=5:4, 3=16:9
    return bytes([h // 8 - 31, (aspect << 6) | (hz - 60)])

e = bytearray(b"\x00\xff\xff\xff\xff\xff\xff\x00")
m = "RMU"
e += struct.pack(">H", ((ord(m[0]) - 64) << 10) | ((ord(m[1]) - 64) << 5) | (ord(m[2]) - 64))
e += struct.pack("<H", 0x1920) + struct.pack("<I", 1)
e += bytes([1, 36, 1, 3])                    # week 1 of 2026, EDID 1.3
e += bytes([0x0E, 52, 32, 0x78, 0x0A])       # analog, 52x32 cm, gamma 2.2, RGB, preferred timing first
e += bytes([0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54])
e += bytes([0x21, 0x08, 0x00])               # 640x480@60, 800x600@60, 1024x768@60
e += std(1920, 3) + std(1680, 0) + std(1600, 1) + std(1280, 2) + std(1440, 0) + std(1280, 0) + std(1280, 3) + std(1152, 1)
# 1920x1200@60 CVT-RB: 154 MHz, H 1920 +48 fp +32 sync +80 bp, V 1200 +3 fp +6 sync +26 bp, +H -V
e += dtd(154000, 1920, 160, 48, 32, 1200, 35, 3, 6, 520, 325, 0x1A)
e += bytes([0, 0, 0, 0xFD, 0, 50, 75, 30, 80, 17, 0, 0x0A]) + b" " * 6  # 50-75 Hz, 30-80 kHz, 170 MHz
e += text(0xFC, "REIMU KVM")
e += text(0xFF, "BMC-VGA")
e += bytes([0])
e += bytes([(-sum(e)) & 0xff])
assert len(e) == 128
open("reimu-1920x1200.bin", "wb").write(e)
