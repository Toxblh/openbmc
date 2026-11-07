SUMMARY = "REIMU: the BMC's logs: a week of journal, the persistent event log, the diagnostic archive"
DESCRIPTION = "reimu-logd serves the web UI its window of the journal; follows the journal (RAM) and appends warnings and worse, the fan/heat/host link/update daemons' lines and a boot record (why the BMC started) to a small rate-limited log on the rwfs; serves it and reimu-diag's archive over D-Bus (REST, Administrators). journald: a week in RAM, nothing to the flash. Quiets the per-minute SoL rotation."
SECTION = "base"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0;md5=801f80980d171dd6425610833a22dbe6"

inherit systemd
DEPENDS = "systemd"
SYSTEMD_SERVICE_${PN} = "reimu-logd.service"

# rbuf.c: shared with reimu-pns, reimu-hwmon
FILESEXTRAPATHS_prepend := "${THISDIR}/../../../meta-common/recipes-mcst/reimu-common/files:"

SRC_URI = " \
            file://Makefile \
            file://reimu-logd.c \
            file://reimu-journal.c \
            file://reimu-journal.h \
            file://rbuf.c \
            file://rbuf.h \
            file://ev-core.c \
            file://ev-core.h \
            file://reimu-logd.service \
            file://reimu-diag \
            file://xyz.openbmc_project.Reimu.Logs.conf \
            file://90-reimu-logs.conf \
          "
S = "${WORKDIR}"

do_install() {
  install -d ${D}/usr/sbin ${D}/usr/libexec ${D}${systemd_system_unitdir} ${D}${sysconfdir}/dbus-1/system.d \
             ${D}${systemd_unitdir}/journald.conf.d
  install -m 755 reimu-logd ${D}/usr/sbin
  install -m 755 reimu-diag ${D}/usr/libexec
  ln -sf ../libexec/reimu-diag ${D}/usr/sbin/reimu-diag
  install -m 644 reimu-logd.service ${D}${systemd_system_unitdir}
  install -m 644 xyz.openbmc_project.Reimu.Logs.conf ${D}${sysconfdir}/dbus-1/system.d
  # after 00-systemd-conf.conf (64M, ForwardToSyslog) and tmux-logger's 01 (5M)
  install -m 644 90-reimu-logs.conf ${D}${systemd_unitdir}/journald.conf.d
  # the SoL log's rotation: a loop instead of a oneshot each minute: PID 1's
  # three lines a minute about it were more than half of the journal (and
  # LogLevelMax doesn't reach PID 1's own lines in systemd 247). The timer stays
  # (a start of a running service is nothing): it brings the loop back
  d=${D}${systemd_system_unitdir}/logrotate-sol.service.d
  install -d $d
  cat > $d/reimu-quiet.conf <<'END'
[Service]
Type=simple
ExecStart=
ExecStart=/bin/sh -c 'while :; do /libexec/logrotate-sol; sleep 60; done'
LogLevelMax=warning
END
}
FILES_${PN} = "/usr/sbin /usr/libexec ${sysconfdir}/dbus-1/system.d ${systemd_unitdir}/journald.conf.d ${systemd_system_unitdir}"
