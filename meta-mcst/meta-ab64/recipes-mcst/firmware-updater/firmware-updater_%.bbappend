# A/B: the BMC is updated by reimu-ab; firmware-updater writes the static
# layout ("kernel", "rofs") and is left out, and with it firmware-updater.plain
# (mtdblock writes, no backup, no check of the host power). update-host-flash
# (the PNS flash) stays as the same command, now writing with reimu-pns-flash;
# without GNU coreutils, GNU wget (TLS) and CIFS: an image is given as a file
# or a plain http/ftp URL (busybox wget).
FILESEXTRAPATHS_prepend := "${THISDIR}/files:"
SRC_URI += " file://update-host-flash-ab "
RDEPENDS_${PN}_remove = "coreutils wget cifs-utils pv"
RDEPENDS_${PN} += "reimu-pns-flash"
do_install_append() {
    rm -f ${D}/usr/sbin/firmware-updater ${D}/libexec/firmware-updater.plain
    install -m 755 ${WORKDIR}/update-host-flash-ab ${D}/usr/sbin/update-host-flash
}
