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
