#!/bin/sh
# Starts the test "BMC". Platform COM ports are unix sockets com1.sock and
# com2.sock next to this script; https://127.0.0.1:8443 is bmcweb, ssh on 2222.
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(realpath -m "${1:-$HERE/work}")
ACCEL=""
[ -w /dev/kvm ] && ACCEL="-enable-kvm -cpu host"
exec qemu-system-x86_64 $ACCEL -m 2048 -smp 2 -nographic \
  -kernel "$W/vmlinuz" -initrd "$W/initrd.img" -append "root=/dev/vda rw console=ttyS0 net.ifnames=1" \
  -drive file="$W/disk.img",if=virtio,format=raw \
  -netdev user,id=n0,hostfwd=tcp:127.0.0.1:8443-:443,hostfwd=tcp:127.0.0.1:2222-:22 -device virtio-net-pci,netdev=n0 \
  -serial mon:stdio \
  -chardev socket,id=com1,path="$HERE/com1.sock",server=on,wait=off -serial chardev:com1 \
  -chardev socket,id=com2,path="$HERE/com2.sock",server=on,wait=off -serial chardev:com2
