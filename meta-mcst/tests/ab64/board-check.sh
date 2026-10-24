#!/bin/sh
# board-check.sh: what the BMC firmware needs on the real REIMU-5564, read
# only (runs on the BMC, the A/B firmware in test mode or installed). Writes
# nothing, doesn't touch the power, the PNS flash or the platform's input.
#   PASS/FAIL/INFO lines; exit 1 if anything failed.
fails=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
info() { echo "INFO $*"; }
t() { # description, command...
    d=$1; shift
    if out=$(sh -c "$*" 2>&1); then pass "$d${out:+ ($(echo "$out" | tr '\n' ' ' | cut -c1-110))}"
    else fail "$d${out:+ ($(echo "$out" | tr '\n' ' ' | cut -c1-110))}"; fi
}

info "firmware $(sed -n 's/^VERSION_ID=//p' /etc/os-release), kernel $(uname -r), $(cat /run/initramfs/reimu-ab 2>/dev/null)"
info "cmdline: $(cat /proc/cmdline)"

# kernel
t "no oops/BUG/panic in dmesg" '! dmesg | grep -E "Oops|BUG:|Unable to handle|kernel BUG|Call trace|WARNING:" | head -3 | grep .'
t "RAM: MemTotal > 470 MiB" 'm=$(sed -n "s/MemTotal: *\([0-9]*\).*/\1/p" /proc/meminfo); echo $((m/1024)) MiB; [ $m -gt 481280 ]'
t "RAM: MemAvailable > 300 MiB" 'm=$(sed -n "s/MemAvailable: *\([0-9]*\).*/\1/p" /proc/meminfo); echo $((m/1024)) MiB; [ $m -gt 307200 ]'
t "no OOM" '! dmesg | grep -iE "out of memory|oom-kill" | grep .'
t "flash: the A/B layout" 'n=$(sed -n "s/.*\"\(.*\)\"/\1/p" /proc/mtd | grep -vx sys | tr "\n" " "); echo $n; [ "$n" = "bmc u-boot u-boot-env kernel-a rofs-a kernel-b rofs-b rwfs " ]'
info "PNS flash: $(grep -q "\"sys\"" /proc/mtd && echo "present (/dev/mtd/sys)" || echo "not found (normal after a kexec: the switch is on the host side)")"
t "flash chip: 64 MiB" 'm=$(grep -xl bmc /sys/class/mtd/mtd*/name | head -n1); [ "$(cat ${m%/name}/size)" = 67108864 ] && cat /sys/bus/spi/devices/spi0.0/spi-nor/partname 2>/dev/null'
grep -qw reimu.test /proc/cmdline && t "test mode: every flash partition read-only" 'for f in /sys/class/mtd/mtd[0-9]*/flags; do [ "$(cat $f)" = 0x800 ] || { echo "$f $(cat $f)"; exit 1; }; done'
t "watchdog0 is WDT1 (1e785000), wdt2 off" 'readlink -f /sys/class/watchdog/watchdog0/device | grep -q 1e785000 && ! ls /sys/class/watchdog/ | grep -q watchdog1'
t "systemd feeds the watchdog" 'cat /sys/class/watchdog/watchdog0/state | grep -q active'
t "GPIO: one aspeed chip, base and line names" 'b=$(cat /sys/class/gpio/gpiochip*/base); echo base $b; [ "$(cat /sys/class/gpio/gpiochip*/label)" = 1e780000.gpio ]'
t "gpio-funcs: N3 -> number" '. /libexec/gpio-funcs; n=$(gpio2num N3); echo $n; [ $n = $(( $(cat /sys/class/gpio/gpiochip*/base) + 107 )) ]'
t "SoC family (gpiotab)" 'grep -qx AST2500 /sys/devices/soc0/family'
t "video engine: /dev/video0" 'ls /dev/video0 && dmesg | grep -i "aspeed-video" | head -1'
t "USB gadget: vhub" 'ls /sys/class/udc/ | grep -q 1e6a0000.usb-vhub; ls /sys/class/udc'
t "I2C buses" 'ls /dev/i2c-* | wc -l | grep -qE "^ *1[0-9]$" && ls /dev/i2c-* | tr "\n" " "'
t "UARTs ttyS0..ttyS4" 'for i in 0 1 2 3 4; do [ -e /dev/ttyS$i ] || exit 1; done'
t "RTC" 'cat /sys/class/rtc/rtc0/name'
t "hwmon: fans (pwm-tacho), ADC" 'for h in /sys/class/hwmon/hwmon*; do echo -n "$(cat $h/name) "; done; ls /sys/class/hwmon/*/fan1_input /sys/bus/iio/devices/iio:device0/in_voltage0_raw >/dev/null'
t "MCST modules: l-pcs-i2c (CPU sensors) loaded" 'lsmod | grep -q l_pcs_i2c'
info "sensors: $(for h in /sys/class/hwmon/hwmon*; do for f in $h/temp*_input; do [ -f $f ] && echo -n "$(cat $h/name):$(( $(cat $f) / 1000 ))C "; done; done)"
info "fans: $(cat /sys/class/hwmon/hwmon*/fan*_input 2>/dev/null | tr '\n' ' ')"

# network
t "eth0 up with the board MAC" 'ip -4 -o addr show eth0 | grep -q inet && m=$(cat /sys/class/net/eth0/address) && [ "$m" = "$(fw_printenv -n ethaddr | tr A-F a-f)" ] && echo $m $(ip -4 -o addr show eth0 | sed "s/.*inet \([^ ]*\).*/\1/")'
t "eth1 MAC" '[ "$(cat /sys/class/net/eth1/address)" = "$(fw_printenv -n eth1addr | tr A-F a-f)" ]'

# user space
f=$(systemctl --failed --no-legend --plain 2>/dev/null | cut -d' ' -f1 | tr '\n' ' ')
# clear-once clears openbmconce in the U-Boot environment: read-only in test mode
grep -qw reimu.test /proc/cmdline && f=$(echo "$f" | sed 's/clear-once.service //')
[ -z "$f" ] && pass "no failed units" || fail "failed units: $f"
for u in bmcweb.socket reimu-serial reimu-pns reimu-kvm start-ipkvm tmux-logger phosphor-hwmon@* \
         xyz.openbmc_project.State.Host xyz.openbmc_project.State.Chassis reimu-power-control \
         dropbear.socket sshd.socket systemd-networkd; do
    s=$(systemctl list-units --plain --no-legend "$u" 2>/dev/null | head -1)
    [ -n "$s" ] && info "unit $(echo $s | cut -d' ' -f1,3,4)"
done
# (the web is checked from outside: busybox wget's TLS doesn't talk to bmcweb)
t "web: bmcweb listens on 443" 'netstat -ltn | grep -q ":443 "'
t "host state readable (D-Bus)" 'busctl get-property xyz.openbmc_project.State.Host /xyz/openbmc_project/state/host0 xyz.openbmc_project.State.Host CurrentHostState'
t "chassis power state readable" 'busctl get-property xyz.openbmc_project.State.Chassis /xyz/openbmc_project/state/chassis0 xyz.openbmc_project.State.Chassis CurrentPowerState'
t "FRU read (mcst-fruid)" 'ls /var/volatile/motherboard_info.xml && systemctl is-active mcst-fruid'
t "FRU MAC = environment MAC (set_mac would leave it)" 'b=$(xml sel -t -v "/fruinfo/internal_use_area/mac_addr" /var/volatile/motherboard_info.xml 2>/dev/null | sed "s/ //g;s/,//g;s/0x//g"); [ -n "$b" ] && e=$(printf %X $((0x$b + 0x1E))) && echo FRU $e env $(fw_printenv -n ethaddr) && [ "$(echo $e | sed "s/\(..\)/\1:/g;s/:$//")" = "$(fw_printenv -n ethaddr | tr a-f A-F)" ]'
t "COM ports: reimu-serial" 'systemctl is-active reimu-serial && ls /run/reimu-serial* 2>/dev/null | head -3'
t "KVM: reimu-kvm" 'systemctl is-active reimu-kvm'
t "locale ru_RU.UTF-8" 'ls /usr/lib/locale/locale-archive >/dev/null && LANG=ru_RU.UTF-8 tmux -V'
info "memory: $(free | sed -n 2p)"
info "RSS KiB: $(for s in /proc/[0-9]*/status; do awk '/^Name/{n=$2} /^VmRSS/{print $2, n}' $s; done 2>/dev/null | sort -rn | head -6 | tr '\n' ' ')"
echo "$fails failed"
[ $fails = 0 ]

# IPMI, read only: the host side (BT) and the LAN side (netipmid). In-band
# IPMI from the host needs a bridge holding /dev/ipmi-bt-host (btbridged);
# neither the stock 1.0.2 firmware nor the A/B one has one (INFO, not FAIL).
t "IPMI: /dev/ipmi-bt-host, driver bound to the BT controller" 'test -c /dev/ipmi-bt-host && ls -d /sys/bus/platform/drivers/ipmi-bt-host/*.ibt'
t "IPMI: phosphor-ipmi-host (ipmid) active" 'systemctl is-active phosphor-ipmi-host'
t "IPMI: ipmid on D-Bus (Ipmi.Host, Control.Host)" 'busctl list --no-pager | grep -qw xyz.openbmc_project.Ipmi.Host && busctl list --no-pager | grep -qw xyz.openbmc_project.Control.Host'
t "IPMI: ipmid answers Get Device ID (D-Bus)" 'busctl call xyz.openbmc_project.Ipmi.Host /xyz/openbmc_project/Ipmi xyz.openbmc_project.Ipmi.Server execute yyyaya{sv} 6 0 1 0 0 | grep -q "^(yyyyay) 7 0 1 0 "'
bt=$(for f in /proc/[0-9]*/fd/*; do case $(readlink $f 2>/dev/null) in /dev/ipmi-bt-host|/dev/ipmi-kcs*) cat ${f%/fd/*}/comm;; esac; done | sort -u | tr '\n' ' ')
info "IPMI: BT device held by: ${bt:-nobody (the host's in-band IPMI is not served)}"
t "IPMI: netipmid (phosphor-ipmi-net@eth0) active, UDP 623" 'systemctl is-active phosphor-ipmi-net@eth0 && netstat -lun | grep -q ":623 "'
info "IPMI: restarts ipmid $(systemctl show -p NRestarts --value phosphor-ipmi-host), netipmid $(systemctl show -p NRestarts --value phosphor-ipmi-net@eth0), bmcweb $(systemctl show -p NRestarts --value bmcweb)"
echo "$fails failed (with the IPMI checks)"
[ $fails = 0 ]

# KVM USB keyboard and mouse (obmc_hid, create_usbhid.sh), read only: each
# report descriptor byte for byte and parsed (known item tags, collections
# balanced, input report = report_length). A descriptor written to configfs
# in two write()s keeps only the last part (the mouse once had its last 14
# bytes: the host's hid-generic "unknown main item tag 0x0", no mouse).
KBD_HEX='05 01 09 06 a1 01 05 07 19 e0 29 e7 15 00 25 01 75 01 95 08 81 02 95 01 75 08 81 03 95 05 75 01 05 08 19 01 29 05 91 02 95 01 75 03 91 03 95 06 75 08 15 00 25 65 05 07 19 00 29 65 81 00 c0'
PTR_HEX='05 01 09 02 a1 01 09 01 a1 00 05 09 19 01 29 03 15 00 25 01 95 03 75 01 81 02 95 01 75 05 81 03 05 01 09 30 09 31 35 00 46 ff 7f 15 00 26 ff 7f 65 11 55 00 75 10 95 02 81 02 09 38 35 00 45 00 65 00 15 81 25 7f 75 08 95 01 81 06 05 0c 0a 38 02 15 81 25 7f 75 08 95 01 81 06 c0 c0'
# hid_parse HEX...: "input report N bytes", exit 1 with the first problem
hid_parse() {
    set -- $*
    n=0 depth=0 rsize=0 rcount=0 inbits=0
    while [ $# -gt 0 ]; do
        b=$((0x$1)); at=$n; shift; n=$((n + 1))
        [ $b = 254 ] && { echo "byte $at: long item"; return 1; }
        s=$((b & 3)); [ $s = 3 ] && s=4
        ty=$(((b >> 2) & 3)); tag=$((b >> 4))
        [ $# -ge $s ] || { echo "byte $at: item 0x$(printf %02x $b) truncated"; return 1; }
        d=0 i=0
        while [ $i -lt $s ]; do d=$((d | (0x$1 << (8 * i)))); shift; i=$((i + 1)); n=$((n + 1)); done
        case $ty:$tag in
            0:8) [ $depth -gt 0 ] || { echo "byte $at: Input outside a collection"; return 1; }
                 inbits=$((inbits + rsize * rcount)) ;;
            0:9|0:11) ;;
            0:10) depth=$((depth + 1)) ;;
            0:12) depth=$((depth - 1)); [ $depth -ge 0 ] || { echo "byte $at: collection stack underflow"; return 1; } ;;
            1:7) rsize=$d ;;
            1:9) rcount=$d ;;
            1:8) echo "byte $at: Report ID (none expected)"; return 1 ;;
            1:[0-9]|1:10|1:11) ;;
            2:[0-5]|2:[7-9]|2:10) ;;
            *) echo "byte $at: unknown $(echo main global local reserved | cut -d' ' -f$((ty + 1))) item tag 0x$(printf %x $tag)"; return 1 ;;
        esac
    done
    [ $depth = 0 ] || { echo "$depth collection(s) left open"; return 1; }
    [ $((inbits % 8)) = 0 ] || { echo "input report $inbits bits"; return 1; }
    echo "input report $((inbits / 8)) bytes"
}
HG=/sys/kernel/config/usb_gadget/obmc_hid
# keyboard: protocol 1 subclass 1 as in the stock firmware (the PNS boot
# menu); mouse: 0 0 (HID_PTR_BOOT=1 in /etc/default/obmc-hid: the stock 2 1)
ptr_ps="0 0"; grep -qs '^HID_PTR_BOOT=1' /etc/default/obmc-hid && ptr_ps="2 1"
for f in "hid.0 keyboard 1 1 8 $KBD_HEX" "hid.1 mouse ${ptr_ps% *} ${ptr_ps#* } 7 $PTR_HEX"; do
    set -- $f; fn=$1 name=$2 proto=$3 sub=$4 len=$5; shift 5; want=$*
    d=$HG/functions/$fn
    if [ ! -d $d ]; then fail "KVM $name: no $d"; continue; fi
    have=$(od -An -tx1 -v $d/report_desc | tr -s ' \n' '  ' | sed 's/^ //; s/ $//')
    nb=$(echo $have | wc -w)
    [ "$have" = "$want" ] && pass "KVM $name ($fn): report descriptor the expected $nb bytes" ||
        fail "KVM $name ($fn): report descriptor $nb bytes, not the expected $(echo $want | wc -w): $have"
    if p=$(hid_parse $have); then
        rl=$(cat $d/report_length)
        [ "$p" = "input report $len bytes" ] && [ "$rl" = $len ] && pass "KVM $name: descriptor parses, $p, report_length $rl" ||
            fail "KVM $name: descriptor $p, report_length $rl, expected $len"
    else
        fail "KVM $name: descriptor doesn't parse: $p"
    fi
    [ "$(cat $d/protocol) $(cat $d/subclass)" = "$proto $sub" ] && pass "KVM $name: protocol $proto subclass $sub" ||
        fail "KVM $name: protocol $(cat $d/protocol) subclass $(cat $d/subclass), expected $proto $sub"
done
# the keyboard's OUT endpoint, as in the stock firmware (HID_KBD_NO_OUT_EP=1 drops it)
t "KVM keyboard: OUT endpoint on (stock layout)" '[ "$(cat /sys/kernel/config/usb_gadget/obmc_hid/functions/hid.0/no_out_endpoint 2>/dev/null || echo 0)" = 0 ] || grep -qs "^HID_KBD_NO_OUT_EP=1" /etc/default/obmc-hid'
info "KVM reimu-kvm: $(busctl call xyz.openbmc_project.Reimu.Kvm /xyz/openbmc_project/reimu/kvm xyz.openbmc_project.Reimu.Kvm Status 2>/dev/null | sed 's/^s //')"
t "KVM: /dev/hidg0, /dev/hidg1" 'test -c /dev/hidg0 && test -c /dev/hidg1'
udc=$(cat $HG/UDC 2>/dev/null)
[ -n "$udc" ] && pass "KVM gadget bound to $udc" || fail "KVM gadget not bound to a UDC"
# "configured" once the host enumerated it (host on, USB up); "suspended"
# when the host suspended it; "not attached" with the host off
[ -n "$udc" ] && info "KVM gadget $udc state: $(cat /sys/class/udc/$udc/state 2>/dev/null)"
info "USB gadgets: $(for g in /sys/kernel/config/usb_gadget/*; do [ -d $g ] && echo -n "${g##*/}=$(cat $g/UDC) "; done)"
echo "$fails failed (with the KVM HID checks)"
[ $fails = 0 ]

# Regressions that only the board shows (meta-mcst/tests/ab64/REGRESSIONS.md,
# bug numbers in brackets), read only: D-Bus, sysfs, the journal.
host=$(busctl get-property xyz.openbmc_project.State.Host /xyz/openbmc_project/state/host0 xyz.openbmc_project.State.Host CurrentHostState 2>/dev/null | sed 's/.*\.//; s/"//')
t "[37] multi-master on the platform's I2C buses 2, 3, 5, 6 (running device tree)" 'for a in c0 100 180 1c0; do d=$(ls -d /proc/device-tree/ahb/apb/bus@1e78a000/i2c@$a 2>/dev/null); [ -e "$d/multi-master" ] || { echo "no multi-master: i2c@$a"; exit 1; }; done'
t "[33] eth0 DHCP as phosphor-networkd sees it (DHCP=true)" 'busctl get-property xyz.openbmc_project.Network /xyz/openbmc_project/network/eth0 xyz.openbmc_project.Network.EthernetInterface DHCPEnabled | grep -E "v4|both"'
t "[36] no fan control besides reimu-fand" '! pidof phosphor-fan-control phosphor-fan-monitor && systemctl is-active reimu-fand'
t "[21] no sensor on D-Bus with a negative value (-errno)" 'for s in $(busctl call xyz.openbmc_project.ObjectMapper /xyz/openbmc_project/object_mapper xyz.openbmc_project.ObjectMapper GetSubTree sias /xyz/openbmc_project/sensors 0 1 xyz.openbmc_project.Sensor.Value 2>/dev/null | grep -o "/xyz/openbmc_project/sensors/[^\"]*"); do svc=$(busctl call xyz.openbmc_project.ObjectMapper /xyz/openbmc_project/object_mapper xyz.openbmc_project.ObjectMapper GetObject sas $s 0 | grep -o "xyz.openbmc_project.Hwmon[^\"]*"); [ -n "$svc" ] || continue; v=$(busctl get-property $svc $s xyz.openbmc_project.Sensor.Value Value); case "$v" in "d -"*) echo "$s $v"; exit 1;; esac; done'
if [ "$host" = "Running" ]; then
    t "[13] overheatd monitors the CPUs (host on: <cpus> lists a CPU index, not blind)" 'x=$(cat /var/volatile/systemstate.xml 2>/dev/null); echo "$x" | grep -q "<cpus>[0-9]" && ! echo "$x" | grep -q blind'
    t "[35] reimu-fand: CPU fan chip in the accurate tach mode, no alarm" 'j=$(busctl get-property xyz.openbmc_project.Reimu.Fans /xyz/openbmc_project/reimu/fans xyz.openbmc_project.Reimu.Fans Json | tr -d "\\\\"); echo "$j" | grep -q "tach_mode_ok.:true" && ! echo "$j" | grep -q "alarm.:true"'
    t "[11][12] phosphor-hwmon sensors of the platform on D-Bus" 'busctl call xyz.openbmc_project.ObjectMapper /xyz/openbmc_project/object_mapper xyz.openbmc_project.ObjectMapper GetSubTree sias /xyz/openbmc_project/sensors 0 1 xyz.openbmc_project.Sensor.Value | grep -o "xyz.openbmc_project.Hwmon-[^\"]*" | sort -u | wc -l | grep -qv "^ *0$"'
else
    info "[13][35][11] host $host: overheatd, fan and sensor checks need the host on"
fi
# [20] rtcread reads the platform RTC only at a cold BMC boot with the platform on
info "[20] rtcread: $(journalctl -b -o cat -u rtcread 2>/dev/null | grep -E 'Time from|left alone|No onboard|failed' | tail -1)"
echo "$fails failed (with the regression checks)"
[ $fails = 0 ]
