SUMMARY = "Host boot firmware (PNS) information for the web UI"
DESCRIPTION = "D-Bus service xyz.openbmc_project.Reimu.Pns: FRU, device tree and partition table of the PNS flash"
SECTION = "base"
PR = "r1"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

inherit systemd
inherit features_check

REQUIRED_DISTRO_FEATURES = "systemd"
DEPENDS = "systemd"
RDEPENDS_${PN} += "bash gpio-funcs mcst-fruid"
SYSTEMD_SERVICE_${PN} = "reimu-pnsd.service"

SRC_URI = " \
            file://LICENSE \
            file://Makefile \
            file://reimu-pnsd.c \
            file://reimu-sensors.c \
            file://reimu-sensors.h \
            file://reimu-pns-scan \
            file://reimu-pnsd.service \
            file://xyz.openbmc_project.Reimu.Pns.conf \
          "

S = "${WORKDIR}"

do_install() {
  install -d ${D}/usr/sbin ${D}/usr/libexec ${D}${systemd_system_unitdir} ${D}${sysconfdir}/dbus-1/system.d
  install -m 755 reimu-pnsd ${D}/usr/sbin
  install -m 755 reimu-pns-scan ${D}/usr/libexec
  install -m 644 reimu-pnsd.service ${D}${systemd_system_unitdir}
  install -m 644 xyz.openbmc_project.Reimu.Pns.conf ${D}${sysconfdir}/dbus-1/system.d
}

FILES_${PN} = "/usr/sbin /usr/libexec ${systemd_system_unitdir} ${sysconfdir}/dbus-1"
