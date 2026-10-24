SUMMARY = "REIMU-5564: IPMI sensors (SDR): host state and the platform's temperatures"
PR = "r1"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

inherit phosphor-ipmi-host
inherit native

FILESEXTRAPATHS_prepend := "${THISDIR}/reimu-ab-ipmi-sensor-inventory:"
SRC_URI += "file://config.yaml"
PROVIDES += "virtual/phosphor-ipmi-sensor-inventory"
S = "${WORKDIR}"

do_install() {
        DEST=${D}${sensor_datadir}
        install -d ${DEST}
        install config.yaml ${DEST}/sensor.yaml
}
