SUMMARY = "Fan control for REIMU-5564"
DESCRIPTION = "reimu-fand: the CPU, board and case fans of REIMU-5564 by temperature curves, fail-safe to 100 %, D-Bus service xyz.openbmc_project.Reimu.Fans"
SECTION = "base"
PR = "r1"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

inherit systemd
inherit features_check

REQUIRED_DISTRO_FEATURES = "systemd"
DEPENDS = "systemd"
SYSTEMD_SERVICE_${PN} = "reimu-fand.service"

SRC_URI = " \
            file://LICENSE \
            file://Makefile \
            file://reimu-fand.c \
            file://fand.c \
            file://fand-hw.c \
            file://fand.h \
            file://reimu-fand.service \
            file://reimu-fand.conf \
            file://xyz.openbmc_project.Reimu.Fans.conf \
          "

S = "${WORKDIR}"

do_install() {
  install -d ${D}/usr/sbin ${D}${systemd_system_unitdir} ${D}${sysconfdir}/dbus-1/system.d
  install -m 755 reimu-fand ${D}/usr/sbin
  install -m 644 reimu-fand.service ${D}${systemd_system_unitdir}
  install -m 644 reimu-fand.conf ${D}${sysconfdir}
  install -m 644 xyz.openbmc_project.Reimu.Fans.conf ${D}${sysconfdir}/dbus-1/system.d
}

FILES_${PN} = "/usr/sbin ${systemd_system_unitdir} ${sysconfdir}"
CONFFILES_${PN} = "${sysconfdir}/reimu-fand.conf"
