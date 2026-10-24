#!/bin/sh
# build-hidvm.sh [WORKDIR]: the HID test VM (arm64, kernel 6.18 like the A/B
# image's, dummy_hcd: the VM is both the BMC side, with the configfs gadgets,
# and the USB host that enumerates them). On a Mac with docker (OrbStack /
# Docker Desktop on Apple silicon: arm64 containers run natively, the VM runs
# with HVF); the kernel source comes from the build container "reimu".
#   WORKDIR (default ./work): Image, root.img, id_hidvm, share/ (9p, /mnt in the VM)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
MCST=$(cd "$HERE/../.." && pwd)
W=${1:-$HERE/work}
mkdir -p "$W/share" "$W/ctx"
[ -f "$W/id_hidvm" ] || ssh-keygen -q -t ed25519 -N "" -f "$W/id_hidvm"
KSRC=${KSRC:-/work/build-ab/tmp/work-shared/reimu-5564/kernel-source}
docker volume create hidvm >/dev/null
echo "== kernel source ($KSRC in the container reimu) and meta-ab64's patches"
docker run --rm -v hidvm:/hv alpine sh -c 'rm -rf /hv/patches; mkdir -p /hv/patches'
if ! docker run --rm -v hidvm:/hv alpine test -d /hv/linux; then
    docker exec reimu tar -C "$(dirname $KSRC)" --exclude=.git -cf - "$(basename $KSRC)" |
        docker run --rm -i -v hidvm:/hv alpine sh -c "tar -C /hv -xf - && mv /hv/$(basename $KSRC) /hv/linux"
fi
COPYFILE_DISABLE=1 tar -C "$MCST/meta-ab64/recipes-kernel/linux/linux-aspeed" -cf - $(cd "$MCST/meta-ab64/recipes-kernel/linux/linux-aspeed" && ls *.patch) |
    docker run --rm -i -v hidvm:/hv alpine tar -C /hv/patches -xf -
echo "== kernel"
docker build --platform linux/arm64 -q -t hidvm-build -f "$HERE/Dockerfile.build" "$HERE" >/dev/null
docker run --rm --platform linux/arm64 -v hidvm:/hv -v "$HERE:/s" hidvm-build /s/kbuild.sh
echo "== root file system (Ubuntu 24.04 arm64)"
cp "$HERE/Dockerfile.root" "$W/id_hidvm.pub" "$W/ctx/"
cp "$MCST/meta-common/recipes-mcst/reimu-kvm/files/xyz.openbmc_project.Reimu.Kvm.conf" "$W/ctx/"
docker build --platform linux/arm64 -q -t hidvm-root -f "$W/ctx/Dockerfile.root" "$W/ctx" >/dev/null
docker rm -f hidvm-exp >/dev/null 2>&1 || true
docker create --platform linux/arm64 --name hidvm-exp hidvm-root >/dev/null
docker export hidvm-exp | docker run --rm -i --platform linux/arm64 -v hidvm:/hv hidvm-build sh -c '
    rm -rf /hv/root && mkdir /hv/root && tar -C /hv/root -xf - 2>/dev/null
    rm -f /hv/root/.dockerenv; mv /hv/root/etc/resolv.conf.vm /hv/root/etc/resolv.conf 2>/dev/null || true
    truncate -s 3G /hv/root.img && mkfs.ext4 -q -F -d /hv/root /hv/root.img'
docker rm hidvm-exp >/dev/null
docker run --rm -v hidvm:/hv -v "$W:/o" alpine sh -c 'cp /hv/Image /o/Image && cp /hv/root.img /o/root.img'
echo "== done: $W (run-hidvm.sh $W)"
