#!/bin/bash
# Builds the x86 test stand used to check the REIMU web UI components with
# real bmcweb / reimu-serial / nbd-proxy / nbd-client / USB mass storage gadget
# (dummy_hcd), without ASPEED hardware. See ../README.md.
#
# Usage (as root, on an x86_64 Linux host with docker, debootstrap,
# qemu-system-x86, mkfs.ext4 >= 1.43):
#   ./build-testvm.sh [WORKDIR]     # default WORKDIR: ./work
#   ./run-vm.sh [WORKDIR]
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
MCST=$(cd "$HERE/../.." && pwd)               # meta-mcst
OPENBMC=$(cd "$MCST/.." && pwd)
W=$(realpath -m "${1:-$HERE/work}")
R=$W/rootfs
MIRROR=${MIRROR:-https://archive.ubuntu.com/ubuntu}
KVER=${KVER:-}                                 # autodetected
mkdir -p "$W/src"

srcrev() { grep -h '^SRCREV' "$1" | head -1 | cut -d'"' -f2; }
BMCWEB_REV=$(srcrev "$OPENBMC/meta-phosphor/recipes-phosphor/interfaces/bmcweb_git.bb")
JSNBD_REV=$(srcrev "$OPENBMC/meta-phosphor/recipes-connectivity/jsnbd/jsnbd_git.bb")
SDBUSPLUS_REV=$(srcrev "$OPENBMC/meta-phosphor/recipes-extended/sdbusplus/sdbusplus-rev.inc")

echo "== 1. root file system (Ubuntu 20.04, same base as the REIMU build host)"
if [ ! -x "$R/bin/bash" ]; then
    debootstrap --variant=minbase --components=main,universe \
        --include=systemd,systemd-sysv,udev,dbus,iproute2,ifupdown,isc-dhcp-client,nbd-client,kmod,ca-certificates,libpam-runtime,libpam-modules,openssl,netbase,procps,less,usbutils,dosfstools,util-linux,e2fsprogs,python3,libjson-c4,libudev1,libsystemd0,libssl1.1,zlib1g,libtinyxml2-6a,sudo \
        focal "$R" "$MIRROR"
fi
cat > "$R/etc/apt/sources.list" <<EOF
deb $MIRROR focal main universe
deb $MIRROR focal-updates main universe
EOF
cp /etc/resolv.conf "$R/etc/" || true
for c in /usr/local/share/ca-certificates/*.crt; do [ -e "$c" ] && cp "$c" "$R/usr/local/share/ca-certificates/"; done
chroot "$R" update-ca-certificates >/dev/null 2>&1 || true
chroot "$R" env DEBIAN_FRONTEND=noninteractive https_proxy="${https_proxy:-${HTTPS_PROXY:-}}" sh -c '
  apt-get update -q &&
  apt-get install -y -q --no-install-recommends linux-image-generic initramfs-tools openssh-server \
      x11vnc xvfb xterm x11-apps python3-dbus python3-gi gir1.2-glib-2.0 curl make gcc &&
  apt-get install -y -q --no-install-recommends linux-modules-extra-$(ls /lib/modules | head -1) linux-headers-$(ls /lib/modules | head -1)'
KVER=${KVER:-$(ls "$R/lib/modules" | head -1)}

echo "== 2. dummy_hcd (USB device+host loopback; the Ubuntu kernel doesn't ship it)"
mkdir -p "$R/root/dummy_hcd"
MAJMIN=$(echo "$KVER" | cut -d. -f1,2)
curl -sfL -o "$R/root/dummy_hcd/dummy_hcd.c" "https://raw.githubusercontent.com/torvalds/linux/v$MAJMIN/drivers/usb/gadget/udc/dummy_hcd.c"
echo 'obj-m := dummy_hcd.o' > "$R/root/dummy_hcd/Makefile"
chroot "$R" make -s -C "/lib/modules/$KVER/build" M=/root/dummy_hcd modules
mkdir -p "$R/lib/modules/$KVER/extra"
cp "$R/root/dummy_hcd/dummy_hcd.ko" "$R/lib/modules/$KVER/extra/"
chroot "$R" depmod "$KVER"
printf 'nbd\nlibcomposite\ndummy_hcd\nusb_storage\n' > "$R/etc/modules-load.d/reimu.conf"

echo "== 3. build bmcweb (REIMU revision + REIMU patches + web UI patch), jsnbd, daemons"
docker build -q -t reimu-testvm-build - <<'EOF' >/dev/null
FROM ubuntu:20.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y git gcc-10 g++-10 ninja-build pkg-config python3-pip libpam0g-dev \
      libsystemd-dev systemd libssl-dev zlib1g-dev libtinyxml2-dev libjson-c-dev libudev-dev autoconf automake make bzip2 && \
    pip3 install meson==0.57.2 && \
    update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100 && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-10 100
EOF
cd "$W/src"
[ -d bmcweb ] || git clone -q https://github.com/openbmc/bmcweb
git -C bmcweb checkout -q -f "$BMCWEB_REV" && git -C bmcweb clean -qfdx -e subprojects
for p in "$MCST"/meta-common/recipes-phosphor/bmcweb/*.patch; do patch -s -d bmcweb -p1 < "$p"; done
[ -d bmcweb/subprojects/sdbusplus ] || git clone -q https://github.com/openbmc/sdbusplus bmcweb/subprojects/sdbusplus
git -C bmcweb/subprojects/sdbusplus checkout -q "$SDBUSPLUS_REV"
if [ ! -d bmcweb/subprojects/boost_1_75_0 ]; then
    # boost 1.75.0 tarball (sha256 953db31e016db7bb207f11432bef7df100516eeb746843fa0486a222e3fd49cb)
    curl -sfL -o boost.tar.bz2 "${BOOST_URL:-https://tarballs.nixos.org/sha256/953db31e016db7bb207f11432bef7df100516eeb746843fa0486a222e3fd49cb}"
    echo "953db31e016db7bb207f11432bef7df100516eeb746843fa0486a222e3fd49cb  boost.tar.bz2" | sha256sum -c -
    tar xjf boost.tar.bz2 -C bmcweb/subprojects
fi
[ -d jsnbd ] || git clone -q https://github.com/openbmc/jsnbd
git -C jsnbd checkout -q -f "$JSNBD_REV"
for p in "$MCST"/meta-common/recipes-connectivity/jsnbd/files/*.patch; do patch -s -d jsnbd -p1 < "$p"; done
cp "$MCST"/meta-common/recipes-mcst/reimu-serial/files/reimu-serial.c "$MCST"/meta-common/recipes-mcst/reimu-pns/files/reimu-pnsd.c "$MCST"/meta-common/recipes-mcst/reimu-pns/files/reimu-sensors.[ch] "$MCST"/meta-common/recipes-mcst/reimu-pns/files/reimu-journal.c .
docker run --rm --network host -e https_proxy -e HTTPS_PROXY -v "$W/src:/w" -w /w reimu-testvm-build bash -c '
  set -e
  (cd bmcweb && { [ -d build ] || meson setup build -Dtests=disabled -Db_lto=false --buildtype=minsize >/dev/null; } && ninja -C build >/dev/null && strip -o build/bmcweb.stripped build/bmcweb)
  (cd jsnbd && autoreconf -fi >/dev/null 2>&1 && ./configure -q --prefix=/usr --sysconfdir=/etc --localstatedir=/var && make -s)
  gcc -O2 -Wall -o reimu-serial reimu-serial.c
  gcc -O2 -Wall -o reimu-pnsd reimu-pnsd.c reimu-sensors.c reimu-journal.c -lsystemd -lm'

echo "== 4. install REIMU components"
install -m755 bmcweb/build/bmcweb.stripped "$R/usr/bin/bmcweb"
cp -a bmcweb/build/subprojects/sdbusplus/libsdbusplus.so* "$R/usr/lib/"
install -m644 bmcweb/pam-webserver "$R/etc/pam.d/webserver"
sed 's|@MESON_INSTALL_PREFIX@|/usr|' bmcweb/bmcweb.service.in > "$R/etc/systemd/system/bmcweb.service"
cp bmcweb/bmcweb.socket "$R/etc/systemd/system/"
install -m755 jsnbd/nbd-proxy "$R/usr/sbin/nbd-proxy"
install -d "$R/etc/nbd-proxy"
install -m644 "$MCST/meta-common/recipes-connectivity/jsnbd/files/config.json" "$R/etc/nbd-proxy/config.json"
install -m755 "$MCST/meta-common/recipes-connectivity/jsnbd/files/reimu_state_hook" "$R/etc/nbd-proxy/state"
install -m755 reimu-serial reimu-pnsd "$R/usr/sbin/"
install -m644 "$MCST"/meta-common/recipes-mcst/reimu-serial/files/reimu-serial.service "$R/etc/systemd/system/"
install -m644 "$MCST"/meta-common/recipes-mcst/reimu-pns/files/reimu-pnsd.service "$R/etc/systemd/system/"
install -m644 "$MCST"/meta-common/recipes-mcst/reimu-pns/files/xyz.openbmc_project.Reimu.Pns.conf "$R/etc/dbus-1/system.d/"
install -d "$R/usr/libexec" "$R/libexec"
install -m755 "$MCST"/meta-common/recipes-mcst/reimu-pns/files/reimu-pns-scan "$R/usr/libexec/"
# web UI, installed like the reimu-webui recipe does (gzip-compressed)
rm -rf "$R/usr/share/www" && cp -r "$MCST/meta-common/recipes-phosphor/reimu-webui/files/www" "$R/usr/share/www"
find "$R/usr/share/www" -name 'LICENSE*' -delete
find "$R/usr/share/www" -type f \( -name '*.js' -o -name '*.css' -o -name '*.html' -o -name '*.svg' \) -exec gzip -9 -n {} \;
# the platform's COM ports are the VM's ttyS1/ttyS2 (QEMU chardev sockets)
cat > "$R/etc/reimu-serial.conf" <<'EOF'
host /dev/ttyS1 115200 lines=20000 rawlog=/var/log/sol.log console Консоль платформы (COM1)
com2 /dev/ttyS2 115200 lines=20000 Второй COM-порт платформы
EOF

echo "== 5. test fixtures standing in for the hardware"
install -d "$R/root/fixtures"
cp "$HERE"/fixtures/motherboard_info.xml "$HERE"/fixtures/motherboard_devtree.dts "$R/root/fixtures/"
cp "$HERE/../fake-phosphor.py" "$R/root/fixtures/"
install -m644 "$HERE/fixtures/gpio-funcs" "$R/libexec/gpio-funcs"
cp "$HERE/fixtures/fake-phosphor.conf" "$R/etc/dbus-1/system.d/"
for s in testvnc fake-fruid fake-phosphor pns-mtd; do cp "$HERE/fixtures/$s.service" "$R/etc/systemd/system/"; done
python3 "$HERE/make-pns-flash.py" "$R/root/fixtures/pns-flash.bin"

echo "== 6. system configuration"
for s in bmcweb.socket bmcweb reimu-serial reimu-pnsd fake-fruid fake-phosphor pns-mtd testvnc ssh; do chroot "$R" systemctl enable "$s" >/dev/null 2>&1; done
echo "root:0penBmc" | chroot "$R" chpasswd
for g in redfish web priv-admin; do chroot "$R" sh -c "groupadd -f $g; usermod -aG $g root"; done
mkdir -p "$R/home/root" "$R/root/.ssh"
grep -q '^PermitRootLogin yes' "$R/etc/ssh/sshd_config" || echo 'PermitRootLogin yes' >> "$R/etc/ssh/sshd_config"
[ -f "$W/id_ed25519" ] || ssh-keygen -q -t ed25519 -N '' -f "$W/id_ed25519"
cp "$W/id_ed25519.pub" "$R/root/.ssh/authorized_keys"
echo reimu-test > "$R/etc/hostname"
printf 'auto lo\niface lo inet loopback\nauto ens3\niface ens3 inet dhcp\n' > "$R/etc/network/interfaces"

echo "== 7. disk image"
cp "$R/boot/vmlinuz-$KVER" "$W/vmlinuz"
cp "$R/boot/initrd.img-$KVER" "$W/initrd.img"
rm -f "$W/disk.img"
mkfs.ext4 -q -F -L root -d "$R" "$W/disk.img" 4G
echo "done: $W (start it with: $HERE/run-vm.sh $W)"
