SUMMARY = "REIMU USB network to the host"
DESCRIPTION = "reimu-usbnet: the BMC as a USB network card (CDC NCM, configfs gadget reimu_net on the virtual hub) for its host, so the host agent and the KVM's picture reach the BMC with no LAN cable; IPv6 link-local with eth0's MAC, IPv4 with a DHCP server, and the host's internet through the BMC's port (NAT) when switched on"
SECTION = "base"
PR = "r2"
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0;md5=801f80980d171dd6425610833a22dbe6"

inherit allarch systemd

SRC_URI = " \
            file://reimu-usbnet \
            file://reimu-usbnet.service \
            file://reimu-usbnet-watch.service \
            file://reimu-usbnet.conf \
          "
S = "${WORKDIR}"

SYSTEMD_SERVICE_${PN} = "reimu-usbnet.service reimu-usbnet-watch.service"
# the NAT of «Интернет для хоста через порт BMC»
RDEPENDS_${PN} = "iptables kernel-module-iptable-nat kernel-module-xt-masquerade kernel-module-xt-conntrack"

do_install() {
    install -d ${D}${libexecdir} ${D}${systemd_system_unitdir} ${D}${sysconfdir}
    install -m 755 reimu-usbnet ${D}${libexecdir}
    install -m 644 reimu-usbnet.service reimu-usbnet-watch.service ${D}${systemd_system_unitdir}
    install -m 644 reimu-usbnet.conf ${D}${sysconfdir}
}

FILES_${PN} = "${libexecdir} ${systemd_system_unitdir} ${sysconfdir}"
CONFFILES_${PN} = "${sysconfdir}/reimu-usbnet.conf"
