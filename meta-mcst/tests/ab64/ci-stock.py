#!/usr/bin/env python3
"""ci-stock.py STATIC_MTD OUTDIR: the stock firmware as the tests take it
(STOCK=OUTDIR: u-boot.bin u-boot-env.bin kernel.bin rofs.bin rwfs.bin
SHA256SUMS, laid out as backup-stock.sh dumps a board), made from the public
REIMU 1.0.2 image instead of a board's dump: on CI, and for anyone without a
board.

U-Boot, kernel and rofs are the very bytes a board with 1.0.2 has. The public
image has no environment (erased): it gets the board's one (redundant, 2 x
64 KiB), with made-up locally administered MACs. Its rwfs is the image's own,
nobody's settings.

    curl -LO https://github.com/makise-homura/openbmc/releases/download/reimu-1.0.2/obmc-phosphor-image-reimu-5564.static.mtd
    ci-stock.py obmc-phosphor-image-reimu-5564.static.mtd /tmp/stock
"""
import hashlib, os, struct, sys, zlib

PARTS = [("u-boot", 0, 0xe0000), ("u-boot-env", 0xe0000, 0x100000), ("kernel", 0x100000, 0x680000),
         ("rofs", 0x680000, 0x3000000), ("rwfs", 0x3000000, 0x4000000)]
ENV = ["bootargs=console=ttyS4,115200n8 root=/dev/ram rw", "bootcmd=bootm 20100000", "bootdelay=2",
       "baudrate=115200", "verify=yes", "spi_dma=yes", "ethaddr=02:52:45:49:4d:01", "eth1addr=02:52:45:49:4d:02"]
COPY = 0x10000


def env_copy(flag):
    data = ("\0".join(ENV) + "\0\0").encode()
    data += b"\0" * (COPY - 5 - len(data))
    return struct.pack("<I", zlib.crc32(data) & 0xffffffff) + bytes([flag]) + data


def main(mtd, out):
    img = open(mtd, "rb").read()
    if len(img) != 64 << 20:
        sys.exit("%s: not a 64 MiB flash image" % mtd)
    os.makedirs(out, exist_ok=True)
    sums = []
    for name, a, e in PARTS:
        part = img[a:e]
        if name == "u-boot-env":
            part = env_copy(1) + env_copy(0)
        open(os.path.join(out, name + ".bin"), "wb").write(part)
        sums.append("%s  %s.bin" % (hashlib.sha256(part).hexdigest(), name))
    open(os.path.join(out, "SHA256SUMS"), "w").write("\n".join(sums) + "\n")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
