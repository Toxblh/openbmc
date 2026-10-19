SUMMARY = "Reliable BMC firmware upgrade"
DESCRIPTION = "Health-checked confirmation of BMC images with fallback to the second flash chip (AST2400/AST2500 2nd boot watchdog), backup mirroring and restore"
SECTION = "base"
PR = "r1"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

inherit systemd
inherit features_check

REQUIRED_DISTRO_FEATURES = "systemd"
DEPENDS = "systemd"
RDEPENDS_${PN} += "bash wget ${@d.getVar('PREFERRED_PROVIDER_u-boot-fw-utils', True) or 'u-boot-fw-utils'}"
SYSTEMD_SERVICE_${PN} = "reimu-bootguard.service reimu-bootguard-update.path reimu-firmwared.service"

SRC_URI = " \
            file://LICENSE \
            file://Makefile \
            file://reimu-wdt2.c \
            file://reimu-flashcopy.c \
            file://reimu-bootguard \
            file://reimu-bootguard.conf \
            file://reimu-bootguard.service \
            file://reimu-bootguard-update.path \
            file://reimu-bootguard-update.service \
            file://reimu-firmwared.c \
            file://reimu-firmwared.service \
            file://xyz.openbmc_project.Reimu.Firmware.conf \
          "

S = "${WORKDIR}"

do_install() {
  install -d ${D}/usr/sbin ${D}${sysconfdir}/dbus-1/system.d ${D}${systemd_system_unitdir}
  install -m 755 reimu-wdt2 reimu-flashcopy reimu-bootguard reimu-firmwared ${D}/usr/sbin
  install -m 644 xyz.openbmc_project.Reimu.Firmware.conf ${D}${sysconfdir}/dbus-1/system.d
  install -m 644 reimu-bootguard.conf ${D}${sysconfdir}
  install -m 644 reimu-bootguard.service reimu-bootguard-update.path reimu-bootguard-update.service reimu-firmwared.service ${D}${systemd_system_unitdir}
}

CONFFILES_${PN} = "${sysconfdir}/reimu-bootguard.conf"
FILES_${PN} = "/usr/sbin ${sysconfdir} ${systemd_system_unitdir}"
