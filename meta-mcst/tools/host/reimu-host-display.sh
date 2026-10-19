#!/bin/sh
# Elbrus host (Linux): lets the BMC's VGA (ASPEED AST2500, what REIMU KVM
# shows) run up to 1920x1080 without a monitor attached.
#
# Without a monitor the VGA has no EDID and Linux offers only 1024x768 and
# below. This installs a "virtual monitor" EDID for the ASPEED output only
# (connector VGA-1, other cards are not touched).
#
#   sudo ./reimu-host-display.sh              apply now + keep after reboot (/boot/boot.conf)
#   sudo ./reimu-host-display.sh --console    also send boot messages and a login to the
#                                             BMC console (console=ttyS1, serial-getty@ttyS1)
#   sudo ./reimu-host-display.sh --initrd     also put the EDID into the initrd (ALT make-initrd),
#                                             needed only if the ast driver loads from the initrd
#   sudo ./reimu-host-display.sh --undo       revert everything this script changed
#        ./reimu-host-display.sh --mirror     (as the desktop user) show the desktop on the
#                                             BMC VGA too, 1920x1080, mirror of the main screen
#
# The EDID advertises 1920x1200 too, but the Linux ast driver (6.1) refuses it on
# this board (VGA scratch CRD1 bit 0), so 1920x1080 is the largest mode in use.
set -eu

EDID_NAME=reimu-1920x1200.bin
FW=/lib/firmware/edid/$EDID_NAME
PARAM="drm.edid_firmware=VGA-1:edid/$EDID_NAME"
CONN=/sys/class/drm/card0-VGA-1
EDID_B64='AP///////wBJtSAZAQAAAAEkAQMONCB4Cu6Ro1RMmSYPUFQhCADRwLMAqUCBgJUAgQCBwHFAKDyAoHCwI0AwIDYACEUhAAAaAAAA/QAySx5QEQAKICAgICAgAAAA/ABSRUlNVSBLVk0KICAgAAAA/wBCTUMtVkdBCiAgICAgANo='
EDID_MD5=f2b7fecf2caa9adbf3b2952c0fc26818

console=0 initrd=0 undo=0 mirror=0
for a in "$@"; do
    case "$a" in
        --console) console=1 ;;
        --initrd) initrd=1 ;;
        --undo) undo=1 ;;
        --mirror) mirror=1 ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option $a (see --help)" >&2; exit 2 ;;
    esac
done

# the ASPEED output: card0-VGA-1 on 1Э8СВ-uATX, but find it by driver
for c in /sys/class/drm/card*-VGA-*; do
    [ -e "$c" ] || continue
    drv=$(basename "$(readlink -f "$c/../device/driver")" 2>/dev/null || true)
    [ "$drv" = ast ] && { CONN=$c; break; }
done
OUT=${CONN##*/card[0-9]-}   # VGA-1

if [ $mirror = 1 ]; then
    [ -n "${DISPLAY:-}" ] || { echo "--mirror needs the desktop session (DISPLAY is empty)" >&2; exit 1; }
    main=$(xrandr --query | awk '/ connected primary/{print $1; exit}')
    [ -n "$main" ] || main=$(xrandr --query | awk -v o="$OUT" '/ connected/ && $1 != o {print $1; exit}')
    xrandr --output "$OUT" --mode 1920x1080 --same-as "$main"
    echo "$OUT: 1920x1080, mirror of $main (until logout; to keep it, save the layout in the display settings)"
    exit 0
fi

[ "$(id -u)" = 0 ] || { echo "run with sudo (or --mirror as the desktop user)" >&2; exit 1; }

BOOTCONF=
for f in /boot/boot.conf /boot.conf; do [ -f "$f" ] && { BOOTCONF=$f; break; }; done

redetect() { [ -w "$CONN/status" ] && echo detect > "$CONN/status" || true; }

if [ $undo = 1 ]; then
    if [ -n "$BOOTCONF" ] && grep -q "$PARAM" "$BOOTCONF"; then
        cp -p "$BOOTCONF" "$BOOTCONF.reimu-undo.$(date +%Y%m%d%H%M%S)"
        sed -i "s# $PARAM##g" "$BOOTCONF"
        echo "$BOOTCONF: $PARAM removed"
    fi
    [ -f /etc/initrd.mk ] && grep -q "$FW" /etc/initrd.mk && {
        sed -i "\#$FW#d" /etc/initrd.mk
        echo "/etc/initrd.mk: EDID removed (run make-initrd to rebuild the initrd)"
    }
    echo > /sys/module/drm/parameters/edid_firmware 2>/dev/null || true
    rm -f "$FW"
    redetect
    echo "undone; modes now: $(tr '\n' ' ' < "$CONN/modes" 2>/dev/null)"
    echo "(console=ttyS1 / serial-getty@ttyS1, if set by --console, are left as they are)"
    exit 0
fi

# 1. the EDID file
mkdir -p "${FW%/*}"
echo "$EDID_B64" | base64 -d > "$FW.tmp"
[ "$(md5sum "$FW.tmp" | cut -d' ' -f1)" = "$EDID_MD5" ] || { rm -f "$FW.tmp"; echo "EDID checksum mismatch" >&2; exit 1; }
mv "$FW.tmp" "$FW"
chmod 644 "$FW"
echo "installed $FW"

# 2. now, without a reboot
if [ -w /sys/module/drm/parameters/edid_firmware ]; then
    echo "$OUT:edid/$EDID_NAME" > /sys/module/drm/parameters/edid_firmware
    redetect
    echo "$OUT modes now: $(tr '\n' ' ' < "$CONN/modes")"
else
    echo "this kernel can't load an EDID at run time (CONFIG_DRM_LOAD_EDID_FIRMWARE); boot.conf only"
fi

# 3. after a reboot: kernel parameter in the boot loader (ПНС) config
if [ -z "$BOOTCONF" ]; then
    echo "no /boot/boot.conf: add to the kernel command line yourself: $PARAM"
elif ! grep -q '^[[:space:]]*cmdline' "$BOOTCONF"; then
    echo "$BOOTCONF has no cmdline= lines: add to the kernel command line yourself: $PARAM"
else
    backup="$BOOTCONF.reimu.$(date +%Y%m%d%H%M%S)"
    cp -p "$BOOTCONF" "$backup"
    grep -q "$PARAM" "$BOOTCONF" || sed -i "/^[[:space:]]*cmdline/ s#\$# $PARAM#" "$BOOTCONF"
    if [ $console = 1 ]; then
        sed -i '/^[[:space:]]*cmdline/ s#console=ttyS0\([,[:space:]]\)#console=ttyS1\1#g; /^[[:space:]]*cmdline/ s#console=ttyS0$#console=ttyS1#' "$BOOTCONF"
        grep '^[[:space:]]*cmdline' "$BOOTCONF" | grep -q 'console=ttyS1' ||
            sed -i "/^[[:space:]]*cmdline/ s#\$# console=ttyS1,115200#" "$BOOTCONF"
    fi
    if cmp -s "$backup" "$BOOTCONF"; then
        rm -f "$backup"
        echo "$BOOTCONF: already set"
    else
        echo "$BOOTCONF changed (backup: $backup):"
        grep '^[[:space:]]*cmdline' "$BOOTCONF" | sed 's/^/    /'
    fi
fi

if [ $console = 1 ]; then
    systemctl enable --now serial-getty@ttyS1.service && echo "serial-getty@ttyS1 enabled (login on the BMC console)"
fi

# 4. optional: initrd (ALT make-initrd)
if [ $initrd = 1 ]; then
    if [ -f /etc/initrd.mk ] && command -v make-initrd >/dev/null; then
        grep -q "$FW" /etc/initrd.mk || echo "PUT_FILES += $FW" >> /etc/initrd.mk
        make-initrd && echo "initrd rebuilt with the EDID"
    else
        echo "no ALT make-initrd here: add $FW to your initrd yourself"
    fi
fi

echo
echo "done. To show the desktop on the BMC VGA (KVM) now, as the desktop user:"
echo "    $0 --mirror"
