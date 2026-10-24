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

Which slot `install` writes (`/run/initramfs/reimu-ab`: `kernel=a|b
rofs=a|b|ram state=ok|fallback|ram`): no slot given, the one not running
(`state=ok`, `ram`), or, on a fallback (rofs-K damaged, kernel K running on
the other slot's rofs), slot K itself: the other slot's rofs is the only
intact one. It never writes a slot whose kernel or rofs is the only intact
one (each part checked against the digests in its slot: U-Boot boots any
intact kernel, the initramfs any intact rofs, so the board stays bootable
while the other slot's parts are intact); `both` writes the damaged slot
first (B when neither or both are intact). While a slot is written
`reimu_slot` points at the other one; it is switched to the written slot only
once its digests match. `reimu-ab boot` (reimu-ab.service) leaves the
environment alone when the system was started by kexec (`reimu.ram`,
`reimu.test`): `reimu-ab ram` doesn't confirm or cancel a pending install.

### restore-stock: the order of the writes

The stock layout over the A/B one: the stock kernel (0x100000-0x680000) is
in kernel-a; the stock rofs (0x680000-0x3000000) covers the end of kernel-a,
rofs-a, kernel-b and the start of rofs-b; the stock rwfs (0x3000000-0x4000000)
the end of rofs-b (its digests) and the A/B rwfs. The A/B bootcmd falls
through from a slot that is gone to kernel-a, i.e. to the stock kernel once
that is written, but it arms WDT1, which nothing of the stock firmware
feeds: the stock environment goes in as soon as the stock kernel and rofs
are complete, one 64 KiB copy at a time (one copy is always valid), the
stock rwfs (over the A/B rwfs, which a running A/B slot would mount) last,
after erasing all of its place, 0x3000000-0x4000000 (a stock jffs2 cut
short mixed with the A/B one left the stock firmware without ssh; the end
of rofs-b in front of an erased A/B rwfs, the stock initramfs can't mount
at all: a panic).
What a power cut leaves (`test_ab_qemu.py restorecut`, each state booted):

| slot B intact (the usual case) | a power cut here boots |
|-|-|
| `reimu_slot=b`, the stock kernel, the stock rofs to 0x1c00000 | A/B slot B |
| the stock rofs from 0x1c00000 (20 MiB: kernel-b, rofs-b) | **nothing** (the window: kernel-b is gone, U-Boot falls through to the stock kernel, which panics on the half-written rofs; a reset loop) |
| environment copy 1 | the stock firmware under the A/B environment: WDT1 resets it every 300 s |
| environment copy 2 | the stock firmware (U-Boot takes the valid stock copy) |
| 0x3000000-0x4000000 erased, the stock rwfs | the stock firmware (empty or cut-short settings) |

With only slot A intact: the stock rofs from 0x1c00000 (slot A boots), then
the kernel and the rofs to 0x1c00000 (the window, 27 MiB), the environment,
the rwfs. With neither intact there is no fallback at all. The window can't
be closed: the stock rofs alone covers kernel-b, rofs-a and the end of
kernel-a, so no A/B slot survives its last part, and the stock firmware
isn't complete before it. A power cut in the window leaves U-Boot with no
bootable kernel + rofs (on the board every reset also resets the platform);
the flash then has to be written from outside (or from U-Boot's console).

## BMC updates from the web (reimu-abd)

phosphor-bmc-code-mgmt is left out (its static-layout flow stages images
for a shutdown flasher that has no partitions to write here). `reimu-abd`
takes its place on D-Bus (xyz.openbmc_project.Software.BMC.Updater): an
upload through Redfish UpdateService (the web UI's firmware card) that is a
`reimu-ab.tar` becomes a Software object; bmcweb activates it and
`reimu-ab install` writes the slot that isn't running (every write read back)
and makes it boot next, pending until healthy; the task completes, the BMC
is not rebooted. Anything else is refused at once (HTTP 400, ImageFailure).
The running version is the functional one (Managers/bmc FirmwareVersion).
State for the web UI: `/xyz/openbmc_project/reimu/ab` (Json).

## The PNS flash (reimu-pns-flash)

The host boot firmware's 64 MiB flash on spi1 ("sys"), switched to the BMC
with GPIO_SPI_CONNECT (O0) only while the platform is off:

    reimu-pns-flash version [FILE] | version --read
    reimu-pns-flash check FILE [--any-board]
    reimu-pns-flash read [FILE]           the whole chip, read twice
    reimu-pns-flash write FILE            backup, only differing erase blocks,
                                          each read back (reimu-flash), then
                                          the whole chip compared
    update-host-flash URL                 the old command, now the same write

A lock (/run/reimu-pns/lock) is held for the whole job; server_ctl (every
power-on/reset path: host-poweron.service, Redfish, IPMI, allow_host_boot)
refuses while it is held, and switches the flash back to the host before it
presses the power button. The flash is switched back on every exit (trap; a
guard process for kill -9; ExecStopPost of the web job). Image layout,
checks and what is never written: the comment at the top of the script.
The web UI (Hardware page, reimu-pnsd `/xyz/openbmc_project/reimu/pns_flash`)
uploads, checks, writes, downloads and writes back backups.

## Build

Its own build directory (the layer after meta-device-reimu-5564):

    /work/bb-ab.sh LOG obmc-phosphor-image

`do_generate_ab` fails the build when a part doesn't fit its partition with
256 KiB to spare, and deploys `reimu-ab.tar` (the update), `*.ab.mtd` (the
whole flash, for QEMU) and `reimu-ab-kexec.tar` (to start it from RAM).

## Tests (meta-mcst/tests/ab64)

* `test_reimu_flash.py`: reimu-flash with lost writes (LD_PRELOAD flaky-pwrite).
* `test_pns_qemu.py`: the PNS flash on spi1 of QEMU (a copy of the board's
  dump): versions, check, read, write, faults (pns-fault.c), power-on
  blocking, the web endpoints and page, and the web BMC update (reimu-abd).
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
