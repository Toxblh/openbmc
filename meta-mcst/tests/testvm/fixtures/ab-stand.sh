#!/bin/bash
# Reliable upgrade test stand inside the test VM (no ASPEED hardware):
#  * two 64 MiB "BMC flash chips": block2mtd over loop devices, /dev/mtd/bmc
#    (running chip) and /dev/mtd/bmc2, rofs of the other chip as a loop
#    device at the rofs offset (/dev/mtd/rofs-sec);
#  * SCU/WDT registers in a file (REIMU_DEVMEM for reimu-wdt2);
#  * U-Boot environment of each chip in a file (fw_printenv/fw_setenv stubs).
# The hardware chip swap after a WDT2 fallback is emulated by swapping the
# links.
#
#   ab-stand.sh setup <chipA.img> <chipB.img>
#   ab-stand.sh reset <chipA.img> <chipB.img>
#                        fresh chip contents and state, then boot primary
#   ab-stand.sh boot primary|backup [healthy|broken]
#                        emulate a BMC start: registers as after reset with
#                        U-Boot (WDT2 armed for 300 s), then reimu-bootguard boot
#   ab-stand.sh flash-staged
#                        what the shutdown script does with /run/initramfs/image-*
#   ab-stand.sh wdt      print the emulated WDT2 registers
set -e
D=/var/lib/ab-stand
REGS=$D/devmem

regs() # secondary(0|1)
{
    python3 - "$REGS" "$1" <<'PY'
import struct, sys
b = bytearray(0x2000)
struct.pack_into("<I", b, 0x70, 1 << 17)                 # SCU70: 2nd boot strap
struct.pack_into("<I", b, 0x1024, 300000000)             # WDT2 reload: 300 s
struct.pack_into("<I", b, 0x1020, 300000000)             # counter
struct.pack_into("<I", b, 0x102c, 0x13)                  # enabled
struct.pack_into("<I", b, 0x1030, 2 if sys.argv[2] == "1" else 0)
open(sys.argv[1], "wb").write(b)
PY
}

links() # running chip loop, other chip loop, other rofs loop
{
    ln -sfn "../$(basename "$(mtd_of "$1")")" /dev/mtd/bmc
    ln -sfn "../$(basename "$(mtd_of "$2")")" /dev/mtd/bmc2
    ln -sfn "$3" /dev/mtd/rofs-sec
    ln -sfn "$D/env.$4" $D/env
}

mtd_of() # loop device -> /dev/mtdN
{
    local n
    n=$(grep -l "block2mtd: $1\"\?$" /sys/class/mtd/mtd*/name 2>/dev/null | head -1)
    [ -n "$n" ] || n=$(grep -l "$1" /sys/class/mtd/mtd*/name | head -1)
    echo "/dev/$(basename "$(dirname "$n")")"
}

case "$1" in
setup)
    mkdir -p $D /dev/mtd /run/initramfs
    cp "$2" $D/chipA.img; cp "$3" $D/chipB.img
    for c in A B; do
        l=$(losetup -f --show $D/chip$c.img); echo "$l" > $D/loop$c
        echo "$l,65536" > /sys/module/block2mtd/parameters/block2mtd
        losetup -f --show -r -o $((0x680000)) --sizelimit $((0x2980000)) $D/chip$c.img > $D/rofs$c
        : > $D/env.$c
    done
    cat > /usr/local/sbin/fw_printenv <<'SH'
#!/bin/sh
[ "$1" = -n ] && shift
v=$(sed -n "s/^$1=//p" /var/lib/ab-stand/env); [ -n "$v" ] && echo "$v"
SH
    cat > /usr/local/sbin/fw_setenv <<'SH'
#!/bin/sh
f=$(readlink -f /var/lib/ab-stand/env)
{ grep -v "^$1=" "$f"; echo "$1=$2"; } > "$f.t" 2>/dev/null; mv "$f.t" "$f"
SH
    chmod +x /usr/local/sbin/fw_printenv /usr/local/sbin/fw_setenv
    cat > /etc/reimu-bootguard.conf <<'SH'
# test stand (ab-stand.sh): emulated registers and environment
export REIMU_DEVMEM=/var/lib/ab-stand/devmem
FW_PRINTENV=/usr/local/sbin/fw_printenv
FW_SETENV=/usr/local/sbin/fw_setenv
OS_RELEASE=/run/ab-stand-os-release
HEALTH_DEADLINE=0
HEALTH_UNITS="bmcweb.socket"
HEALTH_URL=""
HEALTH_CMD="test ! -e /run/ab-stand-broken"
AUTO_SYNC=yes
SH
    "$0" boot primary
    ;;
reset)
    for c in A B; do
        m=$(mtd_of "$(cat $D/loop$c)")
        dd if="$([ $c = A ] && echo "$2" || echo "$3")" of=/dev/mtdblock${m#/dev/mtd} bs=1M conv=fsync 2>/dev/null
        : > $D/env.$c
    done
    rm -rf /var/lib/reimu-bootguard
    "$0" boot primary
    ;;
boot)
    side=$2
    # a new boot: no staged images, no state in /run, units start afresh
    systemctl stop reimu-bootguard-update.service 2>/dev/null || true
    rm -rf /run/reimu-bootguard /run/initramfs/image-*
    [ "$3" = broken ] && touch /run/ab-stand-broken || rm -f /run/ab-stand-broken
    if [ "$side" = backup ]; then
        regs 1; links "$(cat $D/loopB)" "$(cat $D/loopA)" "$(cat $D/rofsA)" B; run=B
    else
        regs 0; links "$(cat $D/loopA)" "$(cat $D/loopB)" "$(cat $D/rofsB)" A; run=A
    fi
    sync; echo 3 > /proc/sys/vm/drop_caches
    # /etc/os-release of the "running" image: from that chip's rofs
    m=$(mktemp -d); mount -t squashfs -o ro "$(cat $D/rofs$run)" "$m"
    cp "$m/etc/os-release" /run/ab-stand-os-release; umount "$m"; rmdir "$m"
    systemctl restart reimu-firmwared 2>/dev/null || true
    /usr/sbin/reimu-bootguard boot || true
    ;;
flash-staged)
    # obmc-flash at shutdown: staged partitions onto the running chip
    dev=$(readlink -f /dev/mtd/bmc); dev=/dev/mtdblock${dev#/dev/mtd}
    for p in u-boot:0 kernel:$((0x100000)) rofs:$((0x680000)); do
        f=/run/initramfs/image-${p%%:*}
        [ -f "$f" ] || continue
        dd if="$f" of="$dev" bs=64k seek=$((${p#*:} / 65536)) conv=notrunc,fsync 2>/dev/null
        echo "flashed ${p%%:*}"
    done
    rm -f /run/initramfs/image-*
    ;;
wdt)
    REIMU_DEVMEM=$REGS /usr/sbin/reimu-wdt2 status
    od -An -tx4 -j $((0x1034)) -N4 $REGS | sed 's/^ */clear=/'
    ;;
esac
