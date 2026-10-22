SUMMARY = "A/B firmware on the one BMC flash"
DESCRIPTION = "reimu-ab: install an update into the other slot with every write read back and compared, the U-Boot boot counter and fallback, confirmation of a healthy boot, the way back to the stock firmware"
SECTION = "base"
PR = "r1"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

inherit systemd
RDEPENDS_${PN} += "u-boot-fw-utils-aspeed kexec-tools"
SYSTEMD_SERVICE_${PN} = "reimu-ab.service"

SRC_URI = " \
            file://LICENSE \
            file://Makefile \
            file://reimu-flash.c \
            file://reimu-fit.c \
            file://reimu-ab \
            file://reimu-ab.env \
            file://reimu-ab.service \
            file://reimu-watchdog.conf \
          "
S = "${WORKDIR}"

do_install() {
  install -d ${D}/usr/sbin ${D}/usr/share/reimu-ab ${D}${systemd_system_unitdir} \
             ${D}${systemd_unitdir}/system.conf.d
  install -m 755 reimu-flash reimu-fit reimu-ab ${D}/usr/sbin
  install -m 644 reimu-ab.env ${D}/usr/share/reimu-ab
  install -m 644 reimu-ab.service ${D}${systemd_system_unitdir}
  install -m 644 reimu-watchdog.conf ${D}${systemd_unitdir}/system.conf.d/reimu-watchdog.conf
}
FILES_${PN} += "${systemd_unitdir}/system.conf.d"
