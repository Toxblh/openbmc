# meta-ab64: A/B firmware on the one 64 MiB flash of REIMU-5564

Two complete copies of the firmware (kernel 6.18 + rootfs) on the board's
single BMC flash; an update goes into the copy that isn't running, and the
board falls back to the other one by itself if the new one doesn't come up.

## Layout

| partition  | offset    | size     | |
|------------|-----------|----------|-|
| u-boot     | 0x0       | 896 KiB  | the stock U-Boot, never written |
| u-boot-env | 0xe0000   | 128 KiB  | redundant (2 x 64 KiB), the MAC addresses |
| kernel-a   | 0x100000  | 6 MiB    | where the stock kernel is: "bootm 20100000" boots slot A |
| rofs-a     | 0x700000  | 21 MiB   | squashfs, digests in the last 4 KiB |
| kernel-b   | 0x1c00000 | 6 MiB    | |
| rofs-b     | 0x2200000 | 21 MiB   | |
| rwfs       | 0x3700000 | 9 MiB    | jffs2, the settings |

"sys" is the PNS flash (spi1), one partition over the whole chip, found at
a cold boot of the BMC. "bmc" is the whole BMC chip as a partition (the MTD master is off, so the test
device tree can make it read-only too).

## Boot

* U-Boot (`reimu_ab` in the environment, `recipes-mcst/reimu-ab/files/reimu-ab.env`):
  counts boots in `reimu_tries`, boots `reimu_slot`, after 3 boots that didn't
  reach "reimu-ab boot" switches to the other slot; arms WDT1 for 300 s, so a
  hang counts as a failed boot; a FIT that fails its hash check falls through
  to the other slot at once. Passes `reimu.slot=a|b`.
* The initramfs (`reimu-ab-rofs`) copies the slot's rofs into RAM after
  checking it against its digest, falls back to the other slot's rofs; the
  running system doesn't read the flash, so any slot can be rewritten.
* `reimu-ab.service` confirms the boot (a pending update only once healthy,
  otherwise it goes back to the other slot); systemd feeds WDT1.

## reimu-ab

    reimu-ab status | verify [a|b] | set-slot a|b
    reimu-ab install IMAGE.tar [a|b|both]   the other slot; every write read back
    reimu-ab setup-env                      the A/B bootcmd (MACs kept)
    reimu-ab ram                            kexec with nothing of the flash mounted
    reimu-ab rwfs-init [SETTINGS.tar]       (ram) erase + check the rwfs, settings in
    reimu-ab restore-stock DIR              (ram) the stock firmware back from a dump

`reimu-flash` writes the erase blocks that differ, reads the whole range back,
rewrites on a mismatch (3-5 tries); it refuses to write U-Boot.

## Build

Its own build directory (the layer after meta-device-reimu-5564):

    /work/bb-ab.sh LOG obmc-phosphor-image

`do_generate_ab` fails the build when a part doesn't fit its partition with
256 KiB to spare, and deploys `reimu-ab.tar` (the update), `*.ab.mtd` (the
whole flash, for QEMU) and `reimu-ab-kexec.tar` (to start it from RAM).

## Tests (meta-mcst/tests/ab64)

* `test_reimu_flash.py`: reimu-flash with lost writes (LD_PRELOAD flaky-pwrite).
* `test_ab_qemu.py`: QEMU with the U-Boot of the real board: boot, memory,
  regressions, update, damaged kernel, damaged rofs, hang + watchdog, and the
  whole way on the stock firmware dumped from the board: restore-stock, test
  from RAM, back by kexec, first install, the new firmware from the flash.
* `board-check.sh`: read-only checks on the real board.

## On the board

    tools/ab64/reimu-ab-from-stock test BMC reimu-ab-kexec.tar
        the new firmware from RAM, every flash partition read-only, nothing
        written; tools/ab64/reimu-ab-back-to-stock returns by kexec
    tools/ab64/reimu-ab-from-stock install BMC reimu-ab-kexec.tar reimu-ab.tar
        saves the stock settings, writes the environment, slot B, slot A, the
        rwfs, verifies; reboots only with --reboot

Neither resets the BMC (kexec), so the platform keeps running; a reboot of
the BMC resets the platform.
