SUMMARY = "Platform sensors of REIMU-5564"
DESCRIPTION = "reimu-hwmon: the platform's sensors read once (the fans' chips and the PCS by reimu-fand, the rest here) and published as Sensor.Value for Redfish and IPMI, the web UI's documents (sensors, overheat, live view) and Prometheus metrics; D-Bus service xyz.openbmc_project.Reimu.Hwmon"
SECTION = "base"
PR = "r1"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

inherit systemd
inherit features_check

REQUIRED_DISTRO_FEATURES = "systemd"
DEPENDS = "systemd"
SYSTEMD_SERVICE_${PN} = "reimu-hwmon.service"

# rbuf.c, dbusjson.c: shared with reimu-pns
FILESEXTRAPATHS_prepend := "${THISDIR}/../reimu-common/files:"

SRC_URI = " \
            file://LICENSE \
            file://Makefile \
            file://reimu-hwmon.c \
            file://hwmon.h \
            file://hwmon-dt.c \
            file://hwmon-chips.c \
            file://hwmon-model.c \
            file://sensorbus.c \
            file://sensorbus.h \
            file://reimu-overheat.c \
            file://reimu-overheat.h \
            file://reimu-view-live.c \
            file://reimu-view-live.h \
            file://rbuf.c \
            file://rbuf.h \
            file://dbusjson.c \
            file://dbusjson.h \
            file://reimu-hwmon.service \
            file://xyz.openbmc_project.Reimu.Hwmon.conf \
          "

S = "${WORKDIR}"

do_install() {
  install -d ${D}/usr/sbin ${D}${systemd_system_unitdir} ${D}${sysconfdir}/dbus-1/system.d
  install -m 755 reimu-hwmon ${D}/usr/sbin
  install -m 644 reimu-hwmon.service ${D}${systemd_system_unitdir}
  install -m 644 xyz.openbmc_project.Reimu.Hwmon.conf ${D}${sysconfdir}/dbus-1/system.d
}

FILES_${PN} = "/usr/sbin ${systemd_system_unitdir} ${sysconfdir}/dbus-1"
