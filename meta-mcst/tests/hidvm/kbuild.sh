#!/bin/sh
# (in the arm64 build container) the HID test VM's kernel: the A/B image's
# openbmc/linux 6.18 with meta-ab64's kernel patches (/hv/patches), as arm64
# defconfig + dummy_hcd (the VM is the USB host of its own gadgets), configfs
# f_hid and mass storage, usbhid, usb-storage, sr, sg, loop, 9p
set -e
cd /hv/linux
for p in /hv/patches/*.patch; do
    [ -f "$p" ] || continue
    if patch -p1 -R --dry-run -s -f < "$p" >/dev/null 2>&1; then echo "applied already: ${p##*/}"
    else patch -p1 -s -f < "$p" && echo "applied: ${p##*/}"; fi
done
O=/hv/kout
mkdir -p $O
[ -f $O/.config ] || make O=$O ARCH=arm64 defconfig >/dev/null
S="scripts/config --file $O/.config"
for y in USB USB_GADGET USB_DUMMY_HCD USB_LIBCOMPOSITE USB_CONFIGFS USB_CONFIGFS_F_HID \
         USB_CONFIGFS_MASS_STORAGE USB_F_HID USB_F_MASS_STORAGE USB_HID HID HID_GENERIC \
         USB_STORAGE BLK_DEV_SD BLK_DEV_SR CHR_DEV_SG SCSI INPUT_EVDEV DEBUG_FS \
         CONFIGFS_FS BLK_DEV_NBD BLK_DEV_LOOP NET_9P NET_9P_VIRTIO 9P_FS VIRTIO_BLK VIRTIO_NET \
         VIRTIO_PCI IPV6 DEVTMPFS DEVTMPFS_MOUNT INPUT_UINPUT HIDRAW USB_HIDDEV USB_MON; do
    $S -e $y
done
$S -d USB_ETH -d USB_G_SERIAL
make O=$O ARCH=arm64 olddefconfig >/dev/null
make O=$O ARCH=arm64 -j"$(nproc)" Image >/hv/kbuild.log 2>&1 || { tail -30 /hv/kbuild.log; exit 1; }
cp $O/arch/arm64/boot/Image /hv/Image
echo KERNEL-OK
