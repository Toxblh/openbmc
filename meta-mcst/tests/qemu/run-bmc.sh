#!/bin/bash
# Boots a REIMU flash image in QEMU (ast2500-evb, 64 MiB SPI NOR) in the
# background. No KVM video, no USB host and no platform hardware: for testing
# the BMC software (web, serial, media up to the USB gadget, devkit).
#
#   run-bmc.sh IMAGE.mtd NAME [SSH_PORT] [HTTPS_PORT]
#
# The image file is written to (rwfs): pass a copy. Sockets in $RQ (/tmp/rq):
#   NAME.con   BMC console (UART5), also logged to NAME.log next to this script
#   NAME.com1  platform COM1 = UART1 = /dev/ttyS0 on the BMC (reimu-serial "host")
#   NAME.qmp   QMP, taken by the UID loop below; NAME.qmp2 is free for other clients
# ssh/https: 127.0.0.1:SSH_PORT (2222), 127.0.0.1:HTTPS_PORT (2443); root/0penBmc.
#
# QEMU has no board around the BMC, so two things are emulated here:
#  - the UID button (GPIO N3, active low) reads as pressed, and revert-to-mfg
#    then wipes rwfs and reboots forever: a QMP loop holds it released (U-Boot
#    resets the GPIO block on every boot, a single qom-set isn't enough);
#  - the NIC gets the MAC set_mac programs. A fresh image still reboots once
#    (set_mac, revert-to-mfg) and may come back with another DHCP lease
#    (10.0.2.16): the forwarding to 10.0.2.15 then gets "connection reset".
#    bmcfwd NAME re-points it to the address the BMC has.
set -eu
img=$1; name=$2; ssh=${3:-2222}; https=${4:-2443}
RQ=${RQ:-/tmp/rq}  # short path: AF_UNIX socket paths are limited to ~104 bytes
d=$(cd "$(dirname "$0")" && pwd); mkdir -p "$RQ"
qemu-system-arm -M ast2500-evb,fmc-model=mx66l51235f -m 512 -nographic \
  -drive file="$img",if=mtd,format=raw \
  -netdev user,id=n0,hostfwd=tcp:127.0.0.1:$ssh-:22,hostfwd=tcp:127.0.0.1:$https-:443 \
  -net nic,netdev=n0,macaddr=98:a7:b0:00:00:01 \
  -qmp unix:$RQ/$name.qmp,server,nowait -qmp unix:$RQ/$name.qmp2,server,nowait \
  -chardev socket,id=con,path=$RQ/$name.con,server=on,wait=off,logfile=$d/$name.log -serial chardev:con \
  -chardev socket,id=com1,path=$RQ/$name.com1,server=on,wait=off -serial chardev:com1 \
  -monitor none >"$d/$name.qemu.log" 2>&1 </dev/null &
sleep 1
nohup python3 - "$RQ/$name.qmp" >/dev/null 2>&1 <<'PY' &
import json, socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); f = s.makefile("rw"); f.readline()
def cmd(c, **a):
    f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r:
            return r
cmd("qmp_capabilities")
while True:  # exits with QEMU (broken socket)
    cmd("qom-set", path="/machine/soc/gpio", property="gpioN3", value=True)
    time.sleep(0.5)
PY
echo "$name.$ssh.$https" > "$RQ/$name.ports"
echo "$name: console $RQ/$name.con (log $d/$name.log), COM1 $RQ/$name.com1, ssh :$ssh, https :$https"
