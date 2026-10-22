#!/usr/bin/env python3
"""ubenv.py IMAGE [NAME=VALUE | NAME= ...]: print or change the redundant
U-Boot environment of a REIMU flash image (0xe0000 and 0xf0000, 64 KiB each,
CRC32 + flag byte). Without changes, prints the active copy."""
import sys, zlib
OFF, SIZE = (0xe0000, 0xf0000), 0x10000

def read(img):
    best = None
    for o in OFF:
        blk = img[o:o + SIZE]
        crc = int.from_bytes(blk[0:4], 'little')
        if zlib.crc32(blk[5:]) & 0xffffffff == crc:
            if best is None or blk[4] == 1:
                best = blk
    if best is None:
        sys.exit('no valid environment')
    env = {}
    for kv in best[5:].split(b'\0'):
        if not kv:
            break
        k, _, v = kv.partition(b'=')
        env[k.decode()] = v.decode()
    return env

def write(img, env):
    data = b''.join(('%s=%s' % kv).encode() + b'\0' for kv in env.items()) + b'\0'
    data = data + b'\0' * (SIZE - 5 - len(data))
    crc = (zlib.crc32(data) & 0xffffffff).to_bytes(4, 'little')
    for o, flag in zip(OFF, (1, 0)):
        img[o:o + SIZE] = crc + bytes([flag]) + data

if __name__ == '__main__':
    p = sys.argv[1]
    img = bytearray(open(p, 'rb').read())
    env = read(img)
    if len(sys.argv) == 2:
        for k, v in env.items():
            print('%s=%s' % (k, v))
        sys.exit()
    for a in sys.argv[2:]:
        k, _, v = a.partition('=')
        if v:
            env[k] = v
        else:
            env.pop(k, None)
    write(img, env)
    open(p, 'r+b').write(img)
