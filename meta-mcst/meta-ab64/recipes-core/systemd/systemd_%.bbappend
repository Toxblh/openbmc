FILESEXTRAPATHS_prepend := "${THISDIR}/files:"
SRC_URI += " file://00-reimu-no-llmnr.conf "

do_install_append() {
    install -d ${D}${sysconfdir}/systemd/resolved.conf.d
    install -m 0644 ${WORKDIR}/00-reimu-no-llmnr.conf ${D}${sysconfdir}/systemd/resolved.conf.d/
}
FILES_${PN} += "${sysconfdir}/systemd/resolved.conf.d"
