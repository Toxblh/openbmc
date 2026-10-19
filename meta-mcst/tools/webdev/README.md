# REIMU web devkit: the new web UI on a stock BMC, from tmpfs

The devkit runs the new web interface and the BMC programs it needs on a BMC
that still has its old (stock REIMU) firmware. It is meant for trying the web
UI on real hardware before building and flashing a full image. Nothing on the
flash changes: everything sits in `/tmp` (tmpfs) and is attached with bind
mounts and transient systemd units. `reimu-webdev stop` or any reboot brings
back the stock firmware.

## Build (on any x86_64 Linux, no Yocto)

    meta-mcst/tools/webdev/build.sh /tmp/webdev
    # -> /tmp/webdev/reimu-webdev-<git describe>.tar.gz (~4 MiB)

Contents:

| Path | What |
|------|------|
| `www/` | the web UI (gzip, as installed by the reimu-webui recipe) |
| `bin/bmcweb` | bmcweb of the REIMU revision with REIMU's patches and the patches of this tree (`/reimu/serial` consoles, read-write media, CD-ROM, CSP, timer queue, firmware upload) |
| `bin/nbd-proxy` | jsnbd with the netlink fix; its configuration and state hook are in `etc/nbd-proxy` |
| `bin/reimu-serial` | consoles with a 20000-line timestamped history |
| `bin/reimu-pnsd`, `libexec/reimu-pns-scan` | ПНС information |
| `reimu-webdev` | the control script |

Cross-compilation without the BMC's sysroot:

* the compiler is [zig](https://ziglang.org) from PyPI (`ziglang`). It targets
  `arm-linux-gnueabi` with **glibc 2.32** (the REIMU Yocto 3.2 version) and
  `-mcpu=arm1176jz_s -mfloat-abi=soft`, which matches the REIMU tune
  `arm1176jzs`. The build checks that no binary needs a newer glibc or uses
  VFP;
* C++ uses zig's libc++, linked statically. The BMC's libstdc++ isn't
  involved, so ABI differences don't matter;
* openssl 1.1.1, zlib, tinyxml2 and json-c are built from source and linked
  statically;
* libsystemd, libudev and libpam are linked against stubs made by
  `mkstub.py` from Ubuntu 20.04 packages, with the same sonames and symbol
  versions (`LIBSYSTEMD_209`…, `LIBPAM_1.0`). At run time the BMC's own
  libraries are used: their sonames are stable, and the symbols needed exist
  in systemd 246.

`patches/` holds what only this build needs. For example, clang's libc++
requires a `static constexpr` `min()`/`max()` in bmcweb's random generator,
which upstream bmcweb later changed the same way.

## Run on the BMC

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
   `/etc/dbus-1/system.d`, bind mounted) and starts it.
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

`TARGET=x86_64 STUB_LIBS=<VM's lib dir> build.sh` builds the same package for
the x86 test VM (`meta-mcst/tests/testvm`). There, `reimu-webdev start` is
tested with `REIMU_WEBDEV_STOP="reimu-serial.service reimu-pnsd.service"`,
because the VM runs the new daemons in place of tmux-logger
(`tests/e2e/webdev.spec.mjs`: 20 checks, among them the `--web-only` console
fallback and the rollback of a broken devkit). With the devkit started, the
console (21) and read-write media (30) suites also pass on its bmcweb,
nbd-proxy and reimu-serial.
