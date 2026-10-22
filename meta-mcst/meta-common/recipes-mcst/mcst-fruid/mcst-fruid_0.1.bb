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
RDEPENDS_${PN} += "dtc"

SRC_URI = "git://github.com/makise-homura/mcst-fruid.git;protocol=https"
SRCREV = "${AUTOREV}"

S = "${WORKDIR}/git"

# /sys/class/i2c-adapter is gone in kernel 6.x (CONFIG_I2C_COMPAT removed);
# the adapters are under /sys/bus/i2c/devices in every kernel
do_configure_prepend() {
    sed -i "s#/sys/class/i2c-adapter/#/sys/bus/i2c/devices/#g" ${S}/i2c-ops.c
}
