SUMMARY = "Sensor instantiator for phosphor-hwmon"
DESCRIPTION = "Sensor instantiator for phosphor-hwmon"
SECTION = "base"
PR = "r1"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

inherit systemd
inherit features_check

DEPENDS = "libreimu"
REQUIRED_DISTRO_FEATURES = "systemd"
RDEPENDS_${PN} += "systemd bash phosphor-hwmon"
SYSTEMD_SERVICE_${PN} = "hwmon-instantiator.service"

SRC_URI = " \
            file://LICENSE \
            file://Makefile \
            file://hwmon-instantiator.c \
            file://hwmon-instantiator.service \
            file://hwmon-instantiator-rescan.service \
          "

S = "${WORKDIR}"

do_install_append() {
  install -d ${D}/sbin
  install -d ${D}${systemd_system_unitdir}
  install -m 755 hwmon-instantiator ${D}/sbin
  install -m 644 hwmon-instantiator.service hwmon-instantiator-rescan.service ${D}${systemd_system_unitdir}
}

FILES_${PN} = "/sbin ${systemd_system_unitdir}"
