#!/usr/bin/env python3
"""uboot-noreset.py IN OUT: REIMU-5564's own U-Boot (the stock 1.0.2 one, as
on every board: "U-Boot 2016.07-00039-g1ded9fa3a2-dirty (Sep 17 2021)") with
the two GPIO writes of its board_init that hold the platform at every BMC
boot taken out of the machine code, nothing else changed:

  0x42a8  mov r2,#1 -> mov r2,#0   SPI_CONNECT (O0) = 0: the PNS flash stays
                                   the host's (was 1: switched to the BMC)
  0x42c4  bl ast_gpio_set -> nop   RST_BTN# (P3) left alone (was driven 0:
                                   the platform held in reset until
                                   allow_host_boot let it go)

Why: the BMC used to read the PNS flash at its boot, so U-Boot took the flash
and held the host in reset; reimu-fru reads it only with the platform off now
(else the copy kept), so a BMC reboot no longer has to reset the host.

IN: the u-boot partition (917504 bytes, /dev/mtd1) or U-Boot alone; its
sha256 must be the stock one, and the two words must be what they were.
Both changes are in one erase block (4 KiB at 0x4000): OUT is the whole
input with them; --block prints that block's offset for the writer.

    uboot-noreset.py u-boot.bin u-boot-noreset.bin
"""
import hashlib, struct, sys

STOCK = {
    # the partition as on the boards (and in the public 1.0.2 image)
    917504: "1ef70f586108d7e486d11993938001c2dd1529652c2108f3a99edbf0534e2eb0",
}
PATCH = [(0x42a8, 0xe3a02001, 0xe3a02000, "SPI_CONNECT (O0) = 0"),
         (0x42c4, 0xebffffc3, 0xe1a00000, "no write to RST_BTN# (P3)")]
BLOCK = 0x4000


def patch(data):
    data = bytearray(data)
    want = STOCK.get(len(data))
    got = hashlib.sha256(data).hexdigest()
    if want and got != want:
        # already patched? the same file with our two words
        if all(struct.unpack_from("<I", data, o)[0] == new for o, _, new, _ in PATCH):
            raise SystemExit("already without the reset: nothing to do")
        raise SystemExit("not the stock U-Boot of REIMU-5564 (sha256 %s)" % got)
    for off, old, new, what in PATCH:
        cur = struct.unpack_from("<I", data, off)[0]
        if cur != old:
            raise SystemExit("0x%x: 0x%08x, expected 0x%08x (%s): not this U-Boot" % (off, cur, old, what))
        struct.pack_into("<I", data, off, new)
    for off, _, _, _ in PATCH:
        assert off // 4096 * 4096 == BLOCK
    return bytes(data)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    out = patch(open(sys.argv[1], "rb").read())
    open(sys.argv[2], "wb").write(out)
    print("%s: %s; the changed erase block: 0x%x (4 KiB), sha256 %s" % (
        sys.argv[2], ", ".join(w for _, _, _, w in PATCH), BLOCK, hashlib.sha256(out).hexdigest()))
