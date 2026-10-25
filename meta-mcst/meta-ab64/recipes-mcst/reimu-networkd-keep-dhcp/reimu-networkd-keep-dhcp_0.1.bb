SUMMARY = "REIMU: systemd-networkd keeps the DHCP address over its restarts"
DESCRIPTION = "Drop-ins KeepConfiguration=dhcp for the BMC's eth0/eth1 networks. \
phosphor-networkd restarts systemd-networkd whenever it rewrites a network \
(at boot, on every change from Redfish/IPMI/web): the new networkd (247) \
dropped the kernel's DHCP address as foreign while its DHCP client was \
configuring the same address, freed it under a pending netlink request and \
aborted (Assertion 'a' failed in address_is_ready(), dhcp4_address_handler): \
a network blip each time, 8 aborts in 25 restarts in QEMU. Keeping the DHCP \
address as networkd does over its own restarts removes that race."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
inherit allarch

# phosphor-networkd writes /etc/systemd/network/00-bmc-<if>.network itself (and
# only the keys it knows): the setting lives in a drop-in it never touches
do_install() {
    for i in eth0 eth1; do
        d=${D}${systemd_unitdir}/network/00-bmc-$i.network.d
        install -d $d
        printf '%s\n' '# reimu-networkd-keep-dhcp: see the recipe' '[Network]' 'KeepConfiguration=dhcp' \
            > $d/reimu-keep-dhcp.conf
    done
}
FILES_${PN} = "${systemd_unitdir}/network"
