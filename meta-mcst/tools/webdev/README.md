# REIMU web devkit: the new web UI on a stock BMC, from tmpfs

The devkit runs the new web interface and the BMC programs it needs on a BMC
that still has its old (stock REIMU) firmware. It is meant for trying the web
UI on real hardware before building and flashing a full image. Nothing on the
flash changes: everything sits in `/tmp` (tmpfs) and is attached with bind
mounts and transient systemd units. `reimu-webdev stop` or any reboot brings
back the stock firmware.

## Build (on any x86_64 Linux, no Yocto)

    # the rootfs of the firmware the BMC runs (here the 1.0.2 release):
    tar xf obmc-phosphor-image-reimu-5564.static.mtd.tar image-rofs
    unsquashfs -d /tmp/stock-rofs image-rofs
    BMC_ROOTFS=/tmp/stock-rofs meta-mcst/tools/webdev/build.sh /tmp/webdev
    # -> /tmp/webdev/reimu-webdev-<git describe>.tar.gz (~2.5 MiB)

Needs `git curl python3 python3-venv cmake ninja-build pkg-config dpkg-dev
binutils-arm-linux-gnueabi squashfs-tools`. `BMC_ROOTFS` is optional but
recommended: the link stubs are then made from the BMC's own libraries and
every symbol the binaries import is checked against them
(`checkimports.py`), which is what the dynamic linker on the BMC does.

Contents:

| Path | What |
|------|------|
| `www/` | the web UI (gzip, as installed by the reimu-webui recipe) |
| `bin/bmcweb` | bmcweb of the REIMU revision with REIMU's patches and the patches of this tree (`/reimu/serial` consoles, read-write media, CD-ROM, CSP, timer queue, firmware upload) |
| `bin/nbd-proxy` | jsnbd; `-L` (no netlink) only for an nbd-client built with netlink, REIMU's has none; its configuration and state hook are in `etc/nbd-proxy` |
| `bin/reimu-serial` | consoles with a 20000-line timestamped history |
| `bin/reimu-pnsd`, `libexec/reimu-pns-scan` | ПНС information |
| `bin/reimu-kvm` | KVM straight to the browser: the video engine's JPEG (or the host's H.264) over its own TLS websocket on :8443, keyboard and mouse into the USB HID gadget; obmc-ikvm runs at 60 frames/s next to it for VNC |
| `libexec/reimu-hid-wheel` | gives the KVM mouse a wheel (the host sees the USB keyboard and mouse reconnect once at start and at stop) |
| `reimu-webdev` | the control script |

Cross-compilation without the BMC's sysroot:

* the compiler is [zig](https://ziglang.org) from PyPI (`ziglang`). It targets
  `arm-linux-gnueabi` with **glibc 2.33** (REIMU 1.0.1 and 1.0.2) and
  `-mcpu=arm1176jz_s -mfloat-abi=soft`, which matches the REIMU tune
  `arm1176jzs`. The build checks that no binary needs a newer glibc or uses
  VFP. glibc can't be older than 2.33 here: for older versions zig's `stat()`
  shim calls `__xstat` with the x86 `_STAT_VER`, ARM's glibc answers EINVAL
  and bmcweb dies in `std::filesystem` at start;
* every program is a PIE: zig 0.13's glibc link stub puts all data objects
  (`environ`, `stderr`, `optarg`…) at one address, so a non-PIE executable
  gets a single copy relocation for all of them and they overwrite each other
  (reimu-serial crashed in `getopt()`). The build refuses copy relocations;
* C++ uses zig's libc++, linked statically. The BMC's libstdc++ isn't
  involved, so ABI differences don't matter;
* zlib, tinyxml2 and json-c are built from source and linked statically;
  openssl 1.1.1 only for its headers: bmcweb and reimu-kvm link against the
  BMC's own libssl/libcrypto (stubs as below), whose ARM assembler makes TLS
  much cheaper than a portable (no-asm) build;
* zig's compiler-rt puts weak byte-by-byte memcpy/memmove/memset into every
  executable, and they win over glibc's tuned ARM ones (bmcweb spent a fifth
  of its time there). `fastmem.c` forwards them to glibc;
* libsystemd, libudev and libpam are linked against stubs made by
  `mkstub.py` from the BMC's libraries (`BMC_ROOTFS`) or Ubuntu 20.04
  packages, with the same sonames and symbol versions (`LIBSYSTEMD_209`…,
  `LIBPAM_1.0`). At run time the BMC's own libraries are used. The stubs
  export only the library's own symbols: libgcc helpers (`__aeabi_*`) that
  Ubuntu's armhf libraries re-export, and the compiler-rt zig links into
  every shared object, are left out, so the programs take them from the
  static compiler-rt (otherwise: `undefined symbol: __aeabi_uldivmod` on the
  BMC).

`patches/` holds what only this build needs. For example, clang's libc++
requires a `static constexpr` `min()`/`max()` in bmcweb's random generator,
which upstream bmcweb later changed the same way.

## Run on the BMC

From a workstation (macOS or Linux, OpenSSH 8.4+), with the devkit tarball
saved as `reimu-webdev.tar.gz` next to the script:

    reimu-webdev-run.sh BMC [LOGIN [PASSWORD]]           # root / 0penBmc by default
    reimu-webdev-run.sh BMC [LOGIN [PASSWORD]] --status
    reimu-webdev-run.sh BMC [LOGIN [PASSWORD]] --stop

It uploads the tarball to `/tmp` over one SSH connection, stops a devkit that
is already running (an update) and starts the new one. By hand:

    scp /tmp/webdev/reimu-webdev-*.tar.gz root@BMC:/tmp/
    ssh root@BMC
    tar xzf /tmp/reimu-webdev-*.tar.gz -C /tmp
    /tmp/reimu-webdev/reimu-webdev start          # or: start --web-only
    /tmp/reimu-webdev/reimu-webdev status
    /tmp/reimu-webdev/reimu-webdev stop

`start`:

1. Bind mounts `www` over `/usr/share/www`, `bmcweb` over `/usr/bin/bmcweb`
   and `nbd-proxy` over `/usr/sbin/nbd-proxy`.
2. Stops `tmux-logger` and the active `obmc-console*` units, which own the
   platform UART. You can set your own list in `REIMU_WEBDEV_STOP`. Then it
   starts `reimu-serial` as a transient unit, with the BMC's
   `/etc/reimu-serial.conf` if there is one, else the devkit's (`ttyS0`, as
   tmux-logger). SSH users keep a `tmux`/`minicom` session `SoL` on its pty,
   as with the new firmware.
3. Adds the D-Bus policy of `reimu-pnsd` (a tmpfs copy of
   `/etc/dbus-1/system.d`, bind mounted) and starts it. Its FRU copy and the
   ПНС flash scan go to `/run/reimu-webdev` (`REIMU_FRU_CACHE`,
   `REIMU_PNS_SCAN_OUT`), not to `/var/lib` on the flash.
4. Restarts bmcweb and waits for `https://127.0.0.1/`. If it doesn't
   answer, all changes are rolled back.

`--web-only` swaps only the UI files and keeps the stock bmcweb and daemons.
Use it to see what the UI does on an unmodified firmware: the consoles fall
back to `/console0`, and there is no media write or CD-ROM.

Memory: the unpacked devkit takes ~10 MiB in `/tmp`. The devkit's bmcweb uses
about as much RAM as the stock one.

What the devkit can't change: the kernel and U-Boot. So there is no reliable
A/B upgrade, because it needs the U-Boot, device tree and kernel config
changes of `meta-reliable-upgrade`. The firmware card then shows "no backup".
The USB gadget and the KVM video stay as in the stock image.

## Test without a BMC

In QEMU, with the release image the board runs (`meta-mcst/tests/qemu`):

    cp obmc-phosphor-image-reimu-5564.static.mtd /tmp/stock.mtd
    meta-mcst/tests/qemu/run-bmc.sh /tmp/stock.mtd stock 2222 2443
    meta-mcst/tests/qemu/bmcssh 2222 'cat > /tmp/wd.tgz' < reimu-webdev-*.tar.gz
    meta-mcst/tests/qemu/bmcssh 2222 'tar xzf /tmp/wd.tgz -C /tmp && /tmp/reimu-webdev/reimu-webdev start'

Checked this way on REIMU 1.0.2 (ARM, the real binaries): `start`, `status`,
`stop` and the stock state after a reboot; `tests/e2e/console.qemu.mjs`
(18 checks), `nbd-rw.qemu.mjs` (read-write media through `/dev/nbd0`, 5),
`pages.qemu.mjs` (every page, 9), the KVM handshake with the stock
obmc-ikvm. Not in QEMU: KVM video, the USB host side of media. On a stock
BMC `--web-only` has no `/reimu/serial` and no media writes, as described
above; the ПНС page shows what `reimu-pnsd` finds (nothing in QEMU).

On the x86 test VM:

`TARGET=x86_64 STUB_LIBS=<VM's lib dir> build.sh` builds the same package for
the x86 test VM (`meta-mcst/tests/testvm`). There, `reimu-webdev start` is
tested with `REIMU_WEBDEV_STOP="reimu-serial.service reimu-pnsd.service"`,
because the VM runs the new daemons in place of tmux-logger
(`tests/e2e/webdev.spec.mjs`: 20 checks, among them the `--web-only` console
fallback and the rollback of a broken devkit). With the devkit started, the
console (21) and read-write media (30) suites also pass on its bmcweb,
nbd-proxy and reimu-serial.
