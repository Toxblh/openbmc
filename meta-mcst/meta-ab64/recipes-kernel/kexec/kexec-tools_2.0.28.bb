SUMMARY = "Kexec: boot a kernel from the running one"
DESCRIPTION = "kexec only (no kdump, vmcore-dmesg): reimu-ab restarts with the rwfs in RAM without resetting the BMC (and with it the platform)"
HOMEPAGE = "https://kernel.org/pub/linux/utils/kernel/kexec/"
SECTION = "kernel/userland"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://COPYING;md5=ea5bed2f60d357618ca161ad539f7c0a"

SRC_URI = "${KERNELORG_MIRROR}/linux/utils/kernel/kexec/kexec-tools-${PV}.tar.xz"
SRC_URI[sha256sum] = "d2f0ef872f39e2fe4b1b01feb62b0001383207239b9f8041f98a95564161d053"

inherit autotools
DEPENDS = "zlib"
EXTRA_OECONF = "--without-xen --without-lzma"

do_install() {
    install -D -m 755 ${B}/build/sbin/kexec ${D}${sbindir}/kexec
}
PACKAGES = "${PN} ${PN}-dbg"
FILES_${PN} = "${sbindir}/kexec"
COMPATIBLE_HOST = "arm.*-linux"
