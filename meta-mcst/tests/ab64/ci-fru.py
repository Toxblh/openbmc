#!/usr/bin/env python3
"""ci-fru.py BOARD_FRU OUT: a board's FRU ID EEPROM (16 KiB) with its own
identity made up: the MAC base 98:A7:B0:12:34:60, the board serial number
0000-00.00 PAN and the FRU file ID zeros (same lengths); the rest (board
name, part number, layout) and the checksums kept valid. The tests' fru.bin,
published for CI, is this one."""
import sys

b = bytearray(open(sys.argv[1], "rb").read())
assert len(b) == 16384 and b[:2] == b"\x01\x01", "not a REIMU FRU EEPROM"
i = b.index(b"mac_addr") + 9          # its length byte, then 6 bytes
b[i:i + 6] = bytes([0x98, 0xA7, 0xB0, 0x12, 0x34, 0x60])
board = 8 * b[3]                      # IPMI board info area
p = board + 6                         # version, length, language, 3 bytes of date
for n in range(5):                    # manufacturer, product, serial, part, FRU file ID
    tl = b[p]
    ln = tl & 0x3f
    if n == 2:
        b[p + 1:p + 1 + ln] = b"0000-00.00 PAN"[:ln].ljust(ln, b"0")
    if n == 4:
        b[p + 1:p + 1 + ln] = b"0" * ln
    p += 1 + ln


def fix(a, n):                        # the area's last byte: its sum to zero
    b[a + n - 1] = (-sum(b[a:a + n - 1])) & 0xff


fix(8, board - 8)                     # internal use: from 8 to the board area
fix(board, 8 * b[board + 1])
fix(0, 8)
open(sys.argv[2], "wb").write(b)
