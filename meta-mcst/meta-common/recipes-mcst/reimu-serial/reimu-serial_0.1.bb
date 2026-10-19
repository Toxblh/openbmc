SUMMARY = "Serial ports multiplexer with timestamped history"
DESCRIPTION = "Owns host COM ports, keeps last lines with timestamps, serves them to the web UI (bmcweb /reimu/serial/N), pty for minicom/tmux and obmc-console compatible socket"
SECTION = "base"
PR = "r1"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

inherit systemd
inherit features_check

REQUIRED_DISTRO_FEATURES = "systemd"
SYSTEMD_SERVICE_${PN} = "reimu-serial.service"

SRC_URI = " \
            file://LICENSE \
            file://Makefile \
            file://reimu-serial.c \
            file://reimu-serial.conf \
            file://reimu-serial.service \
          "

S = "${WORKDIR}"

do_install() {
  install -d ${D}/usr/sbin
  install -d ${D}${sysconfdir}
  install -d ${D}${systemd_system_unitdir}
  install -m 755 reimu-serial ${D}/usr/sbin
  install -m 644 reimu-serial.conf ${D}${sysconfdir}
  install -m 644 reimu-serial.service ${D}${systemd_system_unitdir}
}

CONFFILES_${PN} = "${sysconfdir}/reimu-serial.conf"
FILES_${PN} = "/usr/sbin ${sysconfdir} ${systemd_system_unitdir}"
