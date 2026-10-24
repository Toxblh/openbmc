SUMMARY = "Host boot firmware (PNS) flash: checked, backed up, verified writes"
DESCRIPTION = "reimu-pns-flash: version, check, read (backup) and write of the PNS flash on spi1, only while the platform is off and with power-on blocked; every write read back and compared (reimu-flash), the flash always switched back to the host"
SECTION = "base"
PR = "r1"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

inherit systemd allarch
# reimu-flash (reimu-ab), gpio-funcs, fdtget (dtc)
RDEPENDS_${PN} += "reimu-ab gpio-funcs dtc"
SYSTEMD_SERVICE_${PN} = "reimu-pns-version.service"

SRC_URI = " \
            file://LICENSE \
            file://reimu-pns-flash \
            file://reimu-pns-version.service \
          "
S = "${WORKDIR}"

do_install() {
  install -d ${D}/usr/sbin ${D}${systemd_system_unitdir}
  install -m 755 reimu-pns-flash ${D}/usr/sbin
  install -m 644 reimu-pns-version.service ${D}${systemd_system_unitdir}
}
