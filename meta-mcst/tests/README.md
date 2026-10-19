# Tests of the REIMU web UI components

Unit tests (run on any Linux host, no BMC needed):

    gcc -O2 -fsanitize=address,undefined -o /tmp/reimu-serial ../meta-common/recipes-mcst/reimu-serial/files/reimu-serial.c
    sudo python3 test_reimu_serial.py /tmp/reimu-serial /tmp/reimu-serial-test     # uses /run/reimu-serial

    gcc -DPARSER_ONLY -O2 -fsanitize=address,undefined -o /tmp/reimu-pnsd ../meta-common/recipes-mcst/reimu-pns/files/reimu-pnsd.c
    python3 test_reimu_pns.py /tmp/reimu-pnsd /tmp/reimu-pns-test

    F=../meta-reliable-upgrade/recipes-mcst/reimu-bootguard/files
    gcc -O2 -fsanitize=address,undefined -o /tmp/reimu-wdt2 $F/reimu-wdt2.c
    gcc -O2 -fsanitize=address,undefined -o /tmp/reimu-flashcopy $F/reimu-flashcopy.c
    python3 test_reimu_bootguard.py $F/reimu-bootguard /tmp/reimu-wdt2 /tmp/reimu-flashcopy /tmp/bootguard-test

End-to-end tests run a real browser (Playwright/Chromium) against a test
"BMC": an x86 Ubuntu 20.04 VM with the *real* bmcweb of the REIMU revision
(REIMU patches + web UI patch), reimu-serial, reimu-pnsd, jsnbd/nbd-proxy,
nbd-client and a USB mass storage gadget on `dummy_hcd`, so the VM's own
USB host side plays the platform. Parts of the hardware that QEMU can't
provide are replaced by fixtures (`testvm/fixtures`):

* platform COM ports: QEMU serial ports exported as `testvm/com1.sock`, `com2.sock`;
* obmc-ikvm: x11vnc (same RFB with VNC password and Tight/JPEG);
* phosphor-hwmon / inventory / state / user-manager / ObjectMapper:
  `fake-phosphor.py` publishes D-Bus objects of the same shape, so the real
  bmcweb Redfish code runs on them;
* PNS flash: `block2mtd` over a synthetic 16 MiB image, SPI switch GPIO: a stub
  that logs its state;
* reliable upgrade (`testvm/fixtures/ab-stand.sh`): two 64 MiB BMC flash chips
  (`block2mtd`) with images from `make-ab-images.py`, SCU/WDT registers and the
  U-Boot environment in files; fake-phosphor plays the static layout
  phosphor-image-updater behind bmcweb's UpdateService.

Build and start the stand (root, docker, debootstrap, qemu-system-x86):

    testvm/build-testvm.sh          # -> testvm/work
    testvm/run-vm.sh &              # https://127.0.0.1:8443, ssh -p 2222

Run the tests:

    cd e2e && npm install playwright@1.56 && export CHROME=/path/to/chromium
    node console.spec.mjs       # consoles: 20000-line history, timestamps, input, xterm
    node media.spec.mjs         # RW virtual media: OPFS, files, snapshot, CD-ROM
    node media-stress.spec.mjs  # parallel I/O, fsck, browser crash recovery
    node vm-buffer.spec.mjs     # bmcweb NBD buffer overflow regression
    node kvm.spec.mjs           # web KVM through bmcweb /kvm/0
    node platform.spec.mjs      # power, sensors, inventory, PNS flash scan
    node firmware.spec.mjs      # A/B update: upload, health check, fallback, restore
    node webdev.spec.mjs        # tmpfs web devkit on a "stock" BMC (DEVKIT, STOCK_BMCWEB)
    node jpeg-bench.mjs         # JPEG decode path benchmark

Environment: `BASE`, `VMSSH`, `COM1`, `CHROME`, `OUT` (screenshots), `WORK`;
see `e2e/common.mjs`.

The reliable upgrade stand needs, once per VM boot (reimu-bootguard,
reimu-wdt2, reimu-flashcopy, reimu-firmwared and their units installed):

    testvm/fixtures/make-ab-images.py /tmp/reimu-e2e/ab        # needs mksquashfs
    scp chipA.img chipB.img and ab-stand.sh into the VM (/root/ab, /usr/local/sbin)
    ab-stand.sh setup /root/ab/chipA.img /root/ab/chipB.img     # in the VM

The web devkit test turns the VM into a stock REIMU BMC (REIMU-revision
bmcweb without this tree's patches, old UI page) and runs the devkit built
for x86 on it:

    STUB_LIBS=<dir with the VM's libsystemd.so.0, libudev.so.1, libpam.so.0> \
        TARGET=x86_64 ../tools/webdev/build.sh /tmp/webdev-x86
    DEVKIT=/tmp/webdev-x86/reimu-webdev-*-x86_64.tar.gz STOCK_BMCWEB=<stock bmcweb> node e2e/webdev.spec.mjs
