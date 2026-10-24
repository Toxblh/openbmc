SUMMARY = "Get info for CPU, AHB & PCLK frequency"
DESCRIPTION = "Get info for CPU, AHB & PCLK frequency"
SECTION = "base"
PR = "r1"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

SRC_URI = "git://github.com/makise-homura/astclkinfo.git;protocol=https"
# pinned: a build must not change by itself
SRCREV = "366efec18b2d4a725da3d31136304251f16450dd"

RDEPENDS_${PN} += "bash busybox"

S = "${WORKDIR}/git"

do_install() {
  install -d ${D}/usr/bin
  install -m 755 astclkinfo ${D}/usr/bin
}

FILES_${PN} = "/usr/bin"
