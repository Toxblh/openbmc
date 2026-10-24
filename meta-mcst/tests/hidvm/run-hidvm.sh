#!/bin/sh
# run-hidvm.sh [WORKDIR]: the HID test VM in the background (console in
# WORKDIR/console.log); ssh: vssh, 127.0.0.1:${HIDVM_SSH:-2522}; WORKDIR/share
# is /mnt in the VM. Two dummy UDCs: dummy_udc.0 and .1 (KVM and media).
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(cd "${1:-$HERE/work}" && pwd)
ACCEL="-accel hvf -cpu host"
[ "$(uname)" = Linux ] && ACCEL="-accel kvm -cpu host"
qemu-system-aarch64 -M virt $ACCEL -smp 4 -m 2048 -nographic \
  -kernel "$W/Image" -append "root=/dev/vda rw console=ttyAMA0 quiet dummy_hcd.num=2" \
  -drive file="$W/root.img",if=virtio,format=raw \
  -netdev user,id=n0,hostfwd=tcp:127.0.0.1:${HIDVM_SSH:-2522}-:22 -device virtio-net-pci,netdev=n0 \
  -fsdev local,id=fs0,path="$W/share",security_model=none -device virtio-9p-pci,fsdev=fs0,mount_tag=hostfs \
  -serial file:"$W/console.log" -monitor none > "$W/qemu.out" 2>&1 &
echo $! > "$W/qemu.pid"
for i in $(seq 60); do "$HERE/vssh" "$W" true 2>/dev/null && exit 0; sleep 3; done
echo "the VM did not come up (see $W/console.log)" >&2
exit 1
