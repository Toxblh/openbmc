SUMMARY = "A/B firmware on the one BMC flash"
DESCRIPTION = "reimu-ab: install an update into the other slot with every write read back and compared, the U-Boot boot counter and fallback, confirmation of a healthy boot, the way back to the stock firmware; reimu-abd: the same install through Redfish UpdateService and the web UI"
SECTION = "base"
PR = "r1"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

inherit systemd
DEPENDS = "systemd"
RDEPENDS_${PN} += "u-boot-fw-utils-aspeed kexec-tools"
SYSTEMD_SERVICE_${PN} = "reimu-ab.service reimu-abd.service"

SRC_URI = " \
            file://LICENSE \
            file://Makefile \
            file://reimu-flash.c \
            file://reimu-fit.c \
            file://reimu-ab \
            file://reimu-ab.env \
            file://reimu-ab.service \
            file://reimu-watchdog.conf \
            file://reimu-abd.c \
            file://reimu-abd.service \
            file://xyz.openbmc_project.Software.BMC.Updater.conf \
          "
S = "${WORKDIR}"

do_install() {
  install -d ${D}/usr/sbin ${D}/usr/share/reimu-ab ${D}${systemd_system_unitdir} \
             ${D}${systemd_unitdir}/system.conf.d ${D}${sysconfdir}/dbus-1/system.d
  install -m 755 reimu-flash reimu-fit reimu-ab reimu-abd ${D}/usr/sbin
  install -m 644 reimu-abd.service ${D}${systemd_system_unitdir}
  install -m 644 xyz.openbmc_project.Software.BMC.Updater.conf ${D}${sysconfdir}/dbus-1/system.d
  install -m 644 reimu-ab.env ${D}/usr/share/reimu-ab
  install -m 644 reimu-ab.service ${D}${systemd_system_unitdir}
  install -m 644 reimu-watchdog.conf ${D}${systemd_unitdir}/system.conf.d/reimu-watchdog.conf
}
FILES_${PN} += "${systemd_unitdir}/system.conf.d"
