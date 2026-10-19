SUMMARY = "tmux with minicom for serial-over-lan"
DESCRIPTION = "tmux with minicom for serial-over-lan"
SECTION = "base"
PR = "r1"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

inherit systemd
inherit features_check

REQUIRED_DISTRO_FEATURES = "systemd"
RDEPENDS_${PN} += "systemd bash minicom tmux logrotate reimu-serial"
SYSTEMD_SERVICE_${PN} = "tmux-logger.service logrotate-sol.service logrotate-sol.timer"

SRC_URI = "file://sol.conf \
           file://minirc.dfl \
           file://solrotate.conf \
           file://start-tmux \
           file://tmux-logger.service \
           file://logrotate-sol.service \
           file://logrotate-sol.timer \
           file://01-limit-journal-size.conf \
           file://LICENSE \
          "

S = "${WORKDIR}"

do_install() {
  install -d ${D}/etc
  install -d ${D}/etc/logrotate.d
  install -d ${D}/libexec
  install -d ${D}/usr/log
  install -d ${D}${systemd_system_unitdir}
  install -d ${D}${systemd_unitdir}/journald.conf.d/
  install -m 644 sol.conf ${D}/etc
  install -m 644 minirc.dfl ${D}/etc
  install -m 644 solrotate.conf ${D}/etc/logrotate.d
  install -m 755 start-tmux ${D}/libexec
  install -m 644 tmux-logger.service ${D}${systemd_system_unitdir}
  install -m 644 logrotate-sol.service ${D}${systemd_system_unitdir}
  install -m 644 logrotate-sol.timer ${D}${systemd_system_unitdir}
  install -m 644 01-limit-journal-size.conf ${D}${systemd_unitdir}/journald.conf.d
  ln -s /var/log/sol.log ${D}/usr/log/current
}

FILES_${PN} = "/etc /libexec /usr/log ${systemd_system_unitdir} ${systemd_unitdir}/journald.conf.d"
