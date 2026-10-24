SUMMARY = "MCST FRU ID retriever"
DESCRIPTION = "MCST FRU ID retriever"
SECTION = "base"
PR = "r1"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

inherit meson
inherit systemd
inherit features_check

REQUIRED_DISTRO_FEATURES = "systemd"
SYSTEMD_SERVICE_${PN} = "mcst-fruid.service"
DEPENDS += "libreimu systemd i2c-tools"
RDEPENDS_${PN} += "dtc gpio-funcs"

SRC_URI = "git://github.com/makise-homura/mcst-fruid.git;protocol=https \
           file://reimu-fru \
           file://mcst-fruid.service \
           file://reimu-fru-power.service \
          "
# pinned: a build must not change by itself
SRCREV = "f378aaa09c5becd4446687efec6108f2a60c11c1"

S = "${WORKDIR}/git"

# /sys/class/i2c-adapter is gone in kernel 6.x (CONFIG_I2C_COMPAT removed);
# the adapters are under /sys/bus/i2c/devices in every kernel
do_configure_prepend() {
    sed -i "s#/sys/class/i2c-adapter/#/sys/bus/i2c/devices/#g" ${S}/i2c-ops.c
    # the PNS flash is only read
    sed -i "s#open(dev, O_RDWR)#open(dev, O_RDONLY)#" ${S}/spi-ops-mtd.c
    # an EEPROM that fails the header check: fclose() of an unset FILE *
    sed -i "s#^    FILE \*f;\$#    FILE *f = NULL;#" ${S}/fruid-ops.c
    # FRU_XML: reimu-fru writes a temporary file, renamed once complete
    sed -i 's#char \*path = "/var/volatile/motherboard_info.xml";#char *path = getenv("FRU_XML") ? getenv("FRU_XML") : "/var/volatile/motherboard_info.xml";#' ${S}/get-fruid.c
}

# mcst-fruid.service runs reimu-fru: the device tree whatever the FRU ID
# EEPROM does, the FRU ID again once the platform is powered
do_install_append() {
    install -d ${D}/usr/libexec
    install -m 755 ${WORKDIR}/reimu-fru ${D}/usr/libexec
    install -m 644 ${WORKDIR}/mcst-fruid.service ${WORKDIR}/reimu-fru-power.service ${D}${systemd_system_unitdir}
}
FILES_${PN} += "/usr/libexec ${systemd_system_unitdir}/reimu-fru-power.service"
