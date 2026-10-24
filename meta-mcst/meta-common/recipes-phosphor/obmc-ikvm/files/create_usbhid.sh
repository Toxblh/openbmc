#!/bin/sh
# create_usbhid.sh (REIMU; in place of obmc-ikvm's): the KVM's USB keyboard
# (hid.0) and absolute mouse with a wheel (hid.1) as the configfs gadget
# obmc_hid on a port of the virtual hub. start-ipkvm runs it before every
# start of obmc-ikvm; once the gadget exists it only checks it (no unbind:
# the host would see the keyboard and mouse go and come back).
#
# Each report descriptor goes to configfs in ONE write(): f_hid takes every
# write() to report_desc as the whole descriptor. obmc-ikvm's script used
# echo -ne, and bash (/bin/sh here) flushes echo/printf output at each 0x0a
# byte: the mouse descriptor has one (05 0c 0a 38 02, Consumer / AC Pan), so
# configfs kept its last 14 bytes, and the host refused the mouse ("unknown
# main item tag 0x0"). Here the bytes go into a file in /run first and dd
# copies it in one read() and one write(); then it is read back and compared.
# Any mismatch: the gadget is taken down again and the script fails (so does
# start-ipkvm, which retries).

G=/sys/kernel/config/usb_gadget/obmc_hid
TMP=/run/obmc_hid

# The keyboard is what the host's boot firmware (PNS, its boot menu) sees:
# kept exactly as in the stock firmware, where the PNS takes it (interface 0,
# boot subclass, keyboard protocol, 8-byte reports, the 63-byte descriptor, an
# interrupt IN and an interrupt OUT endpoint, the same device and strings).
# Switches (/etc/default/obmc-hid, both default to the stock layout):
#   HID_KBD_NO_OUT_EP=1  the keyboard without its OUT endpoint: the LED
#       reports come as SET_REPORT on ep0 (f_hid keeps the last one) instead of
#       interrupt OUT transfers that queue up when nobody reads /dev/hidg0
#       (the host's "output queue full"); reimu-kvm reads them either way.
#       Changes the endpoint layout the PNS enumerates: off until checked there.
#   HID_PTR_BOOT=1  the mouse as before (boot subclass 1, protocol 2 "mouse").
#       Default: subclass 0, protocol 0: its reports are absolute, a firmware
#       that switched a boot mouse to the boot protocol would take x/y for
#       relative movement (f_hid refuses SET_PROTOCOL without the boot subclass)
HID_KBD_NO_OUT_EP=0
HID_PTR_BOOT=0
[ -r /etc/default/obmc-hid ] && . /etc/default/obmc-hid
if [ "$HID_PTR_BOOT" = 1 ]; then PTR_SUB=1 PTR_PROTO=2; else PTR_SUB=0 PTR_PROTO=0; fi

# Keyboard, boot protocol, 8-byte reports
#  05 01  USAGE_PAGE (Generic Desktop)      09 06  USAGE (Keyboard)
#  a1 01  COLLECTION (Application)
#  05 07    USAGE_PAGE (Keyboard)
#  19 e0    USAGE_MINIMUM (LeftControl)     29 e7  USAGE_MAXIMUM (Right GUI)
#  15 00    LOGICAL_MINIMUM (0)             25 01  LOGICAL_MAXIMUM (1)
#  75 01    REPORT_SIZE (1)                 95 08  REPORT_COUNT (8)
#  81 02    INPUT (Data,Var,Abs)            modifiers
#  95 01    REPORT_COUNT (1)                75 08  REPORT_SIZE (8)
#  81 03    INPUT (Cnst,Var,Abs)            reserved
#  95 05    REPORT_COUNT (5)                75 01  REPORT_SIZE (1)
#  05 08    USAGE_PAGE (LEDs)
#  19 01    USAGE_MINIMUM (Num Lock)        29 05  USAGE_MAXIMUM (Kana)
#  91 02    OUTPUT (Data,Var,Abs)
#  95 01    REPORT_COUNT (1)                75 03  REPORT_SIZE (3)
#  91 03    OUTPUT (Cnst,Var,Abs)           LED padding
#  95 06    REPORT_COUNT (6)                75 08  REPORT_SIZE (8)
#  15 00    LOGICAL_MINIMUM (0)             25 65  LOGICAL_MAXIMUM (101)
#  05 07    USAGE_PAGE (Keyboard)
#  19 00    USAGE_MINIMUM (0)               29 65  USAGE_MAXIMUM (Application)
#  81 00    INPUT (Data,Ary,Abs)            6 key codes
#  c0     END_COLLECTION
KBD_DESC='05 01 09 06 a1 01 05 07 19 e0 29 e7 15 00 25 01 75 01 95 08 81 02
95 01 75 08 81 03 95 05 75 01 05 08 19 01 29 05 91 02 95 01 75 03 91 03 95 06
75 08 15 00 25 65 05 07 19 00 29 65 81 00 c0'

# Mouse, absolute X/Y 0..32767, wheel and AC Pan, 7-byte reports (interface 1)
#  05 01  USAGE_PAGE (Generic Desktop)      09 02  USAGE (Mouse)
#  a1 01  COLLECTION (Application)
#  09 01    USAGE (Pointer)
#  a1 00    COLLECTION (Physical)
#  05 09      USAGE_PAGE (Button)
#  19 01      USAGE_MINIMUM (Button 1)      29 03  USAGE_MAXIMUM (Button 3)
#  15 00      LOGICAL_MINIMUM (0)           25 01  LOGICAL_MAXIMUM (1)
#  95 03      REPORT_COUNT (3)              75 01  REPORT_SIZE (1)
#  81 02      INPUT (Data,Var,Abs)          buttons
#  95 01      REPORT_COUNT (1)              75 05  REPORT_SIZE (5)
#  81 03      INPUT (Cnst,Var,Abs)          padding
#  05 01      USAGE_PAGE (Generic Desktop)
#  09 30      USAGE (X)                     09 31  USAGE (Y)
#  35 00      PHYSICAL_MINIMUM (0)          46 ff 7f  PHYSICAL_MAXIMUM (32767)
#  15 00      LOGICAL_MINIMUM (0)           26 ff 7f  LOGICAL_MAXIMUM (32767)
#  65 11      UNIT (SI Lin:Distance)        55 00  UNIT_EXPONENT (0)
#  75 10      REPORT_SIZE (16)              95 02  REPORT_COUNT (2)
#  81 02      INPUT (Data,Var,Abs)          x, y
#  09 38      USAGE (Wheel)
#  35 00      PHYSICAL_MINIMUM (0)          45 00  PHYSICAL_MAXIMUM (0)
#  65 00      UNIT (None)
#  15 81      LOGICAL_MINIMUM (-127)        25 7f  LOGICAL_MAXIMUM (127)
#  75 08      REPORT_SIZE (8)               95 01  REPORT_COUNT (1)
#  81 06      INPUT (Data,Var,Rel)          wheel
#  05 0c      USAGE_PAGE (Consumer)
#  0a 38 02   USAGE (AC Pan)
#  15 81      LOGICAL_MINIMUM (-127)        25 7f  LOGICAL_MAXIMUM (127)
#  75 08      REPORT_SIZE (8)               95 01  REPORT_COUNT (1)
#  81 06      INPUT (Data,Var,Rel)          pan
#  c0       END_COLLECTION
#  c0     END_COLLECTION
PTR_DESC='05 01 09 02 a1 01 09 01 a1 00 05 09 19 01 29 03 15 00 25 01 95 03
75 01 81 02 95 01 75 05 81 03 05 01 09 30 09 31 35 00 46 ff 7f 15 00 26 ff 7f
65 11 55 00 75 10 95 02 81 02 09 38 35 00 45 00 65 00 15 81 25 7f 75 08 95 01
81 06 05 0c 0a 38 02 15 81 25 7f 75 08 95 01 81 06 c0 c0'

log() { echo "create_usbhid.sh: $*" >&2; }

# hex2bin "HEX BYTES" FILE: the bytes into FILE (octal escapes in a printf
# format; a regular file, so how printf splits its writes doesn't matter)
hex2bin() {
    fmt=
    for b in $1; do
        o=$((0x$b))
        fmt="$fmt\\$((o / 64))$((o / 8 % 8))$((o % 8))"
    done
    printf "$fmt" > "$2"
}

# check FUNCTION HEX PROTOCOL REPORT_LENGTH SUBCLASS: configfs holds that function
check() {
    hex2bin "$2" $TMP.want || return 1
    cmp -s $TMP.want $G/functions/$1/report_desc || {
        log "$1: report_desc is not the expected $(echo $2 | wc -w) bytes:" \
            "$(od -An -tx1 -v $G/functions/$1/report_desc | tr -s ' \n' '  ')"
        return 1
    }
    have="$(cat $G/functions/$1/protocol) $(cat $G/functions/$1/report_length) $(cat $G/functions/$1/subclass)"
    [ "$have" = "$3 $4 $5" ] || {
        log "$1: protocol, report_length, subclass $have, expected $3 $4 $5"
        return 1
    }
}

# hid FUNCTION PROTOCOL REPORT_LENGTH SUBCLASS HEX: create the function,
# the descriptor in one write()
hid() {
    f=$G/functions/$1
    mkdir $f || return 1
    echo $2 > $f/protocol || return 1
    echo $3 > $f/report_length || return 1
    echo $4 > $f/subclass || return 1	# 1: boot interface
    hex2bin "$5" $TMP.desc || return 1
    dd if=$TMP.desc of=$f/report_desc bs=4096 count=1 status=none || return 1
    check $1 "$5" $2 $3 $4
}

# down: take a half-made gadget down again, so the next run starts afresh
down() {
    echo "" > $G/UDC 2>/dev/null
    rm -f $G/configs/c.1/hid.0 $G/configs/c.1/hid.1
    rmdir $G/configs/c.1/strings/0x409 $G/configs/c.1 $G/functions/hid.0 $G/functions/hid.1 \
        $G/strings/0x409 $G 2>/dev/null
}

trap 'rm -f $TMP.want $TMP.desc' EXIT

if [ -e $G ]; then
    # made by an earlier start: left as it is (a rebind reconnects both
    # devices on the host), only reported when it isn't as expected
    check hid.0 "$KBD_DESC" 1 8 1 && check hid.1 "$PTR_DESC" $PTR_PROTO 7 $PTR_SUB ||
        log "obmc_hid is not as expected: left as it is"
    exit 0
fi

create() {
    mkdir $G || return 1
    cd $G || return 1
    echo 0x0100 > bcdDevice
    echo 0x0200 > bcdUSB
    echo 0x0104 > idProduct		# Multifunction Composite Gadget
    echo 0x1d6b > idVendor		# Linux Foundation
    mkdir strings/0x409 || return 1
    echo "OpenBMC" > strings/0x409/manufacturer
    echo "virtual_input" > strings/0x409/product
    echo "OBMC0001" > strings/0x409/serialnumber

    # the descriptors before the functions are linked into the configuration:
    # 6.18 refuses report_desc writes of a linked function (EBUSY)
    hid hid.0 1 8 1 "$KBD_DESC" || return 1
    if [ "$HID_KBD_NO_OUT_EP" = 1 ]; then
        echo 1 > functions/hid.0/no_out_endpoint || return 1
    fi
    hid hid.1 $PTR_PROTO 7 $PTR_SUB "$PTR_DESC" || return 1

    mkdir configs/c.1 || return 1
    mkdir configs/c.1/strings/0x409 || return 1
    echo 0x80 > configs/c.1/bmAttributes
    echo 200 > configs/c.1/MaxPower
    echo "" > configs/c.1/strings/0x409/configuration
    ln -s functions/hid.0 configs/c.1 || return 1
    ln -s functions/hid.1 configs/c.1 || return 1

    # the first port of the virtual hub without a gadget
    dev_name="1e6a0000.usb-vhub"
    base_usb_dir="/sys/bus/platform/devices/${dev_name}/${dev_name}:p"
    port=1
    while [ $port -lt 5 ] && [ -e "${base_usb_dir}${port}/gadget/suspended" ]; do
        port=$((port + 1))
    done
    # (not bound: the gadget stays, obmc-ikvm still serves the screen)
    echo "${dev_name}:p${port}" > UDC || log "binding to ${dev_name}:p${port} failed"
    cd /
    # once more as the host will read them
    check hid.0 "$KBD_DESC" 1 8 1 && check hid.1 "$PTR_DESC" $PTR_PROTO 7 $PTR_SUB
}

if ! create; then
    log "making the gadget obmc_hid failed: taken down again"
    cd /
    down
    exit 1
fi
exit 0
