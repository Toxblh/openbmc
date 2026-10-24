SUMMARY = "REIMU: the board's FRU ID in the inventory"
DESCRIPTION = "Publishes the board info area of the FRU ID that reimu-fru \
read (XML) as the inventory board object: Redfish Chassis with its sensors, \
IPMI FRU 0. Reads files only, never the EEPROM."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
inherit allarch systemd
RDEPENDS_${PN} = "phosphor-inventory-manager"

SRC_URI = "file://reimu-inventory file://reimu-inventory.service file://reimu-inventory.path"
S = "${WORKDIR}"
SYSTEMD_SERVICE_${PN} = "reimu-inventory.service reimu-inventory.path"

do_install() {
    install -d ${D}/usr/libexec ${D}${systemd_system_unitdir}
    install -m 755 reimu-inventory ${D}/usr/libexec
    install -m 644 reimu-inventory.service reimu-inventory.path ${D}${systemd_system_unitdir}
}
FILES_${PN} = "/usr/libexec ${systemd_system_unitdir}"
