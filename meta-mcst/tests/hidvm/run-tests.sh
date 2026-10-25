#!/bin/sh
# run-tests.sh WORKDIR [SUITE...]: the KVM HID and virtual media tests in the
# HID test VM (build-hidvm.sh; started here if it isn't running). Suites (all
# by default):
#   unit   reimu-kvm's report builder and queue (test_reimu_kvm_hid.c)
#   kvm    reimu-kvm + create_usbhid.sh against the VM's USB host: the keyboard
#          pinned to the stock firmware's (the PNS boot menu), 200 presses in
#          one burst, release on connect/close/SIGTERM, UDC rebind, LEDs,
#          interface fields and SET_PROTOCOL, GET_REPORT time, the watcher
#          (lost configuration; after a kexec: "not attached", no viewer)
#   ikvm   obmc-ikvm's Input: no blocking, release, no stray Shift, rebind
#   msd    reimu_state_hook: INQUIRY, removable, stall, nofua, a 5 GiB CD-ROM
#   watch  reimu-hid-watch's policy (fake /sys)
# The sources come from the build container's repository (REPO, default
# /work/openbmc in the container reimu): the working tree, or REV=<git rev>
# (REV=HEAD: the last commit, i.e. without the uncommitted changes: the code
# the tests were written against shows red).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(cd "$1" && pwd); shift
SUITES=${*:-unit kvm ikvm msd watch}
REPO=${REPO:-/work/openbmc}
REV=${REV:-}
M=meta-mcst/meta-common
S=$W/share/src
rm -rf "$S" && mkdir -p "$S"
get() { # repo path -> file
    if [ -n "$REV" ]; then docker exec reimu git -c safe.directory=* -C $REPO show "$REV:$1" > "$2" 2>/dev/null || rm -f "$2"
    else docker exec reimu cat "$REPO/$1" > "$2" 2>/dev/null || rm -f "$2"; fi
}
get $M/recipes-mcst/reimu-kvm/files/reimu-kvm.c "$S/reimu-kvm.c"
get $M/recipes-mcst/reimu-kvm/files/reimu-hid-watch "$S/reimu-hid-watch"
get $M/recipes-phosphor/obmc-ikvm/files/create_usbhid.sh "$S/create_usbhid.sh"
get $M/recipes-connectivity/jsnbd/files/reimu_state_hook "$S/reimu_state_hook"
# the image's create_usbhid.sh was obmc-ikvm's own (with the mouse-wheel patch) before REIMU's
[ -s "$S/create_usbhid.sh" ] || NEED_UPSTREAM_SCRIPT=1
[ -s "$S/reimu-hid-watch" ] || printf '#!/bin/sh\nexit 0\n' > "$S/reimu-hid-watch"
# obmc-ikvm at the recipe's SRCREV with the REIMU patches
rev=$(docker exec reimu sh -c "grep -h '^SRCREV' $REPO/meta-phosphor/recipes-graphics/obmc-ikvm/obmc-ikvm_git.bb | cut -d'\"' -f2")
docker exec -u builder reimu sh -c "rm -rf /tmp/hidvm-ikvm && git clone -q /work/downloads/git2/github.com.openbmc.obmc-ikvm /tmp/hidvm-ikvm && git -C /tmp/hidvm-ikvm checkout -q $rev"
mkdir -p "$S/patches"
for p in $(docker exec reimu sh -c "cd $REPO/$M/recipes-phosphor/obmc-ikvm/files && ls *.patch"); do
    get $M/recipes-phosphor/obmc-ikvm/files/$p "$S/patches/$p"
done
COPYFILE_DISABLE=1 tar -C "$S" -cf - patches | docker exec -i -u builder reimu sh -c 'rm -rf /tmp/hidvm-ikvm-p && mkdir /tmp/hidvm-ikvm-p && tar -C /tmp/hidvm-ikvm-p -xf - &&
    cd /tmp/hidvm-ikvm && for p in /tmp/hidvm-ikvm-p/patches/*.patch; do [ -s "$p" ] && patch -s -p1 < "$p"; done'
mkdir -p "$S/ikvm"
docker exec reimu tar -C /tmp/hidvm-ikvm --exclude=.git -cf - . | tar -C "$S/ikvm" -xf -
[ "${NEED_UPSTREAM_SCRIPT:-}" = 1 ] && cp "$S/ikvm/create_usbhid.sh" "$S/create_usbhid.sh"
cp -R "$HERE/vm/." "$W/share/"
"$HERE/vssh" "$W" true 2>/dev/null || "$HERE/run-hidvm.sh" "$W" || exit 1
V() { "$HERE/vssh" "$W" "$@"; }
V "cd /mnt/src && gcc -O2 -Wall -o reimu-kvm reimu-kvm.c -lssl -lcrypto -lsystemd" || exit 1
fails=0
for s in $SUITES; do
    echo "##### $s"
    case $s in
        unit)  V "cd /tmp && gcc -O1 -w -DREIMU_KVM_C='\"/mnt/src/reimu-kvm.c\"' -o t-kvm /mnt/test_reimu_kvm_hid.c -lssl -lcrypto -lsystemd && ./t-kvm" ;;
        kvm)   V "cd /mnt && python3 kvmscen.py /mnt/src kbdpin burst release rebind leds iface getreport watch watch-kexec crash" ;;
        ikvm)  b=; grep -q setClients "$S/ikvm/ikvm_input.hpp" || b=base
               V "cd /mnt && python3 ikvmscen.py /mnt/src/ikvm /mnt/src $b" ;;
        msd)   V "cd /mnt && python3 msdscen.py /mnt/src/reimu_state_hook" ;;
        watch) V "cd /mnt && python3 watch_test.py /mnt/src/reimu-hid-watch" ;;
    esac
    [ $? = 0 ] || fails=$((fails + 1))
done
echo "== $fails suite(s) failed"
[ $fails = 0 ]
