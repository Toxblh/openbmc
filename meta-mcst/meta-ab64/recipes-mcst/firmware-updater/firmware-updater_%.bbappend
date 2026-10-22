# A/B: the BMC is updated by reimu-ab; firmware-updater writes the static
# layout ("kernel", "rofs") and is left out. update-host-flash (the PNS flash)
# stays, without GNU coreutils, GNU wget (TLS) and CIFS: an image is given
# as a file or a plain http/ftp URL (busybox wget).
RDEPENDS_${PN}_remove = "coreutils wget cifs-utils"
do_install_append() {
    rm -f ${D}/usr/sbin/firmware-updater ${D}/libexec/firmware-updater.plain
}
